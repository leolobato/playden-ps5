#!/usr/bin/env bash
# Package the selected direct Hui consumer locally, without console discovery, upload or launch.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
environment=${PLAYDEN_PS5_ENV:-"$root/scripts/with-env.sh"}
deps=${PLAYDEN_PS5_DEPS_DIR:-"$root/deps"}
hui=${PLAYDEN_HUI_SOURCE_DIR:-"$root/third_party/ps5-homebrew-ui"}
simulation=${PLAYDEN_UI_SIMULATION:-OFF}
# ON only for local testing against another community recipes server (dev-endpoints.json); the
# package then carries a marker that tools/write_update_manifest.py refuses for a release.
dev_endpoints=${PLAYDEN_DEV_ENDPOINTS:-OFF}
# A local build for testing has every experimental feature on (features.txt not needed). Its package carries a marker
# that tools/write_update_manifest.py refuses, so a release is built with PLAYDEN_DEV_BUILD=OFF.
dev_build=${PLAYDEN_DEV_BUILD:-ON}
native_access=${PLAYDEN_NATIVE_RUNTIME_ACCESS:-ON}
prospero_source=${PLAYDEN_PROSPERO_SOURCE_DIR:-"$root/third_party/prospero-win"}
helper_build="$root/third_party/PS5-Lapy-JB-Daemon/build/owned_root_helper-FAKE77031"
lapy_helper=${PLAYDEN_LAPY_HELPER_ELF:-"$helper_build/lapy.elf"}
lapy_manifest=${PLAYDEN_LAPY_HELPER_MANIFEST:-"$helper_build/lapy-manifest.json"}
title_remover=${PLAYDEN_TITLE_REMOVER_ELF:-"$root/build/title-remover/title-remover.elf"}
relaunch=${PLAYDEN_RELAUNCH_ELF:-"$root/build/self-update-relaunch/self-update-relaunch.elf"}
case "$simulation" in
 ON|on|1|true) simulation=ON ;;
 OFF|off|0|false|'') simulation=OFF ;;
 *) echo "PLAYDEN_UI_SIMULATION must be ON or OFF" >&2; exit 2 ;;
esac
case "$dev_endpoints" in
 ON|on|1|true) dev_endpoints=ON ;;
 OFF|off|0|false|'') dev_endpoints=OFF ;;
 *) echo "PLAYDEN_DEV_ENDPOINTS must be ON or OFF" >&2; exit 2 ;;
esac
case "$dev_build" in
 ON|on|1|true) dev_build=ON ;;
 OFF|off|0|false) dev_build=OFF ;;
 *) echo "PLAYDEN_DEV_BUILD must be ON or OFF" >&2; exit 2 ;;
esac
case "$native_access" in
 ON|on|1|true) native_access=ON ;;
 OFF|off|0|false) native_access=OFF ;;
 *) echo "PLAYDEN_NATIVE_RUNTIME_ACCESS must be ON or OFF" >&2; exit 2 ;;
esac
[[ -f "$environment" ]] || { echo "Missing explicit PS5 environment: $environment" >&2; exit 1; }
# PS5 CMake consumes the pre-baked atlas directory directly. Refresh it from the pinned submodule
# so catalog, font, license or baker changes cannot leave stale package inputs.
PLAYDEN_HUI_SOURCE_DIR="$hui" "$root/scripts/build-homebrew.sh" --prepare-fonts-only
if [[ $native_access == ON && -z ${PLAYDEN_TITLE_REMOVER_ELF:-} ]]; then
 "$root/scripts/build-title-remover.sh"
fi
if [[ $native_access == ON && -z ${PLAYDEN_RELAUNCH_ELF:-} ]]; then
 "$root/scripts/build-self-update-relaunch.sh"
fi
if [[ ! -f "$root/scripts/network-deps/.deps/ps5/lib/libcurl.a" ]]; then
 "$root/scripts/network-deps/prepare-ps5-deps.sh"
fi
bash "$environment" bash -c 'exec "$PS5_PAYLOAD_SDK/bin/prospero-cmake" "$@"' bash \
 -G Ninja -S "$root" -B "$root/build/homebrew-ps5" -DCMAKE_BUILD_TYPE=Release \
 -DPLAYDEN_PLATFORM_PS5=ON -DPLAYDEN_PS5_TITLE_ID=FAKE77031 \
 -DPLAYDEN_UI_SIMULATION="$simulation" -DPLAYDEN_DEV_ENDPOINTS="$dev_endpoints" -DPLAYDEN_DEV_BUILD="$dev_build" \
 -DPLAYDEN_NATIVE_RUNTIME_ACCESS="$native_access" -DPLAYDEN_PROSPERO_SOURCE_DIR="$prospero_source" \
 -DPLAYDEN_LAPY_HELPER_ELF="$lapy_helper" -DPLAYDEN_LAPY_HELPER_MANIFEST="$lapy_manifest" \
 -DPLAYDEN_TITLE_REMOVER_ELF="$title_remover" -DPLAYDEN_RELAUNCH_ELF="$relaunch" \
 -DPLAYDEN_PS5_DEPS_DIR="$deps" -DPLAYDEN_HUI_SOURCE_DIR="$hui"
bash "$environment" cmake --build "$root/build/homebrew-ps5" --target playden-homebrew-ps5-package
bash "$environment" env PLAYDEN_PACKAGE_DIR="$root/build/homebrew-ps5/dist/FAKE77031" \
 python3 "$root/tests/test_native_package_identity.py"
echo "Selected Hui package (not deployed): $root/build/homebrew-ps5/dist/FAKE77031"
# The commit the package says it was built from (Settings › About, and what it shares): local changes are named.
build_commit=$(sed -n 's/^#define PLAYDEN_BUILD_COMMIT "\(.*\)"$/\1/p' "$root/build/homebrew-ps5/generated/playden_build_commit.h")
if grep -q '^#define PLAYDEN_BUILD_LOCAL_CHANGES 1$' "$root/build/homebrew-ps5/generated/playden_build_commit.h"; then
 echo "Built from commit ${build_commit:-unknown} with local changes"
else
 echo "Built from commit ${build_commit:-unknown}"
fi
if [[ $dev_build == ON ]]; then
 echo "Dev build: every experimental feature is on. Build a release with PLAYDEN_DEV_BUILD=OFF." >&2
fi
if [[ $dev_endpoints == ON ]]; then
 echo "Built with PLAYDEN_DEV_ENDPOINTS=ON: for local testing only, not a release package" >&2
fi
