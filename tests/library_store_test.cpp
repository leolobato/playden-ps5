#include "library/store.h"

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <limits>
#include <set>

using namespace playden;

static void Check(bool condition, const char* description) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", description);
    std::exit(1);
  }
}

int main() {
  constexpr const char* kUuid = "1b4e28ba-2fa1-11d2-883f-0016d3cca427";

  Check(!GameId{}.valid(), "a default GameId is invalid");
  Check(!GameId::Steam(0).valid() && GameId::Steam(0) == GameId{}, "Steam app 0 is invalid");
  Check(GameId::Steam(440).valid() && GameId::Steam(440).value == "440", "Steam value is canonical decimal");
  Check(GameId::Steam(440).steam_app() == 440u, "steam_app returns the app ID");
  Check(!GameId::Local(kUuid).steam_app(), "a Local game has no Steam app");
  Check(GameId::Local(kUuid).valid(), "a lower-case UUID is a valid Local value");
  Check(!GameId::Local("1B4E28BA-2FA1-11D2-883F-0016D3CCA427").valid(), "an upper-case UUID is not a valid Local value");
  Check(!(GameId{Store::kSteam, "0440"}).valid(), "a Steam value with a leading zero is invalid");
  Check(!(GameId{Store::kEpic, "1"}).valid(), "Epic is not implemented yet");

  constexpr std::uint64_t kGogProduct = 1207658924;
  const auto gog = GameId::Gog(kGogProduct);
  Check(gog.valid() && gog.store == Store::kGog && gog.value == "1207658924", "GOG value is canonical decimal");
  Check(gog.gog_product() == kGogProduct && !gog.steam_app(), "gog_product returns the product ID");
  Check(!GameId::Steam(440).gog_product() && !GameId::Local(kUuid).gog_product(), "only GOG games have a product");
  Check(GameId::Gog(0) == GameId{} && !GameId::Gog(0).valid(), "GOG product 0 is the default GameId");
  Check(!(GameId{Store::kGog, "01"}).valid() && !(GameId{Store::kGog, "0"}).valid() &&
            !(GameId{Store::kGog, ""}).valid() && !(GameId{Store::kGog, "+1"}).valid() &&
            !(GameId{Store::kGog, "1 "}).valid(),
        "a GOG value with a leading zero, sign or space is invalid");
  Check(!(GameId{Store::kGog, "18446744073709551616"}).valid() && !(GameId{Store::kGog, "99999999999999999999"}).valid(),
        "a GOG value past UINT64_MAX is invalid");
  Check(ParseGogValue("18446744073709551615") == std::numeric_limits<std::uint64_t>::max() &&
            !ParseGogValue("18446744073709551616") && !ParseGogValue("100000000000000000000") && !ParseGogValue("-1"),
        "ParseGogValue takes canonical u64 decimals only");

  const auto max = GameId::Steam(std::numeric_limits<std::uint32_t>::max());
  Check(ToKey(max) == "4294967295" && ParseKey("4294967295") == max, "UINT32_MAX round trips");
  Check(ToKey(GameId::Steam(440)) == "440", "Steam key is the bare app ID");
  Check(ParseKey("440") == GameId::Steam(440), "bare digits parse as Steam");
  Check(ToKey(GameId::Local(kUuid)) == std::string("local-") + kUuid, "Local key has the local- prefix");
  Check(ParseKey(std::string("local-") + kUuid) == GameId::Local(kUuid), "Local key round trips");
  Check(ToKey(gog) == "gog-1207658924" && ParseKey("gog-1207658924") == gog, "GOG key has the gog- prefix");
  const auto gog_max = GameId::Gog(std::numeric_limits<std::uint64_t>::max());
  Check(ToKey(gog_max) == "gog-18446744073709551615" && ParseKey("gog-18446744073709551615") == gog_max &&
            ToInstallId(gog_max) == "gog-18446744073709551615" && ParseInstallId("gog-18446744073709551615") == gog_max,
        "UINT64_MAX round trips as a GOG product");
  Check(!ParseKey("gog-18446744073709551616") && !ParseInstallId("gog-18446744073709551616"),
        "one past UINT64_MAX is refused");

  for (const char* rejected : {"", "0", "0440", "4294967296", "99999999999999999999", "+1", "-1", " 440", "440 ",
                               "440x", "local-", "local-1B4E28BA-2FA1-11D2-883F-0016D3CCA427",
                               "local-1b4e28ba2fa111d2883f0016d3cca427", "local-1b4e28ba-2fa1-11d2-883f-0016d3cca42",
                               "local-1b4e28ba-2fa1-11d2-883f-0016d3cca427-", "steam-440", "LOCAL-x", "gog-", "gog-0", "gog-01",
                               "GOG-1", "gog-+1", "gog--1", "gog-1 ", "gog-18446744073709551616",
                               "gog-99999999999999999999", "epic-1"}) {
    if (ParseKey(rejected)) {
      std::fprintf(stderr, "FAIL: ParseKey accepted \"%s\"\n", rejected);
      return 1;
    }
  }

  Check(GameId::Steam(9) < GameId::Steam(10), "Steam orders numerically");
  Check(!(GameId::Steam(10) < GameId::Steam(9)), "Steam numeric order is strict");
  Check(GameId::Steam(4294967295u) < GameId::Local(kUuid), "Steam sorts before Local");
  Check(GameId::Gog(9) < GameId::Gog(10) && !(GameId::Gog(10) < GameId::Gog(9)), "GOG orders numerically");
  Check(GameId::Steam(4294967295u) < GameId::Gog(1) && GameId::Gog(1) < GameId::Local(kUuid) &&
            GameId::Gog(std::numeric_limits<std::uint64_t>::max()) < GameId::Local(kUuid),
        "GOG sorts between Steam and Local");
  const std::set<GameId> ordered{GameId::Steam(100), GameId::Steam(9), GameId::Steam(20)};
  Check(*ordered.begin() == GameId::Steam(9) && *ordered.rbegin() == GameId::Steam(100), "set order is numeric");
  Check(GameId::Steam(440) != GameId::Steam(441) && !(GameId::Steam(440) != GameId::Steam(440)), "inequality");
  Check(GameId{Store::kSteam, "1"} != GameId{Store::kLocal, "1"}, "the store is part of the identity");

  const std::hash<GameId> hash;
  Check(hash(GameId::Steam(440)) == hash(*ParseKey("440")), "equal Steam ids hash equally");
  Check(hash(GameId::Local(kUuid)) == hash(*ParseKey(std::string("local-") + kUuid)), "equal Local ids hash equally");
  Check(hash(gog) == hash(*ParseKey("gog-1207658924")), "equal GOG ids hash equally");

  // A GOG product and a Steam app with the same number are different games.
  Check(GameId::Gog(440) != GameId::Steam(440), "GOG 440 is not Steam 440");
  Check(ToKey(GameId::Gog(440)) != ToKey(GameId::Steam(440)) &&
            ToInstallId(GameId::Gog(440)) != ToInstallId(GameId::Steam(440)) &&
            GeneratedRecipeId(GameId::Gog(440)) != GeneratedRecipeId(GameId::Steam(440)) &&
            ProfileId(GameId::Gog(440)) != ProfileId(GameId::Steam(440)),
        "GOG and Steam derived IDs differ");
  Check(hash(GameId::Gog(440)) != hash(GameId::Steam(440)), "GOG and Steam ids hash differently");
  Check(ParseKey("440") == GameId::Steam(440) && ParseKey("gog-440") == GameId::Gog(440), "keys name their store");
  Check(ParseInstallId("steam-440") == GameId::Steam(440) && ParseInstallId("gog-440") == GameId::Gog(440),
        "install IDs name their store");

  // Steam derived IDs keep today's exact text.
  const auto steam = GameId::Steam(440);
  Check(ToInstallId(steam) == "steam-440", "Steam install ID");
  Check(ParseInstallId("steam-440") == steam, "Steam install ID parses");
  Check(GeneratedRecipeId(steam) == "steam-440-auto", "Steam generated recipe ID");
  Check(ProfileId(steam) == "playden-steam-440", "Steam profile ID");
  Check(!ParseInstallId("steam-0440") && !ParseInstallId("steam-") && !ParseInstallId("440") && !ParseInstallId("epic-1") &&
            !ParseInstallId("gog-0") && !ParseInstallId("gog-") && !ParseInstallId("gog-01"),
        "malformed install IDs are rejected");

  Check(ToInstallId(gog) == "gog-1207658924", "GOG install ID");
  Check(ParseInstallId("gog-1207658924") == gog, "GOG install ID parses");
  Check(GeneratedRecipeId(gog) == "gog-1207658924-auto", "GOG generated recipe ID");
  Check(ProfileId(gog) == "playden-gog-1207658924", "GOG profile ID");

  const auto local = GameId::Local(kUuid);
  Check(ToInstallId(local) == std::string("local-") + kUuid, "Local install ID");
  Check(ParseInstallId(ToInstallId(local)) == local, "Local install ID parses");
  Check(GeneratedRecipeId(local) == std::string("local-") + kUuid + "-auto", "Local generated recipe ID");
  Check(ProfileId(local) == std::string("playden-local-") + kUuid, "Local profile ID");

  for (const auto& id : {max, local, gog_max}) {
    for (const auto& derived : {ToKey(id), ToInstallId(id), GeneratedRecipeId(id), ProfileId(id)}) {
      bool allowed = derived.size() <= 64;
      for (const char character : derived)
        allowed = allowed && ((character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') ||
                              character == '-');
      if (!allowed) {
        std::fprintf(stderr, "FAIL: derived ID \"%s\" is not [a-z0-9-] within 64 bytes\n", derived.c_str());
        return 1;
      }
    }
    Check(ToKey(id).size() <= 48, "keys fit in 48 bytes");
  }

  Check(Traits(Store::kSteam).key == "steam" && Traits(Store::kSteam).name == "Steam" &&
            Traits(Store::kSteam).needs_account && Traits(Store::kSteam).cloud_saves && Traits(Store::kSteam).downloads,
        "Steam traits");
  Check(Traits(Store::kLocal).key == "local" && Traits(Store::kLocal).name == "Imported" &&
            !Traits(Store::kLocal).needs_account && !Traits(Store::kLocal).cloud_saves && !Traits(Store::kLocal).downloads,
        "Local traits");
  Check(Traits(Store::kGog).key == "gog" && Traits(Store::kGog).name == "GOG" && Traits(Store::kGog).needs_account &&
            !Traits(Store::kGog).cloud_saves && Traits(Store::kGog).downloads,
        "GOG traits");
  Check(Traits(Store::kEpic).key == "epic" && Traits(Store::kEpic).name == "Epic" && Traits(Store::kEpic).needs_account &&
            !Traits(Store::kEpic).cloud_saves && Traits(Store::kEpic).downloads,
        "Epic traits");
  Check(static_cast<int>(Store::kSteam) == 1 && static_cast<int>(Store::kGog) == 2 &&
            static_cast<int>(Store::kEpic) == 3 && static_cast<int>(Store::kLocal) == 4,
        "persisted store values");

  Check(ParseStoreKey("local") == Store::kLocal && ParseStoreKey("gog") == Store::kGog, "stored stores parse");
  for (const char* refused : {"steam", "epic", "GOG", "Local", "", "gog "})
    Check(!ParseStoreKey(refused), "Steam, unknown and malformed store keys are refused");

  // Keys a newer Playden writes for a store this build doesn't read; keys of known stores are never among them.
  for (const char* unknown : {"epic-0f8fad5bd9cb469fa16570867728950e", "itch-abc", "x-1", "store2-a.b_c-d",
                              "abcdefghijklmnop-1"})
    Check(IsUnknownStoreKey(unknown), "an unknown store's key is recognised");
  const std::string longest = "epic-" + std::string(59, 'a');
  Check(longest.size() == 64 && IsUnknownStoreKey(longest), "a 64-byte unknown store key is recognised");
  for (const std::string known :
       {std::string("gog-1"), std::string("gog-0"), std::string("gog-01"), std::string("local-x"),
        std::string("local-UPPER"), std::string("steam-440"), std::string("440"), std::string("0440"),
        std::string("-x"), std::string("epic-"), std::string("Epic-1"), std::string("epic"), std::string(""),
        std::string("1epic-1"), std::string("abcdefghijklmnopq-1"), std::string("epic-A"), std::string("epic-a/b"),
        std::string("ep_ic-1"), longest + "a"})
    Check(!IsUnknownStoreKey(known), "a known store's key or a malformed key is not an unknown store's");

  std::puts("library_store: ok");
  return 0;
}
