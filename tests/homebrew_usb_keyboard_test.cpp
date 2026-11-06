#include "homebrew/usb_keyboard.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#define CHECK(cond)                                                                     \
  do {                                                                                  \
    if (!(cond)) {                                                                      \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);     \
      std::exit(1);                                                                     \
    }                                                                                   \
  } while (0)

using namespace playden::homebrew;

namespace {

KeyboardReport Keys(std::initializer_list<std::uint16_t> keys, std::uint8_t modifiers = 0) {
    KeyboardReport report;
    report.connected = true;
    report.modifiers = modifiers;
    report.keys = keys;
    return report;
}

std::string Typed(const std::vector<KeyboardCommand>& commands) {
    std::string text;
    for (const auto& command : commands) text += command.text.empty() ? "<" + command.action + ">" : command.text;
    return text;
}

}  // namespace

int main() {
    // The 96-byte state: connected at 16, key count at 20, modifiers at 28, usages from 32.
    std::uint8_t state[kKeyboardStateBytes] = {};
    CHECK(!ParseKeyboardState(state).connected);
    state[16] = 1;
    const std::int32_t count = 3;
    std::memcpy(state + 20, &count, 4);
    state[28] = 0x02;
    const std::uint16_t usages[] = {0x04, 0x01, 0x1e};  // a, a rollover error code, 1
    std::memcpy(state + 32, usages, sizeof(usages));
    auto report = ParseKeyboardState(state);
    CHECK(report.connected && report.modifiers == 0x02 && (report.keys == std::vector<std::uint16_t>{0x04, 0x1e}));
    const std::int32_t too_many = 99;
    std::memcpy(state + 20, &too_many, 4);
    CHECK(ParseKeyboardState(state).keys.size() <= 16);

    // In a text box: letters with Shift, digits and symbols type; Enter is Done.
    KeyboardInput input;
    std::uint64_t now = 1000000;
    std::string typed;
    for (const auto& [key, modifiers] : std::initializer_list<std::pair<std::uint16_t, std::uint8_t>>{
             {0x04, 0x02}, {0x05, 0}, {0x2d, 0}, {0x1e, 0x20}, {0x2c, 0}, {0x38, 0}}) {
        typed += Typed(input.Update(Keys({key}, modifiers), true, now += 20000));
        typed += Typed(input.Update(Keys({}), true, now += 20000));
    }
    CHECK(typed == "Ab-! /");
    CHECK(Typed(input.Update(Keys({0x2a}), true, now += 20000)) == "<backspace>");
    CHECK(Typed(input.Update(Keys({0x50, 0x2a}), true, now += 20000)) == "<caret-left>");
    CHECK(Typed(input.Update(Keys({}), true, now += 20000)).empty());
    CHECK(Typed(input.Update(Keys({0x28}), true, now += 20000)) == "<done>");
    CHECK(Typed(input.Update(Keys({}), true, now += 20000)).empty());
    CHECK(Typed(input.Update(Keys({0x29}), true, now += 20000)) == "<circle>");
    // Ctrl with a letter types nothing.
    CHECK(Typed(input.Update(Keys({0x06}, 0x01), true, now += 20000)).empty());
    CHECK(Typed(input.Update(Keys({}), true, now += 20000)).empty());

    // A held arrow's repeats say they are repeats, so a side menu stops at its ends.
    {
        const auto first = input.Update(Keys({0x51}), false, now += 20000);
        CHECK(first.size() == 1 && first[0].action == "down" && !first[0].repeat);
        const auto held = input.Update(Keys({0x51}), false, now += 500000);
        CHECK(held.size() == 1 && held[0].action == "down" && held[0].repeat);
        CHECK(input.Update(Keys({}), false, now += 20000).empty());
    }

    // A held key repeats after a delay; Enter does not.
    CHECK(Typed(input.Update(Keys({0x07}), true, now += 20000)) == "d");
    CHECK(Typed(input.Update(Keys({0x07}), true, now += 100000)).empty());
    CHECK(Typed(input.Update(Keys({0x07}), true, now += 400000)) == "d");
    CHECK(Typed(input.Update(Keys({0x07}), true, now += 50000)) == "d");
    CHECK(Typed(input.Update(Keys({}), true, now += 50000)).empty());
    CHECK(Typed(input.Update(Keys({0x28}), true, now += 20000)) == "<done>");
    CHECK(Typed(input.Update(Keys({0x28}), true, now += 1000000)).empty());
    CHECK(Typed(input.Update(Keys({}), true, now += 20000)).empty());

    // Keys the system keyboard read: Enter held when it closes is not a new press, and a held
    // letter does not keep repeating.
    CHECK(Typed(input.Update(Keys({0x07}), true, now += 20000)) == "d");
    input.Follow(Keys({0x07, 0x28}));
    CHECK(Typed(input.Update(Keys({0x07, 0x28}), false, now += 1000000)).empty());
    CHECK(Typed(input.Update(Keys({}), false, now += 20000)).empty());
    CHECK(Typed(input.Update(Keys({0x28}), false, now += 20000)) == "<cross>");
    CHECK(Typed(input.Update(Keys({}), false, now += 20000)).empty());

    // Elsewhere the keys move around Playden, as in the desktop build.
    CHECK(Typed(input.Update(Keys({0x51}), false, now += 20000)) == "<down>");
    CHECK(Typed(input.Update(Keys({}), false, now += 20000)).empty());
    CHECK(Typed(input.Update(Keys({0x28}), false, now += 20000)) == "<cross>");
    CHECK(Typed(input.Update(Keys({}), false, now += 20000)).empty());
    CHECK(Typed(input.Update(Keys({0x2b}, 0x02), false, now += 20000)) == "<l1>");
    CHECK(Typed(input.Update(Keys({}), false, now += 20000)).empty());
    CHECK(Typed(input.Update(Keys({0x16}), false, now += 20000)) == "<search>");
    // Unplugged: nothing is held any more.
    CHECK(Typed(input.Update(KeyboardReport{}, false, now += 20000)).empty());

    std::puts("usb keyboard tests passed");
}
