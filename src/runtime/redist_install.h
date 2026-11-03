#pragma once

#include "runtime/install_script.h"

#include <cstdint>
#include <functional>
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
RedistInstallResult InstallRedistributables(const std::string& game_root, const std::string& install_dir,
                                            const std::string& prefix_dir, std::int64_t unix_time, bool find_physx = false,
                                            const RedistProgress& progress = {});

// Microsoft's MFC library, which Wine doesn't provide, for the games that import it (runtime/game_probe.h
// FindMfcImports): Playden downloads Microsoft's Visual C++ 2015-2022 redistributable once per console, keeps the
// verified setup program, and installs only the MFC files of its "Additional" package into a game's bottle.

// The redistributable for one architecture, pinned to a release: the URL embeds its SHA-256.
struct VcRedist {
  std::string architecture;  // "x86" or "x64"
  std::string url;
  std::string sha256;        // lower case
  std::uint64_t bytes = 0;
  std::string package_id;    // the bundle's MsiPackage with MFC
  std::string version;       // names the cache folder
};
// Microsoft's 14.44.35211 for x86 and x64.
std::vector<VcRedist> MicrosoftVcRedists();

// Whether a file the package installs is one of MFC's (mfc140*.dll, mfcm140*.dll): the only ones Playden takes
// from it, so the C runtime never replaces the kit's Wine builtins.
bool MfcFileName(std::string_view name);

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

// Whether the bottle at prefix_dir lists redist's MFC as installed.
bool MfcInstalled(const VcRedist& redist, const std::string& prefix_dir);

// The latest MFC scan of a game (runtime/game_probe.h FindMfcImports), kept in its bottle (.playden-mfc) so Play
// doesn't walk the game folder each time: the programs found, per architecture, and the game files' fingerprint
// then.
struct MfcScanRecord {
  std::string fingerprint;
  std::string x86_program;
  std::string x64_program;
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
