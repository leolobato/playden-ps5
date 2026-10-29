#pragma once

#include "platform/storage.h"

#include <string>

namespace playden {

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
  // The UI language, as the code of a catalog in assets/translations ("en", "es"). An unknown
  // code falls back to English at load.
  std::string language = "en";
  // The Steam CDN host to try first for new or resumed downloads. Empty uses Steam's server order (Automatic).
  std::string download_server;
  // Web access: Playden's logs, redacted, on the local network at port 7031 while Playden is open. On by default,
  // so a tester can send logs without first finding a switch; version 1 only reads.
  bool web_access = true;
  // The language game content is downloaded in, as Steam names its depot languages
  // ("german", "koreana"). Steam ships localisations as separate depots; a game that has none
  // in this language is installed in English, as every game was before this could be chosen.
  std::string content_language = "english";
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
