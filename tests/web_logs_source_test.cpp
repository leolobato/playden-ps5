// SPDX-License-Identifier: GPL-3.0-or-later
// Also built as web_logs_source_native_policy with PLAYDEN_PLATFORM_PS5=1: the console's walk by path.
#include "web/logs_source.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <unistd.h>

using namespace playden::web;
namespace fs = std::filesystem;

#define CHECK(condition)                                                                  \
  do {                                                                                    \
    if (!(condition)) {                                                                   \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
      std::exit(1);                                                                       \
    }                                                                                     \
  } while (false)

namespace {
std::string TitleLog(std::uint32_t app, const std::string& account = "Alice", std::int64_t time = 1791601941) {
  return "== session-1.log\nPW_REPORT/1 build=abc profile=playden-steam-" + std::to_string(app) +
         " cycle=0 pid=5 time=" + std::to_string(time) +
         "\nREC seq=561 WINE trace:process:SetEnvironmentVariableW (L\"SteamAppUser\" L\"" + account +
         "\")\nREC seq=562 WINE trace:process:SetEnvironmentVariableW (L\"SteamUser\" L\"" + account + "\")\n";
}

void Put(const fs::path& path, const std::string& text) {
  fs::create_directories(path.parent_path());
  FILE* file = std::fopen(path.c_str(), "wb");
  CHECK(file != nullptr);
  CHECK(std::fwrite(text.data(), 1, text.size(), file) == text.size());
  std::fclose(file);
}

std::string Param(const std::string& name) {
  return R"({"localizedParameters":{"defaultLanguage":"en-US","en-US":{"titleName":")" + name + R"("}},"titleId":"x"})";
}
}  // namespace

int main() {
  // Pure rules, as tests/test_collect_logs.py has them.
  CHECK(TitleLogId("title-FAKE78012.log") == std::optional<std::string>("FAKE78012"));
  CHECK(!TitleLogId("title-FAKE7801.log") && !TitleLogId("title-FAKE78012.reg") && !TitleLogId("title-PPSA12345.log"));
  CHECK(TitleApp(TitleLog(220)) == 220 && TitleApp("no profile") == 0);
  CHECK(TitleApp(std::string(5000, 'x') + "profile=playden-steam-1") == 0);  // first 4 KB only
  CHECK(LastRun(TitleLog(40800, "A", 300) + "PW_REPORT/1 build=abc profile=x time=50\n") == 300);
  CHECK(LastRun("no report yet\n") == 0);
  CHECK(LastRun("x PW_REPORT/1 time=9\n") == 0);                       // the line must start with it
  CHECK(LastRun("PW_REPORT/1 runtime=5 time=7\n") == 7);               // \btime=, not runtime=
  CHECK(WithoutStoreSuffix("Half-Life 2 (Steam)") == "Half-Life 2");
  CHECK(WithoutStoreSuffix("Delores (Epic)") == "Delores" && WithoutStoreSuffix("Gothic (GOG)") == "Gothic");
  CHECK(WithoutStoreSuffix("Imported") == "Imported");
  CHECK(WithoutStoreSuffix("Game (Steam) (GOG)") == "Game (Steam)");
  CHECK(WithoutStoreSuffix(" (Steam)") == " (Steam)" && WithoutStoreSuffix("").empty());
  CHECK(TitleNameFromParam(Param("Half-Life 2 (Steam)")) == "Half-Life 2");
  CHECK(TitleNameFromParam(R"({"localizedParameters":{"defaultLanguage":"ja-JP","ja-JP":{"titleName":"Game"}}})") == "Game");
  CHECK(TitleNameFromParam("not json").empty() && TitleNameFromParam("{}").empty());

  std::set<std::string> names;
  const std::string text = TitleLog(220) + "cloud user=alice id=76561198000000001 path=C:/users/Alice/saves\n";
  AddAccountNames(text, &names);
  CHECK(names == std::set<std::string>{"Alice"});
  auto redacted = Redact(text, names, {});
  CHECK(redacted.find("lice") == std::string::npos && redacted.find("76561198000000001") == std::string::npos);
  CHECK(redacted.find("<steam-user>") != std::string::npos && redacted.find("<steam-id>") != std::string::npos);
  // Word boundaries around SteamIDs, as \b in the Python regex.
  CHECK(Redact("x76561198000000001 765611980000000012 76561198000000001_", {}, {}) ==
        "x76561198000000001 765611980000000012 76561198000000001_");
  CHECK(Redact("(76561198000000001)", {}, {}) == "(<steam-id>)");
  // Non-ASCII punctuation is a boundary too, as in Python ("“76561198000000001”").
  CHECK(Redact("\xe2\x80\x9c" "76561198000000001\xe2\x80\x9d", {}, {}) == "\xe2\x80\x9c<steam-id>\xe2\x80\x9d");
  // The signed-in account, exactly, even where no Wine line names it; the longest name first.
  CHECK(Redact("steam user Bob 123 bobby", {"Bobby"}, {"bob", 123}) == "steam user <steam-user> <steam-id> <steam-user>");
  // A game that stopped before Wine named the user is still covered by another log's name.
  names.clear();
  const std::string stopped = "== session-1.log\nPW_REPORT/1 profile=playden-steam-220\nopen C:/users/Alice failed\n";
  AddAccountNames(stopped, &names);
  AddAccountNames(TitleLog(40800), &names);
  CHECK(Redact(stopped, names, {}).find("lice") == std::string::npos);
  CHECK(Firmware({"console_firmware=13.40 x", "a\nconsole_firmware=13.60 y"}) == "13.60" && Firmware({"nothing"}).empty());
  CHECK(Utf8Clean("ok \xff\xc3\xa9") == "ok \xef\xbf\xbd\xc3\xa9" && Utf8Clean("\xed\xa0\x80") == "\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd");
  CHECK(JsonString("a\"b\\c\n") == "\"a\\\"b\\\\c\\u000a\"");
  CHECK(Summary({{"collected", "x"}, {"firmware", ""}}) == "| Field | Value |\n| --- | --- |\n| collected | x |\n| firmware | unknown |\n");
  CHECK(CollectedTime(1791601941) == "2026-10-10 03:12 UTC" && ReportFileName(1791601941) == "playden-logs-20261010-031221.zip");

  // A console's folders under a temporary root (no links in the path: the walk refuses them).
  const fs::path root = fs::canonical(fs::temp_directory_path()) / ("playden-web-logs-" + std::to_string(::getpid()));
  fs::remove_all(root);
  const fs::path logs = root / "playden/logs", homebrew = root / "homebrew", kits = root / "playden/kits";
  const fs::path app = homebrew / "FAKE77031";
  Put(logs / "runtime.log.1", "console_firmware=4.00 kit=x\n");
  Put(logs / "runtime.log", "console_firmware=4.03 kit=x\n");
  Put(logs / "install.log", "user alice id=76561198000000001\n");
  Put(logs / "steam.log", "signed in as Bob (76561198000000002)\n");
  Put(logs / "title-FAKE78001.log", TitleLog(220, "Alice", 100));
  Put(logs / "title-FAKE78002.log", TitleLog(40800, "Alice", 300));
  Put(logs / "title-FAKE78002.registry.txt", "[Software\\Valve\\Steam] \"LastGameNameUsed\"=\"Alice\"\n");
  Put(logs / "title-FAKE78002.ppm", "P6");
  Put(logs / "title-FAKE78003.log", "no report yet\n");
  Put(logs / "screen.ppm", "P6");
  fs::create_directories(logs / "folder");
  Put(root / "playden/user/session.v1", "secret token");
  fs::create_symlink(root / "playden/user/session.v1", logs / "session-link.log");
  Put(homebrew / "FAKE78001/sce_sys/param.json", Param("Half-Life 2 (Steam)"));
  Put(kits / "old-kit/manifest.json", R"({"kit_id":"old-kit","runtime":{"artifact_revision":"f729534-kit-0.12"},"capabilities":["dxvk"]})");
  {
    // A real kit's manifest lists every file: tens of thousands of JSON values, about a megabyte.
    std::string files;
    for (int i = 0; i < 20000; ++i) files += std::string(i ? "," : "") + "{\"path\":\"runtime/file" + std::to_string(i) + "\",\"size\":1}";
    Put(kits / "playden-kit/manifest.json",
        R"({"kit_id":"playden-kit","runtime":{"artifact_revision":"ce28942-kit-0.13"},"capabilities":["dxvk","wine-mono"],"files":[)" +
            files + "]}");
  }
  fs::create_directories(kits / ".previous-x");
  Put(app / "playden-version", "0.9.0-nightly.20261028.4\n");
  Put(app / "playden-package.identity", "sha256 917f7b2a81c4d5e6f7a8b9c0d1e2f3a4\n");

  LogsSource source({logs.string(), homebrew.string(), kits.string(), app.string()});
  const auto index = source.Index();
  // Games: newest run first, then by title; names from param.json without the store suffix.
  CHECK(index.games.size() == 3);
  CHECK(index.games[0].title == "FAKE78002" && index.games[0].app == 40800 && index.games[0].last_run == 300);
  CHECK(index.games[0].name.empty());
  CHECK((index.games[0].files == std::vector<std::string>{"title-FAKE78002.log", "title-FAKE78002.registry.txt"}));
  CHECK(index.games[0].size == fs::file_size(logs / "title-FAKE78002.log") + fs::file_size(logs / "title-FAKE78002.registry.txt"));
  CHECK(index.games[1].title == "FAKE78001" && index.games[1].name == "Half-Life 2" && index.games[1].app == 220);
  CHECK(index.games[2].title == "FAKE78003" && index.games[2].last_run == 0 && index.games[2].app == 0);
  // Playden's logs by name: no screenshots, folders, links or title files.
  CHECK(index.playden.size() == 4);
  CHECK(index.playden[0].name == "install.log" && index.playden[1].name == "runtime.log" &&
        index.playden[2].name == "runtime.log.1" && index.playden[3].name == "steam.log");
  CHECK(index.playden[1].size == fs::file_size(logs / "runtime.log") && index.playden[1].changed > 0);
  CHECK(!index.HasFile("session-link.log") && !index.HasFile("screen.ppm") && index.HasFile("title-FAKE78002.registry.txt"));
  const auto json = source.IndexJson(index);
  CHECK(json.find("{\"title\":\"FAKE78002\",\"app\":40800,\"name\":null,\"last_run\":300,") != std::string::npos);
  CHECK(json.find("\"name\":\"Half-Life 2\"") != std::string::npos && json.find("\"app\":null") != std::string::npos);

  // The facts: the latest firmware, the current kit folder first and the others named.
  const auto facts = source.Facts(1791601941);
  const auto fact = [&](const std::string& key) {
    for (const auto& [name, value] : facts) if (name == key) return value;
    return std::string("<missing>");
  };
  CHECK(fact("collected") == "2026-10-10 03:12 UTC" && fact("Playden package") == "917f7b2a81c4");
  CHECK(fact("Playden version") == "0.9.0-nightly.20261028.4" && fact("firmware") == "4.03");
  CHECK(fact("kit runtime") == "ce28942-kit-0.13" && fact("kit capabilities") == "dxvk, wine-mono");
  CHECK(fact("other kit folders") == "old-kit");
  const auto status = source.StatusJson(1791601941);
  CHECK(status.find("\"version\":\"0.9.0-nightly.20261028.4\"") != std::string::npos);
  CHECK(status.find("\"other_kit_folders\":\"old-kit\"") != std::string::npos && status.find("\"firmware\":\"4.03\"") != std::string::npos);

  // One log: only names the index lists, always redacted (the found name and the signed-in account).
  const Redaction account{"Bob", 76561198000000002ull};
  const auto install = source.ReadLog(index, "install.log", account);
  CHECK(install && *install == "user <steam-user> id=<steam-id>\n");
  const auto steam = source.ReadLog(index, "steam.log", account);
  CHECK(steam && *steam == "signed in as <steam-user> (<steam-id>)\n");
  CHECK(!source.ReadLog(index, "session-link.log", account) && !source.ReadLog(index, "../user/session.v1", account));
  CHECK(!source.ReadLog(index, "screen.ppm", account) && !source.ReadLog(index, "", account) && !source.ReadLog(index, "..", account));

  // Reports: summary.md + logs/, as collect_logs.py writes them.
  const auto report = [&](const std::string& title, const fs::path& path) {
    std::string bytes;
    CHECK(source.WriteReport(index, title, account, 1791601941,
                             [&bytes](const char* data, std::size_t size) { bytes.append(data, size); return true; }));
    Put(path, bytes);
  };
  report("", root / "newest.zip");
  report("all", root / "all.zip");
  report("FAKE78001", root / "one.zip");
  report("FAKE79999", root / "none.zip");
  const std::string check = R"(
import sys, zipfile
def names(path):
    z = zipfile.ZipFile(path); assert z.testzip() is None
    for name in z.namelist():
        text = z.read(name).decode()
        assert 'alice' not in text.lower() and 'bob' not in text.lower(), name
        assert '76561198000000001' not in text and '76561198000000002' not in text, name
    return z.namelist(), z.read('summary.md').decode()
playden = ['logs/install.log', 'logs/runtime.log', 'logs/runtime.log.1', 'logs/steam.log']
listed, summary = names(sys.argv[1] + '/newest.zip')
assert listed == ['summary.md'] + sorted(playden + ['logs/title-FAKE78002.log', 'logs/title-FAKE78002.registry.txt']), listed
assert '| game titles | FAKE78002 (app 40800) |' in summary and '| redacted | Steam account name and SteamIDs |' in summary
assert summary.startswith('| Field | Value |\n| --- | --- |\n| collected | 2026-10-10 03:12 UTC |\n| Playden package | 917f7b2a81c4 |')
listed, summary = names(sys.argv[1] + '/all.zip')
assert '| game titles | FAKE78001 (app 220), FAKE78002 (app 40800), FAKE78003 (app ?) |' in summary, summary
assert 'logs/title-FAKE78003.log' in listed and 'logs/title-FAKE78002.ppm' not in listed
listed, summary = names(sys.argv[1] + '/one.zip')
assert listed == ['summary.md'] + sorted(playden + ['logs/title-FAKE78001.log']), listed
listed, summary = names(sys.argv[1] + '/none.zip')
assert listed == ['summary.md'] + playden and '| game titles | none |' in summary
)";
  Put(root / "check.py", check);
  CHECK(std::system(("python3 -I '" + (root / "check.py").string() + "' '" + root.string() + "'").c_str()) == 0);

  // A log over the read limit: its newest part, after a line saying how much was left out.
  {
    const std::string last = "last line alice 76561198000000001\n";
    std::string big(LogsSource::kMaxFileBytes + 1000 - last.size(), 'x');
    big += last;
    Put(logs / "huge.log", big);
    const auto index_big = source.Index();
    const auto huge = source.ReadLog(index_big, "huge.log", {});
    CHECK(huge && huge->rfind("[Playden: the first 1000 bytes of this file are left out]\n", 0) == 0);
    CHECK(huge->find("last line <steam-user> <steam-id>") != std::string::npos);
    fs::remove(logs / "huge.log");
  }

  // A logs folder that isn't there: an empty index, not an error.
  LogsSource missing({(root / "nowhere").string(), homebrew.string(), (root / "nokits").string(), app.string()});
  const auto empty = missing.Index();
  CHECK(empty.games.empty() && empty.playden.empty());
  std::string bytes;
  CHECK(missing.WriteReport(empty, "", {}, 0, [&bytes](const char* data, std::size_t size) { bytes.append(data, size); return true; }));
  CHECK(bytes.size() > 22);
  fs::remove_all(root);
  std::puts("web_logs_source: ok");
}
