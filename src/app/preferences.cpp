#include "app/preferences.h"

#include "steam/backend/content_language.h"

#include <string>

namespace playden {
namespace {

constexpr char kPreferencesPath[] = "preferences.conf";

std::string ToString(const Bytes& bytes) {
  return std::string(bytes.begin(), bytes.end());
}


}  // namespace

PreferencesStore::PreferencesStore(Storage& storage) : storage_(storage) {}

PreferencesLoadResult PreferencesStore::Load() const {
  const auto ready=storage_.EnsureRootDirectory();
  if(!ready.ok()) return {Preferences{},PreferencesLoadStatus::kDefaultReadError,"Preference storage is unavailable."};
  const auto read = storage_.Read(kPreferencesPath);
  if (!read.ok()) {
    if (read.error() == StorageError::kNotFound) {
      return PreferencesLoadResult{Preferences{},
                                   PreferencesLoadStatus::kDefaultMissing,
                                   std::string()};
    }
    return PreferencesLoadResult{
        Preferences{}, PreferencesLoadStatus::kDefaultReadError,
        "Preferences could not be read; defaults restored: " + read.message()};
  }

  std::string serialized = ToString(read.value());
  // Version 8 appends content_language. Peel it off and let the version 7 reader below take over.
  std::string content_language{steam::kDefaultContentLanguage};
  if (serialized.rfind("version=8\n", 0) == 0) {
    const auto suffix = serialized.rfind("\ncontent_language=");
    if (suffix == std::string::npos || serialized.empty() || serialized.back() != '\n')
      return {Preferences{}, PreferencesLoadStatus::kDefaultCorrupt, "Preferences could not be read; defaults restored."};
    const auto value = serialized.substr(suffix + 18, serialized.size() - suffix - 19);
    // Only a language Steam names: anything else would pick depots that do not exist.
    if (!steam::IsContentLanguage(value))
      return {Preferences{}, PreferencesLoadStatus::kDefaultCorrupt, "Preferences could not be read; defaults restored."};
    content_language = value;
    serialized.erase(suffix + 1);
    serialized.replace(0, 9, "version=7");
  }
  bool beta_updates = false;
  if (serialized.rfind("version=7\n", 0) == 0) {
    const auto suffix = serialized.rfind("beta_updates=");
    if (suffix == std::string::npos || (serialized.substr(suffix) != "beta_updates=0\n" &&
                                      serialized.substr(suffix) != "beta_updates=1\n"))
      return {Preferences{}, PreferencesLoadStatus::kDefaultCorrupt, "Preferences could not be read; defaults restored."};
    beta_updates = serialized.substr(suffix) == "beta_updates=1\n";
    serialized.erase(suffix);
    serialized.replace(0, 9, "version=6");
  }
  // Version 1 holds only reduce_motion; version 2 adds title_store_suffix; version 3 cloud_saves_auto;
  // version 4 steam_sign_in_password; version 5 downloads_slow_network; version 6 steam_stay_signed_in
  // (on in files older than that). Version 7 appends beta_updates (off in older files); version 8
  // appends content_language (English in older files, which is what they downloaded).
  for (const int motion : {0, 1}) {
    if (serialized == std::string("version=1\nreduce_motion=") + char('0' + motion) + "\n") {
      Preferences preferences;
      preferences.reduce_motion = motion == 1;
      return PreferencesLoadResult{preferences, PreferencesLoadStatus::kLoaded, std::string()};
    }
    for (const int suffix : {0, 1})
      if (serialized == std::string("version=2\nreduce_motion=") + char('0' + motion) +
                            "\ntitle_store_suffix=" + char('0' + suffix) + "\n") {
        Preferences preferences;
        preferences.reduce_motion = motion == 1;
        preferences.title_store_suffix = suffix == 1;
        return PreferencesLoadResult{preferences, PreferencesLoadStatus::kLoaded, std::string()};
      }
    for (const int suffix : {0, 1})
      for (const int cloud : {0, 1})
        if (serialized == std::string("version=3\nreduce_motion=") + char('0' + motion) +
                              "\ntitle_store_suffix=" + char('0' + suffix) + "\ncloud_saves_auto=" + char('0' + cloud) + "\n") {
          Preferences preferences;
          preferences.reduce_motion = motion == 1;
          preferences.title_store_suffix = suffix == 1;
          preferences.cloud_saves_auto = cloud == 1;
          return PreferencesLoadResult{preferences, PreferencesLoadStatus::kLoaded, std::string()};
        }
    for (const int suffix : {0, 1})
      for (const int cloud : {0, 1})
        for (const int password : {0, 1})
          if (serialized == std::string("version=4\nreduce_motion=") + char('0' + motion) +
                                "\ntitle_store_suffix=" + char('0' + suffix) + "\ncloud_saves_auto=" + char('0' + cloud) +
                                "\nsteam_sign_in_password=" + char('0' + password) + "\n") {
            Preferences preferences;
            preferences.reduce_motion = motion == 1;
            preferences.title_store_suffix = suffix == 1;
            preferences.cloud_saves_auto = cloud == 1;
            preferences.steam_sign_in_password = password == 1;
            return PreferencesLoadResult{preferences, PreferencesLoadStatus::kLoaded, std::string()};
          }
    for (const int suffix : {0, 1})
      for (const int cloud : {0, 1})
        for (const int password : {0, 1})
          for (const int slow : {0, 1})
            if (serialized == std::string("version=5\nreduce_motion=") + char('0' + motion) +
                                  "\ntitle_store_suffix=" + char('0' + suffix) + "\ncloud_saves_auto=" + char('0' + cloud) +
                                  "\nsteam_sign_in_password=" + char('0' + password) +
                                  "\ndownloads_slow_network=" + char('0' + slow) + "\n") {
              Preferences preferences;
              preferences.reduce_motion = motion == 1;
              preferences.title_store_suffix = suffix == 1;
              preferences.cloud_saves_auto = cloud == 1;
              preferences.steam_sign_in_password = password == 1;
              preferences.downloads_slow_network = slow == 1;
              return PreferencesLoadResult{preferences, PreferencesLoadStatus::kLoaded, std::string()};
            }
    for (const int suffix : {0, 1})
      for (const int cloud : {0, 1})
        for (const int password : {0, 1})
          for (const int slow : {0, 1})
            for (const int stay : {0, 1})
              if (serialized == std::string("version=6\nreduce_motion=") + char('0' + motion) +
                                    "\ntitle_store_suffix=" + char('0' + suffix) + "\ncloud_saves_auto=" + char('0' + cloud) +
                                    "\nsteam_sign_in_password=" + char('0' + password) +
                                    "\ndownloads_slow_network=" + char('0' + slow) +
                                    "\nsteam_stay_signed_in=" + char('0' + stay) + "\n") {
                Preferences preferences;
                preferences.reduce_motion = motion == 1;
                preferences.title_store_suffix = suffix == 1;
                preferences.cloud_saves_auto = cloud == 1;
                preferences.steam_sign_in_password = password == 1;
                preferences.downloads_slow_network = slow == 1;
                preferences.steam_stay_signed_in = stay == 1;
                preferences.beta_updates = beta_updates;
                preferences.content_language = content_language;
                return PreferencesLoadResult{preferences, PreferencesLoadStatus::kLoaded, std::string()};
              }
  }
  return PreferencesLoadResult{
      Preferences{}, PreferencesLoadStatus::kDefaultCorrupt,
      "Preferences file is corrupt; defaults restored."};
}

Result<void> PreferencesStore::Save(const Preferences& preferences) const {
  const auto ready=storage_.EnsureRootDirectory();
  if(!ready.ok()) return ready;
  const std::string text = std::string("version=8\nreduce_motion=") + (preferences.reduce_motion ? "1" : "0") +
                           "\ntitle_store_suffix=" + (preferences.title_store_suffix ? "1" : "0") +
                           "\ncloud_saves_auto=" + (preferences.cloud_saves_auto ? "1" : "0") +
                           "\nsteam_sign_in_password=" + (preferences.steam_sign_in_password ? "1" : "0") +
                           "\ndownloads_slow_network=" + (preferences.downloads_slow_network ? "1" : "0") +
                           "\nsteam_stay_signed_in=" + (preferences.steam_stay_signed_in ? "1" : "0") +
                           "\nbeta_updates=" + (preferences.beta_updates ? "1" : "0") + "\ncontent_language=" +
                           (steam::IsContentLanguage(preferences.content_language)
                                ? preferences.content_language
                                : std::string(steam::kDefaultContentLanguage)) +
                           "\n";
  return storage_.WriteAtomic(kPreferencesPath, Bytes(text.begin(), text.end()));
}

}  // namespace playden
