#pragma once

#include "platform/storage.h"

#include <cstddef>
#include <string>
#include <string_view>

namespace playden {

// Votes on community recipes: not asked yet (the first prompt asks), on, or off.
enum class RecipeVotesPreference { kUnset, kOn, kOff };

// The longest name a shared recipe credits (the Playden API's limit), in characters.
inline constexpr std::size_t kShareNameMaxCharacters = 40;
// Text a player typed for a shared recipe: at most max_characters characters the Playden API accepts
// (community::IsSubmissionText: valid UTF-8, no control, zero-width, bidi or unusual space characters).
bool IsPrintableText(std::string_view text, std::size_t max_characters);
// A name Playden keeps and sends: empty, or 1-40 such characters.
inline bool IsShareName(std::string_view name) { return IsPrintableText(name, kShareNameMaxCharacters); }

enum class ShareAs { kNone, kName, kAccount };

struct Preferences {
  bool reduce_motion = false;
  // PS5 titles made for games are named "<game> (<store>)", e.g. "Half-Life 2 (Steam)".
  bool title_store_suffix = false;
  // Cloud saves move on their own: before Play, after a session, at startup and before an
  // uninstall. Off (the default, so a fresh install never changes Cloud saves unasked) leaves only
  // the Sync actions the player chooses.
  bool cloud_saves_auto = false;
  // The sign-in screen opens on the account name and password form: the last sign-in used it.
  bool steam_sign_in_password = false;
  // Downloads for slow networks: 8 pieces at a time instead of 4. Far from a fast Steam server each piece waits
  // long for its answer, and more of them in flight make up for it (#29); on a fast connection 4 is quicker.
  bool downloads_slow_network = false;
  // "Stay signed in" on the sign-in screen. Off, Steam's sign-in token stays in memory only and is never
  // saved on the PS5, so every Playden start asks for a sign-in. On (the default) keeps it in session.v1.
  bool steam_stay_signed_in = true;
  bool beta_updates = false;
  // The Steam CDN host to try first for new or resumed downloads. Empty uses Steam's server order (Automatic).
  std::string download_server;
  // Web access: Playden's logs, redacted, on the local network at port 7031 while Playden is open. On by default,
  // so a tester can send logs without first finding a switch; version 1 only reads.
  bool web_access = true;
  // The language game content is downloaded in, as Steam names its depot languages
  // ("german", "koreana"). Steam ships localisations as separate depots; a game that has none
  // in this language is installed in English, as every game was before this could be chosen.
  std::string content_language = "english";
  // "Don't show this again" on the dialogs that explain launcher mode: Run launcher or setup, and turning on
  // Start through a launcher. Separate, because the second warns about other games.
  bool hide_run_once_help = false;
  bool hide_launcher_help = false;
  // The runtime kit whose "Update your games" prompt has been shown (its manifest SHA-256): once per kit.
  std::string runtime_prompt_kit;
  // Community recipes (version 13). Whether Playden asks for and sends votes on the recipes games used.
  RecipeVotesPreference recipe_votes = RecipeVotesPreference::kUnset;
  // The first share showed what a share sends; later shares go straight to Send.
  bool sharing_explained = false;
  // "Your name" on the Share sheet, remembered for the next share; empty: the recipe has no author ("from the community").
  std::string share_name;
  // The first game test report showed what a report sends (version 14).
  bool reports_explained = false;
  // How shares and test reports credit the player (version 14): no name, the typed name, or the linked Playden account.
  ShareAs share_as = ShareAs::kNone;
  // Whether Playden asks how a game ran after it ends (version 14). Separate from recipe_votes: "Don't ask again"
  // stops the question, not votes. Older files: asked unless recipe votes were off.
  bool ask_after_games = true;
};

enum class PreferencesLoadStatus {
  kLoaded,
  kDefaultMissing,
  kDefaultCorrupt,
  kDefaultReadError,
};

struct PreferencesLoadResult {
  Preferences preferences;
  PreferencesLoadStatus status = PreferencesLoadStatus::kLoaded;
  std::string diagnostic;
};

class PreferencesStore {
 public:
  explicit PreferencesStore(Storage& storage);

  PreferencesLoadResult Load() const;
  Result<void> Save(const Preferences& preferences) const;

 private:
  Storage& storage_;
};

}  // namespace playden
