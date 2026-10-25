#include "runtime/kit_trust.h"

#include "runtime/game_settings.h"

#include <algorithm>

namespace playden::runtime {

KitAdmissionConfig DevelopmentKitAdmission() {
  KitAdmissionConfig config;
  config.expected_manifest_sha256 = kExpectedKitManifestSha256;
  // The publication guard capability stays unused until native qualification.
  config.known_capabilities = {"dxvk", "offline_bottle", "per_game_title", "wow64_i386", kKitSharedSections,
                                kKitTitleAnyId, kKitGameRootUnowned, kKitBottleProfileShared, kKitOpenGLZink,
                                kKitAudioVolume, kKitDxvkConfig, kKitDxvkPresent, kKitInputTuning,
                                kKitInputWheel, kKitEnvironment, kKitWineMono, kKitVkd3dProton,
                                kKitDisplay120Hz, kKitProfileVersion2};
  std::sort(config.known_capabilities.begin(), config.known_capabilities.end());
  return config;
}

}  // namespace playden::runtime
