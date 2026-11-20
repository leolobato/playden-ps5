// controller_layout_test.cpp - The controller layout's grammar (prospero-win docs/INPUT_MAP.md): what it reads,
// what it refuses, the canonical text and the built-in templates.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime/controller_layout.h"

#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "%d: %s\n", __LINE__, #c); std::exit(1); } } while (0)

namespace {

using namespace playden::runtime;

std::string Canonical(const std::string& text) {
  std::string out, reason;
  if (!CanonicalControllerLayout(text, &out, &reason)) {
    std::fprintf(stderr, "refused %s: %s\n", text.c_str(), reason.c_str());
    return "<refused>";
  }
  return out;
}

bool Refused(const std::string& text) {
  std::string reason;
  const bool refused = !ParseControllerLayout(text, nullptr, &reason);
  if (refused && reason.empty()) std::fprintf(stderr, "no reason for %s\n", text.c_str());
  return refused && !reason.empty();
}

}  // namespace

int main() {
  // No text is no layout.
  {
    ControllerLayout layout;
    CHECK(ParseControllerLayout("", &layout, nullptr) && layout.empty());
    CHECK(SerializeControllerLayout(layout).empty() && ControllerLayoutCapabilities(layout).empty());
    CHECK(ControllerLayoutSummary("").empty());
  }

  // The templates: written in full, canonical, and the sizes the grammar gives them.
  {
    const auto& templates = ControllerLayoutTemplates();
    CHECK(templates.size() == 4);
    const std::pair<const char*, std::size_t> sizes[] = {{"gamepad", 362}, {"mouse", 335}, {"arrows", 300},
                                                         {"wasd-mouse", 366}};
    for (const auto& [id, size] : sizes) {
      const auto* layout = FindControllerLayoutTemplate(id);
      CHECK(layout && layout->text.size() == size);
      ControllerLayout parsed;
      std::string reason;
      CHECK(ParseControllerLayout(layout->text, &parsed, &reason));
      CHECK(SerializeControllerLayout(parsed) == layout->text);
      for (const auto input : LayoutInputs()) CHECK(parsed[input].set);  // one line for every input
      CHECK(ControllerLayoutCapabilities(parsed) == std::vector<std::string>{kKitInputMap});
      CHECK(ControllerLayoutSummary(layout->text) == layout->name);
    }
    CHECK(FindControllerLayoutTemplate("gamepad")->mode == "xinput" && FindControllerLayoutTemplate("mouse")->mode == "keyboard" &&
          FindControllerLayoutTemplate("arrows")->mode == "keyboard" &&
          FindControllerLayoutTemplate("wasd-mouse")->mode == "keyboard");
    CHECK(FindControllerLayoutTemplate("gamepad")->name == "Gamepad" &&
          FindControllerLayoutTemplate("wasd-mouse")->name == "WASD and mouse");
    // Gamepad is every input's own default in xinput mode.
    ControllerLayout gamepad;
    CHECK(ParseControllerLayout(FindControllerLayoutTemplate("gamepad")->text, &gamepad, nullptr));
    for (const auto input : LayoutInputs()) {
      if (LayoutInputKindOf(input) == LayoutInputKind::kStick)
        CHECK(gamepad[input].stick.mode == LayoutDefaultStick(input));
      else
        CHECK(gamepad[input].target == LayoutDefaultTarget(input, "xinput"));
    }
    CHECK(!FindControllerLayoutTemplate("trackpad"));
  }

  // The grammar's own example, and spacing around = and ; that the runtime kit trims.
  {
    const std::string example =
        "cross = key:space;circle = key:ctrl;square = key:r;triangle = key:e;l2 = mouse:right;r2 = mouse:left;"
        "r2_threshold = 30;up = mouse:wheel_up;down = mouse:wheel_down;left_stick = keys;left_stick_keys = w,s,a,d;"
        "right_stick = mouse;right_stick_speed = 1500";
    CHECK(Canonical(example) == example);
    CHECK(Canonical(" cross=pad:b ;\tcircle =  pad:a") == "cross = pad:b;circle = pad:a");
    // Lines come out in the input table's order, tuning right after its input.
    CHECK(Canonical("right_stick_speed = 1500;right_stick = mouse;cross = none") ==
          "cross = none;right_stick = mouse;right_stick_speed = 1500");
    CHECK(Canonical("r2_threshold = 30;r2 = key:space") == "r2 = key:space;r2_threshold = 30");
    // A line that states the default means no line; a stick with only tuning lines gets its default mode.
    CHECK(Canonical("left_stick = mouse;left_stick_deadzone = 16;left_stick_curve = squared;left_stick_speed = 1200;"
                    "left_stick_invert_x = false;left_stick_outer = 0") == "left_stick = mouse");
    CHECK(Canonical("right_stick_deadzone = 10") == "right_stick = pad:right_stick;right_stick_deadzone = 10");
    CHECK(Canonical("left_stick = keys;left_stick_keys = up,down,left,right;left_stick_deadzone = 40") ==
          "left_stick = keys;left_stick_keys = up,down,left,right");
    CHECK(Canonical("left_stick = wheel;left_stick_speed = 100;left_stick_invert_y = true;left_stick_curve = cubed") ==
          "left_stick = wheel;left_stick_curve = cubed;left_stick_speed = 100;left_stick_invert_y = true");
    CHECK(Canonical("left_stick = pad:right_stick;left_stick_deadzone = 0;left_stick_outer = 10") ==
          "left_stick = pad:right_stick;left_stick_outer = 10");
    CHECK(Canonical("left_stick = pad:dpad;left_stick_deadzone = 10;left_stick_invert_x = true") ==
          "left_stick = pad:dpad;left_stick_deadzone = 10;left_stick_invert_x = true");
    CHECK(Canonical("left_stick = keys;left_stick_keys = none,none,a,vk:0x07") ==
          "left_stick = keys;left_stick_keys = none,none,a,vk:0x07");
    // Every button target.
    CHECK(Canonical("cross = key:ctrl+shift+s;circle = key:lctrl+rctrl+lwin+rwin;square = key:semicolon;"
                    "triangle = key:vk:0x07;l1 = mouse:back;r1 = mouse:forward;options = pad:guide;create = pad:lt;"
                    "touchpad_left = pad:touchpad;touchpad_right = key:num0;l2 = pad:rt;r2 = pad:a") ==
          "cross = key:ctrl+shift+s;circle = key:lctrl+rctrl+lwin+rwin;square = key:semicolon;"
          "triangle = key:vk:0x07;l1 = mouse:back;r1 = mouse:forward;l2 = pad:rt;r2 = pad:a;options = pad:guide;"
          "create = pad:lt;touchpad_left = pad:touchpad;touchpad_right = key:num0");
    // Modifiers first, then the other keys in the order written.
    CHECK(Canonical("cross = key:alt+f4+tab") == "cross = key:alt+f4+tab");
  }

  // What the grammar refuses.
  {
    for (const char* text : {
             // Lines Playden writes or that the input map refuses.
             "map = 1", "mode = keyboard", "rumble = 50", "player2 = arrows", "preset = mouse", "mouse = left_stick",
             "mouse_speed = 900", "touchpad = pad:back",
             // Old-style values.
             "cross = z", "cross = mouse_left", "cross = vk:0x41", "cross = space",
             // Case, spaces, separators and signs.
             "cross = key:Z", "CROSS = pad:a", "cross = key:ctrl + s", "cross = key:ctrl+ s", "cross = pad:a;",
             ";cross = pad:a", "cross = pad:a;;circle = pad:b", "cross pad:a", "cross =", "= pad:a",
             "cross = pad:a # comment", "[input]", "cross = pad:a\ncircle = pad:b", "cross = key:a=b",
             "left stick = mouse",
             // Unknown or repeated lines and values.
             "cross = pad:a;cross = pad:b", "unknown = none", "left_stick_foo = 1", "l1_threshold = 30",
             "cross = pad:start2", "cross = mouse:wheel", "cross = key:", "cross = key:hyper", "cross = keys",
             "left_stick = pad:a", "cross = pad:left_stick", "left_stick = dpad",
             // Key combinations.
             "cross = key:shift+ctrl+s", "cross = key:s+ctrl", "cross = key:a+a", "cross = key:a+b+c+d+e",
             "cross = key:vk:0x20", "cross = key:vk:0x00", "cross = key:vk:0x7", "cross = key:vk:0xzz",
             "cross = key:key:a", "cross = key:a++b",
             // Numbers.
             "left_stick_deadzone = 016", "left_stick_deadzone = 91", "left_stick_deadzone = -1",
             "left_stick_outer = 51", "left_stick = mouse;left_stick_speed = 0", "left_stick = mouse;left_stick_speed = 20001",
             "left_stick = wheel;left_stick_speed = 101", "left_stick_deadzone = 50;left_stick_outer = 41",
             "left_stick = mouse;left_stick_deadzone = 80;left_stick_outer = 11",
             "left_stick_curve = quadratic", "left_stick_invert_y = yes",
             // Tuning lines a stick's mode doesn't take.
             "left_stick = keys", "left_stick = keys;left_stick_keys = w,s,a", "left_stick = keys;left_stick_keys = w,w,a,d",
             "left_stick = keys;left_stick_keys = key:w,s,a,d", "left_stick = keys;left_stick_keys = ctrl+w,s,a,d",
             "left_stick = keys;left_stick_keys = w,s,a,d;left_stick_outer = 5",
             "left_stick = keys;left_stick_keys = w,s,a,d;left_stick_curve = linear",
             "left_stick = keys;left_stick_keys = w,s,a,d;left_stick_speed = 10",
             "left_stick = keys;left_stick_keys = w,s,a,d;left_stick_deadzone = 5",
             "left_stick = pad:dpad;left_stick_deadzone = 9", "left_stick = mouse;left_stick_keys = w,s,a,d",
             "left_stick_speed = 900", "left_stick = wheel;left_stick_invert_x = true", "left_stick = none;left_stick_deadzone = 10",
             "left_stick_keys = w,s,a,d",
             // Thresholds.
             "r2_threshold = 30", "r2 = pad:rt;r2_threshold = 30", "r2 = none;r2_threshold = 30",
             "l2 = pad:lt;l2_threshold = 30", "r2 = key:space;r2_threshold = 4", "r2 = key:space;r2_threshold = 101",
             "r2 = key:space;r2_threshold = 050"})
      CHECK(Refused(text));
    // At most 4096 bytes.
    CHECK(!Refused(std::string(kControllerLayoutMaxBytes - 9, ' ') + "l1 = none"));
    CHECK(Refused(std::string(kControllerLayoutMaxBytes - 8, ' ') + "l1 = none"));
  }

  // Targets, defaults and what reaches the game in each mode.
  {
    LayoutTarget target;
    CHECK(ParseLayoutTarget("key:ctrl+s", &target, nullptr) && target.kind == LayoutTarget::Kind::kKeys &&
          target.keys == (std::vector<std::string>{"ctrl", "s"}) && LayoutTargetText(target) == "key:ctrl+s");
    CHECK(LayoutTargetText(PadTarget("a")) == "pad:a" && LayoutTargetText(MouseTarget("wheel_up")) == "mouse:wheel_up" &&
          LayoutTargetText(LayoutTarget{}) == "none");
    CHECK(LayoutDefaultTarget(LayoutInput::kTouchpadLeft, "xinput") == PadTarget("back"));
    CHECK(LayoutDefaultTarget(LayoutInput::kTouchpadRight, "xinput") == PadTarget("guide"));
    CHECK(LayoutDefaultTarget(LayoutInput::kTouchpadLeft, "playstation") == PadTarget("touchpad"));
    CHECK(LayoutDefaultTarget(LayoutInput::kTouchpadRight, "playstation") == PadTarget("touchpad"));
    CHECK(LayoutDefaultTarget(LayoutInput::kOptions, "keyboard") == PadTarget("start"));
    CHECK(LayoutDefaultTarget(LayoutInput::kCreate, "xinput") == PadTarget("back"));
    CHECK(LayoutDefaultTarget(LayoutInput::kR2, "xinput") == PadTarget("rt"));
    CHECK(LayoutDefaultStick(LayoutInput::kRightStick) == StickMode::kPadRightStick);
    CHECK(!LayoutTargetSends(PadTarget("a"), "keyboard") && LayoutTargetSends(PadTarget("a"), "playstation"));
    CHECK(!LayoutTargetSends(PadTarget("guide"), "playstation") && LayoutTargetSends(PadTarget("guide"), "xinput"));
    CHECK(!LayoutTargetSends(PadTarget("touchpad"), "xinput") && LayoutTargetSends(PadTarget("touchpad"), "playstation"));
    CHECK(LayoutTargetSends(KeyTarget({"a"}), "xinput") && !LayoutTargetSends(LayoutTarget{}, "keyboard"));
    CHECK(LayoutInputName(LayoutInput::kTouchpadLeft) == "touchpad_left" &&
          LayoutInputFromName("right_stick") == LayoutInput::kRightStick && !LayoutInputFromName("touchpad"));
    CHECK(LayoutInputKindOf(LayoutInput::kL2) == LayoutInputKind::kTrigger &&
          LayoutInputKindOf(LayoutInput::kCreate) == LayoutInputKind::kButton);
  }

  // Key names and their codes (the grammar's table).
  {
    std::set<unsigned> codes;
    for (const auto& name : LayoutKeyNames()) {
      CHECK(IsLayoutKey(name));
      const auto code = LayoutKeyCode(name);
      CHECK(code && codes.insert(*code).second);  // each code has one name
    }
    CHECK(LayoutKeyNames().size() == 26 + 10 + 24 + 47 + 11);
    const std::pair<const char*, unsigned> table[] = {
        {"ctrl", 0x11}, {"lctrl", 0xa2}, {"rctrl", 0xa3}, {"shift", 0x10}, {"lshift", 0xa0}, {"rshift", 0xa1},
        {"alt", 0x12}, {"lalt", 0xa4}, {"ralt", 0xa5}, {"capslock", 0x14}, {"printscreen", 0x2c},
        {"scrolllock", 0x91}, {"numlock", 0x90}, {"apps", 0x5d}, {"lwin", 0x5b}, {"rwin", 0x5c}, {"num0", 0x60},
        {"num9", 0x69}, {"multiply", 0x6a}, {"add", 0x6b}, {"subtract", 0x6d}, {"decimal", 0x6e}, {"divide", 0x6f},
        {"a", 0x41}, {"z", 0x5a}, {"0", 0x30}, {"f1", 0x70}, {"f24", 0x87}, {"space", 0x20}, {"quote", 0xde}};
    for (const auto& [name, code] : table) CHECK(LayoutKeyCode(name) == code);
    CHECK(IsLayoutKey("vk:0x07") && LayoutKeyCode("vk:0x07") == 0x07u);
    CHECK(!IsLayoutKey("vk:0x41") && !IsLayoutKey("vk:0x00") && !IsLayoutKey("vk:0xA7") && !IsLayoutKey("f25") &&
          !IsLayoutKey("f01") && !IsLayoutKey("none") && !IsLayoutKey("mouse_left"));
    CHECK(LayoutModifiers().size() == 11 && LayoutModifiers().front() == "ctrl" && LayoutModifiers().back() == "rwin");
    CHECK(IsLayoutModifier("ralt") && !IsLayoutModifier("s"));
    CHECK(OrderLayoutKeys({"s", "shift", "x", "ctrl"}) == (std::vector<std::string>{"ctrl", "shift", "s", "x"}));
  }

  // The old bindings as targets, for a layout made from a game's buttons.
  {
    CHECK(LayoutTargetFromBinding("Z") == KeyTarget({"z"}));
    CHECK(LayoutTargetFromBinding("Enter") == KeyTarget({"enter"}));
    CHECK(LayoutTargetFromBinding("vk:0x20") == KeyTarget({"space"}));
    CHECK(LayoutTargetFromBinding("VK:0X5B") == KeyTarget({"lwin"}));
    CHECK(LayoutTargetFromBinding("vk:0x07") == KeyTarget({"vk:0x07"}));
    CHECK(LayoutTargetFromBinding("mouse_middle") == MouseTarget("middle"));
    CHECK(LayoutTargetFromBinding("none") == LayoutTarget{});
    CHECK(LayoutTargetFromBinding("f12") == KeyTarget({"f12"}));
    CHECK(LayoutTargetFromBinding("F01") == KeyTarget({"f1"}) && LayoutTargetFromBinding("f09") == KeyTarget({"f9"}));
    CHECK(!LayoutTargetFromBinding("f00") && !LayoutTargetFromBinding("f010"));
    CHECK(!LayoutTargetFromBinding("bogus") && !LayoutTargetFromBinding("vk:0x00") && !LayoutTargetFromBinding(""));
  }

  // Changing a stick's mode drops the tuning the new mode doesn't take.
  {
    LayoutStick stick;
    stick.mode = StickMode::kMouse;
    stick.speed = 900;
    stick.curve = StickCurve::kLinear;
    stick.deadzone = 5;
    stick.invert_x = true;
    SetStickMode(&stick, StickMode::kWheel);
    CHECK(stick.speed == std::nullopt && stick.curve == StickCurve::kLinear && stick.deadzone == 5 && !stick.invert_x);
    SetStickMode(&stick, StickMode::kKeys);
    CHECK(!stick.curve && !stick.deadzone);  // 5 is below keys' 10
    CHECK(ResolvedStick(stick).deadzone == 40);
    CHECK(StickDefaults(StickMode::kMouse).speed == 1200 && StickDefaults(StickMode::kWheel).speed == 10 &&
          StickDefaults(StickMode::kMouse).curve == StickCurve::kSquared && StickDefaults(StickMode::kMouse).deadzone == 16);
    CHECK(StickModeTakes(StickMode::kKeys, StickTuning::kKeys) && !StickModeTakes(StickMode::kMouse, StickTuning::kKeys));
    CHECK(!StickModeTakes(StickMode::kNone, StickTuning::kDeadzone));
    CHECK(StickModeText(StickMode::kPadDpad) == "pad:dpad" && StickModeFromText("wheel") == StickMode::kWheel);
    CHECK(StickTuningSuffix(StickTuning::kInvertY) == "_invert_y");
  }

  // A layout that is not a template reads as custom.
  CHECK(ControllerLayoutSummary("cross = pad:b;circle = pad:a") == "Custom");
  std::fprintf(stderr, "controller_layout: ok\n");
  return 0;
}
