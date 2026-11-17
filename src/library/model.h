#pragma once
#include "library/store.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace playden {
// Why a store's library could not be read or kept, the same for every store.
enum class LibraryError {
    kNone,
    kCanceled,
    kNetwork,
    kProtocol,
    kCredentialsRevoked,
    kNoSession,
    kPersistence,
};

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
    // Shared by another member of the account's Steam family, not owned; the Library marks it.
    bool family_shared = false;
    // A wide picture for the game's page, from a store that names one per game (GOG's gamesdb); Steam's comes from
    // the game ID instead. Kept last: tests brace-initialise the fields above in order.
    std::string hero_source;
};

// A collection a store keeps for the player (a Steam collection, a GOG tag), read-only: the store's ID for it, its
// name, and its games (some may not be in the library).
struct LibraryCollection {
    std::string id;  // 1-64 bytes of printable ASCII without ':' or space (ValidLibraryCollectionId)
    std::string name;
    std::vector<GameId> members;

};
inline bool operator==(const LibraryCollection& left, const LibraryCollection& right) {
    return left.id == right.id && left.name == right.name && left.members == right.members;
}
// The ID a store collection may have: what the organization file keeps as its scope. Steam's are "uc-" or "srm-"
// and base64-like text ('+', '=', '*' seen), at most 40 bytes so far; ':' would be ambiguous in a scope.
inline bool ValidLibraryCollectionId(const std::string& id) {
    if (id.empty() || id.size() > 64) return false;
    for (const unsigned char byte : id)
        if (byte <= 0x20 || byte >= 0x7f || byte == ':') return false;
    return true;
}
} // namespace playden
