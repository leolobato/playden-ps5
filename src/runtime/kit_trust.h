#pragma once
#include "runtime/kit_manifest.h"
#include <string>
#include <string_view>

namespace playden::runtime {

// Playden uses whichever kit is installed at kDevelopmentKitRoot, as long as it is
// complete and its files match its own manifest. The values below describe the kit
// this build was tested with; they mirror the "prospero_kit" entry in
// dependencies.lock.json (a host test checks they agree) and are only shown next to
// the installed kit (Settings > About).
inline constexpr const char* kDevelopmentKitId = "playden-kit";
inline constexpr const char* kDevelopmentKitRoot = "/data/playden/kits/playden-kit";
inline constexpr const char* kExpectedKitManifestSha256 =
    "8b77fb688ebcb174c50d7fe6a4ee2cbd74d89d18e6903445862c78da257bf6f4";

// Ids a kit had before it was renamed: its folder under /data/playden/kits and its manifest's kit_id.
// A title, bottle or settings publication made from the earlier id belongs to the renamed kit (Update
// refreshes it, and its publication still starts), and the earlier folder is retired once the kit is
// installed under its new id (update::UpdateService).
struct KitIdRename {
  const char* from;
  const char* to;
};
inline constexpr KitIdRename kKitIdRenames[] = {{"hl2-fw1360-dev1", "playden-kit"}};

// Whether the kit `from` was renamed to `to`.
inline bool KitIdRenamedTo(std::string_view from, std::string_view to) {
  for (const auto& renamed : kKitIdRenames)
    if (from == renamed.from && to == renamed.to) return true;
  return false;
}

// What this build accepts from a kit, on any console firmware.
KitAdmissionConfig DevelopmentKitAdmission();

}  // namespace playden::runtime
