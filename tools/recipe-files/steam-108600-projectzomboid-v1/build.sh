#!/usr/bin/env bash
# Builds the class in runtime/recipes/files from the source here, with a JDK 17 or newer. A Java 17 class also loads on
# the Java 25 the game ships with now.
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
javac --release 17 -g:none -proc:none -d "$out" "$here/zombie/DebugFileWatcher.java" "$here/zombie/PredicatedFileWatcher.java"
cp "$out/zombie/DebugFileWatcher.class" "$here/../../../runtime/recipes/files/steam-108600-projectzomboid-v1/zombie/"
