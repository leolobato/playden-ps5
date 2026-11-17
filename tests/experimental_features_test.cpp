#include "app/experimental_features.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <unistd.h>

using namespace playden::app;

#define CHECK(condition)                                                                  \
  do {                                                                                    \
    if (!(condition)) {                                                                   \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
      std::exit(1);                                                                       \
    }                                                                                     \
  } while (false)

namespace {
void Write(const std::filesystem::path& path, const std::string& text) {
  FILE* file = std::fopen(path.c_str(), "wb");
  CHECK(file != nullptr);
  CHECK(std::fwrite(text.data(), 1, text.size(), file) == text.size());
  std::fclose(file);
}
}  // namespace

int main() {
  // Online Steam features is off in every build, nightlies and dev builds too, until the file names it.
  // Online is on by default in every build, releases included.
  CHECK(DefaultExperimentalFeatures(false).online_steam);
  CHECK(DefaultExperimentalFeatures(true).online_steam);

  // One name per line, surrounding whitespace and CRLF trimmed, comments and blank lines skipped.
  std::vector<std::string> unknown;
  auto features = ParseExperimentalFeatures("# mine\n\n  online-steam \r\nOnline-Steam", {}, &unknown);
  CHECK(features.online_steam);
  CHECK(unknown == std::vector<std::string>{"Online-Steam"});
  CHECK(!ParseExperimentalFeatures("#online-steam\nonline-steam-2\n").online_steam);

  // community-recipes is retired (community recipes are in every build): a file that still has the line keeps
  // working. The line turns nothing on and isn't reported as unknown; a misspelling of it still is.
  unknown.clear();
  features = ParseExperimentalFeatures("community-recipes\n  community-recipes \r\nCommunity-Recipes\ncommunity-recipes-x\n", {}, &unknown);
  CHECK(!features.online_steam);
  CHECK((unknown == std::vector<std::string>{"Community-Recipes", "community-recipes-x"}));
  unknown.clear();
  features = ParseExperimentalFeatures("community-recipes\nonline-steam\n", {}, &unknown);
  CHECK(features.online_steam && unknown.empty());

  // GOG as a store: on by default in every build, releases included; the line is still accepted.
  CHECK(DefaultExperimentalFeatures(false).gog_store && DefaultExperimentalFeatures(true).gog_store);
  {
    std::vector<std::string> gog_unknown;
    const auto gog = ParseExperimentalFeatures(" gog-store\r\ngog\nGOG-STORE\n#gog-store\n", {}, &gog_unknown);
    CHECK(gog.gog_store && !gog.online_steam);
    CHECK((gog_unknown == std::vector<std::string>{"gog", "GOG-STORE"}));
    CHECK(!ParseExperimentalFeatures("gog-store-x\n# gog-store").gog_store);
    CHECK(!ParseExperimentalFeatures("online-steam\ncommunity-recipes\n").gog_store);
    CHECK(ParseExperimentalFeatures("online-steam\n", DefaultExperimentalFeatures(false)).gog_store);
    CHECK(ParseExperimentalFeatures("online-steam\n", DefaultExperimentalFeatures(true)).gog_store);
    CHECK(ParseExperimentalFeatures("gog-store", DefaultExperimentalFeatures(true)).gog_store);
  }

  // A line only turns a feature on: a default that is on stays on with any file.
  ExperimentalFeatures on;
  on.online_steam = true;
  CHECK(ParseExperimentalFeatures("", on).online_steam);
  CHECK(ParseExperimentalFeatures("# nothing\ncommunity-recipes\n", on).online_steam);
  on.gog_store = true;
  CHECK(ParseExperimentalFeatures("", on).gog_store);

  std::string pattern = (std::filesystem::temp_directory_path() / "playden-features-XXXXXX").string();
  const char* made = mkdtemp(pattern.data());
  CHECK(made != nullptr);
  const std::filesystem::path dir = made;
  const auto file = (dir / kExperimentalFeaturesFileName).string();
  // Missing: the defaults.
  CHECK(!ReadExperimentalFeatures(file).online_steam);
  CHECK(ReadExperimentalFeatures(file, on).online_steam);
  // Empty: the defaults.
  Write(file, "");
  CHECK(!ReadExperimentalFeatures(file).online_steam);
  CHECK(ReadExperimentalFeatures(file, on).online_steam);
  Write(file, "online-steam\n");
  CHECK(ReadExperimentalFeatures(file).online_steam);
  // The retired line read from a file: nothing unknown.
  Write(file, "community-recipes\n");
  unknown.clear();
  CHECK(!ReadExperimentalFeatures(file, {}, &unknown).online_steam && unknown.empty());
  // Unreadable (a folder at the path): the defaults.
  const auto folder = (dir / "folder").string();
  std::filesystem::create_directory(folder);
  CHECK(!ReadExperimentalFeatures(folder).online_steam);
  CHECK(ReadExperimentalFeatures(folder, on).online_steam);
  // Only the first 16 KiB are read: a name past it is not seen.
  Write(file, std::string(kExperimentalFeaturesMaxBytes, '\n') + "online-steam\n");
  CHECK(!ReadExperimentalFeatures(file).online_steam);
  Write(file, std::string(kExperimentalFeaturesMaxBytes - 20, '\n') + "online-steam\n");
  CHECK(ReadExperimentalFeatures(file).online_steam);

  std::filesystem::remove_all(dir);
  std::puts("experimental_features: ok");
  return 0;
}
