#!/bin/sh
# Builds runtime/steamworks/steam_api64_online.dll, Playden's online bridge, from online.c with clang and
# lld-link (no Windows SDK or C runtime: kernel32 and winhttp import libraries come from the .def files here).
# exports.txt lists gbe_fork's steam_api64.dll exports; regenerate it when the emulator pin changes. Reproducible:
# update kSteamOnlineBridgeSha256 in src/runtime/steamworks.h after a change.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
out="$here/../../runtime/steamworks/steam_api64_online.dll"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
python3 -I "$here/gen.py" "$here/exports.txt" "$work"
llvm-dlltool -m i386:x86-64 -d "$here/kernel32.def" -l "$work/kernel32.lib"
llvm-dlltool -m i386:x86-64 -d "$here/winhttp.def" -l "$work/winhttp.lib"
flags="--target=x86_64-pc-windows-msvc -O2 -ffreestanding -fno-builtin -fno-stack-protector -mno-stack-arg-probe -Wall -Wextra -Werror"
clang $flags -I"$work" -c "$here/online.c" -o "$work/online.o"
clang --target=x86_64-pc-windows-msvc -c "$work/stubs.S" -o "$work/stubs.o"
lld-link /dll /nodefaultlib /entry:DllMain /def:"$work/steam_api64.def" /brepro /opt:ref /debug:none \
  /out:"$out" "$work/online.o" "$work/stubs.o" "$work/kernel32.lib" "$work/winhttp.lib"
rm -f "${out%.dll}.lib"
sha256sum "$out" 2>/dev/null || shasum -a 256 "$out"
