#pragma once

#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>

namespace playden::runtime {

// What a Windows game's files say about how to start it on the console. Read only and
// bounded: symbolic links are never followed, folders are walked a few levels deep and at
// most kProbeScanBytes of a program are scanned.
struct GameProbe {
  std::string engine;  // "unity", "unreal", "source", "gamemaker", "godot", "love", "nwjs", "sil", "idtech3",
                       // "hashlink", "xna", or empty
  // NW.js: nw.dll's version ("0.94.0"), empty when its version resource carries none (2017 builds such as OMORI's)
  // and for node-webkit before NW.js 0.13 (no nw.dll). Logs and the settings sheet show it.
  std::string engine_version;
  bool x64 = false;     // the program is PE32+ (x64)
  bool il2cpp = false;  // Unity with GameAssembly.dll
  bool legacy_unity = false;  // Unity before 2017.2: the player is built into the program
  int unity_major = 0;  // for legacy_unity, the Unity version from the data folder (4, 5); 0 when unknown
  // Graphics libraries the program or its engine imports or names: "d3d8", "d3d9", "d3d10",
  // "d3d11", "dxgi", "d3d12", "opengl", "vulkan", "ddraw".
  std::set<std::string> apis;
  bool dotnet = false;      // a .NET program (needs Wine Mono, which only some kits carry)
  bool steam_stub = false;  // SteamStub DRM: the entry point is in a ".bind" section
  bool steam_stub_supported = false;  // ...in a variant Playden unpacks (runtime/steam_stub.h)
  std::string steam_stub_detail;      // for logs: the variant, or why it is not unpacked
  // A writable section shared between processes whose pages the console can't map
  // (16 KB pages; GameMaker's runner has one, ".mydata").
  bool shared_section = false;
  bool anticheat = false;   // an EasyAntiCheat or BattlEye folder in the game
  // For an Unreal game started through its root program (which starts the real one as a
  // child process, which the console cannot do): the program to start instead (the
  // *-Shipping.exe, or the one program in <Project>/Binaries/Win64), relative to the install
  // root, as spelled on disk.
  std::string unreal_program;
  bool unreal5 = false;  // Unreal Engine 5 (its Shipping program names Nanite's settings)
};

inline constexpr unsigned long long kProbeScanBytes = 256ull * 1024ull * 1024ull;
// At most this much of a library's resources is read for its version.
inline constexpr std::uint32_t kVersionScanBytes = 8u * 1024u * 1024u;

// executable is relative to install_root with '/' separators, as spelled on disk.
GameProbe ProbeGame(const std::string& install_root, const std::string& executable);

// Whether a program itself draws with OpenGL and nothing else: of the graphics libraries, it imports only OpenGL's.
// Unlike GameProbe::apis, the libraries beside it don't count, so a launcher next to the game's DLLs is not one.
bool ProgramDrawsWithOpenGLOnly(const std::string& install_root, const std::string& executable);

// Whether a program loads PhysX through NVIDIA's loader (PhysXLoader.dll, imported directly or delay-loaded), which
// finds the engine the PhysX System Software installs. executable is relative to install_root, '/' separators.
bool ProgramUsesPhysx(const std::string& install_root, const std::string& executable);

// The program a game's last run couldn't start, from the note the title's Wine leaves in the bottle: what a launcher
// without launcher mode asked for. Relative to host_root, as spelled in the note, when it's a program in the game's
// folder (console_root on the console, host_root as seen here) that draws or has an engine, so a crash reporter or an
// updater isn't taken for the game. The note names one run: this removes it.
std::string TakeRefusedGameProgram(const std::string& prefix_dir, const std::string& console_root,
                                   const std::string& host_root);

// The program that starts an Unreal game when executable (relative to install_root, '/' separators) is its root
// launcher, which only starts that one as a child process, which the console cannot do; relative to install_root,
// as spelled on disk. Empty for any other program. GameProbe::unreal_program, without reading the programs.
std::string UnrealGameProgram(const std::string& install_root, const std::string& executable);

// Which of a game's programs and libraries import Microsoft's MFC library (mfc140*.dll, mfcm140*.dll, directly
// or delay-loaded), which Wine doesn't provide: Playden installs Microsoft's own into the game's bottle. Every
// .exe and .dll of the game folder is read, kMfcScanDepth folders deep, at most kMfcScanPrograms of them (headers
// only) in at most kMfcScanFolders folders, without following links; the walk ends once both architectures are
// found. The first program found per architecture, relative to the root.
struct MfcImports {
  std::string x86_program;  // a PE32 one
  std::string x64_program;  // a PE32+ one
  std::size_t programs = 0;  // programs read
  std::size_t folders = 0;   // folders listed
  bool truncated = false;    // stopped at kMfcScanPrograms or kMfcScanFolders
};
inline constexpr int kMfcScanDepth = 6;
inline constexpr std::size_t kMfcScanPrograms = 4096;
inline constexpr std::size_t kMfcScanFolders = 4096;
MfcImports FindMfcImports(const std::string& install_root);

// How a game without a shipped recipe starts, from its probe.
struct GameDefaults {
  std::string graphics = "auto";  // auto, dxvk or opengl
  std::string arguments;          // recipe arguments; may hold "{resolution}"
  std::string resolution_arguments;
  std::string environment;  // "NAME=value;..." the engine needs, for the Environment variables setting
  std::string d3d12;        // the Direct3D 12 setting the game needs ("vkd3d-proton"), empty for the default
  std::string refusal;  // non-empty: the console cannot run this program; a sentence for the player
  // The refusal comes from a broad detection (.NET, a shared section) that a run could get
  // past, so the player may "Try anyway"; graphics and arguments are still filled in.
  // SteamStub forms Playden can't unpack, Direct3D 12 only (on a kit without vkd3d-proton) and NW.js (on a kit
  // without driver threads) are never soft.
  bool refusal_soft = false;
  std::string summary;  // e.g. "unity,d3d11 graphics=dxvk", for logs and the settings sheet
};
// What the installed kit's runtime can do beyond the baseline, as far as starting a game is concerned.
struct RuntimeAbilities {
  bool shared_sections = false;  // maps a shared section the console's pages can't hold as a private copy
  bool wine_mono = false;        // carries Wine Mono, so .NET programs start
  bool vkd3d_proton = false;     // carries vkd3d-proton, so Direct3D 12 programs draw
  bool driver_threads = false;   // hosted driver threads skip the game's thread attach, so NW.js gets past its start
};
GameDefaults DefaultsFor(const GameProbe& probe, const RuntimeAbilities& runtime = {});
// Which DefaultsFor an engine's games were set up with. Raise an engine's number when what DefaultsFor gives its games
// changes: their saved recipes then take the new defaults on the next Play or kit update, without a Recreate bottle.
// An engine not listed is 0. `engine` may be a GameDefaults::summary, whose first word is the engine.
int DefaultsRevision(std::string_view engine);

// The player's words for a GameDefaults::summary: "Unity · Direct3D 11", "OpenGL"; empty
// when nothing was recognised.
std::string DetectedLabel(const std::string& summary);

// The DLL overrides that make a game use the bottle's DXVK instead of Wine's Direct3D.
inline constexpr const char* kDxvkOverrides = "d3d8,d3d9,d3d10core,d3d11,dxgi=n";
// The Direct3D 12 setting set to vkd3d-proton adds these after the DXVK ones (vkd3d-proton needs DXVK's dxgi).
inline constexpr const char* kVkd3dProtonOverrides = "d3d12,d3d12core=n";
// The same when the list does not already load a dxgi of its own choice.
inline constexpr const char* kVkd3dProtonDxgiOverrides = "d3d12,d3d12core,dxgi=n";

}  // namespace playden::runtime
