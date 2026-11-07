// my_presets_test.cpp - The player's own presets: rules, changes, my-presets.json and its bad-file handling.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime/my_presets.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "%d: %s\n", __LINE__, #c); std::exit(1); } } while (0)

namespace {

using namespace playden::runtime;
namespace fs = std::filesystem;
using Settings = std::map<std::string, std::string>;
using R = MyPresetRefusal;

std::string Id(int n) {
  char text[32];
  std::snprintf(text, sizeof(text), "my-%016x", n);
  return text;
}

std::string Read(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  CHECK(input.good());
  return {std::istreambuf_iterator<char>(input), {}};
}

void Write(const fs::path& path, const std::string& text) {
  std::ofstream(path, std::ios::binary | std::ios::trunc) << text;
  CHECK(::chmod(path.c_str(), 0600) == 0);
}

mode_t Mode(const fs::path& path) {
  struct stat status {};
  CHECK(::lstat(path.c_str(), &status) == 0);
  return status.st_mode & 07777;
}

void Ids() {
  CHECK(IsMyPresetId("my-3f9a0c1d2e4b5a69"));
  CHECK(!IsMyPresetId("my-3F9A0C1D2E4B5A69") && !IsMyPresetId("my-3f9a0c1d2e4b5a6") && !IsMyPresetId("my-3f9a0c1d2e4b5a690") &&
        !IsMyPresetId("xx-3f9a0c1d2e4b5a69") && !IsMyPresetId("my-3f9a0c1d2e4b5a6g") && !IsMyPresetId("mouse"));
  // The UUID's version (4) and variant (8-b) digits are left out: all 16 are random.
  CHECK(MyPresetIdFromUuid("3f9a0c1d-2e4b-4a69-8bcd-0123456789ab") == std::optional<std::string>("my-3f9a0c1d2e4ba69b"));
  CHECK(MyPresetIdFromUuid("3F9A0C1D-2E4B-4A69-8BCD-0123456789AB") == std::optional<std::string>("my-3f9a0c1d2e4ba69b"));
  CHECK(!MyPresetIdFromUuid("3f9a0c1d2e4b4a698bcd0123456789ab"));
  CHECK(!MyPresetIdFromUuid("3f9a0c1d-2e4b-4a69-8bcd-0123456789"));
  CHECK(!MyPresetIdFromUuid("zf9a0c1d-2e4b-4a69-8bcd-0123456789ab"));
}

void Changes() {
  std::vector<MyPreset> presets;
  const Settings mouse{{"controller_preset", "mouse"}, {"resolution", "1280x720"}, {"button.cross", "mouse_left"}};
  // Added with its id, trimmed name, settings and times.
  CHECK(AddMyPreset(&presets, Id(1), "  Point and click ", mouse, 100).ok());
  CHECK(presets.size() == 1 && presets[0].preset.id == Id(1) && presets[0].preset.name == "Point and click" &&
        presets[0].preset.settings == mouse && presets[0].created == 100 && presets[0].updated == 100);
  CHECK(FindMyPreset(presets, Id(1)) == &presets[0] && !FindMyPreset(presets, Id(2)));
  const auto before = presets;

  // Every refusal leaves the list as it was.
  const auto refused = [&](MyPresetChange change, MyPresetRefusal refusal) {
    CHECK(change.refusal == refusal && !change.ok());
    CHECK(presets == before);
  };
  refused(AddMyPreset(&presets, Id(2), "   ", mouse, 101), R::kNameEmpty);
  refused(AddMyPreset(&presets, Id(2), "", mouse, 101), R::kNameEmpty);
  refused(AddMyPreset(&presets, Id(2), std::string(41, 'a'), mouse, 101), R::kNameTooLong);
  refused(AddMyPreset(&presets, Id(2), "a\tb", mouse, 101), R::kNameNotPrintable);
  refused(AddMyPreset(&presets, Id(2), "bad \xff utf-8", mouse, 101), R::kNameNotPrintable);
  refused(AddMyPreset(&presets, Id(2), "zero\xe2\x80\x8bwidth", mouse, 101), R::kNameNotPrintable);
  // Taken ignoring case and accents.
  refused(AddMyPreset(&presets, Id(2), "POINT AND CLICK", mouse, 101), R::kNameTaken);
  refused(AddMyPreset(&presets, Id(2), "P\xc3\xb3int and cl\xc3\xad" "ck", mouse, 101), R::kNameTaken);
  refused(AddMyPreset(&presets, Id(1), "Other", mouse, 101), R::kBadId);
  refused(AddMyPreset(&presets, "mouse", "Other", mouse, 101), R::kBadId);
  refused(AddMyPreset(&presets, Id(2), "Other", {}, 101), R::kNoSettings);
  refused(AddMyPreset(&presets, Id(2), "Other", {{"opengl_thread", "off"}}, 101), R::kNoSettings);
  for (const auto& setting : std::vector<Settings>{{{"graphics", "dxvk"}},
                                                   {{"architecture", "pe64"}},
                                                   {{"launch_option", "game"}},
                                                   {{"start_check", "off"}},
                                                   {{"program", "Game.exe"}},
                                                   {{"extra_arguments", "-windowed"}},
                                                   {{"library_overrides", "d3d9=n"}},
                                                   {{"no_such_setting", "x"}},
                                                   {{"resolution", "huge"}},
                                                   {{"button.cross", "not a key"}}}) {
    const auto change = AddMyPreset(&presets, Id(2), "Other", setting, 101);
    refused(change, R::kSettingRefused);
    CHECK(change.detail.find(setting.begin()->first) == 0);
  }
  refused(RenameMyPreset(&presets, Id(9), "Other", 101), R::kUnknownId);
  refused(ReplaceMyPresetSettings(&presets, Id(9), mouse, 101), R::kUnknownId);
  refused(DeleteMyPreset(&presets, Id(9)), R::kUnknownId);
  refused(RenameMyPreset(&presets, Id(1), " ", 101), R::kNameEmpty);
  refused(ReplaceMyPresetSettings(&presets, Id(1), {{"graphics", "dxvk"}}, 101), R::kSettingRefused);
  refused(ReplaceMyPresetSettings(&presets, Id(1), {}, 101), R::kNoSettings);

  // 40 characters is the limit, not 40 bytes; a built-in preset's name is allowed.
  const std::string accented = [] {
    std::string text;
    for (int i = 0; i < 40; ++i) text += "\xc3\xa9";
    return text;
  }();
  CHECK(AddMyPreset(&presets, Id(2), accented, {{"refresh", "60"}}, 102).ok());
  CHECK(AddMyPreset(&presets, Id(3), "Mouse-driven", {{"controller_preset", "mouse"}}, 103).ok());
  // A retired setting is dropped, the rest kept.
  CHECK(AddMyPreset(&presets, Id(4), "Old", {{"opengl_thread", "off"}, {"refresh", "60"}}, 104).ok());
  CHECK(presets.back().preset.settings == (Settings{{"refresh", "60"}}));

  // Renaming to its own name in another case is allowed; to another's is not.
  CHECK(RenameMyPreset(&presets, Id(1), "point and CLICK", 110).ok());
  CHECK(presets[0].preset.name == "point and CLICK" && presets[0].updated == 110 && presets[0].created == 100);
  CHECK(RenameMyPreset(&presets, Id(1), "mouse-DRIVEN", 111).refusal == R::kNameTaken);
  CHECK(presets[0].preset.name == "point and CLICK" && presets[0].updated == 110);

  CHECK(ReplaceMyPresetSettings(&presets, Id(1), {{"controller_preset", "arrows"}, {"opengl_thread", "on"}}, 120).ok());
  CHECK(presets[0].preset.settings == (Settings{{"controller_preset", "arrows"}}) && presets[0].updated == 120);

  CHECK(DeleteMyPreset(&presets, Id(3)).ok() && presets.size() == 3 && !FindMyPreset(presets, Id(3)));
  // A deleted preset's name is free again.
  CHECK(AddMyPreset(&presets, Id(5), "Mouse-driven", {{"controller_preset", "mouse"}}, 130).ok());

  const auto as_presets = MyPresetsAsSettingsPresets(presets);
  CHECK(as_presets.size() == presets.size() && as_presets[0].id == Id(1) && as_presets[0].settings == presets[0].preset.settings);

  // At most 50.
  std::vector<MyPreset> full;
  for (int n = 0; n < static_cast<int>(kMyPresetsMax); ++n)
    CHECK(AddMyPreset(&full, Id(100 + n), "Preset " + std::to_string(n), {{"refresh", "60"}}, n).ok());
  const auto at_cap = full;
  CHECK(AddMyPreset(&full, Id(999), "One more", {{"refresh", "60"}}, 1).refusal == R::kTooMany && full == at_cap);
  CHECK(DeleteMyPreset(&full, Id(100)).ok() && AddMyPreset(&full, Id(999), "One more", {{"refresh", "60"}}, 1).ok());
}

void Format() {
  std::vector<MyPreset> presets;
  CHECK(AddMyPreset(&presets, Id(1), "Quote \" and \\ caf\xc3\xa9", {{"controller_preset", "mouse"}, {"button.cross", "mouse_left"}},
                    1762473600).ok());
  CHECK(AddMyPreset(&presets, Id(2), "Keyboard", {{"button.square", "space"}}, 1762473700).ok());
  CHECK(RenameMyPreset(&presets, Id(2), "Keyboard only", 1762473800).ok());
  const auto text = SerializeMyPresets(presets);
  std::vector<MyPreset> loaded;
  std::vector<std::string> dropped;
  std::string reason;
  CHECK(ParseMyPresets(text, &loaded, &dropped, &reason) && loaded == presets && dropped.empty());
  CHECK(ParseMyPresets(R"({"format":1,"presets":[]})", &loaded, &dropped, &reason) && loaded.empty());

  // Retired settings and settings this build refuses (a newer Playden's) are dropped; the preset stays.
  CHECK(ParseMyPresets(R"({"format":1,"presets":[{"id":"my-0000000000000001","name":"A","created":1,"updated":2,
      "settings":{"refresh":"60","opengl_thread":"off","future_setting":"x","graphics":"dxvk"}}]})",
                       &loaded, &dropped, &reason));
  CHECK(loaded.size() == 1 && loaded[0].preset.settings == (Settings{{"refresh", "60"}}) && loaded[0].created == 1 &&
        loaded[0].updated == 2);
  CHECK(dropped == (std::vector<std::string>{"my-0000000000000001 opengl_thread", "my-0000000000000001 future_setting",
                                             "my-0000000000000001 graphics"}));

  const auto refused = [&](const std::string& json) {
    std::vector<MyPreset> out = presets;
    CHECK(!ParseMyPresets(json, &out, nullptr, &reason) && !reason.empty() && out == presets);
  };
  const std::string good = R"("name":"A","created":1,"updated":2,"settings":{})";
  refused("not json");
  refused("[]");
  refused(R"({"presets":[]})");
  refused(R"({"format":2,"presets":[]})");
  refused(R"({"format":1})");
  refused(R"({"format":1,"presets":{}})");
  refused(R"({"format":1,"presets":[1]})");
  refused(R"({"format":1,"presets":[{"id":"mouse",)" + good + "}]}");
  refused(R"({"format":1,"presets":[{)" + good + "}]}");
  refused(R"({"format":1,"presets":[{"id":"my-0000000000000001",)" + good + R"(},{"id":"my-0000000000000001","name":"B","created":1,"updated":2,"settings":{}}]})");
  // Two names that differ only in case and accents ("A" and "á") load: the rule is for changes.
  CHECK(ParseMyPresets(R"({"format":1,"presets":[{"id":"my-0000000000000001",)" + good +
          R"(},{"id":"my-0000000000000002","name":")" + "\xc3\xa1" + R"(","created":1,"updated":2,"settings":{}}]})",
      &loaded, nullptr, &reason) && loaded.size() == 2);
  CHECK(RenameMyPreset(&loaded, "my-0000000000000001", "\xc3\x81", 3).refusal == R::kNameTaken);
  refused(R"({"format":1,"presets":[{"id":"my-0000000000000001","name":"","created":1,"updated":2,"settings":{}}]})");
  refused(R"({"format":1,"presets":[{"id":"my-0000000000000001","name":" A","created":1,"updated":2,"settings":{}}]})");
  refused(R"({"format":1,"presets":[{"id":"my-0000000000000001","name":"A","updated":2,"settings":{}}]})");
  refused(R"({"format":1,"presets":[{"id":"my-0000000000000001","name":"A","created":-1,"updated":2,"settings":{}}]})");
  refused(R"({"format":1,"presets":[{"id":"my-0000000000000001","name":"A","created":1,"updated":2}]})");
  refused(R"({"format":1,"presets":[{"id":"my-0000000000000001","name":"A","created":1,"updated":2,"settings":{"refresh":60}}]})");

  // More than 50 in a file still load; only adding is capped.
  std::string many = R"({"format":1,"presets":[)";
  for (int n = 0; n < 60; ++n)
    many += std::string(n ? "," : "") + "{\"id\":\"" + Id(n) + "\",\"name\":\"P" + std::to_string(n) +
            "\",\"created\":1,\"updated\":1,\"settings\":{}}";
  many += "]}";
  CHECK(ParseMyPresets(many, &loaded, nullptr, &reason) && loaded.size() == 60);
}

void Storage() {
  char pattern[] = "/tmp/playden-my-presets-XXXXXX";
  CHECK(::mkdtemp(pattern));
  const auto root = fs::canonical(pattern);
  const auto folder = root / "game-settings-private";
  CHECK(::mkdir(folder.c_str(), 0700) == 0);
  const auto path = MyPresetsPath(folder / "game-settings.json");
  CHECK(path == folder / "my-presets.json");
  const auto bad = folder / kMyPresetsBadFileName;

  // No file: no presets.
  auto load = LoadMyPresetsFile(path.string());
  CHECK(load.status == MyPresetsLoadStatus::kLoaded && load.presets.empty());

  std::vector<MyPreset> presets;
  CHECK(AddMyPreset(&presets, Id(1), "Point and click", {{"controller_preset", "mouse"}}, 5).ok());
  std::string reason;
  CHECK(SaveMyPresetsFile(path.string(), presets, &reason));
  CHECK(Mode(path) == 0600);
  load = LoadMyPresetsFile(path.string());
  CHECK(load.status == MyPresetsLoadStatus::kLoaded && load.presets == presets);

  // A file that isn't presets is kept aside, never written over; Playden starts with none and can save again.
  const std::string garbage = "{\"format\":1,\"presets\":[{\"id\":\"broken\"}]}";
  Write(path, garbage);
  load = LoadMyPresetsFile(path.string());
  CHECK(load.status == MyPresetsLoadStatus::kSetAside && load.presets.empty() && !load.reason.empty());
  CHECK(!fs::exists(path) && Read(bad) == garbage);
  CHECK(SaveMyPresetsFile(path.string(), presets, &reason));
  CHECK(LoadMyPresetsFile(path.string()).presets == presets);

  // Another bad file while one is already kept aside: both stay, nothing is shown or saved.
  Write(path, "truncated {");
  load = LoadMyPresetsFile(path.string());
  CHECK(load.status == MyPresetsLoadStatus::kUnavailable && load.presets.empty());
  CHECK(Read(path) == "truncated {" && Read(bad) == garbage);
  CHECK(!SaveMyPresetsFile(path.string(), presets, &reason) && Read(path) == "truncated {");
  fs::remove(bad);

  // A newer Playden's format stays in place for it: nothing shown, nothing saved, nothing set aside.
  const std::string newer = R"({"format":2,"presets":[]})";
  Write(path, newer);
  load = LoadMyPresetsFile(path.string());
  CHECK(load.status == MyPresetsLoadStatus::kUnavailable && load.presets.empty());
  CHECK(Read(path) == newer && !fs::exists(bad));
  CHECK(!SaveMyPresetsFile(path.string(), presets, &reason) && Read(path) == newer);

  // A file that can't be read safely is not set aside either.
  Write(path, SerializeMyPresets(presets));
  CHECK(::chmod(path.c_str(), 0666) == 0);
  CHECK(LoadMyPresetsFile(path.string()).status == MyPresetsLoadStatus::kUnavailable && !fs::exists(bad));
  CHECK(!SaveMyPresetsFile(path.string(), presets, &reason));
  CHECK(::chmod(path.c_str(), 0600) == 0);
  fs::remove(path);
  fs::create_symlink(root / "elsewhere.json", path);
  CHECK(LoadMyPresetsFile(path.string()).status == MyPresetsLoadStatus::kUnavailable && !fs::exists(bad));
  fs::remove(path);

  // The largest file Playden can write still reads back: 50 presets, each setting every setting a preset may set,
  // with the longest names.
  {
    Settings every;
    for (const auto& definition : SettingsCatalogue()) {
      std::vector<std::string> candidates{"mouse_middle", "1920x1080"};
      for (const auto& choice : definition.choices) candidates.push_back(choice.value);
      for (const auto& value : candidates)
        if (ValidatePresetSetting(definition.id, value, nullptr)) {
          every[definition.id] = value;
          break;
        }
    }
    CHECK(every.size() > 40);
    std::vector<MyPreset> full;
    for (int n = 0; n < static_cast<int>(kMyPresetsMax); ++n) {
      std::string name = std::to_string(n);
      for (std::size_t characters = name.size(); characters < kMyPresetNameMaxCharacters; ++characters)
        name += "\xf0\x9f\x8e\xae";  // 4-byte characters
      CHECK(AddMyPreset(&full, Id(1000 + n), name, every, 1762473600).ok());
    }
    CHECK(SaveMyPresetsFile(path.string(), full, &reason));
    load = LoadMyPresetsFile(path.string());
    CHECK(load.status == MyPresetsLoadStatus::kLoaded && load.presets == full && load.dropped.empty());
    fs::remove(path);
  }

  // The folder must be private.
  CHECK(::chmod(folder.c_str(), 0755) == 0);
  CHECK(!SaveMyPresetsFile(path.string(), presets, &reason) && !fs::exists(path));
  CHECK(::chmod(folder.c_str(), 0700) == 0);
  CHECK(SaveMyPresetsFile(path.string(), presets, &reason));
  fs::remove_all(root);
}

}  // namespace

int main() {
  Ids();
  Changes();
  Format();
  Storage();
  std::puts("my presets tests passed");
}
