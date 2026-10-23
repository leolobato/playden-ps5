#include "runtime/game_settings.h"
#include "runtime/game_probe.h"

#include "homebrew/json.hpp"
#include "platform/managed_directory.h"
#include "steam/backend/app_info.h"
#include "steam/backend/json.h"

#include <algorithm>
#include <cctype>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <limits>
#include <optional>
#include <set>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace playden::runtime {
namespace {

namespace json = homebrew::json;
namespace storage_json = steam::json;
using platform::DirectoryPolicy;
using platform::ManagedDirectory;

// Rows this build no longer has. Settings data written for an earlier build (presets, recipes) may still name them;
// they are dropped where read, so the file still loads.
bool RetiredSetting(std::string_view id) { return id == "opengl_thread"; }

SettingDefinition Choice(std::string id, std::string label, std::string effect, std::string also_called,
                         SettingGroup group, std::vector<SettingChoice> choices, std::string playden_default,
                         int phase = 1, ApplyMode apply = ApplyMode::kNextLaunch) {
  SettingDefinition definition;
  definition.id = std::move(id);
  definition.label = std::move(label);
  definition.effect = std::move(effect);
  definition.also_called = std::move(also_called);
  definition.group = group;
  definition.phase = phase;
  definition.apply = apply;
  definition.choices = std::move(choices);
  definition.playden_default = std::move(playden_default);
  return definition;
}

SettingDefinition Text(std::string id, std::string label, std::string effect, std::string also_called,
                       SettingGroup group) {
  auto definition = Choice(std::move(id), std::move(label), std::move(effect), std::move(also_called), group, {}, "");
  definition.kind = SettingKind::kText;
  return definition;
}

std::vector<SettingDefinition> BuildCatalogue() {
  using G = SettingGroup;
  std::vector<SettingDefinition> list;
  // Game.
  // A new program or folder applies like other settings: applying them switches the prepared
  // title's program (RuntimeHost::RepublishSettings). The picker also lists the game folder's
  // programs, which set "program".
  list.push_back(Choice("launch_option", "Program", "Which program starts when you press Play.",
                        "Steam launch option · [application] executable", G::kGame, {}, "", 1));
  {
    // Not a row of its own: the Program picker sets it to a program from the game folder.
    auto program = Text("program", "Program file", "A program from the game folder instead of a launch option.",
                        "[application] executable", G::kGame);
    list.push_back(std::move(program));
  }
  list.push_back(Text("extra_arguments", "Extra arguments", "Added after the arguments the game needs.",
                      "[application] arguments", G::kGame));
  // Display.
  list.push_back(Choice("resolution", "Resolution", "Size of the game's screen. Higher is sharper and slower.",
                        "[display] desktop", G::kDisplay,
                        {{"1280x720", "1280×720", "Smoother frame rate in heavy scenes. Softer on a 4K TV."},
                         {"1920x1080", "1920×1080", "Full HD."},
                         {"2560x1440", "2560×1440", "Sharper image. May drop below 60 fps."},
                         {"3840x2160", "3840×2160", "The TV's own size. For light games; heavy ones run slowly."}},
                        "1920x1080"));
  list.push_back(Choice("refresh", "Refresh rate",
                        "How often the TV shows a new frame. Some older games run too fast at 120 Hz: if so, set it "
                        "back to 60 Hz.",
                        "[display] refresh", G::kDisplay,
                        {{"60", "60 Hz", "The TV refreshes 60 times a second."},
                         {"120", "120 Hz", "Needs a TV with 120 Hz and the PS5's 120 Hz output on."}},
                        "60"));
  // With a display-120hz runtime the display paces the cap ([display] frame_rate); otherwise DXVK does.
  list.push_back(Choice("frame_limit", "Frame limit", "Caps the frame rate for steadier motion and less heat.",
                        "[display] frame_rate · dxgi.maxFrameRate · d3d9.maxFrameRate", G::kDisplay,
                        {{"off", "Off", "No cap."}, {"120", "120", ""}, {"60", "60", ""}, {"40", "40", ""},
                         {"30", "30", ""}},
                        "off", 2));
  list.push_back(Choice("vsync", "VSync", "Waits for the TV before showing a frame. Stops tearing, adds some delay.",
                        "dxgi.syncInterval · d3d9.presentInterval", G::kDisplay,
                        {{"game", "Game default", "The game's own setting."},
                         {"on", "On", ""},
                         {"off", "Off", ""}},
                        "game", 2));
  list.push_back(Choice("performance_overlay", "Performance overlay", "Frame rate in the corner of the screen.",
                        "DXVK HUD · [display] show_fps", G::kDisplay,
                        {{"off", "Off", ""}, {"on", "On", "Shows frames per second."}}, "off"));
  list.push_back(Choice("overlay_detail", "Overlay detail", "What the performance overlay shows.", "dxvk.hud",
                        G::kDisplay,
                        {{"fps", "Frame rate", ""},
                         {"frametimes", "Frame rate and frame times", "Adds a graph of how long each frame took."},
                         {"full", "Everything", "Also memory, GPU load and the DXVK version."}},
                        "fps", 2));
  list.push_back(Choice("scaling", "Scaling", "How the game's screen fills the TV.", "[display] scaling",
                        G::kDisplay,
                        {{"fit", "Fit", "Keeps the shape; black bars if needed."},
                         {"integer", "Integer", "Whole-number steps; sharp pixels."},
                         {"stretch", "Stretch", "Fills the screen."}},
                        "fit"));
  list.push_back(Choice("window_view", "Window view", "Show only game windows, or the whole Windows desktop.",
                        "[display] view", G::kDisplay,
                        {{"window", "Game windows", ""}, {"desktop", "Entire desktop", ""}}, "window"));
  // Controls.
  list.push_back(Choice("controller", "Controller", "How the game sees your DualSense.", "[input] mode", G::kControls,
                        {{"xinput", "Xbox controller", "For games with controller support."},
                         {"keyboard", "Keyboard and mouse mapping", "Buttons send keys and mouse clicks."}},
                        "xinput"));
  list.push_back(Choice("controller_preset", "Controller preset", "Ready-made button layout for this kind of game.",
                        "[input] preset", G::kControls,
                        {{"none", "None", "Only the buttons set here."},
                         {"gamepad", "Gamepad", "Xbox controller layout; no stick moves the pointer."},
                         {"mouse", "Mouse", "Left stick moves the pointer; Cross clicks, Circle right-clicks, Options Enter."},
                         {"arrows", "Arrow keys", "D-pad sends the arrows; Cross Z, Circle X, Triangle C, Options Enter."}},
                        "none"));
  // Controls › Button mapping.
  static const std::map<std::string, std::string> button_names = {
      {"cross", "Cross"},   {"circle", "Circle"}, {"square", "Square"},   {"triangle", "Triangle"},
      {"l1", "L1"},         {"r1", "R1"},         {"l2", "L2"},           {"r2", "R2"},
      {"l3", "L3"},         {"r3", "R3"},         {"up", "D-pad up"},     {"down", "D-pad down"},
      {"left", "D-pad left"}, {"right", "D-pad right"}, {"options", "Options"}, {"create", "Create"},
      {"touchpad", "Touchpad"}};
  for (const auto& button : ProsperoButtons()) {
    auto definition = Text("button." + button, button_names.at(button), "Key or mouse button this button sends.",
                           "[input] " + button, G::kButtonMapping);
    list.push_back(std::move(definition));
  }
  // Controls › Controller tuning.
  list.push_back(Choice("pointer_stick", "Pointer stick", "Move the mouse pointer with a stick, for menus.",
                        "[input] mouse", G::kControllerTuning,
                        {{"none", "None", ""}, {"left_stick", "Left stick", ""}, {"right_stick", "Right stick", ""}},
                        "none"));
  list.push_back(Choice("pointer_speed", "Pointer speed", "How fast the pointer moves, in pixels per second.",
                        "[input] mouse_speed", G::kControllerTuning,
                        {{"600", "600", ""}, {"900", "900", ""}, {"1200", "1200", ""}, {"1800", "1800", ""},
                         {"2400", "2400", ""}},
                        "1200"));
  list.push_back(Choice("stick_deadzone", "Stick dead zone",
                        "How far a stick moves before the game sees it. Raise it if the camera drifts.",
                        "[input] stick_deadzone", G::kControllerTuning,
                        {{"0", "Off", "The game's own dead zone only."}, {"5", "5%", ""}, {"10", "10%", ""},
                         {"15", "15%", ""}, {"20", "20%", ""}, {"30", "30%", "For a badly drifting stick."}},
                        "0", 4));
  list.push_back(Choice("rumble", "Rumble", "How strongly the controller vibrates.", "[input] rumble",
                        G::kControllerTuning,
                        {{"100", "Full", ""}, {"75", "75%", ""}, {"50", "50%", ""}, {"25", "25%", ""},
                         {"0", "Off", ""}},
                        "100", 4));
  list.push_back(Choice("invert_stick", "Invert stick", "Flip a stick's up and down for this game.",
                        "[input] invert", G::kControllerTuning,
                        {{"none", "None", ""}, {"left_y", "Left stick up/down", ""}, {"right_y", "Right stick up/down", ""}},
                        "none", 4));
  list.push_back(Choice("trigger_deadzone", "Trigger dead zone",
                        "How far L2 and R2 travel before the game sees them. Raise it for worn triggers.",
                        "[input] trigger_deadzone", G::kControllerTuning,
                        {{"0", "Off", ""}, {"5", "5%", ""}, {"10", "10%", ""}, {"20", "20%", ""}, {"30", "30%", ""}},
                        "0", 4));
  list.push_back(Choice("wheel_stick", "Mouse wheel stick", "Scroll the mouse wheel by pushing a stick up or down.",
                        "[input] wheel", G::kControllerTuning,
                        {{"none", "None", ""}, {"left_stick", "Left stick", ""}, {"right_stick", "Right stick", ""}},
                        "none", 4));
  // Sound.
  list.push_back(Choice("volume", "Volume", "How loud this game plays, without changing the console's volume.",
                        "[audio] volume", G::kSound,
                        {{"100", "100%", ""}, {"75", "75%", ""}, {"50", "50%", ""}, {"25", "25%", ""},
                         {"0", "Muted", ""}},
                        "100", 4));
  // Compatibility.
  // Shown only for a game whose program Playden refuses for a reason a run could get past.
  list.push_back(Choice("start_check", "Known problems", "What Playden does with a problem it found in the game's program.",
                        ".NET · shared section", G::kCompatibility,
                        {{"stop", "Don't start", "Playden stops before a program it expects to fail."},
                         {"try", "Try anyway", "Prepares the game anyway. It may not start."}},
                        "stop", 1));
  // Compatibility › Graphics.
  // "OpenGL worker thread" ([display] opengl_thread) was the PS5 OpenGL SDK's; Zink has none, so the row is gone.
  // Saved values for it are dropped when the record loads (RecordFromValue).
  list.push_back(Choice("graphics", "Graphics", "How Direct3D is drawn on the PS5 GPU.", "[application] graphics",
                        G::kGraphics,
                        {{"auto", "Automatic", ""}, {"gdi", "GDI", "2D only."}, {"dxvk", "DXVK", "Direct3D 9-11 on Vulkan."},
                         {"opengl", "OpenGL", ""}},
                        "auto"));
  list.push_back(Choice("d3d12", "Direct3D 12", "What draws a Direct3D 12 game.", "d3d12.dll · d3d12core.dll",
                        G::kGraphics,
                        {{"wine", "Wine", "Wine's own Direct3D 12."},
                         {"vkd3d-proton", "vkd3d-proton", "Direct3D 12 on Vulkan, as in Proton. Experimental."}},
                        "wine"));
  // RADV runs pixel shaders in wave64 on the PS5's GPU by default; some never finish there and the console freezes
  // (Stray). Wave32 ran the same as wave64 in every game tested, so it is the default. A saved "auto" (the earlier
  // Automatic choice) is dropped when the record loads (RecordFromValue).
  list.push_back(Choice("pixel_shader_waves", "Pixel shaders",
                        "How many pixels the GPU shades at once. Change it only if a game freezes or draws wrong.",
                        "RADV_PERFTEST=pswave32", G::kGraphics,
                        {{"wave32", "Wave32", "32 at a time. Works in every game tested so far."},
                         {"wave64", "Wave64",
                          "64 at a time, the older default. Freezes the console in some games: try it only if a game "
                          "draws wrong in Wave32."}},
                        "wave32"));
  list.push_back(Choice("frame_latency", "Frame latency", "How many frames can wait in the queue.",
                        "dxgi.maxFrameLatency · d3d9.maxFrameLatency", G::kGraphics,
                        {{"game", "Game default", ""}, {"1", "1", "Lowest delay."}, {"2", "2", ""}, {"3", "3", ""}},
                        "game", 2));
  list.push_back(Choice("reported_gpu", "Reported graphics card", "Which GPU maker the game is told it runs on.",
                        "dxgi.customVendorId", G::kGraphics,
                        {{"game", "DXVK default", ""}, {"amd", "AMD", ""}, {"nvidia", "NVIDIA", ""}, {"intel", "Intel", ""}},
                        "game", 2));
  list.push_back(Choice("reported_vram", "Reported video memory", "How much VRAM the game is told it has. Does not add memory.",
                        "dxgi.maxDeviceMemory · d3d9.maxAvailableMemory", G::kGraphics,
                        {{"game", "DXVK default", ""}, {"2048", "2 GB", ""}, {"4096", "4 GB", ""}}, "game", 2));
  // Compatibility › Windows.
  list.push_back(Choice("steam_features", "Steam features", "How the game reaches Steam without the Steam client.",
                        "steam_api.dll · steam_api64.dll", G::kWindows,
                        {{"emulate", "Offline emulation", "Playden's Steam emulator answers the game."},
                         {"online", "Online (experimental)",
                          "The emulator, plus a Steam sign-in: tickets, lobbies, friends."},
                         {"game", "Game's own files", "The game's Steam library, unchanged."}},
                        "game", 1, ApplyMode::kGameClosed));
  list.push_back(Choice("windows_version", "Windows version", "Which Windows the game is told it runs on.",
                        "winver · registry", G::kWindows,
                        {{"win10", "Windows 10", ""}, {"win81", "Windows 8.1", ""}, {"win7", "Windows 7", ""},
                         {"winxp", "Windows XP", ""}},
                        "win10", 3, ApplyMode::kGameClosed));
  list.push_back(Choice("text_scale", "Text scale", "Size of Windows text and dialogs. Shared by every game in the same bottle.",
                        "LogPixels · DPI", G::kWindows,
                        {{"96", "100%", ""}, {"120", "125%", ""}, {"144", "150%", ""}}, "96", 3, ApplyMode::kGameClosed));
  list.push_back(Choice("locale", "Language and region", "Which Windows language and region the game sees.",
                        "LocaleName · registry", G::kWindows,
                        {{"en-US", "English (United States)", ""}, {"en-GB", "English (United Kingdom)", ""},
                         {"de-DE", "German", ""}, {"fr-FR", "French", ""}, {"es-ES", "Spanish", ""},
                         {"pt-BR", "Portuguese (Brazil)", ""}, {"ja-JP", "Japanese", ""}},
                        "en-US", 3, ApplyMode::kGameClosed));
  list.push_back(Choice("cpu_topology", "CPU cores", "How many processor cores the game is told about.",
                        "WINE_CPU_TOPOLOGY", G::kWindows,
                        {{"kit", "Runtime default", ""}, {"4", "4 cores", "For games that fail with many cores."},
                         {"8", "8 cores", ""}},
                        "kit", 3, ApplyMode::kGameClosed));
  list.push_back(Choice("logging", "Logging", "How much detail goes into the game's log file.", "WINEDEBUG",
                        G::kWindows,
                        {{"normal", "Normal", "Errors and loaded modules."},
                         {"verbose", "Verbose", "Also warnings and exceptions, for one session; then Normal again."},
                         {"input", "Input", "Also keyboard, mouse and controller calls, for one session; then Normal again."}},
                        "normal"));
  // Advanced.
  {
    auto environment = Text("environment", "Environment variables",
                            "Extra variables for the game, separated by ';', like WINE_CPU_TOPOLOGY=4:0,1,2,3.",
                            "[environment]", G::kAdvanced);
    environment.phase = 2;
    list.push_back(std::move(environment));
  }
  {
    auto folder = Text("program_folder", "Program folder",
                       "Folder the program starts in, inside the game folder; . is the game folder itself. Empty: the program's own folder.",
                       "[application] working_directory", G::kAdvanced);
    list.push_back(std::move(folder));
  }
  list.push_back(Text("program_arguments", "Program arguments", "The program's own arguments, as Steam lists them.",
                      "launch option arguments", G::kAdvanced));
  list.push_back(Text("arguments", "Required arguments",
                      "Arguments the game needs, from its recipe. {resolution} adds the Resolution size.",
                      "[application] arguments", G::kAdvanced));
  list.push_back(Choice("architecture", "Program type", "Follows the program: 32-bit or 64-bit Windows.",
                        "[application] architecture", G::kAdvanced,
                        {{"pe32", "32-bit", ""}, {"pe64", "64-bit", ""}}, "pe32"));
  list.push_back(Text("dll_overrides", "Library overrides",
                      "Windows libraries the game loads from its own copy, from the recipe, DXVK and you.",
                      "WINEDLLOVERRIDES · [application] dll_overrides", G::kAdvanced));
  // Earlier builds kept only the player's additions here. Still read, never shown: the full
  // list above starts from them, and changing it replaces them.
  list.push_back(Text("library_overrides", "Added library overrides", "Library overrides added after the recipe's.",
                      "[application] dll_overrides", G::kAdvanced));
  {
    auto registry = Text("registry_dll_overrides", "Library load order in the registry",
                         "Library overrides kept in the game's Windows registry, like name=n,b.",
                         "HKCU\\Software\\Wine\\DllOverrides", G::kAdvanced);
    registry.phase = 3;
    registry.apply = ApplyMode::kGameClosed;
    list.push_back(std::move(registry));
  }
  return list;
}

std::string TrimSpaces(std::string text) {
  while (!text.empty() && text.front() == ' ') text.erase(0, 1);
  while (!text.empty() && text.back() == ' ') text.pop_back();
  return text;
}

// The Environment variables with the one Pixel shaders sets, which wins over the player's own:
// - wave32: RADV_PERFTEST gets pswave32 (added to the player's value when there is one) and goes first, so the entry
//   limits never drop it; a RADV_PS5_PS_WAVE64 of the player's is left out.
// - wave64: RADV_PS5_PS_WAVE64=1 goes first; pswave32 is taken out of the player's RADV_PERFTEST, which stays where it
//   was (or is left out when nothing else is in it). A RADV_PERFTEST with "all" fails: it turns pswave32 on.
// - empty (the row is unavailable on this kit): the player's text, unchanged.
// driver_wave32: the title applies the driver's own default, wave32 ([runtime] version 2), so wave32 adds no pswave32;
// the player's RADV_PERFTEST stays as typed and a RADV_PS5_PS_WAVE64 of the player's is still left out.
// debug_env: the variables go to [debug] env lines, whose values are shorter than [environment]'s. Fails with a reason
// when RADV_PERFTEST with pswave32 no longer fits there: the game must not start without it.
// Options are read as PS5_Mesa's parse_debug_string (src/util/u_debug.c) reads them.
bool WithPixelShaderEnvironment(const std::string& entries, const std::string& waves, bool driver_wave32, bool debug_env,
                                std::string* out, std::string* reason) {
  if (waves != "wave32" && waves != "wave64") {
    *out = entries;
    return true;
  }
  const bool wave32 = waves == "wave32";
  std::string perftest;  // wave32: the player's RADV_PERFTEST options, pswave32 added
  bool listed = false;
  std::string rest;
  std::size_t start = 0;
  while (!entries.empty() && start <= entries.size()) {
    const auto end = std::min(entries.find(';', start), entries.size());
    const std::string entry = entries.substr(start, end - start);
    start = end + 1;
    const auto equals = entry.find('=');
    const auto name = TrimSpaces(equals == std::string::npos ? entry : entry.substr(0, equals));
    if (equals != std::string::npos && name == "RADV_PS5_PS_WAVE64") continue;
    if (equals != std::string::npos && name == "RADV_PERFTEST") {
      // Options split as RADV's parse_debug_string splits them: at commas, spaces and newlines. Written back with
      // commas.
      const auto item = entry.substr(equals + 1);
      std::string kept;
      std::size_t at = 0;
      while (at <= item.size()) {
        const auto separator = std::min(item.find_first_of(", \n", at), item.size());
        const auto option = item.substr(at, separator - at);
        at = separator + 1;
        if (option.empty()) continue;
        // parse_debug_string turns on every option for "all", and for any option that "all" starts with ("a", "al").
        if (!wave32 && std::string_view("all").substr(0, option.size()) == option) {
          if (reason)
            *reason = "RADV_PERFTEST=" + option +
                      " in Environment variables turns on wave32 pixel shaders. Remove it, or set Pixel shaders to "
                      "Wave32.";
          return false;
        }
        if (option == "pswave32") {
          if (!wave32 || listed) continue;
          listed = true;
        }
        kept += (kept.empty() ? "" : ",") + option;
      }
      if (wave32) perftest = kept;
      else if (!kept.empty()) rest += (rest.empty() ? "" : ";") + std::string("RADV_PERFTEST=") + kept;
      continue;
    }
    if (!TrimSpaces(entry).empty()) rest += (rest.empty() ? "" : ";") + entry;
  }
  std::string first;
  if (wave32 && driver_wave32) {
    if (!perftest.empty()) first = "RADV_PERFTEST=" + perftest;
  } else if (wave32) {
    if (!listed) perftest += (perftest.empty() ? "" : ",") + std::string("pswave32");
    if (debug_env ? !IsProsperoDebugEnvEntry("RADV_PERFTEST", perftest)
                  : !IsProsperoEnvironmentEntry("RADV_PERFTEST", perftest)) {
      if (reason)
        *reason = "RADV_PERFTEST in Environment variables is too long to add pswave32 for Pixel shaders. Shorten it.";
      return false;
    }
    first = "RADV_PERFTEST=" + perftest;
  } else {
    first = "RADV_PS5_PS_WAVE64=1";
  }
  *out = first.empty() ? rest : rest.empty() ? first : first + ";" + rest;
  return true;
}

constexpr const char* kVerboseWinedebug = "err+all,warn+all,fixme-all,+loaddll,+process,+seh";
// Raw Input, DirectInput, the winmm joystick API and the HID drivers the game hosts: what a game asks for and
// what it finds.
constexpr const char* kInputWinedebug =
    "err+all,warn+all,+loaddll,+process,+rawinput,+dinput,+winmm,+ntoskrnl,+plugplay,+hid,+hidclass";

std::string Backslashes(std::string text) {
  std::replace(text.begin(), text.end(), '/', '\\');
  return text;
}

bool HasControl(std::string_view text) {
  return std::any_of(text.begin(), text.end(), [](char value) {
    const auto byte = static_cast<unsigned char>(value);
    return byte < 0x20 || byte == 0x7f;
  });
}

std::string LowerAscii(std::string text) {
  for (auto& value : text)
    if (value >= 'A' && value <= 'Z') value = static_cast<char>(value - 'A' + 'a');
  return text;
}

void ReplaceAll(std::string* text, std::string_view from, std::string_view to) {
  for (auto at = text->find(from); at != std::string::npos; at = text->find(from, at + to.size()))
    text->replace(at, from.size(), to);
}

std::string CollapseSpaces(const std::string& text) {
  std::string out;
  bool quoted = false;
  for (const char value : text) {
    if (value == ' ' && !quoted && (out.empty() || out.back() == ' ')) continue;
    out.push_back(value);
    if (value == '"') quoted = !quoted;
  }
  while (!out.empty() && out.back() == ' ' && !quoted) out.pop_back();
  return out;
}

const LaunchEntry* FindEntry(const GameRecipe& recipe, std::string_view id) {
  for (const auto& entry : recipe.launch_entries)
    if (entry.id == id) return &entry;
  return nullptr;
}

// The launch entry that starts without a choice: the recipe's first. A recipe generated for a
// program the player chose lists it first; without that choice the next preparation drops it,
// so the default is the first of Steam's entries. A recipe with only the player's program keeps it.
std::string DefaultLaunchEntry(const GameRecipe& recipe) {
  for (const auto& entry : recipe.launch_entries)
    if (entry.id != kProgramEntryId) return entry.id;
  return recipe.launch_entries.empty() ? std::string() : recipe.launch_entries.front().id;
}

// A row that settings built by hand (older callers, tests) may leave out reads as unavailable.
const EffectiveValue& OptionalValue(const EffectiveSettings& settings, const char* id) {
  static const EffectiveValue missing = [] {
    EffectiveValue value;
    value.available = false;
    return value;
  }();
  const auto found = settings.values.find(id);
  return found == settings.values.end() ? missing : found->second;
}

// The Frame limit a software limiter applies (DXVK's maxFrameRate, vkd3d-proton's VKD3D_FRAME_RATE), or empty. That
// is every cap without a display-120hz runtime; with one, the display paces the cap ([display] frame_rate) and only 30
// at 120 Hz output, four vblanks a frame, is left to software. Never both: two pacers at one rate miss vblanks.
std::string SoftwareFrameLimit(const EffectiveSettings& settings) {
  const auto& limit = OptionalValue(settings, "frame_limit");
  if (!limit.available || limit.value == "off") return {};
  if (!settings.display_120hz) return limit.value;
  const auto& refresh = OptionalValue(settings, "refresh");
  return limit.value == "30" && refresh.available && refresh.value == "120" ? limit.value : std::string();
}

// Whether Frame limit owns the game's frame cap: a cap set on a display-120hz runtime. One pacer at a time, so a
// DXVK_FRAME_RATE or VKD3D_FRAME_RATE the player typed in Environment variables is then left out; with Off, or on an
// older runtime, they apply as before.
bool FrameLimitOwnsCap(const EffectiveSettings& settings) {
  const auto& limit = OptionalValue(settings, "frame_limit");
  return settings.display_120hz && limit.available && limit.value != "off";
}

std::string Unavailable(const SettingDefinition& definition, const std::string& graphics, const KitCapabilities& kit,
                        const std::map<std::string, EffectiveValue>& values) {
  const auto& id = definition.id;
  const bool dxvk = graphics == "dxvk";
  const bool display_graphics = dxvk || graphics == "opengl";
  // A display-120hz runtime paces the frame limit itself on every Vulkan path (DXVK, vkd3d-proton, Zink).
  if (id == "frame_limit" && kit.display_120hz) {
    if (!display_graphics) return "Only with DXVK or OpenGL.";
  } else if (id == "frame_limit" || id == "vsync" || id == "frame_latency" || id == "reported_vram" ||
             id == "reported_gpu") {
    // The older runtime's OpenGL has no cap at all; the kit's display paces one once the title is updated.
    if (id == "frame_limit" && !dxvk && graphics == "opengl" && kit.display_120hz_needs_update)
      return kAppliesAfterKitUpdate;
    if (!dxvk) return "Only available with DXVK.";
    if (!kit.dxvk_config) return "Needs a runtime that reads dxvk.conf.";
    // Checked on the console (results/20261005-dxvk-conf-probe): the cap works, DXVK applies
    // latency and VRAM, VSync off does not change the display's pacing.
    if (id == "vsync" && !kit.dxvk_present) return "The PS5 display always waits for the TV; VSync cannot be turned off yet.";
  }
  if ((id == "windows_version" || id == "text_scale") && !kit.registry)
    return "Needs a verified runtime kit with access to the game's bottle.";
  // A Zink kit's runtime (upstream prospero-win) reads MESA_, DXVK_, RADV_ and the like as [debug] env.
  // Pixel shaders is an environment variable for the driver, so it goes where Environment variables go.
  if ((id == "environment" || id == "pixel_shader_waves") && !kit.environment && !kit.opengl_zink)
    return "Needs a prospero-win update that reads [environment].";
  if (id == "wheel_stick" && !kit.input_wheel) return "Needs a prospero-win update that turns the mouse wheel.";
  if (id == "volume" && !kit.audio_volume) return "Needs a prospero-win update with per-game volume.";
  if (id == "stick_deadzone" || id == "rumble" || id == "trigger_deadzone") {
    if (!kit.input_tuning) return "Needs a prospero-win update with controller tuning.";
    const auto controller = values.find("controller");
    if (controller == values.end() || controller->second.value != "xinput") return "Only with the Xbox controller.";
  }
  // Rows whose runtime or Playden side is not built yet: listed so players see what is coming.
  if (id == "invert_stick") return "Needs a prospero-win update that inverts sticks.";
  // Wine sets the user locale from the Unix locale (LANG, LC_ALL) at start and only mirrors it into
  // Control Panel\International, so a registry value alone would be overwritten.
  if (id == "locale") return "Needs a prospero-win update that sets the game's language (LANG).";
  // The title publication carries only the two registry values it owns (Windows version, text scale).
  if (id == "registry_dll_overrides") return "Needs a prospero-win update that lets Playden write more registry values.";
  // Wine reads WINE_CPU_TOPOLOGY from the [environment] section; [debug] env does not take WINE_ names.
  if (id == "cpu_topology" && !kit.environment) return "Needs a prospero-win update that reads [environment].";
  if (id == "performance_overlay" && graphics == "gdi") return "Only available with DXVK or OpenGL.";
  // DXVK's overlay is drawn by its Direct3D 9-11 presenter, not by vkd3d-proton's swapchain. The Direct3D 12 row comes
  // later in the catalogue, so its value here is not checked yet: the same test it gets below.
  const auto d3d12 = values.find("d3d12");
  const bool vkd3d_proton = kit.vkd3d_proton && dxvk && d3d12 != values.end() && d3d12->second.value == "vkd3d-proton";
  if ((id == "performance_overlay" || id == "overlay_detail") && vkd3d_proton) return "Not shown for Direct3D 12 games.";
  if (id == "overlay_detail") {
    if (!dxvk) return "Only available with DXVK.";
    if (!kit.dxvk_config) return "Needs a runtime that reads dxvk.conf.";
    const auto overlay = values.find("performance_overlay");
    if (overlay == values.end() || overlay->second.value != "on") return "Turn the performance overlay on first.";
  }
  if (id == "scaling" && dxvk) return "DXVK games always fit the screen and keep their shape.";
  // vkd3d-proton presents through DXVK's dxgi, which only the DXVK overrides load.
  if (id == "d3d12") {
    if (!kit.vkd3d_proton) return "Needs a runtime kit with vkd3d-proton.";
    if (!dxvk) return "Only available with DXVK.";
  }
  if (id == "refresh") {
    if (!display_graphics) return "Only with DXVK or OpenGL.";
    if (kit.display_120hz_needs_update) return kAppliesAfterKitUpdate;
    if (!kit.display_120hz) return "Needs a runtime kit with 120 Hz output.";
  }
  if (id == "pointer_speed") {
    const auto stick = values.find("pointer_stick");
    if (stick == values.end() || stick->second.value == "none") return "Choose a pointer stick first.";
  }
  return {};
}

}  // namespace

const std::map<std::string, std::map<std::string, std::string>>& ControllerPresetInputs() {
  // Mirrors the [input] mode, mouse and mouse_speed of runtime/input/*.input (a host test compares them).
  static const std::map<std::string, std::map<std::string, std::string>> inputs = {
      {"arrows", {{"controller", "keyboard"}, {"pointer_stick", "none"}}},
      {"gamepad", {{"controller", "xinput"}, {"pointer_stick", "none"}}},
      {"mouse", {{"controller", "keyboard"}, {"pointer_stick", "left_stick"}, {"pointer_speed", "900"}}},
  };
  return inputs;
}

const std::vector<std::string>& ProsperoButtons() {
  static const std::vector<std::string> buttons = {"cross", "circle", "square", "triangle", "l1",    "r1",
                                                   "l2",    "r2",     "l3",     "r3",       "up",    "down",
                                                   "left",  "right",  "options", "create",  "touchpad"};
  return buttons;
}

const std::vector<SettingDefinition>& SettingsCatalogue() {
  static const std::vector<SettingDefinition> catalogue = BuildCatalogue();
  return catalogue;
}

const SettingDefinition* FindSetting(std::string_view id) {
  for (const auto& definition : SettingsCatalogue())
    if (definition.id == id) return &definition;
  return nullptr;
}

bool SettingShown(const SettingDefinition& definition, const KitCapabilities&) {
  return definition.id.rfind("button.", 0) != 0 && definition.id != "program" && definition.id != "library_overrides";
}

bool IsUnlockKey(std::string_view key) {
  const std::string_view prefix = kUnlockPrefix;
  if (key.size() <= prefix.size() || key.substr(0, prefix.size()) != prefix) return false;
  const auto* definition = FindSetting(key.substr(prefix.size()));
  return definition != nullptr;
}

std::string SettingChoiceUnavailable(const EffectiveSettings& settings, std::string_view id, std::string_view value) {
  if (id != "frame_limit" || value == "off") return {};
  const bool high = value == "120" || value == "40";
  if (!settings.display_120hz && !settings.display_120hz_needs_update) {
    // Without a runtime that paces it, the cap is DXVK's maxFrameRate: 30 and 60 as before.
    return high ? "Needs a runtime kit with 120 Hz output." : std::string();
  }
  // A title not on the kit's runtime yet is checked against the choices it gets with it: 120 Hz output as saved.
  const auto refresh = settings.values.find("refresh");
  const bool at_120 = refresh != settings.values.end() &&
                      (refresh->second.available || refresh->second.waits_for_update) &&
                      ChosenValue(refresh->second) == "120";
  if (high && !at_120) return "Only with Refresh rate 120 Hz.";
  // 30 at 120 Hz takes four vblanks per frame, which the display cannot pace; DXVK's and vkd3d-proton's limiters can.
  if (value == "30" && at_120 && settings.graphics != "dxvk") return "At 120 Hz, only with DXVK.";
  return {};
}

bool SettingChoiceWaitsForUpdate(const EffectiveSettings& settings, std::string_view id, std::string_view value) {
  if (id != "frame_limit" || value == "off" || !settings.display_120hz_needs_update) return false;
  // The older runtime's OpenGL has no cap; with DXVK it caps 60 and 30 now, and only the display paces 120 and 40.
  return settings.graphics != "dxvk" || value == "120" || value == "40";
}

bool ValidateSettingValue(const GameRecipe& recipe, std::string_view id, std::string_view value, std::string* reason) {
  const auto fail = [&](std::string why) {
    if (reason) *reason = std::move(why);
    return false;
  };
  const auto* definition = FindSetting(id);
  if (!definition) return fail("unknown setting");
  if (HasControl(value)) return fail("contains a control character");
  if (id == "launch_option") return FindEntry(recipe, value) ? true : fail("not a launch option of this game");
  if (definition->kind == SettingKind::kChoice) {
    for (const auto& choice : definition->choices)
      if (choice.value == value) return true;
    return fail("not one of the choices");
  }
  if (id == "extra_arguments" || id == "program_arguments" || id == "arguments") {
    if (value.size() > kProsperoMaxArgumentBytes) return fail("too long");
    // Required arguments may name where the Resolution size goes; nothing else uses braces.
    std::string rest(value);
    if (id == "arguments") ReplaceAll(&rest, "{resolution}", "");
    if (rest.find_first_of("{}") != std::string::npos) return fail("braces are not allowed");
    if (!SplitProsperoArguments(value, nullptr)) return fail("unclosed quote or too many words");
    return true;
  }
  if (id == "program") {
    const auto relative = steam::RelativeGamePath(value);
    if (!relative || value.size() > 1024 || relative->size() < 5) return fail("not a program in the game folder");
    std::string suffix = relative->substr(relative->size() - 4);
    for (auto& c : suffix) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    return suffix == ".exe" ? true : fail("not a Windows program (.exe)");
  }
  if (id == "program_folder") {
    if (value.empty()) return true;
    return value.size() <= 1024 && steam::RelativeGamePath(value, true) ? true : fail("not a folder in the game folder");
  }
  if (id == "environment") {
    if (value.empty()) return true;
    std::size_t count = 0;
    std::set<std::string> names;
    std::size_t start = 0;
    while (start <= value.size()) {
      const auto end = std::min(value.find(';', start), value.size());
      auto entry = value.substr(start, end - start);
      start = end + 1;
      while (!entry.empty() && entry.front() == ' ') entry.remove_prefix(1);
      while (!entry.empty() && entry.back() == ' ') entry.remove_suffix(1);
      if (entry.empty()) continue;
      const auto equals = entry.find('=');
      if (equals == std::string_view::npos) return fail("each entry needs NAME=value");
      auto name = entry.substr(0, equals);
      auto item = entry.substr(equals + 1);
      while (!name.empty() && name.back() == ' ') name.remove_suffix(1);
      while (!item.empty() && item.front() == ' ') item.remove_prefix(1);
      if (!IsProsperoEnvironmentEntry(name, item) && !IsProsperoDebugEnvEntry(name, item) &&
          !IsBottleEnvironmentEntry(name, item))
        return fail(std::string(name) + " is not an allowed variable");
      if (!names.insert(std::string(name)).second) return fail(std::string(name) + " appears twice");
      if (++count > kProsperoMaxEnvironmentEntries) return fail("at most 8 variables");
    }
    return true;
  }
  if (id == "library_overrides" || id == "dll_overrides" || id == "registry_dll_overrides") {
    if (value.empty()) return true;
    return IsProsperoDllOverrides(value) ? true : fail("not a list like name=n;other=n,b");
  }
  if (id.substr(0, 7) == "button.") return IsProsperoBinding(value) ? true : fail("not a key, mouse button or none");
  return fail("unknown setting");
}

std::vector<std::string> SplitDllOverrides(const std::string& list) {
  std::vector<std::string> entries;
  std::size_t start = 0;
  while (start <= list.size()) {
    const auto end = list.find(';', start);
    auto entry = list.substr(start, end == std::string::npos ? std::string::npos : end - start);
    if (!entry.empty()) entries.push_back(std::move(entry));
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return entries;
}

bool IsGraphicsDllOverride(const GameRecipe& recipe, const std::string& entry) {
  return IsGeneratedRecipeId(recipe.id) && entry == kDxvkOverrides;
}

bool BuildEffectiveSettings(const GameRecipe& recipe, const SettingsPreset* preset,
                            const std::map<std::string, std::string>& overrides, const KitCapabilities& kit,
                            EffectiveSettings* settings, SettingsError* error) {
  const auto fail = [&](std::string id, std::string message) {
    if (error) *error = {std::move(id), std::move(message)};
    return false;
  };

  EffectiveSettings out;
  for (const auto& definition : SettingsCatalogue()) {
    EffectiveValue value;
    value.value = definition.id == "launch_option" ? DefaultLaunchEntry(recipe) : definition.playden_default;
    const auto layer = [&](const std::map<std::string, std::string>& source, ValueSource kind) {
      const auto found = source.find(definition.id);
      if (found == source.end()) return true;
      std::string reason;
      if (!ValidateSettingValue(recipe, definition.id, found->second, &reason)) return false;
      value.value = found->second;
      value.source = kind;
      return true;
    };
    if (!layer(recipe.defaults, ValueSource::kRecipe)) return fail(definition.id, "a default for this game is invalid");
    // Graphics comes from the recipe; only a recipe that locks it (Half-Life 2) keeps players from changing it.
    if (definition.id == "graphics") value.value = recipe.graphics, value.source = ValueSource::kRecipe;
    if (definition.id == "architecture") value.value = recipe.architecture, value.source = ValueSource::kRecipe;
    // A shipped recipe that says nothing about Steam features was proven with the game's own Steam
    // library (Half-Life 2), so that stays; one that names them (Final Fantasy XIII: emulation) is followed.
    value.locked = recipe.locked.count(definition.id) > 0;
    if (definition.id == "steam_features" && !IsGeneratedRecipeId(recipe.id) && !IsCommunityRecipeId(recipe.id) &&
        !recipe.defaults.count("steam_features"))
      value.value = "game", value.source = ValueSource::kRecipe, value.locked = true;
    // "Change anyway": the player's value wins over the recipe's lock; the row still says so.
    value.unlocked = value.locked && overrides.count(kUnlockPrefix + definition.id) > 0;
    if (value.locked && !value.unlocked) {
      value.reason = "Required by this game";
    } else {
      if (value.unlocked) value.reason = "Changed anyway: the game may not start.";
      if (preset && !layer(preset->settings, ValueSource::kPreset))
        return fail(definition.id, "preset value is invalid");
      // A player value that no longer validates (for example a launch entry the recipe
      // dropped) is ignored rather than blocking the launch.
      layer(overrides, ValueSource::kUser);
    }
    out.values[definition.id] = std::move(value);
  }
  out.graphics = out.values["graphics"].value;
  out.environment_as_debug_env = !kit.environment && kit.opengl_zink;
  out.debug_env_lines = kit.opengl_zink;
  out.profile_version = kit.profile_version_2 ? 2 : 1;
  out.display_120hz = kit.display_120hz;
  out.display_120hz_needs_update = kit.display_120hz_needs_update;

  // A controller preset (runtime/input/<name>.input) brings its own mode, pointer stick and speed. Rows set less
  // deliberately than the preset show the preset's values, so the sheet tells the truth and Pointer speed can be tuned.
  {
    const auto& preset_row = out.values["controller_preset"];
    const auto found = ControllerPresetInputs().find(preset_row.value);
    if (found != ControllerPresetInputs().end()) {
      const auto source = std::min(preset_row.source, ValueSource::kPreset);
      for (const auto& [id, preset_value] : found->second) {
        auto& row = out.values[id];
        if (row.locked || row.source >= preset_row.source) continue;
        row.value = preset_value;
        row.source = source;
      }
    }
  }

  // Availability depends on graphics, kit and other values; unavailable rows fall back to
  // the Playden default so a hidden setting never takes effect.
  for (const auto& definition : SettingsCatalogue()) {
    auto& value = out.values[definition.id];
    const auto reason = Unavailable(definition, out.graphics, kit, out.values);
    if (reason.empty()) continue;
    value.available = false;
    value.reason = reason;
    // A row the kit's runtime takes keeps the player's choice for the title's update; until then it is not applied.
    if (reason == kAppliesAfterKitUpdate) {
      value.waits_for_update = true;
      if (value.value != definition.playden_default) value.after_update = value.value;
    }
    value.value = definition.playden_default;
  }
  // A choice that needs other values (Frame limit 120 needs 120 Hz output) falls back to the default, and the row
  // says why. The refresh row comes after Frame limit in the catalogue, so this runs once every row is resolved.
  // A choice that waits for the title's update (Frame limit 120 on an older runtime) is kept for then, and the
  // default applies until the update.
  for (const char* id : {"frame_limit"}) {
    auto& value = out.values[id];
    const auto& fallback = FindSetting(id)->playden_default;
    if (!value.available) {
      if (value.after_update.empty()) continue;
      if (auto reason = SettingChoiceUnavailable(out, id, value.after_update); !reason.empty()) {
        value.after_update.clear();
        value.reason = std::move(reason);
      }
      continue;
    }
    if (auto reason = SettingChoiceUnavailable(out, id, value.value); !reason.empty()) {
      value.value = fallback;
      value.reason = std::move(reason);
    } else if (SettingChoiceWaitsForUpdate(out, id, value.value)) {
      value.after_update = value.value;
      value.value = fallback;
      value.reason = kAppliesAfterKitUpdate;
      value.waits_for_update = true;
    }
  }

  // Pixel shaders: only where Environment variables can carry it; empty leaves the driver's default.
  if (const auto& waves = out.values["pixel_shader_waves"]; waves.available) out.pixel_shader_waves = waves.value;

  // The program: the chosen launch entry, or a program the player picked in the game folder.
  // The player's folder and arguments apply to either.
  std::optional<LaunchEntry> launch;
  auto& launch_row = out.values["launch_option"];
  if (const auto* entry = FindEntry(recipe, launch_row.value)) launch = *entry;
  auto& architecture_row = out.values["architecture"];
  const auto& program = out.values["program"];
  if (!program.value.empty()) {
    const auto same = [&](const LaunchEntry& entry) {
      const auto listed = steam::RelativeGamePath(entry.executable);
      return listed && LowerAscii(*listed) == LowerAscii(*steam::RelativeGamePath(program.value));
    };
    // A recipe generated for this program already knows its architecture from the file.
    const LaunchEntry* listed = nullptr;
    for (const auto& entry : recipe.launch_entries)
      if (entry.id == kProgramEntryId && same(entry)) listed = &entry;
    if (listed) {
      launch = *listed;
    } else {
      LaunchEntry entry;
      entry.id = kProgramEntryId;
      entry.executable = *steam::RelativeGamePath(program.value);
      const auto slash = entry.executable.rfind('/');
      entry.label = slash == std::string::npos ? entry.executable : entry.executable.substr(slash + 1);
      launch = std::move(entry);
    }
    launch_row.value = kProgramEntryId;
    launch_row.source = program.source;
    if (listed) architecture_row.value = recipe.architecture, architecture_row.source = ValueSource::kRecipe;
  } else if (architecture_row.source == ValueSource::kUser) {
    // The architecture follows the program; without a program of the player's it is the recipe's.
    architecture_row.value = recipe.architecture;
    architecture_row.source = ValueSource::kRecipe;
  }
  out.architecture = architecture_row.value;
  auto& folder = out.values["program_folder"];
  auto& own_arguments = out.values["program_arguments"];
  if (launch) {
    if (folder.source == ValueSource::kUser) {
      // "." is the game folder itself; empty is the program's own folder.
      const auto relative = steam::RelativeGamePath(folder.value, true);
      launch->working_directory = !relative || *relative == "." ? std::string() : *relative;
      launch->in_install_root = relative && *relative == "." && !folder.value.empty();
    } else {
      folder.value = launch->working_directory.empty() && launch->in_install_root ? "." : launch->working_directory;
      folder.source = folder.value.empty() ? ValueSource::kPlayden : ValueSource::kRecipe;
    }
    if (own_arguments.source == ValueSource::kUser) {
      launch->arguments = own_arguments.value;
    } else {
      own_arguments.value = launch->arguments;
      own_arguments.source = launch->arguments.empty() ? ValueSource::kPlayden : ValueSource::kRecipe;
    }
    out.launch_storage = std::make_shared<const LaunchEntry>(std::move(*launch));
    out.launch = out.launch_storage.get();
  } else if (!launch_row.value.empty()) {
    return fail("launch_option", "launch option not found");
  }

  // Required arguments: the recipe's, unless the player changed them.
  auto& required = out.values["arguments"];
  if (required.source != ValueSource::kUser) {
    required.value = recipe.arguments;
    required.source = recipe.arguments.empty() ? ValueSource::kPlayden : ValueSource::kRecipe;
  }
  // The row shows what the game is started with, so these swaps rewrite the recipe's value itself.
  // Godot 4 cannot open its OpenGL window under Mesa's WGL/Zink on the PS5 (Slay the Spire 2), but its Vulkan
  // renderer runs on RADV directly, at full rate. Generated recipes for Godot ask for opengl3; on a Zink kit they
  // get Vulkan instead, also in arguments the player typed. A player's Extra arguments still come last and win.
  if (kit.opengl_zink && recipe.detected.rfind("godot", 0) == 0)
    ReplaceAll(&required.value, "--rendering-driver opengl3", "--rendering-driver vulkan");
  // Generated recipes ask Unity and Unreal for Direct3D 11, which DXVK draws. With Direct3D 12 on vkd3d-proton they
  // ask for Direct3D 12 instead (CULTIC stays black on 11). Arguments the player typed are left as they are.
  if (IsGeneratedRecipeId(recipe.id) && required.source != ValueSource::kUser &&
      out.values["d3d12"].value == "vkd3d-proton") {
    ReplaceAll(&required.value, "-force-d3d11", "-force-d3d12");
    ReplaceAll(&required.value, "-dx11", "-dx12");
  }
  std::string arguments = required.value;
  std::string resolution_arguments = recipe.resolution_arguments;
  const auto& resolution = out.values["resolution"].value;
  const auto x = resolution.find('x');
  ReplaceAll(&resolution_arguments, "{w}", resolution.substr(0, x));
  ReplaceAll(&resolution_arguments, "{h}", x == std::string::npos ? "" : resolution.substr(x + 1));
  ReplaceAll(&arguments, "{resolution}", resolution_arguments);
  std::string launch_arguments = out.launch ? out.launch->arguments : std::string();
  // A generated recipe picks the graphics API in Required arguments; a Steam launch option that picks one too is
  // dropped from it, or the engine stops: Severed Steel's "-noraytracing -d3d12" next to "-dx11" ends in "-d3d12,
  // -d3d11, -vulkan, and -opengl are mutually exclusive". Required arguments stay the one place to change it.
  if (IsGeneratedRecipeId(recipe.id) && !launch_arguments.empty()) {
    static const std::vector<std::vector<std::string>> api_words = {
        {"-d3d11", "-dx11", "-d3d12", "-dx12", "-d3d10", "-dx10", "-vulkan", "-opengl", "-opengl4"},
        {"-force-d3d9", "-force-d3d11", "-force-d3d12", "-force-vulkan", "-force-glcore", "-force-gles",
         "-force-d3d11-no-singlethreaded"}};
    const auto lower = [](std::string word) {
      std::transform(word.begin(), word.end(), word.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      return word;
    };
    // Words split at spaces outside double quotes, so a quoted value stays one word and is never matched.
    const auto words_of = [&](const std::string& text) {
      std::vector<std::string> words;
      std::string word;
      bool quoted = false;
      for (const char c : text) {
        if (c == '"') quoted = !quoted;
        if (c == ' ' && !quoted) {
          if (!word.empty()) words.push_back(std::move(word));
          word.clear();
        } else {
          word += c;
        }
      }
      if (!word.empty()) words.push_back(std::move(word));
      return words;
    };
    const auto required_words = words_of(required.value);
    for (const auto& family : api_words) {
      const auto in_family = [&](const std::string& word) {
        return std::find(family.begin(), family.end(), lower(word)) != family.end();
      };
      if (std::none_of(required_words.begin(), required_words.end(), in_family)) continue;
      std::string kept;
      for (const auto& word : words_of(launch_arguments))
        if (!in_family(word)) kept += (kept.empty() ? "" : " ") + word;
      launch_arguments = kept;
    }
  }
  if (!launch_arguments.empty()) arguments += " " + launch_arguments;
  const auto& extra = out.values["extra_arguments"].value;
  if (!extra.empty()) arguments += " " + extra;
  out.arguments = CollapseSpaces(arguments);
  if (out.arguments.size() > kProsperoMaxArgumentBytes || !SplitProsperoArguments(out.arguments, nullptr))
    return fail("extra_arguments", "arguments are too long or have too many words");

  // Library overrides: the recipe's, then the DXVK ones a generated recipe needs, then what an
  // earlier build let the player add; or the player's whole list.
  auto& overrides_row = out.values["dll_overrides"];
  if (overrides_row.source != ValueSource::kUser) {
    std::string list = recipe.dll_overrides;
    const auto append = [&](const std::string& more) {
      if (!more.empty()) list = list.empty() ? more : list + ";" + more;
    };
    // A generated recipe reaches the bottle's DXVK through overrides that follow the graphics value,
    // so a player who changes graphics never keeps DXVK half on.
    if (IsGeneratedRecipeId(recipe.id) && out.graphics == "dxvk") list = list.empty() ? std::string(kDxvkOverrides)
                                                                                     : kDxvkOverrides + (";" + list);
    append(out.values["library_overrides"].value);
    overrides_row.value = list;
    overrides_row.source = list.empty() ? ValueSource::kPlayden
                         : out.values["library_overrides"].source == ValueSource::kUser ? ValueSource::kUser
                                                                                         : ValueSource::kRecipe;
  } else if (IsGeneratedRecipeId(recipe.id)) {
    // The player's list never holds the DXVK entry: it follows graphics here as well, so a list
    // saved while DXVK was on does not keep it after the player switches graphics.
    std::string list;
    for (const auto& entry : SplitDllOverrides(overrides_row.value))
      if (entry != kDxvkOverrides) list += (list.empty() ? "" : ";") + entry;
    if (out.graphics == "dxvk") list = list.empty() ? std::string(kDxvkOverrides) : kDxvkOverrides + (";" + list);
    overrides_row.value = list;
  }
  // vkd3d-proton's entry follows the Direct3D 12 row the same way, whoever wrote the list.
  out.vkd3d_proton = out.values["d3d12"].value == "vkd3d-proton";
  // It also brings DXVK's dxgi, which it presents through, when no other entry names dxgi.
  if (out.vkd3d_proton || overrides_row.value.find(kVkd3dProtonOverrides) != std::string::npos ||
      overrides_row.value.find(kVkd3dProtonDxgiOverrides) != std::string::npos) {
    std::string list;
    bool dxgi = false;
    for (const auto& entry : SplitDllOverrides(overrides_row.value)) {
      if (entry == kVkd3dProtonOverrides || entry == kVkd3dProtonDxgiOverrides) continue;
      list += (list.empty() ? "" : ";") + entry;
      auto names = "," + entry.substr(0, entry.find('=')) + ",";
      for (auto& c : names) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
      dxgi = dxgi || names.find(",dxgi,") != std::string::npos;
    }
    if (out.vkd3d_proton)
      list += (list.empty() ? "" : ";") + std::string(dxgi ? kVkd3dProtonOverrides : kVkd3dProtonDxgiOverrides);
    overrides_row.value = list;
  }
  out.dll_overrides = overrides_row.value;
  if (!out.dll_overrides.empty() && !IsProsperoDllOverrides(out.dll_overrides))
    return fail("dll_overrides", "library overrides are too long");

  *settings = std::move(out);
  return true;
}

bool RenderProsperoProfile(const GameRecipe& recipe, const EffectiveSettings& settings, const ProfileTarget& target,
                           ProsperoProfile* profile, std::string* reason) {
  const auto fail = [&](std::string why) {
    if (reason) *reason = std::move(why);
    return false;
  };
  if (!settings.launch) return fail("no launch option");
  const auto root = ProsperoWindowsPath(target.install_root);
  if (!root) return fail("install folder cannot be mapped for Wine");
  const auto executable = steam::RelativeGamePath(settings.launch->executable);
  if (!executable) return fail("launch executable is not a game path");
  const auto folder = LaunchWorkingFolder(*settings.launch);
  if (!folder) return fail("launch working folder is not a game path");
  const auto& working = *folder;
  const auto value = [&](const char* id) -> const std::string& { return settings.values.at(id).value; };
  const auto on = [&](const char* id) { return value(id) == "on" ? "true" : "false"; };

  std::string name = recipe.name.empty() ? target.id : recipe.name;
  if (name.size() > kProsperoMaxNameBytes) name.resize(kProsperoMaxNameBytes);

  std::string text = "; Generated by Playden from recipe " + recipe.id + " v" + std::to_string(recipe.version) +
                     ". Edit settings in Playden.\n[application]\n";
  text += "id = " + target.id + "\n";
  text += "name = " + name + "\n";
  text += "executable = " + *root + "\\" + Backslashes(*executable) + "\n";
  text += "working_directory = " + (working.empty() ? *root : *root + "\\" + Backslashes(working)) + "\n";
  if (!settings.arguments.empty()) text += "arguments = " + settings.arguments + "\n";
  if (!settings.dll_overrides.empty()) text += "dll_overrides = " + settings.dll_overrides + "\n";
  text += "prefix = " + recipe.prefix + "\n";
  text += "runtime = " + recipe.runtime + "\n";
  text += "architecture = " + (settings.architecture.empty() ? recipe.architecture : settings.architecture) + "\n";
  text += "graphics = " + settings.graphics + "\n";

  text += "\n[display]\n";
  text += "desktop = " + value("resolution") + "\n";
  if (settings.values.at("scaling").available) text += "scaling = " + value("scaling") + "\n";
  text += "view = " + value("window_view") + "\n";
  // With a kit that reads dxvk.conf, a DXVK game's overlay is dxvk.hud there; the title's
  // own DXVK_HUD=fps would override it, so show_fps stays false.
  const bool hud_in_config = settings.values.at("overlay_detail").available;
  text += std::string("show_fps = ") + (hud_in_config ? "false" : on("performance_overlay")) + "\n";
  // An OpenGL game always names its rate, as before (60 unless the runtime takes 120 Hz). opengl_thread is no longer
  // written: it was the PS5 OpenGL SDK's, and runtimes default it to off.
  if (settings.values.at("refresh").available || settings.graphics == "opengl")
    text += "refresh = " + value("refresh") + "\n";
  // Only a cap the display paces, for a title whose runtime takes it: an older runtime refuses the whole profile for
  // this key. No cap is the runtime's default (0), so uncapped profiles stay readable by every runtime; 30 at 120 Hz
  // output is left to the software limiters (RenderDxvkConfig, RenderBottleEnvironment).
  const auto& limit = OptionalValue(settings, "frame_limit");
  const bool frame_rate =
      settings.display_120hz && limit.available && limit.value != "off" && SoftwareFrameLimit(settings).empty();
  if (frame_rate) text += "frame_rate = " + limit.value + "\n";

  text += "\n[input]\n";
  if (value("controller_preset") != "none") text += "preset = " + value("controller_preset") + "\n";
  // A controller preset brings its own mode, pointer stick and speed. A row overrides them only when it was
  // set at least as deliberately as the preset (player over settings preset over recipe over Playden).
  const bool preset = value("controller_preset") != "none";
  const auto preset_source = settings.values.at("controller_preset").source;
  const auto chosen = [&](const char* id) {
    const auto source = settings.values.at(id).source;
    return !preset || (source != ValueSource::kPlayden && source >= preset_source);
  };
  if (chosen("controller")) text += "mode = " + value("controller") + "\n";
  if (chosen("pointer_stick")) text += "mouse = " + value("pointer_stick") + "\n";
  if (settings.values.at("pointer_speed").available && chosen("pointer_speed"))
    text += "mouse_speed = " + value("pointer_speed") + "\n";
  // Written only when they differ from the runtime's defaults (no dead zone, full rumble).
  const bool tuned_deadzone = settings.values.at("stick_deadzone").available && value("stick_deadzone") != "0";
  const bool tuned_rumble = settings.values.at("rumble").available && value("rumble") != "100";
  const bool tuned_triggers = settings.values.at("trigger_deadzone").available && value("trigger_deadzone") != "0";
  const bool wheel = settings.values.at("wheel_stick").available && value("wheel_stick") != "none";
  if (tuned_deadzone) text += "stick_deadzone = " + value("stick_deadzone") + "\n";
  if (tuned_rumble) text += "rumble = " + value("rumble") + "\n";
  if (tuned_triggers) text += "trigger_deadzone = " + value("trigger_deadzone") + "\n";
  if (wheel) text += "wheel = " + value("wheel_stick") + "\n";
  for (const auto& button : ProsperoButtons()) {
    const auto& binding = settings.values.at("button." + button).value;
    if (!binding.empty()) text += button + " = " + binding + "\n";
  }

  const bool audio_section = settings.values.at("volume").available && value("volume") != "100";
  if (audio_section) text += "\n[audio]\nvolume = " + value("volume") + "\n";

  // [runtime] version 2: the title takes the driver's defaults. Without the line the profile is version 1.
  if (settings.profile_version == 2) text += "\n[runtime]\nversion = 2\n";
  if (value("logging") == "verbose") text += std::string("\n[debug]\nwinedebug = ") + kVerboseWinedebug + "\n";
  if (value("logging") == "input") text += std::string("\n[debug]\nwinedebug = ") + kInputWinedebug + "\n";

  const auto& environment = settings.values.at("environment");
  // The player's variables, with the one Pixel shaders sets (none for the driver's default).
  std::string environment_entries;
  if (environment.available) {
    std::string why;
    if (!WithPixelShaderEnvironment(environment.value, settings.pixel_shader_waves, settings.profile_version == 2,
                                    settings.environment_as_debug_env, &environment_entries, &why))
      return fail(why);
  }
  bool debug_env = false;
  if (environment.available && !environment_entries.empty() && settings.debug_env_lines) {
    // [debug] env = NAME=value, one line per variable; names the runtime does not take are left out, and so are
    // names that go to [environment] on a kit that reads it.
    std::string lines;
    std::size_t start = 0, count = 0;
    const auto& entries = environment_entries;
    while (start <= entries.size()) {
      const auto end = std::min(entries.find(';', start), entries.size());
      std::string entry = entries.substr(start, end - start);
      start = end + 1;
      const auto equals = entry.find('=');
      if (equals == std::string::npos) continue;
      std::string name = entry.substr(0, equals), item = entry.substr(equals + 1);
      while (!name.empty() && name.front() == ' ') name.erase(0, 1);
      while (!name.empty() && name.back() == ' ') name.pop_back();
      while (!item.empty() && item.front() == ' ') item.erase(0, 1);
      while (!item.empty() && item.back() == ' ') item.pop_back();
      if (!settings.environment_as_debug_env && IsProsperoEnvironmentEntry(name, item)) continue;
      if (name == "DXVK_FRAME_RATE" && FrameLimitOwnsCap(settings)) continue;
      if (!IsProsperoDebugEnvEntry(name, item) || count == kProsperoMaxDebugEnvEntries) continue;
      lines += "env = " + name + "=" + item + "\n";
      ++count;
    }
    if (!lines.empty()) {
      const bool open = text.find("\n[debug]\n") != std::string::npos;
      text += (open ? "" : "\n[debug]\n") + lines;
      debug_env = true;
    }
  }
  // CPU cores: WINE_CPU_TOPOLOGY in [environment], unless Environment variables set it already.
  const auto& cores = OptionalValue(settings, "cpu_topology");
  std::string topology;
  if (cores.available && cores.value != "kit" &&
      (!environment.available || environment.value.find("WINE_CPU_TOPOLOGY") == std::string::npos)) {
    const int count = std::atoi(cores.value.c_str());
    topology = std::to_string(count) + ":";
    for (int core = 0; core < count; ++core) topology += (core ? "," : "") + std::to_string(core);
  }
  // [environment]: CPU cores first, then the Environment variables it takes (the rest went to [debug] env above).
  std::string environment_lines;
  std::size_t environment_count = 0;
  if (!topology.empty()) {
    environment_lines += "WINE_CPU_TOPOLOGY = " + topology + "\n";
    ++environment_count;
  }
  if (environment.available && !settings.environment_as_debug_env) {
    std::size_t start = 0;
    const auto& entries = environment_entries;
    while (!entries.empty() && start <= entries.size()) {
      const auto end = std::min(entries.find(';', start), entries.size());
      std::string entry = entries.substr(start, end - start);
      start = end + 1;
      const auto equals = entry.find('=');
      if (equals == std::string::npos) continue;
      std::string name = entry.substr(0, equals), item = entry.substr(equals + 1);
      while (!name.empty() && name.front() == ' ') name.erase(0, 1);
      while (!name.empty() && name.back() == ' ') name.pop_back();
      while (!item.empty() && item.front() == ' ') item.erase(0, 1);
      while (!item.empty() && item.back() == ' ') item.pop_back();
      if (name == "DXVK_FRAME_RATE" && FrameLimitOwnsCap(settings)) continue;
      if (!IsProsperoEnvironmentEntry(name, item) || environment_count == kProsperoMaxEnvironmentEntries) continue;
      environment_lines += name + " = " + item + "\n";
      ++environment_count;
    }
  }
  const bool environment_section = !environment_lines.empty();
  if (environment_section) text += "\n[environment]\n" + environment_lines;

  std::string why;
  ProsperoProfileFeatures features;
  features.environment_section = environment_section;
  features.debug_env = debug_env;
  features.input_tuning = tuned_deadzone || tuned_rumble || tuned_triggers;
  features.input_wheel = wheel;
  features.audio_section = audio_section;
  features.display_frame_rate = frame_rate;
  features.runtime_version = settings.profile_version == 2;
  if (!CheckProsperoProfile(text, &why, features)) return fail("generated profile is invalid: " + why);
  *profile = {target.id, std::move(text)};
  return true;
}

constexpr const char* kDxvkHeader = "# Generated by Playden from this game's settings.";

std::string RenderDxvkConfig(const EffectiveSettings& settings) {
  const auto option = [&](const char* id) -> const EffectiveValue& { return settings.values.at(id); };
  std::string text;
  if (const auto limit = SoftwareFrameLimit(settings); !limit.empty())
    text += "dxgi.maxFrameRate = " + limit + "\nd3d9.maxFrameRate = " + limit + "\n";
  const auto& vsync = option("vsync");
  if (vsync.available && vsync.value != "game") {
    const char* interval = vsync.value == "on" ? "1" : "0";
    text += std::string("dxgi.syncInterval = ") + interval + "\nd3d9.presentInterval = " + interval + "\n";
  }
  const auto& latency = option("frame_latency");
  if (latency.available && latency.value != "game")
    text += "dxgi.maxFrameLatency = " + latency.value + "\nd3d9.maxFrameLatency = " + latency.value + "\n";
  const auto& detail = option("overlay_detail");
  if (detail.available) {
    const char* elements = detail.value == "full" ? "fps,frametimes,memory,gpuload,version"
                           : detail.value == "frametimes" ? "fps,frametimes" : "fps";
    text += std::string("dxvk.hud = ") + elements + "\n";
  }
  const auto& gpu = OptionalValue(settings, "reported_gpu");
  if (gpu.available && gpu.value != "game") {
    const char* vendor = gpu.value == "amd" ? "1002" : gpu.value == "nvidia" ? "10de" : "8086";
    text += std::string("dxgi.customVendorId = ") + vendor + "\nd3d9.customVendorId = " + vendor + "\n";
  }
  const auto& vram = option("reported_vram");
  if (vram.available && vram.value != "game")
    text += "dxgi.maxDeviceMemory = " + vram.value + "\nd3d9.maxAvailableMemory = " + vram.value + "\n";
  if (text.empty()) return text;
  return std::string(kDxvkHeader) + " Changes here are replaced on the next launch.\n" + text;
}

namespace {

bool StringField(const json::Value& object, const char* key, std::string* out, bool required = true) {
  const auto* field = object.get(key);
  if (!field) return !required;
  const auto* text = field->string();
  if (!text) return false;
  *out = *text;
  return true;
}

bool StringMap(const json::Value* value, std::map<std::string, std::string>* out) {
  if (!value) return true;
  const auto* object = value->object();
  if (!object) return false;
  for (const auto& [key, item] : *object) {
    const auto* text = item.string();
    if (!text) return false;
    (*out)[key] = *text;
  }
  return true;
}

bool ExactUnsigned(const json::Value* value, std::uint64_t maximum, std::uint64_t* out, bool allow_zero = true) {
  const auto number = value ? value->number() : std::optional<double>{};
  if (!number || !std::isfinite(*number) || *number < 0 || (!allow_zero && *number == 0) ||
      *number > static_cast<double>(maximum) || std::floor(*number) != *number)
    return false;
  *out = static_cast<std::uint64_t>(*number);
  return true;
}

constexpr std::size_t kMaxRecipeRegistryValues = 16;

// A registry value a recipe may carry: a key under HKEY_CURRENT_USER as Wine spells it, a
// printable ASCII name and string, and never one of the two values the title publication
// owns (settings_publication.cpp), which the next publication would put back. Only two kinds
// of key (docs/community-recipes.md): the program's own Wine settings, a section under
// Software\Wine\AppDefaults\<program> other than DllOverrides (the recipe's dll_overrides
// lists those where Game settings shows them), and the game's own keys, Software\<publisher>.
// Wine's global keys, Windows' keys and Environment would get around what the kit checks
// (Drivers\Audio), what Game settings shows (overrides) and the [environment] allow-list.
bool RecipeRegistryValueAllowed(const std::string& key, const std::string& name, const std::string& value,
                                std::string* why) {
  const auto printable = [](const std::string& text, bool backslashes) {
    for (const unsigned char c : text)
      if (c < 0x20 || c > 0x7e || c == '"' || (c == '\\' && !backslashes)) return false;
    return true;
  };
  const auto lower = [](std::string text) {
    for (auto& c : text) c = c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    return text;
  };
  // Brackets would end the key's [section] header in user.reg early, so Wine would read another key.
  if (key.empty() || key.size() > 200 || !printable(key, true) || key.front() == '\\' || key.back() == '\\' ||
      key.find("\\\\") != std::string::npos || key.find_first_of("[]") != std::string::npos) {
    *why = "key is not a registry path";
    return false;
  }
  const auto folded = lower(key);
  if (folded.rfind("hkey_", 0) == 0 || folded.rfind("hkcu", 0) == 0 || folded.rfind("hklm", 0) == 0) {
    *why = "keys are relative to HKEY_CURRENT_USER";
    return false;
  }
  if (name.empty() || name.size() > 64 || !printable(name, false)) {
    *why = "value name is not printable ASCII";
    return false;
  }
  if (value.size() > 256 || !printable(value, false)) {
    *why = "value is not a printable ASCII string";
    return false;
  }
  static constexpr std::string_view kAppDefaults = "software\\wine\\appdefaults\\";
  const bool version = folded.rfind(kAppDefaults, 0) == 0 &&
                       folded.find('\\', kAppDefaults.size()) == std::string::npos && lower(name) == "version";
  const bool log_pixels = folded == "control panel\\desktop" && lower(name) == "logpixels";
  if (version || log_pixels) {
    *why = "Game settings own this value";
    return false;
  }
  std::vector<std::string> parts;
  for (std::size_t start = 0; start <= folded.size();) {
    const auto end = std::min(folded.find('\\', start), folded.size());
    parts.push_back(folded.substr(start, end - start));
    start = end + 1;
  }
  if (parts.size() >= 4 && parts[0] == "software" && parts[1] == "wine" && parts[2] == "appdefaults") {
    if (parts.size() >= 5 && parts[4] != "dlloverrides") return true;
    *why = parts.size() >= 5 ? "library overrides belong in dll_overrides"
                             : "use a section of the program's key, like " + key + "\\Direct3D";
    return false;
  }
  static const std::set<std::string> kNotTheGames = {"wine", "microsoft", "classes", "policies", "wow6432node"};
  if (parts.size() >= 2 && parts[0] == "software" && !kNotTheGames.count(parts[1])) return true;
  *why = "only Software\\Wine\\AppDefaults\\<program>\\<section> and the game's own Software\\<publisher> keys";
  return false;
}

}  // namespace

bool ParseGameRecipe(std::string_view text, GameRecipe* recipe, std::string* reason) {
  const auto fail = [&](std::string why) {
    if (reason) *reason = std::move(why);
    return false;
  };
  const auto root = json::parse(text);
  if (!root || !root->object()) return fail("recipe is not a JSON object");
  GameRecipe out;
  const auto* version = root->get("recipe_version");
  const auto* app = root->get("app_id");
  std::uint64_t parsed_version = 0;
  std::uint64_t parsed_app = 0;
  if (!StringField(*root, "recipe_id", &out.id) ||
      !ExactUnsigned(version, static_cast<std::uint64_t>(std::numeric_limits<int>::max()), &parsed_version, false) ||
      !ExactUnsigned(app, std::numeric_limits<std::uint32_t>::max(), &parsed_app, false))
    return fail("recipe needs recipe_id, recipe_version and app_id");
  out.version = static_cast<int>(parsed_version);
  out.app_id = static_cast<std::uint32_t>(parsed_app);
  // Games from other stores name their store and game; absent, the recipe is for Steam's app_id.
  const auto* store = root->get("store");
  const auto* game = root->get("game");
  if ((store == nullptr) != (game == nullptr)) return fail("recipe needs both store and game, or neither");
  if (store) {
    const auto parsed_store = store->string() ? ParseStoreKey(*store->string()) : std::nullopt;
    if (!parsed_store || !game->string()) return fail("recipe names an unknown store");
    out.game = GameId{*parsed_store, *game->string()};
    if (!out.game.valid()) return fail("recipe names an invalid game");
  }
  if (!StringField(*root, "name", &out.name, false) || !StringField(*root, "arguments", &out.arguments, false) ||
      !StringField(*root, "resolution_arguments", &out.resolution_arguments, false) ||
      !StringField(*root, "dll_overrides", &out.dll_overrides, false) ||
      !StringField(*root, "architecture", &out.architecture, false) ||
      !StringField(*root, "runtime", &out.runtime, false) || !StringField(*root, "prefix", &out.prefix, false) ||
      !StringField(*root, "graphics", &out.graphics, false) ||
      !StringField(*root, "detected", &out.detected, false))
    return fail("recipe has a field of the wrong type");
  const auto* entries = root->get("launch_entries");
  if (!entries || !entries->array() || entries->array()->empty()) return fail("recipe needs launch_entries");
  for (const auto& item : *entries->array()) {
    LaunchEntry entry;
    if (!item.object() || !StringField(item, "id", &entry.id) || !StringField(item, "label", &entry.label) ||
        !StringField(item, "executable", &entry.executable) ||
        !StringField(item, "working_directory", &entry.working_directory, false) ||
        !StringField(item, "arguments", &entry.arguments, false))
      return fail("launch entry needs id, label and executable");
    if (const auto* root_folder = item.get("in_install_root")) {
      const auto value = root_folder->boolean();
      if (!value) return fail("launch entry in_install_root must be true or false");
      entry.in_install_root = *value && entry.working_directory.empty();
    }
    if (HasControl(entry.arguments)) return fail("launch entry arguments contain a control character");
    out.launch_entries.push_back(std::move(entry));
  }
  if (!StringMap(root->get("defaults"), &out.defaults)) return fail("defaults must map setting ids to strings");
  for (auto it = out.defaults.begin(); it != out.defaults.end();) it = RetiredSetting(it->first) ? out.defaults.erase(it) : std::next(it);
  if (!StringMap(root->get("requirements"), &out.requirements)) return fail("requirements must map names to strings");
  if (const auto* locked = root->get("locked")) {
    if (!locked->array()) return fail("locked must be a list");
    for (const auto& item : *locked->array()) {
      if (item.string() && RetiredSetting(*item.string())) continue;
      if (!item.string() || !FindSetting(*item.string())) return fail("locked names an unknown setting");
      out.locked.insert(*item.string());
    }
  }
  if (const auto* registry = root->get("registry")) {
    const auto* keys = registry->object();
    if (!keys) return fail("registry must map keys to their values");
    // Registry names ignore case, and the bottle refuses the same value twice.
    std::set<std::string> identities;
    for (const auto& [key, names] : *keys) {
      const auto* list = names.object();
      if (!list) return fail("registry key " + key + " must map value names to strings");
      for (const auto& [name, value] : *list) {
        const auto* string = value.string();
        std::string why;
        if (!string) return fail("registry value " + name + " must be a string");
        if (!RecipeRegistryValueAllowed(key, name, *string, &why)) return fail("registry " + key + " " + name + ": " + why);
        if (out.registry.size() >= kMaxRecipeRegistryValues) return fail("registry has too many values");
        if (!identities.insert(LowerAscii(key + "\n" + name)).second) return fail("registry " + key + " " + name + " appears twice");
        out.registry.push_back({key, name, "\"" + *string + "\""});
      }
    }
  }
  if (out.dll_overrides.size() && !IsProsperoDllOverrides(out.dll_overrides)) return fail("recipe dll_overrides are invalid");
  if (out.architecture != "pe32" && out.architecture != "pe64") return fail("architecture must be pe32 or pe64");
  for (const auto& [id, value] : out.defaults) {
    std::string why;
    if (!ValidateSettingValue(out, id, value, &why)) return fail("default " + id + ": " + why);
  }
  if (!ValidateSettingValue(out, "graphics", out.graphics, nullptr)) return fail("unknown graphics mode");
  if (HasControl(out.arguments) || HasControl(out.resolution_arguments)) return fail("arguments contain a control character");
  *recipe = std::move(out);
  return true;
}

bool ParseSettingsPresets(std::string_view text, std::vector<SettingsPreset>* presets, std::string* reason) {
  const auto fail = [&](std::string why) {
    if (reason) *reason = std::move(why);
    return false;
  };
  const auto root = json::parse(text);
  const auto* list = root ? root->get("presets") : nullptr;
  if (!list || !list->array()) return fail("presets file needs a presets list");
  std::vector<SettingsPreset> out;
  for (const auto& item : *list->array()) {
    SettingsPreset preset;
    if (!item.object() || !StringField(item, "id", &preset.id) || !StringField(item, "name", &preset.name) ||
        !StringField(item, "try_when", &preset.try_when, false) ||
        !StringField(item, "short_hint", &preset.short_hint, false) || !StringMap(item.get("settings"), &preset.settings))
      return fail("preset needs id, name and string settings");
    for (auto it = preset.settings.begin(); it != preset.settings.end();) it = RetiredSetting(it->first) ? preset.settings.erase(it) : std::next(it);
    for (const auto& [id, value] : preset.settings) {
      // Presets bundle choices and button bindings; launch entries, graphics and free text
      // belong to the recipe or the player.
      const auto* definition = FindSetting(id);
      const bool button = id.compare(0, 7, "button.") == 0;
      if (!definition || id == "launch_option" || id == "graphics" || id == "architecture" || id == "start_check" ||
          (definition->kind == SettingKind::kText && !button))
        return fail("preset " + preset.id + " sets unsupported setting " + id);
      std::string why;
      if (!ValidateSettingValue(GameRecipe{}, id, value, &why)) return fail("preset " + preset.id + " " + id + ": " + why);
    }
    out.push_back(std::move(preset));
  }
  *presets = std::move(out);
  return true;
}

namespace {

bool RecordFromValue(const storage_json::Value& root, GameSettingsRecord* out) {
  if (!root.is_object()) return false;
  if (const auto* preset = root.Find("preset")) {
    if (!preset->is_string()) return false;
    out->preset_id = preset->text();
  }
  if (const auto* overrides = root.Find("overrides")) {
    if (!overrides->is_object()) return false;
    for (const auto& [key, item] : overrides->object()) {
      if (!item.is_string()) return false;
      out->overrides[key] = item.text();
    }
  }
  if (const auto* revision = root.Find("revision")) {
    const auto parsed = revision->AsUint64();
    if (!parsed) return false;
    out->revision = *parsed;
  }
  // A malformed app ID is dropped, not the record: the rest of the player's settings still load.
  if (const auto* app = root.Find("steam_app_id"); app && app->is_string())
    out->steam_app_id = ParseSteamAppId(app->text()).value_or(0);
  if (const auto* launch = root.Find("steam_launch"); launch && launch->is_string())
    out->steam_launch = steam::RelativeGamePath(launch->text()).value_or(std::string());
  // Pixel shaders "auto" (Automatic) is gone; such a record takes the default, wave32.
  if (const auto waves = out->overrides.find("pixel_shader_waves");
      waves != out->overrides.end() && waves->second == "auto")
    out->overrides.erase(waves);
  // Settings this build does not know are dropped so an older record still loads.
  for (auto it = out->overrides.begin(); it != out->overrides.end();)
    it = FindSetting(it->first) || IsUnlockKey(it->first) ? std::next(it) : out->overrides.erase(it);
  return true;
}

}  // namespace

std::optional<std::uint32_t> ParseSteamAppId(std::string_view text) {
  const auto space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
  while (!text.empty() && space(text.front())) text.remove_prefix(1);
  while (!text.empty() && space(text.back())) text.remove_suffix(1);
  return GameId{Store::kSteam, std::string(text)}.steam_app();
}

bool ParseGameSettingsRecord(std::string_view text, GameSettingsRecord* record, std::string* reason) {
  const auto root = storage_json::Parse(text);
  GameSettingsRecord out;
  if (!root || !RecordFromValue(*root, &out)) {
    if (reason) *reason = "settings record is malformed";
    return false;
  }
  *record = std::move(out);
  return true;
}

std::string SerializeGameSettingsRecord(const GameSettingsRecord& record) {
  std::string text = "{\"preset\":\"" + json::escape(record.preset_id) + "\",\"revision\":" +
                     std::to_string(record.revision) + ",\"overrides\":{";
  bool first = true;
  for (const auto& [id, value] : record.overrides) {
    if (!first) text += ",";
    first = false;
    text += "\"" + json::escape(id) + "\":\"" + json::escape(value) + "\"";
  }
  text += "}";
  if (record.steam_app_id) text += ",\"steam_app_id\":\"" + std::to_string(record.steam_app_id) + "\"";
  if (!record.steam_launch.empty()) text += ",\"steam_launch\":\"" + json::escape(record.steam_launch) + "\"";
  return text + "}";
}

std::string SerializeGameRecipe(const GameRecipe& recipe) {
  const auto quoted = [](const std::string& text) { return "\"" + json::escape(text) + "\""; };
  const auto game = RecipeGame(recipe);
  std::string text = "{\"recipe_id\":" + quoted(recipe.id) + ",\"recipe_version\":" + std::to_string(recipe.version) +
                     ",\"app_id\":" + std::to_string(recipe.app_id) +
                     (game.store == Store::kSteam ? std::string()
                                                  : ",\"store\":" + quoted(std::string(Traits(game.store).key)) +
                                                        ",\"game\":" + quoted(game.value)) +
                     ",\"name\":" + quoted(recipe.name) +
                     ",\"launch_entries\":[";
  for (std::size_t index = 0; index < recipe.launch_entries.size(); ++index) {
    const auto& entry = recipe.launch_entries[index];
    if (index) text += ",";
    text += "{\"id\":" + quoted(entry.id) + ",\"label\":" + quoted(entry.label) + ",\"executable\":" +
            quoted(entry.executable) + ",\"working_directory\":" + quoted(entry.working_directory) +
            ",\"arguments\":" + quoted(entry.arguments) + (entry.in_install_root ? ",\"in_install_root\":true" : "") + "}";
  }
  text += "],\"arguments\":" + quoted(recipe.arguments) + ",\"resolution_arguments\":" +
          quoted(recipe.resolution_arguments) + ",\"dll_overrides\":" + quoted(recipe.dll_overrides) +
          ",\"architecture\":" + quoted(recipe.architecture) + ",\"runtime\":" + quoted(recipe.runtime) +
          ",\"prefix\":" + quoted(recipe.prefix) + ",\"graphics\":" + quoted(recipe.graphics) +
          (recipe.detected.empty() ? std::string() : ",\"detected\":" + quoted(recipe.detected)) + ",\"defaults\":{";
  bool first = true;
  for (const auto& [id, value] : recipe.defaults) {
    if (!first) text += ",";
    first = false;
    text += quoted(id) + ":" + quoted(value);
  }
  text += "},\"locked\":[";
  first = true;
  for (const auto& id : recipe.locked) {
    if (!first) text += ",";
    first = false;
    text += quoted(id);
  }
  text += "]";
  if (!recipe.requirements.empty()) {
    text += ",\"requirements\":{";
    first = true;
    for (const auto& [name, value] : recipe.requirements) {
      if (!first) text += ",";
      first = false;
      text += quoted(name) + ":" + quoted(value);
    }
    text += "}";
  }
  if (!recipe.registry.empty()) {
    // Grouped by key as the file has it; data holds the quoted string.
    std::map<std::string, std::vector<const RegistryValue*>> keys;
    for (const auto& value : recipe.registry) keys[value.key].push_back(&value);
    text += ",\"registry\":{";
    first = true;
    for (const auto& [key, values] : keys) {
      if (!first) text += ",";
      first = false;
      text += quoted(key) + ":{";
      for (std::size_t index = 0; index < values.size(); ++index) {
        if (index) text += ",";
        text += quoted(values[index]->name) + ":" + quoted(values[index]->data.substr(1, values[index]->data.size() - 2));
      }
      text += "}";
    }
    text += "}";
  }
  return text + "}\n";
}

namespace {

// Finds a game-relative path the way Windows would, ignoring case, and returns the
// spelling on disk. Symbolic links are not followed.
std::optional<std::string> ResolveGameFile(const std::string& root, const std::string& relative) {
  namespace fs = std::filesystem;
  fs::path cursor(root);
  std::string resolved;
  std::size_t start = 0;
  while (start <= relative.size()) {
    const auto end = std::min(relative.find('/', start), relative.size());
    const auto part = relative.substr(start, end - start);
    start = end + 1;
    if (part.empty()) return std::nullopt;
    std::error_code error;
    // The spelling stored in the directory, exact first (filesystems may ignore case).
    std::string match;
    const auto wanted = LowerAscii(part);
    for (fs::directory_iterator it(cursor, error), last; !error && it != last; it.increment(error)) {
      const auto name = it->path().filename().string();
      if (name == part) { match = name; break; }
      if (match.empty() && LowerAscii(name) == wanted) match = name;
    }
    if (match.empty()) return std::nullopt;
    cursor /= match;
    resolved += (resolved.empty() ? "" : "/") + match;
    const auto status = fs::symlink_status(cursor, error);
    if (error || fs::is_symlink(status)) return std::nullopt;
    if (end == relative.size()) return fs::is_regular_file(status) ? std::optional<std::string>(resolved) : std::nullopt;
    if (!fs::is_directory(status)) return std::nullopt;
  }
  return std::nullopt;
}

// "pe32" or "pe64" from a Windows executable's PE header; empty when it is neither.
std::string ExecutableArchitecture(const std::string& path) {
  const int file = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (file < 0) return {};
  const auto read_at = [file](off_t offset, unsigned char* buffer, std::size_t size) {
    std::size_t done = 0;
    while (done < size) {
      const auto count = ::pread(file, buffer + done, size - done, offset + static_cast<off_t>(done));
      if (count < 0 && errno == EINTR) continue;
      if (count <= 0) return false;
      done += static_cast<std::size_t>(count);
    }
    return true;
  };
  unsigned char dos[64];
  unsigned char header[26];
  std::string architecture;
  if (read_at(0, dos, sizeof(dos)) && dos[0] == 'M' && dos[1] == 'Z') {
    const std::uint32_t pe = dos[0x3c] | (dos[0x3d] << 8) | (dos[0x3e] << 16) | (static_cast<std::uint32_t>(dos[0x3f]) << 24);
    if (pe < 64u * 1024u * 1024u && read_at(static_cast<off_t>(pe), header, sizeof(header)) &&
        header[0] == 'P' && header[1] == 'E' && header[2] == 0 && header[3] == 0) {
      const unsigned machine = header[4] | (header[5] << 8);
      const unsigned magic = header[24] | (header[25] << 8);
      if (machine == 0x014c && magic == 0x010b) architecture = "pe32";
      if (machine == 0x8664 && magic == 0x020b) architecture = "pe64";
    }
  }
  ::close(file);
  return architecture;
}

}  // namespace

std::string GeneratedRecipeId(std::uint32_t app_id) { return playden::GeneratedRecipeId(GameId::Steam(app_id)); }

bool IsGeneratedRecipeId(std::string_view id) {
  constexpr std::string_view kSuffix = "-auto";
  if (id.size() <= kSuffix.size() || id.substr(id.size() - kSuffix.size()) != kSuffix) return false;
  const auto install_id = id.substr(0, id.size() - kSuffix.size());
  // Steam's has always been any "steam-…-auto"; a Local one names a valid game.
  if (install_id.size() > 6 && install_id.substr(0, 6) == "steam-") return true;
  return install_id.substr(0, 6) == "local-" && ParseInstallId(install_id).has_value();
}

GameId RecipeGame(const GameRecipe& recipe) {
  return recipe.game.valid() ? recipe.game : GameId::Steam(recipe.app_id);
}

RecipeChoices RecipeChoicesFrom(const GameSettingsRecord& record) {
  RecipeChoices choices;
  const auto program = record.overrides.find("program");
  if (program != record.overrides.end() && ValidateSettingValue(GameRecipe{}, "program", program->second, nullptr))
    choices.program = *steam::RelativeGamePath(program->second);
  const auto check = record.overrides.find("start_check");
  choices.try_anyway = check != record.overrides.end() && check->second == "try";
  return choices;
}

bool IsCommunityRecipeId(std::string_view id) {
  return id.size() > 10 && id.substr(0, 10) == "community-";
}

namespace {

// A program that only starts the game: a launcher, updater or settings window (Win32 dialogs
// the controller can't use) or a .NET launcher. Steam often lists it as the default (#70, #46).
bool StartsTheGameOnly(const std::string& program, const GameProbe& probe) {
  auto name = LowerAscii(program.substr(program.rfind('/') == std::string::npos ? 0 : program.rfind('/') + 1));
  // Redistributable installers and uninstallers are never the game, whatever they import (UE3Redist.exe draws a dialog with D3D9).
  for (const char* word : {"redist", "dxsetup", "uninst"})
    if (name.find(word) != std::string::npos) return true;
  if (probe.dotnet) return true;
  if (!probe.apis.empty() || !probe.engine.empty()) return false;
  for (const char* word : {"launch", "updater", "config", "setup", "settings"})
    if (name.find(word) != std::string::npos) return true;
  return false;
}

bool DrawsGraphics(const std::string& summary) {
  static const std::set<std::string> apis = {"d3d8", "d3d9", "d3d10", "d3d11", "dxgi", "d3d12", "opengl", "vulkan", "ddraw"};
  const auto facts = summary.substr(0, summary.find(' '));
  for (std::size_t start = 0; start <= facts.size();) {
    const auto end = std::min(facts.find(',', start), facts.size());
    if (apis.count(facts.substr(start, end - start))) return true;
    start = end + 1;
  }
  return false;
}

// The game program to start instead of a launcher: one that draws and the console may run,
// in the launcher's folder or below it first, then the largest.
std::optional<std::string> GameInsteadOf(const std::string& install_root, const std::string& launcher,
                                         const KitCapabilities& kit) {
  const auto slash = launcher.rfind('/');
  const auto folder = slash == std::string::npos ? std::string() : LowerAscii(launcher.substr(0, slash + 1));
  std::optional<std::string> best;
  std::pair<bool, std::uintmax_t> best_rank{false, 0};
  for (const auto& program : ListGamePrograms(install_root, kit)) {
    if (program.architecture.empty() || !program.refusal.empty() || !DrawsGraphics(program.detected) ||
        program.path == launcher || StartsTheGameOnly(program.path, GameProbe{}))
      continue;
    std::error_code error;
    const auto size = std::filesystem::file_size(std::filesystem::path(install_root) / program.path, error);
    const std::pair<bool, std::uintmax_t> rank{LowerAscii(program.path).rfind(folder, 0) == 0, error ? 0 : size};
    if (!best || rank > best_rank) best = program.path, best_rank = rank;
  }
  return best;
}

}  // namespace

std::optional<std::string> LaunchWorkingFolder(const LaunchEntry& launch) {
  const auto executable = steam::RelativeGamePath(launch.executable);
  if (!executable) return std::nullopt;
  if (launch.working_directory.empty()) {
    if (launch.in_install_root) return std::string();
    const auto slash = executable->rfind('/');
    return slash == std::string::npos ? std::string() : executable->substr(0, slash);
  }
  const auto relative = steam::RelativeGamePath(launch.working_directory, true);
  if (!relative) return std::nullopt;
  return *relative == "." ? std::string() : *relative;
}

bool GenerateGameRecipe(std::uint32_t app_id, const std::string& name, const std::vector<LaunchEntry>& launches,
                        const std::string& install_root, GameRecipe* recipe, std::string* reason,
                        const KitCapabilities& kit, const RecipeChoices& choices, bool* refusal_soft,
                        GameRecipeDiagnostics* diagnostics) {
  return GenerateGameRecipe(GameId::Steam(app_id), app_id, name, launches, install_root, recipe, reason, kit, choices,
                            refusal_soft, diagnostics);
}

bool GenerateGameRecipe(const GameId& game, std::uint32_t runtime_number, const std::string& name,
                        const std::vector<LaunchEntry>& launches, const std::string& install_root, GameRecipe* recipe,
                        std::string* reason, const KitCapabilities& kit, const RecipeChoices& choices,
                        bool* refusal_soft, GameRecipeDiagnostics* diagnostics) {
  const auto fail = [&](std::string why, bool soft) {
    if (reason) *reason = std::move(why);
    if (refusal_soft) *refusal_soft = soft;
    return false;
  };
  GameRecipe out;
  out.id = playden::GeneratedRecipeId(game);
  out.version = 2;  // 2: graphics and engine arguments from the game's files
  out.app_id = runtime_number;
  if (game.store != Store::kSteam) out.game = game;
  out.name = name;
  std::string first_missing;
  std::string first_refusal;
  bool first_refusal_soft = false;
  // The player's program comes first; Steam's launch options follow as suggestions.
  std::vector<std::pair<LaunchEntry, bool>> candidates;
  if (!choices.program.empty()) candidates.push_back({LaunchEntry{"", "", choices.program, "", ""}, true});
  for (const auto& launch : launches) candidates.push_back({launch, false});
  // Steam's default only starts the game: the game's own program goes first, the launcher stays an
  // option with its Steam number.
  std::string game_program;
  if (choices.program.empty() && !launches.empty()) {
    const auto relative = steam::RelativeGamePath(launches.front().executable);
    const auto on_disk = relative ? ResolveGameFile(install_root, *relative) : std::nullopt;
    if (on_disk && LowerAscii(on_disk->substr(on_disk->size() - std::min<std::size_t>(4, on_disk->size()))) == ".exe" &&
        StartsTheGameOnly(*on_disk, ProbeGame(install_root, *on_disk)))
      if (const auto game = GameInsteadOf(install_root, *on_disk, kit)) {
        game_program = *game;
        candidates.insert(candidates.begin(), {LaunchEntry{"", "", *game, "", ""}, false});
      }
  }
  int steam_entries = 0;
  // Steam's own entries are the last ones; the player's program and the game found instead of a
  // launcher come before them.
  const auto first_steam = candidates.size() - launches.size();
  for (std::size_t index = 0; index < candidates.size(); ++index) {
    const auto& [launch, chosen] = candidates[index];
    const auto relative = steam::RelativeGamePath(launch.executable);
    if (!relative || relative->size() < 4 || LowerAscii(relative->substr(relative->size() - 4)) != ".exe") continue;
    if (HasControl(launch.arguments) || launch.arguments.size() > kProsperoMaxArgumentBytes / 2 ||
        !SplitProsperoArguments(launch.arguments, nullptr))
      continue;
    std::string working;
    // Steam starts an entry that names no folder in the install root (Slay the Spire runs
    // jre/bin/javaw.exe -jar desktop-1.0.jar); other programs start in their own folder.
    bool in_install_root = index >= first_steam;
    if (!launch.working_directory.empty()) {
      const auto directory = steam::RelativeGamePath(launch.working_directory, true);
      if (!directory) continue;
      working = *directory == "." ? std::string() : *directory;
    }
    auto on_disk = ResolveGameFile(install_root, *relative);
    if (!on_disk) {
      if (first_missing.empty()) first_missing = *relative;
      continue;
    }
    const auto probe = ProbeGame(install_root, *on_disk);
    RuntimeAbilities abilities;
    abilities.shared_sections = kit.shared_sections;
    abilities.wine_mono = kit.wine_mono;
    abilities.vkd3d_proton = kit.vkd3d_proton;
    const auto defaults = DefaultsFor(probe, abilities);
    if (!defaults.refusal.empty() && !(defaults.refusal_soft && choices.try_anyway)) {
      if (first_refusal.empty()) first_refusal = defaults.refusal, first_refusal_soft = defaults.refusal_soft;
      if (diagnostics && probe.steam_stub && diagnostics->steam_stub_program.empty()) {
        diagnostics->steam_stub_program = *on_disk;
        diagnostics->steam_stub_detail = probe.steam_stub_detail;
      }
      continue;
    }
    // An Unreal root program only starts the real one as a child process, which the console cannot do.
    // A program the player chose is started as chosen.
    if (!probe.unreal_program.empty() && !chosen) {
      on_disk = probe.unreal_program;
      const auto folder = on_disk->rfind('/');
      working = folder == std::string::npos ? std::string() : on_disk->substr(0, folder);
      in_install_root = false;
    }
    const auto architecture = ExecutableArchitecture(install_root + "/" + *on_disk);
    if (architecture.empty()) continue;
    if (out.launch_entries.empty()) {
      out.architecture = architecture;
      out.graphics = defaults.graphics;
      out.arguments = defaults.arguments;
      out.resolution_arguments = defaults.resolution_arguments;
      out.detected = defaults.summary;
      // Most Steam games stop when the Steam client is missing; the emulator answers instead.
      out.defaults["steam_features"] = "emulate";
      if (!defaults.environment.empty()) out.defaults["environment"] = defaults.environment;
      if (!defaults.d3d12.empty()) out.defaults["d3d12"] = defaults.d3d12;

    } else if (architecture != out.architecture) {
      continue;
    }
    LaunchEntry entry;
    entry.id = chosen ? std::string(kProgramEntryId)
                      : out.launch_entries.empty() && !game_program.empty() && launch.executable == game_program
                      ? std::string("game")
                      : "launch-" + std::to_string(++steam_entries);
    const auto slash = on_disk->rfind('/');
    entry.label = !launch.label.empty() ? launch.label
                                        : slash == std::string::npos ? *on_disk : on_disk->substr(slash + 1);
    entry.label.erase(std::remove_if(entry.label.begin(), entry.label.end(),
                                     [](char value) { return HasControl(std::string_view(&value, 1)); }),
                      entry.label.end());
    if (entry.label.size() > 120) entry.label.resize(120);
    entry.executable = *on_disk;
    entry.working_directory = working;
    // Only when it differs from the program's own folder, so recipes of games in the root stay as they were.
    entry.in_install_root = in_install_root && working.empty() && on_disk->find('/') != std::string::npos;
    entry.arguments = CollapseSpaces(launch.arguments);
    out.launch_entries.push_back(std::move(entry));
  }
  if (out.launch_entries.empty()) {
    if (!first_refusal.empty()) return fail(first_refusal, first_refusal_soft);
    // Choosing a program in Game settings gets past both of these.
    return fail(first_missing.empty() ? "Steam lists no Windows program this console can start for this game."
                                      : "The game's program " + first_missing + " is missing; verify the download.",
                true);
  }
  // Round trip so a generated recipe obeys the same rules as a shipped one.
  GameRecipe checked;
  std::string why;
  if (!ParseGameRecipe(SerializeGameRecipe(out), &checked, &why))
    return fail("Playden could not set up this game: " + why, false);
  *recipe = std::move(checked);
  return true;
}

std::string GameProgramArchitecture(const std::string& install_root, const std::string& relative) {
  const auto path = steam::RelativeGamePath(relative);
  if (!path) return {};
  const auto on_disk = ResolveGameFile(install_root, *path);
  return on_disk ? ExecutableArchitecture(install_root + "/" + *on_disk) : std::string();
}

std::string GameProgramRefusal(const std::string& install_root, const std::string& relative, const KitCapabilities& kit,
                               bool try_anyway) {
  const auto path = steam::RelativeGamePath(relative);
  const auto on_disk = path ? ResolveGameFile(install_root, *path) : std::nullopt;
  if (!on_disk) return "The program " + relative + " is missing.";
  RuntimeAbilities abilities;
  abilities.shared_sections = kit.shared_sections;
  abilities.wine_mono = kit.wine_mono;
  abilities.vkd3d_proton = kit.vkd3d_proton;
  const auto defaults = DefaultsFor(ProbeGame(install_root, *on_disk), abilities);
  return defaults.refusal_soft && try_anyway ? std::string() : defaults.refusal;
}

std::vector<GameProgram> ListGamePrograms(const std::string& install_root, const KitCapabilities& kit) {
  namespace fs = std::filesystem;
  // Installers and redistributables the game ships next to itself are never the game.
  static const std::set<std::string> skipped = {"_commonredist", "__installer", "redist", "redistributables",
                                                "directx", "vcredist", "dotnet", "support", "$pluginsdir"};
  std::vector<std::string> found;
  std::vector<std::pair<std::string, int>> folders = {{"", 0}};
  constexpr int kMaxDepth = 6;
  constexpr std::size_t kMaxEntries = 4096;
  std::size_t seen = 0;
  for (std::size_t next = 0; next < folders.size() && found.size() < kMaxGamePrograms; ++next) {
    const auto [folder, depth] = folders[next];
    std::vector<std::string> names;
    std::error_code error;
    for (fs::directory_iterator it(fs::path(install_root) / folder, error), last;
         !error && it != last && seen < kMaxEntries; it.increment(error), ++seen)
      names.push_back(it->path().filename().string());
    std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) {
      return LowerAscii(a) < LowerAscii(b);
    });
    for (const auto& name : names) {
      const auto relative = folder.empty() ? name : folder + "/" + name;
      const auto status = fs::symlink_status(fs::path(install_root) / relative, error);
      if (error || fs::is_symlink(status)) continue;
      if (fs::is_directory(status)) {
        if (depth < kMaxDepth && !skipped.count(LowerAscii(name)) && name.front() != '.')
          folders.push_back({relative, depth + 1});
      } else if (fs::is_regular_file(status) && name.size() > 4 &&
                 LowerAscii(name.substr(name.size() - 4)) == ".exe" && found.size() < kMaxGamePrograms) {
        found.push_back(relative);
      }
    }
  }
  std::vector<GameProgram> programs;
  RuntimeAbilities abilities;
  abilities.shared_sections = kit.shared_sections;
  abilities.wine_mono = kit.wine_mono;
  abilities.vkd3d_proton = kit.vkd3d_proton;
  for (const auto& relative : found) {
    GameProgram program;
    program.path = relative;
    program.architecture = ExecutableArchitecture(install_root + "/" + relative);
    if (!program.architecture.empty()) {
      const auto defaults = DefaultsFor(ProbeGame(install_root, relative), abilities);
      program.refusal = defaults.refusal;
      program.refusal_soft = defaults.refusal_soft;
      program.detected = defaults.summary;
    }
    programs.push_back(std::move(program));
  }
  return programs;
}

namespace {

bool ReadSmallFile(const std::string& path, std::string* text) {
  const std::filesystem::path value(path);
  auto directory = ManagedDirectory::Open(value.parent_path(), DirectoryPolicy::kApprovedTree);
  if (!directory) return false;
  const int file = directory->OpenRegularFile(value.filename().string());
  if (file < 0) return false;
  struct stat before {}, after {};
  if (::fstat(file, &before) != 0) { ::close(file); return false; }
  text->clear();
  char buffer[4096];
  bool valid = true;
  while (valid) {
    const auto count = ::read(file, buffer, sizeof(buffer));
    if (count < 0 && errno == EINTR) continue;
    if (count < 0 || text->size() + static_cast<std::size_t>(count) > 256u * 1024u) {
      valid = false;
      break;
    }
    if (!count) break;
    text->append(buffer, static_cast<std::size_t>(count));
  }
  const bool unchanged = ::fstat(file, &after) == 0 &&
      before.st_dev == after.st_dev && before.st_ino == after.st_ino &&
      before.st_uid == after.st_uid && before.st_mode == after.st_mode &&
      before.st_size == after.st_size && before.st_mtime == after.st_mtime &&
      before.st_ctime == after.st_ctime;
  return ::close(file) == 0 && valid && unchanged && directory->Validate();
}

std::optional<ManagedDirectory> OpenSafeParent(const std::string& path,
                                               DirectoryPolicy policy,
                                               std::string* name) {
  const std::filesystem::path value(path);
  *name = value.filename().string();
  if (name->empty()) { errno = EINVAL; return std::nullopt; }
  return ManagedDirectory::Open(value.parent_path(), policy);
}

std::optional<ManagedDirectory> OpenLaunchDirectory(const LaunchEntry& launch,
                                                     const std::string& install_root) {
  const auto folder = LaunchWorkingFolder(launch);
  if (!folder) return std::nullopt;
  const auto& working = *folder;
  auto directory = ManagedDirectory::Open(install_root, DirectoryPolicy::kApprovedTree);
  if (!directory) return std::nullopt;
  for (std::size_t start = 0; start < working.size();) {
    const auto slash = working.find('/', start);
    const auto component = working.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
    auto next = directory->OpenChild(component, DirectoryPolicy::kApprovedTree);
    if (!next) return std::nullopt;
    directory = std::move(next);
    if (slash == std::string::npos) break;
    start = slash + 1;
  }
  return directory;
}

enum class RelativeRead { kMissing, kPresent, kError };

RelativeRead ReadRegularAt(const ManagedDirectory& directory, const char* name,
                           std::size_t maximum, std::string* text,
                           mode_t* mode = nullptr, bool private_file = false) {
  const int file = directory.OpenRegularFile(name, private_file);
  if (file < 0) return errno == ENOENT ? RelativeRead::kMissing : RelativeRead::kError;
  struct stat status {}, after {};
  if (::fstat(file, &status) != 0 || !S_ISREG(status.st_mode) ||
      (private_file && (status.st_mode & 07777) != 0600 &&
       (status.st_mode & 07777) != 0644)) {
    ::close(file); return RelativeRead::kError;
  }
  if (mode) *mode = status.st_mode & 0777;
  text->clear();
  char buffer[4096];
  while (true) {
    const auto count = ::read(file, buffer, sizeof(buffer));
    if (count < 0) { ::close(file); return RelativeRead::kError; }
    if (count == 0) break;
    if (text->size() + static_cast<std::size_t>(count) > maximum) {
      ::close(file);
      return RelativeRead::kError;
    }
    text->append(buffer, static_cast<std::size_t>(count));
  }
  const bool unchanged = ::fstat(file, &after) == 0 &&
      status.st_dev == after.st_dev && status.st_ino == after.st_ino &&
      status.st_uid == after.st_uid && status.st_mode == after.st_mode &&
      status.st_size == after.st_size && status.st_mtime == after.st_mtime &&
      status.st_ctime == after.st_ctime;
  if (::close(file) != 0 || !unchanged || !directory.Validate())
    return RelativeRead::kError;
  return RelativeRead::kPresent;
}

bool WriteAtomicAt(const ManagedDirectory& directory, const char* name,
                   const std::string& text, mode_t new_mode,
                   bool preserve_existing_mode = false,
                   bool private_file = false,
                   bool replace_existing = true) {
  mode_t mode = new_mode;
  struct stat status {};
  if (directory.InspectChild(name, &status)) {
    if (!S_ISREG(status.st_mode)) return false;
    if (private_file && (status.st_uid != ::geteuid() ||
        ((status.st_mode & 07777) != 0600 && (status.st_mode & 07777) != 0644))) {
      errno = EACCES;
      return false;
    }
    if (preserve_existing_mode) mode = status.st_mode & 0777;
  } else if (errno != ENOENT) return false;
  static std::atomic<std::uint64_t> sequence{0};
  const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
  std::string temporary;
  int file = -1;
  for (unsigned attempt = 0; attempt < 8; ++attempt) {
    const auto serial = sequence.fetch_add(1, std::memory_order_relaxed);
    temporary = std::string(name) + ".playden-tmp-" + std::to_string(static_cast<long long>(::getpid())) + "-" +
                std::to_string(static_cast<long long>(tick)) + "-" + std::to_string(serial);
    file = directory.CreateExclusiveFile(temporary, mode);
    if (file >= 0) break;
    if (errno != EEXIST) return false;
  }
  if (file < 0) return false;
  if (::fchmod(file, mode) != 0) {
    ::close(file);
    directory.RemoveRegularFile(temporary);
    return false;
  }
  std::size_t written = 0;
  while (written < text.size()) {
    const auto count = ::write(file, text.data() + written, text.size() - written);
    if (count <= 0) break;
    written += static_cast<std::size_t>(count);
  }
  const bool synced = written == text.size() && ::fsync(file) == 0;
  const bool closed = ::close(file) == 0;
  if (!synced || !closed ||
      !directory.RenameChild(temporary, directory, name, replace_existing)) {
    directory.RemoveRegularFile(temporary);
    return false;
  }
  return directory.Sync();
}

// A record's key: ToKey's form, or digits with leading zeros, which earlier Playdens read as that
// Steam app ID (they never wrote them).
std::optional<GameId> SettingsKey(const std::string& key) {
  if (const auto id = ParseKey(key)) return id;
  std::uint32_t app_id = 0;
  const auto parsed = std::from_chars(key.data(), key.data() + key.size(), app_id);
  if (key.empty() || parsed.ec != std::errc{} || parsed.ptr != key.data() + key.size() || app_id == 0)
    return std::nullopt;
  return GameId::Steam(app_id);
}

bool ParseSettingsFile(std::string_view text,
                       std::map<GameId, GameSettingsRecord>* records) {
  const auto root = storage_json::Parse(text);
  const auto* version = root ? root->Find("version") : nullptr;
  const auto* games = root ? root->Find("games") : nullptr;
  if (!root || !root->is_object() || !version || version->AsUint64() != 1 ||
      !games || !games->is_object()) return false;
  records->clear();
  for (const auto& [key, value] : games->object()) {
    const auto id = SettingsKey(key);
    GameSettingsRecord record;
    if (!id || !RecordFromValue(value, &record)) continue;
    (*records)[*id] = std::move(record);
  }
  return true;
}

std::string SerializeSettingsFile(
    const std::map<GameId, GameSettingsRecord>& records) {
  std::string text = "{\"version\":1,\"games\":{";
  bool first = true;
  for (const auto& [id, record] : records) {
    if (!id.valid() || (record.preset_id.empty() && record.overrides.empty() && record.revision == 0 &&
                         !record.steam_app_id && record.steam_launch.empty()))
      continue;
    if (!first) text += ",";
    first = false;
    text += "\"" + ToKey(id) + "\":" + SerializeGameSettingsRecord(record);
  }
  return text + "}}\n";
}

}  // namespace

bool LoadSettingsData(const std::string& runtime_dir, std::vector<SettingsPreset>* presets,
                      std::vector<GameRecipe>* recipes, std::string* reason) {
  const auto fail = [&](std::string why) {
    if (reason) *reason = std::move(why);
    return false;
  };
  std::string text;
  if (!ReadSmallFile(runtime_dir + "/presets.json", &text)) return fail("cannot read presets.json");
  if (!ParseSettingsPresets(text, presets, reason)) return false;
  recipes->clear();
  if (!ReadSmallFile(runtime_dir + "/recipes/recipes.lst", &text)) return fail("cannot read recipes.lst");
  std::size_t start = 0;
  while (start < text.size()) {
    const auto end = std::min(text.find('\n', start), text.size());
    std::string name = text.substr(start, end - start);
    start = end + 1;
    while (!name.empty() && (name.back() == '\r' || name.back() == ' ')) name.pop_back();
    if (name.empty() || name.front() == '#') continue;
    if (name.find('/') != std::string::npos || name.find("..") != std::string::npos) return fail("bad recipe name " + name);
    std::string recipe_text;
    GameRecipe recipe;
    std::string why;
    if (!ReadSmallFile(runtime_dir + "/recipes/" + name, &recipe_text)) return fail("cannot read recipe " + name);
    if (!ParseGameRecipe(recipe_text, &recipe, &why)) return fail(name + ": " + why);
    recipes->push_back(std::move(recipe));
  }
  return true;
}

bool ApplyGameSettings(const GameRecipe& recipe, const EffectiveSettings& settings, const ProfileTarget& target,
                       const std::string& prospero_root, std::string* reason) {
  const auto fail = [&](std::string why) {
    if (reason) *reason = std::move(why);
    return false;
  };
  ProsperoProfile profile;
  if (!RenderProsperoProfile(recipe, settings, target, &profile, reason)) return false;
  if (!settings.launch) return fail("no launch option");

  // Check the launch working folder before writing anything, so a refusal leaves both files as they were.
  auto config_directory = OpenLaunchDirectory(*settings.launch, target.install_root);
  if (!config_directory) return fail("launch working folder is not a safe directory");
  std::string existing;
  const auto read_config = ReadRegularAt(*config_directory, "dxvk.conf", 256u * 1024u, &existing);
  if (read_config == RelativeRead::kError) {
    return fail("cannot read dxvk.conf");
  }
  const bool has_config = read_config == RelativeRead::kPresent;
  if (has_config && existing.compare(0, std::char_traits<char>::length(kDxvkHeader), kDxvkHeader) != 0)
  {
    return fail("the game folder has its own dxvk.conf; Playden will not replace it");
  }

  if (!PublishProsperoProfile(prospero_root, profile, reason)) return false;
  const auto config = RenderDxvkConfig(settings);
  if (config.empty()) {
    if (has_config && (!config_directory->RemoveRegularFile("dxvk.conf") ||
                       !config_directory->Sync())) {
      return fail("cannot remove dxvk.conf");
    }
    return true;
  }
  if (has_config && existing == config) return true;
  const bool written = WriteAtomicAt(*config_directory, "dxvk.conf", config, 0644, true);
  if (!written) return fail("cannot write dxvk.conf");
  return true;
}

bool LoadGameSettingsFile(const std::string& path, std::map<GameId, GameSettingsRecord>* records,
                          std::string* reason) {
  records->clear();
  std::string text;
  std::string name;
  auto directory = OpenSafeParent(path, DirectoryPolicy::kPrivate, &name);
  if (!directory) {
    if (errno == ENOENT) return true;
    if (reason) *reason = "cannot read game settings";
    return false;
  }
  const auto read = ReadRegularAt(*directory, name.c_str(), storage_json::Limits{}.max_bytes,
                                  &text, nullptr, true);
  if (read == RelativeRead::kMissing) return true;
  if (read == RelativeRead::kError) {
    if (reason) *reason = "cannot read game settings";
    return false;
  }
  if (!ParseSettingsFile(text, records)) {
    if (reason) *reason = "game settings file is malformed";
    return false;
  }
  return true;
}

bool SaveGameSettingsFile(const std::string& path, const std::map<GameId, GameSettingsRecord>& records,
                          std::string* reason) {
  const auto text = SerializeSettingsFile(records);
  std::string name;
  auto directory = OpenSafeParent(path, DirectoryPolicy::kPrivate, &name);
  if (!directory) {
    if (reason) *reason = "cannot write game settings";
    return false;
  }
  struct stat status {};
  const bool inspected = directory->InspectChild(name, &status);
  if (inspected && (!S_ISREG(status.st_mode) || status.st_uid != ::geteuid() ||
                    ((status.st_mode & 07777) != 0600 && (status.st_mode & 07777) != 0644))) {
    if (reason) *reason = "cannot write game settings";
    return false;
  }
  if (!inspected && errno != ENOENT) {
    if (reason) *reason = "cannot write game settings";
    return false;
  }
  const bool written = WriteAtomicAt(*directory, name.c_str(), text, 0600, false, true);
  if (!written) {
    if (reason) *reason = "cannot write game settings";
    return false;
  }
  return true;
}

bool MigrateLegacyGameSettingsFile(const std::string& legacy_path,
                                   const std::string& private_path,
                                   std::string* reason) {
  const auto fail = [&](std::string why) {
    if (reason) *reason = std::move(why);
    return false;
  };
  std::string destination_name;
  auto destination = OpenSafeParent(
      private_path, DirectoryPolicy::kPrivate, &destination_name);
  if (!destination) return fail("private game settings folder is unavailable");
  struct stat destination_status {};
  if (destination->InspectChild(destination_name, &destination_status))
    return fail("private game settings already exist");
  if (errno != ENOENT) return fail("cannot inspect private game settings");

  std::string source_name;
  auto source = OpenSafeParent(
      legacy_path, DirectoryPolicy::kApprovedTree, &source_name);
  if (!source) {
    if (errno == ENOENT) return true;
    return fail("legacy game settings folder is unsafe");
  }
  std::string text;
  const auto read = ReadRegularAt(*source, source_name.c_str(),
                                  storage_json::Limits{}.max_bytes,
                                  &text, nullptr, true);
  if (read == RelativeRead::kMissing) return true;
  if (read != RelativeRead::kPresent)
    return fail("legacy game settings are unsafe or unreadable");
  std::map<GameId, GameSettingsRecord> records;
  if (!ParseSettingsFile(text, &records))
    return fail("legacy game settings file is malformed");
  if (!WriteAtomicAt(*destination, destination_name.c_str(),
                     SerializeSettingsFile(records), 0600, false, true, false))
    return fail("cannot publish migrated game settings");
  return true;
}

bool ParseProfileSidecar(std::string_view text, CommunityPreset* community, std::string* reason) {
  const auto fail = [&](std::string why) {
    if (reason) *reason = std::move(why);
    return false;
  };
  const auto root = json::parse(text);
  const auto* version = root ? root->get("schema_version") : nullptr;
  if (!root || !root->object() || !version || version->number() != 1.0) return fail("not a schema 1 sidecar");
  CommunityPreset out;
  const auto* game = root->get("game");
  std::string name;
  if (!StringField(*root, "profile", &out.profile_id) || !game || !StringField(*game, "name", &name))
    return fail("sidecar needs profile and game.name");
  if (const auto* app = game->get("steam_appid")) {
    std::uint64_t parsed_app = 0;
    if (!ExactUnsigned(app, std::numeric_limits<std::uint32_t>::max(), &parsed_app, false))
      return fail("sidecar steam_appid is invalid");
    out.steam_appid = static_cast<std::uint32_t>(parsed_app);
  }
  if (const auto* requires_field = root->get("requires")) {
    if (!requires_field->object()) return fail("requires must be an object");
    if (const auto* prs = requires_field->get("prospero_win")) {
      if (!prs->array()) return fail("requires.prospero_win must be an array");
      for (const auto& item : *prs->array()) {
        if (!item.string() || item.string()->empty())
          return fail("requires.prospero_win must contain nonempty strings");
        out.requires_prospero_win.push_back(*item.string());
      }
    }
  }
  const auto* checks = root->get("checks");
  if (!checks || !checks->array() || checks->array()->empty()) return fail("sidecar has no console check");
  const auto& newest = checks->array()->back();
  std::string date, firmware, notes;
  StringField(newest, "rating", &out.rating, false);
  StringField(newest, "date", &date, false);
  StringField(newest, "firmware", &firmware, false);
  StringField(newest, "notes", &notes, false);
  out.preset.id = "community-" + out.profile_id;
  out.preset.name = "Community · " + name;
  out.preset.try_when = notes;
  out.preset.short_hint = out.rating + (firmware.empty() ? "" : " on " + firmware) + (date.empty() ? "" : ", " + date);
  std::map<std::string, std::string> settings;
  if (!StringMap(root->get("settings"), &settings)) return fail("settings must map ids to strings");
  // Keep what a preset may set and this build accepts; skip the rest rather than refuse.
  for (const auto& [id, value] : settings) {
    const auto* definition = FindSetting(id);
    const bool button = id.compare(0, 7, "button.") == 0;
    if (!definition || id == "launch_option" || id == "graphics" || (definition->kind == SettingKind::kText && !button))
      continue;
    if (ValidateSettingValue(GameRecipe{}, id, value, nullptr)) out.preset.settings[id] = value;
  }
  *community = std::move(out);
  return true;
}

bool ClearShaderCache(const LaunchEntry& launch, const std::string& install_root, std::string* reason) {
  const auto fail = [&](std::string why) {
    if (reason) *reason = std::move(why);
    return false;
  };
  const auto executable = steam::RelativeGamePath(launch.executable);
  if (!executable) return fail("launch executable is not a game path");
  auto stem = executable->substr(executable->rfind('/') == std::string::npos ? 0 : executable->rfind('/') + 1);
  const auto lower = [](char value) { return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value; };
  if (stem.size() > 4 && lower(stem[stem.size() - 4]) == '.' && lower(stem[stem.size() - 3]) == 'e' &&
      lower(stem[stem.size() - 2]) == 'x' && lower(stem[stem.size() - 1]) == 'e')
    stem.resize(stem.size() - 4);
  auto directory = OpenLaunchDirectory(launch, install_root);
  if (!directory) return fail("launch working folder is not a safe directory");
  const auto name = stem + ".dxvk-cache";
  struct stat status {};
  if (!directory->InspectChild(name, &status)) {
    const int error = errno;
    if (error == ENOENT) return true;
    return fail("cannot inspect the shader cache");
  }
  if (!S_ISREG(status.st_mode)) {
    return fail("shader cache is not a regular file");
  }
  const bool removed = directory->RemoveRegularFile(name) && directory->Sync();
  if (!removed) return fail("cannot delete the shader cache");
  return true;
}

namespace {
constexpr const char* kBottleEnvironmentNames[] = {
    "VKD3D_CONFIG",          "VKD3D_DEBUG",          "VKD3D_DISABLE_EXTENSIONS",   "VKD3D_FEATURE_LEVEL",
    "VKD3D_FRAME_RATE",      "VKD3D_LOG_FILE",       "VKD3D_SHADER_CACHE_PATH",    "VKD3D_SHADER_DEBUG",
    "VKD3D_SHADER_MODEL",    "VKD3D_SWAPCHAIN_IMAGES", "VKD3D_SWAPCHAIN_LATENCY_FRAMES",
    "VKD3D_SWAPCHAIN_PRESENT_MODE"};
}  // namespace

bool IsBottleEnvironmentEntry(std::string_view name, std::string_view value) {
  if (std::find(std::begin(kBottleEnvironmentNames), std::end(kBottleEnvironmentNames), name) ==
          std::end(kBottleEnvironmentNames) ||
      value.empty() || value.size() >= 128)
    return false;
  for (const unsigned char c : value)
    if (c < 0x20 || c > 0x7e || c == '"' || c == '\'') return false;
  return true;
}

std::vector<RegistryValue> RenderBottleEnvironment(const EffectiveSettings& settings) {
  std::map<std::string, std::string> listed;
  const auto& environment = settings.values.at("environment");
  if (environment.available)
    for (std::size_t start = 0; start <= environment.value.size();) {
      const auto end = std::min(environment.value.find(';', start), environment.value.size());
      std::string entry = environment.value.substr(start, end - start);
      start = end + 1;
      const auto equals = entry.find('=');
      if (equals == std::string::npos) continue;
      std::string name = entry.substr(0, equals), item = entry.substr(equals + 1);
      while (!name.empty() && name.front() == ' ') name.erase(0, 1);
      while (!name.empty() && name.back() == ' ') name.pop_back();
      while (!item.empty() && item.front() == ' ') item.erase(0, 1);
      while (!item.empty() && item.back() == ' ') item.pop_back();
      if (IsBottleEnvironmentEntry(name, item)) listed[name] = item;
    }
  // A Frame limit on a display-120hz runtime replaces a VKD3D_FRAME_RATE typed in Environment variables: the display
  // paces the cap, or, for 30 at 120 Hz output, vkd3d-proton's own limiter does.
  if (FrameLimitOwnsCap(settings)) {
    listed.erase("VKD3D_FRAME_RATE");
    if (settings.vkd3d_proton)
      if (const auto limit = SoftwareFrameLimit(settings); !limit.empty()) listed["VKD3D_FRAME_RATE"] = limit;
  }
  std::vector<RegistryValue> values;
  for (const char* name : kBottleEnvironmentNames) {
    const auto found = listed.find(name);
    std::string data;
    if (found != listed.end()) {
      data = "\"";
      for (const char c : found->second) data += c == '\\' ? std::string("\\\\") : std::string(1, c);
      data += "\"";
    }
    values.push_back({"Environment", name, data});
  }
  return values;
}

std::vector<RegistryValue> RenderRegistryValues(const EffectiveSettings& settings) {
  std::vector<RegistryValue> values;
  const auto& version = settings.values.at("windows_version");
  if (version.available && settings.launch) {
    auto executable = settings.launch->executable;
    std::replace(executable.begin(), executable.end(), '\\', '/');
    const auto slash = executable.rfind('/');
    const auto exe = slash == std::string::npos ? executable : executable.substr(slash + 1);
    // Windows 10 is Wine's default here, so it removes the per-game value.
    values.push_back({"Software\\Wine\\AppDefaults\\" + exe, "Version",
                      version.value == "win10" ? std::string() : "\"" + version.value + "\""});
  }
  const auto& scale = settings.values.at("text_scale");
  if (scale.available) {
    // Wine reads the DPI from Control Panel\Desktop (winecfg's slider writes it there);
    // Software\Wine\Fonts LogPixels is only its font cache marker, rewritten at start.
    // 96 is Wine's default, so 100% removes the value.
    char dword[16];
    std::snprintf(dword, sizeof(dword), "dword:%08lx", std::strtoul(scale.value.c_str(), nullptr, 10));
    values.push_back({"Control Panel\\Desktop", "LogPixels", scale.value == "96" ? std::string() : dword});
  }
  return values;
}

namespace {

std::string RegistryHeader(const std::string& key) {
  std::string escaped;
  for (const char c : key) escaped += c == '\\' ? std::string("\\\\") : std::string(1, c);
  return "[" + escaped + "]";
}

char AsciiLower(char value) {
  return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : value;
}

bool RegistryEqual(std::string_view left, std::string_view right) {
  if (left.size() != right.size()) return false;
  for (std::size_t index = 0; index < left.size(); ++index)
    if (AsciiLower(left[index]) != AsciiLower(right[index])) return false;
  return true;
}

std::string RegistryIdentity(const RegistryValue& value) {
  auto identity = RegistryHeader(value.key) + "\n" + (value.default_value ? "@" : "\"" + value.name);
  std::transform(identity.begin(), identity.end(), identity.begin(), AsciiLower);
  return identity;
}

}  // namespace

bool ApplyRegistryValues(std::string* user_reg, const std::vector<RegistryValue>& values, std::int64_t unix_time,
                         std::string* reason) {
  const auto fail = [&](std::string why) {
    if (reason) *reason = std::move(why);
    return false;
  };
  if (user_reg->compare(0, 23, "WINE REGISTRY Version 2") != 0) return fail("not a Wine registry file");
  std::string text = *user_reg;
  std::set<std::string> identities;
  for (const auto& value : values) {
    if (value.key.empty() || (!value.default_value && value.name.empty()) ||
        (value.default_value && !value.name.empty()) || value.name.find('"') != std::string::npos ||
        HasControl(value.key) || HasControl(value.name) || HasControl(value.data))
      return fail("bad registry value");
    if (!identities.insert(RegistryIdentity(value)).second) return fail("duplicate registry value");
  }
  for (const auto& value : values) {
    const auto header = RegistryHeader(value.key);
    const auto line_prefix = value.default_value ? std::string("@=") : "\"" + value.name + "\"=";
    std::size_t section = std::string::npos;
    std::size_t body = std::string::npos;
    std::size_t end = text.size();
    std::size_t section_count = 0;
    for (std::size_t at = 0; at < text.size();) {
      const auto newline = text.find('\n', at);
      const auto next = newline == std::string::npos ? text.size() : newline + 1;
      auto content_end = newline == std::string::npos ? text.size() : newline;
      if (content_end > at && text[content_end - 1] == '\r') --content_end;
      const std::string_view line(text.data() + at, content_end - at);
      if (line.size() >= header.size() && RegistryEqual(line.substr(0, header.size()), header)) {
        const auto suffix = line.substr(header.size());
        bool valid_suffix = suffix.empty();
        if (!suffix.empty() && suffix.front() == ' ') {
          valid_suffix = suffix.size() > 1 &&
                         std::all_of(suffix.begin() + 1, suffix.end(), [](char c) { return c >= '0' && c <= '9'; });
        }
        if (!valid_suffix) return fail("malformed registry section");
        if (++section_count > 1) return fail("duplicate registry section");
        section = at;
        body = next;
      } else if (section_count == 1 && end == text.size() && at >= body && !line.empty() && line.front() == '[') {
        end = at;
      }
      if (newline == std::string::npos) break;
      at = next;
    }
    if (section == std::string::npos) {
      if (value.data.empty()) continue;
      const std::string newline = text.find("\r\n") == std::string::npos ? "\n" : "\r\n";
      if (!text.empty() && text.back() != '\n') text += '\n';
      text += newline + header + " " + std::to_string(unix_time) + newline + line_prefix + value.data + newline;
      continue;
    }
    std::size_t line = std::string::npos;
    std::size_t assignment_count = 0;
    for (auto at = body; at < end;) {
      const auto newline = text.find('\n', at);
      const auto line_end = std::min(newline, end);
      if (line_end - at >= line_prefix.size() &&
          RegistryEqual(std::string_view(text).substr(at, line_prefix.size()), line_prefix)) {
        line = at;
        if (++assignment_count > 1) return fail("duplicate registry assignment");
      }
      if (newline == std::string::npos || newline >= end) break;
      at = newline + 1;
    }
    if (line != std::string::npos) {
      auto at = line;
      std::size_t line_end = line;
      std::string logical, terminator;
      while (true) {
        const auto newline = text.find('\n', at);
        auto content_end = std::min(newline, end);
        terminator = newline == std::string::npos || newline >= end ? std::string() : "\n";
        if (content_end > at && text[content_end - 1] == '\r') {
          --content_end;
          if (!terminator.empty()) terminator = "\r\n";
        }
        const bool continuation = content_end > at && text[content_end - 1] == '\\';
        logical += text.substr(at, content_end - at - (continuation ? 1 : 0));
        line_end = newline == std::string::npos || newline >= end ? end : newline + 1;
        if (!continuation) break;
        if (line_end >= end) return fail("unterminated registry assignment");
        at = line_end;
        while (at < end && (text[at] == ' ' || text[at] == '\t')) ++at;
      }
      const auto existing_prefix = text.substr(line, line_prefix.size());
      if (!value.data.empty() && logical == existing_prefix + value.data) continue;
      text.replace(line, line_end - line,
                   value.data.empty() ? std::string() : existing_prefix + value.data + terminator);
    } else if (!value.data.empty()) {
      const std::string newline = text.find("\r\n") == std::string::npos ? "\n" : "\r\n";
      // After the section's last value, before the blank line that separates sections.
      while (end > body && text[end - 1] == '\n') {
        auto blank = end - 1;
        if (blank > body && text[blank - 1] == '\r') --blank;
        if (blank == body || text[blank - 1] != '\n') break;
        end = blank;
      }
      std::string insertion;
      if (end > body && text[end - 1] != '\n') insertion += newline;
      insertion += line_prefix + value.data + newline;
      text.insert(end, insertion);
    }
  }
  *user_reg = std::move(text);
  return true;
}

// Edits one hive of the stopped bottle in place: the original goes to <hive>.playden-backup
// first, and an unchanged hive is not written at all.
static bool ApplyRegistryFileValues(const std::vector<RegistryValue>& values, const std::string& prefix_dir,
                                    std::int64_t unix_time, std::string* reason, const std::string& hive = "user.reg") {
  const auto fail = [&](std::string why) {
    if (reason) *reason = std::move(why);
    return false;
  };
  if (values.empty()) return true;
  // The bottle may be 0711 (bottle-profile-shared).
  auto directory = ManagedDirectory::Open(prefix_dir, DirectoryPolicy::kPrivateTraversable);
  if (!directory) return fail("cannot read the bottle's " + hive);
  std::string original;
  constexpr std::size_t kMaxRegistryBytes = 32u * 1024u * 1024u;
  mode_t original_mode = 0600;
  if (ReadRegularAt(*directory, hive.c_str(), kMaxRegistryBytes, &original,
                    &original_mode, true) != RelativeRead::kPresent) {
    return fail("cannot read the bottle's " + hive);
  }
  std::string edited = original;
  if (!ApplyRegistryValues(&edited, values, unix_time, reason)) return false;
  if (edited == original) return true;
  const auto backup = hive + ".playden-backup";
  if (!WriteAtomicAt(*directory, backup.c_str(), original, 0600,
                     false, true)) {
    return fail("cannot back up " + hive);
  }
  if (!WriteAtomicAt(*directory, hive.c_str(), edited, 0600, false, true)) {
    return fail("cannot write " + hive);
  }
  return true;
}

bool ApplyRegistrySettings(const EffectiveSettings& settings, const std::string& prefix_dir, std::int64_t unix_time,
                           std::string* reason) {
  return ApplyRegistryFileValues(RenderRegistryValues(settings), prefix_dir, unix_time, reason);
}

bool ApplyBottleRegistryValues(const std::vector<RegistryValue>& values, const std::string& prefix_dir,
                               std::int64_t unix_time, const std::string& hive, std::string* reason) {
  return ApplyRegistryFileValues(values, prefix_dir, unix_time, reason, hive);
}

bool ApplyMediaRegistry(const std::string& runtime_dir, const std::string& prefix_dir,
                        std::int64_t unix_time, std::string* reason) {
  const auto fail = [&](const char* why) { if (reason) *reason = why; return false; };
  auto directory = ManagedDirectory::Open(runtime_dir, DirectoryPolicy::kApprovedTree);
  if (!directory) return fail("cannot open kit media metadata");
  for (const auto* child : {"share", "wine", "media-build"}) {
    auto next = directory->OpenChild(child, DirectoryPolicy::kApprovedTree);
    if (!next) return errno == ENOENT ? true : fail("cannot open kit media metadata");
    directory = std::move(next);
  }
  std::string text;
  const auto state = ReadRegularAt(*directory, "media-registry.json", 1024 * 1024,
                                   &text);
  if (state == RelativeRead::kMissing) return true;
  if (state != RelativeRead::kPresent) return fail("cannot read kit media metadata");
  const auto root = storage_json::Parse(text);
  const auto* schema = root ? root->Find("schema") : nullptr;
  const auto* entries = root ? root->Find("values") : nullptr;
  if (!schema || schema->text() != "playden-media-registry/1" || !entries || !entries->is_array() ||
      entries->array().empty()) return fail("invalid kit media metadata");
  std::vector<RegistryValue> values;
  bool native = false, wow = false;
  for (const auto& entry : entries->array()) {
    const auto* key = entry.Find("key"), *name = entry.Find("name"), *data = entry.Find("data");
    if (!key || !key->is_string() || !name || !name->is_string() || !data || !data->is_string() || data->text().empty())
      return fail("invalid kit media value");
    const std::string base = "Software\\Classes\\";
    if (key->text().compare(0, base.size(), base) != 0) return fail("media value outside Classes");
    if (key->text().compare(base.size(), 12, "Wow6432Node\\") == 0) wow = true;
    else native = true;
    values.push_back({key->text(), name->text(), data->text(), name->text().empty()});
  }
  if (!native || !wow) return fail("media metadata lacks both registry views");
  // The DLLs go in first, so a failure leaves the registry as it was. Once a prefix exists, Wine loads a builtin DLL
  // only through its file in system32 or syswow64 (kit bottles never run wineboot), so a bottle made from an earlier
  // template also needs winegstreamer.dll itself. An existing copy is kept: Wine loads the runtime's builtin through it.
  constexpr std::size_t kMaxMediaDllBytes = 64u * 1024u * 1024u;
  for (const auto& [arch, folder] : {std::pair{"x86_64-windows", "system32"}, std::pair{"i386-windows", "syswow64"}}) {
    auto bottle = ManagedDirectory::Open(std::filesystem::path(prefix_dir) / "drive_c/windows" / folder,
                                         DirectoryPolicy::kApprovedTree);
    if (!bottle) return fail("cannot open the bottle's windows folders");
    struct stat existing {};
    if (bottle->InspectChild("winegstreamer.dll", &existing)) continue;
    if (errno != ENOENT) return fail("cannot inspect the bottle's winegstreamer.dll");
    auto source = ManagedDirectory::Open(std::filesystem::path(runtime_dir) / "lib/wine" / arch,
                                         DirectoryPolicy::kApprovedTree);
    std::string dll;
    if (!source || ReadRegularAt(*source, "winegstreamer.dll", kMaxMediaDllBytes, &dll) != RelativeRead::kPresent)
      return fail("the kit's winegstreamer.dll is missing");
    if (!WriteAtomicAt(*bottle, "winegstreamer.dll", dll, 0644, false, false, false))
      return fail("cannot copy winegstreamer.dll into the bottle");
  }
  if (!ApplyBottleRegistryValues(values, prefix_dir, unix_time, "system.reg", reason)) return false;
  return true;
}

bool ApplyD3D12Library(const std::string& runtime_dir, const std::string& template_dir, const std::string& prefix_dir,
                       bool vkd3d_proton, std::string* reason) {
  const auto fail = [&](std::string why) {
    if (reason) *reason = std::move(why);
    return false;
  };
  constexpr std::size_t kMaxD3D12DllBytes = 64u * 1024u * 1024u;
  // Left next to the DLLs while they are vkd3d-proton's, so switching back restores them whichever kit put them there.
  constexpr const char* kMarker = ".playden-vkd3d-proton";
  using Directory = decltype(ManagedDirectory::Open(std::filesystem::path(), DirectoryPolicy::kApprovedTree));
  struct Folder {
    Directory bottle;
    std::vector<std::pair<const char*, std::string>> writes;
    bool marked = false;
  };
  // Everything is read first, so a missing or unreadable file changes nothing.
  std::vector<Folder> folders;
  for (const auto& [arch, folder] : {std::pair{"x86_64-windows", "system32"}, std::pair{"i386-windows", "syswow64"}}) {
    Folder plan;
    plan.bottle = ManagedDirectory::Open(std::filesystem::path(prefix_dir) / "drive_c/windows" / folder,
                                         DirectoryPolicy::kApprovedTree);
    if (!plan.bottle) {
      if (errno == ENOENT) continue;  // no bottle yet: the next Play puts them in
      return fail("cannot open the bottle's windows folders");
    }
    struct stat marker {};
    plan.marked = plan.bottle->InspectChild(kMarker, &marker);
    if (!plan.marked && errno != ENOENT) return fail("cannot inspect the bottle's vkd3d-proton marker");
    auto source = ManagedDirectory::Open(std::filesystem::path(runtime_dir) / "lib/vkd3d-proton" / arch,
                                         DirectoryPolicy::kApprovedTree);
    if (!source && vkd3d_proton) return fail("the kit has no vkd3d-proton");
    Directory original;
    for (const char* name : {"d3d12.dll", "d3d12core.dll"}) {
      std::string ours, current, wanted;
      if (source && ReadRegularAt(*source, name, kMaxD3D12DllBytes, &ours) != RelativeRead::kPresent)
        return fail(std::string("the kit's vkd3d-proton ") + name + " is missing");
      const auto state = ReadRegularAt(*plan.bottle, name, kMaxD3D12DllBytes, &current);
      if (state == RelativeRead::kError) return fail(std::string("cannot read the bottle's ") + name);
      if (vkd3d_proton) {
        wanted = std::move(ours);
      } else {
        // Bottles switched before the marker existed are recognized by the current kit's bytes.
        if (!plan.marked && (!source || state != RelativeRead::kPresent || current != ours)) continue;
        if (!original)
          original = ManagedDirectory::Open(std::filesystem::path(template_dir) / "drive_c/windows" / folder,
                                            DirectoryPolicy::kApprovedTree);
        if (!original || ReadRegularAt(*original, name, kMaxD3D12DllBytes, &wanted) != RelativeRead::kPresent)
          return fail(std::string("the kit template's ") + name + " is missing");
      }
      if (state != RelativeRead::kPresent || current != wanted) plan.writes.emplace_back(name, std::move(wanted));
    }
    folders.push_back(std::move(plan));
  }
  for (auto& plan : folders) {
    for (const auto& [name, data] : plan.writes)
      if (!WriteAtomicAt(*plan.bottle, name, data, 0644, false, false, true))
        return fail(std::string("cannot copy ") + name + " into the bottle");
    if (vkd3d_proton && !plan.marked && !WriteAtomicAt(*plan.bottle, kMarker, "", 0644, false, false, true))
      return fail("cannot mark the bottle's vkd3d-proton files");
    if (!vkd3d_proton && plan.marked && !plan.bottle->RemoveRegularFile(kMarker))
      return fail("cannot remove the bottle's vkd3d-proton marker");
  }
  return true;
}

namespace {

// The data of one value in a Wine registry file, in Wine's text form ("\"...\"" for a string), or nothing when
// its key or the value isn't there or is written more than once.
std::optional<std::string> RegistryData(const std::string& text, const std::string& key, const std::string& name) {
  const auto header = RegistryHeader(key);
  const auto line_prefix = "\"" + name + "\"=";
  std::optional<std::string> data;
  bool inside = false, seen = false;
  for (std::size_t at = 0; at < text.size();) {
    const auto newline = text.find('\n', at);
    const auto next = newline == std::string::npos ? text.size() : newline + 1;
    auto content_end = newline == std::string::npos ? text.size() : newline;
    if (content_end > at && text[content_end - 1] == '\r') --content_end;
    const std::string_view line(text.data() + at, content_end - at);
    at = next;
    if (!line.empty() && line.front() == '[') {
      // Wine writes a key's header with its time after a space.
      inside = line.size() >= header.size() && RegistryEqual(line.substr(0, header.size()), header) &&
               (line.size() == header.size() || line[header.size()] == ' ');
      if (inside && seen) return std::nullopt;
      seen = seen || inside;
      continue;
    }
    if (!inside || line.size() < line_prefix.size() || !RegistryEqual(line.substr(0, line_prefix.size()), line_prefix))
      continue;
    if (data || line.back() == '\\') return std::nullopt;
    data = std::string(line.substr(line_prefix.size()));
  }
  return data;
}

}  // namespace

bool PointBottleAtKitMono(const std::string& template_dir, const std::string& prefix_dir, std::int64_t unix_time,
                          bool* changed, std::string* reason) {
  const auto fail = [&](std::string why) {
    if (reason) *reason = std::move(why);
    return false;
  };
  *changed = false;
  constexpr const char* kKey = "Software\\Wine\\Mono";
  constexpr const char* kName = "RuntimePath";
  constexpr std::size_t kMaxRegistryBytes = 32u * 1024u * 1024u;
  auto bottle = ManagedDirectory::Open(prefix_dir, DirectoryPolicy::kPrivateTraversable);
  if (!bottle) return errno == ENOENT ? true : fail("cannot open the bottle");
  std::string bottle_reg;
  const auto state = ReadRegularAt(*bottle, "user.reg", kMaxRegistryBytes, &bottle_reg, nullptr, true);
  if (state == RelativeRead::kMissing) return true;  // the bottle comes later
  if (state != RelativeRead::kPresent) return fail("cannot read the bottle's user.reg");
  const auto current = RegistryData(bottle_reg, kKey, kName);
  // The packaged template writes Z:\data\playden\kits\<kit id>\mono\wine-mono-<version>, escaped.
  static constexpr std::string_view kKitsFolder = R"("Z:\\data\\playden\\kits\\)";
  if (!current || current->size() <= kKitsFolder.size() ||
      !RegistryEqual(std::string_view(*current).substr(0, kKitsFolder.size()), kKitsFolder))
    return true;
  auto kit = ManagedDirectory::Open(template_dir, DirectoryPolicy::kApprovedTree);
  std::string template_reg;
  if (!kit || ReadRegularAt(*kit, "user.reg", kMaxRegistryBytes, &template_reg) != RelativeRead::kPresent)
    return fail("cannot read the kit template's user.reg");
  const auto wanted = RegistryData(template_reg, kKey, kName);
  if (!wanted || *wanted == *current) return true;
  if (!ApplyRegistryFileValues({{kKey, kName, *wanted}}, prefix_dir, unix_time, reason)) return false;
  *changed = true;
  return true;
}

bool ParseUe3InstallGuid(std::string_view install_info, std::string* guid) {
  static constexpr std::string_view open = "<InstallGuidString>", close = "</InstallGuidString>";
  const auto start = install_info.find(open);
  if (start == std::string_view::npos || install_info.find(open, start + 1) != std::string_view::npos) return false;
  const auto end = install_info.find(close, start + open.size());
  if (end == std::string_view::npos) return false;
  auto value = install_info.substr(start + open.size(), end - start - open.size());
  const auto space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
  while (!value.empty() && space(value.front())) value.remove_prefix(1);
  while (!value.empty() && space(value.back())) value.remove_suffix(1);
  if (value.size() != 36) return false;
  for (std::size_t index = 0; index < value.size(); ++index) {
    const char c = value[index];
    const bool hyphen = index == 8 || index == 13 || index == 18 || index == 23;
    const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    if (hyphen ? c != '-' : !hex) return false;
  }
  if (guid) guid->assign(value);
  return true;
}

Ue3EulaResult AcceptUe3Eula(const std::string& game_root, const std::vector<std::string>& programs,
                            const std::string& prefix_dir, std::int64_t unix_time) {
  Ue3EulaResult result;
  // Only a UDK layout: the launch program in Binaries/Win32 or Binaries/Win64, with
  // InstallInfo.xml and UnSetup.exe in Binaries.
  const auto udk_program = std::any_of(programs.begin(), programs.end(), [](const std::string& program) {
    std::string lower = program;
    std::replace(lower.begin(), lower.end(), '\\', '/');
    std::transform(lower.begin(), lower.end(), lower.begin(), AsciiLower);
    return lower.rfind("binaries/win32/", 0) == 0 || lower.rfind("binaries/win64/", 0) == 0;
  });
  if (!udk_program) return result;
  const auto binaries = std::filesystem::path(game_root) / "Binaries";
  struct stat info {};
  if (::lstat((binaries / "UnSetup.exe").c_str(), &info) != 0 || !S_ISREG(info.st_mode)) return result;
  if (::lstat((binaries / "InstallInfo.xml").c_str(), &info) != 0 || !S_ISREG(info.st_mode)) return result;
  result.found = true;
  const auto fail = [&](std::string why) {
    result.ok = false;
    result.message = std::move(why);
    return result;
  };
  std::string text;
  const int file = ::open((binaries / "InstallInfo.xml").c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (file < 0) return fail("cannot read Binaries/InstallInfo.xml");
  char buffer[4096];
  bool read_ok = true;
  while (true) {
    const auto count = ::read(file, buffer, sizeof(buffer));
    if (count < 0 && errno == EINTR) continue;
    if (count < 0 || text.size() + static_cast<std::size_t>(count) > 64u * 1024u) { read_ok = false; break; }
    if (count == 0) break;
    text.append(buffer, static_cast<std::size_t>(count));
  }
  ::close(file);
  if (!read_ok) return fail("cannot read Binaries/InstallInfo.xml");
  if (!ParseUe3InstallGuid(text, &result.guid)) return fail("Binaries/InstallInfo.xml has no install GUID");
  // UDK.exe opens HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\UDK-<guid> in the
  // 32-bit view, then the 64-bit one, and accepts any EULAAccepted value with a nonzero byte;
  // without it the game runs UnSetup.exe /EULA, which the PS5 cannot start.
  const auto key = "Microsoft\\Windows\\CurrentVersion\\Uninstall\\UDK-" + result.guid;
  const std::vector<RegistryValue> values{{"Software\\" + key, "EULAAccepted", "dword:00000001"},
                                          {"Software\\Wow6432Node\\" + key, "EULAAccepted", "dword:00000001"}};
  std::string before, reason;
  {
    auto directory = ManagedDirectory::Open(prefix_dir, DirectoryPolicy::kPrivateTraversable);
    if (directory) ReadRegularAt(*directory, "system.reg", 32u * 1024u * 1024u, &before, nullptr, true);
  }
  auto intended = before;
  const bool current = !before.empty() && ApplyRegistryValues(&intended, values, unix_time, nullptr) && intended == before;
  if (current) return result;
  if (!ApplyRegistryFileValues(values, prefix_dir, unix_time, &reason, "system.reg")) return fail(reason);
  result.written = true;
  return result;
}

}  // namespace playden::runtime

namespace playden::runtime {

const std::vector<std::string>& SettingsKitCapabilityNames() {
  // Sorted, as a kit manifest lists them.
  static const std::vector<std::string> names = {kKitDisplay120Hz, kKitAudioVolume, kKitDxvkConfig, kKitDxvkPresent,
                                                 kKitInputTuning,  kKitInputWheel,  kKitEnvironment};
  return names;
}

KitCapabilities KitCapabilitiesFromManifest(const std::vector<std::string>& capabilities, bool bottle_access) {
  const auto has = [&](const char* name) {
    return std::find(capabilities.begin(), capabilities.end(), name) != capabilities.end();
  };
  KitCapabilities kit;
  // DXVK reads dxvk.conf from the game's working directory without any runtime support (checked on the console
  // with DXVK 2.6.2: a frame cap there halved HL2's frame rate), so a kit shipping DXVK needs no extra capability.
  kit.dxvk_config = has(kKitDxvkConfig) || has(kKitDxvk);
  kit.dxvk_present = has(kKitDxvkPresent);
  kit.environment = has(kKitEnvironment);
  kit.input_tuning = has(kKitInputTuning);
  kit.input_wheel = has(kKitInputWheel);
  kit.audio_volume = has(kKitAudioVolume);
  kit.shared_sections = has(kKitSharedSections);
  kit.title_any_id = has(kKitTitleAnyId);
  kit.game_root_unowned = has(kKitGameRootUnowned);
  kit.bottle_profile_shared = has(kKitBottleProfileShared);
  kit.opengl_zink = has(kKitOpenGLZink);
  kit.wine_mono = has(kKitWineMono);
  kit.vkd3d_proton = has(kKitVkd3dProton);
  kit.display_120hz = has(kKitDisplay120Hz);
  kit.profile_version_2 = has(kKitProfileVersion2);
  kit.registry = bottle_access;
  return kit;
}

KitCapabilities KitForTitleRuntime(KitCapabilities kit, bool title_runtime_current) {
  if (!title_runtime_current && kit.display_120hz) {
    kit.display_120hz = false;
    kit.display_120hz_needs_update = true;
  }
  if (!title_runtime_current && kit.profile_version_2) {
    kit.profile_version_2 = false;
    kit.profile_version_2_needs_update = true;
  }
  return kit;
}

bool BuildSettingsLaunchPlan(const SettingsLaunchInput& input, SettingsLaunchPlan* plan, std::string* reason) {
  const auto fail = [&](std::string why) {
    if (reason) *reason = std::move(why);
    return false;
  };
  if (!input.recipe || !plan) return fail("no recipe or plan output");
  if (input.record.preset_id.empty()) {
    if (input.preset && (input.preset->id != "playden-default" || !input.preset->settings.empty()))
      return fail("default record does not match the supplied preset");
  } else if (!input.preset || input.preset->id != input.record.preset_id) {
    return fail("record preset is missing or does not match the supplied preset");
  }
  SettingsLaunchPlan out;
  out.app_id = input.recipe->app_id;
  out.recipe_id = input.recipe->id;
  out.recipe_version = std::to_string(input.recipe->version);
  EffectiveSettings settings;
  SettingsError error;
  if (!BuildEffectiveSettings(*input.recipe, input.preset, input.record.overrides, input.kit, &settings, &error))
    return fail(error.setting_id.empty() ? error.message : error.setting_id + ": " + error.message);
  ProsperoProfile profile;
  if (!RenderProsperoProfile(*input.recipe, settings, input.target, &profile, reason)) return false;
  out.profile_id = profile.id;
  out.profile_bytes = std::move(profile.text);
  out.settings_revision = input.record.revision;
  if (!settings.launch) return fail("no launch option");
  out.launch_entry = *settings.launch;
  out.launch_entry_id = out.launch_entry.id;

  // Every preset the profile names must ship with it, or the runtime refuses the profile.
  const auto& preset = settings.values.at("controller_preset").value;
  if (!preset.empty() && preset != "none") {
    if (preset.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-_") != std::string::npos)
      return fail("controller preset name is not valid");
    auto directory = ManagedDirectory::Open(input.runtime_dir + "/input",
                                            DirectoryPolicy::kApprovedTree);
    if (!directory) return fail("controller preset " + preset + " is not available");
    std::string bytes;
    const auto file = preset + ".input";
    const auto read = ReadRegularAt(*directory, file.c_str(), kProsperoMaxProfileBytes, &bytes);
    if (read != RelativeRead::kPresent || bytes.find("[input]") == std::string::npos)
      return fail("controller preset " + preset + " is not available");
    out.input_presets.emplace_back(file, std::move(bytes));
  }
  out.dxvk_config = RenderDxvkConfig(settings);
  out.registry = RenderRegistryValues(settings);
  *plan = std::move(out);
  return true;
}

bool ApplySettingsFiles(const SettingsLaunchPlan& plan, const std::string& install_root, const std::string& prefix_dir,
                        std::int64_t unix_time, std::string* reason) {
  const auto fail = [&](std::string why) {
    if (reason) *reason = std::move(why);
    return false;
  };
  if (plan.launch_entry.id.empty() || plan.launch_entry.id != plan.launch_entry_id)
    return fail("no matching launch option");
  auto directory = OpenLaunchDirectory(plan.launch_entry, install_root);
  if (!directory) return fail("launch working folder is not a safe directory");
  std::string existing;
  mode_t existing_mode = 0644;
  const auto read = ReadRegularAt(*directory, "dxvk.conf", 256u * 1024u, &existing, &existing_mode);
  const bool has_config = read == RelativeRead::kPresent;
  const auto close_fail = [&](std::string why) { return fail(std::move(why)); };
  if (read == RelativeRead::kError) return close_fail("cannot read dxvk.conf");
  if (has_config && existing.compare(0, std::char_traits<char>::length(kDxvkHeader), kDxvkHeader) != 0)
    return close_fail("the game folder has its own dxvk.conf; Playden will not replace it");
  // This is a stopped-title file operation, not a cross-directory transaction.
  // Restore config bytes/mode on registry failure and surface failed rollback.
  // The integration owner journals readiness and reconciles any interrupted apply.
  bool changed = false;
  if (plan.dxvk_config.empty()) {
    if (has_config) {
      if (!directory->RemoveRegularFile("dxvk.conf")) return close_fail("cannot remove dxvk.conf");
      if (!directory->Sync()) return close_fail("cannot sync dxvk.conf removal; reconciliation required");
      changed = true;
    }
  } else if (!has_config || existing != plan.dxvk_config) {
    if (!WriteAtomicAt(*directory, "dxvk.conf", plan.dxvk_config, 0644, true))
      return close_fail("cannot commit dxvk.conf; reconciliation required");
    changed = true;
  }
  if (!plan.registry.empty() && !ApplyRegistryFileValues(plan.registry, prefix_dir, unix_time, reason)) {
    const auto registry_reason = reason ? *reason : std::string("registry apply failed");
    if (changed) {
      const bool restored = has_config
          ? WriteAtomicAt(*directory, "dxvk.conf", existing, existing_mode)
          : (directory->RemoveRegularFile("dxvk.conf") && directory->Sync());
      if (!restored)
        return close_fail("settings rollback failed; reconciliation required: " + registry_reason);
    }
    return close_fail("settings apply failed; reconciliation required: " + registry_reason);
  }
  return true;
}

}  // namespace playden::runtime

namespace playden::runtime {

CommunityRecipe BuildCommunityRecipe(const GameRecipe& recipe, const EffectiveSettings& settings) {
  CommunityRecipe out;
  auto& shared = out.recipe;
  shared.id = "community-" + std::to_string(recipe.app_id);
  shared.version = 1;
  shared.app_id = recipe.app_id;
  shared.name = recipe.name;
  if (settings.launch) {
    shared.launch_entries = {*settings.launch};
    out.executable = settings.launch->executable;
  }
  const auto value = [&](const char* id) -> const EffectiveValue& { return settings.values.at(id); };
  shared.arguments = value("arguments").value;
  shared.resolution_arguments = recipe.resolution_arguments;
  shared.dll_overrides = settings.dll_overrides;
  shared.architecture = settings.architecture.empty() ? recipe.architecture : settings.architecture;
  shared.runtime = recipe.runtime;
  shared.prefix = recipe.prefix;
  shared.graphics = settings.graphics;
  // What the files showed, without the graphics value: that has its own field.
  shared.detected = recipe.detected.substr(0, recipe.detected.find(" graphics="));
  // The recipe fields above carry the program, arguments and overrides; the rest are defaults.
  static const std::set<std::string> fields = {"launch_option", "program", "program_folder", "program_arguments",
                                               "arguments", "architecture", "dll_overrides", "library_overrides",
                                               "graphics", "start_check", "logging"};
  for (const auto& definition : SettingsCatalogue()) {
    const auto& row = settings.values.at(definition.id);
    if (fields.count(definition.id) || !row.available || row.source == ValueSource::kPlayden ||
        row.value == definition.playden_default)
      continue;
    shared.defaults[definition.id] = row.value;
  }
  for (const auto& id : recipe.locked)
    if (!settings.values.at(id).unlocked) shared.locked.insert(id);
  // What the bottle gets and what the kit must provide stay with the game.
  shared.registry = recipe.registry;
  shared.requirements = recipe.requirements;
  out.status = "untested";
  return out;
}

std::string SerializeCommunityRecipe(const CommunityRecipe& community) {
  const auto quoted = [](const std::string& text) { return "\"" + json::escape(text) + "\""; };
  const auto& recipe = community.recipe;
  // One field per line, so a change to a shared recipe reads well in a pull request.
  std::string text = "{\n  \"recipe_id\": " + quoted(recipe.id) + ",\n  \"recipe_version\": " +
                     std::to_string(recipe.version) + ",\n  \"app_id\": " + std::to_string(recipe.app_id) +
                     ",\n  \"name\": " + quoted(recipe.name) + ",\n  \"launch_entries\": [";
  for (std::size_t index = 0; index < recipe.launch_entries.size(); ++index) {
    const auto& entry = recipe.launch_entries[index];
    text += std::string(index ? "," : "") + "\n    {\"id\": " + quoted(entry.id) + ", \"label\": " + quoted(entry.label) +
            ", \"executable\": " + quoted(entry.executable) + ", \"working_directory\": " +
            quoted(entry.working_directory) + ", \"arguments\": " + quoted(entry.arguments) +
            (entry.in_install_root ? ", \"in_install_root\": true" : "") + "}";
  }
  text += "\n  ],\n  \"arguments\": " + quoted(recipe.arguments) + ",\n  \"resolution_arguments\": " +
          quoted(recipe.resolution_arguments) + ",\n  \"dll_overrides\": " + quoted(recipe.dll_overrides) +
          ",\n  \"architecture\": " + quoted(recipe.architecture) + ",\n  \"runtime\": " + quoted(recipe.runtime) +
          ",\n  \"prefix\": " + quoted(recipe.prefix) + ",\n  \"graphics\": " + quoted(recipe.graphics) +
          (recipe.detected.empty() ? std::string() : ",\n  \"detected\": " + quoted(recipe.detected)) +
          ",\n  \"defaults\": {";
  bool first = true;
  for (const auto& [id, value] : recipe.defaults) {
    text += std::string(first ? "" : ",") + "\n    " + quoted(id) + ": " + quoted(value);
    first = false;
  }
  text += first ? "},\n  \"locked\": [" : "\n  },\n  \"locked\": [";
  first = true;
  for (const auto& id : recipe.locked) {
    text += std::string(first ? "" : ", ") + quoted(id);
    first = false;
  }
  text += "]";
  if (!recipe.requirements.empty()) {
    text += ",\n  \"requirements\": {";
    first = true;
    for (const auto& [name, value] : recipe.requirements) {
      text += std::string(first ? "" : ", ") + quoted(name) + ": " + quoted(value);
      first = false;
    }
    text += "}";
  }
  if (!recipe.registry.empty()) {
    // Grouped by key as ParseGameRecipe reads it; data holds the quoted string.
    std::map<std::string, std::vector<const RegistryValue*>> keys;
    for (const auto& value : recipe.registry) keys[value.key].push_back(&value);
    text += ",\n  \"registry\": {";
    first = true;
    for (const auto& [key, values] : keys) {
      text += std::string(first ? "" : ",") + "\n    " + quoted(key) + ": {";
      for (std::size_t index = 0; index < values.size(); ++index)
        text += std::string(index ? ", " : "") + quoted(values[index]->name) + ": " +
                quoted(values[index]->data.substr(1, values[index]->data.size() - 2));
      text += "}";
      first = false;
    }
    text += "\n  }";
  }
  text += ",\n  \"steam_appid\": " + std::to_string(recipe.app_id) + ",\n  \"executable\": " +
          quoted(community.executable) + ",\n  \"community\": {\n    \"format_version\": 1,\n    \"min_kit\": " +
          quoted(community.min_kit) + ",\n    \"tested_firmware\": " + quoted(community.tested_firmware) +
          ",\n    \"tested_date\": " + quoted(community.tested_date) + ",\n    \"status\": " +
          quoted(community.status) + ",\n    \"notes\": " + quoted(community.notes) + ",\n    \"author\": " +
          quoted(community.author);
  if (!community.base_repository.empty())
    text += ",\n    \"base\": {\"repository\": " + quoted(community.base_repository) + ", \"path\": " +
            quoted(community.base_path) + ", \"revision\": " + quoted(community.base_revision) + "}";
  return text + "\n  }\n}\n";
}

bool ParseCommunityRecipe(std::string_view text, CommunityRecipe* community, std::string* reason) {
  const auto fail = [&](std::string why) {
    if (reason) *reason = std::move(why);
    return false;
  };
  CommunityRecipe out;
  if (!ParseGameRecipe(text, &out.recipe, reason)) return false;
  const auto root = json::parse(text);
  std::uint64_t appid = 0;
  if (!ExactUnsigned(root->get("steam_appid"), std::numeric_limits<std::uint32_t>::max(), &appid, false) ||
      appid != out.recipe.app_id)
    return fail("steam_appid must match app_id");
  if (!StringField(*root, "executable", &out.executable)) return fail("community recipe needs executable");
  const auto* meta = root->get("community");
  std::uint64_t format = 0;
  if (!meta || !meta->object() || !ExactUnsigned(meta->get("format_version"), 1000, &format) || format != 1)
    return fail("community recipe needs community.format_version 1");
  if (!StringField(*meta, "min_kit", &out.min_kit, false) ||
      !StringField(*meta, "tested_firmware", &out.tested_firmware, false) ||
      !StringField(*meta, "tested_date", &out.tested_date, false) || !StringField(*meta, "status", &out.status) ||
      !StringField(*meta, "notes", &out.notes, false) || !StringField(*meta, "author", &out.author, false))
    return fail("community metadata has a field of the wrong type");
  if (out.status != "works" && out.status != "playable" && out.status != "broken" && out.status != "untested")
    return fail("status must be works, playable, broken or untested");
  if (const auto* base = meta->get("base")) {
    if (!base->object() || !StringField(*base, "repository", &out.base_repository) ||
        !StringField(*base, "path", &out.base_path) || !StringField(*base, "revision", &out.base_revision, false))
      return fail("base needs repository and path");
  }
  for (const auto& entry : out.recipe.launch_entries)
    if (entry.executable == out.executable) {
      *community = std::move(out);
      return true;
    }
  return fail("executable must be one of the launch entries");
}

bool CommunityPresetSupported(const CommunityPreset& community, const KitCapabilities& kit) {
  for (const auto& feature : community.requires_prospero_win) {
    if (feature == "profile-environment") { if (!kit.environment) return false; }
    else if (feature == "input-tuning") { if (!kit.input_tuning) return false; }
    else if (feature == "input-wheel") { if (!kit.input_wheel) return false; }
    else if (feature == "audio-volume") { if (!kit.audio_volume) return false; }
    // A runtime name, not a settings one: its values apply once the game's title runs the kit's runtime, like the
    // rows they set (KitForTitleRuntime), so a title that waits for its update may choose the preset too.
    else if (feature == "display-120hz") { if (!kit.display_120hz && !kit.display_120hz_needs_update) return false; }
    // Legacy PR references are informational sidecar provenance. Unknown feature
    // names or version requirements need verified support before a preset is shown.
    else if (feature.size() < 2 || feature.front() != '#' ||
             !std::all_of(feature.begin() + 1, feature.end(),
                          [](char byte) { return byte >= '0' && byte <= '9'; })) return false;
  }
  return true;
}

}  // namespace playden::runtime
