#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build the throwaway probe title. Needs a finished PS5 build of Playden in build/homebrew-ps5 for the host tool and libc.prx.
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/../.." && pwd)
main=${PLAYDEN_MAIN_CHECKOUT:-$root}
sdk=${PS5_PAYLOAD_SDK:-$main/deps/ps5-payload-sdk}
title=${PROBE_TITLE_ID:-FAKE77099}
tools=$root/build/homebrew-ps5/host-tools
native=$root/third_party/ps5-native-app-boilerplate/tooling/native
out=$root/build/local-process
app=$out/$title
source_dir=$root/probes/local-process
export PS5_PAYLOAD_SDK=$sdk
rm -rf "$app" "$out/obj"; mkdir -p "$app/sce_sys" "$app/sce_module" "$out/obj"
env_run() { "$root/scripts/with-env.sh" "$@"; }
env_run "$sdk/bin/prospero-clang++" -std=c++20 -O2 -fno-exceptions -fno-rtti -c "$native/app_crt.cpp" -o "$out/obj/app_crt.o"
stubs=("$sdk/target/lib/libkernel.so")
for stub in "$sdk"/target/lib/*.so; do
  case "${stub##*/}" in libkernel*.so) ;; *) stubs+=("$stub") ;; esac
done
link() { # name source output
  env_run "$sdk/bin/prospero-clang" -std=c11 -O2 -Wall -Werror ${PROBE_CFLAGS:-} -c "$source_dir/$2" -o "$out/obj/$1.o"
  (cd "$native/../.." && env_run "$sdk/bin/prospero-lld" -T "$native/ps5-pie.ld" --eh-frame-hdr -e _start \
    -o "$out/obj/$1-pie.elf" "$out/obj/app_crt.o" "$out/obj/$1.o" --as-needed "${stubs[@]}")
  "$tools/ps5-native-tool" link --in "$out/obj/$1-pie.elf" --out "$out/obj/$1.elf" --stub-dir "$sdk/target/lib" \
    --module-sdk 0x02000009 --companion-sdk 0x08050001 --file-name "$1.elf"
  "$tools/ps5-native-tool" self --sign --in "$out/obj/$1.elf" --out "$3" --magic 0x1D3D154F
}
link parent parent.c "$app/eboot.bin"
link child child.c "$app/child.bin"
# The same child under the name that skips the system library preload, built with libkernel only.
env_run "$sdk/bin/prospero-clang" -std=c11 -O2 -Wall -Werror -fno-stack-protector -I"$source_dir" ${PROBE_CFLAGS:-} -c "$source_dir/child-min.c" -o "$out/obj/child-min.o"
(cd "$native/../.." && env_run "$sdk/bin/prospero-lld" -T "$native/ps5-pie.ld" --eh-frame-hdr -e _start \
  -o "$out/obj/child-min-pie.elf" "$out/obj/child-min.o" --as-needed "$sdk/target/lib/libkernel.so")
if [[ ${PROBE_WINE:-0} == 1 ]]; then
  # The Wine child replaces it: the title shell's own start code, with libkernel only.
  win=$root/third_party/prospero-win
  wine_flags=(-std=c11 -O2 -Wall -Werror -fno-stack-protector -D__PROSPERO__=1 -I"$win/src" -I"$win/wine/ps5" -I"$win/include")
  env_run "$sdk/bin/prospero-clang" "${wine_flags[@]}" -c "$source_dir/child-wine.c" -o "$out/obj/child-wine.o"
  env_run "$sdk/bin/prospero-clang" "${wine_flags[@]}" -c "$win/src/pw_wine_start.c" -o "$out/obj/pw_wine_start.o"
  env_run "$sdk/bin/prospero-clang" "${wine_flags[@]}" -c "$win/wine/ps5/pw_wine_prx.c" -o "$out/obj/pw_wine_prx.o"
  (cd "$native/../.." && env_run "$sdk/bin/prospero-lld" -T "$native/ps5-pie.ld" --eh-frame-hdr -e _start \
    -o "$out/obj/child-min-pie.elf" "$out/obj/child-wine.o" "$out/obj/pw_wine_start.o" "$out/obj/pw_wine_prx.o" \
    --as-needed "$sdk/target/lib/libkernel.so" "$sdk/target/lib/libSceLibcInternal.so")
  cp "$out/hello/hello.exe" "$app/hello.exe"
fi
"$tools/ps5-native-tool" link --in "$out/obj/child-min-pie.elf" --out "$out/obj/bdj.elf" --stub-dir "$sdk/target/lib" \
  --module-sdk 0x02000009 --companion-sdk 0x08050001 --file-name bdj.elf
"$tools/ps5-native-tool" self --sign --in "$out/obj/bdj.elf" --out "$app/bdj.elf" --magic 0x1D3D154F
cp "$tools/libc.prx" "$app/sce_module/libc.prx"
cp "$root/third_party/ps5-native-app-boilerplate/sce_sys/icon0.png" "$app/sce_sys/"
python3 - "$root/third_party/ps5-native-app-boilerplate/sce_sys/param.json" "$app/sce_sys/param.json" "$title" <<'PY'
import json, sys
value = json.load(open(sys.argv[1]))
title = sys.argv[3]
value["titleId"] = title
value["conceptId"] = title[-5:]
value["contentId"] = f"UP9000-{title}_00-LOCALPROCESSPROB"
value["localizedParameters"]["en-US"]["titleName"] = "Local process probe"
json.dump(value, open(sys.argv[2], "w"), indent=2)
PY
echo "$app"
