#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace playden::runtime {

// The stage manifest (stage.json at the root of an unpacked publisher archive): what a
// publisher's games need placed in their title folder before Play. Everything specific to a
// publisher (file names, launch arguments, credential fields) is data here; Playden only knows
// these generic shapes. Version 1; unknown top-level keys are refused.
inline constexpr const char* kPublisherStageManifestName = "stage.json";
inline constexpr std::size_t kMaxPublisherStageManifestBytes = 64 * 1024;

enum class StageArch { kX86, kX64 };

// How to read a credential blob: a JSON object of at most max_bytes whose first present
// stage field is a string of stage_min..stage_max characters from the base64 alphabet
// (A-Za-z0-9+/=). The first present display field names the account in the UI.
struct PublisherAccountSpec {
  std::string name;  // UI text
  std::vector<std::string> display_fields;
  std::vector<std::string> stage_fields;
  // Optional: the sign-in fields of a second value the credential file carries on its second line, the first of them
  // the sign-in holds as 1 to 1024 printable characters without spaces (Rockstar's RememberedMachineToken, without
  // which an account with two-factor authentication is refused, #1252). A sign-in without one gets a one-line file.
  std::vector<std::string> stage_extra_fields;
  std::size_t stage_min = 0;
  std::size_t stage_max = 0;
  std::size_t max_bytes = 0;
};

// A runtime extracted from an installer the game ships (paths below the install root).
struct PublisherRuntimeSpec {
  std::string label;                          // UI text
  std::vector<std::string> installers;        // first one present wins
  std::string version_file;                   // inside the installer's archive
  std::map<std::string, std::string> folders;  // archive top folder -> folder below the version folder
};

struct PublisherTitleSpec {
  StageArch arch = StageArch::kX86;
  std::string args;
  // More values for the ini lines, as ${title.<key>}: keys [a-z0-9_]{1,32} (not "args"), single-line
  // values of at most 256 bytes, up to 16 of them.
  std::map<std::string, std::string> vars;
  // The program Playden starts for this title instead of Steam's launch entries (a launcher that
  // would start the game as another process), relative to the game's install root (not the
  // title folder). "" = Steam's.
  std::string program;
  // "status": "untested": nobody has played it through the pack yet. Playden tells the player so once,
  // before the first Play, and reports name it. Absent = tested (every title of an older pack). An older
  // Playden ignores the key: title objects don't refuse keys they don't know.
  bool untested = false;
};

// One file placed in the title folder: a file of the unpacked pack (source) or literal text.
struct PublisherStageFile {
  std::optional<std::string> source;
  std::optional<std::string> text;
  std::string target;
  std::string keep_original;      // "" = none
  std::optional<StageArch> arch;  // none = every title
};

struct PublisherStageIni {
  std::string target;
  std::vector<std::string> lines;  // may use ${title.args} and ${runtime}
};

// Read after a session: when the file's trimmed content equals `expired`, the account expired;
// when it equals `missing` (optional, "" = none), the game started without any sign-in; when it equals
// `refused` (optional, "" = none), the publisher accepted the sign-in but refused this game for the account.
// `detail` (optional, "" = none) is a file beside it naming the refused call, for the log only.
struct PublisherStatusSpec {
  std::string file;
  std::string expired;
  std::string missing;
  std::string refused;
  std::string detail;
};

// A launcher the title runs beside the game, in a second process on the game's Wine server ([helper] in the
// profile, kit capability helper-process). program is relative to the title folder and ends in .exe; the
// program writes ready, a path below the bottle's drive_c, when the game may start; the title waits wait
// seconds (1-120) for it.
struct PublisherHelperSpec {
  std::string program;
  std::string ready;
  std::uint32_t wait = 60;
  // "launcher": the game's required storefront launcher (the runtime reports it as the parent); "" otherwise.
  std::string identity;
};

// How Playden renews a sign-in before it runs out: one HTTPS GET whose answer replaces some of the
// sign-in's fields. Names refer to the credential blob: "Field" or "Object.Field" (one level).
struct PublisherRefreshSpec {
  std::string url;                                           // https://, no spaces or control characters
  std::vector<std::pair<std::string, std::string>> headers;  // sent as is (not Cookie, Host or Content-Length)
  // Cookie name -> blob field: sent from the sign-in, and the answer must set each of them again
  // (Set-Cookie); the new values are stored back in the same fields.
  std::vector<std::pair<std::string, std::string>> cookies;
  std::vector<std::string> fields;      // answer fields copied into the sign-in; a stage field is among them
  std::string ok_field;                 // a boolean the answer must hold true; "" = none
  std::vector<long> expired_statuses;   // HTTP statuses that mean the sign-in is gone
  std::uint32_t every_hours = 24;       // 1-168: renew when the last renewal is older
  std::uint32_t before_launch_minutes = 0;  // 0-1440: renew before placing the credential; 0 = not then
};

struct PublisherStageManifest {
  PublisherAccountSpec account;
  std::string signin_page;  // "" = none
  std::optional<PublisherRefreshSpec> refresh;
  std::optional<PublisherRuntimeSpec> runtime;
  std::map<std::string, PublisherTitleSpec> titles;  // Steam app id (decimal) -> title
  std::vector<PublisherStageFile> files;             // in placement order
  std::optional<PublisherStageIni> ini;
  std::string credential_file;
  std::optional<PublisherStatusSpec> status;
  // Other installers of the game (paths below the install root) the pack stands in for, like the
  // runtime's installers: the game's install scripts don't need to run them.
  std::vector<std::string> covered_installers;
  std::optional<PublisherHelperSpec> helper;
};

// Parses and validates a manifest. Required: version (1), account, titles, files and
// credential_file; signin, refresh, runtime, ini, status, covered_installers and helper are optional. Paths are relative with '/'
// separators and no empty, "." or ".." parts, drive, backslash or control characters. On
// failure, reason says what is wrong.
bool ParsePublisherStageManifest(std::string_view text, PublisherStageManifest* manifest, std::string* reason);

// Reads <directory>/stage.json (a regular file, links not followed) and parses it.
bool LoadPublisherStageManifest(const std::string& directory, PublisherStageManifest* manifest, std::string* reason);

// The ini lines with ${title.args}, ${title.<key>} (the title's vars) and ${runtime} replaced.
// runtime_path is the Windows path of the runtime's version folder ("" when there is none). An
// unknown variable, ${runtime} without a runtime, or a value with a line break is an error. The
// parser accepts ${title.<key>} only when every title defines that key.
bool ExpandPublisherStageIni(const PublisherStageManifest& manifest, const PublisherTitleSpec& title,
                             const std::string& runtime_path, std::vector<std::string>* lines, std::string* reason);

}  // namespace playden::runtime
