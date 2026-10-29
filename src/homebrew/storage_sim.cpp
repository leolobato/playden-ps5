// storage_sim.cpp - Deterministic storage accounting for the UI simulation.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: MIT

#include "homebrew/storage_sim.hpp"

#include <algorithm>
#include <limits>
#include <set>

namespace playden::homebrew {

namespace {

template <typename Items, typename Predicate>
auto find_item(Items& items, Predicate predicate) {
    return std::find_if(items.begin(), items.end(), predicate);
}

std::uint64_t add_saturated(std::uint64_t left, std::uint64_t right) noexcept {
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    return right > maximum - left ? maximum : left + right;
}

} // namespace

InstallAssessment assess_install(const State& state, GameId game_id,
                                  const std::string& volume_id, std::uint64_t safety_margin_bytes,
                                  const GameVersion* version, const std::string& language,
                                  const MeasuredInstallSize* measured) noexcept {
    InstallAssessment result;
    const auto game = find_item(state.games, [game_id](const Game& value) {
        return value.id == game_id;
    });
    if (game != state.games.end()) {
        result.download_bytes = game->download_bytes;
        result.installed_bytes = game->install_bytes;
        if (version != nullptr && version->install_bytes) {
            result.download_bytes = version->download_bytes;
            result.installed_bytes = version->install_bytes;
        }
        if (version != nullptr && !language.empty())
            for (const auto& size : version->languages)
                if (size.language == language && size.install_bytes) {
                    result.download_bytes = size.download_bytes ? std::optional<std::uint64_t>(size.download_bytes) : std::nullopt;
                    result.installed_bytes = size.install_bytes;
                }
        if (measured != nullptr && measured->install_bytes) {
            result.installed_bytes = measured->install_bytes;
            if (measured->download_bytes) result.download_bytes = measured->download_bytes;
        }
        if (result.installed_bytes) result.required_bytes = add_saturated(*result.installed_bytes, safety_margin_bytes);
    }

    const auto volume = find_item(state.volumes, [&volume_id](const Volume& value) {
        return value.id == volume_id;
    });
    if (volume == state.volumes.end()) return result;

    result.reserved_bytes = volume->reserved_bytes;
    if (volume->capacity_known) {
        const std::uint64_t occupied = std::min(
            volume->capacity_bytes, add_saturated(volume->used_bytes, volume->reserved_bytes));
        result.free_bytes = volume->capacity_bytes - occupied;
    }
    if (!volume->available) {
        result.readiness = InstallReadiness::Unavailable;
    } else if (!volume->writable) {
        result.readiness = InstallReadiness::ReadOnly;
    } else if (!volume->capacity_known) {
        result.readiness = InstallReadiness::UnknownCapacity;
    } else if (*result.free_bytes < safety_margin_bytes) {
        result.readiness = InstallReadiness::Insufficient;
    } else if (game == state.games.end() || !result.installed_bytes) {
        result.readiness = InstallReadiness::UnknownSize;
    } else if (*result.required_bytes > *result.free_bytes) {
        result.readiness = InstallReadiness::Insufficient;
    } else {
        result.readiness = InstallReadiness::Ready;
    }
    return result;
}

void reconcile_storage(State& state) noexcept {
    for (auto& job : state.jobs) {
        if (job.state != JobState::Downloaded || job.completion_applied) continue;
        if (job.kind == "install") {
            std::uint64_t footprint = job.reserved_bytes;
            if (footprint == 0) {
                const auto game = find_item(state.games, [&job](const Game& value) {
                    return value.id == job.game_id;
                });
                if (game != state.games.end() && game->install_bytes)
                    footprint = *game->install_bytes;
            }
            auto installation =
                find_item(state.installations, [&job](const Installation& value) {
                    return value.game_id == job.game_id;
                });
            if (installation == state.installations.end()) {
                Installation added;
                added.game_id = job.game_id;
                added.volume_id = job.volume_id;
                added.installed_bytes = footprint;
                added.runtime_ready = false;
                state.installations.push_back(std::move(added));
            } else {
                installation->volume_id = job.volume_id;
                installation->installed_bytes = footprint;
                installation->available = true;
                installation->runtime_ready = false;
            }
            for (auto& duplicate : state.jobs) {
                if (duplicate.kind == "install" && duplicate.game_id == job.game_id)
                    duplicate.reservation_held = false;
            }
        } else if (job.kind == "uninstall") {
            const auto installation =
                find_item(state.installations, [&job](const Installation& value) {
                    return value.game_id == job.game_id;
                });
            if (installation != state.installations.end()) {
                installation->installed_bytes = 0;
                installation->available = false;
                installation->runtime_ready = false;
            }
            job.reservation_held = false;
        }
        job.completion_applied = true;
    }

    for (auto& volume : state.volumes) {
        volume.used_bytes = volume.base_used_bytes;
        volume.reserved_bytes = volume.base_reserved_bytes;
    }

    std::set<GameId> installed_games;
    for (const auto& installation : state.installations) {
        if (installation.installed_bytes == 0) continue;
        installed_games.insert(installation.game_id);
        const auto volume =
            find_item(state.volumes, [&installation](const Volume& value) {
                return value.id == installation.volume_id;
            });
        if (volume != state.volumes.end())
            volume->used_bytes = add_saturated(volume->used_bytes, installation.installed_bytes);
    }

    std::set<GameId> reserved_games;
    for (const auto& job : state.jobs) {
        if (job.kind != "install" || !job.reservation_held || job.reserved_bytes == 0 ||
            installed_games.contains(job.game_id) || reserved_games.contains(job.game_id))
            continue;
        const auto volume = find_item(state.volumes, [&job](const Volume& value) {
            return value.id == job.volume_id;
        });
        if (volume == state.volumes.end()) continue;
        volume->reserved_bytes = add_saturated(volume->reserved_bytes, job.reserved_bytes);
        reserved_games.insert(job.game_id);
    }
}

} // namespace playden::homebrew
