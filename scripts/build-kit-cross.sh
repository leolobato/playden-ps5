#!/usr/bin/env bash
# Playden - Cross-build prospero-win's Wine for the PS5 (the runtime kit's Wine modules) on a Linux x86-64 builder.
# Copyright (C) 2026 Leo Lobato
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Usage, on the builder (a Linux x86-64 machine or VM; from a Mac: ssh <builder> '<checkout>/scripts/build-kit-cross.sh …'):
#   scripts/build-kit-cross.sh <prospero-win commit> [--jobs N|max] [--ccache] [--name NAME] [--no-reuse]
#
# The build uses about half of the builder's cores by default, so the machine (and, for a VM, the computer that hosts
# it) stays usable; --jobs N sets the number of parallel jobs, --jobs max uses every core. A release kit is built with
# --no-reuse and without --ccache, in a new work folder.
#
# The commit is checked out into a clean clone (<work root>/source-<commit>). If a finished work folder under the work
# root has the same input key (tools/build_wine_ps5.sh --print-input-key: the same commit, toolchain and inputs), the
# script prints that folder and builds nothing, unless --no-reuse. Otherwise it builds into a new folder,
# <work root>/wine-ps5-<NAME>, NAME defaulting to <commit:7>-<UTC date>.<N>.
#
# The builder's inputs are named in a configuration file, PLAYDEN_KIT_CONFIG or ~/.config/playden/kit-build.conf, a
# shell file that sets:
#   PROSPERO_WIN_REPO   a prospero-win repository that has the commit (for example a Playden checkout's submodule)
#   KIT_WORK_ROOT       where the source clones and work folders go
#   WINE_SOURCE         the pinned Wine checkout (--source)
#   WINE_HOST_TOOLS     the host Wine build with winebuild and widl (--host-tools)
#   NATIVE_FOUNDATION   the title's foundation with the payload SDK (--foundation)
#   PRX_FOUNDATION      the foundation with module exports (--prx-foundation)
#   RADV                a PS5_Vulkan checkout with its RADV release (--radv)
#   MEDIA_ROOT          the target media prefix (--media-root); empty builds without media
#   TLS_ROOT            build_tls_ps5.sh's root (PROSPERO_TLS_ROOT); empty builds without TLS
#   FREETYPE_TARBALL    a local FreeType 2.13.3 tarball (PROSPERO_FREETYPE_TARBALL); empty downloads it
#   PATH_PREFIX         directories put in front of PATH, such as compiler wrappers (optional)
#   LLVM_CONFIG         the LLVM 18 llvm-config (optional)
set -euo pipefail

usage() { sed -n 's/^#   //p' "$0" | head -n 1 >&2; exit 2; }
fail() { echo "build-kit-cross: $*" >&2; exit 1; }

[[ $# -ge 1 && $1 != -* ]] || usage
commit=$1
shift
jobs=
ccache=0
name=
reuse=1
while [[ $# -gt 0 ]]; do
    case $1 in
    --jobs) [[ $# -ge 2 ]] || usage; jobs=$2; shift ;;
    --ccache) ccache=1 ;;
    --name) [[ $# -ge 2 ]] || usage; name=$2; shift ;;
    --no-reuse) reuse=0 ;;
    *) usage ;;
    esac
    shift
done

cores=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)
case $jobs in
'') jobs=$(( cores / 2 > 0 ? cores / 2 : 1 )) ;;
max) jobs=$cores ;;
*[!0-9]* | 0) fail "--jobs takes a positive number or max, not $jobs" ;;
esac

config=${PLAYDEN_KIT_CONFIG:-$HOME/.config/playden/kit-build.conf}
[[ -f $config ]] || fail "no builder configuration at $config (see the top of $0)"
PROSPERO_WIN_REPO= KIT_WORK_ROOT= WINE_SOURCE= WINE_HOST_TOOLS= NATIVE_FOUNDATION= PRX_FOUNDATION= RADV=
MEDIA_ROOT= TLS_ROOT= FREETYPE_TARBALL= PATH_PREFIX= LLVM_CONFIG=${LLVM_CONFIG:-}
# shellcheck source=/dev/null
. "$config"
for variable in PROSPERO_WIN_REPO KIT_WORK_ROOT WINE_SOURCE WINE_HOST_TOOLS NATIVE_FOUNDATION PRX_FOUNDATION; do
    [[ -n ${!variable} ]] || fail "$config does not set $variable"
done
[[ -z $PATH_PREFIX ]] || export PATH="$PATH_PREFIX:$PATH"
[[ -z $LLVM_CONFIG ]] || export LLVM_CONFIG
export TZ=UTC LC_ALL=${LC_ALL:-C.UTF-8}

# A clean clone at the commit.
commit=$(git -C "$PROSPERO_WIN_REPO" rev-parse --verify "$commit^{commit}") || fail "$PROSPERO_WIN_REPO has no commit $commit"
source=$KIT_WORK_ROOT/source-$commit
mkdir -p "$KIT_WORK_ROOT"
[[ -d $source/.git ]] || git clone -q --no-checkout "$PROSPERO_WIN_REPO" "$source"
git -C "$source" fetch -q "$PROSPERO_WIN_REPO" "$commit"
git -C "$source" checkout -q --force --detach "$commit"
git -C "$source" clean -q -fdx
[[ -z $(git -C "$source" status --porcelain) ]] || fail "$source is not clean"

arguments=(--source "$WINE_SOURCE" --host-tools "$WINE_HOST_TOOLS" --foundation "$NATIVE_FOUNDATION"
    --prx-foundation "$PRX_FOUNDATION" --jobs "$jobs")
[[ -z $RADV ]] || arguments+=(--radv "$RADV")
[[ -z $MEDIA_ROOT ]] || arguments+=(--media-root "$MEDIA_ROOT")
(( ! ccache )) || arguments+=(--ccache)
[[ -z $TLS_ROOT ]] || export PROSPERO_TLS_ROOT=$TLS_ROOT
[[ -z $FREETYPE_TARBALL ]] || export PROSPERO_FREETYPE_TARBALL=$FREETYPE_TARBALL

if (( reuse )); then
    key=$(cd "$source" && sh tools/build_wine_ps5.sh --print-input-key "${arguments[@]}" --work "$KIT_WORK_ROOT/.key")
    for found in "$KIT_WORK_ROOT"/wine-ps5-*/input-key; do
        if [[ -f $found && $(cat "$found") == "$key" ]]; then
            echo "build-kit-cross: reusing $(dirname "$found") (input key $key)"
            echo "work: $(dirname "$found")"
            exit 0
        fi
    done
fi

if [[ -z $name ]]; then
    stem=${commit:0:7}-$(date -u +%Y%m%d)
    number=1
    while [[ -e $KIT_WORK_ROOT/wine-ps5-$stem.$number ]]; do number=$((number + 1)); done
    name=$stem.$number
fi
work=$KIT_WORK_ROOT/wine-ps5-$name
[[ ! -e $work ]] || fail "$work exists; pick another --name"
echo "build-kit-cross: building $commit in $work with $jobs of $cores cores$( (( ccache )) && echo ', through ccache')"
(cd "$source" && sh tools/build_wine_ps5.sh "${arguments[@]}" --work "$work")
echo "work: $work"
