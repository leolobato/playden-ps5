// SPDX-License-Identifier: GPL-3.0-or-later
// goggame-<id>.script: the steps for a language, locations that try to leave the game folder, links, folder copies that
// keep the player's files, and setIni edits that keep every other byte. Inline scripts in the shape of Beneath a Steel
// Sky's (ScummVM) and the synthetic fixture.
#include "gog/install_script.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <vector>

#ifdef PLAYDEN_TEST_AT_UNAVAILABLE
// The console title refuses *at calls with a real folder descriptor: none may be needed.
extern "C" int openat(int, const char*, int, ...) { errno = EINVAL; return -1; }
extern "C" int mkdirat(int, const char*, mode_t) { errno = EINVAL; return -1; }
extern "C" int fstatat(int, const char*, struct stat*, int) { errno = EINVAL; return -1; }
extern "C" int unlinkat(int, const char*, int) { errno = EINVAL; return -1; }
extern "C" int renameat(int, const char*, int, const char*) { errno = EINVAL; return -1; }
#endif

using namespace playden;
using playden::gog::ParseScript;
using playden::gog::PlanRegistry;
using playden::gog::PlanScript;
using playden::gog::RegistrySetting;
using playden::gog::RunScript;
using playden::gog::ScriptContext;
using playden::gog::ScriptLocation;
using playden::gog::ScriptStep;

#define CHECK(condition)                                                                  \
  do {                                                                                    \
    if (!(condition)) {                                                                   \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
      std::exit(1);                                                                       \
    }                                                                                     \
  } while (false)

namespace {

// Plain POSIX helpers: std::filesystem may use descriptor-relative calls, which the console twin refuses.
bool Exists(const std::string& path) {
  struct stat info {};
  return ::lstat(path.c_str(), &info) == 0;
}

void RemoveTree(const std::string& path) {
  struct stat info {};
  if (::lstat(path.c_str(), &info) != 0) return;
  if (S_ISDIR(info.st_mode)) {
    std::vector<std::string> names;
    if (DIR* listing = ::opendir(path.c_str())) {
      while (const auto* entry = ::readdir(listing))
        if (std::strcmp(entry->d_name, ".") != 0 && std::strcmp(entry->d_name, "..") != 0) names.push_back(entry->d_name);
      ::closedir(listing);
    }
    for (const auto& name : names) RemoveTree(path + "/" + name);
    CHECK(::rmdir(path.c_str()) == 0);
  } else {
    CHECK(::unlink(path.c_str()) == 0);
  }
}

void MakeFolders(const std::string& path) {
  for (std::size_t at = 1; at <= path.size(); ++at)
    if (at == path.size() || path[at] == '/') {
      const auto part = path.substr(0, at);
      CHECK(::mkdir(part.c_str(), 0777) == 0 || errno == EEXIST);
    }
}

void Write(const std::string& path, const std::string& text) {
  FILE* file = std::fopen(path.c_str(), "wb");
  CHECK(file);
  CHECK(std::fwrite(text.data(), 1, text.size(), file) == text.size());
  CHECK(std::fclose(file) == 0);
}

std::string Read(const std::string& path) {
  FILE* file = std::fopen(path.c_str(), "rb");
  CHECK(file);
  std::string text;
  char buffer[4096];
  for (std::size_t read = 0; (read = std::fread(buffer, 1, sizeof(buffer), file)) > 0;) text.append(buffer, read);
  std::fclose(file);
  return text;
}

// The shape of Beneath a Steel Sky's script (and Arena's createFolder), as GOG ships them.
const char* const kSteelSky = R"({"actions":[
 {"install":{"action":"supportData","arguments":{"saveGameData":true,"target":"{app}/saves","type":"folder"}},"languages":["*"],"name":"createFolder"},
 {"install":{"action":"setIni","arguments":{"filename":"{app}\\beneath.ini","keyName":"path","keyType":"string","keyValue":"{app}","section":"beneath","utf8":false}},"languages":["*"],"name":"ini1"},
 {"install":{"action":"setIni","arguments":{"filename":"{app}\\beneath.ini","keyName":"savepath","keyType":"string","keyValue":"{app}\\saves","section":"beneath","utf8":false}},"languages":["*"],"name":"ini2"},
 {"install":{"action":"setIni","arguments":{"filename":"{app}\\beneath.ini","keyName":"language","keyType":"string","keyValue":"de","section":"beneath","utf8":false}},"languages":["de-DE"],"name":"inilang"},
 {"install":{"action":"supportData","arguments":{"overwrite":false,"source":"{supportDir}/save","target":"{app}","type":"folder"}},"languages":["*"],"name":"copyConfig"},
 {"install":{"action":"setRegistry","arguments":{"root":"HKEY_LOCAL_MACHINE","subkey":"Software\\GOG.com\\Games\\{productID}"}},"languages":["*"],"name":"reg"},
 {"install":{"action":"supportData","arguments":{"source":"{supportDir}/../../x","target":"{app}","type":"folder"}},"languages":["*"],"name":"escape"}
]})";

struct Fixture {
  std::string game;
  std::string support;
  ScriptContext context;
};

Fixture Fresh(const std::string& scratch) {
  RemoveTree(scratch);
  Fixture fixture;
  fixture.game = scratch + "/game";
  fixture.support = fixture.game + "/.playden-gog/support/7";
  MakeFolders(fixture.support);
  fixture.context = {fixture.game, fixture.support, "7", "en-US", "Z:\\Games\\BASS"};
  return fixture;
}

ScriptLocation Game(const Fixture& fixture, const std::string& relative) { return {fixture.game, relative, false}; }

std::size_t PlanCount(const std::string& action, const ScriptContext& context, std::size_t* skipped) {
  const auto script = ParseScript(R"({"actions":[)" + action + "]}");
  CHECK(script);
  return PlanScript(*script, context, false, skipped).size();
}

void PlansForTheLanguage(const std::string& scratch) {
  const auto fixture = Fresh(scratch);
  const auto script = ParseScript(kSteelSky);
  CHECK(script && script->actions.size() == 7);
  std::size_t skipped = 99;
  const auto steps = PlanScript(*script, fixture.context, false, &skipped);
  // The escape is skipped and counted; setRegistry is PlanRegistry's; the de-DE action is not for this install.
  CHECK(skipped == 1);
  CHECK(steps.size() == 4);
  CHECK(steps[0].kind == ScriptStep::Kind::kCreateFolder && steps[0].target == Game(fixture, "saves") &&
        steps[0].name == "createFolder");
  CHECK(steps[1].kind == ScriptStep::Kind::kSetIni && steps[1].target == Game(fixture, "beneath.ini") &&
        steps[1].section == "beneath" && steps[1].key == "path" && steps[1].value == "Z:\\Games\\BASS" &&
        !steps[1].utf8);
  CHECK(steps[2].key == "savepath" && steps[2].value == "Z:\\Games\\BASS\\saves");
  CHECK(steps[3].kind == ScriptStep::Kind::kCopyFolder && steps[3].target == Game(fixture, "") && !steps[3].overwrite);
  CHECK(steps[3].source.base == fixture.game && steps[3].source.relative == ".playden-gog/support/7/save" &&
        steps[3].source.support);
  CHECK(steps[3].source.Path() == fixture.support + "/save");
  // Before a Play: no copies.
  CHECK(PlanScript(*script, fixture.context, true).size() == 3);
  // The German install gets the German line.
  auto german = fixture.context;
  german.gog_language = "de-DE";
  const auto german_steps = PlanScript(*script, german, false);
  CHECK(german_steps.size() == 5 && german_steps[3].key == "language" && german_steps[3].value == "de");
  // Languages match ignoring case, and GOG's deprecated codes ("en") stand for the current ones.
  std::size_t count = 0;
  CHECK(PlanCount(R"({"install":{"action":"supportData","arguments":{"target":"{app}/a","type":"folder"}},"languages":["EN-us"]})",
                  fixture.context, &count) == 1);
  CHECK(PlanCount(R"({"install":{"action":"supportData","arguments":{"target":"{app}/a","type":"folder"}},"languages":["en"]})",
                  fixture.context, &count) == 1);
  CHECK(PlanCount(R"({"install":{"action":"supportData","arguments":{"target":"{app}/a","type":"folder"}}})", fixture.context,
                  &count) == 1);
  CHECK(PlanCount(R"({"install":{"action":"supportData","arguments":{"target":"{app}/a","type":"folder"}},"languages":["fr-FR"]})",
                  fixture.context, &count) == 0 &&
        count == 0);
  // {productID} in a location and a value.
  const auto product = ParseScript(R"({"actions":[{"install":{"action":"setIni","arguments":{"filename":"{supportDir}/{productID}.ini","section":"s","keyName":"id","keyValue":"{productID} in {app}"}}}]})");
  CHECK(product);
  const auto product_steps = PlanScript(*product, fixture.context, false);
  CHECK(product_steps.size() == 1 && product_steps[0].target.relative == ".playden-gog/support/7/7.ini" &&
        product_steps[0].target.support && product_steps[0].value == "7 in Z:\\Games\\BASS");
}

void DropsEscapes(const std::string& scratch) {
  const auto fixture = Fresh(scratch);
  const auto folder = [](const std::string& target) {
    return R"({"install":{"action":"supportData","arguments":{"target":")" + target + R"(","type":"folder"}}})";
  };
  const auto copy = [](const std::string& source) {
    return R"({"install":{"action":"supportData","arguments":{"source":")" + source + R"(","target":"{app}","type":"folder"}}})";
  };
  const auto ini = [](const std::string& file) {
    return R"({"install":{"action":"setIni","arguments":{"filename":")" + file + R"(","section":"s","keyName":"k","keyValue":"v"}}})";
  };
  for (const auto& action : {
           copy("{supportDir}/../../x"), folder("C:\\\\x"), folder("{other}/x"), folder("{app}saves"),
           folder("{app}/{unknown}/x"), folder("{app}/a/../b"), folder("{app}//x"), folder("{app}/.playden-files"),
           folder("{app}\\\\.PLAYDEN-GOG\\\\x"), folder("/etc"), folder(""), ini("{app}"), ini("{app}/"),
           ini("{app}/../x.ini"), ini("{supportDir}/a:b.ini"),
           std::string(R"({"install":{"action":"supportData","arguments":{"target":"{app}/x","type":"file"}}})"),
           std::string(R"({"install":{"action":"setIni","arguments":{"filename":"{app}/a.ini","section":"s","keyName":"k","keyValue":"a\nb"}}})"),
           std::string(R"({"install":{"action":"setIni","arguments":{"filename":"{app}/a.ini","section":"s","keyName":"k=1","keyValue":"v"}}})"),
           std::string(R"({"install":{"action":"unknownAction","arguments":{}}})"), std::string(R"({"name":"no install"})")}) {
    std::size_t skipped = 0;
    CHECK(PlanCount(action, fixture.context, &skipped) == 0);
    CHECK(skipped == 1);
  }
  // A bare `{app}` or `{supportDir}` is a folder, and a trailing separator is accepted.
  std::size_t skipped = 0;
  CHECK(PlanCount(folder("{app}"), fixture.context, &skipped) == 1 && skipped == 0);
  CHECK(PlanCount(folder("{app}\\\\saves\\\\"), fixture.context, &skipped) == 1);
  CHECK(PlanCount(copy("{supportDir}"), fixture.context, &skipped) == 1);
}

void RunsTheSteelSkyScript(const std::string& scratch) {
  const auto fixture = Fresh(scratch);
  MakeFolders(fixture.support + "/save/sub");
  Write(fixture.support + "/save/SAVE.DAT", "new");
  Write(fixture.support + "/save/NEW.DAT", "fresh");
  Write(fixture.support + "/save/sub/a.cfg", "x");
  Write(fixture.game + "/SAVE.DAT", "mine");
  Write(fixture.game + "/beneath.ini", "[scummvm]\r\nfullscreen=true\r\n\r\n[beneath]\r\ngameid=sky\r\nPATH=old\r\n");
  const auto script = ParseScript(kSteelSky);
  CHECK(script);
  const auto steps = PlanScript(*script, fixture.context, false);
  const auto result = RunScript(steps);
  if (!result.failure.empty()) std::fprintf(stderr, "failure: %s\n", result.failure.c_str());
  CHECK(result.failed == 0 && result.failure.empty() && result.skipped == 0);
  CHECK(result.changed == 4);
  CHECK(Read(fixture.game + "/beneath.ini") ==
        "[scummvm]\r\nfullscreen=true\r\n\r\n[beneath]\r\ngameid=sky\r\npath=Z:\\Games\\BASS\r\n"
        "savepath=Z:\\Games\\BASS\\saves\r\n");
  // overwrite false keeps the player's file and copies the new ones.
  CHECK(Read(fixture.game + "/SAVE.DAT") == "mine");
  CHECK(Read(fixture.game + "/NEW.DAT") == "fresh");
  CHECK(Read(fixture.game + "/sub/a.cfg") == "x");
  struct stat info {};
  CHECK(::lstat((fixture.game + "/saves").c_str(), &info) == 0 && S_ISDIR(info.st_mode));
  // No temporary file is left behind.
  CHECK(!Exists(fixture.game + "/.playden-NEW.DAT.tmp") && !Exists(fixture.game + "/.playden-beneath.ini.tmp"));
  // A second run changes nothing: the folder is there, the keys hold their values, the files are kept.
  const auto again = RunScript(steps);
  CHECK(again.changed == 0 && again.failed == 0);
  // A source spelled in another case than the folder on disk is found and walked (by path on the console).
  {
    const auto script_upper = ParseScript(R"({"actions":[{"install":{"action":"supportData","arguments":{"source":"{supportDir}\\SAVE\\SUB","target":"{app}/upper","type":"folder"}}}]})");
    CHECK(script_upper);
    const auto upper = RunScript(PlanScript(*script_upper, fixture.context, false));
    if (!upper.failure.empty()) std::fprintf(stderr, "failure: %s\n", upper.failure.c_str());
    CHECK(upper.failed == 0 && upper.changed == 1 && Read(fixture.game + "/upper/a.cfg") == "x");
  }
  // overwrite true replaces.
  auto replacing = steps[3];
  replacing.overwrite = true;
  const auto replaced = RunScript({replacing});
  CHECK(replaced.changed == 1 && replaced.failed == 0 && Read(fixture.game + "/SAVE.DAT") == "new");
  // A missing source is skipped, not a failure.
  RemoveTree(fixture.support + "/save");
  const auto missing = RunScript({steps[3]});
  CHECK(missing.skipped == 1 && missing.failed == 0 && missing.changed == 0);
}

void RefusesLinks(const std::string& scratch) {
  const auto fixture = Fresh(scratch);
  const auto outside = scratch + "/outside";
  MakeFolders(outside);
  CHECK(::symlink(outside.c_str(), (fixture.game + "/linked").c_str()) == 0);
  const auto plan = [&](const std::string& action) {
    const auto script = ParseScript(R"({"actions":[)" + action + "]}");
    CHECK(script);
    return PlanScript(*script, fixture.context, false);
  };
  // A target whose folder is a link: nothing is made or written through it.
  const auto created = RunScript(plan(R"({"name":"mk","install":{"action":"supportData","arguments":{"target":"{app}/linked/x","type":"folder"}}})"));
  CHECK(created.failed == 1 && created.changed == 0 && !created.failure.empty() &&
        created.failure.find("mk") != std::string::npos);
  CHECK(!Exists(outside + "/x"));
  const auto ini = RunScript(plan(R"({"install":{"action":"setIni","arguments":{"filename":"{app}/linked/a.ini","section":"s","keyName":"k","keyValue":"v"}}})"));
  CHECK(ini.failed == 1 && !Exists(outside + "/a.ini"));
  MakeFolders(fixture.support + "/save");
  Write(fixture.support + "/save/a.txt", "a");
  const auto copied = RunScript(plan(R"({"install":{"action":"supportData","arguments":{"source":"{supportDir}/save","target":"{app}/linked","type":"folder"}}})"));
  CHECK(copied.failed == 1 && !Exists(outside + "/a.txt"));
  // An INI file that is a link is not written through.
  Write(outside + "/target.ini", "[s]\nk=1\n");
  CHECK(::symlink((outside + "/target.ini").c_str(), (fixture.game + "/game.ini").c_str()) == 0);
  const auto linked_file = RunScript(plan(R"({"install":{"action":"setIni","arguments":{"filename":"{app}/game.ini","section":"s","keyName":"k","keyValue":"2"}}})"));
  CHECK(linked_file.failed == 1 && Read(outside + "/target.ini") == "[s]\nk=1\n");
  // A source folder that is a link is refused; a link inside a source is not copied.
  CHECK(::symlink(outside.c_str(), (fixture.support + "/linked-save").c_str()) == 0);
  const auto from_link = RunScript(plan(R"({"install":{"action":"supportData","arguments":{"source":"{supportDir}/linked-save","target":"{app}/from","type":"folder"}}})"));
  CHECK(from_link.failed == 1 && !Exists(fixture.game + "/from/target.ini"));
  CHECK(::symlink((outside + "/target.ini").c_str(), (fixture.support + "/save/link.ini").c_str()) == 0);
  const auto inner = RunScript(plan(R"({"install":{"action":"supportData","arguments":{"source":"{supportDir}/save","target":"{app}/inner","type":"folder"}}})"));
  CHECK(inner.failed == 0 && inner.changed == 1 && Read(fixture.game + "/inner/a.txt") == "a" &&
        !Exists(fixture.game + "/inner/link.ini"));
  // A file named as Playden's own record is never copied to the game folder's top.
  Write(fixture.game + "/.playden-install.json", "{\"install_bytes\":1}");
  Write(fixture.support + "/save/.playden-install.json", "{\"install_bytes\":2}");
  Write(fixture.support + "/save/.PLAYDEN-FILES", "x");
  const auto own = RunScript(plan(R"({"install":{"action":"supportData","arguments":{"overwrite":true,"source":"{supportDir}/save","target":"{app}","type":"folder"}}})"));
  CHECK(own.failed == 1 && Read(fixture.game + "/.playden-install.json") == "{\"install_bytes\":1}" &&
        Read(fixture.game + "/a.txt") == "a");
  // A copy into its own source is refused.
  const auto into_itself = RunScript(plan(R"({"install":{"action":"supportData","arguments":{"source":"{supportDir}/save","target":"{supportDir}/save/sub","type":"folder"}}})"));
  CHECK(into_itself.failed == 1 && !Exists(fixture.support + "/save/sub"));
}

void SetsIniValues(const std::string& scratch) {
  using gog::SettingIni;
  // An existing key is overwritten (unlike the recipe's SetIniValueIfEmpty), its line ending kept.
  CHECK(SettingIni("[s]\nk=old\nx=1\n", "s", "k", "v") == "[s]\nk=v\nx=1\n");
  CHECK(SettingIni("[s]\r\n  K = old \r\n", "S", "k", "v") == "[s]\r\nk=v\r\n");
  // Already holding the value: unchanged, spacing and all.
  CHECK(SettingIni("[s]\nk = v\n", "s", "k", "v") == "[s]\nk = v\n");
  // A missing key goes after the section's last non-blank line; comments and other sections stay.
  CHECK(SettingIni("[s]\nx=1\n\n[t]\ny=2\n", "S", "k", "v") == "[s]\nx=1\nk=v\n\n[t]\ny=2\n");
  CHECK(SettingIni("; top\n[s]\n;k=commented\n", "s", "k", "v") == "; top\n[s]\n;k=commented\nk=v\n");
  CHECK(SettingIni("[s]", "s", "k", "v") == "[s]\r\nk=v");
  // A missing section goes at the end, after a blank line.
  CHECK(SettingIni("[a]\nx=1\n", "s", "k", "v") == "[a]\nx=1\n\n[s]\nk=v\n");
  CHECK(SettingIni("[a]\r\nx=1", "s", "k", "v") == "[a]\r\nx=1\r\n\r\n[s]\r\nk=v\r\n");
  CHECK(SettingIni("[a]\nx=1\n\n", "s", "k", "v") == "[a]\nx=1\n\n[s]\nk=v\n");
  // An empty file uses CRLF; a byte-order mark stays first.
  CHECK(SettingIni("", "s", "k", "v") == "[s]\r\nk=v\r\n");
  CHECK(SettingIni("\xEF\xBB\xBF[s]\nk=1\n", "s", "k", "2") == "\xEF\xBB\xBF[s]\nk=2\n");
  CHECK(SettingIni("\xEF\xBB\xBF", "s", "k", "v") == "\xEF\xBB\xBF[s]\r\nk=v\r\n");

  const auto fixture = Fresh(scratch);
  const auto plan = [&](const std::string& file, const std::string& value, bool utf8 = false) {
    const auto script = ParseScript(R"({"actions":[{"install":{"action":"setIni","arguments":{"filename":")" + file +
                                    R"(","section":"cfg","keyName":"name","keyValue":")" + value +
                                    R"(","utf8":)" + (utf8 ? "true" : "false") + "}}}]}");
    CHECK(script);
    const auto steps = PlanScript(*script, fixture.context, true);
    CHECK(steps.size() == 1);
    return steps;
  };
  // A missing file is created with its folder, in CRLF.
  const auto created = RunScript(plan("{app}\\\\config\\\\new.ini", "v"));
  CHECK(created.changed == 1 && created.failed == 0);
  CHECK(Read(fixture.game + "/config/new.ini") == "[cfg]\r\nname=v\r\n");
  // LF and CRLF files keep their endings; a missing section is added.
  Write(fixture.game + "/lf.ini", "[other]\nx=1\n");
  Write(fixture.game + "/crlf.ini", "[cfg]\r\nname=old\r\n[next]\r\ny=2\r\n");
  CHECK(RunScript(plan("{app}/lf.ini", "v")).changed == 1);
  CHECK(Read(fixture.game + "/lf.ini") == "[other]\nx=1\n\n[cfg]\nname=v\n");
  CHECK(RunScript(plan("{app}/crlf.ini", "v")).changed == 1);
  CHECK(Read(fixture.game + "/crlf.ini") == "[cfg]\r\nname=v\r\n[next]\r\ny=2\r\n");
  // The file's mode is kept.
  CHECK(::chmod((fixture.game + "/crlf.ini").c_str(), 0640) == 0);
  CHECK(RunScript(plan("{app}/crlf.ini", "w")).changed == 1);
  struct stat info {};
  CHECK(::lstat((fixture.game + "/crlf.ini").c_str(), &info) == 0 && (info.st_mode & 0777) == 0640);
  // An unchanged value leaves the file alone: same bytes, same modification time.
  const struct timeval old_times[2] = {{1000000000, 0}, {1000000000, 0}};
  CHECK(::utimes((fixture.game + "/crlf.ini").c_str(), old_times) == 0);
  const auto unchanged = RunScript(plan("{app}/crlf.ini", "w"));
  CHECK(unchanged.changed == 0 && unchanged.failed == 0);
  CHECK(::lstat((fixture.game + "/crlf.ini").c_str(), &info) == 0 && info.st_mtime == 1000000000);
  CHECK(Read(fixture.game + "/crlf.ini") == "[cfg]\r\nname=w\r\n[next]\r\ny=2\r\n");
  // Existing folders and files are matched ignoring case, as Windows sees them.
  MakeFolders(fixture.game + "/Data");
  Write(fixture.game + "/Data/Game.INI", "[cfg]\nname=1\n");
  CHECK(RunScript(plan("{app}\\\\DATA\\\\game.ini", "2")).changed == 1);
  CHECK(Read(fixture.game + "/Data/Game.INI") == "[cfg]\nname=2\n");
  // utf8 false writes Latin-1 when the value fits; utf8 true keeps UTF-8; a value that doesn't fit stays UTF-8.
  CHECK(RunScript(plan("{app}/latin.ini", "caf\xC3\xA9")).changed == 1);
  CHECK(Read(fixture.game + "/latin.ini") == "[cfg]\r\nname=caf\xE9\r\n");
  CHECK(RunScript(plan("{app}/utf8.ini", "caf\xC3\xA9", true)).changed == 1);
  CHECK(Read(fixture.game + "/utf8.ini") == "[cfg]\r\nname=caf\xC3\xA9\r\n");
  CHECK(RunScript(plan("{app}/wide.ini", "\xE2\x82\xAC")).changed == 1);
  CHECK(Read(fixture.game + "/wide.ini") == "[cfg]\r\nname=\xE2\x82\xAC\r\n");
  // A folder where the file goes is a failure, reported once.
  MakeFolders(fixture.game + "/folder.ini");
  const auto refused = RunScript(plan("{app}/folder.ini", "v"));
  CHECK(refused.failed == 1 && refused.changed == 0 && !refused.failure.empty());
}

void RunsTheFixtureScript(const std::string& scratch, const std::string& fixtures) {
  // The synthetic fixture: a setIni with `conditions` and `keyType`, which are not read.
  const auto fixture = Fresh(scratch);
  const auto script = gog::ReadScript(fixtures + "/goggame-1900000001.script");
  CHECK(script && script->actions.size() == 1 && script->actions[0].languages == std::vector<std::string>{"en-US"});
  auto context = fixture.context;
  context.product = "1900000001";
  const auto steps = PlanScript(*script, context, true);
  CHECK(steps.size() == 1 && steps[0].target == Game(fixture, "synthetic.cfg") && steps[0].section == "system" &&
        steps[0].key == "language" && steps[0].value == "english");
  const auto result = RunScript(steps);
  CHECK(result.changed == 1 && result.failed == 0);
  CHECK(Read(fixture.game + "/synthetic.cfg") == "[system]\r\nlanguage=english\r\n");
  context.gog_language = "de-DE";
  CHECK(PlanScript(*script, context, true).empty());
}

// The registry values of the synthetic fixture (setRegistry in the shape of owned games' scripts) for the install's
// language, and what is skipped and counted.
void PlansRegistryValues(const std::string& scratch, const std::string& fixtures) {
  const auto fixture = Fresh(scratch);
  const auto script = gog::ReadScript(fixtures + "/goggame-1900000003.script");
  CHECK(script && script->actions.size() == 12);
  auto context = fixture.context;
  context.product = "1900000003";
  context.windows_app = "Z:\\data\\playden-games\\gog-1900000003";
  std::size_t skipped = 99;
  const auto settings = PlanRegistry(*script, context, &skipped);
  // A key with no value, binary, HKEY_CLASSES_ROOT, a control character, {publisher}, {userappdata} and a name with
  // a backslash are skipped and counted; the de-DE action is not for this install and not counted (PlanScript's rule).
  CHECK(skipped == 7);
  CHECK(settings.size() == 3);
  // HKLM, `String`: {productID} in the key, {app} in the data.
  CHECK(settings[0].machine && settings[0].key == "Software\\GOG.com\\Games\\1900000003" &&
        settings[0].name == "DOSBOXFOLDER" && !settings[0].default_value &&
        settings[0].type == RegistrySetting::Type::kString &&
        settings[0].data == "Z:\\data\\playden-games\\gog-1900000003\\DOSBox");
  // An empty name is the key's default value; `string` in lower case; the key keeps its capitals.
  CHECK(settings[1].machine && settings[1].key == "SOFTWARE\\Synthetic Games\\Synthetic" && settings[1].name.empty() &&
        settings[1].default_value && settings[1].type == RegistrySetting::Type::kString &&
        settings[1].data == "Z:\\data\\playden-games\\gog-1900000003");
  // HKCU dword 0x10, with `/` in the key.
  CHECK(!settings[2].machine && settings[2].key == "Software\\Synthetic Games\\Synthetic" && settings[2].name == "Volume" &&
        settings[2].type == RegistrySetting::Type::kDword && settings[2].data == "16");
  // The German install gets the German value too.
  auto german = context;
  german.gog_language = "de-DE";
  const auto german_settings = PlanRegistry(*script, german, &skipped);
  CHECK(german_settings.size() == 4 && skipped == 7 && german_settings[3].name == "Language" &&
        german_settings[3].data == "Deutsch");
  // PlanScript no longer counts setRegistry: only the folder.
  const auto steps = PlanScript(*script, context, false, &skipped);
  CHECK(steps.size() == 1 && skipped == 0 && steps[0].kind == ScriptStep::Kind::kCreateFolder);

  const auto one = [&](const std::string& arguments, std::size_t* count) {
    const auto parsed = ParseScript(R"({"actions":[{"install":{"action":"setRegistry","arguments":)" + arguments + "}}]}");
    CHECK(parsed);
    return PlanRegistry(*parsed, context, count);
  };
  std::size_t count = 99;
  // Roots and types ignore case; a missing name is the default value; a dword may be decimal or a JSON number.
  auto planned = one(R"({"root":"hkey_current_user","subkey":"\\\\Software\\\\\\\\A\\\\","valueData":"4294967295","valueType":"DWORD"})", &count);
  CHECK(count == 0 && planned.size() == 1 && !planned[0].machine && planned[0].key == "Software\\A" &&
        planned[0].default_value && planned[0].data == "4294967295");
  planned = one(R"({"root":"HKEY_LOCAL_MACHINE","subkey":"Software\\A","valueName":"N","valueData":7,"valueType":"dword"})", &count);
  CHECK(count == 0 && planned.size() == 1 && planned[0].data == "7");
  // {app} and {productID} in a name; a string with no data is empty.
  planned = one(R"({"root":"HKEY_LOCAL_MACHINE","subkey":"Software\\A","valueName":"id {productID}","valueType":"string"})", &count);
  CHECK(count == 0 && planned.size() == 1 && planned[0].name == "id 1900000003" && planned[0].data.empty());
  // Skipped: a dword past 32 bits, not a number, signed, octal-looking hex, empty; a quote in the name; a key that is
  // only separators or too long; non-ASCII data; a name that isn't a string.
  for (const auto& arguments : {
           std::string(R"({"root":"HKEY_CURRENT_USER","subkey":"A","valueData":"4294967296","valueType":"dword"})"),
           std::string(R"({"root":"HKEY_CURRENT_USER","subkey":"A","valueData":"0x100000000","valueType":"dword"})"),
           std::string(R"({"root":"HKEY_CURRENT_USER","subkey":"A","valueData":"ten","valueType":"dword"})"),
           std::string(R"({"root":"HKEY_CURRENT_USER","subkey":"A","valueData":"-1","valueType":"dword"})"),
           std::string(R"({"root":"HKEY_CURRENT_USER","subkey":"A","valueData":"0x","valueType":"dword"})"),
           std::string(R"({"root":"HKEY_CURRENT_USER","subkey":"A","valueData":"","valueType":"dword"})"),
           std::string(R"({"root":"HKEY_CURRENT_USER","subkey":"A","valueName":"a\"b","valueData":"v","valueType":"string"})"),
           std::string(R"({"root":"HKEY_CURRENT_USER","subkey":"\\\\/","valueData":"v","valueType":"string"})"),
           std::string(R"({"root":"HKEY_CURRENT_USER","subkey":")") + std::string(1025, 'k') + R"(","valueData":"v","valueType":"string"})",
           std::string(R"({"root":"HKEY_CURRENT_USER","subkey":"A","valueData":"caf\u00e9","valueType":"string"})"),
           std::string(R"({"root":"HKEY_CURRENT_USER","subkey":"A","valueName":1,"valueData":"v","valueType":"string"})"),
           std::string(R"({"root":"HKEY_USERS","subkey":"A","valueData":"v","valueType":"string"})"),
           std::string(R"({"subkey":"A","valueData":"v","valueType":"string"})")}) {
    count = 0;
    CHECK(one(arguments, &count).empty() && count == 1);
  }
  // Exactly 1024 bytes of key is taken.
  CHECK(one(R"({"root":"HKEY_CURRENT_USER","subkey":")" + std::string(1024, 'k') + R"(","valueData":"v","valueType":"string"})",
            &count).size() == 1);
}

void Parsing() {
  CHECK(ParseScript("\xEF\xBB\xBF{\"actions\":[]}"));
  CHECK(!ParseScript("{\"actions\":["));
  CHECK(!ParseScript("[]"));
  CHECK(!ParseScript("{\"actions\":[{\"name\":\"" + std::string(1024 * 1024, 'n') + "\"}]}"));
  const auto mixed = ParseScript(R"({"actions":[7,{"name":"a","install":{"action":"setIni"}}]})");
  CHECK(mixed && mixed->actions.size() == 1 && mixed->actions[0].install &&
        mixed->actions[0].install->action == "setIni");
  const auto none = ParseScript(R"({"productId":"7"})");
  CHECK(none && none->actions.empty());
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::fprintf(stderr, "usage: %s <fixtures/gog> <scratch folder>\n", argv[0]);
    return 2;
  }
  const std::string fixtures = argv[1];
  const std::string scratch = argv[2];
  Parsing();
  PlansForTheLanguage(scratch);
  DropsEscapes(scratch);
  RunsTheSteelSkyScript(scratch);
  RefusesLinks(scratch);
  SetsIniValues(scratch);
  RunsTheFixtureScript(scratch, fixtures);
  PlansRegistryValues(scratch, fixtures);
  RemoveTree(scratch);
  return 0;
}
