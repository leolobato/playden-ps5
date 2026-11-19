// 运行期验证：加载 assets/translations，选中文，检查译文真的被取到。
#include "homebrew/i18n.hpp"
#include <cstdio>

int main(int argc, char **argv)
{
    const std::string dir = argc > 1 ? argv[1] : "assets/translations";
    const std::size_t count = playden::homebrew::i18n::load(dir);
    std::printf("catalogs loaded: %zu\n", count);
    for (const auto &lang : playden::homebrew::i18n::available())
        std::printf("  %s = %s\n", lang.code.c_str(), lang.name.c_str());

    if (!playden::homebrew::i18n::select("zh")) { std::printf("select zh failed\n"); return 1; }
    const char *samples[] = {
        "Library", "Downloads", "Play", "Language", "Welcome to Playden",
        "Saves are kept when a game is uninstalled", "Cross Change",
        "This string is not in the catalog", "ELDEN RING",
    };
    for (const char *sample : samples)
        std::printf("%-46s -> %s\n", sample, playden::homebrew::tr(sample).c_str());

    // Selecting English again must give back the source text.
    playden::homebrew::i18n::select("en");
    std::printf("after select(en): Library -> %s\n", playden::homebrew::tr("Library").c_str());
    return 0;
}
