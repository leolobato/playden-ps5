#pragma once
// store_art.hpp - A game's artwork addresses, from its store.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: MIT
#include "library/store.h"

#include <string>

namespace playden::homebrew {

struct Game;
struct State;

// The store's art for a game: its cover (600x900), library hero and logo. Steam games get the
// Steam CDN addresses from their ID (the artwork backend's CanonicalKey accepts exactly these).
// A GOG game's cover and hero are its gamesdb addresses, kept on the Game (artwork_source,
// hero_source), so only the Game overloads have them; GOG has no logo. Imported games have none.
// An empty address means no art (the tile shows the title).
std::string CoverUrl(const GameId& game);
std::string HeroUrl(const GameId& game);
std::string LogoUrl(const GameId& game);
std::string CoverUrl(const Game& game);
std::string HeroUrl(const Game& game);
std::string LogoUrl(const Game& game);

// Whether the view asks for a game's store art: a Steam game's while Steam has an account, as before; any other
// store's whatever Steam's state (a GOG-only player has no Steam account, and installed GOG games keep their art
// after a GOG sign-out).
bool LiveStoreArt(const State& state, const GameId& game);

}  // namespace playden::homebrew
