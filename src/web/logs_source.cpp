// SPDX-License-Identifier: GPL-3.0-or-later
#include "web/logs_source.h"

#include "app/diagnostic_log.h"
#include "platform/directory_walk.h"
#include "steam/backend/json.h"
#include "web/zip.h"

#include <algorithm>
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <limits>
#include <sys/stat.h>
#include <unistd.h>

namespace playden::web {
namespace {
namespace json = steam::json;

constexpr std::string_view kTitlePrefix = "title-";
constexpr std::string_view kSteamIdPrefix = "7656119";
constexpr std::string_view kKitId = "playden-kit";
// What Playden's "Store name in PS5 titles" setting adds to a title name; imported games get none.
constexpr std::string_view kStoreSuffixes[] = {" (Steam)", " (GOG)", " (Epic)"};

bool Digit(char c) { return c >= '0' && c <= '9'; }

// A regex word character for \b: ASCII letters, digits and '_'. Python also counts non-ASCII letters; treating
// every non-ASCII byte as a boundary instead redacts a SteamID in typographic quotes too, and at worst a little more.
bool WordChar(char c) {
  const auto u = static_cast<unsigned char>(c);
  return Digit(c) || ((u | 0x20) >= 'a' && (u | 0x20) <= 'z') || c == '_';
}

char Lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

std::string LowerAscii(std::string_view text) {
  std::string out(text);
  for (auto& c : out) c = Lower(c);
  return out;
}

struct Entry {
  std::string name;
  std::uint64_t size = 0;
  std::int64_t changed = 0;
  bool directory = false;
};

// The plain files and folders of a directory, by absolute path: links and anything else are left out.
std::vector<Entry> ListDirectory(const std::string& path) {
  std::vector<Entry> entries;
  const int fd = platform::OpenDirectoryWalk(path);
  if (fd < 0) return entries;
  DIR* directory = ::fdopendir(fd);
  if (!directory) {
    ::close(fd);
    return entries;
  }
  while (entries.size() < 4096) {
    errno = 0;
    const auto* entry = ::readdir(directory);
    if (!entry) break;
    const std::string name = entry->d_name;
    if (name == "." || name == "..") continue;
    struct stat info{};
    if (::lstat((path + "/" + name).c_str(), &info) != 0) continue;
    if (!S_ISREG(info.st_mode) && !S_ISDIR(info.st_mode)) continue;
    entries.push_back({name, static_cast<std::uint64_t>(info.st_size > 0 ? info.st_size : 0),
                       static_cast<std::int64_t>(info.st_mtime), S_ISDIR(info.st_mode)});
  }
  ::closedir(directory);
  return entries;
}

// A regular file's bytes, opened through its folder without following links. The strict walk checks the file is
// the one the held folder lists, so a folder swapped for a link after the walk (to the user data, say) is refused.
// A file over max_bytes gives its last max_bytes, after a line saying how much was left out.
std::optional<std::string> ReadFile(const std::string& directory, const std::string& name, std::size_t max_bytes) {
  const int parent = platform::OpenDirectoryWalk(directory);
  if (parent < 0) return std::nullopt;
  const int fd = platform::OpenWalkChild(parent, name.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
  ::close(parent);
  if (fd < 0) return std::nullopt;
  struct stat info{};
  if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) {
    ::close(fd);
    return std::nullopt;
  }
  std::string note;
  if (info.st_size > 0 && static_cast<std::uint64_t>(info.st_size) > max_bytes) {
    const auto skipped = static_cast<std::uint64_t>(info.st_size) - max_bytes;
    if (::lseek(fd, static_cast<off_t>(skipped), SEEK_SET) < 0) {
      ::close(fd);
      return std::nullopt;
    }
    note = "[Playden: the first " + std::to_string(skipped) + " bytes of this file are left out]\n";
  }
  std::string bytes = note;
  bytes.reserve(std::min<std::size_t>(static_cast<std::size_t>(info.st_size > 0 ? info.st_size : 0), max_bytes));
  std::string chunk(64 * 1024, '\0');
  bool ok = true;
  while (bytes.size() < max_bytes + note.size()) {
    const auto count = ::read(fd, chunk.data(), std::min(chunk.size(), max_bytes + note.size() - bytes.size()));
    if (count < 0 && errno == EINTR) continue;
    if (count < 0) ok = false;
    if (count <= 0) break;
    bytes.append(chunk.data(), static_cast<std::size_t>(count));
  }
  ::close(fd);
  if (!ok) return std::nullopt;
  return bytes;
}

std::string Trimmed(std::string_view text) {
  while (!text.empty() && static_cast<unsigned char>(text.front()) <= ' ') text.remove_prefix(1);
  while (!text.empty() && static_cast<unsigned char>(text.back()) <= ' ') text.remove_suffix(1);
  return std::string(text);
}

void ReplaceAll(std::string& text, std::string_view from, std::string_view to) {
  if (from.empty()) return;
  std::string out;
  std::size_t start = 0;
  for (std::size_t at; (at = text.find(from, start)) != std::string::npos; start = at + from.size()) {
    out.append(text, start, at - start);
    out += to;
  }
  if (start == 0) return;
  out.append(text, start, std::string::npos);
  text = std::move(out);
}

}  // namespace

bool LogsIndex::HasFile(std::string_view name) const {
  for (const auto& log : playden) if (log.name == name) return true;
  for (const auto& game : games)
    for (const auto& file : game.files) if (file == name) return true;
  return false;
}

const GameLog* LogsIndex::Game(std::string_view title) const {
  for (const auto& game : games) if (game.title == title) return &game;
  return nullptr;
}

std::optional<std::string> TitleLogId(std::string_view file) {
  // ^title-(FAKE\d{5})\.log$
  if (file.size() != 19 || file.substr(0, 10) != "title-FAKE" || file.substr(15) != ".log") return std::nullopt;
  for (const char c : file.substr(10, 5)) if (!Digit(c)) return std::nullopt;
  return std::string(file.substr(6, 9));
}

std::uint32_t TitleApp(std::string_view text) {
  constexpr std::string_view kProfile = "profile=playden-steam-";
  text = text.substr(0, 4096);
  for (std::size_t at = text.find(kProfile); at != std::string_view::npos; at = text.find(kProfile, at + 1)) {
    std::uint64_t value = 0;
    std::size_t i = at + kProfile.size(), digits = 0;
    for (; i < text.size() && Digit(text[i]) && digits < 11; ++i, ++digits) value = value * 10 + (text[i] - '0');
    if (digits == 0) continue;
    return digits <= 10 && value <= std::numeric_limits<std::uint32_t>::max() ? static_cast<std::uint32_t>(value) : 0;
  }
  return 0;
}

std::int64_t LastRun(std::string_view text) {
  constexpr std::string_view kReport = "PW_REPORT/1 ";
  std::int64_t newest = 0;
  std::size_t start = 0;
  while (start < text.size()) {
    const auto end = std::min(text.find('\n', start), text.size());
    const auto line = text.substr(start, end - start);
    start = end + 1;
    if (line.compare(0, kReport.size(), kReport) != 0) continue;
    for (std::size_t at = line.find("time=", kReport.size() - 1); at != std::string_view::npos; at = line.find("time=", at + 1)) {
      if (at > 0 && WordChar(line[at - 1])) continue;
      std::size_t i = at + 5;
      std::int64_t value = 0;
      bool overflow = false;
      for (; i < line.size() && Digit(line[i]); ++i) {
        if (value > (std::numeric_limits<std::int64_t>::max() - 9) / 10) overflow = true;
        else value = value * 10 + (line[i] - '0');
      }
      if (i == at + 5) continue;
      if (!overflow) newest = std::max(newest, value);
      break;
    }
  }
  return newest;
}

std::string WithoutStoreSuffix(const std::string& name) {
  for (const auto suffix : kStoreSuffixes)
    if (name.size() > suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0)
      return name.substr(0, name.size() - suffix.size());
  return name;
}

std::string TitleNameFromParam(std::string_view text) {
  const auto parsed = json::Parse(text, {64 * 1024, 8, 4096, 4096});
  if (!parsed || !parsed->is_object()) return {};
  const auto* localized = parsed->Find("localizedParameters");
  if (!localized || !localized->is_object()) return {};
  const auto* language = localized->Find("defaultLanguage");
  const auto name_in = [&](std::string_view key) -> std::string {
    const auto* entry = localized->Find(key);
    const auto* name = entry && entry->is_object() ? entry->Find("titleName") : nullptr;
    if (!name || !name->is_string() || name->text().size() > 512) return {};
    for (const unsigned char c : name->text()) if (c < 0x20 || c == 0x7f) return {};
    return name->text();
  };
  std::string name = name_in(language && language->is_string() ? std::string_view(language->text()) : "en-US");
  if (name.empty()) name = name_in("en-US");
  return WithoutStoreSuffix(name);
}

void AddAccountNames(std::string_view text, std::set<std::string>* names) {
  for (const std::string_view key : {std::string_view("L\"SteamUser\" L\""), std::string_view("L\"SteamAppUser\" L\"")}) {
    for (std::size_t at = text.find(key); at != std::string_view::npos; at = text.find(key, at + 1)) {
      const auto start = at + key.size();
      const auto end = text.find('"', start);
      if (end == std::string_view::npos) break;
      if (end > start && end - start <= 256) names->insert(std::string(text.substr(start, end - start)));
    }
  }
}

std::string Redact(std::string_view text, const std::set<std::string>& found, const Redaction& account) {
  std::string out;
  out.reserve(text.size());
  // SteamIDs: \b7656119\d{10}\b.
  for (std::size_t i = 0; i < text.size();) {
    if (text.compare(i, kSteamIdPrefix.size(), kSteamIdPrefix) == 0 && (i == 0 || !WordChar(text[i - 1])) &&
        i + 17 <= text.size() && (i + 17 == text.size() || !WordChar(text[i + 17]))) {
      bool digits = true;
      for (std::size_t j = i + 7; j < i + 17; ++j) digits = digits && Digit(text[j]);
      if (digits) {
        out += "<steam-id>";
        i += 17;
        continue;
      }
    }
    out.push_back(text[i++]);
  }
  if (account.steam_id != 0) ReplaceAll(out, std::to_string(account.steam_id), "<steam-id>");
  std::vector<std::string> names(found.begin(), found.end());
  if (!account.account_name.empty()) names.push_back(account.account_name);
  std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) { return a.size() > b.size(); });
  for (const auto& name : names) {
    if (name.empty()) continue;
    // ASCII case only: Python also folds other letters, which account names rarely have.
    const auto needle = LowerAscii(name);
    const auto lowered = LowerAscii(out);
    std::string replaced;
    std::size_t start = 0;
    for (std::size_t at; (at = lowered.find(needle, start)) != std::string::npos; start = at + needle.size()) {
      replaced.append(out, start, at - start);
      replaced += "<steam-user>";
    }
    if (start == 0) continue;
    replaced.append(out, start, std::string::npos);
    out = std::move(replaced);
  }
  return out;
}

std::string Firmware(const std::vector<std::string_view>& texts) {
  constexpr std::string_view kKey = "console_firmware=";
  std::string found;
  for (const auto text : texts)
    for (std::size_t at = text.find(kKey); at != std::string_view::npos; at = text.find(kKey, at + 1)) {
      auto end = at + kKey.size();
      while (end < text.size() && !(text[end] == ' ' || text[end] == '\t' || text[end] == '\n' || text[end] == '\r' ||
                                    text[end] == '\f' || text[end] == '\v')) ++end;
      if (end > at + kKey.size()) found = std::string(text.substr(at + kKey.size(), end - at - kKey.size()));
    }
  return found;
}

std::string Utf8Clean(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size();) {
    const auto c = static_cast<unsigned char>(text[i]);
    std::size_t length = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xe ? 3 : (c >> 3) == 0x1e ? 4 : 0;
    bool valid = length > 0 && i + length <= text.size();
    for (std::size_t j = 1; valid && j < length; ++j) valid = (static_cast<unsigned char>(text[i + j]) & 0xc0) == 0x80;
    if (valid && length > 1) {
      // Overlong forms, surrogates and values past U+10FFFF are invalid too.
      std::uint32_t code = c & (0x7f >> length);
      for (std::size_t j = 1; j < length; ++j) code = (code << 6) | (static_cast<unsigned char>(text[i + j]) & 0x3f);
      const std::uint32_t least[] = {0, 0, 0x80, 0x800, 0x10000};
      valid = code >= least[length] && code <= 0x10ffff && (code < 0xd800 || code > 0xdfff);
    }
    if (!valid) {
      out += "\xef\xbf\xbd";
      ++i;
      continue;
    }
    out.append(text.data() + i, length);
    i += length;
  }
  return out;
}

std::string JsonString(std::string_view text) {
  const auto clean = Utf8Clean(text);
  std::string out = "\"";
  for (const char c : clean) {
    if (c == '"' || c == '\\') {
      out.push_back('\\');
      out.push_back(c);
    } else if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) {
      char escaped[8];
      std::snprintf(escaped, sizeof escaped, "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
      out += escaped;
    } else {
      out.push_back(c);
    }
  }
  return out + "\"";
}

std::string Summary(const std::vector<std::pair<std::string, std::string>>& fields) {
  std::string out = "| Field | Value |\n| --- | --- |";
  for (const auto& [key, value] : fields) out += "\n| " + key + " | " + (value.empty() ? std::string("unknown") : value) + " |";
  return out + "\n";
}

std::string CollectedTime(std::int64_t unix_seconds) {
  const auto stamp = app::FormatLogTimestamp(unix_seconds);  // 2026-10-28T15:04:05Z
  return stamp.substr(0, 10) + " " + stamp.substr(11, 5) + " UTC";
}

std::string ReportFileName(std::int64_t unix_seconds) {
  const auto stamp = app::FormatLogTimestamp(unix_seconds);
  return "playden-logs-" + stamp.substr(0, 4) + stamp.substr(5, 2) + stamp.substr(8, 2) + "-" + stamp.substr(11, 2) +
         stamp.substr(14, 2) + stamp.substr(17, 2) + ".zip";
}

LogsSource::LogsSource(LogsPaths paths) : paths_(std::move(paths)) {}

std::optional<std::string> LogsSource::ReadLogFile(std::string_view name) const {
  auto bytes = ReadFile(paths_.logs, std::string(name), kMaxFileBytes);
  if (!bytes) return std::nullopt;
  return Utf8Clean(*bytes);
}

LogsIndex LogsSource::Index() const {
  LogsIndex index;
  const auto entries = ListDirectory(paths_.logs);
  for (const auto& entry : entries) {
    if (entry.directory || entry.name.size() >= 4 && entry.name.compare(entry.name.size() - 4, 4, ".ppm") == 0) continue;
    if (const auto title = TitleLogId(entry.name)) {
      GameLog game;
      game.title = *title;
      game.files.push_back(entry.name);
      game.size = entry.size;
      if (const auto text = ReadLogFile(entry.name)) {
        game.app = TitleApp(*text);
        game.last_run = LastRun(*text);
      }
      // Per-title extras such as registry dumps go with their title.
      const std::string prefix = "title-" + game.title + ".";
      for (const auto& extra : entries)
        if (!extra.directory && extra.name != entry.name && extra.name.compare(0, prefix.size(), prefix) == 0 &&
            !(extra.name.size() >= 4 && extra.name.compare(extra.name.size() - 4, 4, ".ppm") == 0)) {
          game.files.push_back(extra.name);
          game.size += extra.size;
        }
      std::sort(game.files.begin() + 1, game.files.end());
      if (const auto param = ReadFile(paths_.homebrew + "/" + game.title + "/sce_sys", "param.json", 64 * 1024))
        game.name = TitleNameFromParam(*param);
      index.games.push_back(std::move(game));
    } else if (entry.name.compare(0, kTitlePrefix.size(), kTitlePrefix) != 0) {
      index.playden.push_back({entry.name, entry.changed, entry.size});
    }
  }
  std::sort(index.games.begin(), index.games.end(), [](const GameLog& a, const GameLog& b) {
    return a.last_run != b.last_run ? a.last_run > b.last_run : a.title < b.title;
  });
  std::sort(index.playden.begin(), index.playden.end(), [](const PlaydenLog& a, const PlaydenLog& b) { return a.name < b.name; });
  return index;
}

std::vector<std::pair<std::string, std::string>> LogsSource::Facts(std::int64_t now) const {
  std::vector<std::pair<std::string, std::string>> facts;
  facts.emplace_back("collected", CollectedTime(now));
  std::string package;
  if (const auto identity = ReadFile(paths_.app, "playden-package.identity", 4096)) {
    // The identity's last word, shortened as the Python tool does.
    const auto text = Trimmed(*identity);
    const auto space = text.find_last_of(" \t\r\n");
    package = (space == std::string::npos ? text : text.substr(space + 1)).substr(0, 12);
  }
  facts.emplace_back("Playden package", Utf8Clean(package));
  const auto version = ReadFile(paths_.app, "playden-version", 256);
  facts.emplace_back("Playden version", version ? Utf8Clean(Trimmed(*version)) : std::string());
  const auto older = ReadLogFile("runtime.log.1"), current = ReadLogFile("runtime.log");
  facts.emplace_back("firmware", Firmware({older ? std::string_view(*older) : "", current ? std::string_view(*current) : ""}));

  // The current kit folder first: a console updated from an earlier Playden may still hold the kit under the
  // id it had before, until Playden retires it. Other folders are listed.
  std::vector<std::string> kits;
  for (const auto& entry : ListDirectory(paths_.kits))
    if (entry.directory && entry.name[0] != '.') kits.push_back(entry.name);
  std::sort(kits.begin(), kits.end(), [](const std::string& a, const std::string& b) {
    if ((a == kKitId) != (b == kKitId)) return a == kKitId;
    return a < b;
  });
  std::string others;
  for (std::size_t i = 1; i < kits.size(); ++i) others += (i > 1 ? ", " : "") + kits[i];
  bool found = false;
  for (const auto& kit : kits) {
    // A kit's manifest lists every file of the kit (about a megabyte and 20,000 values): the runtime's own limits.
    constexpr std::size_t kManifestBytes = 8 * 1024 * 1024, kManifestValues = 32768 * 8;
    const auto text = ReadFile(paths_.kits + "/" + kit, "manifest.json", kManifestBytes);
    const auto manifest = text ? json::Parse(*text, {kManifestBytes, 16, kManifestValues, 1024 * 1024}) : std::nullopt;
    if (!manifest || !manifest->is_object()) continue;
    // No kit_id: kit releases share it (the install folder), so it says nothing.
    std::string runtime, capabilities;
    if (const auto* section = manifest->Find("runtime"); section && section->is_object())
      if (const auto* revision = section->Find("artifact_revision"); revision && revision->is_string()) runtime = revision->text();
    if (const auto* list = manifest->Find("capabilities"); list && list->is_array())
      for (const auto& capability : list->array())
        if (capability.is_string()) capabilities += (capabilities.empty() ? "" : ", ") + capability.text();
    facts.emplace_back("kit runtime", Utf8Clean(runtime));
    facts.emplace_back("kit capabilities", Utf8Clean(capabilities));
    found = true;
    break;
  }
  if (!found) facts.emplace_back("kit runtime", "");
  if (!others.empty()) facts.emplace_back("other kit folders", Utf8Clean(others));
  return facts;
}

std::string LogsSource::StatusJson(std::int64_t now) const {
  const auto facts = Facts(now);
  const auto fact = [&](std::string_view key) {
    for (const auto& [name, value] : facts) if (name == key) return value.empty() ? std::string("null") : JsonString(value);
    return std::string("null");
  };
  return "{\"version\":" + fact("Playden version") + ",\"package\":" + fact("Playden package") +
         ",\"firmware\":" + fact("firmware") + ",\"kit_runtime\":" + fact("kit runtime") +
         ",\"kit_capabilities\":" + fact("kit capabilities") + ",\"other_kit_folders\":" + fact("other kit folders") +
         ",\"redacted\":\"Steam account name and SteamIDs\",\"collected\":" + fact("collected") + "}";
}

std::string LogsSource::IndexJson(const LogsIndex& index) const {
  std::string out = "{\"games\":[";
  for (std::size_t i = 0; i < index.games.size(); ++i) {
    const auto& game = index.games[i];
    out += (i ? "," : "") + std::string("{\"title\":") + JsonString(game.title) +
           ",\"app\":" + (game.app ? std::to_string(game.app) : "null") +
           ",\"name\":" + (game.name.empty() ? "null" : JsonString(game.name)) +
           ",\"last_run\":" + std::to_string(game.last_run) + ",\"size\":" + std::to_string(game.size) + ",\"files\":[";
    for (std::size_t j = 0; j < game.files.size(); ++j) out += (j ? "," : "") + JsonString(game.files[j]);
    out += "]}";
  }
  out += "],\"playden\":[";
  for (std::size_t i = 0; i < index.playden.size(); ++i) {
    const auto& log = index.playden[i];
    out += (i ? "," : "") + std::string("{\"name\":") + JsonString(log.name) + ",\"changed\":" + std::to_string(log.changed) +
           ",\"size\":" + std::to_string(log.size) + "}";
  }
  return out + "]}";
}

std::set<std::string> LogsSource::FoundNames(const LogsIndex& index) const {
  // Names come from every log, so a game that stopped before Wine named the user is still covered.
  std::set<std::string> names;
  const auto add = [&](const std::string& file) {
    if (const auto text = ReadLogFile(file)) AddAccountNames(*text, &names);
  };
  for (const auto& log : index.playden) add(log.name);
  for (const auto& game : index.games)
    for (const auto& file : game.files) add(file);
  return names;
}

std::optional<std::string> LogsSource::ReadLog(const LogsIndex& index, std::string_view name, const Redaction& account) const {
  if (name.empty() || name.find('/') != std::string_view::npos || name == "." || name == ".." || !index.HasFile(name))
    return std::nullopt;
  const auto text = ReadLogFile(name);
  if (!text) return std::nullopt;
  return Redact(*text, FoundNames(index), account);
}

bool LogsSource::WriteReport(const LogsIndex& index, std::string_view title, const Redaction& account, std::int64_t now,
                             const Write& write) const {
  std::vector<const GameLog*> chosen;
  if (title == "all") {
    for (const auto& game : index.games) chosen.push_back(&game);
    std::sort(chosen.begin(), chosen.end(), [](const GameLog* a, const GameLog* b) { return a->title < b->title; });
  } else if (title.empty()) {
    if (!index.games.empty()) chosen.push_back(&index.games.front());
  } else if (const auto* game = index.Game(title)) {
    chosen.push_back(game);
  }
  auto fields = Facts(now);
  std::string titles;
  for (const auto* game : chosen)
    titles += (titles.empty() ? "" : ", ") + game->title + " (app " + (game->app ? std::to_string(game->app) : "?") + ")";
  fields.emplace_back("game titles", titles.empty() ? "none" : titles);
  fields.emplace_back("redacted", "Steam account name and SteamIDs");

  std::vector<std::string> files;
  for (const auto& log : index.playden) files.push_back(log.name);
  for (const auto* game : chosen) files.insert(files.end(), game->files.begin(), game->files.end());
  std::sort(files.begin(), files.end());
  const auto names = FoundNames(index);
  StoredZip zip(write, now);
  if (!zip.Add("summary.md", Summary(fields))) return false;
  for (const auto& file : files) {
    // A log that went away since the index was read is left out.
    const auto text = ReadLogFile(file);
    if (text && !zip.Add("logs/" + file, Redact(*text, names, account))) return false;
  }
  return zip.Finish();
}

}  // namespace playden::web
