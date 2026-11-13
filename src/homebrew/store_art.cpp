// store_art.cpp - A game's artwork addresses, from its store.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: MIT
#include "homebrew/store_art.hpp"

#include "gog/art_url.h"
#include "homebrew/simulation.hpp"

namespace playden::homebrew {
namespace {

std::string SteamArt(const GameId& game, const char* name) {
  if (!game.steam_app()) return {};
  return "https://cdn.cloudflare.steamstatic.com/steam/apps/" + ToKey(game) + "/" + name;
}

}  // namespace

std::string CoverUrl(const GameId& game) { return SteamArt(game, "library_600x900.jpg"); }
std::string HeroUrl(const GameId& game) { return SteamArt(game, "library_hero.jpg"); }
std::string LogoUrl(const GameId& game) { return SteamArt(game, "logo.png"); }
// A GOG game's art comes from its gamesdb entry, kept with the game; only the one address shape the artwork backend
// fetches is used. GOG has no logo: gamesdb's "logo" is key art, not a transparent title.
std::string CoverUrl(const Game& game) {
  if (game.id.store == Store::kGog)
    return game.id.valid() && gog::IsArtUrl(game.artwork_source, gog::ArtKind::kCover) ? game.artwork_source
                                                                                         : std::string();
  return CoverUrl(game.id);
}
std::string HeroUrl(const Game& game) {
  if (game.id.store == Store::kGog)
    return game.id.valid() && gog::IsArtUrl(game.hero_source, gog::ArtKind::kHero) ? game.hero_source : std::string();
  return HeroUrl(game.id);
}
std::string LogoUrl(const Game& game) { return LogoUrl(game.id); }

bool LiveStoreArt(const State& state, const GameId& game) {
  if (!game.valid()) return false;
  return game.store != Store::kSteam || !state.account().id.empty();
}

}  // namespace playden::homebrew
