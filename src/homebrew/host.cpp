// Playden - macOS SDL/OpenGL host for the direct homebrew UI candidate.
// Copyright (C) 2026 Leo Lobato
// SPDX-License-Identifier: GPL-3.0-or-later

#include "homebrew/render.hpp"
#include "homebrew/artwork_backend.hpp"
#include "homebrew/simulation.hpp"
#include "homebrew/controller.hpp"
#include "steam/install_volumes.h"
#include "stores/local/copy_runner.h"
#include "stores/local/library_source.h"
#include "app/diagnostic_log.h"
#include "runtime/settings_storage.h"
#include "homebrew/input_adapter.hpp"
#include "homebrew/json.hpp"

#include "steam/backend/auth_client.h"
#include "steam/backend/cm_app_details.h"
#include "steam/backend/entitled_library.h"
#include "steam/backend/steam_install_runner.h"
#include "steam/backend/steam_cloud_runner.h"
#include "steam/backend/library_client.h"
#include "steam/local_session_repository.h"
#include "library/cache.h"

#include "core/input.hpp"
#include "gfx/gl.hpp"
#include "gfx/gl_program.hpp"

#include <SDL.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include "homebrew/store_fetch.hpp"
#include <optional>
#include <string>
#include <string_view>

namespace {

// Settings › Game language, read by the install and details workers when they start a job.
std::mutex content_language_lock;
std::string content_language{playden::steam::kDefaultContentLanguage};
std::string selected_content_language() {
    const std::lock_guard<std::mutex> guard(content_language_lock);
    return content_language;
}

struct Options {
    std::string scenario;
    std::filesystem::path profile = PLAYDEN_HOMEBREW_DEFAULT_PROFILE;
    std::filesystem::path data_directory = PLAYDEN_HOMEBREW_DEFAULT_DATA_DIR;
    std::vector<std::filesystem::path> extra_volumes;  // more drives for installs (--extra-volume)
    std::filesystem::path replay;
    std::filesystem::path capture;
    std::filesystem::path snapshot;
    std::filesystem::path recording;
    int width = 1920;
    int height = 1080;
    bool headless = false;
    bool record_motion = false;
    bool simulation = false;
};

void usage(const char* executable) {
    std::fprintf(stderr,
                 "usage: %s [--simulation] [--scenario ID] [--profile PATH] [--replay PATH]\n"
                 "          [--data-dir PATH] [--extra-volume PATH]...\n"
                 "          [--capture PATH.png] [--snapshot PATH|-] [--record-dir PATH] [--headless]\n"
                 "          [--width PX] [--height PX] [--record-motion]\n",
                 executable);
}

bool positive_integer(std::string_view text, int* value) {
    int parsed = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || parsed <= 0 ||
        parsed > 16384)
        return false;
    *value = parsed;
    return true;
}

std::optional<Options> parse_options(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view option(argv[index]);
        if (option == "--headless") {
            options.headless = true;
            continue;
        }
        if (option == "--simulation") {
            options.simulation = true;
            continue;
        }
        if (option == "--record-motion") {
            options.record_motion = true;
            continue;
        }
        if (option == "--help") {
            usage(argv[0]);
            std::exit(0);
        }
        if (index + 1 >= argc) {
            usage(argv[0]);
            return std::nullopt;
        }
        const char* argument = argv[++index];
        if (option == "--scenario") {
            options.scenario = argument;
            options.simulation = true;
        } else if (option == "--profile") {
            options.profile = argument;
            options.simulation = true;
        } else if (option == "--replay") {
            options.replay = argument;
            options.simulation = true;
        } else if (option == "--data-dir")
            options.data_directory = argument;
        else if (option == "--extra-volume")
            options.extra_volumes.emplace_back(argument);
        else if (option == "--capture")
            options.capture = argument;
        else if (option == "--snapshot")
            options.snapshot = argument;
        else if (option == "--record-dir")
            options.recording = argument;
        else if (option == "--width") {
            if (!positive_integer(argument, &options.width)) return std::nullopt;
        } else if (option == "--height") {
            if (!positive_integer(argument, &options.height)) return std::nullopt;
        } else {
            usage(argv[0]);
            return std::nullopt;
        }
    }
    if (!options.capture.empty() || !options.recording.empty()) options.headless = true;
    const bool exposes_simulation_state = options.headless || !options.capture.empty() ||
        !options.snapshot.empty() || !options.recording.empty() || options.record_motion;
    if (exposes_simulation_state && !options.simulation) {
        std::fprintf(stderr, "capture, snapshot, recording, and headless options require --simulation\n");
        usage(argv[0]);
        return std::nullopt;
    }
    if (!options.simulation) {
        std::error_code error;
        options.data_directory = std::filesystem::absolute(options.data_directory, error).lexically_normal();
        if (error) return std::nullopt;
    }
    return options;
}

std::string trim(std::string text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

bool apply_replay(playden::homebrew::Simulation& simulation, const std::filesystem::path& path,
                  const std::function<bool(const std::string&)>& record = {}) {
    if (path.empty()) return true;
    std::ifstream input(path);
    if (!input) {
        std::fprintf(stderr, "could not read replay: %s\n", path.c_str());
        return false;
    }
    std::string line;
    int line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        line = trim(std::move(line));
        if (line.empty() || line[0] == '#') continue;
        const auto separator = line.find_first_of(" \t");
        const std::string command = line.substr(0, separator);
        const std::string argument =
            separator == std::string::npos ? std::string{} : trim(line.substr(separator + 1));
        bool accepted = true;
        if (command == "frame" && record) {
            int frames = 0;
            if (positive_integer(argument, &frames)) {
                for (int frame = 0; frame < frames; ++frame)
                    if (!record(line)) return false;
                continue;
            }
            accepted = false;
        } else if (command == "scenario")
            accepted = !argument.empty() && simulation.scenario(argument);
        else if (command == "action")
            accepted = !argument.empty() && simulation.action(argument);
        else if (command == "text")
            simulation.text(argument);
        else if (command == "tick") {
            int ticks = 1;
            accepted = argument.empty() || positive_integer(argument, &ticks);
            if (accepted) simulation.tick(ticks);
        } else {
            accepted = false;
        }
        if (!accepted) {
            std::fprintf(stderr, "%s:%d: invalid replay command: %s%s%s\n", path.c_str(),
                         line_number, command.c_str(), argument.empty() ? "" : " ",
                         argument.c_str());
            if (!simulation.last_error().empty())
                std::fprintf(stderr, "%s\n", simulation.last_error().c_str());
            return false;
        }
        if (record && !record(line)) return false;
    }
    return true;
}

bool write_snapshot(const playden::homebrew::Simulation& simulation,
                    const std::filesystem::path& path) {
    if (path.empty()) return true;
    const std::string snapshot = simulation.snapshot() + "\n";
    if (path == "-") return std::fwrite(snapshot.data(), 1, snapshot.size(), stdout) == snapshot.size();
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << snapshot;
    return output.good();
}

std::uint32_t navigation_bit(SDL_Keycode key) {
    switch (key) {
    case SDLK_UP: return hui::pad_bits::kUp;
    case SDLK_DOWN: return hui::pad_bits::kDown;
    case SDLK_LEFT: return hui::pad_bits::kLeft;
    case SDLK_RIGHT: return hui::pad_bits::kRight;
    default: return 0;
    }
}

const char* keyboard_action(const SDL_KeyboardEvent& event, bool editing) {
    if (event.repeat != 0) return nullptr;
    if (navigation_bit(event.keysym.sym) != 0) return nullptr;
    if (editing) {
        if (event.keysym.sym == SDLK_RETURN) return "cross";
        if (event.keysym.sym == SDLK_ESCAPE) return "circle";
        if (event.keysym.sym == SDLK_BACKSPACE) return "square";
        return nullptr;
    }
    switch (event.keysym.sym) {
    case SDLK_RETURN:
    case SDLK_SPACE: return "cross";
    case SDLK_ESCAPE: return "circle";
    case SDLK_BACKSPACE: return "square";
    case SDLK_PAGEUP: return "l2";
    case SDLK_PAGEDOWN: return "r2";
    case SDLK_TAB: return (event.keysym.mod & KMOD_SHIFT) != 0 ? "l1" : "r1";
    case SDLK_q: return "l1";
    case SDLK_e: return "r1";
    case SDLK_f: return "triangle";
    case SDLK_x: return "square";
    case SDLK_s:
    case SDLK_SLASH: return "search";
    case SDLK_o: return "options";
    default: return nullptr;
    }
}

void text_key_event(playden::homebrew::Simulation& simulation, const SDL_Event& event,
                    bool& suppress_initiating_text) {
    if (event.type == SDL_KEYDOWN) {
        if (event.key.repeat == 0 && event.key.keysym.sym == SDLK_t &&
            !simulation.state().editor.active) {
            simulation.tick();
        } else if (const char* action = keyboard_action(event.key, simulation.state().editor.active)) {
            const bool was_editing = simulation.state().editor.active;
            simulation.action(action);
            suppress_initiating_text = !was_editing && simulation.state().editor.active &&
                                      event.key.keysym.sym >= SDLK_SPACE &&
                                      event.key.keysym.sym <= SDLK_z;
        }
    } else if (event.type == SDL_KEYUP) {
        suppress_initiating_text = false;
    } else if (event.type == SDL_TEXTINPUT) {
        if (!suppress_initiating_text) simulation.text(event.text.text);
        suppress_initiating_text = false;
    }
}

std::uint8_t axis_byte(Sint16 value) {
    return static_cast<std::uint8_t>(
        (static_cast<std::int32_t>(value) + 32768) * 255 / 65535);
}

hui::PadSample controller_sample(SDL_GameController* controller, std::uint64_t now_us) {
    hui::PadSample sample;
    sample.timestamp_us = now_us;
    sample.connected = controller != nullptr && SDL_GameControllerGetAttached(controller);
    if (!sample.connected) return sample;
    const auto button = [&](SDL_GameControllerButton name, std::uint32_t bit) {
        if (SDL_GameControllerGetButton(controller, name) != 0) sample.buttons |= bit;
    };
    button(SDL_CONTROLLER_BUTTON_DPAD_UP, hui::pad_bits::kUp);
    button(SDL_CONTROLLER_BUTTON_DPAD_DOWN, hui::pad_bits::kDown);
    button(SDL_CONTROLLER_BUTTON_DPAD_LEFT, hui::pad_bits::kLeft);
    button(SDL_CONTROLLER_BUTTON_DPAD_RIGHT, hui::pad_bits::kRight);
    button(SDL_CONTROLLER_BUTTON_A, hui::pad_bits::kCross);
    button(SDL_CONTROLLER_BUTTON_B, hui::pad_bits::kCircle);
    button(SDL_CONTROLLER_BUTTON_X, hui::pad_bits::kSquare);
    button(SDL_CONTROLLER_BUTTON_Y, hui::pad_bits::kTriangle);
    button(SDL_CONTROLLER_BUTTON_LEFTSHOULDER, hui::pad_bits::kL1);
    button(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, hui::pad_bits::kR1);
    button(SDL_CONTROLLER_BUTTON_START, hui::pad_bits::kOptions);
    button(SDL_CONTROLLER_BUTTON_LEFTSTICK, hui::pad_bits::kL3);
    button(SDL_CONTROLLER_BUTTON_RIGHTSTICK, hui::pad_bits::kR3);
#if SDL_VERSION_ATLEAST(2, 0, 14)
    button(SDL_CONTROLLER_BUTTON_TOUCHPAD, hui::pad_bits::kTouchpad);
#endif
    sample.left_x = axis_byte(SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTX));
    sample.left_y = axis_byte(SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTY));
    sample.right_x = axis_byte(SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_RIGHTX));
    sample.right_y = axis_byte(SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_RIGHTY));
    sample.l2 = static_cast<std::uint8_t>(
        std::max<Sint16>(0, SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT)) /
        128);
    sample.r2 = static_cast<std::uint8_t>(
        std::max<Sint16>(0, SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT)) /
        128);
    return sample;
}

const char* action_name(hui::Action action) {
    switch (action) {
    case hui::Action::confirm: return "cross";
    case hui::Action::back: return "circle";
    case hui::Action::north: return "triangle";
    case hui::Action::west: return "square";
    case hui::Action::page_prev: return "l1";
    case hui::Action::page_next: return "r1";
    case hui::Action::jump_prev: return "l2";
    case hui::Action::jump_next: return "r2";
    case hui::Action::menu: return "options";
    case hui::Action::touch: return "search";
    default: return nullptr;
    }
}

void apply_input(playden::homebrew::Simulation& simulation, const hui::InputFrame& frame) {
    switch (frame.nav) {
    case hui::Direction::up: simulation.action("up"); break;
    case hui::Direction::down: simulation.action("down"); break;
    case hui::Direction::left: simulation.action("left"); break;
    case hui::Direction::right: simulation.action("right"); break;
    case hui::Direction::none: break;
    }
    for (unsigned index = static_cast<unsigned>(hui::Action::confirm);
         index < static_cast<unsigned>(hui::Action::count); ++index) {
        const auto action = static_cast<hui::Action>(index);
        if (frame.is_pressed(action)) {
            if (const char* name = action_name(action)) {
                simulation.action(name);
                break;
            }
        }
    }
}

} // namespace

bool write_render_snapshot(const playden::homebrew::Render& renderer,
                           const std::filesystem::path& png) {
    const auto stats = renderer.diagnostics();
    const auto path = png.string() + ".render.json";
    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (!file) return false;
    const int written = std::fprintf(file,
        "{\"grid_focused_game_id\":%u,\"grid_drawn_cell_count\":%zu,\"artwork_texture_count\":%zu,\"native_width\":%d,\"native_height\":%d,\"focused_control_key\":\"%s\"}\n",
        stats.grid_focused_game_id, stats.grid_drawn_cell_count, stats.artwork_texture_count,
        stats.native_width, stats.native_height,
        playden::homebrew::json::escape(stats.focused_control_key).c_str());
    return std::fclose(file) == 0 && written > 0;
}

int main(int argc, char** argv) {
    auto options = parse_options(argc, argv);
    if (!options) return 2;
    if (!options->simulation) {
        // The host CLI explicitly selects this trusted destination. Resolve its
        // existing aliases before choosing the managed root; bottle/JSON paths
        // still use strict no-symlink validation.
        std::error_code error;
        options->data_directory = std::filesystem::weakly_canonical(options->data_directory, error);
        if (error) { std::fprintf(stderr, "live data directory is unavailable\n"); return 2; }
    }

    const auto mode = options->simulation ? playden::homebrew::ModelMode::Simulation
                                          : playden::homebrew::ModelMode::Live;
    playden::homebrew::Simulation simulation(
        options->simulation ? std::filesystem::path(PLAYDEN_HOMEBREW_FIXTURES_DIR)
                            : std::filesystem::path{},
        options->simulation ? options->profile : std::filesystem::path{}, mode);
    {
        std::vector<playden::runtime::SettingsPreset> presets;
        std::vector<playden::runtime::GameRecipe> recipes;
        std::string reason;
        // The kit's DXVK reads dxvk.conf from the game folder (results/20261005-dxvk-conf-probe).
        // Live mode here has no runtime kit, so nothing gated applies.
        playden::runtime::KitCapabilities kit;
        kit.dxvk_config = options->simulation;
        simulation.SetKitCapabilities(kit);
        if (playden::runtime::LoadSettingsData(std::string(PLAYDEN_HOMEBREW_ASSETS_DIR) + "/runtime", &presets, &recipes, &reason))
            simulation.SetSettingsData(std::move(presets), std::move(recipes));
        else
            std::fprintf(stderr, "game settings data unavailable: %s\n", reason.c_str());
    }
    std::unique_ptr<playden::Storage> auth_storage;
    std::unique_ptr<playden::steam::CmClient> auth_cm;
    std::unique_ptr<playden::steam::AuthClient> auth;
    std::unique_ptr<playden::SessionVault> session_vault;  // declared first: every session repository uses it
    std::unique_ptr<playden::LocalSessionRepository> sessions;
    std::unique_ptr<playden::steam::LibraryClient> owned_library;
    std::unique_ptr<playden::LocalSessionRepository> library_sessions;
    std::unique_ptr<playden::Storage> library_storage;
    std::unique_ptr<playden::steam::CmClient> cm_client;
    std::unique_ptr<playden::steam::EntitledLibraryBackend> library_backend;
    playden::steam::LicensedDepots licensed_depots;
    std::unique_ptr<playden::LibraryCache> library_cache;
    std::unique_ptr<playden::LibraryService> library_service;
    std::unique_ptr<playden::PreferencesStore> preferences;
    std::unique_ptr<playden::OrganizationStore> organization;
    std::unique_ptr<playden::app::PersistenceService> persistence;
    std::unique_ptr<playden::steam::CmClient> details_cm;
    std::unique_ptr<playden::steam::CmAppDetailsSource> details_source;
    std::unique_ptr<playden::LocalSessionRepository> details_sessions;
    std::unique_ptr<playden::steam::AppDetailsService> app_details;
    std::unique_ptr<playden::steam::CmClient> install_cm;
    std::unique_ptr<playden::steam::CdnClient> install_cdn;
    std::unique_ptr<playden::steam::SteamInstallRunner> install_runner;
    std::unique_ptr<playden::LocalSessionRepository> install_sessions;
    std::unique_ptr<playden::Storage> import_storage;  // imports.json; declared first: the copy runner and the Local store use it
    std::unique_ptr<playden::stores::local::ImportStore> imports;
    std::unique_ptr<playden::stores::local::LocalCopyRunner> copy_runner;  // declared first: the install service uses it
    std::unique_ptr<playden::steam::InstallService> installs;
    std::unique_ptr<playden::steam::CmClient> cloud_cm;
    std::unique_ptr<playden::steam::HttpClient> cloud_http;
    std::unique_ptr<playden::steam::SteamCloudRunner> cloud_runner;
    std::unique_ptr<playden::LocalSessionRepository> cloud_sessions;
    std::unique_ptr<playden::steam::CloudService> cloud;
    std::unique_ptr<playden::steam::StoreDetailsService> store_details;  // declared first: the controller uses it
    std::unique_ptr<playden::stores::Registry> stores;  // declared first: the controller uses it
    std::unique_ptr<playden::homebrew::Controller> services;
    std::unique_ptr<playden::homebrew::HttpArtworkBackend> artwork_backend;
    std::unique_ptr<playden::homebrew::ArtworkService> artwork;
    if (!options->scenario.empty() && !simulation.scenario(options->scenario)) {
        std::fprintf(stderr, "%s\n", simulation.last_error().c_str());
        return 2;
    }
    if (options->recording.empty() && !apply_replay(simulation, options->replay)) return 2;

    SDL_SetHint(SDL_HINT_VIDEO_HIGHDPI_DISABLED, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        std::fprintf(stderr, "SDL initialization failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    const Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE |
                         (options->headless ? SDL_WINDOW_HIDDEN : SDL_WINDOW_SHOWN);
    SDL_Window* window = SDL_CreateWindow("Playden — ps5-homebrew-ui", SDL_WINDOWPOS_CENTERED,
                                          SDL_WINDOWPOS_CENTERED, options->width, options->height,
                                          flags);
    SDL_GLContext context = window ? SDL_GL_CreateContext(window) : nullptr;
    if (!window || !context) {
        std::fprintf(stderr, "OpenGL 4.1 context creation failed: %s\n", SDL_GetError());
        if (context) SDL_GL_DeleteContext(context);
        if (window) SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    SDL_GL_SetSwapInterval(options->headless ? 0 : 1);
    hui::gfx::set_glsl_prefix("#version 410 core\n");

    if (!options->simulation) {
        std::string settings_reason;
        const auto settings_file = playden::runtime::PrepareGameSettingsStorage(options->data_directory, &settings_reason);
        if (!settings_file) std::fprintf(stderr, "private game settings unavailable: %s\n", settings_reason.c_str());
        simulation.SetGameSettingsFile(settings_file.value_or(std::filesystem::path{}));
        auth_storage = std::make_unique<playden::Storage>(options->data_directory.string());
        auth_cm = std::make_unique<playden::steam::CmClient>(PLAYDEN_HOMEBREW_CA_BUNDLE, *auth_storage);
        auth = std::make_unique<playden::steam::AuthClient>(PLAYDEN_HOMEBREW_CA_BUNDLE);
        auth->SetMachineId([storage = auth_storage.get()] {
            return playden::steam::MakeDeviceIdentity(playden::steam::LoadOrCreateDeviceSeed(*storage)).machine_id;
        });
        auth->SetCmRenewal([cm = auth_cm.get()](const playden::StoredSession& stored, const std::atomic<bool>& cancel) {
            return playden::steam::RenewThroughCm(*cm, stored, cancel);
        });
        const std::string log_directory = (options->data_directory / "logs").string();
        // Same sanitized logs as the console: never tokens or account identity.
        auth->SetFailureReport([log_directory](const char* method, long http_status, int eresult) {
            char line[160];
            std::snprintf(line, sizeof(line), "auth_failure method=%s http=%ld eresult=%d", method, http_status, eresult);
            playden::app::AppendLogLine(log_directory, "auth.log", line);
        });
        auth->SetSignInReport([log_directory](long long persistence, std::uint64_t lifetime_seconds) {
            playden::app::AppendLogLine(log_directory, "auth.log", "signed_in persistence=" + std::to_string(persistence) +
                " lifetime_days=" + std::to_string(lifetime_seconds / 86400));
        });
        // "Stay signed in" is known before the controller restores the session.
        const bool stay_signed_in = playden::PreferencesStore(*auth_storage).Load().preferences.steam_stay_signed_in;
        session_vault = std::make_unique<playden::SessionVault>(stay_signed_in);
        sessions = std::make_unique<playden::LocalSessionRepository>(
            options->data_directory.string(), [log_directory](const std::string& diagnostic) {
                playden::app::AppendLogLine(log_directory, "storage.log", diagnostic);
            }, session_vault.get());
        // Off: a session.v1 left from before is deleted unread.
        if (!stay_signed_in) (void)sessions->Clear();
        owned_library = std::make_unique<playden::steam::LibraryClient>(PLAYDEN_HOMEBREW_CA_BUNDLE);
        library_sessions = std::make_unique<playden::LocalSessionRepository>(options->data_directory.string(), playden::LocalSessionRepository::DiagnosticReport{}, session_vault.get());
        library_storage = std::make_unique<playden::Storage>(options->data_directory.string());
        cm_client = std::make_unique<playden::steam::CmClient>(PLAYDEN_HOMEBREW_CA_BUNDLE, *library_storage);
        library_backend = std::make_unique<playden::steam::EntitledLibraryBackend>(*owned_library, *cm_client);
        library_backend->SetLicensedDepots(&licensed_depots);
        library_cache = std::make_unique<playden::LibraryCache>(*library_storage);
        library_service = std::make_unique<playden::LibraryService>(*library_backend,*library_sessions,library_cache.get());
        preferences=std::make_unique<playden::PreferencesStore>(*library_storage);
        organization=std::make_unique<playden::OrganizationStore>(*library_storage);
        persistence=std::make_unique<playden::app::PersistenceService>(*preferences,*organization);
        details_cm = std::make_unique<playden::steam::CmClient>(PLAYDEN_HOMEBREW_CA_BUNDLE, *library_storage);
        details_source = std::make_unique<playden::steam::CmAppDetailsSource>(*details_cm);
        details_source->SetLicensedDepots(&licensed_depots);
        details_source->SetContentLanguage([] { return selected_content_language(); });
        details_sessions = std::make_unique<playden::LocalSessionRepository>(options->data_directory.string(), playden::LocalSessionRepository::DiagnosticReport{}, session_vault.get());
        app_details = std::make_unique<playden::steam::AppDetailsService>(*details_source, *details_sessions);
        install_cm = std::make_unique<playden::steam::CmClient>(PLAYDEN_HOMEBREW_CA_BUNDLE, *library_storage);
        install_cdn = std::make_unique<playden::steam::CdnClient>(PLAYDEN_HOMEBREW_CA_BUNDLE);
        install_sessions = std::make_unique<playden::LocalSessionRepository>(options->data_directory.string(), playden::LocalSessionRepository::DiagnosticReport{}, session_vault.get());
        install_runner = std::make_unique<playden::steam::SteamInstallRunner>(*install_sessions, *install_cm, *install_cdn);
        install_runner->SetContentLanguage([] { return selected_content_language(); });
        install_runner->SetLog([log_directory](std::uint32_t app, const char* step, const char* category, const char* detail) {
            char line[640];
            if (std::string(step) == "install-removal")
                std::snprintf(line, sizeof(line), "install-removal done app=%u %s", app, detail);
            else
                std::snprintf(line, sizeof(line), "app=%u step=%s result=%s detail=%s", app, step, category, detail);
            playden::app::AppendLogLine(log_directory, "install.log", line);
        });
        std::vector<playden::steam::VolumeCandidate> candidates = {
            {playden::steam::InstallService::kPrimaryVolumeId, "Mac storage", options->data_directory.string(),
             (options->data_directory / "games").string()}};
        for (std::size_t index = 0; index < options->extra_volumes.size(); ++index) {
            const auto& mount = options->extra_volumes[index];
            candidates.push_back({"extra" + std::to_string(index + 1), "Extra drive " + std::to_string(index + 1),
                                  mount.string(), (mount / "playden" / "games").string()});
        }
        // Imported games: imports.json, and "Copy into Playden" as the install service's Local runner.
        import_storage = std::make_unique<playden::Storage>(options->data_directory.string());
        imports = std::make_unique<playden::stores::local::ImportStore>(*import_storage);
        playden::stores::local::LocalCopyOptions copy_options;
        copy_options.log = [log_directory](const std::string& line) {
            playden::app::AppendLogLine(log_directory, "install.log", line);
        };
        copy_runner = std::make_unique<playden::stores::local::LocalCopyRunner>(*imports, std::move(copy_options));
        installs = std::make_unique<playden::steam::InstallService>(
            playden::installs::InstallService::Runners{{playden::Store::kSteam, install_runner.get()},
                                                       {playden::Store::kLocal, copy_runner.get()}},
            playden::steam::DiscoverInstallVolumes(candidates));
        cloud_cm = std::make_unique<playden::steam::CmClient>(PLAYDEN_HOMEBREW_CA_BUNDLE, *library_storage);
        cloud_http = std::make_unique<playden::steam::HttpClient>(PLAYDEN_HOMEBREW_CA_BUNDLE);
        cloud_runner = std::make_unique<playden::steam::SteamCloudRunner>(*cloud_cm, *cloud_http);
        cloud_runner->SetLog([log_directory](std::uint32_t app, const char* event, const std::string& message) {
            playden::app::AppendLogLine(log_directory, "cloud.log", "app=" + std::to_string(app) + " event=" + event + " " + message);
        });
        cloud_sessions = std::make_unique<playden::LocalSessionRepository>(options->data_directory.string(), playden::LocalSessionRepository::DiagnosticReport{}, session_vault.get());
        cloud = std::make_unique<playden::steam::CloudService>(*cloud_runner, *cloud_sessions);
        simulation.SetLogDirectory(options->data_directory / "logs");
        simulation.SetRecipeExportTarget(options->data_directory / "logs", "");
        const auto auth_log = [log_directory](const std::string& line) {
            playden::app::AppendLogLine(log_directory, "auth.log", "sign-in " + line);
        };
        stores = std::make_unique<playden::stores::Registry>();
        stores->Add(std::make_unique<playden::stores::SteamAccount>(*auth, *sessions, auth_log));
        stores->Add(std::make_unique<playden::stores::SteamLibrarySource>(*library_service));
        stores->Add(std::make_unique<playden::stores::SteamDetailsSource>(*app_details));
        // Imported games: <data>/playden-import is the drop folder, playden/import on each extra drive.
        playden::stores::local::LocalLibraryConfig import_config;
        import_config.volumes = {{playden::steam::InstallService::kPrimaryVolumeId, options->data_directory.string(),
                                  "playden-import", false}};
        for (std::size_t index = 0; index < options->extra_volumes.size(); ++index)
            import_config.volumes.push_back({"extra" + std::to_string(index + 1), options->extra_volumes[index].string(),
                                             "playden/import", false});
        import_config.log = [log_directory](const std::string& line) {
            playden::app::AppendLogLine(log_directory, "install.log", line);
        };
        import_config.copy_into_playden = [service = installs.get(), runner = copy_runner.get()](
                                              const playden::stores::local::ImportEntry& entry, const std::string& source,
                                              const std::string& volume_id, std::uint32_t runtime_number) {
            return runner->Enqueue(*service, entry, source, volume_id, runtime_number);
        };
        import_config.move_into_playden = [service = installs.get(), runner = copy_runner.get()](
                                              const playden::stores::local::ImportEntry& entry, const std::string& source,
                                              std::uint32_t runtime_number) {
            return runner->Move(*service, entry, source, runtime_number);
        };
        import_config.remove_copy_folder = [runner = copy_runner.get()](const std::string& root) { return runner->Remove(root).empty(); };
        auto local_library = std::make_unique<playden::stores::local::LocalLibrarySource>(*imports, std::move(import_config));
        auto* local = local_library.get();
        copy_runner->SetChanged([local] { local->Reload(); });
        stores->Add(std::move(local_library));
        services = std::make_unique<playden::homebrew::Controller>(simulation, *stores, persistence.get(), installs.get(),
                                                                   cloud.get(), nullptr, auth_log);
        services->SetLocalLibrary(local);
        services->SetStaySignedIn([repository = sessions.get(), log_directory](bool stay) {
            const auto moved = repository->SetPersist(stay);
            playden::app::AppendLogLine(log_directory, "auth.log", std::string("stay signed in ") + (stay ? "on" : "off") +
                                                                       " moved=" + (moved.ok() ? "1" : "0"));
        });
        store_details = std::make_unique<playden::steam::StoreDetailsService>(playden::homebrew::StoreFetch(PLAYDEN_HOMEBREW_CA_BUNDLE));
        services->SetStoreDetails(store_details.get());
    }

    if (!options->simulation) {
        artwork_backend = std::make_unique<playden::homebrew::HttpArtworkBackend>(PLAYDEN_HOMEBREW_CA_BUNDLE);
        artwork = std::make_unique<playden::homebrew::ArtworkService>(*artwork_backend);
    }
    auto render = std::make_unique<playden::homebrew::Render>(PLAYDEN_HOMEBREW_ASSETS_DIR, artwork.get());
    if (!options->capture.empty() && options->recording.empty()) {
        // Settle presentation motion without advancing the manual fixture clock.
        for (int settle = 0; settle < 16; ++settle)
            render->draw(simulation, options->width, options->height);
    }
    if (!options->recording.empty()) {
        std::error_code error;
        std::filesystem::create_directories(options->recording, error);
        std::ofstream states(options->recording / "states.jsonl");
        unsigned frame_index = 0;
        const auto record = [&](const std::string& command) {
            for (int settle = 0; settle < (options->record_motion ? 1 : 16); ++settle)
                render->draw(simulation, options->width, options->height);
            char filename[32];
            std::snprintf(filename, sizeof(filename), "frame-%04u.png", frame_index++);
            if (!render->capture_png(options->recording / filename, options->width, options->height) ||
                !write_render_snapshot(*render, options->recording / filename) ||
                glGetError() != GL_NO_ERROR) return false;
            states << "{\"command\":\"" << playden::homebrew::json::escape(command)
                   << "\",\"state\":" << simulation.snapshot() << "}\n";
            return states.good();
        };
        if (error || !states || !record("initial") ||
            !apply_replay(simulation, options->replay, record)) {
            render.reset();
            SDL_GL_DeleteContext(context);
            SDL_DestroyWindow(window);
            SDL_Quit();
            return 1;
        }
    }
    SDL_StartTextInput();
    SDL_GameController* controller = nullptr;
    for (int index = 0; index < SDL_NumJoysticks() && !controller; ++index)
        if (SDL_IsGameController(index)) controller = SDL_GameControllerOpen(index);
    hui::InputTracker controller_input;
    hui::InputTracker keyboard_input;
    std::uint32_t keyboard_buttons = 0;
    int width = options->width;
    int height = options->height;
    bool running = true;
    bool rendered_once = false;
    bool was_connected = true;
    bool suppress_initiating_text = false;

    std::uint64_t last_render_us = 0;
    while (running) {
        SDL_Event event{};
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) running = false;
            else if (event.type == SDL_WINDOWEVENT &&
                     event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED && !options->headless) {
                width = event.window.data1;
                height = event.window.data2;
            } else if (event.type == SDL_KEYDOWN) {
                keyboard_buttons |= navigation_bit(event.key.keysym.sym);
                text_key_event(simulation, event, suppress_initiating_text);
            } else if (event.type == SDL_KEYUP) {
                keyboard_buttons &= ~navigation_bit(event.key.keysym.sym);
                text_key_event(simulation, event, suppress_initiating_text);
            } else if (event.type == SDL_CONTROLLERDEVICEADDED && !controller) {
                controller = SDL_GameControllerOpen(event.cdevice.which);
            } else if (event.type == SDL_CONTROLLERDEVICEREMOVED && controller &&
                       SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controller)) ==
                           event.cdevice.which) {
                SDL_GameControllerClose(controller);
                controller = nullptr;
            } else if (event.type == SDL_TEXTINPUT) {
                text_key_event(simulation, event, suppress_initiating_text);
            }
        }

        const auto now_us = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
        const hui::PadSample pad = controller_sample(controller, now_us);
        const auto input = controller_input.update(std::span(&pad, 1), now_us);
        if (!options->headless && input.connected != was_connected) {
            simulation.SetControllerConnected(input.connected);
            was_connected = input.connected;
        }
        apply_input(simulation, input);
        auto diagnostics = playden::homebrew::diagnostic_sample(input);
        if (!input.connected && !simulation.state().editor.active) {
            const auto* keyboard = SDL_GetKeyboardState(nullptr);
            diagnostics.cross = keyboard[SDL_SCANCODE_RETURN];
            diagnostics.circle = keyboard[SDL_SCANCODE_ESCAPE];
            diagnostics.square = keyboard[SDL_SCANCODE_X];
            diagnostics.triangle = keyboard[SDL_SCANCODE_F];
            diagnostics.l1 = keyboard[SDL_SCANCODE_Q]; diagnostics.r1 = keyboard[SDL_SCANCODE_E];
            diagnostics.options = keyboard[SDL_SCANCODE_O];
            diagnostics.left_x = float(keyboard[SDL_SCANCODE_RIGHT]) - float(keyboard[SDL_SCANCODE_LEFT]);
            diagnostics.left_y = float(keyboard[SDL_SCANCODE_DOWN]) - float(keyboard[SDL_SCANCODE_UP]);
        }
        simulation.controller_sample(diagnostics);
        hui::PadSample keys;
        keys.connected = true;
        keys.buttons = keyboard_buttons;
        keys.timestamp_us = now_us;
        apply_input(simulation, keyboard_input.update(std::span(&keys, 1), now_us));
        const float frame_dt = last_render_us == 0 ? 1.0F / 60.0F :
            static_cast<float>(now_us - last_render_us) / 1000000.0F;
        last_render_us = now_us;
        if (services) services->Tick();
        {
            const std::lock_guard<std::mutex> guard(content_language_lock);
            content_language = simulation.state().content_language;
        }
        render->draw(simulation, width, height, options->headless ? 1.0F / 60.0F : frame_dt);
        rendered_once = true;
        if (simulation.state().quit_requested) running = false;

        if (!options->capture.empty()) {
            glFinish();
            if (!render->capture_png(options->capture, width, height) ||
                !write_render_snapshot(*render, options->capture)) {
                std::fprintf(stderr, "PNG capture failed: %s\n", options->capture.c_str());
                running = false;
                rendered_once = false;
                break;
            }
        }
        const GLenum gl_error = glGetError();
        if (gl_error != GL_NO_ERROR) {
            std::fprintf(stderr, "OpenGL error after frame: 0x%x\n", gl_error);
            running = false;
            rendered_once = false;
            break;
        }
        SDL_GL_SwapWindow(window);
        if (options->headless) running = false;
    }

    const bool snapshot_written = write_snapshot(simulation, options->snapshot);
    if (controller) SDL_GameControllerClose(controller);
    SDL_StopTextInput();
    if (services) services->Shutdown();
    // The install service stops before the Local store goes (`stores` is destroyed before
    // `installs`): its copy runner's change callback calls into the Local store. Explicit here, so
    // it doesn't rest on the controller's shutdown or on declaration order.
    if (installs) installs->Shutdown();
    if (copy_runner) copy_runner->SetChanged({});
    if (artwork) artwork->Shutdown();
    render.reset();
    SDL_GL_DeleteContext(context);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return rendered_once && snapshot_written ? 0 : 1;
}
