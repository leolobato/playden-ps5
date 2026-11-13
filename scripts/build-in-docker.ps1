# Build the PS5 package in a Linux container (see docs/building.md, "Build with Docker").
# SPDX-License-Identifier: GPL-3.0-or-later
Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
Push-Location $Root
try {
    docker compose build
    if ($LASTEXITCODE -ne 0) { throw "docker compose build failed" }
    docker compose run --rm playden-builder
    if ($LASTEXITCODE -ne 0) { throw "the container build failed" }
    Write-Host "PS5 package (not deployed): $Root\out\FAKE77031"
} finally {
    Pop-Location
}
