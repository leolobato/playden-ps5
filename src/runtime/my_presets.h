#pragma once
// my_presets.h - The player's own presets ("My presets"): settings saved from one game to use on any game, kept per
// console in my-presets.json beside game-settings.json. A game uses one like a built-in preset: the record's
// preset_id names it, and the player's changes stay on top.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime/game_settings.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace playden::runtime {

inline constexpr std::size_t kMyPresetsMax = 50;
inline constexpr std::size_t kMyPresetNameMaxCharacters = 40;
inline constexpr char kMyPresetsFileName[] = "my-presets.json";
// Where a my-presets.json that can't be read as presets is moved, so Playden starts with none and never writes over it.
inline constexpr char kMyPresetsBadFileName[] = "my-presets.json.bad";

struct MyPreset {
  // id ("my-" and 16 lowercase hex, IsMyPresetId), name and settings; try_when and short_hint stay empty. Put it in
  // the list of presets a record is resolved with like a built-in one.
  SettingsPreset preset;
  std::int64_t created = 0;  // Unix seconds
  std::int64_t updated = 0;  // last rename or settings change
};
bool operator==(const MyPreset& left, const MyPreset& right);

// "my-" and 16 lowercase hex digits.
bool IsMyPresetId(std::string_view id);
// A new id from a random UUID (RandomUuid): "my-" and 16 of its random hex digits. Nothing for anything not a UUID.
std::optional<std::string> MyPresetIdFromUuid(std::string_view uuid);
const MyPreset* FindMyPreset(const std::vector<MyPreset>& presets, std::string_view id);
// The presets as SettingsPreset, to add to the built-in ones.
std::vector<SettingsPreset> MyPresetsAsSettingsPresets(const std::vector<MyPreset>& presets);

// Why a change to the player's presets was refused.
enum class MyPresetRefusal {
  kNone,
  kNameEmpty,         // nothing but spaces
  kNameTooLong,       // over kMyPresetNameMaxCharacters
  kNameNotPrintable,  // invalid UTF-8 or a control, format or other character a recipe title may not have
  kNameTaken,         // another preset has it, ignoring case and accents
  kTooMany,           // kMyPresetsMax already
  kUnknownId,         // no preset has this id
  kBadId,             // a new id that is not IsMyPresetId, or one already used
  kNoSettings,        // nothing left to save (after retired settings are dropped)
  kSettingRefused,    // a setting a preset may not set (ValidatePresetSetting); detail names it and why
};
struct MyPresetChange {
  MyPresetRefusal refusal = MyPresetRefusal::kNone;
  std::string detail;
  bool ok() const { return refusal == MyPresetRefusal::kNone; }
};

// The pure changes. On a refusal the list is left as it was. A name is trimmed of spaces at both ends and must then be
// 1-40 printable characters (the rule for recipe titles) unlike every other preset's name ignoring case and accents
// (FoldedName); a built-in preset's name is allowed. Settings drop retired ones (RetiredSetting) and must then be
// non-empty and each pass ValidatePresetSetting. now is Unix seconds; it sets created and updated.
MyPresetChange AddMyPreset(std::vector<MyPreset>* presets, std::string_view id, std::string_view name,
                           std::map<std::string, std::string> settings, std::int64_t now);
MyPresetChange RenameMyPreset(std::vector<MyPreset>* presets, std::string_view id, std::string_view name,
                              std::int64_t now);
MyPresetChange ReplaceMyPresetSettings(std::vector<MyPreset>* presets, std::string_view id,
                                       std::map<std::string, std::string> settings, std::int64_t now);
MyPresetChange DeleteMyPreset(std::vector<MyPreset>* presets, std::string_view id);
// The name as AddMyPreset and RenameMyPreset keep it, or the refusal (kNameEmpty, kNameTooLong, kNameNotPrintable).
MyPresetRefusal CheckMyPresetName(std::string_view name, std::string* trimmed);

// {"format":1,"presets":[{"id","name","settings":{...},"created","updated"}]}. Parsing refuses (false) a file that is
// not that: malformed JSON, another format, a preset without a valid id, name, string settings or timestamps, or two
// presets with one id. Within a preset, a retired setting and a setting this build refuses (a newer Playden's) are
// dropped and listed in dropped as "<preset id> <setting>"; the preset is kept. Like the cap, names unique ignoring
// case and accents are a rule for changes only: two names that fold alike (another build's folding) both load, and
// more than kMyPresetsMax presets load.
bool ParseMyPresets(std::string_view text, std::vector<MyPreset>* presets, std::vector<std::string>* dropped,
                    std::string* reason);
std::string SerializeMyPresets(const std::vector<MyPreset>& presets);

// my-presets.json beside game-settings.json (the path PrepareGameSettingsStorage returns).
std::filesystem::path MyPresetsPath(const std::filesystem::path& game_settings_file);

enum class MyPresetsLoadStatus {
  kLoaded,       // the presets in the file; none when there is no file
  kSetAside,     // the file could not be read as presets and is now my-presets.json.bad: start with none
  kUnavailable,  // the file can't be read safely, is a newer format, or could not be set aside: show none and do not
                 // save (SaveMyPresetsFile refuses too), so the file stays for a later Playden or for recovery
};
struct MyPresetsLoad {
  MyPresetsLoadStatus status = MyPresetsLoadStatus::kLoaded;
  std::vector<MyPreset> presets;
  std::vector<std::string> dropped;  // settings dropped while parsing, for the log
  std::string reason;                // why the file was set aside or is unavailable, for the log
};
MyPresetsLoad LoadMyPresetsFile(const std::string& path);
// Replaces the file atomically (0600, ReadPrivateSettingsFile's folder and file rules). Refuses while the file there
// does not parse as presets (a load that came back kUnavailable), so a file Playden could not read is never replaced.
bool SaveMyPresetsFile(const std::string& path, const std::vector<MyPreset>& presets, std::string* reason);

}  // namespace playden::runtime
