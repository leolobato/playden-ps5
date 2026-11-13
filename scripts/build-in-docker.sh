#!/usr/bin/env bash
# Build the PS5 package in a Linux container (see docs/building.md, "Build with Docker").
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"
docker compose build
docker compose run --rm playden-builder
echo "PS5 package (not deployed): $root/out/FAKE77031"
