#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace playden::homebrew {

// A USB keyboard on the PS5, as libSceKeyboard reports it: a 96-byte state per read.
// Layout (the one prospero-win reads, which PS4/PS5 reimplementations agree on):
// u64 timestamp; 8 bytes; u8 connected (+3); s32 key count; u32 locks; u32 modifiers
// (the USB boot report's bits); u16 USB HID usages[16]; 32 reserved bytes.
inline constexpr std::size_t kKeyboardStateBytes = 96;

struct KeyboardReport {
    bool connected = false;
    std::uint8_t modifiers = 0;  // bit 0 left Ctrl, 1 left Shift, 2 left Alt, 3 left GUI, 4..7 the right ones
    std::vector<std::uint16_t> keys;  // pressed USB HID usages
};

KeyboardReport ParseKeyboardState(const std::uint8_t* state);

// What a key press does in Playden: text typed into the visible text box, or one of the
// simulation's actions ("done", "backspace", "cross", "up", ...).
struct KeyboardCommand {
    std::string text;
    std::string action;
    bool repeat = false;  // a held key's repeat, not a new press
};

// Turns successive reports into commands: a key acts when it goes down, and a held key repeats
// after a short delay. While a text box is open, printable keys type (US layout); Enter is Done,
// Escape cancels, Backspace deletes and the arrows move the caret. Elsewhere the keys follow the
// desktop build: arrows move, Enter/Space confirm, Escape goes back, Tab/Q/E change section.
class KeyboardInput {
public:
    std::vector<KeyboardCommand> Update(const KeyboardReport& report, bool editing, std::uint64_t now_us);
    // Another owner (the system keyboard) reads the keys: remember which are down, so none of them
    // reads as a new press afterwards, and stop any repeat.
    void Follow(const KeyboardReport& report);

private:
    std::vector<std::uint16_t> held_;
    std::uint16_t repeating_ = 0;
    std::uint64_t next_repeat_us_ = 0;
};

// The command for one key, or an empty command.
KeyboardCommand KeyCommand(std::uint16_t usage, std::uint8_t modifiers, bool editing);

}  // namespace playden::homebrew
