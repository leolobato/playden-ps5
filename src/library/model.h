#pragma once
#include "library/store.h"

#include <cstdint>
#include <optional>
#include <string>

namespace playden {
struct GameSummary {
    GameId id;  // stable across refreshes and account sessions; default: invalid
    std::string title;
    std::string artwork_source;
    std::string description;
    std::string developer;
    std::optional<std::uint64_t> playtime_minutes;
    std::optional<std::uint64_t> last_played_unix_seconds;
    // Earliest license acquisition, from the Steam CM; absent when CM was unavailable.
    std::optional<std::uint64_t> added_at_unix_seconds;
    // A Steam demo (PICS type "demo"), which the Library can hide.
    bool demo = false;
};
} // namespace playden
