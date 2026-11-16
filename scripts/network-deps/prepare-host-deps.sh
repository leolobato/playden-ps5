#!/usr/bin/env bash
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
deps="$root/.deps"
downloads="$deps/downloads"
prefix="$deps/host"
jobs=${JOBS:-$(sysctl -n hw.logicalcpu 2>/dev/null || getconf _NPROCESSORS_ONLN)}

curl_version=8.22.0
curl_sha256=f7ef3ae8a22e521f289803fe93543eb64c329b58aa73a9e224dfd915a2a5f4f7
mbedtls_version=3.6.7
mbedtls_sha256=a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6
cacert_sha256=a41b5d356aea97a529fe27e0f7316d2f9d946d75927476cf9cf1b90637d00505

fetch() {
    local url=$1 output=$2 expected=$3 actual
    if [[ ! -f "$output" ]]; then curl --fail --location --proto '=https' --tlsv1.2 "$url" -o "$output"; fi
    actual=$(shasum -a 256 "$output" | awk '{print $1}')
    [[ "$actual" == "$expected" ]] || { echo "hash mismatch for $output" >&2; exit 1; }
}

mkdir -p "$downloads" "$deps/src" "$deps/build"
fetch "https://curl.se/download/curl-$curl_version.tar.xz" "$downloads/curl-$curl_version.tar.xz" "$curl_sha256"
fetch "https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-$mbedtls_version/mbedtls-$mbedtls_version.tar.bz2" \
    "$downloads/mbedtls-$mbedtls_version.tar.bz2" "$mbedtls_sha256"
fetch "https://curl.se/ca/cacert-2026-09-25.pem" "$downloads/cacert.pem" "$cacert_sha256"

rm -rf "$deps/src/mbedtls-$mbedtls_version" "$deps/src/curl-$curl_version" \
    "$deps/build/mbedtls-host" "$deps/build/curl-host" "$prefix"
mkdir -p "$prefix"
tar -xjf "$downloads/mbedtls-$mbedtls_version.tar.bz2" -C "$deps/src"
# TLS uses shared entropy/DRBG and PSA state even with distinct client contexts.
# Configure the installed header as well as the libraries to keep their mutex layouts consistent.
for option in MBEDTLS_THREADING_C MBEDTLS_THREADING_PTHREAD; do
    python3 "$deps/src/mbedtls-$mbedtls_version/scripts/config.py" \
        --file "$deps/src/mbedtls-$mbedtls_version/include/mbedtls/mbedtls_config.h" set "$option"
done
tar -xJf "$downloads/curl-$curl_version.tar.xz" -C "$deps/src"

# Relative source paths keep local paths out of the objects (see prepare-ps5-deps.sh).
physical_src=$(cd "$deps/src" && pwd -P)
# CMake may spell the tree through a symlink (macOS /tmp is /private/tmp); map every spelling.
prefix_map="-ffile-prefix-map=$deps/src/= -ffile-prefix-map=$physical_src/= -ffile-prefix-map=${physical_src#/private}/="
cmake -S "$deps/src/mbedtls-$mbedtls_version" -B "$deps/build/mbedtls-host" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_C_FLAGS="$prefix_map" \
    -DENABLE_PROGRAMS=OFF -DENABLE_TESTING=OFF -DUSE_SHARED_MBEDTLS_LIBRARY=OFF
cmake --build "$deps/build/mbedtls-host" --parallel "$jobs"
cmake --install "$deps/build/mbedtls-host"

cmake -S "$deps/src/curl-$curl_version" -B "$deps/build/curl-host" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_C_FLAGS="$prefix_map" \
    -DBUILD_CURL_EXE=OFF -DBUILD_EXAMPLES=OFF -DBUILD_LIBCURL_DOCS=OFF -DBUILD_MISC_DOCS=OFF \
    -DBUILD_SHARED_LIBS=OFF -DHTTP_ONLY=OFF -DCURL_USE_MBEDTLS=ON \
    -DCURL_DISABLE_WEBSOCKETS=OFF -DCURL_USE_LIBPSL=OFF -DCURL_USE_LIBSSH2=OFF \
    -DCURL_ZLIB=OFF -DCURL_BROTLI=OFF -DCURL_ZSTD=OFF -DUSE_NGHTTP2=OFF -DUSE_LIBIDN2=OFF \
    -DCURL_DISABLE_DICT=ON -DCURL_DISABLE_FILE=ON -DCURL_DISABLE_FTP=ON \
    -DCURL_DISABLE_GOPHER=ON -DCURL_DISABLE_IMAP=ON -DCURL_DISABLE_LDAP=ON \
    -DCURL_DISABLE_LDAPS=ON -DCURL_DISABLE_MQTT=ON -DCURL_DISABLE_POP3=ON \
    -DCURL_DISABLE_RTSP=ON -DCURL_ENABLE_SMB=OFF -DCURL_DISABLE_SMTP=ON \
    -DCURL_DISABLE_TELNET=ON -DCURL_DISABLE_TFTP=ON \
    -DCMAKE_PREFIX_PATH="$prefix"
cmake --build "$deps/build/curl-host" --parallel "$jobs"
cmake --install "$deps/build/curl-host"
install -m 0644 "$downloads/cacert.pem" "$prefix/cacert.pem"

echo "host dependency prefix: $prefix"
