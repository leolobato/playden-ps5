#!/usr/bin/env bash
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
launcher_root=$(cd -- "$root/../.." && pwd)
sdk=${PS5_PAYLOAD_SDK:-"$launcher_root/deps/ps5-payload-sdk"}
with_env="$launcher_root/scripts/with-env.sh"
toolchain="$sdk/toolchain/prospero.cmake"
compiler_launcher="$sdk/bin/prospero-clang"
deps="$root/.deps"
downloads="$deps/downloads"
prefix="$deps/ps5"
jobs=${JOBS:-$(sysctl -n hw.logicalcpu 2>/dev/null || getconf _NPROCESSORS_ONLN)}

curl_version=8.22.0
curl_sha256=f7ef3ae8a22e521f289803fe93543eb64c329b58aa73a9e224dfd915a2a5f4f7
curl_patch="$root/patches/curl-ps5-native-nonblock.patch"
curl_patch_sha256=8ad3d3b23ed6c526d89e289c0acc11784df994f6ba99a3d6392cac0f5065e526
mbedtls_version=3.6.7
mbedtls_sha256=a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6
cacert_sha256=a41b5d356aea97a529fe27e0f7316d2f9d946d75927476cf9cf1b90637d00505

fetch() {
    local url=$1 output=$2 expected=$3 actual
    if [[ ! -f "$output" ]]; then curl --fail --location --proto '=https' --tlsv1.2 "$url" -o "$output"; fi
    actual=$(shasum -a 256 "$output" | awk '{print $1}')
    [[ "$actual" == "$expected" ]] || { echo "hash mismatch for $output" >&2; exit 1; }
}

[[ -f "$with_env" ]] || { echo "missing PS5 environment wrapper: $with_env" >&2; exit 1; }
[[ -f "$toolchain" ]] || { echo "missing PS5 CMake toolchain: $toolchain (run scripts/fetch-deps.sh)" >&2; exit 1; }
[[ -f "$compiler_launcher" ]] || { echo "missing PS5 compiler launcher: $compiler_launcher" >&2; exit 1; }
[[ "$(shasum -a 256 "$curl_patch" | awk '{print $1}')" == "$curl_patch_sha256" ]] || {
    echo "hash mismatch for $curl_patch" >&2
    exit 1
}
mkdir -p "$downloads" "$deps/src" "$deps/build"
fetch "https://curl.se/download/curl-$curl_version.tar.xz" "$downloads/curl-$curl_version.tar.xz" "$curl_sha256"
fetch "https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-$mbedtls_version/mbedtls-$mbedtls_version.tar.bz2" \
    "$downloads/mbedtls-$mbedtls_version.tar.bz2" "$mbedtls_sha256"
fetch "https://curl.se/ca/cacert-2026-09-25.pem" "$downloads/cacert.pem" "$cacert_sha256"

rm -rf "$deps/src/mbedtls-$mbedtls_version" "$deps/src/curl-$curl_version" \
    "$deps/build/mbedtls-ps5" "$deps/build/curl-ps5" "$prefix"
mkdir -p "$prefix"
tar -xjf "$downloads/mbedtls-$mbedtls_version.tar.bz2" -C "$deps/src"
# TLS uses shared entropy/DRBG and PSA state even with distinct client contexts.
# Configure the installed header as well as the libraries to keep their mutex layouts consistent.
for option in MBEDTLS_THREADING_C MBEDTLS_THREADING_PTHREAD; do
    python3 "$deps/src/mbedtls-$mbedtls_version/scripts/config.py" \
        --file "$deps/src/mbedtls-$mbedtls_version/include/mbedtls/mbedtls_config.h" set "$option"
done
tar -xJf "$downloads/curl-$curl_version.tar.xz" -C "$deps/src"
patch --fuzz=0 -d "$deps/src/curl-$curl_version" -p1 < "$curl_patch"
# Record source paths relative to the extracted tree: the archives are then identical in every
# checkout and no local path reaches the PS5 title.
physical_src=$(cd "$deps/src" && pwd -P)
# CMake may spell the tree through a symlink (macOS /tmp is /private/tmp); map every spelling.
prefix_map="-ffile-prefix-map=$deps/src/= -ffile-prefix-map=$physical_src/= -ffile-prefix-map=${physical_src#/private}/="

bash "$with_env" cmake -S "$deps/src/mbedtls-$mbedtls_version" \
    -B "$deps/build/mbedtls-ps5" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$toolchain" \
    -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_C_FLAGS="$prefix_map" \
    -DENABLE_PROGRAMS=OFF -DENABLE_TESTING=OFF -DUSE_SHARED_MBEDTLS_LIBRARY=OFF
bash "$with_env" cmake --build "$deps/build/mbedtls-ps5" --parallel "$jobs"
bash "$with_env" cmake --install "$deps/build/mbedtls-ps5"

bash "$with_env" cmake -S "$deps/src/curl-$curl_version" \
    -B "$deps/build/curl-ps5" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$toolchain" \
    -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_C_FLAGS="-DPLAYDEN_PS5_NATIVE_NONBLOCK=1 $prefix_map" \
    -DBUILD_CURL_EXE=OFF -DBUILD_EXAMPLES=OFF \
    -DBUILD_LIBCURL_DOCS=OFF -DBUILD_MISC_DOCS=OFF -DBUILD_SHARED_LIBS=OFF \
    -DHTTP_ONLY=OFF -DCURL_USE_MBEDTLS=ON -DCURL_USE_CMAKECONFIG=ON \
    -DMbedTLS_DIR="$prefix/lib/cmake/MbedTLS" -DCURL_DISABLE_WEBSOCKETS=OFF \
    -DENABLE_IPV6=OFF -DENABLE_THREADED_RESOLVER=OFF -DCURL_DISABLE_SOCKETPAIR=ON \
    -DCURL_USE_LIBPSL=OFF -DCURL_USE_LIBSSH2=OFF -DCURL_ZLIB=OFF \
    -DCURL_DISABLE_NETRC=ON -DHAVE_GETPWUID_R=0 -DHAVE_GMTIME_R=0 \
    -DHAVE_IF_NAMETOINDEX=0 -DHAVE_PIPE2=0 -DHAVE_FCNTL_O_NONBLOCK=0 \
    -DCURL_BROTLI=OFF -DCURL_ZSTD=OFF -DUSE_NGHTTP2=OFF -DUSE_LIBIDN2=OFF \
    -DCURL_DISABLE_DICT=ON -DCURL_DISABLE_FILE=ON -DCURL_DISABLE_FTP=ON \
    -DCURL_DISABLE_GOPHER=ON -DCURL_DISABLE_IMAP=ON -DCURL_DISABLE_LDAP=ON \
    -DCURL_DISABLE_LDAPS=ON -DCURL_DISABLE_MQTT=ON -DCURL_DISABLE_POP3=ON \
    -DCURL_DISABLE_RTSP=ON -DCURL_ENABLE_SMB=OFF -DCURL_DISABLE_SMTP=ON \
    -DCURL_DISABLE_TELNET=ON -DCURL_DISABLE_TFTP=ON
bash "$with_env" cmake --build "$deps/build/curl-ps5" --parallel "$jobs"
bash "$with_env" cmake --install "$deps/build/curl-ps5"
install -m 0644 "$downloads/cacert.pem" "$prefix/cacert.pem"

manifest="$prefix/build-manifest.txt"
llvm_config=${LLVM_CONFIG:-}
if [[ -z "$llvm_config" ]]; then
    if command -v brew >/dev/null 2>&1; then
        llvm_config="$(brew --prefix llvm@18)/bin/llvm-config"
    else
        llvm_config=llvm-config-18
    fi
fi
compiler_binary="$($llvm_config --bindir)/clang"
[[ -f "$compiler_binary" ]] || { echo "missing compiler binary: $compiler_binary" >&2; exit 1; }
compiler_version=$(bash "$with_env" "$compiler_launcher" --version | head -n 1)
sdk_archive_sha256=$(cat "$sdk/.playden-source-sha256" 2>/dev/null || echo unknown)
{
    printf 'format=playden-steam-network-ps5-deps-v2\n'
    printf 'curl_version=%s\n' "$curl_version"
    printf 'curl_source_sha256=%s\n' "$curl_sha256"
    printf 'curl_ps5_native_nonblock_patch_sha256=%s\n' "$curl_patch_sha256"
    printf 'mbedtls_version=%s\n' "$mbedtls_version"
    printf 'mbedtls_source_sha256=%s\n' "$mbedtls_sha256"
    printf 'cacert_version=2026-09-25\n'
    printf 'cacert_source_sha256=%s\n' "$cacert_sha256"
    printf 'ps5_payload_sdk_archive_sha256=%s\n' "$sdk_archive_sha256"
    printf 'ps5_toolchain_sha256=%s\n' "$(shasum -a 256 "$toolchain" | awk '{print $1}')"
    printf 'ps5_compiler_launcher_sha256=%s\n' \
        "$(shasum -a 256 "$compiler_launcher" | awk '{print $1}')"
    printf 'ps5_compiler_binary_sha256=%s\n' \
        "$(shasum -a 256 "$compiler_binary" | awk '{print $1}')"
    printf 'ps5_compiler_version=%s\n' "$compiler_version"
    printf '%s\n' \
        'mbedtls_config=MBEDTLS_THREADING_C;MBEDTLS_THREADING_PTHREAD' \
        'mbedtls_flags=CMAKE_BUILD_TYPE=Release;CMAKE_C_FLAGS=-ffile-prefix-map=<src>/=;ENABLE_PROGRAMS=OFF;ENABLE_TESTING=OFF;USE_SHARED_MBEDTLS_LIBRARY=OFF' \
        'curl_flags=CMAKE_BUILD_TYPE=Release;CMAKE_C_FLAGS=-DPLAYDEN_PS5_NATIVE_NONBLOCK=1 -ffile-prefix-map=<src>/=;BUILD_CURL_EXE=OFF;BUILD_EXAMPLES=OFF;BUILD_LIBCURL_DOCS=OFF;BUILD_MISC_DOCS=OFF;BUILD_SHARED_LIBS=OFF;HTTP_ONLY=OFF;CURL_USE_MBEDTLS=ON;CURL_USE_CMAKECONFIG=ON;CURL_DISABLE_WEBSOCKETS=OFF;ENABLE_IPV6=OFF;ENABLE_THREADED_RESOLVER=OFF;CURL_DISABLE_SOCKETPAIR=ON;CURL_USE_LIBPSL=OFF;CURL_USE_LIBSSH2=OFF;CURL_ZLIB=OFF;CURL_DISABLE_NETRC=ON;HAVE_GETPWUID_R=0;HAVE_GMTIME_R=0;HAVE_IF_NAMETOINDEX=0;HAVE_PIPE2=0;HAVE_FCNTL_O_NONBLOCK=0;CURL_BROTLI=OFF;CURL_ZSTD=OFF;USE_NGHTTP2=OFF;USE_LIBIDN2=OFF;CURL_DISABLE_DICT=ON;CURL_DISABLE_FILE=ON;CURL_DISABLE_FTP=ON;CURL_DISABLE_GOPHER=ON;CURL_DISABLE_IMAP=ON;CURL_DISABLE_LDAP=ON;CURL_DISABLE_LDAPS=ON;CURL_DISABLE_MQTT=ON;CURL_DISABLE_POP3=ON;CURL_DISABLE_RTSP=ON;CURL_ENABLE_SMB=OFF;CURL_DISABLE_SMTP=ON;CURL_DISABLE_TELNET=ON;CURL_DISABLE_TFTP=ON'
    printf 'mbedtls_config_sha256=%s\n' \
        "$(shasum -a 256 "$prefix/include/mbedtls/mbedtls_config.h" | awk '{print $1}')"
    for archive in libcurl.a libmbedtls.a libmbedx509.a libmbedcrypto.a libeverest.a libp256m.a; do
        printf 'output_%s_sha256=%s\n' "$archive" \
            "$(shasum -a 256 "$prefix/lib/$archive" | awk '{print $1}')"
    done
    printf 'output_cacert.pem_sha256=%s\n' \
        "$(shasum -a 256 "$prefix/cacert.pem" | awk '{print $1}')"
} > "$manifest"

echo "PS5 dependency prefix: $prefix"
echo "PS5 dependency manifest: $manifest"
