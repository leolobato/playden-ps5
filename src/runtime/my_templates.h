#pragma once
// my_templates.h - The player's own controller layout templates ("My templates"): a layout and the controller mode it
// is for, saved from one game to start another game's layout from, kept per console in my-templates.json beside
// game-settings.json. A template is copied into a game when it is picked: no game names one, so changing or deleting a
// template changes no game, and recipes and shares never carry one.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime/controller_layout.h"
#include "runtime/my_presets.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace playden::runtime {

inline constexpr std::size_t kMyTemplatesMax = 50;
inline constexpr char kMyTemplatesFileName[] = "my-templates.json";
// Where a my-templates.json that can't be read as templates is moved, so Playden starts with none and never writes over it.
inline constexpr char kMyTemplatesBadFileName[] = "my-templates.json.bad";

struct MyTemplate {
  std::string id;      // "my-" and 16 lowercase hex (IsMyPresetId's form)
  std::string name;    // CheckMyPresetName's rules, unique among the player's templates ignoring case and accents
  std::string mode;    // the controller mode: xinput, playstation or keyboard
  std::string layout;  // canonical layout text (CanonicalControllerLayout), never empty
  std::int64_t created = 0;  // Unix seconds
  std::int64_t updated = 0;  // last rename or layout change
};
bool operator==(const MyTemplate& left, const MyTemplate& right);

const MyTemplate* FindMyTemplate(const std::vector<MyTemplate>& templates, std::string_view id);
// The player's template with this layout and mode, or nothing. The text is compared canonical.
const MyTemplate* FindMyTemplateByLayout(const std::vector<MyTemplate>& templates, std::string_view layout,
                                         std::string_view mode);
// A built-in template with this layout and mode (ControllerLayoutTemplates), or nothing.
const ControllerLayoutTemplate* FindBuiltInTemplateByLayout(std::string_view layout, std::string_view mode);

// Why a change to the player's templates was refused. Names follow My presets (CheckMyPresetName).
enum class MyTemplateRefusal {
  kNone,
  kNameEmpty,
  kNameTooLong,
  kNameNotPrintable,
  kNameTaken,      // another template has it, ignoring case and accents
  kTooMany,        // kMyTemplatesMax already
  kUnknownId,
  kBadId,          // a new id that is not IsMyPresetId's form, or one already used
  kBadLayout,      // empty, or a layout or mode Playden can't write; detail says why
  kSameAsBuiltIn,  // the layout and mode are a built-in template's; detail names it
  kNotSaved,       // my-templates.json could not be read at startup or written now; detail says why
};
struct MyTemplateChange {
  MyTemplateRefusal refusal = MyTemplateRefusal::kNone;
  std::string detail;
  bool ok() const { return refusal == MyTemplateRefusal::kNone; }
};

// The pure changes. On a refusal the list is left as it was. now is Unix seconds.
MyTemplateChange AddMyTemplate(std::vector<MyTemplate>* templates, std::string_view id, std::string_view name,
                               std::string_view mode, std::string_view layout, std::int64_t now);
MyTemplateChange RenameMyTemplate(std::vector<MyTemplate>* templates, std::string_view id, std::string_view name,
                                  std::int64_t now);
MyTemplateChange ReplaceMyTemplateLayout(std::vector<MyTemplate>* templates, std::string_view id, std::string_view mode,
                                         std::string_view layout, std::int64_t now);
MyTemplateChange DeleteMyTemplate(std::vector<MyTemplate>* templates, std::string_view id);

// {"format":1,"templates":[{"id","name","mode","layout","created","updated"}]}. Parsing refuses (false) a file that is
// not that: malformed JSON, another format, a template without a valid id, name or timestamps, or two with one id. A
// template whose mode or layout this build can't write (a newer Playden's) is dropped and listed in dropped as
// "<id> <why>"; the others load.
bool ParseMyTemplates(std::string_view text, std::vector<MyTemplate>* templates, std::vector<std::string>* dropped,
                      std::string* reason);
std::string SerializeMyTemplates(const std::vector<MyTemplate>& templates);

// my-templates.json beside game-settings.json.
std::filesystem::path MyTemplatesPath(const std::filesystem::path& game_settings_file);

enum class MyTemplatesLoadStatus { kLoaded, kSetAside, kUnavailable };  // as MyPresetsLoadStatus
struct MyTemplatesLoad {
  MyTemplatesLoadStatus status = MyTemplatesLoadStatus::kLoaded;
  std::vector<MyTemplate> templates;
  std::vector<std::string> dropped;
  std::string reason;
};
MyTemplatesLoad LoadMyTemplatesFile(const std::string& path);
std::vector<std::string> MyTemplatesLoadLog(const MyTemplatesLoad& load);
// Reads the templates without ever moving the file: for a worker while Playden runs (the file was set aside once at
// startup by LoadMyTemplatesFile). No file: true and none. A file that can't be read or parsed: false and a reason.
bool ReadMyTemplatesFile(const std::string& path, std::vector<MyTemplate>* templates, std::string* reason);
// The templates' names by ControllerLayoutNameKey, for SettingsLaunchInput::layout_names.
std::map<std::string, std::string> MyTemplateNames(const std::vector<MyTemplate>& templates);

// Replaces the file atomically, with My presets' file rules. Refuses while the file there does not parse as templates
// or has templates this build skips (a newer Playden's, which a rewrite would lose), and proposed data that cannot be
// parsed back without loss.
bool SaveMyTemplatesFile(const std::string& path, const std::vector<MyTemplate>& templates, std::string* reason);

}  // namespace playden::runtime
