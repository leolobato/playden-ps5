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
  // Releases have nothing on; nightlies have community recipes on.
  CHECK(!DefaultExperimentalFeatures(false).community_recipes);
  CHECK(DefaultExperimentalFeatures(true).community_recipes);

  // One name per line, surrounding whitespace and CRLF trimmed, comments and blank lines skipped.
  std::vector<std::string> unknown;
  auto features = ParseExperimentalFeatures("# mine\n\n  community-recipes \r\nonline-steam\nCommunity-Recipes", {}, &unknown);
  CHECK(features.community_recipes);
  CHECK(unknown.size() == 2 && unknown[0] == "online-steam" && unknown[1] == "Community-Recipes");
  CHECK(!ParseExperimentalFeatures("#community-recipes\n").community_recipes);
  CHECK(!ParseExperimentalFeatures("community-recipes-x").community_recipes);
  // A line only turns a feature on: a nightly's default stays on with any file.
  CHECK(ParseExperimentalFeatures("", DefaultExperimentalFeatures(true)).community_recipes);
  CHECK(ParseExperimentalFeatures("# nothing\n", DefaultExperimentalFeatures(true)).community_recipes);

  std::string pattern = (std::filesystem::temp_directory_path() / "playden-features-XXXXXX").string();
  const char* made = mkdtemp(pattern.data());
  CHECK(made != nullptr);
  const std::filesystem::path dir = made;
  const auto file = (dir / kExperimentalFeaturesFileName).string();
  // Missing: the defaults.
  CHECK(!ReadExperimentalFeatures(file).community_recipes);
  CHECK(ReadExperimentalFeatures(file, DefaultExperimentalFeatures(true)).community_recipes);
  // Empty: the defaults.
  Write(file, "");
  CHECK(!ReadExperimentalFeatures(file).community_recipes);
  CHECK(ReadExperimentalFeatures(file, DefaultExperimentalFeatures(true)).community_recipes);
  Write(file, "community-recipes\n");
  CHECK(ReadExperimentalFeatures(file).community_recipes);
  // Unreadable (a folder at the path): the defaults.
  const auto folder = (dir / "folder").string();
  std::filesystem::create_directory(folder);
  CHECK(!ReadExperimentalFeatures(folder).community_recipes);
  CHECK(ReadExperimentalFeatures(folder, DefaultExperimentalFeatures(true)).community_recipes);
  // Only the first 16 KiB are read: a name past it is not seen.
  Write(file, std::string(kExperimentalFeaturesMaxBytes, '\n') + "community-recipes\n");
  CHECK(!ReadExperimentalFeatures(file).community_recipes);
  Write(file, std::string(kExperimentalFeaturesMaxBytes - 20, '\n') + "community-recipes\n");
  CHECK(ReadExperimentalFeatures(file).community_recipes);

  std::filesystem::remove_all(dir);
  std::puts("experimental_features: ok");
  return 0;
}
