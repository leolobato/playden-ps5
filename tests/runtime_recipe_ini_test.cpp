// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime/recipe_ini.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(condition)                                                                  \
  do {                                                                                    \
    if (!(condition)) {                                                                   \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
      std::exit(1);                                                                       \
    }                                                                                     \
  } while (false)

namespace fs = std::filesystem;
using namespace playden::runtime;

namespace {
void Write(const fs::path& path, const std::string& bytes) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << bytes;
}
std::string Read(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), {}};
}
std::string Set(std::string text, const std::string& section, const std::string& key, const std::string& value,
                bool* changed = nullptr) {
  const bool did = SetIniValueIfEmpty(&text, section, key, value);
  if (changed) *changed = did;
  return text;
}
}  // namespace

int main() {
  // Batman: Arkham Asylum's shipped UserEngine.ini (CRLF): Language goes after the section's last line.
  const std::string asylum =
      "[Configuration]\r\nBasedOn=..\\BmGame\\Config\\DefaultEngine.ini\r\n\r\n[Engine.Engine]\r\n"
      "bOnScreenKismetWarnings=FALSE\r\nbSubtitlesEnabled=FALSE\r\n";
  bool changed = false;
  CHECK(Set(asylum, "Engine.Engine", "Language", "int", &changed) == asylum + "Language=int\r\n" && changed);
  // The player's own language stays, whatever the case of the section and key.
  const auto chosen = asylum + "Language=fra\r\n";
  CHECK(Set(chosen, "engine.engine", "LANGUAGE", "int", &changed) == chosen && !changed);
  // An empty value is filled where it is, with spaces around '=' and the line ending kept.
  CHECK(Set("[Engine.Engine]\r\nLanguage = \r\nOther=1\r\n", "Engine.Engine", "Language", "int") ==
        "[Engine.Engine]\r\nLanguage = int\r\nOther=1\r\n");
  // Inserted before blank lines and the next section, in the first matching section.
  CHECK(Set("[A]\nx=1\n\n[B]\ny=2\n[A]\nz=3\n", "A", "Language", "int") == "[A]\nx=1\nLanguage=int\n\n[B]\ny=2\n[A]\nz=3\n");
  // A value in a repeated section counts too.
  CHECK(Set("[A]\nx=1\n[B]\n[A]\nLanguage=deu\n", "A", "Language", "int", &changed) == "[A]\nx=1\n[B]\n[A]\nLanguage=deu\n" &&
        !changed);
  // Comments and other keys that start the same way don't count.
  CHECK(Set("[A]\n;Language=deu\nLanguageX=1\n", "A", "Language", "int") == "[A]\n;Language=deu\nLanguageX=1\nLanguage=int\n");
  // An empty section, the last line without a line ending, a missing section and an empty file.
  CHECK(Set("[A]\n[B]\n", "A", "k", "v") == "[A]\nk=v\n[B]\n");
  CHECK(Set("[A]\nx=1", "A", "k", "v") == "[A]\nx=1\nk=v\n");
  CHECK(Set("[A]\r\nx=1\r\n", "Engine.Engine", "Language", "int") == "[A]\r\nx=1\r\n\r\n[Engine.Engine]\r\nLanguage=int\r\n");
  CHECK(Set("[A]\nx=1", "B", "k", "v") == "[A]\nx=1\n\n[B]\nk=v\n");
  CHECK(Set("", "B", "k", "v") == "[B]\nk=v\n");
  // A UTF-8 byte-order mark stays in front and doesn't hide the first section.
  const std::string bom = "\xEF\xBB\xBF";
  CHECK(Set(bom + "[A]\r\nLanguage=fra\r\n", "A", "Language", "int", &changed) == bom + "[A]\r\nLanguage=fra\r\n" && !changed);
  CHECK(Set(bom + "[A]\r\nx=1\r\n", "A", "Language", "int") == bom + "[A]\r\nx=1\r\nLanguage=int\r\n");
  CHECK(Set(bom, "B", "k", "v") == bom + "[B]\nk=v\n");
  // Headers match with spaces inside the brackets.
  CHECK(Set("[ A ]\nLanguage=deu\n", "A", "Language", "int", &changed) == "[ A ]\nLanguage=deu\n" && !changed);

  const auto root = fs::temp_directory_path() / ("playden-recipe-ini-" + std::to_string(::getpid()));
  fs::remove_all(root);
  const auto config = root / "BmGame/Config";
  Write(config / "UserEngine.ini", asylum);
  ::chmod((config / "UserEngine.ini").c_str(), 0666);
  const std::vector<IniValue> values = {{"BmGame/Config/UserEngine.ini", "Engine.Engine", "Language", "int"}};
  auto result = ApplyRecipeIniValues(root.string(), values);
  CHECK(result.ok && result.changed == std::vector<std::string>{"BmGame/Config/UserEngine.ini"} && result.skipped.empty());
  CHECK(Read(config / "UserEngine.ini") == asylum + "Language=int\r\n");
  // The file keeps its permissions (the game writes it back), and no temporary file is left.
  struct stat info {};
  CHECK(::stat((config / "UserEngine.ini").c_str(), &info) == 0 && (info.st_mode & 0777) == 0666);
  CHECK(std::distance(fs::directory_iterator(config), fs::directory_iterator()) == 1);
  // Idempotent: nothing to change, nothing written.
  const auto written = fs::last_write_time(config / "UserEngine.ini");
  result = ApplyRecipeIniValues(root.string(), values);
  CHECK(result.ok && result.changed.empty() && fs::last_write_time(config / "UserEngine.ini") == written);

  // File names match ignoring case, as on Windows; several values in one file are one write.
  fs::remove_all(root);
  Write(root / "bmgame/CONFIG/userengine.INI", "[Engine.Engine]\n");
  result = ApplyRecipeIniValues(root.string(), {{"BmGame/Config/UserEngine.ini", "Engine.Engine", "Language", "int"},
                                                {"BmGame/Config/UserEngine.ini", "Engine.Engine", "Other", "1"}});
  CHECK(result.ok && result.changed.size() == 1);
  CHECK(Read(root / "bmgame/CONFIG/userengine.INI") == "[Engine.Engine]\nLanguage=int\nOther=1\n");

  // A missing file is not created; a link is not followed; UTF-16 and oversized files are left alone.
  fs::remove_all(root);
  fs::create_directories(root / "Config");
  Write(root / "outside.ini", "[A]\n");
  fs::create_symlink(root / "outside.ini", root / "Config/Linked.ini");
  fs::create_symlink(root / "Config", root / "Folder");
  Write(root / "Config/Wide.ini", std::string("\xFF\xFE[\0A\0]\0", 8));
  Write(root / "Config/Big.ini", "[A]\n" + std::string(300 * 1024, ';'));
  result = ApplyRecipeIniValues(root.string(), {{"Config/Missing.ini", "A", "k", "v"},
                                                {"Config/Linked.ini", "A", "k", "v"},
                                                {"Folder/Wide.ini", "A", "k", "v"},
                                                {"Config/Wide.ini", "A", "k", "v"},
                                                {"Config/Big.ini", "A", "k", "v"}});
  CHECK(result.ok && result.changed.empty());
  CHECK(result.skipped == (std::vector<std::string>{"Config/Missing.ini: missing", "Config/Linked.ini: a link",
                                                    "Folder/Wide.ini: a link", "Config/Wide.ini: not a plain text INI",
                                                    "Config/Big.ini: unreadable or too large"}));
  CHECK(!fs::exists(root / "Config/Missing.ini") && Read(root / "outside.ini") == "[A]\n");

  // A folder the file can't be written in fails with a sentence for the player.
  if (::geteuid() != 0) {
    fs::remove_all(root);
    Write(root / "Config/Game.ini", "[A]\n");
    ::chmod((root / "Config").c_str(), 0555);
    result = ApplyRecipeIniValues(root.string(), {{"Config/Game.ini", "A", "k", "v"}});
    CHECK(!result.ok && result.message.find("Config/Game.ini") != std::string::npos);
    CHECK(Read(root / "Config/Game.ini") == "[A]\n");
    ::chmod((root / "Config").c_str(), 0755);
  }

  // A flat key=value file (UNCHARTED's screeninfo.cfg): only the key's value changes, every other byte stays.
  {
    const std::string screen = "DisplayMode=1\nResolutionWidth=1920\nDeviceId=56a0\nMonitor=0\nDLSS=0\n";
    std::string text = screen;
    std::optional<std::string> held;
    CHECK(ReplaceKeyValue(&text, "DeviceId", "13fe", &held) && held == std::string("56a0"));
    CHECK(text == "DisplayMode=1\nResolutionWidth=1920\nDeviceId=13fe\nMonitor=0\nDLSS=0\n");
    // The same value, in another case, is left as it is; so is a file without the key.
    text = "deviceid = 13FE\r\nMonitor=0\r\n";
    CHECK(!ReplaceKeyValue(&text, "DeviceId", "13fe", &held) && held == std::string("13FE") &&
          text == "deviceid = 13FE\r\nMonitor=0\r\n");
    text = "Monitor=0\nDeviceIdOld=0\n";
    CHECK(!ReplaceKeyValue(&text, "DeviceId", "13fe", &held) && !held && text == "Monitor=0\nDeviceIdOld=0\n");
    // Spaces around the old value and the CRLF ending: the value goes after '=', the line ending stays.
    text = "DeviceId = 0 \r\nMonitor=0\r\n";
    CHECK(ReplaceKeyValue(&text, "DeviceId", "13fe", &held) && held == std::string("0") &&
          text == "DeviceId =13fe\r\nMonitor=0\r\n");
    // The last line, with no line ending.
    text = "Monitor=0\nDeviceId=0";
    CHECK(ReplaceKeyValue(&text, "DeviceId", "13fe", &held) && text == "Monitor=0\nDeviceId=13fe");

    fs::remove_all(root);
    Write(root / "Uncharted4_data/screeninfo.cfg", screen);
    ::chmod((root / "Uncharted4_data/screeninfo.cfg").c_str(), 0666);
    auto saved = ReplaceGameFileValue(root.string(), "uncharted4_data/ScreenInfo.cfg", "DeviceId", "13fe");
    CHECK(saved.ok && saved.found && saved.changed && saved.previous == "56a0");
    CHECK(Read(root / "Uncharted4_data/screeninfo.cfg") == "DisplayMode=1\nResolutionWidth=1920\nDeviceId=13fe\nMonitor=0\nDLSS=0\n");
    struct stat info {};
    CHECK(::stat((root / "Uncharted4_data/screeninfo.cfg").c_str(), &info) == 0 && (info.st_mode & 0777) == 0666);
    saved = ReplaceGameFileValue(root.string(), "Uncharted4_data/screeninfo.cfg", "DeviceId", "13fe");
    CHECK(saved.ok && saved.found && !saved.changed && saved.previous == "13fe");
    // No file (a copy never started), or a link: left for the game.
    saved = ReplaceGameFileValue(root.string(), "Uncharted4_data/missing.cfg", "DeviceId", "13fe");
    CHECK(saved.ok && !saved.found && !saved.changed);
    Write(root / "outside.cfg", "DeviceId=0\n");
    fs::create_symlink(root / "outside.cfg", root / "Uncharted4_data/linked.cfg");
    saved = ReplaceGameFileValue(root.string(), "Uncharted4_data/linked.cfg", "DeviceId", "13fe");
    CHECK(saved.ok && !saved.found && Read(root / "outside.cfg") == "DeviceId=0\n");
  }

  fs::remove_all(root);
  std::puts("recipe ini tests passed");
}
