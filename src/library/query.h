#pragma once
#include "library/model.h"
#include <string_view>
#include <vector>

namespace playden {
enum class LibrarySort { TitleAscending, TitleDescending, AppId };
struct LibraryQuery {
    std::string text;
    std::string developer; // Empty means all; exact case-folded developer label otherwise.
};
// Shared deterministic folding; malformed UTF-8 retains byte-exact behavior.
std::string FoldLibraryText(const std::string& value);
// A search as the player typed it, without the spaces around it: any Unicode space (U+00A0, U+3000…), line or
// paragraph separator, tab, CR and LF. Spaces inside are kept; a malformed byte counts as text and is kept as it is.
std::string TrimLibraryQuery(std::string_view value);
// Unicode case folding, then bytewise UTF-8 order; deliberately not locale collation.
// Only supplied ownership records are queried. No install/playability state is inferred.
std::vector<GameId> QueryLibrary(const std::vector<GameSummary>& games,
                                const LibraryQuery& query = {},
                                LibrarySort sort = LibrarySort::TitleAscending);
} // namespace playden
