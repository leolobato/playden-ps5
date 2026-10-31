#include "homebrew/artwork_cache.hpp"
// Playden - Direct ps5-homebrew-ui comparison renderer.
// Copyright (C) 2026 Leo Lobato
// SPDX-License-Identifier: GPL-3.0-or-later

#include "homebrew/render.hpp"

#include "homebrew/browse.hpp"
#include "homebrew/focus_motion.hpp"
#include "homebrew/qr.hpp"
#include "homebrew/queue_layout.hpp"
#include "homebrew/simulation.hpp"
#include "homebrew/store_art.hpp"
#include <ctime>

#include "gfx/draw_list.hpp"
#include "gfx/font.hpp"
#include "gfx/gl.hpp"
#include "gfx/renderer.hpp"
#include "ui/components/grid.hpp"
#include "ui/components/keyboard.hpp"
#include "ui/components/list.hpp"
#include "ui/components/progress.hpp"
#include "ui/components/tabs.hpp"
#include "ui/fonts.hpp"
#include "ui/glyphs.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace playden::homebrew
{
namespace
{
namespace gfx = hui::gfx;
namespace ui = hui::ui;

constexpr float kWidth = 1920.0F;
constexpr float kHeight = 1080.0F;
constexpr float kSafeX = 96.0F;
constexpr float kFooterY = 986.0F;

const gfx::Color kBackground = gfx::Color::rgb(0x0E0D0C);
const gfx::Color kPanel = gfx::Color::rgb(0x16140F);
const gfx::Color kText = gfx::Color::rgb(0xF3EFE9);
const gfx::Color kSecondary = gfx::Color::rgb(0xA8A29E);
const gfx::Color kMuted = gfx::Color::rgb(0x6B655F);
const gfx::Color kAccent = gfx::Color::rgb(0xF0863A);
const gfx::Color kGreen = gfx::Color::rgb(0x7BBF6A);
const gfx::Color kAmber = gfx::Color::rgb(0xE2B53C);
const gfx::Color kRed = gfx::Color::rgb(0xE05A4F);
const gfx::Color kBlue = gfx::Color::rgb(0x7AA7D9);

ui::Theme playden_theme()
{
    ui::Theme theme{};
    theme.id = "playden";
    theme.name = "Playden";
    theme.family = "Warm console";
    theme.summary = "Warm near-black surfaces with restrained orange focus";
    theme.backdrop.mode = gfx::BackdropMode::gradient;
    theme.backdrop.colors[0] = kBackground;
    theme.backdrop.colors[1] = kBackground;
    theme.page = kBackground;
    theme.page_text = kText;
    theme.page_text_muted = kSecondary;
    theme.surface = kPanel;
    theme.surface_high = gfx::Color::rgb(0x2A2623);
    theme.text = kText;
    theme.text_muted = kSecondary;
    theme.primary = kAccent;
    theme.on_primary = gfx::Color::rgb(0x1A1210);
    theme.secondary = gfx::Color::rgb(0x24211E);
    theme.on_secondary = kText;
    theme.accent = kAccent;
    theme.outline = kText.with_alpha(0.20F);
    theme.focus = kAccent;
    theme.shadow = gfx::Color::rgb(0x000000, 0.55F);
    theme.light = kText.with_alpha(0.12F);
    theme.danger = kRed;
    theme.success = kGreen;
    theme.warning = kAmber;
    theme.style = ui::SurfaceStyle::flat;
    theme.corner = ui::Corner::round;
    theme.radius = 8.0F;
    theme.radius_card = 10.0F;
    theme.border = 2.0F;
    theme.button_border = 2.0F;
    theme.focus_width = 4.0F;
    theme.focus_gap = 5.0F;
    theme.heading = ui::FontRole::display;
    theme.label = ui::FontRole::semibold;
    theme.omega = 22.0F;
    theme.damping = 1.0F;
    theme.dark = true;
    return theme;
}

bool read_file(const std::filesystem::path &path, std::string *out)
{
    std::FILE *file = std::fopen(path.string().c_str(), "rb");
    if (file == nullptr)
        return false;

    const auto close_file = [&file]() { std::fclose(file); };
    if (std::fseek(file, 0, SEEK_END) != 0)
    {
        close_file();
        return false;
    }
    const long length = std::ftell(file);
    if (length < 0 || static_cast<unsigned long>(length) > std::numeric_limits<std::size_t>::max())
    {
        close_file();
        return false;
    }
    if (std::fseek(file, 0, SEEK_SET) != 0)
    {
        close_file();
        return false;
    }

    std::string bytes(static_cast<std::size_t>(length), '\0');
    const std::size_t read = bytes.empty() ? 0 : std::fread(bytes.data(), 1, bytes.size(), file);
    const bool ok = read == bytes.size() && std::ferror(file) == 0;
    close_file();
    if (!ok)
        return false;
    *out = std::move(bytes);
    return true;
}

std::filesystem::path first_existing(const std::vector<std::filesystem::path> &paths)
{
    for (const auto &path : paths)
    {
        std::error_code error;
        if (std::filesystem::is_regular_file(path, error))
            return path;
    }
    return {};
}

// ui::upper, and the accented Latin-1 letters too (a game's name in a sheet's label: "CAFÉ").
std::string upper_latin(std::string_view text)
{
    std::string result = ui::upper(text);
    for (std::size_t index = 0; index + 1 < result.size(); ++index)
    {
        const auto lead = static_cast<unsigned char>(result[index]);
        const auto next = static_cast<unsigned char>(result[index + 1]);
        // U+00E0-U+00FE, but not U+00F7 (division sign), are U+00C0-U+00DE plus 0x20.
        if (lead == 0xC3 && next >= 0xA0 && next <= 0xBE && next != 0xB7)
            result[index + 1] = static_cast<char>(next - 0x20);
    }
    return result;
}

std::string fit(const ui::FontRef &font, std::string_view text, float size, float width)
{
    return font.font->fit(text, size, width);
}

gfx::Color id_color(GameId id)
{
    static constexpr std::array<std::uint32_t, 8> colors = {
        0x634B3B, 0x354F52, 0x53405B, 0x5E4936, 0x334D3E, 0x4D435E, 0x675039, 0x3D4B63};
    const std::size_t number = id.store == Store::kSteam ? id.steam_app().value_or(0) : std::hash<GameId>{}(id);
    return gfx::Color::rgb(colors[number % colors.size()]);
}

const Game *game_by_id(const State &state, GameId id)
{
    const auto found = std::find_if(state.games.begin(), state.games.end(),
                                    [id](const Game &game) { return game.id == id; });
    return found == state.games.end() ? nullptr : &*found;
}

std::string byte_count(std::optional<std::uint64_t> bytes)
{
    if (!bytes)
        return "Unknown";
    // Decimal units, as the rest of Playden (game pages, sheets, storage) and macOS show sizes.
    constexpr double mib = 1000.0 * 1000.0;
    constexpr double gib = 1000.0 * mib;
    std::array<char, 32> value{};
    // Below a megabyte, kilobytes (at least 1): a small update isn't "0 MB". Nothing at all stays "0 MB".
    if (*bytes > 0 && *bytes < 999'500)
    {
        const int written = std::snprintf(value.data(), value.size(), "%.0f KB",
                                          std::max(1.0, static_cast<double>(*bytes) / 1000.0));
        return written > 0 ? std::string(value.data(), static_cast<std::size_t>(written)) : "Unknown";
    }
    // Below a gigabyte, megabytes: "0.2 GB" says little about a small game.
    if (*bytes < static_cast<std::uint64_t>(gib))
    {
        const int written = std::snprintf(value.data(), value.size(), "%.0f MB", static_cast<double>(*bytes) / mib);
        return written > 0 ? std::string(value.data(), static_cast<std::size_t>(written)) : "Unknown";
    }
    const int precision = *bytes >= static_cast<std::uint64_t>(10.0 * gib) ? 0 : 1;
    const int written = std::snprintf(value.data(), value.size(), "%.*f GB", precision,
                                      static_cast<double>(*bytes) / gib);
    return written > 0 ? std::string(value.data(), static_cast<std::size_t>(written)) : "Unknown";
}

// Sizes as macOS shows files (decimal units), matching the model's labels: megabytes below a gigabyte.
std::string short_size(std::uint64_t bytes)
{
    if (bytes == 0) return "0 MB";
    std::array<char, 32> value{};
    const double amount = static_cast<double>(bytes);
    const int written = amount >= 1e9 ? std::snprintf(value.data(), value.size(), "%.1f GB", amount / 1e9)
                                      : std::snprintf(value.data(), value.size(), "%.0f MB", std::max(1.0, amount / 1e6));
    return written > 0 ? std::string(value.data(), static_cast<std::size_t>(written)) : std::string("Unknown");
}

std::string join_words(const std::vector<std::string> &words)
{
    if (words.empty())
        return "Unknown";
    std::string result;
    for (const std::string &word : words)
    {
        if (!result.empty())
            result += " · ";
        result += word;
    }
    return result;
}

std::string playtime_label(std::optional<std::uint64_t> minutes)
{
    if (!minutes)
        return "Unknown";
    if (*minutes < 60)
        return std::to_string(*minutes) + " min";
    return std::to_string(*minutes / 60) + " h " + std::to_string(*minutes % 60) + " min";
}

std::string relative_time(std::optional<std::int64_t> timestamp, std::int64_t now)
{
    if (!timestamp)
        return "Unknown";
    const std::int64_t seconds = std::max<std::int64_t>(0, now - *timestamp);
    if (seconds < 3600)
        return "Less than an hour ago";
    if (seconds < 86400)
        return std::to_string(seconds / 3600) + " h ago";
    if (seconds < 30 * 86400)
        return std::to_string(seconds / 86400) + " d ago";
    // Further back a month and year reads better than a count of days ("Jan 2014").
    static constexpr const char *kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                              "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    // Civil date from days since 1970 (Howard Hinnant's algorithm): the PS5 libc has no gmtime_r.
    const std::int64_t days = (*timestamp >= 0 ? *timestamp : *timestamp - 86399) / 86400 + 719468;
    const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const std::int64_t day_of_era = days - era * 146097;
    const std::int64_t year_of_era = (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365;
    const std::int64_t day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    const std::int64_t shifted_month = (5 * day_of_year + 2) / 153;  // 0 is March
    const std::int64_t month = shifted_month < 10 ? shifted_month + 3 : shifted_month - 9;
    const std::int64_t year = year_of_era + era * 400 + (month <= 2 ? 1 : 0);
    return std::string(kMonths[month - 1]) + " " + std::to_string(year);
}

std::size_t utf8_boundary(std::string_view text, std::size_t offset)
{
    offset = std::min(offset, text.size());
    while (offset > 0 && offset < text.size() &&
           (static_cast<unsigned char>(text[offset]) & 0xC0U) == 0x80U)
        --offset;
    return offset;
}

const char *rating_label(Compatibility rating)
{
    switch (rating)
    {
    case Compatibility::Untested:
        return "Untested";
    case Compatibility::Works:
        return "Works · user rating";
    case Compatibility::Playable:
        return "Playable · user rating";
    case Compatibility::Broken:
        return "Broken · user rating";
    }
    return "Untested";
}

const char* job_stage_label(JobStage stage)
{
    switch (stage)
    {
    case JobStage::Download:
        return "Download";
    case JobStage::Verify:
        return "Verification";
    case JobStage::Install:
        return "Install";
    case JobStage::Uninstall:
        return "Removal";
    }
    return "Transfer";
}

ui::Button legend_button(std::string_view legend, std::size_t index)
{
    if (legend.rfind("Cross", 0) == 0)
        return ui::Button::cross;
    if (legend.rfind("Circle", 0) == 0)
        return ui::Button::circle;
    if (legend.rfind("Square", 0) == 0)
        return ui::Button::square;
    if (legend.rfind("Triangle", 0) == 0)
        return ui::Button::triangle;
    if (legend.rfind("Options", 0) == 0)
        return ui::Button::options;
    if (legend.rfind("Touchpad", 0) == 0)
        return ui::Button::touchpad;
    if (legend.find("Favorite") != std::string_view::npos)
        return ui::Button::square;
    if (legend.find("Backspace") != std::string_view::npos)
        return ui::Button::square;
    if (legend.find("Sort") != std::string_view::npos || legend.find("Filter") != std::string_view::npos)
        return ui::Button::triangle;
    if (legend.find("More") != std::string_view::npos || legend.find("Action") != std::string_view::npos)
        return ui::Button::options;
    if (legend.find("Back") != std::string_view::npos || legend.find("Cancel") != std::string_view::npos)
        return ui::Button::circle;
    if (legend.find("Previous") != std::string_view::npos)
        return ui::Button::l1;
    if (legend.find("Next") != std::string_view::npos)
        return ui::Button::r1;
    return index == 0 ? ui::Button::cross : ui::Button::circle;
}

// The glyph already names the button, so "Cross Open" reads "Open" beside it.
const char *legend_text(const std::string &legend)
{
    for (const std::string_view button : {"Cross ", "Circle ", "Square ", "Triangle ", "Options ", "Touchpad "})
    {
        if (legend.rfind(button, 0) == 0)
            return legend.c_str() + button.size();
    }
    return legend.c_str();
}

ui::Status progress_status(const ViewRow &row)
{
    if (row.dangerous || row.badge.find("Failed") != std::string::npos)
        return ui::Status::danger;
    if (row.badge.find("Paused") != std::string::npos || row.badge.find("Offline") != std::string::npos)
        return ui::Status::warning;
    if (row.progress >= 1.0F)
        return ui::Status::success;
    return ui::Status::accent;
}

// How the overlay layer (sheets, dialogs, the keyboard) moves between frames. The scrim and
// a sheet's panel animate in only when an overlay opens over the page; going from one overlay
// to another (a sheet to its picker, a dialog back to its sheet) keeps the scrim and the panel
// where they are and only fades the new content in, so the page behind never flashes.
class OverlayTransition
{
public:
    void update(const std::string &key, bool reduced_motion)
    {
        if (key != key_)
        {
            const bool swap = !key_.empty() && !key.empty();
            key_ = key;
            if (key.empty() || reduced_motion)
                enter_ = content_ = 1.0F;
            else if (swap)
            {
                content_ = 0.0F;
                from_width_ = panel_width_;
            }
            else
                enter_ = content_ = 0.0F;
            return;
        }
        enter_ = reduced_motion ? 1.0F : std::min(1.0F, enter_ + kStep / 0.22F);
        content_ = reduced_motion ? 1.0F : std::min(1.0F, content_ + kStep / 0.16F);
    }
    // Scrim and panel entry, eased: 0 closed, 1 fully in.
    float enter() const { return 1.0F - std::pow(1.0F - enter_, 3.0F); }
    // The current overlay's content: fades in after a swap.
    float content() const { return 1.0F - std::pow(1.0F - content_, 3.0F); }
    // A sheet panel's width, easing from the previous sheet's width after a swap.
    float panel_width(float target)
    {
        if (from_width_ <= 0.0F) from_width_ = target;
        panel_width_ = from_width_ + (target - from_width_) * content();
        if (content_ >= 1.0F) from_width_ = target;
        return panel_width_;
    }

private:
    static constexpr float kStep = 1.0F / 60.0F;
    std::string key_;
    float enter_ = 1.0F;
    float content_ = 1.0F;
    float from_width_ = 0.0F;
    float panel_width_ = 0.0F;
};

} // namespace

class Render::Impl
{
  public:
    explicit Impl(std::filesystem::path assets_directory, ArtworkService* artwork)
        : assets_(std::move(assets_directory)), theme_(playden_theme()), texture_backend_(renderer_)
    {
        initialise(artwork == nullptr);
        if (artwork) live_artwork_ = std::make_unique<LiveArtworkCache>(*artwork, texture_backend_);
    }

    ~Impl()
    {
        live_artwork_.reset();
        for (const std::uint32_t texture : art_)
        {
            if (texture != 0)
                glDeleteTextures(1, &texture);
        }
        if (hero_ != 0)
            glDeleteTextures(1, &hero_);
        if (capture_color_ != 0)
            glDeleteRenderbuffers(1, &capture_color_);
        if (capture_framebuffer_ != 0)
            glDeleteFramebuffers(1, &capture_framebuffer_);
    }

    void draw(Simulation &simulation, int width, int height, float dt)
    {
        if (!ready_ || width <= 0 || height <= 0)
            return;

        scene_.clear();
        overlay_.clear();
        stats_.grid_focused_game_id = 0;
        grid_focused_game_ = {};
        stats_.grid_drawn_cell_count = 0;
        stats_.focused_control_key.clear();
        stats_.native_width = width;
        stats_.native_height = height;
        const State &state = simulation.state();
        if (live_artwork_) live_artwork_->BeginFrame(state.simulation_mode ? 0 : state.account_generation);
        update_qr_cache(state);
        focus_motion_.begin_frame(dt, state.reduced_motion);
        frame_dt_ = std::isfinite(dt) ? std::clamp(dt, 0.0F, 0.1F) : 0.0F;
        const ViewData &view = simulation.view();
        const ViewData &route_view = simulation.route_view();
        const std::string overlay_key = state.editor.active ? "keyboard" :
            state.navigation.modals.empty() ? std::string{} : state.navigation.modals.back().id;
        overlay_transition_.update(overlay_key, state.reduced_motion);
        route_focus_ = !state.editor.active && state.navigation.modals.empty() && !state.navigation.tabs_focused;
        current_state_ = &state;
        if (state.simulation_mode) clock_ = static_cast<float>(state.clock_ticks) / 60.0F;
        else {
            live_animation_clock_ = std::fmod(live_animation_clock_ +
                std::clamp(std::isfinite(dt) ? dt : 0.0F, 0.0F, 0.25F), 3600.0F);
            clock_ = live_animation_clock_;
        }
        ui::Canvas scene{scene_, fonts_, renderer_.glass_texture(), clock_};

        draw_background();
        const bool root = state.navigation.route == Route::Home || state.navigation.route == Route::Library ||
                          state.navigation.route == Route::Downloads || state.navigation.route == Route::Saves ||
                          state.navigation.route == Route::Settings;
        if (root)
            draw_top_navigation(scene, state.navigation.route, state.reduced_motion);

        switch (state.navigation.route)
        {
        case Route::Startup:
            draw_startup(scene, route_view, state);
            break;
        case Route::SignIn:
            draw_sign_in(scene, route_view, state);
            break;
        case Route::Home:
            draw_home(scene, simulation);
            break;
        case Route::Library:
            draw_library(scene, simulation);
            break;
        case Route::Downloads:
            draw_queue(scene, route_view, state);
            break;
        case Route::Settings:
            draw_settings(scene, route_view, state);
            break;
        case Route::Saves:
            draw_saves(route_view, state);
            break;
        case Route::SaveDetails:
            draw_save_details(route_view, state);
            break;
        case Route::ManageGames:
            draw_manage_games(route_view, state);
            break;
        case Route::Details:
            draw_details(scene, route_view, state);
            break;
        case Route::Session:
            draw_session(scene, route_view, state);
            break;
        }

        draw_persistent_status(scene, state);
        draw_footer(scene, view);
        draw_overlays(state, view);
        focus_motion_.end_frame();
        if (live_artwork_) {
            live_artwork_->EndFrame();
            const auto artwork_stats = live_artwork_->Stats();
            stats_.live_artwork_texture_count = artwork_stats.texture_count;
            stats_.live_artwork_gpu_bytes = artwork_stats.gpu_bytes;
        }

        renderer_.begin();
        gfx::BackdropSpec backdrop = theme_.backdrop;
        backdrop.time = clock_;
        renderer_.backdrop(backdrop);
        renderer_.draw(scene_);
        if (!overlay_.empty())
        {
            renderer_.glass();
            renderer_.draw(overlay_);
        }
        renderer_.present(0, width, height);
        last_width_ = width;
        last_height_ = height;
        has_frame_ = true;
    }

    [[nodiscard]] RenderStats diagnostics() const noexcept
    {
        return stats_;
    }

    bool capture_png(const std::filesystem::path &path, int width, int height) const
    {
        if (!ready_ || !has_frame_ || width <= 0 || height <= 0 || !ensure_capture(width, height))
            return false;

        glBindFramebuffer(GL_FRAMEBUFFER, capture_framebuffer_);
        glViewport(0, 0, width, height);
        glClearColor(kBackground.r, kBackground.g, kBackground.b, 1.0F);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        renderer_.present(capture_framebuffer_, width, height);
        std::vector<unsigned char> pixels(static_cast<std::size_t>(width) * height * 4);
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        for (std::size_t index = 3; index < pixels.size(); index += 4)
            pixels[index] = 255;
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        stbi_flip_vertically_on_write(1);
        return stbi_write_png(path.string().c_str(), width, height, 4, pixels.data(), width * 4) != 0;
    }

  private:
    bool initialise(bool fixture_artwork)
    {
        if (!renderer_.init())
            return false;
        if (!load_fonts())
            return false;
        if (fixture_artwork) load_art();
        stats_.artwork_texture_count = static_cast<std::size_t>(std::count_if(
            art_.begin(), art_.end(), [](std::uint32_t texture) { return texture != 0; })) +
            (hero_ == 0 ? 0U : 1U);
        configure_components();
        ready_ = true;
        return true;
    }

    std::filesystem::path asset(std::string_view relative) const
    {
        const std::filesystem::path direct = assets_ / relative;
        const std::filesystem::path nested = assets_ / "assets" / relative;
        const std::filesystem::path comparison = assets_ / "ui-comparison" / "assets" / relative;
        const std::filesystem::path fixture = assets_ / "fixtures" / "ui-comparison" / "assets" / relative;
        return first_existing({direct, nested, comparison, fixture});
    }

    bool load_face(gfx::Font *face, ui::FontRef *ref,
                   const std::vector<std::string> &names)
    {
        std::vector<std::filesystem::path> candidates;
        for (const std::string &name : names)
        {
            candidates.push_back(asset("fonts/" + name));
            candidates.push_back(assets_ / "fonts" / name);
        }
        const auto path = first_existing(candidates);
        std::string bytes;
        if (path.empty() || !read_file(path, &bytes) || !face->load(bytes))
            return false;
        ref->font = face;
        ref->texture = renderer_.batch().create_font_texture(*face);
        return ref->texture != 0;
    }

    bool load_fonts()
    {
        const bool regular = load_face(&regular_, &fonts_.regular,
                                       {"barlow-regular.huifont", "inter-regular.huifont"});
        const bool semibold = load_face(&semibold_, &fonts_.semibold,
                                        {"barlow-semibold.huifont", "barlow-medium.huifont",
                                         "inter-semibold.huifont"});
        const bool display = load_face(&display_, &fonts_.display,
                                       {"barlow-condensed-semibold.huifont",
                                        "barlow-condensed-medium.huifont", "montserrat-medium.huifont"});
        if (!regular || !semibold || !display)
            return false;
        fonts_.mono = fonts_.regular;
        fonts_.pixel = fonts_.semibold;
        fonts_.hand = fonts_.regular;
        if (!load_face(&greek_face_, &greek_, {"noto-greek-fallback.huifont"}))
            return false;
        load_face(&cjk_face_, &cjk_, {"noto-cjk-fallback.huifont"});
        load_face(&arabic_face_, &arabic_, {"noto-arabic-fallback.huifont"});
        return true;
    }

    const ui::FontRef &font_for(std::string_view value, const ui::FontRef &preferred) const
    {
        std::size_t index = 0;
        while (index < value.size())
        {
            const std::uint32_t codepoint = gfx::next_codepoint(value, &index);
            const bool greek = (codepoint >= 0x0370 && codepoint <= 0x03FF) ||
                               (codepoint >= 0x1F00 && codepoint <= 0x1FFF);
            if (greek && greek_.font != nullptr && greek_.font->has_glyph(codepoint))
                return greek_;
            const bool arabic = codepoint >= 0x0600 && codepoint <= 0x06FF;
            if (arabic && arabic_.font != nullptr)
                return arabic_;
            if (!preferred.font->has_glyph(codepoint) && cjk_.font != nullptr &&
                cjk_.font->has_glyph(codepoint))
                return cjk_;
        }
        return preferred;
    }

    void dynamic_text(std::string_view value, float x, float y, float size, gfx::Color color,
                      float max_width, bool display = true,
                      gfx::Align align = gfx::Align::left)
    {
        const ui::FontRef &preferred = display ? fonts_.display : fonts_.regular;
        const ui::FontRef &face = font_for(value, preferred);
        ui::text(scene_, face, face.font->fit(value, size, max_width), x, y, size, color, align);
    }

    void dynamic_paragraph(std::string_view value, float x, float y, float size, float width,
                           float line_height, gfx::Color color, int lines,
                           gfx::Align align = gfx::Align::left)
    {
        const ui::FontRef &face = font_for(value, fonts_.regular);
        ui::paragraph(scene_, face, value, x, y, size, width, line_height, color, lines, align);
    }

    std::uint32_t load_texture(const std::filesystem::path &path)
    {
        if (path.empty())
            return 0;
        int width = 0;
        int height = 0;
        int channels = 0;
        stbi_uc *pixels = stbi_load(path.string().c_str(), &width, &height, &channels, 4);
        if (pixels == nullptr)
            return 0;
        const std::uint32_t texture = renderer_.batch().create_texture(width, height, pixels);
        stbi_image_free(pixels);
        return texture;
    }

    void load_art()
    {
        for (std::size_t index = 0; index < art_.size(); ++index)
            art_[index] = load_texture(asset("art/poster" + std::to_string(index + 1) + ".png"));
        hero_ = load_texture(asset("art/hero.png"));
    }

    void configure_components()
    {
        tabs_.style.theme = theme_;
        tabs_.style.theme.primary = kAccent;
        tabs_.style.theme.on_primary = theme_.on_primary;
        tabs_.style.theme.label = ui::FontRole::display;
        tabs_.style.kind = ui::TabKind::boxed;
        tabs_.style.height = 56.0F;
        tabs_.style.gap = 14.0F;
        tabs_.style.padding = 20.0F;
        tabs_.style.text_size = 29.0F;
        tabs_.style.track = false;
        tabs_.style.focus_ring = false;
        tabs_.style.on_page = true;
        tabs_.set_tabs({{"Home"}, {"Library"}, {"Queue", 1}, {"Settings"}});
        tabs_.set_bounds({96.0F, 54.0F, 760.0F, 56.0F});

        grid_.style.theme = theme_;
        grid_.style.columns = 6;
        grid_.style.cell_width = 210.0F;
        grid_.style.cell_height = 315.0F;
        grid_.style.gap_x = 24.0F;
        grid_.style.gap_y = 24.0F;
        grid_.style.padding = 27.0F;
        grid_.style.scroll_thumb = false;
        grid_.style.edge_fade = 0.0F;
        grid_.style.entrance_step = 0.0F;
        grid_.style.card.text = ui::CardText::none;
        // Custom poster content owns the per-key spring so fast retargets retain
        // momentum instead of snapping the grid highlight on each model draw.
        grid_.style.card.focus_scale = 1.0F;
        grid_.style.card.ring = false;
        grid_.style.card.glow = false;
        grid_.style.card.lift = 0.0F;
        grid_.style.card.radius = 8.0F;
        // The third row continues beneath the y=986 legend, matching the pinned
        // reference rather than reserving a separate blank footer strip.
        grid_.set_bounds({417.0F, 123.0F, 1434.0F, 903.0F});

        list_.style.theme = theme_;
        list_.style.panel = false;
        list_.style.cards = true;
        list_.style.row_height = 90.0F;
        list_.style.header_height = 48.0F;
        list_.style.gap = 10.0F;
        list_.style.title_size = 28.0F;
        list_.style.subtitle_size = 20.0F;
        list_.style.value_size = 23.0F;
        list_.style.entrance_step = 0.0F;
        list_.style.highlight.kind = ui::HighlightKind::ring;

        keyboard_.style.theme = theme_;
        keyboard_.style.reduced_motion = false;
        keyboard_.style.key_width = 76.0F;
        keyboard_.style.key_height = 66.0F;
        keyboard_.style.gap = 10.0F;
        keyboard_.style.label_size = 28.0F;
        keyboard_.style.panel = true;
        keyboard_.set_bounds({500.0F, 466.0F, 920.0F, 440.0F});
    }

    void draw_background()
    {
        scene_.rounded_rect({0.0F, 0.0F, kWidth, kHeight}, 0.0F, kBackground);
    }

    // Tab icons drawn from primitives, after the SF Symbols the macOS tabs use.
    void tab_icon(int tab, float x, float cy, gfx::Color ink)
    {
        constexpr float w = 1.8F;
        switch (tab)
        {
        case 0: // house
            scene_.line(x + 1.0F, cy - 1.0F, x + 13.0F, cy - 11.0F, w, ink);
            scene_.line(x + 13.0F, cy - 11.0F, x + 25.0F, cy - 1.0F, w, ink);
            scene_.line(x + 5.0F, cy - 4.0F, x + 5.0F, cy + 11.0F, w, ink);
            scene_.line(x + 21.0F, cy - 4.0F, x + 21.0F, cy + 11.0F, w, ink);
            scene_.line(x + 5.0F, cy + 11.0F, x + 21.0F, cy + 11.0F, w, ink);
            scene_.line(x + 10.5F, cy + 11.0F, x + 10.5F, cy + 4.0F, w, ink);
            scene_.line(x + 15.5F, cy + 11.0F, x + 15.5F, cy + 4.0F, w, ink);
            scene_.line(x + 10.5F, cy + 4.0F, x + 15.5F, cy + 4.0F, w, ink);
            break;
        case 1: // grid
            for (int index = 0; index < 4; ++index)
                scene_.bordered_rect({x + 2.0F + (index % 2) * 13.0F, cy - 11.0F + (index / 2) * 13.0F, 10.0F, 10.0F},
                                     2.5F, ink.with_alpha(0.0F), w, ink);
            break;
        case 2: // arrow down to a line
            scene_.line(x + 13.0F, cy - 11.0F, x + 13.0F, cy + 5.0F, w, ink);
            scene_.line(x + 6.0F, cy - 2.0F, x + 13.0F, cy + 5.0F, w, ink);
            scene_.line(x + 20.0F, cy - 2.0F, x + 13.0F, cy + 5.0F, w, ink);
            scene_.line(x + 3.0F, cy + 11.0F, x + 23.0F, cy + 11.0F, w, ink);
            break;
        case 3: // cloud
            scene_.line(x + 4.0F, cy + 9.0F, x + 22.0F, cy + 9.0F, w, ink);
            scene_.ring(x + 6.5F, cy + 3.0F, 6.0F, w, ink);
            scene_.ring(x + 13.0F, cy - 3.0F, 8.0F, w, ink);
            scene_.ring(x + 20.0F, cy + 3.0F, 6.0F, w, ink);
            scene_.rounded_rect({x + 3.0F, cy + 3.5F, 20.0F, 4.5F}, 0.0F, kBackground);
            scene_.line(x + 4.0F, cy + 9.0F, x + 22.0F, cy + 9.0F, w, ink);
            break;
        default: // gear
            for (int tooth = 0; tooth < 8; ++tooth)
            {
                const float angle = static_cast<float>(tooth) * 0.785398F;
                scene_.line(x + 13.0F + std::sin(angle) * 8.0F, cy - std::cos(angle) * 8.0F,
                            x + 13.0F + std::sin(angle) * 12.0F, cy - std::cos(angle) * 12.0F, 3.4F, ink);
            }
            scene_.ring(x + 13.0F, cy, 8.5F, w, ink);
            scene_.ring(x + 13.0F, cy, 3.5F, w, ink);
            break;
        }
    }

    // The macOS top bar: icon and label per tab; the current one sits in a quiet box.
    void draw_top_navigation(ui::Canvas &, Route route, bool)
    {
        int selected = 0;
        if (route == Route::Library)
            selected = 1;
        else if (route == Route::Downloads)
            selected = 2;
        else if (route == Route::Saves)
            selected = 3;
        else if (route == Route::Settings)
            selected = 4;
        std::size_t pending = 0;
        if (current_state_->simulation_mode)
            pending = 1;
        else
            pending = QueuePending(*current_state_);
        // Saves: the games that need the player, or a spinner while a check or Sync all runs.
        std::size_t needs_you = 0;
        for (const auto &[id, cloud] : current_state_->cloud)
            if (!cloud.queued && (cloud.status == CloudStatus::Conflict || cloud.status == CloudStatus::NeedsAccount ||
                                  cloud.status == CloudStatus::Failed))
                ++needs_you;
        const bool saves_busy = current_state_->saves_job.kind != SavesJobKind::None;
        static constexpr std::array<const char *, 5> names = {"Home", "Library", "Queue", "Saves", "Settings"};
        float x = kSafeX;
        constexpr float cy = 82.0F;
        for (int tab = 0; tab < 5; ++tab)
        {
            const bool active = tab == selected;
            const float label = fonts_.display.measure(names[static_cast<std::size_t>(tab)], 30.0F);
            const std::string count = tab == 2 && pending > 0 ? std::to_string(pending)
                                    : tab == 3 && !saves_busy && needs_you > 0 ? std::to_string(needs_you) : std::string{};
            const float badge = tab == 3 && saves_busy ? 34.0F
                              : count.empty() ? 0.0F : fonts_.semibold.measure(count, 16.0F) + 26.0F;
            const float width = 22.0F + 26.0F + 10.0F + label + badge + 22.0F;
            const gfx::Rect box{x, cy - 26.0F, width, 52.0F};
            // With the bar focused (Up from the top of a page) the focused tab carries the focus ring.
            const bool tab_focused = current_state_->navigation.tabs_focused &&
                                     current_state_->navigation.modals.empty() && current_state_->navigation.tab_focus == tab;
            const float amount = control_focus("tab-" + std::to_string(tab), tab_focused);
            focus_glow(scene_, box, amount);
            if (active)
                scene_.bordered_rect(box, 8.0F, kText.with_alpha(0.12F), 2.0F, kText.with_alpha(0.35F));
            focus_ring(scene_, box, amount);
            if (tab_focused)
                stats_.focused_control_key = "tab-" + std::to_string(tab);
            const gfx::Color ink = active ? kText : kSecondary;
            tab_icon(tab, x + 22.0F, cy, ink);
            ui::text(scene_, fonts_.display, names[static_cast<std::size_t>(tab)], x + 58.0F, cy + 11.0F, 30.0F, ink);
            if (tab == 3 && saves_busy)
                spinner(x + 58.0F + label + 22.0F, cy, 10.0F, kBlue);
            if (!count.empty())
            {
                const float badge_x = x + 58.0F + label + 10.0F;
                scene_.rounded_rect({badge_x, cy - 14.0F, badge - 10.0F, 28.0F}, 14.0F, kAccent);
                ui::text(scene_, fonts_.semibold, count, badge_x + (badge - 10.0F) * 0.5F, cy + 6.0F, 16.0F,
                         kBackground, gfx::Align::center);
            }
            x += width + 8.0F;
        }

        std::string clock_label = "21:42";
        if (!current_state_->simulation_mode) {
            const std::time_t wall_time = static_cast<std::time_t>(current_state_->clock_ticks);
            char formatted[6]{};
            const auto* local = std::localtime(&wall_time);
            clock_label = local != nullptr && std::strftime(formatted, sizeof(formatted), "%H:%M", local)
                ? formatted : "--:--";
        }
        ui::text(scene_, fonts_.regular, clock_label, 1824.0F, 91.0F, 22.0F, kSecondary,
                 gfx::Align::right);
    }

    void section_label(std::string_view value, float x, float y)
    {
        ui::text(scene_, fonts_.display, ui::upper(value), x, y, 22.0F, kSecondary,
                 gfx::Align::left, 2.6F);
    }

    void draw_title(const ViewData &view, float x = kSafeX, float y = 212.0F, float width = 760.0F)
    {
        ui::text(scene_, fonts_.display, fit(fonts_.display, view.title, 68.0F, width), x, y, 68.0F,
                 kText);
        if (!view.subtitle.empty())
            ui::paragraph(scene_, fonts_.regular, view.subtitle, x, y + 58.0F, 27.0F, width, 37.0F,
                          kSecondary, 4);
    }

    // The macOS focus treatment: an accent glow spilling out of the control (drawn before it,
    // so the control covers the glow's middle) and a ring just outside it (drawn after).
    void focus_glow(gfx::DrawList &list, const gfx::Rect &rect, float amount, bool compact = false)
    {
        if (amount <= 0.0F)
            return;
        list.glow(rect.inset(compact ? -5.0F : -7.0F), compact ? 12.0F : 13.0F, 22.0F,
                  kAccent.with_alpha(0.5F * amount));
    }

    void focus_ring(gfx::DrawList &list, const gfx::Rect &rect, float amount, bool compact = false)
    {
        if (amount <= 0.0F)
            return;
        list.bordered_rect(rect.inset(compact ? -6.5F : -9.0F), compact ? 13.5F : 15.0F,
                           kBackground.with_alpha(0.0F), compact ? 3.0F : 4.0F, kAccent.with_alpha(amount));
    }

    // Focus for a pill (a filter chip): the glow and ring follow its round ends.
    void pill_focus_glow(const gfx::Rect &rect, float amount)
    {
        if (amount > 0.0F)
            scene_.glow(rect.inset(-5.0F), rect.h * 0.5F + 5.0F, 22.0F, kAccent.with_alpha(0.5F * amount));
    }
    void pill_focus_ring(const gfx::Rect &rect, float amount)
    {
        if (amount > 0.0F)
            scene_.bordered_rect(rect.inset(-6.5F), rect.h * 0.5F + 6.5F, kBackground.with_alpha(0.0F), 3.0F,
                                 kAccent.with_alpha(amount));
    }

    // Two arrows chasing round a circle: sync.
    void sync_icon(float cx, float cy, float radius, float width, gfx::Color ink)
    {
        constexpr int kSteps = 10;
        for (const float start : {0.35F, 3.49F})  // two arcs of 140°, leaving gaps at the ends
        {
            const float sweep = 2.44F;
            float px = cx + std::cos(start) * radius, py = cy + std::sin(start) * radius;
            for (int step = 1; step <= kSteps; ++step)
            {
                const float angle = start + sweep * static_cast<float>(step) / kSteps;
                const float x = cx + std::cos(angle) * radius, y = cy + std::sin(angle) * radius;
                scene_.line(px, py, x, y, width, ink);
                px = x;
                py = y;
            }
            // The arrowhead at the arc's end, pointing along it.
            const float end = start + sweep;
            const float tx = -std::sin(end), ty = std::cos(end);  // direction of travel
            const float nx = std::cos(end), ny = std::sin(end);   // outward
            const float head = radius * 0.55F;
            scene_.line(px, py, px - tx * head + nx * head * 0.7F, py - ty * head + ny * head * 0.7F, width, ink);
            scene_.line(px, py, px - tx * head - nx * head * 0.7F, py - ty * head - ny * head * 0.7F, width, ink);
        }
    }

    // An arrow rising from a tray: upload.
    void upload_icon(float cx, float cy, float size, float width, gfx::Color ink)
    {
        const float half = size * 0.5F;
        scene_.line(cx, cy + half * 0.4F, cx, cy - half, width, ink);
        scene_.line(cx, cy - half, cx - half * 0.6F, cy - half * 0.4F, width, ink);
        scene_.line(cx, cy - half, cx + half * 0.6F, cy - half * 0.4F, width, ink);
        scene_.line(cx - half, cy + half * 0.3F, cx - half, cy + half, width, ink);
        scene_.line(cx - half, cy + half, cx + half, cy + half, width, ink);
        scene_.line(cx + half, cy + half, cx + half, cy + half * 0.3F, width, ink);
    }

    // The macOS `ActionButton`: primary is filled with the accent, the others are outlined;
    // focus adds the ring and glow and lifts the button slightly.
    void mac_button(gfx::DrawList &list, const gfx::Rect &rect, std::string_view label, bool primary,
                    float amount, bool disabled = false, std::string_view detail = {}, bool large = false,
                    gfx::Color backdrop = kBackground)
    {
        list.push_transform(1.0F + 0.04F * amount, rect.cx(), rect.cy(), 0.0F, 0.0F);
        if (disabled)
            list.push_opacity(0.45F);
        focus_glow(list, rect, amount);
        if (primary)
            list.rounded_rect(rect, 8.0F, kAccent);
        else
            list.bordered_rect(rect, 8.0F, amount > 0.0F ? backdrop : backdrop.with_alpha(0.0F), 2.0F,
                               kText.with_alpha(0.2F));
        const float size = large ? 38.0F : 26.0F;
        const gfx::Color ink = primary ? theme_.on_primary : kText;
        const float detail_width = detail.empty() ? 0.0F : fonts_.semibold.measure(detail, 22.0F) + 18.0F;
        const float label_width = fonts_.display.measure(label, size);
        const float content = std::min(rect.w - 32.0F, label_width + detail_width);
        const float x = rect.cx() - content * 0.5F;
        const float baseline = rect.cy() + size * 0.36F;
        ui::text(list, fonts_.display, fit(fonts_.display, label, size, rect.w - 32.0F - detail_width), x,
                 baseline, size, ink);
        if (!detail.empty())
            ui::text(list, fonts_.semibold, detail, x + content, rect.cy() + 8.0F, 22.0F, ink.with_alpha(0.8F),
                     gfx::Align::right);
        focus_ring(list, rect, amount);
        if (disabled)
            list.pop_opacity();
        list.pop_transform();
    }

    float control_focus(std::string_view key, bool focused)
    {
        return focus_motion_.amount(std::string("control-") + std::string(key), focused);
    }

    void draw_action(const ViewRow &row, const gfx::Rect &rect, bool focused, bool primary = false)
    {
        if (focused && route_focus_)
            stats_.focused_control_key = row.key;
        const float amount = control_focus(row.key, focused && route_focus_);
        const auto title = row.kind == RowKind::Toggle ? row.title + (row.selected ? " · On" : " · Off") : row.title;
        mac_button(scene_, rect, title, primary && row.enabled, amount, !row.enabled, row.value, primary && rect.h >= 80.0F);
    }

    // Returns the y just below the last action drawn.
    float draw_actions(const ViewData &view, float x, float y, float width, bool large_first = true)
    {
        std::vector<const ViewRow *> actions;
        if (!view.actions.empty())
        {
            for (const ViewRow &row : view.actions)
                actions.push_back(&row);
        }
        else
        {
            for (const ViewRow &row : view.rows)
            {
                if (row.kind == RowKind::Action || row.kind == RowKind::Toggle)
                    actions.push_back(&row);
            }
        }
        const int focused = view.actions.empty() ? view.focused_row : view.focused_action;
        for (std::size_t index = 0; index < actions.size(); ++index)
        {
            const bool details = current_state_->navigation.route == Route::Details;
            const float height = large_first && index == 0 ? (details ? 72.0F : 84.0F) : (details ? 54.0F : 64.0F);
            const bool is_focused = view.actions.empty() ?
                (focused >= 0 && static_cast<std::size_t>(focused) < view.rows.size() &&
                 view.rows[static_cast<std::size_t>(focused)].key == actions[index]->key) :
                static_cast<int>(index) == focused;
            draw_action(*actions[index], {x, y, width, height},
                        is_focused, index == 0);
            y += height + (details ? 12.0F : 16.0F);
        }
        return y;
    }

    void draw_startup(ui::Canvas &, const ViewData &view, const State &state)
    {
        section_label("Playden", 96.0F, 78.0F);
        if (state.root_access_missing)
        {
            // No artwork or account here: the problem, the startup error and the way out.
            draw_title(view, 96.0F, 292.0F, 1180.0F);
            float y = 470.0F;
            for (const ViewRow &row : view.rows)
                if (row.kind == RowKind::Status)
                {
                    // The whole error: it is what the player reports, so it is never cut short.
                    y = ui::paragraph(scene_, fonts_.regular, row.title, 96.0F, y, 22.0F, 1180.0F, 30.0F, kAmber, 12) + 32.0F;
                }
            draw_actions(view, 96.0F, y, 560.0F);
            return;
        }
        draw_title(view, 96.0F, 292.0F, 720.0F);
        if (state.startup_loading)
        {
            const gfx::Rect loading{96.0F, 520.0F, 560.0F, 112.0F};
            scene_.rounded_rect(loading, 10.0F, kPanel);
            scene_.circle(130.0F, 558.0F, 7.0F, kAccent);
            ui::text(scene_, fonts_.semibold,
                     state.simulation_mode ? "Loading saved fixture state…" :
                                             "Restoring your accounts…",
                     154.0F, 566.0F,
                     24.0F, kText);
            scene_.rounded_rect({128.0F, 594.0F, 496.0F, 6.0F}, 3.0F,
                                kText.with_alpha(0.14F));
            const float pulse = 0.18F + 0.22F * (0.5F + 0.5F * std::sin(clock_ * 5.0F));
            scene_.rounded_rect({128.0F, 594.0F, 496.0F * pulse, 6.0F}, 3.0F, kAccent);
        }
        else
            draw_actions(view, 96.0F, 520.0F, 560.0F);

        scene_.rounded_rect({988.0F, 194.0F, 724.0F, 680.0F}, 14.0F, kPanel);
        scene_.glow({1050.0F, 268.0F, 600.0F, 360.0F}, 12.0F, 42.0F, kAccent.with_alpha(0.08F));
        if (hero_ != 0)
            scene_.image_gradient(hero_, {1050.0F, 268.0F, 600.0F, 360.0F}, gfx::kFullUv,
                                  kText.with_alpha(0.75F), kBackground.with_alpha(0.90F), 12.0F);
        else
            scene_.gradient_rect({1050.0F, 268.0F, 600.0F, 360.0F}, 12.0F,
                                 kAccent.with_alpha(0.25F), kPanel);
        ui::text(scene_, fonts_.display, "YOUR GAMES", 1100.0F, 704.0F, 48.0F, kText);
        ui::text(scene_, fonts_.regular,
                 state.startup_loading ?
                     (state.simulation_mode ? "Restoring local fixture…" : "Connecting securely…") :
                 state.persistence_available ? "Ready when you are." : "Browsing can continue without saving.",
                 1100.0F, 752.0F, 25.0F,
                 state.persistence_available || state.startup_loading ? kSecondary : kAmber);
    }

    void clear_qr_cache()
    {
        std::fill(cached_qr_url_.begin(), cached_qr_url_.end(), '\0');
        std::string{}.swap(cached_qr_url_);
        cached_qr_ = {};
        cached_qr_error_.clear();
    }

    void update_qr_cache(const State &state)
    {
        const std::string link = state.navigation.modals.empty() ? std::string() : state.navigation.modals.back().link;
        if (link != link_qr_url_)
        {
            link_qr_url_ = link;
            link_qr_ = link.empty() ? QrMatrix{} : encode_qr(link);
        }
        const bool usable = state.account().authentication == AuthState::Waiting &&
                            !state.account().challenge_url.empty();
        if (!usable) {
            clear_qr_cache();
            return;
        }
        if (cached_qr_url_ == state.account().challenge_url)
            return;
        clear_qr_cache();
        cached_qr_url_ = state.account().challenge_url;
        cached_qr_ = encode_qr(cached_qr_url_, &cached_qr_error_);
    }

    void draw_simulated_qr(float x, float y, float side)
    {
        scene_.rounded_rect({x, y, side, side}, 16.0F, kText);
        constexpr int cells = 21;
        const float cell = (side - 48.0F) / static_cast<float>(cells);
        for (int row = 0; row < cells; ++row)
        {
            for (int column = 0; column < cells; ++column)
            {
                const bool finder = ((row < 7 || row >= cells - 7) && column < 7) ||
                                    (row < 7 && column >= cells - 7);
                const bool bit = finder ? (row % 6 != 1 && column % 6 != 1) :
                                          ((row * 17 + column * 31 + row * column) % 7 < 3);
                if (bit)
                    scene_.rounded_rect({x + 24.0F + column * cell, y + 24.0F + row * cell,
                                         std::ceil(cell), std::ceil(cell)},
                                        0.0F, kBackground);
            }
        }
    }

    bool draw_encoded_qr(float x, float y, int available_side)
    {
        return draw_qr_matrix(scene_, cached_qr_, x, y, available_side);
    }

    bool draw_qr_matrix(gfx::DrawList &list, const QrMatrix &matrix, float x, float y, int available_side)
    {
        const QrLayout layout = qr_layout(matrix, available_side);
        if (layout.module_pixels <= 0)
            return false;
        const float x0 = x + static_cast<float>(layout.offset_pixels);
        const float y0 = y + static_cast<float>(layout.offset_pixels);
        list.rounded_rect({x0, y0, static_cast<float>(layout.side_pixels),
                             static_cast<float>(layout.side_pixels)},
                            0.0F, kText);
        for (int row = 0; row < matrix.size(); ++row) {
            for (int column = 0; column < matrix.size();) {
                if (!matrix.at(column, row)) {
                    ++column;
                    continue;
                }
                int end = column + 1;
                while (end < matrix.size() && matrix.at(end, row))
                    ++end;
                list.rounded_rect(
                    {x0 + static_cast<float>((kQrQuietZoneModules + column) * layout.module_pixels),
                     y0 + static_cast<float>((kQrQuietZoneModules + row) * layout.module_pixels),
                     static_cast<float>((end - column) * layout.module_pixels),
                     static_cast<float>(layout.module_pixels)},
                    0.0F, kBackground);
                column = end;
            }
        }
        return true;
    }

    void draw_qr_placeholder(float x, float y, float side, std::string_view title,
                             std::string_view detail)
    {
        scene_.rounded_rect({x, y, side, side}, 0.0F, kText);
        ui::text(scene_, fonts_.display, title, x + side * 0.5F, y + side * 0.48F,
                 30.0F, kBackground, gfx::Align::center);
        ui::text(scene_, fonts_.regular, detail, x + side * 0.5F, y + side * 0.57F,
                 20.0F, kBackground.with_alpha(0.78F), gfx::Align::center);
    }

    // Beside the account name and password form: what the current step waits for, and what Playden keeps.
    void draw_password_panel(const State &state)
    {
        const SignInForm &form = state.sign_in;
        const gfx::Rect panel{1036.0F, 224.0F, 640.0F, 430.0F};
        scene_.rounded_rect(panel, 14.0F, kPanel);
        const char *title = "Account name and password";
        const char *detail = state.stay_signed_in
            ? "Playden sends your password to Steam, encrypted, and never saves it. It keeps Steam's sign-in token "
              "on this PS5 so you stay signed in."
            : "Playden sends your password to Steam, encrypted, and never saves it. It keeps Steam's sign-in token "
              "only until you close Playden. You will sign in again next time.";
        if (form.step == SignInStep::Submitting)
        {
            title = "Signing in";
            detail = "Waiting for Steam to check your account name and password.";
        }
        else if (form.step == SignInStep::Approval && !form.code_entry)
        {
            title = "Check your phone";
            detail = form.code_allowed ? "Approve the sign-in in the Steam Mobile app, or type its code instead."
                                       : "Approve the sign-in when Steam asks you to.";
        }
        else if (form.step != SignInStep::Password)
        {
            title = "Steam Guard";
            detail = "Steam asks for a code to confirm that it is you. Codes have five letters or digits.";
        }
        ui::text(scene_, fonts_.display, title, panel.x + 48.0F, panel.y + 96.0F, 40.0F, kText);
        ui::paragraph(scene_, fonts_.regular, detail, panel.x + 48.0F, panel.y + 156.0F, 25.0F,
                      panel.w - 96.0F, 36.0F, kSecondary, 6);
        if (state.simulation_mode)
        {
            scene_.rounded_rect({1185.0F, 678.0F, 292.0F, 42.0F}, 21.0F, kAmber.with_alpha(0.18F));
            ui::text(scene_, fonts_.semibold, "SIMULATED SIGN-IN", 1331.0F, 706.0F, 18.0F, kAmber,
                     gfx::Align::center);
        }
    }

    void draw_sign_in(ui::Canvas &, const ViewData &view, const State &state)
    {
        const bool storage_full = state.account().authentication == AuthState::Failed && state.account().storage_full;
        section_label(storage_full ? "Console storage" : "Connect Steam", 96.0F, 78.0F);
        draw_title(view, 96.0F, 272.0F, 720.0F);
        // Four buttons fit above the footer; a fifth (the password form) lifts the column by one.
        const auto buttons = std::count_if(view.rows.begin(), view.rows.end(), [](const ViewRow &row) {
            return row.kind == RowKind::Action || row.kind == RowKind::Toggle;
        });
        draw_actions(view, 96.0F, buttons > 4 ? 514.0F : 594.0F, 540.0F);
        constexpr float qr_x = 1116.0F;
        constexpr float qr_y = 224.0F;
        constexpr int qr_side = 430;
        if (state.sign_in.step != SignInStep::Qr && state.account().authentication != AuthState::Syncing)
        {
            draw_password_panel(state);
            return;
        }
        if (state.simulation_mode && state.account().challenge_url.empty()) {
            draw_simulated_qr(qr_x, qr_y, static_cast<float>(qr_side));
        } else if (state.account().authentication == AuthState::Waiting &&
                   state.account().challenge_url.empty()) {
            draw_qr_placeholder(qr_x, qr_y, static_cast<float>(qr_side),
                                "Preparing QR code", "Waiting for a secure challenge…");
        } else if (state.account().authentication == AuthState::Syncing) {
            draw_qr_placeholder(qr_x, qr_y, static_cast<float>(qr_side),
                                "Sign-in approved", "Loading your library…");
        } else if (state.account().authentication == AuthState::Expired) {
            draw_qr_placeholder(qr_x, qr_y, static_cast<float>(qr_side),
                                "QR code expired", "Refresh to continue");
        } else if (storage_full) {
            draw_qr_placeholder(qr_x, qr_y, static_cast<float>(qr_side),
                                "Storage is full", "Uninstall games to sign in");
        } else if (state.account().authentication == AuthState::Failed) {
            draw_qr_placeholder(qr_x, qr_y, static_cast<float>(qr_side),
                                "Connection failed", "Retry the connection");
        } else if (!cached_qr_error_.empty() || !draw_encoded_qr(qr_x, qr_y, qr_side)) {
            draw_qr_placeholder(qr_x, qr_y, static_cast<float>(qr_side),
                                "QR code unavailable", "Retry the connection");
        }
        if (state.simulation_mode) {
            scene_.rounded_rect({1185.0F, 678.0F, 292.0F, 42.0F}, 21.0F,
                                kAmber.with_alpha(0.18F));
            ui::text(scene_, fonts_.semibold, "SIMULATED QR — NOT LIVE", 1331.0F, 706.0F,
                     18.0F, kAmber, gfx::Align::center);
        }
        const char *status = !state.simulation_mode && state.account().challenge_url.empty() ?
            "Connecting securely" : "Waiting for approval";
        gfx::Color color = kAccent;
        if (!cached_qr_error_.empty())
        {
            status = "QR code unavailable · Retry connection";
            color = kRed;
        }
        else if (state.account().authentication == AuthState::Expired)
        {
            status = "Challenge expired";
            color = kAmber;
        }
        else if (storage_full)
        {
            status = "Console storage is full · Retrying";
            color = kRed;
        }
        else if (state.account().authentication == AuthState::Failed)
        {
            status = state.account().error.empty() ? "Connection failed" : state.account().error.c_str();
            color = kRed;
        }
        else if (state.account().authentication == AuthState::Syncing)
        {
            status = state.simulation_mode ? "Approved · Syncing library" : "Approved · Loading library";
            color = kGreen;
        }
        else if (state.account().authentication == AuthState::Restoring)
        {
            status = state.simulation_mode ? "Restoring fixture state" : "Restoring your accounts";
        }
        else if (state.account().authentication == AuthState::SigningOut)
        {
            status = "Signing out securely";
        }
        scene_.circle(1198.0F, 772.0F, 7.0F, color);
        ui::text(scene_, fonts_.regular, status, 1220.0F, 780.0F, 25.0F, kText);
    }

    void draw_home(ui::Canvas &, Simulation &simulation)
    {
        const ViewData &view = simulation.route_view();
        const State &state = simulation.state();
        if (view.shelves.empty())
        {
            section_label(view.title.empty() ? "Home" : view.title, 96.0F, 168.0F);
            ViewData copy = view;
            draw_title(copy, 96.0F, 360.0F, 820.0F);
            draw_actions(copy, 96.0F, 560.0F, 520.0F);
            return;
        }

        // As on macOS (`FocusViewport.reveal`): Home scrolls only when the focused tile or shelf
        // would leave the screen, and eases there, so the highlight is what moves; each shelf
        // keeps its own column and scroll position.
        // macOS spacing: room under each shelf for the focused game's title and status.
        constexpr float shelf_pitch = 456.0F;
        constexpr float tile_pitch = 233.0F;
        constexpr float tile_width = 213.0F;
        const int active_shelf = std::clamp(state.navigation.home_shelf, 0,
                                            static_cast<int>(view.shelves.size()) - 1);
        const float shelves_height = static_cast<float>(view.shelves.size()) * shelf_pitch;
        // Vertically too, only as far as needed to show the focused shelf and its title.
        constexpr float shelf_extent = 430.0F;
        home_scroll_.target = reveal(home_scroll_.target, active_shelf * shelf_pitch,
                                     active_shelf * shelf_pitch + shelf_extent, kFooterY - 205.0F,
                                     shelves_height - shelf_pitch + shelf_extent);
        const float vertical = home_scroll_.step(frame_dt_, state.reduced_motion);
        scene_.push_clip({0.0F, 180.0F, kWidth, kFooterY - 180.0F});
        for (int shelf_index = 0; shelf_index < static_cast<int>(view.shelves.size()); ++shelf_index)
        {
            const ViewData::Shelf &shelf = view.shelves[static_cast<std::size_t>(shelf_index)];
            const float top = 205.0F + shelf_index * shelf_pitch - vertical;
            const auto remembered = state.navigation.home_shelf_focus.find(shelf.key);
            const int count = static_cast<int>(shelf.items.size());
            const int focused_item = std::clamp(remembered == state.navigation.home_shelf_focus.end() ?
                                                    shelf.focused_item : remembered->second, 0, std::max(0, count - 1));
            Scroll &row = home_rows_[shelf.key];
            row.target = reveal(row.target, focused_item * tile_pitch, focused_item * tile_pitch + tile_width,
                                kWidth - 2.0F * kSafeX, count * tile_pitch - (tile_pitch - tile_width));
            const float horizontal = row.step(frame_dt_, state.reduced_motion);
            if (top + shelf_pitch < 180.0F || top > kFooterY)
                continue;
            section_label(shelf.title, 96.0F, top);
            const int first = std::max(0, static_cast<int>(horizontal / tile_pitch) - 1);
            const int last = std::min(count, static_cast<int>((horizontal + kWidth) / tile_pitch) + 1);
            for (int item_index = first; item_index < last; ++item_index)
            {
                const ViewRow &cell = shelf.items[static_cast<std::size_t>(item_index)];
                const gfx::Rect art{kSafeX + item_index * tile_pitch - horizontal, top + 28.0F, tile_width, 320.0F};
                const bool focused = route_focus_ && shelf_index == active_shelf &&
                                     item_index == focused_item;
                if (!cell.game_id.valid() && cell.action == "shelf-more")
                {
                    // The rest of the shelf lives in the Library: a poster-sized card with the count.
                    if (focused)
                        stats_.focused_control_key = cell.key;
                    const float amount = control_focus(cell.key, focused);
                    focus_glow(scene_, art, amount);
                    scene_.rounded_rect(art, 8.0F, kBackground);
                    scene_.rounded_rect(art, 8.0F, kText.with_alpha(0.06F));
                    scene_.bordered_rect(art, 8.0F, kBackground.with_alpha(0.0F), 2.0F, kText.with_alpha(0.2F));
                    ui::text(scene_, fonts_.display, cell.value, art.cx(), art.cy() + 4.0F, 56.0F, kText, gfx::Align::center);
                    ui::text(scene_, fonts_.display, "Library", art.cx(), art.cy() + 52.0F, 26.0F, kSecondary,
                             gfx::Align::center);
                    focus_ring(scene_, art, amount);
                    if (amount > 0.0F)
                    {
                        scene_.push_opacity(amount);
                        dynamic_text(cell.title, art.x, art.y + art.h + 46.0F, 24.0F, kText, 420.0F);
                        scene_.pop_opacity();
                    }
                    continue;
                }
                const Game *game = game_by_id(state, cell.game_id);
                if (game == nullptr)
                    continue;
                if (focused)
                    stats_.focused_control_key = shelf.items[static_cast<std::size_t>(item_index)].key;
                const std::string key = "home-" + shelf.key + "-" + ToKey(game->id);
                draw_poster(*game, art, focused, job_progress(game->id), false, key);
                const float amount = focus_motion_.amount(key, focused);
                if (amount > 0.0F)
                {
                    scene_.push_opacity(amount);
                    dynamic_text(game->title, art.x, art.y + art.h + 46.0F, 24.0F, kText, 420.0F);
                    ui::text(scene_, fonts_.regular, fit(fonts_.regular, tile_subtitle(*game, tile_status(game->id)), 18.0F, 420.0F),
                             art.x, art.y + art.h + 74.0F, 18.0F, kSecondary);
                    scene_.pop_opacity();
                }
            }
        }
        scene_.pop_clip();
    }

    // The smallest scroll that keeps [item_min, item_max] inside the viewport (macOS FocusViewport).
    static float reveal(float offset, float item_min, float item_max, float viewport, float content)
    {
        const float maximum = std::max(0.0F, content - viewport);
        float result = std::clamp(offset, 0.0F, maximum);
        if (item_min < result)
            result = item_min;
        else if (item_max > result + viewport)
            result = item_max - viewport;
        return std::clamp(result, 0.0F, maximum);
    }

    // A scroll position easing to its target, close to the macOS 0.18 s ease-out.
    struct Scroll
    {
        float target = 0.0F;
        float value = 0.0F;
        bool started = false;
        float step(float dt, bool reduced_motion)
        {
            if (!started || reduced_motion)
            {
                started = true;
                value = target;
                return value;
            }
            value += (target - value) * (1.0F - std::exp(-dt / 0.05F));
            if (std::fabs(target - value) < 0.5F)
                value = target;
            return value;
        }
    };

    // A vertical list's scroll, eased like Home's: keeps [item_min, item_max] (content coordinates)
    // in a viewport of the given height, one position per list key.
    float list_scroll(const std::string &key, float item_min, float item_max, float viewport, float content,
                      bool reduced_motion)
    {
        Scroll &scroll = list_scrolls_[key];
        scroll.target = reveal(scroll.target, item_min, item_max, viewport, content);
        return scroll.step(frame_dt_, reduced_motion);
    }

    void draw_library(ui::Canvas &canvas, Simulation &simulation)
    {
        const State &state = simulation.state();
        const ViewData &view = simulation.route_view();
        const auto games = simulation.visible_games();
        draw_library_rail(view, state);

        if (games.empty())
        {
            ui::text(scene_, fonts_.display, view.title.empty() ? "No games here" : view.title,
                     1134.0F, 434.0F, 56.0F, kText, gfx::Align::center);
            ui::paragraph(scene_, fonts_.regular,
                          view.subtitle.empty() ? "Clear the search or choose another library view."
                                                : view.subtitle,
                          784.0F, 486.0F, 26.0F, 700.0F, 36.0F, kSecondary, 3,
                          gfx::Align::center);
            draw_actions(view, 914.0F, 620.0F, 440.0F);
            return;
        }

        visible_grid_games_ = games;
        grid_.style.reduced_motion = state.reduced_motion;
        const bool grid_active = route_focus_ && !state.navigation.library_rail_focused;
        grid_.style.theme.focus = grid_active ? kAccent : kAccent.with_alpha(0.0F);
        grid_.set_count(static_cast<int>(games.size()));
        int grid_focus = std::clamp(state.navigation.selected_index, 0,
                                    static_cast<int>(games.size()) - 1);
        const auto selected = std::find_if(games.begin(), games.end(), [&state](const Game *game) {
            return game->id == state.navigation.selected_game_id;
        });
        if (selected != games.end())
            grid_focus = static_cast<int>(std::distance(games.begin(), selected));
        grid_focused_game_ = games[static_cast<std::size_t>(grid_focus)]->id;
        stats_.grid_focused_game_id = grid_focused_game_.steam_app().value_or(0);
        if (grid_active)
            stats_.focused_control_key = "game-" + ToKey(grid_focused_game_);
        grid_.set_focus(grid_focus, true);
        grid_.set_active(grid_active);
        grid_.content = [this](ui::Canvas &, const gfx::Rect &cell, const ui::CardItem &, int index,
                              float /*focus*/) {
            ++stats_.grid_drawn_cell_count;
            const Game &game = *visible_grid_games_[static_cast<std::size_t>(index)];
            const bool focused = route_focus_ && !current_state_->navigation.library_rail_focused &&
                                 game.id == grid_focused_game_;
            draw_poster(game, cell, focused, job_progress(game.id), true,
                        "library-" + ToKey(game.id));
        };
        grid_.accent = [this](int index) {
            return id_color(visible_grid_games_[static_cast<std::size_t>(index)]->id);
        };
        grid_.update(1.0F / 60.0F);
        grid_.draw(canvas);
        if (state.library_syncing || state.library_loading)
        {
            scene_.rounded_rect({1398.0F, 86.0F, 426.0F, 48.0F}, 8.0F, kPanel);
            scene_.circle(1424.0F, 110.0F, 6.0F, kAccent);
            ui::text(scene_, fonts_.regular, "Refreshing library · focus preserved", 1444.0F,
                     119.0F, 20.0F, kSecondary);
        }
    }

    void draw_library_rail(const ViewData &view, const State &state)
    {
        struct RailEntry
        {
            std::string key;
            std::string title;
            std::string value;
            bool selected = false;
        };
        // As the Installed scope counts them: imported games playing from their folder too.
        const std::size_t installed = std::count_if(
            state.installations.begin(), state.installations.end(),
            [&state](const Installation &installation) {
                return installation.installed_bytes > 0 || !state.simulation_mode;
            }) + state.external_games.size();
        std::vector<RailEntry> entries = {
            {"rail-installed", "Installed", std::to_string(installed), state.library.scope == "installed"},
            {"rail-all", "All", std::to_string(state.games.size()), state.library.scope == "all"},
            {"rail-favorites", "Favorites", std::to_string(state.library.favorites.size()),
             state.library.scope == "favorites"},
            {"rail-hidden", "Hidden", std::to_string(state.library.hidden.size()), state.library.scope == "hidden"}};
        if (has_imported_games(state))
            entries.push_back({"rail-imported", "Imported",
                               std::to_string(std::count_if(state.games.begin(), state.games.end(),
                                                            [](const Game &game) { return game.id.store == Store::kLocal; })),
                               state.library.scope == "imported"});
        for (const Collection &collection : state.library.collections)
            entries.push_back({"rail-" + collection.id, collection.name,
                               std::to_string(collection.members.size()),
                               state.library.scope == collection.id});
        entries.push_back({"rail-new-collection", "New collection", "+", false});
        const auto selected_entry = std::find_if(entries.begin(), entries.end(),
                                                 [](const RailEntry &entry) { return entry.selected; });
        const int selected = selected_entry == entries.end() ? -1 :
                                 static_cast<int>(std::distance(entries.begin(), selected_entry));
        const int focus_index = state.navigation.library_rail_focused ? state.navigation.selected_index :
                                                                         std::max(0, selected);
        // An applied search gets its own row above the rail, so nothing is drawn over a focus ring.
        const bool searching = !state.library.query.empty();
        const float rail_top = searching ? 200.0F : 150.0F;
        if (searching)
        {
            scene_.rounded_rect({96.0F, 150.0F, 300.0F, 38.0F}, 6.0F, kPanel);
            ui::text(scene_, fonts_.regular, fit(fonts_.regular, state.library.query, 22.0F, 260.0F),
                     118.0F, 177.0F, 22.0F, kText);
        }
        const ShelfWindow window = shelf_window(static_cast<int>(entries.size()), focus_index, searching ? 11 : 12);
        for (int visible = 0; visible < window.count; ++visible)
        {
            const int index = window.first + visible;
            const float y = rail_top + visible * 66.0F;
            const bool focused = route_focus_ && state.navigation.library_rail_focused &&
                                 index == state.navigation.selected_index;
            const gfx::Rect entry{96.0F, y, 300.0F, 60.0F};
            const float amount = control_focus(entries[static_cast<std::size_t>(index)].key, focused);
            focus_glow(scene_, entry, amount, true);
            scene_.rounded_rect(entry, 8.0F, kBackground);
            if (entries[static_cast<std::size_t>(index)].selected || index == selected)
                scene_.rounded_rect(entry, 8.0F, kText.with_alpha(0.10F));
            focus_ring(scene_, entry, amount, true);
            if (focused)
                stats_.focused_control_key = entries[static_cast<std::size_t>(index)].key;
            ui::text(scene_, fonts_.display,
                     fit(fonts_.display, entries[static_cast<std::size_t>(index)].title, 28.0F, 225.0F),
                     118.0F, y + 40.0F, 28.0F,
                     entries[static_cast<std::size_t>(index)].selected || index == selected ? kText
                                                                                             : kSecondary);
            ui::text(scene_, fonts_.regular, entries[static_cast<std::size_t>(index)].value,
                     374.0F, y + 38.0F, 20.0F, kMuted,
                     gfx::Align::right);
        }
        if (!view.subtitle.empty() && entries.size() <= (searching ? 7U : 8U))
            ui::paragraph(scene_, fonts_.regular, view.subtitle, 96.0F, 722.0F, 22.0F, 300.0F,
                          30.0F, kSecondary, 4);
    }

    float job_progress(GameId id) const
    {
        if (current_state_ == nullptr)
            return -1.0F;
        const auto found = std::find_if(current_state_->jobs.begin(), current_state_->jobs.end(),
                                        [id](const Job &job) { return job.game_id == id; });
        if (found == current_state_->jobs.end() || found->total_bytes == 0)
            return -1.0F;
        return static_cast<float>(found->completed_bytes) / static_cast<float>(found->total_bytes);
    }

    // What a tile says about a game's install, as the macOS tiles do: a badge for a job or a
    // missing drive, the size once installed, a download mark while it is not.
    struct TileStatus
    {
        const Job *job = nullptr;
        const Installation *installation = nullptr;
        bool installed = false;
        bool disconnected = false;
        bool in_place = false;  // an imported game playing from its folder (no installation)
    };

    TileStatus tile_status(GameId id) const
    {
        TileStatus status;
        if (current_state_ == nullptr)
            return status;
        const State &state = *current_state_;
        const auto job = std::find_if(state.jobs.begin(), state.jobs.end(), [id](const Job &item) {
            // A finished download listed in Downloads is the installation now, not a transfer.
            return item.game_id == id && item.state != JobState::Cancelled && item.state != JobState::Downloaded &&
                   item.stage != JobStage::Uninstall;
        });
        if (job != state.jobs.end())
            status.job = &*job;
        const auto installation = std::find_if(state.installations.begin(), state.installations.end(),
                                               [id, &state](const Installation &item) {
                                                   return item.game_id == id &&
                                                          (item.installed_bytes > 0 || !state.simulation_mode);
                                               });
        if (installation != state.installations.end())
        {
            status.installation = &*installation;
            const auto volume = std::find_if(state.volumes.begin(), state.volumes.end(),
                                             [&installation](const Volume &item) {
                                                 return item.id == installation->volume_id;
                                             });
            status.disconnected = !installation->available || (volume != state.volumes.end() && !volume->available);
            status.installed = !status.disconnected;
        }
        else if (const auto external = std::find_if(state.external_games.begin(), state.external_games.end(),
                                                    [id](const LiveExternalGame &item) { return item.game_id == id; });
                 external != state.external_games.end())
        {
            status.in_place = true;
            status.disconnected = !external->available;
            status.installed = external->available;
        }
        return status;
    }

    std::string tile_subtitle(const Game &game, const TileStatus &status) const
    {
        if (status.job != nullptr)
        {
            if (status.job->state == JobState::Downloading && status.job->checking_licenses)
                return "Checking licenses";
            if (status.job->state == JobState::Downloading && status.job->total_bytes > 0)
                return std::to_string(static_cast<int>(100.0 * status.job->completed_bytes / status.job->total_bytes)) +
                       "% · " + JobStateText(*status.job);
            return JobStateText(*status.job);
        }
        if (status.disconnected)
            return "Reconnect its drive";
        if (status.installed)
        {
            const std::uint64_t bytes = status.installation != nullptr ? status.installation->installed_bytes :
                                        game.install_bytes.value_or(0);
            const std::string size = bytes > 0 ? short_size(bytes) + " · " : std::string{};
            return size + (game.playtime_minutes && *game.playtime_minutes > 0 ?
                               playtime_label(game.playtime_minutes) + " played" : std::string("Installed"));
        }
        return game.download_bytes ? "Not installed · " + short_size(*game.download_bytes) : "Not installed";
    }

    // reserve: room kept free at the right for the corner badges.
    void tile_badge(const gfx::Rect &rect, std::string_view text, gfx::Color dot, float reserve = 0.0F)
    {
        constexpr float size = 16.0F;
        const float width = std::min(rect.w - 20.0F - reserve, fonts_.semibold.measure(text, size) + 44.0F);
        scene_.rounded_rect({rect.x + 10.0F, rect.y + 10.0F, width, 32.0F}, 6.0F, kBackground.with_alpha(0.85F));
        scene_.circle(rect.x + 26.0F, rect.y + 26.0F, 4.0F, dot);
        ui::text(scene_, fonts_.semibold, fit(fonts_.semibold, text, size, width - 44.0F), rect.x + 38.0F,
                 rect.y + 32.0F, size, kText);
    }

    // A word in the tile's top-right corner, `right` px in from the corner's usual spot; returns the
    // width it takes, gap included.
    float corner_badge(const gfx::Rect &rect, std::string_view text, float right = 0.0F)
    {
        constexpr float size = 16.0F;
        const float width = fonts_.semibold.measure(text, size) + 20.0F;
        scene_.rounded_rect({rect.x + rect.w - 10.0F - right - width, rect.y + 10.0F, width, 32.0F}, 6.0F,
                            kBackground.with_alpha(0.85F));
        ui::text(scene_, fonts_.semibold, text, rect.x + rect.w - 20.0F - right, rect.y + 32.0F, size, kAccent,
                 gfx::Align::right);
        return width + 8.0F;
    }

    // A Steam demo says "Demo" and a game a Steam family member shares says "Family", right to left;
    // returns the width they take. Beside a status badge a tile has room for one: Demo.
    float corner_badges(const Game &game, const gfx::Rect &rect, bool status_badge)
    {
        float taken = 0.0F;
        if (game.demo)
            taken += corner_badge(rect, "Demo", taken);
        if (game.family_shared && !(game.demo && status_badge))
            taken += corner_badge(rect, "Family", taken);
        return taken;
    }

    void download_mark(float cx, float cy, float opacity)
    {
        scene_.circle(cx, cy, 15.0F, kBackground.with_alpha(0.8F * opacity));
        scene_.ring(cx, cy, 15.0F, 1.5F, kText.with_alpha(0.5F * opacity));
        const gfx::Color ink = kText.with_alpha(opacity);
        scene_.line(cx, cy - 7.0F, cx, cy + 3.0F, 1.8F, ink);
        scene_.line(cx - 4.5F, cy - 1.5F, cx, cy + 3.0F, 1.8F, ink);
        scene_.line(cx + 4.5F, cy - 1.5F, cx, cy + 3.0F, 1.8F, ink);
        scene_.line(cx - 6.0F, cy + 7.0F, cx + 6.0F, cy + 7.0F, 1.8F, ink);
    }

    void draw_poster(const Game &game, const gfx::Rect &rect, bool focused, float progress,
                     bool title_overlay = true, std::string_view motion_key = {})
    {
        // The focus amount eases in and out, so a focus change cross-fades the two cells.
        const float amount = motion_key.empty() ? (focused ? 1.0F : 0.0F) : focus_motion_.amount(motion_key, focused);
        const float scale = motion_key.empty() ? 1.0F : 1.0F + (FocusMotion::focused_scale - 1.0F) * amount;
        scene_.push_transform(scale, rect.cx(), rect.cy(), 0.0F, 0.0F);
        // Fixture games are numbered from 900001.
        const std::uint32_t number = game.id.steam_app().value_or(0);
        const std::size_t texture_index = number >= 900001 ? (number - 900001) % art_.size() :
                                                             number % art_.size();
        const bool simulated = current_state_ != nullptr && current_state_->simulation_mode;
        // Fixture posters belong to the fixture's Steam games; an imported game has none (its title shows).
        const std::uint32_t texture = simulated
            ? (game.art_failed || game.art_pending || !game.id.steam_app() ? 0 : art_[texture_index])
            : (live_artwork_ && current_state_ && !current_state_->account().id.empty()
                ? store_texture(game.id, CoverUrl(game)) : 0);
        // No cover from the store: the icon inside the game's program, over its colour (#213).
        const std::uint32_t icon = simulated || texture != 0 ? 0 : program_icon_texture(game.id, CoverUrl(game));
        const TileStatus status = tile_status(game.id);
        // Glow first: it is a filled soft shape, and the art must cover its middle so only the
        // light outside the tile shows, as on macOS. The art itself is never tinted.
        scene_.shadow(rect, 8.0F, 12.0F + 14.0F * amount, gfx::Color::rgb(0x000000, 0.35F + 0.3F * amount));
        if (amount > 0.0F)
            scene_.glow(rect.inset(-7.0F), 13.0F, 24.0F, kAccent.with_alpha(0.5F * amount));
        if (texture != 0)
            scene_.image(texture, rect, gfx::kFullUv, kText, 8.0F);
        else
        {
            scene_.gradient_rect(rect, 8.0F, id_color(game.id), gfx::Color::rgb(0x24201D));
            // A thumbnail (the Queue's) is too narrow for words: its row names the game.
            const bool words_fit = rect.w >= 120.0F;
            if (icon != 0)
            {
                // Above the title on a tile; alone, and larger, on a thumbnail.
                const float side = words_fit ? std::min(rect.w * 0.5F, rect.h * 0.36F) : std::min(rect.w, rect.h) * 0.72F;
                const float centre = words_fit ? rect.y + rect.h * 0.34F : rect.cy();
                scene_.image(icon, {rect.cx() - side * 0.5F, centre - side * 0.5F, side, side}, gfx::kFullUv, kText, 0.0F);
            }
            if (words_fit && game.art_pending)
            {
                ui::text(scene_, fonts_.display, "Artwork loading", rect.cx(), rect.y + rect.h * 0.52F,
                         25.0F, kText, gfx::Align::center);
            }
            else if (words_fit)
            {
                // The focus overlay names a focused tile, so the placeholder need not repeat it.
                const float placeholder = title_overlay ? 1.0F - amount : 1.0F;
                const ui::FontRef &face = font_for(game.title, fonts_.display);
                ui::paragraph(scene_, face, game.title, rect.cx(), rect.y + rect.h * (icon != 0 ? 0.62F : 0.42F),
                              28.0F, rect.w - 36.0F, 31.0F, kText.with_alpha(placeholder), icon != 0 ? 3 : 4, gfx::Align::center);
            }
        }
        const bool downloading = status.job != nullptr &&
            (status.job->state == JobState::Downloading || status.job->state == JobState::Verifying ||
             status.job->state == JobState::Pausing || status.job->state == JobState::Paused);
        const bool show_download_mark = status.job == nullptr && status.installation == nullptr && !status.in_place;
        if (title_overlay && amount > 0.0F && !game.art_pending && !downloading)
        {
            scene_.push_opacity(amount);
            scene_.gradient_rect({rect.x, rect.y + rect.h - 105.0F, rect.w, 105.0F}, 8.0F,
                                 kBackground.with_alpha(0.0F), kBackground.with_alpha(0.92F));
            const float text_width = rect.w - 28.0F - (show_download_mark ? 36.0F : 0.0F);
            dynamic_text(game.title, rect.x + 14.0F, rect.y + rect.h - 40.0F, 24.0F, kText, text_width);
            ui::text(scene_, fonts_.regular, fit(fonts_.regular, tile_subtitle(game, status), 15.0F, text_width),
                     rect.x + 14.0F, rect.y + rect.h - 17.0F, 15.0F, kSecondary);
            scene_.pop_opacity();
        }
        if (amount > 0.0F)
            scene_.bordered_rect(rect.inset(-9.0F), 15.0F, kBackground.with_alpha(0.0F), 4.0F,
                                 kAccent.with_alpha(amount));
        // A thumbnail (the Downloads card) is too small for tile badges; its card says it all.
        if (rect.w < 180.0F)
        {
            scene_.pop_transform();
            return;
        }
        const bool job_badge = status.job != nullptr && !downloading;
        const float corner = corner_badges(game, rect, job_badge || status.disconnected);
        if (job_badge)
            tile_badge(rect, JobStateText(*status.job),
                       status.job->state == JobState::Failed ? kAmber : kSecondary, corner);
        else if (status.disconnected)
            tile_badge(rect, "Drive disconnected", kAmber, corner);
        if (show_download_mark)
            download_mark(rect.x + rect.w - 25.0F, rect.y + rect.h - 25.0F, 1.0F);
        else if (status.installed && status.installation != nullptr && status.installation->installed_bytes > 0 && amount < 1.0F && title_overlay)
        {
            const std::string size = short_size(status.installation->installed_bytes);
            const float width = fonts_.semibold.measure(size, 16.0F) + 20.0F;
            scene_.push_opacity(1.0F - amount);
            scene_.rounded_rect({rect.x + rect.w - 10.0F - width, rect.y + rect.h - 42.0F, width, 32.0F}, 6.0F,
                                kBackground.with_alpha(0.85F));
            ui::text(scene_, fonts_.semibold, size, rect.x + rect.w - 20.0F, rect.y + rect.h - 20.0F, 16.0F,
                     kText, gfx::Align::right);
            scene_.pop_opacity();
        }
        if (downloading && progress >= 0.0F)
        {
            const float value = std::clamp(progress, 0.0F, 1.0F);
            scene_.gradient_rect({rect.x, rect.y + rect.h - 64.0F, rect.w, 64.0F}, 8.0F,
                                 kBackground.with_alpha(0.0F), kBackground.with_alpha(0.9F));
            ui::text(scene_, fonts_.semibold, JobStateText(*status.job), rect.x + 12.0F,
                     rect.y + rect.h - 30.0F, 16.0F, kText);
            if (status.job->state == JobState::Downloading)
                ui::text(scene_, fonts_.semibold, std::to_string(static_cast<int>(value * 100.0F)) + "%",
                         rect.x + rect.w - 12.0F, rect.y + rect.h - 30.0F, 16.0F, kText, gfx::Align::right);
            scene_.rounded_rect({rect.x + 12.0F, rect.y + rect.h - 18.0F, rect.w - 24.0F, 6.0F},
                                3.0F, kText.with_alpha(0.20F));
            scene_.rounded_rect({rect.x + 12.0F, rect.y + rect.h - 18.0F, (rect.w - 24.0F) * value, 6.0F},
                                3.0F, status.job->state == JobState::Paused ? kAmber : kAccent);
        }
        scene_.pop_transform();
    }

    std::vector<ui::ListItem> list_items(const ViewData &view) const
    {
        std::vector<ui::ListItem> items;
        items.reserve(view.rows.size());
        for (const ViewRow &row : view.rows)
        {
            ui::ListItem item;
            item.title = row.title;
            item.subtitle = row.subtitle;
            item.value = row.value;
            item.badge = row.badge;
            item.disabled = !row.enabled;
            item.header = row.kind == RowKind::Heading || row.kind == RowKind::Separator;
            item.chevron = row.kind == RowKind::Choice || row.kind == RowKind::Profile ||
                           !row.action.empty();
            items.push_back(std::move(item));
        }
        return items;
    }

    void draw_view_list(ui::Canvas &canvas, const ViewData &view, const gfx::Rect &bounds,
                        float row_height = 90.0F)
    {
        list_.style.row_height = row_height;
        list_.style.theme.focus = route_focus_ ? kAccent : kAccent.with_alpha(0.0F);
        list_.set_items(list_items(view));
        list_.set_bounds(bounds);
        list_.set_focus(std::clamp(view.focused_row, 0, std::max(0, static_cast<int>(view.rows.size()) - 1)),
                        true);
        list_.set_active(route_focus_);
        list_.update(1.0F / 60.0F);
        list_.draw(canvas);

        for (std::size_t index = 0; index < view.rows.size(); ++index)
        {
            const ViewRow &row = view.rows[index];
            if (row.progress <= 0.0F && row.kind != RowKind::Job && row.kind != RowKind::Volume)
                continue;
            const gfx::Rect row_rect = list_.row_rect(static_cast<int>(index));
            if (row_rect.y + row_rect.h < bounds.y || row_rect.y > bounds.y + bounds.h)
                continue;
            ui::ProgressBar progress;
            progress.style.theme = theme_;
            progress.style.height = 7.0F;
            progress.style.placement = ui::LabelPlacement::none;
            progress.style.status = progress_status(row);
            progress.style.finish_flash = false;
            progress.set_bounds({row_rect.x + 24.0F, row_rect.y + row_rect.h - 16.0F,
                                 row_rect.w - 48.0F, 7.0F});
            progress.set_value(std::clamp(row.progress, 0.0F, 1.0F), true);
            progress.draw(canvas);
        }
    }

    // When a finished download or preparation ended: "just now", "12 min ago", then relative_time.
    static std::string finished_words(std::int64_t finished_at, std::int64_t now)
    {
        const std::int64_t seconds = std::max<std::int64_t>(0, now - finished_at);
        return seconds < 60     ? std::string("just now")
             : seconds < 3600 ? std::to_string(seconds / 60) + " min ago"
                              : relative_time(finished_at, now);
    }

    // Queue: Downloads, then Preparation, one section expanded, then one line of drives. The model's rows
    // say what shows and in which order; the list between the page title and the drives scrolls to keep
    // the focused row whole, and the drives and the button guide stay put.
    void draw_queue(ui::Canvas &canvas, const ViewData &view, const State &state)
    {
        const QueueLayout layout = BuildQueueLayout(state);
        section_label("Downloads and preparation", kSafeX, 160.0F);
        ui::text(scene_, fonts_.semibold, fit(fonts_.semibold, view.subtitle, 22.0F, 1000.0F), 1824.0F, 161.0F, 22.0F,
                 kSecondary, gfx::Align::right);
        // While a sheet is open the row it came from stays in view.
        const std::string &list_focus = state.navigation.modals.empty() ? state.navigation.focus_key
                                                                         : state.navigation.modals.front().opener_focus_key;
        const auto focus_of = [&](const std::string &key) {
            const bool focused = route_focus_ && state.navigation.focus_key == key;
            if (focused)
                stats_.focused_control_key = key;
            return control_focus(key, focused);
        };
        const auto entry_of = [&](const std::string &key) -> const QueueEntry * {
            for (const QueueSection *section : {&layout.downloads, &layout.preparation})
            {
                if (section->active && section->active->key == key)
                    return &*section->active;
                for (const QueueEntry &entry : section->rows)
                    if (entry.key == key)
                        return &entry;
            }
            return nullptr;
        };
        const auto card = [&](const std::string &key) {
            return (layout.downloads.active && layout.downloads.active->key == key) ||
                   (layout.preparation.active && layout.preparation.active->key == key);
        };

        constexpr float list_top = 196.0F, list_bottom = 884.0F, viewport = list_bottom - list_top;
        constexpr float heading_height = 52.0F, card_height = 116.0F, row_height = 90.0F;
        struct Placed { const ViewRow *row; float y; float height; };
        std::vector<Placed> placed;
        float y = 0.0F, focus_top = 0.0F, focus_bottom = 0.0F;
        for (const ViewRow &row : view.rows)
        {
            if (row.key.rfind("storage-", 0) == 0)
                continue;
            const bool heading = row.kind == RowKind::Heading;
            if (heading && !placed.empty())
                y += 14.0F;  // the space between the two sections
            const float height = heading ? heading_height : card(row.key) ? card_height : row_height;
            placed.push_back({&row, y, height});
            if (row.key == list_focus)
            {
                focus_top = y;
                focus_bottom = y + height;
            }
            y += height + (heading ? 6.0F : 10.0F);
        }
        const float content = placed.empty() ? 0.0F : placed.back().y + placed.back().height;
        // Focus on a drive (or its sheet) keeps the list where it is.
        if (focus_bottom <= 0.0F)
        {
            focus_top = list_scrolls_["queue"].target + 12.0F;
            focus_bottom = focus_top;
        }
        // Room for the focus ring above and below the focused row.
        const float offset = list_scroll("queue", std::max(0.0F, focus_top - 12.0F), focus_bottom + 12.0F, viewport,
                                         content + 12.0F, state.reduced_motion);

        if (layout.nothing())
        {
            ui::text(scene_, fonts_.display, "Nothing in the queue", kWidth * 0.5F, 500.0F, 56.0F, kText, gfx::Align::center);
            ui::paragraph(scene_, fonts_.regular, "Downloads and game preparation show here. Install a game from its details to start.",
                          kWidth * 0.5F, 552.0F, 26.0F, 800.0F, 36.0F, kSecondary, 2, gfx::Align::center);
        }

        const std::int64_t now = static_cast<std::int64_t>(state.clock_ticks);
        constexpr float left = kSafeX, width = 1824.0F - kSafeX;
        scene_.push_clip({0.0F, list_top - 12.0F, kWidth, viewport + 18.0F});
        for (const Placed &item : placed)
        {
            const float top = list_top + item.y - offset;
            if (top + item.height < list_top - 12.0F || top > list_bottom + 12.0F)
                continue;
            const ViewRow &row = *item.row;
            const gfx::Rect box{left, top, width, item.height};
            const float amount = focus_of(row.key);
            if (row.kind == RowKind::Heading)
            {
                // The section's name, its counts and a chevron: down while open, right while closed.
                const gfx::Color ink = row.selected ? kText : kSecondary;
                ui::text(scene_, fonts_.display, ui::upper(row.title), left + 4.0F, top + 34.0F, 22.0F, ink, gfx::Align::left,
                         2.6F);
                ui::text(scene_, fonts_.regular, fit(fonts_.regular, row.value, 22.0F, 1100.0F), left + width - 40.0F,
                         top + 34.0F, 22.0F, kSecondary, gfx::Align::right);
                const float cx = left + width - 14.0F, cy = top + 26.0F;
                if (row.selected)
                {
                    scene_.line(cx - 8.0F, cy - 4.0F, cx, cy + 4.0F, 2.5F, kAccent);
                    scene_.line(cx, cy + 4.0F, cx + 8.0F, cy - 4.0F, 2.5F, kAccent);
                }
                else
                {
                    scene_.line(cx - 4.0F, cy - 8.0F, cx + 4.0F, cy, 2.5F, kAccent);
                    scene_.line(cx + 4.0F, cy, cx - 4.0F, cy + 8.0F, 2.5F, kAccent);
                }
                focus_ring(scene_, box, amount, true);
                continue;
            }
            const QueueEntry *entry = entry_of(row.key);
            if (entry == nullptr)
                continue;
            const Game *game = game_by_id(state, entry->game_id);
            focus_glow(scene_, box, amount, true);
            scene_.rounded_rect(box, 10.0F, kPanel);
            if (card(row.key))
            {
                const gfx::Rect poster{left + 16.0F, top + 13.0F, 60.0F, 90.0F};
                if (game != nullptr)
                    draw_poster(*game, poster, false, -1.0F, false);
                else
                    scene_.rounded_rect(poster, 6.0F, kMuted.with_alpha(0.25F));
                const float text_x = left + 100.0F, right = left + width - 28.0F;
                dynamic_text(entry->title, text_x, top + 44.0F, 32.0F, kText, 900.0F);
                if (const Job *job = entry->job)
                {
                    const gfx::Color tone = job->state == JobState::Failed ? kRed : job->state == JobState::Paused ? kAmber : kAccent;
                    // A download says only its state; another stage (the Playden update unpacking) names itself.
                    std::string state_text = JobStateText(*job);
                    if (!job->stage_text.empty() || job->stage != JobStage::Download)
                        state_text = (job->stage_text.empty() ? job_stage_label(job->stage) : job->stage_text) + " · " + state_text;
                    const float progress = job->total_bytes == 0 ? 0.0F :
                        static_cast<float>(job->completed_bytes) / static_cast<float>(job->total_bytes);
                    const std::string percent = std::to_string(static_cast<int>(std::clamp(progress, 0.0F, 1.0F) * 100.0F)) + "%";
                    ui::text(scene_, fonts_.regular, percent, right, top + 42.0F, 22.0F, kSecondary, gfx::Align::right);
                    ui::text(scene_, fonts_.semibold, state_text, right - fonts_.regular.measure(percent, 22.0F) - 20.0F,
                             top + 42.0F, 22.0F, tone, gfx::Align::right);
                    ui::ProgressBar bar;
                    bar.style.theme = theme_;
                    bar.style.height = 8.0F;
                    bar.style.placement = ui::LabelPlacement::none;
                    bar.style.status = job->state == JobState::Failed ? ui::Status::danger :
                                       job->state == JobState::Paused ? ui::Status::warning : ui::Status::accent;
                    bar.style.finish_flash = false;
                    bar.set_bounds({text_x, top + 60.0F, right - text_x, 8.0F});
                    bar.set_value(std::clamp(progress, 0.0F, 1.0F), true);
                    bar.draw(canvas);
                    if (job->state == JobState::Downloading && job->checking_licenses)
                    {
                        // Large accounts take a minute here, before Steam gives the download size (issue #47).
                        spinner(text_x + 10.0F, top + 92.0F, 8.0F, kBlue);
                        ui::text(scene_, fonts_.regular, "Checking your Steam licenses…", text_x + 30.0F, top + 99.0F, 21.0F, kText);
                    }
                    else if (job->update && job->state == JobState::Downloading && !job->state_text.empty())
                    {
                        // An update's own steps: hashing the installed files, then putting the new ones in place.
                        spinner(text_x + 10.0F, top + 92.0F, 8.0F, kBlue);
                        ui::text(scene_, fonts_.regular,
                                 job->locked ? "Putting the new files in place… The game can be played once this finishes."
                                             : "Checking which installed files changed…",
                                 text_x + 30.0F, top + 99.0F, 21.0F, kText);
                    }
                    else
                    {
                        ui::text(scene_, fonts_.regular, byte_count(job->completed_bytes) + " / " + byte_count(job->total_bytes),
                                 text_x, top + 99.0F, 21.0F, kText);
                        ui::text(scene_, fonts_.regular,
                                 !job->failure.empty() ? job->failure
                                     : FormatDownloadRate(job->state == JobState::Downloading ? job->bytes_per_second : std::nullopt,
                                                          job->state == JobState::Downloading ? job->eta_seconds : std::nullopt,
                                                          job->state == JobState::Downloading ? job->server_name : std::string()),
                                 right, top + 99.0F, 21.0F, job->failure.empty() ? kSecondary : kRed, gfx::Align::right);
                    }
                }
                else
                {
                    // Preparing or uninstalling: the actual step, with a spinner; no percentage or time is made up.
                    const char *label = entry->leftover ? "Deleting title" : entry->uninstall ? "Uninstalling" : "Preparing";
                    const float status_width = fonts_.semibold.measure(label, 24.0F);
                    spinner(right - status_width - 22.0F, top + 36.0F, 9.0F, kBlue);
                    ui::text(scene_, fonts_.semibold, label, right, top + 44.0F, 24.0F, kBlue, gfx::Align::right);
                    ui::text(scene_, fonts_.regular, fit(fonts_.regular, entry->note.empty() ? "Starting…" : entry->note, 24.0F, 1100.0F),
                             text_x, top + 88.0F, 24.0F, kText);
                    ui::text(scene_, fonts_.regular, entry->leftover || entry->uninstall ? "Removing from this PS5" : "Setting up for this PS5", right,
                             top + 88.0F, 21.0F, kSecondary, gfx::Align::right);
                }
                focus_ring(scene_, box, amount, true);
                continue;
            }
            // A row, as in the Saves list: art, the name with why it waits or failed under it, the status
            // with its colour, then the size and when it finished. Waiting preparations show their place.
            float art_x = left + 14.0F;
            if (entry->position > 0)
            {
                ui::text(scene_, fonts_.display, std::to_string(entry->position), left + 30.0F, top + 55.0F, 30.0F, kSecondary,
                         gfx::Align::center);
                art_x = left + 54.0F;
            }
            save_art(state, entry->game_id, {art_x, top + 9.0F, 48.0F, 72.0F});
            std::string value = entry->status, subtitle = entry->note, aside, badge;
            std::string tone = entry->tone;
            float progress = 0.0F;
            if (const Job *job = entry->job)
            {
                value = JobStateText(*job);
                // An update waiting in the queue says so: "Update queued", "Update paused".
                if (job->update && !entry->completed && job->state_text.empty() && !value.empty())
                {
                    value[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(value[0])));
                    value = "Update " + value;
                }
                tone = job->state == JobState::Failed ? "problem"
                     : job->state == JobState::Paused || job->state == JobState::Interrupted ? "attention"
                     : job->state == JobState::Downloading || job->state == JobState::Verifying ? "busy" : "quiet";
                if (entry->completed)
                {
                    // The files are here; preparing them is the Preparation section's.
                    value = job->game_id.store == Store::kLocal ? "Copied" : job->update ? "Updated" : "Downloaded";
                    tone = "good";
                    aside = byte_count(job->total_bytes);
                    if (job->finished_at > 0 && !state.simulation_mode)
                        badge = finished_words(job->finished_at, now);
                }
                else
                {
                    aside = byte_count(job->completed_bytes) + " / " + byte_count(job->total_bytes);
                    if (job->total_bytes > 0 && job->completed_bytes > 0)
                        progress = static_cast<float>(job->completed_bytes) / static_cast<float>(job->total_bytes);
                }
            }
            else if (entry->completed && entry->finished_at > 0)
            {
                badge = finished_words(entry->finished_at, now);
            }
            const float text_x = art_x + 70.0F;
            const bool two_lines = !subtitle.empty();
            dynamic_text(entry->title, text_x, top + (two_lines ? 40.0F : 56.0F), 32.0F, kText, 1040.0F - 40.0F - text_x);
            if (two_lines)
                ui::text(scene_, fonts_.regular, fit(fonts_.regular, subtitle, 21.0F, 1040.0F - 40.0F - text_x), text_x, top + 72.0F,
                         21.0F, tone == "problem" ? kRed : kSecondary);
            const float status_x = 1040.0F;
            const bool moving = progress > 0.0F;
            if (tone == "busy" && entry->job != nullptr)
                spinner(status_x + 8.0F, top + (moving ? 38.0F : 45.0F), 8.0F, kBlue);
            else
                scene_.circle(status_x + 8.0F, top + (moving ? 38.0F : 45.0F), 7.0F, save_tone(tone));
            ui::text(scene_, fonts_.semibold, value, status_x + 28.0F, top + (moving ? 46.0F : 53.0F), 24.0F, kText);
            if (moving)
            {
                scene_.rounded_rect({status_x + 28.0F, top + 60.0F, 200.0F, 5.0F}, 2.5F, kText.with_alpha(0.12F));
                scene_.rounded_rect({status_x + 28.0F, top + 60.0F, 200.0F * std::clamp(progress, 0.0F, 1.0F), 5.0F}, 2.5F,
                                    tone == "problem" ? kRed : kAccent);
            }
            ui::text(scene_, fonts_.semibold, aside, 1490.0F, top + 53.0F, 24.0F, kText.with_alpha(0.85F), gfx::Align::right);
            ui::text(scene_, fonts_.regular, badge, box.x + box.w - 24.0F, top + 53.0F, 21.0F, kSecondary, gfx::Align::right);
            focus_ring(scene_, box, amount, true);
        }
        scene_.pop_clip();
        draw_storage_strip(view, state, focus_of);
    }

    // One line of drives under the queue: name, a small usage bar and the free space; each opens its details.
    template <class Focus>
    void draw_storage_strip(const ViewData &view, const State &state, const Focus &focus_of)
    {
        constexpr float line_y = 902.0F, baseline = 946.0F;
        scene_.rounded_rect({kSafeX, line_y, 1824.0F - kSafeX, 1.5F}, 0.0F, kText.with_alpha(0.12F));
        ui::text(scene_, fonts_.display, ui::upper("Storage"), kSafeX, baseline, 22.0F, kSecondary, gfx::Align::left, 2.6F);
        const float first_x = kSafeX + fonts_.display.measure(ui::upper("Storage"), 22.0F) + 2.6F * 7.0F + 36.0F;
        struct Drive { const ViewRow *row; const Volume *volume; std::string free; float width; };
        std::vector<Drive> drives;
        int focused = -1;
        for (const ViewRow &row : view.rows)
        {
            if (row.key.rfind("storage-", 0) != 0)
                continue;
            const auto volume = std::find_if(state.volumes.begin(), state.volumes.end(),
                                             [&row](const Volume &item) { return "storage-" + item.id == row.key; });
            if (volume == state.volumes.end())
                continue;
            const std::string free = drive_free_words(*volume);
            const float width = fonts_.regular.measure(volume->label, 24.0F) + (volume->capacity_known ? 88.0F : 0.0F) + 16.0F +
                                fonts_.regular.measure(free, 22.0F) + 34.0F;
            if (row.key == state.navigation.focus_key ||
                (!state.navigation.modals.empty() && row.key == state.navigation.modals.front().opener_focus_key))
                focused = static_cast<int>(drives.size());
            drives.push_back({&row, &*volume, free, width});
        }
        // Drives that don't fit wait past the end; the line starts later so the focused one shows.
        constexpr float room_end = 1824.0F - 80.0F;
        std::size_t begin = 0;
        const auto fits = [&](std::size_t from, std::size_t to) {
            float x = first_x;
            for (std::size_t index = from; index <= to; ++index)
                x += drives[index].width + 40.0F;
            return x - 40.0F <= room_end;
        };
        while (focused > 0 && begin < static_cast<std::size_t>(focused) && !fits(begin, static_cast<std::size_t>(focused)))
            ++begin;
        float x = first_x;
        if (begin > 0)
        {
            const std::string earlier = "+" + std::to_string(begin);
            ui::text(scene_, fonts_.regular, earlier, first_x, baseline, 22.0F, kMuted);
            x += fonts_.regular.measure(earlier, 22.0F) + 28.0F;
        }
        std::size_t index = begin;
        for (; index < drives.size(); ++index)
        {
            const Drive &drive = drives[index];
            if (x + drive.width > room_end)
                break;
            const Volume *volume = drive.volume;
            const gfx::Rect box{x - 12.0F, baseline - 32.0F, drive.width + 24.0F, 46.0F};
            const float amount = focus_of(drive.row->key);
            focus_glow(scene_, box, amount, true);
            ui::text(scene_, fonts_.regular, volume->label, x, baseline, 24.0F, volume->available ? kText : kSecondary);
            float cursor = x + fonts_.regular.measure(volume->label, 24.0F) + 16.0F;
            if (volume->capacity_known)
            {
                draw_usage_bar(*volume, cursor, baseline - 12.0F, 72.0F, 6.0F);
                cursor += 88.0F;
            }
            ui::text(scene_, fonts_.regular, drive.free, cursor, baseline, 22.0F,
                     volume->available && volume->writable ? kSecondary : kAmber);
            cursor += fonts_.regular.measure(drive.free, 22.0F) + 14.0F;
            scene_.line(cursor, baseline - 15.0F, cursor + 6.0F, baseline - 9.0F, 2.0F, kSecondary);
            scene_.line(cursor + 6.0F, baseline - 9.0F, cursor, baseline - 3.0F, 2.0F, kSecondary);
            focus_ring(scene_, box, amount, true);
            x += drive.width + 40.0F;
        }
        if (index < drives.size())
            ui::text(scene_, fonts_.regular, "+" + std::to_string(drives.size() - index), 1824.0F, baseline, 22.0F, kMuted,
                     gfx::Align::right);
    }

    // What the strip says after a drive's name: its free space, or why there is none to report.
    static std::string drive_free_words(const Volume &volume)
    {
        if (!volume.available)
            return "Disconnected";
        if (!volume.writable)
            return "No write access";
        if (!volume.capacity_known)
            return "Free space unknown";
        const std::uint64_t unused = volume.capacity_bytes - std::min(volume.used_bytes, volume.capacity_bytes);
        return short_size(unused - std::min(volume.reserved_bytes, unused)) + " free";
    }

    // A drive's bar: used space, then space reserved for queued downloads, over its capacity.
    void draw_usage_bar(const Volume &volume, float x, float y, float width, float height = 12.0F)
    {
        draw_usage_bar(scene_, volume, x, y, width, height);
    }
    void draw_usage_bar(gfx::DrawList &list, const Volume &volume, float x, float y, float width, float height)
    {
        const float fraction = volume.capacity_bytes == 0 ? 0.0F :
            static_cast<float>(volume.used_bytes) / static_cast<float>(volume.capacity_bytes);
        const float reserved = volume.capacity_bytes == 0 ? 0.0F :
            static_cast<float>(volume.reserved_bytes) / static_cast<float>(volume.capacity_bytes);
        list.rounded_rect({x, y, width, height}, height * 0.5F, kText.with_alpha(0.14F));
        list.rounded_rect({x, y, width * std::clamp(fraction, 0.0F, 1.0F), height}, height * 0.5F - 1.0F,
                          volume.available ? kAccent : kMuted);
        list.rounded_rect({x + width * std::clamp(fraction, 0.0F, 1.0F), y,
                           width * std::clamp(reserved, 0.0F, 1.0F - std::clamp(fraction, 0.0F, 1.0F)), height},
                          height * 0.5F - 1.0F, volume.available ? kAmber : kMuted);
    }

    // One drive's usage: its name and state, then a bar of used and reserved space with the
    // figures under it; without a space report only what Playden knows. `y` is the name's baseline.
    void draw_volume_usage(const Volume &volume, bool is_default, float x, float y, float width)
    {
        draw_volume_usage(scene_, volume, is_default, x, y, width);
    }
    // named false: a sheet that has the drive's name as its title shows only the state on that line.
    void draw_volume_usage(gfx::DrawList &list, const Volume &volume, bool is_default, float x, float y, float width,
                           bool named = true)
    {
        const float right = x + width;
        if (named)
            ui::text(list, fonts_.display, volume.label, x, y, 30.0F, kText);
        ui::text(list, fonts_.regular,
                 volume.available ? (volume.writable ? (is_default ? "Default · Available" : "Available") : "No write access")
                                  : "Disconnected",
                 named ? right : x, y, 22.0F, volume.available && volume.writable ? kSecondary : kAmber,
                 named ? gfx::Align::right : gfx::Align::left);
        if (!volume.capacity_known)
        {
            // A failed space query leaves capacity unknown: show only what Playden knows.
            ui::text(list, fonts_.regular, "Used by games  " + short_size(volume.games_bytes), x, y + 42.0F, 22.0F,
                     kSecondary);
            // Space held for queued downloads, only when there is some.
            if (volume.reserved_bytes > 0)
                ui::text(list, fonts_.regular, "Reserved by queue  " + short_size(volume.reserved_bytes), right,
                         y + 42.0F, 22.0F, kSecondary, gfx::Align::right);
            return;
        }
        const std::uint64_t unused = volume.capacity_bytes - std::min(volume.used_bytes, volume.capacity_bytes);
        const std::uint64_t free = unused - std::min(volume.reserved_bytes, unused);
        draw_usage_bar(list, volume, x, y + 28.0F, width, 12.0F);
        ui::text(list, fonts_.regular, "Capacity  " + short_size(volume.capacity_bytes), x, y + 76.0F, 22.0F, kSecondary);
        ui::text(list, fonts_.regular, "Used  " + short_size(volume.used_bytes), x, y + 106.0F, 22.0F, kSecondary);
        if (volume.reserved_bytes > 0)
            ui::text(list, fonts_.regular, "Reserved  " + short_size(volume.reserved_bytes), right, y + 76.0F, 22.0F,
                     kSecondary, gfx::Align::right);
        ui::text(list, fonts_.semibold, "Free  " + short_size(free), right, y + 106.0F, 22.0F,
                 volume.available ? kText : kMuted, gfx::Align::right);
    }

    // The macOS Settings screen: a rail of sections, then the focused section's rows, each
    // with its title and summary and a toggle or a button on the right.
    void draw_settings(ui::Canvas &, const ViewData &view, const State &state)
    {
        if (view.rows.empty())
            return;
        const int focused = std::clamp(view.focused_row, 0, static_cast<int>(view.rows.size()) - 1);
        const std::string &current = view.rows[static_cast<std::size_t>(focused)].section;
        std::vector<std::string> sections;
        for (const ViewRow &row : view.rows)
            if (sections.empty() || sections.back() != row.section)
                sections.push_back(row.section);
        const bool rail = state.navigation.settings_rail_focused;
        for (std::size_t index = 0; index < sections.size(); ++index)
        {
            const gfx::Rect entry{kSafeX, 150.0F + index * 66.0F, 300.0F, 60.0F};
            const bool active = sections[index] == current;
            const float amount = control_focus("section-" + sections[index], route_focus_ && rail && active);
            focus_glow(scene_, entry, amount, true);
            scene_.rounded_rect(entry, 8.0F, kBackground);
            if (active)
                scene_.rounded_rect(entry, 8.0F, kText.with_alpha(0.10F));
            focus_ring(scene_, entry, amount, true);
            ui::text(scene_, fonts_.display, sections[index], entry.x + 22.0F, entry.y + 40.0F, 28.0F,
                     active ? kText : kSecondary);
            if (route_focus_ && rail && active)
                stats_.focused_control_key = "section-" + sections[index];
        }

        constexpr float left = 444.0F;
        constexpr float width = 1380.0F;
        section_label(current, left, 172.0F);
        if (!view.subtitle.empty())
            ui::text(scene_, fonts_.regular, fit(fonts_.regular, view.subtitle, 22.0F, width), left + 300.0F, 172.0F,
                     22.0F, kAmber);
        // Rows of the section; long sections scroll to keep the focused row in view.
        std::vector<int> rows;
        for (int index = 0; index < static_cast<int>(view.rows.size()); ++index)
            if (view.rows[static_cast<std::size_t>(index)].section == current &&
                !view.rows[static_cast<std::size_t>(index)].rail_only)
                rows.push_back(index);
        // Games storage carries the default drive's usage under its title.
        const Volume *usage = nullptr;
        if (current == "Storage" && !state.volumes.empty())
        {
            const auto found = std::find_if(state.volumes.begin(), state.volumes.end(),
                                            [&state](const Volume &volume) { return volume.id == state.default_volume_id; });
            usage = found != state.volumes.end() ? &*found : &state.volumes.front();
        }
        const auto drive_row = [&](const ViewRow &row) { return usage != nullptr && row.key == "settings-storage"; };
        const auto height = [&](const ViewRow &row) {
            if (!row.qr.empty())
                return 300.0F;
            return drive_row(row) ? (usage->capacity_known ? 186.0F : 160.0F) : row.subtitle.empty() ? 92.0F : 118.0F;
        };
        float offset = 0.0F;
        {
            // Eased to the focused row like Home; with the focus on the rail the list stays where it was.
            float top = 0.0F, focus_min = -1.0F, focus_max = 0.0F;
            for (const int index : rows)
            {
                const float h = height(view.rows[static_cast<std::size_t>(index)]);
                if (index == focused)
                {
                    focus_min = top;
                    focus_max = top + h;
                }
                top += h + 24.0F;
            }
            Scroll &scroll = list_scrolls_["settings-" + current];
            const float content = std::max(0.0F, top - 24.0F);
            scroll.target = focus_min >= 0.0F ? reveal(scroll.target, focus_min, focus_max, 740.0F, content)
                                              : std::clamp(scroll.target, 0.0F, std::max(0.0F, content - 740.0F));
            offset = scroll.step(frame_dt_, state.reduced_motion);
        }
        scene_.push_clip({left - 24.0F, 190.0F, width + 48.0F, 790.0F});
        float y = 206.0F - offset;
        for (const int index : rows)
        {
            const ViewRow &row = view.rows[static_cast<std::size_t>(index)];
            const float h = height(row);
            const gfx::Rect card{left, y, width, h};
            // The title, summary and control sit in the row's usual height; the drive's bar goes below.
            const gfx::Rect head{left, y, width, drive_row(row) ? 118.0F : h};
            const bool is_focused = route_focus_ && !rail && index == focused;
            if (is_focused)
                stats_.focused_control_key = row.key;
            const float amount = control_focus(row.key, is_focused);
            focus_glow(scene_, card, amount);
            scene_.rounded_rect(card, 8.0F, kBackground);
            scene_.rounded_rect(card, 8.0F, kText.with_alpha(row.dangerous ? 0.0F : 0.04F));
            if (row.dangerous)
                scene_.bordered_rect(card, 8.0F, kBackground.with_alpha(0.0F), 2.0F, kRed.with_alpha(0.35F));
            if (!row.qr.empty())
            {
                // The QR code, then what to do, the address and what the page shows (Web access).
                if (row.qr != row_qr_url_)
                {
                    row_qr_url_ = row.qr;
                    row_qr_ = encode_qr(row.qr);
                }
                constexpr int qr_side = 240;
                if (!draw_qr_matrix(scene_, row_qr_, card.x + 30.0F, card.y + 30.0F, qr_side))
                    scene_.rounded_rect({card.x + 30.0F, card.y + 30.0F, static_cast<float>(qr_side), static_cast<float>(qr_side)},
                                        8.0F, kMuted.with_alpha(0.25F));
                const float text_x = card.x + 30.0F + qr_side + 40.0F;
                const float text_w = card.w - (text_x - card.x) - 30.0F;
                ui::text(scene_, fonts_.regular, fit(fonts_.regular, row.title, 24.0F, text_w), text_x, card.y + 96.0F, 24.0F,
                         kSecondary);
                ui::text(scene_, fonts_.semibold, fit(fonts_.semibold, row.value, 40.0F, text_w), text_x, card.y + 150.0F,
                         40.0F, kText);
                ui::paragraph(scene_, fonts_.regular, row.subtitle, text_x, card.y + 200.0F, 22.0F, text_w, 30.0F,
                              kSecondary, 2);
                focus_ring(scene_, card, amount);
                y += h + 24.0F;
                continue;
            }
            const gfx::Color title_ink = !row.enabled ? kMuted : row.dangerous ? kRed : kText;
            const float title_y = row.subtitle.empty() ? head.cy() + 11.0F : card.y + 50.0F;
            ui::text(scene_, fonts_.display, fit(fonts_.display, row.title, 30.0F, 760.0F), card.x + 30.0F, title_y,
                     30.0F, title_ink);
            if (!row.subtitle.empty())
                ui::text(scene_, fonts_.regular, fit(fonts_.regular, row.subtitle, 22.0F, 900.0F), card.x + 30.0F,
                         card.y + 88.0F, 22.0F, kSecondary);
            const float right = card.x + card.w - 30.0F;
            if (row.kind == RowKind::Toggle)
            {
                const gfx::Rect track{right - 84.0F, head.cy() - 22.0F, 84.0F, 44.0F};
                scene_.rounded_rect(track, 22.0F, row.selected ? kAccent : kText.with_alpha(0.15F));
                scene_.circle(row.selected ? track.x + track.w - 22.0F : track.x + 22.0F, track.cy(), 18.0F,
                              row.selected ? kBackground : kSecondary);
            }
            else if (!row.enabled || row.kind == RowKind::Status)
            {
                if (!row.value.empty())
                    ui::text(scene_, fonts_.display, row.value, right, head.cy() + 9.0F, 26.0F, kMuted,
                             gfx::Align::right);
            }
            else if (row.value.empty())
            {
                // Nothing to say: the chevron alone says another screen comes first.
                const float chevron = right - 8.0F;
                const gfx::Color ink = row.dangerous ? kRed : kText;
                scene_.line(chevron - 7.0F, head.cy() - 11.0F, chevron + 3.0F, head.cy(), 2.6F, ink);
                scene_.line(chevron + 3.0F, head.cy(), chevron - 7.0F, head.cy() + 11.0F, 2.6F, ink);
            }
            else
            {
                // "Manage ›" on macOS; the chevron is drawn, as the console fonts lack it.
                const std::string& label = row.value;
                const float pill = fonts_.display.measure(label, 26.0F) + 64.0F;
                const gfx::Rect button{right - pill, head.cy() - 28.0F, pill, 56.0F};
                const gfx::Color ink = row.dangerous ? kRed : kText;
                scene_.bordered_rect(button, 8.0F, kBackground.with_alpha(0.0F), 2.0F, ink.with_alpha(0.2F));
                ui::text(scene_, fonts_.display, label, button.x + 22.0F, button.cy() + 9.0F, 26.0F, ink);
                const float chevron = button.x + button.w - 26.0F;
                scene_.line(chevron - 3.0F, button.cy() - 6.0F, chevron + 2.5F, button.cy(), 2.2F, kText);
                scene_.line(chevron + 2.5F, button.cy(), chevron - 3.0F, button.cy() + 6.0F, 2.2F, kText);
            }
            if (drive_row(row))
            {
                const float bar_x = card.x + 30.0F, bar_w = card.w - 60.0F;
                float line_y = card.y + 132.0F;
                std::string line;
                if (usage->capacity_known)
                {
                    const std::uint64_t unused = usage->capacity_bytes - std::min(usage->used_bytes, usage->capacity_bytes);
                    const std::uint64_t free = unused - std::min(usage->reserved_bytes, unused);
                    draw_usage_bar(*usage, bar_x, card.y + 116.0F, bar_w);
                    line = short_size(usage->used_bytes) + " used · " +
                           (usage->reserved_bytes > 0 ? short_size(usage->reserved_bytes) + " reserved · " : std::string()) +
                           short_size(free) + " free of " + short_size(usage->capacity_bytes);
                    line_y = card.y + 162.0F;
                }
                else
                {
                    // Without a space report, only what Playden knows (as in Downloads).
                    line = short_size(usage->games_bytes) + " used by games" +
                           (usage->reserved_bytes > 0 ? " · " + short_size(usage->reserved_bytes) + " reserved" : std::string());
                }
                ui::text(scene_, fonts_.regular, line, bar_x, line_y, 22.0F, kSecondary);
                // A drive that is gone or read-only says so, as in Downloads.
                if (!usage->available || !usage->writable)
                    ui::text(scene_, fonts_.regular, usage->available ? "No write access" : "Disconnected", bar_x + bar_w,
                             line_y, 22.0F, kAmber, gfx::Align::right);
            }
            focus_ring(scene_, card, amount);
            y += h + 24.0F;
        }
        scene_.pop_clip();
    }

    // The live texture of a store's art (store_art.hpp); a game whose store has none gets 0, its title tile.
    std::uint32_t store_texture(GameId id, const std::string &url, std::uint32_t *width = nullptr,
                                std::uint32_t *height = nullptr)
    {
        return url.empty() || !live_artwork_ ? 0 : live_artwork_->TextureFor(id, url, width, height);
    }

    // The icon inside a prepared game's program (program_icon.hpp), for a game whose store has no cover
    // or whose cover failed to load; 0 otherwise, and while the cover may still come.
    std::uint32_t program_icon_texture(GameId id, const std::string &cover_url)
    {
        if (!live_artwork_ || current_state_ == nullptr) return 0;
        if (!cover_url.empty() && !live_artwork_->Failed(id, cover_url)) return 0;
        const auto game = current_state_->runtime_games.find(id);
        if (game == current_state_->runtime_games.end() || game->second.program_icon.empty()) return 0;
        return live_artwork_->TextureFor(id, game->second.program_icon);
    }

    // A label over its value, as the macOS details metadata.
    void detail_cell(std::string_view label, std::string_view value, float x, float y, float width,
                     gfx::Color tone = kText)
    {
        ui::text(scene_, fonts_.display, ui::upper(label), x, y + 14.0F, 17.0F, kSecondary, gfx::Align::left, 1.7F);
        const ui::FontRef &face = font_for(value, fonts_.semibold);
        ui::text(scene_, face, face.font->fit(value, 26.0F, width), x, y + 50.0F, 26.0F, tone);
    }

    // The macOS game page: the game's hero across the top with its logo, a row of actions,
    // then the description on the left and the facts on the right.
    // A ring of dots that turns (still with reduced motion).
    void spinner(float cx, float cy, float radius, gfx::Color color)
    {
        const bool still = current_state_ != nullptr && current_state_->reduced_motion;
        const float turn = still ? 0.0F : std::fmod(clock_ * 1.2F, 1.0F);
        for (int dot = 0; dot < 8; ++dot)
        {
            const float angle = static_cast<float>(dot) * 0.785398F;
            const float age = std::fmod(static_cast<float>(dot) / 8.0F - turn + 1.0F, 1.0F);
            scene_.circle(cx + std::sin(angle) * radius, cy - std::cos(angle) * radius, 2.2F,
                          color.with_alpha(0.25F + 0.75F * age));
        }
    }

    gfx::Color save_tone(std::string_view tone) const
    {
        return tone == "good" ? kGreen : tone == "busy" ? kBlue : tone == "attention" ? kAmber
             : tone == "problem" ? kRed : kMuted.with_alpha(0.9F);
    }

    // A game's picture for a Saves row or page: its cover when it is loaded, else its colour.
    void save_art(const State &state, GameId id, const gfx::Rect &rect)
    {
        // The cover when it is loaded; a small tile would wrap the poster's title letter by letter.
        if (const Game *game = game_by_id(state, id); game != nullptr && state.simulation_mode && id.steam_app())
        {
            const std::uint32_t number = id.steam_app().value_or(0);
            const std::size_t index = number >= 900001 ? (number - 900001) % art_.size() : number % art_.size();
            if (!game->art_failed && !game->art_pending && art_[index] != 0)
            {
                scene_.image(art_[index], rect, gfx::kFullUv, kText, 6.0F);
                return;
            }
        }
        if (live_artwork_ && !state.account().id.empty())
            if (const std::uint32_t cover = store_texture(id, CoverUrl(id)))
            {
                scene_.image(cover, rect, gfx::kFullUv, kText, 6.0F);
                return;
            }
        // No cover: the game's colour, with its program's icon when there is one.
        scene_.gradient_rect(rect, 6.0F, id_color(id), gfx::Color::rgb(0x24201D));
        if (const std::uint32_t icon = program_icon_texture(id, CoverUrl(id)))
        {
            const float side = std::min(rect.w, rect.h) * 0.72F;
            scene_.image(icon, {rect.cx() - side * 0.5F, rect.cy() - side * 0.5F, side, side}, gfx::kFullUv, kText, 0.0F);
        }
    }

    // The Saves tab: filter chips and Sync all over one list of games with saves on this console.
    void draw_saves(const ViewData &view, const State &state)
    {
        section_label("Game saves on this console", kSafeX, 160.0F);
        const bool busy = state.saves_job.kind != SavesJobKind::None;
        float summary_x = 1824.0F;
        if (busy)
        {
            const float width = fonts_.semibold.measure(view.subtitle, 22.0F);
            spinner(summary_x - width - 22.0F, 153.0F, 9.0F, kBlue);
        }
        ui::text(scene_, fonts_.semibold, fit(fonts_.semibold, view.subtitle, 22.0F, 1000.0F), summary_x, 161.0F, 22.0F,
                 kSecondary, gfx::Align::right);

        const auto focused_key = [&]() -> std::string {
            if (view.focused_row < 0 || static_cast<std::size_t>(view.focused_row) >= view.rows.size()) return {};
            return view.rows[static_cast<std::size_t>(view.focused_row)].key;
        }();
        const auto focus_of = [&](const ViewRow &row) {
            const bool focused = route_focus_ && focused_key == row.key;
            if (focused)
                stats_.focused_control_key = row.key;
            return control_focus(row.key, focused);
        };
        // Chips, then Upload all (when games wait for the account) and Sync all at the right.
        float x = kSafeX;
        float sync_left = 1824.0F;
        for (const ViewRow &row : view.rows)
            if (row.key == "saves-sync-all")
                sync_left = 1824.0F - fonts_.display.measure(row.title, 26.0F) - 74.0F;
        std::vector<const ViewRow *> games;
        for (const ViewRow &row : view.rows)
        {
            if (row.key.rfind("saves-filter-", 0) == 0)
            {
                const float label = fonts_.display.measure(row.title, 26.0F);
                const float count = fonts_.semibold.measure(row.value, 20.0F);
                const gfx::Rect chip{x, 186.0F, label + count + 54.0F, 48.0F};
                const float amount = focus_of(row);
                pill_focus_glow(chip, amount);
                if (row.selected)
                    scene_.rounded_rect(chip, 24.0F, kText);
                else
                    scene_.bordered_rect(chip, 24.0F, kText.with_alpha(0.0F), 1.5F, kText.with_alpha(0.25F));
                const gfx::Color ink = row.selected ? kBackground : kText;
                ui::text(scene_, fonts_.display, row.title, chip.x + 20.0F, chip.cy() + 9.0F, 26.0F, ink);
                ui::text(scene_, fonts_.semibold, row.value, chip.x + 20.0F + label + 12.0F, chip.cy() + 7.0F, 20.0F,
                         row.selected ? kBackground.with_alpha(0.65F) : kSecondary);
                pill_focus_ring(chip, amount);
                x += chip.w + 12.0F;
            }
            else if (row.key == "saves-upload-all")
            {
                const float label = fonts_.display.measure(row.title, 26.0F);
                const float count = fonts_.semibold.measure(row.value, 20.0F);
                const gfx::Rect button{sync_left - 12.0F - label - count - 86.0F, 186.0F, label + count + 86.0F, 48.0F};
                const float amount = focus_of(row);
                focus_glow(scene_, button, amount, true);
                scene_.bordered_rect(button, 8.0F, kText.with_alpha(0.06F), 1.5F, kText.with_alpha(0.5F));
                upload_icon(button.x + 26.0F, button.cy(), 18.0F, 2.0F, kText);
                ui::text(scene_, fonts_.display, row.title, button.x + 48.0F, button.cy() + 9.0F, 26.0F, kText);
                ui::text(scene_, fonts_.semibold, row.value, button.x + 48.0F + label + 12.0F, button.cy() + 7.0F, 20.0F,
                         kSecondary);
                focus_ring(scene_, button, amount, true);
            }
            else if (row.key == "saves-sync-all")
            {
                const float label = fonts_.display.measure(row.title, 26.0F);
                const gfx::Rect button{1824.0F - label - 74.0F, 186.0F, label + 74.0F, 48.0F};
                const float amount = focus_of(row);
                focus_glow(scene_, button, amount, true);
                if (!row.enabled)
                    scene_.push_opacity(0.45F);
                scene_.bordered_rect(button, 8.0F, kText.with_alpha(0.06F), 1.5F, (row.selected ? kBlue : kText).with_alpha(0.5F));
                if (row.selected)
                    spinner(button.x + 26.0F, button.cy(), 8.0F, kBlue);
                else
                    sync_icon(button.x + 26.0F, button.cy(), 9.0F, 2.0F, kText);
                ui::text(scene_, fonts_.display, row.title, button.x + 48.0F, button.cy() + 9.0F, 26.0F,
                         row.selected ? kBlue : kText);
                if (!row.enabled)
                    scene_.pop_opacity();
                focus_ring(scene_, button, amount, true);
                if (busy)
                {
                    scene_.rounded_rect({kSafeX, 250.0F, 1824.0F - kSafeX, 4.0F}, 2.0F, kText.with_alpha(0.1F));
                    scene_.rounded_rect({kSafeX, 250.0F, (1824.0F - kSafeX) * std::clamp(row.progress, 0.0F, 1.0F), 4.0F},
                                        2.0F, kBlue);
                }
            }
            else
                games.push_back(&row);
        }

        // The list: seven rows, scrolled to keep the focus in view.
        int focus_index = 0;
        for (std::size_t index = 0; index < games.size(); ++index)
            if (games[index]->key == focused_key)
                focus_index = static_cast<int>(index);
        constexpr float save_pitch = 100.0F, save_top = 272.0F, save_viewport = 7 * save_pitch;
        const float save_offset = list_scroll("saves", focus_index * save_pitch, focus_index * save_pitch + 90.0F, save_viewport,
                                              games.empty() ? 0.0F : games.size() * save_pitch - 10.0F, state.reduced_motion);
        scene_.push_clip({0.0F, save_top - 8.0F, kWidth, save_viewport + 4.0F});
        for (std::size_t index = 0; index < games.size(); ++index)
        {
            const float y = save_top + index * save_pitch - save_offset;
            if (y + save_pitch < save_top - 8.0F || y > save_top + save_viewport)
                continue;
            const ViewRow &row = *games[index];
            const gfx::Rect box{kSafeX, y, 1824.0F - kSafeX, 90.0F};
            if (row.kind == RowKind::Status)
            {
                ui::text(scene_, fonts_.regular, row.title, kSafeX + 8.0F, y + 50.0F, 26.0F, kSecondary);
                break;
            }
            const float amount = focus_of(row);
            focus_glow(scene_, box, amount, true);
            scene_.rounded_rect(box, 10.0F, kPanel);
            save_art(state, row.game_id, {box.x + 14.0F, y + 9.0F, 48.0F, 72.0F});
            const bool two_lines = !row.subtitle.empty();
            dynamic_text(row.title, box.x + 84.0F, y + (two_lines ? 40.0F : 56.0F), 32.0F, kText, 820.0F);
            if (two_lines)
                ui::text(scene_, fonts_.regular, fit(fonts_.regular, row.subtitle, 21.0F, 820.0F), box.x + 84.0F, y + 72.0F,
                         21.0F, kSecondary);
            const float status_x = 1040.0F;
            const gfx::Color tone = save_tone(row.tone);
            const bool moving = row.progress > 0.0F;
            if (row.tone == "busy" && row.value != "Will sync after play")
                spinner(status_x + 8.0F, y + (moving ? 38.0F : 45.0F), 8.0F, kBlue);
            else
                scene_.circle(status_x + 8.0F, y + (moving ? 38.0F : 45.0F), 7.0F, tone);
            ui::text(scene_, fonts_.semibold, row.value, status_x + 28.0F, y + (moving ? 46.0F : 53.0F), 24.0F, kText);
            if (moving)
            {
                scene_.rounded_rect({status_x + 28.0F, y + 60.0F, 200.0F, 5.0F}, 2.5F, kText.with_alpha(0.12F));
                scene_.rounded_rect({status_x + 28.0F, y + 60.0F, 200.0F * std::clamp(row.progress, 0.0F, 1.0F), 5.0F}, 2.5F, kBlue);
            }
            ui::text(scene_, fonts_.semibold, row.aside, 1490.0F, y + 53.0F, 24.0F, kText.with_alpha(0.85F), gfx::Align::right);
            ui::text(scene_, fonts_.regular, row.badge, box.x + box.w - 24.0F, y + 53.0F, 21.0F, kSecondary, gfx::Align::right);
            focus_ring(scene_, box, amount, true);
        }
        scene_.pop_clip();
    }

    // A button glyph as the hint row draws them ("L1", "R1"), standing alone.
    void glyph_pill(std::string_view label, float x, float cy)
    {
        const float width = std::max(30.0F, fonts_.semibold.measure(label, 15.0F) + 14.0F);
        scene_.rounded_rect({x, cy - 15.0F, width, 30.0F}, 15.0F, kText.with_alpha(0.10F));
        ui::text(scene_, fonts_.semibold, label, x + width * 0.5F, cy + 5.0F, 15.0F, kText, gfx::Align::center);
    }

    // Manage installed games, fullscreen: tabs switched with L1/R1, each with its toolbar, a table of
    // games and a footer with the selection and the tab's one action. Blue is Update, amber Move, red Uninstall.
    void draw_manage_games(const ViewData &view, const State &state)
    {
        const auto shown = state.navigation.manage_tab;
        const bool updates = shown == ManageTab::Updates;
        const bool moves = shown == ManageTab::Move;
        const gfx::Color tone = updates ? kBlue : moves ? kAmber : kRed;
        const auto focused_key = [&]() -> std::string {
            if (view.focused_row < 0 || static_cast<std::size_t>(view.focused_row) >= view.rows.size()) return {};
            return view.rows[static_cast<std::size_t>(view.focused_row)].key;
        }();
        const auto focus_of = [&](const ViewRow &row) {
            const bool focused = route_focus_ && focused_key == row.key;
            if (focused)
                stats_.focused_control_key = row.key;
            return control_focus(row.key, focused);
        };

        // The tabs, as the main bar draws them, between their shoulder buttons.
        constexpr float cy = 82.0F;
        float x = kSafeX;
        glyph_pill("L1", x, cy);
        x += 46.0F;
        for (const ViewRow &tab : view.tabs)
        {
            const float label = fonts_.display.measure(tab.title, 30.0F);
            const float badge = tab.value.empty() ? 0.0F : fonts_.semibold.measure(tab.value, 16.0F) + 30.0F;
            const gfx::Rect box{x, cy - 26.0F, 44.0F + label + badge, 52.0F};
            if (tab.selected)
                scene_.bordered_rect(box, 8.0F, kText.with_alpha(0.12F), 2.0F, kText.with_alpha(0.35F));
            ui::text(scene_, fonts_.display, tab.title, x + 22.0F, cy + 11.0F, 30.0F, tab.selected ? kText : kSecondary);
            if (!tab.value.empty())
            {
                const float badge_x = x + 22.0F + label + 12.0F;
                scene_.rounded_rect({badge_x, cy - 14.0F, badge - 12.0F, 28.0F}, 14.0F, kAccent);
                ui::text(scene_, fonts_.semibold, tab.value, badge_x + (badge - 12.0F) * 0.5F, cy + 6.0F, 16.0F, kBackground,
                         gfx::Align::center);
            }
            x += box.w + 8.0F;
        }
        glyph_pill("R1", x, cy);
        ui::text(scene_, fonts_.regular, fit(fonts_.regular, view.aside, 22.0F, 1824.0F - x - 80.0F), 1824.0F, 91.0F, 22.0F,
                 kSecondary, gfx::Align::right);

        ui::text(scene_, fonts_.display, view.title, kSafeX, 192.0F, 56.0F, kText);
        const ViewRow *check = nullptr;
        for (const ViewRow &row : view.rows)
            if (row.key == "manage-check")
                check = &row;
        if (check != nullptr && check->selected)
        {
            // A check is running: its progress, with a bar while Steam's answers come in.
            const float words = fonts_.regular.measure(view.subtitle, 23.0F);
            ui::text(scene_, fonts_.regular, view.subtitle, kSafeX, 230.0F, 23.0F, kSecondary);
            const float bar_x = kSafeX + words + 18.0F, bar_w = std::max(0.0F, 900.0F - words - 18.0F);
            scene_.rounded_rect({bar_x, 219.0F, bar_w, 6.0F}, 3.0F, kText.with_alpha(0.10F));
            if (check->progress > 0.0F)
                scene_.rounded_rect({bar_x, 219.0F, bar_w * check->progress, 6.0F}, 3.0F, kBlue);
        }
        else
            ui::text(scene_, fonts_.regular, fit(fonts_.regular, view.subtitle, 23.0F, 1728.0F), kSafeX, 230.0F, 23.0F,
                     kSecondary);

        // The toolbar: Move from and Move to (on Move), Drive (on Uninstall), Select all and Sort at the left, Check for updates at the right.
        float tool_x = kSafeX;
        std::vector<const ViewRow *> games, notes;
        const ViewRow *primary = nullptr;
        for (const ViewRow &row : view.rows)
        {
            if (row.key == "manage-move-from" || row.key == "manage-move-to" || row.key == "manage-uninstall-from" ||
                row.key == "manage-select-all" || row.key == "manage-sort" || row.key == "manage-check")
            {
                const bool picker = row.key == "manage-move-to";
                const float label = fonts_.regular.measure(row.title, 22.0F);
                const float value = row.value.empty() ? 0.0F : fonts_.regular.measure(row.value, 22.0F) + 12.0F;
                const float width = label + value + 44.0F + (picker ? 26.0F : 0.0F);
                const bool right = row.key == "manage-check";
                const gfx::Rect box{right ? 1824.0F - width : tool_x, 252.0F, width, 56.0F};
                if (!right)
                    tool_x += width + 12.0F;
                const float amount = focus_of(row);
                focus_glow(scene_, box, amount, true);
                scene_.rounded_rect(box, 8.0F, kBackground);
                scene_.bordered_rect(box, 8.0F, kText.with_alpha(0.05F), 2.0F,
                                     picker && row.enabled ? kAmber.with_alpha(0.55F) : kText.with_alpha(0.10F));
                const bool muted = !row.enabled;
                if (picker)
                {
                    // A chevron: it opens the drive picker.
                    const float cx = box.x + box.w - 30.0F, cy = box.cy();
                    scene_.line(cx - 2.0F, cy - 5.0F, cx + 3.0F, cy, 2.0F, kSecondary);
                    scene_.line(cx + 3.0F, cy, cx - 2.0F, cy + 5.0F, 2.0F, kSecondary);
                }
                if (row.value.empty())
                    ui::text(scene_, fonts_.regular, row.title, box.x + 22.0F, box.cy() + 8.0F, 22.0F, muted ? kMuted : kText);
                else
                {
                    ui::text(scene_, fonts_.semibold, row.title, box.x + 22.0F, box.cy() + 8.0F, 22.0F, kSecondary);
                    ui::text(scene_, fonts_.regular, row.value, box.x + 22.0F + label + 12.0F, box.cy() + 8.0F, 22.0F,
                             muted ? kMuted : kText);
                }
                focus_ring(scene_, box, amount, true);
            }
            else if (row.key.rfind("manage-toggle-", 0) == 0)
                games.push_back(&row);
            else if (row.key == "manage-primary")
                primary = &row;
            else
                notes.push_back(&row);
        }

        // The table: checkbox, cover, the game and its note, when it was last played, its size.
        constexpr float table_x = kSafeX, table_w = 1824.0F - kSafeX;
        constexpr float played_x = 1356.0F, size_right = 1800.0F;
        constexpr float name_x = table_x + 24.0F + 30.0F + 24.0F + 44.0F + 24.0F;
        const auto heading = [&](std::string_view label, float at, gfx::Align align) {
            ui::text(scene_, fonts_.semibold, label, at, 352.0F, 17.0F, kSecondary, align, 2.0F);
        };
        if (!games.empty())
        {
            heading("GAME", name_x, gfx::Align::left);
            heading("LAST PLAYED", played_x, gfx::Align::left);
            heading(updates ? "DOWNLOAD" : "SIZE", size_right, gfx::Align::right);
        }
        const std::string list_key = "manage-" + std::string(updates ? "updates" : moves ? "move" : "uninstall");
        int focus_index = 0;
        bool on_game = false;
        for (std::size_t index = 0; index < games.size(); ++index)
            if (games[index]->key == focused_key)
            {
                focus_index = static_cast<int>(index);
                on_game = true;
            }
        // On the footer button the list keeps showing the game the focus left (Up goes back to it); from the
        // toolbar it shows the top.
        if (on_game)
            manage_list_focus_[list_key] = focus_index;
        else if (const auto left = manage_list_focus_.find(list_key);
                 focused_key == "manage-primary" && left != manage_list_focus_.end() && !games.empty())
            focus_index = std::min(left->second, static_cast<int>(games.size()) - 1);
        constexpr float pitch = 84.0F, top = 366.0F, viewport = 872.0F - top;
        const float content = games.size() * pitch + (notes.empty() ? 0.0F : 80.0F);
        const float offset = list_scroll(list_key, focus_index * pitch, focus_index * pitch + 80.0F, viewport, content,
                                         state.reduced_motion);
        scene_.push_clip({0.0F, top - 8.0F, kWidth, viewport + 8.0F});
        for (std::size_t index = 0; index < games.size(); ++index)
        {
            const float y = top + index * pitch - offset;
            if (y + pitch < top - 8.0F || y > top + viewport)
                continue;
            const ViewRow &row = *games[index];
            const gfx::Rect box{table_x, y, table_w, 80.0F};
            const float amount = focus_of(row);
            focus_glow(scene_, box, amount, true);
            if (amount > 0.0F)
            {
                // The row covers the glow's middle, lifted a little.
                scene_.rounded_rect(box, 10.0F, kBackground);
                scene_.rounded_rect(box, 10.0F, kText.with_alpha(0.07F * amount));
            }
            if (!row.enabled)
                scene_.push_opacity(0.5F);
            const gfx::Rect tick{box.x + 24.0F, y + 25.0F, 30.0F, 30.0F};
            if (row.selected)
            {
                scene_.rounded_rect(tick, 7.0F, tone);
                scene_.line(tick.x + 7.0F, tick.y + 15.0F, tick.x + 12.0F, tick.y + 21.0F, 3.0F, kPanel);
                scene_.line(tick.x + 12.0F, tick.y + 21.0F, tick.x + 23.0F, tick.y + 9.0F, 3.0F, kPanel);
            }
            else
                scene_.bordered_rect(tick, 7.0F, kPanel.with_alpha(0.0F), 2.0F, kText.with_alpha(0.45F));
            save_art(state, row.game_id, {box.x + 78.0F, y + 7.0F, 44.0F, 66.0F});
            const ui::FontRef &face = font_for(row.title, fonts_.semibold);
            ui::text(scene_, face, face.font->fit(row.title, 26.0F, played_x - name_x - 32.0F), name_x, y + 37.0F, 26.0F, kText);
            const gfx::Color note = row.tone == "problem" ? kRed : row.tone == "update" ? kBlue : kSecondary;
            ui::text(scene_, fonts_.regular, fit(fonts_.regular, row.subtitle, 19.0F, played_x - name_x - 32.0F), name_x, y + 64.0F,
                     19.0F, note);
            ui::text(scene_, fonts_.regular, row.badge, played_x, y + 49.0F, 22.0F, kSecondary);
            ui::text(scene_, fonts_.regular, row.value, size_right, y + 49.0F, 24.0F, kText, gfx::Align::right);
            if (!row.enabled)
                scene_.pop_opacity();
            focus_ring(scene_, box, amount, true);
        }
        // A running check's note under the games, or the empty tab's words.
        for (const ViewRow *row : notes)
        {
            if (!games.empty() || row->key == "manage-more")
            {
                const float y = top + games.size() * pitch - offset;
                ui::text(scene_, fonts_.regular, row->title, table_x + 78.0F, y + 46.0F, 21.0F, kSecondary);
            }
            else
            {
                ui::text(scene_, fonts_.display, row->title, kSafeX, 420.0F, 40.0F, kText);
                if (!row->subtitle.empty())
                    ui::text(scene_, fonts_.regular, row->subtitle, kSafeX, 462.0F, 23.0F, kSecondary);
            }
        }
        scene_.pop_clip();

        // The footer: the selection's summary, and the tab's one action.
        scene_.rounded_rect({0.0F, 880.0F, kWidth, 1.0F}, 0.0F, kText.with_alpha(0.10F));
        float summary_w = 1728.0F;
        if (primary != nullptr)
        {
            const float width = fonts_.display.measure(primary->title, 27.0F) + 64.0F;
            const gfx::Rect button{1824.0F - width, 896.0F, width, 60.0F};
            summary_w -= width + 16.0F;
            const float amount = focus_of(*primary);
            focus_glow(scene_, button, amount);
            scene_.rounded_rect(button, 8.0F, kBackground);
            scene_.bordered_rect(button, 8.0F, kText.with_alpha(0.04F), 2.0F,
                                 primary->enabled ? tone.with_alpha(updates ? 0.65F : moves ? 0.60F : 0.55F) : kText.with_alpha(0.12F));
            ui::text(scene_, fonts_.display, primary->title, button.cx(), button.cy() + 10.0F, 27.0F,
                     !primary->enabled ? kMuted : updates || moves ? kText : kRed, gfx::Align::center);
            focus_ring(scene_, button, amount);
        }
        ui::text(scene_, fonts_.regular, fit(fonts_.regular, view.footer, 24.0F, summary_w), kSafeX, 935.0F, 24.0F, kSecondary);
    }

    // One game's saves: Steam Cloud and this console side by side, and what can be done.
    void draw_save_details(const ViewData &view, const State &state)
    {
        const GameId id = state.navigation.save_id;
        const auto found = state.cloud.find(id);
        const LiveCloudGame *cloud = found == state.cloud.end() ? nullptr : &found->second;
        const LiveKeptBottle *kept = nullptr;
        for (const auto &item : state.kept_bottles)
            if (item.game_id == id)
                kept = &item;
        const bool installed = std::any_of(state.installations.begin(), state.installations.end(),
                                           [id](const Installation &item) { return item.game_id == id; });
        scene_.gradient_rect({0.0F, 0.0F, kWidth, 380.0F}, 0.0F, id_color(id), kBackground);
        ui::draw_button(scene_, fonts_, ui::GlyphStyle::mono(kText, kBackground), ui::Button::circle, kSafeX, 74.0F, 36.0F);
        ui::text(scene_, fonts_.semibold, "Saves", kSafeX + 48.0F, 82.0F, 22.0F, kText);
        save_art(state, id, {kSafeX, 170.0F, 120.0F, 180.0F});
        dynamic_text(view.title, kSafeX + 150.0F, 262.0F, 64.0F, kText, 1500.0F);
        // The status, from the same row the Saves list shows.
        scene_.circle(kSafeX + 158.0F, 306.0F, 7.0F, save_tone(view.tone));
        ui::text(scene_, fonts_.semibold, view.subtitle, kSafeX + 176.0F, 314.0F, 26.0F, kText);

        // The comparison.
        const gfx::Color label = gfx::Color::rgb(0xD9D3CB);
        const gfx::Color newer = gfx::Color::rgb(0xF2C94C);
        const gfx::Color absent = gfx::Color::rgb(0x9A938B);
        const float left = kSafeX, cloud_x = kSafeX + 300.0F, here_x = kSafeX + 680.0F, right = kSafeX + 1060.0F;
        float y = 420.0F;
        ui::text(scene_, fonts_.display, ui::upper("Steam Cloud"), cloud_x, y, 22.0F, label, gfx::Align::left, 2.6F);
        ui::text(scene_, fonts_.display, ui::upper("This console"), here_x, y, 22.0F, label, gfx::Align::left, 2.6F);
        const bool has_cloud = cloud != nullptr && cloud->status != CloudStatus::Unsupported && cloud->status != CloudStatus::None;
        const bool conflict = cloud != nullptr && cloud->status == CloudStatus::Conflict;
        const auto when = [&](std::int64_t time) -> std::string {
            if (time <= 0) return "—";
            const std::time_t value = static_cast<std::time_t>(time);
            char text[32]{};
            const auto *found = std::localtime(&value);
            if (found == nullptr) return "—";
            const std::tm local = *found;  // localtime reuses one buffer
            const std::time_t now = std::time(nullptr);
            const auto *today = std::localtime(&now);
            // Another year shows the year instead of the time: "31 Aug 2024" next to "770 days ago" (#50).
            const char *format = today != nullptr && today->tm_year != local.tm_year ? "%d %b %Y" : "%d %b %H:%M";
            return std::strftime(text, sizeof(text), format, &local) ? text : "—";
        };
        const auto size = [](std::uint64_t bytes) {
            char text[32];
            if (bytes >= 1000ull * 1000) std::snprintf(text, sizeof(text), "%.1f MB", static_cast<double>(bytes) / 1e6);
            else std::snprintf(text, sizeof(text), "%.0f KB", static_cast<double>(bytes) / 1e3);
            return std::string(text);
        };
        const auto line = [&](const char *name, const std::string &remote, const std::string &local, gfx::Color remote_ink,
                              gfx::Color local_ink) {
            y += 58.0F;
            scene_.rounded_rect({left, y - 40.0F, right - left, 1.5F}, 0.0F, kText.with_alpha(0.14F));
            ui::text(scene_, fonts_.regular, name, left, y, 28.0F, label);
            ui::text(scene_, fonts_.semibold, remote, cloud_x, y, 28.0F, remote_ink);
            ui::text(scene_, fonts_.semibold, local, here_x, y, 28.0F, local_ink);
        };
        const std::uint64_t local_bytes = state.save_sizes.count(id) ? state.save_sizes.at(id) : 0;
        if (has_cloud)
        {
            line("Files", std::to_string(cloud->remote_files), std::to_string(cloud->local_files), kText, kText);
            line("Size", size(cloud->remote_bytes), size(cloud->local_bytes), kText, kText);
            const bool cloud_newer = cloud->remote_latest > cloud->local_latest;
            line("Newest change", when(static_cast<std::int64_t>(cloud->remote_latest)), when(static_cast<std::int64_t>(cloud->local_latest)),
                 conflict && cloud_newer ? newer : kText, conflict && !cloud_newer ? newer : kText);
            // Synced before Playden kept the time: unknown, not never.
            line("Last sync", cloud->last_sync > 0 ? when(cloud->last_sync) : cloud->first_sync ? "Never" : "—", "",
                 cloud->last_sync > 0 ? kText : absent, kText);
        }
        else
        {
            line("Size", "No Steam Cloud", local_bytes > 0 ? size(local_bytes) : "—", absent, kText);
        }
        if (!installed && kept != nullptr)
        {
            line("Other data", "—", short_size(kept->bytes) + " (Windows folder, caches)", absent, kText);
        }

        // What is going on, at the right.
        std::string note;
        const auto status = cloud == nullptr ? CloudStatus::None : cloud->status;
        if (cloud != nullptr && (status == CloudStatus::Checking || status == CloudStatus::Syncing || cloud->queued))
            note = "Playden is checking this game with Steam Cloud. Its actions come back when that is done; you can leave this page meanwhile.";
        else if (cloud != nullptr && cloud->after_play)
            note = "The game may be running. Playden syncs its saves once it stops.";
        else if (status == CloudStatus::Conflict)
            note = cloud->first_sync ? "Never synced on this PS5, and both sides already have saves. Pick which to keep; the other side is backed up first."
                   : cloud->first_overwrite
                       ? "Saves here would replace the ones in Steam Cloud, and this PS5 hasn't uploaded any for this game before. If the game didn't show your progress, keep the Cloud saves. Either side is backed up first."
                       : "Both changed since the last sync. Pick which saves to keep; saves replaced here are backed up first.";
        else if (status == CloudStatus::NeedsAccount)
            note = "Saves here were never synced with this Steam account. Confirm to upload them.";
        else if (status == CloudStatus::Failed)
            note = cloud->message + " Playden tries again on the next check.";
        else if (status == CloudStatus::Synced || status == CloudStatus::UpToDate)
            note = "Matches Steam Cloud.";
        else if (status == CloudStatus::Unsupported)
            note = "This game doesn't use Steam Cloud. Its saves exist only on this PS5.";
        else if (status == CloudStatus::Waiting)
            note = installed ? cloud->message + ". Steam Cloud has saves this console doesn't; Sync now or Play brings them down."
                             : "Steam Cloud has newer saves. They come down when you reinstall the game or press Sync now.";
        else
            note = "Not checked yet. Opening Saves or Sync now checks it.";
        if (!installed)
            note = "The game was uninstalled; its saves and settings were kept. " + note;
        ui::text(scene_, fonts_.display, ui::upper("What's going on"), 1240.0F, 420.0F, 22.0F, label, gfx::Align::left, 2.6F);
        ui::paragraph(scene_, fonts_.regular, note, 1240.0F, 470.0F, 25.0F, 584.0F, 36.0F, kText.with_alpha(0.9F), 6);

        // Actions in a row.
        float x = kSafeX;
        for (const ViewRow &row : view.rows)
        {
            if (row.kind != RowKind::Action)
                continue;
            const float height = 60.0F;
            const float width = fonts_.display.measure(row.title, 26.0F) +
                                (row.value.empty() ? 0.0F : fonts_.semibold.measure(row.value, 22.0F) + 18.0F) + 56.0F;
            const bool focused = view.focused_row >= 0 && static_cast<std::size_t>(view.focused_row) < view.rows.size() &&
                                 view.rows[static_cast<std::size_t>(view.focused_row)].key == row.key;
            if (focused && route_focus_)
                stats_.focused_control_key = row.key;
            const float amount = control_focus(row.key, focused && route_focus_);
            const gfx::Rect button{x, 860.0F - height * 0.5F, width, height};
            const bool primary = &row == &view.rows.front() && !row.dangerous;
            if (row.dangerous)
            {
                scene_.push_transform(1.0F + 0.04F * amount, button.cx(), button.cy(), 0.0F, 0.0F);
                focus_glow(scene_, button, amount);
                if (!row.enabled)
                    scene_.push_opacity(0.45F);
                scene_.bordered_rect(button, 8.0F, kRed.with_alpha(0.08F), 2.0F, kRed.with_alpha(0.55F));
                const float content = fonts_.display.measure(row.title, 26.0F) +
                                      (row.value.empty() ? 0.0F : fonts_.semibold.measure(row.value, 22.0F) + 18.0F);
                const float text_x = button.cx() - content * 0.5F;
                ui::text(scene_, fonts_.display, row.title, text_x, button.cy() + 9.0F, 26.0F, gfx::Color::rgb(0xF1B0AA));
                if (!row.value.empty())
                    ui::text(scene_, fonts_.semibold, row.value, text_x + content, button.cy() + 8.0F, 22.0F, kSecondary,
                             gfx::Align::right);
                if (!row.enabled)
                    scene_.pop_opacity();
                focus_ring(scene_, button, amount);
                scene_.pop_transform();
            }
            else
                mac_button(scene_, button, row.title, primary, amount, !row.enabled, row.value);
            x += width + 26.0F;
        }
    }

    void draw_details(ui::Canvas &, const ViewData &view, const State &state)
    {
        const GameId id = state.navigation.detail_id.value_or(state.navigation.selected_game_id);
        const Game *game = game_by_id(state, id);
        const gfx::Rect hero{0.0F, 0.0F, kWidth, 620.0F};
        const bool live_art = !state.simulation_mode && live_artwork_ && !state.account().id.empty() && id.valid();
        const std::uint32_t hero_texture = state.simulation_mode ? hero_ :
            live_art ? store_texture(id, HeroUrl(id)) : 0;
        if (hero_texture != 0)
            scene_.image(hero_texture, hero, gfx::kFullUv, kText);
        else
            scene_.gradient_rect(hero, 0.0F, id_color(id), kBackground);
        scene_.gradient_rect({0.0F, 0.0F, kWidth, 186.0F}, 0.0F, kBackground.with_alpha(0.4F), kBackground.with_alpha(0.0F));
        scene_.gradient_rect({0.0F, 465.0F, kWidth, 156.0F}, 0.0F, kBackground.with_alpha(0.0F), kBackground);

        // Where Circle returns, over the hero.
        const Route origin = state.navigation.detail_origin;
        const char *back = origin == Route::Home ? "Home" : origin == Route::Downloads ? "Queue" :
                           origin == Route::Settings ? "Settings" : "Library";
        ui::draw_button(scene_, fonts_, ui::GlyphStyle::mono(kText, kBackground), ui::Button::circle, kSafeX, 74.0F, 36.0F);
        ui::text(scene_, fonts_.semibold, back, kSafeX + 48.0F, 82.0F, 22.0F, kText);

        const std::string title = game == nullptr ? view.title : game->title;
        std::uint32_t logo_width = 0;
        std::uint32_t logo_height = 0;
        const std::uint32_t logo = live_art ?
            store_texture(id, LogoUrl(id), &logo_width, &logo_height) : 0;
        if (logo != 0 && logo_width > 0 && logo_height > 0)
        {
            const float fit_scale = std::min(460.0F / static_cast<float>(logo_width),
                                             160.0F / static_cast<float>(logo_height));
            const float w = static_cast<float>(logo_width) * fit_scale;
            const float h = static_cast<float>(logo_height) * fit_scale;
            const gfx::Rect box{kSafeX, 430.0F + (160.0F - h) * 0.5F, w, h};
            scene_.shadow(box.inset(10.0F), 20.0F, 30.0F, gfx::Color::rgb(0x000000, 0.35F));
            scene_.image(logo, box, gfx::kFullUv, kText);
        }
        else
        {
            // Without a logo the title stands in, as the macOS artwork fallback does.
            const ui::FontRef &title_face = font_for(title, fonts_.display);
            const bool long_title = title_face.font->measure(title, 72.0F) > 900.0F;
            ui::paragraph(scene_, title_face, title, kSafeX, long_title ? 520.0F : 586.0F, 72.0F, 900.0F, 70.0F,
                          kText, 2);
        }

        // Actions in a row, all one size; Install, Play or a running download is the primary
        // one, filled with the accent whatever its state.
        float x = kSafeX;
        for (const ViewRow &row : view.rows)
        {
            if (row.kind != RowKind::Action && row.kind != RowKind::Toggle)
                continue;
            const bool primary = row.key == "details-install" || row.key == "details-play" ||
                                 row.key == "details-download";
            const bool favorite = row.key == "details-favorite";
            const std::string label = favorite ? (row.selected ? "Favorited" : "Favorite") : row.title;
            // A download shows its percentage from the start ("0%" before the first bytes), and Play
            // while it prepares a known amount; other job states read as words ("Queued", "Paused").
            const bool percent = (row.key == "details-download" && (row.progress > 0.0F || row.value == "downloading")) ||
                                 (row.key == "details-play" && row.progress > 0.0F);
            std::string detail = percent ? std::to_string(static_cast<int>(row.progress * 100.0F)) + "%" : row.value;
            if (row.key == "details-download" && !percent && !detail.empty())
                detail[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(detail[0])));
            const float height = 60.0F;
            // Favorite is a square icon button: a star, filled while the game is a favorite.
            // A percentage is sized for "100%" so the button keeps its width as it counts up, and
            // Install for the largest size it shows, so it keeps its width once Steam gives the size.
            const auto wider = [this](const std::string &a, const std::string &b) {
                return fonts_.semibold.measure(a, 22.0F) >= fonts_.semibold.measure(b, 22.0F) ? a : b;
            };
            const std::string widest = percent ? std::string("100%")
                                     : row.key == "details-install" ? wider(detail, "999.9 GB") : detail;
            const float width = favorite ? height :
                fonts_.display.measure(label, 26.0F) +
                    (widest.empty() ? 0.0F : fonts_.semibold.measure(widest, 22.0F) + 18.0F) + 56.0F;
            const bool focused = view.focused_row >= 0 && static_cast<std::size_t>(view.focused_row) < view.rows.size() &&
                                 view.rows[static_cast<std::size_t>(view.focused_row)].key == row.key;
            if (focused && route_focus_)
                stats_.focused_control_key = row.key;
            const float amount = control_focus(row.key, focused && route_focus_);
            const gfx::Rect button{x, 682.0F - height * 0.5F, width, height};
            if ((row.key == "details-play" || row.key == "details-download") && row.progress > 0.0F)
            {
                // Downloading, preparing or removing with a known fraction: the button is not
                // dimmed; it is outlined and the accent fills it from the left as the work advances.
                scene_.push_transform(1.0F + 0.04F * amount, button.cx(), button.cy(), 0.0F, 0.0F);
                focus_glow(scene_, button, amount);
                scene_.rounded_rect(button, 8.0F, kAccent.with_alpha(0.14F));
                scene_.rounded_rect({button.x, button.y, button.w * std::clamp(row.progress, 0.0F, 1.0F), button.h}, 8.0F,
                                    kAccent.with_alpha(0.55F));
                scene_.bordered_rect(button, 8.0F, kBackground.with_alpha(0.0F), 2.0F, kAccent.with_alpha(0.7F));
                const float detail_width = widest.empty() ? 0.0F : fonts_.semibold.measure(widest, 22.0F) + 18.0F;
                const float content = std::min(button.w - 32.0F, fonts_.display.measure(label, 26.0F) + detail_width);
                const float text_x = button.cx() - content * 0.5F;
                ui::text(scene_, fonts_.display, label, text_x, button.cy() + 26.0F * 0.36F, 26.0F, kText);
                if (!detail.empty())
                    ui::text(scene_, fonts_.semibold, detail, text_x + content, button.cy() + 8.0F, 22.0F,
                             kText.with_alpha(0.8F), gfx::Align::right);
                focus_ring(scene_, button, amount);
                scene_.pop_transform();
            }
            else if (favorite)
            {
                mac_button(scene_, button, "", false, amount, !row.enabled);
                scene_.push_transform(1.0F + 0.04F * amount, button.cx(), button.cy(), 0.0F, 0.0F);
                if (row.selected)
                    scene_.star(button.cx(), button.cy() + 1.0F, 16.0F, kAccent);
                else
                    scene_.star(button.cx(), button.cy() + 1.0F, 16.0F, kText, 2.5F);
                scene_.pop_transform();
            }
            else
                mac_button(scene_, button, label, primary, amount, !row.enabled, detail);
            // Wide enough that a focused button's ring (9 px out, 4 px stroke, 4% larger) clears its neighbours.
            x += width + 26.0F;
        }

        // The setup hint belongs to the actions, with a smaller face and a coloured dot.
        // Leave a clear gap before the description; ready games have no hint.
        float y = 774.0F;
        for (const ViewRow &row : view.rows)
        {
            if (row.key != "details-hint" || row.title.empty())
                continue;
            scene_.circle(kSafeX + 6.0F, 752.0F, 6.0F, save_tone(row.tone));
            const ui::FontRef &face = font_for(row.title, fonts_.semibold);
            ui::text(scene_, face, face.font->fit(row.title, 23.0F, 1100.0F), kSafeX + 26.0F, 760.0F,
                     23.0F, kText);
            y += 52.0F;
            break;
        }
        // Non-installed games retain their download, destination and kept-save notes; an imported
        // game says where it plays from (the fixture's too).
        const bool folder_note = std::any_of(view.rows.begin(), view.rows.end(), [](const ViewRow &row) {
            return row.key == "details-folder" || row.key == "details-update-note" || row.key == "details-updating";
        });
        if (!state.simulation_mode || folder_note)
        {
            std::string note;
            gfx::Color tone = kSecondary;
            for (const ViewRow &row : view.rows)
            {
                if (row.key.rfind("details-", 0) != 0 || row.key == "details-kept" ||
                    row.key == "details-hint" || row.key == "details-setup")
                    continue;
                if (row.kind == RowKind::Status)
                {
                    note = row.title;
                    if (!row.value.empty())
                        note += " · " + row.value;
                    if (!row.subtitle.empty())
                        note += " · " + row.subtitle;
                    tone = row.title == "Storage disconnected" ? kAmber
                         : row.key == "details-update-note" || row.key == "details-updating" ? kAccent : kSecondary;
                    break;
                }
                if (row.key == "details-install" && !row.subtitle.empty())
                {
                    note = row.subtitle.rfind("To ", 0) == 0 ? "Installs to " + row.subtitle.substr(3) : row.subtitle;
                    break;
                }
                // The Download button already shows the state, or the percentage while it downloads;
                // only a state hidden behind a percentage (paused, verifying) is said here.
                if (row.key == "details-download")
                {
                    if (row.progress > 0.0F && row.value != "downloading" && row.value != "copying")
                        note = row.title + " " + row.value;  // "Download paused", "Copying paused"
                    break;
                }
            }
            if (!note.empty())
            {
                ui::text(scene_, fonts_.semibold, fit(fonts_.semibold, note, 22.0F, 1128.0F), kSafeX, y + 4.0F, 22.0F,
                         tone);
                y += 42.0F;
            }
            // Saves kept from a previous install, under the install note.
            for (const ViewRow &row : view.rows)
            {
                if (row.key != "details-kept")
                    continue;
                std::string kept_note = row.title;
                if (!row.value.empty())
                    kept_note += " · " + row.value;
                if (!row.subtitle.empty())
                    kept_note += " · " + row.subtitle;
                ui::text(scene_, fonts_.semibold, fit(fonts_.semibold, kept_note, 22.0F, 1128.0F), kSafeX, y + 4.0F,
                         22.0F, kSecondary);
                y += 42.0F;
            }
        }
        const std::string description = game == nullptr || game->description.empty() ?
                                            "Description unavailable." : game->description;
        const auto detail_scroll = state.navigation.scroll_offsets.find("details-content");
        // Keep description pagination aligned with the model and above the footer hints.
        constexpr float column_bottom = 975.0F;
        const int description_lines = state.simulation_mode ? 3 : 2;
        const std::size_t page_size = DetailsDescriptionPage(state.simulation_mode);
        const int requested_page = detail_scroll == state.navigation.scroll_offsets.end() ? 0 :
                                       std::max(0, detail_scroll->second);
        const int max_page = description.empty() ? 0 :
                                 static_cast<int>((description.size() - 1) / page_size);
        const int page = std::min(requested_page, max_page);
        const std::size_t offset = utf8_boundary(description, static_cast<std::size_t>(page) * page_size);
        const std::size_t end = utf8_boundary(description, std::min(description.size(), offset + page_size));
        std::string page_text = description.substr(offset, end - offset);
        if (offset > 0)
            page_text.insert(0, "…");
        if (offset + page_size < description.size())
            page_text += "…";
        const ui::FontRef &body = font_for(page_text, fonts_.regular);
        const float text_bottom = ui::paragraph(scene_, body, page_text, kSafeX, y + 20.0F, 26.0F, 1128.0F, 37.0F,
                                                gfx::Color::rgb(0xD6D0C8), description_lines);
        y = std::max(y + 20.0F + 37.0F, text_bottom) + 18.0F;
        if (description.size() > page_size)
        {
            ui::text(scene_, fonts_.regular, "L2/R2 Read more · page " + std::to_string(page + 1) + " of " +
                         std::to_string(max_page + 1), kSafeX, y + 6.0F, 18.0F, kMuted);
            y += 30.0F;
        }
        if (game != nullptr && y + 42.0F <= column_bottom)
        {
            float tag_x = kSafeX;
            for (std::size_t index = 0; index < game->genres.size() && index < 4; ++index)
            {
                const std::string &genre = game->genres[index];
                const float width = fonts_.semibold.measure(genre, 20.0F) + 32.0F;
                if (tag_x + width > kSafeX + 1128.0F)
                    break;
                scene_.rounded_rect({tag_x, y, width, 42.0F}, 6.0F, kText.with_alpha(0.1F));
                ui::text(scene_, fonts_.semibold, genre, tag_x + 16.0F, y + 28.0F, 20.0F, kText);
                tag_x += width + 12.0F;
            }
        }

        // Right column: the facts.
        if (game == nullptr)
            return;
        const Installation *installation = nullptr;
        const auto found_installation = std::find_if(
            state.installations.begin(), state.installations.end(), [&game, &state](const Installation &item) {
                return item.game_id == game->id && (item.installed_bytes > 0 || !state.simulation_mode);
            });
        if (found_installation != state.installations.end())
            installation = &*found_installation;
        std::string location = "Not installed";
        // An imported game playing from its folder: the drive it is on, as the Import sheet names it.
        const auto external = std::find_if(state.external_games.begin(), state.external_games.end(),
                                           [&game](const LiveExternalGame &item) { return item.game_id == game->id; });
        if (installation == nullptr && external != state.external_games.end())
        {
            static const std::map<std::string, std::string> drives = {
                {"usb0", "USB drive"}, {"usb1", "USB drive 2"}, {"m2", "M.2 SSD"}, {"extended", "Extended storage"},
                {"internal", "Console drop folder"}};
            const auto drive = drives.find(external->volume_id);
            location = drive == drives.end() ? external->volume_id : drive->second;
            if (!external->available)
                location += " · Disconnected";
        }
        if (installation != nullptr)
        {
            const auto volume = std::find_if(state.volumes.begin(), state.volumes.end(),
                                             [installation](const Volume &item) {
                                                 return item.id == installation->volume_id;
                                             });
            location = volume == state.volumes.end() ? installation->volume_id : volume->label;
            if (!installation->available || (volume != state.volumes.end() && !volume->available))
                location += " · Disconnected";
        }
        const auto rating = state.library.ratings.find(game->id);
        const Compatibility compatibility = rating == state.library.ratings.end() ?
                                                Compatibility::Untested : rating->second;
        const bool installed = installation != nullptr && installation->installed_bytes > 0;
        // One size for a game: what it takes on the drive (installed, or the install estimate).
        std::optional<std::uint64_t> scanned_bytes;  // an imported game playing in place: its folder's size
        for (const auto &candidate : state.import_candidates)
            if (candidate.imported == game->id && candidate.total_bytes > 0)
                scanned_bytes = candidate.total_bytes;
        const std::string size = installed ? short_size(installation->installed_bytes) :
            game->install_bytes ? short_size(*game->install_bytes) :
            scanned_bytes ? short_size(*scanned_bytes) : std::string("Unknown");
        const std::string playtime = game->playtime_minutes && *game->playtime_minutes > 0 ?
            playtime_label(game->playtime_minutes) : std::string("Never played");
        std::string compat = rating_label(compatibility);
        if (const auto dot = compat.find(" · "); dot != std::string::npos)
            compat.resize(dot);
        std::string controller = game->controller_support.empty() ? "Unknown" : game->controller_support;
        controller.front() = static_cast<char>(std::toupper(static_cast<unsigned char>(controller.front())));
        constexpr float left = 1304.0F;
        constexpr float right = 1584.0F;
        const auto setup = std::find_if(view.rows.begin(), view.rows.end(), [](const ViewRow &row) {
            return row.key == "details-setup";
        });
        // Progress reads "Step 3 of 6 · Copying …": the step on one line, what it does below it.
        std::string setup_step, setup_detail;
        if (setup != view.rows.end()) {
            setup_step = setup->value;
            if (const auto dot = setup_step.find(" · "); dot != std::string::npos) {
                setup_detail = setup_step.substr(dot + std::string(" · ").size());
                setup_step.resize(dot);
            }
        }
        const float facts_top = setup == view.rows.end() ? 754.0F : setup_detail.empty() ? 694.0F : 662.0F;
        detail_cell("Playtime", playtime, left, facts_top, 240.0F);
        detail_cell("Size", size, right, facts_top, 240.0F);
        // A game a Steam family member shares comes from Steam Families, not the account's own library.
        const std::string store = game->id.store == Store::kLocal ? std::string("Imported") :
                                  game->family_shared ? std::string("Steam Family") :
                                  game->source.empty() ? std::string("Steam") : game->source;
        detail_cell("Store", store, left, facts_top + 76.0F, 240.0F);
        detail_cell("Location", location, right, facts_top + 76.0F, 240.0F);
        detail_cell("Compatibility", compat, left, facts_top + 152.0F, 240.0F);
        if (game->last_played && setup == view.rows.end())
            detail_cell("Last played", relative_time(game->last_played, state.clock_ticks), right,
                        facts_top + 152.0F, 240.0F);
        else
            detail_cell("Controller", controller, right, facts_top + 152.0F, 240.0F);
        if (setup != view.rows.end()) {
            const gfx::Color tone = setup->tone.empty() || setup->tone == "quiet" ? kText : save_tone(setup->tone);
            detail_cell("Setup", setup_step, left, facts_top + 228.0F, 520.0F, tone);
            if (!setup_detail.empty()) {
                const ui::FontRef &face = font_for(setup_detail, fonts_.regular);
                ui::text(scene_, face, face.font->fit(setup_detail, 24.0F, 520.0F), left, facts_top + 228.0F + 84.0F,
                         24.0F, kSecondary);
            }
        }
    }

    void draw_session(ui::Canvas &, const ViewData &view, const State &state)
    {
        if (hero_ != 0)
            scene_.image(hero_, {0.0F, 0.0F, 1920.0F, 1080.0F}, gfx::kFullUv,
                         kText.with_alpha(0.42F));
        scene_.rounded_rect({0.0F, 0.0F, 1920.0F, 1080.0F}, 0.0F, kBackground.with_alpha(0.68F));
        const Game *game = game_by_id(state, state.session_game_id);
        section_label("Simulated game session", 96.0F, 82.0F);
        dynamic_text(game == nullptr ? std::string_view(view.title) : std::string_view(game->title),
                     96.0F, 256.0F, 64.0F, kText, 1000.0F);
        ui::text(scene_, fonts_.display, view.subtitle, 96.0F, 320.0F, 34.0F,
                 state.session == SessionState::Failed || state.session == SessionState::Crashed ? kRed : kSecondary);
        draw_actions(view, 96.0F, 428.0F, 540.0F);

        scene_.rounded_rect({1210.0F, 226.0F, 614.0F, 540.0F}, 16.0F, kPanel.with_alpha(0.94F));
        ui::text(scene_, fonts_.display, "Session controls", 1254.0F, 294.0F, 44.0F, kText);
        ui::paragraph(scene_, fonts_.regular,
                      state.session_error.empty() ? "This is a deterministic fixture. No game process is launched."
                                                  : state.session_error,
                      1254.0F, 352.0F, 25.0F, 522.0F, 36.0F,
                      state.session_error.empty() ? kSecondary : kAmber, 5);
    }

    void draw_persistent_status(ui::Canvas &, const State &state)
    {
        float y = 126.0F;
        for (const Notice &notice : state.notices)
        {
            if (!notice.persistent)
                continue;
            const float width = 560.0F;
            scene_.rounded_rect({kWidth - kSafeX - width, y, width, 54.0F}, 8.0F,
                                kAmber.with_alpha(0.16F));
            scene_.circle(kWidth - kSafeX - width + 25.0F, y + 27.0F, 6.0F, kAmber);
            ui::text(scene_, fonts_.regular, fit(fonts_.regular, notice.message, 21.0F, width - 64.0F),
                     kWidth - kSafeX - width + 44.0F, y + 35.0F, 21.0F, kText);
            y += 64.0F;
        }
    }

    // The hint row for the view's legends. `shoulder` adds L1/R1 (Sections unless a legend gives
    // them another job, "L1/R1 Jump").
    std::vector<ui::Hint> legend_hints(const ViewData &view, std::string &shoulder, bool with_shoulder)
    {
        const std::vector<std::string> &labels = view.legends;
        std::vector<ui::Hint> hints;
        hints.reserve(std::max<std::size_t>(labels.size(), 2) + 1);
        shoulder = "Sections";
        if (labels.empty())
        {
            hints.push_back({ui::Button::cross, "Select"});
            hints.push_back({ui::Button::circle, "Back"});
        }
        else
        {
            for (std::size_t index = 0; index < labels.size() && hints.size() < 5; ++index)
            {
                if (labels[index].find("Sections") != std::string::npos)
                    continue;
                if (labels[index].rfind("L1/R1 ", 0) == 0)
                {
                    shoulder = labels[index].substr(6);
                    continue;
                }
                hints.push_back({legend_button(labels[index], index), legend_text(labels[index])});
            }
            // Circle is already spoken for by "Circle …" legends ("Circle Clear search") and Back/Cancel ones.
            const bool has_back = std::any_of(labels.begin(), labels.end(), [](const std::string &label) {
                return label.rfind("Circle", 0) == 0 || label.find("Back") != std::string::npos ||
                       label.find("Cancel") != std::string::npos;
            });
            if (!has_back)
                hints.push_back({ui::Button::circle, "Back"});
        }
        if (with_shoulder)
            hints.push_back({ui::Button::l1, shoulder.c_str(), ui::Button::r1});
        return hints;
    }

    void draw_hint_row(gfx::DrawList &list, const std::vector<ui::Hint> &hints, float x)
    {
        ui::HintLayout layout;
        layout.size = 38.0F;
        layout.text_size = 22.0F;
        layout.cy = kFooterY + 20.0F;
        layout.item_gap = 28.0F;
        layout.font = &fonts_.semibold;
        ui::GlyphStyle glyphs = ui::GlyphStyle::mono(kText, kBackground);
        ui::draw_hints(list, fonts_, glyphs, hints.data(), static_cast<int>(hints.size()), x, false, layout);
    }

    void draw_footer(ui::Canvas &, const ViewData &view)
    {
        // "Root access is missing" has one action: no Back or Sections.
        if (current_state_ && current_state_->root_access_missing)
        {
            std::vector<ui::Hint> hints;
            for (std::size_t index = 0; index < view.legends.size(); ++index)
                hints.push_back({legend_button(view.legends[index], index), legend_text(view.legends[index])});
            draw_hint_row(scene_, hints, 96.0F);
            return;
        }
        std::string shoulder;
        draw_hint_row(scene_, legend_hints(view, shoulder, true), 96.0F);
        if (view.show_simulation_label)
        {
            scene_.rounded_rect({1652.0F, 989.0F, 172.0F, 34.0F}, 17.0F, kText.with_alpha(0.08F));
            ui::text(scene_, fonts_.semibold, "UI SIMULATION", 1738.0F, 1012.0F, 16.0F, kSecondary,
                     gfx::Align::center, 1.2F);
        }
    }

    void draw_overlays(const State &state, const ViewData &view)
    {
        ui::Canvas overlay{overlay_, fonts_, renderer_.glass_texture(), clock_};
        if (state.editor.active)
        {
            // The PS5's own keyboard is on screen instead.
            if (!state.editor.system_keyboard)
                draw_editor_overlay(overlay, state);
            return;
        }
        if (!state.navigation.modals.empty())
        {
            const ModalEntry &modal = state.navigation.modals.back();
            if (modal.destructive || modal.id == "confirmation")
                draw_modal_overlay(modal, view);
            else
                draw_sheet_overlay(view);
            return;
        }
        if (view.surface != "home" && view.surface != "library" && view.surface != "downloads" &&
            view.surface != "settings" && view.surface != "details" && view.surface != "qr" &&
            view.surface != "startup" && view.surface != "session" && view.surface != "saves" &&
            view.surface != "save-details" && view.surface != "manage-games")
            draw_sheet_overlay(view);

        // Live notifications wait behind dialogs (they also stop counting down there).
        const bool hide_transient = !state.simulation_mode &&
            (!state.navigation.modals.empty() || state.editor.active);
        float y = 118.0F;
        for (const Notice &notice : state.notices)
        {
            if (notice.persistent || hide_transient)
                continue;
            overlay_.shadow({1288.0F, y, 536.0F, 84.0F}, 10.0F, 28.0F, gfx::Color::rgb(0x000000, 0.6F));
            overlay_.rounded_rect({1288.0F, y, 536.0F, 84.0F}, 10.0F, kPanel);
            overlay_.circle(1320.0F, y + 42.0F, 7.0F, notice.failure ? kRed : kAccent);
            ui::text(overlay_, fonts_.semibold, fit(fonts_.semibold, notice.message, 22.0F, 440.0F),
                     1342.0F, y + 36.0F, 22.0F, kText);
            const std::string &second = notice.detail.empty() ? notice.action : notice.detail;
            if (!second.empty())
                ui::text(overlay_, fonts_.regular, fit(fonts_.regular, second, 18.0F, 460.0F), 1342.0F, y + 62.0F,
                         18.0F, notice.detail.empty() ? kAccent : kSecondary);
            y += 98.0F;
        }
    }

    // A password's keyboard: the bottom row gains a Show (or Hide) key between space and backspace,
    // where the simulation expects it.
    static std::vector<ui::KeyboardLayout> secret_layouts(bool revealed)
    {
        const auto key = [](ui::KeyKind kind, int span) {
            ui::KeyboardKey result;
            result.kind = kind;
            result.span = span;
            if (kind == ui::KeyKind::space)
                result.text = " ";
            return result;
        };
        ui::KeyboardKey show;
        show.label = revealed ? "Hide" : "Show";
        ui::KeyboardLayout letters = ui::KeyboardLayout::letters();
        letters.rows.back() = {key(ui::KeyKind::shift, 2), key(ui::KeyKind::layout, 1), key(ui::KeyKind::space, 2), show,
                               key(ui::KeyKind::backspace, 2), key(ui::KeyKind::done, 2)};
        ui::KeyboardLayout symbols = ui::KeyboardLayout::symbols();
        symbols.rows.back() = {key(ui::KeyKind::layout, 2), key(ui::KeyKind::space, 3), show,
                               key(ui::KeyKind::backspace, 2), key(ui::KeyKind::done, 2)};
        return {letters, symbols};
    }

    void draw_editor_overlay(ui::Canvas &canvas, const State &state)
    {
        overlay_.rounded_rect({0.0F, 0.0F, kWidth, kHeight}, 0.0F, gfx::Color::rgb(0x000000, 0.68F));
        overlay_.rounded_rect({420.0F, 116.0F, 1080.0F, 820.0F}, 16.0F, kPanel);
        ui::text(overlay_, fonts_.display,
                 state.editor.purpose.empty() ? "Enter text" : state.editor.purpose,
                 476.0F, 188.0F, 46.0F, kText);
        ui::Painter painter(overlay_, fonts_, theme_, renderer_.glass_texture());
        const gfx::Rect field{476.0F, 232.0F, 968.0F, 76.0F};
        const bool masked = state.editor.secret && !state.editor.revealed;
        const std::string shown = masked ? mask_secret(state.editor.value) : state.editor.value;
        painter.field(field, shown, false,
                      {0.0F, 0.0F, false});
        const std::size_t caret = std::min(state.editor.caret, state.editor.value.size());
        const std::string_view typed = std::string_view(state.editor.value).substr(0, caret);
        const float caret_x = field.x + 22.0F +
                              (masked ? fonts_.regular.measure(mask_secret(typed), 24.0F) : fonts_.regular.measure(typed, 24.0F));
        overlay_.rounded_rect({std::min(caret_x, field.x + field.w - 24.0F), field.y + 18.0F,
                               2.0F, field.h - 36.0F},
                              1.0F, kAccent);
        keyboard_.style.reduced_motion = state.reduced_motion;
        const int layouts = !state.editor.secret ? 0 : state.editor.revealed ? 2 : 1;
        if (layouts != keyboard_layouts_)
        {
            keyboard_layouts_ = layouts;
            keyboard_.set_layouts(layouts == 0 ? std::vector<ui::KeyboardLayout>{ui::KeyboardLayout::letters(), ui::KeyboardLayout::symbols()}
                                               : secret_layouts(layouts == 2));
        }
        keyboard_.set_length(static_cast<int>(state.editor.value.size()));
        keyboard_.set_shift(state.editor.shifted ? ui::KeyboardShift::lock : ui::KeyboardShift::off);
        keyboard_.set_layout(state.editor.symbols ? 1 : 0);
        keyboard_.set_focus(state.editor.focus_row, state.editor.focus_key, true);
        keyboard_.set_active(true);
        keyboard_.update(1.0F / 60.0F);
        keyboard_.draw(canvas);
    }

    void draw_modal_overlay(const ModalEntry &modal, const ViewData &view)
    {
        overlay_.rounded_rect({0.0F, 0.0F, kWidth, kHeight}, 0.0F, gfx::Color::rgb(0x000000, 0.72F));
        if (!modal.link.empty())
        {
            draw_link_modal(modal, view);
            return;
        }
        // Three buttons need the wide panel or their labels are cut.
        const float width = modal.destructive && view.rows.size() < 3 ? 820.0F : 960.0F;
        const gfx::Rect panel{(kWidth - width) * 0.5F, 254.0F, width, 500.0F};
        overlay_.shadow(panel, 16.0F, 40.0F, gfx::Color::rgb(0x000000, 0.65F));
        overlay_.rounded_rect(panel, 16.0F, kPanel);
        const float heading_bottom = ui::paragraph(
            overlay_, fonts_.display, modal.title, panel.x + 48.0F, panel.y + 76.0F,
            48.0F, panel.w - 96.0F, 54.0F, modal.destructive ? kAmber : kText, 3);
        const std::string body = !modal.body.empty() ? modal.body : view.subtitle;
        ui::paragraph(overlay_, fonts_.regular, body, panel.x + 48.0F, heading_bottom + 24.0F, 25.0F,
                      panel.w - 96.0F, 35.0F, kSecondary,
                      std::max(1, static_cast<int>((panel.y + 352.0F - heading_bottom - 24.0F) / 35.0F) + 1));
        const std::size_t count = std::max<std::size_t>(1, view.rows.size());
        const float button_width = (panel.w - 112.0F) / count;
        for (std::size_t index = 0; index < view.rows.size(); ++index)
        {
            std::string label = overlay_label(view.rows[index]);
            if (view.rows[index].action == "confirm-destructive")
            {
                if (modal.title.find("Uninstall") != std::string::npos)
                    label = "Uninstall";
                else if (modal.title.find("download") != std::string::npos)
                    label = "Cancel download";
                else if (modal.title == "Cancel the move?")
                    label = "Cancel move";
                else if (modal.title == "Update anyway?")
                    label = "Update anyway";
                else
                    label = "Confirm";
            }
            const gfx::Rect button{panel.x + 48.0F + index * (button_width + 16.0F), panel.y + 388.0F,
                                   button_width, 68.0F};
            // A choice in the dialog (Delete saves too) says whether it is on.
            const bool toggle = view.rows[index].kind == RowKind::Toggle;
            mac_button(overlay_, button, label, view.rows[index].dangerous,
                       control_focus("modal-" + view.rows[index].key, static_cast<int>(index) == view.focused_row),
                       !view.rows[index].enabled, toggle ? (view.rows[index].selected ? "On" : "Off") : "", false,
                       kPanel);
        }
    }

    // A dialog with a web page (release notes): the title, the text, then a strip with the page as a
    // QR code and its address (the console has no browser), then the buttons. The panel's height
    // follows the text, so a short note gives a short dialog.
    void draw_link_modal(const ModalEntry &modal, const ViewData &view)
    {
        constexpr float width = 1080.0F, pad = 56.0F, qr_side = 232.0F, strip_gap = 40.0F;
        constexpr float body_size = 25.0F, body_line = 35.0F, address_size = 21.0F, address_line = 28.0F;
        const float inner = width - 2.0F * pad;
        const float address_w = inner - qr_side - strip_gap;
        std::string address = modal.link;
        if (address.rfind("https://", 0) == 0) address.erase(0, 8);

        const int body_lines = modal.body.empty() ? 0 :
            std::min(10, static_cast<int>(fonts_.regular.font->wrap(modal.body, body_size, inner).size()));
        const int address_lines =
            std::min(3, std::max(1, static_cast<int>(fonts_.regular.font->wrap(address, address_size, address_w).size())));
        constexpr float title_h = 60.0F;
        const float body_h = body_lines * body_line;
        const float height = pad + title_h + (body_lines ? 28.0F + body_h : 0.0F) + 36.0F + qr_side + 40.0F + 68.0F + pad;
        const gfx::Rect panel{(kWidth - width) * 0.5F, std::round((kHeight - height) * 0.5F), width, height};
        overlay_.shadow(panel, 16.0F, 40.0F, gfx::Color::rgb(0x000000, 0.65F));
        overlay_.rounded_rect(panel, 16.0F, kPanel);

        float y = panel.y + pad;
        ui::text(overlay_, fonts_.display, fit(fonts_.display, modal.title, 48.0F, inner), panel.x + pad, y + 46.0F,
                 48.0F, kText);
        y += title_h;
        if (body_lines)
        {
            ui::paragraph(overlay_, fonts_.regular, modal.body, panel.x + pad, y + 28.0F + 26.0F, body_size, inner,
                          body_line, kSecondary, 10);
            y += 28.0F + body_h;
        }
        y += 36.0F;
        // The strip: the code, then what it is and where it leads, centred on the code.
        if (!draw_qr_matrix(overlay_, link_qr_, panel.x + pad, y, static_cast<int>(qr_side)))
            overlay_.rounded_rect({panel.x + pad, y, qr_side, qr_side}, 12.0F, kMuted.with_alpha(0.25F));
        const float text_x = panel.x + pad + qr_side + strip_gap;
        const float block_h = 30.0F + 14.0F + address_lines * address_line;
        const float block_y = y + (qr_side - block_h) * 0.5F;
        ui::text(overlay_, fonts_.semibold, "Scan for the full release notes", text_x, block_y + 24.0F, 26.0F, kText);
        ui::paragraph(overlay_, fonts_.regular, address, text_x, block_y + 30.0F + 14.0F + 21.0F, address_size,
                      address_w, address_line, kSecondary, 3);
        y += qr_side + 40.0F;

        const std::size_t count = std::max<std::size_t>(1, view.rows.size());
        const float button_width = (inner - (count - 1) * 16.0F) / count;
        for (std::size_t index = 0; index < view.rows.size(); ++index)
        {
            const gfx::Rect button{panel.x + pad + index * (button_width + 16.0F), y, button_width, 68.0F};
            mac_button(overlay_, button, overlay_label(view.rows[index]), view.rows[index].dangerous,
                       control_focus("modal-" + view.rows[index].key, static_cast<int>(index) == view.focused_row),
                       !view.rows[index].enabled, "", false, kPanel);
        }
    }

    // The small label above a sheet's title names where the sheet belongs, never its surface id.
    static std::string sheet_eyebrow(const std::string &surface)
    {
        static const std::map<std::string, std::string> names = {
            {"choice-picker", "Game settings"}, {"preset-picker", "Game settings"}, {"button-mapping", "Game settings"}, {"controller-tuning", "Game settings"},
            {"game-settings", "Playden"}, {"game-maintenance", "Playden"}, {"kept-data", "Settings"}, {"leftover-titles", "Settings"},
            {"download-server", "Settings"},
            {"import-games", "Library"}, {"import-programs", "Import games"}, {"steam-programs", "Setup"}, {"import-choice", "Import games"},
            {"import-storage", "Import games"}, {"move-storage", "Move game"},
            {"job-actions", "Downloads"}, {"prep-actions", "Preparation"}, {"storage-details", "Storage"}, {"context", "Game"}, {"compatibility", "Game"}, {"collections", "Game"}, {"collections-manage", "Library"},
            {"filters", "Library"}, {"logs-controls", "Logs"}, {"setting-value", "Settings"}, {"diagnostics", "Settings"},
            {"notices", "Notifications"}, {"qr-controls", "Sign in"}, {"session-controls", "Simulation"},
            {"simulation-controls", "Simulation"}, {"scenarios", "Simulation"}};
        if (const auto found = names.find(surface); found != names.end())
            return found->second;
        std::string label = surface;
        std::replace(label.begin(), label.end(), '-', ' ');
        return label;
    }

    // A drive from Queue's storage line: the same usage block Settings shows, and how reserved space counts.
    void draw_drive_details(const ViewData &view, const gfx::Rect &panel)
    {
        const State &state = *current_state_;
        const std::string &opener = state.navigation.modals.back().opener_focus_key;
        const auto volume = std::find_if(state.volumes.begin(), state.volumes.end(),
                                         [&opener](const Volume &item) { return "storage-" + item.id == opener; });
        float y = 210.0F;
        if (volume != state.volumes.end())
        {
            draw_volume_usage(overlay_, *volume, !state.simulation_mode && volume->id == state.default_volume_id,
                              panel.x + 48.0F, y, panel.w - 96.0F, false);
            y += 150.0F;
            if (volume->reserved_bytes > 0)
            {
                ui::paragraph(overlay_, fonts_.regular, "Reserved space is part of the free space, set aside for the downloads still queued.",
                              panel.x + 48.0F, y, 22.0F, panel.w - 96.0F, 32.0F, kSecondary, 3);
                y += 90.0F;
            }
        }
        draw_overlay_rows(view, {panel.x + 40.0F, y, panel.w - 80.0F, 120.0F}, false);
    }

    void draw_sheet_overlay(const ViewData &view)
    {
        const float ease = overlay_transition_.enter();
        overlay_.rounded_rect({0.0F, 0.0F, kWidth, kHeight}, 0.0F,
                              gfx::Color::rgb(0x000000, 0.58F * ease));
        const bool wide = view.surface.find("settings") != std::string::npos ||
                          view.surface.find("profile") != std::string::npos ||
                          view.surface == "choice-picker" || view.surface == "preset-picker" ||
                          view.surface == "button-mapping" || view.surface == "controller-tuning" ||
                          view.surface == "game-maintenance" ||
                          view.surface == "controller" || view.surface == "logs" ||
                          view.surface == "storage" || view.surface == "move-storage" || view.surface == "install" || view.surface == "update" || view.surface == "kept-data" || view.surface == "leftover-titles" ||
                          view.surface == "download-server" ||
                          view.surface == "import-games" ||
                          view.surface == "import-programs" || view.surface == "steam-programs";
        const float width = overlay_transition_.panel_width(wide ? 960.0F : 640.0F);
        const gfx::Rect panel{kWidth - width + (1.0F - ease) * width, 0.0F, width, kHeight};
        overlay_.shadow({panel.x - 18.0F, 0.0F, panel.w + 18.0F, panel.h}, 0.0F, 36.0F,
                        gfx::Color::rgb(0x000000, 0.75F));
        overlay_.rounded_rect(panel, 0.0F, kPanel);
        overlay_.push_opacity(overlay_transition_.content());
        section_overlay(view.eyebrow.empty() ? sheet_eyebrow(view.surface) : view.eyebrow, panel.x + 48.0F, 72.0F);
        // A title too long for one line wraps onto a second; what follows moves down with it.
        float drop = 0.0F;
        if (fonts_.display.measure(view.title, 48.0F) > panel.w - 96.0F)
        {
            ui::paragraph(overlay_, fonts_.display, view.title, panel.x + 48.0F, 142.0F, 48.0F, panel.w - 96.0F, 54.0F,
                          kText, 2);
            drop = 54.0F;
        }
        else
            ui::text(overlay_, fonts_.display, view.title, panel.x + 48.0F, 142.0F, 48.0F, kText);
        if (!view.subtitle.empty())
            ui::paragraph(overlay_, fonts_.regular, view.subtitle, panel.x + 48.0F, 190.0F + drop, 23.0F,
                          panel.w - 96.0F, 32.0F, kSecondary, view.surface == "install" ? 6 : 3);
        if (view.surface == "storage")
            draw_storage_overlay_rows(view, {panel.x + 40.0F, 246.0F, panel.w - 80.0F, 710.0F});
        else if (view.surface == "import-storage" || view.surface == "move-storage")
            // The wizard's drive choice, or a move's: the drive cards of the storage picker, under a one-line subtitle.
            draw_storage_overlay_rows(view, {panel.x + 40.0F, 262.0F + drop, panel.w - 80.0F, 694.0F - drop});
        else if (view.surface == "storage-details")
            draw_drive_details(view, panel);
        else if (view.surface == "logs")
            draw_log_overlay(view, {panel.x + 40.0F, 246.0F, panel.w - 80.0F, 710.0F});
        else if (view.surface == "controller")
            draw_controller_overlay(view, {panel.x + 40.0F, 236.0F, panel.w - 80.0F, 720.0F});
        else
            draw_overlay_rows(view, {panel.x + 40.0F, (view.surface == "install" ? 414.0F : 292.0F) + drop,
                                     panel.w - 80.0F, (view.surface == "install" ? 528.0F : 650.0F) - drop});
        if (view.comparison)
            draw_preset_comparison(*view.comparison, {72.0F, 150.0F, panel.x - 144.0F, 760.0F}, ease);
        overlay_.pop_opacity();
    }

    // Preset chooser: what the focused preset changes, beside the sheet (Current → This preset).
    void draw_preset_comparison(const ViewData::Comparison &comparison, const gfx::Rect &bounds, float ease)
    {
        if (bounds.w < 360.0F)
            return;
        const gfx::Rect card{bounds.x, bounds.y, bounds.w, bounds.h};
        overlay_.shadow(card, 14.0F, 32.0F, gfx::Color::rgb(0x000000, 0.55F * ease));
        overlay_.rounded_rect(card, 14.0F, kPanel.with_alpha(0.96F * ease));
        float y = card.y + 64.0F;
        ui::text(overlay_, fonts_.display, fit(fonts_.display, comparison.title, 40.0F, card.w - 96.0F), card.x + 48.0F, y,
                 40.0F, kText);
        y += 18.0F;
        if (!comparison.hint.empty())
            y = ui::paragraph(overlay_, fonts_.regular, comparison.hint, card.x + 48.0F, y + 24.0F, 21.0F,
                              card.w - 96.0F, 29.0F, kSecondary, 3);
        y += 36.0F;
        const float name_w = card.w * 0.34F;
        const float column_w = (card.w - 96.0F - name_w - 40.0F) * 0.5F;
        const float current_x = card.x + 48.0F + name_w;
        const float arrow_x = current_x + column_w + 20.0F;
        const float preset_x = arrow_x + 20.0F;
        ui::text(overlay_, fonts_.semibold, "SETTING", card.x + 48.0F, y, 14.0F, kMuted, gfx::Align::left, 1.4F);
        ui::text(overlay_, fonts_.semibold, "CURRENT", current_x, y, 14.0F, kMuted, gfx::Align::left, 1.4F);
        ui::text(overlay_, fonts_.semibold, "THIS PRESET", preset_x, y, 14.0F, kMuted, gfx::Align::left, 1.4F);
        y += 16.0F;
        for (const auto &row : comparison.rows)
        {
            if (y + 52.0F > card.y + card.h - 80.0F)
                break;
            overlay_.rounded_rect({card.x + 40.0F, y + 6.0F, card.w - 80.0F, 1.0F}, 0.0F, kText.with_alpha(0.10F));
            const bool changes = row[1] != row[2];
            ui::text(overlay_, fonts_.semibold, fit(fonts_.semibold, row[0], 21.0F, name_w - 16.0F), card.x + 48.0F,
                     y + 40.0F, 21.0F, kText);
            ui::text(overlay_, fonts_.regular, fit(fonts_.regular, row[1], 20.0F, column_w), current_x, y + 40.0F, 20.0F,
                     kSecondary);
            if (changes)
            {
                // The UI fonts have no arrow glyph: draw one.
                const float cy = y + 33.0F;
                overlay_.line(arrow_x - 8.0F, cy, arrow_x + 7.0F, cy, 2.0F, kAccent);
                overlay_.line(arrow_x + 2.0F, cy - 5.0F, arrow_x + 8.0F, cy, 2.0F, kAccent);
                overlay_.line(arrow_x + 2.0F, cy + 5.0F, arrow_x + 8.0F, cy, 2.0F, kAccent);
            }
            ui::text(overlay_, fonts_.regular, fit(fonts_.regular, row[2], 20.0F, column_w), preset_x, y + 40.0F, 20.0F,
                     changes ? kAccent : kSecondary);
            y += 56.0F;
        }
        ui::paragraph(overlay_, fonts_.regular, comparison.note, card.x + 48.0F, card.y + card.h - 56.0F, 19.0F,
                      card.w - 96.0F, 26.0F, kSecondary, 2);
    }

    void draw_storage_overlay_rows(const ViewData &view, const gfx::Rect &bounds)
    {
        if (current_state_ == nullptr)
            return;
        float y = bounds.y;
        ui::Painter painter(overlay_, fonts_, theme_, renderer_.glass_texture());
        for (const ViewRow &row : view.rows)
        {
            if (row.action.rfind("volume-", 0) != 0)
                continue;
            const std::string id = row.action.substr(7);
            const auto found = std::find_if(current_state_->volumes.begin(), current_state_->volumes.end(),
                                            [&id](const Volume &volume) { return volume.id == id; });
            if (found == current_state_->volumes.end())
                continue;
            const Volume &volume = *found;
            const std::uint64_t unused = volume.capacity_bytes - std::min(volume.used_bytes, volume.capacity_bytes);
            const std::uint64_t free = unused - std::min(volume.reserved_bytes, unused);
            const int row_index = static_cast<int>(&row - view.rows.data());
            const gfx::Rect card{bounds.x, y, bounds.w, 156.0F};
            const float amount = control_focus("volume-" + row.key, row_index == view.focused_row);
            // The import wizard dims a drive that can't take the game, as its other rows dim.
            const bool dimmed = (view.surface == "import-storage" || view.surface == "move-storage") && !row.enabled;
            if (dimmed)
                overlay_.push_opacity(0.5F);
            focus_glow(overlay_, card, amount);
            overlay_.rounded_rect(card, 8.0F, kPanel);
            overlay_.rounded_rect(card, 8.0F, kText.with_alpha(row.selected ? 0.10F : 0.04F));
            focus_ring(overlay_, card, amount);
            ui::text(overlay_, fonts_.display, volume.label, card.x + 22.0F, card.y + 38.0F,
                     27.0F, row.enabled ? kText : kMuted);
            // The import wizard's rows say all of their state (its default drive included); a
            // disabled one is amber only for a problem with the drive (tone "warn").
            const bool wizard = view.surface == "import-storage" || view.surface == "move-storage";
            const bool is_default = !wizard && !current_state_->simulation_mode && volume.id == current_state_->default_volume_id;
            const bool warn = wizard ? row.tone == "warn" : !row.enabled;
            ui::text(overlay_, fonts_.regular, is_default && row.enabled ? "Default · " + row.subtitle : row.subtitle,
                     card.x + card.w - 22.0F, card.y + 38.0F, 18.0F, warn ? kAmber : row.enabled ? kSecondary : kMuted,
                     gfx::Align::right);
            if (!volume.capacity_known)
            {
                ui::text(overlay_, fonts_.regular, "Used by games  " + short_size(volume.games_bytes),
                         card.x + 22.0F, card.y + 98.0F, 17.0F, kSecondary);
                if (volume.reserved_bytes > 0)
                    ui::text(overlay_, fonts_.regular, "Reserved by queue  " + short_size(volume.reserved_bytes),
                             card.x + card.w - 22.0F, card.y + 98.0F, 17.0F, kSecondary, gfx::Align::right);
                ui::text(overlay_, fonts_.regular, "Free space unavailable",
                         card.x + 22.0F, card.y + 128.0F, 17.0F, kMuted);
                if (dimmed)
                    overlay_.pop_opacity();
                y += 174.0F;
                continue;
            }
            const float fraction = volume.capacity_bytes == 0 ? 0.0F :
                static_cast<float>(volume.capacity_bytes - free) / static_cast<float>(volume.capacity_bytes);
            overlay_.rounded_rect({card.x + 22.0F, card.y + 58.0F, card.w - 44.0F, 8.0F}, 4.0F,
                                  kText.with_alpha(0.14F));
            overlay_.rounded_rect({card.x + 22.0F, card.y + 58.0F,
                                   (card.w - 44.0F) * std::clamp(fraction, 0.0F, 1.0F), 8.0F},
                                  4.0F, volume.available ? kAccent : kMuted);
            ui::text(overlay_, fonts_.regular, "Capacity  " + short_size(volume.capacity_bytes),
                     card.x + 22.0F, card.y + 98.0F, 17.0F, kSecondary);
            ui::text(overlay_, fonts_.regular, "Used  " + short_size(volume.used_bytes),
                     card.x + 22.0F, card.y + 128.0F, 17.0F, kSecondary);
            if (volume.reserved_bytes > 0)
                ui::text(overlay_, fonts_.regular, "Reserved  " + short_size(volume.reserved_bytes),
                         card.x + card.w - 22.0F, card.y + 98.0F, 17.0F, kSecondary,
                         gfx::Align::right);
            ui::text(overlay_, fonts_.semibold, "Free  " + short_size(free),
                     card.x + card.w - 22.0F, card.y + 128.0F, 17.0F, kText,
                     gfx::Align::right);
            if (dimmed)
                overlay_.pop_opacity();
            y += 174.0F;
        }
        const auto cancel = std::find_if(view.rows.begin(), view.rows.end(), [](const ViewRow &row) {
            return row.action == "cancel";
        });
        if (cancel != view.rows.end() && y + 70.0F < bounds.y + bounds.h)
        {
            const int index = static_cast<int>(std::distance(view.rows.begin(), cancel));
            painter.row({bounds.x, y, bounds.w, 66.0F}, overlay_label(*cancel), "", 0.0F,
                        {index == view.focused_row ? 1.0F : 0.0F, 0.0F, false});
        }
    }

    void draw_log_overlay(const ViewData &view, const gfx::Rect &bounds)
    {
        overlay_.rounded_rect(bounds, 8.0F, kBackground.with_alpha(0.72F));
        if (current_state_ == nullptr)
            return;
        const State &state = *current_state_;
        const gfx::Rect viewport{bounds.x + 20.0F, bounds.y + 20.0F,
                                 bounds.w - 40.0F, bounds.h - 132.0F};
        overlay_.rounded_rect(viewport, 6.0F, kPanel.with_alpha(0.48F));

        if (state.log_state == LogState::Loading)
        {
            overlay_.circle(viewport.x + 34.0F, viewport.y + 44.0F, 7.0F, kAccent);
            ui::text(overlay_, fonts_.semibold, "Loading fixture logs…", viewport.x + 58.0F,
                     viewport.y + 52.0F, 22.0F, kText);
            overlay_.rounded_rect({viewport.x + 24.0F, viewport.y + 78.0F,
                                   viewport.w - 48.0F, 5.0F}, 2.5F, kText.with_alpha(0.12F));
            const float pulse = 0.18F + 0.22F * (0.5F + 0.5F * std::sin(clock_ * 5.0F));
            overlay_.rounded_rect({viewport.x + 24.0F, viewport.y + 78.0F,
                                   (viewport.w - 48.0F) * pulse, 5.0F}, 2.5F, kAccent);
        }
        else if (state.log_state == LogState::Failed)
        {
            ui::text(overlay_, fonts_.display, "Logs unavailable", viewport.x + 24.0F,
                     viewport.y + 54.0F, 30.0F, kRed);
            const auto error = std::find_if(view.rows.begin(), view.rows.end(), [](const ViewRow &row) {
                return row.key == "logs-error";
            });
            const std::string_view message = error == view.rows.end() || error->subtitle.empty() ?
                                                 (state.simulation_mode ? std::string_view("The simulated log source failed.")
                                                                        : std::string_view(state.log_context)) :
                                                 std::string_view(error->subtitle);
            dynamic_overlay_text(message, viewport.x + 24.0F, viewport.y + 96.0F, 20.0F,
                                 kSecondary, viewport.w - 48.0F);
        }
        else if (state.log_lines.empty())
        {
            ui::text(overlay_, fonts_.display, "No log entries", viewport.cx(),
                     viewport.y + viewport.h * 0.48F, 34.0F, kSecondary, gfx::Align::center);
            ui::text(overlay_, fonts_.regular,
                     state.simulation_mode ? "This fixture has no lines for the current context."
                                           : "Nothing has been logged here yet.",
                     viewport.cx(), viewport.y + viewport.h * 0.48F + 38.0F, 19.0F, kMuted,
                     gfx::Align::center);
        }
        else
        {
            constexpr int visible_lines = 8;
            const int begin = std::clamp(state.log_offset, 0,
                                         std::max(0, static_cast<int>(state.log_lines.size()) - visible_lines));
            const int end = std::min(static_cast<int>(state.log_lines.size()), begin + visible_lines);
            float y = viewport.y + 38.0F;
            for (int index = begin; index < end; ++index)
            {
                std::array<char, 12> number{};
                std::snprintf(number.data(), number.size(), "%03d", index + 1);
                ui::text(overlay_, fonts_.regular, number.data(), viewport.x + 22.0F, y,
                         16.0F, kMuted);
                dynamic_overlay_text(state.log_lines[static_cast<std::size_t>(index)],
                                     viewport.x + 72.0F, y, 18.0F, kText, viewport.w - 106.0F);
                y += 52.0F;
            }
            const std::string range = std::to_string(begin + 1) + "–" + std::to_string(end) +
                                      " of " + std::to_string(state.log_lines.size());
            ui::text(overlay_, fonts_.regular, range, viewport.x + viewport.w - 24.0F,
                     viewport.y + viewport.h - 18.0F, 16.0F, kMuted, gfx::Align::right);
            if (state.log_lines.size() > visible_lines)
            {
                const float track_h = viewport.h - 52.0F;
                const float thumb_h = std::max(36.0F, track_h * visible_lines /
                                                        static_cast<float>(state.log_lines.size()));
                const float travel = track_h - thumb_h;
                const float denominator = static_cast<float>(state.log_lines.size() - visible_lines);
                const float thumb_y = viewport.y + 18.0F + travel * begin / denominator;
                overlay_.rounded_rect({viewport.x + viewport.w - 7.0F, viewport.y + 18.0F,
                                       3.0F, track_h}, 1.5F, kText.with_alpha(0.10F));
                overlay_.rounded_rect({viewport.x + viewport.w - 7.0F, thumb_y, 3.0F, thumb_h},
                                      1.5F, kSecondary);
            }
        }

        std::vector<const ViewRow*> actions;
        for (const ViewRow &row : view.rows)
            if (!row.action.empty())
                actions.push_back(&row);
        if (!actions.empty())
        {
            const float width = std::min(260.0F, (bounds.w - 48.0F) / actions.size() - 10.0F);
            for (std::size_t index = 0; index < actions.size(); ++index)
            {
                const bool focused = state.navigation.focus_key == actions[index]->key;
                mac_button(overlay_, {bounds.x + 24.0F + index * (width + 16.0F), bounds.y + bounds.h - 82.0F, width, 58.0F},
                           overlay_label(*actions[index]), false, control_focus("log-" + actions[index]->key, focused),
                           !actions[index]->enabled, {}, false, kBackground);
            }
        }
    }

    void draw_controller_overlay(const ViewData &view, const gfx::Rect &bounds)
    {
        const bool connected = current_state_ != nullptr && current_state_->controller_connected;
        const ControllerState input = current_state_ == nullptr ? ControllerState{} :
                                                                  current_state_->controller;
        overlay_.rounded_rect(bounds, 12.0F, kBackground.with_alpha(0.52F));
        ui::text(overlay_, fonts_.display, connected ? "Controller connected" : "Controller disconnected",
                 bounds.x + bounds.w * 0.5F, bounds.y + 54.0F, 31.0F,
                 connected ? kGreen : kAmber, gfx::Align::center);
        const gfx::Color shell = connected ? kText.with_alpha(0.12F) : kMuted.with_alpha(0.10F);
        const float cx = bounds.x + bounds.w * 0.5F;
        const float cy = bounds.y + 292.0F;
        overlay_.rounded_rect({cx - 286.0F, cy - 136.0F, 572.0F, 250.0F}, 88.0F, shell);
        overlay_.circle(cx - 224.0F, cy + 96.0F, 92.0F, shell);
        overlay_.circle(cx + 224.0F, cy + 96.0F, 92.0F, shell);
        overlay_.rounded_rect({cx - 242.0F, cy - 164.0F, 158.0F, 24.0F}, 8.0F,
                              input.l2 > 0.5F ? kAccent : kText.with_alpha(0.16F));
        overlay_.rounded_rect({cx + 84.0F, cy - 164.0F, 158.0F, 24.0F}, 8.0F,
                              input.r2 > 0.5F ? kAccent : kText.with_alpha(0.16F));
        ui::text(overlay_, fonts_.semibold, "L2", cx - 163.0F, cy - 146.0F, 16.0F, kText,
                 gfx::Align::center);
        ui::text(overlay_, fonts_.semibold, "R2", cx + 163.0F, cy - 146.0F, 16.0F, kText,
                 gfx::Align::center);

        const auto stick = [this](float x, float y, float axis_x, float axis_y) {
            overlay_.circle(x, y, 48.0F, kBackground.with_alpha(0.72F));
            overlay_.circle(x + axis_x * 24.0F, y + axis_y * 24.0F, 22.0F,
                            (axis_x != 0.0F || axis_y != 0.0F) ? kAccent : kSecondary);
        };
        stick(cx - 112.0F, cy + 48.0F, input.left_x, input.left_y);
        stick(cx + 72.0F, cy + 48.0F, input.right_x, input.right_y);

        const auto face = [this](float x, float y, bool active, ui::Button button) {
            overlay_.circle(x, y, 23.0F, active ? kAccent : kBackground.with_alpha(0.78F));
            ui::draw_button(overlay_, fonts_,
                            ui::GlyphStyle::mono(active ? kBackground : kText, kBackground),
                            button, x - ui::button_width(button, 30.0F) * 0.5F, y, 30.0F);
        };
        face(cx + 200.0F, cy - 28.0F, input.triangle, ui::Button::triangle);
        face(cx + 248.0F, cy + 20.0F, input.circle, ui::Button::circle);
        face(cx + 200.0F, cy + 68.0F, input.cross, ui::Button::cross);
        face(cx + 152.0F, cy + 20.0F, input.square, ui::Button::square);

        const gfx::Color dpad = (input.left_x != 0.0F || input.left_y != 0.0F) ? kAccent : kSecondary;
        overlay_.rounded_rect({cx - 232.0F, cy - 4.0F, 92.0F, 28.0F}, 5.0F, dpad);
        overlay_.rounded_rect({cx - 200.0F, cy - 36.0F, 28.0F, 92.0F}, 5.0F, dpad);
        overlay_.rounded_rect({cx - 34.0F, cy - 18.0F, 68.0F, 30.0F}, 8.0F,
                              input.options ? kAccent : kBackground.with_alpha(0.72F));
        ui::text(overlay_, fonts_.semibold, "OPTIONS", cx, cy + 3.0F, 12.0F, kText,
                 gfx::Align::center, 0.8F);
        ui::text(overlay_, fonts_.regular, "Left stick", cx - 112.0F, cy + 122.0F, 16.0F,
                 kSecondary, gfx::Align::center);
        ui::text(overlay_, fonts_.regular, "Right stick", cx + 72.0F, cy + 122.0F, 16.0F,
                 kSecondary, gfx::Align::center);
        draw_overlay_rows(view, {bounds.x + 20.0F, bounds.y + bounds.h - 104.0F,
                                 bounds.w - 40.0F, 84.0F});
    }

    void dynamic_overlay_text(std::string_view value, float x, float y, float size,
                              gfx::Color color, float max_width)
    {
        const ui::FontRef &face = font_for(value, fonts_.regular);
        ui::text(overlay_, face, face.font->fit(value, size, max_width), x, y, size, color);
    }

    void section_overlay(std::string_view value, float x, float y)
    {
        ui::text(overlay_, fonts_.display, upper_latin(value), x, y, 20.0F, kSecondary,
                 gfx::Align::left, 2.4F);
    }

    std::string overlay_label(const ViewRow &row) const
    {
        // A sheet that only shows things says Close or Back on its cancel row.
        if (row.action == "cancel")
            return row.title == "Close" || row.title == "Back" ? row.title : "Cancel";
        // Titles come labeled from the model; only a bare action key needs its dashes softened.
        std::string label = row.title.empty() ? row.action : row.title;
        if (row.title.empty())
            std::replace(label.begin(), label.end(), '-', ' ');
        // A program path ("redist/setup.exe") is shown as it is on the drive.
        if (!label.empty() && label.front() >= 'a' && label.front() <= 'z' && row.action.rfind("import-program-", 0) != 0 &&
            row.action.rfind("steam-program-", 0) != 0)
            label.front() = static_cast<char>(label.front() - 'a' + 'A');
        return label;
    }

    std::string overlay_value(const ViewRow &row) const
    {
        if (row.key == "account-auth")
        {
            std::string status = row.value;
            if (!status.empty() && status.front() >= 'a' && status.front() <= 'z')
                status.front() = static_cast<char>(status.front() - 'a' + 'A');
            if (!row.subtitle.empty())
                status += (status.empty() ? "" : " · ") + row.subtitle;
            return status;
        }
        if (row.key == "account-last-sync" && current_state_ != nullptr && row.value != "Never")
        {
            std::int64_t timestamp = 0;
            const auto parsed = std::from_chars(row.value.data(), row.value.data() + row.value.size(), timestamp);
            if (parsed.ec == std::errc{})
                return relative_time(timestamp, current_state_->clock_ticks);
        }
        if (!row.value.empty())
            return row.value;
        if (!row.badge.empty())
            return row.badge;
        if (!row.subtitle.empty())
            return row.subtitle;
        return {};
    }

    // Installed games: Sort and Select all pinned on top, the games scrolling under them, and a
    // footer with the selection (or the batch) and Uninstall selected / Show all games / Close.
    // `scrolls` false: rows pinned in place (a sheet's fixed header), leaving the list's scroll alone.
    void draw_overlay_rows(const ViewData &view, const gfx::Rect &bounds, bool scrolls = true)
    {
        const bool settings_sheet = view.surface == "game-settings" || view.surface == "choice-picker" ||
                                    view.surface == "download-server" ||
                                    view.surface == "preset-picker" || view.surface == "button-mapping" ||
                                    view.surface == "controller-tuning" ||
                                    view.surface == "game-maintenance";
        // The Import sheets: each folder's path (or why it can't be added) under its name.
        const bool import_sheet = view.surface == "import-games" || view.surface == "import-programs" ||
                                  view.surface == "import-choice" || view.surface == "steam-programs";
        // About: each fact as its name over its value and note, at the row's full width, wrapped and never cut.
        const bool about_sheet = view.surface == "about";
        const auto about_lines = [&](const std::string &text, float size) {
            return text.empty() ? 0.0F
                                : static_cast<float>(fonts_.regular.font->wrap(text, size, bounds.w - 48.0F).size());
        };
        const auto about_height = [&](const ViewRow &row) {
            // Room for one note line on every fact, so rows with and without one line up.
            return 58.0F + 30.0F * std::max(1.0F, about_lines(overlay_value(row), 24.0F)) +
                   26.0F * std::max(1.0F, about_lines(row.subtitle, 19.0F));
        };
        // The import wizard's hints wrap onto a second line rather than being cut.
        const float hint_width = bounds.w - 48.0F;
        const auto hint_extra = [&](const ViewRow &row) {
            return view.surface == "import-choice" && !row.subtitle.empty() &&
                           fonts_.regular.measure(row.subtitle, 19.0F) > hint_width
                       ? 26.0F
                       : 0.0F;
        };
        // Every row's place in the list; the list scrolls only when it is taller than the sheet,
        // just enough to keep the focused row and a little of its neighbours in view, and eases there.
        const auto pitch = [&](const ViewRow &row) {
            if (about_sheet && row.kind == RowKind::Status)
                return about_height(row) + 12.0F;
            if (settings_sheet && row.kind == RowKind::Heading)
                return 54.0F + 12.0F;
            if (row.kind == RowKind::Heading || row.kind == RowKind::Separator)
                return 54.0F;
            const bool checkbox = (view.surface == "kept-data" || view.surface == "leftover-titles" || (view.surface == "collections" && row.action.rfind("collection-toggle-", 0) == 0)) &&
                                  row.kind == RowKind::Toggle;
            const bool described = (settings_sheet || checkbox || import_sheet) && !row.subtitle.empty();
            return (described ? 96.0F + hint_extra(row) : 72.0F) + 12.0F;
        };
        std::vector<float> tops;
        float total = 0.0F;
        for (const ViewRow &row : view.rows)
        {
            tops.push_back(total);
            total += pitch(row);
        }
        const std::string scroll_key = view.surface + "\n" + view.title;
        if (scrolls && scroll_key != sheet_scroll_key_)
        {
            sheet_scroll_key_ = scroll_key;
            sheet_scroll_ = sheet_scroll_target_ = 0.0F;
        }
        const float limit = std::max(0.0F, total - 12.0F - bounds.h);
        if (scrolls && view.focused_row >= 0 && static_cast<std::size_t>(view.focused_row) < view.rows.size())
        {
            const auto focused = static_cast<std::size_t>(view.focused_row);
            const float context = 48.0F;  // part of the next row stays visible
            const float top = tops[focused] - context;
            const float bottom = tops[focused] + pitch(view.rows[focused]) - 12.0F + context;
            if (top < sheet_scroll_target_)
                sheet_scroll_target_ = top;
            else if (bottom > sheet_scroll_target_ + bounds.h)
                sheet_scroll_target_ = bottom - bounds.h;
        }
        if (scrolls)
        {
            sheet_scroll_target_ = std::clamp(sheet_scroll_target_, 0.0F, limit);
            sheet_scroll_ = current_state_ && current_state_->reduced_motion ? sheet_scroll_target_
                : sheet_scroll_ + (sheet_scroll_target_ - sheet_scroll_) * std::min(1.0F, frame_dt_ * 14.0F);
            if (std::fabs(sheet_scroll_target_ - sheet_scroll_) < 0.5F)
                sheet_scroll_ = sheet_scroll_target_;
        }
        const float scroll = scrolls ? sheet_scroll_ : 0.0F;
        // The clip reaches the panel's edges (the rows sit 40 px in) so a focused row's glow and
        // zoom are never cut; above and below it leaves room for the glow too.
        overlay_.push_clip({bounds.x - 40.0F, bounds.y - 32.0F, bounds.w + 80.0F, bounds.h + 64.0F});
        ui::Painter painter(overlay_, fonts_, theme_, renderer_.glass_texture());
        for (int index = 0; index < static_cast<int>(view.rows.size()); ++index)
        {
            const ViewRow &row = view.rows[static_cast<std::size_t>(index)];
            float y = bounds.y + tops[static_cast<std::size_t>(index)] - scroll;
            if (y + pitch(row) < bounds.y - 32.0F || y > bounds.y + bounds.h + 32.0F)
                continue;
            if (settings_sheet && row.kind == RowKind::Heading)
            {
                // Collapsible section header: focusable, shows "Show n" or "Hide".
                const gfx::Rect rect{bounds.x, y, bounds.w, 54.0F};
                const float amount = control_focus("sheet-" + row.key, index == view.focused_row);
                focus_glow(overlay_, rect, amount);
                ui::text(overlay_, fonts_.display, ui::upper(row.title), rect.x + 24.0F, rect.y + 35.0F,
                         20.0F, kSecondary, gfx::Align::left, 2.4F);
                ui::text(overlay_, fonts_.regular, row.value, rect.x + rect.w - 24.0F, rect.y + 35.0F, 18.0F,
                         kMuted, gfx::Align::right);
                focus_ring(overlay_, rect, amount);
                y += rect.h + 12.0F;
                continue;
            }
            if (about_sheet && row.kind == RowKind::Status)
            {
                const gfx::Rect rect{bounds.x, y, bounds.w, about_height(row)};
                const float amount = control_focus("sheet-" + row.key, index == view.focused_row);
                focus_glow(overlay_, rect, amount);
                overlay_.rounded_rect(rect, 8.0F, kPanel);
                overlay_.rounded_rect(rect, 8.0F, kText.with_alpha(0.04F));
                const float width = rect.w - 48.0F;
                ui::text(overlay_, fonts_.semibold, fit(fonts_.semibold, overlay_label(row), 19.0F, width),
                         rect.x + 24.0F, rect.y + 36.0F, 19.0F, kSecondary);
                // The facts are disabled rows, but they are what this sheet is for: full ink.
                const float note_y = ui::paragraph(overlay_, fonts_.regular, overlay_value(row), rect.x + 24.0F,
                                                   rect.y + 70.0F, 24.0F, width, 30.0F, kText);
                if (!row.subtitle.empty())
                    ui::paragraph(overlay_, fonts_.regular, row.subtitle, rect.x + 24.0F, note_y + 2.0F, 19.0F, width,
                                  26.0F, row.tone == "problem" ? kRed : kSecondary);
                focus_ring(overlay_, rect, amount);
                continue;
            }
            if (row.kind == RowKind::Heading || row.kind == RowKind::Separator)
            {
                ui::text(overlay_, fonts_.display, ui::upper(row.title), bounds.x + 12.0F, y + 28.0F,
                         18.0F, kMuted, gfx::Align::left, 1.8F);
                y += 54.0F;
                continue;
            }
            // Game settings rows carry an effect line under the name (or the reason a row is
            // disabled), an accent dot when the player changed the value, and a tag.
            // Kept game data: each game is a checkbox row with its kept date under the name.
            const bool checkbox = (view.surface == "kept-data" || view.surface == "leftover-titles" || (view.surface == "collections" && row.action.rfind("collection-toggle-", 0) == 0)) &&
                                  row.kind == RowKind::Toggle;
            const bool described = (settings_sheet || checkbox || import_sheet) && !row.subtitle.empty() && row.kind != RowKind::Heading;
            const gfx::Rect rect{bounds.x, y, bounds.w, described ? 96.0F + hint_extra(row) : 72.0F};
            const float amount = control_focus("sheet-" + row.key, index == view.focused_row);
            focus_glow(overlay_, rect, amount);
            overlay_.rounded_rect(rect, 8.0F, kPanel);
            overlay_.rounded_rect(rect, 8.0F, kText.with_alpha(row.selected ? 0.10F : 0.04F));
            const gfx::Color ink = !row.enabled ? kMuted : row.dangerous ? kRed : kText;
            // A row with its subtitle under the name never repeats it on the right.
            const std::string value = settings_sheet && row.kind != RowKind::Heading ? row.value
                : described && row.value.empty() && row.badge.empty() ? std::string() : overlay_value(row);
            const float value_width = value.empty() ? 0.0F : std::min(rect.w * 0.45F, fonts_.regular.measure(value, 22.0F));
            const float name_y = described ? rect.y + 40.0F : rect.cy() + 10.0F;
            float name_x = rect.x + 24.0F;
            if (checkbox)
            {
                const gfx::Rect box{name_x, name_y - 24.0F, 28.0F, 28.0F};
                if (row.selected)
                {
                    overlay_.rounded_rect(box, 6.0F, kAccent);
                    // A check mark drawn as two strokes.
                    overlay_.line(box.x + 7.0F, box.y + 14.0F, box.x + 12.0F, box.y + 20.0F, 3.0F, theme_.on_primary);
                    overlay_.line(box.x + 12.0F, box.y + 20.0F, box.x + 22.0F, box.y + 8.0F, 3.0F, theme_.on_primary);
                }
                else
                    overlay_.bordered_rect(box, 6.0F, kPanel.with_alpha(0.0F), 2.0F, kText.with_alpha(row.enabled ? 0.45F : 0.18F));
                name_x += 44.0F;
            }
            if (row.modified)
            {
                overlay_.circle(name_x + 5.0F, name_y - 9.0F, 5.0F, kAccent);
                name_x += 20.0F;
            }
            // Game names may need a fallback face (Greek, CJK, Arabic).
            const ui::FontRef &face = font_for(overlay_label(row), fonts_.display);
            const std::string label = fit(face, overlay_label(row), 28.0F, rect.w - 64.0F - value_width);
            ui::text(overlay_, face, label, name_x, name_y, 28.0F, ink);
            if (settings_sheet && !row.badge.empty() && row.kind != RowKind::Heading)
            {
                const float tag_x = name_x + face.measure(label, 28.0F) + 14.0F;
                const float tag_w = fonts_.semibold.measure(ui::upper(row.badge), 13.0F) + 16.0F;
                if (tag_x + tag_w < rect.x + rect.w - 40.0F - value_width)
                {
                    overlay_.rounded_rect({tag_x, name_y - 21.0F, tag_w, 24.0F}, 4.0F, kText.with_alpha(0.08F));
                    ui::text(overlay_, fonts_.semibold, ui::upper(row.badge), tag_x + 8.0F, name_y - 4.0F, 13.0F, kSecondary);
                }
            }
            if (described && view.surface == "import-choice")
                // Under the value's line: the whole width, on two lines when it needs them.
                ui::paragraph(overlay_, fonts_.regular, row.subtitle, rect.x + 24.0F, rect.y + 74.0F, 19.0F, hint_width,
                              26.0F, row.enabled ? kSecondary : kMuted, 2);
            else if (described)
                ui::text(overlay_, fonts_.regular, fit(fonts_.regular, row.subtitle, 19.0F, rect.w - 72.0F - value_width),
                         checkbox ? name_x : rect.x + 24.0F, rect.y + 74.0F, 19.0F,
                         row.tone == "problem" ? kRed : row.tone == "good" ? kGreen : row.enabled ? kSecondary : kMuted);
            if (!value.empty())
                ui::text(overlay_, fonts_.regular, fit(fonts_.regular, value, 22.0F, value_width), rect.x + rect.w - 24.0F,
                         described ? rect.y + 52.0F : rect.cy() + 8.0F, 22.0F,
                         row.modified || row.selected ? kAccent : row.enabled ? kSecondary : kMuted, gfx::Align::right);
            if (row.progress > 0.0F)
                painter.progress({rect.x + 22.0F, rect.y + 60.0F, rect.w - 44.0F, 6.0F}, row.progress);
            focus_ring(overlay_, rect, amount);
            y += rect.h + 12.0F;
        }
        overlay_.pop_clip();
    }

    bool ensure_capture(int width, int height) const
    {
        if (capture_framebuffer_ != 0 && capture_width_ == width && capture_height_ == height)
            return true;
        if (capture_color_ != 0)
            glDeleteRenderbuffers(1, &capture_color_);
        if (capture_framebuffer_ != 0)
            glDeleteFramebuffers(1, &capture_framebuffer_);
        capture_color_ = 0;
        capture_framebuffer_ = 0;
        glGenFramebuffers(1, &capture_framebuffer_);
        glGenRenderbuffers(1, &capture_color_);
        glBindRenderbuffer(GL_RENDERBUFFER, capture_color_);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, width, height);
        glBindFramebuffer(GL_FRAMEBUFFER, capture_framebuffer_);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER,
                                  capture_color_);
        const bool okay = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        if (!okay)
            return false;
        capture_width_ = width;
        capture_height_ = height;
        return true;
    }

    class GlTextureBackend final : public TextureBackend {
      public:
        explicit GlTextureBackend(gfx::Renderer& renderer) : renderer_(renderer) {}
        std::uint32_t Upload(const ArtworkImage& image) override {
            while (glGetError() != GL_NO_ERROR) {}
            std::uint32_t texture = renderer_.batch().create_texture(static_cast<int>(image.width),
                static_cast<int>(image.height), image.rgba.data());
            // A failed allocation still yields a name; report it as a failure so the
            // placeholder stays and the blank texture is not charged to the budget.
            if (texture != 0 && glGetError() != GL_NO_ERROR) {
                glDeleteTextures(1, &texture);
                return 0;
            }
            return texture;
        }
        void Release(std::uint32_t texture) override { glDeleteTextures(1, &texture); }
      private:
        gfx::Renderer& renderer_;
    };

    std::filesystem::path assets_;
    ui::Theme theme_;
    mutable gfx::Renderer renderer_;
    GlTextureBackend texture_backend_;
    std::unique_ptr<LiveArtworkCache> live_artwork_;
    gfx::Font regular_;
    gfx::Font semibold_;
    gfx::Font display_;
    gfx::Font greek_face_;
    gfx::Font cjk_face_;
    gfx::Font arabic_face_;
    ui::Fonts fonts_;
    ui::FontRef greek_;
    ui::FontRef cjk_;
    ui::FontRef arabic_;
    gfx::DrawList scene_;
    gfx::DrawList overlay_;
    FocusMotion focus_motion_;
    Scroll home_scroll_;
    std::map<std::string, Scroll> list_scrolls_;  // Downloads, Saves and Settings lists
    std::map<std::string, int> manage_list_focus_;  // each Manage tab's game row the focus was last on
    std::map<std::string, Scroll> home_rows_;
    float frame_dt_ = 1.0F / 60.0F;
    ui::TabBar tabs_;
    ui::GridView grid_;
    ui::ListView list_;
    ui::Keyboard keyboard_;
    int keyboard_layouts_ = 0;  // 0 the default layouts, 1 a password's (Show), 2 a revealed password's (Hide)
    std::array<std::uint32_t, 8> art_{};
    std::uint32_t hero_ = 0;
    std::string cached_qr_url_;
    QrMatrix cached_qr_;
    std::string cached_qr_error_;
    // The open dialog's link (release notes), drawn as a QR code.
    std::string link_qr_url_;
    QrMatrix link_qr_;
    std::string row_qr_url_;  // Settings › Web access: the page's address
    QrMatrix row_qr_;
    std::vector<const Game *> visible_grid_games_;
    GameId grid_focused_game_;
    const State *current_state_ = nullptr;
    bool simulation_tabs_ = true;
    float clock_ = 0.0F;
    float live_animation_clock_ = 0.0F;
    bool ready_ = false;
    bool has_frame_ = false;
    bool route_focus_ = true;
    OverlayTransition overlay_transition_;
    std::string sheet_scroll_key_;  // the sheet the scroll belongs to
    float sheet_scroll_ = 0.0F;
    float sheet_scroll_target_ = 0.0F;
    int last_width_ = 0;
    int last_height_ = 0;
    mutable GLuint capture_framebuffer_ = 0;
    mutable GLuint capture_color_ = 0;
    mutable int capture_width_ = 0;
    mutable int capture_height_ = 0;
    RenderStats stats_{};
};

Render::Render(std::filesystem::path assets_directory, ArtworkService* artwork)
    : impl_(std::make_unique<Impl>(std::move(assets_directory), artwork))
{
}

Render::~Render() = default;
Render::Render(Render &&) noexcept = default;
Render &Render::operator=(Render &&) noexcept = default;

void Render::draw(Simulation &simulation, int width, int height, float dt)
{
    impl_->draw(simulation, width, height, dt);
}

bool Render::capture_png(const std::filesystem::path &path, int width, int height) const
{
    return impl_->capture_png(path, width, height);
}

RenderStats Render::diagnostics() const noexcept
{
    return impl_->diagnostics();
}

} // namespace playden::homebrew
