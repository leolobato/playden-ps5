#include "homebrew/usb_keyboard.hpp"

#include <algorithm>
#include <cstring>

namespace playden::homebrew {
namespace {

constexpr std::size_t kMaxKeys = 16;
constexpr std::uint64_t kRepeatDelayUs = 450000;
constexpr std::uint64_t kRepeatIntervalUs = 45000;

std::int32_t s32(const std::uint8_t* p) { std::int32_t v; std::memcpy(&v, p, 4); return v; }
std::uint32_t u32(const std::uint8_t* p) { std::uint32_t v; std::memcpy(&v, p, 4); return v; }

bool shifted(std::uint8_t modifiers) { return (modifiers & 0x22) != 0; }
bool control(std::uint8_t modifiers) { return (modifiers & 0x11) != 0; }

// USB HID usages (keyboard page) and the US layout's characters.
constexpr std::uint16_t kA = 0x04, kZ = 0x1d, k1 = 0x1e, k0 = 0x27, kEnter = 0x28, kEscape = 0x29,
                        kBackspace = 0x2a, kTab = 0x2b, kSpace = 0x2c, kSlash = 0x38, kPageUp = 0x4b,
                        kDelete = 0x4c, kPageDown = 0x4e, kRight = 0x4f, kLeft = 0x50, kDown = 0x51, kUp = 0x52,
                        kKeypadSlash = 0x54, kKeypadEnter = 0x58, kKeypad1 = 0x59, kKeypad0 = 0x62, kKeypadDot = 0x63;

char Printable(std::uint16_t usage, bool shift) {
    if (usage >= kA && usage <= kZ) return static_cast<char>((shift ? 'A' : 'a') + (usage - kA));
    if (usage >= k1 && usage <= k0) {
        static const char plain[] = "1234567890", upper[] = "!@#$%^&*()";
        return (shift ? upper : plain)[usage - k1];
    }
    if (usage >= kKeypad1 && usage <= kKeypad0) return "1234567890"[usage - kKeypad1];
    switch (usage) {
    case kSpace: return ' ';
    case 0x2d: return shift ? '_' : '-';
    case 0x2e: return shift ? '+' : '=';
    case 0x2f: return shift ? '{' : '[';
    case 0x30: return shift ? '}' : ']';
    case 0x31: return shift ? '|' : '\\';
    case 0x33: return shift ? ':' : ';';
    case 0x34: return shift ? '"' : '\'';
    case 0x35: return shift ? '~' : '`';
    case 0x36: return shift ? '<' : ',';
    case 0x37: return shift ? '>' : '.';
    case kSlash: return shift ? '?' : '/';
    case kKeypadSlash: return '/';
    case 0x55: return '*';
    case 0x56: return '-';
    case 0x57: return '+';
    case kKeypadDot: return '.';
    default: return 0;
    }
}

bool Repeats(std::uint16_t usage) {
    return usage != kEnter && usage != kKeypadEnter && usage != kEscape && usage != kTab;
}

}  // namespace

KeyboardReport ParseKeyboardState(const std::uint8_t* state) {
    KeyboardReport report;
    if (state == nullptr || state[16] == 0) return report;
    report.connected = true;
    const auto count = static_cast<std::size_t>(std::clamp<std::int32_t>(s32(state + 20), 0, kMaxKeys));
    report.modifiers = static_cast<std::uint8_t>(u32(state + 28));
    for (std::size_t index = 0; index < count; ++index) {
        std::uint16_t usage;
        std::memcpy(&usage, state + 32 + 2 * index, 2);
        // 0 is no key; 1-3 are the keyboard's error codes (too many keys pressed).
        if (usage > 3) report.keys.push_back(usage);
    }
    return report;
}

KeyboardCommand KeyCommand(std::uint16_t usage, std::uint8_t modifiers, bool editing) {
    const bool shift = shifted(modifiers);
    if (editing) {
        switch (usage) {
        case kEnter: case kKeypadEnter: return {{}, "done"};
        case kEscape: return {{}, "circle"};
        case kBackspace: return {{}, "backspace"};
        case kLeft: return {{}, "caret-left"};
        case kRight: return {{}, "caret-right"};
        default: break;
        }
        if (control(modifiers)) return {};
        if (const char typed = Printable(usage, shift)) return {std::string(1, typed), {}};
        return {};
    }
    switch (usage) {
    case kUp: return {{}, "up"};
    case kDown: return {{}, "down"};
    case kLeft: return {{}, "left"};
    case kRight: return {{}, "right"};
    case kEnter: case kKeypadEnter: case kSpace: return {{}, "cross"};
    case kEscape: return {{}, "circle"};
    case kBackspace: case kDelete: return {{}, "square"};
    case kPageUp: return {{}, "l2"};
    case kPageDown: return {{}, "r2"};
    case kTab: return {{}, shift ? "l1" : "r1"};
    case kA + ('q' - 'a'): return {{}, "l1"};
    case kA + ('e' - 'a'): return {{}, "r1"};
    case kA + ('f' - 'a'): return {{}, "triangle"};
    case kA + ('x' - 'a'): return {{}, "square"};
    case kA + ('s' - 'a'): case kSlash: return {{}, "search"};
    case kA + ('o' - 'a'): return {{}, "options"};
    default: return {};
    }
}

std::vector<KeyboardCommand> KeyboardInput::Update(const KeyboardReport& report, bool editing, std::uint64_t now_us) {
    std::vector<KeyboardCommand> commands;
    const auto& keys = report.connected ? report.keys : std::vector<std::uint16_t>{};
    for (const auto usage : keys) {
        if (std::find(held_.begin(), held_.end(), usage) != held_.end()) continue;
        if (auto command = KeyCommand(usage, report.modifiers, editing); !command.text.empty() || !command.action.empty()) {
            commands.push_back(std::move(command));
            // The newest key repeats while it stays down, as on a computer.
            repeating_ = Repeats(usage) ? usage : 0;
            next_repeat_us_ = now_us + kRepeatDelayUs;
        }
    }
    if (repeating_ != 0 && std::find(keys.begin(), keys.end(), repeating_) == keys.end()) repeating_ = 0;
    if (repeating_ != 0 && commands.empty() && now_us >= next_repeat_us_) {
        if (auto command = KeyCommand(repeating_, report.modifiers, editing); !command.text.empty() || !command.action.empty()) {
            command.repeat = true;
            commands.push_back(std::move(command));
        }
        next_repeat_us_ = now_us + kRepeatIntervalUs;
    }
    held_ = keys;
    return commands;
}

void KeyboardInput::Follow(const KeyboardReport& report) {
    held_ = report.connected ? report.keys : std::vector<std::uint16_t>{};
    repeating_ = 0;
}

}  // namespace playden::homebrew
