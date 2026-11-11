#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds Playden's Steam host from core/shc.c and win/host.c with clang and lld-link (no Windows SDK or C runtime: the
# kernel32 and advapi32 import libraries come from the .def files here):
#   runtime/steam-host/steam.exe (64-bit).
# Needs LLVM 18 on PATH (PATH="$(brew --prefix llvm@18)/bin:$PATH").
# Reproducible: update kSteamHostSha256 in src/runtime/steam_host.h after a change.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
dist="$here/../../runtime/steam-host"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir -p "$dist"
llvm-dlltool -m i386:x86-64 -k -d "$here/kernel32.def" -l "$work/kernel32.lib"
llvm-dlltool -m i386:x86-64 -k -d "$here/advapi32.def" -l "$work/advapi32.lib"
flags="--target=x86_64-pc-windows-msvc -O2 -ffreestanding -fno-builtin -fno-stack-protector -mno-stack-arg-probe -Wall -Wextra -Werror"
objects=""
for source in "$here/core/shc.c" "$here/win/host.c"; do
  object="$work/$(basename "$source" .c).o"
  # shellcheck disable=SC2086
  clang $flags -I"$here/core" -c "$source" -o "$object"
  objects="$objects $object"
done
# shellcheck disable=SC2086
lld-link /nodefaultlib /entry:Entry /subsystem:windows /brepro /opt:ref /debug:none \
  /out:"$dist/steam.exe" $objects "$work/kernel32.lib" "$work/advapi32.lib"
shasum -a 256 "$dist/steam.exe" 2>/dev/null || sha256sum "$dist/steam.exe"
