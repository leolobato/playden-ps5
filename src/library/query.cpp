#include "library/query.h"
#include <utf8proc.h>
#include <algorithm>
#include <cstdlib>
#include <unordered_set>

namespace playden {
std::string FoldLibraryText(const std::string& value) {
    utf8proc_uint8_t* mapped = nullptr;
    const auto count = utf8proc_map(reinterpret_cast<const utf8proc_uint8_t*>(value.data()),
        static_cast<utf8proc_ssize_t>(value.size()), &mapped,
        static_cast<utf8proc_option_t>(UTF8PROC_STABLE | UTF8PROC_CASEFOLD));
    if (count < 0) return value; // Malformed input has deterministic byte-exact behavior.
    std::string result(reinterpret_cast<const char*>(mapped), static_cast<std::size_t>(count));
    std::free(mapped);
    return result;
}
std::string TrimLibraryQuery(std::string_view value) {
    const auto* bytes = reinterpret_cast<const utf8proc_uint8_t*>(value.data());
    const auto size = static_cast<utf8proc_ssize_t>(value.size());
    std::size_t first = value.size();
    std::size_t end = 0;
    for (utf8proc_ssize_t offset = 0; offset < size;) {
        utf8proc_int32_t code_point = -1;
        auto used = utf8proc_iterate(bytes + offset, size - offset, &code_point);
        if (used <= 0) used = 1;  // a malformed byte is text
        const auto category = code_point < 0 ? UTF8PROC_CATEGORY_CN : utf8proc_category(code_point);
        const bool space = code_point == '\t' || code_point == '\n' || code_point == '\v' || code_point == '\f' ||
                           code_point == '\r' || code_point == 0x85 || category == UTF8PROC_CATEGORY_ZS ||
                           category == UTF8PROC_CATEGORY_ZL || category == UTF8PROC_CATEGORY_ZP;
        if (!space) {
            first = std::min(first, static_cast<std::size_t>(offset));
            end = static_cast<std::size_t>(offset + used);
        }
        offset += used;
    }
    return first < end ? std::string(value.substr(first, end - first)) : std::string();
}
namespace {
struct Match { GameId id; std::string title; };
}

std::vector<GameId> QueryLibrary(const std::vector<GameSummary>& games,
                                const LibraryQuery& query, LibrarySort sort) {
    const auto text = FoldLibraryText(query.text);
    const auto developer = FoldLibraryText(query.developer);
    std::vector<Match> matches;
    matches.reserve(games.size());
    std::unordered_set<GameId> seen;
    for (const auto& game : games) {
        if (!game.id.valid() || !seen.insert(game.id).second) continue;
        auto title = FoldLibraryText(game.title);
        if (!text.empty() && title.find(text) == std::string::npos) continue;
        if (!developer.empty() && FoldLibraryText(game.developer) != developer) continue;
        matches.push_back({game.id, std::move(title)});
    }
    std::sort(matches.begin(), matches.end(), [sort](const Match& a, const Match& b) {
        if (sort == LibrarySort::AppId || a.title == b.title) return a.id < b.id;
        return sort == LibrarySort::TitleDescending ? a.title > b.title : a.title < b.title;
    });
    std::vector<GameId> result;
    result.reserve(matches.size());
    for (const auto& match : matches) result.push_back(match.id);
    return result;
}
} // namespace playden
