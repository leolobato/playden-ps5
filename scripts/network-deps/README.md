# Network dependencies

Playden's Steam client uses curl 8.22.0 with Mbed TLS 3.6.7 LTS and the Mozilla CA bundle dated 2026-09-25. These
scripts download the pinned sources (checked by SHA-256) and build them as static libraries into `.deps/<platform>`:

```sh
scripts/network-deps/prepare-host-deps.sh   # desktop simulation build
scripts/network-deps/prepare-ps5-deps.sh    # PS5 build
```

`scripts/build-homebrew.sh` and `scripts/build-homebrew-ps5.sh` run them when the libraries are missing. Each run
starts from clean source, build and install folders. The PS5 build applies `patches/curl-ps5-native-nonblock.patch`,
uses relative source paths so the archives are reproducible, and writes `.deps/ps5/build-manifest.txt`. CMake checks
the PS5 archives, the curl headers and the CA bundle against `steam_network_probe` in `dependencies.lock.json`.

Both builds enable Mbed TLS `MBEDTLS_THREADING_C` and `MBEDTLS_THREADING_PTHREAD` through its source configuration
header. TLS shares entropy/DRBG and PSA state across clients; separate HTTP clients still require these locks.
Changing this configuration requires rebuilding Mbed TLS, curl and all title objects together because the public
context layouts include mutex fields. The installed configuration hash is pinned for both platforms, and PS5
archive hashes remain pinned. The host suite tests concurrent PSA key operations and independent HTTPS clients.
