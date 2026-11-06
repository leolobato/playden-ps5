#include "app/preferences.h"

#include "community/printable_text.h"
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

// Versions 8 to 11: one "key=value" line per field, in this order, each ending in a newline. Version 9 adds
// web_access (on in older files), version 10 content_language (English in older files, which is what they
// downloaded), version 11 the launcher dialogs' "Don't show this again" (shown in older files), version 12 the kit
// whose "Update your games" prompt was shown (none in older files, so they are offered it), version 13 the
// community recipe fields (votes unasked, sharing not explained, no share name in older files), version 14 whether
// the first test report said what it sends (not in older files) and how shares credit the player (share_as: none,
// name or account; older files: their name when they kept one, else none).
constexpr const char* kKeys[] = {"reduce_motion", "title_store_suffix", "cloud_saves_auto",
                                          "steam_sign_in_password", "downloads_slow_network", "steam_stay_signed_in",
                                          "beta_updates", "download_server", "web_access", "content_language",
                                          "hide_run_once_help", "hide_launcher_help", "runtime_prompt_kit",
                                          "recipe_votes", "sharing_explained", "share_name", "reports_explained",
                                          "share_as"};

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

bool ParseKeyed(const std::string& serialized, int version, Preferences* preferences, bool without_share_as = false) {
  const std::string header = "version=" + std::to_string(version) + "\n";
  if (serialized.compare(0, header.size(), header) != 0 || serialized.back() != '\n') return false;
  std::vector<std::string> values;
  std::size_t start = header.size();
  // Version 11 added two fields, version 13 three and version 14 two; the others one each from version 8's eight.
  // A version 14 file from before share_as (a development build) has one field less; it reads as version 13 did.
  const std::size_t count = static_cast<std::size_t>(version >= 14 ? version + (without_share_as ? 3 : 4)
                                                     : version >= 13 ? version + 3 : version >= 11 ? version + 1 : version);
  for (std::size_t field = 0; field < count; ++field) {
    const char* key = kKeys[field];
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
  if (version >= 11 && (!ParseFlag(values[10], &parsed.hide_run_once_help) || !ParseFlag(values[11], &parsed.hide_launcher_help)))
    return false;
  // Anything but a SHA-256 is dropped: the prompt is offered again, and nothing else resets.
  if (version >= 12 && values[12].size() == 64 &&
      std::all_of(values[12].begin(), values[12].end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }))
    parsed.runtime_prompt_kit = values[12];
  // A host the current rule refuses is dropped without resetting unrelated preferences.
  if (ValidDownloadServer(values[7])) parsed.download_server = values[7];
  if (version >= 13) {
    if (!ParseFlag(values[14], &parsed.sharing_explained)) return false;
    if (values[13] == "0") parsed.recipe_votes = RecipeVotesPreference::kUnset;
    else if (values[13] == "1") parsed.recipe_votes = RecipeVotesPreference::kOn;
    else if (values[13] == "2") parsed.recipe_votes = RecipeVotesPreference::kOff;
    else return false;
    // A name the current rule refuses (an older, looser check let it in) is dropped, not the file.
    if (IsShareName(values[15])) parsed.share_name = values[15];
  }
  parsed.share_as = parsed.share_name.empty() ? ShareAs::kNone : ShareAs::kName;
  if (version >= 14) {
    if (!ParseFlag(values[16], &parsed.reports_explained)) return false;
    if (without_share_as) {
      *preferences = std::move(parsed);
      return true;
    }
    if (values[17] == "none") parsed.share_as = ShareAs::kNone;
    else if (values[17] == "name") parsed.share_as = ShareAs::kName;
    else if (values[17] == "account") parsed.share_as = ShareAs::kAccount;
    else return false;
  }
  *preferences = std::move(parsed);
  return true;
}

}  // namespace

bool IsPrintableText(std::string_view text, std::size_t max_characters) {
  return community::IsSubmissionText(text, 0, max_characters);
}

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
  Preferences keyed;
  if (ParseKeyed(serialized, 14, &keyed) || ParseKeyed(serialized, 14, &keyed, true) || ParseKeyed(serialized, 13, &keyed) || ParseKeyed(serialized, 12, &keyed) || ParseKeyed(serialized, 11, &keyed) || ParseKeyed(serialized, 10, &keyed) || ParseKeyed(serialized, 9, &keyed) || ParseKeyed(serialized, 8, &keyed))
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
  // the content language (English in older files), and the keyed versions after it (ParseKeyed) more fields.
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
  // A name that could not be read back is not written: the recipe then has no author and shows as "from the community".
  const std::string name = IsShareName(preferences.share_name) ? preferences.share_name : std::string();
  const char* votes = preferences.recipe_votes == RecipeVotesPreference::kOn ? "1"
                    : preferences.recipe_votes == RecipeVotesPreference::kOff ? "2" : "0";
  const std::string text = std::string("version=14\nreduce_motion=") + (preferences.reduce_motion ? "1" : "0") +
                           "\ntitle_store_suffix=" + (preferences.title_store_suffix ? "1" : "0") +
                           "\ncloud_saves_auto=" + (preferences.cloud_saves_auto ? "1" : "0") +
                           "\nsteam_sign_in_password=" + (preferences.steam_sign_in_password ? "1" : "0") +
                           "\ndownloads_slow_network=" + (preferences.downloads_slow_network ? "1" : "0") +
                           "\nsteam_stay_signed_in=" + (preferences.steam_stay_signed_in ? "1" : "0") +
                           "\nbeta_updates=" + (preferences.beta_updates ? "1" : "0") +
                           "\ndownload_server=" + server +
                           "\nweb_access=" + (preferences.web_access ? "1" : "0") + "\ncontent_language=" +
                           (steam::IsContentLanguage(preferences.content_language)
                                ? preferences.content_language
                                : std::string(steam::kDefaultContentLanguage)) +
                           "\nhide_run_once_help=" + (preferences.hide_run_once_help ? "1" : "0") +
                           "\nhide_launcher_help=" + (preferences.hide_launcher_help ? "1" : "0") +
                           "\nruntime_prompt_kit=" + preferences.runtime_prompt_kit + "\nrecipe_votes=" + votes +
                           "\nsharing_explained=" + (preferences.sharing_explained ? "1" : "0") +
                           "\nshare_name=" + name +
                           "\nreports_explained=" + (preferences.reports_explained ? "1" : "0") +
                           "\nshare_as=" + (preferences.share_as == ShareAs::kAccount ? "account"
                                            : preferences.share_as == ShareAs::kName ? "name" : "none") + "\n";
  return storage_.WriteAtomic(kPreferencesPath, Bytes(text.begin(), text.end()));
}

}  // namespace playden
