// storage_sim.hpp - Deterministic storage accounting for the UI simulation.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "homebrew/simulation.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace playden::homebrew {

enum class InstallReadiness {
    Ready,
    UnknownSize,
    UnknownCapacity,
    Insufficient,
    Unavailable,
    ReadOnly,
};

struct InstallAssessment {
    InstallReadiness readiness = InstallReadiness::Unavailable;
    std::optional<std::uint64_t> download_bytes;
    std::optional<std::uint64_t> installed_bytes;
    std::optional<std::uint64_t> required_bytes;
    std::optional<std::uint64_t> free_bytes;
    std::optional<std::uint64_t> reserved_bytes;
};

// `version`: the version chosen for this install; its sizes, when known, replace the game's. `language`: the
// content language chosen for it; that language's sizes on `version`, when known, replace both. `measured`: that
// choice's size from its manifests, which replaces them all.
[[nodiscard]] InstallAssessment assess_install(const State& state, GameId game_id,
                                               const std::string& volume_id,
                                               std::uint64_t safety_margin_bytes = 0,
                                               const GameVersion* version = nullptr,
                                               const std::string& language = {},
                                               const MeasuredInstallSize* measured = nullptr) noexcept;

void reconcile_storage(State& state) noexcept;

} // namespace playden::homebrew
