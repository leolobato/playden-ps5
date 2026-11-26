#include "runtime/publisher_account.h"

#include "platform/storage.h"
#include "steam/backend/json.h"
#include "steam/secret.h"

#include <algorithm>
#include <cctype>
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

// A parsed value written back as JSON (numbers keep their text; strings are escaped like JsonString).
void Serialize(const Value& value, std::string& out) {
  switch (value.type()) {
    case Value::Type::kNull: out += "null"; return;
    case Value::Type::kBool: out += value.boolean() ? "true" : "false"; return;
    case Value::Type::kNumber: out += value.text(); return;
    case Value::Type::kString: {
      auto quoted = JsonString(value.text());
      out += quoted;
      ZeroizeSecret(quoted);
      return;
    }
    case Value::Type::kArray: {
      out += '[';
      bool first = true;
      for (const auto& item : value.array()) {
        if (!first) out += ',';
        first = false;
        Serialize(item, out);
      }
      out += ']';
      return;
    }
    case Value::Type::kObject: {
      out += '{';
      bool first = true;
      for (const auto& [key, item] : value.object()) {
        if (!first) out += ',';
        first = false;
        out += JsonString(key) + ':';
        Serialize(item, out);
      }
      out += '}';
      return;
    }
  }
}

// "Name" or "Object.Name" inside a blob (nullptr when absent).
const Value* FindField(const Value& blob, const std::string& field) {
  const auto dot = field.find('.');
  if (dot == std::string::npos) return blob.Find(field);
  const auto* parent = blob.Find(std::string_view(field).substr(0, dot));
  return parent && parent->is_object() ? parent->Find(std::string_view(field).substr(dot + 1)) : nullptr;
}

std::string_view Trim(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
  return text;
}

// A cookie value Playden sends and keeps as is: printable ASCII without space, '"', ',', ';' or '\'.
bool CookieValue(std::string_view value) {
  return !value.empty() && value.size() <= 8192 && std::all_of(value.begin(), value.end(), [](unsigned char c) {
    return c > 0x20 && c < 0x7f && c != '"' && c != ',' && c != ';' && c != '\\';
  });
}

bool SameText(std::string_view a, std::string_view b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](unsigned char x, unsigned char y) {
    return std::tolower(x) == std::tolower(y);
  });
}

// The blob with the refreshed members: top-level fields replaced (or added) from `top`, and
// Object.Name fields replaced inside their objects from `nested`. Values are JSON text.
std::string Rewrite(const Value& blob, const std::vector<std::pair<std::string, std::string>>& top,
                    const std::vector<std::pair<std::string, std::string>>& nested) {
  const auto lookup = [](const std::vector<std::pair<std::string, std::string>>& list, std::string_view key) {
    const auto found = std::find_if(list.begin(), list.end(), [&](const auto& entry) { return entry.first == key; });
    return found == list.end() ? nullptr : &found->second;
  };
  std::string out = "{";
  bool first = true;
  const auto member = [&](const std::string& key) {
    if (!first) out += ',';
    first = false;
    out += JsonString(key) + ':';
  };
  for (const auto& [key, value] : blob.object()) {
    member(key);
    if (const auto* replaced = lookup(top, key)) {
      out += *replaced;
      continue;
    }
    if (!value.is_object()) {
      Serialize(value, out);
      continue;
    }
    // An object some nested fields live in: its members, the renewed ones replaced.
    out += '{';
    bool inner_first = true;
    for (const auto& [inner_key, inner] : value.object()) {
      if (!inner_first) out += ',';
      inner_first = false;
      out += JsonString(inner_key) + ':';
      if (const auto* renewed = lookup(nested, key + "." + inner_key)) out += *renewed;
      else Serialize(inner, out);
    }
    out += '}';
  }
  for (const auto& [key, value] : top)
    if (!blob.Find(key)) {
      member(key);
      out += value;
    }
  return out + "}";
}

}  // namespace

const char* PublisherRefreshResultName(PublisherRefreshResult result) {
  switch (result) {
    case PublisherRefreshResult::kRefreshed: return "refreshed";
    case PublisherRefreshResult::kExpired: return "expired";
    case PublisherRefreshResult::kFailed: return "failed";
    case PublisherRefreshResult::kNotRefreshable: return "not_refreshable";
    case PublisherRefreshResult::kSignedOut: return "signed_out";
  }
  return "failed";
}

const char* PublisherSlug(Publisher publisher) {
  switch (publisher) {
    case Publisher::kRockstar: return "rockstar";
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
  const auto* renewed = document.value->Find("refreshed_at");
  account.refreshed = renewed && renewed->AsUint64();
  account.renewed_at = account.refreshed ? *renewed->AsUint64() : account.obtained_at;
  return account;
}

PublisherRefreshOutcome RefreshPublisherAccount(const std::string& state_root, Publisher publisher,
                                                const PublisherAccountSpec& account, const PublisherRefreshSpec& refresh,
                                                const PublisherRefreshTransport& transport, std::uint64_t now) {
  PublisherRefreshOutcome outcome;
  const auto finish = [&](PublisherRefreshResult result, std::string reason) {
    outcome.result = result;
    outcome.reason = std::move(reason);
    return outcome;
  };
  // The account as it was when the request left: written over only if it is still the same.
  PublisherAccount before;
  PublisherRefreshRequest request;
  {
    WipedJson document;
    if (!Read(state_root, publisher, &document) || document.value->Find("state")->text() != "signed_in")
      return finish(PublisherRefreshResult::kSignedOut, "no signed-in account");
    const auto& blob = *document.value->Find("blob");
    std::string cookie;
    for (const auto& [name, field] : refresh.cookies) {
      const auto* value = FindField(blob, field);
      if (!value || !value->is_string() || !CookieValue(value->text())) {
        ZeroizeSecret(cookie);
        return finish(PublisherRefreshResult::kNotRefreshable, "the sign-in has no " + name + " to renew it with");
      }
      cookie += (cookie.empty() ? "" : "; ") + name + "=" + value->text();
    }
    request.url = refresh.url;
    request.headers = refresh.headers;
    if (!cookie.empty()) request.headers.emplace_back("Cookie", std::move(cookie));
    ZeroizeSecret(cookie);
  }
  before = LoadPublisherAccount(state_root, publisher);
  auto answer = transport ? transport(request) : PublisherRefreshAnswer{};
  for (auto& [name, value] : request.headers) ZeroizeSecret(value);
  struct WipeAnswer {
    PublisherRefreshAnswer& answer;
    ~WipeAnswer() {
      ZeroizeSecret(answer.body);
      for (auto& [name, value] : answer.headers) ZeroizeSecret(value);
    }
  } wipe_answer{answer};
  outcome.status = answer.status;
  if (!answer.reached) return finish(PublisherRefreshResult::kFailed, "no answer");
  const auto unchanged = [&] {
    const auto now_account = LoadPublisherAccount(state_root, publisher);
    return now_account.state == PublisherAccountState::kSignedIn && now_account.obtained_at == before.obtained_at &&
           now_account.renewed_at == before.renewed_at;
  };
  if (std::find(refresh.expired_statuses.begin(), refresh.expired_statuses.end(), answer.status) !=
      refresh.expired_statuses.end()) {
    if (!unchanged()) return finish(PublisherRefreshResult::kFailed, "the account changed during the refresh");
    std::string reason;
    if (!MarkPublisherAccountExpired(state_root, publisher, &reason)) return finish(PublisherRefreshResult::kFailed, reason);
    return finish(PublisherRefreshResult::kExpired, "the server no longer accepts the sign-in");
  }
  if (answer.status != 200) return finish(PublisherRefreshResult::kFailed, "unexpected answer");

  json::Limits limits;
  limits.max_bytes = account.max_bytes;
  limits.max_values = 16000;
  limits.max_depth = 15;
  WipedJson parsed;
  parsed.value = json::Parse(answer.body, limits);
  if (!parsed.value || !parsed.value->is_object() || !UniqueKeys(*parsed.value))
    return finish(PublisherRefreshResult::kFailed, "the answer isn't a JSON object");
  if (!refresh.ok_field.empty()) {
    const auto* ok = parsed.value->Find(refresh.ok_field);
    if (!ok || !ok->is_bool() || !ok->boolean()) return finish(PublisherRefreshResult::kFailed, "the answer says it failed");
  }

  // The renewed values, as JSON text: answer fields at the top, cookies where the recipe keeps them.
  std::vector<std::pair<std::string, std::string>> top, nested;
  struct WipeList {
    std::vector<std::pair<std::string, std::string>>& list;
    ~WipeList() {
      for (auto& [name, value] : list) ZeroizeSecret(value);
    }
  } wipe_top{top}, wipe_nested{nested};
  for (const auto& field : refresh.fields) {
    const auto* value = parsed.value->Find(field);
    if (!value) continue;
    std::string text;
    Serialize(*value, text);
    top.emplace_back(field, std::move(text));
  }
  for (const auto& [name, field] : refresh.cookies) {
    std::string value;
    for (const auto& [header, line] : answer.headers) {
      if (!SameText(header, "set-cookie")) continue;
      const auto equals = line.find('=');
      if (equals == std::string::npos || Trim(std::string_view(line).substr(0, equals)) != name) continue;
      const auto end = line.find(';', equals + 1);
      ZeroizeSecret(value);
      value = std::string(Trim(std::string_view(line).substr(equals + 1, end == std::string::npos ? std::string::npos
                                                                                                   : end - equals - 1)));
    }
    if (!CookieValue(value)) {
      ZeroizeSecret(value);
      return finish(PublisherRefreshResult::kFailed, "the answer didn't renew " + name);
    }
    auto text = JsonString(value);
    ZeroizeSecret(value);
    if (field.find('.') == std::string::npos) top.emplace_back(field, std::move(text));
    else nested.emplace_back(field, std::move(text));
  }

  // The renewed sign-in must still be one Playden accepts.
  std::string blob;
  {
    WipedJson document;
    if (!Read(state_root, publisher, &document) || document.value->Find("state")->text() != "signed_in")
      return finish(PublisherRefreshResult::kFailed, "the account changed during the refresh");
    blob = Rewrite(*document.value->Find("blob"), top, nested);
  }
  struct WipeText {
    std::string& text;
    ~WipeText() { ZeroizeSecret(text); }
  } wipe_blob{blob};
  if (blob.size() > account.max_bytes) return finish(PublisherRefreshResult::kFailed, "the renewed sign-in is too large");
  {
    WipedJson renewed;
    renewed.value = json::Parse(blob, limits);
    const auto* stage = renewed.value && renewed.value->is_object() ? FirstString(*renewed.value, account.stage_fields)
                                                                   : nullptr;
    if (!stage || !StageValueMatches(stage->text(), account))
      return finish(PublisherRefreshResult::kFailed, "the answer has no usable " + account.name + " credential");
  }
  if (!unchanged()) return finish(PublisherRefreshResult::kFailed, "the account changed during the refresh");
  std::string text = "{\"version\":1,\"obtained_at\":" + std::to_string(before.obtained_at) +
                     ",\"refreshed_at\":" + std::to_string(now) + ",\"state\":\"signed_in\",\"display\":" +
                     JsonString(before.display) + ",\"blob\":" + blob + "}";
  std::string reason;
  if (!Write(state_root, publisher, text, &reason)) return finish(PublisherRefreshResult::kFailed, reason);
  return finish(PublisherRefreshResult::kRefreshed, {});
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
  // The second line, in the same file so it is written, renewed and removed with the credential. One that isn't a
  // token is left out: the publisher then refuses the sign-in, which the status says.
  for (const auto& name : spec.stage_extra_fields) {
    const auto* extra = document.value->Find("blob")->Find(name);
    if (!extra || !extra->is_string() || extra->text().empty()) continue;
    const auto& text = extra->text();
    if (text.size() <= 1024 &&
        std::all_of(text.begin(), text.end(), [](unsigned char c) { return c > 0x20 && c < 0x7f; }))
      *value += "\n" + text;
    break;
  }
  return true;
}

bool MarkPublisherAccountExpired(const std::string& state_root, Publisher publisher, std::string* reason) {
  const auto account = LoadPublisherAccount(state_root, publisher);
  if (account.state != PublisherAccountState::kSignedIn) return true;
  std::string text = "{\"version\":1,\"state\":\"expired\",\"display\":" + JsonString(account.display) +
                     ",\"obtained_at\":" + std::to_string(account.obtained_at) + "}";
  return Write(state_root, publisher, text, reason);
}

std::string PublisherUntestedFileName(Publisher publisher) {
  return std::string("publisher-untested-") + PublisherSlug(publisher) + ".v1";
}

namespace {
constexpr std::size_t kMaxUntestedBytes = 16 * 1024;

std::set<std::uint32_t> UntestedSeen(const std::string& state_root, Publisher publisher) {
  std::set<std::uint32_t> seen;
  const Storage storage(state_root);
  const auto bytes = storage.Read(PublisherUntestedFileName(publisher), kMaxUntestedBytes);
  if (!bytes.ok()) return seen;
  const std::string text(bytes.value().begin(), bytes.value().end());
  for (std::size_t begin = 0; begin < text.size();) {
    auto end = text.find('\n', begin);
    if (end == std::string::npos) end = text.size();
    const auto line = text.substr(begin, end - begin);
    begin = end + 1;
    const bool digits = !line.empty() && line.size() <= 10 && line[0] != '0' &&
                        std::all_of(line.begin(), line.end(), [](char c) { return c >= '0' && c <= '9'; });
    if (!digits) return {};  // damaged: the notice shows again
    const auto value = std::stoull(line);
    if (value > 0xffffffffull) return {};
    seen.insert(static_cast<std::uint32_t>(value));
  }
  return seen;
}
}  // namespace

bool PublisherUntestedNoticeSeen(const std::string& state_root, Publisher publisher, std::uint32_t app) {
  return UntestedSeen(state_root, publisher).count(app) != 0;
}

bool NotePublisherUntestedNoticeSeen(const std::string& state_root, Publisher publisher, std::uint32_t app,
                                     std::string* reason) {
  auto seen = UntestedSeen(state_root, publisher);
  if (!seen.insert(app).second) return true;
  // Not a secret: open like Playden's other data (#868).
  std::string text;
  for (const auto id : seen) text += std::to_string(id) + "\n";
  const Storage storage(state_root);
  const Bytes bytes(text.begin(), text.end());
  const auto root = storage.EnsureRootDirectory();
  const auto written = root.ok() ? storage.WriteAtomicOpen(PublisherUntestedFileName(publisher), bytes) : root;
  return written.ok() || Fail(reason, "Playden couldn't remember the notice: " + written.message());
}

bool SignOutPublisherAccount(const std::string& state_root, Publisher publisher, std::string* reason) {
  const Storage storage(state_root);
  const auto name = PublisherAccountFileName(publisher);
  const auto removed = storage.Remove(name);
  const auto leftover = storage.Remove(name + ".tmp");  // a write that failed half-way
  if (!removed.ok()) return Fail(reason, "Playden couldn't remove the account: " + removed.message());
  return leftover.ok() || Fail(reason, "Playden couldn't remove the account: " + leftover.message());
}

namespace {

// <title_folder>/<file>, every folder from the title folder down checked with lstat so a link or file is
// never followed (path-based calls only: the PS5 title can't use openat). *path is "" when a folder on the
// way is missing.
bool StatusPath(const std::string& title_folder, const std::string& file, std::string* path, std::string* reason) {
  path->clear();
  std::vector<std::string> parts;
  for (std::size_t begin = 0;;) {
    const auto end = file.find('/', begin);
    parts.push_back(file.substr(begin, end == std::string::npos ? std::string::npos : end - begin));
    if (parts.back().empty() || parts.back() == "." || parts.back() == "..")
      return Fail(reason, "the status file path is not safe");
    if (end == std::string::npos) break;
    begin = end + 1;
  }
  struct stat info {};
  const auto reasoned = [&](const std::string& text, int error) {
    return Fail(reason, text + " (" + std::strerror(error) + ")");
  };
  if (::lstat(title_folder.c_str(), &info) != 0)
    return errno == ENOENT || reasoned("cannot open the title folder", errno);
  if (!S_ISDIR(info.st_mode)) return Fail(reason, "cannot open the title folder (not a plain folder)");
  std::string at = title_folder;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    at += (!at.empty() && at.back() == '/' ? "" : "/") + parts[i];
    if (i + 1 == parts.size()) break;
    if (::lstat(at.c_str(), &info) != 0)
      return errno == ENOENT || reasoned("the status file's folder is not a plain folder", errno);
    if (!S_ISDIR(info.st_mode)) return Fail(reason, "the status file's folder is not a plain folder");
  }
  *path = at;
  return true;
}

// The whole file, up to kMaxStatusBytes; *found is false (and the call succeeds) when it doesn't exist.
bool ReadStatusFile(const std::string& path, std::string* text, bool* found, std::string* reason,
                    std::uint64_t* written_at = nullptr) {
  *found = false;
  text->clear();
  const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) return errno == ENOENT || Fail(reason, std::string("cannot open the status file (") + std::strerror(errno) + ")");
  struct stat info {};
  bool ok = ::fstat(fd, &info) == 0 && S_ISREG(info.st_mode) &&
            static_cast<std::uint64_t>(info.st_size) <= kMaxStatusBytes;
  if (ok) {
    text->resize(static_cast<std::size_t>(info.st_size));
    std::size_t at = 0;
    while (at < text->size()) {
      const auto count = ::read(fd, text->data() + at, text->size() - at);
      if (count < 0 && errno == EINTR) continue;
      if (count <= 0) break;
      at += static_cast<std::size_t>(count);
    }
    ok = at == text->size();
  }
  ::close(fd);
  if (!ok) return Fail(reason, "cannot read the status file");
  if (written_at) *written_at = info.st_mtime > 0 ? static_cast<std::uint64_t>(info.st_mtime) : 0;
  *found = true;
  return true;
}

bool RemoveStatusFile(const std::string& path, std::string* reason) {
  if (path.empty() || ::unlink(path.c_str()) == 0 || errno == ENOENT) return true;
  return Fail(reason, std::string("cannot remove the status file (") + std::strerror(errno) + ")");
}

}  // namespace

bool ReadPublisherSessionStatus(const std::string& title_folder, const PublisherStatusSpec& status,
                                PublisherSessionStatus* session, std::string* reason) {
  *session = {};
  std::string path, text;
  bool found = false;
  if (!StatusPath(title_folder, status.file, &path, reason)) return false;
  if (path.empty() || !ReadStatusFile(path, &text, &found, reason, &session->written_at)) return path.empty();
  if (!found) return true;
  const auto first = text.find_first_not_of(" \t\r\n");
  const auto last = text.find_last_not_of(" \t\r\n");
  const auto trimmed = first == std::string::npos ? std::string() : text.substr(first, last - first + 1);
  if (trimmed == status.expired) session->verdict = PublisherSessionVerdict::kExpired;
  else if (!status.missing.empty() && trimmed == status.missing) session->verdict = PublisherSessionVerdict::kMissing;
  else if (!status.refused.empty() && trimmed == status.refused) session->verdict = PublisherSessionVerdict::kRefused;
  // The detail is only ever logged: an unreadable one is left out, never a failure.
  std::string detail_path, detail, ignored;
  bool detail_found = false;
  if (!status.detail.empty() && StatusPath(title_folder, status.detail, &detail_path, &ignored) &&
      !detail_path.empty() && ReadStatusFile(detail_path, &detail, &detail_found, &ignored) && detail_found) {
    const auto line = detail.substr(0, detail.find_first_of("\r\n"));
    for (const unsigned char c : line) {
      if (session->detail.size() >= 160) break;
      if (std::isalnum(c) || c == ' ' || std::string_view("=._:-").find(static_cast<char>(c)) != std::string_view::npos)
        session->detail += static_cast<char>(c);
    }
    const auto end = session->detail.find_last_not_of(' ');
    session->detail.erase(end == std::string::npos ? 0 : end + 1);
    // The log redaction takes a code= of 16 characters or more for a sign-in code (#1252: Rockstar's
    // code=AuthenticationFailed came out as code=<redacted>): the publisher's error code is logged as error=.
    for (std::size_t at = 0; (at = session->detail.find("code=", at)) != std::string::npos;) {
      if (at == 0 || session->detail[at - 1] == ' ') session->detail.replace(at, 4, "error");
      at += 5;
    }
  }
  return true;
}

bool RemovePublisherSessionStatus(const std::string& title_folder, const PublisherStatusSpec& status,
                                  std::string* reason) {
  std::string path;
  if (!StatusPath(title_folder, status.file, &path, reason) || !RemoveStatusFile(path, reason)) return false;
  // A detail file that stays is replaced at the next start, and only ever logged.
  std::string ignored;
  if (!status.detail.empty() && StatusPath(title_folder, status.detail, &path, &ignored)) RemoveStatusFile(path, &ignored);
  return true;
}

bool ApplyPublisherSessionStatus(const std::string& state_root, Publisher publisher, const std::string& title_folder,
                                 const PublisherStatusSpec& status, bool* expired, std::string* reason,
                                 bool* missing) {
  *expired = false;
  if (missing) *missing = false;
  PublisherSessionStatus session;
  if (!ReadPublisherSessionStatus(title_folder, status, &session, reason)) return false;
  if (session.verdict == PublisherSessionVerdict::kExpired) {
    if (!MarkPublisherAccountExpired(state_root, publisher, reason)) return false;  // the file stays for the next import
    *expired = true;
  }
  if (!RemovePublisherSessionStatus(title_folder, status, reason)) return false;
  // Said once: a status file that stays would say it again at every read.
  if (missing && session.verdict == PublisherSessionVerdict::kMissing) *missing = true;
  return true;
}

}  // namespace playden::runtime
