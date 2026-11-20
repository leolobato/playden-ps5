// controller_layout.h - A game's controller layout (the controller_layout setting): what each DualSense input sends
// to the game. The text is the runtime kit's input-map lines for [input], word for word, joined by ';' (a setting's
// value holds no line break): "cross = key:space;circle = key:ctrl;left_stick = keys;left_stick_keys = w,s,a,d".
// prospero-win's docs/INPUT_MAP.md is the grammar; this parser refuses what it refuses, and settings, recipes, My
// presets and the profile check all use it.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace playden::runtime {

// The runtime kit capability that reads `map = 1` and the lines below it in [input].
inline constexpr const char* kKitInputMap = "input-map";
// The longest layout text a setting holds.
inline constexpr std::size_t kControllerLayoutMaxBytes = 4096;

// The DualSense inputs a layout maps, in the order a writer writes them. PS, the two-finger touchpad click, Options +
// Create held (close) and L3 + R3 together (menu) are reserved: no line names them.
enum class LayoutInput {
  kCross, kCircle, kSquare, kTriangle, kL1, kR1, kL2, kR2, kL3, kR3, kUp, kDown, kLeft, kRight, kOptions, kCreate,
  kTouchpadLeft, kTouchpadRight, kLeftStick, kRightStick,
};
inline constexpr std::size_t kLayoutInputCount = 20;
enum class LayoutInputKind { kButton, kTrigger, kStick };

std::string_view LayoutInputName(LayoutInput input);  // "cross", "touchpad_left", "left_stick"
LayoutInputKind LayoutInputKindOf(LayoutInput input);
std::optional<LayoutInput> LayoutInputFromName(std::string_view name);
const std::array<LayoutInput, kLayoutInputCount>& LayoutInputs();  // in writer order

// What a button or trigger sends.
struct LayoutTarget {
  enum class Kind { kNone, kPad, kKeys, kMouse };
  Kind kind = Kind::kNone;
  // kPad: a, b, x, y, lb, rb, lt, rt, ls, rs, dpad_up, dpad_down, dpad_left, dpad_right, start, back, guide, touchpad.
  std::string pad;
  // kKeys: 1 to 4 key names (LayoutKeyNames, or vk:0xNN), each once, modifiers first in kLayoutModifiers order.
  std::vector<std::string> keys;
  // kMouse: left, right, middle, back, forward, wheel_up, wheel_down.
  std::string mouse;
};
bool operator==(const LayoutTarget& left, const LayoutTarget& right);
inline bool operator!=(const LayoutTarget& left, const LayoutTarget& right) { return !(left == right); }

// "pad:a", "key:ctrl+s", "mouse:wheel_up", "none".
std::string LayoutTargetText(const LayoutTarget& target);
bool ParseLayoutTarget(std::string_view text, LayoutTarget* target, std::string* reason);
LayoutTarget PadTarget(std::string_view button);
LayoutTarget KeyTarget(std::vector<std::string> keys);
LayoutTarget MouseTarget(std::string_view button);
// The names each target kind takes, for a picker.
const std::vector<std::string>& LayoutPadButtons();
const std::vector<std::string>& LayoutMouseButtons();

// A stick's mode and its tuning.
enum class StickMode { kPadLeftStick, kPadRightStick, kPadDpad, kKeys, kMouse, kWheel, kNone };
enum class StickCurve { kLinear, kSquared, kCubed };
enum class StickTuning { kKeys, kDeadzone, kOuter, kCurve, kSpeed, kInvertX, kInvertY };  // in writer order
std::string_view StickModeText(StickMode mode);  // "pad:left_stick", "keys", "none"
std::optional<StickMode> StickModeFromText(std::string_view text);
std::string_view StickCurveText(StickCurve curve);  // "linear", "squared", "cubed"
std::string_view StickTuningSuffix(StickTuning tuning);  // "_keys", "_deadzone", ...
// Whether a mode takes a tuning line. `none` takes none; `_keys` is `keys` only, and required there.
bool StickModeTakes(StickMode mode, StickTuning tuning);

struct LayoutStick {
  StickMode mode = StickMode::kNone;
  // Empty: the mode's default (StickDefaults). Parsing leaves a line that states the default empty.
  std::optional<int> deadzone;   // percent: 0-90 (10-90 for pad:dpad and keys)
  std::optional<int> outer;      // percent, 0-50; deadzone + outer <= 90
  std::optional<StickCurve> curve;
  std::optional<int> speed;      // mouse: pixels per second at full tilt, 1-20000; wheel: notches per second, 1-100
  std::optional<bool> invert_x;
  std::optional<bool> invert_y;
  std::array<std::string, 4> keys;  // keys: up, down, left, right; each a key name or "none"
};

// A stick's tuning as it applies: what the lines say, or the mode's defaults.
struct StickValues {
  int deadzone = 0;
  int outer = 0;
  StickCurve curve = StickCurve::kLinear;
  int speed = 0;  // 0 when the mode takes no speed
  bool invert_x = false;
  bool invert_y = false;
};
StickValues StickDefaults(StickMode mode);
StickValues ResolvedStick(const LayoutStick& stick);
// Changes a stick's mode and drops the tuning the new mode doesn't take (and keys, unless it is `keys`).
void SetStickMode(LayoutStick* stick, StickMode mode);

// One input's line. Unset: the input has no line and sends its default (LayoutDefaultTarget, LayoutDefaultStick).
struct InputMapping {
  bool set = false;
  LayoutTarget target;  // a button or trigger
  int threshold = 0;    // a trigger with a target other than pad:lt, pad:rt or none: 5-100, 0 = the trigger's own click
  LayoutStick stick;    // a stick
};

struct ControllerLayout {
  std::array<InputMapping, kLayoutInputCount> entries;
  InputMapping& operator[](LayoutInput input) { return entries[static_cast<std::size_t>(input)]; }
  const InputMapping& operator[](LayoutInput input) const { return entries[static_cast<std::size_t>(input)]; }
  bool empty() const;  // no input has a line
};

// Parses the setting's text. Empty text is an empty layout (no layout). Refuses with a reason what the runtime kit
// refuses: an unknown line or value, a line given twice, a tuning line its stick's mode doesn't take, upper case,
// a space inside a value, a leading zero, and lines Playden writes itself (map, mode, rumble, player2) or that the
// input map refuses (preset, mouse, mouse_speed, touchpad).
bool ParseControllerLayout(std::string_view text, ControllerLayout* layout, std::string* reason);
// The canonical text: one line per input that has one, in LayoutInputs order, each followed by its tuning lines that
// differ from the default (a trigger's _threshold; a stick's in StickTuning order). Lines are "key = value", joined
// by ';' with no space around it. A stick whose only lines were tuning lines is written with its default mode.
std::string SerializeControllerLayout(const ControllerLayout& layout);
// The [input] lines a profile gets after `map = 1`: SerializeControllerLayout's, one per element.
std::vector<std::string> ControllerLayoutLines(const ControllerLayout& layout);
// Parse, then serialize: the text as Playden stores and writes it.
bool CanonicalControllerLayout(std::string_view text, std::string* canonical, std::string* reason);

// What an input sends without a line. `mode` is the [input] mode (xinput, playstation, keyboard): the touchpad halves
// are Back and Guide in xinput mode and the touchpad click in playstation mode. In keyboard mode a pad: target sends
// nothing, but the default is still the pad target.
LayoutTarget LayoutDefaultTarget(LayoutInput input, std::string_view mode);
StickMode LayoutDefaultStick(LayoutInput input);
// Gives every input without a line its default line for `mode`, as the writer's form has one line per input. An
// editor starts from this, so putting one input back never leaves the layout empty.
void FillControllerLayout(ControllerLayout* layout, std::string_view mode);
// Puts one input back to its default line for `mode`.
void ResetLayoutInput(ControllerLayout* layout, LayoutInput input, std::string_view mode);
// Whether a target reaches the game in a mode: pad: targets send nothing in keyboard mode, pad:guide nothing in
// playstation mode and pad:touchpad nothing in xinput mode.
bool LayoutTargetSends(const LayoutTarget& target, std::string_view mode);

// The runtime kit capabilities a layout needs: kKitInputMap for any line. (Motion and layers will add theirs.)
std::vector<std::string> ControllerLayoutCapabilities(const ControllerLayout& layout);

// Key names, in the grammar's order: letters, digits, f1-f24, the named keys, punctuation, the number pad, then the
// modifiers. vk:0xNN covers a key with no name.
const std::vector<std::string>& LayoutKeyNames();
const std::vector<std::string>& LayoutModifiers();  // ctrl, lctrl, rctrl, shift, ..., rwin
bool IsLayoutKey(std::string_view name);             // a name, or vk:0xNN for a code with no name
bool IsLayoutModifier(std::string_view name);
std::optional<unsigned> LayoutKeyCode(std::string_view name);  // its Windows virtual key code
// A combination in the grammar's order: modifiers first, in LayoutModifiers order, then the other keys as given.
std::vector<std::string> OrderLayoutKeys(std::vector<std::string> keys);

// A button binding of the old grammar (IsProsperoBinding: "z", "Enter", "mouse_left", "vk:0x20", "none") as a target:
// "key:z", "key:enter", "mouse:left", "key:space", "none". Empty for a value that is not a binding.
std::optional<LayoutTarget> LayoutTargetFromBinding(std::string_view binding);

// The built-in layouts. Gamepad, Mouse and Arrow keys give the same input as the controller presets of those names;
// each names the controller mode ([input] mode) it is for, which is a row of its own.
struct ControllerLayoutTemplate {
  std::string id;    // gamepad, mouse, arrows, wasd-mouse
  std::string name;  // shown to players
  std::string mode;  // xinput or keyboard
  std::string text;  // canonical layout text
};
const std::vector<ControllerLayoutTemplate>& ControllerLayoutTemplates();
const ControllerLayoutTemplate* FindControllerLayoutTemplate(std::string_view id);

// A short name for a row: empty for no layout, the template's name for a template's exact text, else "Custom".
std::string ControllerLayoutSummary(std::string_view text);

}  // namespace playden::runtime
