#include "app/preferences.h"

#include "steam/backend/content_language.h"

#include <algorithm>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace playden {
namespace {

constexpr char kPreferencesPath[] = "preferences.conf";

std::string ToString(const Bytes& bytes) {
  return std::string(bytes.begin(), bytes.end());
}

// Versions 8 to 10: one "key=value" line per field, in this order, each ending in a newline. Version 9 adds
// web_access (on in older files), version 10 content_language (English in older files, which is what they
// downloaded).
constexpr const char* kVersionTenKeys[] = {"reduce_motion", "title_store_suffix", "cloud_saves_auto",
                                          "steam_sign_in_password", "downloads_slow_network", "steam_stay_signed_in",
                                          "beta_updates", "download_server", "web_access", "content_language"};

bool ParseFlag(const std::string& value, bool* flag) {
  if (value != "0" && value != "1") return false;
  *flag = value == "1";
  return true;
}

bool ValidDownloadServer(const std::string& host) {
  if (host.empty() || host.size() > 253) return false;
  return std::all_of(host.begin(), host.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.';
  });
}

bool ParseKeyed(const std::string& serialized, int version, Preferences* preferences) {
  const std::string header = "version=" + std::to_string(version) + "\n";
  if (serialized.compare(0, header.size(), header) != 0 || serialized.back() != '\n') return false;
  std::vector<std::string> values;
  std::size_t start = header.size();
  const std::size_t count = std::size(kVersionTenKeys) - static_cast<std::size_t>(10 - version);
  for (std::size_t field = 0; field < count; ++field) {
    const char* key = kVersionTenKeys[field];
    const std::string prefix = std::string(key) + "=";
    const auto end = serialized.find('\n', start);
    if (end == std::string::npos || serialized.compare(start, prefix.size(), prefix) != 0) return false;
    values.push_back(serialized.substr(start + prefix.size(), end - start - prefix.size()));
    start = end + 1;
  }
  if (start != serialized.size()) return false;
  Preferences parsed;
  if (!ParseFlag(values[0], &parsed.reduce_motion) || !ParseFlag(values[1], &parsed.title_store_suffix) ||
      !ParseFlag(values[2], &parsed.cloud_saves_auto) || !ParseFlag(values[3], &parsed.steam_sign_in_password) ||
      !ParseFlag(values[4], &parsed.downloads_slow_network) || !ParseFlag(values[5], &parsed.steam_stay_signed_in) ||
      !ParseFlag(values[6], &parsed.beta_updates) || (version >= 9 && !ParseFlag(values[8], &parsed.web_access)))
    return false;
  // Only a language Steam names: anything else would pick depots that do not exist.
  if (version >= 10) {
    if (!steam::IsContentLanguage(values[9])) return false;
    parsed.content_language = values[9];
  }
  // A host the current rule refuses is dropped without resetting unrelated preferences.
  if (ValidDownloadServer(values[7])) parsed.download_server = values[7];
  *preferences = std::move(parsed);
  return true;
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
  // Version 8 appends language. Peel it off and let the version 7 reader below take over.
  std::string language = "en";
  if (serialized.rfind("version=8\n", 0) == 0) {
    const auto suffix = serialized.rfind("\nlanguage=");
    if (suffix == std::string::npos || serialized.empty() || serialized.back() != '\n' ||
        suffix + 11 > serialized.size())
      return {Preferences{}, PreferencesLoadStatus::kDefaultCorrupt, "Preferences could not be read; defaults restored."};
    language = serialized.substr(suffix + 10, serialized.size() - suffix - 11);
    const bool named = !language.empty() && language.size() <= 16 &&
                       language.find_first_not_of("abcdefghijklmnopqrstuvwxyz-") == std::string::npos;
    if (!named)
      return {Preferences{}, PreferencesLoadStatus::kDefaultCorrupt, "Preferences could not be read; defaults restored."};
    serialized.erase(suffix + 1);
    serialized.replace(0, 9, "version=7");
  }
  Preferences keyed;
  if (ParseKeyed(serialized, 10, &keyed) || ParseKeyed(serialized, 9, &keyed) || ParseKeyed(serialized, 8, &keyed))
    return PreferencesLoadResult{std::move(keyed), PreferencesLoadStatus::kLoaded, std::string()};
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
  // (on in files older than that). Version 7 appends beta_updates (off in older files); version 8 (above)
  // adds the download server (Automatic in older files), version 9 web access (on in older files), version 10
  // the content language (English in older files).
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
                preferences.language = language;
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
  // Never write a host that cannot be read back, including embedded newlines.
  const std::string server = ValidDownloadServer(preferences.download_server) ? preferences.download_server : std::string();
  const std::string text = std::string("version=10\nreduce_motion=") + (preferences.reduce_motion ? "1" : "0") +
                           "\ntitle_store_suffix=" + (preferences.title_store_suffix ? "1" : "0") +
                           "\ncloud_saves_auto=" + (preferences.cloud_saves_auto ? "1" : "0") +
                           "\nsteam_sign_in_password=" + (preferences.steam_sign_in_password ? "1" : "0") +
                           "\ndownloads_slow_network=" + (preferences.downloads_slow_network ? "1" : "0") +
                           "\nsteam_stay_signed_in=" + (preferences.steam_stay_signed_in ? "1" : "0") +
                           "\nbeta_updates=" + (preferences.beta_updates ? "1" : "0") +
                           "\nlanguage=" + (preferences.language.empty() ? "en" : preferences.language) + "\n";
                           "\ndownload_server=" + server +
                           "\nweb_access=" + (preferences.web_access ? "1" : "0") + "\ncontent_language=" +
                           (steam::IsContentLanguage(preferences.content_language)
                                ? preferences.content_language
                                : std::string(steam::kDefaultContentLanguage)) +
                           "\n";
  return storage_.WriteAtomic(kPreferencesPath, Bytes(text.begin(), text.end()));
}

}  // namespace playden
