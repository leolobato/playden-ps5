#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace playden {

// Where a game comes from. The values are persisted: never renumber them.
enum class Store : std::uint8_t { kSteam = 1, kGog = 2, kEpic = 3, kLocal = 4 };

struct StoreTraits {
  Store store;
  std::string_view key;   // "steam", "local": key prefix and file names
  std::string_view name;  // "Steam", "Imported": UI and title suffix
  bool needs_account;
  bool cloud_saves;
  bool downloads;         // Local copies instead
};
const StoreTraits& Traits(Store store);
// The store named by Traits(store).key in a file ("local", "gog"). Only stores whose games can be
// stored are accepted: Local and GOG (Steam is never written, it is the default; Epic has no games yet).
std::optional<Store> ParseStoreKey(std::string_view key);

// Whether key looks like a game key a newer Playden writes for a store this build doesn't read: <prefix>-<rest>,
// the prefix 1-16 bytes of [a-z0-9] starting with a letter and not "local", "gog" or "steam", the rest 1 or more
// bytes of [a-z0-9._-], at most 64 bytes in all ("epic-0f8f…"). A reader skips such a key instead of refusing its
// file. A malformed key of a store this build knows ("gog-01", "local-UPPER", "0440") is not one: it stays damage.
bool IsUnknownStoreKey(std::string_view key);

// A GOG product ID: canonical decimal (^[1-9][0-9]{0,19}$, no sign, space or leading zero) that fits in 64 bits.
std::optional<std::uint64_t> ParseGogValue(std::string_view text);

// A game's identity across stores. Steam values are canonical decimal app IDs (1..UINT32_MAX, no leading zero);
// GOG values are canonical decimal product IDs (1..UINT64_MAX, ParseGogValue); Local values are lower-case UUIDs.
// A default-constructed GameId is invalid.
struct GameId {
  Store store = Store::kSteam;
  std::string value;

  bool valid() const;
  // Steam only: the boundary into Steam code and every u32-keyed format.
  std::optional<std::uint32_t> steam_app() const;
  // GOG only: the product ID.
  std::optional<std::uint64_t> gog_product() const;

  static GameId Steam(std::uint32_t app);
  static GameId Gog(std::uint64_t product);  // 0 gives the default (invalid) GameId
  static GameId Local(std::string uuid);
};

bool operator==(const GameId& left, const GameId& right);
bool operator!=(const GameId& left, const GameId& right);
// Store order (Steam, GOG, Local), then Steam and GOG numerically, others by value.
bool operator<(const GameId& left, const GameId& right);

// Row and file key: Steam "440", GOG "gog-<id>", Local "local-<uuid>". ParseKey accepts only those three forms, with
// canonical values.
std::string ToKey(const GameId& id);
std::optional<GameId> ParseKey(std::string_view key);

// Derived identifiers, [a-z0-9-] and at most 64 bytes. GOG's and Local's install ID is their key.
std::string ToInstallId(const GameId& id);  // "steam-440", "gog-<id>", "local-<uuid>"
std::optional<GameId> ParseInstallId(std::string_view install_id);
std::string GeneratedRecipeId(const GameId& id);  // "steam-440-auto", "gog-<id>-auto", "local-<uuid>-auto"
std::string ProfileId(const GameId& id);          // "playden-steam-440", "playden-gog-<id>", "playden-local-<uuid>"

}  // namespace playden

namespace std {
template <>
struct hash<playden::GameId> {
  std::size_t operator()(const playden::GameId& id) const noexcept {
    const auto value = std::hash<std::string>{}(id.value);
    return value ^ (static_cast<std::size_t>(id.store) + 0x9e3779b9u + (value << 6) + (value >> 2));
  }
};
}  // namespace std
