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
