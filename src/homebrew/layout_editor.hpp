// layout_editor.hpp - Words and choices for the controller layout editor (Game settings › Controller layout): what
// each input and target is called, the editor's sections, the values its sheets offer, and the key picker's grid
// for moving the focus. Renderer-neutral: the simulation builds rows from it and view.cpp draws them.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "runtime/controller_layout.h"

#include <string>
#include <string_view>
#include <vector>

namespace playden::homebrew::layout {

namespace rt = playden::runtime;

// The editor's list: a heading, then its inputs. Reserved inputs are listed with Centre buttons.
struct Section {
    std::string key;    // "face", "dpad", "shoulders", "sticks", "centre"
    std::string title;  // "Face buttons"
    std::vector<rt::LayoutInput> inputs;
};
const std::vector<Section>& Sections();

// What a player can't map (docs/INPUT_MAP.md › Reserved): its row key, name and what it does instead.
struct ReservedInput {
    std::string key;    // "ps", "two-finger", "close", "menu"
    std::string title;  // "PS button"
    std::string what;   // "Stays the console's."
};
const std::vector<ReservedInput>& ReservedInputs();

// "Cross", "L2", "Touchpad click, left half", "Left stick".
std::string InputLabel(rt::LayoutInput input);
// "Xbox controller", "PlayStation controller", "Keyboard and mouse": the [input] mode as the Controller row names it.
std::string ModeLabel(std::string_view mode);

// A controller button as the game sees it in a mode: "A" in xinput (and keyboard) mode, "Cross" in playstation mode.
std::string PadLabel(std::string_view pad, std::string_view mode);
// "Left button", "Wheel up".
std::string MouseLabel(std::string_view button);
// A key name as a player reads it: "Ctrl", "A", "Num 4", "F13", "Left Shift", "Key 0x92".
std::string KeyLabel(std::string_view name);
// Keys joined by " + ": "Ctrl + S".
std::string KeysLabel(const std::vector<std::string>& keys);

// "Controller", "Keys", "Mouse", "Nothing".
std::string TargetKind(const rt::LayoutTarget& target);
// What a target sends, without its kind: "A", "Ctrl + S", "Left button", "Nothing".
std::string TargetName(const rt::LayoutTarget& target, std::string_view mode);
// "Controller · A", "Keys · Ctrl + S", "Mouse · Wheel up", "Nothing".
std::string TargetSummary(const rt::LayoutTarget& target, std::string_view mode);

// "Controller left stick", "Keys", "Mouse pointer", "Mouse wheel", "Nothing".
std::string StickModeLabel(rt::StickMode mode);
// The modes a stick sheet offers, in its order.
const std::vector<rt::StickMode>& StickModes();
std::string CurveLabel(rt::StickCurve curve);  // "Linear", "Squared", "Cubed"

// An input's target or stick as it applies: its line, or its default.
rt::LayoutTarget EffectiveTarget(const rt::ControllerLayout& layout, rt::LayoutInput input, std::string_view mode);
rt::LayoutStick EffectiveStick(const rt::ControllerLayout& layout, rt::LayoutInput input);
// The list's value for an input: "Controller · A", "Mouse · Left button at 30%", "Keys · W, S, A, D".
std::string InputSummary(const rt::ControllerLayout& layout, rt::LayoutInput input, std::string_view mode);

// One line of the panel that shows what an input does.
struct Fact {
    std::string label;
    std::string value;
    bool warn = false;
};
// Everything an input does, for the panel beside the list (E2), and a note under it: the default, what Create also
// opens, a target that sends nothing in this mode.
struct InputDetails {
    std::string title;  // "R2"
    std::string kind;   // "Trigger"
    std::vector<Fact> facts;
    std::vector<std::string> notes;
};
InputDetails DescribeInput(const rt::ControllerLayout& layout, rt::LayoutInput input, std::string_view mode);
InputDetails DescribeReserved(const ReservedInput& reserved);

// Why a target sends nothing in a mode, or empty when it sends: "Sends nothing in Keyboard and mouse mode: the game
// has no controller there."
std::string SilentReason(const rt::LayoutTarget& target, std::string_view mode);

// The values a sheet offers for a tuning line, for a stick in a mode (the current value and the default are always
// in it). The trigger threshold's list starts with 0: the trigger's own click.
enum class Tuning { kDeadzone, kOuter, kSpeed, kCurve, kThreshold };
std::vector<int> TuningValues(Tuning tuning, rt::StickMode mode, int current);
// Whether a value can be set: a dead zone and an outer dead zone add up to 90 at most, and pad:dpad and keys take a dead
// zone of 10 or more.
bool TuningAllowed(Tuning tuning, const rt::LayoutStick& stick, int value);
// "16%", "1200 pixels per second", "10 notches per second", "Squared", "At the trigger's click".
std::string TuningLabel(Tuning tuning, rt::StickMode mode, int value);
std::string TuningTitle(Tuning tuning);  // "Dead zone"

// The key picker's grid, as ps5-homebrew-ui's KeyPicker draws it (kKeyPickerKeys, then its Done key): the model moves
// the focus over it and the view draws the picker at the same index. homebrew_host_input checks the two match.
struct PickerKey {
    std::string_view name;
    std::string_view label;
    int row;
    int span;
};
const std::vector<PickerKey>& PickerKeys();  // without the Done key
inline constexpr int kPickerColumns = 15;
inline constexpr int kPickerRows = 6;
inline constexpr int kPickerDoneSpan = 2;
// The index of the Done key (one past the last key).
int PickerDoneIndex();
int PickerIndex(std::string_view name);  // -1 for a name that is not on the grid
// The focus after a move by one step (dx or dy is -1 or 1). `column` is where the player is across the grid; an up or
// down move keeps it, so going back returns to the key they left. Left and right go round a row.
int PickerMove(int focus, int dx, int dy, float* column);
float PickerColumn(int focus);

}  // namespace playden::homebrew::layout
