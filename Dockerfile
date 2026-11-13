# SPDX-License-Identifier: GPL-3.0-or-later
#
# Linux build environment for the PS5 package (scripts/build-homebrew-ps5.sh). It needs only what the scripts
# expect on PATH: LLVM 18, CMake, Ninja, Python 3 and the tools fetch-deps.sh uses. The PS5 SDKs are downloaded
# into a volume by scripts/fetch-deps.sh, hash-checked against dependencies.lock.json.
# It does not run the host tests: the host build supports macOS only (cmake/HomebrewUI.cmake).
# Known issue: the PS5 archives built here do not yet match the hashes pinned in dependencies.lock.json.

FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
ENV LANG=C.UTF-8

# LLVM 18.1.8 from apt.llvm.org, the version the pinned archives name. Ubuntu's own clang-18 is 18.1.3.
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates curl gnupg \
    && curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key | gpg --dearmor -o /usr/share/keyrings/llvm.gpg \
    && echo "deb [signed-by=/usr/share/keyrings/llvm.gpg] https://apt.llvm.org/noble/ llvm-toolchain-noble-18 main" \
        > /etc/apt/sources.list.d/llvm.list \
    && rm -rf /var/lib/apt/lists/*

RUN apt-get update && apt-get install -y --no-install-recommends \
    bash \
    build-essential \
    bzip2 \
    clang-18=1:18.1.8* \
    cmake \
    git \
    libdigest-sha-perl \
    lld-18=1:18.1.8* \
    llvm-18=1:18.1.8* \
    llvm-18-dev=1:18.1.8* \
    ninja-build \
    patch \
    pkg-config \
    python3 \
    tar \
    unzip \
    wget \
    xz-utils \
    zstd \
    && rm -rf /var/lib/apt/lists/* \
    && clang-18 --version | grep -q 'version 18.1.8'

RUN update-alternatives --install /usr/bin/clang clang /usr/bin/clang-18 100 && \
    update-alternatives --install /usr/bin/clang++ clang++ /usr/bin/clang++-18 100 && \
    update-alternatives --install /usr/bin/llvm-config llvm-config /usr/bin/llvm-config-18 100

ENV LLVM_CONFIG=llvm-config-18
ENV HOST_CXX=clang++-18

# The checkout is a bind mount owned by another user; git must still read the submodules.
RUN git config --system --add safe.directory '*'

WORKDIR /workspace

CMD ["/bin/bash", "-c", "scripts/fetch-deps.sh && scripts/build-lapy-helper.sh && scripts/build-homebrew-ps5.sh"]
