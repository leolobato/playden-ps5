// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The GOG Galaxy client's identity and GOG's hosts, in one place.
//
// The client ID and secret are the public pair every Galaxy client uses (gogdl, Heroic, lgogdownloader, minigalaxy);
// GOG_PROTOCOL.md §1. They are not secrets, but with a sign-in code they get an account's tokens, so they never go
// into a log, and neither does a URL that carries them.

namespace playden::gog {

inline constexpr char kClientId[] = "46899977096215655";
inline constexpr char kClientSecret[] = "9d85c43b1482497dbbce61f6e4aa173a433796eeae2ca8c5f6129f2dc4de46d9";
// The only redirect GOG's login accepts for this client. The code arrives in its query.
inline constexpr char kRedirectUri[] = "https://embed.gog.com/on_login_success?origin=client";

inline constexpr char kAuthHost[] = "auth.gog.com";                    // sign-in page and tokens
inline constexpr char kEmbedHost[] = "embed.gog.com";                  // user data, owned games
inline constexpr char kGamesDbHost[] = "gamesdb.gog.com";              // titles and art
inline constexpr char kApiHost[] = "api.gog.com";                      // products
inline constexpr char kContentSystemHost[] = "content-system.gog.com";  // builds, secure links, dependencies
inline constexpr char kImagesHost[] = "images.gog.com";                // art

}  // namespace playden::gog
