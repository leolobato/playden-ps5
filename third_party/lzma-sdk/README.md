# LZMA SDK (LZMA1 decoder, 7z reader, BCJ2, x86 BCJ)

- Source: https://github.com/ip7z/7zip, tag `24.09`, directory `C/`
  (fetched as `https://raw.githubusercontent.com/ip7z/7zip/24.09/C/<file>`)
- License: public domain (see LICENSE)
- Copied files (unmodified) and SHA-256:
  - `LzmaDec.c`  b9ca2b8707400347c75d6fef288d1354a3b4cf93e8da42f26207b8bd4c4d5e59
  - `LzmaDec.h`  3aaf07b4ae4173a2d103179455dc7089b5ddbc7fc3db3c0e40964a7499c69266
  - `7zTypes.h`  5de943c886d5d7bbcf16203e9b292a9e7be327b43fe3dfd9f4ad07d6d0870ef6
  - `Precomp.h`  fea249c753dfbbdd69d9397e91497205452b784279f70cad4658dc567cd2f85f
  - `Compiler.h` 959dac87ac7109502f6467e910d0722b932176e243153bf8c663479aa8c29b20
  - `7z.h`       b3f59ce743927000b704d925aa6e31795c572acb43814a7e50ebbcc785bfccf1
  - `7zArcIn.c`  9b863803e963b276fe74f2dabb8c25f4094e98ef5bf9974d5720dc02a29082a5
  - `7zDec.c`    774c32cfa322175be5a43073a6fb1ff918ec9dcaa9014ea949d96701aa740079
  - `7zStream.c` 835941df3324d828770bc09749a976bad8449e3733896d51bf102557c9ac0acb
  - `7zBuf.c`    54ecf8ed84545b20f2fb8519de4de309efcef6434353b03383dea6bd79e9c4c7
  - `7zBuf.h`    8077b812692353fa662bcb267a38737cdca7660d4b086067afac16dfe5809c3d
  - `7zCrc.c`    6c826585a824f991488d16fc4fa236ab6abe317b8f853fea77282e31d5160c54
  - `7zCrc.h`    75ff8362de5ef390c286adfec246f16eceaff6dff1090a80240495c47d88e923
  - `7zCrcOpt.c` df23d84d9297358f401e36c0a0a5b004304f72f588f0f1db346a74d30e10036a
  - `CpuArch.h`  051d8a61373d9b6c90eb3b80f2aa1d932126fec7091242b085276e0bbd9bec03
  - `Bcj2.c`     52f5c6bbaf512f867fe46a378d79888825a5b4e31abbd4c94f007058a890174e
  - `Bcj2.h`     e45c841e5d805deeb2ae3f212c8860ce312252efdc55f713f52f837cf8c4452f
  - `Bra.h`      bf4addce6209562a700edf040a8efc5b392bb33d4427b1513b6e3a877d04f431
  - `Bra86.c`    0925d655955bdd2b7b25a01bbb3d2d395e5bac6d572f2cfd273141d1896af0ec
  - `Delta.h`    ce359ff7eb4c9d806777e21d0ec0416cf06c1f556242cbbcf565492b47141eeb
  - `Lzma2Dec.h` a4b97083c3817d3e1e3049f8b1abc0b4ca3e91192606497c02bb5351b57422c7
- `CpuArch.c` is not needed (`CpuArch.h` macros only).
- The 7z files are built with `Z7_NO_METHODS_FILTERS` and `Z7_NO_METHOD_LZMA2`, so `7zDec.c` only
  decodes LZMA, BCJ2 and copy; `Delta.h` and `Lzma2Dec.h` are included but none of their code is used
  (their `.c` files are not copied). `Bra86.c` (the x86 BCJ filter) is used directly by
  `publisher_runtime.cpp`, not through `7zDec.c`; the other `Bra*.c` converters are not copied.
- Used by `src/steam/backend/depot_chunk.cpp` for Steam "VZ" (LZMA1) depot chunks, and by
  `src/runtime/publisher_runtime.cpp`, which reads a 7z archive inside a game's installer with
  `7zArcIn.c` (the header; `7zDec.c` decodes a compressed header) and decodes the files itself, to
  disk through fixed buffers, with `LzmaDec.c`, `Bcj2.c` and `Bra86.c`.
