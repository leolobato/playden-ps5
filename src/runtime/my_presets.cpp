// my_presets.cpp - The player's own presets: rules, changes and my-presets.json.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime/my_presets.h"

#include "community/printable_text.h"
#include "homebrew/json.hpp"
#include "library/game_ref.h"
#include "steam/backend/json.h"

#include <algorithm>
#include <cerrno>
#include <limits>
#include <set>
#include <utility>

namespace playden::runtime {
namespace {

namespace storage_json = steam::json;

constexpr std::size_t kIdHexDigits = 16;

// A full file is about 50 * (6 + settings) values: 2,753 with today's 49 settings a preset may set, near the default
// 4,096. Room for many more settings, so a file Playden wrote never reads back as malformed.
storage_json::Limits FileLimits() {
  storage_json::Limits limits;
  limits.max_values = 64 * 1024;
  return limits;
}

bool LowerHex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }

std::vector<MyPreset>::iterator FindById(std::vector<MyPreset>* presets, std::string_view id) {
  return std::find_if(presets->begin(), presets->end(), [&](const MyPreset& item) { return item.preset.id == id; });
}

// Whether another preset than `except` has a name that folds to the same text.
bool NameTaken(const std::vector<MyPreset>& presets, std::string_view name, std::string_view except) {
  const auto folded = FoldedName(name);
  return std::any_of(presets.begin(), presets.end(), [&](const MyPreset& item) {
    return item.preset.id != except && FoldedName(item.preset.name) == folded;
  });
}

// Drops retired settings, then checks the rest.
MyPresetChange CheckSettings(std::map<std::string, std::string>* settings) {
  for (auto it = settings->begin(); it != settings->end();) it = RetiredSetting(it->first) ? settings->erase(it) : std::next(it);
  if (settings->empty()) return {MyPresetRefusal::kNoSettings, {}};
  for (const auto& [id, value] : *settings) {
    std::string why;
    if (!ValidatePresetSetting(id, value, &why)) return {MyPresetRefusal::kSettingRefused, id + ": " + why};
  }
  return {};
}

std::optional<std::int64_t> Timestamp(const storage_json::Value* value) {
  const auto parsed = value ? value->AsUint64() : std::nullopt;
  if (!parsed || *parsed > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) return std::nullopt;
  return static_cast<std::int64_t>(*parsed);
}

}  // namespace

bool operator==(const MyPreset& left, const MyPreset& right) {
  return left.preset.id == right.preset.id && left.preset.name == right.preset.name &&
         left.preset.try_when == right.preset.try_when && left.preset.short_hint == right.preset.short_hint &&
         left.preset.settings == right.preset.settings && left.created == right.created && left.updated == right.updated;
}

bool IsMyPresetId(std::string_view id) {
  if (id.size() != 3 + kIdHexDigits || id.compare(0, 3, "my-") != 0) return false;
  return std::all_of(id.begin() + 3, id.end(), LowerHex);
}

std::optional<std::string> MyPresetIdFromUuid(std::string_view uuid) {
  // 8-4-4-4-12; the version digit (14th) and the variant digit (19th) are not random, so they are skipped.
  if (uuid.size() != 36 || uuid[8] != '-' || uuid[13] != '-' || uuid[18] != '-' || uuid[23] != '-') return std::nullopt;
  std::string id = "my-";
  for (std::size_t index = 0; index < uuid.size(); ++index) {
    if (index == 8 || index == 13 || index == 18 || index == 23) continue;
    char c = uuid[index];
    if (c >= 'A' && c <= 'F') c = static_cast<char>(c - 'A' + 'a');
    if (!LowerHex(c)) return std::nullopt;
    if (index == 14 || index == 19) continue;
    if (id.size() < 3 + kIdHexDigits) id += c;
  }
  return IsMyPresetId(id) ? std::optional<std::string>(id) : std::nullopt;
}

const MyPreset* FindMyPreset(const std::vector<MyPreset>& presets, std::string_view id) {
  const auto found =
      std::find_if(presets.begin(), presets.end(), [&](const MyPreset& item) { return item.preset.id == id; });
  return found == presets.end() ? nullptr : &*found;
}

std::vector<SettingsPreset> MyPresetsAsSettingsPresets(const std::vector<MyPreset>& presets) {
  std::vector<SettingsPreset> out;
  out.reserve(presets.size());
  for (const auto& item : presets) out.push_back(item.preset);
  return out;
}

std::string MyPresetSummary(const std::map<std::string, std::string>& settings, std::size_t items) {
  const auto value_of = [&](const char* id) {
    const auto found = settings.find(id);
    return found == settings.end() ? std::string() : found->second;
  };
  // Rows the chosen controller preset brings with these very values go without saying.
  const auto& inputs = ControllerPresetInputs();
  const auto controller_preset = inputs.find(value_of("controller_preset"));
  const auto implied = [&](const std::string& id, const std::string& value) {
    if (controller_preset == inputs.end()) return false;
    const auto found = controller_preset->second.find(id);
    return found != controller_preset->second.end() && found->second == value;
  };
  // A choice that names itself ("Mouse", "1280×720", "60 Hz") stands alone; others take the row's name.
  static const std::set<std::string> self_named = {"controller_preset", "controller", "resolution", "refresh",
                                                   "steam_features", "windows_version"};
  const auto item = [&](const SettingDefinition& definition, const std::string& value) {
    std::string label = value;
    for (const auto& choice : definition.choices)
      if (choice.value == value) label = choice.label;
    if (definition.id == "controller_preset" && value == "none") return std::string("No controller preset");
    if (self_named.count(definition.id)) return label;
    if (label == "On" || label == "Off") label = label == "On" ? "on" : "off";
    return definition.label + " " + label;
  };
  std::vector<std::string> parts;
  // The rows players recognise a preset by lead, as in the chooser's comparison; then the rest in sheet order.
  static const std::vector<std::string> leading = {"controller_preset", "controller", "resolution", "refresh",
                                                   "performance_overlay"};
  for (const auto& id : leading)
    if (const auto* definition = FindSetting(id); definition && settings.count(id) && !implied(id, settings.at(id)))
      parts.push_back(item(*definition, settings.at(id)));
  std::size_t buttons = 0;
  for (const auto& definition : SettingsCatalogue()) {
    const auto found = settings.find(definition.id);
    if (found == settings.end() || std::find(leading.begin(), leading.end(), definition.id) != leading.end() ||
        implied(definition.id, found->second))
      continue;
    if (definition.id.rfind("button.", 0) == 0) ++buttons;
    else parts.push_back(item(definition, found->second));
  }
  if (buttons) parts.push_back(std::to_string(buttons) + (buttons == 1 ? " button" : " buttons"));
  std::string text;
  const std::size_t shown = parts.size() > items ? items : parts.size();
  for (std::size_t index = 0; index < shown; ++index) text += (index ? " \u00b7 " : "") + parts[index];
  if (parts.size() > shown) text += std::string(shown ? " \u00b7 +" : "+") + std::to_string(parts.size() - shown) + " more";
  return text;
}

std::optional<SettingsPreset> ResolveRecordPreset(const std::vector<SettingsPreset>& built_in,
                                                  const std::vector<MyPreset>& mine, GameSettingsRecord* record,
                                                  bool* gone) {
  if (gone) *gone = false;
  if (!record || record->preset_id.empty()) return std::nullopt;
  for (const auto& preset : built_in)
    if (preset.id == record->preset_id) return preset;
  if (!IsMyPresetId(record->preset_id)) return std::nullopt;
  if (const auto* found = FindMyPreset(mine, record->preset_id)) return found->preset;
  record->preset_id.clear();
  if (gone) *gone = true;
  return std::nullopt;
}

MyPresetRefusal CheckMyPresetName(std::string_view name, std::string* trimmed) {
  while (!name.empty() && name.front() == ' ') name.remove_prefix(1);
  while (!name.empty() && name.back() == ' ') name.remove_suffix(1);
  if (name.empty()) return MyPresetRefusal::kNameEmpty;
  if (!community::IsSubmissionText(name, 1, std::numeric_limits<std::size_t>::max()))
    return MyPresetRefusal::kNameNotPrintable;
  if (!community::IsSubmissionText(name, 1, kMyPresetNameMaxCharacters)) return MyPresetRefusal::kNameTooLong;
  if (trimmed) *trimmed = std::string(name);
  return MyPresetRefusal::kNone;
}

MyPresetChange AddMyPreset(std::vector<MyPreset>* presets, std::string_view id, std::string_view name,
                           std::map<std::string, std::string> settings, std::int64_t now) {
  if (presets->size() >= kMyPresetsMax) return {MyPresetRefusal::kTooMany, {}};
  if (!IsMyPresetId(id) || FindMyPreset(*presets, id)) return {MyPresetRefusal::kBadId, std::string(id)};
  std::string trimmed;
  if (const auto refusal = CheckMyPresetName(name, &trimmed); refusal != MyPresetRefusal::kNone) return {refusal, {}};
  if (NameTaken(*presets, trimmed, {})) return {MyPresetRefusal::kNameTaken, trimmed};
  if (auto change = CheckSettings(&settings); !change.ok()) return change;
  MyPreset added;
  added.preset.id = std::string(id);
  added.preset.name = std::move(trimmed);
  added.preset.settings = std::move(settings);
  added.created = added.updated = now;
  presets->push_back(std::move(added));
  return {};
}

MyPresetChange RenameMyPreset(std::vector<MyPreset>* presets, std::string_view id, std::string_view name,
                              std::int64_t now) {
  const auto found = FindById(presets, id);
  if (found == presets->end()) return {MyPresetRefusal::kUnknownId, std::string(id)};
  std::string trimmed;
  if (const auto refusal = CheckMyPresetName(name, &trimmed); refusal != MyPresetRefusal::kNone) return {refusal, {}};
  // Its own name in another case or with other accents is a rename, not a clash.
  if (NameTaken(*presets, trimmed, id)) return {MyPresetRefusal::kNameTaken, trimmed};
  found->preset.name = std::move(trimmed);
  found->updated = now;
  return {};
}

MyPresetChange ReplaceMyPresetSettings(std::vector<MyPreset>* presets, std::string_view id,
                                       std::map<std::string, std::string> settings, std::int64_t now) {
  const auto found = FindById(presets, id);
  if (found == presets->end()) return {MyPresetRefusal::kUnknownId, std::string(id)};
  if (auto change = CheckSettings(&settings); !change.ok()) return change;
  found->preset.settings = std::move(settings);
  found->updated = now;
  return {};
}

MyPresetChange DeleteMyPreset(std::vector<MyPreset>* presets, std::string_view id) {
  const auto found = FindById(presets, id);
  if (found == presets->end()) return {MyPresetRefusal::kUnknownId, std::string(id)};
  presets->erase(found);
  return {};
}

bool ParseMyPresets(std::string_view text, std::vector<MyPreset>* presets, std::vector<std::string>* dropped,
                    std::string* reason) {
  const auto fail = [&](std::string why) {
    if (reason) *reason = std::move(why);
    return false;
  };
  const auto root = storage_json::Parse(text, FileLimits());
  if (!root || !root->is_object()) return fail("not a JSON object");
  const auto* format = root->Find("format");
  if (!format || format->AsUint64() != 1) return fail("not format 1");
  const auto* list = root->Find("presets");
  if (!list || !list->is_array()) return fail("no presets list");
  std::vector<MyPreset> out;
  std::vector<std::string> out_dropped;
  std::set<std::string> ids;
  for (const auto& item : list->array()) {
    if (!item.is_object()) return fail("a preset is not an object");
    const auto* id = item.Find("id");
    const auto* name = item.Find("name");
    const auto* settings = item.Find("settings");
    const auto created = Timestamp(item.Find("created"));
    const auto updated = Timestamp(item.Find("updated"));
    if (!id || !id->is_string() || !IsMyPresetId(id->text())) return fail("a preset has no valid id");
    if (!ids.insert(id->text()).second) return fail("preset " + id->text() + " is listed twice");
    std::string trimmed;
    if (!name || !name->is_string() || CheckMyPresetName(name->text(), &trimmed) != MyPresetRefusal::kNone ||
        trimmed != name->text())
      return fail("preset " + id->text() + " has no valid name");
    if (!settings || !settings->is_object()) return fail("preset " + id->text() + " has no settings");
    if (!created || !updated) return fail("preset " + id->text() + " has no valid created and updated times");
    MyPreset preset;
    preset.preset.id = id->text();
    preset.preset.name = std::move(trimmed);
    preset.created = *created;
    preset.updated = *updated;
    for (const auto& [key, value] : settings->object()) {
      if (!value.is_string()) return fail("preset " + id->text() + " setting " + key + " is not a string");
      // A retired setting, or one a newer Playden wrote that this one refuses: the rest of the preset still applies.
      if (RetiredSetting(key) || !ValidatePresetSetting(key, value.text(), nullptr)) {
        out_dropped.push_back(id->text() + " " + key);
        continue;
      }
      preset.preset.settings[key] = value.text();
    }
    out.push_back(std::move(preset));
  }
  *presets = std::move(out);
  if (dropped) *dropped = std::move(out_dropped);
  return true;
}

std::string SerializeMyPresets(const std::vector<MyPreset>& presets) {
  namespace json = homebrew::json;
  std::string text = "{\"format\":1,\"presets\":[";
  bool first = true;
  for (const auto& item : presets) {
    if (!first) text += ",";
    first = false;
    text += "{\"id\":\"" + json::escape(item.preset.id) + "\",\"name\":\"" + json::escape(item.preset.name) +
            "\",\"settings\":{";
    bool first_setting = true;
    for (const auto& [id, value] : item.preset.settings) {
      if (!first_setting) text += ",";
      first_setting = false;
      text += "\"" + json::escape(id) + "\":\"" + json::escape(value) + "\"";
    }
    text += "},\"created\":" + std::to_string(item.created) + ",\"updated\":" + std::to_string(item.updated) + "}";
  }
  return text + "]}\n";
}

std::filesystem::path MyPresetsPath(const std::filesystem::path& game_settings_file) {
  return game_settings_file.parent_path() / kMyPresetsFileName;
}

MyPresetsLoad LoadMyPresetsFile(const std::string& path) {
  MyPresetsLoad out;
  std::string text;
  const auto read = ReadPrivateSettingsFile(path, &text);
  if (read == PrivateSettingsRead::kMissing) return out;
  if (read == PrivateSettingsRead::kError) {
    out.status = MyPresetsLoadStatus::kUnavailable;
    out.reason = "cannot read my-presets.json";
    return out;
  }
  std::string why;
  if (ParseMyPresets(text, &out.presets, &out.dropped, &why)) return out;
  out.presets.clear();
  out.dropped.clear();
  // A newer Playden's file stays where it is for that Playden; this one shows none and does not save.
  const auto root = storage_json::Parse(text, FileLimits());
  const auto* format = root && root->is_object() ? root->Find("format") : nullptr;
  const auto version = format ? format->AsUint64() : std::nullopt;
  if (version && *version > 1) {
    out.status = MyPresetsLoadStatus::kUnavailable;
    out.reason = "my-presets.json is format " + std::to_string(*version) + ", from a newer Playden";
    return out;
  }
  errno = 0;
  if (!SetPrivateSettingsFileAside(path, kMyPresetsBadFileName)) {
    const bool taken = errno == EEXIST;
    out.status = MyPresetsLoadStatus::kUnavailable;
    out.reason = "my-presets.json is malformed (" + why + ") and " +
                 (taken ? std::string("my-presets.json.bad already exists") : std::string("cannot be set aside"));
    return out;
  }
  out.status = MyPresetsLoadStatus::kSetAside;
  out.reason = "my-presets.json is malformed (" + why + "); kept as my-presets.json.bad";
  return out;
}

std::vector<std::string> MyPresetsLoadLog(const MyPresetsLoad& load) {
  std::vector<std::string> lines;
  if (load.status == MyPresetsLoadStatus::kSetAside) lines.push_back("my presets set aside: " + load.reason);
  if (load.status == MyPresetsLoadStatus::kUnavailable) lines.push_back("my presets unavailable: " + load.reason);
  for (const auto& dropped : load.dropped) lines.push_back("my presets dropped setting: " + dropped);
  return lines;
}

bool ReadMyPresetsFile(const std::string& path, std::vector<MyPreset>* presets, std::string* reason) {
  presets->clear();
  std::string text;
  const auto read = ReadPrivateSettingsFile(path, &text);
  if (read == PrivateSettingsRead::kMissing) return true;
  std::string why = "cannot read my-presets.json";
  if (read == PrivateSettingsRead::kPresent && ParseMyPresets(text, presets, nullptr, &why)) return true;
  presets->clear();
  if (reason) *reason = why;
  return false;
}

bool SaveMyPresetsFile(const std::string& path, const std::vector<MyPreset>& presets, std::string* reason) {
  const auto fail = [&](std::string why) {
    if (reason) *reason = std::move(why);
    return false;
  };
  std::string text;
  const auto read = ReadPrivateSettingsFile(path, &text);
  if (read == PrivateSettingsRead::kError) return fail("cannot read my-presets.json");
  std::vector<MyPreset> current;
  if (read == PrivateSettingsRead::kPresent && !ParseMyPresets(text, &current, nullptr, nullptr))
    return fail("my-presets.json could not be read as presets; it is left as it is");
  // Do not replace recoverable data with bytes this build cannot load intact. In particular, the pure changes
  // accept signed Unix times, while the file format requires nonnegative timestamps.
  const auto proposed = SerializeMyPresets(presets);
  std::vector<MyPreset> checked;
  if (!ParseMyPresets(proposed, &checked, nullptr, nullptr) || checked != presets)
    return fail("the proposed presets cannot be saved and loaded intact");
  if (!WritePrivateSettingsFile(path, proposed)) return fail("cannot write my-presets.json");
  return true;
}

}  // namespace playden::runtime
