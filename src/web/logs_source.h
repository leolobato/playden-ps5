// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace playden::web {

// What the web page reads, all of it what tools/collect_logs.py reads over FTP: the logs folder,
// the kits' manifests, the app's version and package identity, and each listed title's param.json.
// Never the user data folder, a bottle or a game's files.
struct LogsPaths {
  std::string logs;      // /data/playden/logs
  std::string homebrew;  // /data/homebrew: <title>/sce_sys/param.json
  std::string kits;      // /data/playden/kits
  std::string app;       // /data/homebrew/FAKE77031: playden-version, playden-package.identity
};

// The signed-in Steam account, replaced in everything served. Not a credential.
struct Redaction {
  std::string account_name;
  std::uint64_t steam_id = 0;
};

struct GameLog {
  std::string title;      // FAKE78012
  std::uint32_t app = 0;  // 0: unknown
  std::string name;       // from the title's param.json, without Playden's store suffix; empty: unknown
  std::int64_t last_run = 0;
  std::uint64_t size = 0;          // the title log and its extras
  std::vector<std::string> files;  // title-FAKE78012.log first, then its extras (title-FAKE78012.*)
};

struct PlaydenLog {
  std::string name;
  std::int64_t changed = 0;
  std::uint64_t size = 0;
};

struct LogsIndex {
  std::vector<GameLog> games;       // most recently run first
  std::vector<PlaydenLog> playden;  // by name
  bool HasFile(std::string_view name) const;
  const GameLog* Game(std::string_view title) const;
};

// Pure helpers, the same rules as collect_logs.py.
// "title-FAKE78012.log" -> "FAKE78012".
std::optional<std::string> TitleLogId(std::string_view file);
// The Steam app ID from "profile=playden-steam-<id>" in a title log's first 4 KB, or 0.
std::uint32_t TitleApp(std::string_view text);
// The newest "time=" of a "PW_REPORT/1 ..." line (seconds since 1970), or 0.
std::int64_t LastRun(std::string_view text);
// "Half-Life 2 (Steam)" -> "Half-Life 2"; one store suffix at most, and never the whole name.
std::string WithoutStoreSuffix(const std::string& name);
// The default language's titleName in a title's param.json, without the store suffix; empty if none.
std::string TitleNameFromParam(std::string_view json);
// Steam account names a log shows in Wine's L"SteamUser" L"<name>" and L"SteamAppUser" L"<name>".
void AddAccountNames(std::string_view text, std::set<std::string>* names);
// SteamIDs (7656119 and ten more digits) become <steam-id>, the signed-in account's ID too; every
// name (the signed-in one and the found ones, longest first, ignoring ASCII case) becomes <steam-user>.
std::string Redact(std::string_view text, const std::set<std::string>& names, const Redaction& account);
// The last console_firmware= value in the texts, in order; empty if none.
std::string Firmware(const std::vector<std::string_view>& texts);
// Invalid UTF-8 replaced with U+FFFD, as Python's decode(errors="replace").
std::string Utf8Clean(std::string_view text);
std::string JsonString(std::string_view text);
// summary.md: a two-column Markdown table; an empty value says "unknown".
std::string Summary(const std::vector<std::pair<std::string, std::string>>& fields);
// "2026-10-28 15:04 UTC" and "20261028-150405" for a Unix time.
std::string CollectedTime(std::int64_t unix_seconds);
std::string ReportFileName(std::int64_t unix_seconds);

class LogsSource {
 public:
  using Write = std::function<bool(const char* data, std::size_t size)>;
  explicit LogsSource(LogsPaths paths);

  LogsIndex Index() const;
  // The summary's facts, in its order, before the game titles: collected, Playden package, Playden
  // version, firmware, kit runtime, kit capabilities (and other kit folders when there are some).
  std::vector<std::pair<std::string, std::string>> Facts(std::int64_t now) const;
  std::string StatusJson(std::int64_t now) const;
  std::string IndexJson(const LogsIndex& index) const;
  // One log from the index, redacted. Nothing for a name the index doesn't list.
  std::optional<std::string> ReadLog(const LogsIndex& index, std::string_view name, const Redaction& account) const;
  // The report: summary.md, then logs/<name> for Playden's logs and the chosen games' logs, each
  // redacted and written one at a time. title: a title ID, "all", or empty for the most recent game.
  // A title with no log gives Playden's logs only, as collect_logs.py does.
  bool WriteReport(const LogsIndex& index, std::string_view title, const Redaction& account, std::int64_t now,
                   const Write& write) const;

  static constexpr std::size_t kMaxFileBytes = 8 * 1024 * 1024;

 private:
  std::optional<std::string> ReadLogFile(std::string_view name) const;
  std::set<std::string> FoundNames(const LogsIndex& index) const;
  LogsPaths paths_;
};

}  // namespace playden::web
