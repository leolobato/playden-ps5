# Third-party notices

Playden is licensed under the GNU General Public License v3.0 or later (see [LICENSE](LICENSE)). It includes, links
or ships the software below, each under its own license. The exact revisions and file hashes are pinned in
[`dependencies.lock.json`](dependencies.lock.json) and in the submodules under `third_party/`.

## Built into the Playden app

These are compiled into Playden's PS5 package (`FAKE77031`) or copied into it.

| Component | Version | License | Source |
| --- | --- | --- | --- |
| ps5-homebrew-ui | fork `0a6fb4b` | GPL-3.0 | by blackbearreloaded (the original repository is gone); my fork [leolobato/ps5-homebrew-ui](https://github.com/leolobato/ps5-homebrew-ui) in `third_party/ps5-homebrew-ui` |
| ps5-opengl (`libPS5OpenGL`) | 1.0.2, fork `384d1bd` | GPL-3.0 | by blackbearreloaded (the original repository is gone); my fork [leolobato/ps5-opengl](https://github.com/leolobato/ps5-opengl) in `third_party/ps5-opengl` |
| ps5-native-app-boilerplate (build tools and `libc.prx`) | `f98de73` | GPL-3.0 | [drakmor/ps5-native-app-boilerplate](https://github.com/drakmor/ps5-native-app-boilerplate); its `main` no longer has the pinned commit, so the submodule uses my copy [leolobato/ps5-native-app-boilerplate](https://github.com/leolobato/ps5-native-app-boilerplate) |
| ProsperoStore's system keyboard (adapted in `src/platform/ps5/system_keyboard.cpp` and `common_dialog_stub.cpp`) | `f73ec17` | GPL-3.0 | [darkxex/ProsperoStore](https://github.com/darkxex/ProsperoStore) |
| ps5-payload-sdk | v0.43 (v0.42 for the root helper) | GPL-3.0 | [ps5-payload-dev/sdk](https://github.com/ps5-payload-dev/sdk) |
| PS5-Lapy-JB-Daemon (root helper, `lapy.elf`) | fork `66ab4f1` | MIT, Copyright (c) 2026 Arksama (Team PHU) | [mpereiraesaa/PS5-Lapy-JB-Daemon](https://github.com/mpereiraesaa/PS5-Lapy-JB-Daemon), my fork in `third_party/PS5-Lapy-JB-Daemon` |
| curl | 8.22.0, with a PS5 non-blocking patch | curl license (MIT-style) | [curl.se](https://curl.se/) |
| Mbed TLS | 3.6.6 | Apache-2.0 | [Mbed-TLS/mbedtls](https://github.com/Mbed-TLS/mbedtls) |
| CA certificate bundle (`trust/cacert.pem`) | 2026-09-25 | MPL-2.0 | Mozilla's root store, as extracted by [curl](https://curl.se/docs/caextract.html) |
| SQLite | 3.53.1 | Public domain | [sqlite.org](https://sqlite.org/) |
| zstd (decompression) | 1.5.6 | BSD-3-Clause | [facebook/zstd](https://github.com/facebook/zstd) |
| LZMA SDK (decoder) | 24.09 | Public domain | [7-zip.org/sdk](https://www.7-zip.org/sdk.html) |
| stb_image | 2.30 | MIT or public domain | [nothings/stb](https://github.com/nothings/stb) |
| miniz (zip reading, for updates) | 3.0.2 | MIT, Copyright 2013-2014 RAD Game Tools and Valve Software, 2010-2014 Rich Geldreich and Tenacious Software LLC | [richgel999/miniz](https://github.com/richgel999/miniz) |
| utf8proc | 2.11.3 | MIT, with Unicode data under the Unicode license | [JuliaStrings/utf8proc](https://github.com/JuliaStrings/utf8proc) |
| QR Code generator | `3c6d0b3` | MIT, Copyright (c) Project Nayuki | [nayuki/QR-Code-generator](https://github.com/nayuki/QR-Code-generator) |
| compiler-rt builtins | 18.1.8 | Apache-2.0 WITH LLVM-exception | [llvm/llvm-project](https://github.com/llvm/llvm-project) |
| Barlow, Barlow Condensed | — | SIL Open Font License 1.1 | [jpt/barlow](https://github.com/jpt/barlow) |
| Noto Sans, Noto Sans CJK SC, Noto Sans Arabic | — | SIL Open Font License 1.1 | [notofonts](https://github.com/notofonts) |

The font licenses are shipped next to the fonts, in `assets/fonts/` of the package. The web access page serves two
Barlow faces from inside the app, with the license at `/assets/barlow-OFL.txt`.

## Shipped with Playden for games

| Component | License | Source |
| --- | --- | --- |
| gbe_fork (`runtime/steamworks/steam_api.dll`, `steam_api64.dll`) | LGPL-3.0 ([text](runtime/steamworks/LICENSE)) | [Detanup01/gbe_fork](https://github.com/Detanup01/gbe_fork). Shipped unmodified. Playden does not link against it: it replaces a game's own Steam API when the game is prepared. See [`runtime/steamworks/README.md`](runtime/steamworks/README.md). |

## In the runtime kit

The runtime kit is a separate download. It carries these components. The kit release lists the exact source
revisions it was built from and includes their license texts in `LICENSES/`, and our changes are on the
`main` branches of the forks.

| Component | Version | License | Source |
| --- | --- | --- | --- |
| Wine | upstream with the prospero-win patch series | LGPL-2.1-or-later | [winehq.org](https://www.winehq.org/), patches in prospero-win `wine/patches/` |
| prospero-win (Wine's PS5 side, the 32-bit translator, the per-game app shell) | kit 0.12: Wine and runtime `f729534`, app shell `f729534` | LGPL-2.1 | [mpereiraesaa/prospero-win](https://github.com/mpereiraesaa/prospero-win), my fork [leolobato/prospero-win](https://github.com/leolobato/prospero-win) |
| prospero-foundation (native app tools for the per-game app shell) | kit 0.12: `9c0b994` | GPL-3.0 | Fork of [mpereiraesaa/ps5-native-app-boilerplate](https://github.com/mpereiraesaa/ps5-native-app-boilerplate) |
| DXVK | 2.6.2 | Zlib | [doitsujin/dxvk](https://github.com/doitsujin/dxvk) |
| vkd3d-proton (Direct3D 12 for games set to it), with dxil-spirv and dxbc-spirv | 3.0.1 (`3b10bd7`), unmodified | LGPL-2.1-or-later; MIT (dxil-spirv, dxbc-spirv); Apache-2.0 and MIT (Khronos SPIR-V and Vulkan headers and tools) | [HansKristian-Work/vkd3d-proton](https://github.com/HansKristian-Work/vkd3d-proton) |
| Mesa (WGL and Zink, for OpenGL games) | PS5_Mesa `e4607ef` | MIT | [mpereiraesaa/PS5_Mesa](https://github.com/mpereiraesaa/PS5_Mesa), based on [Mesa](https://mesa3d.org/) |
| RADV for the PS5 (`libvulkan.prx`) | PS5_Vulkan `8aee3af`, PS5_Mesa `156e555` (`01d7057` with 120 Hz output) | GPL-3.0 (PS5_Vulkan); MIT (Mesa) | [mpereiraesaa/PS5_Vulkan](https://github.com/mpereiraesaa/PS5_Vulkan), [mpereiraesaa/PS5_Mesa](https://github.com/mpereiraesaa/PS5_Mesa), my fork [leolobato/PS5_Mesa](https://github.com/leolobato/PS5_Mesa) |
| Wine Mono (the .NET runtime for Windows programs) | 11.3.0, unmodified | MIT for most of it; parts of Mono are LGPL, FNA is MIT and MS-PL (see its `COPYING`) | [wine-mono/wine-mono](https://github.com/wine-mono/wine-mono), [release archive](https://dl.winehq.org/wine/wine-mono/11.3.0/) |
| GnuTLS, with its libtasn1 | 3.8.13 | LGPL-2.1-or-later | [gnutls.org](https://www.gnutls.org/) |
| Nettle, with its mini-gmp | 3.10.2 | LGPL-3.0-or-later or GPL-2.0-or-later | [lysator.liu.se/~nisse/nettle](https://www.lysator.liu.se/~nisse/nettle/) |
| GStreamer, with gst-plugins-base, -good, -bad and gst-libav (video and audio decoding, LGPL plugins only) | 1.24.12 | LGPL-2.1-or-later | [gstreamer.freedesktop.org](https://gstreamer.freedesktop.org/) |
| FFmpeg (built without `--enable-gpl` or `--enable-nonfree`) | 7.1.1 | LGPL-2.1-or-later | [ffmpeg.org](https://ffmpeg.org/) |
| GLib | 2.80.5 | LGPL-2.1-or-later | [gitlab.gnome.org/GNOME/glib](https://gitlab.gnome.org/GNOME/glib) |
| libiconv | 1.17 | LGPL-2.1-or-later | [gnu.org/software/libiconv](https://www.gnu.org/software/libiconv/) |
| PCRE2 | 10.44 | BSD-3-Clause with the PCRE2 exception | [github.com/PCRE2Project/pcre2](https://github.com/PCRE2Project/pcre2) |
| libffi | 3.4.6 | MIT | [sourceware.org/libffi](https://sourceware.org/libffi/) |
| zlib | 1.3.1 | Zlib | [zlib.net](https://zlib.net/) |
| FreeType | 2.13.3 | FreeType License (FTL) or GPL-2.0 | [freetype.org](https://freetype.org/) |
| CA certificate bundle (for schannel) | — | MPL-2.0 | Mozilla's root store |
| PS5-Lapy-JB-Daemon (root helper in the app shell) | fork `66ab4f1` | MIT | See above |

## Your rights

For the GPL and LGPL components, you can get the complete source of the version we ship from the links above, or
from the submodules and pinned revisions in this repository. If a link stops working, open an issue and we will
provide the source.
