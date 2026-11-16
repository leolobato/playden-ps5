#include "runtime/publisher_account.h"

#include "platform/storage.h"
#include "steam/backend/json.h"
#include "steam/secret.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <optional>
#include <set>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace playden::runtime {
namespace {

namespace json = steam::json;
using json::Value;
using steam::ZeroizeSecret;

constexpr std::size_t kMaxDisplayBytes = 128;
constexpr std::size_t kMaxStoredBytes = 80 * 1024;  // a blob of up to 64 KiB plus the wrapper
constexpr std::size_t kMaxStatusBytes = 4096;

bool Fail(std::string* reason, std::string text) {
  if (reason) *reason = std::move(text);
  return false;
}

void WipeBytes(Bytes& bytes) {
  volatile std::uint8_t* data = bytes.data();
  for (std::size_t i = 0; i < bytes.size(); ++i) data[i] = 0;
  bytes.clear();
}

// Wipes every string of a parsed document (keys included): blobs hold secrets.
void WipeJson(Value& value) {
  ZeroizeSecret(const_cast<std::string&>(value.text()));
  for (auto& item : const_cast<Value::Array&>(value.array())) WipeJson(item);
  for (auto& [key, item] : const_cast<Value::Object&>(value.object())) {
    ZeroizeSecret(key);
    WipeJson(item);
  }
}

// Holds a parsed document and wipes it when it goes.
struct WipedJson {
  std::optional<Value> value;
  ~WipedJson() {
    if (value) WipeJson(*value);
  }
};

bool UniqueKeys(const Value& object) {
  std::set<std::string_view> keys;
  for (const auto& entry : object.object())
    if (!keys.insert(entry.first).second) return false;
  return true;
}

bool Base64Char(unsigned char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '/' ||
         c == '=';
}

// The first present string among names (nullptr when none).
const Value* FirstString(const Value& object, const std::vector<std::string>& names) {
  for (const auto& name : names) {
    const auto* value = object.Find(name);
    if (value && value->is_string()) return value;
  }
  return nullptr;
}

bool StageValueMatches(const std::string& value, const PublisherAccountSpec& spec) {
  return value.size() >= spec.stage_min && value.size() <= spec.stage_max &&
         std::all_of(value.begin(), value.end(), [](unsigned char c) { return Base64Char(c); });
}

std::string JsonString(const std::string& text) {
  static const char kHex[] = "0123456789abcdef";
  std::string out = "\"";
  for (unsigned char c : text) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += static_cast<char>(c);
    } else if (c < 0x20) {
      out += "\\u00";
      out += kHex[c >> 4];
      out += kHex[c & 15];
    } else {
      out += static_cast<char>(c);
    }
  }
  return out + "\"";
}

bool Write(const std::string& state_root, Publisher publisher, std::string& text, std::string* reason) {
  const Storage storage(state_root);
  Bytes bytes(text.begin(), text.end());
  ZeroizeSecret(text);
  const auto root = storage.EnsureRootDirectory();
  const auto written = root.ok() ? storage.WriteAtomicPrivate(PublisherAccountFileName(publisher), bytes) : root;
  WipeBytes(bytes);
  return written.ok() || Fail(reason, "Playden couldn't save the account: " + written.message());
}

// The stored wrapper, parsed; empty when missing or damaged.
bool Read(const std::string& state_root, Publisher publisher, WipedJson* document) {
  const Storage storage(state_root);
  auto bytes = storage.Read(PublisherAccountFileName(publisher), kMaxStoredBytes);
  if (!bytes.ok()) return false;
  std::string text(bytes.value().begin(), bytes.value().end());
  WipeBytes(bytes.value());
  json::Limits limits;
  limits.max_bytes = kMaxStoredBytes;
  limits.max_values = 16384;
  document->value = json::Parse(text, limits);
  ZeroizeSecret(text);
  const auto& value = document->value;
  if (!value || !value->is_object() || !UniqueKeys(*value)) return false;
  const auto* version = value->Find("version");
  const auto* state = value->Find("state");
  const auto* display = value->Find("display");
  const auto* obtained = value->Find("obtained_at");
  if (!version || version->AsUint64() != std::optional<std::uint64_t>(1) || !state || !state->is_string() ||
      !display || !display->is_string() || !obtained || !obtained->AsUint64())
    return false;
  if (state->text() == "signed_in") {
    const auto* blob = value->Find("blob");
    return blob && blob->is_object();
  }
  return state->text() == "expired";
}

}  // namespace

const char* PublisherSlug(Publisher publisher) {
  switch (publisher) {
    case Publisher::kSteamHost: return "steamhost";
    case Publisher::kEa: return "ea";
    case Publisher::kRockstar: return "rockstar";
    case Publisher::kUbisoft: return "ubisoft";
  }
  return "";
}

std::string PublisherAccountFileName(Publisher publisher) {
  return std::string("publisher-account-") + PublisherSlug(publisher) + ".v1";
}

bool AcceptPublisherCredential(const std::string& state_root, Publisher publisher, std::string_view blob,
                               const PublisherAccountSpec& spec, std::uint64_t now, std::string* display,
                               std::string* reason) {
  if (blob.empty() || blob.size() > spec.max_bytes) return Fail(reason, "The sign-in is empty or too large.");
  json::Limits limits;
  limits.max_bytes = spec.max_bytes;
  limits.max_values = 16000;  // the stored wrapper adds a level and a few values
  limits.max_depth = 15;
  WipedJson parsed;
  parsed.value = json::Parse(blob, limits);
  if (!parsed.value || !parsed.value->is_object() || !UniqueKeys(*parsed.value))
    return Fail(reason, "The sign-in isn't in the expected form.");
  const auto* stage = FirstString(*parsed.value, spec.stage_fields);
  if (!stage || !StageValueMatches(stage->text(), spec))
    return Fail(reason, "The sign-in doesn't hold a usable " + spec.name + " credential.");
  std::string name;
  if (const auto* shown = FirstString(*parsed.value, spec.display_fields)) {
    name = shown->text();
    const bool control = std::any_of(name.begin(), name.end(), [](unsigned char c) { return c < 0x20 || c == 0x7f; });
    if (name.size() > kMaxDisplayBytes || control) return Fail(reason, "The sign-in's account name isn't valid.");
  }
  std::string text = "{\"version\":1,\"obtained_at\":" + std::to_string(now) +
                     ",\"state\":\"signed_in\",\"display\":" + JsonString(name) + ",\"blob\":";
  text.append(blob.data(), blob.size());
  text += "}";
  if (!Write(state_root, publisher, text, reason)) return false;
  if (display) *display = name;
  return true;
}

PublisherAccount LoadPublisherAccount(const std::string& state_root, Publisher publisher) {
  PublisherAccount account;
  WipedJson document;
  if (!Read(state_root, publisher, &document)) return account;
  account.state = document.value->Find("state")->text() == "signed_in" ? PublisherAccountState::kSignedIn
                                                                        : PublisherAccountState::kExpired;
  account.display = document.value->Find("display")->text();
  account.obtained_at = *document.value->Find("obtained_at")->AsUint64();
  return account;
}

bool PublisherStageValue(const std::string& state_root, Publisher publisher, const PublisherAccountSpec& spec,
                         std::string* value, std::string* reason) {
  WipedJson document;
  if (!Read(state_root, publisher, &document) || document.value->Find("state")->text() != "signed_in")
    return Fail(reason, "Sign in to " + spec.name + " first.");
  const auto* stage = FirstString(*document.value->Find("blob"), spec.stage_fields);
  if (!stage || !StageValueMatches(stage->text(), spec))
    return Fail(reason, "The " + spec.name + " sign-in can't be used. Sign in again.");
  *value = stage->text();
  return true;
}

bool MarkPublisherAccountExpired(const std::string& state_root, Publisher publisher, std::string* reason) {
  const auto account = LoadPublisherAccount(state_root, publisher);
  if (account.state != PublisherAccountState::kSignedIn) return true;
  std::string text = "{\"version\":1,\"state\":\"expired\",\"display\":" + JsonString(account.display) +
                     ",\"obtained_at\":" + std::to_string(account.obtained_at) + "}";
  return Write(state_root, publisher, text, reason);
}

bool SignOutPublisherAccount(const std::string& state_root, Publisher publisher, std::string* reason) {
  const Storage storage(state_root);
  const auto name = PublisherAccountFileName(publisher);
  const auto removed = storage.Remove(name);
  const auto leftover = storage.Remove(name + ".tmp");  // a write that failed half-way
  if (!removed.ok()) return Fail(reason, "Playden couldn't remove the account: " + removed.message());
  return leftover.ok() || Fail(reason, "Playden couldn't remove the account: " + leftover.message());
}

bool ApplyPublisherSessionStatus(const std::string& state_root, Publisher publisher, const std::string& title_folder,
                                 const PublisherStatusSpec& status, bool* expired, std::string* reason,
                                 bool* missing) {
  *expired = false;
  if (missing) *missing = false;
  std::vector<std::string> parts;
  for (std::size_t begin = 0;;) {
    const auto end = status.file.find('/', begin);
    parts.push_back(status.file.substr(begin, end == std::string::npos ? std::string::npos : end - begin));
    if (parts.back().empty() || parts.back() == "." || parts.back() == "..")
      return Fail(reason, "the status file path is not safe");
    if (end == std::string::npos) break;
    begin = end + 1;
  }
  // Path-based calls only (the PS5 title can't use openat): every folder from the title folder
  // down is checked with lstat so a link or file is never followed.
  struct stat info {};
  const auto reasoned = [&](const std::string& text, int error) {
    return Fail(reason, text + " (" + std::strerror(error) + ")");
  };
  if (::lstat(title_folder.c_str(), &info) != 0)
    return errno == ENOENT || reasoned("cannot open the title folder", errno);
  if (!S_ISDIR(info.st_mode)) return Fail(reason, "cannot open the title folder (not a plain folder)");
  std::string path = title_folder;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    path += (!path.empty() && path.back() == '/' ? "" : "/") + parts[i];
    if (i + 1 == parts.size()) break;
    if (::lstat(path.c_str(), &info) != 0)
      return errno == ENOENT || reasoned("the status file's folder is not a plain folder", errno);
    if (!S_ISDIR(info.st_mode)) return Fail(reason, "the status file's folder is not a plain folder");
  }
  const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) return errno == ENOENT || reasoned("cannot open the status file", errno);
  std::string text;
  bool ok = ::fstat(fd, &info) == 0 && S_ISREG(info.st_mode) &&
            static_cast<std::uint64_t>(info.st_size) <= kMaxStatusBytes;
  if (ok) {
    text.resize(static_cast<std::size_t>(info.st_size));
    std::size_t at = 0;
    while (at < text.size()) {
      const auto count = ::read(fd, text.data() + at, text.size() - at);
      if (count < 0 && errno == EINTR) continue;
      if (count <= 0) break;
      at += static_cast<std::size_t>(count);
    }
    ok = at == text.size();
  }
  ::close(fd);
  if (!ok) return Fail(reason, "cannot read the status file");
  const auto first = text.find_first_not_of(" \t\r\n");
  const auto last = text.find_last_not_of(" \t\r\n");
  const auto trimmed = first == std::string::npos ? std::string() : text.substr(first, last - first + 1);
  if (trimmed == status.expired) {
    if (!MarkPublisherAccountExpired(state_root, publisher, reason))
      return false;  // the file stays for the next session import
    *expired = true;
  }
  if (::unlink(path.c_str()) != 0 && errno != ENOENT) return reasoned("cannot remove the status file", errno);
  // Said once: a status file that stays would say it again at every read.
  if (missing && !status.missing.empty() && trimmed == status.missing) *missing = true;
  return true;
}

}  // namespace playden::runtime
