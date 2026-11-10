#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build the shared Wine server test: the title, its child, the stand-in server module, and the two Windows programs.
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/../.." && pwd)
main=${PLAYDEN_MAIN_CHECKOUT:-$root}
sdk=${PS5_PAYLOAD_SDK:-$main/deps/ps5-payload-sdk}
tools=$root/build/homebrew-ps5/host-tools
native=$root/third_party/ps5-native-app-boilerplate/tooling/native
win=$root/third_party/prospero-win
out=$root/build/local-process/shared
app=$out/FAKE77099
src=$root/probes/local-process
export PS5_PAYLOAD_SDK=$sdk
run() { "$root/scripts/with-env.sh" "$@"; }
rm -rf "$out"; mkdir -p "$out/obj" "$app/sce_sys" "$app/sce_module"
flags=(-std=c11 -O2 -Wall -Werror -fno-stack-protector -D__PROSPERO__=1 -I"$win/src" -I"$win/wine/ps5" -I"$win/include" -I"$src")
for unit in "$win/src/pw_wine_start.c" "$win/wine/ps5/pw_wine_prx.c"; do
  run "$sdk/bin/prospero-clang" "${flags[@]}" -c "$unit" -o "$out/obj/$(basename "$unit" .c).o"
done
stage() { # name extra-flag output
  run "$sdk/bin/prospero-clang" "${flags[@]}" $2 -c "$src/wine-shared.c" -o "$out/obj/$1.o"
  (cd "$native/../.." && run "$sdk/bin/prospero-lld" -T "$native/ps5-pie.ld" --eh-frame-hdr -e _start -o "$out/obj/$1-pie.elf" \
    "$out/obj/$1.o" "$out/obj/pw_wine_start.o" "$out/obj/pw_wine_prx.o" --as-needed "$sdk/target/lib/libkernel.so" \
    "$sdk/target/lib/libSceLibcInternal.so" "$sdk/target/lib/libSceSystemService.so")
  "$tools/ps5-native-tool" link --in "$out/obj/$1-pie.elf" --out "$out/obj/$1.elf" --stub-dir "$sdk/target/lib" \
    --module-sdk 0x02000009 --companion-sdk 0x08050001 --file-name "$1.elf" >/dev/null
  "$tools/ps5-native-tool" self --sign --in "$out/obj/$1.elf" --out "$3" --magic 0x1D3D154F >/dev/null
}
stage eboot -DPROBE_TITLE "$app/eboot.bin"
stage bdj -DPROBE_CHILD_ROLE "$app/bdj.elf"
# The stand-in server module, built as the kit builds its small modules.
run "$sdk/bin/prospero-clang" -std=c11 -O2 -Wall -Werror -fPIC -c "$src/server-shim.c" -o "$out/obj/server-shim.o"
python3 "$win/tools/gen_prx_descriptor.py" "$out/obj/shim-descriptor.c" pw_wineserver_connect pw_wine_thread_register
run "$sdk/bin/prospero-clang" -std=c11 -O2 -Wall -Wextra -Werror -fPIC -I"$win/wine/ps5" -c "$out/obj/shim-descriptor.c" -o "$out/obj/shim-descriptor.o"
(cd "$native/../.." && run "$sdk/bin/prospero-lld" --shared -Bsymbolic -T "$native/ps5-pie.ld" -T "$win/wine/ps5/prx_eh_frame.ld" --eh-frame-hdr \
  -soname wineserver.prx -z defs -o "$out/obj/shim.shared.elf" "$out/obj/server-shim.o" "$out/obj/shim-descriptor.o" \
  "$sdk/target/lib/libunwind.a" --as-needed "$sdk/target/lib/libkernel.so" "$sdk/target/lib/libSceLibcInternal.so")
# Modules need the foundation's converter, which a worktree may not have built; built from third_party/prospero-foundation.
module_tool=$root/build/local-process/foundation-tool/ps5-native-tool
"$module_tool" link --module --in "$out/obj/shim.shared.elf" --out "$out/obj/shim.elf" --stub-dir "$sdk/target/lib" \
  --module-sdk 0x02000009 --companion-sdk 0x08050001 --file-name wineserver.prx >/dev/null
"$module_tool" self --sign --in "$out/obj/shim.elf" --out "$out/wineserver.prx" >/dev/null
# The Windows programs.
llvm=$(brew --prefix llvm@18)/bin
"$llvm/llvm-dlltool" -m i386:x86-64 -d "$src/kernel32.def" -l "$out/obj/kernel32.lib"
for program in ipc-owner ipc-peer; do
  "$llvm/clang" --target=x86_64-pc-windows-msvc -O2 -fno-stack-protector -DPROBE_SECTION -I"$src" -c "$src/$program.c" -o "$out/obj/$program.o"
  "$llvm/lld-link" /nodefaultlib /entry:Entry /subsystem:console "/out:$app/$program.exe" "$out/obj/$program.o" "$out/obj/kernel32.lib"
done
cp "$tools/libc.prx" "$app/sce_module/libc.prx"
cp "$root/third_party/ps5-native-app-boilerplate/sce_sys/icon0.png" "$app/sce_sys/"
cp "$root/build/local-process/FAKE77099/sce_sys/param.json" "$app/sce_sys/param.json"
ls -la "$app" "$out/wineserver.prx"
