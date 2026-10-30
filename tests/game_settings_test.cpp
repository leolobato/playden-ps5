#include "runtime/game_settings.h"
#include "steam_id.h"
#include "runtime/game_probe.h"
#include "runtime/install_script.h"
#include "runtime/settings_publication.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(cond)                                                                     \
  do {                                                                                  \
    if (!(cond)) {                                                                      \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);     \
      std::exit(1);                                                                     \
    }                                                                                   \
  } while (0)

namespace {

using namespace playden::runtime;

std::string ReadText(const std::filesystem::path& path) {
  std::ifstream file(path);
  CHECK(file.good());
  return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

const std::string& Value(const EffectiveSettings& settings, const char* id) { return settings.values.at(id).value; }

// A minimal PE header: "MZ", e_lfanew, "PE\0\0", machine and optional-header magic.
void WritePe(const std::filesystem::path& path, bool pe64) {
  std::filesystem::create_directories(path.parent_path());
  std::string bytes(0x80 + 26, '\0');
  bytes[0] = 'M'; bytes[1] = 'Z'; bytes[0x3c] = 0x40;
  bytes[0x40] = 'P'; bytes[0x41] = 'E';
  bytes[0x44] = pe64 ? 0x64 : 0x4c; bytes[0x45] = pe64 ? static_cast<char>(0x86) : 0x01;
  bytes[0x40 + 24] = 0x0b; bytes[0x40 + 25] = pe64 ? 0x02 : 0x01;
  std::ofstream(path, std::ios::binary) << bytes;
}

}  // namespace

int main(int argc, char** argv) {
  const std::filesystem::path source = argc > 1 ? argv[1] : ".";
  std::string reason;

  GameRecipe hl2;
  CHECK(ParseGameRecipe(ReadText(source / "runtime/recipes/steam-220-hl2-v1.json"), &hl2, &reason));
  // What the kit's template must provide is read and written back (UnmetRecipeRequirements checks it).
  CHECK(hl2.requirements == (std::map<std::string, std::string>{{"audio", "ps5"}}));
  {
    GameRecipe reread;
    CHECK(ParseGameRecipe(SerializeGameRecipe(hl2), &reread, &reason) && reread.requirements == hl2.requirements);
    CHECK(!ParseGameRecipe(R"({"recipe_id":"x","recipe_version":1,"app_id":1,"launch_entries":[{"id":"a","label":"A","executable":"a.exe"}],"requirements":{"audio":1}})", &reread, &reason));
  }
  std::vector<SettingsPreset> presets;
  CHECK(ParseSettingsPresets(ReadText(source / "runtime/presets.json"), &presets, &reason));
  CHECK(presets.size() == 4 && presets.front().id == "playden-default");
  const SettingsPreset* mouse = nullptr;
  for (const auto& preset : presets)
    if (preset.id == "mouse-driven") mouse = &preset;
  CHECK(mouse);

  // The sheet lists rows in catalogue order, grouped by topic: each group in one run, in SettingGroup order.
  {
    std::vector<std::string> ids;
    for (const auto& definition : SettingsCatalogue())
      if (definition.group != SettingGroup::kButtonMapping) ids.push_back(definition.id);
    const std::vector<std::string> expected = {
        "launch_option", "program", "extra_arguments",
        "resolution", "refresh", "frame_limit", "framegen", "vsync", "performance_overlay", "overlay_detail", "scaling",
        "window_view",
        "controller", "controller_preset",
        "pointer_stick", "pointer_speed", "stick_deadzone", "rumble", "invert_stick", "trigger_deadzone", "wheel_stick",
        "volume",
        "start_check",
        "graphics", "d3d12", "pixel_shader_waves", "frame_latency", "reported_gpu", "reported_vram",
        "steam_features", "windows_version", "text_scale", "locale", "cpu_topology", "logging",
        "environment", "program_folder", "program_arguments", "arguments", "architecture", "dll_overrides",
        "library_overrides", "registry_dll_overrides"};
    CHECK(ids == expected);
    for (std::size_t index = 1; index < SettingsCatalogue().size(); ++index)
      CHECK(SettingsCatalogue()[index - 1].group <= SettingsCatalogue()[index].group);
    CHECK(FindSetting("refresh")->group == SettingGroup::kDisplay &&
          FindSetting("button.cross")->group == SettingGroup::kButtonMapping &&
          FindSetting("rumble")->group == SettingGroup::kControllerTuning);
  }

  // Every catalogue choice default is one of its own choices.
  for (const auto& definition : SettingsCatalogue()) {
    if (definition.kind != SettingKind::kChoice || definition.choices.empty()) continue;
    CHECK(ValidateSettingValue(hl2, definition.id, definition.playden_default, &reason));
  }

  // DefaultSkipsPlayerProgram: a recipe generated for the player's program lists it first; without
  // that choice the default is Steam's entry, as the next preparation makes it, and the choice still finds it.
  {
    GameRecipe chosen = hl2;
    chosen.launch_entries = {{kProgramEntryId, "FalloutNV.exe", "FalloutNV.exe", "", ""},
                             {"launch-1", "Launcher", "FalloutNVLauncher.exe", "", ""}};
    EffectiveSettings values;
    SettingsError problem;
    CHECK(BuildEffectiveSettings(chosen, nullptr, {}, KitCapabilities{}, &values, &problem));
    CHECK(values.launch && values.launch->id == "launch-1");
    CHECK(BuildEffectiveSettings(chosen, nullptr, {{"program", "FalloutNV.exe"}}, KitCapabilities{}, &values, &problem));
    CHECK(values.launch && values.launch->id == kProgramEntryId && values.launch->executable == "FalloutNV.exe");
    chosen.launch_entries.resize(1);
    CHECK(BuildEffectiveSettings(chosen, nullptr, {}, KitCapabilities{}, &values, &problem));
    CHECK(values.launch && values.launch->id == kProgramEntryId);
  }

  // RecipeArgumentsPreserved: with no overrides the HL2 recipe gives the owner-verified profile.
  const ProfileTarget target{"playden-steam-220", "/data/homebrew/FAKE77031/games/220"};
  EffectiveSettings settings;
  SettingsError error;
  CHECK(BuildEffectiveSettings(hl2, nullptr, {}, KitCapabilities{}, &settings, &error));
  CHECK(settings.arguments == "-game hl2 -steam -w 1920 -h 1080 -novid +exec 360controller.cfg +joystick 1");
  CHECK(settings.dll_overrides == "d3d9=n");
  CHECK(settings.values.at("resolution").source == ValueSource::kRecipe);
  CHECK(settings.values.at("performance_overlay").source == ValueSource::kPlayden);
  CHECK(!settings.values.at("scaling").available);          // DXVK output ignores scaling today
  CHECK(!settings.values.at("refresh").available);          // OpenGL only
  CHECK(!settings.values.at("frame_limit").available);      // needs the dxvk.conf probe
  CHECK(!settings.values.at("windows_version").available);  // needs registry overlays
  CHECK(!settings.values.at("pointer_speed").available);    // no pointer stick
  ProsperoProfile profile;
  CHECK(RenderProsperoProfile(hl2, settings, target, &profile, &reason));
  CHECK(profile.text ==
        "; Generated by Playden from recipe steam-220-hl2-v1 v1. Edit settings in Playden.\n"
        "[application]\n"
        "id = playden-steam-220\n"
        "name = Half-Life 2\n"
        "executable = Z:\\data\\homebrew\\FAKE77031\\games\\220\\hl2.exe\n"
        "working_directory = Z:\\data\\homebrew\\FAKE77031\\games\\220\n"
        "arguments = -game hl2 -steam -w 1920 -h 1080 -novid +exec 360controller.cfg +joystick 1\n"
        "dll_overrides = d3d9=n\n"
        "prefix = game\n"
        "runtime = wine-wow64\n"
        "architecture = pe32\n"
        "graphics = dxvk\n"
        "\n[display]\n"
        "desktop = 1920x1080\n"
        "view = window\n"
        "show_fps = false\n"
        "\n[input]\n"
        "mode = xinput\n"
        "mouse = none\n"
        "options = escape\n");
  CHECK(RenderDxvkConfig(settings).empty());

  // Overrides: resolution fills the recipe's -w/-h; extra arguments and libraries append.
  std::map<std::string, std::string> overrides = {{"resolution", "2560x1440"},
                                                  {"extra_arguments", "-console \"+map d1_trainstation_01\""},
                                                  {"library_overrides", "winmm=n,b"},
                                                  {"performance_overlay", "on"},
                                                  {"logging", "verbose"},
                                                  {"button.touchpad", "f5"},
                                                  {"graphics", "gdi"}};  // locked: ignored
  CHECK(BuildEffectiveSettings(hl2, nullptr, overrides, KitCapabilities{}, &settings, &error));
  CHECK(settings.arguments ==
        "-game hl2 -steam -w 2560 -h 1440 -novid +exec 360controller.cfg +joystick 1 -console \"+map d1_trainstation_01\"");
  CHECK(settings.dll_overrides == "d3d9=n;winmm=n,b");
  CHECK(settings.graphics == "dxvk" && settings.values.at("graphics").locked);
  CHECK(Value(settings, "resolution") == "2560x1440" && settings.values.at("resolution").source == ValueSource::kUser);
  CHECK(RenderProsperoProfile(hl2, settings, target, &profile, &reason));
  CHECK(profile.text.find("desktop = 2560x1440\n") != std::string::npos);
  CHECK(profile.text.find("show_fps = true\n") != std::string::npos);
  CHECK(profile.text.find("touchpad = f5\n") != std::string::npos);
  CHECK(profile.text.find("[debug]\nwinedebug = ") != std::string::npos);
  CHECK(CheckProsperoProfile(profile.text, &reason));
  // Input logging traces Raw Input, DirectInput and winmm joysticks, in a channel list the profile accepts.
  CHECK(BuildEffectiveSettings(hl2, nullptr, {{"logging", "input"}}, KitCapabilities{}, &settings, &error));
  CHECK(RenderProsperoProfile(hl2, settings, target, &profile, &reason));
  CHECK(profile.text.find("[debug]\nwinedebug = err+all,warn+all,+loaddll,+process,+rawinput,+dinput,+winmm,+ntoskrnl,+plugplay,+hid,+hidclass\n") !=
        std::string::npos);
  CHECK(CheckProsperoProfile(profile.text, &reason));
  CHECK(BuildEffectiveSettings(hl2, nullptr, {{"extra_arguments", "-name \"John  Doe\""}}, KitCapabilities{},
                               &settings, &error));
  std::vector<std::string> spaced_arguments;
  CHECK(SplitProsperoArguments(settings.arguments, &spaced_arguments));
  CHECK(spaced_arguments.back() == "John  Doe");
  CHECK(BuildEffectiveSettings(hl2, nullptr, {{"extra_arguments", R"(-path C:\"Program  Files\")"}},
                               KitCapabilities{}, &settings, &error));
  CHECK(SplitProsperoArguments(settings.arguments, &spaced_arguments));
  CHECK(spaced_arguments.back() == "C:\\Program  Files\\");

  // An explicit "off" is a value, not an absence: it beats a preset that turns it on.
  SettingsPreset overlay{"overlay", "Overlay", "", "", {{"performance_overlay", "on"}}};
  CHECK(BuildEffectiveSettings(hl2, &overlay, {}, KitCapabilities{}, &settings, &error));
  CHECK(Value(settings, "performance_overlay") == "on" && settings.values.at("performance_overlay").source == ValueSource::kPreset);
  CHECK(BuildEffectiveSettings(hl2, &overlay, {{"performance_overlay", "off"}}, KitCapabilities{}, &settings, &error));
  CHECK(Value(settings, "performance_overlay") == "off" && settings.values.at("performance_overlay").source == ValueSource::kUser);

  // Presets beat recipe defaults; the mouse preset turns on the pointer and its speed.
  CHECK(BuildEffectiveSettings(hl2, mouse, {}, KitCapabilities{}, &settings, &error));
  CHECK(Value(settings, "controller") == "keyboard" && settings.values.at("controller").source == ValueSource::kPreset);
  CHECK(settings.values.at("pointer_speed").available && Value(settings, "pointer_speed") == "900");
  CHECK(RenderProsperoProfile(hl2, settings, target, &profile, &reason));
  CHECK(profile.text.find("preset = mouse\nmode = keyboard\nmouse = left_stick\nmouse_speed = 900\n") != std::string::npos);
  // Choosing only the Controller preset row leaves the input file's mode, stick and speed in charge:
  // Playden's defaults (Xbox controller, no pointer stick) must not override them.
  CHECK(BuildEffectiveSettings(hl2, nullptr, {{"controller_preset", "mouse"}}, KitCapabilities{}, &settings, &error));
  CHECK(RenderProsperoProfile(hl2, settings, target, &profile, &reason));
  CHECK(profile.text.find("[input]\npreset = mouse\n") != std::string::npos);
  CHECK(profile.text.find("\nmode = ") == std::string::npos && profile.text.find("\nmouse = ") == std::string::npos &&
        profile.text.find("\nmouse_speed = ") == std::string::npos);
  CHECK(CheckProsperoProfile(profile.text, &reason));
  // A row the player set still wins over the preset.
  CHECK(BuildEffectiveSettings(hl2, nullptr, {{"controller_preset", "mouse"}, {"pointer_stick", "right_stick"}},
                               KitCapabilities{}, &settings, &error));
  CHECK(RenderProsperoProfile(hl2, settings, target, &profile, &reason));
  CHECK(profile.text.find("\nmouse = right_stick\n") != std::string::npos && profile.text.find("\nmode = ") == std::string::npos);
  // The sheet shows the preset's own values, and Pointer speed can be tuned; a slower speed reaches the profile.
  CHECK(BuildEffectiveSettings(hl2, nullptr, {{"controller_preset", "mouse"}}, KitCapabilities{}, &settings, &error));
  CHECK(Value(settings, "controller") == "keyboard" && Value(settings, "pointer_stick") == "left_stick" &&
        Value(settings, "pointer_speed") == "900" && settings.values.at("pointer_speed").available &&
        settings.values.at("pointer_stick").source == ValueSource::kPreset);
  CHECK(BuildEffectiveSettings(hl2, nullptr, {{"controller_preset", "mouse"}, {"pointer_speed", "600"}}, KitCapabilities{},
                               &settings, &error));
  CHECK(RenderProsperoProfile(hl2, settings, target, &profile, &reason));
  CHECK(profile.text.find("\nmouse_speed = 600\n") != std::string::npos && profile.text.find("\nmouse = ") == std::string::npos);
  // Playden's table of preset rows matches the shipped input files.
  for (const auto& [name, rows] : ControllerPresetInputs()) {
    const auto file = ReadText(source / ("runtime/input/" + name + ".input"));
    const std::map<std::string, std::string> keys = {{"controller", "mode"}, {"pointer_stick", "mouse"}, {"pointer_speed", "mouse_speed"}};
    for (const auto& [id, key] : keys) {
      const auto line = "\n" + key + " = ";
      const auto at = file.find(line);
      const std::string in_file = at == std::string::npos ? "" : file.substr(at + line.size(), file.find('\n', at + 1) - at - line.size());
      const auto row = rows.find(id);
      const std::string in_table = row == rows.end() ? "" : row->second;
      if (in_file != in_table && !(in_file.empty() && in_table == "none"))
        std::fprintf(stderr, "%s.input %s: file '%s' table '%s'\n", name.c_str(), key.c_str(), in_file.c_str(), in_table.c_str());
      CHECK(in_file == in_table || (in_file.empty() && in_table == "none"));
    }
  }

  // dxvk.conf settings apply only when the kit reads dxvk.conf.
  KitCapabilities dxvk_kit;
  dxvk_kit.dxvk_config = true;
  overrides = {{"frame_limit", "30"}, {"vsync", "off"}, {"frame_latency", "1"}, {"reported_vram", "4096"}};
  // VSync off needs a display that honors DXVK's present interval; the rest apply today.
  CHECK(BuildEffectiveSettings(hl2, nullptr, overrides, dxvk_kit, &settings, &error));
  CHECK(settings.values.at("frame_limit").available && !settings.values.at("vsync").available &&
        settings.values.at("frame_latency").available && settings.values.at("reported_vram").available);
  CHECK(RenderDxvkConfig(settings).find("presentInterval") == std::string::npos);
  dxvk_kit.dxvk_present = true;
  CHECK(BuildEffectiveSettings(hl2, nullptr, overrides, KitCapabilities{}, &settings, &error));
  CHECK(RenderDxvkConfig(settings).empty());
  CHECK(BuildEffectiveSettings(hl2, nullptr, overrides, dxvk_kit, &settings, &error));
  const auto config = RenderDxvkConfig(settings);
  CHECK(config.find("dxgi.maxFrameRate = 30\nd3d9.maxFrameRate = 30\n") != std::string::npos);
  CHECK(config.find("dxgi.syncInterval = 0\nd3d9.presentInterval = 0\n") != std::string::npos);
  CHECK(config.find("d3d9.maxFrameLatency = 1\n") != std::string::npos);
  CHECK(config.find("dxgi.maxDeviceMemory = 4096\nd3d9.maxAvailableMemory = 4096\n") != std::string::npos);

  // With dxvk.conf, a DXVK overlay is written as dxvk.hud and show_fps stays false.
  CHECK(BuildEffectiveSettings(hl2, nullptr, {{"performance_overlay", "on"}, {"overlay_detail", "frametimes"}}, dxvk_kit,
                               &settings, &error));
  CHECK(RenderDxvkConfig(settings).find("dxvk.hud = fps,frametimes\n") != std::string::npos);
  CHECK(RenderProsperoProfile(hl2, settings, target, &profile, &reason));
  CHECK(profile.text.find("show_fps = false\n") != std::string::npos);
  CHECK(BuildEffectiveSettings(hl2, nullptr, {{"overlay_detail", "full"}}, dxvk_kit, &settings, &error));
  CHECK(!settings.values.at("overlay_detail").available);   // overlay off
  CHECK(RenderDxvkConfig(settings).find("dxvk.hud") == std::string::npos);
  CHECK(BuildEffectiveSettings(hl2, nullptr, {{"performance_overlay", "on"}}, KitCapabilities{}, &settings, &error));
  CHECK(RenderProsperoProfile(hl2, settings, target, &profile, &reason));
  CHECK(profile.text.find("show_fps = true\n") != std::string::npos);  // no dxvk.conf: the title's HUD

  // OpenGL games name their rate (60 without a 120 Hz runtime); the OpenGL worker thread row is gone.
  auto opengl = hl2;
  opengl.graphics = "opengl";
  CHECK(!FindSetting("opengl_thread"));
  CHECK(BuildEffectiveSettings(opengl, nullptr, {{"refresh", "120"}, {"scaling", "integer"}}, KitCapabilities{}, &settings, &error));
  CHECK(RenderProsperoProfile(opengl, settings, target, &profile, &reason));
  CHECK(profile.text.find("scaling = integer\n") != std::string::npos);
  CHECK(profile.text.find("show_fps = false\nrefresh = 60\n\n[input]") != std::string::npos);
  CHECK(profile.text.find("opengl_thread") == std::string::npos && profile.text.find("frame_rate") == std::string::npos);
  KitCapabilities zink;
  zink.opengl_zink = true;
  CHECK(BuildEffectiveSettings(opengl, nullptr, {{"refresh", "120"}}, zink, &settings, &error));
  CHECK(!settings.values.at("refresh").available);
  CHECK(settings.values.at("refresh").reason == "Needs a runtime kit with 120 Hz output.");
  CHECK(RenderProsperoProfile(opengl, settings, target, &profile, &reason));
  CHECK(profile.text.find("refresh = 60\n") != std::string::npos);

  // 120 Hz output and Frame limit with a display-120hz runtime.
  {
    const auto display = KitCapabilitiesFromManifest({kKitDxvk, kKitOpenGLZink, kKitVkd3dProton, kKitDisplay120Hz}, true);
    CHECK(display.display_120hz && !display.display_120hz_needs_update);
    CHECK(!KitCapabilitiesFromManifest({kKitDxvk, kKitOpenGLZink}, true).display_120hz);
    // A capability this build does not know is ignored.
    CHECK(!KitCapabilitiesFromManifest({kKitDxvk, "display-240hz"}, true).display_120hz);
    CHECK(std::find(SettingsKitCapabilityNames().begin(), SettingsKitCapabilityNames().end(), kKitDisplay120Hz) !=
          SettingsKitCapabilityNames().end());
    EffectiveSettings shown;
    // The row: DXVK and OpenGL, not GDI; the reason names what is missing.
    for (const char* graphics : {"dxvk", "opengl"}) {
      auto game = hl2;
      game.graphics = graphics;
      CHECK(BuildEffectiveSettings(game, nullptr, {{"refresh", "120"}}, display, &shown, &error));
      CHECK(shown.values.at("refresh").available && Value(shown, "refresh") == "120");
      CHECK(shown.values.at("frame_limit").available);
    }
    auto gdi = hl2;
    gdi.graphics = "gdi";
    CHECK(BuildEffectiveSettings(gdi, nullptr, {{"refresh", "120"}, {"frame_limit", "60"}}, display, &shown, &error));
    CHECK(!shown.values.at("refresh").available && shown.values.at("refresh").reason == "Only with DXVK or OpenGL.");
    CHECK(!shown.values.at("frame_limit").available && shown.values.at("frame_limit").reason == "Only with DXVK or OpenGL.");
    CHECK(RenderProsperoProfile(gdi, shown, target, &profile, &reason));
    CHECK(profile.text.find("refresh") == std::string::npos && profile.text.find("frame_rate") == std::string::npos);
    CHECK(BuildEffectiveSettings(hl2, nullptr, {{"refresh", "120"}}, KitCapabilities{}, &shown, &error));
    CHECK(shown.values.at("refresh").reason == "Needs a runtime kit with 120 Hz output.");

    // DXVK at 120 Hz with Frame limit 60: the display paces it, so dxvk.conf has no maxFrameRate.
    CHECK(BuildEffectiveSettings(hl2, nullptr, {{"refresh", "120"}, {"frame_limit", "60"}}, display, &shown, &error));
    CHECK(RenderProsperoProfile(hl2, shown, target, &profile, &reason));
    CHECK(profile.text.find("\nrefresh = 120\nframe_rate = 60\n") != std::string::npos);
    // A runtime without the capability refuses frame_rate (the [debug] env line is the default wave32).
    CHECK(!CheckProsperoProfile(profile.text, &reason, {.debug_env = true}));
    CHECK(CheckProsperoProfile(profile.text, &reason, {.debug_env = true, .display_frame_rate = true}));
    CHECK(RenderDxvkConfig(shown).find("maxFrameRate") == std::string::npos);
    // 120 and 40 are paced at 120 Hz output.
    for (const char* rate : {"120", "40"}) {
      CHECK(BuildEffectiveSettings(hl2, nullptr, {{"refresh", "120"}, {"frame_limit", rate}}, display, &shown, &error));
      CHECK(Value(shown, "frame_limit") == rate && shown.values.at("frame_limit").reason.empty());
      CHECK(RenderProsperoProfile(hl2, shown, target, &profile, &reason));
      CHECK(profile.text.find(std::string("\nframe_rate = ") + rate + "\n") != std::string::npos);
      // At 60 Hz they fall back to Off, and the row says why.
      CHECK(BuildEffectiveSettings(hl2, nullptr, {{"frame_limit", rate}}, display, &shown, &error));
      CHECK(Value(shown, "frame_limit") == "off" && shown.values.at("frame_limit").reason == "Only with Refresh rate 120 Hz.");
      CHECK(SettingChoiceUnavailable(shown, "frame_limit", rate) == "Only with Refresh rate 120 Hz.");
      CHECK(RenderProsperoProfile(hl2, shown, target, &profile, &reason));
      CHECK(profile.text.find("\nrefresh = 60\n") != std::string::npos && profile.text.find("frame_rate") == std::string::npos);
    }
    // Off is no cap, the runtime's default, so the profile has no frame_rate and every runtime reads it; 60 and 30 at
    // 60 Hz output are paced by the display as well.
    CHECK(BuildEffectiveSettings(hl2, nullptr, {}, display, &shown, &error));
    CHECK(RenderProsperoProfile(hl2, shown, target, &profile, &reason));
    CHECK(profile.text.find("\nrefresh = 60\n") != std::string::npos && profile.text.find("frame_rate") == std::string::npos);
    CHECK(BuildEffectiveSettings(hl2, nullptr, {{"frame_limit", "30"}}, display, &shown, &error));
    CHECK(RenderProsperoProfile(hl2, shown, target, &profile, &reason));
    CHECK(profile.text.find("\nframe_rate = 30\n") != std::string::npos && RenderDxvkConfig(shown).find("maxFrameRate") == std::string::npos);
    // 30 at 120 Hz output takes four vblanks, which the display cannot pace: DXVK's limiter does it, and the profile
    // has no frame_rate.
    CHECK(BuildEffectiveSettings(hl2, nullptr, {{"refresh", "120"}, {"frame_limit", "30"}}, display, &shown, &error));
    CHECK(RenderProsperoProfile(hl2, shown, target, &profile, &reason));
    CHECK(profile.text.find("\nrefresh = 120\n") != std::string::npos && profile.text.find("frame_rate") == std::string::npos);
    CHECK(RenderDxvkConfig(shown).find("\ndxgi.maxFrameRate = 30\nd3d9.maxFrameRate = 30\n") != std::string::npos);
    bool frame_rate_env = false;
    for (const auto& value : RenderBottleEnvironment(shown))
      frame_rate_env |= value.name == "VKD3D_FRAME_RATE" && !value.data.empty();
    CHECK(!frame_rate_env);  // not a vkd3d-proton game
    // OpenGL has no software limiter: 30 at 120 Hz falls back to Off.
    CHECK(BuildEffectiveSettings(opengl, nullptr, {{"refresh", "120"}, {"frame_limit", "30"}}, display, &shown, &error));
    CHECK(Value(shown, "frame_limit") == "off" && shown.values.at("frame_limit").reason == "At 120 Hz, only with DXVK.");
    CHECK(BuildEffectiveSettings(opengl, nullptr, {{"refresh", "120"}, {"frame_limit", "40"}}, display, &shown, &error));
    CHECK(RenderProsperoProfile(opengl, shown, target, &profile, &reason));
    CHECK(profile.text.find("\nrefresh = 120\nframe_rate = 40\n") != std::string::npos);
    CHECK(RenderDxvkConfig(shown).find("maxFrameRate") == std::string::npos);

    // A Direct3D 12 game on vkd3d-proton (a recipe default since automatic Direct3D 12): 30 at 120 Hz is
    // vkd3d-proton's own limiter, through the bottle's Environment key. It has no performance overlay.
    auto d3d12 = hl2;
    d3d12.defaults["d3d12"] = "vkd3d-proton";
    CHECK(BuildEffectiveSettings(d3d12, nullptr, {{"refresh", "120"}, {"frame_limit", "30"}, {"performance_overlay", "on"}},
                                 display, &shown, &error));
    CHECK(shown.vkd3d_proton);
    CHECK(!shown.values.at("performance_overlay").available &&
          shown.values.at("performance_overlay").reason == "Not shown for Direct3D 12 games.");
    CHECK(!shown.values.at("overlay_detail").available);
    std::string vkd3d_rate;
    for (const auto& value : RenderBottleEnvironment(shown))
      if (value.name == "VKD3D_FRAME_RATE") vkd3d_rate = value.data;
    CHECK(vkd3d_rate == "\"30\"");
    CHECK(RenderProsperoProfile(d3d12, shown, target, &profile, &reason));
    CHECK(profile.text.find("\nshow_fps = false\nrefresh = 120\n\n[input]") != std::string::npos);
    // Paced by the display at 60: no VKD3D_FRAME_RATE.
    CHECK(BuildEffectiveSettings(d3d12, nullptr, {{"refresh", "120"}, {"frame_limit", "60"}}, display, &shown, &error));
    vkd3d_rate.clear();
    for (const auto& value : RenderBottleEnvironment(shown))
      if (value.name == "VKD3D_FRAME_RATE") vkd3d_rate = value.data;
    CHECK(vkd3d_rate.empty());
    // vkd3d-proton reports feature level 12_0 unless Environment variables sets another one; other games get none.
    const auto feature_level = [](const EffectiveSettings& values) {
      for (const auto& value : RenderBottleEnvironment(values))
        if (value.name == "VKD3D_FEATURE_LEVEL") return value.data;
      return std::string();
    };
    CHECK(feature_level(shown) == "\"12_0\"");
    CHECK(BuildEffectiveSettings(d3d12, nullptr, {{"environment", "VKD3D_FEATURE_LEVEL=11_1"}}, display, &shown, &error));
    CHECK(feature_level(shown) == "\"11_1\"");
    CHECK(BuildEffectiveSettings(hl2, nullptr, {}, display, &shown, &error));
    CHECK(feature_level(shown).empty());

    // One pacer: a Frame limit replaces frame-rate variables the player typed in Environment variables, both
    // vkd3d-proton's (bottle environment) and DXVK's (profile); with Off they apply as before.
    const auto bottle_rate = [](const EffectiveSettings& values) {
      for (const auto& value : RenderBottleEnvironment(values))
        if (value.name == "VKD3D_FRAME_RATE") return value.data;
      return std::string();
    };
    CHECK(BuildEffectiveSettings(d3d12, nullptr, {{"refresh", "120"}, {"frame_limit", "60"},
        {"environment", "VKD3D_FRAME_RATE=30; DXVK_FRAME_RATE=30"}}, display, &shown, &error));
    CHECK(bottle_rate(shown).empty());
    CHECK(RenderProsperoProfile(d3d12, shown, target, &profile, &reason));
    CHECK(profile.text.find("DXVK_FRAME_RATE") == std::string::npos && profile.text.find("\nframe_rate = 60\n") != std::string::npos);
    CHECK(BuildEffectiveSettings(d3d12, nullptr, {{"refresh", "120"}, {"frame_limit", "30"},
        {"environment", "VKD3D_FRAME_RATE=60; DXVK_FRAME_RATE=60"}}, display, &shown, &error));
    CHECK(bottle_rate(shown) == "\"30\"");
    CHECK(RenderProsperoProfile(d3d12, shown, target, &profile, &reason));
    CHECK(profile.text.find("DXVK_FRAME_RATE") == std::string::npos);
    CHECK(BuildEffectiveSettings(d3d12, nullptr, {{"refresh", "120"},
        {"environment", "VKD3D_FRAME_RATE=60; DXVK_FRAME_RATE=60"}}, display, &shown, &error));
    CHECK(bottle_rate(shown) == "\"60\"");
    CHECK(RenderProsperoProfile(d3d12, shown, target, &profile, &reason));
    CHECK(profile.text.find("env = DXVK_FRAME_RATE=60\n") != std::string::npos);

    // Mixed versions: the kit has the capability, the game's title still runs an older runtime. Frame limit keeps
    // its DXVK behaviour, 120 Hz can be chosen but waits for Update, and the profile has no frame_rate an older
    // runtime would refuse.
    const auto stale = KitForTitleRuntime(display, false);
    CHECK(!stale.display_120hz && stale.display_120hz_needs_update);
    CHECK(KitForTitleRuntime(display, true).display_120hz);
    CHECK(!KitForTitleRuntime(KitCapabilities{}, false).display_120hz_needs_update);
    CHECK(BuildEffectiveSettings(hl2, nullptr, {{"refresh", "120"}, {"frame_limit", "60"}}, stale, &shown, &error));
    {
      const auto& refresh = shown.values.at("refresh");
      CHECK(!refresh.available && refresh.waits_for_update && refresh.reason == kAppliesAfterKitUpdate);
      CHECK(refresh.value == "60" && refresh.after_update == "120" && ChosenValue(refresh) == "120");
      CHECK(refresh.source == ValueSource::kUser);
    }
    CHECK(shown.values.at("frame_limit").available && Value(shown, "frame_limit") == "60");
    CHECK(shown.values.at("frame_limit").after_update.empty() && !shown.values.at("frame_limit").waits_for_update);
    CHECK(RenderProsperoProfile(hl2, shown, target, &profile, &reason));
    CHECK(profile.text.find("frame_rate") == std::string::npos && profile.text.find("refresh") == std::string::npos);
    CHECK(CheckProsperoProfile(profile.text, &reason, {.debug_env = true}));  // the old key set, and wave32's env line
    CHECK(RenderDxvkConfig(shown).find("\ndxgi.maxFrameRate = 60\nd3d9.maxFrameRate = 60\n") != std::string::npos);
    // 120 and 40 need 120 Hz output, chosen or not; with it they wait for Update and nothing caps the game until then.
    CHECK(BuildEffectiveSettings(hl2, nullptr, {{"frame_limit", "40"}}, stale, &shown, &error));
    CHECK(Value(shown, "frame_limit") == "off" && shown.values.at("frame_limit").reason == "Only with Refresh rate 120 Hz.");
    CHECK(shown.values.at("frame_limit").after_update.empty());
    CHECK(SettingChoiceUnavailable(shown, "frame_limit", "120") == "Only with Refresh rate 120 Hz.");
    CHECK(RenderDxvkConfig(shown).find("maxFrameRate") == std::string::npos);
    for (const char* limit : {"120", "40"}) {
      CHECK(BuildEffectiveSettings(hl2, nullptr, {{"refresh", "120"}, {"frame_limit", limit}}, stale, &shown, &error));
      const auto& row = shown.values.at("frame_limit");
      CHECK(row.available && row.waits_for_update && row.value == "off" && row.after_update == limit);
      CHECK(row.reason == kAppliesAfterKitUpdate && ChosenValue(row) == limit);
      CHECK(SettingChoiceUnavailable(shown, "frame_limit", limit).empty());
      CHECK(SettingChoiceWaitsForUpdate(shown, "frame_limit", limit));
      CHECK(!SettingChoiceWaitsForUpdate(shown, "frame_limit", "60") && !SettingChoiceWaitsForUpdate(shown, "frame_limit", "30"));
      CHECK(RenderDxvkConfig(shown).find("maxFrameRate") == std::string::npos);
      CHECK(RenderProsperoProfile(hl2, shown, target, &profile, &reason));
      CHECK(profile.text.find("frame_rate") == std::string::npos && profile.text.find("refresh") == std::string::npos);
      CHECK(CheckProsperoProfile(profile.text, &reason, {.debug_env = true}));
      // Once the title runs the kit's runtime, the same saved values apply.
      CHECK(BuildEffectiveSettings(hl2, nullptr, {{"refresh", "120"}, {"frame_limit", limit}}, display, &shown, &error));
      CHECK(Value(shown, "frame_limit") == limit && shown.values.at("frame_limit").after_update.empty());
      CHECK(!shown.values.at("frame_limit").waits_for_update && !shown.values.at("refresh").waits_for_update);
      CHECK(RenderProsperoProfile(hl2, shown, target, &profile, &reason));
      CHECK(profile.text.find("\nrefresh = 120\nframe_rate = " + std::string(limit) + "\n") != std::string::npos);
    }
    // 30 at 120 Hz output stays DXVK's cap, now and after Update.
    CHECK(BuildEffectiveSettings(hl2, nullptr, {{"refresh", "120"}, {"frame_limit", "30"}}, stale, &shown, &error));
    CHECK(Value(shown, "frame_limit") == "30" && !shown.values.at("frame_limit").waits_for_update);
    CHECK(RenderDxvkConfig(shown).find("\ndxgi.maxFrameRate = 30\n") != std::string::npos);
    // OpenGL on the older runtime has no cap: every Frame limit waits for Update, and the profile keeps refresh = 60.
    CHECK(BuildEffectiveSettings(opengl, nullptr, {{"refresh", "120"}, {"frame_limit", "60"}}, stale, &shown, &error));
    {
      const auto& row = shown.values.at("frame_limit");
      CHECK(!row.available && row.waits_for_update && row.reason == kAppliesAfterKitUpdate && row.after_update == "60");
      CHECK(SettingChoiceWaitsForUpdate(shown, "frame_limit", "60"));
      CHECK(SettingChoiceUnavailable(shown, "frame_limit", "30") == "At 120 Hz, only with DXVK.");
    }
    CHECK(RenderProsperoProfile(opengl, shown, target, &profile, &reason));
    CHECK(profile.text.find("frame_rate") == std::string::npos && profile.text.find("\nrefresh = 60\n") != std::string::npos);
    // A saved choice the kit's runtime would not take either is dropped with its reason.
    CHECK(BuildEffectiveSettings(opengl, nullptr, {{"refresh", "120"}, {"frame_limit", "30"}}, stale, &shown, &error));
    CHECK(shown.values.at("frame_limit").after_update.empty() &&
          shown.values.at("frame_limit").reason == "At 120 Hz, only with DXVK.");
    CHECK(BuildEffectiveSettings(d3d12, nullptr, {{"refresh", "120"}, {"frame_limit", "30"}}, stale, &shown, &error));
    for (const auto& value : RenderBottleEnvironment(shown))
      CHECK(value.name != "VKD3D_FRAME_RATE" || value.data.empty());
    // An older runtime keeps the player's variables, as before.
    CHECK(BuildEffectiveSettings(d3d12, nullptr, {{"frame_limit", "60"}, {"environment", "VKD3D_FRAME_RATE=30"}}, stale,
                                 &shown, &error));
    CHECK(bottle_rate(shown) == "\"30\"");
    // Without the capability: 30 and 60 through maxFrameRate, 120 and 40 need a newer kit.
    CHECK(BuildEffectiveSettings(hl2, nullptr, {{"frame_limit", "120"}}, dxvk_kit, &shown, &error));
    CHECK(Value(shown, "frame_limit") == "off" && shown.values.at("frame_limit").reason == "Needs a runtime kit with 120 Hz output.");
    CHECK(BuildEffectiveSettings(hl2, nullptr, {{"frame_limit", "30"}}, dxvk_kit, &shown, &error));
    CHECK(RenderDxvkConfig(shown).find("\ndxgi.maxFrameRate = 30\n") != std::string::npos);
    CHECK(RenderProsperoProfile(hl2, shown, target, &profile, &reason) && profile.text.find("frame_rate") == std::string::npos);

    // Other display rows.
    CHECK(BuildEffectiveSettings(hl2, nullptr, {}, display, &shown, &error));
    CHECK(shown.values.at("scaling").reason == "DXVK games always fit the screen and keep their shape.");
    CHECK(shown.values.at("vsync").reason == "The PS5 display always waits for the TV; VSync cannot be turned off yet.");
  }

  // A record saved while the OpenGL worker thread row existed still loads, without it, and so do settings data
  // written for that build: the earlier presets.json, and a recipe that sets or locks the row.
  {
    std::vector<SettingsPreset> old_presets;
    CHECK(ParseSettingsPresets(R"({"presets":[{"id":"playden-default","name":"Playden default","settings":{}},
        {"id":"opengl-game","name":"OpenGL game","settings":{"refresh":"60","opengl_thread":"off"}}]})",
        &old_presets, &reason));
    CHECK(old_presets.size() == 2 &&
          old_presets[1].settings == (std::map<std::string, std::string>{{"refresh", "60"}}));
    // Other unknown settings are still refused.
    CHECK(!ParseSettingsPresets(R"({"presets":[{"id":"x","name":"X","settings":{"opengl_threads":"off"}}]})",
                                &old_presets, &reason));
    GameRecipe old_recipe;
    CHECK(ParseGameRecipe(R"({"recipe_id":"old","recipe_version":1,"app_id":70,
        "launch_entries":[{"id":"game","label":"Game","executable":"hl.exe"}],"graphics":"opengl",
        "defaults":{"opengl_thread":"on","refresh":"60"},"locked":["opengl_thread","graphics"]})", &old_recipe, &reason));
    CHECK(old_recipe.defaults == (std::map<std::string, std::string>{{"refresh", "60"}}) &&
          old_recipe.locked == (std::set<std::string>{"graphics"}));
    CHECK(!ParseGameRecipe(R"({"recipe_id":"old","recipe_version":1,"app_id":70,
        "launch_entries":[{"id":"game","label":"Game","executable":"hl.exe"}],"locked":["opengl_threads"]})",
        &old_recipe, &reason));
  }
  {
    GameSettingsRecord old_record;
    CHECK(ParseGameSettingsRecord(R"({"preset":"opengl-game","overrides":{"opengl_thread":"on","refresh":"120"}})",
                                  &old_record, &reason));
    CHECK(old_record.overrides.count("opengl_thread") == 0 && old_record.overrides.at("refresh") == "120");
    const SettingsPreset* opengl_preset = nullptr;
    for (const auto& preset : presets)
      if (preset.id == "opengl-game") opengl_preset = &preset;
    CHECK(opengl_preset && opengl_preset->settings == (std::map<std::string, std::string>{{"refresh", "60"}}));
    CHECK(BuildEffectiveSettings(opengl, opengl_preset, old_record.overrides, zink, &settings, &error));
  }
  // A Zink kit's runtime reads Environment variables as [debug] env lines; names it does not take are left out.
  CHECK(!settings.values.at("environment").reason.size());
  CHECK(BuildEffectiveSettings(opengl, nullptr,
                               {{"environment", "MESA_GL_VERSION_OVERRIDE=3.3; SDL_HINT_X=1; ZINK_DEBUG=compact"},
                                {"logging", "verbose"}},
                               zink, &settings, &error));
  CHECK(RenderProsperoProfile(opengl, settings, target, &profile, &reason));
  CHECK(profile.text.find("\n[debug]\nwinedebug = ") != std::string::npos);
  CHECK(profile.text.find("\nenv = MESA_GL_VERSION_OVERRIDE=3.3\nenv = ZINK_DEBUG=compact\n") != std::string::npos);
  CHECK(profile.text.find("SDL_HINT_X") == std::string::npos && profile.text.find("[environment]") == std::string::npos);
  CHECK(!CheckProsperoProfile(profile.text, &reason));  // without the feature the checker refuses env lines
  // A Godot game asks for opengl3; on a Zink kit it draws with Vulkan instead, elsewhere it keeps OpenGL.
  auto godot = opengl;
  godot.detected = "godot,d3d12,dxgi,opengl,vulkan graphics=opengl";
  godot.arguments = "--rendering-driver opengl3";
  CHECK(BuildEffectiveSettings(godot, nullptr, {}, zink, &settings, &error));
  CHECK(settings.arguments == "--rendering-driver vulkan" &&
        settings.values.at("arguments").value == "--rendering-driver vulkan");
  CHECK(BuildEffectiveSettings(godot, nullptr, {}, KitCapabilities{}, &settings, &error));
  CHECK(settings.arguments == "--rendering-driver opengl3");
  auto gdi = hl2;
  gdi.graphics = "gdi";
  CHECK(BuildEffectiveSettings(gdi, nullptr, {{"performance_overlay", "on"}}, KitCapabilities{}, &settings, &error));
  CHECK(!settings.values.at("performance_overlay").available && Value(settings, "performance_overlay") == "off");

  // Environment variables need a runtime that reads [environment]; released ones refuse it.
  const std::map<std::string, std::string> environment = {{"environment", "WINE_CPU_TOPOLOGY=4:0,1,2,3; DXVK_FRAME_RATE = 45"}};
  CHECK(BuildEffectiveSettings(hl2, nullptr, environment, KitCapabilities{}, &settings, &error));
  CHECK(!settings.values.at("environment").available);
  CHECK(RenderProsperoProfile(hl2, settings, target, &profile, &reason));
  CHECK(profile.text.find("[environment]") == std::string::npos);
  KitCapabilities environment_kit;
  environment_kit.environment = true;
  CHECK(BuildEffectiveSettings(hl2, nullptr, environment, environment_kit, &settings, &error));
  CHECK(RenderProsperoProfile(hl2, settings, target, &profile, &reason));
  // Pixel shaders' wave32 (the default) comes first among the player's variables.
  CHECK(profile.text.find("\n[environment]\nRADV_PERFTEST = pswave32\nWINE_CPU_TOPOLOGY = 4:0,1,2,3\nDXVK_FRAME_RATE = 45\n") !=
        std::string::npos);
  CHECK(!CheckProsperoProfile(profile.text, &reason));                // a released runtime refuses it
  CHECK(CheckProsperoProfile(profile.text, &reason, {.environment_section = true}));
  // A Zink kit that also reads [environment] (kit H r5): CPU cores and the names [environment] takes go there, the
  // diagnostics names only [debug] env takes stay env lines, and no name is written twice.
  {
    auto both = zink;
    both.environment = true;
    CHECK(BuildEffectiveSettings(opengl, nullptr,
                                 {{"environment", "DXVK_FRAME_RATE=60; PW_NATIVE_PROFILE=1; MESA_GL_VERSION_OVERRIDE=3.3"},
                                  {"cpu_topology", "4"}},
                                 both, &settings, &error));
    CHECK(settings.values.at("cpu_topology").available);
    CHECK(RenderProsperoProfile(opengl, settings, target, &profile, &reason));
    CHECK(profile.text.find("\n[debug]\nenv = PW_NATIVE_PROFILE=1\n") != std::string::npos);
    CHECK(profile.text.find(
              "\n[environment]\nWINE_CPU_TOPOLOGY = 4:0,1,2,3\nRADV_PERFTEST = pswave32\nDXVK_FRAME_RATE = 60\n"
              "MESA_GL_VERSION_OVERRIDE = 3.3\n") !=
          std::string::npos);
    CHECK(profile.text.find("env = DXVK_") == std::string::npos && profile.text.find("env = MESA_") == std::string::npos);
    CHECK(CheckProsperoProfile(profile.text, &reason, {.environment_section = true, .debug_env = true}));
  }
  // Pixel shaders: Automatic runs a game known to hang the GPU in wave64 (Stray) in wave32, through the same
  // environment the Environment variables row writes; every other game's profile stays as it was.
  {
    auto stray = hl2;
    stray.app_id = 1332010;
    KitCapabilities debug_kit = zink;  // a Zink kit without [environment]: [debug] env lines
    const auto render = [&](const GameRecipe& recipe, const std::map<std::string, std::string>& overrides,
                            const KitCapabilities& kit) {
      CHECK(BuildEffectiveSettings(recipe, nullptr, overrides, kit, &settings, &error));
      reason.clear();
      if (!RenderProsperoProfile(recipe, settings, target, &profile, &reason)) profile.text.clear();
      return profile.text;
    };
    const auto count = [](const std::string& text, const std::string& what) {
      std::size_t found = 0;
      for (auto at = text.find(what); at != std::string::npos; at = text.find(what, at + 1)) ++found;
      return found;
    };
    CHECK(FindSetting("pixel_shader_waves") && FindSetting("pixel_shader_waves")->playden_default == "wave32" &&
          FindSetting("pixel_shader_waves")->group == SettingGroup::kGraphics);
    CHECK(!ValidateSettingValue(hl2, "pixel_shader_waves", "wave16", &reason));
    CHECK(!ValidateSettingValue(hl2, "pixel_shader_waves", "auto", &reason));
    // Every game runs wave32 by default; the profile ends in its [environment] section.
    const std::string hl2_default = render(hl2, {}, environment_kit);
    const auto environment_at = hl2_default.find("\n[environment]\n");
    CHECK(environment_at != std::string::npos && settings.pixel_shader_waves == "wave32" &&
          hl2_default.substr(environment_at) == "\n[environment]\nRADV_PERFTEST = pswave32\n");
    const std::string base = hl2_default.substr(0, environment_at);
    CHECK(base.find("[environment]") == std::string::npos);
    CHECK(render(stray, {}, environment_kit) == base + "\n[environment]\nRADV_PERFTEST = pswave32\n");
    CHECK(settings.values.at("pixel_shader_waves").available && settings.pixel_shader_waves == "wave32");
    CHECK(CheckProsperoProfile(profile.text, &reason, {.environment_section = true}));
    if (std::getenv("PLAYDEN_PRINT_PROFILES")) std::printf("--- stray default ---\n%s", profile.text.c_str());
    // Another game on the default, with conflicting variables of the player's: pswave32 goes into the player's
    // RADV_PERFTEST, and the player's RADV_PS5_PS_WAVE64 is left out.
    const std::map<std::string, std::string> conflicting = {
        {"environment", " ; RADV_PERFTEST = pswave32 , sam ;; RADV_PS5_PS_WAVE64=0;DXVK_FRAME_RATE = 45"},
        {"cpu_topology", "4"}};
    const std::string hl2_expected =
        "; Generated by Playden from recipe steam-220-hl2-v1 v1. Edit settings in Playden.\n"
        "[application]\n"
        "id = playden-steam-220\n"
        "name = Half-Life 2\n"
        "executable = Z:\\data\\homebrew\\FAKE77031\\games\\220\\hl2.exe\n"
        "working_directory = Z:\\data\\homebrew\\FAKE77031\\games\\220\n"
        "arguments = -game hl2 -steam -w 1920 -h 1080 -novid +exec 360controller.cfg +joystick 1\n"
        "dll_overrides = d3d9=n\n"
        "prefix = game\n"
        "runtime = wine-wow64\n"
        "architecture = pe32\n"
        "graphics = dxvk\n"
        "\n[display]\n"
        "desktop = 1920x1080\n"
        "view = window\n"
        "show_fps = false\n"
        "\n[input]\n"
        "mode = xinput\n"
        "mouse = none\n"
        "options = escape\n"
        "\n[environment]\n"
        "WINE_CPU_TOPOLOGY = 4:0,1,2,3\n"
        "RADV_PERFTEST = pswave32,sam\n"
        "DXVK_FRAME_RATE = 45\n";
    CHECK(render(hl2, conflicting, environment_kit) == hl2_expected);
    CHECK(settings.pixel_shader_waves == "wave32");
    if (std::getenv("PLAYDEN_PRINT_PROFILES")) std::printf("--- hl2 default ---\n%s", profile.text.c_str());
    // Wave32 wins: pswave32 once (spaces and empty options dropped), the player's RADV_PS5_PS_WAVE64 left out.
    auto wave32 = conflicting;
    wave32["pixel_shader_waves"] = "wave32";
    CHECK(render(hl2, wave32, environment_kit).find("\n[environment]\nWINE_CPU_TOPOLOGY = 4:0,1,2,3\n"
                                                     "RADV_PERFTEST = pswave32,sam\nDXVK_FRAME_RATE = 45\n") !=
          std::string::npos);
    CHECK(count(profile.text, "RADV_PERFTEST") == 1 && profile.text.find("RADV_PS5_PS_WAVE64") == std::string::npos);
    CHECK(render(stray, {{"environment", "DXVK_FRAME_RATE=45;RADV_PERFTEST= sam,,nggc "}}, environment_kit)
              .find("\n[environment]\nRADV_PERFTEST = sam,nggc,pswave32\nDXVK_FRAME_RATE = 45\n") != std::string::npos);
    // Wave64 wins, for Stray too: RADV_PS5_PS_WAVE64=1, pswave32 taken out of the player's RADV_PERFTEST.
    auto wave64 = conflicting;
    wave64["pixel_shader_waves"] = "wave64";
    CHECK(render(stray, wave64, environment_kit).find("\n[environment]\nWINE_CPU_TOPOLOGY = 4:0,1,2,3\n"
                                                      "RADV_PS5_PS_WAVE64 = 1\nRADV_PERFTEST = sam\n"
                                                      "DXVK_FRAME_RATE = 45\n") != std::string::npos);
    CHECK(count(profile.text, "RADV_PS5_PS_WAVE64") == 1 && profile.text.find("pswave32") == std::string::npos);
    CHECK(render(stray, {{"pixel_shader_waves", "wave64"}, {"environment", "RADV_PERFTEST=pswave32"}}, environment_kit) ==
          base + "\n[environment]\nRADV_PS5_PS_WAVE64 = 1\n");
    // Options are split as RADV splits them, at spaces as well as commas, and written back with commas; on either
    // destination. "all" (or "a", "al": RADV takes any start of it) turns pswave32 on, so Wave64 refuses it.
    for (const auto* kit : {&environment_kit, &debug_kit}) {
      const std::string line = kit == &environment_kit ? "\nRADV_PERFTEST = " : "\nenv = RADV_PERFTEST=";
      CHECK(render(stray, {{"environment", "RADV_PERFTEST=sam  nggc"}}, *kit).find(line + "sam,nggc,pswave32\n") !=
            std::string::npos);
      CHECK(render(hl2, {{"environment", "RADV_PERFTEST=sam pswave32,nggc"}, {"pixel_shader_waves", "wave64"}}, *kit)
                .find(line + "sam,nggc\n") != std::string::npos);
      CHECK(profile.text.find("pswave32") == std::string::npos);
      CHECK(render(stray, {{"environment", "RADV_PERFTEST=all"}}, *kit).find(line + "all,pswave32\n") != std::string::npos);
      for (const auto* all : {"all", "sam all", "sam,al", "a"}) {
        CHECK(render(stray, {{"environment", std::string("RADV_PERFTEST=") + all}, {"pixel_shader_waves", "wave64"}}, *kit)
                  .empty());
        CHECK(reason.find("turns on wave32 pixel shaders") != std::string::npos);
      }
      // A word that only starts with "all" is not "all".
      CHECK(render(stray, {{"environment", "RADV_PERFTEST=allx"}, {"pixel_shader_waves", "wave64"}}, *kit)
                .find(line + "allx\n") != std::string::npos);
    }
    // The entry limits never drop it: eight variables of the player's after CPU cores in [environment]...
    const std::string eight = "DXVK_A=1;DXVK_B=2;DXVK_C=3;DXVK_D=4;DXVK_E=5;DXVK_F=6;DXVK_G=7;DXVK_H=8";
    CHECK(render(stray, {{"environment", eight}, {"cpu_topology", "4"}}, environment_kit)
              .find("\n[environment]\nWINE_CPU_TOPOLOGY = 4:0,1,2,3\nRADV_PERFTEST = pswave32\nDXVK_A = 1\n") !=
          std::string::npos);
    CHECK(count(profile.text, "DXVK_") == 6 && profile.text.find("DXVK_G") == std::string::npos);
    // ...and five [debug] env lines on a Zink kit.
    CHECK(render(stray, {{"environment", "DXVK_A=1;DXVK_B=2;DXVK_C=3;DXVK_D=4;DXVK_E=5"}}, debug_kit)
              .find("\n[debug]\nenv = RADV_PERFTEST=pswave32\nenv = DXVK_A=1\n") != std::string::npos);
    CHECK(count(profile.text, "env = ") == 5 && profile.text.find("DXVK_E") == std::string::npos);
    CHECK(profile.text.find("[environment]") == std::string::npos);
    CHECK(CheckProsperoProfile(profile.text, &reason, {.debug_env = true}));
    // A player's RADV_PERFTEST too long to take pswave32 stops the launch with a reason, never starts without it:
    // [environment] values are under 128 bytes, [debug] env values under 96.
    const auto perftest = [](std::size_t bytes) { return "RADV_PERFTEST=" + std::string(bytes, 'x'); };
    CHECK(render(stray, {{"environment", perftest(118)}}, environment_kit)
              .find("\nRADV_PERFTEST = " + std::string(118, 'x') + ",pswave32\n") != std::string::npos);
    CHECK(render(stray, {{"environment", perftest(119)}}, environment_kit).empty());
    CHECK(reason.find("RADV_PERFTEST") != std::string::npos && reason.find("pswave32") != std::string::npos);
    CHECK(render(stray, {{"environment", perftest(86)}}, debug_kit)
              .find("\nenv = RADV_PERFTEST=" + std::string(86, 'x') + ",pswave32\n") != std::string::npos);
    CHECK(render(stray, {{"environment", perftest(87)}}, debug_kit).empty() && reason.find("pswave32") != std::string::npos);
    SettingsLaunchInput too_long;
    too_long.recipe = &stray;
    too_long.kit = environment_kit;
    too_long.target = target;
    too_long.record.overrides = {{"environment", perftest(119)}};
    SettingsLaunchPlan refused;
    CHECK(!BuildSettingsLaunchPlan(too_long, &refused, &reason) && reason.find("pswave32") != std::string::npos);
    // On Wave64 the same long value is the player's to keep.
    CHECK(render(hl2, {{"environment", perftest(119)}, {"pixel_shader_waves", "wave64"}}, environment_kit)
              .find(std::string(119, 'x') + "\n") != std::string::npos);
    CHECK(!render(stray, {{"environment", perftest(119)}, {"pixel_shader_waves", "wave64"}}, environment_kit).empty());
    // A kit without either: the row is unavailable for the same reason, and nothing is written.
    CHECK(render(stray, {{"pixel_shader_waves", "wave32"}}, KitCapabilities{}).find("RADV_") == std::string::npos);
    CHECK(!settings.values.at("pixel_shader_waves").available &&
          settings.values.at("pixel_shader_waves").reason == settings.values.at("environment").reason &&
          settings.pixel_shader_waves.empty());
    CHECK(render(stray, {}, KitCapabilities{}).find("RADV_") == std::string::npos);
    // Presets and recipes may set it; a recipe keeps it through a save.
    std::vector<SettingsPreset> wave_presets;
    CHECK(ParseSettingsPresets(R"({"presets":[{"id":"wave32","name":"Wave32","settings":{"pixel_shader_waves":"wave32"}}]})",
                               &wave_presets, &reason));
    CHECK(BuildEffectiveSettings(hl2, &wave_presets.front(), {}, environment_kit, &settings, &error));
    CHECK(settings.values.at("pixel_shader_waves").source == ValueSource::kPreset && settings.pixel_shader_waves == "wave32");
    auto recipe_wave64 = stray;
    recipe_wave64.defaults["pixel_shader_waves"] = "wave64";
    GameRecipe reread_wave;
    CHECK(ParseGameRecipe(SerializeGameRecipe(recipe_wave64), &reread_wave, &reason));
    CHECK(reread_wave.defaults.at("pixel_shader_waves") == "wave64");
    CHECK(render(reread_wave, {}, environment_kit) == base + "\n[environment]\nRADV_PS5_PS_WAVE64 = 1\n");
    CHECK(settings.values.at("pixel_shader_waves").source == ValueSource::kRecipe);
    reread_wave.defaults["pixel_shader_waves"] = "wave16";
    CHECK(!BuildEffectiveSettings(reread_wave, nullptr, {}, environment_kit, &settings, &error));

    // A kit whose title applies the profile's version (profile-version-2): Playden writes version 2, and the driver's
    // own default is wave32, so Wave32 needs no variable at all.
    CHECK(KitCapabilitiesFromManifest({kKitProfileVersion2}, false).profile_version_2);
    CHECK(!KitCapabilitiesFromManifest({kKitEnvironment}, false).profile_version_2);
    auto version_kit = environment_kit;
    version_kit.profile_version_2 = true;
    const std::string version_2 = "\n[runtime]\nversion = 2\n";
    CHECK(render(hl2, {}, version_kit) == base + version_2);
    CHECK(settings.profile_version == 2 && settings.pixel_shader_waves == "wave32");
    CHECK(render(stray, {}, version_kit) == base + version_2);
    CHECK(!CheckProsperoProfile(profile.text, &reason));  // a runtime without it refuses [runtime]
    CHECK(CheckProsperoProfile(profile.text, &reason, {.runtime_version = true}));
    CHECK(!CheckProsperoProfile(base + "\n[runtime]\nversion = 3\n", &reason, {.runtime_version = true}));
    CHECK(!CheckProsperoProfile(base + "\n[runtime]\ncpu = native\n", &reason, {.runtime_version = true}));
    // Wave64 is the opt-out: RADV_PS5_PS_WAVE64=1, as before.
    CHECK(render(stray, {{"pixel_shader_waves", "wave64"}}, version_kit) ==
          base + version_2 + "\n[environment]\nRADV_PS5_PS_WAVE64 = 1\n");
    CHECK(CheckProsperoProfile(profile.text, &reason, {.environment_section = true, .runtime_version = true}));
    // The player's variables: RADV_PERFTEST stays as typed, a RADV_PS5_PS_WAVE64 of the player's is left out, and a
    // RADV_PERFTEST too long to take pswave32 is fine, since nothing is added to it.
    CHECK(render(hl2, conflicting, version_kit) ==
          hl2_expected.substr(0, hl2_expected.find("\n[environment]\n")) + version_2 +
              "\n[environment]\nWINE_CPU_TOPOLOGY = 4:0,1,2,3\nRADV_PERFTEST = pswave32,sam\nDXVK_FRAME_RATE = 45\n");
    CHECK(render(stray, {{"environment", "RADV_PERFTEST=sam"}}, version_kit) ==
          base + version_2 + "\n[environment]\nRADV_PERFTEST = sam\n");
    CHECK(render(stray, {{"environment", perftest(119)}}, version_kit).find(std::string(119, 'x') + "\n") !=
          std::string::npos);
    // A Zink kit with it: no [debug] env line for the default.
    auto version_debug_kit = debug_kit;
    version_debug_kit.profile_version_2 = true;
    CHECK(render(stray, {}, version_debug_kit).find("RADV_") == std::string::npos);
    CHECK(profile.text.find(version_2) != std::string::npos);
    // A title still on an older runtime (Update pending) refuses [runtime] version: version 1, pswave32 written.
    const auto stale_version = KitForTitleRuntime(version_kit, false);
    CHECK(!stale_version.profile_version_2 && stale_version.profile_version_2_needs_update);
    CHECK(KitForTitleRuntime(version_kit, true).profile_version_2);
    CHECK(!KitForTitleRuntime(environment_kit, false).profile_version_2_needs_update);
    CHECK(render(stray, {}, stale_version) == base + "\n[environment]\nRADV_PERFTEST = pswave32\n");
    CHECK(settings.profile_version == 1);
  }
  // Stick dead zone and rumble need a runtime with controller tuning and the Xbox controller;
  // only values that differ from the runtime's defaults are written.
  const std::map<std::string, std::string> tuning = {{"stick_deadzone", "15"}, {"rumble", "50"}};
  CHECK(BuildEffectiveSettings(hl2, nullptr, tuning, KitCapabilities{}, &settings, &error));
  CHECK(!settings.values.at("stick_deadzone").available && !settings.values.at("rumble").available);
  CHECK(RenderProsperoProfile(hl2, settings, target, &profile, &reason));
  CHECK(profile.text.find("stick_deadzone") == std::string::npos && profile.text.find("rumble") == std::string::npos);
  KitCapabilities tuning_kit;
  tuning_kit.input_tuning = true;
  // Every setting is listed whatever the kit; what the kit lacks is disabled with its reason.
  CHECK(SettingShown(*FindSetting("rumble"), KitCapabilities{}) && SettingShown(*FindSetting("environment"), dxvk_kit));
  CHECK(!SettingShown(*FindSetting("button.cross"), KitCapabilities{}));  // under Button mapping
  // Rows nothing can apply yet are listed but never take effect, even with every capability.
  {
    const auto every = KitCapabilitiesFromManifest(SettingsKitCapabilityNames(), true);
    const std::map<std::string, std::string> unbuilt = {{"invert_stick", "right_y"},
        {"locale", "de-DE"}, {"registry_dll_overrides", "dinput8=n,b"}};
    CHECK(BuildEffectiveSettings(hl2, nullptr, unbuilt, every, &settings, &error));
    for (const auto& [id, value] : unbuilt) {
      CHECK(SettingShown(*FindSetting(id), every));
      CHECK(!settings.values.at(id).available && !settings.values.at(id).reason.empty());
    }
    // The reported GPU is a dxvk.conf option and CPU cores a Wine variable in [environment]: no runtime change.
    CHECK(BuildEffectiveSettings(hl2, nullptr, {{"reported_gpu", "nvidia"}, {"cpu_topology", "4"}}, every, &settings, &error));
    CHECK(settings.values.at("reported_gpu").available && settings.values.at("cpu_topology").available);
    CHECK(RenderDxvkConfig(settings).find("\ndxgi.customVendorId = 10de\nd3d9.customVendorId = 10de\n") != std::string::npos);
    CHECK(RenderProsperoProfile(hl2, settings, target, &profile, &reason));
    CHECK(profile.text.find("\n[environment]\nWINE_CPU_TOPOLOGY = 4:0,1,2,3\n") != std::string::npos);
    CHECK(BuildEffectiveSettings(hl2, nullptr, {{"cpu_topology", "4"}}, KitCapabilities{}, &settings, &error));
    CHECK(!settings.values.at("cpu_topology").available && !settings.values.at("reported_gpu").available);
    CHECK(!ValidateSettingValue(hl2, "registry_dll_overrides", "not a list", &reason));
    // The title publication carries at most the two registry values it owns; a longer plan cannot be applied.
    CHECK(BuildEffectiveSettings(hl2, nullptr, {{"windows_version", "win7"}, {"text_scale", "120"},
        {"registry_dll_overrides", "winmm=d"}}, every, &settings, &error));
    CHECK(RenderRegistryValues(settings).size() <= 2);
    CHECK(FindSetting("launch_option")->apply == ApplyMode::kNextLaunch);
    CHECK(FindSetting("locale")->apply == ApplyMode::kGameClosed);
  }
  CHECK(BuildEffectiveSettings(hl2, nullptr, tuning, tuning_kit, &settings, &error));
  CHECK(RenderProsperoProfile(hl2, settings, target, &profile, &reason));
  CHECK(profile.text.find("\nstick_deadzone = 15\nrumble = 50\n") != std::string::npos);
  CHECK(!CheckProsperoProfile(profile.text, &reason));                // a released runtime refuses it
  CHECK(CheckProsperoProfile(profile.text, &reason, {.input_tuning = true}));
  CHECK(BuildEffectiveSettings(hl2, nullptr, {}, tuning_kit, &settings, &error));
  CHECK(RenderProsperoProfile(hl2, settings, target, &profile, &reason));
  CHECK(profile.text.find("stick_deadzone") == std::string::npos && profile.text.find("rumble") == std::string::npos);
  CHECK(BuildEffectiveSettings(hl2, nullptr, {{"controller", "keyboard"}, {"rumble", "0"}}, tuning_kit, &settings, &error));
  CHECK(!settings.values.at("rumble").available && settings.values.at("rumble").reason == "Only with the Xbox controller.");
  CHECK(!ValidateSettingValue(hl2, "stick_deadzone", "60", &reason) && ValidateSettingValue(hl2, "rumble", "25", &reason));
  // A kit with rumble alone (input-rumble) applies Rumble but not the dead zones, once the title runs its runtime.
  {
    const auto rumble_kit = KitCapabilitiesFromManifest({kKitInputRumble}, false);
    CHECK(rumble_kit.input_rumble && !rumble_kit.input_tuning);
    CHECK(BuildEffectiveSettings(hl2, nullptr, tuning, rumble_kit, &settings, &error));
    CHECK(settings.values.at("rumble").available && !settings.values.at("stick_deadzone").available);
    CHECK(RenderProsperoProfile(hl2, settings, target, &profile, &reason));
    CHECK(profile.text.find("\nrumble = 50\n") != std::string::npos && profile.text.find("stick_deadzone") == std::string::npos);
    CHECK(!CheckProsperoProfile(profile.text, &reason));
    CHECK(CheckProsperoProfile(profile.text, &reason, {.input_rumble = true}));
    CHECK(BuildEffectiveSettings(hl2, nullptr, {{"rumble", "0"}}, rumble_kit, &settings, &error));
    CHECK(RenderProsperoProfile(hl2, settings, target, &profile, &reason));
    CHECK(profile.text.find("\nrumble = 0\n") != std::string::npos);
    // A title on an older runtime keeps the choice for its update and writes nothing until then.
    const auto stale = KitForTitleRuntime(rumble_kit, false);
    CHECK(!stale.input_rumble && stale.input_rumble_needs_update);
    CHECK(KitForTitleRuntime(rumble_kit, true).input_rumble);
    CHECK(!KitForTitleRuntime(KitCapabilities{}, false).input_rumble_needs_update);
    CHECK(BuildEffectiveSettings(hl2, nullptr, {{"rumble", "0"}}, stale, &settings, &error));
    const auto& waiting = settings.values.at("rumble");
    CHECK(!waiting.available && waiting.waits_for_update && waiting.after_update == "0");
    CHECK(RenderProsperoProfile(hl2, settings, target, &profile, &reason));
    CHECK(profile.text.find("rumble") == std::string::npos);
    CHECK(BuildEffectiveSettings(hl2, nullptr, {{"controller", "keyboard"}, {"rumble", "0"}}, rumble_kit, &settings, &error));
    CHECK(settings.values.at("rumble").reason == "Only with the Xbox controller.");
  }
  // Frame generation (display-framegen) on DXVK and OpenGL, once the title runs the kit's runtime. Its pictures fill
  // every vblank, so it takes over Frame limit and VSync, and the overlay is its own counter.
  {
    const auto framegen_kit =
        KitCapabilitiesFromManifest({kKitDxvk, kKitDxvkPresent, kKitVkd3dProton, kKitDisplay120Hz, kKitDisplayFramegen}, false);
    CHECK(framegen_kit.display_framegen && !KitCapabilitiesFromManifest({kKitDxvk}, false).display_framegen);
    auto game = hl2;
    game.graphics = "dxvk";
    const std::map<std::string, std::string> chosen = {
        {"framegen", "2"}, {"frame_limit", "30"}, {"vsync", "off"}, {"performance_overlay", "on"}};
    CHECK(BuildEffectiveSettings(game, nullptr, chosen, framegen_kit, &settings, &error));
    CHECK(settings.values.at("framegen").available && Value(settings, "framegen") == "2");
    CHECK(!settings.values.at("frame_limit").available &&
          settings.values.at("frame_limit").reason == "Frame generation sets the frame rate.");
    CHECK(!settings.values.at("vsync").available &&
          settings.values.at("vsync").reason == "Frame generation always waits for the TV.");
    CHECK(!settings.values.at("overlay_detail").available && settings.values.at("performance_overlay").available);
    CHECK(RenderProsperoProfile(game, settings, target, &profile, &reason));
    CHECK(profile.text.find("\nframegen = 2\n") != std::string::npos);
    CHECK(profile.text.find("frame_rate") == std::string::npos && profile.text.find("show_fps = true\n") != std::string::npos);
    const auto dxvk_conf = RenderDxvkConfig(settings);
    CHECK(dxvk_conf.find("maxFrameRate") == std::string::npos && dxvk_conf.find("syncInterval") == std::string::npos &&
          dxvk_conf.find("dxvk.hud") == std::string::npos);
    CHECK(!CheckProsperoProfile(profile.text, &reason));
    CHECK(CheckProsperoProfile(profile.text, &reason, {.display_framegen = true}));
    // A Direct3D 12 game gets the counter too: frame generation draws it, not DXVK.
    auto d3d12 = chosen;
    d3d12["d3d12"] = "vkd3d-proton";
    CHECK(BuildEffectiveSettings(game, nullptr, d3d12, framegen_kit, &settings, &error));
    CHECK(settings.values.at("performance_overlay").available && !settings.values.at("overlay_detail").available);
    // Off writes nothing and leaves the other rows as they were.
    CHECK(BuildEffectiveSettings(game, nullptr, {{"frame_limit", "30"}}, framegen_kit, &settings, &error));
    CHECK(settings.values.at("frame_limit").available && settings.values.at("vsync").available);
    CHECK(RenderProsperoProfile(game, settings, target, &profile, &reason));
    CHECK(profile.text.find("framegen") == std::string::npos);
    // GDI does not present through Vulkan; a kit without the capability says so.
    game.graphics = "gdi";
    CHECK(BuildEffectiveSettings(game, nullptr, chosen, framegen_kit, &settings, &error));
    CHECK(!settings.values.at("framegen").available &&
          settings.values.at("framegen").reason == "Only with DXVK or OpenGL.");
    game.graphics = "opengl";
    CHECK(BuildEffectiveSettings(game, nullptr, chosen, KitCapabilitiesFromManifest({kKitDxvk}, false), &settings, &error));
    CHECK(settings.values.at("framegen").reason == "Needs a runtime kit with frame generation.");
    CHECK(BuildEffectiveSettings(game, nullptr, chosen, framegen_kit, &settings, &error));
    CHECK(settings.values.at("framegen").available && !settings.values.at("frame_limit").available);
    // A title on an older runtime keeps the choice for its update; until then the other rows apply as before.
    const auto stale = KitForTitleRuntime(framegen_kit, false);
    CHECK(!stale.display_framegen && stale.display_framegen_needs_update);
    CHECK(KitForTitleRuntime(framegen_kit, true).display_framegen);
    CHECK(!KitForTitleRuntime(KitCapabilities{}, false).display_framegen_needs_update);
    game.graphics = "dxvk";
    CHECK(BuildEffectiveSettings(game, nullptr, chosen, stale, &settings, &error));
    const auto& waiting = settings.values.at("framegen");
    CHECK(!waiting.available && waiting.waits_for_update && waiting.after_update == "2");
    CHECK(settings.values.at("vsync").available);
    CHECK(RenderProsperoProfile(game, settings, target, &profile, &reason));
    CHECK(profile.text.find("framegen") == std::string::npos);
  }

  // Trigger dead zone rides with controller tuning; the wheel and volume have their own gates.
  const std::map<std::string, std::string> more_tuning = {
      {"trigger_deadzone", "10"}, {"wheel_stick", "right_stick"}, {"volume", "50"}};
  CHECK(BuildEffectiveSettings(hl2, nullptr, more_tuning, tuning_kit, &settings, &error));
  CHECK(settings.values.at("trigger_deadzone").available && !settings.values.at("wheel_stick").available &&
        !settings.values.at("volume").available);
  CHECK(RenderProsperoProfile(hl2, settings, target, &profile, &reason));
  CHECK(profile.text.find("\ntrigger_deadzone = 10\n") != std::string::npos &&
        profile.text.find("wheel") == std::string::npos && profile.text.find("[audio]") == std::string::npos);
  const auto all_kit = KitCapabilitiesFromManifest(SettingsKitCapabilityNames(), false);
  CHECK(BuildEffectiveSettings(hl2, nullptr, more_tuning, all_kit, &settings, &error));
  CHECK(RenderProsperoProfile(hl2, settings, target, &profile, &reason));
  CHECK(profile.text.find("\nwheel = right_stick\n") != std::string::npos);
  CHECK(profile.text.find("\n[audio]\nvolume = 50\n") != std::string::npos);
  CHECK(!CheckProsperoProfile(profile.text, &reason));                // a released runtime refuses both
  CHECK(CheckProsperoProfile(profile.text, &reason,
                             {.environment_section = true, .input_tuning = true, .input_wheel = true, .audio_section = true,
                              .debug_env = true}));
  CHECK(BuildEffectiveSettings(hl2, nullptr, {{"volume", "100"}, {"wheel_stick", "none"}}, all_kit, &settings, &error));
  CHECK(RenderProsperoProfile(hl2, settings, target, &profile, &reason));
  CHECK(profile.text.find("[audio]") == std::string::npos && profile.text.find("wheel") == std::string::npos);
  CHECK(std::is_sorted(SettingsKitCapabilityNames().begin(), SettingsKitCapabilityNames().end()));
  CHECK(!ValidateSettingValue(hl2, "environment", "WINEPREFIX=C:\\x", &reason));
  CHECK(!ValidateSettingValue(hl2, "environment", "DXVK_HUD=full", &reason));
  CHECK(!ValidateSettingValue(hl2, "environment", "DXVK_A=1;DXVK_A=2", &reason));
  CHECK(!ValidateSettingValue(hl2, "environment", "DXVK_A", &reason));

  // Registry settings: Windows version per executable and text scale, edited in user.reg.
  KitCapabilities registry_kit;
  registry_kit.registry = true;
  CHECK(BuildEffectiveSettings(hl2, nullptr, {{"windows_version", "win7"}, {"text_scale", "120"}}, KitCapabilities{},
                               &settings, &error));
  CHECK(RenderRegistryValues(settings).empty());   // no bottle access: nothing to write
  CHECK(BuildEffectiveSettings(hl2, nullptr, {{"windows_version", "win7"}, {"text_scale", "120"}}, registry_kit,
                               &settings, &error));
  auto registry = RenderRegistryValues(settings);
  CHECK(registry.size() == 2 && registry[0].key == "Software\\Wine\\AppDefaults\\hl2.exe" && registry[0].data == "\"win7\"");
  CHECK(registry[1].key == "Control Panel\\Desktop" && registry[1].data == "dword:00000078");
  std::string user_reg =
      "WINE REGISTRY Version 2\n;; All keys relative to REGISTRY\\\\User\\\\S-1-5-21-0-0-0-1000\n\n#arch=win64\n\n"
      "[Control Panel\\\\Desktop] 1791211318\n#time=1dd54d7acc7d208\n\"CaretWidth\"=dword:00000001\n\"Other\"=\"x\"\n\n"
      "[Software\\\\Wine\\\\AppDefaults] 1791210546\n#time=1dd54d5e1bfad0c\n\n"
      "[Software\\\\Wine\\\\Fonts] 1791229249\n#time=1dd54d7acc7d208\n\"LogPixels\"=dword:00000060\n\n"
      "[Software\\\\Wine\\\\Fonts\\\\External Fonts] 1791211318\n\"A\"=\"b\"\n";
  const auto before_registry = user_reg;
  CHECK(ApplyRegistryValues(&user_reg, registry, 1791300000, &reason));
  CHECK(user_reg.find("\"CaretWidth\"=dword:00000001\n\"Other\"=\"x\"\n\"LogPixels\"=dword:00000078\n\n[Software") != std::string::npos);
  // Wine's font cache marker is left alone.
  CHECK(user_reg.find("[Software\\\\Wine\\\\Fonts] 1791229249\n#time=1dd54d7acc7d208\n\"LogPixels\"=dword:00000060\n") != std::string::npos);
  CHECK(user_reg.find("\n[Software\\\\Wine\\\\AppDefaults\\\\hl2.exe] 1791300000\n\"Version\"=\"win7\"\n") != std::string::npos);
  CHECK(user_reg.find("[Software\\\\Wine\\\\Fonts\\\\External Fonts] 1791211318\n\"A\"=\"b\"\n") != std::string::npos);
  // Applying again changes nothing; Windows 10 and 100% remove or reset the values.
  auto again = user_reg;
  CHECK(ApplyRegistryValues(&again, registry, 1791400000, &reason) && again == user_reg);
  CHECK(BuildEffectiveSettings(hl2, nullptr, {}, registry_kit, &settings, &error));
  CHECK(ApplyRegistryValues(&user_reg, RenderRegistryValues(settings), 1791400000, &reason));
  CHECK(user_reg.find("\"Version\"") == std::string::npos && user_reg.find("dword:00000078") == std::string::npos);
  CHECK(user_reg.find("\"LogPixels\"=dword:00000060\n") != std::string::npos);
  std::string not_registry = "hello";
  CHECK(!ApplyRegistryValues(&not_registry, registry, 1, &reason));
  // Wine section and value names are case-insensitive. Sections need no timestamp, and
  // CRLF plus an unterminated final assignment are both valid input forms.
  std::string crlf_registry =
      "WINE REGISTRY Version 2\r\n\r\n[control panel\\\\desktop]\r\n\"logpixels\"=dword:00000060";
  CHECK(ApplyRegistryValues(&crlf_registry, {registry[1]}, 1, &reason));
  CHECK(crlf_registry.find("\"logpixels\"=dword:00000078") != std::string::npos);
  CHECK(crlf_registry.find("[Control Panel\\\\Desktop]") == std::string::npos);
  std::string duplicate_sections =
      "WINE REGISTRY Version 2\n\n[Control Panel\\\\Desktop]\n\"LogPixels\"=dword:00000060\n\n"
      "[control panel\\\\desktop] 2\n\"Other\"=\"x\"\n";
  const auto duplicate_sections_before = duplicate_sections;
  CHECK(!ApplyRegistryValues(&duplicate_sections, {registry[1]}, 1, &reason));
  CHECK(duplicate_sections == duplicate_sections_before);
  std::string duplicate_assignments =
      "WINE REGISTRY Version 2\n\n[Control Panel\\\\Desktop]\n"
      "\"LogPixels\"=dword:00000060\n\"logpixels\"=dword:00000070\n";
  const auto duplicate_assignments_before = duplicate_assignments;
  CHECK(!ApplyRegistryValues(&duplicate_assignments, {registry[1]}, 1, &reason));
  CHECK(duplicate_assignments == duplicate_assignments_before);
  (void)before_registry;

  // An Unreal Engine 3 (UDK) game's EULA is marked accepted in the bottle's system.reg.
  {
    std::string guid;
    CHECK(ParseUe3InstallGuid("<?xml version=\"1.0\"?>\r\n<InstallInfo>\r\n  <InstallGuidString>"
                              "dcf140f3-829e-4033-950d-b3be8d76b933</InstallGuidString>\r\n</InstallInfo>", &guid));
    CHECK(guid == "dcf140f3-829e-4033-950d-b3be8d76b933");
    CHECK(ParseUe3InstallGuid("<InstallGuidString> DCF140F3-829E-4033-950D-B3BE8D76B933\n</InstallGuidString>", &guid));
    CHECK(guid == "DCF140F3-829E-4033-950D-B3BE8D76B933");
    for (const char* bad : {"<InstallGuidString>dcf140f3-829e-4033-950d-b3be8d76b93</InstallGuidString>",
                            "<InstallGuidString>dcf140f3-829e-4033-950d-b3be8d76b93g</InstallGuidString>",
                            "<InstallGuidString>dcf140f3-829e-4033-950d]b3be8d76b933</InstallGuidString>",
                            "<InstallGuidString>dcf140f3\\829e-4033-950d-b3be8d76b933</InstallGuidString>",
                            "<InstallGuidString>dcf140f3-829e-4033-950d-b3be8d76b933",
                            "<InstallGuidString></InstallGuidString>", "no guid here",
                            "<InstallGuidString>dcf140f3-829e-4033-950d-b3be8d76b933</InstallGuidString>"
                            "<InstallGuidString>dcf140f3-829e-4033-950d-b3be8d76b933</InstallGuidString>"})
      CHECK(!ParseUe3InstallGuid(bad, &guid));

    const auto eula_root = std::filesystem::canonical(std::filesystem::temp_directory_path()) /
                           ("playden-ue3-eula-" + std::to_string(::getpid()));
    std::filesystem::remove_all(eula_root);
    const auto game = eula_root / "game", bottle = eula_root / "bottle";
    std::filesystem::create_directories(game / "Binaries/Win32");
    std::filesystem::create_directories(bottle);
    std::filesystem::permissions(eula_root, std::filesystem::perms::owner_all);
    std::filesystem::permissions(bottle, std::filesystem::perms::owner_all);
    const std::string system_reg =
        "WINE REGISTRY Version 2\n;; All keys relative to REGISTRY\\\\Machine\n\n#arch=win64\n\n"
        "[Software\\\\Microsoft\\\\Windows\\\\CurrentVersion\\\\Uninstall] 1791227731\n\n"
        "[Software\\\\Wow6432Node\\\\Microsoft\\\\Windows\\\\CurrentVersion\\\\Uninstall] 1791227735\n";
    std::ofstream(bottle / "system.reg") << system_reg;
    ::chmod((bottle / "system.reg").c_str(), 0600);
    const std::vector<std::string> programs{"Binaries/Win32/UDK.exe"};
    // Not a UDK game: no InstallInfo.xml (and then no UnSetup.exe) means nothing is touched.
    auto eula = AcceptUe3Eula(game.string(), programs, bottle.string(), 1791500000);
    CHECK(!eula.found && eula.ok && !eula.written);
    std::ofstream(game / "Binaries/InstallInfo.xml")
        << "<?xml version=\"1.0\"?>\r\n<InstallInfo>\r\n  <InstallGuidString>dcf140f3-829e-4033-950d-b3be8d76b933"
           "</InstallGuidString>\r\n</InstallInfo>";
    eula = AcceptUe3Eula(game.string(), programs, bottle.string(), 1791500000);
    CHECK(!eula.found && !eula.written);
    std::ofstream(game / "Binaries/UnSetup.exe") << "MZ";
    // A program outside Binaries/Win32 or Win64 is not the UDK layout.
    eula = AcceptUe3Eula(game.string(), {"Game.exe"}, bottle.string(), 1791500000);
    CHECK(!eula.found && !eula.written);
    CHECK(ReadText(bottle / "system.reg") == system_reg && !std::filesystem::exists(bottle / "system.reg.playden-backup"));
    eula = AcceptUe3Eula(game.string(), {"Binaries\\Win32\\UDK.exe"}, bottle.string(), 1791500000);
    CHECK(eula.found && eula.ok && eula.written && eula.guid == "dcf140f3-829e-4033-950d-b3be8d76b933");
    const auto accepted = ReadText(bottle / "system.reg");
    CHECK(accepted.find("\n[Software\\\\Microsoft\\\\Windows\\\\CurrentVersion\\\\Uninstall\\\\"
                        "UDK-dcf140f3-829e-4033-950d-b3be8d76b933] 1791500000\n\"EULAAccepted\"=dword:00000001\n") !=
          std::string::npos);
    CHECK(accepted.find("\n[Software\\\\Wow6432Node\\\\Microsoft\\\\Windows\\\\CurrentVersion\\\\Uninstall\\\\"
                        "UDK-dcf140f3-829e-4033-950d-b3be8d76b933] 1791500000\n\"EULAAccepted\"=dword:00000001\n") !=
          std::string::npos);
    CHECK(accepted.compare(0, system_reg.size(), system_reg) == 0);
    CHECK(ReadText(bottle / "system.reg.playden-backup") == system_reg);
    // Again: already accepted, so neither the hive nor its backup is rewritten.
    eula = AcceptUe3Eula(game.string(), programs, bottle.string(), 1791600000);
    CHECK(eula.found && eula.ok && !eula.written);
    CHECK(ReadText(bottle / "system.reg") == accepted && ReadText(bottle / "system.reg.playden-backup") == system_reg);
    // A bad InstallInfo.xml is reported, not guessed at.
    std::ofstream(game / "Binaries/InstallInfo.xml") << "<InstallGuidString>not-a-guid</InstallGuidString>";
    eula = AcceptUe3Eula(game.string(), programs, bottle.string(), 1791600000);
    CHECK(eula.found && !eula.ok && !eula.written && ReadText(bottle / "system.reg") == accepted);
    std::filesystem::remove_all(eula_root);
  }

  // Wine REG_BINARY continuations form one logical assignment. Equal values keep their original bytes.
  std::string binary_hive = "WINE REGISTRY Version 2\n\n[Software\\\\Classes\\\\Media]\n"
      "\"FilterData\"=hex:01,02,\\\n  03,04\n\"keep\"=\"yes\"\n";
  const auto binary_original = binary_hive;
  CHECK(ApplyRegistryValues(&binary_hive, {{"Software\\Classes\\Media", "FilterData", "hex:01,02,03,04"}}, 1, &reason));
  CHECK(binary_hive == binary_original);
  CHECK(ApplyRegistryValues(&binary_hive, {{"Software\\Classes\\Media", "FilterData", "hex:05,06"}}, 1, &reason));
  CHECK(binary_hive.find("03,04") == std::string::npos && binary_hive.find("hex:05,06\n") != std::string::npos);
  CHECK(binary_hive.find("\"keep\"=\"yes\"") != std::string::npos);
  const auto binary_updated = binary_hive;
  CHECK(ApplyRegistryValues(&binary_hive, {{"Software\\Classes\\Media", "FilterData", "hex:05,06"}}, 2, &reason));
  CHECK(binary_hive == binary_updated);

  // A Steam install script's Registry section goes into the bottle (Fallout: New Vegas's launcher
  // offers only "Install" without it); its Run Process entries are not run.
  {
    const std::string fnv =
        "\"InstallScript\"\n{\n    \"Registry\"\n    {\n"
        "        \"HKEY_LOCAL_MACHINE\\\\SOFTWARE\\\\Bethesda Softworks\\\\FalloutNV\"\n        {\n"
        "            \"string\"\n            {\n                \"Installed Path\"      \"%INSTALLDIR%\\\\\"\n"
        "            }\n        }\n    }\n    \"Run Process\"\n    {\n        \"DirectX\"\n        {\n"
        "            \"process 1\"     \"%INSTALLDIR%\\\\Redists\\\\directx\\\\DXSETUP.exe\"\n        }\n    }\n}\n";
    InstallScriptValues values;
    CHECK(ParseInstallScript(fnv, "Z:\\data\\games\\22380", &values, &reason));
    CHECK(values.user.empty() && values.machine.size() == 2);
    CHECK(values.machine[0].key == "SOFTWARE\\Bethesda Softworks\\FalloutNV" && values.machine[0].name == "Installed Path" &&
          values.machine[0].data == "\"Z:\\\\data\\\\games\\\\22380\\\\\"");
    CHECK(values.machine[1].key == "Software\\Wow6432Node\\Bethesda Softworks\\FalloutNV" &&
          values.machine[1].data == values.machine[0].data);
    // UTF-16 with its mark, HKCU, dwords; another %TOKEN%, other roots and other types are skipped.
    const std::string mixed =
        "\"InstallScript\" { \"Registry\" { \"HKEY_CURRENT_USER\\\\Software\\\\Studio\\\\Game\" { "
        "\"dword\" { \"Installed\" \"1\" \"Bad\" \"x\" } \"string\" { \"Lang\" \"%LANGUAGE%\" \"Ok\" \"yes\" } "
        "\"binary\" { \"Blob\" \"00ff\" } } \"HKEY_CLASSES_ROOT\\\\.sav\" { \"string\" { \"\" \"Game.Save\" } } } }";
    std::string utf16 = "\xff\xfe";
    for (const char value : mixed) utf16 += std::string{value, '\0'};
    values = {};
    CHECK(ParseInstallScript(utf16, "Z:\\g", &values, &reason));
    CHECK(values.machine.empty() && values.user.size() == 2);
    CHECK(values.user[0].key == "Software\\Studio\\Game" && values.user[0].name == "Installed" &&
          values.user[0].data == "dword:00000001");
    CHECK(values.user[1].name == "Ok" && values.user[1].data == "\"yes\"");
    values = {};
    CHECK(!ParseInstallScript("\"InstallScript\" {", "Z:\\g", &values, &reason));

    const auto script_root = std::filesystem::canonical(std::filesystem::temp_directory_path()) /
                             ("playden-install-script-" + std::to_string(::getpid()));
    std::filesystem::remove_all(script_root);
    const auto game = script_root / "game", bottle = script_root / "bottle";
    std::filesystem::create_directories(game);
    std::filesystem::create_directories(bottle);
    std::filesystem::permissions(script_root, std::filesystem::perms::owner_all);
    std::filesystem::permissions(bottle, std::filesystem::perms::owner_all);
    const std::string system_reg = "WINE REGISTRY Version 2\n;; All keys relative to REGISTRY\\\\Machine\n\n#arch=win64\n";
    std::ofstream(bottle / "system.reg") << system_reg;
    ::chmod((bottle / "system.reg").c_str(), 0600);
    // No script: nothing found, nothing written.
    auto applied = ApplyInstallScripts(game.string(), "Z:\\data\\games\\22380", bottle.string(), 1791500000);
    CHECK(applied.ok && applied.found.empty() && !std::filesystem::exists(bottle / "system.reg.playden-backup"));
    std::ofstream(game / "InstallScript.vdf") << fnv;
    applied = ApplyInstallScripts(game.string(), "Z:\\data\\games\\22380", bottle.string(), 1791500000);
    CHECK(applied.ok && applied.found == std::vector<std::string>{"InstallScript.vdf"} && applied.values == 2);
    const auto written = ReadText(bottle / "system.reg");
    CHECK(written.find("\n[SOFTWARE\\\\Bethesda Softworks\\\\FalloutNV] 1791500000\n"
                       "\"Installed Path\"=\"Z:\\\\data\\\\games\\\\22380\\\\\"\n") != std::string::npos);
    CHECK(written.find("\n[Software\\\\Wow6432Node\\\\Bethesda Softworks\\\\FalloutNV] 1791500000\n") != std::string::npos);
    CHECK(ReadText(bottle / "system.reg.playden-backup") == system_reg);
    // Again: already there, so the hive is left as it is.
    applied = ApplyInstallScripts(game.string(), "Z:\\data\\games\\22380", bottle.string(), 1791600000);
    CHECK(applied.ok && ReadText(bottle / "system.reg") == written);
    std::filesystem::remove_all(script_root);
  }

  // A prospero-win-profiles sidecar becomes a community preset for its game.
  CommunityPreset community;
  CHECK(ParseProfileSidecar(R"({"schema_version":1,"profile":"half-life-2",
      "game":{"name":"Half-Life 2","steam_appid":220,"executable":"hl2.exe"},
      "requires":{"prospero_win":["#289","#296"]},
      "checks":[{"date":"2026-09-30","firmware":"12.02","rating":"works"},
                {"date":"2026-10-02","firmware":"12.02","rating":"playable","notes":"59.7 fps in the train station."}],
      "settings":{"resolution":"1920x1080","controller":"keyboard","graphics":"gdi","future_setting":"x","vsync":"maybe"}})",
                            &community, &reason));
  CHECK(community.steam_appid == 220 && community.preset.id == "community-half-life-2");
  CHECK(community.preset.name == "Community · Half-Life 2" && community.rating == "playable");
  CHECK(community.preset.short_hint == "playable on 12.02, 2026-10-02");
  CHECK(community.preset.settings.size() == 2 && community.preset.settings.at("controller") == "keyboard");
  CHECK(community.requires_prospero_win.size() == 2);
  CHECK(CommunityPresetSupported(community, KitCapabilities{}));       // pull request numbers are not features
  auto needs_unknown = community;
  needs_unknown.requires_prospero_win.push_back("future-runtime-feature");
  CHECK(!CommunityPresetSupported(needs_unknown,
      KitCapabilitiesFromManifest(SettingsKitCapabilityNames(), true)));
  for (const auto& requirements_json : {"{\"prospero_win\":[17]}", "{\"prospero_win\":[\"\"]}",
                               "{\"prospero_win\":false}", "[]"}) {
    const std::string malformed = std::string(R"({"schema_version":1,"profile":"half-life-2",
      "game":{"name":"Half-Life 2"},"checks":[{}],"requires":)") + requirements_json + "}";
    CHECK(!ParseProfileSidecar(malformed, &needs_unknown, &reason));
  }
  auto needs_volume = community;
  needs_volume.requires_prospero_win.push_back("audio-volume");
  CHECK(!CommunityPresetSupported(needs_volume, KitCapabilities{}));
  CHECK(CommunityPresetSupported(needs_volume, KitCapabilitiesFromManifest({kKitAudioVolume}, false)));
  needs_volume.requires_prospero_win.push_back("profile-environment");
  CHECK(!CommunityPresetSupported(needs_volume, KitCapabilitiesFromManifest({kKitAudioVolume}, false)));
  // 120 Hz output counts for a game whose title runs a display-120hz runtime.
  {
    auto needs_display = community;
    needs_display.requires_prospero_win.push_back("display-120hz");
    const auto display = KitCapabilitiesFromManifest({kKitDxvk, kKitDisplay120Hz}, false);
    CHECK(CommunityPresetSupported(needs_display, display));
    CHECK(!CommunityPresetSupported(needs_display, KitCapabilitiesFromManifest({kKitDxvk}, false)));
    // A title that waits for its update may take the preset as well: its values wait with the rows they set.
    CHECK(CommunityPresetSupported(needs_display, KitForTitleRuntime(display, false)));
    auto needs_rumble = community;
    needs_rumble.requires_prospero_win.push_back("input-rumble");
    const auto rumble = KitCapabilitiesFromManifest({kKitInputRumble}, false);
    CHECK(CommunityPresetSupported(needs_rumble, rumble) && CommunityPresetSupported(needs_rumble, KitForTitleRuntime(rumble, false)));
    CHECK(CommunityPresetSupported(needs_rumble, KitCapabilitiesFromManifest({kKitInputTuning}, false)));
    CHECK(!CommunityPresetSupported(needs_rumble, KitCapabilitiesFromManifest({kKitDxvk}, false)));
    auto needs_framegen = community;
    needs_framegen.requires_prospero_win.push_back("display-framegen");
    const auto framegen = KitCapabilitiesFromManifest({kKitDisplayFramegen}, false);
    CHECK(CommunityPresetSupported(needs_framegen, framegen) &&
          CommunityPresetSupported(needs_framegen, KitForTitleRuntime(framegen, false)));
    CHECK(!CommunityPresetSupported(needs_framegen, KitCapabilitiesFromManifest({kKitDxvk}, false)));
  }
  CHECK(BuildEffectiveSettings(hl2, &community.preset, {}, KitCapabilities{}, &settings, &error));
  CHECK(settings.values.at("controller").value == "keyboard");
  CHECK(!ParseProfileSidecar(R"({"schema_version":2,"profile":"x","game":{"name":"X"},"checks":[{}]})", &community, &reason));
  CHECK(!ParseProfileSidecar(R"({"schema_version":1,"profile":"x","game":{"name":"X"},"checks":[]})", &community, &reason));
  CHECK(!ParseProfileSidecar(R"({"schema_version":1,"profile":"x","game":{"name":"X","steam_appid":1.5},"checks":[{}]})", &community, &reason));
  CHECK(!ParseProfileSidecar(R"({"schema_version":1,"profile":"x","game":{"name":"X","steam_appid":4294967296},"checks":[{}]})", &community, &reason));

  // Bad player values are rejected before saving; stale ones never block a launch.
  CHECK(!ValidateSettingValue(hl2, "resolution", "5120x2880", &reason));
  CHECK(ValidateSettingValue(hl2, "resolution", "3840x2160", &reason));  // the TV's own size
  CHECK(!ValidateSettingValue(hl2, "extra_arguments", "-a \"b", &reason));
  CHECK(!ValidateSettingValue(hl2, "extra_arguments", "{resolution}", &reason));
  CHECK(!ValidateSettingValue(hl2, "library_overrides", "d3d9", &reason));
  CHECK(!ValidateSettingValue(hl2, "button.cross", "f99", &reason));
  CHECK(!ValidateSettingValue(hl2, "launch_option", "missing", &reason));
  CHECK(!ValidateSettingValue(hl2, "unknown", "x", &reason));
  CHECK(BuildEffectiveSettings(hl2, nullptr, {{"launch_option", "missing"}, {"resolution", "640x480"}}, KitCapabilities{}, &settings, &error));
  CHECK(settings.launch->id == "hl2" && Value(settings, "resolution") == "1920x1080");
  std::string long_arguments;
  for (int index = 0; index < 30; ++index) long_arguments += "-x ";
  CHECK(!BuildEffectiveSettings(hl2, nullptr, {{"extra_arguments", long_arguments}}, KitCapabilities{}, &settings, &error));
  CHECK(error.setting_id == "extra_arguments");

  // Recipes and presets with bad data are refused.
  CHECK(!ParseGameRecipe("{}", &hl2, &reason));
  CHECK(!ParseGameRecipe(R"({"recipe_id":"x","recipe_version":1,"app_id":1,"launch_entries":[{"id":"a","label":"A","executable":"a.exe"}],"graphics":"vulkan"})", &hl2, &reason));
  CHECK(!ParseGameRecipe(R"({"recipe_id":"x","recipe_version":1,"app_id":1,"launch_entries":[{"id":"a","label":"A","executable":"a.exe"}],"defaults":{"resolution":"1x1"}})", &hl2, &reason));
  CHECK(!ParseGameRecipe(R"({"recipe_id":"x","recipe_version":1.5,"app_id":1,"launch_entries":[{"id":"a","label":"A","executable":"a.exe"}]})", &hl2, &reason));
  CHECK(!ParseGameRecipe(R"({"recipe_id":"x","recipe_version":1,"app_id":-1,"launch_entries":[{"id":"a","label":"A","executable":"a.exe"}]})", &hl2, &reason));
  CHECK(!ParseGameRecipe(R"({"recipe_id":"x","recipe_version":1,"app_id":4294967296,"launch_entries":[{"id":"a","label":"A","executable":"a.exe"}]})", &hl2, &reason));
  // A recipe can carry registry values the bottle gets when the game's files are prepared, like a
  // Steam install script: HKEY_CURRENT_USER strings only, never the two values the title publication owns.
  {
    const std::string head = R"({"recipe_id":"x","recipe_version":1,"app_id":1,"launch_entries":[{"id":"a","label":"A","executable":"a.exe"}],"registry":)";
    GameRecipe with_registry;
    CHECK(ParseGameRecipe(head + R"({"Software\\Wine\\AppDefaults\\a.exe\\Direct3D":{"VideoMemorySize":"4096"}}})", &with_registry, &reason));
    CHECK(with_registry.registry.size() == 1 && with_registry.registry[0].key == "Software\\Wine\\AppDefaults\\a.exe\\Direct3D" &&
          with_registry.registry[0].name == "VideoMemorySize" && with_registry.registry[0].data == "\"4096\"");
    GameRecipe reread;
    CHECK(ParseGameRecipe(SerializeGameRecipe(with_registry), &reread, &reason) && reread.registry.size() == 1 &&
          reread.registry[0].key == with_registry.registry[0].key && reread.registry[0].data == "\"4096\"");
    CHECK(SerializeGameRecipe(hl2).find("registry") == std::string::npos);   // absent when empty
    GameRecipe refused;
    CHECK(!ParseGameRecipe(head + R"([]})", &refused, &reason));                                    // not an object
    CHECK(!ParseGameRecipe(head + R"({"Software\\X":{"A":1}}})", &refused, &reason));                  // not a string
    CHECK(!ParseGameRecipe(head + R"({"HKEY_CURRENT_USER\\Software\\X":{"A":"1"}}})", &refused, &reason));  // keys are relative to HKCU
    CHECK(!ParseGameRecipe(head + R"({"Software\\X":{"A":"say \"hi\""}}})", &refused, &reason));         // no quotes in a value
    CHECK(!ParseGameRecipe(head + R"({"Software\\Wine\\AppDefaults\\a.exe":{"version":"win7"}}})", &refused, &reason));  // owned by the publication
    CHECK(!ParseGameRecipe(head + R"({"Control Panel\\Desktop":{"LogPixels":"120"}}})", &refused, &reason));
    // Only the program's own Wine sections and the game's own keys: nothing that gets around the kit's
    // audio check, the overrides Game settings shows or the [environment] allow-list.
    GameRecipe allowed;
    CHECK(ParseGameRecipe(head + R"({"Software\\Square Enix\\FINAL FANTASY XIII":{"Language":"en"}}})", &allowed, &reason));
    CHECK(ParseGameRecipe(head + R"({"Software\\Wine\\AppDefaults\\a.exe\\DirectInput":{"MouseWarpOverride":"force"}}})", &allowed, &reason));
    for (const char* key : {R"(Software\\Wine\\Drivers)", R"(Software\\Wine\\DllOverrides)", R"(Software\\Wine\\Direct3D)",
                            R"(Software\\Wine\\AppDefaults\\a.exe\\DllOverrides)", R"(Software\\Wine\\AppDefaults\\a.exe)",
                            R"(Software\\Microsoft\\Windows\\CurrentVersion\\Run)", R"(Software\\Classes\\CLSID)",
                            R"(Software\\Policies\\X)", R"(software\\WINE\\drivers)", R"(Software)", R"(Environment)",
                            R"(Control Panel\\Mouse)", R"(Console)"}) {
      CHECK(!ParseGameRecipe(head + "{\"" + key + R"(":{"A":"1"}}})", &refused, &reason));
    }
    CHECK(!ParseGameRecipe(head + R"({"Software\\Wine\\AppDefaults\\a.exe\\DllOverrides":{"d3d9":"native"}}})", &refused, &reason) &&
          reason.find("dll_overrides") != std::string::npos);
    CHECK(!ParseGameRecipe(head + R"({"Environment":{"WINE_D3D_CONFIG":"x"}}})", &refused, &reason) &&
          reason.find("Software\\<publisher>") != std::string::npos);
    // A bracket would end the key's [section] header in user.reg early, so Wine would read another key.
    CHECK(!ParseGameRecipe(head + R"({"Software\\Wine\\AppDefaults\\a.exe\\DllOverrides]":{"d3d9":"native"}}})", &refused, &reason));
    CHECK(!ParseGameRecipe(head + R"({"Software\\Wine\\AppDefaults\\a.exe]\\Direct3D":{"Version":"win7"}}})", &refused, &reason));
    CHECK(!ParseGameRecipe(head + R"({"Software\\[Studio\\Game":{"A":"1"}}})", &refused, &reason));
    // The bottle refuses the same value twice, and registry names ignore case.
    CHECK(!ParseGameRecipe(head + R"({"Software\\Studio\\Game":{"Language":"en","language":"fr"}}})", &refused, &reason) &&
          reason.find("appears twice") != std::string::npos);
    CHECK(!ParseGameRecipe(head + R"({"Software\\Studio\\Game":{"A":"1"},"software\\studio\\game":{"a":"2"}}})", &refused, &reason));
    CHECK(ParseGameRecipe(head + R"({"Software\\Studio\\Game":{"A":"1","B":"2"},"Software\\Studio\\Other":{"A":"3"}}})", &allowed, &reason));
    // Applied to a bottle the way an install script is.
    const auto registry_root = std::filesystem::canonical(std::filesystem::temp_directory_path()) /
                               ("playden-recipe-registry-" + std::to_string(::getpid()));
    std::filesystem::remove_all(registry_root);
    std::filesystem::create_directories(registry_root);
    std::filesystem::permissions(registry_root, std::filesystem::perms::owner_all);
    std::ofstream(registry_root / "user.reg") << "WINE REGISTRY Version 2\n;; All keys relative to REGISTRY\\\\User\\\\S-1-5-21-0-0-0-1000\n\n#arch=win64\n";
    ::chmod((registry_root / "user.reg").c_str(), 0600);
    CHECK(ApplyBottleRegistryValues(with_registry.registry, registry_root.string(), 1791500000, "user.reg", &reason));
    CHECK(ReadText(registry_root / "user.reg").find("\n[Software\\\\Wine\\\\AppDefaults\\\\a.exe\\\\Direct3D] 1791500000\n\"VideoMemorySize\"=\"4096\"\n") != std::string::npos);
    std::filesystem::remove_all(registry_root);
  }
  // A recipe can carry values for the game's own INI files, written at preparation while the key is missing or
  // empty (runtime/recipe_ini.h): a .ini inside the game folder, one value per line under its own section.
  {
    const std::string head = R"({"recipe_id":"x","recipe_version":1,"app_id":1,"launch_entries":[{"id":"a","label":"A","executable":"a.exe"}],"ini":)";
    GameRecipe with_ini;
    CHECK(ParseGameRecipe(head + R"({"BmGame/Config/UserEngine.ini":{"Engine.Engine":{"Language":"int","Other":"1"}},"b.ini":{"S":{"k":"v"}}}})",
                          &with_ini, &reason));
    CHECK(with_ini.ini.size() == 3);
    CHECK(std::count(with_ini.ini.begin(), with_ini.ini.end(),
                     IniValue{"BmGame/Config/UserEngine.ini", "Engine.Engine", "Language", "int"}) == 1);
    GameRecipe reread;
    CHECK(ParseGameRecipe(SerializeGameRecipe(with_ini), &reread, &reason));
    CHECK(std::is_permutation(reread.ini.begin(), reread.ini.end(), with_ini.ini.begin(), with_ini.ini.end()));
    CHECK(SerializeGameRecipe(hl2).find("\"ini\"") == std::string::npos);  // absent when empty
    GameRecipe refused;
    const auto refuses = [&](const std::string& ini) {
      return !ParseGameRecipe(head + ini + "}", &refused, &reason);
    };
    CHECK(refuses(R"([])"));                                            // not an object
    CHECK(refuses(R"({"a.ini":{"S":{"k":1}}})"));                       // not a string
    CHECK(refuses(R"({"a.ini":{"S":"k=v"}})"));                         // a section maps keys
    CHECK(refuses(R"({"/etc/a.ini":{"S":{"k":"v"}}})"));                // outside the game folder
    CHECK(refuses(R"({"Config/../../a.ini":{"S":{"k":"v"}}})"));
    CHECK(refuses(R"({"Config\\a.ini":{"S":{"k":"v"}}})"));             // '/' separators only
    CHECK(refuses(R"({"Config//a.ini":{"S":{"k":"v"}}})"));
    CHECK(refuses(R"({"Config/a.cfg":{"S":{"k":"v"}}})"));              // only .ini files
    CHECK(refuses(R"({"a.ini":{"S]":{"k":"v"}}})"));                    // would end the header early
    CHECK(refuses(R"({"a.ini":{" S ":{"k":"v"}}})"));                  // the file's [S] would never match
    CHECK(refuses(R"({"a.ini":{"S":{"k=x":"v"}}})"));                   // would be another key
    CHECK(refuses(R"({"a.ini":{"S":{";k":"v"}}})"));                    // would be a comment
    CHECK(refuses(R"({"a.ini":{"S":{"k":"two\nlines"}}})"));
    CHECK(refuses(R"({"a.ini":{"S":{"k":""}}})"));                      // nothing to write
    CHECK(refuses(R"({"a.ini":{"S":{"k":"v"}},"A.INI":{"s":{"K":"w"}}})"));  // the same value twice, ignoring case
    CHECK(reason.find("appears twice") != std::string::npos);
    std::string many = "{\"a.ini\":{\"S\":{";
    for (int index = 0; index < 17; ++index) many += std::string(index ? "," : "") + "\"k" + std::to_string(index) + "\":\"v\"";
    CHECK(refuses(many + "}}}") && reason.find("too many") != std::string::npos);
  }
  // Final Fantasy XIII asks DxDiag for its video memory, which Wine answers from wined3d, so the shipped
  // recipe sets wined3d's VideoMemorySize for its program. Its Direct3D 9 goes to DXVK (a shipped recipe
  // names its own overrides), and it wants the Steam emulator, not the game's own files.
  {
    GameRecipe ffxiii;
    CHECK(ParseGameRecipe(ReadText(source / "runtime/recipes/steam-292120-ffxiii-v1.json"), &ffxiii, &reason));
    CHECK(ffxiii.app_id == 292120 && ffxiii.registry.size() == 1 && ffxiii.registry[0].name == "VideoMemorySize" &&
          ffxiii.registry[0].key == "Software\\Wine\\AppDefaults\\ffxiiiimg.exe\\Direct3D" && ffxiii.registry[0].data == "\"4096\"");
    EffectiveSettings settings;
    SettingsError error;
    CHECK(BuildEffectiveSettings(ffxiii, nullptr, {}, KitCapabilities{}, &settings, &error));
    CHECK(settings.graphics == "dxvk" && settings.dll_overrides == "d3d9=n");
    // Both rows are locked: Direct3D 9 goes to DXVK whatever Graphics says, and without the emulator
    // sgw.dll quits two seconds in. "Change anyway" still lets the player try.
    CHECK(settings.values.at("steam_features").value == "emulate" && settings.values.at("steam_features").locked);
    CHECK(settings.values.at("graphics").locked);
    CHECK(BuildEffectiveSettings(ffxiii, nullptr, {{"steam_features", "game"}}, KitCapabilities{}, &settings, &error));
    CHECK(settings.values.at("steam_features").value == "emulate");
    CHECK(BuildEffectiveSettings(ffxiii, nullptr, {{"steam_features", "game"}, {"unlock.steam_features", "on"}},
                                 KitCapabilities{}, &settings, &error));
    CHECK(settings.values.at("steam_features").value == "game" && settings.values.at("steam_features").unlocked);
    CHECK(BuildEffectiveSettings(ffxiii, nullptr, {}, KitCapabilities{}, &settings, &error));
    // Its own full-screen, size and quality arguments; the size follows the Resolution row.
    CHECK(settings.arguments == "-FullScreenMode=Force -Width=1920 -Height=1080 -Shadow=4096 -MSAA=4");
    CHECK(BuildEffectiveSettings(ffxiii, nullptr, {{"resolution", "1280x720"}}, KitCapabilities{}, &settings, &error));
    CHECK(settings.arguments == "-FullScreenMode=Force -Width=1280 -Height=720 -Shadow=4096 -MSAA=4");
  }
  // Scott Pilgrim EX looks like XNA, which gets OpenGL; it draws badly or crashes at start there, and
  // works through DXVK with the Steam emulator, so its recipe names DXVK's overrides and locks Graphics.
  {
    GameRecipe scott;
    CHECK(ParseGameRecipe(ReadText(source / "runtime/recipes/steam-2640950-scottpilgrimex-v1.json"), &scott, &reason));
    EffectiveSettings settings;
    SettingsError error;
    CHECK(BuildEffectiveSettings(scott, nullptr, {}, KitCapabilities{}, &settings, &error));
    CHECK(settings.graphics == "dxvk" && settings.dll_overrides == kDxvkOverrides);
    CHECK(settings.values.at("graphics").locked && !settings.values.at("steam_features").locked);
    CHECK(settings.values.at("steam_features").value == "emulate" && settings.arguments.empty());
  }
  // BRAZILIAN DRUG DEALER BEFORE 4 was tested with OpenGL and the Steam emulator; nothing is locked.
  {
    GameRecipe bdd4;
    CHECK(ParseGameRecipe(ReadText(source / "runtime/recipes/steam-4241920-braziliandrugdealer4-v1.json"), &bdd4, &reason));
    EffectiveSettings settings;
    SettingsError error;
    CHECK(BuildEffectiveSettings(bdd4, nullptr, {}, KitCapabilities{}, &settings, &error));
    CHECK(settings.graphics == "opengl" && settings.dll_overrides.empty());
    CHECK(settings.values.at("steam_features").value == "emulate" && !settings.values.at("graphics").locked);
  }
  // Batman: Arkham Asylum starts BmLauncher.exe, which can't start the game on the console, while UserEngine.ini has
  // no [Engine.Engine] Language, so its recipe writes Language=int (English). Otherwise it is what Playden made from
  // the game's files: DXVK for its Direct3D 9 and the Steam emulator.
  {
    GameRecipe asylum;
    CHECK(ParseGameRecipe(ReadText(source / "runtime/recipes/steam-35140-arkhamasylum-v1.json"), &asylum, &reason));
    CHECK(asylum.app_id == 35140 && asylum.launch_entries.size() == 1 &&
          asylum.launch_entries[0].executable == "Binaries/ShippingPC-BmGame.exe");
    CHECK(asylum.ini == (std::vector<IniValue>{{"BmGame/Config/UserEngine.ini", "Engine.Engine", "Language", "int"}}));
    EffectiveSettings settings;
    SettingsError error;
    CHECK(BuildEffectiveSettings(asylum, nullptr, {}, KitCapabilities{}, &settings, &error));
    CHECK(settings.graphics == "dxvk" && settings.dll_overrides == kDxvkOverrides && settings.architecture == "pe32");
    CHECK(settings.values.at("steam_features").value == "emulate" && !settings.values.at("graphics").locked);
    CHECK(settings.launch && settings.launch->executable == "Binaries/ShippingPC-BmGame.exe" && settings.arguments.empty());
  }
  CHECK(!ParseSettingsPresets(R"({"presets":[{"id":"x","name":"X","settings":{"graphics":"gdi"}}]})", &presets, &reason));
  CHECK(!ParseSettingsPresets(R"({"presets":[{"id":"x","name":"X","settings":{"extra_arguments":"-a"}}]})", &presets, &reason));

  // The per-game record round-trips and drops settings this build does not know.
  GameSettingsRecord record{"mouse-driven", {{"resolution", "1280x720"}, {"extra_arguments", "-a \"b c\"\\"}}, 7};
  GameSettingsRecord loaded;
  CHECK(ParseGameSettingsRecord(SerializeGameSettingsRecord(record), &loaded, &reason));
  CHECK(loaded.preset_id == record.preset_id && loaded.overrides == record.overrides && loaded.revision == 7);
  for (const auto revision : {UINT64_C(9007199254740993), std::numeric_limits<std::uint64_t>::max()}) {
    record.revision = revision;
    const auto serialized = SerializeGameSettingsRecord(record);
    CHECK(serialized.find(std::to_string(revision)) != std::string::npos);
    CHECK(ParseGameSettingsRecord(serialized, &loaded, &reason) && loaded.revision == revision);
  }
  CHECK(!ParseGameSettingsRecord(R"({"preset":"","revision":-1,"overrides":{}})", &loaded, &reason));
  CHECK(!ParseGameSettingsRecord(R"({"preset":"","revision":1.5,"overrides":{}})", &loaded, &reason));
  CHECK(!ParseGameSettingsRecord(
      R"({"preset":"","revision":18446744073709551616,"overrides":{}})", &loaded, &reason));
  CHECK(ParseGameSettingsRecord(R"({"preset":"","overrides":{"future_setting":"x","vsync":"on"}})", &loaded, &reason));
  CHECK(loaded.overrides.size() == 1 && loaded.overrides.count("vsync"));
  // Pixel shaders "auto" (the removed Automatic choice) gives way to the default, wave32; a real choice stays.
  loaded = {};
  CHECK(ParseGameSettingsRecord(R"({"preset":"","overrides":{"pixel_shader_waves":"auto","vsync":"on"}})", &loaded, &reason));
  CHECK(loaded.overrides.size() == 1 && loaded.overrides.count("vsync"));
  loaded = {};
  CHECK(ParseGameSettingsRecord(R"({"preset":"","overrides":{"pixel_shader_waves":"wave64"}})", &loaded, &reason));
  CHECK(loaded.overrides.at("pixel_shader_waves") == "wave64");
  CHECK(!ParseGameSettingsRecord("[]", &loaded, &reason));

  // The bundled data loads through the recipe list, as the console title reads it.
  std::vector<SettingsPreset> loaded_presets;
  std::vector<GameRecipe> loaded_recipes;
  CHECK(LoadSettingsData((source / "runtime").string(), &loaded_presets, &loaded_recipes, &reason));
  CHECK(loaded_presets.size() == 4 && loaded_recipes.size() == 5 && loaded_recipes.front().app_id == 220 &&
        loaded_recipes.back().id == "steam-4241920-braziliandrugdealer4-v1");
  CHECK(!LoadSettingsData((source / "missing").string(), &loaded_presets, &loaded_recipes, &reason));

  // ApplyGameSettings writes the profile and a Playden dxvk.conf, removes it when nothing is
  // set, and refuses to touch a dxvk.conf that came with the game.
  const auto root = std::filesystem::canonical(std::filesystem::temp_directory_path()) /
                    ("playden-apply-" + std::to_string(::getpid()));
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root / "game");
  std::filesystem::permissions(root, std::filesystem::perms::owner_all);
  const ProfileTarget apply_target{"playden-steam-220", (root / "game").string()};
  const auto prospero = (root / "prospero").string();
  CHECK(BuildEffectiveSettings(hl2, nullptr, {{"frame_limit", "30"}}, dxvk_kit, &settings, &error));
  CHECK(ApplyGameSettings(hl2, settings, apply_target, prospero, &reason));
  CHECK(ReadText(root / "game" / "dxvk.conf") == RenderDxvkConfig(settings));
  CHECK(ReadText(root / "prospero" / "profiles" / "playden-steam-220.profile").find("[display]") != std::string::npos);
  CHECK(ReadText(root / "prospero" / "profiles" / "profiles.lst") == "playden-steam-220.profile\n");
  CHECK(BuildEffectiveSettings(hl2, nullptr, {}, dxvk_kit, &settings, &error));
  CHECK(ApplyGameSettings(hl2, settings, apply_target, prospero, &reason));
  CHECK(!std::filesystem::exists(root / "game" / "dxvk.conf"));
  std::ofstream(root / "game" / "dxvk.conf") << "d3d9.samplerAnisotropy = 16\n";
  CHECK(BuildEffectiveSettings(hl2, nullptr, {{"frame_limit", "60"}}, dxvk_kit, &settings, &error));
  const auto profile_before = ReadText(root / "prospero" / "profiles" / "playden-steam-220.profile");
  CHECK(!ApplyGameSettings(hl2, settings, apply_target, prospero, &reason) && reason.find("own dxvk.conf") != std::string::npos);
  CHECK(ReadText(root / "game" / "dxvk.conf") == "d3d9.samplerAnisotropy = 16\n");
  CHECK(ReadText(root / "prospero" / "profiles" / "playden-steam-220.profile") == profile_before);
  // DXVK reads dxvk.conf from the launch working directory.
  auto nested = hl2;
  nested.launch_entries.front().executable = "bin/hl2.exe";
  nested.launch_entries.front().working_directory = "bin";
  std::filesystem::create_directory(root / "game" / "bin");
  CHECK(BuildEffectiveSettings(nested, nullptr, {{"frame_limit", "30"}}, dxvk_kit, &settings, &error));
  CHECK(ApplyGameSettings(nested, settings, apply_target, prospero, &reason));
  CHECK(std::filesystem::exists(root / "game" / "bin" / "dxvk.conf"));
  CHECK(ReadText(root / "game" / "dxvk.conf") == "d3d9.samplerAnisotropy = 16\n");
  std::filesystem::create_directory(root / "game" / "fifo-cwd");
  CHECK(::mkfifo((root / "game" / "fifo-cwd" / "dxvk.conf").c_str(), 0600) == 0);
  nested.launch_entries.front().working_directory = "fifo-cwd";
  CHECK(BuildEffectiveSettings(nested, nullptr, {{"frame_limit", "30"}}, dxvk_kit, &settings, &error));
  CHECK(!ApplyGameSettings(nested, settings, apply_target, prospero, &reason));
  // Clearing the shader cache removes only <exe>.dxvk-cache.
  std::ofstream(root / "game" / "hl2.dxvk-cache") << "cache";
  std::ofstream(root / "game" / "hl2.exe") << "game";
  CHECK(ClearShaderCache(hl2.launch_entries.front(), (root / "game").string(), &reason));
  CHECK(!std::filesystem::exists(root / "game" / "hl2.dxvk-cache") && std::filesystem::exists(root / "game" / "hl2.exe"));
  CHECK(ClearShaderCache(hl2.launch_entries.front(), (root / "game").string(), &reason));  // nothing to clear
  std::ofstream(root / "game" / "Mixed.dxvk-cache") << "cache";
  CHECK(ClearShaderCache({"mixed", "Mixed", "Mixed.Exe", ""}, (root / "game").string(), &reason));
  CHECK(!std::filesystem::exists(root / "game" / "Mixed.dxvk-cache"));
  CHECK(!ClearShaderCache({"bad", "Bad", "../escape.exe", ""}, (root / "game").string(), &reason));
  const auto outside = root / "outside";
  std::filesystem::create_directory(outside);
  std::ofstream(outside / "escape.dxvk-cache") << "foreign";
  std::filesystem::create_directory_symlink(outside, root / "game" / "linked");
  CHECK(!ClearShaderCache({"escape", "Escape", "escape.exe", "linked"}, (root / "game").string(), &reason));
  CHECK(ReadText(outside / "escape.dxvk-cache") == "foreign");
  nested.launch_entries.front().working_directory = "linked";
  CHECK(BuildEffectiveSettings(nested, nullptr, {{"frame_limit", "30"}}, dxvk_kit, &settings, &error));
  CHECK(!ApplyGameSettings(nested, settings, apply_target, prospero, &reason));
  CHECK(!std::filesystem::exists(outside / "dxvk.conf"));

  // Fresh and existing bottles converge on both media views; repeating does not rewrite either hive or backup.
  const auto media_runtime = root / "media-runtime";
  std::filesystem::create_directories(media_runtime / "share/wine/media-build");
  const auto media_manifest = media_runtime / "share/wine/media-build/media-registry.json";
  const std::string media_json = R"({"schema":"playden-media-registry/1","values":[
    {"key":"Software\\Classes\\CLSID\\{317df618-5e5a-468a-9f15-d827a9a08162}\\InprocServer32","name":"","data":"\"C:\\windows\\system32\\winegstreamer.dll\""},
    {"key":"Software\\Classes\\Wow6432Node\\CLSID\\{317df618-5e5a-468a-9f15-d827a9a08162}\\InprocServer32","name":"","data":"\"C:\\windows\\syswow64\\winegstreamer.dll\""}]})";
  std::ofstream(media_manifest) << media_json;
  for (const auto* arch : {"x86_64-windows", "i386-windows"}) {
    std::filesystem::create_directories(media_runtime / "lib/wine" / arch);
    std::ofstream(media_runtime / "lib/wine" / arch / "winegstreamer.dll") << "builtin " << arch;
  }
  std::string upgraded;
  for (const auto* kind : {"old", "fresh"}) {
    const auto bottle = root / kind;
    std::filesystem::create_directory(bottle);
    std::filesystem::permissions(bottle, std::filesystem::perms::owner_all);
    const std::string original = upgraded.empty()
        ? "WINE REGISTRY Version 2\n\n[Software\\\\Game]\n\"keep\"=\"yes\"\n" : upgraded;
    std::ofstream(bottle / "system.reg") << original;
    for (const auto* folder : {"system32", "syswow64"}) std::filesystem::create_directories(bottle / "drive_c/windows" / folder);
    // A fresh bottle's template already has the DLL; it is kept as it is.
    if (std::string(kind) == "fresh") std::ofstream(bottle / "drive_c/windows/syswow64/winegstreamer.dll") << "template";
    CHECK(ApplyMediaRegistry(media_runtime.string(), bottle.string(), 123, &reason));
    CHECK(ReadText(bottle / "drive_c/windows/system32/winegstreamer.dll") == "builtin x86_64-windows");
    CHECK(ReadText(bottle / "drive_c/windows/syswow64/winegstreamer.dll") ==
          (std::string(kind) == "old" ? "builtin i386-windows" : "template"));
    const auto after = ReadText(bottle / "system.reg");
    CHECK(after.find("@=\"C:") != std::string::npos && after.find("Wow6432Node") != std::string::npos);
    CHECK(after.find("\"keep\"=\"yes\"") != std::string::npos);
    if (std::string(kind) == "old") CHECK(ReadText(bottle / "system.reg.playden-backup") == original);
    else CHECK(!std::filesystem::exists(bottle / "system.reg.playden-backup"));
    if (upgraded.empty()) upgraded = after;
    else CHECK(after == upgraded);
    const auto stamp = std::filesystem::last_write_time(bottle / "system.reg");
    CHECK(ApplyMediaRegistry(media_runtime.string(), bottle.string(), 999, &reason));
    CHECK(ReadText(bottle / "system.reg") == after && std::filesystem::last_write_time(bottle / "system.reg") == stamp);
    if (std::string(kind) == "old") CHECK(ReadText(bottle / "system.reg.playden-backup") == original);
    else CHECK(!std::filesystem::exists(bottle / "system.reg.playden-backup"));
  }
  // A missing kit DLL fails before the registry is touched.
  {
    const auto bare = root / "bare";
    std::filesystem::create_directory(bare);
    std::filesystem::permissions(bare, std::filesystem::perms::owner_all);
    const std::string hive = "WINE REGISTRY Version 2\n";
    std::ofstream(bare / "system.reg") << hive;
    for (const auto* folder : {"system32", "syswow64"}) std::filesystem::create_directories(bare / "drive_c/windows" / folder);
    std::filesystem::rename(media_runtime / "lib/wine/i386-windows/winegstreamer.dll", root / "held.dll");
    CHECK(!ApplyMediaRegistry(media_runtime.string(), bare.string(), 123, &reason));
    CHECK(ReadText(bare / "system.reg") == hive && !std::filesystem::exists(bare / "system.reg.playden-backup"));
    std::filesystem::rename(root / "held.dll", media_runtime / "lib/wine/i386-windows/winegstreamer.dll");
  }
  std::ofstream(media_manifest) << R"({"schema":"playden-media-registry/1","values":[]})";
  CHECK(!ApplyMediaRegistry(media_runtime.string(), (root / "old").string(), 123, &reason));
  CHECK(ReadText(root / "old/system.reg") == upgraded);
  std::filesystem::remove(media_manifest);
  CHECK(ApplyMediaRegistry(media_runtime.string(), (root / "old").string(), 123, &reason));
  std::filesystem::create_symlink(root / "old/system.reg", media_manifest);
  CHECK(!ApplyMediaRegistry(media_runtime.string(), (root / "old").string(), 123, &reason));

  // A bottle copied from a kit under another folder name (hl2-fw1360-dev1 before kit 0.13) points Wine Mono at that
  // folder: it is pointed where the kit template points, keeping its other values. A value Playden didn't write, a
  // bottle without the value and a bottle not made yet are left as they are.
  {
    const auto mono_template = root / "mono-template", mono_bottle = root / "mono-bottle";
    std::filesystem::create_directories(mono_template);
    std::filesystem::create_directories(mono_bottle);
    std::filesystem::permissions(mono_bottle, std::filesystem::perms::owner_all);
    const std::string head = "WINE REGISTRY Version 2\n;; All keys relative to REGISTRY\\\\User\\\\S-1-5-21-0-0-0-1000\n\n#arch=win64\n\n";
    const std::string drivers = "[Software\\\\Wine\\\\Drivers]\n\"Audio\"=\"ps5\"\n\n";
    const auto mono = [](const std::string& kit, const std::string& stamp) {
      return "[Software\\\\Wine\\\\Mono]" + stamp + "\n" + (stamp.empty() ? "" : "#time=1dc2b\n") +
             "\"RuntimePath\"=\"Z:\\\\data\\\\playden\\\\kits\\\\" + kit + "\\\\mono\\\\wine-mono-11.3.0\"\n\n";
    };
    std::ofstream(mono_template / "user.reg") << head << drivers << mono("playden-kit", "");
    const auto write_bottle = [&](const std::string& text) {
      std::ofstream(mono_bottle / "user.reg", std::ios::trunc) << text;
      ::chmod((mono_bottle / "user.reg").c_str(), 0600);
    };
    // Wine saved the bottle: its keys carry their time.
    write_bottle(head + "[Software\\\\Wine\\\\Drivers] 1791500000\n\"Audio\"=\"ps5\"\n\n" + mono("hl2-fw1360-dev1", " 1791500000"));
    bool changed = false;
    CHECK(PointBottleAtKitMono(mono_template.string(), mono_bottle.string(), 1791600000, &changed, &reason) && changed);
    CHECK(ReadText(mono_bottle / "user.reg") ==
          head + "[Software\\\\Wine\\\\Drivers] 1791500000\n\"Audio\"=\"ps5\"\n\n" + mono("playden-kit", " 1791500000"));
    CHECK(ReadText(mono_bottle / "user.reg.playden-backup").find("kits\\\\hl2-fw1360-dev1\\\\mono") != std::string::npos);
    CHECK(PointBottleAtKitMono(mono_template.string(), mono_bottle.string(), 1791600000, &changed, &reason) && !changed);
    for (const auto& kept : {head + drivers,
                             head + "[Software\\\\Wine\\\\Mono]\n\"RuntimePath\"=\"Z:\\\\opt\\\\mono\"\n"}) {
      write_bottle(kept);
      CHECK(PointBottleAtKitMono(mono_template.string(), mono_bottle.string(), 1791600000, &changed, &reason) && !changed);
      CHECK(ReadText(mono_bottle / "user.reg") == kept);
    }
    CHECK(PointBottleAtKitMono(mono_template.string(), (root / "mono-no-bottle").string(), 1791600000, &changed, &reason) &&
          !changed);
    // A kit without Mono leaves the value alone.
    std::ofstream(mono_template / "user.reg", std::ios::trunc) << head << drivers;
    write_bottle(head + mono("hl2-fw1360-dev1", ""));
    CHECK(PointBottleAtKitMono(mono_template.string(), mono_bottle.string(), 1791600000, &changed, &reason) && !changed);
  }

  // Unreal's root launcher checks the Visual C++ runtime key: it is raised to the kit's builtin DLLs (14.50.35719.0)
  // in both views, keeping the other values; a key a redistributable raised further is kept, and a bottle not made
  // yet is left for later.
  {
    const auto vc_bottle = root / "vc-bottle";
    std::filesystem::create_directories(vc_bottle);
    std::filesystem::permissions(vc_bottle, std::filesystem::perms::owner_all);
    const std::string head = "WINE REGISTRY Version 2\n;; All keys relative to \\\\Machine\n\n#arch=win64\n\n";
    const auto runtime = [](const std::string& view, const std::string& arch, const std::string& minor,
                            const std::string& build, const std::string& version) {
      return "[Software\\\\" + view + "Microsoft\\\\VisualStudio\\\\14.0\\\\VC\\\\Runtimes\\\\" + arch +
             "] 1791500000\n#time=1dc2b\n\"Bld\"=dword:" + build + "\n\"Installed\"=dword:00000001\n" +
             "\"Major\"=dword:0000000e\n\"Minor\"=dword:" + minor + "\n\"Rbld\"=dword:00000000\n\"Version\"=\"" + version +
             "\"\n\n";
    };
    const auto old_runtime = [&](const std::string& view, const std::string& arch) {
      return runtime(view, arch, "0000002a", "00008681", "14.42.34433.0");
    };
    const auto new_runtime = [&](const std::string& view, const std::string& arch) {
      return runtime(view, arch, "00000032", "00008b87", "14.50.35719.0");
    };
    const std::string wbem = "[Software\\\\Microsoft\\\\WBEM] 1791500000\n\"Installation Directory\"=\"C:\\\\windows\\\\system32\\\\wbem\"\n\n";
    const auto write_hive = [&](const std::string& text) {
      std::ofstream(vc_bottle / "system.reg", std::ios::trunc) << text;
      ::chmod((vc_bottle / "system.reg").c_str(), 0600);
    };
    write_hive(head + old_runtime("", "x64") + wbem + old_runtime("Wow6432Node\\\\", "x64") +
               old_runtime("Wow6432Node\\\\", "x86"));
    bool changed = false;
    CHECK(RaiseVcRuntimeVersion(vc_bottle.string(), 1791600000, &changed, &reason) && changed);
    const auto raised = head + new_runtime("", "x64") + wbem + new_runtime("Wow6432Node\\\\", "x64") +
                        new_runtime("Wow6432Node\\\\", "x86");
    CHECK(ReadText(vc_bottle / "system.reg") == raised);
    CHECK(ReadText(vc_bottle / "system.reg.playden-backup").find("14.42.34433.0") != std::string::npos);
    CHECK(RaiseVcRuntimeVersion(vc_bottle.string(), 1791600000, &changed, &reason) && !changed);
    CHECK(ReadText(vc_bottle / "system.reg") == raised);
    // A later runtime stays; a missing view is added.
    const auto later = runtime("", "x64", "00000033", "00000001", "14.51.1.0");
    write_hive(head + later + wbem);
    CHECK(RaiseVcRuntimeVersion(vc_bottle.string(), 1791600000, &changed, &reason) && changed);
    const auto partial = ReadText(vc_bottle / "system.reg");
    CHECK(partial.find(later) == head.size() && partial.find("14.51.1.0") != std::string::npos);
    CHECK(partial.find("[Software\\\\Wow6432Node\\\\Microsoft\\\\VisualStudio\\\\14.0\\\\VC\\\\Runtimes\\\\x86]") !=
          std::string::npos);
    CHECK(partial.find("\"Version\"=\"14.50.35719.0\"") != std::string::npos);
    CHECK(RaiseVcRuntimeVersion((root / "vc-no-bottle").string(), 1791600000, &changed, &reason) && !changed);
  }

  // vkd3d-proton's DLLs go into both windows folders of the bottle and come out again, replaced by the template's,
  // without touching a bottle whose files were never vkd3d-proton's.
  {
    const auto vkd3d_runtime = root / "vkd3d-runtime", vkd3d_template = root / "vkd3d-template";
    const auto vkd3d_bottle = root / "vkd3d-bottle", untouched = root / "vkd3d-untouched";
    const std::vector<std::pair<std::string, std::string>> folders = {{"x86_64-windows", "system32"},
                                                                      {"i386-windows", "syswow64"}};
    for (const auto& [arch, folder] : folders)
      for (const auto* name : {"d3d12.dll", "d3d12core.dll"}) {
        std::filesystem::create_directories(vkd3d_runtime / "lib/vkd3d-proton" / arch);
        std::ofstream(vkd3d_runtime / "lib/vkd3d-proton" / arch / name) << "vkd3d-proton " << arch << " " << name;
        for (const auto& prefix : {vkd3d_template, vkd3d_bottle, untouched}) {
          std::filesystem::create_directories(prefix / "drive_c/windows" / folder);
          std::ofstream(prefix / "drive_c/windows" / folder / name) << (prefix == untouched ? "older " : "fake ") << name;
        }
      }
    const auto dll = [&](const std::filesystem::path& prefix, const std::string& folder, const char* name) {
      return ReadText(prefix / "drive_c/windows" / folder / name);
    };
    CHECK(ApplyD3D12Library(vkd3d_runtime.string(), vkd3d_template.string(), vkd3d_bottle.string(), true, &reason));
    for (const auto& [arch, folder] : folders)
      CHECK(dll(vkd3d_bottle, folder, "d3d12core.dll") == "vkd3d-proton " + arch + " d3d12core.dll");
    const auto stamp = std::filesystem::last_write_time(vkd3d_bottle / "drive_c/windows/system32/d3d12.dll");
    CHECK(ApplyD3D12Library(vkd3d_runtime.string(), vkd3d_template.string(), vkd3d_bottle.string(), true, &reason));
    CHECK(std::filesystem::last_write_time(vkd3d_bottle / "drive_c/windows/system32/d3d12.dll") == stamp);
    CHECK(ApplyD3D12Library(vkd3d_runtime.string(), vkd3d_template.string(), vkd3d_bottle.string(), false, &reason));
    CHECK(ApplyD3D12Library(vkd3d_runtime.string(), vkd3d_template.string(), untouched.string(), false, &reason));
    for (const auto& [arch, folder] : folders) {
      CHECK(dll(vkd3d_bottle, folder, "d3d12.dll") == "fake d3d12.dll");
      CHECK(dll(untouched, folder, "d3d12core.dll") == "older d3d12core.dll");
    }
    // A game whose bottle is not there yet starts without error; the next Play puts the DLLs in.
    CHECK(ApplyD3D12Library(vkd3d_runtime.string(), vkd3d_template.string(), (root / "vkd3d-no-bottle").string(), true,
                            &reason));
    for (const auto& [arch, folder] : folders)
      CHECK(!std::filesystem::exists(vkd3d_bottle / "drive_c/windows" / folder / ".playden-vkd3d-proton"));
    // A bottle switched by an earlier kit's vkd3d-proton is switched back through its marker, whatever the kit now holds.
    CHECK(ApplyD3D12Library(vkd3d_runtime.string(), vkd3d_template.string(), vkd3d_bottle.string(), true, &reason));
    CHECK(std::filesystem::exists(vkd3d_bottle / "drive_c/windows/syswow64/.playden-vkd3d-proton"));
    std::ofstream(vkd3d_runtime / "lib/vkd3d-proton/x86_64-windows/d3d12.dll") << "newer vkd3d-proton";
    CHECK(ApplyD3D12Library(vkd3d_runtime.string(), vkd3d_template.string(), vkd3d_bottle.string(), false, &reason));
    CHECK(dll(vkd3d_bottle, "system32", "d3d12.dll") == "fake d3d12.dll");
    CHECK(!std::filesystem::exists(vkd3d_bottle / "drive_c/windows/system32/.playden-vkd3d-proton"));
    // Nothing is written when a file it needs is missing.
    CHECK(ApplyD3D12Library(vkd3d_runtime.string(), vkd3d_template.string(), vkd3d_bottle.string(), true, &reason));
    std::filesystem::rename(vkd3d_template / "drive_c/windows/syswow64/d3d12core.dll", root / "held-core.dll");
    CHECK(!ApplyD3D12Library(vkd3d_runtime.string(), vkd3d_template.string(), vkd3d_bottle.string(), false, &reason) &&
          reason.find("template") != std::string::npos);
    CHECK(dll(vkd3d_bottle, "system32", "d3d12.dll") == "newer vkd3d-proton");
    std::filesystem::rename(root / "held-core.dll", vkd3d_template / "drive_c/windows/syswow64/d3d12core.dll");
    // Without the kit's DLLs, turning it on fails, and turning it off still restores a marked bottle.
    std::filesystem::remove_all(vkd3d_runtime / "lib/vkd3d-proton");
    CHECK(!ApplyD3D12Library(vkd3d_runtime.string(), vkd3d_template.string(), vkd3d_bottle.string(), true, &reason) &&
          reason.find("no vkd3d-proton") != std::string::npos);
    CHECK(ApplyD3D12Library(vkd3d_runtime.string(), vkd3d_template.string(), vkd3d_bottle.string(), false, &reason));
    CHECK(dll(vkd3d_bottle, "system32", "d3d12.dll") == "fake d3d12.dll");
    CHECK(dll(vkd3d_bottle, "syswow64", "d3d12core.dll") == "fake d3d12core.dll");
    CHECK(ApplyD3D12Library(vkd3d_runtime.string(), vkd3d_template.string(), untouched.string(), false, &reason));
    CHECK(dll(untouched, "system32", "d3d12.dll") == "older d3d12.dll");
  }

  // vkd3d-proton's variables go through the bottle's Environment key: set while Environment variables lists them,
  // removed after, and a bottle that never had them keeps its user.reg untouched.
  {
    const auto env_bottle = root / "env-bottle";
    std::filesystem::create_directory(env_bottle);
    std::filesystem::permissions(env_bottle, std::filesystem::perms::owner_all);
    const std::string hive = "WINE REGISTRY Version 2\n;; All keys relative to REGISTRY\\\\User\\\\S-1-5-21-0-0-0-1000\n\n#arch=win64\n";
    std::ofstream(env_bottle / "user.reg") << hive;
    ::chmod((env_bottle / "user.reg").c_str(), 0600);
    KitCapabilities env_kit;
    env_kit.environment = true;
    EffectiveSettings env_settings;
    SettingsError env_error;
    CHECK(BuildEffectiveSettings(hl2, nullptr, {}, env_kit, &env_settings, &env_error));
    CHECK(ApplyBottleRegistryValues(RenderBottleEnvironment(env_settings), env_bottle.string(), 100, "user.reg", &reason));
    CHECK(ReadText(env_bottle / "user.reg") == hive && !std::filesystem::exists(env_bottle / "user.reg.playden-backup"));
    CHECK(!ValidateSettingValue(hl2, "environment", "VKD3D_NOPE=1", &reason));
    CHECK(!ValidateSettingValue(hl2, "environment", "VKD3D_DEBUG=\"x\"", &reason));
    CHECK(BuildEffectiveSettings(hl2, nullptr, {{"environment", "VKD3D_DEBUG=warn; VKD3D_LOG_FILE=C:\\vkd3d.log;DXVK_LOG_LEVEL=info"}},
                                 env_kit, &env_settings, &env_error));
    ProsperoProfile env_profile;
    CHECK(RenderProsperoProfile(hl2, env_settings, target, &env_profile, &reason));
    CHECK(env_profile.text.find("VKD3D") == std::string::npos && env_profile.text.find("DXVK_LOG_LEVEL") != std::string::npos);
    CHECK(ApplyBottleRegistryValues(RenderBottleEnvironment(env_settings), env_bottle.string(), 100, "user.reg", &reason));
    auto text = ReadText(env_bottle / "user.reg");
    CHECK(text.find("[Environment] 100\n") != std::string::npos);
    CHECK(text.find("\"VKD3D_DEBUG\"=\"warn\"\n") != std::string::npos);
    CHECK(text.find("\"VKD3D_LOG_FILE\"=\"C:\\\\vkd3d.log\"\n") != std::string::npos);
    CHECK(BuildEffectiveSettings(hl2, nullptr, {{"environment", "VKD3D_CONFIG=no_upload_hvv"}}, env_kit, &env_settings, &env_error));
    CHECK(ApplyBottleRegistryValues(RenderBottleEnvironment(env_settings), env_bottle.string(), 200, "user.reg", &reason));
    text = ReadText(env_bottle / "user.reg");
    CHECK(text.find("VKD3D_DEBUG") == std::string::npos && text.find("\"VKD3D_CONFIG\"=\"no_upload_hvv\"") != std::string::npos);
  }

  // An oversized registry file is rejected before backup or replacement.
  const auto prefix = root / "prefix";
  std::filesystem::create_directory(prefix);
  std::filesystem::permissions(prefix, std::filesystem::perms::owner_all);
  std::string oversized_registry = "WINE REGISTRY Version 2\n#";
  oversized_registry.resize(32u * 1024 * 1024 + 1, 'x');
  std::ofstream(prefix / "user.reg", std::ios::binary) << oversized_registry;
  CHECK(BuildEffectiveSettings(hl2, nullptr, {{"text_scale", "120"}}, registry_kit, &settings, &error));
  CHECK(!ApplyRegistrySettings(settings, prefix.string(), 1, &reason));
  CHECK(ReadText(prefix / "user.reg") == oversized_registry);
  CHECK(!std::filesystem::exists(prefix / "user.reg.playden-backup"));
  const auto linked_prefix = root / "linked-prefix";
  std::filesystem::create_directory(linked_prefix);
  std::ofstream(outside / "user.reg") << "WINE REGISTRY Version 2\n";
  std::filesystem::create_symlink(outside / "user.reg", linked_prefix / "user.reg");
  CHECK(!ApplyRegistrySettings(settings, linked_prefix.string(), 1, &reason));
  CHECK(ReadText(outside / "user.reg") == "WINE REGISTRY Version 2\n");

  // The per-game settings file round-trips reset records and tolerates junk entries without
  // truncating out-of-range app ids into valid keys.
  const auto file = (root / "game-settings.json").string();
  std::map<playden::GameId, GameSettingsRecord> records = {
      {SteamId(220), {"mouse-driven", {{"resolution", "1280x720"}}, UINT64_C(9007199254740993)}},
      {SteamId(400), {"", {}, std::numeric_limits<std::uint64_t>::max()}}};
  std::map<playden::GameId, GameSettingsRecord> loaded_records;
  CHECK(LoadGameSettingsFile(file, &loaded_records, &reason) && loaded_records.empty());  // missing file
  const auto stale_temporary = file + ".playden-tmp-" + std::to_string(static_cast<long long>(::getpid()));
  std::ofstream(stale_temporary) << "stale";
  CHECK(SaveGameSettingsFile(file, records, &reason));
  CHECK(ReadText(stale_temporary) == "stale");
  CHECK(LoadGameSettingsFile(file, &loaded_records, &reason));
  CHECK(loaded_records == records);
  std::ofstream(outside / "settings.json") << "foreign";
  const auto linked_settings = root / "linked-settings.json";
  std::filesystem::create_symlink(outside / "settings.json", linked_settings);
  CHECK(!SaveGameSettingsFile(linked_settings.string(), records, &reason));
  CHECK(ReadText(outside / "settings.json") == "foreign");
  std::ofstream(file) << R"({"version":1,"games":{"220":{"preset":"","overrides":{"vsync":"on"}},"221":{"preset":"bad","revision":-1,"overrides":{}},"222":{"preset":"bad","revision":1.5,"overrides":{}},"223":{"preset":"bad","revision":18446744073709551616,"overrides":{}},"4294967295":{"preset":"","revision":1,"overrides":{}},"4294967296":{"preset":"bad","revision":2,"overrides":{}},"x":{},"0":{}}})";
  CHECK(LoadGameSettingsFile(file, &loaded_records, &reason) && loaded_records.size() == 2);
  CHECK(loaded_records.count(SteamId(std::numeric_limits<std::uint32_t>::max())) == 1);
  CHECK(loaded_records.count(SteamId(220)) == 1);
  std::ofstream(file) << "{";
  CHECK(!LoadGameSettingsFile(file, &loaded_records, &reason));
  const auto unreadable = root / "settings-directory";
  std::filesystem::create_directory(unreadable);
  CHECK(!LoadGameSettingsFile(unreadable.string(), &loaded_records, &reason));
  CHECK(reason.find("cannot read") != std::string::npos);
  const auto fifo_settings = root / "settings-fifo";
  CHECK(::mkfifo(fifo_settings.c_str(), 0600) == 0);
  CHECK(!LoadGameSettingsFile(fifo_settings.string(), &loaded_records, &reason));
  CHECK(reason.find("cannot read") != std::string::npos);
  std::filesystem::remove_all(root);

  // Integration contract: kit capabilities come from the verified kit manifest; bottle access
  // is the adapter's own fact.
  const auto none_kit = KitCapabilitiesFromManifest({"wine-wow64"}, false);
  CHECK(!none_kit.dxvk_config && !none_kit.environment && !none_kit.input_tuning && !none_kit.registry);
  const auto full_kit = KitCapabilitiesFromManifest(SettingsKitCapabilityNames(), true);
  CHECK(full_kit.dxvk_config && full_kit.dxvk_present && full_kit.environment && full_kit.input_tuning && full_kit.registry);
  // A kit that ships DXVK (kit H) takes dxvk.conf settings without declaring settings-dxvk-config.
  const auto dxvk_only = KitCapabilitiesFromManifest({"dxvk", "opengl-zink"}, true);
  CHECK(dxvk_only.dxvk_config && !dxvk_only.dxvk_present && !dxvk_only.environment);

  // BuildSettingsLaunchPlan: profile, referenced input presets, dxvk.conf, registry, revision.
  const auto plan_root = std::filesystem::canonical(std::filesystem::temp_directory_path()) /
                         ("playden-plan-" + std::to_string(::getpid()));
  std::filesystem::remove_all(plan_root);
  std::filesystem::create_directories(plan_root / "game");
  std::filesystem::create_directories(plan_root / "prefix");
  std::filesystem::permissions(plan_root / "prefix", std::filesystem::perms::owner_all);
  { std::ofstream(plan_root / "game" / "hl2.exe") << "MZ"; }
  const std::string bottle_reg = "WINE REGISTRY Version 2\n\n[Control Panel\\\\Desktop] 1\n\"CaretWidth\"=dword:00000001\n";
  { std::ofstream(plan_root / "prefix" / "user.reg") << bottle_reg; }
  SettingsLaunchInput launch_input;
  launch_input.recipe = &hl2;
  launch_input.record.overrides = {{"frame_limit", "30"}, {"controller_preset", "gamepad"}, {"windows_version", "win7"}};
  launch_input.record.revision = 42;
  launch_input.kit = KitCapabilitiesFromManifest({kKitDxvkConfig}, false);
  launch_input.target = {"playden-steam-220", (plan_root / "game").string()};
  launch_input.runtime_dir = (source / "runtime").string();
  SettingsLaunchPlan plan;
  launch_input.record.preset_id = "mouse-driven";
  CHECK(!BuildSettingsLaunchPlan(launch_input, &plan, &reason));  // named preset missing
  launch_input.preset = &presets.front();
  CHECK(!BuildSettingsLaunchPlan(launch_input, &plan, &reason));  // wrong named preset
  launch_input.preset = mouse;
  CHECK(BuildSettingsLaunchPlan(launch_input, &plan, &reason));
  launch_input.record.preset_id.clear();
  CHECK(!BuildSettingsLaunchPlan(launch_input, &plan, &reason));  // named preset cannot masquerade as default
  launch_input.preset = &presets.front();
  CHECK(BuildSettingsLaunchPlan(launch_input, &plan, &reason));  // canonical default object
  launch_input.preset = nullptr;
  CHECK(BuildSettingsLaunchPlan(launch_input, &plan, &reason));
  CHECK(plan.app_id == 220 && plan.recipe_id == hl2.id && plan.recipe_version == std::to_string(hl2.version));
  CHECK(plan.profile_id == "playden-steam-220" && plan.settings_revision == 42 && plan.launch_entry_id == "hl2");
  CHECK(plan.profile_bytes.find("\npreset = gamepad\n") != std::string::npos && CheckProsperoProfile(plan.profile_bytes, &reason));
  CHECK(plan.input_presets.size() == 1 && plan.input_presets[0].first == "gamepad.input" &&
        plan.input_presets[0].second == ReadText(source / "runtime/input/gamepad.input"));
  CHECK(plan.dxvk_config.find("d3d9.maxFrameRate = 30\n") != std::string::npos);
  CHECK(plan.registry.empty());                                   // no bottle access: nothing for the registry
  ProfilePublication publication;
  publication.recipe_id = "wrong-recipe";
  publication.recipe_version = "999";
  publication.required_input_presets = {"stale.input"};
  FillProfilePublication(plan, &publication);
  CHECK(publication.profile_id == plan.profile_id && publication.profile_bytes == plan.profile_bytes &&
        publication.settings_revision == 42);
  CHECK(publication.recipe_id == plan.recipe_id && publication.recipe_version == plan.recipe_version);
  CHECK(publication.required_input_presets == std::vector<std::string>{"gamepad.input"} &&
        publication.input_presets.size() == 1 && publication.input_presets[0].filename == "gamepad.input");
  // A preset the build does not ship blocks the plan; a stale override is ignored instead.
  launch_input.runtime_dir = (plan_root / "missing").string();
  CHECK(!BuildSettingsLaunchPlan(launch_input, &plan, &reason) && reason.find("gamepad") != std::string::npos);
  launch_input.runtime_dir = (source / "runtime").string();
  auto bad_input = launch_input;
  bad_input.record.overrides["frame_limit"] = "999";
  CHECK(BuildSettingsLaunchPlan(bad_input, &plan, &reason) && plan.dxvk_config.empty());

  // ApplySettingsFiles writes dxvk.conf and, with bottle access, the registry with a backup.
  launch_input.kit = KitCapabilitiesFromManifest({kKitDxvkConfig}, true);
  CHECK(BuildSettingsLaunchPlan(launch_input, &plan, &reason) && plan.registry.size() == 2);  // version, and 100% text scale (removes)
  CHECK(::chmod((plan_root / "prefix/user.reg").c_str(), 0644) == 0);  // legacy owned file
  CHECK(ApplySettingsFiles(plan, (plan_root / "game").string(), (plan_root / "prefix").string(), 1791300000, &reason));
  CHECK(ReadText(plan_root / "game" / "dxvk.conf") == plan.dxvk_config);
  CHECK(ReadText(plan_root / "prefix" / "user.reg").find("[Software\\\\Wine\\\\AppDefaults\\\\hl2.exe] 1791300000\n\"Version\"=\"win7\"\n") !=
        std::string::npos);
  CHECK(ReadText(plan_root / "prefix" / "user.reg.playden-backup") == bottle_reg);
  struct stat registry_mode {}, registry_backup_mode {};
  CHECK(::stat((plan_root / "prefix/user.reg").c_str(), &registry_mode) == 0);
  CHECK((registry_mode.st_mode & 0777) == 0600);
  CHECK(::stat((plan_root / "prefix/user.reg.playden-backup").c_str(), &registry_backup_mode) == 0);
  CHECK((registry_backup_mode.st_mode & 0777) == 0600);
  // The apply consumes a value snapshot, even after recipes reload/change.
  GameRecipe temporary_recipe = hl2;
  launch_input.recipe = &temporary_recipe;
  CHECK(BuildSettingsLaunchPlan(launch_input, &plan, &reason));
  for (auto& launch : temporary_recipe.launch_entries) launch.working_directory = "missing-after-reload";
  plan.registry = {{"Software\\Wine\\AppDefaults\\hl2.exe", "Version", "\"winxp\""}};
  CHECK(ApplySettingsFiles(plan, (plan_root / "game").string(), (plan_root / "prefix").string(), 1791300000, &reason));
  CHECK(ReadText(plan_root / "prefix" / "user.reg").find("\"Version\"=\"winxp\"") != std::string::npos);
  launch_input.recipe = &hl2;

  // Back to defaults: Playden's dxvk.conf goes, the per-game Windows version is removed.
  launch_input.record.overrides.clear();
  CHECK(BuildSettingsLaunchPlan(launch_input, &plan, &reason) && plan.dxvk_config.empty() && plan.input_presets.empty());
  CHECK(ApplySettingsFiles(plan, (plan_root / "game").string(), (plan_root / "prefix").string(), 1791300001, &reason));
  CHECK(!std::filesystem::exists(plan_root / "game" / "dxvk.conf"));
  CHECK(ReadText(plan_root / "prefix" / "user.reg").find("\"Version\"") == std::string::npos);
  // A dxvk.conf that came with the game is never replaced, and then the bottle is untouched too.
  { std::ofstream(plan_root / "game" / "dxvk.conf") << "d3d9.maxFrameRate = 20\n"; }
  launch_input.record.overrides = {{"frame_limit", "30"}, {"windows_version", "winxp"}};
  CHECK(BuildSettingsLaunchPlan(launch_input, &plan, &reason));
  const auto reg_before = ReadText(plan_root / "prefix" / "user.reg");
  CHECK(!ApplySettingsFiles(plan, (plan_root / "game").string(), (plan_root / "prefix").string(), 1791300002, &reason));
  CHECK(reason.find("own dxvk.conf") != std::string::npos);
  CHECK(ReadText(plan_root / "game" / "dxvk.conf") == "d3d9.maxFrameRate = 20\n" &&
        ReadText(plan_root / "prefix" / "user.reg") == reg_before);
  // A bottle whose user.reg cannot be edited refuses the whole apply: dxvk.conf is put back.
  std::filesystem::remove(plan_root / "game" / "dxvk.conf");
  { std::ofstream(plan_root / "prefix" / "user.reg") << "not a registry"; }
  CHECK(!ApplySettingsFiles(plan, (plan_root / "game").string(), (plan_root / "prefix").string(), 1791300003, &reason));
  CHECK(!std::filesystem::exists(plan_root / "game" / "dxvk.conf"));
  CHECK(ReadText(plan_root / "prefix" / "user.reg") == "not a registry");
  { std::ofstream(plan_root / "game" / "dxvk.conf") << "# Generated by Playden from this game's settings. old\n"; }
  CHECK(!ApplySettingsFiles(plan, (plan_root / "game").string(), (plan_root / "prefix").string(), 1791300004, &reason));
  CHECK(ReadText(plan_root / "game" / "dxvk.conf") == "# Generated by Playden from this game's settings. old\n");
  CHECK(::chmod((plan_root / "game" / "dxvk.conf").c_str(), 0600) == 0);
  CHECK(!ApplySettingsFiles(plan, (plan_root / "game").string(), (plan_root / "prefix").string(), 1791300005, &reason));
  struct stat preserved_mode {};
  CHECK(::stat((plan_root / "game" / "dxvk.conf").c_str(), &preserved_mode) == 0);
  CHECK((preserved_mode.st_mode & 0777) == 0600);
  // Removing a default config then restoring it must also preserve its mode.
  launch_input.record.overrides = {{"windows_version", "winxp"}};
  CHECK(BuildSettingsLaunchPlan(launch_input, &plan, &reason) && plan.dxvk_config.empty());
  CHECK(!ApplySettingsFiles(plan, (plan_root / "game").string(), (plan_root / "prefix").string(), 1791300006, &reason));
  CHECK(::stat((plan_root / "game" / "dxvk.conf").c_str(), &preserved_mode) == 0);
  CHECK((preserved_mode.st_mode & 0777) == 0600);
  std::filesystem::remove_all(plan_root);

  // Games without a shipped recipe: one is generated from Steam's launch options.
  {
    const auto game_root = std::filesystem::temp_directory_path() / ("playden-generated-recipe-" + std::to_string(::getpid()));
    std::filesystem::remove_all(game_root);
    WritePe(game_root / "Bin" / "Game.exe", true);
    WritePe(game_root / "Launcher32.exe", false);
    WritePe(game_root / "Bin" / "Tool.exe", true);
    std::vector<LaunchEntry> launches = {
        {"", "", "setup.bat", "", ""},                                  // not an .exe
        {"", "Play", "bin\\game.exe", "Bin", "-windowed  -nosplash"},  // case differs from the disk
        {"", "", "Launcher32.exe", "", ""},                             // other architecture
        {"", "", "Missing.exe", "", ""},
        {"", "", "Bin/Tool.exe", "", ""},
    };
    GameRecipe generated;
    CHECK(GenerateGameRecipe(4242, "Example", launches, game_root.string(), &generated, &reason));
    CHECK(generated.id == GeneratedRecipeId(4242) && IsGeneratedRecipeId(generated.id) && !IsGeneratedRecipeId(hl2.id));
    CHECK(generated.app_id == 4242 && generated.version == 2 && generated.architecture == "pe64");
    CHECK(generated.graphics == "auto" && generated.dll_overrides.empty() && generated.locked.empty());
    CHECK(generated.launch_entries.size() == 2);
    CHECK(generated.launch_entries[0].executable == "Bin/Game.exe" && generated.launch_entries[0].label == "Play");
    CHECK(generated.launch_entries[0].working_directory == "Bin");
    CHECK(generated.launch_entries[0].arguments == "-windowed -nosplash");
    CHECK(generated.launch_entries[1].executable == "Bin/Tool.exe" && generated.launch_entries[1].label == "Tool.exe");
    // A Steam entry that names no folder starts in the install root, as on Steam; one that names a folder starts there.
    CHECK(generated.launch_entries[1].working_directory.empty() && generated.launch_entries[1].in_install_root);
    CHECK(!generated.launch_entries[0].in_install_root);
    CHECK(LaunchWorkingFolder(generated.launch_entries[1]) == std::string());
    // The recipe round-trips through its JSON form.
    GameRecipe reread;
    CHECK(ParseGameRecipe(SerializeGameRecipe(generated), &reread, &reason));
    CHECK(SerializeGameRecipe(reread) == SerializeGameRecipe(generated) && reread.launch_entries[1].in_install_root);
    {
      // The Program folder row shows it as the game folder; "." chooses it, empty the program's own folder.
      EffectiveSettings root_settings;
      SettingsError root_error;
      CHECK(BuildEffectiveSettings(generated, nullptr, {{"launch_option", "launch-2"}}, KitCapabilities{},
                                   &root_settings, &root_error));
      CHECK(root_settings.values.at("program_folder").value == "." && root_settings.launch->in_install_root);
      CHECK(BuildEffectiveSettings(generated, nullptr, {{"launch_option", "launch-2"}, {"program_folder", ""}},
                                   KitCapabilities{}, &root_settings, &root_error));
      CHECK(!root_settings.launch->in_install_root && LaunchWorkingFolder(*root_settings.launch) == std::string("Bin"));
      CHECK(BuildEffectiveSettings(generated, nullptr, {{"program_folder", "."}}, KitCapabilities{},
                                   &root_settings, &root_error));
      CHECK(root_settings.launch->working_directory.empty() && root_settings.launch->in_install_root);
      // A player's program starts in its own folder.
      GameRecipe chosen;
      RecipeChoices choices;
      choices.program = "Bin/Tool.exe";
      CHECK(GenerateGameRecipe(4246, "Chosen", {}, game_root.string(), &chosen, &reason, KitCapabilities{}, choices));
      CHECK(!chosen.launch_entries.front().in_install_root);
    }
    // The launch entry's own arguments reach the profile, before the player's.
    EffectiveSettings generated_settings;
    SettingsError generated_error;
    CHECK(BuildEffectiveSettings(generated, nullptr, {{"extra_arguments", "-dev"}}, KitCapabilities{},
                                 &generated_settings, &generated_error));
    CHECK(generated_settings.arguments == "-windowed -nosplash -dev");
    CHECK(BuildEffectiveSettings(generated, nullptr, {{"launch_option", "launch-2"}}, KitCapabilities{},
                                 &generated_settings, &generated_error));
    CHECK(generated_settings.arguments.empty() && generated_settings.launch->executable == "Bin/Tool.exe");
    // A 32-bit game is pe32.
    CHECK(GenerateGameRecipe(4243, "Old", {{"", "", "Launcher32.exe", "", ""}}, game_root.string(), &generated, &reason));
    CHECK(generated.architecture == "pe32");
    // Nothing startable: a missing program names itself; no .exe at all says so.
    CHECK(!GenerateGameRecipe(4244, "Gone", {{"", "", "Missing.exe", "", ""}}, game_root.string(), &generated, &reason));
    CHECK(reason.find("Missing.exe") != std::string::npos);
    CHECK(!GenerateGameRecipe(4245, "Script", {{"", "", "run.sh", "", ""}}, game_root.string(), &generated, &reason));
    CHECK(reason.find("no Windows program") != std::string::npos);
    std::filesystem::remove_all(game_root);
  }

  {  // Everything the recipe sets has a row the player can change and reset (issue 52).
    const ProfileTarget target{"playden-steam-220", "/data/homebrew/FAKE77031/games/220"};
    EffectiveSettings settings;
    SettingsError error;
    // Required arguments start as the recipe's and can be replaced; {resolution} still expands.
    CHECK(BuildEffectiveSettings(hl2, nullptr, {}, KitCapabilities{}, &settings, &error));
    CHECK(settings.values.at("arguments").value == hl2.arguments && settings.values.at("arguments").source == ValueSource::kRecipe);
    CHECK(BuildEffectiveSettings(hl2, nullptr, {{"arguments", "-game hl2 {resolution}"}, {"extra_arguments", "-dev"}},
                                 KitCapabilities{}, &settings, &error));
    CHECK(settings.arguments == "-game hl2 -w 1920 -h 1080 -dev");
    CHECK(ValidateSettingValue(hl2, "arguments", "", &reason) && !ValidateSettingValue(hl2, "arguments", "{w}", &reason));
    // Library overrides: the whole effective list, from the recipe and what an earlier build added.
    CHECK(BuildEffectiveSettings(hl2, nullptr, {{"library_overrides", "dinput8=n,b"}}, KitCapabilities{}, &settings, &error));
    CHECK(settings.values.at("dll_overrides").value == "d3d9=n;dinput8=n,b" && settings.dll_overrides == "d3d9=n;dinput8=n,b");
    CHECK(BuildEffectiveSettings(hl2, nullptr, {{"dll_overrides", ""}, {"library_overrides", "dinput8=n"}}, KitCapabilities{},
                                 &settings, &error));
    CHECK(settings.dll_overrides.empty() && settings.values.at("dll_overrides").source == ValueSource::kUser);
    ProsperoProfile profile;
    CHECK(RenderProsperoProfile(hl2, settings, target, &profile, &reason));
    CHECK(profile.text.find("dll_overrides") == std::string::npos);
    // A generated DXVK recipe lists its DXVK overrides. They follow graphics, also in a list the
    // player saved: kept while graphics is DXVK, gone after a switch, and never removable.
    auto generated = hl2;
    generated.id = GeneratedRecipeId(220);
    generated.dll_overrides.clear();
    generated.locked.clear();
    CHECK(BuildEffectiveSettings(generated, nullptr, {}, KitCapabilities{}, &settings, &error));
    CHECK(settings.values.at("dll_overrides").value == kDxvkOverrides);
    CHECK(IsGraphicsDllOverride(generated, kDxvkOverrides) && !IsGraphicsDllOverride(hl2, kDxvkOverrides));
    const std::string saved = std::string(kDxvkOverrides) + ";dinput8=n,b";
    CHECK(BuildEffectiveSettings(generated, nullptr, {{"dll_overrides", saved}}, KitCapabilities{}, &settings, &error));
    CHECK(settings.dll_overrides == saved);
    CHECK(BuildEffectiveSettings(generated, nullptr, {{"dll_overrides", "dinput8=n,b"}}, KitCapabilities{}, &settings, &error));
    CHECK(settings.dll_overrides == saved);
    CHECK(BuildEffectiveSettings(generated, nullptr, {{"dll_overrides", saved}, {"graphics", "opengl"}}, KitCapabilities{},
                                 &settings, &error));
    CHECK(settings.graphics == "opengl" && settings.dll_overrides == "dinput8=n,b");
    // Direct3D 12 through vkd3d-proton needs the kit's DLLs and DXVK's dxgi. Its overrides follow the row after the
    // DXVK ones, in the recipe's list or the player's, and go away with it.
    CHECK(BuildEffectiveSettings(generated, nullptr, {{"d3d12", "vkd3d-proton"}}, KitCapabilities{}, &settings, &error));
    CHECK(!settings.values.at("d3d12").available && settings.values.at("d3d12").value == "wine" && !settings.vkd3d_proton);
    KitCapabilities vkd3d_kit;
    vkd3d_kit.vkd3d_proton = true;
    CHECK(BuildEffectiveSettings(generated, nullptr, {}, vkd3d_kit, &settings, &error));
    CHECK(settings.values.at("d3d12").available && !settings.vkd3d_proton && settings.dll_overrides == kDxvkOverrides);
    CHECK(BuildEffectiveSettings(generated, nullptr, {{"d3d12", "vkd3d-proton"}}, vkd3d_kit, &settings, &error));
    CHECK(settings.vkd3d_proton && settings.dll_overrides == std::string(kDxvkOverrides) + ";" + kVkd3dProtonOverrides);
    CHECK(BuildEffectiveSettings(generated, nullptr, {{"d3d12", "vkd3d-proton"}, {"dll_overrides", "dinput8=n,b"}}, vkd3d_kit,
                                 &settings, &error));
    CHECK(settings.dll_overrides == saved + ";" + kVkd3dProtonOverrides);
    const auto with_vkd3d = settings.dll_overrides;
    CHECK(BuildEffectiveSettings(generated, nullptr, {{"dll_overrides", with_vkd3d}}, vkd3d_kit, &settings, &error));
    CHECK(!settings.vkd3d_proton && settings.dll_overrides == saved);
    CHECK(BuildEffectiveSettings(generated, nullptr, {{"d3d12", "vkd3d-proton"}, {"dll_overrides", with_vkd3d}}, vkd3d_kit,
                                 &settings, &error));
    CHECK(settings.dll_overrides == with_vkd3d);
    CHECK(BuildEffectiveSettings(generated, nullptr, {{"d3d12", "vkd3d-proton"}, {"graphics", "opengl"}}, vkd3d_kit,
                                 &settings, &error));
    CHECK(!settings.values.at("d3d12").available && !settings.vkd3d_proton && settings.dll_overrides.empty());
    // A recipe whose own list does not load DXVK's dxgi gets it with vkd3d-proton; a dxgi the player chose stays.
    KitCapabilities vkd3d_dxvk_kit = vkd3d_kit;
    CHECK(BuildEffectiveSettings(hl2, nullptr, {{"d3d12", "vkd3d-proton"}}, vkd3d_dxvk_kit, &settings, &error));
    CHECK(settings.dll_overrides == std::string("d3d9=n;") + kVkd3dProtonDxgiOverrides);
    CHECK(BuildEffectiveSettings(hl2, nullptr, {{"d3d12", "vkd3d-proton"}, {"dll_overrides", "d3d9=n;DXGI=b"}},
                                 vkd3d_dxvk_kit, &settings, &error));
    CHECK(settings.dll_overrides == std::string("d3d9=n;DXGI=b;") + kVkd3dProtonOverrides);
    CHECK(BuildEffectiveSettings(hl2, nullptr, {{"dll_overrides", std::string("d3d9=n;") + kVkd3dProtonDxgiOverrides}},
                                 vkd3d_dxvk_kit, &settings, &error));
    CHECK(!settings.vkd3d_proton && settings.dll_overrides == "d3d9=n");
    // Generated Unity and Unreal recipes ask for Direct3D 12 while vkd3d-proton draws it; arguments the player typed
    // stay as they are.
    {
      auto engine = generated;
      engine.arguments = "-force-d3d11 -dx11";
      CHECK(BuildEffectiveSettings(engine, nullptr, {}, vkd3d_kit, &settings, &error));
      CHECK(settings.arguments == "-force-d3d11 -dx11" && settings.values.at("arguments").value == "-force-d3d11 -dx11");
      CHECK(BuildEffectiveSettings(engine, nullptr, {{"d3d12", "vkd3d-proton"}}, vkd3d_kit, &settings, &error));
      CHECK(settings.arguments == "-force-d3d12 -dx12" && settings.values.at("arguments").value == "-force-d3d12 -dx12");
      CHECK(BuildEffectiveSettings(engine, nullptr, {{"d3d12", "vkd3d-proton"}, {"arguments", "-dx11"}}, vkd3d_kit,
                                   &settings, &error));
      CHECK(settings.arguments == "-dx11");
      auto shipped = engine;
      shipped.id = hl2.id;
      CHECK(BuildEffectiveSettings(shipped, nullptr, {{"d3d12", "vkd3d-proton"}, {"unlock.graphics", "on"}}, vkd3d_kit,
                                   &settings, &error));
      CHECK(settings.arguments == "-force-d3d11 -dx11");
    }
    // A Steam launch option that picks the API too is dropped from it: Required arguments decide (Severed Steel's
    // "-noraytracing -d3d12" next to "-dx11" stopped Unreal). Its other words stay.
    {
      auto unreal = generated;
      unreal.arguments = "-dx11";
      CHECK(!unreal.launch_entries.empty());
      unreal.launch_entries[0].arguments = "-noraytracing -D3D12";
      CHECK(BuildEffectiveSettings(unreal, nullptr, {}, vkd3d_kit, &settings, &error));
      CHECK(settings.arguments == "-dx11 -noraytracing");
      // A quoted value is one word: a switch inside it is the game's text, not a launch option.
      unreal.launch_entries[0].arguments = "-d3d12 -title \"Use -d3d12 mode\"";
      CHECK(BuildEffectiveSettings(unreal, nullptr, {}, vkd3d_kit, &settings, &error));
      CHECK(settings.arguments == "-dx11 -title \"Use -d3d12 mode\"");
      unreal.launch_entries[0].arguments = "-noraytracing -D3D12";
      CHECK(BuildEffectiveSettings(unreal, nullptr, {{"d3d12", "vkd3d-proton"}}, vkd3d_kit, &settings, &error));
      CHECK(settings.arguments == "-dx12 -noraytracing");
      unreal.arguments.clear();
      CHECK(BuildEffectiveSettings(unreal, nullptr, {}, vkd3d_kit, &settings, &error));
      CHECK(settings.arguments == "-noraytracing -D3D12");
      auto shipped = unreal;
      shipped.id = hl2.id;
      shipped.arguments = "-dx11";
      CHECK(BuildEffectiveSettings(shipped, nullptr, {{"unlock.graphics", "on"}}, vkd3d_kit, &settings, &error));
      CHECK(settings.arguments == "-dx11 -noraytracing -D3D12");
    }
    CHECK(KitCapabilitiesFromManifest({kKitVkd3dProton}, false).vkd3d_proton &&
          !KitCapabilitiesFromManifest({kKitDxvk}, false).vkd3d_proton);
    CHECK(KitCapabilitiesFromManifest({kKitBottleProfileShared}, false).bottle_profile_shared &&
          !KitCapabilitiesFromManifest({kKitGameRootUnowned}, false).bottle_profile_shared &&
          !KitCapabilitiesFromManifest({kKitBottleProfileShared}, false).game_root_unowned);
    // A locked row keeps the recipe's value until the player changes it anyway.
    CHECK(BuildEffectiveSettings(hl2, nullptr, {{"graphics", "opengl"}}, KitCapabilities{}, &settings, &error));
    CHECK(settings.graphics == "dxvk" && settings.values.at("graphics").locked && !settings.values.at("graphics").unlocked);
    CHECK(BuildEffectiveSettings(hl2, nullptr, {{"graphics", "opengl"}, {"unlock.graphics", "on"}}, KitCapabilities{},
                                 &settings, &error));
    CHECK(settings.graphics == "opengl" && settings.values.at("graphics").unlocked &&
          settings.values.at("graphics").reason.find("may not start") != std::string::npos);
    // ...including the Steam library a shipped recipe keeps.
    CHECK(BuildEffectiveSettings(hl2, nullptr, {{"steam_features", "emulate"}, {"unlock.steam_features", "on"}},
                                 KitCapabilities{}, &settings, &error));
    CHECK(settings.values.at("steam_features").value == "emulate");
    // The unlock survives the saved record; an unlock for an unknown row does not.
    GameSettingsRecord record;
    record.overrides = {{"unlock.graphics", "on"}, {"graphics", "opengl"}};
    GameSettingsRecord reread;
    CHECK(ParseGameSettingsRecord(SerializeGameSettingsRecord(record), &reread, &reason) && reread == record);
    record.overrides["unlock.nothing"] = "on";
    CHECK(ParseGameSettingsRecord(SerializeGameSettingsRecord(record), &reread, &reason) && !reread.overrides.count("unlock.nothing"));
    // A program from the game folder, with its own folder and arguments; the architecture follows it.
    const std::map<std::string, std::string> program = {{"program", "bin\\win64\\hl2_win64.exe"}, {"architecture", "pe64"},
        {"program_folder", "bin"}, {"program_arguments", "-console"}};
    CHECK(BuildEffectiveSettings(hl2, nullptr, program, KitCapabilities{}, &settings, &error));
    CHECK(settings.launch->id == kProgramEntryId && settings.launch->executable == "bin/win64/hl2_win64.exe" &&
          settings.launch->label == "hl2_win64.exe" && settings.launch->working_directory == "bin");
    CHECK(settings.values.at("launch_option").value == kProgramEntryId && settings.architecture == "pe64");
    CHECK(settings.arguments == "-game hl2 -steam -w 1920 -h 1080 -novid +exec 360controller.cfg +joystick 1 -console");
    CHECK(RenderProsperoProfile(hl2, settings, target, &profile, &reason));
    CHECK(profile.text.find("\\games\\220\\bin\\win64\\hl2_win64.exe\nworking_directory = Z:\\data\\homebrew\\FAKE77031\\games\\220\\bin\n") !=
          std::string::npos);
    CHECK(profile.text.find("\narchitecture = pe64\n") != std::string::npos);
    // Without a program of the player's, the architecture is the recipe's; a launch entry takes the folder too.
    CHECK(BuildEffectiveSettings(hl2, nullptr, {{"architecture", "pe64"}, {"program_folder", "bin"}}, KitCapabilities{},
                                 &settings, &error));
    CHECK(settings.architecture == "pe32" && settings.launch->id == "hl2" && settings.launch->working_directory == "bin");
    CHECK(settings.values.at("program_folder").source == ValueSource::kUser);
    CHECK(!ValidateSettingValue(hl2, "program", "run.bat", &reason) && !ValidateSettingValue(hl2, "program", "../x.exe", &reason));
    CHECK(!ValidateSettingValue(hl2, "program_folder", "C:\\Games", &reason) && ValidateSettingValue(hl2, "program_folder", "", &reason));
    // A recipe without a launch entry still gives a sheet: the Program row is where the player starts.
    auto empty = generated;
    empty.launch_entries.clear();
    CHECK(BuildEffectiveSettings(empty, nullptr, {}, KitCapabilities{}, &settings, &error));
    CHECK(!settings.launch && !RenderProsperoProfile(empty, settings, target, &profile, &reason));
    CHECK(BuildEffectiveSettings(empty, nullptr, {{"program", "Game.exe"}}, KitCapabilities{}, &settings, &error) && settings.launch);

    // Export as recipe: the effective settings as a community recipe, read back as a recipe and as one.
    CHECK(BuildEffectiveSettings(hl2, nullptr, {{"resolution", "1280x720"}, {"program", "hl2_win64.exe"},
                                 {"architecture", "pe64"}, {"unlock.graphics", "on"}, {"graphics", "opengl"},
                                 {"arguments", "-game hl2 {resolution}"}, {"dll_overrides", "d3d9=n;xinput1_3=n"}},
                                 KitCapabilities{}, &settings, &error));
    auto community = BuildCommunityRecipe(hl2, settings);
    {  // What the files showed travels without the graphics value, which has its own field (#67).
      auto detected = hl2;
      detected.detected = "d3d9 graphics=dxvk";
      CHECK(BuildCommunityRecipe(detected, settings).recipe.detected == "d3d9");
    }
    community.min_kit = "kit-h";
    community.tested_date = "2026-10-10";
    community.status = "works";
    community.notes = "Starts \"fine\".";
    community.base_repository = "https://github.com/example/prospero-win-profiles";
    community.base_path = "profiles/hl2.profile";
    const auto text = SerializeCommunityRecipe(community);
    GameRecipe as_recipe;
    CHECK(ParseGameRecipe(text, &as_recipe, &reason));
    CHECK(as_recipe.id == "community-220" && as_recipe.launch_entries.size() == 1 &&
          as_recipe.launch_entries[0].executable == "hl2_win64.exe" && as_recipe.architecture == "pe64" &&
          as_recipe.graphics == "opengl" && as_recipe.arguments == "-game hl2 {resolution}" &&
          as_recipe.resolution_arguments == hl2.resolution_arguments && as_recipe.dll_overrides == "d3d9=n;xinput1_3=n");
    CHECK(as_recipe.defaults.at("resolution") == "1280x720" && as_recipe.defaults.at("button.options") == "escape");
    CHECK(!as_recipe.defaults.count("controller") && as_recipe.locked.empty());  // xinput is Playden's default too
    CommunityRecipe parsed;
    CHECK(ParseCommunityRecipe(text, &parsed, &reason));
    CHECK(parsed.executable == "hl2_win64.exe" && parsed.status == "works" && parsed.notes == "Starts \"fine\"." &&
          parsed.base_path == "profiles/hl2.profile" && SerializeCommunityRecipe(parsed) == text);
    // The same game started from the exported recipe gets the same command.
    EffectiveSettings again;
    CHECK(BuildEffectiveSettings(as_recipe, nullptr, {}, KitCapabilities{}, &again, &error));
    CHECK(again.arguments == "-game hl2 -w 1280 -h 720" && again.dll_overrides == settings.dll_overrides);
    // A recipe exported from a generated one keeps the Steam emulator, unlocked.
    auto from_generated = generated;
    from_generated.defaults["steam_features"] = "emulate";
    CHECK(BuildEffectiveSettings(from_generated, nullptr, {}, KitCapabilities{}, &settings, &error));
    GameRecipe exported;
    CHECK(ParseGameRecipe(SerializeCommunityRecipe(BuildCommunityRecipe(from_generated, settings)), &exported, &reason));
    CHECK(exported.defaults.at("steam_features") == "emulate" && IsCommunityRecipeId(exported.id));
    CHECK(BuildEffectiveSettings(exported, nullptr, {}, KitCapabilities{}, &again, &error));
    CHECK(again.values.at("steam_features").value == "emulate" && !again.values.at("steam_features").locked);
    CHECK(again.dll_overrides == kDxvkOverrides);  // the DXVK overrides are written out, not added again
    // Registry values and requirements stay with the game when it is exported.
    auto with_extras = from_generated;
    with_extras.registry = {{"Software\\Wine\\AppDefaults\\a.exe\\Direct3D", "VideoMemorySize", "\"4096\""},
                            {"Software\\Studio\\Game", "Language", "\"en\""}};
    with_extras.requirements = {{"audio", "ps5"}};
    with_extras.ini = {{"BmGame/Config/UserEngine.ini", "Engine.Engine", "Language", "int"}};
    CHECK(ParseGameRecipe(SerializeCommunityRecipe(BuildCommunityRecipe(with_extras, settings)), &exported, &reason));
    CHECK(exported.requirements == with_extras.requirements && exported.registry.size() == 2);
    CHECK(exported.ini == with_extras.ini);
    for (const auto& value : with_extras.registry)
      CHECK(std::any_of(exported.registry.begin(), exported.registry.end(), [&](const RegistryValue& read) {
        return read.key == value.key && read.name == value.name && read.data == value.data;
      }));
    auto broken = text;
    broken.replace(broken.find("\"works\""), 7, "\"great\"");
    CHECK(!ParseCommunityRecipe(broken, &parsed, &reason) && reason.find("status") != std::string::npos);
  }

  std::puts("game settings tests passed");
}
