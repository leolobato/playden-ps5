#include "runtime/game_settings.h"
#include "runtime/kit_admission.h"
#include "runtime/kit_trust.h"
#include "steam/backend/json.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>

using namespace playden::runtime;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #x); std::exit(1); } } while (0)

using Json = playden::steam::json::Value;
const Json& Get(const Json& value, const std::string& key) {
  for (const auto& [name, child] : value.object()) if (name == key) return child;
  std::fprintf(stderr, "missing key %s\n", key.c_str()); std::exit(1);
}

// The kit this build was tested with must agree with the dependency lock, and the
// locally packaged development kit (when present) must be admitted and recognised as it.
int main(int argc, char** argv) {
  CHECK(argc == 3);
  std::ifstream in(argv[1]);
  const std::string text{std::istreambuf_iterator<char>(in), {}};
  playden::steam::json::Limits limits; limits.max_bytes = 1 << 20; limits.max_depth = 16;
  limits.max_values = 1 << 16; limits.max_string_bytes = 4096;
  const auto lock = playden::steam::json::Parse(text, limits);
  CHECK(lock && lock->is_object());
  const auto& kit = Get(*lock, "prospero_kit");
  const auto field = [&](const char* name) { return Get(kit, name).text(); };
  const auto config = DevelopmentKitAdmission();
  CHECK(field("kit_id") == kDevelopmentKitId && field("console_root") == kDevelopmentKitRoot);
  CHECK(field("manifest_sha256") == config.expected_manifest_sha256);
  // Everything the tested kit offers is something this build can use.
  for (const auto& item : Get(kit, "capabilities").array())
    CHECK(std::find(config.known_capabilities.begin(), config.known_capabilities.end(), item.text()) !=
          config.known_capabilities.end());
  // A kit with 120 Hz output is admitted with that capability, not as an ignored one.
  CHECK(std::count(config.known_capabilities.begin(), config.known_capabilities.end(), kKitDisplay120Hz) == 1);
  // A kit whose title shows the online bridge's notifications is admitted with that capability too.
  CHECK(std::count(config.known_capabilities.begin(), config.known_capabilities.end(), kKitTitleNotify) == 1);
  CHECK(std::is_sorted(config.known_capabilities.begin(), config.known_capabilities.end()));

  const std::filesystem::path root = argv[2];
  if (!std::filesystem::exists(root / "manifest.json")) {
    std::puts("expected kit matches the lock; packaged kit absent, admission skipped");
    return 0;
  }
  const auto admitted = VerifyInstalledKit(std::filesystem::canonical(root), config);
  if (admitted.status != KitManifestStatus::kVerified) std::fprintf(stderr, "admission: %s\n", admitted.reason.c_str());
  CHECK(admitted.status == KitManifestStatus::kVerified && admitted.view);
  CHECK(admitted.view->kit_id == kDevelopmentKitId);
  CHECK(admitted.manifest_sha256 == config.expected_manifest_sha256 && admitted.ignored_capabilities.empty());
  // Kit C carries one template shell; the installed title chooses its ID at runtime.
  CHECK(admitted.available_variant_ids ==
        std::vector<std::string>{"any-title-" + field("wrapper_revision").substr(0, 7)});
  CHECK(admitted.view->variants.size() == 1 && admitted.view->variants.front().any_title);
  CHECK(KitVariantFor(*admitted.view, "FAKE78011") == &admitted.view->variants.front());
  std::puts("development kit admitted and recognised as the expected kit");
  return 0;
}
