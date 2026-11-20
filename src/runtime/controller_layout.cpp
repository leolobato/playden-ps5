// controller_layout.cpp - The controller layout's parser, writer and templates (prospero-win docs/INPUT_MAP.md).
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime/controller_layout.h"

#include <algorithm>
#include <set>

namespace playden::runtime {
namespace {

constexpr std::array<std::string_view, kLayoutInputCount> kInputNames = {
    "cross", "circle", "square", "triangle", "l1",   "r1",      "l2",     "r2",            "l3",             "r3",
    "up",    "down",   "left",   "right",    "options", "create", "touchpad_left", "touchpad_right", "left_stick",
    "right_stick"};

constexpr std::array<std::string_view, 7> kTuningSuffixes = {"_keys",  "_deadzone", "_outer",   "_curve",
                                                             "_speed", "_invert_x", "_invert_y"};

struct NamedKey {
  std::string_view name;
  unsigned code;
};

// The names other than letters, digits and f1-f24, in the grammar's order (prospero-win src/pw_game_profile.c).
constexpr NamedKey kNamedKeys[] = {
    {"space", 0x20},     {"enter", 0x0d},       {"escape", 0x1b},     {"tab", 0x09},       {"backspace", 0x08},
    {"capslock", 0x14},  {"pause", 0x13},       {"printscreen", 0x2c}, {"scrolllock", 0x91}, {"numlock", 0x90},
    {"insert", 0x2d},    {"delete", 0x2e},      {"home", 0x24},       {"end", 0x23},       {"pageup", 0x21},
    {"pagedown", 0x22},  {"up", 0x26},          {"down", 0x28},       {"left", 0x25},      {"right", 0x27},
    {"apps", 0x5d},      {"semicolon", 0xba},   {"equals", 0xbb},     {"comma", 0xbc},     {"minus", 0xbd},
    {"period", 0xbe},    {"slash", 0xbf},       {"backquote", 0xc0},  {"lbracket", 0xdb},  {"backslash", 0xdc},
    {"rbracket", 0xdd},  {"quote", 0xde},       {"num0", 0x60},       {"num1", 0x61},      {"num2", 0x62},
    {"num3", 0x63},      {"num4", 0x64},        {"num5", 0x65},       {"num6", 0x66},      {"num7", 0x67},
    {"num8", 0x68},      {"num9", 0x69},        {"multiply", 0x6a},   {"add", 0x6b},       {"subtract", 0x6d},
    {"decimal", 0x6e},   {"divide", 0x6f},
};

// In the order a combination writes them.
constexpr NamedKey kModifierKeys[] = {
    {"ctrl", 0x11},  {"lctrl", 0xa2}, {"rctrl", 0xa3}, {"shift", 0x10}, {"lshift", 0xa0}, {"rshift", 0xa1},
    {"alt", 0x12},   {"lalt", 0xa4},  {"ralt", 0xa5},  {"lwin", 0x5b},  {"rwin", 0x5c},
};

std::string_view Trim(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
  return text;
}

std::vector<std::string_view> Split(std::string_view text, char separator) {
  std::vector<std::string_view> parts;
  std::size_t start = 0;
  while (true) {
    const auto end = text.find(separator, start);
    parts.push_back(text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  return parts;
}

int HexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

// A number as the grammar writes it: digits, no leading zero, at most 5 digits.
std::optional<int> Number(std::string_view text) {
  if (text.empty() || text.size() > 5 || (text.size() > 1 && text.front() == '0')) return std::nullopt;
  int value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') return std::nullopt;
    value = value * 10 + (c - '0');
  }
  return value;
}

// The code of a name, without vk:.
std::optional<unsigned> NamedCode(std::string_view name) {
  if (name.size() == 1 && name[0] >= 'a' && name[0] <= 'z') return 0x41u + static_cast<unsigned>(name[0] - 'a');
  if (name.size() == 1 && name[0] >= '0' && name[0] <= '9') return 0x30u + static_cast<unsigned>(name[0] - '0');
  if ((name.size() == 2 || name.size() == 3) && name[0] == 'f') {
    const auto number = Number(name.substr(1));
    if (number && *number >= 1 && *number <= 24) return 0x70u + static_cast<unsigned>(*number - 1);
    return std::nullopt;
  }
  for (const auto& key : kNamedKeys)
    if (key.name == name) return key.code;
  for (const auto& key : kModifierKeys)
    if (key.name == name) return key.code;
  return std::nullopt;
}

bool CodeHasName(unsigned code) {
  if ((code >= 0x41 && code <= 0x5a) || (code >= 0x30 && code <= 0x39) || (code >= 0x70 && code <= 0x87)) return true;
  for (const auto& key : kNamedKeys)
    if (key.code == code) return true;
  for (const auto& key : kModifierKeys)
    if (key.code == code) return true;
  return false;
}

std::string NameForCode(unsigned code) {
  if (code >= 0x41 && code <= 0x5a) return std::string(1, static_cast<char>('a' + (code - 0x41)));
  if (code >= 0x30 && code <= 0x39) return std::string(1, static_cast<char>('0' + (code - 0x30)));
  if (code >= 0x70 && code <= 0x87) return "f" + std::to_string(code - 0x70 + 1);
  for (const auto& key : kNamedKeys)
    if (key.code == code) return std::string(key.name);
  for (const auto& key : kModifierKeys)
    if (key.code == code) return std::string(key.name);
  static const char digits[] = "0123456789abcdef";
  return std::string("vk:0x") + digits[(code >> 4) & 0xf] + digits[code & 0xf];
}

int ModifierIndex(std::string_view name) {
  for (std::size_t index = 0; index < std::size(kModifierKeys); ++index)
    if (kModifierKeys[index].name == name) return static_cast<int>(index);
  return -1;
}

// Why a name is not a key name, with the name to write for a vk: code that has one.
std::string NotAKey(std::string_view name) {
  if (name.size() == 7 && name.substr(0, 5) == "vk:0x") {
    const int high = HexDigit(name[5]), low = HexDigit(name[6]);
    if (high >= 0 && low >= 0 && (high | low) != 0 && CodeHasName(static_cast<unsigned>(high * 16 + low)))
      return std::string(name) + " has a name: write " + NameForCode(static_cast<unsigned>(high * 16 + low));
  }
  return "\"" + std::string(name) + "\" is not a key name";
}

bool IsTrigger(LayoutInput input) { return input == LayoutInput::kL2 || input == LayoutInput::kR2; }

bool IsAnalogPull(const LayoutTarget& target) {
  return target.kind == LayoutTarget::Kind::kPad && (target.pad == "lt" || target.pad == "rt");
}

bool Fail(std::string* reason, std::string why) {
  if (reason) *reason = std::move(why);
  return false;
}

std::optional<StickCurve> CurveFromText(std::string_view text) {
  if (text == "linear") return StickCurve::kLinear;
  if (text == "squared") return StickCurve::kSquared;
  if (text == "cubed") return StickCurve::kCubed;
  return std::nullopt;
}

// The lines one stick's or trigger's tuning gave, before the mode is known.
struct PendingTuning {
  std::array<bool, kTuningSuffixes.size()> given{};
  int threshold = 0;
  bool threshold_given = false;
};

int DeadzoneMinimum(StickMode mode) { return mode == StickMode::kPadDpad || mode == StickMode::kKeys ? 10 : 0; }
int SpeedMaximum(StickMode mode) { return mode == StickMode::kWheel ? 100 : 20000; }

}  // namespace

std::string_view LayoutInputName(LayoutInput input) { return kInputNames[static_cast<std::size_t>(input)]; }

LayoutInputKind LayoutInputKindOf(LayoutInput input) {
  if (input == LayoutInput::kLeftStick || input == LayoutInput::kRightStick) return LayoutInputKind::kStick;
  return IsTrigger(input) ? LayoutInputKind::kTrigger : LayoutInputKind::kButton;
}

std::optional<LayoutInput> LayoutInputFromName(std::string_view name) {
  for (std::size_t index = 0; index < kInputNames.size(); ++index)
    if (kInputNames[index] == name) return static_cast<LayoutInput>(index);
  return std::nullopt;
}

const std::array<LayoutInput, kLayoutInputCount>& LayoutInputs() {
  static const auto inputs = [] {
    std::array<LayoutInput, kLayoutInputCount> list{};
    for (std::size_t index = 0; index < list.size(); ++index) list[index] = static_cast<LayoutInput>(index);
    return list;
  }();
  return inputs;
}

bool operator==(const LayoutTarget& left, const LayoutTarget& right) {
  return left.kind == right.kind && left.pad == right.pad && left.keys == right.keys && left.mouse == right.mouse;
}

const std::vector<std::string>& LayoutPadButtons() {
  static const std::vector<std::string> buttons = {"a",  "b",  "x",       "y",         "lb",        "rb",
                                                   "lt", "rt", "ls",      "rs",        "dpad_up",   "dpad_down",
                                                   "dpad_left", "dpad_right", "start", "back", "guide", "touchpad"};
  return buttons;
}

const std::vector<std::string>& LayoutMouseButtons() {
  static const std::vector<std::string> buttons = {"left", "right", "middle", "back", "forward", "wheel_up",
                                                   "wheel_down"};
  return buttons;
}

LayoutTarget PadTarget(std::string_view button) {
  LayoutTarget target;
  target.kind = LayoutTarget::Kind::kPad;
  target.pad = std::string(button);
  return target;
}

LayoutTarget KeyTarget(std::vector<std::string> keys) {
  LayoutTarget target;
  target.kind = LayoutTarget::Kind::kKeys;
  target.keys = std::move(keys);
  return target;
}

LayoutTarget MouseTarget(std::string_view button) {
  LayoutTarget target;
  target.kind = LayoutTarget::Kind::kMouse;
  target.mouse = std::string(button);
  return target;
}

std::string LayoutTargetText(const LayoutTarget& target) {
  switch (target.kind) {
    case LayoutTarget::Kind::kPad: return "pad:" + target.pad;
    case LayoutTarget::Kind::kMouse: return "mouse:" + target.mouse;
    case LayoutTarget::Kind::kKeys: {
      std::string text = "key:";
      for (std::size_t index = 0; index < target.keys.size(); ++index) text += (index ? "+" : "") + target.keys[index];
      return text;
    }
    case LayoutTarget::Kind::kNone: break;
  }
  return "none";
}

bool ParseLayoutTarget(std::string_view text, LayoutTarget* target, std::string* reason) {
  LayoutTarget out;
  if (text == "none") {
    *target = out;
    return true;
  }
  if (text.substr(0, 4) == "pad:") {
    const auto button = text.substr(4);
    const auto& buttons = LayoutPadButtons();
    if (std::find(buttons.begin(), buttons.end(), button) == buttons.end())
      return Fail(reason, "pad:" + std::string(button) + " is not a controller button");
    *target = PadTarget(button);
    return true;
  }
  if (text.substr(0, 6) == "mouse:") {
    const auto button = text.substr(6);
    const auto& buttons = LayoutMouseButtons();
    if (std::find(buttons.begin(), buttons.end(), button) == buttons.end())
      return Fail(reason, "mouse:" + std::string(button) + " is not a mouse button or the wheel");
    *target = MouseTarget(button);
    return true;
  }
  if (text.substr(0, 4) == "key:") {
    const auto names = Split(text.substr(4), '+');
    if (names.size() > 4) return Fail(reason, "a combination has at most 4 keys");
    std::vector<std::string> keys;
    int last_modifier = -1;
    bool other = false;
    for (const auto name : names) {
      if (!IsLayoutKey(name)) return Fail(reason, NotAKey(name));
      if (std::find(keys.begin(), keys.end(), name) != keys.end())
        return Fail(reason, std::string(name) + " is in the combination twice");
      const int modifier = ModifierIndex(name);
      if (modifier >= 0 && (other || modifier < last_modifier))
        return Fail(reason, "modifiers come first, in the order ctrl, lctrl, rctrl, shift, lshift, rshift, alt, lalt, "
                            "ralt, lwin, rwin");
      if (modifier >= 0) last_modifier = modifier;
      else other = true;
      keys.emplace_back(name);
    }
    *target = KeyTarget(std::move(keys));
    return true;
  }
  return Fail(reason, "\"" + std::string(text) + "\" is not a target: write pad:<button>, key:<key>, mouse:<button> "
                      "or none");
}

std::string_view StickModeText(StickMode mode) {
  switch (mode) {
    case StickMode::kPadLeftStick: return "pad:left_stick";
    case StickMode::kPadRightStick: return "pad:right_stick";
    case StickMode::kPadDpad: return "pad:dpad";
    case StickMode::kKeys: return "keys";
    case StickMode::kMouse: return "mouse";
    case StickMode::kWheel: return "wheel";
    case StickMode::kNone: break;
  }
  return "none";
}

std::optional<StickMode> StickModeFromText(std::string_view text) {
  for (const auto mode : {StickMode::kPadLeftStick, StickMode::kPadRightStick, StickMode::kPadDpad, StickMode::kKeys,
                          StickMode::kMouse, StickMode::kWheel, StickMode::kNone})
    if (StickModeText(mode) == text) return mode;
  return std::nullopt;
}

std::string_view StickCurveText(StickCurve curve) {
  return curve == StickCurve::kSquared ? "squared" : curve == StickCurve::kCubed ? "cubed" : "linear";
}

std::string_view StickTuningSuffix(StickTuning tuning) { return kTuningSuffixes[static_cast<std::size_t>(tuning)]; }

bool StickModeTakes(StickMode mode, StickTuning tuning) {
  const bool pad = mode == StickMode::kPadLeftStick || mode == StickMode::kPadRightStick;
  const bool directions = mode == StickMode::kPadDpad || mode == StickMode::kKeys;
  switch (tuning) {
    case StickTuning::kKeys: return mode == StickMode::kKeys;
    case StickTuning::kDeadzone:
    case StickTuning::kInvertY: return mode != StickMode::kNone;
    case StickTuning::kOuter:
    case StickTuning::kCurve: return pad || mode == StickMode::kMouse || mode == StickMode::kWheel;
    case StickTuning::kSpeed: return mode == StickMode::kMouse || mode == StickMode::kWheel;
    case StickTuning::kInvertX: return pad || directions || mode == StickMode::kMouse;
  }
  return false;
}

StickValues StickDefaults(StickMode mode) {
  StickValues values;
  switch (mode) {
    case StickMode::kPadDpad:
    case StickMode::kKeys: values.deadzone = 40; break;
    case StickMode::kMouse:
      values.deadzone = 16, values.curve = StickCurve::kSquared, values.speed = 1200;
      break;
    case StickMode::kWheel:
      values.deadzone = 16, values.curve = StickCurve::kSquared, values.speed = 10;
      break;
    default: break;
  }
  return values;
}

StickValues ResolvedStick(const LayoutStick& stick) {
  auto values = StickDefaults(stick.mode);
  if (stick.deadzone && StickModeTakes(stick.mode, StickTuning::kDeadzone)) values.deadzone = *stick.deadzone;
  if (stick.outer && StickModeTakes(stick.mode, StickTuning::kOuter)) values.outer = *stick.outer;
  if (stick.curve && StickModeTakes(stick.mode, StickTuning::kCurve)) values.curve = *stick.curve;
  if (stick.speed && StickModeTakes(stick.mode, StickTuning::kSpeed)) values.speed = *stick.speed;
  if (stick.invert_x && StickModeTakes(stick.mode, StickTuning::kInvertX)) values.invert_x = *stick.invert_x;
  if (stick.invert_y && StickModeTakes(stick.mode, StickTuning::kInvertY)) values.invert_y = *stick.invert_y;
  return values;
}

void SetStickMode(LayoutStick* stick, StickMode mode) {
  stick->mode = mode;
  if (!StickModeTakes(mode, StickTuning::kDeadzone)) stick->deadzone.reset();
  if (!StickModeTakes(mode, StickTuning::kOuter)) stick->outer.reset();
  if (!StickModeTakes(mode, StickTuning::kCurve)) stick->curve.reset();
  if (!StickModeTakes(mode, StickTuning::kSpeed)) stick->speed.reset();
  if (!StickModeTakes(mode, StickTuning::kInvertX)) stick->invert_x.reset();
  if (!StickModeTakes(mode, StickTuning::kInvertY)) stick->invert_y.reset();
  if (!StickModeTakes(mode, StickTuning::kKeys)) stick->keys = {};
  // A dead zone the new mode doesn't allow (pad:dpad and keys start at 10) goes back to its default.
  if (stick->deadzone && *stick->deadzone < DeadzoneMinimum(mode)) stick->deadzone.reset();
  if (stick->speed && *stick->speed > SpeedMaximum(mode)) stick->speed.reset();
}

bool ControllerLayout::empty() const {
  return std::none_of(entries.begin(), entries.end(), [](const InputMapping& entry) { return entry.set; });
}

bool ParseControllerLayout(std::string_view text, ControllerLayout* layout, std::string* reason) {
  ControllerLayout out;
  if (text.empty()) {
    if (layout) *layout = out;
    return true;
  }
  if (text.size() > kControllerLayoutMaxBytes) return Fail(reason, "a layout is at most 4096 bytes");
  for (const char c : text) {
    const auto byte = static_cast<unsigned char>(c);
    if (byte < 0x20 && byte != '\t') return Fail(reason, "a layout is one line: its lines are joined by ;");
    if (byte == 0x7f || byte >= 0x80) return Fail(reason, "a layout holds only plain letters, digits and signs");
    if (c == '#' || c == '[' || c == ']') return Fail(reason, std::string("a layout holds no ") + c);
    if (c >= 'A' && c <= 'Z') return Fail(reason, "a layout is written in lower case");
  }
  std::set<std::string, std::less<>> seen;
  std::array<PendingTuning, kLayoutInputCount> pending{};
  for (const auto raw : Split(text, ';')) {
    const auto line = Trim(raw);
    if (line.empty()) return Fail(reason, "an empty line: a ; at an end, or two in a row");
    const auto equals = line.find('=');
    if (equals == std::string_view::npos) return Fail(reason, "\"" + std::string(line) + "\" is not name = value");
    const auto key = Trim(line.substr(0, equals));
    const auto value = Trim(line.substr(equals + 1));
    const std::string name(key);
    if (key.empty() || value.empty()) return Fail(reason, "\"" + std::string(line) + "\" is not name = value");
    if (key.find_first_of(" \t") != std::string_view::npos) return Fail(reason, "\"" + name + "\" is not a line name");
    if (value.find_first_of(" \t=") != std::string_view::npos)
      return Fail(reason, name + ": a value holds no space and no =");
    if (!seen.insert(name).second) return Fail(reason, name + " comes twice");
    if (key == "map") return Fail(reason, "map = 1 is written by Playden: leave it out of the layout");
    if (key == "mode" || key == "rumble" || key == "player2")
      return Fail(reason, name + " is a setting of its own, not part of the layout");
    if (key == "preset" || key == "mouse" || key == "mouse_speed")
      return Fail(reason, name + " does not go with a layout: the stick lines replace it");
    if (key == "touchpad") return Fail(reason, "touchpad: map touchpad_left and touchpad_right instead");
    if (const auto input = LayoutInputFromName(key)) {
      auto& entry = out[*input];
      entry.set = true;
      if (LayoutInputKindOf(*input) == LayoutInputKind::kStick) {
        const auto mode = StickModeFromText(value);
        if (!mode)
          return Fail(reason, name + ": \"" + std::string(value) + "\" is not a stick mode (pad:left_stick, "
                                    "pad:right_stick, pad:dpad, keys, mouse, wheel or none)");
        entry.stick.mode = *mode;
        continue;
      }
      std::string why;
      if (!ParseLayoutTarget(value, &entry.target, &why)) return Fail(reason, name + ": " + why);
      continue;
    }
    // A tuning line: <trigger>_threshold, or <stick>_<tuning>.
    bool known = false;
    for (const auto trigger : {LayoutInput::kL2, LayoutInput::kR2}) {
      if (key != std::string(LayoutInputName(trigger)) + "_threshold") continue;
      known = true;
      const auto number = Number(value);
      if (!number || *number < 5 || *number > 100) return Fail(reason, name + ": not 5-100");
      auto& tuning = pending[static_cast<std::size_t>(trigger)];
      tuning.threshold = *number;
      tuning.threshold_given = true;
    }
    for (const auto stick : {LayoutInput::kLeftStick, LayoutInput::kRightStick}) {
      const auto prefix = LayoutInputName(stick);
      if (key.size() <= prefix.size() || key.substr(0, prefix.size()) != prefix) continue;
      const auto suffix = key.substr(prefix.size());
      for (std::size_t index = 0; index < kTuningSuffixes.size(); ++index) {
        if (suffix != kTuningSuffixes[index]) continue;
        known = true;
        auto& entry = out[stick].stick;
        pending[static_cast<std::size_t>(stick)].given[index] = true;
        const auto tuning = static_cast<StickTuning>(index);
        if (tuning == StickTuning::kKeys) {
          const auto parts = Split(value, ',');
          if (parts.size() != 4) return Fail(reason, name + ": four keys, up,down,left,right (w,s,a,d)");
          for (std::size_t part = 0; part < 4; ++part) {
            if (parts[part] != "none" && !IsLayoutKey(parts[part]))
              return Fail(reason, name + ": " + NotAKey(parts[part]));
            for (std::size_t before = 0; before < part; ++before)
              if (parts[part] != "none" && parts[before] == parts[part])
                return Fail(reason, name + ": " + std::string(parts[part]) + " comes twice");
            entry.keys[part] = std::string(parts[part]);
          }
        } else if (tuning == StickTuning::kCurve) {
          entry.curve = CurveFromText(value);
          if (!entry.curve) return Fail(reason, name + ": not linear, squared or cubed");
        } else if (tuning == StickTuning::kInvertX || tuning == StickTuning::kInvertY) {
          if (value != "true" && value != "false") return Fail(reason, name + ": not true or false");
          (tuning == StickTuning::kInvertX ? entry.invert_x : entry.invert_y) = value == "true";
        } else {
          const auto number = Number(value);
          const int maximum = tuning == StickTuning::kDeadzone ? 90 : tuning == StickTuning::kOuter ? 50 : 20000;
          const int minimum = tuning == StickTuning::kSpeed ? 1 : 0;
          if (!number || *number < minimum || *number > maximum)
            return Fail(reason, name + ": not " + std::to_string(minimum) + "-" + std::to_string(maximum));
          (tuning == StickTuning::kDeadzone ? entry.deadzone : tuning == StickTuning::kOuter ? entry.outer : entry.speed) =
              *number;
        }
      }
    }
    if (!known) return Fail(reason, "unknown line " + name);
  }
  // Checked once every line is read: a tuning line may come before its input's line.
  for (const auto trigger : {LayoutInput::kL2, LayoutInput::kR2}) {
    const auto& tuning = pending[static_cast<std::size_t>(trigger)];
    if (!tuning.threshold_given) continue;
    const auto name = std::string(LayoutInputName(trigger)) + "_threshold";
    auto& entry = out[trigger];
    if (!entry.set) return Fail(reason, name + " needs a " + std::string(LayoutInputName(trigger)) + " line");
    if (entry.target.kind == LayoutTarget::Kind::kNone || IsAnalogPull(entry.target))
      return Fail(reason, name + ": only for a trigger that works as a button");
    entry.threshold = tuning.threshold;
  }
  for (const auto input : {LayoutInput::kLeftStick, LayoutInput::kRightStick}) {
    const auto& tuning = pending[static_cast<std::size_t>(input)];
    auto& entry = out[input];
    const bool any = std::any_of(tuning.given.begin(), tuning.given.end(), [](bool given) { return given; });
    // A stick with no line has its default mode, and its tuning lines are checked against it.
    if (!entry.set && any) entry.set = true, entry.stick.mode = LayoutDefaultStick(input);
    if (!entry.set) continue;
    auto& stick = entry.stick;
    const std::string name(LayoutInputName(input));
    for (std::size_t index = 0; index < tuning.given.size(); ++index)
      if (tuning.given[index] && !StickModeTakes(stick.mode, static_cast<StickTuning>(index)))
        return Fail(reason, name + kTuningSuffixes[index].data() + ": " + name + " = " +
                                std::string(StickModeText(stick.mode)) + " takes no such line");
    if (stick.mode == StickMode::kKeys && !tuning.given[static_cast<std::size_t>(StickTuning::kKeys)])
      return Fail(reason, name + " = keys needs " + name + "_keys (up,down,left,right)");
    if (stick.deadzone && *stick.deadzone < DeadzoneMinimum(stick.mode))
      return Fail(reason, name + "_deadzone: not 10-90 for " + std::string(StickModeText(stick.mode)));
    if (stick.speed && *stick.speed > SpeedMaximum(stick.mode)) return Fail(reason, name + "_speed: not 1-100 for wheel");
    const auto values = ResolvedStick(stick);
    if (values.deadzone + values.outer > 90) return Fail(reason, name + ": the dead zone and outer zone add up past 90");
    // A line that states the default means the same as no line.
    const auto defaults = StickDefaults(stick.mode);
    if (stick.deadzone == defaults.deadzone) stick.deadzone.reset();
    if (stick.outer == defaults.outer) stick.outer.reset();
    if (stick.curve == defaults.curve) stick.curve.reset();
    if (stick.speed == defaults.speed) stick.speed.reset();
    if (stick.invert_x == defaults.invert_x) stick.invert_x.reset();
    if (stick.invert_y == defaults.invert_y) stick.invert_y.reset();
  }
  if (layout) *layout = std::move(out);
  return true;
}

std::vector<std::string> ControllerLayoutLines(const ControllerLayout& layout) {
  std::vector<std::string> lines;
  for (const auto input : LayoutInputs()) {
    const auto& entry = layout[input];
    if (!entry.set) continue;
    const std::string name(LayoutInputName(input));
    if (LayoutInputKindOf(input) != LayoutInputKind::kStick) {
      lines.push_back(name + " = " + LayoutTargetText(entry.target));
      if (IsTrigger(input) && entry.threshold > 0 && entry.target.kind != LayoutTarget::Kind::kNone &&
          !IsAnalogPull(entry.target))
        lines.push_back(name + "_threshold = " + std::to_string(entry.threshold));
      continue;
    }
    const auto& stick = entry.stick;
    lines.push_back(name + " = " + std::string(StickModeText(stick.mode)));
    const auto defaults = StickDefaults(stick.mode);
    const auto values = ResolvedStick(stick);
    const auto line = [&](StickTuning tuning, const std::string& value) {
      lines.push_back(name + std::string(StickTuningSuffix(tuning)) + " = " + value);
    };
    if (StickModeTakes(stick.mode, StickTuning::kKeys))
      line(StickTuning::kKeys, stick.keys[0] + "," + stick.keys[1] + "," + stick.keys[2] + "," + stick.keys[3]);
    if (values.deadzone != defaults.deadzone) line(StickTuning::kDeadzone, std::to_string(values.deadzone));
    if (values.outer != defaults.outer) line(StickTuning::kOuter, std::to_string(values.outer));
    if (values.curve != defaults.curve) line(StickTuning::kCurve, std::string(StickCurveText(values.curve)));
    if (values.speed != defaults.speed) line(StickTuning::kSpeed, std::to_string(values.speed));
    if (values.invert_x != defaults.invert_x) line(StickTuning::kInvertX, values.invert_x ? "true" : "false");
    if (values.invert_y != defaults.invert_y) line(StickTuning::kInvertY, values.invert_y ? "true" : "false");
  }
  return lines;
}

std::string SerializeControllerLayout(const ControllerLayout& layout) {
  std::string text;
  for (const auto& line : ControllerLayoutLines(layout)) text += (text.empty() ? "" : ";") + line;
  return text;
}

bool CanonicalControllerLayout(std::string_view text, std::string* canonical, std::string* reason) {
  ControllerLayout layout;
  if (!ParseControllerLayout(text, &layout, reason)) return false;
  auto out = SerializeControllerLayout(layout);
  // What the writer made must read back the same; a stick left without keys, say, never reaches a profile.
  if (!ParseControllerLayout(out, nullptr, reason)) return false;
  if (canonical) *canonical = std::move(out);
  return true;
}

LayoutTarget LayoutDefaultTarget(LayoutInput input, std::string_view mode) {
  static constexpr std::array<std::string_view, kLayoutInputCount> pads = {
      "a",  "b",  "x",  "y",       "lb",         "rb",        "lt",         "rt",    "ls",   "rs",
      "dpad_up", "dpad_down", "dpad_left", "dpad_right", "start", "back", "back", "guide", "", ""};
  if (LayoutInputKindOf(input) == LayoutInputKind::kStick) return {};
  if ((input == LayoutInput::kTouchpadLeft || input == LayoutInput::kTouchpadRight) && mode == "playstation")
    return PadTarget("touchpad");
  return PadTarget(pads[static_cast<std::size_t>(input)]);
}

StickMode LayoutDefaultStick(LayoutInput input) {
  return input == LayoutInput::kRightStick ? StickMode::kPadRightStick : StickMode::kPadLeftStick;
}

bool LayoutTargetSends(const LayoutTarget& target, std::string_view mode) {
  if (target.kind == LayoutTarget::Kind::kNone) return false;
  if (target.kind != LayoutTarget::Kind::kPad) return true;
  if (mode == "keyboard") return false;
  if (target.pad == "guide") return mode != "playstation";
  if (target.pad == "touchpad") return mode == "playstation";
  return true;
}

std::vector<std::string> ControllerLayoutCapabilities(const ControllerLayout& layout) {
  if (layout.empty()) return {};
  return {kKitInputMap};
}

const std::vector<std::string>& LayoutKeyNames() {
  static const std::vector<std::string> names = [] {
    std::vector<std::string> list;
    for (char c = 'a'; c <= 'z'; ++c) list.emplace_back(1, c);
    for (char c = '0'; c <= '9'; ++c) list.emplace_back(1, c);
    for (int f = 1; f <= 24; ++f) list.push_back("f" + std::to_string(f));
    for (const auto& key : kNamedKeys) list.emplace_back(key.name);
    for (const auto& key : kModifierKeys) list.emplace_back(key.name);
    return list;
  }();
  return names;
}

const std::vector<std::string>& LayoutModifiers() {
  static const std::vector<std::string> names = [] {
    std::vector<std::string> list;
    for (const auto& key : kModifierKeys) list.emplace_back(key.name);
    return list;
  }();
  return names;
}

bool IsLayoutKey(std::string_view name) {
  if (NamedCode(name)) return true;
  if (name.size() != 7 || name.substr(0, 5) != "vk:0x") return false;
  const int high = HexDigit(name[5]), low = HexDigit(name[6]);
  if (high < 0 || low < 0) return false;
  const auto code = static_cast<unsigned>(high * 16 + low);
  return code != 0 && !CodeHasName(code);
}

bool IsLayoutModifier(std::string_view name) { return ModifierIndex(name) >= 0; }

std::optional<unsigned> LayoutKeyCode(std::string_view name) {
  if (const auto code = NamedCode(name)) return code;
  if (!IsLayoutKey(name)) return std::nullopt;
  return static_cast<unsigned>(HexDigit(name[5]) * 16 + HexDigit(name[6]));
}

std::vector<std::string> OrderLayoutKeys(std::vector<std::string> keys) {
  std::stable_sort(keys.begin(), keys.end(), [](const std::string& left, const std::string& right) {
    const int a = ModifierIndex(left), b = ModifierIndex(right);
    return (a < 0 ? 1000 : a) < (b < 0 ? 1000 : b);
  });
  return keys;
}

std::optional<LayoutTarget> LayoutTargetFromBinding(std::string_view binding) {
  std::string value(binding);
  for (auto& c : value)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  if (value == "none") return LayoutTarget{};
  if (value == "mouse_left" || value == "mouse_right" || value == "mouse_middle") return MouseTarget(value.substr(6));
  if (value.size() == 7 && value.compare(0, 5, "vk:0x") == 0) {
    const int high = HexDigit(value[5]), low = HexDigit(value[6]);
    if (high < 0 || low < 0 || (high | low) == 0) return std::nullopt;
    // A code that has a name is written by its name.
    return KeyTarget({NameForCode(static_cast<unsigned>(high * 16 + low))});
  }
  if (!NamedCode(value)) return std::nullopt;
  return KeyTarget({value});
}

const std::vector<ControllerLayoutTemplate>& ControllerLayoutTemplates() {
  // The old presets' input, line for line (runtime/input/*.input): Gamepad passes every input through, Mouse and
  // Arrow keys send what their bindings send and nothing else (keyboard mode has no controller).
  static const std::vector<ControllerLayoutTemplate> templates = {
      {"gamepad", "Gamepad", "xinput",
       "cross = pad:a;circle = pad:b;square = pad:x;triangle = pad:y;l1 = pad:lb;r1 = pad:rb;l2 = pad:lt;"
       "r2 = pad:rt;l3 = pad:ls;r3 = pad:rs;up = pad:dpad_up;down = pad:dpad_down;left = pad:dpad_left;"
       "right = pad:dpad_right;options = pad:start;create = pad:back;touchpad_left = pad:back;"
       "touchpad_right = pad:guide;left_stick = pad:left_stick;right_stick = pad:right_stick"},
      {"mouse", "Mouse", "keyboard",
       "cross = mouse:left;circle = mouse:right;square = mouse:middle;triangle = none;l1 = none;r1 = none;"
       "l2 = mouse:right;r2 = mouse:left;l3 = none;r3 = none;up = none;down = none;left = none;right = none;"
       "options = key:enter;create = none;touchpad_left = none;touchpad_right = none;left_stick = mouse;"
       "left_stick_speed = 900;right_stick = none"},
      {"arrows", "Arrow keys", "keyboard",
       "cross = key:z;circle = key:x;square = key:shift;triangle = key:c;l1 = none;r1 = none;l2 = none;r2 = none;"
       "l3 = none;r3 = none;up = key:up;down = key:down;left = key:left;right = key:right;options = key:enter;"
       "create = none;touchpad_left = none;touchpad_right = none;left_stick = none;right_stick = none"},
      {"wasd-mouse", "WASD and mouse", "keyboard",
       "cross = key:space;circle = key:ctrl;square = key:r;triangle = key:e;l1 = key:q;r1 = key:f;"
       "l2 = mouse:right;r2 = mouse:left;l3 = key:shift;r3 = key:v;up = mouse:wheel_up;down = mouse:wheel_down;"
       "left = key:1;right = key:2;options = key:escape;create = key:tab;touchpad_left = key:m;"
       "touchpad_right = key:i;left_stick = keys;left_stick_keys = w,s,a,d;right_stick = mouse"},
  };
  return templates;
}

const ControllerLayoutTemplate* FindControllerLayoutTemplate(std::string_view id) {
  for (const auto& layout : ControllerLayoutTemplates())
    if (layout.id == id) return &layout;
  return nullptr;
}

std::string ControllerLayoutSummary(std::string_view text) {
  if (text.empty()) return {};
  std::string canonical;
  if (CanonicalControllerLayout(text, &canonical, nullptr))
    for (const auto& layout : ControllerLayoutTemplates())
      if (layout.text == canonical) return layout.name;
  return "Custom";
}

}  // namespace playden::runtime
