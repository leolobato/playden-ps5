// steam_input.cpp - A game's Steam Input action map, for the offline Steam emulator.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime/steam_input.h"

#include "steam/backend/vdf.h"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <filesystem>
#include <map>
#include <optional>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace playden::runtime {
namespace {

namespace fs = std::filesystem;
using steam::VdfNode;

constexpr std::size_t kMaxVdfFileBytes = 8u * 1024u * 1024u;
constexpr int kMaxManifestDepth = 6;
constexpr std::size_t kMaxEntries = 50000;  // a walk of a huge install stops here

std::string Lower(std::string_view text) {
  std::string out(text);
  for (auto& value : out)
    if (value >= 'A' && value <= 'Z') value = static_cast<char>(value - 'A' + 'a');
  return out;
}

std::vector<std::string> Tokens(std::string_view text) {
  std::vector<std::string> out;
  std::size_t at = 0;
  while (at < text.size()) {
    while (at < text.size() && (text[at] == ' ' || text[at] == '\t')) ++at;
    const auto start = at;
    while (at < text.size() && text[at] != ' ' && text[at] != '\t') ++at;
    if (at > start) out.emplace_back(text.substr(start, at - start));
  }
  return out;
}

// An action or action set name the emulator can take as a file name or a key.
bool Plain(std::string_view name) {
  if (name.empty() || name.size() > 128) return false;
  return std::all_of(name.begin(), name.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
  });
}

class Bindings {
 public:
  void Add(const std::string& action, const std::string& button, const std::string& mode = {}) {
    if (!Plain(action) || button.empty()) return;
    auto found = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& entry) { return entry.action == action; });
    if (found == entries_.end()) found = entries_.insert(entries_.end(), Entry{action, {}, {}});
    if (std::find(found->buttons.begin(), found->buttons.end(), button) == found->buttons.end())
      found->buttons.push_back(button);
    if (found->mode.empty()) found->mode = mode;
  }

  std::string Text() const {
    std::string out;
    for (const auto& entry : entries_) {
      out += entry.action + "=";
      for (std::size_t i = 0; i < entry.buttons.size(); ++i) out += (i ? "," : "") + entry.buttons[i];
      if (!entry.mode.empty()) out += "=" + entry.mode;
      out += "\n";
    }
    return out;
  }

  bool Empty() const { return entries_.empty(); }

 private:
  struct Entry {
    std::string action;
    std::vector<std::string> buttons;
    std::string mode;
  };
  std::vector<Entry> entries_;
};

// The action of a "game_action <set> <action>, <label>" binding; empty for other bindings (keys, mouse, xinput).
std::string GameAction(std::string_view binding) {
  const auto tokens = Tokens(binding);
  if (tokens.size() < 3 || Lower(tokens[0]) != "game_action") return {};
  auto action = tokens[2];
  if (!action.empty() && action.back() == ',') action.pop_back();
  return action;
}

// The game actions an ordinary press of one input of a group triggers (inputs/<input>/activators/Full_Press/bindings/
// binding). The emulator has no long, double or release presses: binding those to the plain button would fire an action
// meant for a hold on every tap, so they stay out.
std::vector<std::string> InputActions(const VdfNode& input) {
  std::vector<std::string> out;
  const auto* activators = input.Find("activators");
  if (!activators) return out;
  for (const auto& [kind, activator] : activators->children) {
    if (Lower(kind) != "full_press") continue;
    const auto* bindings = activator.Find("bindings");
    if (!bindings) continue;
    for (const auto& [key, binding] : bindings->children)
      if (!binding.is_dict && Lower(key) == "binding")
        if (auto action = GameAction(binding.value); !action.empty()) out.push_back(std::move(action));
  }
  return out;
}

using Keymap = std::map<std::string, std::string>;

const Keymap& ButtonKeys() {
  static const Keymap keys = {
      {"button_a", "A"},          {"button_b", "B"},         {"button_x", "X"},           {"button_y", "Y"},
      {"dpad_north", "DUP"},      {"dpad_south", "DDOWN"},   {"dpad_east", "DRIGHT"},     {"dpad_west", "DLEFT"},
      {"button_escape", "START"}, {"button_menu", "BACK"},   {"left_bumper", "LBUMPER"},  {"right_bumper", "RBUMPER"},
  };
  return keys;
}

// Binds each input of the group through the keymap, or every input to `forced` when it is set.
void AddInputs(const VdfNode& group, const Keymap& keys, Bindings* bindings, const std::string& forced = {}) {
  const auto* inputs = group.Find("inputs");
  if (!inputs) return;
  for (const auto& [name, input] : inputs->children) {
    std::string button = forced;
    if (button.empty()) {
      const auto key = keys.find(Lower(name));
      if (key == keys.end()) continue;
      button = key->second;
    }
    for (const auto& action : InputActions(input)) bindings->Add(action, button);
  }
}

// The group's analog action for this action set (gameactions/<set>).
std::string GroupAction(const VdfNode& group, const std::string& set) {
  const auto* actions = group.Find("gameactions");
  const auto action = actions ? actions->String(set) : std::nullopt;
  return action ? std::string(*action) : std::string();
}

void AddGroup(const std::string& source, const VdfNode& group, const std::string& set, Bindings* bindings) {
  const auto mode = Lower(group.String("mode").value_or(""));
  if (source == "switch" || source == "button_diamond") {
    AddInputs(group, ButtonKeys(), bindings);
  } else if (source == "left_trigger" || source == "right_trigger") {
    const bool left = source == "left_trigger";
    if (mode == "trigger") bindings->Add(GroupAction(group, set), left ? "LTRIGGER" : "RTRIGGER", "trigger");
    AddInputs(group, ButtonKeys(), bindings, left ? "DLTRIGGER" : "DRTRIGGER");
  } else if (source == "joystick" || source == "right_joystick" || source == "dpad") {
    const bool left = source == "joystick";
    if (mode == "joystick_move" || mode == "joystick_camera") {
      bindings->Add(GroupAction(group, set), source == "dpad" ? "DPAD" : left ? "LJOY" : "RJOY", mode);
      if (source != "dpad") {
        static const Keymap left_click = {{"click", "LSTICK"}};
        static const Keymap right_click = {{"click", "RSTICK"}};
        AddInputs(group, left ? left_click : right_click, bindings);
      }
    } else if (mode == "dpad") {
      static const Keymap left_keys = {{"dpad_north", "DLJOYUP"}, {"dpad_south", "DLJOYDOWN"}, {"dpad_west", "DLJOYLEFT"},
                                       {"dpad_east", "DLJOYRIGHT"}, {"click", "LSTICK"}};
      static const Keymap right_keys = {{"dpad_north", "DRJOYUP"}, {"dpad_south", "DRJOYDOWN"}, {"dpad_west", "DRJOYLEFT"},
                                        {"dpad_east", "DRJOYRIGHT"}, {"click", "RSTICK"}};
      AddInputs(group, source == "dpad" ? ButtonKeys() : left ? left_keys : right_keys, bindings);
    }
  }
}

// Whole regular file, not through a link, at most limit bytes.
std::optional<std::string> ReadFile(const fs::path& path, std::size_t limit) {
  const int file = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (file < 0) return std::nullopt;
  struct stat info {};
  std::optional<std::string> out;
  if (::fstat(file, &info) == 0 && S_ISREG(info.st_mode) && static_cast<std::size_t>(info.st_size) <= limit) {
    std::string bytes(static_cast<std::size_t>(info.st_size), '\0');
    std::size_t done = 0;
    while (done < bytes.size()) {
      const auto count = ::read(file, bytes.data() + done, bytes.size() - done);
      if (count < 0 && errno == EINTR) continue;
      if (count <= 0) break;
      done += static_cast<std::size_t>(count);
    }
    if (done == bytes.size()) out = std::move(bytes);
  }
  ::close(file);
  return out;
}

// An entry of the folder whose name matches in any case, not through a link; empty when there is none.
fs::path EntryIn(const fs::path& folder, std::string_view name) {
  const auto wanted = Lower(name);
  std::error_code error;
  for (fs::directory_iterator it(folder, error), last; !error && it != last; it.increment(error))
    if (Lower(it->path().filename().string()) == wanted) {
      const auto status = it->symlink_status(error);
      if (error || fs::is_symlink(status)) return {};
      return it->path();
    }
  return {};
}

void FindManifest(const fs::path& folder, int depth, std::size_t* entries, fs::path* found) {
  std::error_code error;
  std::vector<fs::path> folders;
  for (fs::directory_iterator it(folder, error), last; !error && it != last; it.increment(error)) {
    if (++*entries > kMaxEntries) return;
    const auto name = it->path().filename().string();
    if (name.empty() || name[0] == '.') continue;
    const auto status = it->symlink_status(error);
    if (error) { error.clear(); continue; }
    if (fs::is_regular_file(status) && Lower(name) == "steam_input_manifest.vdf") {
      *found = it->path();
      return;
    }
    if (fs::is_directory(status) && depth < kMaxManifestDepth) folders.push_back(it->path());
  }
  // The manifest usually sits near the top: look at a folder's own files before its subfolders.
  std::sort(folders.begin(), folders.end());
  for (const auto& sub : folders) {
    FindManifest(sub, depth + 1, entries, found);
    if (!found->empty() || *entries > kMaxEntries) return;
  }
}

}  // namespace

std::vector<SteamInputActionSet> ConvertSteamInputConfig(std::string_view controller_vdf) {
  std::vector<SteamInputActionSet> out;
  const auto root = steam::ParseVdf(controller_vdf);
  const auto* mappings = root ? root->Find("controller_mappings") : nullptr;
  if (!mappings || !mappings->is_dict) return out;
  std::vector<std::string> sets;
  std::map<std::string, const VdfNode*> groups;
  for (const auto& [key, child] : mappings->children) {
    const auto name = Lower(key);
    if (name == "actions" && child.is_dict) {
      for (const auto& [set, unused] : child.children) sets.push_back(set);
    } else if (name == "group" && child.is_dict) {
      if (const auto id = child.String("id"); id && groups.find(std::string(*id)) == groups.end())
        groups[std::string(*id)] = &child;
    }
  }
  for (const auto& [key, preset] : mappings->children) {
    if (Lower(key) != "preset" || !preset.is_dict) continue;
    const auto name = preset.String("name");
    if (!name || !Plain(*name)) continue;
    const auto set = std::find_if(sets.begin(), sets.end(), [&](const std::string& s) { return Lower(s) == Lower(*name); });
    if (set == sets.end()) continue;  // an action layer, or the legacy "Default"
    const auto* sources = preset.Find("group_source_bindings");
    if (!sources) continue;
    Bindings bindings;
    for (const auto& [id, value] : sources->children) {
      const auto tokens = Tokens(value.value);
      // "<source> active": the group in use. "inactive" and "active modeshift" groups only take over on demand.
      if (value.is_dict || tokens.size() != 2 || Lower(tokens[1]) != "active") continue;
      const auto group = groups.find(id);
      if (group != groups.end()) AddGroup(Lower(tokens[0]), *group->second, *set, &bindings);
    }
    if (!bindings.Empty() && std::none_of(out.begin(), out.end(), [&](const auto& s) { return Lower(s.name) == Lower(*set); }))
      out.push_back({*set, bindings.Text()});
  }
  return out;
}

std::string SteamInputConfigPath(std::string_view manifest_vdf) {
  const auto root = steam::ParseVdf(manifest_vdf);
  const auto* manifest = root ? root->Find("Action Manifest") : nullptr;
  const auto* configurations = manifest ? manifest->Find("configurations") : nullptr;
  if (!configurations) return {};
  for (const char* controller : {"controller_xboxone", "controller_xbox360", "controller_ps5", "controller_ps4"}) {
    const auto* entries = configurations->Find(controller);
    if (!entries || !entries->is_dict) continue;
    for (const auto& [index, entry] : entries->children)
      if (const auto path = entry.String("path"); path && !path->empty()) return std::string(*path);
  }
  return {};
}

std::vector<SteamInputActionSet> SteamInputActionSets(const std::string& install_root) {
  std::size_t entries = 0;
  fs::path manifest_path;
  FindManifest(fs::path(install_root), 0, &entries, &manifest_path);
  if (manifest_path.empty()) return {};
  const auto manifest = ReadFile(manifest_path, kMaxVdfFileBytes);
  if (!manifest) return {};
  auto relative = SteamInputConfigPath(*manifest);
  std::replace(relative.begin(), relative.end(), '\\', '/');
  // Steam names files on a case-insensitive disk; look each part up in any case. No way out of the folder.
  fs::path config = manifest_path.parent_path();
  std::size_t at = 0;
  while (!relative.empty() && at <= relative.size()) {
    const auto end = std::min(relative.find('/', at), relative.size());
    const auto part = relative.substr(at, end - at);
    at = end + 1;
    if (part.empty() || part == ".") continue;
    if (part == "..") return {};
    config = EntryIn(config, part);
    if (config.empty()) return {};
  }
  if (relative.empty()) return {};
  const auto text = ReadFile(config, kMaxVdfFileBytes);
  if (!text) return {};
  std::string_view view(*text);
  while (!view.empty() && view.back() == '\0') view.remove_suffix(1);
  return ConvertSteamInputConfig(view);
}

}  // namespace playden::runtime
