// layout_editor.cpp - Words and choices for the controller layout editor.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "homebrew/layout_editor.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>

namespace playden::homebrew::layout {
namespace {

using Input = rt::LayoutInput;

bool IsTrigger(Input input) { return input == Input::kL2 || input == Input::kR2; }

bool IsAnalogPull(const rt::LayoutTarget& target) {
    return target.kind == rt::LayoutTarget::Kind::kPad && (target.pad == "lt" || target.pad == "rt");
}

std::string Percent(int value) { return std::to_string(value) + "%"; }

// ps5-homebrew-ui's kKeyPickerKeys, row by row.
const std::vector<PickerKey> kGrid = {
    {"escape", "Esc", 0, 1}, {"f1", "F1", 0, 1}, {"f2", "F2", 0, 1}, {"f3", "F3", 0, 1}, {"f4", "F4", 0, 1},
    {"f5", "F5", 0, 1}, {"f6", "F6", 0, 1}, {"f7", "F7", 0, 1}, {"f8", "F8", 0, 1}, {"f9", "F9", 0, 1},
    {"f10", "F10", 0, 1}, {"f11", "F11", 0, 1}, {"f12", "F12", 0, 1}, {"insert", "Ins", 0, 1}, {"delete", "Del", 0, 1},

    {"backquote", "`", 1, 1}, {"1", "1", 1, 1}, {"2", "2", 1, 1}, {"3", "3", 1, 1}, {"4", "4", 1, 1},
    {"5", "5", 1, 1}, {"6", "6", 1, 1}, {"7", "7", 1, 1}, {"8", "8", 1, 1}, {"9", "9", 1, 1},
    {"0", "0", 1, 1}, {"minus", "-", 1, 1}, {"equals", "=", 1, 1}, {"backspace", "Backspace", 1, 2},

    {"tab", "Tab", 2, 1}, {"q", "Q", 2, 1}, {"w", "W", 2, 1}, {"e", "E", 2, 1}, {"r", "R", 2, 1},
    {"t", "T", 2, 1}, {"y", "Y", 2, 1}, {"u", "U", 2, 1}, {"i", "I", 2, 1}, {"o", "O", 2, 1},
    {"p", "P", 2, 1}, {"lbracket", "[", 2, 1}, {"rbracket", "]", 2, 1}, {"backslash", "\\", 2, 1}, {"home", "Home", 2, 1},

    {"capslock", "Caps", 3, 1}, {"a", "A", 3, 1}, {"s", "S", 3, 1}, {"d", "D", 3, 1}, {"f", "F", 3, 1},
    {"g", "G", 3, 1}, {"h", "H", 3, 1}, {"j", "J", 3, 1}, {"k", "K", 3, 1}, {"l", "L", 3, 1},
    {"semicolon", ";", 3, 1}, {"quote", "'", 3, 1}, {"enter", "Enter", 3, 2}, {"end", "End", 3, 1},

    {"shift", "Shift", 4, 2}, {"z", "Z", 4, 1}, {"x", "X", 4, 1}, {"c", "C", 4, 1}, {"v", "V", 4, 1},
    {"b", "B", 4, 1}, {"n", "N", 4, 1}, {"m", "M", 4, 1}, {"comma", ",", 4, 1}, {"period", ".", 4, 1},
    {"slash", "/", 4, 1}, {"pageup", "PgUp", 4, 1}, {"up", "Up", 4, 1}, {"pagedown", "PgDn", 4, 1},

    {"ctrl", "Ctrl", 5, 2}, {"lwin", "Win", 5, 1}, {"alt", "Alt", 5, 1}, {"space", "Space", 5, 6},
    {"left", "Left", 5, 1}, {"down", "Down", 5, 1}, {"right", "Right", 5, 1},
};

struct Cell {
    int row;
    int column;
    int span;
};

const std::vector<Cell>& Cells() {
    static const std::vector<Cell> cells = [] {
        std::vector<Cell> list;
        int row = -1, column = 0;
        for (const auto& key : kGrid) {
            if (key.row != row) row = key.row, column = 0;
            list.push_back({row, column, key.span});
            column += key.span;
        }
        list.push_back({kPickerRows - 1, kPickerColumns - kPickerDoneSpan, kPickerDoneSpan});
        return list;
    }();
    return cells;
}

}  // namespace

const std::vector<Section>& Sections() {
    static const std::vector<Section> sections = {
        {"face", "Face buttons", {Input::kCross, Input::kCircle, Input::kSquare, Input::kTriangle}},
        {"dpad", "D-pad", {Input::kUp, Input::kDown, Input::kLeft, Input::kRight}},
        {"shoulders", "Shoulders and triggers", {Input::kL1, Input::kR1, Input::kL2, Input::kR2}},
        {"sticks", "Sticks", {Input::kLeftStick, Input::kL3, Input::kRightStick, Input::kR3}},
        {"centre", "Centre buttons", {Input::kOptions, Input::kCreate, Input::kTouchpadLeft, Input::kTouchpadRight}},
    };
    return sections;
}

const std::vector<ReservedInput>& ReservedInputs() {
    static const std::vector<ReservedInput> reserved = {
        {"ps", "PS button", "Stays the console's."},
        {"two-finger", "Two-finger touchpad click", "Opens the PS5 keyboard."},
        {"close", "Options + Create, held", "Held for three seconds, closes the game."},
        {"menu", "L3 + R3", "Pressed together, opens the in-game menu."},
    };
    return reserved;
}

std::string InputLabel(Input input) {
    switch (input) {
        case Input::kCross: return "Cross";
        case Input::kCircle: return "Circle";
        case Input::kSquare: return "Square";
        case Input::kTriangle: return "Triangle";
        case Input::kL1: return "L1";
        case Input::kR1: return "R1";
        case Input::kL2: return "L2";
        case Input::kR2: return "R2";
        case Input::kL3: return "L3 (left stick click)";
        case Input::kR3: return "R3 (right stick click)";
        case Input::kUp: return "Up";
        case Input::kDown: return "Down";
        case Input::kLeft: return "Left";
        case Input::kRight: return "Right";
        case Input::kOptions: return "Options";
        case Input::kCreate: return "Create";
        case Input::kTouchpadLeft: return "Touchpad click, left half";
        case Input::kTouchpadRight: return "Touchpad click, right half";
        case Input::kLeftStick: return "Left stick";
        case Input::kRightStick: return "Right stick";
    }
    return {};
}

std::string ModeLabel(std::string_view mode) {
    if (mode == "playstation") return "PlayStation controller";
    if (mode == "keyboard") return "Keyboard and mouse";
    return "Xbox controller";
}

std::string PadLabel(std::string_view pad, std::string_view mode) {
    const bool playstation = mode == "playstation";
    static const std::map<std::string_view, std::pair<const char*, const char*>> names = {
        {"a", {"A", "Cross"}},
        {"b", {"B", "Circle"}},
        {"x", {"X", "Square"}},
        {"y", {"Y", "Triangle"}},
        {"lb", {"LB", "L1"}},
        {"rb", {"RB", "R1"}},
        {"lt", {"LT", "L2"}},
        {"rt", {"RT", "R2"}},
        {"ls", {"Left stick click", "L3"}},
        {"rs", {"Right stick click", "R3"}},
        {"dpad_up", {"D-pad up", "D-pad up"}},
        {"dpad_down", {"D-pad down", "D-pad down"}},
        {"dpad_left", {"D-pad left", "D-pad left"}},
        {"dpad_right", {"D-pad right", "D-pad right"}},
        {"start", {"Start", "Options"}},
        {"back", {"Back", "Share"}},
        {"guide", {"Guide (Xbox button)", "Guide (Xbox button)"}},
        {"touchpad", {"Touchpad click", "Touchpad click"}},
    };
    const auto found = names.find(pad);
    if (found == names.end()) return std::string(pad);
    return playstation ? found->second.second : found->second.first;
}

std::string MouseLabel(std::string_view button) {
    if (button == "left") return "Left button";
    if (button == "right") return "Right button";
    if (button == "middle") return "Middle button";
    if (button == "back") return "Back button";
    if (button == "forward") return "Forward button";
    if (button == "wheel_up") return "Wheel up";
    if (button == "wheel_down") return "Wheel down";
    return std::string(button);
}

std::string KeyLabel(std::string_view name) {
    static const std::map<std::string_view, const char*> names = {
        {"escape", "Esc"}, {"space", "Space"}, {"enter", "Enter"}, {"tab", "Tab"}, {"backspace", "Backspace"},
        {"capslock", "Caps Lock"}, {"pause", "Pause"}, {"printscreen", "Print Screen"}, {"scrolllock", "Scroll Lock"},
        {"numlock", "Num Lock"}, {"insert", "Insert"}, {"delete", "Delete"}, {"home", "Home"}, {"end", "End"},
        {"pageup", "Page Up"}, {"pagedown", "Page Down"}, {"up", "Up"}, {"down", "Down"}, {"left", "Left"},
        {"right", "Right"}, {"apps", "Menu key"}, {"semicolon", ";"}, {"equals", "="}, {"comma", ","}, {"minus", "-"},
        {"period", "."}, {"slash", "/"}, {"backquote", "`"}, {"lbracket", "["}, {"backslash", "\\"}, {"rbracket", "]"},
        {"quote", "'"}, {"multiply", "Num *"}, {"add", "Num +"}, {"subtract", "Num -"}, {"decimal", "Num ."},
        {"divide", "Num /"}, {"ctrl", "Ctrl"}, {"lctrl", "Left Ctrl"}, {"rctrl", "Right Ctrl"}, {"shift", "Shift"},
        {"lshift", "Left Shift"}, {"rshift", "Right Shift"}, {"alt", "Alt"}, {"lalt", "Left Alt"},
        {"ralt", "Right Alt"}, {"lwin", "Win"}, {"rwin", "Right Win"},
    };
    if (const auto found = names.find(name); found != names.end()) return found->second;
    if (name.size() == 4 && name.substr(0, 3) == "num") return "Num " + std::string(name.substr(3));
    if (name.substr(0, 3) == "vk:") return "Key " + std::string(name.substr(3));
    std::string label(name);
    for (auto& c : label)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return label;
}

std::string KeysLabel(const std::vector<std::string>& keys) {
    std::string label;
    for (const auto& key : keys) label += (label.empty() ? "" : " + ") + KeyLabel(key);
    return label;
}

std::string TargetKind(const rt::LayoutTarget& target) {
    switch (target.kind) {
        case rt::LayoutTarget::Kind::kPad: return "Controller";
        case rt::LayoutTarget::Kind::kKeys: return target.keys.size() == 1 ? "Key" : "Keys";
        case rt::LayoutTarget::Kind::kMouse: return "Mouse";
        case rt::LayoutTarget::Kind::kNone: break;
    }
    return "Nothing";
}

std::string TargetName(const rt::LayoutTarget& target, std::string_view mode) {
    switch (target.kind) {
        case rt::LayoutTarget::Kind::kPad: return PadLabel(target.pad, mode);
        case rt::LayoutTarget::Kind::kKeys: return KeysLabel(target.keys);
        case rt::LayoutTarget::Kind::kMouse: return MouseLabel(target.mouse);
        case rt::LayoutTarget::Kind::kNone: break;
    }
    return "Nothing";
}

std::string TargetSummary(const rt::LayoutTarget& target, std::string_view mode) {
    if (target.kind == rt::LayoutTarget::Kind::kNone) return "Nothing";
    return TargetKind(target) + " · " + TargetName(target, mode);
}

std::string StickModeLabel(rt::StickMode mode) {
    switch (mode) {
        case rt::StickMode::kPadLeftStick: return "Controller left stick";
        case rt::StickMode::kPadRightStick: return "Controller right stick";
        case rt::StickMode::kPadDpad: return "Controller D-pad";
        case rt::StickMode::kKeys: return "Keys";
        case rt::StickMode::kMouse: return "Mouse pointer";
        case rt::StickMode::kWheel: return "Mouse wheel";
        case rt::StickMode::kNone: break;
    }
    return "Nothing";
}

const std::vector<rt::StickMode>& StickModes() {
    static const std::vector<rt::StickMode> modes = {rt::StickMode::kPadLeftStick, rt::StickMode::kPadRightStick,
                                                     rt::StickMode::kPadDpad,      rt::StickMode::kKeys,
                                                     rt::StickMode::kMouse,        rt::StickMode::kWheel,
                                                     rt::StickMode::kNone};
    return modes;
}

std::string CurveLabel(rt::StickCurve curve) {
    switch (curve) {
        case rt::StickCurve::kSquared: return "Squared";
        case rt::StickCurve::kCubed: return "Cubed";
        case rt::StickCurve::kLinear: break;
    }
    return "Linear";
}

rt::LayoutTarget EffectiveTarget(const rt::ControllerLayout& layout, Input input, std::string_view mode) {
    const auto& entry = layout[input];
    return entry.set ? entry.target : rt::LayoutDefaultTarget(input, mode);
}

rt::LayoutStick EffectiveStick(const rt::ControllerLayout& layout, Input input) {
    const auto& entry = layout[input];
    if (entry.set) return entry.stick;
    rt::LayoutStick stick;
    stick.mode = rt::LayoutDefaultStick(input);
    return stick;
}

namespace {

std::string StickKeys(const rt::LayoutStick& stick) {
    std::string keys;
    for (const auto& key : stick.keys) keys += (keys.empty() ? "" : ", ") + (key == "none" || key.empty() ? std::string("-") : KeyLabel(key));
    return keys;
}

}  // namespace

std::string InputSummary(const rt::ControllerLayout& layout, Input input, std::string_view mode) {
    if (rt::LayoutInputKindOf(input) == rt::LayoutInputKind::kStick) {
        const auto stick = EffectiveStick(layout, input);
        switch (stick.mode) {
            case rt::StickMode::kPadLeftStick: return "Controller · Left stick";
            case rt::StickMode::kPadRightStick: return "Controller · Right stick";
            case rt::StickMode::kPadDpad: return "Controller · D-pad";
            case rt::StickMode::kKeys: return "Keys · " + StickKeys(stick);
            case rt::StickMode::kMouse: return "Mouse · Pointer";
            case rt::StickMode::kWheel: return "Mouse · Wheel";
            case rt::StickMode::kNone: break;
        }
        return "Nothing";
    }
    const auto target = EffectiveTarget(layout, input, mode);
    std::string summary = TargetSummary(target, mode);
    if (IsTrigger(input) && IsAnalogPull(target)) summary += " (analog)";
    const int threshold = layout[input].set ? layout[input].threshold : 0;
    if (IsTrigger(input) && threshold > 0 && !IsAnalogPull(target) && target.kind != rt::LayoutTarget::Kind::kNone)
        summary += " at " + Percent(threshold);
    return summary;
}

std::string SilentReason(const rt::LayoutTarget& target, std::string_view mode) {
    if (target.kind != rt::LayoutTarget::Kind::kPad || rt::LayoutTargetSends(target, mode)) return {};
    if (mode == "keyboard") return "Sends nothing with Keyboard and mouse: the game has no controller.";
    if (target.pad == "guide") return "Sends nothing with the PlayStation controller.";
    return "Sends nothing with the Xbox controller.";
}

InputDetails DescribeInput(const rt::ControllerLayout& layout, Input input, std::string_view mode) {
    InputDetails details;
    details.title = InputLabel(input);
    const auto kind = rt::LayoutInputKindOf(input);
    const bool set = layout[input].set;
    if (kind == rt::LayoutInputKind::kStick) {
        details.kind = "Stick";
        const auto stick = EffectiveStick(layout, input);
        const auto values = rt::ResolvedStick(stick);
        details.facts.push_back({"Sends", StickModeLabel(stick.mode)});
        if (stick.mode == rt::StickMode::kKeys) {
            static const char* directions[] = {"Push up", "Push down", "Push left", "Push right"};
            for (std::size_t index = 0; index < 4; ++index) {
                const auto& key = stick.keys[index];
                details.facts.push_back({directions[index], key.empty() || key == "none" ? std::string("Nothing") : KeyLabel(key)});
            }
        }
        if (rt::StickModeTakes(stick.mode, rt::StickTuning::kSpeed))
            details.facts.push_back({"Speed", TuningLabel(Tuning::kSpeed, stick.mode, values.speed)});
        if (rt::StickModeTakes(stick.mode, rt::StickTuning::kDeadzone))
            details.facts.push_back({"Dead zone", Percent(values.deadzone)});
        if (rt::StickModeTakes(stick.mode, rt::StickTuning::kOuter))
            details.facts.push_back({"Outer dead zone", Percent(values.outer)});
        if (rt::StickModeTakes(stick.mode, rt::StickTuning::kCurve))
            details.facts.push_back({"Response curve", CurveLabel(values.curve)});
        if (rt::StickModeTakes(stick.mode, rt::StickTuning::kInvertX))
            details.facts.push_back({"Invert left and right", values.invert_x ? "On" : "Off"});
        if (rt::StickModeTakes(stick.mode, rt::StickTuning::kInvertY))
            details.facts.push_back({"Invert up and down", values.invert_y ? "On" : "Off"});
        const bool pad = stick.mode == rt::StickMode::kPadLeftStick || stick.mode == rt::StickMode::kPadRightStick ||
                         stick.mode == rt::StickMode::kPadDpad;
        if (pad && mode == "keyboard") details.notes.push_back("Sends nothing with Keyboard and mouse: the game has no controller.");
        details.notes.push_back(std::string("Default: ") + StickModeLabel(rt::LayoutDefaultStick(input)) + ".");
    } else {
        details.kind = kind == rt::LayoutInputKind::kTrigger ? "Trigger" : "Button";
        const auto target = EffectiveTarget(layout, input, mode);
        details.facts.push_back({"Sends", TargetKind(target)});
        if (target.kind != rt::LayoutTarget::Kind::kNone)
            details.facts.push_back({TargetKind(target) == "Controller" ? "Button" : TargetKind(target) == "Mouse" ? "Action" : TargetKind(target),
                                     TargetName(target, mode) + (kind == rt::LayoutInputKind::kTrigger && IsAnalogPull(target) ? " (analog)" : "")});
        if (kind == rt::LayoutInputKind::kTrigger && !IsAnalogPull(target) && target.kind != rt::LayoutTarget::Kind::kNone)
            details.facts.push_back({"Pull to press", TuningLabel(Tuning::kThreshold, rt::StickMode::kNone, set ? layout[input].threshold : 0)});
        if (const auto silent = SilentReason(target, mode); !silent.empty()) {
            details.facts.back().warn = true;
            details.notes.push_back(silent);
        }
        if (target.kind == rt::LayoutTarget::Kind::kMouse && (target.mouse == "wheel_up" || target.mouse == "wheel_down"))
            details.notes.push_back("Turns the wheel one notch for each press.");
        if (target.kind == rt::LayoutTarget::Kind::kKeys && target.keys.size() > 1)
            details.notes.push_back("Presses the keys in this order and lets them go in the opposite order.");
        if (input == Input::kCreate) details.notes.push_back("Pressing Create also opens the PS5 capture menu.");
        if (input == Input::kOptions) details.notes.push_back("Options + Create held for three seconds still closes the game.");
        if (input == Input::kL3 || input == Input::kR3) details.notes.push_back("L3 + R3 together still open the in-game menu.");
        const auto fallback = rt::LayoutDefaultTarget(input, mode);
        details.notes.push_back("Default: " + TargetSummary(fallback, mode) + ".");
    }
    if (!set) details.facts.push_back({"Line", "Default"});
    return details;
}

InputDetails DescribeReserved(const ReservedInput& reserved) {
    InputDetails details;
    details.title = reserved.title;
    details.kind = "Reserved";
    details.facts.push_back({"Does", reserved.what});
    details.notes.push_back("A controller layout can't change it.");
    return details;
}

std::vector<int> TuningValues(Tuning tuning, rt::StickMode mode, int current) {
    std::vector<int> values;
    const auto defaults = rt::StickDefaults(mode);
    switch (tuning) {
        case Tuning::kDeadzone:
            values = mode == rt::StickMode::kPadDpad || mode == rt::StickMode::kKeys
                         ? std::vector<int>{10, 20, 30, 40, 50, 60, 70, 80, 90}
                         : std::vector<int>{0, 5, 10, 16, 20, 25, 30, 40, 50};
            values.push_back(defaults.deadzone);
            break;
        case Tuning::kOuter: values = {0, 5, 10, 15, 20, 30, 40, 50}; break;
        case Tuning::kSpeed:
            values = mode == rt::StickMode::kWheel ? std::vector<int>{2, 5, 10, 15, 20, 30, 50, 100}
                                                   : std::vector<int>{300, 600, 900, 1200, 1500, 2000, 3000, 4000, 6000};
            values.push_back(defaults.speed);
            break;
        case Tuning::kCurve:
            values = {static_cast<int>(rt::StickCurve::kLinear), static_cast<int>(rt::StickCurve::kSquared),
                      static_cast<int>(rt::StickCurve::kCubed)};
            break;
        case Tuning::kThreshold: values = {0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100}; break;
    }
    values.push_back(current);
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
    return values;
}

bool TuningAllowed(Tuning tuning, const rt::LayoutStick& stick, int value) {
    const auto values = rt::ResolvedStick(stick);
    if (tuning == Tuning::kDeadzone) {
        const int minimum = stick.mode == rt::StickMode::kPadDpad || stick.mode == rt::StickMode::kKeys ? 10 : 0;
        return value >= minimum && value <= 90 && value + values.outer <= 90;
    }
    if (tuning == Tuning::kOuter) return value >= 0 && value <= 50 && value + values.deadzone <= 90;
    if (tuning == Tuning::kSpeed) return value >= 1 && value <= (stick.mode == rt::StickMode::kWheel ? 100 : 20000);
    if (tuning == Tuning::kThreshold) return value == 0 || (value >= 5 && value <= 100);
    return true;
}

std::string TuningLabel(Tuning tuning, rt::StickMode mode, int value) {
    switch (tuning) {
        case Tuning::kDeadzone:
        case Tuning::kOuter: return Percent(value);
        case Tuning::kSpeed:
            return std::to_string(value) + (mode == rt::StickMode::kWheel ? (value == 1 ? " notch a second" : " notches a second")
                                                                          : " pixels a second");
        case Tuning::kCurve: return CurveLabel(static_cast<rt::StickCurve>(value));
        case Tuning::kThreshold: return value == 0 ? "At the trigger's click" : Percent(value) + " of a full pull";
    }
    return std::to_string(value);
}

std::string TuningTitle(Tuning tuning) {
    switch (tuning) {
        case Tuning::kDeadzone: return "Dead zone";
        case Tuning::kOuter: return "Outer dead zone";
        case Tuning::kSpeed: return "Speed";
        case Tuning::kCurve: return "Response curve";
        case Tuning::kThreshold: return "Pull to press";
    }
    return {};
}

const std::vector<PickerKey>& PickerKeys() { return kGrid; }

int PickerDoneIndex() { return static_cast<int>(kGrid.size()); }

int PickerIndex(std::string_view name) {
    for (std::size_t index = 0; index < kGrid.size(); ++index)
        if (kGrid[index].name == name) return static_cast<int>(index);
    return -1;
}

float PickerColumn(int focus) {
    const auto& cells = Cells();
    const auto& cell = cells[static_cast<std::size_t>(std::clamp(focus, 0, static_cast<int>(cells.size()) - 1))];
    return static_cast<float>(cell.column) + static_cast<float>(cell.span) * 0.5F;
}

int PickerMove(int focus, int dx, int dy, float* column) {
    const auto& cells = Cells();
    const int count = static_cast<int>(cells.size());
    focus = std::clamp(focus, 0, count - 1);
    const Cell here = cells[static_cast<std::size_t>(focus)];
    if (dx != 0) {
        int first = focus, last = focus;
        while (first > 0 && cells[static_cast<std::size_t>(first - 1)].row == here.row) --first;
        while (last + 1 < count && cells[static_cast<std::size_t>(last + 1)].row == here.row) ++last;
        int next = focus + (dx > 0 ? 1 : -1);
        if (next < first) next = last;
        if (next > last) next = first;
        if (column) *column = PickerColumn(next);
        return next;
    }
    if (dy == 0) return focus;
    const int row = here.row + (dy > 0 ? 1 : -1);
    if (row < 0 || row >= kPickerRows) return focus;
    const float wanted = column ? *column : PickerColumn(focus);
    int nearest = focus;
    float distance = 1.0e9F;
    for (int index = 0; index < count; ++index) {
        const Cell& cell = cells[static_cast<std::size_t>(index)];
        if (cell.row != row) continue;
        const float start = static_cast<float>(cell.column), span = static_cast<float>(cell.span);
        if (wanted >= start && wanted < start + span) return index;
        const float away = std::fabs(start + span * 0.5F - wanted);
        if (away < distance) distance = away, nearest = index;
    }
    return nearest;
}

}  // namespace playden::homebrew::layout
