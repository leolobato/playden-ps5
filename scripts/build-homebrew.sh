#!/usr/bin/env bash
# Playden - Prepare and build the selected direct ps5-homebrew-ui consumer.
# Copyright (C) 2026 Leo Lobato
# SPDX-License-Identifier: GPL-3.0-or-later

set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
revision=24d74b721168a08a12f93e9f2aa2ee1abfd0e878
hui_source=${PLAYDEN_HUI_SOURCE_DIR:-"$root/third_party/ps5-homebrew-ui"}
build_dir="$root/build/homebrew-host"
font_dir="$root/build/homebrew-fonts"
prepare_only=0

if [[ ${1:-} == --prepare-only ]]; then
    prepare_only=1
    shift
elif [[ ${1:-} == --prepare-fonts-only ]]; then
    prepare_only=1
    shift
fi

# The submodule must be exactly the pinned commit of its fork's main, without local edits.
if [[ ! -e $hui_source/.git ]]; then
    printf 'ps5-homebrew-ui not found at %s: run git submodule update --init third_party/ps5-homebrew-ui\n' "$hui_source" >&2
    exit 1
fi
actual_revision=$(git -C "$hui_source" rev-parse HEAD)
if [[ $actual_revision != "$revision" ]]; then
    printf 'ps5-homebrew-ui revision mismatch: expected %s, found %s\n' "$revision" "$actual_revision" >&2
    exit 1
fi
if [[ -n $(git -C "$hui_source" status --porcelain --untracked-files=all) ]]; then
    printf 'ps5-homebrew-ui has local changes; commit them on its main branch first\n' >&2
    git -C "$hui_source" status --short >&2
    exit 1
fi

cxx=$(command -v "${HOST_CXX:-clang++}")
baker="$root/build/hui-bake-font"
mkdir -p "$font_dir"
"$cxx" -std=c++20 -O2 -w \
    "$hui_source/tools/font-baker/bake_font.cpp" -o "$baker"

while IFS='|' read -r input output glyphs; do
    if [[ $glyphs == catalog-european ]]; then
        glyphs="european,text=$root/fixtures/ui-comparison/catalog.json"
    elif [[ $glyphs == catalog ]]; then
        glyphs="text=$root/fixtures/ui-comparison/catalog.json"
    fi
    "$baker" "$root/fixtures/ui-comparison/assets/fonts/$input" \
        "$font_dir/$output" 56 8 2048 "$glyphs" >/dev/null
done <<'FONTS'
Barlow-Regular.ttf|barlow-regular.huifont|european
Barlow-Medium.ttf|barlow-medium.huifont|european
Barlow-SemiBold.ttf|barlow-semibold.huifont|european
BarlowCondensed-Medium.ttf|barlow-condensed-medium.huifont|european
BarlowCondensed-SemiBold.ttf|barlow-condensed-semibold.huifont|european
NotoSansCJKsc-Regular.otf|noto-cjk-fallback.huifont|catalog-european
NotoSansArabic.ttf|noto-arabic-fallback.huifont|catalog
FONTS

# Supplemental font for complete Greek coverage; frozen comparison fixtures stay intact.
"$baker" "$root/assets/fonts/NotoSans-Regular.ttf" \
    "$font_dir/noto-greek-fallback.huifont" 56 8 2048 "european,text=$root/fixtures/ui-comparison/catalog.json" >/dev/null
cp "$root/assets/fonts/NotoSans-OFL.txt" "$font_dir/NotoSans-OFL.txt"

if (( prepare_only )); then
    printf 'Verified ps5-homebrew-ui %s and refreshed font atlases in %s\n' "$revision" "$font_dir"
    exit 0
fi

# Rebuild when the prefix is missing or predates the current curl patches (CMake refuses such a prefix).
curl_patches_wanted="curl_mbedtls_blocked_send_patch_sha256=$(sed -n 's/^curl_mbedtls_patch_sha256=//p' \
    "$root/scripts/network-deps/prepare-host-deps.sh")"
if [[ ! -f "$root/scripts/network-deps/.deps/host/lib/libcurl.a" ]] ||
    [[ "$(cat "$root/scripts/network-deps/.deps/host/curl-patches.txt" 2>/dev/null)" != "$curl_patches_wanted" ]]; then
    "$root/scripts/network-deps/prepare-host-deps.sh"
fi

cmake --preset homebrew-host \
    -DPLAYDEN_HUI_SOURCE_DIR="$hui_source"
cmake --build --preset homebrew-host --target playden-homebrew

if (( $# > 0 )); then
    "$build_dir/playden-homebrew" "$@"
fi
