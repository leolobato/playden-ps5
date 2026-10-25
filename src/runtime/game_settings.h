#pragma once

#include "library/store.h"
#include "runtime/prospero_profile.h"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace playden::runtime {

// Per-game settings: the catalogue the settings sheet is drawn from, the recipe and preset
// data that supply defaults, and the effective values that become the prospero profile and
// dxvk.conf.
//
// Resolution order for each setting:
//   Playden default -> recipe default -> preset -> player override,
// except that a setting the recipe locks keeps the recipe value until the player chooses
// "Change anyway" (an "unlock.<id>" entry in the overrides). An absent override means
// "inherit"; an explicit value, including "off" or an empty text, is never treated as absent.
//
// A recipe only supplies starting values: the program, its folder and arguments, the
// arguments the game needs and the library overrides each have a row the player can change.

// Where the Game settings sheet lists a setting, in sheet order. Button mapping and Controller tuning are submenus of
// Controls; Compatibility (with its Graphics and Windows parts) and Advanced start collapsed.
enum class SettingGroup {
  kGame,
  kDisplay,
  kControls,
  kButtonMapping,
  kControllerTuning,
  kSound,
  kCompatibility,
  kGraphics,
  kWindows,
  kAdvanced,
};
enum class ApplyMode { kNextLaunch, kGameClosed, kPreparesBottle };
enum class SettingKind { kChoice, kText };
enum class ValueSource { kPlayden, kRecipe, kPreset, kUser };

// What the runtime kit can apply beyond the prospero profile.
struct KitCapabilities {
  bool dxvk_config = false;   // dxvk.conf in the game folder is read (roadmap step S0)
  bool dxvk_present = false;  // the display honors DXVK's present interval (VSync off)
  bool environment = false;  // prospero-win [environment] section (upstream U1, not released)
  bool registry = false;     // Playden can edit the stopped bottle's user.reg (decision D2)
  bool input_tuning = false;  // prospero-win [input] stick_deadzone, rumble, trigger_deadzone (U4, not released)
  bool input_wheel = false;   // prospero-win [input] wheel, with Wine patch 0745 (U4, not built yet)
  bool audio_volume = false;  // prospero-win [audio] volume (U4, not released)
  bool shared_sections = false;  // Wine maps unaligned shared PE sections as private copies (patch 0602)
  bool title_any_id = false;     // one title shell for every game; no fixed set of titles
  bool game_root_unowned = false;  // games start from drives that record no file owners (exFAT)
  bool bottle_profile_shared = false;  // the bottle's 4 folders to its user profile may be 0711
  bool opengl_zink = false;        // OpenGL games draw through Mesa's Zink, not the PS5 OpenGL SDK
  bool wine_mono = false;          // .NET programs start on the kit's Wine Mono
  bool vkd3d_proton = false;       // the kit's runtime carries vkd3d-proton's Direct3D 12 DLLs
  // [display] refresh on every graphics path and [display] frame_rate (paced by the display). Counts only for a title
  // that runs this kit's runtime: an older runtime refuses a profile with frame_rate (KitForTitleRuntime).
  bool display_120hz = false;
  bool display_120hz_needs_update = false;  // the kit has it, but the game's title still runs an older runtime
  // [runtime] version = 2: the title applies the driver's defaults (wave32 pixel shaders), and keeps wave64 for a
  // profile without the key. Counts only for a title that runs this kit's runtime, like display_120hz.
  bool profile_version_2 = false;
  bool profile_version_2_needs_update = false;
};

struct SettingChoice {
  std::string value;
  std::string label;
  std::string explanation;
};

struct SettingDefinition {
  std::string id;
  std::string label;
  std::string effect;       // one-line description under the name
  std::string also_called;  // technical names: profile keys, DXVK options
  SettingGroup group = SettingGroup::kGame;
  int phase = 1;            // roadmap phase that ships it
  ApplyMode apply = ApplyMode::kNextLaunch;
  SettingKind kind = SettingKind::kChoice;
  std::vector<SettingChoice> choices;  // empty for text and for launch_option (from the recipe)
  std::string playden_default;         // empty means "not written"
};

const std::vector<SettingDefinition>& SettingsCatalogue();
const SettingDefinition* FindSetting(std::string_view id);

// Whether the sheet lists a setting at all. Every setting is listed, so players see what is
// coming; one the kit, the game or Playden cannot apply yet is disabled with its reason
// (EffectiveValue::available). Button rows live under Button mapping.
bool SettingShown(const SettingDefinition& definition, const KitCapabilities& kit);

// The 17 controller buttons prospero-win binds, as setting ids "button.<name>".
// The rows a controller preset sets, by preset id: setting id → value.
const std::map<std::string, std::map<std::string, std::string>>& ControllerPresetInputs();
const std::vector<std::string>& ProsperoButtons();

struct LaunchEntry {
  std::string id;
  std::string label;
  std::string executable;         // relative to the install root, '/' or '\' separated
  std::string working_directory;  // relative to the install root; empty = executable's folder (see below)
  std::string arguments;          // this entry's own arguments, after the recipe's
  // With an empty working_directory: start in the install root instead, as Steam does for an
  // entry that names no folder.
  bool in_install_root = false;
};

// The folder an entry starts in, relative to the install root ("" is the root itself); nothing
// when its paths leave the game folder.
std::optional<std::string> LaunchWorkingFolder(const LaunchEntry& launch);

// One value Playden owns in the bottle's user.reg (HKEY_CURRENT_USER). An empty data
// removes the value, so the game inherits Wine's default again.
struct RegistryValue {
  std::string key;   // e.g. Software\Wine\AppDefaults\hl2.exe (single backslashes)
  std::string name;  // e.g. Version
  std::string data;  // Wine's text form: "\"win7\"" or "dword:00000078"; empty removes
  bool default_value = false;  // Wine uses @= for a key's unnamed value
};

struct GameRecipe {
  std::string id;
  int version = 0;
  // A Steam game's app ID, or the runtime number of a game from another store, which `game` names.
  std::uint32_t app_id = 0;
  // The game ("store"/"game" on disk, written only when not Steam). Default: the Steam game app_id
  // names, as in every shipped recipe; RecipeGame resolves it.
  GameId game;
  std::string name;
  std::vector<LaunchEntry> launch_entries;  // the first is the default
  // Arguments the game needs. "{resolution}" is replaced by resolution_arguments with
  // {w} and {h} filled from the Resolution setting. Player arguments are appended after.
  std::string arguments;
  std::string resolution_arguments;
  std::string dll_overrides;  // always written first; the player can only add entries
  std::string architecture = "pe32";
  std::string runtime = "wine-wow64";
  std::string prefix = "game";
  std::string graphics = "auto";
  std::map<std::string, std::string> defaults;  // setting id -> value
  std::set<std::string> locked;                 // setting ids the player cannot change
  std::string detected;  // generated recipes: what the game's files showed (GameDefaults::summary)
  // Values the bottle's user.reg gets when the game's files are prepared, as a Steam install
  // script's would be (docs/community-recipes.md): strings under HKEY_CURRENT_USER.
  std::vector<RegistryValue> registry;
  // What the kit's template bottle must provide ("audio": "ps5"); see UnmetRecipeRequirements.
  std::map<std::string, std::string> requirements;
};

struct SettingsPreset {
  std::string id;
  std::string name;
  std::string try_when;
  std::string short_hint;
  std::map<std::string, std::string> settings;  // only the settings it changes
};

// What Playden persists per game.
struct GameSettingsRecord {
  std::string preset_id;  // empty = Playden default
  std::map<std::string, std::string> overrides;
  std::uint64_t revision = 0;
  // A game from another store that uses Steamworks: its Steam app ID for the emulator, when the game's
  // folder has no steam_appid.txt (the game's own file wins). 0: not set. Saved only when set.
  std::uint32_t steam_app_id = 0;
  // A Steam game whose launch options are all for other branches: the program the player chose at
  // Install. Its option goes first when the game is prepared. Empty: not set. Saved only when set.
  std::string steam_launch;
};

inline bool operator==(const GameSettingsRecord& left, const GameSettingsRecord& right) {
  return left.preset_id == right.preset_id && left.overrides == right.overrides && left.revision == right.revision &&
         left.steam_app_id == right.steam_app_id && left.steam_launch == right.steam_launch;
}
inline bool operator!=(const GameSettingsRecord& left, const GameSettingsRecord& right) { return !(left == right); }

struct EffectiveValue {
  std::string value;  // empty = not written
  ValueSource source = ValueSource::kPlayden;
  bool locked = false;
  bool unlocked = false;   // locked by the recipe, but the player chose "Change anyway"
  bool available = true;   // false: the row is disabled and the value is not applied
  std::string reason;      // why it is unavailable or locked
  // The row needs the kit's runtime, which the game's title does not run yet (KitCapabilities::
  // display_120hz_needs_update): it can be chosen and saved now, but `value` stays what the title's runtime takes
  // until the title is updated. after_update is the saved choice that applies then, when it differs from `value`.
  bool waits_for_update = false;
  std::string after_update;
};

// Why a row's saved choice waits: the title must move to the kit's runtime first.
inline constexpr const char* kAppliesAfterKitUpdate = "Applies after the kit update: Play or Maintenance > Update runtime.";
// The value a row shows and the player chose: the one that applies after the kit update, else the applied one.
inline const std::string& ChosenValue(const EffectiveValue& value) {
  return value.after_update.empty() ? value.value : value.after_update;
}

struct EffectiveSettings {
  std::map<std::string, EffectiveValue> values;
  // The program that starts: a recipe launch entry, or the player's own program
  // (id kProgramEntryId), with the player's folder and arguments applied. Null when the
  // recipe has no launch entry and the player chose no program yet.
  const LaunchEntry* launch = nullptr;
  std::shared_ptr<const LaunchEntry> launch_storage;  // owns *launch
  std::string arguments;                // full argument string written to the profile
  std::string dll_overrides;
  std::string graphics;
  std::string architecture;             // pe32 or pe64; follows the program
  // The runtime takes Environment variables as [debug] env lines (upstream prospero-win), not [environment].
  bool environment_as_debug_env = false;
  // The runtime reads [debug] env lines (Zink kits). With [environment] as well, a variable [environment] does not
  // take (PW_, GALLIUM_, ZINK_, VK_) still goes there.
  bool debug_env_lines = false;
  // Direct3D 12 through vkd3d-proton: ApplyD3D12Library puts its DLLs in the bottle before the game starts.
  bool vkd3d_proton = false;
  // Pixel shaders as the driver gets them: "wave32" or "wave64" adds a variable to the environment; empty (a kit
  // without environment variables) leaves the driver's default.
  std::string pixel_shader_waves;
  // The profile's [runtime] version: 2 when the game's title applies the driver's defaults (wave32 pixel shaders, so
  // Wave32 needs no variable); 1 (no key written) otherwise, and Wave32 adds pswave32 itself.
  int profile_version = 1;
  // The title's runtime honors [display] refresh on every graphics path and paces [display] frame_rate itself
  // (KitCapabilities::display_120hz); without it Frame limit is DXVK's maxFrameRate, as before.
  bool display_120hz = false;
  bool display_120hz_needs_update = false;
};

// The launch entry id of a program the player chose from the game folder.
inline constexpr const char* kProgramEntryId = "program";
// Override key prefix for "Change anyway" on a row the recipe locks: "unlock.<setting id>" = "on".
inline constexpr const char* kUnlockPrefix = "unlock.";
bool IsUnlockKey(std::string_view key);

struct SettingsError {
  std::string setting_id;
  std::string message;
};

// A WINEDLLOVERRIDES list split at ';', without empty entries.
std::vector<std::string> SplitDllOverrides(const std::string& list);
// The DXVK entry a generated recipe adds while graphics is "dxvk": it follows graphics, so the
// player can't remove it from the list.
bool IsGraphicsDllOverride(const GameRecipe& recipe, const std::string& entry);

bool BuildEffectiveSettings(const GameRecipe& recipe, const SettingsPreset* preset,
                            const std::map<std::string, std::string>& overrides, const KitCapabilities& kit,
                            EffectiveSettings* settings, SettingsError* error);

// Why one choice of a row cannot apply with the other effective values (Frame limit 120 and 40 need 120 Hz output),
// or empty when it can. BuildEffectiveSettings turns such a saved choice into the Playden default, with this reason.
// On a title that still runs an older runtime, a choice that needs the kit's (Frame limit 120 with 120 Hz output
// chosen) is not unavailable: it waits for the update (SettingChoiceWaitsForUpdate).
std::string SettingChoiceUnavailable(const EffectiveSettings& settings, std::string_view id, std::string_view value);
// Whether a choice SettingChoiceUnavailable allows applies only once the game's title runs the kit's runtime. It can
// be saved now; until then the row applies the Playden default (EffectiveValue::after_update).
bool SettingChoiceWaitsForUpdate(const EffectiveSettings& settings, std::string_view id, std::string_view value);

// Checks one player value for one setting; used by the UI before saving an override.
bool ValidateSettingValue(const GameRecipe& recipe, std::string_view id, std::string_view value, std::string* reason);

struct ProfileTarget {
  std::string id;            // prospero profile id, also the file stem
  std::string install_root;  // console path of the game files
};

// The full prospero profile: [application], [display], [input] and [debug].
bool RenderProsperoProfile(const GameRecipe& recipe, const EffectiveSettings& settings, const ProfileTarget& target,
                           ProsperoProfile* profile, std::string* reason);

// dxvk.conf for the game folder; empty when no DXVK option differs from DXVK's default.
std::string RenderDxvkConfig(const EffectiveSettings& settings);

// Writes what the next launch reads: <prospero_root>/profiles/<id>.profile (listed in
// profiles.lst) and <install_root>/dxvk.conf. An empty config removes Playden's dxvk.conf.
// A dxvk.conf Playden did not write (no Playden header) is never replaced or removed; the
// call fails with a reason instead. Call only while the game is not running.
bool ApplyGameSettings(const GameRecipe& recipe, const EffectiveSettings& settings, const ProfileTarget& target,
                       const std::string& prospero_root, std::string* reason);

// JSON data files: runtime/recipes/<id>.json and runtime/presets.json.
// The game a recipe is for: `game`, or by default the Steam game app_id names. Shipped and
// generated recipes are matched to a game by it, never by app_id alone.
GameId RecipeGame(const GameRecipe& recipe);
bool ParseGameRecipe(std::string_view json, GameRecipe* recipe, std::string* reason);
bool ParseSettingsPresets(std::string_view json, std::vector<SettingsPreset>* presets, std::string* reason);
bool ParseGameSettingsRecord(std::string_view json, GameSettingsRecord* record, std::string* reason);
// A Steam app ID as a steam_appid.txt or the "steam_app_id" setting holds it: canonical decimal
// 1..UINT32_MAX (no sign, no leading zero) once surrounding whitespace is trimmed; nothing otherwise.
std::optional<std::uint32_t> ParseSteamAppId(std::string_view text);
// The recipe as JSON that ParseGameRecipe reads back.
std::string SerializeGameRecipe(const GameRecipe& recipe);

// Games without a shipped recipe get one generated from Steam's Windows launch options.
// Its id is playden::GeneratedRecipeId(game); it starts the first launch entry with the game's own
// arguments, takes graphics and engine arguments from the game's files (game_probe.h) and
// locks nothing. With graphics "dxvk", BuildEffectiveSettings adds kDxvkOverrides.
// A Steam game's generated recipe id, "steam-<app id>-auto" (playden::GeneratedRecipeId).
std::string GeneratedRecipeId(std::uint32_t app_id);
// "steam-<app id>-auto" or "local-<uuid>-auto".
bool IsGeneratedRecipeId(std::string_view id);
// A community recipe (community-<app id>, docs/community-recipes.md). Like a generated one, it
// is not a shipped recipe, so it does not force the game's own Steam library.
bool IsCommunityRecipeId(std::string_view id);
// What the player chose in Game settings that changes how the recipe is generated.
struct RecipeChoices {
  std::string program;      // game-relative .exe to start (entry kProgramEntryId, first); empty: Steam's
  bool try_anyway = false;  // soft refusals (.NET, a shared section) do not skip a program
};
// The player's choices from a settings record.
RecipeChoices RecipeChoicesFrom(const GameSettingsRecord& record);

// Builds that recipe from Steam's launch options (default first). Entries whose executable is
// not a game-relative .exe are skipped. The architecture comes from the first usable entry's
// PE header under install_root; entries built for the other architecture are skipped, and so
// are programs the console cannot run (SteamStub, NW.js, Direct3D 12 only, and .NET or a
// shared section unless the player chose "Try anyway"). An Unreal root program from Steam is
// replaced by its *-Shipping.exe. The player's program comes first and Steam's entries stay
// as suggestions. On failure, reason is a short sentence for the player and refusal_soft
// says whether "Try anyway" or another program could get past it.
// What a refused recipe saw, for logs: the first SteamStub program and why it was not unpacked.
struct GameRecipeDiagnostics {
  std::string steam_stub_program;
  std::string steam_stub_detail;
};
bool GenerateGameRecipe(std::uint32_t app_id, const std::string& name, const std::vector<LaunchEntry>& launches,
                        const std::string& install_root, GameRecipe* recipe, std::string* reason,
                        const KitCapabilities& kit = {}, const RecipeChoices& choices = {},
                        bool* refusal_soft = nullptr, GameRecipeDiagnostics* diagnostics = nullptr);
// The same for any game: app_id is its runtime number (a Steam game's app ID), the id
// playden::GeneratedRecipeId(game).
bool GenerateGameRecipe(const GameId& game, std::uint32_t runtime_number, const std::string& name,
                        const std::vector<LaunchEntry>& launches, const std::string& install_root, GameRecipe* recipe,
                        std::string* reason, const KitCapabilities& kit = {}, const RecipeChoices& choices = {},
                        bool* refusal_soft = nullptr, GameRecipeDiagnostics* diagnostics = nullptr);

// One Windows program in a game folder, for the Program picker.
struct GameProgram {
  std::string path;          // relative to the install root, '/' separated, as spelled on disk
  std::string architecture;  // pe32 or pe64; empty: not a program the console can start
  std::string refusal;       // why the console cannot run it; empty when it may
  bool refusal_soft = false; // "Try anyway" can get past the refusal
  std::string detected;      // GameDefaults::summary
};
inline constexpr std::size_t kMaxGamePrograms = 32;
// The .exe files under install_root, up to six folders deep, without following symbolic links
// or entering redistributable folders; at most kMaxGamePrograms, shallowest first.
std::vector<GameProgram> ListGamePrograms(const std::string& install_root, const KitCapabilities& kit = {});
// "pe32" or "pe64" for a game-relative program under install_root (any case); empty when it
// is missing or not a Windows program.
std::string GameProgramArchitecture(const std::string& install_root, const std::string& relative);
// Why the console won't start this program (a sentence for the player), or empty when it may:
// a soft refusal (.NET, a shared section) counts only without "Try anyway".
std::string GameProgramRefusal(const std::string& install_root, const std::string& relative, const KitCapabilities& kit,
                               bool try_anyway);
std::string SerializeGameSettingsRecord(const GameSettingsRecord& record);

// Deletes the DXVK pipeline cache for the launch entry's executable (<install_root>/
// <exe stem>.dxvk-cache, next to where DXVK writes it from the working directory) and
// nothing else. Missing cache is success. Call only while the game is not running.
bool ClearShaderCache(const LaunchEntry& launch, const std::string& install_root, std::string* reason);

// The registry values the effective settings ask for: Windows version for the launch
// executable (AppDefaults) and text scale (Wine's LogPixels). Empty when the kit cannot
// edit the bottle.
std::vector<RegistryValue> RenderRegistryValues(const EffectiveSettings& settings);

// vkd3d-proton's own variables (VKD3D_DEBUG, VKD3D_CONFIG, ...), which neither profile section takes. Environment
// variables accepts them and they reach the game through the bottle's HKEY_CURRENT_USER\Environment, which Wine adds
// to the environment of the program it starts.
bool IsBottleEnvironmentEntry(std::string_view name, std::string_view value);

// The bottle's Environment values for those variables: each one Environment variables lists is set, every other one
// is removed, so a variable taken out of the row leaves the bottle as well.
std::vector<RegistryValue> RenderBottleEnvironment(const EffectiveSettings& settings);

// Applies values to Wine's user.reg text, keeping every other line. Sections Playden adds
// carry the given Unix time. Returns false for text that is not a Wine registry file.
bool ApplyRegistryValues(std::string* user_reg, const std::vector<RegistryValue>& values, std::int64_t unix_time,
                         std::string* reason);

// Edits <prefix>/user.reg in place: copies it to user.reg.playden-backup first, then replaces
// it atomically. Call only while the bottle is stopped (no wineserver).
bool ApplyRegistrySettings(const EffectiveSettings& settings, const std::string& prefix_dir, std::int64_t unix_time,
                           std::string* reason);

// Applies values to one hive of the stopped bottle (user.reg or system.reg) the same way:
// backed up to <hive>.playden-backup first, replaced atomically, untouched when nothing changes.
bool ApplyBottleRegistryValues(const std::vector<RegistryValue>& values, const std::string& prefix_dir,
                               std::int64_t unix_time, const std::string& hive, std::string* reason);

// Merge the staged kit media values into a stopped bottle, and copy the kit's winegstreamer.dll into its system32
// and syswow64 when missing (Wine finds a builtin only through that file). Older kits without the file are unchanged.
bool ApplyMediaRegistry(const std::string& runtime_dir, const std::string& prefix_dir,
                        std::int64_t unix_time, std::string* reason);

// Puts the Direct3D 12 DLLs a game asked for in the stopped bottle's system32 and syswow64: vkd3d-proton's from
// <runtime_dir>/lib/vkd3d-proton when vkd3d_proton is set (the profile's overrides then load them as native), and
// otherwise the kit template's own copies back, but only where vkd3d-proton was put (a marker file next to the DLLs,
// or the current kit's bytes), so bottles that never used it are not touched. Every file is read before any is
// written, and files that already match are not rewritten. A bottle that does not exist yet is left for later.
bool ApplyD3D12Library(const std::string& runtime_dir, const std::string& template_dir, const std::string& prefix_dir,
                       bool vkd3d_proton, std::string* reason);

// Points the stopped bottle's Wine Mono (HKCU\Software\Wine\Mono RuntimePath) where the kit template's points: a
// bottle copied from an earlier kit names that kit's folder, which a renamed kit (or another Mono version) no longer
// has. Only a value that names a folder under /data/playden/kits changes; a bottle without the value (made before
// the kit had Mono), a template without it, or a bottle not made yet is left as it is. changed tells whether
// user.reg was written (with a user.reg.playden-backup, as ApplyBottleRegistryValues does).
bool PointBottleAtKitMono(const std::string& template_dir, const std::string& prefix_dir, std::int64_t unix_time,
                          bool* changed, std::string* reason);

// The <InstallGuidString> of an Unreal Engine 3 (UDK) Binaries/InstallInfo.xml: exactly one,
// a 36-character 8-4-4-4-12 hex GUID once surrounding whitespace is trimmed.
bool ParseUe3InstallGuid(std::string_view install_info, std::string* guid);

// A UDK game (launch program in Binaries/Win32 or Binaries/Win64, InstallInfo.xml and
// UnSetup.exe in Binaries) asks for its EULA through UnSetup.exe on first start, and the PS5
// cannot start a second program. This marks the EULA accepted in the bottle's system.reg
// (HKLM Uninstall\UDK-<guid>, both registry views), backing the hive up first. Idempotent;
// the bottle must be stopped. found is false for other games, which are left alone.
struct Ue3EulaResult {
  bool found = false;
  bool ok = true;
  bool written = false;
  std::string guid;
  std::string message;  // on failure
};
Ue3EulaResult AcceptUe3Eula(const std::string& game_root, const std::vector<std::string>& programs,
                            const std::string& prefix_dir, std::int64_t unix_time);

// A prospero-win-profiles sidecar (sidecars/<id>.json, schema_version 1) as a preset for
// the game it describes: "Community · <name>", its newest console check as the hint, and
// only the settings this build knows and accepts. Returns false for other schema versions.
struct CommunityPreset {
  SettingsPreset preset;
  std::uint32_t steam_appid = 0;  // 0 when the sidecar names none
  std::string profile_id;
  std::string rating;             // newest check: works, playable or broken
  std::vector<std::string> requires_prospero_win;
};
bool ParseProfileSidecar(std::string_view json, CommunityPreset* community, std::string* reason);

// A game's effective settings in the community recipe format (docs/community-recipes.md):
// a Playden recipe that ParseGameRecipe reads, plus who tested it where.
struct CommunityRecipe {
  GameRecipe recipe;  // id community-<app id>; the effective program, arguments, overrides and settings
  std::string executable;       // identity: the program, relative to the install root
  std::string min_kit;          // the runtime kit it was tested with
  std::string tested_firmware;  // PS5 system software, when known
  std::string tested_date;      // YYYY-MM-DD
  std::string status;           // works, playable, broken or untested
  std::string notes;
  std::string author;
  // Optional prospero-win-profiles profile used as the base; Playden's values apply on top.
  std::string base_repository;
  std::string base_path;
  std::string base_revision;
};
// The recipe the effective settings describe. Only values that differ from Playden's default
// and rows the runtime applies become defaults; rows the recipe locks stay locked unless the
// player changed them anyway.
CommunityRecipe BuildCommunityRecipe(const GameRecipe& recipe, const EffectiveSettings& settings);
std::string SerializeCommunityRecipe(const CommunityRecipe& community);
bool ParseCommunityRecipe(std::string_view json, CommunityRecipe* community, std::string* reason);

// Whether the kit's runtime has every newer profile feature the sidecar declares in
// requires.prospero_win: the kit capability names without "settings-" (profile-environment,
// input-tuning, input-wheel, audio-volume; see prospero-win-profiles SIDECARS.md) and display-120hz, which needs the
// game's own title on such a runtime: pass the kit as KitForTitleRuntime gives it for the game. Other entries
// (pull requests, versions) are not checked.
bool CommunityPresetSupported(const CommunityPreset& community, const KitCapabilities& kit);

// --- Integration contract (roadmap S4, integration Tasks 4-6 and 8) ---

// Capability names a runtime kit manifest may list for settings ("capabilities", sorted).
// The kit trust config should allow exactly these besides its own runtime names.
inline constexpr const char* kKitDxvkConfig = "settings-dxvk-config";        // DXVK reads dxvk.conf
// A kit that ships DXVK: DXVK reads dxvk.conf from the working directory, which prospero-win sets to the game's.
inline constexpr const char* kKitDxvk = "dxvk";
inline constexpr const char* kKitDxvkPresent = "settings-dxvk-present";      // VSync off honored
inline constexpr const char* kKitEnvironment = "settings-profile-environment";  // [environment]
inline constexpr const char* kKitInputTuning = "settings-input-tuning";      // stick/trigger dead zones, rumble
inline constexpr const char* kKitInputWheel = "settings-input-wheel";        // [input] wheel
inline constexpr const char* kKitAudioVolume = "settings-audio-volume";      // [audio] volume
// Runtime ability (not a setting): games with a shared section, such as GameMaker's, can start.
inline constexpr const char* kKitSharedSections = "wine-shared-sections";
// Runtime ability: the kit carries one title shell for every game (an any-title variant), so
// Playden makes a new PS5 title per game instead of using a fixed set of titles.
inline constexpr const char* kKitTitleAnyId = "title-any-id";
// Runtime ability: launch admission accepts a game folder on a drive that records no file
// owners or permissions, such as an exFAT USB disk (publication metadata schema 3).
inline constexpr const char* kKitGameRootUnowned = "game-root-unowned";
// Runtime ability: launch admission accepts 0711 as well as 0700 for the bottle's way to its user
// profile (games/<id>, library, library/prefixes, library/prefixes/game), so FTP can reach the saves.
inline constexpr const char* kKitBottleProfileShared = "bottle-profile-shared";
// Runtime ability: OpenGL games draw through Mesa's WGL/Zink on Vulkan (prospero-win upstream #463-#465)
// instead of the PS5 OpenGL SDK, which had the 120 Hz output and the OpenGL worker thread.
inline constexpr const char* kKitOpenGLZink = "opengl-zink";
// Runtime ability: the kit carries Wine Mono (at mono/wine-mono-<version>) and its template bottle points
// Wine's mscoree at it, so .NET and mixed C++/CLI programs can start.
inline constexpr const char* kKitWineMono = "wine-mono";
// Runtime ability: the kit's runtime carries vkd3d-proton's d3d12.dll and d3d12core.dll at
// lib/vkd3d-proton/<arch>-windows, for games that draw with Direct3D 12 (with DXVK's dxgi).
inline constexpr const char* kKitVkd3dProton = "vkd3d-proton";
// The runtime honors [display] refresh = 60|120 on every graphics path (Wine reports the rate it asked for) and
// [display] frame_rate = 0|120|60|40|30, which the display paces. Runtimes without it refuse a profile with frame_rate.
inline constexpr const char* kKitDisplay120Hz = "display-120hz";
// The title reads [runtime] version: version 2 takes the driver's defaults (pixel shaders in wave32), a profile without
// it (version 1) keeps wave64. Runtimes without it refuse a profile with [runtime] version.
inline constexpr const char* kKitProfileVersion2 = "profile-version-2";
const std::vector<std::string>& SettingsKitCapabilityNames();

// Settings gates from a verified kit's capabilities. bottle_access: the adapter can edit the
// stopped bottle's user.reg (decision D2); it is not a kit property.
KitCapabilities KitCapabilitiesFromManifest(const std::vector<std::string>& capabilities, bool bottle_access);

// The installed kit's capabilities for one game's title. A prepared title keeps its own copy of the runtime until
// Update, so a capability that adds profile keys counts only when the title runs this kit's runtime
// (title_runtime_current: the title's owner markers name this kit, PackageInstaller::TitleCurrent). Otherwise
// display_120hz and profile_version_2 are off and their _needs_update say why.
KitCapabilities KitForTitleRuntime(KitCapabilities kit, bool title_runtime_current);

// Everything one game's settings put on disk for its next launch.
struct SettingsLaunchPlan {
  std::uint32_t app_id = 0;
  std::string recipe_id;
  std::string recipe_version;
  std::string profile_id;
  std::string profile_bytes;             // full prospero profile, checked for the kit's features
  std::uint64_t settings_revision = 0;   // GameSettingsRecord::revision it was built from
  std::string launch_entry_id;
  // Input preset files the profile references ([input] preset), as <name>.input and bytes.
  std::vector<std::pair<std::string, std::string>> input_presets;
  std::string dxvk_config;               // empty: no Playden dxvk.conf (an old one is removed)
  std::vector<RegistryValue> registry;   // empty unless the kit has bottle access
  LaunchEntry launch_entry;             // owned snapshot; recipe reloads cannot change the plan
};

struct SettingsLaunchInput {
  const GameRecipe* recipe = nullptr;
  const SettingsPreset* preset = nullptr;  // exact record preset; empty record accepts null or empty playden-default
  GameSettingsRecord record;
  KitCapabilities kit;
  ProfileTarget target;     // profile id and console path of the game files
  std::string runtime_dir;  // bundled settings data; input presets are read from <runtime_dir>/input
};

// Builds the plan, or false with a reason (invalid override, missing input preset, a profile
// the kit's runtime would refuse). Pure apart from reading the input preset files.
bool BuildSettingsLaunchPlan(const SettingsLaunchInput& input, SettingsLaunchPlan* plan, std::string* reason);

// Writes the plan's game-folder and bottle files: dxvk.conf in the launch working folder
// (never replacing one Playden did not write) and the registry values in <prefix_dir>/user.reg
// (with a backup). If the registry edit fails, dxvk.conf is put back; should that rewrite itself
// fail, the folder keeps the new dxvk.conf and the call still returns false. The profile is not written here: the integration publishes profile_bytes
// through its journaled profile/descriptor publication. Call only while the title is stopped.
// Writes include file/directory durability barriers and preserve file modes. This helper
// is not a cross-directory transaction: the owner must journal apply/publication and
// block readiness on failure/interruption until reconciled. A failed config rollback
// returns a distinct reconciliation diagnostic; false does not promise unchanged files.
bool ApplySettingsFiles(const SettingsLaunchPlan& plan, const std::string& install_root, const std::string& prefix_dir,
                        std::int64_t unix_time, std::string* reason);

// Per-game records in one small JSON file ({"version":1,"games":{"<key>":record}}), keyed by
// ToKey: a Steam game's app ID, "local-<uuid>" for an imported game. A missing file loads as no
// records. Saving replaces the file atomically.
bool LoadGameSettingsFile(const std::string& path, std::map<GameId, GameSettingsRecord>* records,
                          std::string* reason);
bool SaveGameSettingsFile(const std::string& path, const std::map<GameId, GameSettingsRecord>& records,
                          std::string* reason);
// One-time import from the legacy approved state tree into an already-created
// private settings root. The owned legacy file must be 0600/0644; the private
// destination must be absent and is published as 0600. The source is preserved.
bool MigrateLegacyGameSettingsFile(const std::string& legacy_path,
                                   const std::string& private_path,
                                   std::string* reason);

// Loads <runtime_dir>/presets.json and the recipes listed in <runtime_dir>/recipes/recipes.lst.
// The console title cannot list folders, so recipes are found through the list file.
bool LoadSettingsData(const std::string& runtime_dir, std::vector<SettingsPreset>* presets,
                      std::vector<GameRecipe>* recipes, std::string* reason);

}  // namespace playden::runtime
