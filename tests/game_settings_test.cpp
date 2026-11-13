#include "runtime/game_settings.h"
#include "steam_id.h"
#include "runtime/game_probe.h"
#include "runtime/install_script.h"
#include "runtime/kit_version.h"
#include "runtime/recipe_choice.h"
#include "runtime/settings_publication.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
  // The Steam network and Online cases run with the experimental feature on; the last block turns it off.
  SetOnlineSteamEnabled(true);

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
        "launch_option", "program", "start_through_launcher", "launcher_controller_preset", "extra_arguments",
        "steam_features", "steam_sign_in", "game_invites",
        "resolution", "refresh", "frame_limit", "framegen", "vsync", "performance_overlay", "overlay_detail", "scaling",
        "window_view",
        "controller", "controller_preset", "touchpad_hold",
        "pointer_stick", "pointer_speed", "stick_deadzone", "rumble", "invert_stick", "trigger_deadzone", "wheel_stick",
        "volume",
        "start_check",
        "graphics", "d3d12", "pixel_shader_waves", "frame_latency", "reported_gpu", "reported_vram",
        "steam_library_path", "steam_network", "steam_input", "windows_version", "text_scale",
        "locale", "cpu_topology", "address_space", "cpu", "network_adapters", "logging",
        "environment", "program_folder", "program_arguments", "arguments", "architecture", "dll_overrides",
        "library_overrides", "registry_dll_overrides"};
    CHECK(ids == expected);
    for (std::size_t index = 1; index < SettingsCatalogue().size(); ++index)
      CHECK(SettingsCatalogue()[index - 1].group <= SettingsCatalogue()[index].group);
    CHECK(FindSetting("refresh")->group == SettingGroup::kDisplay &&
          FindSetting("button.cross")->group == SettingGroup::kButtonMapping &&
          FindSetting("rumble")->group == SettingGroup::kControllerTuning);
    // Online play has its own section after Game; Compatibility › Steam keeps the other Steam rows, above Windows;
    // Logging is in Advanced.
    for (const char* id : {"steam_features", "steam_sign_in", "game_invites"})
      CHECK(FindSetting(id)->group == SettingGroup::kOnline);
    for (const char* id : {"steam_library_path", "steam_network", "steam_input"})
      CHECK(FindSetting(id)->group == SettingGroup::kSteam);
    CHECK(FindSetting("windows_version")->group == SettingGroup::kWindows &&
          FindSetting("logging")->group == SettingGroup::kAdvanced);
  }

  // "Also called" names only what a player finds in Wine, DXVK or Steam tips: never a profile key ([section] key),
  // a Playden file or a prospero-win variable.
  for (const auto& definition : SettingsCatalogue()) {
    const auto& also = definition.also_called;
    CHECK(also.find('[') == std::string::npos && also.find("playden") == std::string::npos &&
          also.find("PW_") == std::string::npos);
  }
  CHECK(FindSetting("logging")->also_called == "WINEDEBUG" && FindSetting("resolution")->also_called.empty());

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
  // Starting through a launcher: only with the kit, written out, and refused when the kit can't read it.
  {
    KitCapabilities launcher_kit;
    launcher_kit.launcher_mode = true;
    KitCapabilities needs_update;
    needs_update.launcher_mode_needs_update = true;
    CHECK(!SettingShown(*FindSetting("start_through_launcher"), KitCapabilities{}));
    CHECK(SettingShown(*FindSetting("start_through_launcher"), launcher_kit));
    CHECK(SettingShown(*FindSetting("launcher_controller_preset"), needs_update));
    CHECK(BuildEffectiveSettings(hl2, nullptr, {{"controller_preset", "gamepad"}}, launcher_kit, &settings, &error));
    CHECK(settings.values.at("start_through_launcher").value == "off");
    CHECK(!settings.values.at("launcher_controller_preset").available);
    CHECK(RenderProsperoProfile(hl2, settings, target, &profile, &reason));
    CHECK(profile.text.find("[launcher]") == std::string::npos);
    CHECK(BuildEffectiveSettings(hl2, nullptr, {{"controller_preset", "gamepad"}, {"start_through_launcher", "on"}}, launcher_kit,
                                 &settings, &error));
    CHECK(settings.values.at("launcher_controller_preset").value == "mouse");
    CHECK(RenderProsperoProfile(hl2, settings, target, &profile, &reason));
    CHECK(profile.text.find("\npreset = gamepad\n") != std::string::npos);
    CHECK(profile.text.find("\n[launcher]\nlabel = Mouse\ngame_label = Gamepad\nmode = keyboard\nmouse = left_stick\n"
                            "mouse_speed = 900\ncross = mouse_left\ncircle = mouse_right\nsquare = mouse_middle\n") != std::string::npos);
    CHECK(BuildEffectiveSettings(hl2, nullptr, {{"start_through_launcher", "on"}, {"launcher_controller_preset", "gamepad"}},
                                 launcher_kit, &settings, &error));
    CHECK(RenderProsperoProfile(hl2, settings, target, &profile, &reason));
    CHECK(profile.text.find("\n[launcher]\nlabel = Gamepad\ngame_label = Game controls\nmode = xinput\nmouse = none\n") != std::string::npos);
    // A title on an older runtime gets no section: its parser would refuse the profile.
    CHECK(BuildEffectiveSettings(hl2, nullptr, {{"start_through_launcher", "on"}}, needs_update, &settings, &error));
    CHECK(RenderProsperoProfile(hl2, settings, target, &profile, &reason));
    CHECK(profile.text.find("[launcher]") == std::string::npos);
    // The section is only accepted when the renderer says so.
    const std::string with_section = std::string(profile.text) + "\n[launcher]\nmode = keyboard\n";
    std::string why;
    CHECK(!CheckProsperoProfile(with_section, &why, {}));
    ProsperoProfileFeatures launcher_features;
    launcher_features.launcher_section = true;
    const std::string bad_label = std::string("[application]\n") + "\n[launcher]\nlabel = a\"b\n";
    CHECK(!CheckProsperoProfile(bad_label, &why, launcher_features));
  }
  // Touchpad hold: the menu preset a touchpad hold swaps to, without a launcher ([launcher] chain = off). Only with the
  // kit and a title on its runtime; Start through a launcher has its own switch and wins.
  {
    const auto touchpad_kit = KitCapabilitiesFromManifest({kKitTouchpadSwitch, kKitLauncherMode}, false);
    CHECK(touchpad_kit.touchpad_switch && !KitCapabilitiesFromManifest({kKitLauncherMode}, false).touchpad_switch);
    const auto older_title = KitForTitleRuntime(touchpad_kit, false);
    CHECK(!older_title.touchpad_switch && older_title.touchpad_switch_needs_update);
    CHECK(BuildEffectiveSettings(hl2, nullptr, {}, touchpad_kit, &settings, &error));
    CHECK(settings.values.at("touchpad_hold").value == "off" && settings.values.at("touchpad_hold").available);
    CHECK(RenderProsperoProfile(hl2, settings, target, &profile, &reason));
    CHECK(profile.text.find("[launcher]") == std::string::npos);
    // On: the game keeps its own [input]; the menu preset is written out, with the D-pad as arrows and Triangle as Esc.
    CHECK(BuildEffectiveSettings(hl2, nullptr, {{"touchpad_hold", "mouse"}}, touchpad_kit, &settings, &error));
    CHECK(RenderProsperoProfile(hl2, settings, target, &profile, &reason));
    CHECK(profile.text.find("\n[launcher]\nchain = off\nlabel = Mouse\ngame_label = Game controls\nmode = keyboard\n"
                            "mouse = left_stick\nmouse_speed = 900\ncross = mouse_left\ncircle = mouse_right\n"
                            "square = mouse_middle\ntriangle = escape\nl2 = mouse_right\nr2 = mouse_left\nup = up\n"
                            "down = down\nleft = left\nright = right\noptions = enter\n") != std::string::npos);
    CHECK(profile.text.find("\n[input]\nmode = xinput\nmouse = none\n") != std::string::npos);
    CHECK(BuildEffectiveSettings(hl2, nullptr, {{"touchpad_hold", "mouse"}, {"controller_preset", "gamepad"}}, touchpad_kit,
                                 &settings, &error));
    CHECK(RenderProsperoProfile(hl2, settings, target, &profile, &reason));
    CHECK(profile.text.find("\n[launcher]\nchain = off\nlabel = Mouse\ngame_label = Gamepad\n") != std::string::npos);
    // Start through a launcher: one section, the launcher's, and the row says why it is off.
    CHECK(BuildEffectiveSettings(hl2, nullptr, {{"touchpad_hold", "mouse"}, {"start_through_launcher", "on"}}, touchpad_kit,
                                 &settings, &error));
    CHECK(!settings.values.at("touchpad_hold").available &&
          settings.values.at("touchpad_hold").reason ==
              "Start through a launcher already switches presets with the touchpad.");
    CHECK(RenderProsperoProfile(hl2, settings, target, &profile, &reason));
    CHECK(profile.text.find("chain = off") == std::string::npos &&
          profile.text.find("[launcher]") == profile.text.rfind("[launcher]") && profile.text.find("[launcher]") != std::string::npos);
    // Without the kit, or with a title on an older runtime, the row is off and nothing is written.
    CHECK(BuildEffectiveSettings(hl2, nullptr, {{"touchpad_hold", "mouse"}}, KitCapabilities{}, &settings, &error));
    CHECK(!settings.values.at("touchpad_hold").available &&
          settings.values.at("touchpad_hold").reason == "Needs a newer runtime kit that switches presets with the touchpad.");
    CHECK(RenderProsperoProfile(hl2, settings, target, &profile, &reason));
    CHECK(profile.text.find("[launcher]") == std::string::npos);
    CHECK(BuildEffectiveSettings(hl2, nullptr, {{"touchpad_hold", "mouse"}}, older_title, &settings, &error));
    CHECK(!settings.values.at("touchpad_hold").available);
    CHECK(RenderProsperoProfile(hl2, settings, target, &profile, &reason));
    CHECK(profile.text.find("[launcher]") == std::string::npos);
    // chain = off is only accepted when the renderer says so, and only as off.
    std::string why;
    ProsperoProfileFeatures launcher_only;
    launcher_only.launcher_section = true;
    const std::string chain_off = "[application]\n\n[launcher]\nchain = off\n";
    CHECK(!CheckProsperoProfile(chain_off, &why, launcher_only));
    ProsperoProfileFeatures chain_features = launcher_only;
    chain_features.launcher_chain_off = true;
    CHECK(!CheckProsperoProfile("[application]\n\n[launcher]\nchain = on\n", &why, chain_features));
  }
  // A kit whose only newer capability needs the title on its runtime still makes the host read the title's markers:
  // otherwise a profile line the older title refuses goes out (helper-process alone once skipped the check).
  {
    for (const char* name : {kKitDisplay120Hz, kKitProfileVersion2, kKitInputRumble, kKitDisplayFramegen, kKitDisplayFpsCounter,
                             kKitLauncherMode, kKitTouchpadSwitch, kKitInputPlaystation, kKitTitleNotify, kKitHelperProcess,
                             kKitGameRootUsbSearch, kKitLaunchArguments4096, kKitLargeAddressAware, kKitNsiproxySwitch,
                             kKitEnvironmentFileBudgets, kKitSteamLibraryPath}) {
      if (!KitDependsOnTitleRuntime(KitCapabilitiesFromManifest({name}, false)))
        std::fprintf(stderr, "%s alone doesn't check the title's runtime\n", name);
      CHECK(KitDependsOnTitleRuntime(KitCapabilitiesFromManifest({name}, false)));
    }
    CHECK(!KitDependsOnTitleRuntime(KitCapabilities{}));
    CHECK(!KitDependsOnTitleRuntime(KitCapabilitiesFromManifest({kKitDxvkConfig, kKitWineMono, kKitOpenGLZink}, true)));
  }
  // Steam features = Online: with a runtime that runs a helper, the profile starts the Steam host beside the game and
  // holds the game until it is ready; without one, nothing is written. The section is refused unless the renderer says so.
  {
    const auto helper_kit = KitCapabilitiesFromManifest({kKitHelperProcess}, false);
    CHECK(helper_kit.helper_process && !KitCapabilitiesFromManifest({kKitInputRumble}, false).helper_process);
    CHECK(std::find(SettingsKitCapabilityNames().begin(), SettingsKitCapabilityNames().end(), kKitHelperProcess) !=
          SettingsKitCapabilityNames().end());
    const auto stale = KitForTitleRuntime(helper_kit, false);
    CHECK(!stale.helper_process && stale.helper_process_needs_update && KitForTitleRuntime(helper_kit, true).helper_process);
    const std::map<std::string, std::string> online = {{"unlock.steam_features", "on"}, {"steam_features", "online"}};
    CHECK(BuildEffectiveSettings(hl2, nullptr, online, helper_kit, &settings, &error) && settings.helper_process);
    CHECK(RenderProsperoProfile(hl2, settings, target, &profile, &reason));
    CHECK(profile.text.find("\n[helper]\nprogram = Z:\\data\\playden\\steam-client\\client\\steam.exe\n"
                            "ready = playden/steam-host.ready\nwait = 60\n") != std::string::npos);
    // Without the Steam host, Online falls back to the game's default and says why; a title still on an older runtime
    // keeps Online for its update.
    CHECK(BuildEffectiveSettings(hl2, nullptr, online, KitCapabilities{}, &settings, &error) && !settings.helper_process);
    CHECK(settings.values.at("steam_features").value != "online" &&
          settings.values.at("steam_features").reason == "Needs a runtime kit that runs Steam beside the game.");
    CHECK(RenderProsperoProfile(hl2, settings, target, &profile, &reason));
    CHECK(profile.text.find("[helper]") == std::string::npos);
    CHECK(BuildEffectiveSettings(hl2, nullptr, online, stale, &settings, &error));
    CHECK(settings.values.at("steam_features").value != "online" && settings.values.at("steam_features").waits_for_update &&
          settings.values.at("steam_features").after_update == "online");
    CHECK(BuildEffectiveSettings(hl2, nullptr, {{"unlock.steam_features", "on"}, {"steam_features", "emulate"}}, helper_kit,
                                 &settings, &error));
    CHECK(RenderProsperoProfile(hl2, settings, target, &profile, &reason));
    CHECK(profile.text.find("[helper]") == std::string::npos);
    std::string why;
    const std::string base = profile.text;
    ProsperoProfileFeatures helper_features;
    helper_features.helper_section = true;
    const std::string program = "program = C:\\Tools\\a b.exe\n";
    CHECK(!CheckProsperoProfile(base + "\n[helper]\n" + program, &why, {}));
    CHECK(CheckProsperoProfile(base + "\n[helper]\n" + program, &why, helper_features));
    CHECK(CheckProsperoProfile(base + "\n[helper]\n" + program + "ready = a/b.ready\nwait = 120\n", &why, helper_features));
    for (const char* bad : {"", "program = tools\\a.exe\n", "program = C:/a.exe\n", "program = C:\\a\"b.exe\n",
                            "program = C:\\a.exe\nready = ../b\n", "program = C:\\a.exe\nready = /b\n",
                            "program = C:\\a.exe\nready = a//b\n", "program = C:\\a.exe\nready = a\\b\n",
                            "program = C:\\a.exe\nwait = 0\n", "program = C:\\a.exe\nwait = 121\n",
                            "program = C:\\a.exe\nother = 1\n", "program = C:\\a.exe\nprogram = C:\\b.exe\n"})
      CHECK(!CheckProsperoProfile(base + "\n[helper]\n" + bad, &why, helper_features));
  }
  // A Steam game shown in steamapps\common: the target names its folder only for a title whose runtime reads [steam]
  // (the host decides, SteamLibraryFolder); the profile keeps the real paths and adds the section. The section is
  // refused unless the renderer says so, and left out when a path there would be too long for the runtime.
  {
    const auto steam_kit = KitCapabilitiesFromManifest({kKitSteamLibraryPath}, false);
    CHECK(steam_kit.steam_library_path && !KitCapabilitiesFromManifest({kKitHelperProcess}, false).steam_library_path);
    CHECK(!KitForTitleRuntime(steam_kit, false).steam_library_path && KitForTitleRuntime(steam_kit, true).steam_library_path);
    auto steam_target = target;
    steam_target.steam_install_dir = "Half-Life 2";
    // Only with Steam library folder = Steam's folder: a game installed before this Playden keeps Playden's folder, so
    // does one whose player chose it, and a runtime kit without the capability shows none.
    for (const auto& overrides : std::vector<std::map<std::string, std::string>>{
             {}, {{"steam_library_path", "playden"}}, {{"default.steam_library_path", "steam"}, {"steam_library_path", "playden"}}}) {
      CHECK(BuildEffectiveSettings(hl2, nullptr, overrides, steam_kit, &settings, &error));
      CHECK(settings.values.at("steam_library_path").value == "playden");
      CHECK(RenderProsperoProfile(hl2, settings, steam_target, &profile, &reason));
      CHECK(profile.text.find("[steam]") == std::string::npos);
    }
    CHECK(BuildEffectiveSettings(hl2, nullptr, {{"default.steam_library_path", "steam"}}, KitCapabilities{}, &settings, &error));
    CHECK(RenderProsperoProfile(hl2, settings, steam_target, &profile, &reason));
    CHECK(profile.text.find("[steam]") == std::string::npos);
    CHECK(BuildEffectiveSettings(hl2, nullptr, {{"steam_library_path", "steam"}}, steam_kit, &settings, &error));
    CHECK(RenderProsperoProfile(hl2, settings, steam_target, &profile, &reason));
    CHECK(profile.text.find("\n[steam]\ninstall_dir = Half-Life 2\n") != std::string::npos);
    CHECK(BuildEffectiveSettings(hl2, nullptr, {{"default.steam_library_path", "steam"}}, steam_kit, &settings, &error));
    CHECK(RenderProsperoProfile(hl2, settings, steam_target, &profile, &reason));
    CHECK(profile.text.find("\n[steam]\ninstall_dir = Half-Life 2\n") != std::string::npos);
    CHECK(profile.text.find("executable = Z:\\data\\homebrew\\FAKE77031\\games\\220\\") != std::string::npos);
    CHECK(RenderProsperoProfile(hl2, settings, target, &profile, &reason));
    CHECK(profile.text.find("[steam]") == std::string::npos);
    // 46 bytes of C:\Program Files (x86)\Steam\steamapps\common\, the folder and the program: a folder name that fits
    // alone but not with the program keeps the game at its real path.
    steam_target.steam_install_dir = std::string(128, 'a');
    CHECK(RenderProsperoProfile(hl2, settings, steam_target, &profile, &reason));
    CHECK(profile.text.find("[steam]") != std::string::npos);
    {
      auto deep = settings;
      auto entry = std::make_shared<LaunchEntry>(*settings.launch);
      // 45 + 1 + 128 + 1 + 84 = 259 bytes fit, one more doesn't.
      entry->executable = std::string(80, 'b') + ".exe";
      deep.launch_storage = entry;
      deep.launch = entry.get();
      CHECK(RenderProsperoProfile(hl2, deep, steam_target, &profile, &reason));
      CHECK(profile.text.find("[steam]") != std::string::npos);
      entry->executable = std::string(81, 'b') + ".exe";
      CHECK(RenderProsperoProfile(hl2, deep, steam_target, &profile, &reason));
      CHECK(profile.text.find("[steam]") == std::string::npos);
    }
    steam_target.steam_install_dir = "a/b";
    CHECK(RenderProsperoProfile(hl2, settings, steam_target, &profile, &reason));
    CHECK(profile.text.find("[steam]") == std::string::npos);
    std::string why;
    const std::string base = profile.text;
    ProsperoProfileFeatures steam_features;
    steam_features.steam_section = true;
    CHECK(!CheckProsperoProfile(base + "\n[steam]\ninstall_dir = Half-Life 2\n", &why, {}));
    CHECK(CheckProsperoProfile(base + "\n[steam]\ninstall_dir = Half-Life 2\n", &why, steam_features));
    CHECK(CheckProsperoProfile(base + "\n[steam]\ninstall_dir = 257510\n", &why, steam_features));
    for (const char* bad : {"", "install_dir =\n", "install_dir = ..\n", "install_dir = a\\b\n", "install_dir = a/b\n",
                            "install_dir = C:\n", "install_dir = a\"b\n", "install_dir = trailing.\n", "other = 1\n",
                            "install_dir = a\ninstall_dir = b\n"})
      CHECK(!CheckProsperoProfile(base + "\n[steam]\n" + bad, &why, steam_features));
    CHECK(IsProsperoSteamInstallDir("The Talos Principle") && IsProsperoSteamInstallDir(std::string(128, 'a')) &&
          !IsProsperoSteamInstallDir(std::string(129, 'a')) && !IsProsperoSteamInstallDir("a\tb"));
  }
  // Steam library folder: Playden's folder by default, so a game installed before this Playden keeps its path. A Steam
  // game this Playden installs holds "default.steam_library_path" = steam, its own default: Playden's value
  // (kPlayden), not a change of the player's, never shared or put in a preset; a recipe and the player win over it.
  {
    const auto* row = FindSetting("steam_library_path");
    CHECK(row && row->label == "Steam library folder" && row->group == SettingGroup::kSteam &&
          row->apply == ApplyMode::kGameClosed && row->playden_default == "playden" && row->choices.size() == 2 &&
          row->choices[0].value == "steam" && row->choices[0].label == "Steam's folder" &&
          row->choices[1].value == "playden" && row->choices[1].label == "Playden's folder");
    CHECK(SettingShown(*row, KitCapabilities{}));
    const auto steam_kit = KitCapabilitiesFromManifest({kKitSteamLibraryPath}, true);
    EffectiveSettings values;
    const auto effective = [&](const std::map<std::string, std::string>& overrides, const KitCapabilities& kit,
                               const GameRecipe& recipe) -> const EffectiveValue& {
      CHECK(BuildEffectiveSettings(recipe, nullptr, overrides, kit, &values, &error));
      return values.values.at("steam_library_path");
    };
    auto value = effective({}, steam_kit, hl2);
    CHECK(value.value == "playden" && value.source == ValueSource::kPlayden && value.available);
    value = effective({{"default.steam_library_path", "steam"}}, steam_kit, hl2);
    CHECK(value.value == "steam" && value.source == ValueSource::kPlayden && value.available);
    value = effective({{"default.steam_library_path", "steam"}, {"steam_library_path", "playden"}}, steam_kit, hl2);
    CHECK(value.value == "playden" && value.source == ValueSource::kUser);
    value = effective({{"steam_library_path", "steam"}}, steam_kit, hl2);
    CHECK(value.value == "steam" && value.source == ValueSource::kUser);
    // A default whose value isn't one of the row's is ignored.
    value = effective({{"default.steam_library_path", "elsewhere"}}, steam_kit, hl2);
    CHECK(value.value == "playden" && value.source == ValueSource::kPlayden);
    // A recipe may set the row (The Talos Principle would); the player still wins.
    auto talos = hl2;
    talos.defaults["steam_library_path"] = "steam";
    {
      GameRecipe reread;
      CHECK(ParseGameRecipe(SerializeGameRecipe(talos), &reread, &reason) &&
            reread.defaults.at("steam_library_path") == "steam");
    }
    value = effective({}, steam_kit, talos);
    CHECK(value.value == "steam" && value.source == ValueSource::kRecipe);
    value = effective({{"steam_library_path", "playden"}}, steam_kit, talos);
    CHECK(value.value == "playden" && value.source == ValueSource::kUser);
    // A runtime kit that can't show the game there, or a title still on an older runtime, disables the row, which then
    // is Playden's folder whatever was chosen. The second applies once the game's runtime is updated, and says so.
    for (const auto& kit : {KitCapabilities{}, KitForTitleRuntime(steam_kit, false)}) {
      value = effective({{"default.steam_library_path", "steam"}, {"steam_library_path", "steam"}}, kit, hl2);
      CHECK(!value.available && value.value == "playden");
    }
    CHECK(effective({}, KitCapabilities{}, hl2).reason == "Needs a newer runtime kit that shows the game in Steam's folder.");
    CHECK(KitForTitleRuntime(steam_kit, false).steam_library_path_needs_update &&
          !KitForTitleRuntime(steam_kit, true).steam_library_path_needs_update &&
          !KitForTitleRuntime(KitCapabilities{}, false).steam_library_path_needs_update);
    CHECK(effective({}, KitForTitleRuntime(steam_kit, false), hl2).reason == kAppliesAfterKitUpdate);

    // The reserved key.
    CHECK(IsGameDefaultKey("default.steam_library_path") && !IsGameDefaultKey("default.") &&
          !IsGameDefaultKey("default.resolution") && !IsGameDefaultKey("steam_library_path") &&
          !IsUnlockKey("default.steam_library_path"));
    const std::map<std::string, std::string> mixed = {{"default.steam_library_path", "steam"}, {"resolution", "1280x720"}};
    CHECK(GameDefaultOverrides(mixed) == (std::map<std::string, std::string>{{"default.steam_library_path", "steam"}}));
    CHECK(HasPlayerOverrides(mixed) && !HasPlayerOverrides(GameDefaultOverrides(mixed)) && !HasPlayerOverrides({}));
    // Saved and read back with the record; one this build doesn't take, or with a value the row doesn't have, is dropped.
    GameSettingsRecord record;
    record.overrides = {{"default.steam_library_path", "steam"}, {"default.resolution", "1280x720"}};
    GameSettingsRecord reread;
    CHECK(ParseGameSettingsRecord(SerializeGameSettingsRecord(record), &reread, &reason));
    CHECK(reread.overrides == (std::map<std::string, std::string>{{"default.steam_library_path", "steam"}}));
    record.overrides = {{"default.steam_library_path", "elsewhere"}};
    CHECK(ParseGameSettingsRecord(SerializeGameSettingsRecord(record), &reread, &reason) && reread.overrides.empty());

    // Where a game is installed is its own: never in a preset, whoever chose it.
    CHECK(!ValidatePresetSetting("steam_library_path", "steam", &reason));
    CHECK(BuildEffectiveSettings(hl2, nullptr, {{"steam_library_path", "steam"}}, steam_kit, &values, &error));
    CHECK(!PresetSettingsFromGame(hl2, values, PresetCapture::kSave).count("steam_library_path"));

    // Shares: the game's own default stays out (an older Playden refuses a recipe naming a row it doesn't know); a
    // value the player or a recipe set goes in.
    const auto ref = playden::GameRef::Steam(220);
    CHECK(BuildEffectiveSettings(hl2, nullptr, {{"default.steam_library_path", "steam"}}, steam_kit, &values, &error));
    CHECK(!BuildCommunityRecipe(hl2, values, ref, "Half-Life 2").recipe.defaults.count("steam_library_path"));
    CHECK(BuildEffectiveSettings(hl2, nullptr, {}, steam_kit, &values, &error));
    CHECK(!BuildCommunityRecipe(hl2, values, ref, "Half-Life 2").recipe.defaults.count("steam_library_path"));
    CHECK(BuildEffectiveSettings(hl2, nullptr, {{"steam_library_path", "steam"}}, steam_kit, &values, &error));
    CHECK(BuildCommunityRecipe(hl2, values, ref, "Half-Life 2").recipe.defaults.at("steam_library_path") == "steam");
    // A chosen Playden's folder goes in too: on another new install the game's own default would be Steam's folder.
    CHECK(BuildEffectiveSettings(hl2, nullptr, {{"default.steam_library_path", "steam"}, {"steam_library_path", "playden"}},
                                 steam_kit, &values, &error));
    CHECK(BuildCommunityRecipe(hl2, values, ref, "Half-Life 2").recipe.defaults.at("steam_library_path") == "playden");
    CHECK(BuildEffectiveSettings(talos, nullptr, {}, steam_kit, &values, &error));
    CHECK(BuildCommunityRecipe(talos, values, ref, "Half-Life 2").recipe.defaults.at("steam_library_path") == "steam");
  }
  // Playden's tables of preset rows and buttons match the shipped input files.
  for (const auto& [name, buttons] : ControllerPresetButtons()) {
    const auto file = ReadText(source / ("runtime/input/" + name + ".input"));
    for (const auto& button : ProsperoButtons()) {
      const auto line = "\n" + button + " = ";
      const auto at = file.find(line);
      const std::string in_file = at == std::string::npos ? "" : file.substr(at + line.size(), file.find('\n', at + line.size()) - at - line.size());
      const auto row = buttons.find(button);
      CHECK(in_file == (row == buttons.end() ? std::string() : row->second));
    }
  }
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
    // A runtime with the driver's counter shows the frame rate instead of DXVK's overlay.
    auto counter_kit = display;
    counter_kit.display_fps_counter = true;
    KitCapabilities counter_stale = display;
    counter_stale.display_fps_counter_needs_update = true;
    EffectiveSettings counted;
    CHECK(BuildEffectiveSettings(d3d12, nullptr, {{"performance_overlay", "on"}}, counter_kit, &counted, &error));
    CHECK(counted.values.at("performance_overlay").available && !counted.values.at("overlay_detail").available);
    CHECK(counted.values.at("overlay_detail").reason == "Direct3D 12 games show only the frame rate.");
    ProsperoProfile counted_profile;
    CHECK(RenderProsperoProfile(d3d12, counted, target, &counted_profile, &reason));
    CHECK(counted_profile.text.find("\nfps_counter = true\n") != std::string::npos);
    CHECK(counted_profile.text.find("show_fps = false\n") != std::string::npos);
    CHECK(!CheckProsperoProfile(counted_profile.text, &reason, {.debug_env = true}));
    CHECK(CheckProsperoProfile(counted_profile.text, &reason, {.debug_env = true, .display_fps_counter = true}));
    // Off writes nothing an older runtime would refuse, and neither does a game on Direct3D 9-11.
    CHECK(BuildEffectiveSettings(d3d12, nullptr, {}, counter_kit, &counted, &error));
    CHECK(RenderProsperoProfile(d3d12, counted, target, &counted_profile, &reason));
    CHECK(counted_profile.text.find("fps_counter") == std::string::npos);
    CHECK(BuildEffectiveSettings(hl2, nullptr, {{"performance_overlay", "on"}}, counter_kit, &counted, &error));
    CHECK(RenderProsperoProfile(hl2, counted, target, &counted_profile, &reason));
    CHECK(counted_profile.text.find("fps_counter") == std::string::npos);
    // A title on an older runtime keeps the choice for its update, and its profile stays readable there.
    CHECK(BuildEffectiveSettings(d3d12, nullptr, {{"performance_overlay", "on"}}, counter_stale, &counted, &error));
    CHECK(!counted.values.at("performance_overlay").available &&
          counted.values.at("performance_overlay").reason == kAppliesAfterKitUpdate);
    CHECK(RenderProsperoProfile(d3d12, counted, target, &counted_profile, &reason));
    CHECK(counted_profile.text.find("fps_counter") == std::string::npos);
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

  // The switch that keeps a game off the kit's direct-memory sections reaches the title as [debug] env.
  {
    KitCapabilities debug_kit;
    debug_kit.environment = true;
    debug_kit.opengl_zink = true;
    CHECK(BuildEffectiveSettings(hl2, nullptr, {{"environment", "PW_SECTIONS_FLEXIBLE=1"}}, debug_kit, &settings, &error));
    CHECK(RenderProsperoProfile(hl2, settings, target, &profile, &reason));
    CHECK(profile.text.find("env = PW_SECTIONS_FLEXIBLE=1\n") != std::string::npos);
  }

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
    CHECK(CheckProsperoProfile(base + "\n[runtime]\ncpu = native\n", &reason, {.runtime_version = true}));
    CHECK(!CheckProsperoProfile(base + "\n[runtime]\ncpu = fast\n", &reason, {.runtime_version = true}));
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
    // Address space: WINE_LARGE_ADDRESS_AWARE in [environment] for a 32-bit program, with a runtime that takes it.
    {
      auto lae = every;
      lae.environment = lae.large_address_aware = lae.environment_file_budgets = lae.launch_arguments_4096 = true;
      CHECK(BuildEffectiveSettings(hl2, nullptr, {{"address_space", "4gb"}, {"cpu_topology", "4"}}, lae, &settings,
                                   &error));
      CHECK(settings.values.at("address_space").available);
      CHECK(RenderProsperoProfile(hl2, settings, target, &profile, &reason));
      CHECK(profile.text.find("\n[environment]\nWINE_CPU_TOPOLOGY = 4:0,1,2,3\nWINE_LARGE_ADDRESS_AWARE = 1\n") !=
            std::string::npos);
      CHECK(CheckProsperoProfile(profile.text, &reason, {.environment_section = true}));
      // Environment variables may set it itself, and it is written once.
      CHECK(BuildEffectiveSettings(hl2, nullptr, {{"address_space", "4gb"}, {"environment", "WINE_LARGE_ADDRESS_AWARE=1"}},
                                   lae, &settings, &error));
      CHECK(RenderProsperoProfile(hl2, settings, target, &profile, &reason));
      CHECK(profile.text.find("WINE_LARGE_ADDRESS_AWARE") == profile.text.rfind("WINE_LARGE_ADDRESS_AWARE"));
      // An older runtime has no such row, and refuses the variable from Environment variables too.
      CHECK(BuildEffectiveSettings(hl2, nullptr, {{"address_space", "4gb"}}, every, &settings, &error));
      CHECK(!settings.values.at("address_space").available && settings.values.at("address_space").value == "kit");
      auto plain = every;
      plain.environment = true;
      CHECK(!BuildEffectiveSettings(hl2, nullptr, {{"environment", "WINE_LARGE_ADDRESS_AWARE=1"}}, plain, &settings,
                                    &error));
      CHECK(error.setting_id == "environment" && error.message == "WINE_LARGE_ADDRESS_AWARE needs a newer runtime kit");
      CHECK(!BuildEffectiveSettings(hl2, nullptr, {{"environment", "WINE_PS5_PARK_FILES=96"}}, plain, &settings, &error));
      // A value that only contains the name sets nothing: an older runtime takes it, and the row still writes its own.
      CHECK(BuildEffectiveSettings(hl2, nullptr, {{"environment", "SDL_HINT_X=a WINE_LARGE_ADDRESS_AWARE=1"}}, plain,
                                   &settings, &error));
      CHECK(BuildEffectiveSettings(hl2, nullptr, {{"address_space", "4gb"}, {"environment", "SDL_HINT_X=X_WINE_LARGE_ADDRESS_AWARE=1"}},
                                   lae, &settings, &error));
      CHECK(RenderProsperoProfile(hl2, settings, target, &profile, &reason));
      CHECK(profile.text.find("\nWINE_LARGE_ADDRESS_AWARE = 1\n") != std::string::npos);
      CHECK(!BuildEffectiveSettings(hl2, nullptr, {{"environment", "WINE_PS5_PARK_FILES=96; WINE_PS5_CACHED_FILES=8"}},
                                    KitForTitleRuntime(lae, false), &settings, &error));
      CHECK(error.message == "WINE_PS5_PARK_FILES needs the game's runtime kit update");
      CHECK(BuildEffectiveSettings(hl2, nullptr, {{"environment", "WINE_PS5_PARK_FILES=96; WINE_PS5_CACHED_FILES=8"}},
                                   lae, &settings, &error));
      CHECK(RenderProsperoProfile(hl2, settings, target, &profile, &reason));
      CHECK(profile.text.find("WINE_PS5_PARK_FILES = 96\nWINE_PS5_CACHED_FILES = 8\n") != std::string::npos);
      CHECK(!ValidateSettingValue(hl2, "environment", "WINE_LARGE_ADDRESS_AWARE=2", &reason));
      CHECK(!ValidateSettingValue(hl2, "environment", "WINE_PS5_PARK_FILES=12345", &reason));
      // A title that waits for its update keeps the choice for then.
      CHECK(BuildEffectiveSettings(hl2, nullptr, {{"address_space", "4gb"}}, KitForTitleRuntime(lae, false), &settings,
                                   &error));
      CHECK(settings.values.at("address_space").waits_for_update && settings.values.at("address_space").after_update == "4gb");
      // Only for 32-bit programs.
      auto pe64 = hl2;
      pe64.architecture = "pe64";
      CHECK(BuildEffectiveSettings(pe64, nullptr, {{"address_space", "4gb"}}, lae, &settings, &error));
      CHECK(!settings.values.at("address_space").available &&
            settings.values.at("address_space").reason == "Only for 32-bit programs.");
      // Network adapters Off: PW_NSIPROXY_DISABLE in [environment] for a 64-bit program, with a runtime that takes it.
      {
        auto nsi = every;
        nsi.environment = nsi.nsiproxy_switch = true;
        CHECK(BuildEffectiveSettings(pe64, nullptr, {{"network_adapters", "off"}}, nsi, &settings, &error));
        CHECK(settings.values.at("network_adapters").available);
        CHECK(RenderProsperoProfile(pe64, settings, target, &profile, &reason));
        CHECK(profile.text.find("\n[environment]\nPW_NSIPROXY_DISABLE = 1\n") != std::string::npos);
        CHECK(CheckProsperoProfile(profile.text, &reason, {.environment_section = true}));
        // Runtime default writes nothing.
        CHECK(BuildEffectiveSettings(pe64, nullptr, {}, nsi, &settings, &error));
        CHECK(RenderProsperoProfile(pe64, settings, target, &profile, &reason));
        CHECK(profile.text.find("PW_NSIPROXY_DISABLE") == std::string::npos);
        // Environment variables may set it itself, and it is written once.
        CHECK(BuildEffectiveSettings(pe64, nullptr, {{"network_adapters", "off"}, {"environment", "PW_NSIPROXY_DISABLE=1"}},
                                     nsi, &settings, &error));
        CHECK(RenderProsperoProfile(pe64, settings, target, &profile, &reason));
        CHECK(profile.text.find("PW_NSIPROXY_DISABLE") == profile.text.rfind("PW_NSIPROXY_DISABLE"));
        // An older runtime has no such row, and refuses the variable from Environment variables too.
        auto older = every;
        older.environment = true;
        CHECK(BuildEffectiveSettings(pe64, nullptr, {{"network_adapters", "off"}}, older, &settings, &error));
        CHECK(!settings.values.at("network_adapters").available && settings.values.at("network_adapters").value == "kit");
        CHECK(!BuildEffectiveSettings(pe64, nullptr, {{"environment", "PW_NSIPROXY_DISABLE=1"}}, older, &settings, &error));
        CHECK(error.setting_id == "environment" && error.message == "PW_NSIPROXY_DISABLE needs a newer runtime kit");
        // A title that waits for its update keeps the choice for then.
        CHECK(BuildEffectiveSettings(pe64, nullptr, {{"network_adapters", "off"}}, KitForTitleRuntime(nsi, false),
                                     &settings, &error));
        CHECK(settings.values.at("network_adapters").waits_for_update &&
              settings.values.at("network_adapters").after_update == "off");
        CHECK(!BuildEffectiveSettings(pe64, nullptr, {{"environment", "PW_NSIPROXY_DISABLE=1"}},
                                      KitForTitleRuntime(nsi, false), &settings, &error));
        CHECK(error.message == "PW_NSIPROXY_DISABLE needs the game's runtime kit update");
        // A 32-bit program hosts no nsiproxy anyway: the row stays open (an unprepared game has no architecture yet).
        CHECK(BuildEffectiveSettings(hl2, nullptr, {{"network_adapters", "off"}}, nsi, &settings, &error));
        CHECK(settings.values.at("network_adapters").available);
        // Like CPU cores, it takes its place before the player's variables, which the limit trims.
        CHECK(BuildEffectiveSettings(pe64, nullptr,
                                     {{"network_adapters", "off"},
                                      {"environment", "DXVK_A=1; DXVK_B=1; DXVK_C=1; DXVK_D=1; DXVK_E=1; DXVK_F=1; "
                                                      "DXVK_G=1; DXVK_H=1"}},
                                     nsi, &settings, &error));
        CHECK(RenderProsperoProfile(pe64, settings, target, &profile, &reason));
        CHECK(profile.text.find("\n[environment]\nPW_NSIPROXY_DISABLE = 1\n") != std::string::npos);
        CHECK(KitCapabilitiesFromManifest({kKitNsiproxySwitch}, true).nsiproxy_switch);
      }
      // 32-bit code: Native writes nothing; Translator writes [runtime] cpu, once, with the version when there is one.
      auto v2 = every;
      v2.profile_version_2 = true;
      CHECK(BuildEffectiveSettings(hl2, nullptr, {}, v2, &settings, &error));
      CHECK(settings.values.at("cpu").available && settings.values.at("cpu").value == "native");
      CHECK(RenderProsperoProfile(hl2, settings, target, &profile, &reason));
      CHECK(profile.text.find("cpu =") == std::string::npos);
      CHECK(BuildEffectiveSettings(hl2, nullptr, {{"cpu", "translator"}}, v2, &settings, &error));
      CHECK(RenderProsperoProfile(hl2, settings, target, &profile, &reason));
      CHECK(profile.text.find("\ncpu = translator\n") != std::string::npos);
      CHECK(profile.text.find("[runtime]") == profile.text.rfind("[runtime]"));
      CHECK(CheckProsperoProfile(profile.text, &reason, {.runtime_version = true}));
      // A generated recipe's default (a Watcom program) is the row's value.
      auto watcom = hl2;
      watcom.defaults["cpu"] = "translator";
      CHECK(BuildEffectiveSettings(watcom, nullptr, {}, v2, &settings, &error));
      CHECK(settings.values.at("cpu").value == "translator");
      CHECK(BuildEffectiveSettings(pe64, nullptr, {{"cpu", "translator"}}, v2, &settings, &error));
      CHECK(!settings.values.at("cpu").available && settings.values.at("cpu").reason == "Only for 32-bit programs.");
      // A title that waits for its update keeps the choice for then.
      CHECK(BuildEffectiveSettings(hl2, nullptr, {{"cpu", "translator"}}, KitForTitleRuntime(v2, false), &settings,
                                   &error));
      CHECK(settings.values.at("cpu").waits_for_update && settings.values.at("cpu").after_update == "translator");
      auto version_1 = v2;
      version_1.profile_version_2 = false;
      CHECK(BuildEffectiveSettings(hl2, nullptr, {{"cpu", "translator"}}, version_1, &settings, &error));
      CHECK(!settings.values.at("cpu").available);
      CHECK(RenderProsperoProfile(hl2, settings, target, &profile, &reason));
      CHECK(profile.text.find("cpu =") == std::string::npos);
      // A runtime that takes 4096-byte arguments also takes a profile up to 16384 bytes, with short arguments too.
      CHECK(BuildEffectiveSettings(hl2, nullptr, {}, lae, &settings, &error) && settings.long_profile);
      CHECK(BuildEffectiveSettings(hl2, nullptr, {}, every, &settings, &error) && !settings.long_profile);
      CHECK(BuildEffectiveSettings(hl2, nullptr, {}, KitForTitleRuntime(lae, false), &settings, &error) &&
            !settings.long_profile);
      // Arguments past 512 bytes: only for a runtime that takes 4096 (a long mod list on one command line).
      const std::string mods = "/singleplayer _MODULES_*" + std::string(1500, 'M') + "*_MODULES_";
      CHECK(ValidateSettingValue(hl2, "extra_arguments", mods, &reason));
      CHECK(!ValidateSettingValue(hl2, "extra_arguments", std::string(4097, 'a'), &reason));
      CHECK(!BuildEffectiveSettings(hl2, nullptr, {{"extra_arguments", mods}}, every, &settings, &error));
      CHECK(error.setting_id == "extra_arguments" && error.message == "arguments past 512 bytes need a newer runtime kit");
      CHECK(!BuildEffectiveSettings(hl2, nullptr, {{"extra_arguments", mods}}, KitForTitleRuntime(lae, false), &settings,
                                    &error));
      CHECK(error.message == "arguments past 512 bytes need the game's runtime kit update");
      CHECK(BuildEffectiveSettings(hl2, nullptr, {{"extra_arguments", mods}}, lae, &settings, &error));
      CHECK(RenderProsperoProfile(hl2, settings, target, &profile, &reason));
      CHECK(profile.text.find(mods) != std::string::npos);
      CHECK(!CheckProsperoProfile(profile.text, &reason, {.environment_section = true}));  // a 512-byte title refuses it
      CHECK(CheckProsperoProfile(profile.text, &reason, {.environment_section = true, .long_arguments = true}));
    }
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
  CHECK(!settings.values.at("rumble").available && settings.values.at("rumble").reason == "Only with a controller.");
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
    CHECK(settings.values.at("rumble").reason == "Only with a controller.");
  }
  // The PlayStation controller (input-playstation): offered with the capability, waiting for the title's update on an
  // older runtime, and Xbox controller (no new mode in the profile) until it counts. Rumble applies to it; the dead
  // zones stay the Xbox controller's.
  {
    const auto playstation_kit = KitCapabilitiesFromManifest({kKitInputPlaystation, kKitInputRumble, kKitInputTuning}, false);
    CHECK(playstation_kit.input_playstation && !KitCapabilitiesFromManifest({kKitInputRumble}, false).input_playstation);
    CHECK(FindSetting("controller")->choices.size() == 3 && FindSetting("controller")->choices[1].value == "playstation");
    CHECK(FindSetting("controller")->choices[1].label == "PlayStation controller");
    CHECK(ValidateSettingValue(hl2, "controller", "playstation", &reason));
    const std::map<std::string, std::string> chosen = {
        {"controller", "playstation"}, {"rumble", "50"}, {"stick_deadzone", "10"}};
    CHECK(BuildEffectiveSettings(hl2, nullptr, chosen, playstation_kit, &settings, &error));
    CHECK(Value(settings, "controller") == "playstation" && settings.values.at("controller").available);
    CHECK(SettingChoiceUnavailable(settings, "controller", "playstation").empty());
    CHECK(!SettingChoiceWaitsForUpdate(settings, "controller", "playstation"));
    CHECK(settings.values.at("rumble").available);
    CHECK(!settings.values.at("stick_deadzone").available &&
          settings.values.at("stick_deadzone").reason == "Only with the Xbox controller.");
    CHECK(RenderProsperoProfile(hl2, settings, target, &profile, &reason));
    CHECK(profile.text.find("\nmode = playstation\n") != std::string::npos);
    CHECK(profile.text.find("\nrumble = 50\n") != std::string::npos && profile.text.find("stick_deadzone") == std::string::npos);
    CHECK(!CheckProsperoProfile(profile.text, &reason, {.input_tuning = true}));  // an older runtime refuses the mode
    CHECK(CheckProsperoProfile(profile.text, &reason, {.input_tuning = true, .input_playstation = true}));
    // Without the capability the choice is unavailable: the Xbox controller applies, the record keeps the choice.
    const auto rumble_kit = KitCapabilitiesFromManifest({kKitInputRumble}, false);
    CHECK(BuildEffectiveSettings(hl2, nullptr, chosen, rumble_kit, &settings, &error));
    CHECK(Value(settings, "controller") == "xinput");
    CHECK(settings.values.at("controller").reason == "Needs a runtime kit with the PlayStation controller.");
    CHECK(SettingChoiceUnavailable(settings, "controller", "playstation") ==
          "Needs a runtime kit with the PlayStation controller.");
    CHECK(SettingChoiceUnavailable(settings, "controller", "xinput").empty() &&
          SettingChoiceUnavailable(settings, "controller", "keyboard").empty());
    CHECK(settings.values.at("rumble").available);
    CHECK(RenderProsperoProfile(hl2, settings, target, &profile, &reason));
    CHECK(profile.text.find("\nmode = xinput\n") != std::string::npos);
    CHECK(CheckProsperoProfile(profile.text, &reason, {.input_rumble = true}));
    // With controller tuning, the dead zones follow the Xbox controller that applies.
    const auto tuning_only = KitCapabilitiesFromManifest({kKitInputTuning}, false);
    CHECK(BuildEffectiveSettings(hl2, nullptr, chosen, tuning_only, &settings, &error));
    CHECK(Value(settings, "controller") == "xinput" && settings.values.at("stick_deadzone").available);
    // A title on an older runtime keeps the choice for its update; until then it is the Xbox controller.
    const auto stale = KitForTitleRuntime(playstation_kit, false);
    CHECK(!stale.input_playstation && stale.input_playstation_needs_update);
    CHECK(KitForTitleRuntime(playstation_kit, true).input_playstation);
    CHECK(!KitForTitleRuntime(KitCapabilities{}, false).input_playstation_needs_update);
    CHECK(BuildEffectiveSettings(hl2, nullptr, {{"controller", "playstation"}}, stale, &settings, &error));
    const auto& waiting = settings.values.at("controller");
    CHECK(waiting.value == "xinput" && waiting.waits_for_update && waiting.after_update == "playstation");
    CHECK(waiting.reason == kAppliesAfterKitUpdate && ChosenValue(waiting) == "playstation");
    CHECK(SettingChoiceUnavailable(settings, "controller", "playstation").empty());
    CHECK(SettingChoiceWaitsForUpdate(settings, "controller", "playstation"));
    CHECK(!SettingChoiceWaitsForUpdate(settings, "controller", "keyboard"));
    CHECK(RenderProsperoProfile(hl2, settings, target, &profile, &reason));
    CHECK(profile.text.find("\nmode = xinput\n") != std::string::npos && profile.text.find("playstation") == std::string::npos);
    CHECK(CheckProsperoProfile(profile.text, &reason));
    // A controller preset's mode gives way to a player's PlayStation choice.
    CHECK(BuildEffectiveSettings(hl2, nullptr, {{"controller_preset", "gamepad"}, {"controller", "playstation"}},
                                 playstation_kit, &settings, &error));
    CHECK(RenderProsperoProfile(hl2, settings, target, &profile, &reason));
    CHECK(profile.text.find("\npreset = gamepad\nmode = playstation\n") != std::string::npos);
    // And a keyboard preset's: its keys still apply, the DualSense is also the DualShock 4.
    CHECK(BuildEffectiveSettings(hl2, nullptr, {{"controller_preset", "arrows"}, {"controller", "playstation"}},
                                 playstation_kit, &settings, &error));
    CHECK(RenderProsperoProfile(hl2, settings, target, &profile, &reason));
    CHECK(profile.text.find("\npreset = arrows\nmode = playstation\n") != std::string::npos);
    CHECK(CheckProsperoProfile(profile.text, &reason, {.input_playstation = true}));
    // A recipe that locks the controller keeps it over a player's preset, in the sheet and in the profile.
    {
      GameRecipe locked = hl2;
      locked.defaults["controller"] = "playstation";
      locked.locked.insert("controller");
      CHECK(BuildEffectiveSettings(locked, nullptr, {{"controller_preset", "arrows"}}, playstation_kit, &settings, &error));
      CHECK(Value(settings, "controller") == "playstation" && settings.values.at("controller").locked);
      CHECK(settings.values.at("rumble").available);
      CHECK(RenderProsperoProfile(locked, settings, target, &profile, &reason));
      CHECK(profile.text.find("\npreset = arrows\nmode = playstation\n") != std::string::npos);
      // Without the capability the lock still decides: the Xbox controller, not the preset's keyboard mode.
      CHECK(BuildEffectiveSettings(locked, nullptr, {{"controller_preset", "arrows"}}, rumble_kit, &settings, &error));
      CHECK(Value(settings, "controller") == "xinput");
      CHECK(RenderProsperoProfile(locked, settings, target, &profile, &reason));
      CHECK(profile.text.find("\npreset = arrows\nmode = xinput\n") != std::string::npos);
    }
    // Without the capability, or before the title's update, the keyboard preset's own mode applies.
    for (const auto& kit : {rumble_kit, stale}) {
      CHECK(BuildEffectiveSettings(hl2, nullptr, {{"controller_preset", "arrows"}, {"controller", "playstation"}},
                                   kit, &settings, &error));
      CHECK(Value(settings, "controller") == "keyboard");
      CHECK(RenderProsperoProfile(hl2, settings, target, &profile, &reason));
      CHECK(profile.text.find("playstation") == std::string::npos);
      CHECK(profile.text.find("\npreset = arrows\n") != std::string::npos &&
            profile.text.find("mode = xinput") == std::string::npos);
    }
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
    KitCapabilities with_counter;
    with_counter.display_fps_counter = true;
    CHECK(KitForTitleRuntime(with_counter, false).display_fps_counter_needs_update);
    CHECK(!KitForTitleRuntime(with_counter, false).display_fps_counter);
    CHECK(KitForTitleRuntime(with_counter, true).display_fps_counter);
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
  // A refused name says which names are allowed, and every name the hint lists is (#480).
  CHECK(!ValidateSettingValue(hl2, "environment", "PROTON_LOG=1", &reason) &&
        reason == std::string("PROTON_LOG is not an allowed variable. ") + kEnvironmentNamesHint);
  for (const char* prefix : {"DXVK_", "MESA_", "RADV_", "SDL_", "VK_", "ZINK_", "GALLIUM_", "PW_"}) {
    CHECK(std::string_view(kEnvironmentNamesHint).find(std::string(prefix) + ",") != std::string_view::npos ||
          std::string_view(kEnvironmentNamesHint).find(std::string(prefix) + " ") != std::string_view::npos);
    CHECK(ValidateSettingValue(hl2, "environment", std::string(prefix) + "PLAYDEN_TEST=1", &reason));
  }
  CHECK(ValidateSettingValue(hl2, "environment", "VKD3D_CONFIG=dxr;WINE_CPU_TOPOLOGY=4:0,1,2,3", &reason));

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
    CHECK(ParseInstallScript(fnv, "Z:\\data\\games\\22380", "", &values, &reason));
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
    CHECK(ParseInstallScript(utf16, "Z:\\g", "", &values, &reason));
    CHECK(values.machine.empty() && values.user.size() == 2);
    CHECK(values.user[0].key == "Software\\Studio\\Game" && values.user[0].name == "Installed" &&
          values.user[0].data == "dword:00000001");
    CHECK(values.user[1].name == "Ok" && values.user[1].data == "\"yes\"");
    values = {};
    CHECK(!ParseInstallScript("\"InstallScript\" {", "Z:\\g", "", &values, &reason));

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
    auto applied = ApplyInstallScripts(game.string(), "Z:\\data\\games\\22380", "", bottle.string(), 1791500000);
    CHECK(applied.ok && applied.found.empty() && !std::filesystem::exists(bottle / "system.reg.playden-backup"));
    std::ofstream(game / "InstallScript.vdf") << fnv;
    applied = ApplyInstallScripts(game.string(), "Z:\\data\\games\\22380", "", bottle.string(), 1791500000);
    CHECK(applied.ok && applied.found == std::vector<std::string>{"InstallScript.vdf"} && applied.values == 2);
    const auto written = ReadText(bottle / "system.reg");
    CHECK(written.find("\n[SOFTWARE\\\\Bethesda Softworks\\\\FalloutNV] 1791500000\n"
                       "\"Installed Path\"=\"Z:\\\\data\\\\games\\\\22380\\\\\"\n") != std::string::npos);
    CHECK(written.find("\n[Software\\\\Wow6432Node\\\\Bethesda Softworks\\\\FalloutNV] 1791500000\n") != std::string::npos);
    CHECK(ReadText(bottle / "system.reg.playden-backup") == system_reg);
    // Again: already there, so the hive is left as it is.
    applied = ApplyInstallScripts(game.string(), "Z:\\data\\games\\22380", "", bottle.string(), 1791600000);
    CHECK(applied.ok && ReadText(bottle / "system.reg") == written);
    // A later script setting the same value, in another case: its value wins instead of the registry refusing a
    // duplicate. The same holds for HKCU and a key's unnamed value, under either name of the hive.
    std::ofstream(game / "installscript_2.vdf")
        << "\"InstallScript\" { \"Registry\" { \"HKEY_LOCAL_MACHINE\\\\Software\\\\bethesda softworks\\\\FalloutNV\" "
           "{ \"string\" { \"installed path\" \"%INSTALLDIR%\\\\Data\" } } \"HKCU\\\\Software\\\\Studio\" "
           "{ \"string\" { \"(Default)\" \"two\" } } } }";
    std::ofstream(game / "installscript_3.vdf")
        << "\"InstallScript\" { \"Registry\" { \"HKEY_CURRENT_USER\\\\software\\\\studio\" "
           "{ \"string\" { \"(default)\" \"three\" } } } }";
    std::ofstream(bottle / "user.reg") << "WINE REGISTRY Version 2\n;; All keys relative to \\\\User\\\\S-1-5-21-0-0-0-1000\n\n#arch=win64\n";
    ::chmod((bottle / "user.reg").c_str(), 0600);
    applied = ApplyInstallScripts(game.string(), "Z:\\data\\games\\22380", "", bottle.string(), 1791700000);
    CHECK(applied.ok && applied.found.size() == 3 && applied.values == 3);
    const auto user_reg = ReadText(bottle / "user.reg");
    CHECK(user_reg.find("@=\"three\"") != std::string::npos && user_reg.find("\"two\"") == std::string::npos);
    const auto replaced = ReadText(bottle / "system.reg");
    CHECK(replaced.find("\"Installed Path\"=\"Z:\\\\data\\\\games\\\\22380\\\\\"") == std::string::npos);
    CHECK(replaced.find("\"installed path\"=\"Z:\\\\data\\\\games\\\\22380\\\\Data\"") != std::string::npos ||
          replaced.find("\"Installed Path\"=\"Z:\\\\data\\\\games\\\\22380\\\\Data\"") != std::string::npos);
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
    auto needs_playstation = community;
    needs_playstation.requires_prospero_win.push_back("input-playstation");
    const auto playstation = KitCapabilitiesFromManifest({kKitInputPlaystation}, false);
    CHECK(CommunityPresetSupported(needs_playstation, playstation) &&
          CommunityPresetSupported(needs_playstation, KitForTitleRuntime(playstation, false)));
    CHECK(!CommunityPresetSupported(needs_playstation, KitCapabilitiesFromManifest({kKitInputRumble}, false)));
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
    // Steam network acts only on the emulator: with the game's own files it is unavailable and keeps its default.
    CHECK(BuildEffectiveSettings(ffxiii, nullptr,
                                 {{"steam_features", "game"}, {"unlock.steam_features", "on"}, {"steam_network", "offline"}},
                                 KitCapabilities{}, &settings, &error));
    CHECK(!settings.values.at("steam_network").available && settings.values.at("steam_network").value == "online");
    CHECK(BuildEffectiveSettings(ffxiii, nullptr, {}, KitCapabilities{}, &settings, &error));
    CHECK(settings.values.at("steam_network").available && settings.values.at("steam_network").value == "online");
    CHECK(BuildEffectiveSettings(ffxiii, nullptr, {{"steam_network", "offline"}}, KitCapabilities{}, &settings, &error));
    CHECK(settings.values.at("steam_network").value == "offline");
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
  // Clair Obscur: Expedition 33 hashes the Steam library at its project path and loads the emulator from the Engine
  // path, so it needs both, and quits on the game's own files without a Steam client.
  {
    GameRecipe expedition;
    CHECK(ParseGameRecipe(ReadText(source / "runtime/recipes/steam-1903340-expedition33-v1.json"), &expedition, &reason));
    CHECK(expedition.app_id == 1903340 && expedition.launch_entries.size() == 1 &&
          expedition.launch_entries[0].executable == "Sandfall/Binaries/Win64/SandFall-Win64-Shipping.exe");
    CHECK(expedition.steam_api_copies == std::vector<std::string>{"Sandfall/Binaries/Win64/steam_api64.dll"});
    KitCapabilities kit;
    kit.vkd3d_proton = true;
    EffectiveSettings settings;
    SettingsError error;
    CHECK(BuildEffectiveSettings(expedition, nullptr, {}, kit, &settings, &error));
    CHECK(settings.graphics == "dxvk" && settings.arguments == "-dx12" && settings.vkd3d_proton);
    CHECK(settings.values.at("steam_features").value == "emulate" && settings.values.at("steam_features").locked);
    // The field survives a round trip, and only names a steam_api DLL inside the game's folders.
    GameRecipe again;
    CHECK(ParseGameRecipe(SerializeGameRecipe(expedition), &again, &reason) && again.steam_api_copies == expedition.steam_api_copies);
    const auto with_copies = [](const std::string& copies, const std::string& id = "steam-1903340-test-v1") {
      return R"({"recipe_id":")" + id + R"(","recipe_version":1,"app_id":1903340,"launch_entries":[{"id":"game","label":"G","executable":"g.exe"}],)"
             R"("architecture":"pe64","runtime":"wine-wow64","prefix":"game","graphics":"dxvk","steam_api_copies":)" + copies + "}";
    };
    GameRecipe parsed;
    CHECK(ParseGameRecipe(with_copies(R"(["Game/Binaries/STEAM_API64.dll"])"), &parsed, &reason));
    for (const auto* bad : {R"("Game/steam_api64.dll")", R"(["steam_api64.dll"])", R"(["Game/other.dll"])",
                            R"(["../steam_api64.dll"])", R"(["Game/../steam_api64.dll"])", R"(["/Game/steam_api64.dll"])",
                            R"(["Game\steam_api64.dll"])", R"([1])", R"(["A/steam_api.dll","a/STEAM_API.dll"])",
                            R"(["A/steam_api.dll","B/steam_api.dll","C/steam_api.dll","D/steam_api.dll","E/steam_api.dll"])"})
      CHECK(!ParseGameRecipe(with_copies(bad), &parsed, &reason));
    // A community recipe can't place a file in the game folder.
    CHECK(!ParseGameRecipe(with_copies(R"(["Game/steam_api64.dll"])", "community-steam-1903340-test"), &parsed, &reason));
    CommunityRecipe community;
    auto shared = with_copies(R"(["Game/steam_api64.dll"])");
    shared.replace(shared.size() - 1, 1, R"(,"executable":"g.exe","community":{"format_version":1,"status":"works"}})");
    CHECK(!ParseCommunityRecipe(shared, &community, &reason) && reason.find("steam_api_copies") != std::string::npos);
  }
  // Project Zomboid watches each of its folders, which the runtime kit now does without a file each. 42.22 signs the
  // game's classes, so the watcher that watched nothing, which Playden placed before, stops the game: it goes.
  {
    GameRecipe zomboid;
    CHECK(ParseGameRecipe(ReadText(source / "runtime/recipes/steam-108600-projectzomboid-v1.json"), &zomboid, &reason));
    CHECK(zomboid.game_files.empty());
    const std::vector<std::pair<std::string, std::string>> stub{
        {"zombie/DebugFileWatcher.class", "598b38e10d4cdbda29743824ad7d936a6d3ea1381268c859bbfd7f3b835c5d44"}};
    CHECK(zomboid.retired_game_files == stub);
    // The field survives a round trip.
    GameRecipe again;
    CHECK(ParseGameRecipe(SerializeGameRecipe(zomboid), &again, &reason) && again.retired_game_files == stub);
  }
  // A shipped recipe can place files in the game folder, and retire those an earlier version placed.
  {
    const auto with_files = [](const std::string& paths, const std::string& id = "steam-108600-test-v1") {
      return R"({"recipe_id":")" + id + R"(","recipe_version":1,"app_id":108600,"launch_entries":[{"id":"game","label":"G","executable":"g.exe"}],)"
             R"("architecture":"pe64","runtime":"wine-wow64","prefix":"game","graphics":"opengl","game_files":)" + paths + "}";
    };
    GameRecipe parsed;
    CHECK(ParseGameRecipe(with_files(R"(["a.class","b/c.class"])"), &parsed, &reason));
    for (const auto* bad : {R"("a.class")", R"(["../a.class"])", R"(["b/../a.class"])", R"(["/a.class"])", R"(["b\\a.class"])",
                            R"([""])", R"([1])", R"(["a.class","A.CLASS"])", R"(["a","b","c","d","e"])"})
      CHECK(!ParseGameRecipe(with_files(bad), &parsed, &reason));
    // A community recipe can't place a file in the game folder.
    CHECK(!ParseGameRecipe(with_files(R"(["a.class"])", "community-steam-108600-test"), &parsed, &reason));
    CommunityRecipe community;
    auto shared = with_files(R"(["a.class"])");
    shared.replace(shared.size() - 1, 1, R"(,"executable":"g.exe","community":{"format_version":1,"status":"works"}})");
    CHECK(!ParseCommunityRecipe(shared, &community, &reason) && reason.find("game_files") != std::string::npos);

    const std::string hash(64, 'a');
    const auto with_retired = [&](const std::string& retired, const std::string& id = "steam-108600-test-v1",
                                  const std::string& placed = "[]") {
      return R"({"recipe_id":")" + id + R"(","recipe_version":1,"app_id":108600,"launch_entries":[{"id":"game","label":"G","executable":"g.exe"}],)"
             R"("architecture":"pe64","runtime":"wine-wow64","prefix":"game","graphics":"opengl","game_files":)" + placed +
             R"(,"retired_game_files":)" + retired + "}";
    };
    CHECK(ParseGameRecipe(with_retired(R"({"a.class":")" + hash + R"(","b/c.class":")" + hash + R"("})"), &parsed, &reason));
    CHECK(parsed.retired_game_files.size() == 2);
    for (const std::string bad : std::vector<std::string>{
             R"(["a.class"])", R"({"../a.class":")" + hash + R"("})", R"({"/a.class":")" + hash + R"("})",
          R"({"a.class":")" + std::string(63, 'a') + R"("})", R"({"a.class":")" + std::string(64, 'A') + R"("})",
          R"({"a.class":1})", R"({"a.class":")" + hash + R"(","A.CLASS":")" + hash + R"("})",
          R"({"a":")" + hash + R"(","b":")" + hash + R"(","c":")" + hash + R"(","d":")" + hash + R"(","e":")" + hash + R"("})"})
      CHECK(!ParseGameRecipe(with_retired(bad), &parsed, &reason));
    // A path is placed or retired, not both.
    CHECK(!ParseGameRecipe(with_retired(R"({"a.class":")" + hash + R"("})", "steam-108600-test-v1", R"(["A.class"])"),
                           &parsed, &reason));
    // Only Playden's own recipes remove a file from the game folder.
    CHECK(!ParseGameRecipe(with_retired(R"({"a.class":")" + hash + R"("})", "community-steam-108600-test"), &parsed, &reason));
    auto shared_retired = with_retired(R"({"a.class":")" + hash + R"("})");
    shared_retired.replace(shared_retired.size() - 1, 1,
                           R"(,"executable":"g.exe","community":{"format_version":1,"status":"works"}})");
    CHECK(!ParseCommunityRecipe(shared_retired, &community, &reason) &&
          reason.find("retired_game_files") != std::string::npos);
  }
  // Batman: Arkham City is Asylum's sibling: BatmanAC.exe starts BmLauncher.exe and exits too while UserEngine.ini has
  // no Language.
  {
    GameRecipe city;
    CHECK(ParseGameRecipe(ReadText(source / "runtime/recipes/steam-200260-arkhamcity-v1.json"), &city, &reason));
    CHECK(city.app_id == 200260 && city.launch_entries.size() == 1 &&
          city.launch_entries[0].executable == "Binaries/Win32/BatmanAC.exe");
    CHECK(city.ini == (std::vector<IniValue>{{"BmGame/Config/UserEngine.ini", "Engine.Engine", "Language", "int"}}));
    EffectiveSettings settings;
    SettingsError error;
    CHECK(BuildEffectiveSettings(city, nullptr, {}, KitCapabilities{}, &settings, &error));
    CHECK(settings.graphics == "dxvk" && settings.dll_overrides == kDxvkOverrides && settings.architecture == "pe32");
    CHECK(settings.values.at("steam_features").value == "emulate" && !settings.values.at("graphics").locked);
    CHECK(settings.launch && settings.launch->executable == "Binaries/Win32/BatmanAC.exe" && settings.arguments.empty());
  }
  // Project Zomboid: its launcher picks ZGC for Windows 10, which can't reserve its heap on the console, and G1 for
  // Windows 7; LWJGL draws with OpenGL, loaded at run time, so nothing in the files names it; and the launcher quits
  // when the Steam emulator says nobody is signed in, unless the game runs with -nosteam.
  {
    GameRecipe zomboid;
    CHECK(ParseGameRecipe(ReadText(source / "runtime/recipes/steam-108600-projectzomboid-v1.json"), &zomboid, &reason));
    CHECK(zomboid.app_id == 108600 && zomboid.launch_entries.size() == 1 &&
          zomboid.launch_entries[0].executable == "ProjectZomboid64.exe");
    KitCapabilities kit;
    kit.registry = true;
    EffectiveSettings settings;
    SettingsError error;
    CHECK(BuildEffectiveSettings(zomboid, nullptr, {}, kit, &settings, &error));
    CHECK(settings.graphics == "opengl" && settings.architecture == "pe64" && settings.arguments == "-nosteam");
    CHECK(settings.values.at("windows_version").value == "win7" && !settings.values.at("windows_version").locked);
  }
  CHECK(!ParseSettingsPresets(R"({"presets":[{"id":"x","name":"X","settings":{"graphics":"gdi"}}]})", &presets, &reason));
  CHECK(!ParseSettingsPresets(R"({"presets":[{"id":"x","name":"X","settings":{"extra_arguments":"-a"}}]})", &presets, &reason));
  // The "my-" prefix is the player's own presets'; a built-in one may not take it.
  {
    std::vector<SettingsPreset> mine;
    CHECK(!ParseSettingsPresets(R"({"presets":[{"id":"my-3f9a0c1d2e4b5a69","name":"Mine","settings":{}}]})", &mine, &reason));
    CHECK(reason.find("my-") != std::string::npos);
    CHECK(ParseSettingsPresets(R"({"presets":[{"id":"mouse-my-way","name":"Mine","settings":{}}]})", &mine, &reason));
  }
  // The rule every preset's settings follow, built-in or the player's.
  CHECK(ValidatePresetSetting("controller_preset", "mouse", &reason));
  CHECK(ValidatePresetSetting("resolution", "1280x720", &reason));
  CHECK(!ValidatePresetSetting("resolution", "huge", &reason));
  for (const char* refused : {"launch_option", "graphics", "architecture", "start_check", "extra_arguments", "program",
                              "no_such_setting", "opengl_thread"})
    CHECK(!ValidatePresetSetting(refused, "x", &reason));
  CHECK(RetiredSetting("opengl_thread") && !RetiredSetting("refresh"));

  // A player preset made from a game keeps what the player chose for it, by where each value came from.
  {
    GameRecipe own;
    own.id = "steam-4000-own-v1";
    own.app_id = 4000;
    own.launch_entries = {{"game", "Game", "game.exe", "", ""}};
    own.defaults = {{"resolution", "1280x720"}, {"performance_overlay", "on"}, {"window_view", "desktop"},
                    {"steam_features", "emulate"}};
    own.locked = {"vsync", "trigger_deadzone"};
    SettingsPreset preset{"keyboard-on-controller", "Keyboard", "", "", {{"controller_preset", "gamepad"}, {"refresh", "60"}}};
    const std::map<std::string, std::string> overrides = {
        {"rumble", "50"},                    // a player change
        {"performance_overlay", "off"},      // equal to Playden's default, over the recipe's value
        {"button.cross", "space"},           // a button binding: free text a preset may hold
        {"extra_arguments", "-windowed"},    // free text a preset may not hold
        {"graphics", "dxvk"},                // never in a preset
        {"trigger_deadzone", "10"},          // a locked row, changed anyway
        {"unlock.trigger_deadzone", "on"},
        {"vsync", "off"},                    // a locked row: the player's value doesn't apply
        {"volume", "50"},                    // unavailable: the kit has no volume
    };
    const auto tuning = KitCapabilitiesFromManifest({kKitInputRumble, kKitInputTuning, kKitDisplay120Hz}, false);
    EffectiveSettings settings;
    SettingsError error;
    CHECK(BuildEffectiveSettings(own, &preset, overrides, tuning, &settings, &error));
    // What the rows are, so each case below checks what it says.
    CHECK(settings.values.at("resolution").source == ValueSource::kRecipe);
    CHECK(settings.values.at("refresh").source == ValueSource::kPreset && Value(settings, "refresh") == "60" &&
          settings.values.at("refresh").available);
    CHECK(settings.values.at("rumble").source == ValueSource::kUser && settings.values.at("rumble").available);
    CHECK(settings.values.at("trigger_deadzone").locked && settings.values.at("trigger_deadzone").unlocked &&
          settings.values.at("trigger_deadzone").available && Value(settings, "trigger_deadzone") == "10");
    CHECK(settings.values.at("vsync").locked && settings.values.at("vsync").source == ValueSource::kPlayden);
    CHECK(settings.values.at("volume").source == ValueSource::kUser && !settings.values.at("volume").available &&
          !settings.values.at("volume").waits_for_update);
    CHECK(settings.values.at("graphics").source == ValueSource::kUser);
    CHECK(settings.values.at("controller").source == ValueSource::kPreset);  // brought by the controller preset

    auto kept = PresetSettingsFromGame(own, settings, PresetCapture::kSave);
    const std::map<std::string, std::string> chosen = {
        {"controller_preset", "gamepad"}, {"refresh", "60"}, {"controller", Value(settings, "controller")},
        {"rumble", "50"}, {"performance_overlay", "off"}, {"button.cross", "space"}};
    for (const auto& [id, value] : chosen) CHECK(kept.count(id) && kept.at(id) == value);
    // Everything else is left out: the recipe's values, Playden's, locked and unavailable rows, what a preset can't set.
    for (const char* left : {"resolution", "window_view", "steam_features", "extra_arguments", "graphics", "trigger_deadzone",
                             "unlock.trigger_deadzone", "vsync", "volume", "launch_option", "architecture"})
      CHECK(!kept.count(left));
    for (const auto& [id, value] : kept) {
      CHECK(ValidatePresetSetting(id, value, &reason));
      const auto source = settings.values.at(id).source;
      CHECK(source == ValueSource::kUser || source == ValueSource::kPreset);
    }
    // Own changes only: the preset's values stay with the preset.
    kept = PresetSettingsFromGame(own, settings, PresetCapture::kOwnChanges);
    CHECK(kept == (std::map<std::string, std::string>{
                      {"rumble", "50"}, {"performance_overlay", "off"}, {"button.cross", "space"}}));

    // Changing the controller layout also changes inputs inherited from the old settings preset.
    // Update must move those effective inputs, or the old preset's explicit rows win again.
    SettingsPreset old_inputs{"old-inputs", "Old inputs", "", "",
                              {{"controller", "xinput"}, {"pointer_stick", "right_stick"}, {"pointer_speed", "1800"}}};
    CHECK(BuildEffectiveSettings(own, &old_inputs, {{"controller_preset", "mouse"}}, tuning, &settings, &error));
    CHECK(PresetSettingsFromGame(own, settings, PresetCapture::kOwnChanges) ==
          (std::map<std::string, std::string>{{"controller_preset", "mouse"}, {"controller", "keyboard"},
                                            {"pointer_stick", "left_stick"}, {"pointer_speed", "900"}}));

    // A community recipe as the base: its values are someone's choice for the game, and come along. Its locks don't.
    GameRecipe community = own;
    community.id = "community-steam-4000-keyboard-play";
    CHECK(IsCommunityRecipeId(community.id));
    CHECK(BuildEffectiveSettings(community, nullptr, {{"rumble", "50"}}, tuning, &settings, &error));
    CHECK(settings.values.at("resolution").source == ValueSource::kRecipe);
    kept = PresetSettingsFromGame(community, settings, PresetCapture::kSave);
    CHECK(kept == (std::map<std::string, std::string>{{"resolution", "1280x720"}, {"performance_overlay", "on"},
                                                      {"window_view", "desktop"}, {"steam_features", "emulate"},
                                                      {"rumble", "50"}}));
    CHECK(PresetSettingsFromGame(community, settings, PresetCapture::kOwnChanges) ==
          (std::map<std::string, std::string>{{"rumble", "50"}}));
    // The game's own recipe alone: nothing chosen.
    CHECK(BuildEffectiveSettings(own, nullptr, {}, tuning, &settings, &error));
    CHECK(PresetSettingsFromGame(own, settings, PresetCapture::kSave).empty());

    // A choice that waits for the kit update is kept as chosen, not as the fallback that applies until then.
    const auto stale = KitForTitleRuntime(KitCapabilitiesFromManifest({kKitInputPlaystation}, false), false);
    CHECK(BuildEffectiveSettings(own, nullptr, {{"controller", "playstation"}}, stale, &settings, &error));
    CHECK(Value(settings, "controller") == "xinput" && settings.values.at("controller").waits_for_update);
    CHECK(PresetSettingsFromGame(own, settings, PresetCapture::kSave) ==
          (std::map<std::string, std::string>{{"controller", "playstation"}}));
    // So is a row that doesn't apply until then.
    const auto stale_rumble = KitForTitleRuntime(KitCapabilitiesFromManifest({kKitInputRumble}, false), false);
    CHECK(BuildEffectiveSettings(own, nullptr, {{"rumble", "50"}}, stale_rumble, &settings, &error));
    CHECK(!settings.values.at("rumble").available && settings.values.at("rumble").waits_for_update);
    CHECK(PresetSettingsFromGame(own, settings, PresetCapture::kSave) ==
          (std::map<std::string, std::string>{{"rumble", "50"}}));
  }

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
  CHECK(loaded_presets.size() == 4 && loaded_recipes.size() == 8 && loaded_recipes.front().app_id == 220 &&
        loaded_recipes.back().id == "steam-1903340-expedition33-v1");
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
    // No graphics library named: DXVK (game_probe.cpp, DefaultsFor).
    CHECK(generated.graphics == "dxvk" && generated.dll_overrides.empty() && generated.locked.empty());
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

  // Steam names a working folder as Windows sees it: Unreal Tournament (13240) and UT2004 (13230) start
  // "system\UnrealTournament.exe" in "system", which is System/ on disk (#844). The recipe, the profile and the
  // folder dxvk.conf goes in use the disk's spelling, as the console's disk tells the two apart.
  {
    const auto game_root = std::filesystem::temp_directory_path() / ("playden-workdir-case-" + std::to_string(::getpid()));
    std::filesystem::remove_all(game_root);
    WritePe(game_root / "System" / "UnrealTournament.exe", false);
    std::filesystem::create_directories(game_root / "Maps");
    GameRecipe generated;
    CHECK(GenerateGameRecipe(13240, "Unreal Tournament", {{"", "", "system\\UnrealTournament.exe", "system", ""}},
                             game_root.string(), &generated, &reason));
    CHECK(generated.launch_entries.size() == 1);
    CHECK(generated.launch_entries[0].executable == "System/UnrealTournament.exe");
    CHECK(generated.launch_entries[0].working_directory == "System");
    CHECK(LaunchWorkingFolder(generated.launch_entries[0]) == std::string("System"));
    EffectiveSettings case_settings;
    SettingsError case_error;
    CHECK(BuildEffectiveSettings(generated, nullptr, {}, KitCapabilities{}, &case_settings, &case_error));
    CHECK(case_settings.launch && case_settings.launch->working_directory == "System");
    ProsperoProfile case_profile;
    CHECK(RenderProsperoProfile(generated, case_settings, {"playden-steam-13240", "/data/playden-games/13240"},
                                &case_profile, &reason));
    CHECK(case_profile.text.find("\nworking_directory = Z:\\data\\playden-games\\13240\\System\n") != std::string::npos);
    // A folder Steam names that isn't there stays as Steam names it.
    CHECK(GenerateGameRecipe(13240, "Unreal Tournament", {{"", "", "system\\UnrealTournament.exe", "missing", ""}},
                             game_root.string(), &generated, &reason));
    CHECK(generated.launch_entries[0].working_directory == "missing");
    // A recipe from elsewhere (a community recipe exported from an older setup) is matched to the disk when bound.
    GameRecipe shared = generated;
    shared.launch_entries[0].working_directory = "system";
    shared.launch_entries.push_back({"maps", "Maps", "System/UnrealTournament.exe", "MAPS", ""});
    shared.launch_entries.push_back({"root", "Root", "System/UnrealTournament.exe", "", ""});
    shared.launch_entries.push_back({"gone", "Gone", "System/UnrealTournament.exe", "Gone", ""});
    CHECK(MatchWorkingFoldersToDisk(game_root.string(), &shared));
    CHECK(shared.launch_entries[0].working_directory == "System" && shared.launch_entries[1].working_directory == "Maps");
    CHECK(shared.launch_entries[2].working_directory.empty() && shared.launch_entries[3].working_directory == "Gone");
    CHECK(!MatchWorkingFoldersToDisk(game_root.string(), &shared));
    // A file is not a folder.
    shared.launch_entries[0].working_directory = "system/unrealtournament.exe";
    CHECK(!MatchWorkingFoldersToDisk(game_root.string(), &shared));
    // Every folder in a nested path takes the disk's spelling; a link on the way is not followed.
    std::filesystem::create_directories(game_root / "Maps" / "Custom");
    shared.launch_entries[0].working_directory = "maps\\CUSTOM";
    CHECK(MatchWorkingFoldersToDisk(game_root.string(), &shared) && shared.launch_entries[0].working_directory == "Maps/Custom");
    std::filesystem::create_directory_symlink(game_root / "System", game_root / "Linked");
    shared.launch_entries[0].working_directory = "linked";
    CHECK(!MatchWorkingFoldersToDisk(game_root.string(), &shared) && shared.launch_entries[0].working_directory == "linked");
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
    const auto kHalfLife2 = playden::GameRef::Steam(220);
    CHECK(BuildEffectiveSettings(hl2, nullptr, {{"resolution", "1280x720"}, {"program", "hl2_win64.exe"},
                                 {"architecture", "pe64"}, {"unlock.graphics", "on"}, {"graphics", "opengl"},
                                 {"arguments", "-game hl2 {resolution}"}, {"dll_overrides", "d3d9=n;xinput1_3=n"}},
                                 KitCapabilities{}, &settings, &error));
    auto community = BuildCommunityRecipe(hl2, settings, kHalfLife2, "Half-Life 2");
    {  // Online is shared, the sign-in is not; Offline emulation and its network value are shared too.
      EffectiveSettings steam;
      const auto host_kit = KitCapabilitiesFromManifest({kKitHelperProcess}, false);
      CHECK(BuildEffectiveSettings(hl2, nullptr, {{"unlock.steam_features", "on"}, {"steam_features", "online"},
                                   {"steam_sign_in", "kept"}}, host_kit, &steam, &error));
      CHECK(steam.values.at("steam_features").value == "online" && steam.values.at("steam_sign_in").value == "kept");
      const auto online = BuildCommunityRecipe(hl2, steam, kHalfLife2, "Half-Life 2").recipe;
      CHECK(online.defaults.at("steam_features") == "online" && !online.defaults.count("steam_sign_in"));
      // Nor what the game does with the player's invites.
      CHECK(BuildEffectiveSettings(hl2, nullptr, {{"unlock.steam_features", "on"}, {"steam_features", "online"},
                                   {"game_invites", "off"}}, host_kit, &steam, &error));
      CHECK(steam.values.at("game_invites").value == "off" && steam.values.at("game_invites").source == ValueSource::kUser);
      CHECK(!BuildCommunityRecipe(hl2, steam, kHalfLife2, "Half-Life 2").recipe.defaults.count("game_invites"));
      CHECK(BuildEffectiveSettings(hl2, nullptr, {{"unlock.steam_features", "on"}, {"steam_features", "emulate"},
                                   {"steam_network", "offline"}}, KitCapabilities{}, &steam, &error));
      const auto offline = BuildCommunityRecipe(hl2, steam, kHalfLife2, "Half-Life 2").recipe;
      CHECK(offline.defaults.at("steam_features") == "emulate" && offline.defaults.at("steam_network") == "offline");
    }
    {  // The performance overlay is the player's own: a recipe fixes the game.
      EffectiveSettings overlay;
      CHECK(BuildEffectiveSettings(hl2, nullptr, {{"performance_overlay", "on"}, {"overlay_detail", "full"}},
                                   KitCapabilitiesFromManifest({kKitDxvkConfig}, false), &overlay, &error));
      CHECK(overlay.values.at("performance_overlay").value == "on" && overlay.values.at("overlay_detail").available);
      const auto shared = BuildCommunityRecipe(hl2, overlay, kHalfLife2, "Half-Life 2").recipe;
      CHECK(!shared.defaults.count("performance_overlay") && !shared.defaults.count("overlay_detail"));
    }
    {  // What the files showed travels without the graphics value, which has its own field (#67).
      auto detected = hl2;
      detected.detected = "d3d9 graphics=dxvk";
      CHECK(BuildCommunityRecipe(detected, settings, kHalfLife2, "Half-Life 2").recipe.detected == "d3d9");
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
    // The game object stands for app_id: the recipe reads back unbound, for the game it is applied to.
    CHECK(as_recipe.app_id == 0 && as_recipe.game == playden::GameId{});
    CHECK(as_recipe.id == "community-steam-220" && as_recipe.launch_entries.size() == 1 &&
          as_recipe.launch_entries[0].executable == "hl2_win64.exe" && as_recipe.architecture == "pe64" &&
          as_recipe.graphics == "opengl" && as_recipe.arguments == "-game hl2 {resolution}" &&
          as_recipe.resolution_arguments == hl2.resolution_arguments && as_recipe.dll_overrides == "d3d9=n;xinput1_3=n");
    CHECK(as_recipe.defaults.at("resolution") == "1280x720" && as_recipe.defaults.at("button.options") == "escape");
    CHECK(!as_recipe.defaults.count("controller") && as_recipe.locked.empty());  // xinput is Playden's default too
    CommunityRecipe parsed;
    CHECK(ParseCommunityRecipe(text, &parsed, &reason));
    CHECK(parsed.game == kHalfLife2 && parsed.game_name == "Half-Life 2" && parsed.made_on.empty());
    CHECK(text.find("\"game\": {\"store\": \"steam\", \"key\": \"220\", \"name\": \"Half-Life 2\"}") != std::string::npos &&
          text.find("app_id") == std::string::npos && text.find("steam_appid") == std::string::npos &&
          text.find("made_on") == std::string::npos);
    CHECK(parsed.executable == "hl2_win64.exe" && parsed.status == "works" && parsed.notes == "Starts \"fine\"." &&
          parsed.base_path == "profiles/hl2.profile" && SerializeCommunityRecipe(parsed) == text);
    // The same game started from the exported recipe gets the same command.
    EffectiveSettings again;
    CHECK(BuildEffectiveSettings(as_recipe, nullptr, {}, KitCapabilities{}, &again, &error));
    CHECK(again.arguments == "-game hl2 -w 1280 -h 720" && again.dll_overrides == settings.dll_overrides);
    {  // The player's Extra arguments stay theirs: the export has none, so the community rules don't refuse it.
      EffectiveSettings extra;
      CHECK(BuildEffectiveSettings(hl2, nullptr, {{"extra_arguments", "-console"}}, KitCapabilities{}, &extra, &error));
      CHECK(extra.arguments.find("-console") != std::string::npos);
      auto exported_extra = BuildCommunityRecipe(hl2, extra, kHalfLife2, "Half-Life 2");
      CHECK(!exported_extra.recipe.defaults.count("extra_arguments") &&
            SerializeCommunityRecipe(exported_extra).find("-console") == std::string::npos);
      exported_extra.recipe.id = CommunityRecipeIdFor(kHalfLife2, "console");
      for (const auto& refusal : CommunityRecipeRefusals(exported_extra)) CHECK(refusal.find("extra_arguments") == std::string::npos);
    }
    // A recipe exported from a generated one keeps the Steam emulator, unlocked.
    auto from_generated = generated;
    from_generated.defaults["steam_features"] = "emulate";
    CHECK(BuildEffectiveSettings(from_generated, nullptr, {}, KitCapabilities{}, &settings, &error));
    GameRecipe exported;
    CHECK(ParseGameRecipe(SerializeCommunityRecipe(BuildCommunityRecipe(from_generated, settings, kHalfLife2, "Half-Life 2")), &exported, &reason));
    CHECK(exported.defaults.at("steam_features") == "emulate" && IsCommunityRecipeId(exported.id));
    CHECK(BuildEffectiveSettings(exported, nullptr, {}, KitCapabilities{}, &again, &error));
    CHECK(again.values.at("steam_features").value == "emulate" && !again.values.at("steam_features").locked);
    CHECK(again.dll_overrides == kDxvkOverrides);  // the DXVK overrides are written out, not added again
    {  // A row the player didn't change stays out of a share, so an older Playden, which refuses a recipe naming a
       // setting it doesn't know, still reads it. A new row must not change that (Steam Input came in 0.11).
      // Rows the recipe sets go with it, and so do the rows its controller preset brings (runtime/input/*.input).
      const auto only_recipe_rows = [&](const GameRecipe& recipe) {
        EffectiveSettings untouched;
        CHECK(BuildEffectiveSettings(recipe, nullptr, {}, KitCapabilities{}, &untouched, &error));
        auto allowed = recipe.defaults;
        if (const auto preset = recipe.defaults.find("controller_preset"); preset != recipe.defaults.end())
          for (const auto& [id, value] : ControllerPresetInputs().at(preset->second)) allowed.emplace(id, value);
        for (const auto& [id, value] : BuildCommunityRecipe(recipe, untouched, kHalfLife2, "Half-Life 2").recipe.defaults) {
          const auto own = allowed.find(id);
          if (own == allowed.end() || own->second != value)
            std::fprintf(stderr, "untouched row %s=%s is shared\n", id.c_str(), value.c_str());
          CHECK(own != allowed.end() && own->second == value);
        }
      };
      only_recipe_rows(from_generated);
      auto with_preset = from_generated;
      with_preset.defaults["controller_preset"] = "mouse";
      only_recipe_rows(with_preset);
      EffectiveSettings chosen;
      CHECK(BuildEffectiveSettings(from_generated, nullptr, {{"steam_input", "auto"}}, KitCapabilities{}, &chosen, &error));
      CHECK(!BuildCommunityRecipe(from_generated, chosen, kHalfLife2, "Half-Life 2").recipe.defaults.count("steam_input"));
      CHECK(BuildEffectiveSettings(from_generated, nullptr, {{"steam_input", "off"}}, KitCapabilities{}, &chosen, &error));
      CHECK(BuildCommunityRecipe(from_generated, chosen, kHalfLife2, "Half-Life 2").recipe.defaults.at("steam_input") == "off");
      // Touchpad hold came later still: off stays out of a share, a player's choice goes with it.
      const auto touchpad_kit = KitCapabilitiesFromManifest({kKitTouchpadSwitch}, false);
      CHECK(BuildEffectiveSettings(from_generated, nullptr, {}, touchpad_kit, &chosen, &error));
      CHECK(!BuildCommunityRecipe(from_generated, chosen, kHalfLife2, "Half-Life 2").recipe.defaults.count("touchpad_hold"));
      CHECK(BuildEffectiveSettings(from_generated, nullptr, {{"touchpad_hold", "mouse"}}, touchpad_kit, &chosen, &error));
      CHECK(BuildCommunityRecipe(from_generated, chosen, kHalfLife2, "Half-Life 2").recipe.defaults.at("touchpad_hold") == "mouse");
    }
    // Registry values and requirements stay with the game when it is exported.
    auto with_extras = from_generated;
    with_extras.registry = {{"Software\\Wine\\AppDefaults\\a.exe\\Direct3D", "VideoMemorySize", "\"4096\""},
                            {"Software\\Studio\\Game", "Language", "\"en\""}};
    with_extras.requirements = {{"audio", "ps5"}};
    with_extras.ini = {{"BmGame/Config/UserEngine.ini", "Engine.Engine", "Language", "int"}};
    CHECK(ParseGameRecipe(SerializeCommunityRecipe(BuildCommunityRecipe(with_extras, settings, kHalfLife2, "Half-Life 2")), &exported, &reason));
    CHECK(exported.requirements == with_extras.requirements && exported.registry.size() == 2);
    CHECK(exported.ini == with_extras.ini);
    for (const auto& value : with_extras.registry)
      CHECK(std::any_of(exported.registry.begin(), exported.registry.end(), [&](const RegistryValue& read) {
        return read.key == value.key && read.name == value.name && read.data == value.data;
      }));
    auto broken = text;
    broken.replace(broken.find("\"works\""), 7, "\"great\"");
    CHECK(!ParseCommunityRecipe(broken, &parsed, &reason) && reason.find("status") != std::string::npos);

    // The export leaves the slug, title and contributors for the author: community-<app id>, which reads back
    // here but the community rules refuse.
    const auto exported_text = SerializeCommunityRecipe(BuildCommunityRecipe(hl2, settings, kHalfLife2, "Half-Life 2"));
    CHECK(exported_text.find("\"recipe_id\": \"community-steam-220\"") != std::string::npos &&
          exported_text.find("\"title\": \"\"") != std::string::npos &&
          exported_text.find("\"tested_kit\": \"\"") != std::string::npos &&
          exported_text.find("\"contributors\": []") != std::string::npos &&
          exported_text.find("min_kit") == std::string::npos);
    CHECK(ParseCommunityRecipe(exported_text, &parsed, &reason) && parsed.title.empty() && parsed.contributors.empty());
    // Named by the author: the id with its slug, the title, the kit it was tested on and who helped.
    auto named = community;
    named.recipe.id = CommunityRecipeIdFor(kHalfLife2, "opengl-1280");
    named.title = "OpenGL at 1280×720";
    named.tested_kit = "0.12-dev.20261022.1";
    named.min_kit = "0.12";
    named.author = "Ana";
    named.contributors = {"Bo", "Cy \"the tester\""};
    const auto named_text = SerializeCommunityRecipe(named);
    CHECK(ParseCommunityRecipe(named_text, &parsed, &reason));
    CHECK(parsed.recipe.id == "community-steam-220-opengl-1280" && parsed.title == "OpenGL at 1280×720" &&
          parsed.tested_kit == "0.12-dev.20261022.1" && parsed.min_kit == "0.12" && parsed.author == "Ana" &&
          parsed.contributors == named.contributors && SerializeCommunityRecipe(parsed) == named_text);
    // A recipe_id for another game, or with a slug the rules refuse, does not read.
    for (const char* id : {"community-steam-221-opengl-1280", "community-steam-220-OpenGL", "community-steam-220-",
                           "community-imported-220-opengl-1280", "community-220-opengl-1280", "steam-220-auto",
                           "community-steam-22-0-opengl-1280"}) {
      auto other = named_text;
      other.replace(other.find("community-steam-220-opengl-1280"), std::strlen("community-steam-220-opengl-1280"), id);
      CHECK(!ParseCommunityRecipe(other, &parsed, &reason) && reason.find("recipe_id") != std::string::npos);
    }
    auto listed = named_text;
    listed.replace(listed.find("[\"Bo\""), 5, "[7");
    CHECK(!ParseCommunityRecipe(listed, &parsed, &reason) && reason.find("contributors") != std::string::npos);

    // An imported copy shares under its Steam app ID and says it was made on an imported copy; without one, under its
    // name key. made_on is left out when it is the game's own store.
    auto imported = SerializeCommunityRecipe(BuildCommunityRecipe(hl2, settings, kHalfLife2, "Half-Life 2", "imported"));
    CHECK(imported.find("\"made_on\": \"imported\"") != std::string::npos);
    CHECK(ParseCommunityRecipe(imported, &parsed, &reason) && parsed.made_on == "imported" && parsed.game == kHalfLife2);
    CHECK(BuildCommunityRecipe(hl2, settings, kHalfLife2, "Half-Life 2", "steam").made_on.empty());
    const playden::GameRef night_sky{"imported", "night-sky"};
    const auto own = BuildCommunityRecipe(hl2, settings, night_sky, "Night Sky", "imported");
    CHECK(own.recipe.id == "community-imported-night-sky" && own.made_on.empty());
    CHECK(ParseCommunityRecipe(SerializeCommunityRecipe(own), &parsed, &reason) && parsed.game == night_sky);
    // Bound to the installed game it is applied to: then it is that game's recipe.
    RuntimeInstall installed;
    installed.runtime_number = 0xF0000001u;
    installed.game = playden::GameId::Local("6d1f3a52-0c55-4f3e-9b2a-5d1c7e0a9f11");
    CHECK(!RecipeForGame(parsed.recipe, installed));
    BindCommunityRecipe(&parsed, installed.game, installed.runtime_number);
    CHECK(RecipeForGame(parsed.recipe, installed) && parsed.recipe.app_id == 0xF0000001u);
  }

  // Community recipes in use: identity by revision, the player's one base (preset or recipe), which
  // recipe a game runs with, what a switch drops, and the registry values a recipe leaves in the bottle.
  {
    const std::string rev_a(64, 'a'), rev_b(64, 'b');
    GameRecipe a = hl2;
    a.id = "community-steam-220-fast";
    a.revision = rev_a;
    a.locked.clear();
    a.registry = {{"Software\\Wine\\AppDefaults\\hl2.exe", "Version", "\"win7\""}, {"Software\\Playden\\Test", "A", "\"1\""}};
    GameRecipe b = a;
    b.revision = rev_b;
    b.registry = {{"Software\\Wine\\AppDefaults\\hl2.exe", "Version", "\"win10\""}, {"Software\\Playden\\Test", "B", "\"2\""}};
    // The tag an install record carries: the whole number for Playden's recipes, 32 hex for a community one.
    CHECK(RecipeVersionTag(hl2) == "1" && RecipeVersionTag(a) == std::string(32, 'a') && RecipeVersionTag(b) == std::string(32, 'b'));
    CHECK(IsCommunityRevision(rev_a) && !IsCommunityRevision(std::string(63, 'a')) && !IsCommunityRevision(std::string(64, 'A')));
    CHECK(IsCommunityRevisionPrefix(std::string(32, 'a')) && !IsCommunityRevisionPrefix(rev_a) && !IsCommunityRevisionPrefix(""));
    CHECK(RevisionHasTag(rev_a, std::string(32, 'a')) && !RevisionHasTag(rev_a, std::string(32, 'b')) && !RevisionHasTag(rev_a, rev_a));
    // Two revisions of one community recipe are never confused with each other.
    RuntimeInstall record;
    record.runtime_number = 220;
    record.recipe_id = a.id;
    record.recipe_version = RecipeVersionTag(a);
    CHECK(RecipeForGame(a, record) && RecipeMatchesInstall(a, record) && !RecipeMatchesInstall(b, record));
    record.recipe_version = RecipeVersionTag(b);
    CHECK(RecipeMatchesInstall(b, record) && !RecipeMatchesInstall(a, record));
    record.recipe_id = hl2.id;
    record.recipe_version = "1";
    CHECK(RecipeMatchesInstall(hl2, record) && !RecipeMatchesInstall(a, record));
    record.runtime_number = 221;
    CHECK(!RecipeForGame(hl2, record) && !RecipeMatchesInstall(hl2, record));

    // One base: choosing a recipe clears the preset and the reverse; each is a new revision.
    GameSettingsRecord choice{"mouse-driven", {{"resolution", "1280x720"}}, 3};
    ChooseCommunityRecipe(&choice, a.id, rev_a);
    CHECK(choice.preset_id.empty() && choice.community_recipe.id == a.id && choice.community_recipe.revision == rev_a &&
          choice.revision == 4 && choice.overrides.size() == 1);
    GameSettingsRecord reread;
    CHECK(ParseGameSettingsRecord(SerializeGameSettingsRecord(choice), &reread, &reason) && reread == choice);
    CHECK(SerializeGameSettingsRecord(choice).find("\"community_recipe\":{\"id\":\"community-steam-220-fast\",\"revision\":\"" + rev_a + "\"}") !=
          std::string::npos);
    ChoosePreset(&choice, "mouse-driven");
    CHECK(choice.preset_id == "mouse-driven" && choice.community_recipe.id.empty() && choice.community_recipe.revision.empty() &&
          choice.revision == 5);
    CHECK(SerializeGameSettingsRecord(choice).find("community_recipe") == std::string::npos);
    choice.shared_recipe = {"sub-1", a.id};
    CHECK(ParseGameSettingsRecord(SerializeGameSettingsRecord(choice), &reread, &reason) && reread == choice &&
          reread.shared_recipe.submission_id == "sub-1");
    // The game a share was made for is kept; an older record without it reads back as unknown.
    CHECK(!reread.shared_recipe.game.valid() &&
          SerializeGameSettingsRecord(choice).find("\"game\"") == std::string::npos);
    choice.shared_recipe = {"sub-1", a.id, playden::GameRef{"imported", "night-sky"}};
    CHECK(SerializeGameSettingsRecord(choice).find(",\"game\":\"imported/night-sky\"}") != std::string::npos);
    CHECK(ParseGameSettingsRecord(SerializeGameSettingsRecord(choice), &reread, &reason) && reread == choice &&
          reread.shared_recipe.game == (playden::GameRef{"imported", "night-sky"}));
    // Another game's recipe (chosen from a search): the choice names that game, which becomes the linked game. Both are
    // saved; an older record has neither (the game's own recipe, nothing linked), and Unlink keeps the choice.
    const playden::GameRef gog{"gog", "1207658924"};
    const auto gog_id = CommunityRecipeIdFor(gog, "pad");
    GameSettingsRecord linked;
    ChooseCommunityRecipe(&linked, gog_id, rev_a, gog);
    CHECK(linked.community_recipe.game == gog && linked.linked_game == gog && linked.revision == 1);
    CHECK(SerializeGameSettingsRecord(linked).find("\"community_recipe\":{\"id\":\"" + gog_id + "\",\"revision\":\"" + rev_a +
                                                   "\",\"game\":\"gog/1207658924\"},\"linked_game\":\"gog/1207658924\"") != std::string::npos);
    CHECK(ParseGameSettingsRecord(SerializeGameSettingsRecord(linked), &reread, &reason) && reread == linked);
    ChooseCommunityRecipe(&linked, a.id, rev_a);  // the game's own recipe: the link stays
    CHECK(!linked.community_recipe.game.valid() && linked.linked_game == gog);
    linked.linked_game = {};
    CHECK(SerializeGameSettingsRecord(linked).find("linked_game") == std::string::npos &&
          SerializeGameSettingsRecord(linked).find("\"game\"") == std::string::npos);
    CHECK(ParseGameSettingsRecord("{\"community_recipe\":{\"id\":\"" + gog_id + "\",\"revision\":\"" + rev_a +
                                  "\",\"game\":\"gog/x\"},\"linked_game\":\"nowhere\"}", &reread, &reason) &&
          reread.community_recipe.id == gog_id && !reread.community_recipe.game.valid() && !reread.linked_game.valid());
    // A malformed choice is dropped, not the record.
    CHECK(ParseGameSettingsRecord("{\"community_recipe\":{\"id\":\"community-steam-220-fast\",\"revision\":\"abc\"}}", &reread, &reason) &&
          reread.community_recipe.id.empty());
    CHECK(ParseGameSettingsRecord("{\"community_recipe\":{\"id\":\"steam-220-auto\",\"revision\":\"" + rev_a + "\"}}", &reread, &reason) &&
          reread.community_recipe.id.empty());
    // A file naming both a preset and a recipe loads as the recipe alone: one base, never stacked.
    CHECK(ParseGameSettingsRecord("{\"preset\":\"mouse-driven\",\"community_recipe\":{\"id\":\"community-steam-220-fast\",\"revision\":\"" +
                                      rev_a + "\"}}", &reread, &reason) &&
          reread.preset_id.empty() && reread.community_recipe.id == "community-steam-220-fast");

    // Precedence: the wanted community recipe when it loaded and fits, else shipped, else generated.
    CommunityRecipe ca;
    ca.recipe = a;
    ca.executable = "hl2.exe";
    ca.title = "Fast";
    ca.status = "works";
    GameRecipe generated = hl2;
    generated.id = "steam-220-auto";
    VerifiedKitView kit;
    kit.template_audio_ps5 = true;
    RecipeCandidates candidates;
    candidates.shipped = &hl2;
    candidates.generated = &generated;
    candidates.kit = &kit;
    candidates.kit_version = ParseKitVersion("0.12");
    candidates.title_kit_version = candidates.kit_version;
    auto selection = SelectRecipe(candidates);
    CHECK(selection.recipe == &hl2 && !selection.community && selection.fallback_reason.empty());
    candidates.shipped = nullptr;
    CHECK(SelectRecipe(candidates).recipe == &generated);
    candidates.shipped = &hl2;
    const CommunityRecipeChoice wanted{a.id, rev_a};
    candidates.wanted = &wanted;
    candidates.community = &ca;
    candidates.community_state = CommunityObjectState::kLoaded;
    selection = SelectRecipe(candidates);
    CHECK(selection.recipe == &ca.recipe && selection.community && selection.fallback_reason.empty());
    // The object missing, unreadable, or another revision: Playden's recipe, with the reason.
    candidates.community = nullptr;
    candidates.community_state = CommunityObjectState::kMissing;
    selection = SelectRecipe(candidates);
    CHECK(selection.recipe == &hl2 && !selection.community && selection.fallback_reason == kCommunityRecipeRemoved);
    candidates.community_state = CommunityObjectState::kUnreadable;
    CHECK(SelectRecipe(candidates).fallback_reason == kCommunityRecipeUnreadable);
    CommunityRecipe cb = ca;
    cb.recipe = b;
    candidates.community = &cb;
    candidates.community_state = CommunityObjectState::kLoaded;
    CHECK(SelectRecipe(candidates).fallback_reason == kCommunityRecipeUnreadable);
    // An id longer than the game's title holds (another store's) is selected: the title gets its alias.
    auto long_id = ca;
    long_id.recipe.id = CommunityRecipeIdFor({"imported", std::string(48, 'a')}, "fast");
    const CommunityRecipeChoice long_wanted{long_id.recipe.id, rev_a};
    candidates.wanted = &long_wanted;
    candidates.community = &long_id;
    selection = SelectRecipe(candidates);
    CHECK(selection.recipe == &long_id.recipe && selection.community && selection.fallback_reason.empty());
    CHECK(InstalledRecipeId(long_id.recipe.id) == "community-h-6e9d499dc48358f916810bf5");
    candidates.wanted = &wanted;
    // The kit: a min_kit newer than the kit, and one the kit has but the game's title (older runtime) doesn't.
    candidates.community = &ca;
    ca.min_kit = "0.13";
    selection = SelectRecipe(candidates);
    CHECK(selection.recipe == &hl2 && selection.fallback_reason == "The recipe needs runtime kit version 0.13 or newer.");
    ca.min_kit = "0.12";
    CHECK(SelectRecipe(candidates).community);
    candidates.title_kit_version.reset();
    selection = SelectRecipe(candidates);
    CHECK(!selection.community && selection.fallback_reason == std::string("The recipe needs runtime kit version 0.12 or newer.") + kCommunityRecipeNeedsRuntimeUpdate);
    candidates.title_kit_version = candidates.kit_version;
    ca.min_kit.clear();
    ca.recipe.requirements = {{"audio", "ps5"}};
    kit.template_audio_ps5 = false;
    CHECK(SelectRecipe(candidates).fallback_reason.find("lacks what this recipe needs") != std::string::npos);
    kit.template_audio_ps5 = true;
    candidates.kit = nullptr;
    CHECK(!SelectRecipe(candidates).community);

    // Dropped overrides: a launch option the new recipe doesn't have, a row it locks.
    GameRecipe two = a;
    two.launch_entries.push_back({"editor", "Editor", "hammer.exe", ""});
    const std::map<std::string, std::string> overrides = {
        {"launch_option", "editor"}, {"resolution", "1280x720"}, {"graphics", "opengl"}};
    auto dropped = DroppedOverrides(two, nullptr, hl2, nullptr, overrides, KitCapabilities{});
    CHECK(dropped.size() == 2);
    CHECK(dropped[0].setting_id == "graphics" && dropped[0].reason == "Locked by the recipe.");
    CHECK(dropped[1].setting_id == "launch_option" && dropped[1].value == "editor" &&
          dropped[1].reason.find("no launch option") != std::string::npos);
    // Nothing is dropped going the other way, and a value that didn't apply before is no loss.
    CHECK(DroppedOverrides(hl2, nullptr, two, nullptr, overrides, KitCapabilities{}).empty());
    CHECK(DroppedOverrides(hl2, nullptr, a, nullptr, {{"launch_option", "nowhere"}}, KitCapabilities{}).empty());

    // Another game's recipe whose program isn't in this copy starts Playden's own: its launch entries and architecture,
    // none of the recipe's program defaults or locks; the rest stays the recipe's.
    GameRecipe other = a;
    other.launch_entries = {{"game", "Game", "GOG/Launcher.exe", ""}};
    other.architecture = "pe32";
    other.defaults["launch_option"] = "game";
    other.locked.insert("launch_option");
    other.defaults["resolution"] = "1280x720";
    GameRecipe own = two;
    own.architecture = "pe64";
    std::vector<std::string> asked;
    auto adapted = other;
    CHECK(UseOwnProgram(&adapted, own, [&](const std::string& program) { asked.push_back(program); return std::string(); }) ==
          "GOG/Launcher.exe");
    CHECK(asked == std::vector<std::string>{"GOG/Launcher.exe"} && adapted.launch_entries.size() == 2 &&
          adapted.launch_entries[1].executable == "hammer.exe" && adapted.architecture == "pe64" &&
          !adapted.defaults.count("launch_option") && !adapted.locked.count("launch_option") &&
          adapted.defaults.at("resolution") == "1280x720" && adapted.id == other.id);
    // With Playden's program the player's launch option still applies; with the recipe's it is dropped, as a switch says.
    CHECK(DroppedOverrides(two, nullptr, adapted, nullptr, {{"launch_option", "editor"}}, KitCapabilities{}).empty());
    CHECK(DroppedOverrides(two, nullptr, other, nullptr, {{"launch_option", "editor"}}, KitCapabilities{}).size() == 1);
    // The program is there (any '\\' made '/'), or Playden has no program either: nothing changes.
    auto present = other;
    present.launch_entries[0].executable = "GOG\\Launcher.exe";
    CHECK(UseOwnProgram(&present, own, [](const std::string& program) {
            return program == "GOG/Launcher.exe" ? std::string("pe32") : std::string();
          }).empty() &&
          present.launch_entries[0].executable == "GOG\\Launcher.exe");
    auto no_own = other;
    CHECK(UseOwnProgram(&no_own, GameRecipe{}, [](const std::string&) { return std::string(); }).empty() &&
          no_own.launch_entries[0].executable == "GOG/Launcher.exe");
    // The program the recipe selects decides (its launch_option default, else its first entry), not its first entry.
    auto selects_second = other;
    selects_second.launch_entries = {{"launcher", "Launcher", "Launcher.exe", ""}, {"game", "Game", "Game.exe", ""}};
    selects_second.defaults["launch_option"] = "game";
    auto copy = selects_second;
    asked.clear();
    CHECK(UseOwnProgram(&copy, own, [&](const std::string& program) {
            asked.push_back(program);
            return program == "Launcher.exe" ? std::string("pe32") : std::string();
          }) == "Game.exe" &&
          asked == std::vector<std::string>{"Game.exe"} && copy.launch_entries.size() == own.launch_entries.size() &&
          copy.launch_entries[0].executable == own.launch_entries[0].executable);
    copy = selects_second;
    CHECK(UseOwnProgram(&copy, own, [](const std::string& program) {
            return program == "Game.exe" ? std::string("pe32") : std::string();
          }).empty() &&
          copy.launch_entries.size() == 2 && copy.launch_entries[1].executable == "Game.exe" &&
          copy.defaults.at("launch_option") == "game");
    CHECK(SelectedProgram(selects_second) == "Game.exe");
    selects_second.defaults.erase("launch_option");
    CHECK(SelectedProgram(selects_second) == "Launcher.exe");
    // A program the copy has, built for the other architecture: the copy's file decides, defaults and locks included.
    auto other_arch = other;
    other_arch.defaults["architecture"] = "pe32";
    other_arch.locked.insert("architecture");
    CHECK(UseOwnProgram(&other_arch, own, [](const std::string&) { return std::string("pe64"); }).empty() &&
          other_arch.architecture == "pe64" && !other_arch.defaults.count("architecture") &&
          !other_arch.locked.count("architecture") && other_arch.launch_entries.size() == 1 &&
          other_arch.launch_entries[0].executable == "GOG/Launcher.exe");

    // Registry: A, then B (A's value B doesn't set is removed), then Playden's own (everything removed).
    RecipeRegistryState after_a, after_b, after_default;
    auto values = RecipeRegistryTransition(nullptr, a, &after_a);
    CHECK(values.size() == 2 && values[0].data == "\"win7\"" && after_a.recipe_id == a.id && after_a.revision == rev_a &&
          after_a.values.size() == 2);
    values = RecipeRegistryTransition(&after_a, b, &after_b);
    CHECK(values.size() == 3 && values[0].key == "Software\\Playden\\Test" && values[0].name == "A" && values[0].data.empty() &&
          values[1].data == "\"win10\"" && values[2].name == "B" && after_b.revision == rev_b && after_b.values.size() == 2);
    values = RecipeRegistryTransition(&after_b, hl2, &after_default);
    CHECK(values.size() == 2 && values[0].data.empty() && values[1].data.empty() && after_default.values.empty() &&
          after_default.recipe_id == hl2.id && after_default.revision.empty());
    CHECK(RecipeRegistryTransition(&after_b, b, &after_default).size() == 2);  // the same recipe again: nothing removed
    RecipeRegistryState parsed_state;
    CHECK(ParseRecipeRegistryState(SerializeRecipeRegistryState(after_b), &parsed_state, &reason) &&
          parsed_state.recipe_id == after_b.recipe_id && parsed_state.revision == rev_b && parsed_state.values == after_b.values);
    CHECK(!ParseRecipeRegistryState("{\"recipe_id\":1}", &parsed_state, &reason) && !ParseRecipeRegistryState("[]", &parsed_state, &reason));
    // Case: Wine folds it, so a value in both recipes is only assigned (a removal plus an assignment would be
    // refused as a duplicate), whether the value name, the key, or both differ in case.
    {
      GameRecipe upper = a, lower = a, keyed = a;
      upper.registry = {{"Software\\Publisher\\Game", "Quality", "\"1\""}, {"Software\\Publisher\\Game", "Old", "\"1\""}};
      lower.registry = {{"Software\\Publisher\\Game", "quality", "\"2\""}};
      keyed.registry = {{"SOFTWARE\\publisher\\GAME", "QUALITY", "\"3\""}};
      RecipeRegistryState state_upper, state_lower, state_keyed;
      RecipeRegistryTransition(nullptr, upper, &state_upper);
      auto moved = RecipeRegistryTransition(&state_upper, lower, &state_lower);
      CHECK(moved.size() == 2 && moved[0].name == "Old" && moved[0].data.empty() && moved[1].name == "quality" && moved[1].data == "\"2\"");
      moved = RecipeRegistryTransition(&state_lower, keyed, &state_keyed);
      CHECK(moved.size() == 1 && moved[0].key == "SOFTWARE\\publisher\\GAME" && moved[0].data == "\"3\"");
      std::string user_reg = "WINE REGISTRY Version 2\n;; All keys relative to \\\\User\\\\S-1-5-21-0-0-0-1000\n\n";
      CHECK(ApplyRegistryValues(&user_reg, upper.registry, 1760000000, &reason));
      CHECK(ApplyRegistryValues(&user_reg, moved, 1760000000, &reason));
    }
    // Interruption: the interim state lists the old values too, so after the bottle took the new recipe's
    // values but the final state never got written, a switch to Playden's own removes both recipes' values.
    {
      const auto interim = RecipeRegistryInterim(&after_a, after_b);
      CHECK(interim.recipe_id == after_b.recipe_id && interim.revision == rev_b);
      for (const auto& value : after_a.values)
        CHECK(std::find(interim.values.begin(), interim.values.end(), value) != interim.values.end());
      for (const auto& value : after_b.values)
        CHECK(std::find(interim.values.begin(), interim.values.end(), value) != interim.values.end());
      CHECK(interim.values.size() == 3);  // the shared value is listed once
      RecipeRegistryState back;
      const auto removed = RecipeRegistryTransition(&interim, hl2, &back);
      CHECK(removed.size() == 3 && back.values.empty());
      for (const auto& value : removed) CHECK(value.data.empty());
      CHECK(RecipeRegistryInterim(nullptr, after_b).values == after_b.values);
    }
  }

  // Game invites (title-notify): listed beside the Steam rows, Ask by default, and only with Steam features = Online.
  // Ask and Only show need a title that shows notifications: without one both are Off (never Join at once), and
  // the row says so; on a title still on an older runtime the choice waits for its update.
  {
    GameRecipe ffxiii;
    CHECK(ParseGameRecipe(ReadText(source / "runtime/recipes/steam-292120-ffxiii-v1.json"), &ffxiii, &reason));
    const auto* invites = FindSetting("game_invites");
    CHECK(invites && invites->label == "Game invites" && invites->playden_default == "ask");
    CHECK(invites->group == FindSetting("steam_sign_in")->group);
    CHECK(invites->choices.size() == 4 && invites->choices[0].value == "ask" && invites->choices[1].value == "join" &&
          invites->choices[2].value == "show" && invites->choices[3].value == "off");
    CHECK(invites->choices[0].label == "Ask with L3 + R3" && invites->choices[1].label == "Join at once" &&
          invites->choices[2].label == "Only show" && invites->choices[3].label == "Off");
    CHECK(SettingShown(*invites, KitCapabilities{}));
    const auto notify = KitCapabilitiesFromManifest({kKitTitleNotify}, false);
    CHECK(notify.title_notify && !KitCapabilitiesFromManifest({kKitInputRumble}, false).title_notify);
    CHECK(std::find(SettingsKitCapabilityNames().begin(), SettingsKitCapabilityNames().end(), kKitTitleNotify) !=
          SettingsKitCapabilityNames().end());
    const auto stale = KitForTitleRuntime(notify, false);
    CHECK(!stale.title_notify && stale.title_notify_needs_update);
    CHECK(KitForTitleRuntime(notify, true).title_notify);
    EffectiveSettings settings;
    SettingsError error;
    const auto effective = [&](std::map<std::string, std::string> overrides, const KitCapabilities& kit) {
      overrides["unlock.steam_features"] = "on";  // the recipe locks Steam features
      CHECK(BuildEffectiveSettings(ffxiii, nullptr, overrides, kit, &settings, &error));
      return settings.values.at("game_invites");
    };
    // Not Online: the row does not apply.
    auto row = effective({{"steam_features", "emulate"}, {"game_invites", "off"}}, notify);
    CHECK(!row.available && row.reason == "Only with Steam features = Online.");
    // With notifications every choice applies as chosen; Ask is the default.
    row = effective({{"steam_features", "online"}}, notify);
    CHECK(row.available && row.value == "ask" && row.reason.empty());
    for (const char* value : {"ask", "join", "show", "off"}) {
      row = effective({{"steam_features", "online"}, {"game_invites", value}}, notify);
      CHECK(row.value == value && row.reason.empty() && !row.waits_for_update);
      CHECK(SettingChoiceUnavailable(settings, "game_invites", value).empty());
    }
    // Without them only Join at once and Off: Ask (also the default) and Only show are Off.
    row = effective({{"steam_features", "online"}}, KitCapabilities{});
    CHECK(row.available && row.value == "off" && row.reason == "Needs a runtime kit that shows game invites.");
    row = effective({{"steam_features", "online"}, {"game_invites", "ask"}}, KitCapabilities{});
    CHECK(row.value == "off" && row.reason == "Needs a runtime kit that shows game invites.");
    row = effective({{"steam_features", "online"}, {"game_invites", "show"}}, KitCapabilities{});
    CHECK(row.value == "off" && row.reason == "Needs a runtime kit that shows game invites.");
    CHECK(!SettingChoiceUnavailable(settings, "game_invites", "ask").empty() &&
          !SettingChoiceUnavailable(settings, "game_invites", "show").empty());
    CHECK(SettingChoiceUnavailable(settings, "game_invites", "join").empty() &&
          SettingChoiceUnavailable(settings, "game_invites", "off").empty());
    row = effective({{"steam_features", "online"}, {"game_invites", "off"}}, KitCapabilities{});
    CHECK(row.value == "off" && row.reason.empty());
    // A title on an older runtime: the same fallback until its update, the choice kept for it.
    row = effective({{"steam_features", "online"}, {"game_invites", "show"}}, stale);
    CHECK(row.value == "off" && row.waits_for_update && row.after_update == "show" && ChosenValue(row) == "show");
    CHECK(row.reason == kAppliesAfterKitUpdate && SettingChoiceWaitsForUpdate(settings, "game_invites", "show"));
    row = effective({{"steam_features", "online"}, {"game_invites", "join"}}, stale);
    CHECK(row.value == "join" && !row.waits_for_update);
  }

  // Without online-steam: no Online choice, Steam sign-in and Steam network are not listed, a saved Online is ignored
  // and the emulator stays off the network.
  {
    SetOnlineSteamEnabled(false);
    GameRecipe ffxiii;
    CHECK(ParseGameRecipe(ReadText(source / "runtime/recipes/steam-292120-ffxiii-v1.json"), &ffxiii, &reason));
    CHECK(!SettingChoiceOffered("steam_features", "online") && SettingChoiceOffered("steam_features", "emulate"));
    CHECK(!SettingShown(*FindSetting("steam_sign_in"), KitCapabilities{}));
    CHECK(!SettingShown(*FindSetting("steam_network"), KitCapabilities{}));
    CHECK(!SettingShown(*FindSetting("game_invites"), KitCapabilities{}));
    CHECK(SettingShown(*FindSetting("steam_features"), KitCapabilities{}));
    CHECK(!ValidateSettingValue(ffxiii, "steam_features", "online", &reason));
    EffectiveSettings settings;
    SettingsError error;
    CHECK(BuildEffectiveSettings(ffxiii, nullptr,
                                 {{"steam_features", "online"}, {"unlock.steam_features", "on"}, {"steam_sign_in", "kept"},
                                  {"steam_network", "online"}},
                                 KitCapabilities{}, &settings, &error));
    CHECK(settings.values.at("steam_features").value == "emulate");
    CHECK(settings.values.at("steam_sign_in").value == "play");
    CHECK(settings.values.at("steam_network").value == "offline" && !settings.values.at("steam_network").available);
    // The saved Online goes, with its sign-in; Steam network stays for when the feature is back.
    GameSettingsRecord record;
    // A recipe's Online is kept when it is read, and ignored while online-steam is off: the game still starts.
    {
      auto online_recipe = ffxiii;
      online_recipe.defaults["steam_features"] = "online";
      GameRecipe reread;
      CHECK(ParseGameRecipe(SerializeGameRecipe(online_recipe), &reread, &reason));
      CHECK(reread.defaults.at("steam_features") == "online");
      EffectiveSettings ignored;
      CHECK(BuildEffectiveSettings(reread, nullptr, {}, KitCapabilities{}, &ignored, &error));
      CHECK(ignored.values.at("steam_features").value != "online");
    }
    record.overrides = {{"steam_features", "online"}, {"unlock.steam_features", "on"}, {"steam_sign_in", "kept"},
                        {"game_invites", "off"}, {"steam_network", "offline"}};
    CHECK(DropOnlineSteamSettings(&record) && record.revision == 1);
    CHECK(record.overrides == (std::map<std::string, std::string>{{"steam_network", "offline"}}));
    CHECK(!DropOnlineSteamSettings(&record) && record.revision == 1);
    record.overrides = {{"steam_features", "emulate"}};
    CHECK(!DropOnlineSteamSettings(&record));
    record.overrides = {{"steam_features", "emulate"}, {"game_invites", "join"}};
    CHECK(DropOnlineSteamSettings(&record) && record.overrides.size() == 1);
    SetOnlineSteamEnabled(true);
    CHECK(SettingChoiceOffered("steam_features", "online"));
  }

  std::puts("game settings tests passed");
}
