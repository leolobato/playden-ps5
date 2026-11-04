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

// The values a script's Registry section sets, by hive. %INSTALLDIR% becomes install_dir (the
// game folder's Windows path). HKLM\Software keys go to both registry views, as Steam, a
// 32-bit program, writes them where 32-bit and 64-bit games find them. Only "string" and
// "dword" values are taken; a value with another %TOKEN% or non-ASCII text, and keys under
// other roots, are skipped. Text may be UTF-8 or UTF-16 with a byte order mark. A language subkey
// of "string" or "dword" gives its values only for that language: language (Steam's name, like
// "russian"; empty for none) is used, then "english". A value named (Default) is the key's unnamed one.
struct InstallScriptValues {
  std::vector<RegistryValue> machine;  // system.reg (HKEY_LOCAL_MACHINE)
  std::vector<RegistryValue> user;     // user.reg (HKEY_CURRENT_USER)
};
bool ParseInstallScript(std::string_view text, const std::string& install_dir, const std::string& language,
                        InstallScriptValues* values, std::string* reason);

// Applies the Registry sections of the install scripts at the top of game_root (file names
// starting with "installscript" and ending ".vdf", any case) to the stopped bottle at
// prefix_dir, backing each hive up first. An unchanged hive is not written. found lists the
// scripts read; a script that cannot be read or parsed is skipped and named in skipped.
struct InstallScriptResult {
  std::vector<std::string> found;
  std::vector<std::string> skipped;
  std::size_t values = 0;
  bool ok = true;
  std::string message;  // on failure
};
InstallScriptResult ApplyInstallScripts(const std::string& game_root, const std::string& install_dir,
                                        const std::string& language, const std::string& prefix_dir,
                                        std::int64_t unix_time);

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
