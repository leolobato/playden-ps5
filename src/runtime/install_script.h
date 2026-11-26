#pragma once

#include "runtime/game_settings.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace playden::runtime {

// A Steam install script (installscript.vdf in the game folder): Steam applies its Registry
// section on a game's first start, and some games look for those keys (Fallout: New Vegas's
// launcher offers only "Install" without HKLM\SOFTWARE\Bethesda Softworks\FalloutNV
// "Installed Path"). Playden writes them into the bottle instead. Run Process entries
// (redistributable installers) are not run: Wine's built-ins stand in for them.

// A File Template entry of an install script: Steam's client fills template_file in with values from the registry and
// writes the result to output_file, once (DOOM 3's base\STEAM_doomkey, "%CDKEY%" and two comment lines, becomes
// base\doomkey with the account's CD key). Every other field names a registry value, which becomes the %TOKEN% of
// the field's name. The file paths and has_run_key have their %TOKEN%s replaced; the fields are as written. Steam's
// MinimumHasRunValue is not kept: Playden fills an entry in again only as ApplyInstallScripts says.
struct FileTemplate {
  std::string name;           // the entry's name ("doomkey_template"): the has-run value's name under has_run_key
  std::string has_run_key;    // HKEY_CURRENT_USER\Software\Valve\Steam\Apps\9050
  std::string template_file;  // Windows paths
  std::string output_file;
  std::vector<std::pair<std::string, std::string>> fields;  // token name, registry value path
};

// The values a script's Registry section sets, by hive. Steam's %TOKEN%s become the bottle's folders
// (%INSTALLDIR% is install_dir, the game folder's Windows path; %APPDATA%, %USER_MYDOCS%, %WinDir% and the
// like the folders Steam, a 32-bit program, would see). HKLM\Software keys go to both registry views, as
// Steam writes them where 32-bit and 64-bit games find them; HKEY_LOCAL_MACHINE_WOW64_32 and _WOW64_64 keys
// go to that one view (Software\Wow6432Node, or the key), and such a value wins over a plain HKLM key's copy
// for the same view. HKEY_CLASSES_ROOT is HKLM\Software\Classes, and HKCU's view names are HKCU. Steam takes
// "string" and "dword" values; names and strings may be any UTF-8 text, written as Wine writes it, while keys
// stay ASCII. Text may be UTF-8 or UTF-16 with a byte order mark. A language subkey of "string" or "dword"
// gives its values only for that language: language (Steam's name, like "russian"; empty for none) is used,
// then "english". A value named (Default) is the key's unnamed one. What can't be written (another root, an
// unknown %TOKEN%, another type, a dword that isn't a number) is left out and named in dropped, with why.
// The script's File Template entries are read too; an entry without both files or a has-run key, or naming an unknown
// %TOKEN% in them, is left out and named in dropped ("<entry>: <reason>").
struct InstallScriptValues {
  std::vector<RegistryValue> machine;  // system.reg (HKEY_LOCAL_MACHINE)
  std::vector<RegistryValue> user;     // user.reg (HKEY_CURRENT_USER)
  std::vector<FileTemplate> templates;
  std::vector<std::string> dropped;    // "<key> [<name>]: <reason>", the first 16, log-safe
  std::size_t dropped_count = 0;       // every value or entry left out
};
bool ParseInstallScript(std::string_view text, const std::string& install_dir, const std::string& language,
                        InstallScriptValues* values, std::string* reason);

// The keys a value under `subkey` goes to. An HKLM Software key (any case) goes to both registry views, as Steam,
// a 32-bit program, writes it where 32-bit and 64-bit games find it: the key itself and Software\Wow6432Node\<rest>
// (Software\Classes\Wow6432Node\<rest> for a Classes key, where Wine keeps those).
// Any other key, and any HKCU key, is the key alone. Used for Steam's scripts and GOG's alike.
std::vector<std::string> InstallScriptKeys(bool machine, const std::string& subkey);

// Wine's text form of a REG_SZ: quoted, with backslashes and quotes escaped, and UTF-8 text past ASCII as Wine's
// \x escapes (server/unicode.c dump_strW). Bytes that aren't UTF-8 are kept as they are.
std::string RegistryString(std::string_view text);

// Adds value to list, replacing one of the same key and name (any case, named or unnamed): a registry key holds one
// of each, so the last value set wins, and a hive never gets the same value twice.
void SetRegistryValue(std::vector<RegistryValue>* list, RegistryValue value);

// Whether a file is named as one of Steam's install scripts (any case): "installscript*.vdf", or
// "<app id>_install.vdf" (Singularity's 42670_install.vdf). A game's appinfo names its script, in either form.
bool InstallScriptName(std::string_view name);

// The list of a Steam game's install scripts that Playden keeps when it downloads the game
// (steam::DepotInstaller::kInstallScripts): the files its depots flag as install scripts, which the Steam client runs
// whatever their names.
inline constexpr const char* kInstallScriptList = ".playden-install-scripts";

// The install scripts of the game at game_root, as paths relative to it ('/' separators), sorted with the top
// level's first. A game with kInstallScriptList: the files it names (at any depth) that are there as regular files,
// with no link on the way; flagged is set. Without the list (a Steam game installed before it was kept, an
// imported or GOG game): the files at the top that InstallScriptName accepts.
struct InstallScriptFiles {
  std::vector<std::string> paths;
  bool flagged = false;
};
InstallScriptFiles FindInstallScripts(const std::string& game_root);

struct LegacyKey;  // runtime/steamworks.h

// Applies the Registry sections of the game's install scripts (FindInstallScripts) to the stopped bottle at
// prefix_dir, backing each hive up first. A later script's value replaces an earlier one's of
// the same key and name. An unchanged hive is not written. found lists the scripts read (paths in game_root); a
// script that cannot be read or parsed is skipped and named in skipped; dropped names the values
// left out of the scripts that were read ("<script>: <key> [<name>]: <reason>", the first 16), and dropped_count
// counts them all.
//
// legacy_key: the account's CD key the download asked Steam for (ReadLegacyKey), or none. Its "registry" method puts the
// key in the value its location names, as Steam's client does before it runs a script; the key is also %CDKEY%.
// Then each File Template entry is filled in, its file written into the game folder (output_file under install_dir;
// neither file through a link), and its has-run value (dword 1) set, as Steam's client does, when its has-run value
// isn't in the bottle yet, or its output file is missing, empty or back to the bytes the game's depots shipped
// (SameAsDepotFile): the bottle outlives the game's files, so a reinstall with saves kept has no base\doomkey yet, and
// an update puts back a file a depot ships. Otherwise the file stays as the game or the player left it. To fill one
// in, a field reads the key when it names the key's location, or a string value the scripts set; %CDKEY%, %INSTALLDIR%
// and the folders Registry values may name are known too. An entry that needs anything else (no key, another registry
// value, an unknown %TOKEN%) is left for a later start and named in dropped. templates lists the files written this
// time (paths in game_root). The key itself never reaches the result.
struct InstallScriptResult {
  std::vector<std::string> found;
  std::vector<std::string> skipped;
  std::vector<std::string> dropped;
  std::size_t dropped_count = 0;
  std::size_t values = 0;
  bool legacy_key = false;             // the key went into the registry
  std::vector<std::string> templates;  // File Template outputs written this time
  bool ok = true;
  std::string message;  // on failure
};
InstallScriptResult ApplyInstallScripts(const std::string& game_root, const std::string& install_dir,
                                        const std::string& language, const std::string& prefix_dir,
                                        std::int64_t unix_time, const LegacyKey* legacy_key = nullptr);

// A redistributable an install script runs on the game's first start ("Run Process" entry).
struct RedistEntry {
  std::string name;         // the entry's name ("PhysX")
  std::string process;      // "process 1", %INSTALLDIR% replaced
  std::string command;      // "command 1", %INSTALLDIR% replaced
  std::string has_run_key;  // "HasRunKey", e.g. HKEY_LOCAL_MACHINE\Software\Valve\Steam\Apps\696180
};

// The Run Process entries of one install script (only the first process of each); entries
// naming another %TOKEN% or non-ASCII text are left out.
bool ParseRunProcess(std::string_view text, const std::string& install_dir, std::vector<RedistEntry>* entries,
                     std::string* reason);

// Reads an install script file (at most 1 MiB, no symlink).
bool ReadInstallScript(const std::string& path, std::string* text);

}  // namespace playden::runtime
