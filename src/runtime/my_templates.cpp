// my_templates.cpp - The player's own controller layout templates: rules, changes and my-templates.json.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime/my_templates.h"

#include "homebrew/json.hpp"
#include "library/game_ref.h"
#include "steam/backend/json.h"

#include <algorithm>
#include <cerrno>
#include <limits>
#include <optional>
#include <set>
#include <utility>

namespace playden::runtime {
namespace {

namespace storage_json = steam::json;

// 50 templates of 6 values each; strings keep the parser's default limit, so a newer Playden's longer layout is
// dropped with its template rather than failing the whole file.
storage_json::Limits FileLimits() {
  storage_json::Limits limits;
  limits.max_values = 16 * 1024;
  return limits;
}

std::vector<MyTemplate>::iterator FindById(std::vector<MyTemplate>* templates, std::string_view id) {
  return std::find_if(templates->begin(), templates->end(), [&](const MyTemplate& item) { return item.id == id; });
}

bool NameTaken(const std::vector<MyTemplate>& templates, std::string_view name, std::string_view except) {
  const auto folded = FoldedName(name);
  return std::any_of(templates.begin(), templates.end(), [&](const MyTemplate& item) {
    return item.id != except && FoldedName(item.name) == folded;
  });
}

MyTemplateRefusal FromPresetRefusal(MyPresetRefusal refusal) {
  switch (refusal) {
    case MyPresetRefusal::kNameEmpty: return MyTemplateRefusal::kNameEmpty;
    case MyPresetRefusal::kNameTooLong: return MyTemplateRefusal::kNameTooLong;
    case MyPresetRefusal::kNameNotPrintable: return MyTemplateRefusal::kNameNotPrintable;
    default: return MyTemplateRefusal::kNone;
  }
}

bool KnownMode(std::string_view mode) {
  const auto* controller = FindSetting("controller");
  if (!controller) return false;
  return std::any_of(controller->choices.begin(), controller->choices.end(),
                     [&](const auto& choice) { return choice.value == mode; });
}

// The layout as stored: canonical, non-empty, for a known mode. On a refusal, why.
MyTemplateChange CheckLayout(std::string_view mode, std::string_view layout, std::string* canonical) {
  if (!KnownMode(mode)) return {MyTemplateRefusal::kBadLayout, "unknown controller mode " + std::string(mode)};
  std::string why;
  if (!CanonicalControllerLayout(layout, canonical, &why)) return {MyTemplateRefusal::kBadLayout, why};
  if (canonical->empty()) return {MyTemplateRefusal::kBadLayout, "the layout is empty"};
  return {};
}

std::optional<std::int64_t> Timestamp(const storage_json::Value* value) {
  const auto parsed = value ? value->AsUint64() : std::nullopt;
  if (!parsed || *parsed > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) return std::nullopt;
  return static_cast<std::int64_t>(*parsed);
}

}  // namespace

bool operator==(const MyTemplate& left, const MyTemplate& right) {
  return left.id == right.id && left.name == right.name && left.mode == right.mode && left.layout == right.layout &&
         left.created == right.created && left.updated == right.updated;
}

const MyTemplate* FindMyTemplate(const std::vector<MyTemplate>& templates, std::string_view id) {
  const auto found = std::find_if(templates.begin(), templates.end(), [&](const MyTemplate& item) { return item.id == id; });
  return found == templates.end() ? nullptr : &*found;
}

const MyTemplate* FindMyTemplateByLayout(const std::vector<MyTemplate>& templates, std::string_view layout,
                                         std::string_view mode) {
  std::string canonical;
  if (!CanonicalControllerLayout(layout, &canonical, nullptr) || canonical.empty()) return nullptr;
  const auto found = std::find_if(templates.begin(), templates.end(), [&](const MyTemplate& item) {
    return item.mode == mode && item.layout == canonical;
  });
  return found == templates.end() ? nullptr : &*found;
}

const ControllerLayoutTemplate* FindBuiltInTemplateByLayout(std::string_view layout, std::string_view mode) {
  std::string canonical;
  if (!CanonicalControllerLayout(layout, &canonical, nullptr) || canonical.empty()) return nullptr;
  for (const auto& item : ControllerLayoutTemplates())
    if (item.mode == mode && item.text == canonical) return &item;
  return nullptr;
}

MyTemplateChange AddMyTemplate(std::vector<MyTemplate>* templates, std::string_view id, std::string_view name,
                               std::string_view mode, std::string_view layout, std::int64_t now) {
  if (templates->size() >= kMyTemplatesMax) return {MyTemplateRefusal::kTooMany, {}};
  if (!IsMyPresetId(id) || FindMyTemplate(*templates, id)) return {MyTemplateRefusal::kBadId, std::string(id)};
  std::string trimmed;
  if (const auto refusal = CheckMyPresetName(name, &trimmed); refusal != MyPresetRefusal::kNone)
    return {FromPresetRefusal(refusal), {}};
  if (NameTaken(*templates, trimmed, {})) return {MyTemplateRefusal::kNameTaken, trimmed};
  std::string canonical;
  if (auto change = CheckLayout(mode, layout, &canonical); !change.ok()) return change;
  if (const auto* built_in = FindBuiltInTemplateByLayout(canonical, mode))
    return {MyTemplateRefusal::kSameAsBuiltIn, built_in->name};
  templates->push_back({std::string(id), std::move(trimmed), std::string(mode), std::move(canonical), now, now});
  return {};
}

MyTemplateChange RenameMyTemplate(std::vector<MyTemplate>* templates, std::string_view id, std::string_view name,
                                  std::int64_t now) {
  const auto found = FindById(templates, id);
  if (found == templates->end()) return {MyTemplateRefusal::kUnknownId, std::string(id)};
  std::string trimmed;
  if (const auto refusal = CheckMyPresetName(name, &trimmed); refusal != MyPresetRefusal::kNone)
    return {FromPresetRefusal(refusal), {}};
  // Its own name in another case or with other accents is a rename, not a clash.
  if (NameTaken(*templates, trimmed, id)) return {MyTemplateRefusal::kNameTaken, trimmed};
  found->name = std::move(trimmed);
  found->updated = now;
  return {};
}

MyTemplateChange ReplaceMyTemplateLayout(std::vector<MyTemplate>* templates, std::string_view id, std::string_view mode,
                                         std::string_view layout, std::int64_t now) {
  const auto found = FindById(templates, id);
  if (found == templates->end()) return {MyTemplateRefusal::kUnknownId, std::string(id)};
  std::string canonical;
  if (auto change = CheckLayout(mode, layout, &canonical); !change.ok()) return change;
  if (const auto* built_in = FindBuiltInTemplateByLayout(canonical, mode))
    return {MyTemplateRefusal::kSameAsBuiltIn, built_in->name};
  found->mode = std::string(mode);
  found->layout = std::move(canonical);
  found->updated = now;
  return {};
}

MyTemplateChange DeleteMyTemplate(std::vector<MyTemplate>* templates, std::string_view id) {
  const auto found = FindById(templates, id);
  if (found == templates->end()) return {MyTemplateRefusal::kUnknownId, std::string(id)};
  templates->erase(found);
  return {};
}

bool ParseMyTemplates(std::string_view text, std::vector<MyTemplate>* templates, std::vector<std::string>* dropped,
                      std::string* reason) {
  const auto fail = [&](std::string why) {
    if (reason) *reason = std::move(why);
    return false;
  };
  const auto root = storage_json::Parse(text, FileLimits());
  if (!root || !root->is_object()) return fail("not a JSON object");
  const auto* format = root->Find("format");
  if (!format || format->AsUint64() != 1) return fail("not format 1");
  const auto* list = root->Find("templates");
  if (!list || !list->is_array()) return fail("no templates list");
  std::vector<MyTemplate> out;
  std::vector<std::string> out_dropped;
  std::set<std::string> ids;
  for (const auto& item : list->array()) {
    if (!item.is_object()) return fail("a template is not an object");
    const auto* id = item.Find("id");
    const auto* name = item.Find("name");
    const auto* mode = item.Find("mode");
    const auto* layout = item.Find("layout");
    const auto created = Timestamp(item.Find("created"));
    const auto updated = Timestamp(item.Find("updated"));
    if (!id || !id->is_string() || !IsMyPresetId(id->text())) return fail("a template has no valid id");
    if (!ids.insert(id->text()).second) return fail("template " + id->text() + " is listed twice");
    std::string trimmed;
    if (!name || !name->is_string() || CheckMyPresetName(name->text(), &trimmed) != MyPresetRefusal::kNone ||
        trimmed != name->text())
      return fail("template " + id->text() + " has no valid name");
    if (!created || !updated) return fail("template " + id->text() + " has no valid created and updated times");
    // A mode or layout a newer Playden wrote that this one can't read: the other templates still load.
    if (!mode || !mode->is_string() || !layout || !layout->is_string()) {
      out_dropped.push_back(id->text() + " has no mode and layout this Playden reads");
      continue;
    }
    std::string canonical;
    if (auto change = CheckLayout(mode->text(), layout->text(), &canonical); !change.ok()) {
      out_dropped.push_back(id->text() + " " + change.detail);
      continue;
    }
    out.push_back({id->text(), std::move(trimmed), mode->text(), std::move(canonical), *created, *updated});
  }
  *templates = std::move(out);
  if (dropped) *dropped = std::move(out_dropped);
  return true;
}

std::string SerializeMyTemplates(const std::vector<MyTemplate>& templates) {
  namespace json = homebrew::json;
  std::string text = "{\"format\":1,\"templates\":[";
  bool first = true;
  for (const auto& item : templates) {
    if (!first) text += ",";
    first = false;
    text += "{\"id\":\"" + json::escape(item.id) + "\",\"name\":\"" + json::escape(item.name) + "\",\"mode\":\"" +
            json::escape(item.mode) + "\",\"layout\":\"" + json::escape(item.layout) +
            "\",\"created\":" + std::to_string(item.created) + ",\"updated\":" + std::to_string(item.updated) + "}";
  }
  return text + "]}\n";
}

std::filesystem::path MyTemplatesPath(const std::filesystem::path& game_settings_file) {
  return game_settings_file.parent_path() / kMyTemplatesFileName;
}

MyTemplatesLoad LoadMyTemplatesFile(const std::string& path) {
  MyTemplatesLoad out;
  std::string text;
  const auto read = ReadPrivateSettingsFile(path, &text);
  if (read == PrivateSettingsRead::kMissing) return out;
  if (read == PrivateSettingsRead::kError) {
    out.status = MyTemplatesLoadStatus::kUnavailable;
    out.reason = "cannot read my-templates.json";
    return out;
  }
  std::string why;
  if (ParseMyTemplates(text, &out.templates, &out.dropped, &why)) return out;
  out.templates.clear();
  out.dropped.clear();
  // A newer Playden's file stays where it is for that Playden; this one shows none and does not save.
  const auto root = storage_json::Parse(text, FileLimits());
  const auto* format = root && root->is_object() ? root->Find("format") : nullptr;
  const auto version = format ? format->AsUint64() : std::nullopt;
  if (version && *version > 1) {
    out.status = MyTemplatesLoadStatus::kUnavailable;
    out.reason = "my-templates.json is format " + std::to_string(*version) + ", from a newer Playden";
    return out;
  }
  errno = 0;
  if (!SetPrivateSettingsFileAside(path, kMyTemplatesBadFileName)) {
    const bool taken = errno == EEXIST;
    out.status = MyTemplatesLoadStatus::kUnavailable;
    out.reason = "my-templates.json is malformed (" + why + ") and " +
                 (taken ? std::string("my-templates.json.bad already exists") : std::string("cannot be set aside"));
    return out;
  }
  out.status = MyTemplatesLoadStatus::kSetAside;
  out.reason = "my-templates.json is malformed (" + why + "); kept as my-templates.json.bad";
  return out;
}

std::vector<std::string> MyTemplatesLoadLog(const MyTemplatesLoad& load) {
  std::vector<std::string> lines;
  if (load.status == MyTemplatesLoadStatus::kSetAside) lines.push_back("my templates set aside: " + load.reason);
  if (load.status == MyTemplatesLoadStatus::kUnavailable) lines.push_back("my templates unavailable: " + load.reason);
  for (const auto& dropped : load.dropped) lines.push_back("my templates dropped: " + dropped);
  return lines;
}

bool SaveMyTemplatesFile(const std::string& path, const std::vector<MyTemplate>& templates, std::string* reason) {
  const auto fail = [&](std::string why) {
    if (reason) *reason = std::move(why);
    return false;
  };
  std::string text;
  const auto read = ReadPrivateSettingsFile(path, &text);
  if (read == PrivateSettingsRead::kError) return fail("cannot read my-templates.json");
  std::vector<MyTemplate> current;
  std::vector<std::string> skipped;
  if (read == PrivateSettingsRead::kPresent && !ParseMyTemplates(text, &current, &skipped, nullptr))
    return fail("my-templates.json could not be read as templates; it is left as it is");
  // Templates this build skipped (a newer Playden's) would be lost by a rewrite: the file stays for that Playden.
  if (!skipped.empty()) return fail("my-templates.json has templates from a newer Playden; it is left as it is");
  const auto proposed = SerializeMyTemplates(templates);
  std::vector<MyTemplate> checked;
  std::vector<std::string> dropped;
  if (!ParseMyTemplates(proposed, &checked, &dropped, nullptr) || !dropped.empty() || checked != templates)
    return fail("the proposed templates cannot be saved and loaded intact");
  if (!WritePrivateSettingsFile(path, proposed)) return fail("cannot write my-templates.json");
  return true;
}

}  // namespace playden::runtime
