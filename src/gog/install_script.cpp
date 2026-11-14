// SPDX-License-Identifier: GPL-3.0-or-later
#include "gog/install_script.h"

#include "gog/languages.h"
#include "gog/path_rules.h"
#include "platform/directory_walk.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <dirent.h>
#include <fcntl.h>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace playden::gog {
namespace {

using Value = steam::json::Value;

constexpr std::string_view kBom = "\xEF\xBB\xBF";
constexpr std::size_t kMaxActions = 1024;
constexpr std::size_t kMaxCopyDepth = 32;
constexpr std::size_t kMaxCopyEntries = 100000;
constexpr std::size_t kCopyBuffer = 64 * 1024;

char Lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; }

bool SameIgnoringCase(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i)
    if (Lower(a[i]) != Lower(b[i])) return false;
  return true;
}

bool HasControl(std::string_view text) {
  for (const char c : text)
    if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) return true;
  return false;
}

void ReplaceAll(std::string* text, std::string_view from, std::string_view to) {
  for (std::size_t at = 0; (at = text->find(from, at)) != std::string::npos; at += to.size())
    text->replace(at, from.size(), to);
}

const std::string* StringArgument(const Value& arguments, std::string_view key) {
  const auto* value = arguments.Find(key);
  return value && value->is_string() ? &value->text() : nullptr;
}

bool FlagArgument(const Value& arguments, std::string_view key) {
  const auto* value = arguments.Find(key);
  return value && value->is_bool() && value->boolean();
}

bool ForLanguage(const ScriptAction& action, const ScriptContext& context) {
  if (action.languages.empty()) return true;
  for (const auto& language : action.languages)
    if (language == "*" || SameIgnoringCase(language, context.gog_language)) return true;
  // GOG's deprecated codes ("en" for "en-US") through the Steam language they stand for.
  const auto steam = SteamCodeFor(context.gog_language);
  return !steam.empty() && LanguageMatches(action.languages, steam);
}

// `{app}…` or `{supportDir}…` as a checked location; nothing for anything else. `bare` allows the folder itself.
std::optional<ScriptLocation> Locate(std::string_view raw, const ScriptContext& context, bool bare) {
  constexpr std::string_view kApp = "{app}";
  constexpr std::string_view kSupport = "{supportDir}";
  ScriptLocation location;
  std::string rest;
  if (raw.substr(0, kApp.size()) == kApp) {
    rest = std::string(raw.substr(kApp.size()));
  } else if (raw.substr(0, kSupport.size()) == kSupport) {
    rest = std::string(raw.substr(kSupport.size()));
    location.support = true;
  } else {
    return std::nullopt;
  }
  if (!rest.empty() && rest.front() != '\\' && rest.front() != '/') return std::nullopt;  // "{app}saves"
  ReplaceAll(&rest, "{productID}", context.product);
  if (rest.find('{') != std::string::npos || rest.find('}') != std::string::npos) return std::nullopt;
  while (!rest.empty() && (rest.back() == '\\' || rest.back() == '/')) rest.pop_back();
  std::string relative;
  if (!rest.empty()) {
    const auto normalized = NormalizePath(rest);
    if (!normalized) return std::nullopt;
    relative = *normalized;
  } else if (!bare) {
    return std::nullopt;
  }
  // The support folder sits under the game folder: one base, so every component below it is checked.
  const auto& game = context.game_root;
  if (!location.support) {
    location.base = game;
    location.relative = relative;
  } else if (context.support_root.size() > game.size() + 1 && context.support_root.compare(0, game.size(), game) == 0 &&
             context.support_root[game.size()] == '/') {
    location.base = game;
    location.relative = context.support_root.substr(game.size() + 1) + (relative.empty() ? "" : "/" + relative);
  } else {
    location.base = context.support_root;
    location.relative = relative;
  }
  if (location.base.empty() || location.base.front() != '/') return std::nullopt;
  return location;
}

// Decodes UTF-8 and writes it as Latin-1 when every character fits; otherwise the text stays as it is.
std::string Latin1IfFits(const std::string& text) {
  std::string out;
  for (std::size_t i = 0; i < text.size();) {
    const auto lead = static_cast<unsigned char>(text[i]);
    if (lead < 0x80) {
      out += static_cast<char>(lead);
      ++i;
    } else if ((lead == 0xC2 || lead == 0xC3) && i + 1 < text.size() &&
               (static_cast<unsigned char>(text[i + 1]) & 0xC0) == 0x80) {
      out += static_cast<char>(((lead & 0x1F) << 6) | (static_cast<unsigned char>(text[i + 1]) & 0x3F));
      i += 2;
    } else {
      return text;
    }
  }
  return out;
}

std::string_view Trim(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) text.remove_suffix(1);
  return text;
}

struct Line {
  std::size_t begin = 0;
  std::size_t end = 0;  // before the line ending
};

std::vector<Line> Lines(const std::string& text) {
  std::vector<Line> lines;
  for (std::size_t begin = 0; begin < text.size();) {
    const auto newline = text.find('\n', begin);
    const auto stop = newline == std::string::npos ? text.size() : newline;
    lines.push_back({begin, stop > begin && text[stop - 1] == '\r' ? stop - 1 : stop});
    begin = newline == std::string::npos ? text.size() : newline + 1;
  }
  return lines;
}

std::optional<std::string_view> Header(std::string_view line) {
  line = Trim(line);
  if (line.size() < 2 || line.front() != '[') return std::nullopt;
  const auto close = line.find(']');
  if (close == std::string_view::npos) return std::nullopt;
  return Trim(line.substr(1, close - 1));
}

// ---- Files -----------------------------------------------------------------------------------------------------

enum class Found { kMissing, kFile, kFolder, kLink, kOther, kError };

Found Look(const std::string& path) {
  struct stat info {};
  if (::lstat(path.c_str(), &info) != 0) return errno == ENOENT ? Found::kMissing : Found::kError;
  if (S_ISLNK(info.st_mode)) return Found::kLink;
  if (S_ISDIR(info.st_mode)) return Found::kFolder;
  if (S_ISREG(info.st_mode)) return Found::kFile;
  return Found::kOther;
}

// `name` in `folder`, spelled as the folder's listing has it: the exact name first, else one matching ignoring ASCII
// case (a case-sensitive volume, such as the console's internal storage). Always by the listing, also where the volume
// ignores case (exFAT): the console's walk by path checks each name it opens against the listing, exactly. The
// script's spelling when there is none.
std::pair<std::string, Found> Entry(const std::string& folder, const std::string& name) {
  DIR* listing = ::opendir(folder.c_str());
  if (!listing) return {name, Look(folder + "/" + name)};
  std::optional<std::string> match;
  while (const auto* entry = ::readdir(listing)) {
    const std::string_view listed = entry->d_name;
    if (listed == name) {
      match = std::string(listed);
      break;
    }
    if (!match && listed != "." && listed != ".." && SameIgnoringCase(listed, name)) match = std::string(listed);
  }
  ::closedir(listing);
  if (!match) {
    // Not listed: missing, unless the volume finds it by another spelling the listing didn't show.
    const auto exact = Look(folder + "/" + name);
    return {name, exact};
  }
  return {*match, Look(folder + "/" + *match)};
}

std::vector<std::string> Components(const std::string& relative) {
  std::vector<std::string> parts;
  for (std::size_t start = 0; start < relative.size();) {
    const auto end = std::min(relative.find('/', start), relative.size());
    parts.push_back(relative.substr(start, end - start));
    start = end + 1;
  }
  return parts;
}

enum class Walk { kOk, kMissing, kRefused, kError };

// The folder `relative` under `base`, every component a real folder (lstat) and matched ignoring case. With `make`,
// missing folders are made (0777); `made` says whether one was. `path` gets the folder's absolute path.
Walk Folder(const std::string& base, const std::string& relative, bool make, std::string* path, bool* made = nullptr) {
  if (Look(base) != Found::kFolder) return Walk::kRefused;
  std::string cursor = base;
  for (const auto& part : Components(relative)) {
    const auto [name, found] = Entry(cursor, part);
    if (found == Found::kMissing) {
      if (!make) return Walk::kMissing;
      if (::mkdir((cursor + "/" + name).c_str(), 0777) != 0 && errno != EEXIST) return Walk::kError;
      if (Look(cursor + "/" + name) != Found::kFolder) return Walk::kRefused;
      if (made) *made = true;
    } else if (found != Found::kFolder) {
      return found == Found::kError ? Walk::kError : Walk::kRefused;
    }
    cursor += "/" + name;
  }
  *path = cursor;
  return Walk::kOk;
}

bool WriteAll(int fd, const char* data, std::size_t size) {
  while (size > 0) {
    const auto count = ::write(fd, data, size);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) return false;
    data += count;
    size -= static_cast<std::size_t>(count);
  }
  return true;
}

// A file written as a synced temporary file in the same folder, then renamed over `name`. The temporary file gets
// `mode`; a file system without modes (exFAT) refuses the chmod and keeps its own, which is fine.
template <typename Fill>
bool WriteReplacing(const std::string& folder, const std::string& name, mode_t mode, Fill fill) {
  const auto temporary = folder + "/.playden-" + name + ".tmp";
  ::unlink(temporary.c_str());
  const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (fd < 0) return false;
  bool ok = ::fchmod(fd, mode) == 0 || errno == EPERM || errno == ENOTSUP || errno == EINVAL;
  ok = ok && fill(fd);
  ok = ::fsync(fd) == 0 && ok;
  ok = ::close(fd) == 0 && ok;
  if (!ok || ::rename(temporary.c_str(), (folder + "/" + name).c_str()) != 0) {
    ::unlink(temporary.c_str());
    return false;
  }
  return true;
}

class Runner {
 public:
  RunResult result;

  void Fail(const ScriptStep& step, const std::string& sentence) {
    ++result.failed;
    if (result.failure.empty())
      result.failure = sentence + " (" + (step.name.empty() ? std::string("step") : step.name) + ")";
  }

  static std::string Where(const ScriptLocation& location) {
    return location.relative.empty() ? std::string(location.support ? "the support folder" : "the game folder")
                                     : location.relative;
  }

  void CreateFolder(const ScriptStep& step) {
    std::string path;
    bool made = false;
    const auto walk = Folder(step.target.base, step.target.relative, true, &path, &made);
    if (walk != Walk::kOk) return Fail(step, "Could not make the folder " + Where(step.target) + ".");
    if (made) ++result.changed;
  }

  void CopyFolder(const ScriptStep& step) {
    std::string source;
    const auto found = Folder(step.source.base, step.source.relative, false, &source);
    if (found == Walk::kMissing) {
      ++result.skipped;
      return;
    }
    if (found != Walk::kOk) return Fail(step, "Could not read the folder " + Where(step.source) + ".");
    // Never into the source itself, which the walk would then find again.
    const auto folded_source = FoldCase(step.source.relative);
    const auto folded_target = FoldCase(step.target.relative);
    if (step.source.base == step.target.base &&
        (folded_target == folded_source ||
         (folded_target.size() > folded_source.size() &&
          folded_target.compare(0, folded_source.size() + 1, folded_source + "/") == 0) ||
         folded_source.empty()))
      return Fail(step, "Could not copy " + Where(step.source) + " into itself.");
    buffer_.reset(new char[kCopyBuffer]);
    bool changed = false;
    bool ok = true;
    // Folders still to copy, by path: each is opened from the root when its turn comes (OpenDirectoryWalk checks every
    // component again), so one folder descriptor is open at a time however wide the source is.
    struct Pending {
      std::string path;      // absolute, in the listing's spelling
      std::string relative;  // under the source
      std::size_t depth;
    };
    std::vector<Pending> pending{{source, std::string(), 0}};
    std::size_t entries = 0;
    while (ok && !pending.empty()) {
      auto folder = std::move(pending.back());
      pending.pop_back();
      const int fd = platform::OpenDirectoryWalk(folder.path);
      if (fd < 0) {
        ok = false;
        break;
      }
      // Each name with the inode the listing gives it: the walk checks what it opens against it. (Its own listing of
      // the parent would share this descriptor's offset, already at the end.)
      std::vector<std::pair<std::string, std::uint64_t>> names;
      const int listing_fd = ::dup(fd);
      DIR* listing = listing_fd >= 0 ? ::fdopendir(listing_fd) : nullptr;
      if (!listing) {
        if (listing_fd >= 0) ::close(listing_fd);
        ok = false;
      } else {
        while (const auto* entry = ::readdir(listing)) {
          const std::string name = entry->d_name;
          if (name != "." && name != "..") names.emplace_back(name, static_cast<std::uint64_t>(entry->d_fileno));
        }
        ::closedir(listing);
      }
      for (const auto& [name, inode] : names) {
        if (!ok) break;
        if (++entries > kMaxCopyEntries) {
          ok = false;
          break;
        }
        const auto relative = folder.relative.empty() ? name : folder.relative + "/" + name;
        const auto kind = Look(folder.path + "/" + name);
        if (kind == Found::kFolder) {
          if (folder.depth + 1 > kMaxCopyDepth) {
            ok = false;
            break;
          }
          pending.push_back({folder.path + "/" + name, relative, folder.depth + 1});
        } else if (kind == Found::kFile) {
          const int file = platform::OpenWalkChildUnprivileged(fd, name.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC,
                                                              inode);
          if (file < 0) {
            ok = false;
            break;
          }
          struct stat info {};
          const bool regular = ::fstat(file, &info) == 0 && S_ISREG(info.st_mode);
          ok = regular && CopyFile(step, file, relative, info.st_mode & 0777, &changed);
          ::close(file);
        }
        // Links and anything else are not copied.
      }
      ::close(fd);
    }
    buffer_.reset();
    if (changed) ++result.changed;
    if (!ok) Fail(step, "Could not copy " + Where(step.source) + " to " + Where(step.target) + ".");
    else if (refused_) Fail(step, "Left out a file of " + Where(step.source) + " named as one of Playden's own.");
    refused_ = false;
  }

  void SetIni(const ScriptStep& step) {
    const auto slash = step.target.relative.rfind('/');
    const auto parent = slash == std::string::npos ? std::string() : step.target.relative.substr(0, slash);
    const auto file_name = slash == std::string::npos ? step.target.relative : step.target.relative.substr(slash + 1);
    std::string folder;
    if (file_name.empty() || Folder(step.target.base, parent, true, &folder) != Walk::kOk)
      return Fail(step, "Could not reach " + Where(step.target) + ".");
    const auto [name, found] = Entry(folder, file_name);
    if (found != Found::kMissing && found != Found::kFile) return Fail(step, "Could not write " + Where(step.target) + ".");
    std::string text;
    mode_t mode = 0666;
    if (found == Found::kFile && !ReadSmall(folder + "/" + name, &text, &mode))
      return Fail(step, "Could not read " + Where(step.target) + ".");
    const auto section = step.utf8 ? step.section : Latin1IfFits(step.section);
    const auto key = step.utf8 ? step.key : Latin1IfFits(step.key);
    const auto value = step.utf8 ? step.value : Latin1IfFits(step.value);
    const auto updated = SettingIni(text, section, key, value);
    if (found == Found::kFile && updated == text) return;
    if (!WriteReplacing(folder, name, mode, [&](int fd) { return WriteAll(fd, updated.data(), updated.size()); }))
      return Fail(step, "Could not write " + Where(step.target) + ".");
    ++result.changed;
  }

 private:
  std::unique_ptr<char[]> buffer_;
  bool refused_ = false;  // the copy left out a file named as one of Playden's own

  static bool ReadSmall(const std::string& path, std::string* text, mode_t* mode) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return false;
    struct stat info {};
    bool ok = ::fstat(fd, &info) == 0 && S_ISREG(info.st_mode);
    *mode = info.st_mode & 07777;
    char chunk[4096];
    while (ok) {
      const auto count = ::read(fd, chunk, sizeof(chunk));
      if (count < 0 && errno == EINTR) continue;
      if (count < 0 || text->size() + static_cast<std::size_t>(count) > kMaxIniBytes) ok = false;
      else if (count == 0) break;
      else text->append(chunk, static_cast<std::size_t>(count));
    }
    return ::close(fd) == 0 && ok;
  }

  // One file of a copy, `relative` under the source, into the step's target.
  bool CopyFile(const ScriptStep& step, int source, const std::string& relative, mode_t mode, bool* changed) {
    const auto destination =
        step.target.relative.empty() ? relative : step.target.relative + "/" + relative;
    // Never over Playden's own record or journals at the game folder's top.
    if (!step.target.support && IsReservedName(destination.substr(0, destination.find('/')))) {
      refused_ = true;
      return true;
    }
    const auto slash = destination.rfind('/');
    const auto parent = slash == std::string::npos ? std::string() : destination.substr(0, slash);
    const auto file_name = slash == std::string::npos ? destination : destination.substr(slash + 1);
    std::string folder;
    bool made = false;
    if (Folder(step.target.base, parent, true, &folder, &made) != Walk::kOk) return false;
    if (made) *changed = true;
    const auto [name, found] = Entry(folder, file_name);
    if (found == Found::kFile && !step.overwrite) return true;  // the player's or the game's own file stays
    if (found != Found::kFile && found != Found::kMissing) return false;
    if (!WriteReplacing(folder, name, mode == 0 ? 0666 : mode, [&](int fd) {
          if (::lseek(source, 0, SEEK_SET) != 0) return false;
          for (;;) {
            const auto count = ::read(source, buffer_.get(), kCopyBuffer);
            if (count < 0 && errno == EINTR) continue;
            if (count < 0) return false;
            if (count == 0) return true;
            if (!WriteAll(fd, buffer_.get(), static_cast<std::size_t>(count))) return false;
          }
        }))
      return false;
    *changed = true;
    return true;
  }
};

constexpr std::size_t kMaxRegistryText = 1024;

bool PrintableAscii(std::string_view text) {
  for (const char c : text)
    if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7e) return false;
  return true;
}

// `{productID}` and `{app}` filled in; nothing when another `{token}` is left or the text isn't printable ASCII.
std::optional<std::string> RegistryText(const std::string& raw, const ScriptContext& context) {
  std::string text = raw;
  ReplaceAll(&text, "{productID}", context.product);
  ReplaceAll(&text, "{app}", context.windows_app);
  if (text.find('{') != std::string::npos || text.find('}') != std::string::npos || !PrintableAscii(text))
    return std::nullopt;
  return text;
}

// A dword's data, decimal or 0x hex, as decimal text; nothing past 32 bits or for anything else.
std::optional<std::string> DwordData(const Value* data) {
  if (!data || (!data->is_string() && !data->is_number())) return std::nullopt;
  const std::string& text = data->text();
  const bool hex = text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X');
  const std::size_t start = hex ? 2 : 0;
  if (text.size() <= start) return std::nullopt;
  std::uint64_t number = 0;
  for (std::size_t i = start; i < text.size(); ++i) {
    const char c = text[i];
    unsigned digit = 0;
    if (c >= '0' && c <= '9') digit = static_cast<unsigned>(c - '0');
    else if (hex && c >= 'a' && c <= 'f') digit = static_cast<unsigned>(c - 'a' + 10);
    else if (hex && c >= 'A' && c <= 'F') digit = static_cast<unsigned>(c - 'A' + 10);
    else return std::nullopt;
    number = number * (hex ? 16 : 10) + digit;
    if (number > 0xffffffffull) return std::nullopt;
  }
  return std::to_string(number);
}

// One setRegistry action's setting; nothing when it can't be written.
std::optional<RegistrySetting> RegistryFrom(const Value& arguments, const ScriptContext& context) {
  const auto* root = StringArgument(arguments, "root");
  const auto* subkey = StringArgument(arguments, "subkey");
  if (!root || !subkey) return std::nullopt;
  RegistrySetting setting;
  if (SameIgnoringCase(*root, "HKEY_LOCAL_MACHINE")) setting.machine = true;
  else if (!SameIgnoringCase(*root, "HKEY_CURRENT_USER")) return std::nullopt;
  auto key = RegistryText(*subkey, context);
  if (!key) return std::nullopt;
  std::replace(key->begin(), key->end(), '/', '\\');
  for (const char c : *key) {
    if (c == '\\' && (setting.key.empty() || setting.key.back() == '\\')) continue;
    setting.key += c;
  }
  while (!setting.key.empty() && setting.key.back() == '\\') setting.key.pop_back();
  if (setting.key.empty() || setting.key.size() > kMaxRegistryText) return std::nullopt;
  const auto* type = StringArgument(arguments, "valueType");
  const auto* data = arguments.Find("valueData");
  // Only the key (no type, no data): the bottle's writer sets values, not bare keys.
  if (!type) return std::nullopt;
  if (const auto* name = arguments.Find("valueName"); name && !name->is_null()) {
    if (!name->is_string()) return std::nullopt;
    auto filled = RegistryText(name->text(), context);
    if (!filled || filled->size() > kMaxRegistryText || filled->find('\\') != std::string::npos ||
        filled->find('"') != std::string::npos)
      return std::nullopt;
    setting.name = std::move(*filled);
  }
  setting.default_value = setting.name.empty();
  if (SameIgnoringCase(*type, "string")) {
    setting.type = RegistrySetting::Type::kString;
    if (data && !data->is_string()) return std::nullopt;
    auto filled = RegistryText(data ? data->text() : std::string(), context);
    if (!filled) return std::nullopt;
    setting.data = std::move(*filled);
  } else if (SameIgnoringCase(*type, "dword")) {
    setting.type = RegistrySetting::Type::kDword;
    auto number = DwordData(data);
    if (!number) return std::nullopt;
    setting.data = std::move(*number);
  } else {
    return std::nullopt;
  }
  return setting;
}

}  // namespace

std::optional<std::string> FindScriptFile(const std::string& root, std::uint64_t product) {
  if (root.empty() || root.front() != '/') return std::nullopt;
  const std::string name = "goggame-" + std::to_string(product) + ".script";
  // Always by the listing, so the result is the name as the folder spells it (the host's volume may ignore case).
  DIR* listing = ::opendir(root.c_str());
  if (!listing) return std::nullopt;
  std::optional<std::string> found;
  while (const auto* entry = ::readdir(listing)) {
    if (!SameIgnoringCase(entry->d_name, name)) continue;
    const auto path = root + "/" + entry->d_name;
    struct stat info {};
    if (::lstat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode)) {
      found = path;
      break;
    }
  }
  ::closedir(listing);
  return found;
}

std::optional<Script> ParseScript(std::string_view text) {
  if (text.substr(0, kBom.size()) == kBom) text.remove_prefix(kBom.size());
  if (text.size() > kMaxScriptBytes) return std::nullopt;
  steam::json::Limits limits;
  limits.max_bytes = kMaxScriptBytes;
  limits.max_depth = 16;
  limits.max_values = 64 * 1024;
  limits.max_string_bytes = 64 * 1024;
  auto root = steam::json::Parse(text, limits);
  if (!root || !root->is_object()) return std::nullopt;
  Script script;
  const auto* actions = root->Find("actions");
  if (!actions || !actions->is_array()) return script;
  for (const auto& entry : actions->array()) {
    if (script.actions.size() >= kMaxActions) break;
    if (!entry.is_object()) continue;
    ScriptAction action;
    if (const auto* name = entry.Find("name"); name && name->is_string()) action.name = name->text();
    if (const auto* languages = entry.Find("languages"); languages && languages->is_array())
      for (const auto& language : languages->array())
        if (language.is_string()) action.languages.push_back(language.text());
    if (const auto* install = entry.Find("install"); install && install->is_object()) {
      ScriptAction::Install step;
      if (const auto* name = install->Find("action"); name && name->is_string()) step.action = name->text();
      if (const auto* arguments = install->Find("arguments"); arguments && arguments->is_object())
        step.arguments = *arguments;
      action.install = std::move(step);
    }
    script.actions.push_back(std::move(action));
  }
  return script;
}

std::optional<Script> ReadScript(const std::string& path) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) return std::nullopt;
  struct stat info {};
  bool ok = ::fstat(fd, &info) == 0 && S_ISREG(info.st_mode);
  std::string text;
  char buffer[4096];
  while (ok) {
    const auto count = ::read(fd, buffer, sizeof(buffer));
    if (count < 0 && errno == EINTR) continue;
    if (count < 0 || text.size() + static_cast<std::size_t>(count) > kMaxScriptBytes + kBom.size()) ok = false;
    else if (count == 0) break;
    else text.append(buffer, static_cast<std::size_t>(count));
  }
  ::close(fd);
  if (!ok) return std::nullopt;
  return ParseScript(text);
}

std::vector<ScriptStep> PlanScript(const Script& script, const ScriptContext& context, bool skip_copies,
                                   std::size_t* skipped) {
  std::vector<ScriptStep> steps;
  std::size_t dropped = 0;
  for (const auto& action : script.actions) {
    if (!ForLanguage(action, context)) continue;
    if (!action.install) {
      ++dropped;
      continue;
    }
    const auto& arguments = action.install->arguments;
    ScriptStep step;
    step.name = action.name;
    if (action.install->action == "supportData") {
      const auto* type = StringArgument(arguments, "type");
      const auto* target = StringArgument(arguments, "target");
      const auto* source = StringArgument(arguments, "source");
      const auto to = target ? Locate(*target, context, true) : std::nullopt;
      if (!type || *type != "folder" || !to) {
        ++dropped;
        continue;
      }
      step.target = *to;
      if (source && !source->empty()) {
        // Copies run once, at install; before a Play they could overwrite the player's files.
        if (skip_copies) continue;
        const auto from = Locate(*source, context, true);
        if (!from) {
          ++dropped;
          continue;
        }
        step.kind = ScriptStep::Kind::kCopyFolder;
        step.source = *from;
        step.overwrite = FlagArgument(arguments, "overwrite");
      } else {
        step.kind = ScriptStep::Kind::kCreateFolder;
      }
    } else if (action.install->action == "setIni") {
      const auto* file = StringArgument(arguments, "filename");
      const auto* section = StringArgument(arguments, "section");
      const auto* key = StringArgument(arguments, "keyName");
      const auto* raw = StringArgument(arguments, "keyValue");
      const auto at = file ? Locate(*file, context, false) : std::nullopt;
      if (!at || !section || !key || !raw || key->empty() || HasControl(*section) || HasControl(*key) ||
          HasControl(*raw) || key->find('=') != std::string::npos || section->find(']') != std::string::npos) {
        ++dropped;
        continue;
      }
      std::string value = *raw;
      ReplaceAll(&value, "{productID}", context.product);
      ReplaceAll(&value, "{app}", context.windows_app);
      if (HasControl(value)) {
        ++dropped;
        continue;
      }
      step.kind = ScriptStep::Kind::kSetIni;
      step.target = *at;
      step.section = *section;
      step.key = *key;
      step.value = std::move(value);
      step.utf8 = FlagArgument(arguments, "utf8");
    } else if (action.install->action == "setRegistry") {
      continue;  // PlanRegistry's
    } else {
      ++dropped;
      continue;
    }
    steps.push_back(std::move(step));
  }
  if (skipped) *skipped = dropped;
  return steps;
}

std::vector<RegistrySetting> PlanRegistry(const Script& script, const ScriptContext& context, std::size_t* skipped) {
  std::vector<RegistrySetting> settings;
  std::size_t dropped = 0;
  for (const auto& action : script.actions) {
    if (!action.install || action.install->action != "setRegistry" || !ForLanguage(action, context)) continue;
    auto setting = RegistryFrom(action.install->arguments, context);
    if (!setting) {
      ++dropped;
      continue;
    }
    settings.push_back(std::move(*setting));
  }
  if (skipped) *skipped = dropped;
  return settings;
}

RunResult RunScript(const std::vector<ScriptStep>& steps) {
  Runner runner;
  for (const auto& step : steps) {
    switch (step.kind) {
      case ScriptStep::Kind::kCreateFolder: runner.CreateFolder(step); break;
      case ScriptStep::Kind::kCopyFolder: runner.CopyFolder(step); break;
      case ScriptStep::Kind::kSetIni: runner.SetIni(step); break;
    }
  }
  return runner.result;
}

std::string SettingIni(const std::string& text, const std::string& section, const std::string& key,
                       const std::string& value) {
  const std::string newline =
      text.find("\r\n") != std::string::npos ? "\r\n" : text.find('\n') != std::string::npos ? "\n" : "\r\n";
  const bool bom = std::string_view(text).substr(0, kBom.size()) == kBom;
  auto lines = Lines(text);
  if (bom && !lines.empty()) lines.front().begin = std::min(kBom.size(), lines.front().end);
  const std::string entry = key + "=" + value;
  std::optional<std::size_t> header;
  for (std::size_t index = 0; index < lines.size(); ++index) {
    const std::string_view line(text.data() + lines[index].begin, lines[index].end - lines[index].begin);
    if (const auto name = Header(line); name && SameIgnoringCase(*name, section)) {
      header = index;
      break;
    }
  }
  std::string out = text;
  if (header) {
    std::size_t last = *header;  // the section's last non-blank line
    for (std::size_t index = *header + 1; index < lines.size(); ++index) {
      const std::string_view line(text.data() + lines[index].begin, lines[index].end - lines[index].begin);
      if (Header(line)) break;
      const auto trimmed = Trim(line);
      if (trimmed.empty()) continue;
      last = index;
      if (trimmed.front() == ';' || trimmed.front() == '#') continue;
      const auto equals = trimmed.find('=');
      if (equals == std::string_view::npos || !SameIgnoringCase(Trim(trimmed.substr(0, equals)), key)) continue;
      if (Trim(trimmed.substr(equals + 1)) == value) return text;  // already holds it
      out.replace(lines[index].begin, lines[index].end - lines[index].begin, entry);
      return out;
    }
    out.insert(lines[last].end, newline + entry);
    return out;
  }
  if (out.size() > (bom ? kBom.size() : 0)) {
    if (out.back() != '\n') out += newline;
    // A blank line before the new section, unless the file ends with one.
    const auto& final_line = lines.back();
    if (!Trim(std::string_view(text.data() + final_line.begin, final_line.end - final_line.begin)).empty())
      out += newline;
  }
  out += "[" + section + "]" + newline + entry + newline;
  return out;
}

}  // namespace playden::gog
