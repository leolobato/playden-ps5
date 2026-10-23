#!/bin/sh
# Builds Playden's online bridge from online.c with clang and lld-link (no Windows SDK or C runtime: the kernel32 and
# winhttp import libraries come from the .def files here):
#   runtime/steamworks/steam_api64_online.dll (64-bit) and runtime/steamworks/steam_api_online.dll (32-bit).
# exports.txt lists gbe_fork's steam_api exports (the same names in both builds); regenerate it when the emulator pin
# changes. Reproducible: update kSteamOnlineBridge64Sha256/kSteamOnlineBridge32Sha256 in src/runtime/steamworks.h
# after a change.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
dist="$here/../../runtime/steamworks"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
build() {  # arch target machine dll entry kernel32.def winhttp.def extra-link-flag
  arch=$1 target=$2 machine=$3 out=$4 entry=$5
  mkdir -p "$work/$arch"
  python3 -I "$here/gen.py" "$here/exports.txt" "$work/$arch" "$arch"
  llvm-dlltool -m "$machine" -k -d "$6" -l "$work/$arch/kernel32.lib"
  llvm-dlltool -m "$machine" -k -d "$7" -l "$work/$arch/winhttp.lib"
  flags="--target=$target -O2 -ffreestanding -fno-builtin -fno-stack-protector -mno-stack-arg-probe -Wall -Wextra -Werror"
  clang $flags -I"$work/$arch" -c "$here/online.c" -o "$work/$arch/online.o"
  clang --target="$target" -c "$work/$arch/stubs.S" -o "$work/$arch/stubs.o"
  lld-link /dll /nodefaultlib /entry:"$entry" /def:"$work/$arch/steam_api64.def" /brepro /opt:ref /debug:none $8 \
    /out:"$dist/$out" "$work/$arch/online.o" "$work/$arch/stubs.o" "$work/$arch/kernel32.lib" "$work/$arch/winhttp.lib"
  rm -f "$dist/${out%.dll}.lib"
  sha256sum "$dist/$out" 2>/dev/null || shasum -a 256 "$dist/$out"
}
build x86_64 x86_64-pc-windows-msvc i386:x86-64 steam_api64_online.dll DllMain "$here/kernel32.def" "$here/winhttp.def" ""
build x86 i686-pc-windows-msvc i386 steam_api_online.dll DllMain@12 "$here/kernel32_x86.def" "$here/winhttp_x86.def" /safeseh:no
