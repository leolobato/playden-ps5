// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime/recipe_ini.h"

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <filesystem>
#include <map>
#include <optional>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

namespace playden::runtime {
namespace {
namespace fs = std::filesystem;

constexpr std::size_t kMaxIniFile = 256 * 1024;

char Lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; }

bool SameIgnoringCase(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i)
    if (Lower(a[i]) != Lower(b[i])) return false;
  return true;
}

std::string_view Trim(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) text.remove_suffix(1);
  return text;
}

// One line of the file: where it starts and ends (before its line ending).
struct Line {
  std::size_t begin = 0;
  std::size_t end = 0;
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

// The section a [header] line opens, or nothing for other lines.
std::optional<std::string_view> Header(std::string_view line) {
  line = Trim(line);
  if (line.size() < 2 || line.front() != '[') return std::nullopt;
  const auto close = line.find(']');
  if (close == std::string_view::npos) return std::nullopt;
  return Trim(line.substr(1, close - 1));
}

// A path in the game folder, matched part by part ignoring case; nothing when a part is missing or a link.
std::optional<fs::path> FindIgnoringCase(const fs::path& root, const std::string& relative, bool* linked) {
  fs::path cursor = root;
  std::size_t start = 0;
  while (start <= relative.size()) {
    const auto end = std::min(relative.find('/', start), relative.size());
    const auto part = relative.substr(start, end - start);
    const bool last = end == relative.size();
    std::optional<fs::path> found;
    struct stat info {};
    if (::lstat((cursor / part).c_str(), &info) == 0) {
      found = cursor / part;
    } else {
      std::error_code error;
      for (fs::directory_iterator it(cursor, error), done; !error && it != done; it.increment(error))
        if (SameIgnoringCase(it->path().filename().string(), part)) {
          found = it->path();
          break;
        }
      if (!found || ::lstat(found->c_str(), &info) != 0) return std::nullopt;
    }
    if (S_ISLNK(info.st_mode)) {
      *linked = true;
      return std::nullopt;
    }
    if (last ? !S_ISREG(info.st_mode) : !S_ISDIR(info.st_mode)) return std::nullopt;
    cursor = *found;
    start = end + 1;
  }
  return cursor;
}

bool ReadSmall(const fs::path& path, std::string* bytes, mode_t* mode) {
  const int file = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (file < 0) return false;
  struct stat info {};
  bool ok = ::fstat(file, &info) == 0 && S_ISREG(info.st_mode);
  *mode = info.st_mode & 07777;
  bytes->clear();
  char buffer[4096];
  while (ok) {
    const auto count = ::read(file, buffer, sizeof(buffer));
    if (count < 0 && errno == EINTR) continue;
    if (count < 0 || bytes->size() + static_cast<std::size_t>(count) > kMaxIniFile) ok = false;
    else if (count == 0) break;
    else bytes->append(buffer, static_cast<std::size_t>(count));
  }
  return ::close(file) == 0 && ok;
}

// Through a temporary file in the same folder, synced, then renamed over the original. The game may write the file
// back, so the temporary gets the original's permissions; a file system without them (exFAT) refuses the chmod and
// keeps its own, which is fine.
bool WriteAtomic(const fs::path& path, std::string_view bytes, mode_t mode) {
  const auto temporary = path.parent_path() / (".playden-" + path.filename().string() + ".tmp");
  ::unlink(temporary.c_str());
  const int file = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (file < 0) return false;
  bool ok = ::fchmod(file, mode) == 0 || errno == EPERM || errno == ENOTSUP || errno == EINVAL;
  std::size_t done = 0;
  while (ok && done < bytes.size()) {
    const auto count = ::write(file, bytes.data() + done, bytes.size() - done);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) ok = false;
    else done += static_cast<std::size_t>(count);
  }
  ok = ::fsync(file) == 0 && ok;
  ok = ::close(file) == 0 && ok;
  if (!ok || ::rename(temporary.c_str(), path.c_str()) != 0) {
    ::unlink(temporary.c_str());
    return false;
  }
  return true;
}

}  // namespace

bool SetIniValueIfEmpty(std::string* text, const std::string& section, const std::string& key, const std::string& value) {
  const std::string newline = text->find("\r\n") != std::string::npos ? "\r\n" : "\n";
  // A UTF-8 byte-order mark stays where it is, outside the first line.
  constexpr std::string_view kBom = "\xEF\xBB\xBF";
  const bool bom = std::string_view(*text).substr(0, kBom.size()) == kBom;
  auto lines = Lines(*text);
  if (bom) lines.front().begin = std::min(kBom.size(), lines.front().end);
  bool inside = false;
  std::optional<std::size_t> last_line;    // the first matching section's last non-blank line
  std::optional<std::size_t> empty_value;  // where an empty value of the key starts
  bool first_section_done = false;
  for (std::size_t index = 0; index < lines.size(); ++index) {
    const std::string_view line(text->data() + lines[index].begin, lines[index].end - lines[index].begin);
    if (const auto header = Header(line)) {
      if (inside) first_section_done = true;
      inside = SameIgnoringCase(*header, section);
      if (inside && !first_section_done) last_line = index;
      continue;
    }
    if (!inside) continue;
    const auto trimmed = Trim(line);
    if (!trimmed.empty() && !first_section_done) last_line = index;
    if (trimmed.empty() || trimmed.front() == ';' || trimmed.front() == '#') continue;
    const auto equals = trimmed.find('=');
    if (equals == std::string_view::npos || !SameIgnoringCase(Trim(trimmed.substr(0, equals)), key)) continue;
    if (!Trim(trimmed.substr(equals + 1)).empty()) return false;  // the player's or the game's own value
    if (!empty_value) empty_value = lines[index].begin + (line.find('=') + 1);
  }
  if (empty_value) {
    // After any spaces that follow '=', so the line ending stays.
    auto at = *empty_value;
    while (at < text->size() && ((*text)[at] == ' ' || (*text)[at] == '\t')) ++at;
    text->insert(at, value);
    return true;
  }
  if (last_line) {
    const auto at = lines[*last_line].end;
    if (at == text->size()) text->append(newline + key + "=" + value + newline);
    else text->insert(at, newline + key + "=" + value);
    return true;
  }
  const bool empty = text->size() == (bom ? kBom.size() : 0);
  if (!empty && text->back() != '\n') text->append(newline);
  if (!empty) text->append(newline);
  text->append("[" + section + "]" + newline + key + "=" + value + newline);
  return true;
}

RecipeIniResult ApplyRecipeIniValues(const std::string& game_root, const std::vector<IniValue>& values) {
  RecipeIniResult result;
  // One read and at most one write per file, in the order the recipe names them.
  std::vector<std::string> order;
  std::map<std::string, std::vector<const IniValue*>> files;
  for (const auto& value : values) {
    if (!files.count(value.file)) order.push_back(value.file);
    files[value.file].push_back(&value);
  }
  for (const auto& relative : order) {
    bool linked = false;
    const auto path = FindIgnoringCase(game_root, relative, &linked);
    if (!path) {
      result.skipped.push_back(relative + (linked ? ": a link" : ": missing"));
      continue;
    }
    std::string bytes;
    mode_t mode = 0644;
    if (!ReadSmall(*path, &bytes, &mode)) {
      result.skipped.push_back(relative + ": unreadable or too large");
      continue;
    }
    // UTF-16 (a byte-order mark, or NULs in the text) would need wide edits; such a file is left as it is.
    if (bytes.compare(0, 2, "\xFF\xFE") == 0 || bytes.compare(0, 2, "\xFE\xFF") == 0 ||
        bytes.find('\0') != std::string::npos) {
      result.skipped.push_back(relative + ": not a plain text INI");
      continue;
    }
    bool changed = false;
    for (const auto* value : files[relative])
      changed = SetIniValueIfEmpty(&bytes, value->section, value->key, value->value) || changed;
    if (!changed) continue;
    if (!WriteAtomic(*path, bytes, mode)) {
      result.ok = false;
      result.message = "Playden could not update the game's settings file (" + relative + ").";
      return result;
    }
    result.changed.push_back(relative);
  }
  return result;
}

bool ReplaceKeyValue(std::string* text, const std::string& key, const std::string& value,
                     std::optional<std::string>* previous) {
  if (previous) previous->reset();
  for (const auto& line : Lines(*text)) {
    const std::string_view view(text->data() + line.begin, line.end - line.begin);
    const auto equals = view.find('=');
    if (equals == std::string_view::npos || !SameIgnoringCase(Trim(view.substr(0, equals)), key)) continue;
    const auto held = Trim(view.substr(equals + 1));
    if (previous) *previous = std::string(held);
    if (SameIgnoringCase(held, value)) return false;
    // The held value with the spaces around it, up to the line ending.
    const auto at = line.begin + equals + 1;
    text->replace(at, line.end - at, value);
    return true;
  }
  return false;
}

GameFileValueResult ReplaceGameFileValue(const std::string& game_root, const std::string& relative,
                                         const std::string& key, const std::string& value) {
  GameFileValueResult result;
  bool linked = false;
  const auto path = FindIgnoringCase(game_root, relative, &linked);
  if (!path) return result;
  std::string bytes;
  mode_t mode = 0644;
  if (!ReadSmall(*path, &bytes, &mode) || bytes.find('\0') != std::string::npos) return result;
  std::optional<std::string> held;
  const bool changed = ReplaceKeyValue(&bytes, key, value, &held);
  result.found = held.has_value();
  result.previous = held.value_or(std::string());
  if (!changed) return result;
  if (!WriteAtomic(*path, bytes, mode)) {
    result.ok = false;
    result.message = "could not write " + relative;
    return result;
  }
  result.changed = true;
  return result;
}

}  // namespace playden::runtime
