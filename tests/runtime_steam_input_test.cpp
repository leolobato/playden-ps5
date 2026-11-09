#include "runtime/steam_input.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>

#define CHECK(cond)                                                                     \
  do {                                                                                  \
    if (!(cond)) {                                                                      \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);     \
      std::exit(1);                                                                     \
    }                                                                                   \
  } while (0)

using namespace playden::runtime;
namespace fs = std::filesystem;

namespace {

// A controller configuration shaped like the ones games ship (controller_xboxone.vdf): two action sets, an action
// layer, groups bound by id, a mode shift and an inactive group.
const char* const kConfig = R"VDF("controller_mappings"
{
	"version"		"3"
	"controller_type"		"controller_xboxone"
	"actions"
	{
		"Menu"
		{
			"title"		"#Set_Menu"
			"StickPadGyro" { "Cursor" { "input_mode" "joystick_move" } }
			"Button" { "select" "#Select" "back" "#Back" }
		}
		"OnFoot"
		{
			"title"		"#Set_OnFoot"
		}
	}
	"action_layers"
	{
		"OnFootQuick" { "title" "#Quick" "parent_set_name" "OnFoot" }
	}
	"group"
	{
		"id"		"0"
		"mode"		"four_buttons"
		"inputs"
		{
			"button_a" { "activators" { "Full_Press" { "bindings" { "binding" "game_action Menu select, #Select" } } } }
			"button_b" { "activators" { "Full_Press" { "bindings" { "binding" "game_action Menu back, #Back" } } } }
		}
	}
	"group"
	{
		"id"		"1"
		"mode"		"joystick_move"
		"gameactions" { "Menu" "Cursor" }
	}
	"group"
	{
		"id"		"2"
		"mode"		"four_buttons"
		"inputs"
		{
			"button_a"
			{
				"activators"
				{
					"Full_Press" { "bindings" { "binding" "game_action OnFoot jump, #Jump" "binding" "game_action OnFoot interact, #Interact" } }
					"Long_Press" { "bindings" { "binding" "game_action OnFoot crouch, #Crouch" } }
				}
			}
			"button_x" { "activators" { "Full_Press" { "bindings" { "binding" "key_press SPACE, #Space" } } } }
		}
	}
	"group"
	{
		"id"		"3"
		"mode"		"joystick_move"
		"gameactions" { "OnFoot" "Move" }
		"inputs" { "click" { "activators" { "Full_Press" { "bindings" { "binding" "game_action OnFoot run, #Run" } } } } }
	}
	"group"
	{
		"id"		"4"
		"mode"		"joystick_camera"
		"gameactions" { "OnFoot" "Look" }
	}
	"group"
	{
		"id"		"5"
		"mode"		"trigger"
		"gameactions" { "OnFoot" "Aim" }
		"inputs" { "click" { "activators" { "Full_Press" { "bindings" { "binding" "game_action OnFoot zoom, #Zoom" } } } } }
	}
	"group"
	{
		"id"		"6"
		"mode"		"dpad"
		"inputs"
		{
			"dpad_north" { "activators" { "Full_Press" { "bindings" { "binding" "game_action OnFoot build, #Build" } } } }
			"dpad_west" { "activators" { "Full_Press" { "bindings" { "binding" "game_action OnFoot hud, #Hud" } } } }
		}
	}
	"group"
	{
		"id"		"7"
		"mode"		"four_buttons"
		"inputs"
		{
			"button_a" { "activators" { "Full_Press" { "bindings" { "binding" "game_action OnFoot shifted, #Shifted" } } } }
			"button_y" { "activators" { "Full_Press" { "bindings" { "binding" "game_action OnFoot quick, #Quick" } } } }
		}
	}
	"group"
	{
		"id"		"8"
		"mode"		"dpad"
		"inputs"
		{
			"dpad_north" { "activators" { "Full_Press" { "bindings" { "binding" "game_action OnFoot lean, #Lean" } } } }
			"click" { "activators" { "Full_Press" { "bindings" { "binding" "game_action OnFoot melee, #Melee" } } } }
		}
	}
	"group"
	{
		"id"		"9"
		"mode"		"four_buttons"
		"inputs"
		{
			"button_b" { "activators" { "Full_Press" { "bindings" { "binding" "game_action OnFoot inactive, #Inactive" } } } }
		}
	}
	"preset"
	{
		"id"		"0"
		"name"		"Menu"
		"group_source_bindings"
		{
			"0"		"button_diamond active"
			"1"		"joystick active"
		}
	}
	"preset"
	{
		"id"		"1"
		"name"		"OnFoot"
		"group_source_bindings"
		{
			"2"		"button_diamond active"
			"3"		"joystick active"
			"4"		"right_joystick active"
			"5"		"right_trigger active"
			"6"		"dpad active"
			"7"		"button_diamond active modeshift"
			"8"		"right_joystick inactive"
			"9"		"switch inactive"
		}
	}
	"preset"
	{
		"id"		"2"
		"name"		"OnFootQuick"
		"group_source_bindings"
		{
			"7"		"button_diamond active"
		}
	}
	"preset"
	{
		"id"		"3"
		"name"		"Default"
		"group_source_bindings"
		{
			"7"		"button_diamond active"
		}
	}
}
)VDF";

const char* const kManifest = R"VDF("Action Manifest"
{
	"configurations"
	{
		"controller_ps4" { "0" { "path" "controller_ps4.vdf" } }
		"controller_xboxone" { "0" { "path" "Configs\\Controller_XboxOne.vdf" } }
	}
}
)VDF";

const SteamInputActionSet* Find(const std::vector<SteamInputActionSet>& sets, const std::string& name) {
  for (const auto& set : sets)
    if (set.name == name) return &set;
  return nullptr;
}

void Write(const fs::path& path, const std::string& text) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << text;
}

std::string Read(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream text;
  text << in.rdbuf();
  return text.str();
}

void TestConvertsActionSets() {
  const auto sets = ConvertSteamInputConfig(kConfig);
  CHECK(sets.size() == 2);  // the layer and "Default" stay out; a Long_Press (crouch) isn't a plain press
  const auto* menu = Find(sets, "Menu");
  CHECK(menu);
  CHECK(menu->text == "select=A\nback=B\nCursor=LJOY=joystick_move\n");
  const auto* foot = Find(sets, "OnFoot");
  CHECK(foot);
  CHECK(foot->text ==
        "jump=A\n"
        "interact=A\n"
        "Move=LJOY=joystick_move\n"
        "run=LSTICK\n"
        "Look=RJOY=joystick_camera\n"
        "Aim=RTRIGGER=trigger\n"
        "zoom=DRTRIGGER\n"
        "build=DUP\n"
        "hud=DLEFT\n");
}

void TestRejectsOtherText() {
  CHECK(ConvertSteamInputConfig("").empty());
  CHECK(ConvertSteamInputConfig("\"Action Manifest\" { }").empty());
  CHECK(ConvertSteamInputConfig("\"controller_mappings\" { \"actions\" { \"Menu\" { } } }").empty());
  CHECK(ConvertSteamInputConfig("\"controller_mappings\" {").empty());
}

void TestPicksXboxConfig() {
  CHECK(SteamInputConfigPath(kManifest) == "Configs\\Controller_XboxOne.vdf");
  CHECK(SteamInputConfigPath("\"Action Manifest\" { \"configurations\" { \"controller_ps4\" { \"0\" { \"path\" \"p.vdf\" } } } }") ==
        "p.vdf");
  CHECK(SteamInputConfigPath("\"Action Manifest\" { }").empty());
  CHECK(SteamInputConfigPath("nope").empty());
}

fs::path TempRoot(const char* name) {
  const auto root = fs::temp_directory_path() / (std::string(name) + "-" + std::to_string(::getpid()));
  fs::remove_all(root);
  fs::create_directories(root);
  return fs::canonical(root);
}

void TestFindsManifestInInstall() {
  const auto root = TempRoot("playden-steam-input");
  CHECK(SteamInputActionSets(root.string()).empty());
  Write(root / "GAMEDATA" / "INPUT" / "STEAM_INPUT_MANIFEST.VDF", kManifest);
  CHECK(SteamInputActionSets(root.string()).empty());  // the configuration it names isn't there yet
  Write(root / "GAMEDATA" / "INPUT" / "configs" / "controller_xboxone.vdf", kConfig);
  const auto sets = SteamInputActionSets(root.string());
  CHECK(sets.size() == 2);
  CHECK(Find(sets, "OnFoot"));
  // A manifest can't point out of its folder.
  Write(root / "GAMEDATA" / "INPUT" / "STEAM_INPUT_MANIFEST.VDF",
        "\"Action Manifest\" { \"configurations\" { \"controller_xboxone\" { \"0\" { \"path\" \"../outside.vdf\" } } } }");
  Write(root / "GAMEDATA" / "outside.vdf", kConfig);
  CHECK(SteamInputActionSets(root.string()).empty());
  fs::remove_all(root);
}

}  // namespace

int main() {
  TestConvertsActionSets();
  TestRejectsOtherText();
  TestPicksXboxConfig();
  TestFindsManifestInInstall();
  std::puts("runtime_steam_input: ok");
  return 0;
}
