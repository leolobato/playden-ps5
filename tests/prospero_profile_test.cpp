#include "runtime/prospero_profile.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <unistd.h>
#include <vector>

#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      std::exit(1);                                                    \
    }                                                                  \
  } while (0)

namespace {

using namespace playden::runtime;

std::vector<std::uint8_t> PeImage(unsigned machine) {
  std::vector<std::uint8_t> image(0x200, 0);
  image[0] = 'M';
  image[1] = 'Z';
  image[0x3c] = 0x80;
  image[0x80] = 'P';
  image[0x81] = 'E';
  image[0x84] = machine & 0xff;
  image[0x85] = machine >> 8;
  return image;
}

std::string ReadText(const std::filesystem::path& path) {
  std::ifstream file(path);
  return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

ProsperoGame HalfLife() {
  ProsperoGame game;
  game.app_id = 220;
  game.name = "Half-Life 2";
  game.install_root = "/data/homebrew/FAKE77031/games/220/";
  game.launch.executable = "hl2.exe";
  game.launch.arguments = "-game hl2 -steam";
  return game;
}

}  // namespace

int main(int argc, char** argv) {
  const std::filesystem::path fixtures = argc > 1 ? argv[1] : "fixtures/prospero-win-profiles";
  // PE headers.
  auto pe32 = PeImage(0x014c);
  auto pe64 = PeImage(0x8664);
  auto arm = PeImage(0xaa64);
  CHECK(ParsePeArchitecture(pe32.data(), pe32.size()) == PeArchitecture::kPe32);
  CHECK(ParsePeArchitecture(pe64.data(), pe64.size()) == PeArchitecture::kPe64);
  CHECK(!ParsePeArchitecture(arm.data(), arm.size()));
  pe32[0x3c] = 0xff;
  pe32[0x3d] = 0xff;
  CHECK(!ParsePeArchitecture(pe32.data(), pe32.size()));  // header offset outside the file
  CHECK(!ParsePeArchitecture(pe64.data(), 0x20));

  // A Steam launch entry becomes an [application] profile with Z: paths.
  ProsperoProfile profile;
  std::string reason;
  CHECK(DraftProsperoProfile(HalfLife(), &profile, &reason));
  CHECK(profile.id == "playden-steam-220");
  CHECK(profile.text ==
        "; Drafted by Playden from Steam launch data.\n"
        "[application]\n"
        "id = playden-steam-220\n"
        "name = Half-Life 2\n"
        "executable = Z:\\data\\homebrew\\FAKE77031\\games\\220\\hl2.exe\n"
        "working_directory = Z:\\data\\homebrew\\FAKE77031\\games\\220\n"
        "arguments = -game hl2 -steam\n"
        "prefix = playden\n"
        "runtime = wine-wow64\n"
        "architecture = pe32\n"
        "graphics = auto\n");

  // Nested executables default to their own folder; an explicit working folder wins.
  auto nested = HalfLife();
  nested.launch.executable = "bin\\win64\\Game.exe";
  nested.launch.arguments.clear();
  nested.architecture = PeArchitecture::kPe64;
  nested.name = std::string("Bad\tName") + std::string(200, 'x');
  CHECK(DraftProsperoProfile(nested, &profile, &reason));
  CHECK(profile.text.find("executable = Z:\\data\\homebrew\\FAKE77031\\games\\220\\bin\\win64\\Game.exe\n") != std::string::npos);
  CHECK(profile.text.find("working_directory = Z:\\data\\homebrew\\FAKE77031\\games\\220\\bin\\win64\n") != std::string::npos);
  CHECK(profile.text.find("arguments") == std::string::npos);
  CHECK(profile.text.find("architecture = pe64") != std::string::npos);
  CHECK(profile.text.find("name = Bad Name") != std::string::npos);
  const auto name_line = profile.text.substr(profile.text.find("name = "));
  CHECK(name_line.find('\n') == 7 + kProsperoMaxNameBytes);
  nested.launch.working_directory = "bin";
  CHECK(DraftProsperoProfile(nested, &profile, &reason));
  CHECK(profile.text.find("working_directory = Z:\\data\\homebrew\\FAKE77031\\games\\220\\bin\n") != std::string::npos);

  // Unsafe or oversized inputs are refused with a reason, leaving the output untouched.
  const auto before = profile.text;
  auto bad = HalfLife();
  bad.launch.executable = "..\\escape.exe";
  CHECK(!DraftProsperoProfile(bad, &profile, &reason) && !reason.empty());
  bad = HalfLife();
  bad.install_root = "relative/games/220";
  CHECK(!DraftProsperoProfile(bad, &profile, &reason));
  bad.install_root = "/data/C:/games";
  CHECK(!DraftProsperoProfile(bad, &profile, &reason));
  bad = HalfLife();
  bad.launch.arguments = "-a\n-b";
  CHECK(!DraftProsperoProfile(bad, &profile, &reason));
  bad = HalfLife();
  bad.install_root = "/data/" + std::string(260, 'p');
  CHECK(!DraftProsperoProfile(bad, &profile, &reason));
  bad = HalfLife();
  bad.prefix = "../wine";
  CHECK(!DraftProsperoProfile(bad, &profile, &reason));
  CHECK(profile.text == before);

  // Publishing writes the profile and adds one catalog entry, keeping what was there.
  const auto root = std::filesystem::temp_directory_path() / ("playden-prospero-" + std::to_string(::getpid()));
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root / "profiles");
  std::ofstream(root / "profiles" / "profiles.lst") << "# games\nnotepad.profile";
  CHECK(DraftProsperoProfile(HalfLife(), &profile, &reason));
  CHECK(PublishProsperoProfile(root.string(), profile, &reason));
  CHECK(PublishProsperoProfile(root.string(), profile, &reason));  // idempotent
  CHECK(ReadText(root / "profiles" / "playden-steam-220.profile") == profile.text);
  CHECK(ReadText(root / "profiles" / "profiles.lst") == "# games\nnotepad.profile\nplayden-steam-220.profile\n");

  // The catalog holds at most 16 entries.
  std::ofstream full(root / "profiles" / "profiles.lst");
  for (int index = 0; index < 16; ++index) full << "game-" << index << ".profile\n";
  full.close();
  auto other = HalfLife();
  other.app_id = 400;
  CHECK(DraftProsperoProfile(other, &profile, &reason));
  CHECK(!PublishProsperoProfile(root.string(), profile, &reason) && reason == "prospero-win profile list is full");
  std::filesystem::remove_all(root);

  // Every upstream profile passes the checker; Playden's own draft does too.
  int checked = 0;
  for (const auto& entry : std::filesystem::directory_iterator(fixtures / "profiles")) {
    if (entry.path().extension() != ".profile") continue;
    if (!CheckProsperoProfile(ReadText(entry.path()), &reason)) {
      std::fprintf(stderr, "%s: %s\n", entry.path().filename().c_str(), reason.c_str());
      CHECK(false);
    }
    ++checked;
  }
  CHECK(checked >= 15);
  CHECK(DraftProsperoProfile(HalfLife(), &profile, &reason));
  CHECK(CheckProsperoProfile(profile.text, &reason));

  // The checker refuses what prospero-win refuses.
  const std::string base = "[application]\nid = a\nname = A\nexecutable = C:\\a.exe\nworking_directory = C:\\\n"
                           "prefix = a\nruntime = wine-wow64\narchitecture = pe32\ngraphics = dxvk\n";
  CHECK(CheckProsperoProfile(base, &reason));
  CHECK(CheckProsperoProfile(base + "[display]\ndesktop = 1920x1080\nshow_fps = false\n[input]\nmode = xinput\noptions = escape\n"
                             "cross = f12\nsquare = vk:0x41\n[debug]\nwinedebug = err+all,+loaddll\n", &reason));
  CHECK(!CheckProsperoProfile("[display]\ndesktop = 1920x1080\n" + base, &reason));       // [application] first
  CHECK(!CheckProsperoProfile(base + "[display]\nhdr = true\n", &reason));                 // unknown key
  CHECK(!CheckProsperoProfile(base + "[audio]\n", &reason));                                // unknown section
  CHECK(!CheckProsperoProfile(base + "graphics = gdi\n", &reason));                         // duplicate key
  CHECK(!CheckProsperoProfile(base + "[display]\ndesktop = 4096x2160\n", &reason));
  CHECK(!CheckProsperoProfile(base + "[display]\nrefresh = 90\n", &reason));
  // [display] frame_rate needs a display-120hz runtime; older ones refuse the key. opengl_thread still parses.
  CHECK(!CheckProsperoProfile(base + "[display]\nframe_rate = 60\n", &reason));
  for (const char* rate : {"0", "120", "60", "40", "30"})
    CHECK(CheckProsperoProfile(base + "[display]\nrefresh = 120\nframe_rate = " + rate + "\n", &reason,
                               {.display_frame_rate = true}));
  CHECK(!CheckProsperoProfile(base + "[display]\nframe_rate = 50\n", &reason, {.display_frame_rate = true}));
  // [display] framegen needs a display-framegen runtime.
  CHECK(!CheckProsperoProfile(base + "[display]\nframegen = 2\n", &reason));
  for (const char* factor : {"off", "2", "3", "4"})
    CHECK(CheckProsperoProfile(base + "[display]\nframegen = " + factor + "\n", &reason, {.display_framegen = true}));
  CHECK(!CheckProsperoProfile(base + "[display]\nframegen = 5\n", &reason, {.display_framegen = true}));
  CHECK(CheckProsperoProfile(base + "[display]\nopengl_thread = true\n", &reason));
  CHECK(!CheckProsperoProfile(base + "[input]\nmouse_speed = 0\n", &reason));
  CHECK(!CheckProsperoProfile(base + "[input]\nrumble = 50\n", &reason));                 // released runtimes refuse it
  CHECK(CheckProsperoProfile(base + "[input]\nstick_deadzone = 50\nrumble = 0\n", &reason, {.input_tuning = true}));
  CHECK(!CheckProsperoProfile(base + "[input]\nstick_deadzone = 51\n", &reason, {.input_tuning = true}));
  CHECK(!CheckProsperoProfile(base + "[input]\nrumble = 101\n", &reason, {.input_tuning = true}));
  CHECK(CheckProsperoProfile(base + "[input]\nrumble = 0\n", &reason, {.input_rumble = true}));  // input-rumble alone
  CHECK(!CheckProsperoProfile(base + "[input]\nrumble = 101\n", &reason, {.input_rumble = true}));
  CHECK(!CheckProsperoProfile(base + "[input]\nstick_deadzone = 5\n", &reason, {.input_rumble = true}));
  CHECK(!CheckProsperoProfile(base + "[input]\nrumble = -1\n", &reason, {.input_tuning = true}));
  CHECK(CheckProsperoProfile(base + "[input]\ntrigger_deadzone = 50\n", &reason, {.input_tuning = true}));
  CHECK(!CheckProsperoProfile(base + "[input]\nwheel = left_stick\n", &reason, {.input_tuning = true}));
  CHECK(CheckProsperoProfile(base + "[input]\nwheel = left_stick\n", &reason, {.input_wheel = true}));
  CHECK(!CheckProsperoProfile(base + "[input]\nwheel = dpad\n", &reason, {.input_wheel = true}));
  CHECK(!CheckProsperoProfile(base + "[audio]\nvolume = 50\n", &reason));
  CHECK(CheckProsperoProfile(base + "[audio]\nvolume = 0\n", &reason, {.audio_section = true}));
  CHECK(!CheckProsperoProfile(base + "[audio]\nvolume = 101\n", &reason, {.audio_section = true}));
  CHECK(!CheckProsperoProfile(base + "[audio]\nmute = true\n", &reason, {.audio_section = true}));
  CHECK(!CheckProsperoProfile(base + "[input]\ncross = f25\n", &reason));
  CHECK(!CheckProsperoProfile(base + "[input]\ncross = vk:0x00\n", &reason));
  CHECK(!CheckProsperoProfile(base + "[debug]\nwinedebug = +all relay\n", &reason));
  CHECK(!CheckProsperoProfile(base + "arguments = -a \"b\n", &reason));                   // unclosed quote
  CHECK(!CheckProsperoProfile(base + "dll_overrides = d3d9=x\n", &reason));
  CHECK(!CheckProsperoProfile(std::string(8193, ';'), &reason));

  std::vector<std::string> words;
  CHECK(SplitProsperoArguments("-game hl2  \"+exec my cfg\"\t-novid", &words));
  CHECK((words == std::vector<std::string>{"-game", "hl2", "+exec my cfg", "-novid"}));
  std::string many;
  for (int index = 0; index < 33; ++index) many += "a ";
  CHECK(!SplitProsperoArguments(many, &words));
  CHECK(IsProsperoDllOverrides("d3d8,d3d9,d3d10core,d3d11,dxgi=n;winmm=n,b;nvapi="));
  CHECK(!IsProsperoDllOverrides("=n") && !IsProsperoDllOverrides("d3d9") && !IsProsperoDllOverrides("a,,b=n"));

  std::puts("prospero profile tests passed");
}
