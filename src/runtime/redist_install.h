#pragma once

#include "runtime/install_script.h"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace playden::runtime {

// Redistributables a game's Steam install scripts run on its first start ("Run Process"
// entries: PhysX, Visual C++, DirectX, .NET). The console can't start their installers, so:
// Windows Installer packages are installed by Playden itself (runtime/msi_package.h), installers
// whose DLLs Wine provides are skipped, and other programs are skipped and logged.

enum class RedistAction { kMsi, kCovered, kNotRun };

// What Playden does with an entry. For kMsi, package is the package's path inside the game
// folder ('/' separators), found from `msiexec /i <package>` or a process that is the package.
RedistAction ClassifyRedist(const RedistEntry& entry, const std::string& install_dir, std::string* package);

// A Windows Installer package a setup program carries as is, uncompressed (WISE and other
// self-extracting installers, NVIDIA's PhysX System Software among them): the offset of the first
// compound file in the program that reads as a package with files, or nullopt. Compressed payloads
// are not looked into.
std::optional<std::size_t> FindEmbeddedMsi(std::string_view program);

struct RedistOutcome {
  std::string script;   // the install script, relative to the game folder
  std::string name;
  std::string result;   // "installed", "already", "covered", "not-run", "failed"
  std::string detail;   // the package, program or failure reason
  std::size_t files = 0;
};

struct RedistInstallResult {
  std::vector<RedistOutcome> outcomes;
  bool physx_missing = false;  // find_physx, and the game folder has no PhysX package
  bool physx_searched = false;  // find_physx, and no script names PhysX: a package added to the folder would be found
};

// The PhysX version ("2.8.1") a program was built for, from the file version of the SDK libraries beside it
// (NxCooking.dll, PhysXCooking.dll, PhysXExtensions.dll). Empty when none says. program is relative to game_root.
std::string PhysxVersionBeside(const std::string& game_root, const std::string& program);

// Whether the program can run on its own PhysX engine: PhysXCore.dll or a PhysXLocal folder beside it.
bool PhysxEngineBeside(const std::string& game_root, const std::string& program);

// Whether the bottle has that version's PhysX engine, where PhysX's packages install it (NVIDIA's or AGEIA's folder).
// With an empty version, any engine.
bool BottleHasPhysxEngine(const std::string& prefix_dir, const std::string& version);

// The PhysX version the first of a game's programs needs and the bottle lacks (empty when the program doesn't say
// which), of those uses_physx accepts and that have no engine of their own; nullopt when none lacks one.
std::optional<std::string> MissingPhysxEngine(const std::string& game_root, const std::vector<std::string>& programs,
                                              const std::string& prefix_dir,
                                              const std::function<bool(const std::string&)>& uses_physx);

// What MissingPhysxEngine last found, kept in the bottle so a later Playden start can say it without reading the game's
// drive (which may not be there). RecordPhysxNeeded replaces it (nullopt: nothing is missing); RecordedPhysxNeeded reads
// it: the version, empty when it isn't known, or nullopt.
bool RecordPhysxNeeded(const std::string& prefix_dir, const std::optional<std::string>& needed);
std::optional<std::string> RecordedPhysxNeeded(const std::string& prefix_dir);

// Whether a redistributable's name (an install script's entry) is PhysX's.
bool RedistIsPhysx(const std::string& name);

// A package being installed: called as it starts (done 0, total 1, before its size is known), then with the files
// written so far of its total, on the calling thread. name is the entry's, which comes from the game's files.
using RedistProgress = std::function<void(const std::string& name, std::size_t done, std::size_t total)>;

// Finds install scripts at the top of game_root and in its redistributable folders
// (_CommonRedist, Redist, Support, __Installer…, up to four folders deep), and installs their
// Windows Installer packages into the stopped bottle at prefix_dir, including a package a setup
// program in the game folder carries (FindEmbeddedMsi). A package installed before (the same
// SHA-256, listed in <prefix>/.playden-redist; a setup program's own) is not installed again. A
// package that fails is reported and left out; the others still install.
// With find_physx (the game's program uses PhysX, ProgramUsesPhysx) and no script that names PhysX, PhysX's
// packages in those folders are installed the same way, found by name: a game imported from a folder has no script.
// publisher_installers: programs in the game folder ('/' separators, any case) that the game's staged publisher
// stands in for (its stage manifest lists them); entries that run one are "covered" with detail
// "publisher: <path>".
RedistInstallResult InstallRedistributables(const std::string& game_root, const std::string& install_dir,
                                            const std::string& prefix_dir, std::int64_t unix_time, bool find_physx = false,
                                            const RedistProgress& progress = {},
                                            const std::vector<std::string>& publisher_installers = {});

// Installs entries a caller found itself (a GOG game's redist list, gog/dependencies.h) the way
// InstallRedistributables installs its scripts' entries: ClassifyRedist, the package or the one a setup program
// carries, each known by its SHA-256 in .playden-redist; script_label names them in the outcomes. With burn_bundles, a
// setup program in the game folder that reads as a WiX Burn bundle (runtime/burn_bundle.h) has each of its
// MsiPackages installed with all of its files, and the bundle is recorded once every one has; a bundle with none is
// not run. Programs ClassifyRedist calls covered (Visual C++, DirectX, .NET Framework) are never opened.
RedistInstallResult InstallRedistEntries(const std::string& game_root, const std::string& install_dir,
                                         const std::string& prefix_dir, std::int64_t unix_time,
                                         const std::vector<RedistEntry>& entries, const std::string& script_label,
                                         bool burn_bundles, const RedistProgress& progress = {});

// Microsoft's MFC library, which Wine doesn't provide, for the games that import it (runtime/game_probe.h
// FindMfcImports): Playden installs only the MFC files of a Visual C++ redistributable into a game's bottle, and the C
// runtime stays Wine's builtin. The versions (kMfcVersions): MFC 8 (2005) and 9 (2008) go in as side-by-side
// assemblies (Microsoft.VC80.MFC, Microsoft.VC90.MFC), the MFC and MFCLOC assembly folders under windows\winsxs with
// each one's manifest also in winsxs\manifests, where Wine looks assemblies up; MFC 10 (2010) to 14 (2015-2022) as
// mfc<version>*.dll and mfcm<version>*.dll in the system folder of their architecture.
//
// The redistributable comes from Steam's shared depot for that version (app 228980, SteamVcRedists): downloaded once
// per console, its manifest and chunks checked as a game's are. Where that isn't there, MFC 14 and MFC 8 for x86
// come from Microsoft's own download instead, pinned to a release (MicrosoftVcRedists): downloaded once per console
// and kept, its SHA-256 checked.
inline constexpr const char* kMfcVersions[] = {"80", "90", "100", "110", "120", "140"};

// The redistributable for one architecture, pinned to a release: the URL embeds its SHA-256.
struct VcRedist {
  std::string architecture;  // "x86" or "x64"
  std::string url;
  std::string sha256;        // lower case
  std::uint64_t bytes = 0;
  std::string package_id;    // MFC 14: the bundle's MsiPackage with MFC; MFC 8: the package in its cabinet
  std::string version;       // names the cache folder
  std::string mfc = "140";   // the MFC it carries: "140" (a Burn bundle) or "80" (an IExpress setup program)
};
// Microsoft's 14.44.35211 for x86 and x64, and 8.0.50727.6195 (Visual C++ 2005 SP1, MFC security update) for x86.
std::vector<VcRedist> MicrosoftVcRedists();

// The depot of app 228980 that holds a version's Visual C++ redistributable, and its setup program's file name for
// each architecture (in _CommonRedist/vcredist/<year>/; FindRedistProgram finds it by name).
struct SteamVcRedist {
  std::string mfc;
  std::uint32_t depot = 0;
  std::string x86_program;
  std::string x64_program;
};
// Visual C++ 2005 (228981), 2008, 2010, 2012, 2013 and 2022 (228989).
std::vector<SteamVcRedist> SteamVcRedists();
// The one for an MFC version, or nullopt.
std::optional<SteamVcRedist> SteamVcRedistFor(const std::string& mfc);

// A file in folder by name (any case), up to four folders deep, without following links: its path, or empty.
std::string FindRedistProgram(const std::string& folder, const std::string& name);

// Whether a file the package installs is one of MFC 14's (mfc140*.dll, mfcm140*.dll): the only ones Playden takes
// from it, so the C runtime never replaces the kit's Wine builtins. MfcFileNameOf is the same for any version.
bool MfcFileName(std::string_view name);
bool MfcFileNameOf(std::string_view name, const std::string& mfc);
// The same for Visual C++ 2005's package: whether a file it installs (a path below drive_c, '/' separators) is in
// the folder of the Microsoft.VC80.MFC or Microsoft.VC80.MFCLOC assembly under Windows/winsxs. Their publisher
// policies, the C runtime, ATL and OpenMP are left out. MfcAssemblyFile is the same for MFC 8 or 9 and either
// architecture (x86_ or amd64_ folders).
bool Mfc80AssemblyFile(std::string_view target);
bool MfcAssemblyFile(std::string_view target, const std::string& mfc, const std::string& architecture);

// One ranged GET: the bytes [offset, offset + length) of url, or nothing with a reason.
using RangeFetch = std::function<std::optional<std::string>(const std::string& url, std::uint64_t offset,
                                                            std::uint64_t length, std::string* error)>;
inline constexpr std::uint64_t kRedistChunkBytes = 8ull << 20;

struct MfcInstallResult {
  std::string result;  // "installed", "already" or "failed"
  std::string detail;  // why it failed
  std::string source;  // "download" or "cache", when installed
  std::size_t files = 0;
  std::string cache_problem;  // the download installed but could not be kept for other games: why
  bool download_failed = false;  // failed because the bundle could not be downloaded (or wasn't the pinned one)
};

// Installs redist's MFC files into the stopped bottle at prefix_dir, once: the bottle's .playden-redist lists the
// bundle's SHA-256 afterwards. The bundle is <cache_root>/vc-<version>/<architecture>/<file name of url> when that
// file has the pinned SHA-256 (a file that doesn't is removed), otherwise fetched in kRedistChunkBytes ranges,
// verified, and kept there for other games.
MfcInstallResult InstallMfcRuntime(const VcRedist& redist, const std::string& cache_root, const std::string& prefix_dir,
                                   const RangeFetch& fetch, std::int64_t unix_time);

// Installs MFC mfc for architecture from the Visual C++ setup program at program (Steam's copy, SteamVcRedists) into
// the stopped bottle, once: .playden-redist lists it as mfc<version>-<architecture> afterwards. The program is
// trusted as Steam's depot download checked it; it is mapped, not read into the heap. source is "steam".
MfcInstallResult InstallMfcFromProgram(const std::string& program, const std::string& mfc, const std::string& architecture,
                                       const std::string& prefix_dir, std::int64_t unix_time);

// Whether the bottle at prefix_dir lists redist's MFC as installed.
bool MfcInstalled(const VcRedist& redist, const std::string& prefix_dir);
// Whether it lists MFC mfc for architecture from either source: Steam's copy, or one of pinned.
bool MfcInstalledFrom(const std::string& prefix_dir, const std::string& mfc, const std::string& architecture,
                      const std::vector<VcRedist>& pinned);

// The latest MFC scan of a game (runtime/game_probe.h FindMfcImports), kept in its bottle (.playden-mfc) so Play
// doesn't walk the game folder each time: the programs found, per kind, and the game files' fingerprint then. A
// record from before every version was looked for (version 1 or 2) is not read, so each game's folder is walked
// once more.
struct MfcScanRecord {
  std::string fingerprint;
  std::map<std::string, std::string> programs;  // "<architecture>-<version>" → the first program that imports it
};
std::optional<MfcScanRecord> ReadMfcScan(const std::string& prefix_dir);
bool WriteMfcScan(const std::string& prefix_dir, const MfcScanRecord& record);
// What changes when a game's files are installed or updated: the size and time of the install record and file
// list Playden's downloads and copies write at the game's root (.playden-install.json, .playden-files). Empty for
// a folder without them (a game added over FTP), whose scan is never reused.
std::string GameFilesFingerprint(const std::string& game_root);

// Adds folders to a PATH value: prepend in order before the existing entries, append after
// them, each only when not already listed (case-insensitive).
std::string ExtendPath(const std::string& current, const std::vector<std::string>& prepend,
                       const std::vector<std::string>& append);

}  // namespace playden::runtime
