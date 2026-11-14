// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// A GOG build's goggame-<product>.script: the steps Galaxy's script interpreter runs after an install. Playden runs the
// two that only touch the game folder, as Galaxy would:
// - `supportData`: copy a support folder into the game (`source`), never over an existing file unless the script says
//   so, or create a folder (no `source`);
// - `setIni`: set one key in an INI file, such as ScummVM's game `path`. `{app}` in a value is the game folder as Wine
//   sees it, so these steps run again before each Play (without the copies), which keeps them right after a move.
// - `setRegistry`: set one value in the game's bottle (PlanRegistry), again before each Play for the same reason.
// Anything else is skipped and counted. Every path must stay in the game folder or the product's support folder; a
// link anywhere under them refuses the step.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "steam/backend/json.h"

namespace playden::gog {

inline constexpr std::size_t kMaxScriptBytes = 1024 * 1024;
inline constexpr std::size_t kMaxIniBytes = 1024 * 1024;

struct ScriptAction {
  std::string name;
  std::vector<std::string> languages;  // GOG codes or "*"; a script that names none means every language
  struct Install {
    std::string action;              // "supportData", "setIni", "setRegistry", ...
    steam::json::Value arguments;    // the action's own object, as written
  };
  std::optional<Install> install;
};

struct Script {
  std::vector<ScriptAction> actions;
};

// The file's text, after a UTF-8 byte-order mark. Nothing when it is over kMaxScriptBytes or not a JSON object. An
// action that isn't an object is skipped; one without an `install` object keeps `install` empty.
std::optional<Script> ParseScript(std::string_view text);

// The file at `path`, read and parsed; nothing when it is missing, a link, too large or doesn't parse.
std::optional<Script> ReadScript(const std::string& path);

// The absolute path of goggame-<product>.script at the install root, matched ignoring case in the root's listing and
// spelled as the listing has it; nothing when there is none or it isn't a regular file.
std::optional<std::string> FindScriptFile(const std::string& root, std::uint64_t product);

struct ScriptContext {
  std::string game_root;     // absolute: `{app}`
  std::string support_root;  // absolute: `{supportDir}`, .playden-gog/support/<product> under the game root
  std::string product;       // `{productID}`
  std::string gog_language;  // the install's GOG code ("en-US")
  std::string windows_app;   // the game folder as Wine sees it ("Z:\Games\BASS"): `{app}` in values
};

// A place a step touches: `relative` (`/` separated, already checked, empty for the folder itself) under `base`, an
// absolute folder that exists. `support` is set for a place in the support folder.
struct ScriptLocation {
  std::string base;
  std::string relative;
  bool support = false;
  std::string Path() const { return relative.empty() ? base : base + "/" + relative; }
  bool operator==(const ScriptLocation& other) const {
    return base == other.base && relative == other.relative && support == other.support;
  }
};

struct ScriptStep {
  enum class Kind { kCopyFolder, kCreateFolder, kSetIni };
  Kind kind = Kind::kCreateFolder;
  std::string name;           // the action's name, for logs
  ScriptLocation target;      // the folder copied into or created, or the INI file
  ScriptLocation source;      // kCopyFolder: the folder copied
  bool overwrite = false;     // kCopyFolder: replace files the game folder already has
  std::string section;        // kSetIni
  std::string key;
  std::string value;          // with `{app}` and `{productID}` filled in
  bool utf8 = false;          // false: the value is written as Latin-1 when it fits
};

// The steps for the install's language, in script order, with locations and values filled in. An action runs when its
// languages hold "*" or the install's language (ignoring case, and GOG's deprecated codes such as "en" for "en-US");
// `conditions` (onLanguageChange, ...) are not read. `skip_copies` leaves out folder copies, for the run before each
// Play, where they could overwrite the player's files.
//
// A location must start with `{app}` or `{supportDir}`, then end or go on after `\` or `/`; `{productID}` is replaced,
// and the rest must pass NormalizePath (no `..`, no empty component, no drive; nothing Playden keeps at the game
// folder's top). Any other location (an unknown token, `C:\`, `{supportDir}/../../x`) drops the step. A section, key
// or value with a control character drops it too. `skipped`, when given, gets the number of actions for the language
// that give no step: unknown actions and dropped ones. setRegistry is PlanRegistry's and is not counted here.
std::vector<ScriptStep> PlanScript(const Script& script, const ScriptContext& context, bool skip_copies,
                                   std::size_t* skipped = nullptr);

struct RunResult {
  std::size_t changed = 0;  // steps that changed something on disk
  std::size_t skipped = 0;  // copies whose source folder is missing
  std::size_t failed = 0;   // steps refused (a link, a file where a folder goes) or that could not write
  std::string failure;      // the first failure, a short sentence naming the step's path in the script's terms
};

// Runs the steps in order; a failed step doesn't stop the others. POSIX calls on absolute paths only, and every
// component under a location's base is checked with lstat: a link refuses the step. Existing components are matched
// ignoring ASCII case, as the game sees them.
// - kCopyFolder walks the source (platform/directory_walk.h) and copies its regular files through a synced temporary
//   file renamed into place; an existing file is kept unless `overwrite`. A missing source is skipped. Nothing is
//   written over one of Playden's own names at the game folder's top, nor into the source itself.
// - kCreateFolder makes the folder and its parents (mode 0777).
// - kSetIni sets the key, creating the file and its folder when missing: an existing key's line is replaced (unless it
//   already holds the value), a missing key goes after the section's last non-blank line, a missing section at the
//   end. Sections and keys match ignoring ASCII case; every other byte and the file's line ending stay (a new file
//   uses CRLF). The file is written through a synced temporary file with the original's mode, only when it changed.
RunResult RunScript(const std::vector<ScriptStep>& steps);

// One value a `setRegistry` action sets, in Wine's terms but before any hive is chosen.
struct RegistrySetting {
  bool machine = false;        // HKEY_LOCAL_MACHINE (system.reg); otherwise HKEY_CURRENT_USER (user.reg)
  std::string key;             // under the root, `\` separated ("Software\GOG.com\Games\1207658690")
  std::string name;            // empty for the key's default value
  bool default_value = false;
  enum class Type { kString, kDword };
  Type type = Type::kString;
  std::string data;            // kString: the text, tokens filled in; kDword: the number in decimal ("16")
};

// The registry values of the actions for the install's language (PlanScript's filter), in script order. What GOG
// writes (`arguments`: `root`, `subkey`, `valueName`, `valueData`, `valueType`; 25 actions in 10 of the 19 owned
// games' scripts read on 2026-11-14):
// - `root` HKEY_LOCAL_MACHINE or HKEY_CURRENT_USER, ignoring case; any other root is skipped;
// - `subkey`: `{productID}` and `{app}` (context.windows_app) are replaced, `/` becomes `\`, repeated and outer `\`
//   go; at most 1024 bytes;
// - `valueName`: the same tokens; empty or missing is the key's default value. A name with `\` or `"` is skipped:
//   the bottle's writer keeps names as they are (Monkey Island SE names a program path, for a Windows-only flag);
// - `valueType` `string` (GOG writes `String` and `string`) or `dword`, ignoring case; `valueData` of a string takes
//   the same tokens, a dword's is decimal or 0x hex (a JSON number too) up to 32 bits. Other types are skipped;
// - an action with no `valueType` and no `valueData` only makes the key (Jazz Jackrabbit 2's): skipped, as the
//   bottle's writer sets values, and a game makes its own key when it writes one.
// Any other `{token}` (`{userappdata}`), a control character or a byte outside printable ASCII in the key, name or
// data skips the action. `skipped`, when given, gets the number of setRegistry actions for the language that give no
// setting. Two actions for the same value both stay; the later one wins when they are written.
std::vector<RegistrySetting> PlanRegistry(const Script& script, const ScriptContext& context,
                                          std::size_t* skipped = nullptr);

// SetIni's text change on its own: `text` with `key=value` set in `section`. Exposed for tests.
std::string SettingIni(const std::string& text, const std::string& section, const std::string& key,
                       const std::string& value);

}  // namespace playden::gog
