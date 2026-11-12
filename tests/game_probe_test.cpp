#include "runtime/game_probe.h"
#include "runtime/game_settings.h"

#include "steam_stub_fixture.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
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

struct PeSpec {
  bool pe64 = true;
  std::vector<std::string> imports;
  bool clr = false;
  bool bind_entry = false;   // entry point inside a ".bind" section (SteamStub)
  std::string text;          // bytes placed in the code section (names loaded at run time)
  std::size_t data_at = 0x1400;  // file offset of the section holding the import table
  bool shared_data = false;  // the data section is writable and shared, on 4 KB pages
  std::vector<std::string> delay_imports;  // delay-loaded DLLs (descriptors at RVA 0x2a00)
  std::string resource;      // the resource directory's bytes, at RVA 0x2c00 (at most 0x400)
  std::string code_section = ".text";  // the first section's name (Watcom's linker calls it BEGTEXT)
  std::uint32_t code_raw_size = 0x1000;  // the first section's SizeOfRawData
};

// Watcom's startup at a program's entry: a near jump over its banner (NFS3's Modern Patch, 0x4f7fa0).
const std::string kWatcomEntry =
    std::string("\xe9\xfb\xd8\x01\x00\x03\x10\x40\x00", 9) + "WATCOM C/C++32 Run-Time system. (c) Copyright";

void Put32(std::string* bytes, std::size_t at, std::uint32_t value) {
  for (int i = 0; i < 4; ++i) (*bytes)[at + i] = static_cast<char>((value >> (8 * i)) & 0xff);
}
void Put16(std::string* bytes, std::size_t at, std::uint16_t value) {
  (*bytes)[at] = static_cast<char>(value & 0xff);
  (*bytes)[at + 1] = static_cast<char>(value >> 8);
}

void WritePe(const fs::path& path, const PeSpec& spec) {
  fs::create_directories(path.parent_path());
  const std::size_t optional_size = spec.pe64 ? 0xf0 : 0xe0;
  std::string bytes(spec.data_at + 0x1000, '\0');
  bytes[0] = 'M'; bytes[1] = 'Z'; Put32(&bytes, 0x3c, 0x40);
  bytes[0x40] = 'P'; bytes[0x41] = 'E';
  Put16(&bytes, 0x44, spec.pe64 ? 0x8664 : 0x014c);
  Put16(&bytes, 0x46, 2);
  Put16(&bytes, 0x54, static_cast<std::uint16_t>(optional_size));
  const std::size_t optional = 0x58;
  Put16(&bytes, optional, spec.pe64 ? 0x20b : 0x10b);
  Put32(&bytes, optional + 16, 0x1010);  // entry point, in the first section
  const std::size_t directories = optional + (spec.pe64 ? 112 : 96);
  if (!spec.imports.empty()) Put32(&bytes, directories + 8, 0x2000);
  if (spec.clr) Put32(&bytes, directories + 14 * 8, 0x2800);
  const std::size_t table = optional + optional_size;
  const auto section = [&](std::size_t row, const char* name, std::uint32_t address, std::uint32_t raw) {
    for (std::size_t i = 0; name[i]; ++i) bytes[table + row * 40 + i] = name[i];
    Put32(&bytes, table + row * 40 + 8, 0x1000);
    Put32(&bytes, table + row * 40 + 12, address);
    Put32(&bytes, table + row * 40 + 16, 0x1000);
    Put32(&bytes, table + row * 40 + 20, raw);
  };
  section(0, spec.bind_entry ? ".bind" : spec.code_section.c_str(), 0x1000, 0x400);
  Put32(&bytes, table + 16, spec.code_raw_size);
  section(1, ".rdata", 0x2000, static_cast<std::uint32_t>(spec.data_at));
  if (spec.shared_data) Put32(&bytes, table + 40 + 36, 0xd0000040);
  bytes.replace(0x400, spec.text.size(), spec.text);
  std::uint32_t name_rva = 0x2400;
  for (std::size_t i = 0; i < spec.imports.size(); ++i) {
    Put32(&bytes, spec.data_at + i * 20 + 12, name_rva);
    bytes.replace(spec.data_at + (name_rva - 0x2000), spec.imports[i].size(), spec.imports[i]);
    name_rva += static_cast<std::uint32_t>(spec.imports[i].size() + 1);
  }
  if (!spec.delay_imports.empty()) Put32(&bytes, directories + 13 * 8, 0x2a00);
  for (std::size_t i = 0; i < spec.delay_imports.size(); ++i) {
    const auto row = spec.data_at + 0xa00 + i * 32;
    Put32(&bytes, row, 1);  // addresses are relative to the image
    Put32(&bytes, row + 4, name_rva);
    bytes.replace(spec.data_at + (name_rva - 0x2000), spec.delay_imports[i].size(), spec.delay_imports[i]);
    name_rva += static_cast<std::uint32_t>(spec.delay_imports[i].size() + 1);
  }
  if (!spec.resource.empty()) {
    Put32(&bytes, directories + 2 * 8, 0x2c00);
    Put32(&bytes, directories + 2 * 8 + 4, static_cast<std::uint32_t>(spec.resource.size()));
    bytes.replace(spec.data_at + 0xc00, spec.resource.size(), spec.resource);
  }
  std::ofstream(path, std::ios::binary) << bytes;
}

std::string Wide(const std::string& text) {
  std::string out;
  for (char value : text) out += value, out += '\0';
  return out;
}

void Write(const fs::path& path, const std::string& bytes) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary | std::ios::trunc) << bytes;
}

// A version resource's FileVersion String entry (key, padding, value), and its VS_FIXEDFILEINFO.
std::string FileVersionString(const std::string& value) {
  std::string entry(6, '\0');
  entry += Wide("FileVersion") + std::string(2, '\0');
  entry.resize((entry.size() + 3) / 4 * 4, '\0');
  entry += Wide(value) + std::string(2, '\0');
  Put16(&entry, 0, static_cast<std::uint16_t>(entry.size()));
  Put16(&entry, 2, static_cast<std::uint16_t>(value.size() + 1));
  Put16(&entry, 4, 1);
  return std::string(10, 'x') + entry;  // anywhere in the resources
}
std::string FixedFileInfo(std::uint32_t major, std::uint32_t minor) {
  std::string info(52, '\0');
  Put32(&info, 0, 0xfeef04bd);
  Put32(&info, 4, 0x10000);
  Put32(&info, 8, major);
  Put32(&info, 12, minor);
  return std::string(4, 'x') + info;
}

void Touch(const fs::path& path) {
  fs::create_directories(path.parent_path());
  std::ofstream(path) << "x";
}

}  // namespace

int main() {
  const auto base = fs::temp_directory_path() / ("playden-game-probe-" + std::to_string(::getpid()));
  fs::remove_all(base);
  int case_number = 0;
  const auto fresh = [&] { return base / std::to_string(++case_number); };

  {  // Imported Direct3D 9, with the import table far into a large program.
    const auto root = fresh();
    WritePe(root / "Game.exe", {false, {"KERNEL32.dll", "d3d9.dll"}, false, false, "", 9u * 1024u * 1024u});
    const auto probe = ProbeGame(root.string(), "Game.exe");
    CHECK(probe.apis.count("d3d9") && probe.engine.empty() && !probe.dotnet && !probe.steam_stub);
    const auto defaults = DefaultsFor(probe);
    CHECK(defaults.graphics == "dxvk" && defaults.arguments.empty() && defaults.refusal.empty());
    CHECK(defaults.summary == "d3d9 graphics=dxvk");
  }
  {  // Unity loads Direct3D at run time (wide strings); D3D11 is forced.
    const auto root = fresh();
    WritePe(root / "Balls.exe", {true, {"UnityPlayer.dll", "KERNEL32.dll"}});
    WritePe(root / "UnityPlayer.dll", {true, {"KERNEL32.dll"}, false, false, Wide("d3d11.dll") + Wide("D3D12.dll")});
    WritePe(root / "GameAssembly.dll", {true, {"KERNEL32.dll"}});
    fs::create_directories(root / "Balls_Data");
    const auto probe = ProbeGame(root.string(), "Balls.exe");
    CHECK(probe.engine == "unity" && probe.il2cpp && probe.apis.count("d3d11") && probe.apis.count("d3d12"));
    const auto defaults = DefaultsFor(probe);
    CHECK(defaults.graphics == "dxvk" && defaults.arguments == "-force-d3d11 {resolution}");
    CHECK(defaults.resolution_arguments == "-screen-width {w} -screen-height {h}");
    CHECK(defaults.summary == "unity-il2cpp,d3d11,d3d12 graphics=dxvk");
  }
  {  // Unity before 2017.2: the player is in the program. Without a version to read, Direct3D 9 is forced.
    const auto root = fresh();
    WritePe(root / "Old.exe", {false, {"KERNEL32.dll", "d3d9.dll"}, false, false, "d3d11.dll"});
    fs::create_directories(root / "Old_Data/Managed");
    const auto probe = ProbeGame(root.string(), "Old.exe");
    CHECK(probe.engine == "unity" && probe.legacy_unity && !probe.il2cpp && probe.unity_major == 0);
    const auto defaults = DefaultsFor(probe);
    CHECK(defaults.graphics == "dxvk" && defaults.arguments == "-force-d3d9 {resolution}");
    CHECK(defaults.summary == "unity-legacy,d3d11,d3d9 graphics=dxvk");
    CHECK(DetectedLabel(defaults.summary) == "Unity \xC2\xB7 Direct3D 9");
    // The data folder's first serialized file names the Unity version 20 bytes in (big-endian header first).
    const auto serialized = [](const std::string& version) {
      std::string bytes(20, '\0');
      bytes[11] = 15;  // format version
      return bytes + version + std::string(1, '\0') + std::string(40, '\x05');
    };
    // Unity 4 (Hexcells, Thomas Was Alone): Direct3D 9.
    Write(root / "Old_Data/mainData", serialized("4.6.1f1"));
    const auto unity4 = ProbeGame(root.string(), "Old.exe");
    CHECK(unity4.unity_major == 4 && DefaultsFor(unity4).arguments == "-force-d3d9 {resolution}");
    // Unity 5 (I Can't Believe It's Not Gambling, 5.6.2f1, built without Direct3D 9; 140, 5.3): Direct3D 11.
    Write(root / "Old_Data/globalgamemanagers", serialized("5.6.2f1"));
    const auto unity5 = ProbeGame(root.string(), "Old.exe");
    CHECK(unity5.unity_major == 5);
    const auto unity5_defaults = DefaultsFor(unity5);
    CHECK(unity5_defaults.arguments == "-force-d3d11 {resolution}");
    CHECK(unity5_defaults.summary == "unity-legacy5,d3d11,d3d9 graphics=dxvk");
    CHECK(DetectedLabel(unity5_defaults.summary) == "Unity \xC2\xB7 Direct3D 11");
    // Text that isn't a version is no version; mainData is read next (Unity 5.0 has no globalgamemanagers).
    Write(root / "Old_Data/globalgamemanagers", serialized("5.6.x"));
    Write(root / "Old_Data/mainData", serialized("5.0.4f1"));
    CHECK(ProbeGame(root.string(), "Old.exe").unity_major == 5);
    fs::remove(root / "Old_Data/mainData");
    CHECK(ProbeGame(root.string(), "Old.exe").unity_major == 0);
    Write(root / "Old_Data/globalgamemanagers", serialized("5.6.2f1garbage"));
    CHECK(ProbeGame(root.string(), "Old.exe").unity_major == 0);
    Write(root / "Old_Data/globalgamemanagers", serialized("5.6.2f1").substr(0, 26));  // cut short
    CHECK(ProbeGame(root.string(), "Old.exe").unity_major == 0);
    // A linked data file is not followed.
    fs::remove(root / "Old_Data/globalgamemanagers");
    Write(base / "unity5-elsewhere", serialized("5.6.2f1"));
    fs::create_symlink(base / "unity5-elsewhere", root / "Old_Data/globalgamemanagers");
    CHECK(ProbeGame(root.string(), "Old.exe").unity_major == 0);
  }
  {  // A linked UnityPlayer.dll is not followed.
    const auto root = fresh();
    WritePe(root / "Game.exe", {true, {"KERNEL32.dll"}});
    WritePe(base / "elsewhere.dll", {true, {}, false, false, "d3d11.dll"});
    fs::create_symlink(base / "elsewhere.dll", root / "UnityPlayer.dll");
    const auto probe = ProbeGame(root.string(), "Game.exe");
    CHECK(probe.engine.empty() && probe.apis.empty() && DefaultsFor(probe).graphics == "dxvk");
  }
  {  // No graphics library named anywhere (Alan Wake loads d3d9 at run time): DXVK, as Wine's own Direct3D can't draw
     // on the console. The label claims nothing.
    const auto root = fresh();
    WritePe(root / "AlanWake.exe", {false, {"KERNEL32.dll", "USER32.dll"}});
    const auto probe = ProbeGame(root.string(), "AlanWake.exe");
    CHECK(probe.engine.empty() && probe.apis.empty());
    const auto defaults = DefaultsFor(probe);
    CHECK(defaults.graphics == "dxvk" && defaults.summary == "unknown graphics=dxvk" && defaults.refusal.empty());
    CHECK(DetectedLabel(defaults.summary).empty());
    CHECK(DetectedLabel("anticheat graphics=dxvk").empty() && DetectedLabel("shared-section graphics=dxvk").empty());
    // .NET without Wine Mono: still refused softly, with DXVK for a player who tries anyway.
    auto dotnet = probe;
    dotnet.dotnet = true;
    const auto soft = DefaultsFor(dotnet);
    CHECK(soft.refusal_soft && !soft.refusal.empty() && soft.graphics == "dxvk");
    // A library beside the program that imports Direct3D 9 says so.
    WritePe(root / "renderer.dll", {false, {"d3d9.dll", "KERNEL32.dll"}});
    const auto sibling = DefaultsFor(ProbeGame(root.string(), "AlanWake.exe"));
    CHECK(sibling.graphics == "dxvk" && sibling.summary == "d3d9 graphics=dxvk");
    CHECK(DetectedLabel(sibling.summary) == "Direct3D 9");
    // One that imports OpenGL keeps the game on OpenGL.
    fs::remove(root / "renderer.dll");
    WritePe(root / "gl.dll", {false, {"OPENGL32.dll", "KERNEL32.dll"}});
    const auto opengl = DefaultsFor(ProbeGame(root.string(), "AlanWake.exe"));
    CHECK(opengl.graphics == "opengl" && opengl.summary == "opengl graphics=opengl");
    // Vulkan alone needs no translation: Automatic.
    fs::remove(root / "gl.dll");
    WritePe(root / "vk.dll", {false, {"vulkan-1.dll", "KERNEL32.dll"}});
    const auto vulkan = ProbeGame(root.string(), "AlanWake.exe");
    CHECK(vulkan.apis.count("vulkan") && DefaultsFor(vulkan).graphics == "auto");
  }
  {  // GameMaker names D3D11 in upper case.
    const auto root = fresh();
    WritePe(root / "Risk.exe", {true, {"KERNEL32.dll"}, false, false, "D3D11.DLL"});
    Touch(root / "data.win");
    const auto probe = ProbeGame(root.string(), "Risk.exe");
    CHECK(probe.engine == "gamemaker" && probe.apis.count("d3d11") && !probe.shared_section);
    CHECK(DefaultsFor(probe).graphics == "dxvk");
  }
  {  // A shared writable section on 4 KB pages (GameMaker's ".mydata", Pizza Tower) can't be mapped.
    const auto root = fresh();
    WritePe(root / "PizzaTower.exe", {true, {"KERNEL32.dll"}, false, false, "D3D11.DLL", 0x1400, true});
    Touch(root / "data.win");
    const auto probe = ProbeGame(root.string(), "PizzaTower.exe");
    CHECK(probe.engine == "gamemaker" && probe.shared_section);
    const auto defaults = DefaultsFor(probe);
    CHECK(defaults.refusal.find("shared memory section") != std::string::npos);
    CHECK(defaults.refusal_soft && defaults.summary == "gamemaker,d3d11,shared-section graphics=dxvk");
    // A kit whose Wine maps the section as a private copy starts it like any other game.
    RuntimeAbilities runtime;
    runtime.shared_sections = true;
    CHECK(DefaultsFor(probe, runtime).refusal.empty());
    GameRecipe recipe;
    std::string reason;
    CHECK(!GenerateGameRecipe(2231450, "Pizza Tower", {{"", "", "PizzaTower.exe", "", ""}}, root.string(), &recipe, &reason));
    KitCapabilities kit;
    kit.shared_sections = true;
    CHECK(GenerateGameRecipe(2231450, "Pizza Tower", {{"", "", "PizzaTower.exe", "", ""}}, root.string(), &recipe, &reason, kit));
    CHECK(KitCapabilitiesFromManifest({"dxvk", kKitSharedSections}, false).shared_sections);
  }
  {  // Programs the console cannot run are refused with a reason.
    const auto root = fresh();
    WritePe(root / "Stub.exe", {false, {"d3d9.dll"}, false, true});
    WritePe(root / "Net.exe", {false, {"mscoree.dll"}});
    WritePe(root / "Clr.exe", {true, {"KERNEL32.dll"}, true});
    WritePe(root / "Twelve.exe", {true, {"d3d12.dll"}});
    WritePe(root / "TwelveDxgi.exe", {true, {"dxgi.dll", "d3d12.dll"}});
    CHECK(DefaultsFor(ProbeGame(root.string(), "Stub.exe")).refusal.find("Steam DRM") != std::string::npos);
    CHECK(DefaultsFor(ProbeGame(root.string(), "Net.exe")).refusal.find(".NET") != std::string::npos);
    CHECK(DefaultsFor(ProbeGame(root.string(), "Clr.exe")).refusal.find(".NET") != std::string::npos);
    // A kit with Wine Mono starts .NET programs; the other refusals stay.
    RuntimeAbilities mono;
    mono.wine_mono = true;
    CHECK(DefaultsFor(ProbeGame(root.string(), "Net.exe"), mono).refusal.empty());
    CHECK(DefaultsFor(ProbeGame(root.string(), "Clr.exe"), mono).refusal.empty());
    CHECK(DefaultsFor(ProbeGame(root.string(), "Stub.exe"), mono).refusal.find("Steam DRM") != std::string::npos);
    CHECK(KitCapabilitiesFromManifest({"dxvk", kKitWineMono}, false).wine_mono);
    CHECK(!KitCapabilitiesFromManifest({"dxvk"}, false).wine_mono);
    KitCapabilities kit;
    kit.wine_mono = true;
    CHECK(GameProgramRefusal(root.string(), "Net.exe", kit, false).empty());
    CHECK(GameProgramRefusal(root.string(), "Net.exe", KitCapabilities{}, false).find(".NET") != std::string::npos);
    CHECK(DefaultsFor(ProbeGame(root.string(), "Twelve.exe")).refusal.find("Direct3D 12") != std::string::npos);
    // A kit with vkd3d-proton draws it: DXVK for dxgi, and the Direct3D 12 setting on vkd3d-proton.
    RuntimeAbilities vkd3d;
    vkd3d.vkd3d_proton = true;
    const auto twelve = DefaultsFor(ProbeGame(root.string(), "Twelve.exe"), vkd3d);
    CHECK(twelve.refusal.empty() && twelve.graphics == "dxvk" && twelve.d3d12 == "vkd3d-proton");
    // Direct3D 12 games load dxgi too; that doesn't make them Direct3D 11 games.
    const auto twelve_dxgi = DefaultsFor(ProbeGame(root.string(), "TwelveDxgi.exe"), vkd3d);
    CHECK(twelve_dxgi.refusal.empty() && twelve_dxgi.graphics == "dxvk" && twelve_dxgi.d3d12 == "vkd3d-proton");
    // An older Direct3D next to it is a fallback: the game still asks for Direct3D 12 first.
    WritePe(root / "TwelveNine.exe", {true, {"d3d9.dll", "dxgi.dll", "d3d12.dll"}});
    const auto twelve_nine = DefaultsFor(ProbeGame(root.string(), "TwelveNine.exe"), vkd3d);
    CHECK(twelve_nine.refusal.empty() && twelve_nine.graphics == "dxvk" && twelve_nine.d3d12 == "vkd3d-proton");
    CHECK(DefaultsFor(ProbeGame(root.string(), "TwelveNine.exe")).d3d12.empty());
    KitCapabilities vkd3d_kit;
    vkd3d_kit.vkd3d_proton = true;
    CHECK(GameProgramRefusal(root.string(), "Twelve.exe", vkd3d_kit, false).empty());
    GameRecipe twelve_recipe;
    std::string twelve_reason;
    CHECK(GenerateGameRecipe(1091500, "Twelve", {{"", "", "Twelve.exe", "", ""}}, root.string(), &twelve_recipe,
                             &twelve_reason, vkd3d_kit));
    CHECK(twelve_recipe.graphics == "dxvk" && twelve_recipe.defaults.at("d3d12") == "vkd3d-proton");
    EffectiveSettings twelve_settings;
    SettingsError twelve_error;
    CHECK(BuildEffectiveSettings(twelve_recipe, nullptr, {}, vkd3d_kit, &twelve_settings, &twelve_error));
    CHECK(twelve_settings.vkd3d_proton);
  }
  {  // A SteamStub variant Playden unpacks (BALL x PIT) is no reason to refuse; the recipe is made.
    const auto root = fresh();
    fs::create_directories(root);
    std::ofstream(root / "Balls.exe", std::ios::binary) << steam_stub_fixture::Build();
    WritePe(root / "UnityPlayer.dll", {true, {"KERNEL32.dll"}, false, false, Wide("d3d11.dll")});
    fs::create_directories(root / "Balls_Data");
    const auto probe = ProbeGame(root.string(), "Balls.exe");
    CHECK(probe.steam_stub && probe.steam_stub_supported && probe.engine == "unity");
    CHECK(DefaultsFor(probe).refusal.empty() && DefaultsFor(probe).graphics == "dxvk");
    GameRecipe recipe;
    std::string reason;
    CHECK(GenerateGameRecipe(2062430, "BALL x PIT", {{"", "", "Balls.exe", "", ""}}, root.string(), &recipe, &reason));
    CHECK(recipe.launch_entries.size() == 1 && recipe.launch_entries[0].executable == "Balls.exe");
    // Unsupported options are still refused.
    steam_stub_fixture::Spec unsupported;
    unsupported.flags = 0x100;
    std::ofstream(root / "Balls.exe", std::ios::binary | std::ios::trunc) << steam_stub_fixture::Build(unsupported);
    CHECK(DefaultsFor(ProbeGame(root.string(), "Balls.exe")).refusal.find("can't unpack") != std::string::npos);
  }
  {  // OpenGL and plain Windows programs.
    const auto root = fresh();
    WritePe(root / "Gl.exe", {false, {"OPENGL32.dll"}});
    WritePe(root / "Plain.exe", {false, {"USER32.dll", "ddraw.dll"}});
    CHECK(DefaultsFor(ProbeGame(root.string(), "Gl.exe")).graphics == "opengl");
    const auto plain = ProbeGame(root.string(), "Plain.exe");
    CHECK(plain.apis.count("ddraw") && DefaultsFor(plain).graphics == "auto");
    CHECK(!plain.watcom && DefaultsFor(plain).cpu.empty());
  }
  {  // A 32-bit program linked by Watcom (Fallout) runs on the translator: its runtime reloads FS.
    const auto root = fresh();
    PeSpec fallout{false, {"USER32.dll", "ddraw.dll"}};
    fallout.code_section = "BEGTEXT";
    WritePe(root / "falloutw.exe", fallout);
    const auto probe = ProbeGame(root.string(), "falloutw.exe");
    CHECK(probe.watcom);
    const auto defaults = DefaultsFor(probe);
    CHECK(defaults.cpu == "translator" && defaults.summary.find(",watcom ") != std::string::npos);
    CHECK(DetectedLabel(defaults.summary).empty());
    PeSpec x64{true, {"USER32.dll"}};
    x64.code_section = "BEGTEXT";
    WritePe(root / "w64.exe", x64);
    CHECK(!ProbeGame(root.string(), "w64.exe").watcom && DefaultsFor(ProbeGame(root.string(), "w64.exe")).cpu.empty());
    // NFS3's Modern Patch is linked by Watcom with .code and .data sections; its entry jumps over Watcom's banner.
    PeSpec nfs3{false, {"USER32.dll", "DDRAW.dll"}};
    nfs3.code_section = ".code";
    nfs3.text = std::string(0x10, '\x90') + kWatcomEntry;
    WritePe(root / "nfs3.exe", nfs3);
    const auto modern = ProbeGame(root.string(), "nfs3.exe");
    CHECK(modern.watcom && DefaultsFor(modern).cpu == "translator");
    // The banner elsewhere in a program (a string in its data) says nothing; nor does a 64-bit program's.
    PeSpec quoted{false, {"USER32.dll"}};
    quoted.text = std::string(0x100, '\x90') + kWatcomEntry;
    WritePe(root / "quoted.exe", quoted);
    CHECK(!ProbeGame(root.string(), "quoted.exe").watcom);
    PeSpec banner64{true, {"USER32.dll"}};
    banner64.text = nfs3.text;
    WritePe(root / "b64.exe", banner64);
    CHECK(!ProbeGame(root.string(), "b64.exe").watcom);
    // Only the entry section's file bytes count: a banner past them is in another section or not in the file.
    PeSpec short_raw = nfs3;
    short_raw.code_raw_size = 0x20;
    WritePe(root / "short.exe", short_raw);
    CHECK(!ProbeGame(root.string(), "short.exe").watcom);
  }
  {  // Godot and Source.
    const auto root = fresh();
    WritePe(root / "win/garagerally.exe", {true, {"KERNEL32.dll"}, false, false, "vulkan-1.dll"});
    Touch(root / "win/garagerally.pck");
    const auto godot = DefaultsFor(ProbeGame(root.string(), "win/garagerally.exe"));
    CHECK(godot.graphics == "opengl" && godot.arguments == "--rendering-driver opengl3");
    WritePe(root / "hl2.exe", {false, {"KERNEL32.dll"}});
    WritePe(root / "bin/engine.dll", {false, {"d3d9.dll"}});
    const auto source = DefaultsFor(ProbeGame(root.string(), "hl2.exe"));
    CHECK(source.graphics == "dxvk" && source.resolution_arguments == "-w {w} -h {h}");
  }
  {  // HashLink (Dead Cells): the graphics library is a .hdll beside the program, loaded at run time.
    const auto root = fresh();
    WritePe(root / "deadcells.exe", {true, {"libhl.dll", "KERNEL32.dll"}});
    Touch(root / "libhl.dll");
    Touch(root / "directx.hdll");
    Touch(root / "sdl.hdll");
    const auto hashlink = DefaultsFor(ProbeGame(root.string(), "deadcells.exe"));
    CHECK(hashlink.graphics == "dxvk" && hashlink.summary == "hashlink,d3d11,opengl graphics=dxvk");
    CHECK(DetectedLabel(hashlink.summary) == "HashLink \xC2\xB7 Direct3D 11");
    fs::remove(root / "directx.hdll");
    CHECK(DefaultsFor(ProbeGame(root.string(), "deadcells.exe")).graphics == "opengl");
  }
  {  // Java (Slay the Spire): the Steam entry runs the bundled JRE; LWJGL draws with OpenGL.
    const auto root = fresh();
    WritePe(root / "jre/bin/javaw.exe", {true, {"KERNEL32.dll"}});
    WritePe(root / "SlayTheSpire.exe", {true, {"KERNEL32.dll"}});
    const auto java = DefaultsFor(ProbeGame(root.string(), "jre/bin/javaw.exe"));
    CHECK(java.graphics == "opengl" && DetectedLabel(java.summary) == "Java \xC2\xB7 OpenGL");
    CHECK(DefaultsFor(ProbeGame(root.string(), "SlayTheSpire.exe")).graphics == "opengl");
    WritePe(root / "Native.exe", {true, {"d3d11.dll", "KERNEL32.dll"}});
    CHECK(DefaultsFor(ProbeGame(root.string(), "Native.exe")).graphics == "dxvk");
  }
  {  // XNA (Celeste): a .NET program naming the framework runs on Wine Mono's FNA, which draws with OpenGL.
    const auto root = fresh();
    WritePe(root / "Celeste.exe", {false, {"mscoree.dll"}, true, false, "Microsoft.Xna.Framework.Game"});
    RuntimeAbilities mono;
    mono.wine_mono = true;
    const auto xna = DefaultsFor(ProbeGame(root.string(), "Celeste.exe"), mono);
    CHECK(xna.refusal.empty() && xna.graphics == "opengl" && DetectedLabel(xna.summary) == "XNA \xC2\xB7 OpenGL");
    // FNA builds have no Microsoft.Xna.Framework.Game assembly; FEZ.exe names the graphics namespace and
    // GraphicsDeviceManager.
    WritePe(root / "FEZ.exe", {false, {"mscoree.dll"}, true, false,
                               "Microsoft.Xna.Framework Microsoft.Xna.Framework.Graphics GraphicsDeviceManager"});
    CHECK(ProbeGame(root.string(), "FEZ.exe").engine == "xna");
    // The framework's name alone is not an XNA game: Borderlands 2's 2K launcher reads the pad through XNA's input.
    WritePe(root / "Launcher.exe",
            {false, {"mscoree.dll"}, true, false, "Microsoft.Xna.Framework Microsoft.Xna.Framework.Input"});
    const auto launcher = ProbeGame(root.string(), "Launcher.exe");
    CHECK(launcher.dotnet && launcher.engine.empty());
  }
  {  // LÖVE (Balatro): OpenGL, found through love.dll beside the program.
    const auto root = fresh();
    WritePe(root / "Balatro.exe", {true, {"love.dll", "lua51.dll", "KERNEL32.dll"}});
    WritePe(root / "love.dll", {true, {"SDL2.dll", "KERNEL32.dll"}});
    const auto defaults = DefaultsFor(ProbeGame(root.string(), "Balatro.exe"));
    CHECK(defaults.graphics == "opengl" && defaults.summary == "love graphics=opengl");
    CHECK(DetectedLabel(defaults.summary) == "L\xC3\x96VE \xC2\xB7 OpenGL");
  }
  {  // SIL (FTL): OpenGL, though the program also imports Direct3D 11; known by its settings names.
    const auto root = fresh();
    PeSpec ftl{false, {"OPENGL32.dll", "d3d11.dll", "KERNEL32.dll"}};
    ftl.text = "SIL_WINDOWS_USE_RAWINPUT";
    WritePe(root / "FTLGame.exe", ftl);
    const auto defaults = DefaultsFor(ProbeGame(root.string(), "FTLGame.exe"));
    CHECK(defaults.graphics == "opengl" && defaults.summary.rfind("sil,", 0) == 0);
    CHECK(DetectedLabel(defaults.summary) == "SIL \xC2\xB7 OpenGL");
  }
  {  // 64-bit NW.js (Look Outside, RPG Maker MZ) runs Chromium in one process on DXVK.
    const auto root = fresh();
    WritePe(root / "Game.exe", {true, {"nw.dll", "KERNEL32.dll"}});
    WritePe(root / "nw.dll", {true, {"dxgi.dll", "KERNEL32.dll"}});
    RuntimeAbilities abilities;
    abilities.driver_threads = true;
    const auto defaults = DefaultsFor(ProbeGame(root.string(), "Game.exe"), abilities);
    CHECK(defaults.refusal.empty());
    CHECK(defaults.summary.rfind("nwjs,", 0) == 0 && defaults.graphics == "dxvk");
    CHECK(defaults.arguments.find("--single-process") != std::string::npos);
    CHECK(defaults.arguments.find("--enable-low-end-device-mode") == std::string::npos);
    CHECK(defaults.arguments.find("--js-flags=--max-old-space-size=1024") != std::string::npos);
    // On a kit without it the game would hang in Xbox controller mode, so it stays refused.
    const auto older_kit = DefaultsFor(ProbeGame(root.string(), "Game.exe"));
    CHECK(older_kit.refusal.find("this runtime kit can't start") != std::string::npos && !older_kit.refusal_soft);
    CHECK(KitCapabilitiesFromManifest({"dxvk", kKitDriverThreads}, false).driver_threads);
    CHECK(!KitCapabilitiesFromManifest({"dxvk"}, false).driver_threads);
  }
  {  // 32-bit NW.js starts like 64-bit, on a kit with driver threads and with no "Try anyway". Older builds don't play yet.
    const auto root = fresh();
    WritePe(root / "OMORI.exe", {false, {"nw.dll", "KERNEL32.dll"}});
    WritePe(root / "nw.dll", {false, {"d3d11.dll", "dxgi.dll", "KERNEL32.dll"}});
    RuntimeAbilities abilities;
    abilities.driver_threads = true;
    const auto defaults = DefaultsFor(ProbeGame(root.string(), "OMORI.exe"), abilities);
    CHECK(defaults.refusal.empty() && !defaults.refusal_soft);
    CHECK(defaults.graphics == "dxvk" && defaults.arguments.find("--single-process") != std::string::npos);
    const auto older_kit = DefaultsFor(ProbeGame(root.string(), "OMORI.exe"));
    CHECK(older_kit.refusal.find("this runtime kit can't start") != std::string::npos && !older_kit.refusal_soft);
  }
  {  // A saved recipe set up with older defaults for its engine takes the current ones, and keeps what it is.
    const auto root = fresh();
    WritePe(root / "Game.exe", {true, {"nw.dll", "KERNEL32.dll"}});
    WritePe(root / "nw.dll", {true, {"dxgi.dll", "KERNEL32.dll"}});
    KitCapabilities kit;
    kit.driver_threads = true;
    GameRecipe recipe;
    std::string reason;
    CHECK(GenerateGameRecipe(3373660, "Look Outside", {{"", "", "Game.exe", "", ""}}, root.string(), &recipe, &reason, kit));
    // Changed these arguments? Raise DefaultsRevision("nwjs") too, so games already set up take them.
    CHECK(DefaultsRevision("nwjs") == 2);
    CHECK(recipe.arguments == "--single-process --in-process-gpu --no-sandbox --disable-gpu-sandbox --disable-breakpad "
                              "--js-flags=--max-old-space-size=1024");
    CHECK(recipe.defaults_revision == 2 && !RefreshRecipeDefaults(root.string(), kit, &recipe));
    GameRecipe saved;
    CHECK(ParseGameRecipe(SerializeGameRecipe(recipe), &saved, &reason) && saved.defaults_revision == 2);

    // As a recipe from before the revisions reads: no number, the old arguments.
    auto old = recipe;
    old.defaults_revision = 0;
    old.arguments = "--single-process";
    CHECK(ParseGameRecipe(SerializeGameRecipe(old), &saved, &reason) && saved.defaults_revision == 0);
    CHECK(SerializeGameRecipe(old).find("defaults_revision") == std::string::npos);
    auto missing = old;
    CHECK(!RefreshRecipeDefaults((root / "not-plugged-in").string(), kit, &missing) && missing.arguments == old.arguments);
    CHECK(RefreshRecipeDefaults(root.string(), kit, &old));
    CHECK(old.arguments == recipe.arguments && old.defaults_revision == 2);
    CHECK(old.id == recipe.id && old.version == recipe.version && old.launch_entries.size() == recipe.launch_entries.size());
    CHECK(!RefreshRecipeDefaults(root.string(), kit, &old));

    // A recipe Playden didn't generate, and an engine whose defaults never changed, stay as they are.
    auto shipped = recipe;
    shipped.id = "steam-3373660-shipped-v1";
    shipped.defaults_revision = 0;
    shipped.arguments = "--mine";
    CHECK(!RefreshRecipeDefaults(root.string(), kit, &shipped) && shipped.arguments == "--mine");
    auto unity = recipe;
    unity.detected = "unity,d3d11 graphics=dxvk";
    unity.defaults_revision = 0;
    unity.arguments = "-force-d3d11";
    CHECK(!RefreshRecipeDefaults(root.string(), kit, &unity) && unity.arguments == "-force-d3d11");
    CHECK(DefaultsRevision("unity,d3d11 graphics=dxvk") == 0 && DefaultsRevision("nwjs,nwjs-0.86,d3d11") == 2);
  }
  {  // A Watcom program set up before Watcom got Translator: the next Play adds only that default.
    const auto root = fresh();
    PeSpec fallout{false, {"USER32.dll", "ddraw.dll"}};
    fallout.code_section = "BEGTEXT";
    WritePe(root / "falloutwHR.exe", fallout);
    WritePe(root / "Plain.exe", {false, {"USER32.dll", "ddraw.dll"}});
    KitCapabilities kit;
    GameRecipe recipe;
    std::string reason;
    CHECK(GenerateGameRecipe(38400, "Fallout", {{"", "", "falloutwHR.exe", "", ""}}, root.string(), &recipe, &reason, kit));
    CHECK(recipe.defaults.at("cpu") == "translator" && !RefreshRecipeDefaults(root.string(), kit, &recipe));
    // As it was saved before: no cpu default, no watcom fact, a graphics value of its own.
    auto old = recipe;
    old.defaults.erase("cpu");
    old.detected = "ddraw graphics=auto";
    old.graphics = "dxvk";
    CHECK(WatcomProgram((root / "falloutwHR.exe").string()) && !WatcomProgram((root / "Plain.exe").string()));
    auto missing = old;
    CHECK(!RefreshRecipeDefaults((root / "not-plugged-in").string(), kit, &missing) && !missing.defaults.count("cpu"));
    CHECK(RefreshRecipeDefaults(root.string(), kit, &old));
    CHECK(old.defaults.at("cpu") == "translator" && old.detected == "ddraw,watcom graphics=auto" && old.graphics == "dxvk");
    CHECK(!RefreshRecipeDefaults(root.string(), kit, &old));
    // A Native the recipe already says, a 32-bit program that isn't Watcom's and a 64-bit recipe stay as they are.
    // One linked with other section names (NFS3's Modern Patch), saved before as an unknown engine.
    PeSpec nfs3{false, {"USER32.dll", "DDRAW.dll"}};
    nfs3.code_section = ".code";
    nfs3.text = std::string(0x10, '\x90') + kWatcomEntry;
    WritePe(root / "nfs3.exe", nfs3);
    GameRecipe modern;
    CHECK(GenerateGameRecipe(4026531863u, "NFS3", {{"", "", "nfs3.exe", "", ""}}, root.string(), &modern, &reason, kit));
    CHECK(modern.defaults.at("cpu") == "translator");
    modern.defaults.erase("cpu");
    modern.detected = "unknown graphics=dxvk";
    CHECK(RefreshRecipeDefaults(root.string(), kit, &modern));
    CHECK(modern.defaults.at("cpu") == "translator" && modern.detected == "watcom graphics=dxvk");
    auto native = recipe;
    native.defaults["cpu"] = "native";
    native.detected = "ddraw graphics=auto";
    CHECK(!RefreshRecipeDefaults(root.string(), kit, &native) && native.defaults.at("cpu") == "native");
    auto plain = old;
    plain.defaults.erase("cpu");
    plain.detected = "unknown graphics=auto";
    plain.launch_entries.front().executable = "Plain.exe";
    CHECK(!RefreshRecipeDefaults(root.string(), kit, &plain) && !plain.defaults.count("cpu"));
    auto x64 = plain;
    x64.launch_entries.front().executable = "falloutwHR.exe";
    x64.architecture = "pe64";
    CHECK(!RefreshRecipeDefaults(root.string(), kit, &x64) && !x64.defaults.count("cpu"));
    auto unknown = plain;
    unknown.launch_entries.front().executable = "falloutwHR.exe";
    CHECK(RefreshRecipeDefaults(root.string(), kit, &unknown) && unknown.detected == "watcom graphics=auto");
  }
  {  // node-webkit before NW.js 0.13 (CrossCode's demo): Chromium in the program, nw.pak beside it, no nw.dll.
    const auto root = fresh();
    WritePe(root / "crosscode-beta.exe", {false, {"KERNEL32.dll"}});
    Touch(root / "nw.pak");
    Touch(root / "libGLESv2.dll");
    const auto probe = ProbeGame(root.string(), "crosscode-beta.exe");
    CHECK(probe.engine == "nwjs" && probe.engine_version.empty() && !probe.x64);
    RuntimeAbilities abilities;
    abilities.driver_threads = true;
    CHECK(DefaultsFor(probe, abilities).refusal.empty());
  }
  {  // NW.js's version comes from nw.dll's FileVersion string (Look Outside: 0.94.0) and shows on the settings sheet.
    const auto root = fresh();
    WritePe(root / "Game.exe", {true, {"nw.dll", "KERNEL32.dll"}});
    PeSpec nw{true, {"dxgi.dll", "KERNEL32.dll"}};
    nw.resource = FixedFileInfo(0, 0) + FileVersionString("0.94.0");
    WritePe(root / "nw.dll", nw);
    const auto probe = ProbeGame(root.string(), "Game.exe");
    CHECK(probe.engine == "nwjs" && probe.engine_version == "0.94.0");
    const auto defaults = DefaultsFor(probe);
    CHECK(defaults.summary == "nwjs,nwjs-0.94.0,dxgi graphics=dxvk");
    CHECK(DetectedLabel(defaults.summary) == "NW.js 0.94.0 \xC2\xB7 Direct3D 11");
  }
  {  // Without a FileVersion string, the fixed file version; a build with neither (OMORI's) has no version.
    const auto root = fresh();
    WritePe(root / "Game.exe", {false, {"nw.dll", "KERNEL32.dll"}});
    PeSpec nw{false, {"dxgi.dll", "KERNEL32.dll"}};
    nw.resource = FixedFileInfo(29, 4u << 16);
    WritePe(root / "nw.dll", nw);
    CHECK(ProbeGame(root.string(), "Game.exe").engine_version == "0.29.4");
    nw.resource = FixedFileInfo(0, 0) + FileVersionString("");
    WritePe(root / "nw.dll", nw);
    const auto probe = ProbeGame(root.string(), "Game.exe");
    CHECK(probe.engine == "nwjs" && probe.engine_version.empty());
    CHECK(DefaultsFor(probe).summary == "nwjs,dxgi graphics=dxvk");
    CHECK(DetectedLabel(DefaultsFor(probe).summary) == "NW.js \xC2\xB7 Direct3D 11");
  }
  {  // An Unreal root program is replaced by its Shipping program, which starts in its own folder.
    const auto root = fresh();
    WritePe(root / "Game.exe", {true, {"KERNEL32.dll"}});
    fs::create_directories(root / "Engine/Binaries/Win64");
    WritePe(root / "Game/Binaries/Win64/Game-Win64-Shipping.exe", {true, {"d3d11.dll", "d3d12.dll"}});
    Touch(root / "EasyAntiCheat/settings.json");
    const auto probe = ProbeGame(root.string(), "Game.exe");
    CHECK(probe.engine == "unreal" && probe.unreal_program == "Game/Binaries/Win64/Game-Win64-Shipping.exe");
    CHECK(probe.anticheat && DefaultsFor(probe).arguments == "-dx11");
    GameRecipe recipe;
    std::string reason;
    CHECK(GenerateGameRecipe(9001, "Unreal", {{"", "", "Game.exe", "", "-nosplash"}}, root.string(), &recipe, &reason));
    CHECK(recipe.launch_entries[0].executable == "Game/Binaries/Win64/Game-Win64-Shipping.exe");
    CHECK(recipe.launch_entries[0].working_directory == "Game/Binaries/Win64");
    CHECK(recipe.launch_entries[0].arguments == "-nosplash" && recipe.arguments == "-dx11" && recipe.graphics == "dxvk");
    CHECK(recipe.detected == "unreal,anticheat graphics=dxvk");
    // Unreal 4 stays on Direct3D 11 with vkd3d-proton in the kit.
    RuntimeAbilities vkd3d;
    vkd3d.vkd3d_proton = true;
    CHECK(!probe.unreal5 && DefaultsFor(probe, vkd3d).d3d12.empty());
  }
  {  // Unreal 5 (Nanite's settings in the Shipping program, behind the root launcher) gets vkd3d-proton.
    const auto root = fresh();
    WritePe(root / "Hellraiser.exe", {true, {"KERNEL32.dll"}});
    fs::create_directories(root / "Engine/Binaries/Win64");
    PeSpec shipping{true, {"d3d11.dll", "d3d12.dll"}};
    shipping.text = std::string("r\0.\0N\0a\0n\0i\0t\0e\0.\0", 18);
    WritePe(root / "Hellraiser/Binaries/Win64/Hellraiser-Win64-Shipping.exe", shipping);
    const auto probe = ProbeGame(root.string(), "Hellraiser.exe");
    CHECK(probe.engine == "unreal" && probe.unreal5);
    RuntimeAbilities vkd3d;
    vkd3d.vkd3d_proton = true;
    const auto defaults = DefaultsFor(probe, vkd3d);
    CHECK(defaults.d3d12 == "vkd3d-proton" && defaults.arguments == "-dx11" && defaults.graphics == "dxvk");
    CHECK(defaults.summary == "unreal5,dx12 graphics=dxvk");
    CHECK(DetectedLabel(defaults.summary) == "Unreal Engine 5 \xC2\xB7 Direct3D 12");
    // Without vkd3d-proton it starts on Direct3D 11, as before.
    const auto older = DefaultsFor(probe);
    CHECK(older.d3d12.empty() && older.arguments == "-dx11");
    CHECK(DetectedLabel(older.summary) == "Unreal Engine 5 \xC2\xB7 Direct3D 11");
    // Started as the Shipping program itself (which imports d3d12), the same, label included.
    // A redistributable installer beside the Engine folder is not the game (the Program picker labelled it Unreal 5).
    WritePe(root / "VC_redist.x64.exe", {false, {"KERNEL32.dll"}});
    const auto redist = ProbeGame(root.string(), "VC_redist.x64.exe");
    CHECK(redist.engine.empty() && !redist.unreal5 && redist.unreal_program.empty());
    const auto direct = ProbeGame(root.string(), "Hellraiser/Binaries/Win64/Hellraiser-Win64-Shipping.exe");
    CHECK(direct.unreal5 && DefaultsFor(direct, vkd3d).d3d12 == "vkd3d-proton");
    CHECK(DetectedLabel(DefaultsFor(direct).summary) == "Unreal Engine 5 \xC2\xB7 Direct3D 11");
    CHECK(DetectedLabel(DefaultsFor(direct, vkd3d).summary) == "Unreal Engine 5 \xC2\xB7 Direct3D 12");
    // The generated recipe starts it with -dx12 on a kit with vkd3d-proton.
    KitCapabilities vkd3d_kit;
    vkd3d_kit.vkd3d_proton = true;
    GameRecipe recipe;
    std::string reason;
    CHECK(GenerateGameRecipe(5184670, "Hellraiser", {{"", "", "Hellraiser.exe", "", ""}}, root.string(), &recipe,
                             &reason, vkd3d_kit));
    CHECK(recipe.defaults.at("d3d12") == "vkd3d-proton" && recipe.arguments == "-dx11");
    EffectiveSettings settings;
    SettingsError error;
    CHECK(BuildEffectiveSettings(recipe, nullptr, {}, vkd3d_kit, &settings, &error));
    CHECK(settings.vkd3d_proton && settings.values.at("arguments").value == "-dx12");
  }
  {  // Nanite's name in a game that isn't Unreal, or no Shipping program behind the launcher: no Unreal 5.
    const auto root = fresh();
    PeSpec named{true, {"d3d11.dll"}};
    named.text = std::string("r\0.\0N\0a\0n\0i\0t\0e\0", 16);
    WritePe(root / "Other.exe", named);
    const auto other = ProbeGame(root.string(), "Other.exe");
    CHECK(other.engine.empty() && !other.unreal5 && DefaultsFor(other).summary == "d3d11 graphics=dxvk");
    const auto bare = fresh();
    WritePe(bare / "Game.exe", {true, {"KERNEL32.dll"}});
    fs::create_directories(bare / "Engine/Binaries/Win64");
    const auto launcher = ProbeGame(bare.string(), "Game.exe");
    CHECK(launcher.engine == "unreal" && launcher.unreal_program.empty() && !launcher.unreal5);
  }
  {  // An Unreal program not named *-Shipping.exe (Thomas & Friends: Wonders of Sodor): the only program in
     // <Project>/Binaries/Win64 replaces the root launcher, past Unreal's own tools there.
    const auto root = fresh();
    WritePe(root / "WindowsNoEditor/WondersOfSodor.exe", {true, {"KERNEL32.dll"}});
    fs::create_directories(root / "WindowsNoEditor/Engine/Binaries/Win64");
    WritePe(root / "WindowsNoEditor/TS2Prototype/Binaries/Win64/WondersOfSodor.exe", {true, {"d3d11.dll"}});
    WritePe(root / "WindowsNoEditor/TS2Prototype/Binaries/Win64/CrashReportClient.exe", {true, {"KERNEL32.dll"}});
    WritePe(root / "WindowsNoEditor/TS2Prototype/Binaries/Win64/TS2Prototype-Cmd.exe", {true, {"KERNEL32.dll"}});
    const auto probe = ProbeGame(root.string(), "WindowsNoEditor/WondersOfSodor.exe");
    CHECK(probe.engine == "unreal" &&
          probe.unreal_program == "WindowsNoEditor/TS2Prototype/Binaries/Win64/WondersOfSodor.exe");
    CHECK(UnrealGameProgram(root.string(), "WindowsNoEditor/WondersOfSodor.exe") == probe.unreal_program);
    GameRecipe recipe;
    std::string reason;
    CHECK(GenerateGameRecipe(9002, "Sodor", {{"", "", "WindowsNoEditor/WondersOfSodor.exe", "", ""}}, root.string(),
                             &recipe, &reason));
    CHECK(recipe.launch_entries[0].executable == "WindowsNoEditor/TS2Prototype/Binaries/Win64/WondersOfSodor.exe");
    CHECK(recipe.launch_entries[0].working_directory == "WindowsNoEditor/TS2Prototype/Binaries/Win64");
    // Two programs: the one named like the project.
    WritePe(root / "WindowsNoEditor/TS2Prototype/Binaries/Win64/Editor.exe", {true, {"KERNEL32.dll"}});
    WritePe(root / "WindowsNoEditor/TS2Prototype/Binaries/Win64/TS2Prototype.exe", {true, {"KERNEL32.dll"}});
    fs::remove(root / "WindowsNoEditor/TS2Prototype/Binaries/Win64/WondersOfSodor.exe");
    CHECK(UnrealGameProgram(root.string(), "WindowsNoEditor/WondersOfSodor.exe") ==
          "WindowsNoEditor/TS2Prototype/Binaries/Win64/TS2Prototype.exe");
    // Two programs named like neither: none is guessed, the launcher stays.
    fs::remove(root / "WindowsNoEditor/TS2Prototype/Binaries/Win64/TS2Prototype.exe");
    WritePe(root / "WindowsNoEditor/TS2Prototype/Binaries/Win64/Other.exe", {true, {"KERNEL32.dll"}});
    CHECK(UnrealGameProgram(root.string(), "WindowsNoEditor/WondersOfSodor.exe").empty());
    // A Shipping program anywhere wins over a program named otherwise.
    WritePe(root / "WindowsNoEditor/Other/Binaries/Win64/Other-Win64-Shipping.exe", {true, {"KERNEL32.dll"}});
    CHECK(UnrealGameProgram(root.string(), "WindowsNoEditor/WondersOfSodor.exe") ==
          "WindowsNoEditor/Other/Binaries/Win64/Other-Win64-Shipping.exe");
    // The Shipping program itself and an installer beside Engine are not launchers.
    CHECK(UnrealGameProgram(root.string(), "WindowsNoEditor/Other/Binaries/Win64/Other-Win64-Shipping.exe").empty());
    WritePe(root / "WindowsNoEditor/UE4PrereqSetup_x64.exe", {true, {"KERNEL32.dll"}});
    CHECK(UnrealGameProgram(root.string(), "WindowsNoEditor/UE4PrereqSetup_x64.exe").empty());
  }
  {  // SUPERHOT (#755): Steam starts SUPERHOT.exe, a bootstrap beside UnityPlayer.dll that starts the launcher as a child
     // process; the console refuses that and the bootstrap stops. The player is SH.exe, the one with a <stem>_Data folder.
    const auto root = fresh();
    // A 32-bit .NET bootstrap with SteamStub (both refused when it is the program) in front of a 64-bit native player:
    // the player's own files decide.
    WritePe(root / "SUPERHOT.exe", {false, {"mscoree.dll"}, true, true});
    WritePe(root / "SH.exe", {true, {"UnityPlayer.dll", "KERNEL32.dll"}});
    WritePe(root / "UnityPlayer.dll", {true, {"KERNEL32.dll"}, false, false, Wide("d3d11.dll")});
    WritePe(root / "UnityCrashHandler64.exe", {true, {"KERNEL32.dll"}});
    fs::create_directories(root / "SH_Data");
    const auto probe = ProbeGame(root.string(), "SUPERHOT.exe");
    CHECK(probe.engine == "unity" && probe.unity_program == "SH.exe" && probe.unreal_program.empty());
    CHECK(UnityGameProgram(root.string(), "SUPERHOT.exe") == "SH.exe");
    CHECK(GameProgramInstead(root.string(), "SUPERHOT.exe") == "SH.exe");
    GameRecipe recipe;
    std::string reason;
    CHECK(GenerateGameRecipe(322500, "SUPERHOT", {{"", "", "SUPERHOT.exe", "", ""}}, root.string(), &recipe, &reason));
    CHECK(recipe.launch_entries[0].executable == "SH.exe" && recipe.graphics == "dxvk" && recipe.architecture == "pe64");
    CHECK(recipe.arguments.find("-force-d3d11") != std::string::npos);
    // The player itself, and a program with no UnityPlayer.dll beside it, are started as they are.
    CHECK(ProbeGame(root.string(), "SH.exe").unity_program.empty());
    CHECK(UnityGameProgram(root.string(), "SH.exe").empty());
    // A recipe generated before this (SUPERHOT.exe, current defaults) starts the player from the next Play on.
    GameRecipe old = recipe;
    old.launch_entries[0].executable = "SUPERHOT.exe";
    old.architecture = "pe32";
    KitCapabilities kit;
    CHECK(RefreshRecipeDefaults(root.string(), kit, &old));
    CHECK(old.launch_entries[0].executable == "SH.exe" && old.launch_entries[0].working_directory.empty() &&
          old.arguments == recipe.arguments && old.architecture == "pe64");
    CHECK(!RefreshRecipeDefaults(root.string(), kit, &old));  // nothing more to do
    // A program the player chose is kept as chosen.
    GameRecipe picked = recipe;
    picked.launch_entries[0].id = kProgramEntryId;
    picked.launch_entries[0].executable = "SUPERHOT.exe";
    CHECK(!RefreshRecipeDefaults(root.string(), kit, &picked) && picked.launch_entries[0].executable == "SUPERHOT.exe");
    // A program with a <name>_Data folder that doesn't load UnityPlayer.dll is not a player.
    WritePe(root / "Editor.exe", {true, {"KERNEL32.dll"}});
    fs::create_directories(root / "Editor_Data");
    CHECK(UnityGameProgram(root.string(), "SUPERHOT.exe") == "SH.exe");
    // Two players: none is guessed.
    WritePe(root / "Other.exe", {true, {"UnityPlayer.dll"}});
    fs::create_directories(root / "Other_Data");
    CHECK(UnityGameProgram(root.string(), "SUPERHOT.exe").empty());
    // In a subfolder, the path keeps it.
    const auto nested = fresh();
    WritePe(nested / "Game/Start.exe", {true, {"KERNEL32.dll"}});
    WritePe(nested / "Game/Player.exe", {true, {"UnityPlayer.dll"}});
    WritePe(nested / "Game/UnityPlayer.dll", {true, {"KERNEL32.dll"}});
    fs::create_directories(nested / "Game/Player_Data");
    CHECK(UnityGameProgram(nested.string(), "Game/Start.exe") == "Game/Player.exe");
    WritePe(nested / "Lonely.exe", {true, {"KERNEL32.dll"}});
    CHECK(UnityGameProgram(nested.string(), "Lonely.exe").empty());
  }
  {  // Satisfactory (Epic): the root FactoryGameEGS.exe starts FactoryGame/Binaries/Win64/*-Shipping.exe.
    const auto root = fresh();
    WritePe(root / "FactoryGameEGS.exe", {true, {"KERNEL32.dll"}});
    WritePe(root / "Engine/Binaries/Win64/CrashReportClient.exe", {true, {"KERNEL32.dll"}});
    WritePe(root / "FactoryGame/Binaries/Win64/FactoryGameEGS-Win64-Shipping.exe", {true, {"d3d11.dll", "d3d12.dll"}});
    CHECK(UnrealGameProgram(root.string(), "FactoryGameEGS.exe") ==
          "FactoryGame/Binaries/Win64/FactoryGameEGS-Win64-Shipping.exe");
    // Unreal 3 keeps its own program in Binaries/Win64 at the root: it is not a project folder.
    const auto udk = fresh();
    WritePe(udk / "Launcher.exe", {true, {"KERNEL32.dll"}});
    fs::create_directories(udk / "Engine");
    WritePe(udk / "Binaries/Win64/UDK.exe", {true, {"KERNEL32.dll"}});
    CHECK(UnrealGameProgram(udk.string(), "Launcher.exe").empty());
    // The same under a folder of its own: that folder is not a project either.
    const auto wrapped = fresh();
    WritePe(wrapped / "Package/Launcher.exe", {true, {"KERNEL32.dll"}});
    fs::create_directories(wrapped / "Package/Engine");
    WritePe(wrapped / "Package/Binaries/Win64/UDK.exe", {true, {"KERNEL32.dll"}});
    CHECK(UnrealGameProgram(wrapped.string(), "Package/Launcher.exe").empty());
    // A Unity game beside an Engine folder is Unity's, as ProbeGame decides.
    const auto unity = fresh();
    WritePe(unity / "Game.exe", {true, {"KERNEL32.dll"}});
    fs::create_directories(unity / "Game_Data");
    fs::create_directories(unity / "Engine");
    WritePe(unity / "Bonus/Binaries/Win64/Bonus-Win64-Shipping.exe", {true, {"KERNEL32.dll"}});
    CHECK(UnrealGameProgram(unity.string(), "Game.exe").empty() && ProbeGame(unity.string(), "Game.exe").engine == "unity");
  }
  {  // Generated recipes: DXVK through overrides that follow the graphics value; graphics is open.
    const auto root = fresh();
    WritePe(root / "Balls.exe", {true, {"UnityPlayer.dll"}});
    WritePe(root / "UnityPlayer.dll", {true, {}, false, false, "d3d11.dll"});
    GameRecipe recipe;
    std::string reason;
    CHECK(GenerateGameRecipe(2062430, "BALL x PIT", {{"", "", "Balls.exe", "", ""}}, root.string(), &recipe, &reason));
    CHECK(recipe.graphics == "dxvk" && recipe.dll_overrides.empty() && recipe.locked.empty());
    GameRecipe reread;
    CHECK(ParseGameRecipe(SerializeGameRecipe(recipe), &reread, &reason) && reread.detected == recipe.detected);
    EffectiveSettings settings;
    SettingsError error;
    CHECK(BuildEffectiveSettings(recipe, nullptr, {{"library_overrides", "winmm=n"}}, KitCapabilities{}, &settings, &error));
    CHECK(!settings.values.at("graphics").locked && settings.graphics == "dxvk");
    CHECK(settings.dll_overrides == std::string(kDxvkOverrides) + ";winmm=n");
    CHECK(settings.arguments == "-force-d3d11 -screen-width 1920 -screen-height 1080");
    CHECK(BuildEffectiveSettings(recipe, nullptr, {{"graphics", "auto"}}, KitCapabilities{}, &settings, &error));
    CHECK(settings.graphics == "auto" && settings.dll_overrides.empty());
    // Steam features: emulation by default here, and the player may choose the game's own files.
    CHECK(settings.values.at("steam_features").value == "emulate" && !settings.values.at("steam_features").locked);
    CHECK(settings.values.at("steam_input").value == "auto" && settings.values.at("steam_input").available);
    CHECK(BuildEffectiveSettings(recipe, nullptr, {{"steam_input", "off"}}, KitCapabilities{}, &settings, &error));
    CHECK(settings.values.at("steam_input").value == "off");
    CHECK(BuildEffectiveSettings(recipe, nullptr, {{"steam_features", "game"}, {"steam_input", "on"}}, KitCapabilities{},
                                 &settings, &error));
    CHECK(settings.values.at("steam_features").value == "game");
    // Steam Input comes from the emulator: with the game's own files the row is off and back to Automatic.
    CHECK(!settings.values.at("steam_input").available && settings.values.at("steam_input").value == "auto");
    // A shipped recipe that says nothing about Steam features keeps the game's own library, whatever the player asks.
    auto shipped = recipe;
    shipped.id = "steam-2062430-shipped-v1";
    shipped.defaults.erase("steam_features");
    CHECK(BuildEffectiveSettings(shipped, nullptr, {{"steam_features", "emulate"}}, KitCapabilities{}, &settings, &error));
    CHECK(settings.values.at("steam_features").value == "game" && settings.values.at("steam_features").locked);
    // A shipped recipe that names its Steam features (Final Fantasy XIII: emulation) is followed, and
    // the player may still choose the game's own files unless the recipe locks the row.
    auto emulated = shipped;
    emulated.defaults["steam_features"] = "emulate";
    CHECK(BuildEffectiveSettings(emulated, nullptr, {}, KitCapabilities{}, &settings, &error));
    CHECK(settings.values.at("steam_features").value == "emulate" && !settings.values.at("steam_features").locked);
    CHECK(BuildEffectiveSettings(emulated, nullptr, {{"steam_features", "game"}}, KitCapabilities{}, &settings, &error));
    CHECK(settings.values.at("steam_features").value == "game");
    emulated.locked.insert("steam_features");
    CHECK(BuildEffectiveSettings(emulated, nullptr, {{"steam_features", "game"}}, KitCapabilities{}, &settings, &error));
    CHECK(settings.values.at("steam_features").value == "emulate" && settings.values.at("steam_features").locked);
    // Every program refused: the first reason is the player's.
    WritePe(root / "Stub.exe", {false, {"d3d9.dll"}, false, true});
    GameRecipeDiagnostics diagnostics;
    CHECK(!GenerateGameRecipe(1, "Stub", {{"", "", "Stub.exe", "", ""}}, root.string(), &recipe, &reason, {}, {}, nullptr, &diagnostics));
    CHECK(diagnostics.steam_stub_program == "Stub.exe" && !diagnostics.steam_stub_detail.empty());
    CHECK(reason.find("Steam DRM") != std::string::npos);
    // The Program list keeps the SteamStub form of every program and names it in the refusal (#320).
    bool listed = false;
    for (const auto& program : ListGamePrograms(root.string())) {
      if (program.path != "Stub.exe") {
        CHECK(!program.steam_stub && program.steam_stub_detail.empty());
        continue;
      }
      listed = true;
      CHECK(program.steam_stub && !program.steam_stub_supported &&
            program.steam_stub_detail == diagnostics.steam_stub_detail);
      CHECK(program.refusal == "This game's program has Steam DRM in a form Playden can't unpack yet (SteamStub " +
                                   diagnostics.steam_stub_detail + ").");
    }
    CHECK(listed);
    // The recipe comes from Steam's other program, and the skipped one is still named (The Saboteur).
    WritePe(root / "VideoSetup.exe", {false, {"KERNEL32.dll", "d3d9.dll"}});
    GameRecipeDiagnostics skipped;
    CHECK(GenerateGameRecipe(24880, "The Saboteur", {{"", "", "Stub.exe", "", ""}, {"", "Video Setup", "VideoSetup.exe", "", ""}},
                             root.string(), &recipe, &reason, {}, {}, nullptr, &skipped));
    CHECK(recipe.launch_entries[0].executable == "VideoSetup.exe");
    CHECK(skipped.steam_stub_program == "Stub.exe" && skipped.steam_stub_detail == diagnostics.steam_stub_detail);
  }

  {  // Final Fantasy XIII keeps its program four folders deep; the picker reaches it, but not
     // a tree that is deeper than any game ships.
    const auto root = fresh();
    Touch(root / "FFXiiiLauncher.exe");
    Touch(root / "white_data/prog/win/bin/ffxiiiimg.exe");
    Touch(root / "a/b/c/d/e/f/g/too-deep.exe");
    const auto programs = ListGamePrograms(root.string());
    CHECK(programs.size() == 2 && programs[0].path == "FFXiiiLauncher.exe" &&
          programs[1].path == "white_data/prog/win/bin/ffxiiiimg.exe");
  }

  {  // Selaco: Steam lists only .bat files around a .NET helper; the game's own program would start.
    const auto root = fresh();
    Touch(root / "Launch.bat");
    Touch(root / "LaunchEditor.bat");
    WritePe(root / "SELACO.exe", {true, {"KERNEL32.dll", "OPENGL32.dll"}});
    WritePe(root / "Tools/Editor.exe", {true, {"KERNEL32.dll"}, true});
    WritePe(root / "_CommonRedist/vcredist/vc_redist.x64.exe", {true, {"KERNEL32.dll"}});
    const std::vector<LaunchEntry> steam = {{"", "Play", "Launch.bat", "", ""}, {"", "Editor", "LaunchEditor.bat", "", ""}};
    GameRecipe recipe;
    std::string reason;
    bool soft = false;
    CHECK(!GenerateGameRecipe(1592280, "Selaco", steam, root.string(), &recipe, &reason, {}, {}, &soft));
    CHECK(reason.find("no Windows program") != std::string::npos && soft);  // choosing a program gets past it
    // The Program picker lists the folder's programs, not the redistributables.
    const auto programs = ListGamePrograms(root.string());
    CHECK(programs.size() == 2 && programs[0].path == "SELACO.exe" && programs[1].path == "Tools/Editor.exe");
    CHECK(programs[0].architecture == "pe64" && programs[0].refusal.empty() && programs[0].detected == "opengl graphics=opengl");
    CHECK(programs[1].refusal.find(".NET") != std::string::npos && programs[1].refusal_soft);
    CHECK(GameProgramArchitecture(root.string(), "selaco.EXE") == "pe64" && GameProgramArchitecture(root.string(), "x.exe").empty());
    // "Don't start" holds for a picked program; Try anyway gets past a soft refusal only.
    CHECK(GameProgramRefusal(root.string(), "Tools/Editor.exe", {}, false).find(".NET") != std::string::npos);
    CHECK(GameProgramRefusal(root.string(), "Tools/Editor.exe", {}, true).empty());
    CHECK(GameProgramRefusal(root.string(), "SELACO.exe", {}, false).empty());
    CHECK(!GameProgramRefusal(root.string(), "Missing.exe", {}, true).empty());
    // The player's program makes the recipe: first, with its architecture and engine defaults.
    RecipeChoices choices;
    choices.program = "SELACO.exe";
    CHECK(GenerateGameRecipe(1592280, "Selaco", steam, root.string(), &recipe, &reason, {}, choices));
    CHECK(recipe.launch_entries.size() == 1 && recipe.launch_entries[0].id == kProgramEntryId &&
          recipe.launch_entries[0].executable == "SELACO.exe" && recipe.architecture == "pe64" && recipe.graphics == "opengl");
    EffectiveSettings settings;
    SettingsError error;
    CHECK(BuildEffectiveSettings(recipe, nullptr, {{"program", "SELACO.exe"}}, KitCapabilities{}, &settings, &error));
    CHECK(settings.launch && settings.launch->id == kProgramEntryId && settings.architecture == "pe64");
    // A .NET program is refused softly: "Try anyway" makes the recipe, with the engine's defaults.
    choices.program = "Tools/Editor.exe";
    CHECK(!GenerateGameRecipe(1592280, "Selaco", steam, root.string(), &recipe, &reason, {}, choices, &soft));
    CHECK(reason.find(".NET") != std::string::npos && soft);
    choices.try_anyway = true;
    CHECK(GenerateGameRecipe(1592280, "Selaco", steam, root.string(), &recipe, &reason, {}, choices));
    CHECK(recipe.launch_entries[0].executable == "Tools/Editor.exe" && recipe.launch_entries[0].working_directory.empty());
    GameSettingsRecord record;
    record.overrides = {{"program", "Tools\\Editor.exe"}, {"start_check", "try"}};
    const auto from_record = RecipeChoicesFrom(record);
    CHECK(from_record.program == "Tools/Editor.exe" && from_record.try_anyway);
    // Hard limits stay refused, even when the player tries anyway.
    WritePe(root / "Stub.exe", {false, {"d3d9.dll"}, false, true});
    WritePe(root / "Twelve.exe", {true, {"d3d12.dll"}});
    for (const char* program : {"Stub.exe", "Twelve.exe"}) {
      choices.program = program;
      CHECK(!GenerateGameRecipe(1592280, "Selaco", steam, root.string(), &recipe, &reason, {}, choices, &soft));
      CHECK(!soft);
    }
    WritePe(root / "Both.exe", {true, {"mscoree.dll", "d3d12.dll"}});
    CHECK(!DefaultsFor(ProbeGame(root.string(), "Both.exe")).refusal_soft);
    // Steam's entries stay as suggestions after the player's program, numbered as before.
    WritePe(root / "Other.exe", {true, {"KERNEL32.dll", "d3d11.dll"}});
    choices.program = "SELACO.exe";
    CHECK(GenerateGameRecipe(1592280, "Selaco", {{"", "Other", "Other.exe", "", "-x"}}, root.string(), &recipe, &reason, {}, choices));
    CHECK(recipe.launch_entries.size() == 2 && recipe.launch_entries[1].id == "launch-1" &&
          recipe.launch_entries[1].arguments == "-x");
  }

  {  // id Tech 3 (Return to Castle Wolfenstein): OpenGL, with Mesa's extension list cut to 2001.
    const auto root = fresh();
    WritePe(root / "WolfSP.exe", {false, {"KERNEL32.dll"}});
    Touch(root / "Main/pak0.pk3");
    const auto defaults = DefaultsFor(ProbeGame(root.string(), "WolfSP.exe"));
    CHECK(defaults.graphics == "opengl" && defaults.environment == "MESA_EXTENSION_MAX_YEAR=2001");
    CHECK(DetectedLabel(defaults.summary) == "id Tech 3 \xC2\xB7 OpenGL");
    GameRecipe recipe;
    std::string reason;
    CHECK(GenerateGameRecipe(9010, "Wolfenstein", {{"", "", "WolfSP.exe", "", ""}}, root.string(), &recipe, &reason));
    CHECK(recipe.graphics == "opengl" && recipe.defaults.at("environment") == "MESA_EXTENSION_MAX_YEAR=2001");
    EffectiveSettings settings;
    SettingsError error;
    KitCapabilities zink;
    zink.opengl_zink = true;
    CHECK(BuildEffectiveSettings(recipe, nullptr, {}, zink, &settings, &error));
    CHECK(settings.values.at("environment").value == "MESA_EXTENSION_MAX_YEAR=2001");
  }

  {  // Steam's default is a launcher (#70) or a .NET launcher (#46): the game's own program goes first.
    const auto root = fresh();
    WritePe(root / "Launcher.exe", {false, {"KERNEL32.dll", "USER32.dll", "DINPUT8.dll"}});
    WritePe(root / "BulletSoulWin.exe", {false, {"KERNEL32.dll", "d3d9.dll"}});
    WritePe(root / "Tools/Viewer.exe", {false, {"KERNEL32.dll", "d3d9.dll"}});
    WritePe(root / "Mono.exe", {false, {"mscoree.dll", "d3d9.dll"}});
    GameRecipe recipe;
    std::string reason;
    CHECK(GenerateGameRecipe(544580, "BULLET SOUL", {{"", "Play", "Launcher.exe", "", ""}}, root.string(), &recipe, &reason));
    CHECK(recipe.launch_entries.size() == 2 && recipe.launch_entries[0].executable == "BulletSoulWin.exe" &&
          recipe.launch_entries[0].id == "game" && recipe.graphics == "dxvk");
    CHECK(recipe.launch_entries[1].executable == "Launcher.exe" && recipe.launch_entries[1].id == "launch-1");
    // In the launcher's folder first: an updater under bin/ picks the game beside it.
    WritePe(root / "bin/Updater.exe", {false, {"KERNEL32.dll"}});
    WritePe(root / "bin/game_d3d11.exe", {false, {"KERNEL32.dll", "d3d11.dll"}});
    CHECK(GenerateGameRecipe(233270, "Updater", {{"", "", "bin/Updater.exe", "", ""}}, root.string(), &recipe, &reason));
    CHECK(recipe.launch_entries[0].executable == "bin/game_d3d11.exe");
    // A .NET launcher: the native game instead of a refusal.
    WritePe(root / "Hotline.exe", {false, {"mscoree.dll"}, true});
    CHECK(GenerateGameRecipe(219150, "Hotline", {{"", "", "Hotline.exe", "", ""}}, root.string(), &recipe, &reason));
    CHECK(recipe.launch_entries[0].executable == "BulletSoulWin.exe" && recipe.launch_entries.size() == 1);
    {  // A redistributable installer is never the game, even when it imports Direct3D (Antichamber's UE3Redist.exe).
      const auto udk = fresh();
      WritePe(udk / "Binaries/Windows/UE3Redist.exe", {false, {"KERNEL32.dll", "d3d9.dll"}});
      WritePe(udk / "Binaries/Win32/UDK.exe", {false, {"KERNEL32.dll", "d3d9.dll"}});
      CHECK(GenerateGameRecipe(219890, "Antichamber", {{"", "", "Binaries/Windows/UE3Redist.exe", "", ""}}, udk.string(), &recipe, &reason));
      CHECK(recipe.launch_entries[0].executable == "Binaries/Win32/UDK.exe");
    }
    {  // Never a dedicated server or relay, even the largest (Cry of Fear lists only CoFLaunchApp.exe, #318).
      const auto cof = fresh();
      WritePe(cof / "CoFLaunchApp.exe", {false, {"KERNEL32.dll", "USER32.dll"}});
      WritePe(cof / "cof.exe", {false, {"KERNEL32.dll", "DDRAW.dll"}, false, false, "opengl32.dll"});
      WritePe(cof / "hlds.exe", {false, {"KERNEL32.dll", "DDRAW.dll"}, false, false, "opengl32.dll", 0x40000});
      WritePe(cof / "hltv.exe", {false, {"KERNEL32.dll", "DDRAW.dll"}, false, false, "opengl32.dll", 0x30000});
      CHECK(GenerateGameRecipe(223710, "Cry of Fear", {{"", "Launch with Console", "CoFLaunchApp.exe", "", ""}},
                               cof.string(), &recipe, &reason));
      CHECK(recipe.launch_entries.size() == 2 && recipe.launch_entries[0].executable == "cof.exe" &&
            recipe.launch_entries[0].id == "game" && recipe.launch_entries[1].executable == "CoFLaunchApp.exe");
      // A server name only ranks last: a game whose own program is named like one is still found.
      const auto side = fresh();
      WritePe(side / "Launcher.exe", {false, {"KERNEL32.dll", "USER32.dll"}});
      WritePe(side / "ServerSide.exe", {false, {"KERNEL32.dll", "d3d9.dll"}});
      CHECK(GenerateGameRecipe(1, "Server Side", {{"", "", "Launcher.exe", "", ""}}, side.string(), &recipe, &reason));
      CHECK(recipe.launch_entries.size() == 2 && recipe.launch_entries[0].executable == "ServerSide.exe");
    }
    {  // An XNA game's own program is the game, not a .NET launcher: Guncraft starts Guncraft.exe, not the
       // dedicated server beside it (#576).
      const auto guncraft = fresh();
      // Both name the framework and OpenGL ("detected=xna,opengl"); the server is the larger.
      WritePe(guncraft / "Guncraft.exe", {false, {"mscoree.dll"}, true, false, "Microsoft.Xna.Framework.Game opengl32.dll"});
      WritePe(guncraft / "GunCraftServer.exe",
              {false, {"mscoree.dll"}, true, false, "Microsoft.Xna.Framework.Game opengl32.dll", 0x40000});
      KitCapabilities mono;
      mono.wine_mono = true;
      CHECK(GenerateGameRecipe(241720, "Guncraft", {{"", "", "Guncraft.exe", "", ""}}, guncraft.string(), &recipe, &reason,
                               mono));

      CHECK(recipe.launch_entries.size() == 1 && recipe.launch_entries[0].executable == "Guncraft.exe" &&
            recipe.graphics == "opengl");
      // A .NET launcher that draws nothing is still one: the XNA game beside it is started, not its server.
      WritePe(guncraft / "Launcher.exe", {false, {"mscoree.dll"}, true});
      CHECK(GenerateGameRecipe(241720, "Guncraft", {{"", "", "Launcher.exe", "", ""}}, guncraft.string(), &recipe,
                               &reason, mono));
      CHECK(recipe.launch_entries.size() == 2 && recipe.launch_entries[0].executable == "Guncraft.exe" &&
            recipe.launch_entries[0].id == "game");
      // ...even beside a DLL that imports Direct3D: the folder's graphics libraries are not the launcher's.
      WritePe(guncraft / "Render.dll", {false, {"KERNEL32.dll", "d3d9.dll"}});
      CHECK(GenerateGameRecipe(241720, "Guncraft", {{"", "", "Launcher.exe", "", ""}}, guncraft.string(), &recipe,
                               &reason, mono));
      CHECK(recipe.launch_entries.size() == 2 && recipe.launch_entries[0].executable == "Guncraft.exe");
      // The swap is logged with why, and the game page names the program that starts until the player picks another.
      GameRecipeDiagnostics swap;
      CHECK(GenerateGameRecipe(241720, "Guncraft", {{"", "", "Launcher.exe", "", ""}}, guncraft.string(), &recipe,
                               &reason, mono, {}, nullptr, &swap));
      CHECK(swap.steam_default == "Launcher.exe" && swap.steam_default_skipped_because == "dotnet-launcher" &&
            swap.game_instead == "Guncraft.exe");
      CHECK(ProgramInsteadOfSteamDefault(recipe, nullptr) == std::optional<std::string>("Guncraft.exe"));
      CHECK(ProgramInsteadOfSteamDefault(recipe, &recipe.launch_entries[0]).has_value());
      CHECK(!ProgramInsteadOfSteamDefault(recipe, &recipe.launch_entries[1]));
      GameRecipeDiagnostics none;
      CHECK(GenerateGameRecipe(241720, "Guncraft", {{"", "", "Guncraft.exe", "", ""}}, guncraft.string(), &recipe,
                               &reason, mono, {}, nullptr, &none));
      CHECK(none.steam_default.empty() && !ProgramInsteadOfSteamDefault(recipe, nullptr));
    }
    {  // Borderlands 2: Steam's only program is the 2K launcher, a .NET program that uses XNA for the pad and nothing
       // else. It is a launcher: the game's own program starts, on DXVK, not the launcher on OpenGL.
      const auto bl2 = fresh();
      WritePe(bl2 / "Binaries/Win32/Launcher.exe",
              {false, {"mscoree.dll"}, true, false, "Microsoft.Xna.Framework Microsoft.Xna.Framework.Input"});
      WritePe(bl2 / "Binaries/Win32/Microsoft.Xna.Framework.dll", {false, {"mscoree.dll"}, true});
      WritePe(bl2 / "Binaries/Win32/Borderlands2.exe", {false, {"KERNEL32.dll", "d3d9.dll"}, false, false, "", 0x40000});
      KitCapabilities mono;
      mono.wine_mono = true;
      GameRecipeDiagnostics swap;
      CHECK(GenerateGameRecipe(49520, "Borderlands 2", {{"", "", "Binaries/Win32/Launcher.exe", "", ""}}, bl2.string(),
                               &recipe, &reason, mono, {}, nullptr, &swap));
      CHECK(recipe.launch_entries.size() == 2 && recipe.launch_entries[0].executable == "Binaries/Win32/Borderlands2.exe" &&
            recipe.launch_entries[0].id == "game" && recipe.launch_entries[1].executable == "Binaries/Win32/Launcher.exe");
      CHECK(recipe.graphics == "dxvk" && recipe.architecture == "pe32");
      CHECK(swap.steam_default_skipped_because == "dotnet-launcher" &&
            swap.game_instead == "Binaries/Win32/Borderlands2.exe");

      // A copy set up while the launcher counted as an XNA game starts the launcher on OpenGL. Its recipe is made
      // again at the next Play, as this setup made it.
      auto old = recipe;
      old.launch_entries = {recipe.launch_entries[1]};
      old.launch_entries[0].id = "launch-1";
      old.graphics = "opengl";
      old.arguments.clear();
      old.detected = "xna graphics=opengl";
      old.defaults_revision = 0;
      CHECK(DefaultsRevision("xna") == 1);
      CHECK(RefreshRecipeDefaults(bl2.string(), mono, &old));
      CHECK(old.id == recipe.id && old.launch_entries.size() == 2 &&
            old.launch_entries[0].executable == "Binaries/Win32/Borderlands2.exe" && old.launch_entries[0].id == "game" &&
            old.launch_entries[1].executable == "Binaries/Win32/Launcher.exe" && old.graphics == "dxvk" &&
            old.detected == recipe.detected);
      CHECK(!RefreshRecipeDefaults(bl2.string(), mono, &old));
      // Steam's entries keep their ids (a saved choice names them) and folders; the game's program goes before them.
      auto numbered = old;
      numbered.launch_entries = {recipe.launch_entries[1], recipe.launch_entries[1]};
      numbered.launch_entries[0].id = "launch-1";
      numbered.launch_entries[0].in_install_root = false;
      numbered.launch_entries[1].id = "launch-3";
      numbered.launch_entries[1].arguments = "-safe";
      numbered.launch_entries[1].working_directory = "Binaries";
      numbered.graphics = "opengl";
      numbered.detected = "xna graphics=opengl";
      numbered.defaults_revision = 0;
      CHECK(RefreshRecipeDefaults(bl2.string(), mono, &numbered));
      CHECK(numbered.launch_entries.size() == 3 && numbered.launch_entries[0].id == "game" &&
            numbered.launch_entries[1].id == "launch-1" && !numbered.launch_entries[1].in_install_root &&
            numbered.launch_entries[2].id == "launch-3" && numbered.launch_entries[2].arguments == "-safe" &&
            numbered.launch_entries[2].working_directory == "Binaries" && numbered.graphics == "dxvk");
      // A program the player chose is kept with what it was set up with, and isn't read again at each Play.
      auto chosen = recipe;
      chosen.launch_entries = {recipe.launch_entries[1]};
      chosen.launch_entries[0].id = kProgramEntryId;
      chosen.graphics = "opengl";
      chosen.detected = "xna graphics=opengl";
      chosen.defaults_revision = 0;
      CHECK(RefreshRecipeDefaults(bl2.string(), mono, &chosen));
      CHECK(chosen.launch_entries.size() == 1 && chosen.launch_entries[0].executable == "Binaries/Win32/Launcher.exe" &&
            chosen.graphics == "opengl" && chosen.detected == "xna graphics=opengl" && chosen.defaults_revision == 1);
      CHECK(!RefreshRecipeDefaults(bl2.string(), mono, &chosen));
      // An XNA game set up before keeps its program and takes the new revision once.
      WritePe(bl2 / "Celeste.exe", {false, {"mscoree.dll"}, true, false, "Microsoft.Xna.Framework.Game"});
      CHECK(GenerateGameRecipe(504230, "Celeste", {{"", "", "Celeste.exe", "", ""}}, bl2.string(), &recipe, &reason, mono));
      CHECK(recipe.defaults_revision == 1);
      auto celeste = recipe;
      celeste.defaults_revision = 0;
      CHECK(RefreshRecipeDefaults(bl2.string(), mono, &celeste));
      CHECK(celeste.launch_entries.size() == 1 && celeste.launch_entries[0].executable == "Celeste.exe" &&
            celeste.graphics == "opengl" && celeste.defaults_revision == 1);
      CHECK(!RefreshRecipeDefaults(bl2.string(), mono, &celeste));
    }
    // A program that draws, or the player's choice, is started as listed.
    CHECK(GenerateGameRecipe(1, "Game", {{"", "", "Tools/Viewer.exe", "", ""}}, root.string(), &recipe, &reason));
    CHECK(recipe.launch_entries.size() == 1 && recipe.launch_entries[0].executable == "Tools/Viewer.exe");
    RecipeChoices choices;
    choices.program = "Launcher.exe";
    CHECK(GenerateGameRecipe(544580, "BULLET SOUL", {{"", "", "Launcher.exe", "", ""}}, root.string(), &recipe, &reason, {}, choices));
    CHECK(recipe.launch_entries[0].executable == "Launcher.exe" && recipe.launch_entries[0].id == kProgramEntryId);
  }

  {  // MFC: a Unity plugin deep in the game's data imports mfc140u.dll (Moonlighter); per architecture.
    const auto root = fresh();
    WritePe(root / "Moonlighter.exe", {true, {"UnityPlayer.dll", "KERNEL32.dll"}});
    WritePe(root / "Moonlighter_Data/Plugins/UnityNativeChromaSDK.dll", {true, {"KERNEL32.dll", "MFC140U.dll"}});
    fs::create_directories(root / "Moonlighter_Data/Managed");
    auto found = FindMfcImports(root.string());
    CHECK(found.x64_program == "Moonlighter_Data/Plugins/UnityNativeChromaSDK.dll" && found.x86_program.empty());
    CHECK(found.programs == 2 && !found.truncated);
    // A 32-bit helper that delay-loads mfcm140.dll counts for x86; other DLLs named alike don't.
    WritePe(root / "Tools/Editor.exe", {false, {"KERNEL32.dll"}, false, false, "", 0x1400, false, {"mfcm140.dll"}});
    WritePe(root / "Tools/Other.dll", {false, {"mfc42.dll", "mfc140x.lib", "msvcp140.dll"}});
    found = FindMfcImports(root.string());
    CHECK(found.x86_program == "Tools/Editor.exe" && !found.x64_program.empty());
    // PhysX's loader, imported or delay-loaded (a game may not load it until it needs it); a bare engine DLL doesn't count.
    WritePe(root / "Physx/Direct.exe", {false, {"KERNEL32.dll", "PhysXLoader.dll"}});
    WritePe(root / "Physx/Delayed.exe", {false, {"KERNEL32.dll"}, false, false, "", 0x1400, false, {"PhysXLoader.dll"}});
    WritePe(root / "Physx/None.exe", {false, {"KERNEL32.dll", "PhysXDevice.dll"}});
    CHECK(ProgramUsesPhysx(root.string(), "Physx/Direct.exe") && ProgramUsesPhysx(root.string(), "Physx/Delayed.exe"));
    CHECK(!ProgramUsesPhysx(root.string(), "Physx/None.exe") && !ProgramUsesPhysx(root.string(), "Physx/Missing.exe"));
    // Neither: a game without MFC.
    const auto plain = fresh();
    WritePe(plain / "Game.exe", {true, {"KERNEL32.dll", "VCRUNTIME140.dll", "d3d11.dll"}});
    found = FindMfcImports(plain.string());
    CHECK(found.x86_program.empty() && found.x64_program.empty() && found.programs == 1);
    // Too deep is not looked into; a link is not followed.
    WritePe(plain / "a/b/c/d/e/f/g/Deep.dll", {true, {"mfc140.dll"}});
    fs::create_directory_symlink(root, plain / "linked");
    found = FindMfcImports(plain.string());
    CHECK(found.x86_program.empty() && found.x64_program.empty());
    WritePe(plain / "a/b/c/d/e/f/Six.dll", {true, {"mfc140.dll"}});
    CHECK(FindMfcImports(plain.string()).x64_program == "a/b/c/d/e/f/Six.dll");
    // Once both are found the walk ends: the folder after them isn't listed.
    const auto both = fresh();
    WritePe(both / "A.dll", {true, {"mfc140u.dll"}});
    WritePe(both / "B.dll", {false, {"mfc140u.dll"}});
    WritePe(both / "later/C.dll", {true, {"KERNEL32.dll"}});
    found = FindMfcImports(both.string());
    CHECK(!found.x86_program.empty() && !found.x64_program.empty() && found.folders == 1 && found.programs == 2);
    CHECK(!found.truncated);
  }

  {  // A program's own imports decide, not the libraries beside it: Hotline Miami's launcher sits next to cgGL.dll.
    const auto root = fresh();
    WritePe(root / "GL.exe", {false, {"KERNEL32.dll", "OPENGL32.dll"}});
    WritePe(root / "Both.exe", {false, {"OPENGL32.dll", "d3d9.dll"}});
    WritePe(root / "Launcher.exe", {false, {"KERNEL32.dll"}});
    WritePe(root / "cgGL.dll", {false, {"OPENGL32.dll"}});
    CHECK(ProgramDrawsWithOpenGLOnly(root.string(), "GL.exe"));
    CHECK(!ProgramDrawsWithOpenGLOnly(root.string(), "Both.exe"));
    CHECK(!ProgramDrawsWithOpenGLOnly(root.string(), "Launcher.exe"));
    CHECK(!ProgramDrawsWithOpenGLOnly(root.string(), "Missing.exe"));
  }

  {  // A run's note: a program in the game's folder that draws, once; a helper or a path elsewhere is nothing.
    const auto root = fresh();
    const auto prefix = root / "prefix";
    fs::create_directories(prefix);
    WritePe(root / "game/bin/Game.exe", {false, {"KERNEL32.dll", "d3d11.dll"}});
    WritePe(root / "game/Reporter.exe", {false, {"KERNEL32.dll"}});
    const auto note = [&](const std::string& image) {
      std::ofstream(prefix / ".pw-exec-refused", std::ios::binary) << "pw-exec-refused 1\nimage=" << image << "\n";
    };
    const auto take = [&] { return TakeRefusedGameProgram(prefix.string(), "/data/games/1", (root / "game").string()); };
    CHECK(take().empty());
    note("Z:\\data\\games\\1\\bin\\Game.exe");
    CHECK(take() == "bin/Game.exe");
    // A helper beside the game's libraries is still a helper; a program that names its engine is the game.
    WritePe(root / "game/d3d.dll", {false, {"d3d11.dll"}});
    note("Z:\\data\\games\\1\\Reporter.exe");
    CHECK(take().empty());
    WritePe(root / "game/UnityGame.exe", {false, {"UnityPlayer.dll"}});
    note("Z:\\data\\games\\1\\UnityGame.exe");
    CHECK(take() == "UnityGame.exe");
    note("Z:\\data\\games\\1\\\\bin\\Game.exe");
    CHECK(take().empty());
    // Graphics named inside the program count; a folder's name doesn't make a helper the game.
    WritePe(root / "game/Risk.exe", {true, {"KERNEL32.dll"}, false, false, "D3D11.DLL"});
    note("Z:\\data\\games\\1\\Risk.exe");
    CHECK(take() == "Risk.exe");
    WritePe(root / "game/Game-Shipping/Reporter.exe", {false, {"KERNEL32.dll"}});
    note("Z:\\data\\games\\1\\Game-Shipping\\Reporter.exe");
    CHECK(take().empty());
    WritePe(root / "game/Game-Win64-Shipping.exe", {false, {"KERNEL32.dll"}});
    note("Z:\\data\\games\\1\\Game-Win64-Shipping.exe");
    CHECK(take() == "Game-Win64-Shipping.exe");
    // A title with [steam] shows the game in steamapps\common through links in the bottle: a program named there, in
    // any case and by any of the links' names, counts; with no link there, it doesn't.
    note("C:\\Program Files (x86)\\Steam\\steamapps\\common\\The Game\\bin\\Game.exe");
    CHECK(take().empty());
    const auto common = prefix / "drive_c/Program Files (x86)/Steam/steamapps/common";
    fs::create_directories(common);
    std::ofstream(common / ".pw-symlinks", std::ios::binary) << "1\t/data/games/1\nThe Game\t/data/games/1\n";
    note("C:\\Program Files (x86)\\Steam\\steamapps\\common\\The Game\\bin\\Game.exe");
    CHECK(take() == "bin/Game.exe");
    note("c:\\program files (x86)\\steam\\steamapps\\common\\the game\\bin\\Game.exe");
    CHECK(take() == "bin/Game.exe");
    note("C:\\Program Files (x86)\\Steam\\steamapps\\common\\1\\bin\\Game.exe");
    CHECK(take() == "bin/Game.exe");
    note("Z:\\data\\games\\1\\bin\\Game.exe");
    CHECK(take() == "bin/Game.exe");
    // Another folder there, or the library folder itself: nothing.
    note("C:\\Program Files (x86)\\Steam\\steamapps\\common\\Other\\bin\\Game.exe");
    CHECK(take().empty());
    note("C:\\Program Files (x86)\\Steam\\steamapps\\common\\The Game\\");
    CHECK(take().empty());
    fs::remove_all(prefix / "drive_c");
    // A program that can't be read keeps the note for a later start.
    { std::ofstream(root / "game/Broken.exe", std::ios::binary) << "x"; }
    note("Z:\\data\\games\\1\\Broken.exe");
    CHECK(take().empty() && fs::exists(prefix / ".pw-exec-refused"));
    fs::remove(prefix / ".pw-exec-refused");
    CHECK(!fs::exists(prefix / ".pw-exec-refused") && take().empty());
    note("Z:\\data\\games\\1\\Reporter.exe");
    CHECK(take().empty());
    note("Z:\\data\\games\\12\\bin\\Game.exe");
    CHECK(take().empty());
    note("Z:\\data\\games\\1\\..\\1\\bin\\Game.exe");
    CHECK(take().empty());
    note("C:\\data\\games\\1\\bin\\Game.exe");
    CHECK(take().empty());
    note("Z:\\data\\games\\1\\bin\\Missing.exe");
    CHECK(take().empty());
    std::ofstream(prefix / ".pw-exec-refused", std::ios::binary) << "something else\n";
    CHECK(take().empty() && !fs::exists(prefix / ".pw-exec-refused"));
  }

  // The player's words for what was found.
  CHECK(DetectedLabel("unity-il2cpp,d3d11,d3d12 graphics=dxvk") == "Unity \xC2\xB7 Direct3D 11");
  CHECK(DetectedLabel("d3d9 graphics=dxvk") == "Direct3D 9");
  CHECK(DetectedLabel("d3d12,d3d9,dxgi graphics=dxvk") == "Direct3D 12");
  CHECK(DetectedLabel("d3d11,d3d12,dxgi,vulkan graphics=dxvk") == "Direct3D 11 and 12");
  CHECK(DetectedLabel("godot,opengl,vulkan graphics=opengl") == "Godot \xC2\xB7 OpenGL");
  CHECK(DetectedLabel("unknown graphics=auto").empty() && DetectedLabel("").empty());

  fs::remove_all(base);
  std::puts("game probe tests passed");
}
