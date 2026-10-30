// ps5.cpp - Direct Hui comparison lifecycle and controller adapter.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: MIT
#include "homebrew/render.hpp"
#include "homebrew/artwork_backend.hpp"
#include "homebrew/simulation.hpp"
#include "homebrew/store_art.hpp"
#include "homebrew/controller.hpp"
#include "steam/install_volumes.h"
#include "installs/game_move.h"
#include "stores/local/copy_runner.h"
#include "stores/local/library_source.h"
#include "app/diagnostic_log.h"
#include "platform/ps5/title_registry.h"
#include "platform/ps5/title_remover.h"
#include "platform/ps5/title_runtime.h"
#include "platform/ps5/native_startup.h"
#include "runtime/game_probe.h"
#include "runtime/settings_storage.h"
#include "runtime/writable_folders.h"
#include "homebrew/runtime_host.hpp"
#include "homebrew/program_icon.hpp"
#include "homebrew/title_icon.hpp"
#include "platform/ps5/title_allow_list.h"
#include "platform/ps5/console_address.h"
#include "platform/ps5/console_services.h"
#include "platform/ps5/system_keyboard.h"
#include "runtime/artifact_hash.h"
#include "runtime/kit_admission.h"
#include "runtime/kit_trust.h"
#include "update/update_service.h"
#include "web/address.h"
#include "web/server.h"
#include "web/service.h"
#include "homebrew/input_adapter.hpp"
#include "homebrew/usb_keyboard.hpp"
#include "core/input.hpp"
#include "steam/backend/auth_client.h"
#include "steam/backend/cm_app_details.h"
#include "steam/backend/download_server_source.h"
#include "steam/backend/entitled_library.h"
#include "steam/backend/steam_install_runner.h"
#include "steam/backend/steam_cloud_runner.h"
#include "steam/backend/library_client.h"
#include "steam/local_session_repository.h"
#include "library/cache.h"
#include "platform/ps5/display_egl.hpp"
#include "platform/ps5/pad.hpp"
#include "platform/ps5/system.hpp"

#include <array>
#include <atomic>
#include <mutex>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <string>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include "homebrew/store_fetch.hpp"
#include <memory>
#include <optional>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

extern "C" void playden_release_splash(void);
// The native signer requires every referenced export to resolve, including
// the kit's optional weak splash callback. Use the launcher's held splash.
extern "C" void hui_release_splash(void) { playden_release_splash(); }

namespace {
void dispatch(playden::homebrew::Simulation& simulation, const hui::InputFrame& input) {
    switch (input.nav) {
        case hui::Direction::up: simulation.action("up"); break;
        case hui::Direction::down: simulation.action("down"); break;
        case hui::Direction::left: simulation.action("left"); break;
        case hui::Direction::right: simulation.action("right"); break;
        default: break;
    }
    const std::array<std::pair<hui::Action, const char*>, 10> actions{{
        {hui::Action::confirm, "cross"}, {hui::Action::back, "circle"},
        {hui::Action::page_prev, "l1"}, {hui::Action::page_next, "r1"},
        {hui::Action::jump_prev, "l2"}, {hui::Action::jump_next, "r2"},
        {hui::Action::menu, "options"}, {hui::Action::north, "triangle"},
        {hui::Action::west, "square"}, {hui::Action::touch, "search"}
    }};
    // Dispatch one activation edge per frame so opening a modal cannot also
    // activate its default control through another buffered physical edge.
    for (const auto& [action, name] : actions) {
        if (input.is_pressed(action)) { simulation.action(name); break; }
    }
}
}

// Title-to-title launch, as Itemzflow does: the target receives argv as its arguments.
extern "C" {
int sceLncUtilInitialize(void);
uint32_t sceLncUtilLaunchApp(const char* title_id, const char* argv[], playden::ps5::LaunchParameters* param);
uint32_t sceSystemServiceLaunchApp(const char* title_id, const char* argv[], void* param);
int sceSysmoduleLoadModuleInternal(uint32_t id);
int sceSysmoduleLoadModule(uint16_t id);
int sceSysmoduleGetModuleHandleInternal(uint32_t id, int32_t* handle);
int sceKernelDlsym(int32_t handle, const char* name, void** address);
int sceUserServiceGetForegroundUser(uint32_t* user_id);
int sceUserServiceGetInitialUser(int* user_id);
int sceUserServiceInitialize(const void* parameters);
}

namespace {
// A USB keyboard through the system's libSceKeyboard, as prospero-win reads it for games: the
// system loads the module (a title may not load it by path), then its functions are looked up.
constexpr uint16_t kKeyboardModule = 0x0106;

struct UsbKeyboard {
    int32_t handle = -1;
    int (*read_state)(int32_t, void*) = nullptr;
    bool failed = false;
    std::uint64_t retry_us = 0;

    // Opens the keyboard when one is plugged in; tried again every few seconds. Logs the first
    // failure and every success.
    void open(int user, std::uint64_t now_us, const std::string& log_root) {
        if (handle >= 0 || now_us < retry_us) return;
        retry_us = now_us + 3000000;
        int32_t module = -1;
        if (sceSysmoduleGetModuleHandleInternal(kKeyboardModule, &module) != 0 || module < 0) {
            module = -1;
            if (sceSysmoduleGetModuleHandleInternal(0x80000000u | kKeyboardModule, &module) != 0) module = -1;
        }
        int (*init)(void) = nullptr;
        int (*open_keyboard)(int32_t, int32_t, int32_t, void*) = nullptr;
        int init_rc = -1, opened = -1;
        if (module >= 0 && sceKernelDlsym(module, "sceKeyboardInit", reinterpret_cast<void**>(&init)) == 0 &&
            sceKernelDlsym(module, "sceKeyboardOpen", reinterpret_cast<void**>(&open_keyboard)) == 0 &&
            sceKernelDlsym(module, "sceKeyboardReadState", reinterpret_cast<void**>(&read_state)) == 0) {
            init_rc = init();
            uint8_t param[8] = {};
            opened = open_keyboard(user, 0, 0, param);
        }
        if (opened >= 0) handle = opened;
        if (opened >= 0 || !failed) {
            char line[128];
            std::snprintf(line, sizeof(line), "keyboard module=%d init=0x%08x open=0x%08x user=%d", int(module),
                          unsigned(init_rc), unsigned(opened), user);
            playden::app::AppendLogLine(log_root, "launch.log", line);
        }
        failed = opened < 0;
    }

    playden::homebrew::KeyboardReport read() {
        uint8_t state[playden::homebrew::kKeyboardStateBytes] = {};
        if (handle < 0 || read_state(handle, state) != 0) return {};
        return playden::homebrew::ParseKeyboardState(state);
    }
};

std::string title_root = "/app0";
std::string log_root = "/app0/logs";
// Settings › Downloads for slow networks, read by the install worker when a download starts.
std::atomic<bool> downloads_slow_network{false};
// The main loop updates this selection; the install worker copies it at download start.
std::mutex download_server_mutex;
std::string download_server;
// Settings › Game language, read by the install worker when a download starts and by the
// details worker when it estimates sizes. A string needs a lock; the UI writes it once a frame.
std::mutex content_language_lock;
std::string content_language{playden::steam::kDefaultContentLanguage};
std::string selected_content_language() {
    const std::lock_guard<std::mutex> guard(content_language_lock);
    return content_language;
}
// Development requests are processed in file order on the first frame.
std::vector<playden::steam::InstallRequest> take_install_requests(bool runtime_ready) {
    const std::string request_path = title_root + "/dev/install-request.txt";
    FILE* request = std::fopen(request_path.c_str(), "r");
    if (!request) return {};
    std::string text;
    char line[512] = {};
    while (text.size() < 16384 && std::fgets(line, sizeof(line), request)) {
        // Reject an overlong line rather than executing its truncated prefix.
        if (!std::strchr(line, '\n') && !std::feof(request)) {
            int value;
            while ((value = std::fgetc(request)) != '\n' && value != EOF) {}
            continue;
        }
        text += line;
    }
    std::fclose(request);
    const auto requests = playden::steam::ParseInstallRequests(text);
    if (!runtime_ready) {
        playden::app::AppendLogLine(log_root, "install.log", "install-request kept: the game runtime is not available (" +
                                                                 std::to_string(requests.size()) + " commands)");
        return {};
    }
    std::remove(request_path.c_str());
    return requests;
}

// Without the root grant Playden can't reach /data, so no service starts: this screen shows
// until the player quits to Home. A new grant needs a single-threaded title, so it can't be
// retried once the display is open: opening Playden again is the retry.
void show_root_access_missing(hui::ps5::Display& display, const playden::ps5::NativeAccessResult& access) {
    // granted: the grant ran but its data (/data/playden) failed; the screen says so.
    const char* what = access.granted ? "runtime data unusable after the grant" : "root access missing";
    hui::sys::log("Playden %s: %s", what, access.error.c_str());
    playden::app::AppendLogLine("/app0/logs", "launch.log", std::string(what) + ": " + access.error);
    // The pad belongs to a user; the bootstrap may have stopped before UserService started.
    sceUserServiceInitialize(nullptr);
    playden::homebrew::Simulation simulation({}, {}, playden::homebrew::ModelMode::Live);
    simulation.ShowRootAccessMissing(access.error, access.granted);
    playden::homebrew::Render renderer(access.launcher_ready ? access.paths.assets_root : std::string("/app0/assets"));
    hui::ps5::Pad pad;
    pad.open();
    hui::InputTracker tracker;
    std::array<hui::PadSample, 64> samples{};
    bool presented = false;
    while (!simulation.state().quit_requested) {
        const auto count = pad.read(samples);
        dispatch(simulation, tracker.update(std::span<const hui::PadSample>(samples.data(), count),
                                            hui::sys::monotonic_us()));
        renderer.draw(simulation, display.width(), display.height(), 1.0F / 60.0F);
        if (!display.swap()) break;
        if (!presented) {
            hui::sys::hide_splash_screen();
            presented = true;
        }
        pad.tick(1.0F / 60.0F);
    }
}
}  // namespace

int main() {
    // Before the title's /data grant: a title whose credentials changed may fail to load system modules.
    const int keyboard_module = sceSysmoduleLoadModule(kKeyboardModule);
    const int system_keyboard_module = playden::ps5::SystemKeyboard::Preload();
    auto native_access = playden::ps5::PrepareNativeStartup(PLAYDEN_PS5_TITLE_ID);
    hui::ps5::Display display;
    if (display.width() <= 0 && !display.open(3840, 2160)) {
        hui::sys::log("Playden Hui display failed"); hui::sys::park();
    }
#if PLAYDEN_UI_SIMULATION
    // Fixtures need only the package.
    const bool root_access_missing = !native_access.launcher_ready;
#else
    const bool root_access_missing = !native_access.runtime_ready;
#endif
    if (root_access_missing) {
        show_root_access_missing(display, native_access);
        display.close();
        hui::sys::quit();
        return 0;
    }
    const auto& paths = native_access.paths;
    title_root = paths.title_root;
    // After the grant, user data, logs and games live outside the app folder (0.6 layout).
    log_root = playden::ps5::EffectiveLogRoot(paths);
    const std::string user_root = playden::ps5::EffectiveUserRoot(paths);
    const std::string art_cache_dir = paths.runtime_data_root.empty() ? std::string{} : (paths.runtime_data_root + "/art");
    // Startup checkpoints: a launch that never shows its first frame can be told
    // apart from one that never opened the display.
    playden::app::AppendLogLine(log_root, "launch.log", "display open width=" + std::to_string(display.width()) +
                                                        " height=" + std::to_string(display.height()));
    if (!native_access.runtime_ready)
        hui::sys::log("Playden native runtime unavailable: %s", native_access.error.c_str());
    {
#if PLAYDEN_UI_SIMULATION
        mkdir((paths.title_root + "/playden-hui-comparison").c_str(), 0777);
        playden::homebrew::Simulation simulation(
            paths.title_root + "/fixtures/ui-comparison", paths.title_root + "/playden-hui-comparison/profile.json",
            playden::homebrew::ModelMode::Simulation);
#else
        playden::homebrew::Simulation simulation({}, {}, playden::homebrew::ModelMode::Live);
#endif
        {
            std::vector<playden::runtime::SettingsPreset> presets;
            std::vector<playden::runtime::GameRecipe> recipes;
            std::string reason;
#if PLAYDEN_UI_SIMULATION
            // The kit's DXVK reads dxvk.conf from the game folder (results/20261005-dxvk-conf-probe).
            playden::runtime::KitCapabilities kit;
            kit.dxvk_config = true;
            simulation.SetKitCapabilities(kit);
#endif
            // Live: the controller passes on the verified kit's capabilities once the runtime starts.
            if (playden::runtime::LoadSettingsData(paths.title_root + "/runtime", &presets, &recipes, &reason))
                simulation.SetSettingsData(std::move(presets), std::move(recipes));
            else
                playden::app::AppendLogLine(log_root, "launch.log", "game settings data unavailable: " + reason);
        }
#if !PLAYDEN_UI_SIMULATION
        std::string settings_reason;
        const auto settings_file = playden::runtime::PrepareGameSettingsStorage(user_root, &settings_reason);
        if (!settings_file)
            playden::app::AppendLogLine(log_root, "launch.log",
                "private game settings unavailable: " + settings_reason);
        else playden::app::AppendLogLine(log_root, "launch.log", "game settings storage initialized");
        // The layout move held something back this start: say so where the logs are read.
        if (!native_access.notice.empty()) {
            playden::app::AppendLogLine(log_root, "runtime.log", "layout notice: " + native_access.notice);
            simulation.ShowLayoutNotice(native_access.notice);
        }
        // An empty path disables live writes and surfaces a settings load error.
        simulation.SetGameSettingsFile(settings_file.value_or(std::filesystem::path{}));
        playden::Storage auth_storage(user_root);
        // One line per Steam CM logon or refusal, with Steam's result code (#51).
        playden::steam::SetCmLog([](const std::string& line) {
            playden::app::AppendLogLine(log_root, "steam-cm.log", line);
        });
        // One logged-on Steam connection for the whole run, shared by every CM client below (#51).
        const auto steam_cm = playden::steam::MakeCmSharedSession();
        playden::steam::CmClient auth_cm(paths.trust_store, auth_storage);
        auth_cm.SetOwner("auth");
        auth_cm.SetSharedSession(steam_cm);
        playden::steam::AuthClient auth(paths.trust_store);
        // A sign-in names the machine the CM logons come from, in case Steam ties the token to it (saved sign-ins
        // died about a day later without it).
        auth.SetMachineId([&auth_storage] {
            return playden::steam::MakeDeviceIdentity(playden::steam::LoadOrCreateDeviceSeed(auth_storage)).machine_id;
        });
        // Whether Steam kept the new session (its "per" claim) and how long the token says it lasts.
        auth.SetSignInReport([](long long persistence, std::uint64_t lifetime_seconds) {
            playden::app::AppendLogLine(log_root, "auth.log", "signed_in persistence=" + std::to_string(persistence) +
                " lifetime_days=" + std::to_string(lifetime_seconds / 86400));
        });
        auth.SetCmRenewal([&auth_cm](const playden::StoredSession& stored, const std::atomic<bool>& cancel) {
            return playden::steam::RenewThroughCm(auth_cm, stored, cancel);
        });
        // Sanitized: method, HTTP status and X-EResult number; never tokens or account identity.
        auth.SetFailureReport([](const char* method, long http_status, int eresult) {
            char line[160];
            std::snprintf(line, sizeof(line), "auth_failure method=%s http=%ld eresult=%d", method, http_status, eresult);
            playden::app::AppendLogLine(log_root, "auth.log", line);
        });
        auth.SetRestoreReport([](const char* path, const std::string& before, const std::string& after) {
            playden::app::AppendLogLine(log_root, "auth.log", std::string("restored path=") + path + " before " + before +
                " after " + after);
        });
        // "Stay signed in" is known before the controller restores the session. Off, the session lives
        // only in this vault, which all repositories below share.
        const bool stay_signed_in = playden::PreferencesStore(auth_storage).Load().preferences.steam_stay_signed_in;
        playden::SessionVault session_vault(stay_signed_in);
        playden::LocalSessionRepository sessions(user_root, [](const std::string& diagnostic) {
            playden::app::AppendLogLine(log_root, "storage.log", diagnostic);
        }, &session_vault);
        // Off: a session.v1 left from before is deleted unread.
        if (!stay_signed_in) (void)sessions.Clear();
        playden::steam::LibraryClient owned_library(paths.trust_store);
        playden::LocalSessionRepository library_sessions(user_root, {}, &session_vault);
        playden::Storage library_storage(user_root);
        playden::steam::CmClient cm_client(paths.trust_store, library_storage);
        cm_client.SetOwner("library");
        cm_client.SetSharedSession(steam_cm);
        playden::steam::EntitledLibraryBackend library_backend(owned_library, cm_client);
        playden::steam::LicensedDepots licensed_depots;
        library_backend.SetLicensedDepots(&licensed_depots);
        library_backend.SetAppNameStorage(&library_storage);
        // Family sharing diagnostics, with the CM license lines.
        library_backend.SetFamilyDiagnostics(paths.trust_store, [](const std::string& line) {
            playden::app::AppendLogLine(log_root, "steam-cm.log", line);
        });
        playden::LibraryCache library_cache(library_storage);
        playden::LibraryService library_service(library_backend,library_sessions,&library_cache);
        playden::PreferencesStore preferences(library_storage);
        {
            // A download resumed at startup starts before the UI has loaded the settings: it reads the saved ones.
            const auto saved = preferences.Load().preferences;
            downloads_slow_network.store(saved.downloads_slow_network);
            {
                std::lock_guard lock(download_server_mutex);
                download_server = saved.download_server;
            }
            const std::lock_guard<std::mutex> guard(content_language_lock);
            content_language = saved.content_language;
        }
        playden::OrganizationStore organization(library_storage);
        // Each Steam account's last update check, next to its library cache.
        playden::UpdateCheckStore update_checks(library_storage);
        playden::app::PersistenceService persistence(preferences,organization,&update_checks);
        playden::steam::CmClient details_cm(paths.trust_store, library_storage);
        details_cm.SetOwner("details");
        details_cm.SetSharedSession(steam_cm);
        // The install sheet's exact sizes: manifests through the details connection, never written anywhere.
        playden::steam::CmClient sizes_cm(paths.trust_store, library_storage);
        sizes_cm.SetOwner("sizes");
        sizes_cm.SetSharedSession(steam_cm);
        playden::steam::CdnClient details_cdn(paths.trust_store);
        playden::steam::DepotInstaller details_measurer(sizes_cm, details_cdn);
        playden::steam::CmAppDetailsSource details_source(details_cm);
        details_source.SetMeasurer(&details_measurer, &sizes_cm);
        details_source.SetLog([](std::uint32_t app, const char* step, const char* result, const std::string& detail) {
            playden::app::AppendLogLine(log_root, "install.log", "app=" + std::to_string(app) + " step=" + step + " result=" + result + " detail=" + detail);
        });
        details_source.SetLicensedDepots(&licensed_depots);
        details_source.SetContentLanguage([] { return selected_content_language(); });
        playden::LocalSessionRepository details_sessions(user_root, {}, &session_vault);
        playden::steam::AppDetailsService app_details(details_source, details_sessions);
        playden::steam::CmClient install_cm(paths.trust_store, library_storage);
        install_cm.SetOwner("install");
        install_cm.SetSharedSession(steam_cm);
        playden::steam::CdnClient install_cdn(paths.trust_store);
        playden::LocalSessionRepository install_sessions(user_root, {}, &session_vault);
        playden::steam::SteamInstallRunner install_runner(install_sessions, install_cm, install_cdn);
        install_runner.SetTuning([root = paths.title_root] {
            // A developer's tuning file wins over the setting.
            const auto file = root + "/dev/download-tuning.txt";
            if (::access(file.c_str(), F_OK) == 0) return playden::steam::ReadDownloadTuning(file);
            playden::steam::DownloadTuning tuning;
            if (downloads_slow_network.load()) tuning.slots = 8;
            {
                std::lock_guard lock(download_server_mutex);
                tuning.fastest = download_server == playden::steam::kFastestDownloadServer;
                if (!tuning.fastest) tuning.preferred_server = download_server;
            }
            return tuning;
        });
        install_runner.SetContentLanguage([] { return selected_content_language(); });
        install_runner.SetLicensedDepots(&licensed_depots);
        install_runner.SetFreeDlcMemory(&library_storage);
        install_runner.SetLog([](std::uint32_t app, const char* step, const char* category, const char* detail) {
            char line[640];
            if (std::string(step) == "install-removal")
                std::snprintf(line, sizeof(line), "install-removal done app=%u %s", app, detail);
            else
                std::snprintf(line, sizeof(line), "app=%u step=%s result=%s detail=%s", app, step, category, detail);
            playden::app::AppendLogLine(log_root, "install.log", line);
        });
        playden::steam::CmClient cloud_cm(paths.trust_store, library_storage);
        cloud_cm.SetOwner("cloud");
        cloud_cm.SetSharedSession(steam_cm);
        playden::steam::HttpClient cloud_http(paths.trust_store);
        playden::steam::SteamCloudRunner cloud_runner(cloud_cm, cloud_http);
        cloud_runner.SetLog([](std::uint32_t app, const char* event, const std::string& message) {
            playden::app::AppendLogLine(log_root, "cloud.log", "app=" + std::to_string(app) + " event=" + event + " " + message);
        });
        playden::LocalSessionRepository cloud_sessions(user_root, {}, &session_vault);
        playden::steam::CloudService cloud(cloud_runner, cloud_sessions);
        simulation.SetLogDirectory(log_root);
        // Playden's own update: it looks for a newer release on GitHub at startup, stages it on
        // request, and swaps the app (and kit) when Playden quits for it. Start() runs before the
        // runtime admits the kit: an update that did not take puts the previous kit back.
        playden::steam::HttpClient update_http(paths.trust_store);
        playden::update::UpdateConfig update_config;
        update_config.latest_release_url = "https://api.github.com/repos/leolobato/playden-ps5/releases/latest";
        update_config.releases_page = "https://github.com/leolobato/playden-ps5/releases";
        update_config.nightly_page = "https://builds.playden.club/";
        update_config.title_id = PLAYDEN_PS5_TITLE_ID;
        update_config.installed_root = std::string("/data/homebrew/") + PLAYDEN_PS5_TITLE_ID;
        update_config.work_root = "/data/playden/self-update";
        update_config.kits_root = "/data/playden/kits";
        // The kit this build was made for: downloaded at the first start (or over a kit it can't use).
        update_config.kit_id = playden::runtime::kDevelopmentKitId;
        update_config.kit_manifest_sha256 = playden::runtime::kExpectedKitManifestSha256;
        for (const auto& renamed : playden::runtime::kKitIdRenames)
            if (std::string(renamed.to) == playden::runtime::kDevelopmentKitId) update_config.renamed_kit_ids.push_back(renamed.from);
        update_config.appmeta_root = std::string("/user/appmeta/") + PLAYDEN_PS5_TITLE_ID;
        update_config.registered_app_root = std::string("/user/app/") + PLAYDEN_PS5_TITLE_ID;
        update_config.layout_journal = "/data/playden/runtime-state/layout-0.6.json";
        update_config.current = playden::update::CurrentVersion();
        update_config.include_betas = preferences.Load().preferences.beta_updates;
        update_config.fetch = [&update_http](const playden::steam::HttpRequest& request, const std::atomic<bool>& cancel) {
            return update_http.Perform(request, cancel);
        };
        update_config.hash_file = [](const std::string& path) { return playden::runtime::HashArtifactFile(path); };
        // Which kit a folder holds. A newer kit may use a schema or contract this build can't judge;
        // its bytes are vouched for by GitHub's digest and the per-file checks, and the new Playden
        // admits it at startup. Only a kit whose files don't match its own manifest is refused.
        update_config.verify_kit = [](const std::string& root) -> std::optional<std::string> {
            namespace rt = playden::runtime;
            const auto result = rt::VerifyInstalledKit(root, rt::DevelopmentKitAdmission());
            if (result.manifest_sha256.empty() || result.status == rt::KitManifestStatus::kIntegrityFailure)
                return std::nullopt;
            if (result.status != rt::KitManifestStatus::kVerified)
                playden::app::AppendLogLine(log_root, "update.log", "self-update: kit " + root + " status=" +
                    std::to_string(static_cast<int>(result.status)) + " (" + result.reason + ") accepted by digest");
            return result.manifest_sha256;
        };
        // The title's folders under /data and /user are root-owned: renames there need root briefly.
        update_config.as_root = [](const std::function<bool()>& work) { return playden::ps5::WithRoot(work); };
        update_config.free_bytes = [](const std::string& path) -> std::optional<std::uint64_t> {
            const auto space = playden::steam::QueryVolumeSpace(path, playden::steam::kConsoleSystemReserve);
            if (!space) return std::nullopt;
            return space->free_bytes;
        };
        update_config.log = [](const std::string& line) { playden::app::AppendLogLine(log_root, "update.log", line); };
        update_config.local_release_dir = paths.title_root + "/dev/update-local";
        playden::update::UpdateService self_update(std::move(update_config));
        playden::app::AppendLogLine(log_root, "update.log",
                                    "self-update: Playden " + playden::update::CurrentVersion().ToString() + " started");
        self_update.Start();
        self_update.Check(false);
        // "Export as recipe" writes next to the logs, which are read over FTP already.
        simulation.SetRecipeExportTarget(log_root, playden::ps5::ConsoleFirmwareVersion().value_or(""));
        // Per-game runtime: private bottles and PS5 titles from the admitted kit.
        // Its worker owns OWNER records, native inventory and publication.
        std::unique_ptr<playden::homebrew::RuntimeHost> runtime_host;
        if (native_access.runtime_ready && !paths.runtime_data_root.empty()) {
            namespace rt = playden::runtime;
            playden::homebrew::RuntimeHostConfig config;
            config.boundary = "/data";
            config.state_root = std::filesystem::path(paths.runtime_data_root) / "runtime-state";
            config.homebrew_root = "/data/homebrew";
            config.layout_paths = rt::LayoutPaths{paths.title_root, "/data/playden/user", "/data/playden/logs",
                                                   "/data/playden-games", "/data/playden/runtime-state", "/data"};
            config.as_root = [](const std::function<bool()>& work) { return playden::ps5::WithRoot(work); };
            config.writable_folders = {"/data/homebrew", "/data/playden", "/data/playden-games", "/data/playden/user",
                                       "/data/playden/logs"};
            config.registry_transport = playden::ps5::ShadowMountPost;
            config.admit_kit = [](std::string* reason) -> std::optional<playden::homebrew::RuntimeKit> {
                const auto firmware = playden::ps5::ConsoleFirmwareVersion();
                playden::app::AppendLogLine(log_root, "runtime.log", "console_firmware=" + firmware.value_or("unknown") + " " +
                    playden::ps5::ConsoleFirmwareDiagnostics() + " " +
                    playden::ps5::TitleRegistry(playden::ps5::ShadowMountPost).DescribeVersion());
                const auto admission = rt::DevelopmentKitAdmission();
                auto admitted = rt::VerifyInstalledKit(rt::kDevelopmentKitRoot, admission);
                std::string ignored;
                for (const auto& name : admitted.ignored_capabilities) ignored += (ignored.empty() ? "" : ",") + name;
                const auto wrapper = admitted.sources.find("wrapper");
                const auto revision = wrapper != admitted.sources.end() && wrapper->second.count("revision")
                    ? wrapper->second.at("revision") : std::string{};
                playden::app::AppendLogLine(log_root, "runtime.log", std::string("kit_admission root=") +
                    rt::kDevelopmentKitRoot + " status=" + std::to_string(static_cast<int>(admitted.status)) + " reason=" + admitted.reason +
                    " manifest=" + admitted.manifest_sha256 + " expected=" + admission.expected_manifest_sha256 +
                    " prospero_win=" + revision + (admitted.source_dirty ? "-dirty" : "") +
                    " ignored_capabilities=" + (ignored.empty() ? "-" : ignored));
                if (admitted.status != rt::KitManifestStatus::kVerified || !admitted.view) {
                    *reason = "The runtime kit is missing or incomplete.";
                    return std::nullopt;
                }
                return playden::homebrew::RuntimeKit{*admitted.view, admitted.capabilities, admitted.manifest_sha256,
                                                     admission.expected_manifest_sha256, revision, admitted.source_dirty};
            };
            config.package_operations.hash_file = rt::HashArtifactFile;
            config.package_operations.hash_bytes = rt::HashArtifactBytes;
            config.package_operations.free_bytes = [](const std::filesystem::path& path) -> std::optional<std::uint64_t> {
                const auto space = playden::steam::QueryVolumeSpace(path.string(), playden::steam::kConsoleSystemReserve);
                return space ? std::optional<std::uint64_t>{space->free_bytes} : std::nullopt;
            };
            config.package_operations.free_space_unverifiable = !playden::steam::VolumeSpaceSupported();
            config.package_operations.validate_bottle = [](const std::filesystem::path& bottle) {
                std::error_code error;
                for (const char* name : {"system.reg", "user.reg", "userdef.reg"})
                    if (!std::filesystem::is_regular_file(bottle / name, error)) return false;
                return true;
            };
            config.runtime_dir = paths.title_root + "/runtime";
            config.settings_record = [file = settings_file](const playden::GameId& game) -> std::optional<rt::GameSettingsRecord> {
                std::map<playden::GameId, rt::GameSettingsRecord> records; std::string reason;
                if (!file || !rt::LoadGameSettingsFile(file->string(), &records, &reason)) return std::nullopt;
                const auto found = records.find(game);
                return found == records.end() ? std::nullopt : std::optional<rt::GameSettingsRecord>(found->second);
            };
            config.log = [](const std::string& line) { playden::app::AppendLogLine(log_root, "runtime.log", line); };
            config.title_log = [](const std::string& title, const std::string& text) {
                const auto path = log_root + "/title-" + title + ".log";
                if (FILE* file = std::fopen(path.c_str(), "wb")) {
                    std::fwrite(text.data(), 1, text.size(), file);
                    std::fclose(file);
                }
            };
            // The game's PS5 title icon, from its Steam cover; without one, its program's own
            // icon, then the packaged generic icon. Only a missing generic icon leaves the kit's.
            std::optional<std::string> generic_icon;
            if (FILE* file = std::fopen((paths.assets_root + "/generic-title-icon.png").c_str(), "rb")) {
                std::string bytes;
                char chunk[4096];
                for (std::size_t count; (count = std::fread(chunk, 1, sizeof(chunk), file)) > 0;)
                    bytes.append(chunk, count);
                std::fclose(file);
                if (!bytes.empty()) generic_icon = std::move(bytes);
            }
            // Owned DLC: the game's looked-up DLC that the last license read of this account grants.
            config.owned_dlc = [&library_backend, &licensed_depots](std::uint64_t steam_id, std::uint32_t app_id) {
                std::vector<std::uint32_t> owned;
                const auto grant = licensed_depots.Get(steam_id);
                if (!grant) return owned;
                for (const auto dlc : library_backend.DlcOf(app_id))
                    if (grant->apps.count(dlc)) owned.push_back(dlc);
                return owned;
            };
            config.title_icon = [trust = paths.trust_store, art_cache_dir, generic_icon](
                                    const playden::GameId& game, const std::filesystem::path& program) -> std::optional<std::string> {
                // A store with no cover for the game (an imported one) gets the generic icon.
                const auto source = playden::homebrew::CoverUrl(game);
                std::optional<playden::homebrew::ArtworkImage> cover;
                if (!source.empty()) {
                    playden::homebrew::HttpArtworkBackend backend(trust, art_cache_dir);
                    const std::atomic<bool> cancel{false};
                    cover = backend.Load({1, game, source}, cancel);
                }
                if (auto icon = cover ? playden::homebrew::MakeTitleIcon(*cover) : std::nullopt) return icon;
                // An imported game, or a Steam game without a cover: the icon inside its program.
                const auto own = program.empty() ? std::nullopt : playden::homebrew::ReadProgramIcon(program.string());
                if (auto icon = own ? playden::homebrew::MakeProgramTitleIcon(*own) : std::nullopt) {
                    playden::app::AppendLogLine(log_root, "runtime.log", "title icon app=" + playden::ToKey(game) +
                                                " uses the program's icon: no cover program=" + program.filename().string());
                    return icon;
                }
                playden::app::AppendLogLine(log_root, "runtime.log", "title icon app=" + playden::ToKey(game) +
                                            (generic_icon ? " uses the generic icon: no cover or program icon"
                                                          : " has no cover, program icon or generic icon"));
                return generic_icon;
            };
            // What Playden downloads for a game's bottle (Microsoft's Visual C++ redistributable, for games that need
            // MFC): ranged HTTPS GETs on the worker, with its own client made on first use. Redirects are followed by
            // hand to other HTTPS addresses; the bytes are checked against their pinned SHA-256 afterwards.
            config.fetch_range = [client = std::make_shared<std::unique_ptr<playden::steam::HttpClient>>(),
                                  trust = paths.trust_store](const std::string& url, std::uint64_t offset,
                                                             std::uint64_t length, const std::atomic<bool>& cancel,
                                                             std::string* error) -> std::optional<std::string> {
                if (!*client) *client = std::make_unique<playden::steam::HttpClient>(trust);
                std::string target = url;
                for (int hop = 0; hop <= 5; ++hop) {
                    playden::steam::HttpRequest request;
                    request.url = target;
                    request.headers = {{"User-Agent", "Playden"},
                                       {"Range", "bytes=" + std::to_string(offset) + "-" + std::to_string(offset + length - 1)}};
                    request.timeout_ms = playden::steam::kMaxHttpTimeoutMs;
                    request.max_response_bytes = static_cast<std::size_t>(length) + 1;
                    const auto response = (*client)->Perform(request, cancel);
                    const long status = response.status_code;
                    if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
                        target.clear();
                        for (const auto& [name, value] : response.headers)
                            if (strcasecmp(name.c_str(), "location") == 0) target = value;
                        if (target.rfind("https://", 0) != 0) {
                            *error = "redirected to an address Playden doesn't follow";
                            return std::nullopt;
                        }
                        continue;
                    }
                    if (!response.ok() || (status != 206 && !(status == 200 && offset == 0)) ||
                        response.body.size() != length) {
                        *error = "status " + std::to_string(status) + " error " +
                                 std::to_string(static_cast<int>(response.error)) + " bytes " +
                                 std::to_string(response.body.size());
                        return std::nullopt;
                    }
                    return std::string(response.body.begin(), response.body.end());
                }
                *error = "too many redirects";
                return std::nullopt;
            };
            // An any-title kit's helper serves the game titles listed here (root-owned).
            config.write_title_allow_list = playden::ps5::WriteTitleAllowList;
            config.hold_effective_uid = playden::ps5::HoldEffectiveUid;
            config.try_hold_effective_uid = [](bool* busy) {
                auto held = playden::ps5::TryHoldEffectiveUid();
                *busy = !held;
                return held;
            };
            // Play: start a prepared title with its session argument. LncUtil is
            // resolved through the system service module (its static imports are null).
            config.launch_title = [](const std::string& title, const std::vector<std::string>& arguments) {
                static auto api = [] {
                    auto resolved = playden::ps5::ResolveTitleApi(
                        {sceSysmoduleLoadModuleInternal, sceSysmoduleGetModuleHandleInternal, sceKernelDlsym},
                        sceUserServiceGetForegroundUser);
                    if (!resolved.launch && !resolved.public_launch) {
                        resolved.public_launch = sceSystemServiceLaunchApp;
                        resolved.foreground_user = sceUserServiceGetForegroundUser;
                    }
                    return resolved;
                }();
                static playden::ps5::TitleBackend backend(api);
                const auto result = backend.Launch(title, arguments);
                char line[160];
                std::snprintf(line, sizeof(line), "launch_title title=%s stage=%s accepted=%d result=0x%08x",
                              title.c_str(), result.stage.c_str(), result.ok, unsigned(result.code));
                playden::app::AppendLogLine(log_root, "runtime.log", line);
                return result;
            };
            // Uninstall removes the game's PS5 title through Playden's one-shot title
            // remover payload, which the payload loader runs outside this title.
            config.unregister_title = [title_root = paths.title_root](const std::string& title, const std::string& nonce) {
                playden::ps5::TitleRemoverConfig remover;
                remover.payload = title_root + "/title-remover.elf";
                remover.send = playden::ps5::SendToPayloadLoader;
                const auto outcome = playden::ps5::RunTitleRemover(remover, title, nonce);
                return playden::homebrew::TitleRemoverResult{outcome.sent, outcome.code, outcome.detail};
            };
            runtime_host = std::make_unique<playden::homebrew::RuntimeHost>(std::move(config));
            if (!runtime_host->Start())
                playden::app::AppendLogLine(log_root, "runtime.log", "runtime worker could not start");
        }
        if (runtime_host && !runtime_host->WaitForStartup()) {
            native_access.runtime_ready = false;
            native_access.error = runtime_host->Snapshot().reason;
            show_root_access_missing(display, native_access);
            display.close();
            hui::sys::quit();
            return 0;
        }
        auto games_root = playden::ps5::EffectiveGamesRoot(paths);
        if (const auto journal = playden::runtime::LoadLayoutJournal("/data/playden/runtime-state");
            journal && (journal->step == "games-moved" || journal->step == "done"))
            games_root = "/data/playden-games";
        // A console-formatted M.2's root belongs to root: Playden's folder there is made as root.
        auto install_volumes = playden::steam::DiscoverInstallVolumes(
            playden::steam::ConsoleVolumeCandidates(paths.title_root, games_root),
            [](const std::function<bool()>& work) { return playden::ps5::WithRoot(work); });
        for (const auto& volume : install_volumes)
            playden::app::AppendLogLine(log_root, "install.log", "volume id=" + volume.id + " root=" + volume.games_root +
                                        " writable=" + (volume.available ? "1" : "0"));
        // imports.json, declared before the install service and the registry: the copy runner and the
        // Local store read it until they stop.
        std::unique_ptr<playden::Storage> import_storage;
        std::unique_ptr<playden::stores::local::ImportStore> imports;
        std::unique_ptr<playden::stores::local::LocalCopyRunner> copy_runner;
        if (!paths.runtime_data_root.empty()) {
            import_storage = std::make_unique<playden::Storage>(
                (std::filesystem::path(paths.runtime_data_root) / "runtime-state").string());
            imports = std::make_unique<playden::stores::local::ImportStore>(*import_storage);
            // "Copy into Playden": imported games become managed installs.
            playden::stores::local::LocalCopyOptions copy_options;
            copy_options.log = [](const std::string& line) { playden::app::AppendLogLine(log_root, "install.log", line); };
            // A game set up to play from its folder keeps its PS5 title: the title follows the copy,
            // or the move's rename runs under the title's lease (RuntimeHost::Relocate).
            copy_options.relocate = [host = runtime_host.get(), volumes = install_volumes](
                                        const playden::stores::local::LocalRelocation& relocation) {
                using Local = playden::stores::local::LocalRelocationResult;
                using Runtime = playden::homebrew::RuntimeRelocationResult;
                Local answer;
                if (!host) {
                    answer.outcome = Local::Outcome::kBusy;
                    answer.message = "The runtime services are unavailable.";
                    return answer;
                }
                playden::homebrew::RuntimeRelocation request;
                request.runtime_number = relocation.runtime_number;
                request.to = relocation.root;
                request.volume_id = relocation.volume_id;
                if (request.volume_id.empty())
                    for (const auto& volume : volumes)
                        if (relocation.root.rfind(volume.games_root + "/", 0) == 0) request.volume_id = volume.id;
                request.move_files = relocation.move_files;
                const auto result = host->Relocate(std::move(request), std::chrono::minutes(2));
                answer.outcome = result.outcome == Runtime::Outcome::kDone   ? Local::Outcome::kDone
                                 : result.outcome == Runtime::Outcome::kBusy ? Local::Outcome::kBusy
                                                                             : Local::Outcome::kFailed;
                answer.message = result.message;
                return answer;
            };
            copy_options.as_root = [](const std::function<bool()>& work) { return playden::ps5::WithRoot(work); };
            copy_runner = std::make_unique<playden::stores::local::LocalCopyRunner>(*imports, std::move(copy_options));
        }
        // Moving installed games between drives: the game's PS5 title follows the copy under its publication
        // lease (RuntimeHost::Relocate), as for an imported game's copy.
        playden::installs::GameMoveOptions move_options;
        move_options.log = [](const std::string& line) { playden::app::AppendLogLine(log_root, "install.log", line); };
        move_options.hold_effective_uid = playden::ps5::HoldEffectiveUid;
        move_options.relocate = [host = runtime_host.get()](const playden::installs::GameRelocation& relocation) {
            using Move = playden::installs::GameRelocationResult;
            using Runtime = playden::homebrew::RuntimeRelocationResult;
            Move answer;
            if (!host) {
                answer.outcome = Move::Outcome::kBusy;
                answer.message = "The runtime services are unavailable.";
                return answer;
            }
            // Without its number the game's title can't be found: nothing moves yet.
            const auto number = host->RuntimeNumberFor(relocation.game);
            if (!number) {
                answer.outcome = Move::Outcome::kBusy;
                answer.message = "Playden is busy with this game. Try again in a moment.";
                return answer;
            }
            playden::homebrew::RuntimeRelocation request;
            request.runtime_number = *number;
            request.to = relocation.to;
            request.volume_id = relocation.volume_id;
            request.move_files = relocation.move_files;
            const auto result = host->Relocate(std::move(request), std::chrono::minutes(2));
            answer.outcome = result.outcome == Runtime::Outcome::kDone   ? Move::Outcome::kDone
                             : result.outcome == Runtime::Outcome::kBusy ? Move::Outcome::kBusy
                                                                         : Move::Outcome::kFailed;
            answer.message = result.message;
            return answer;
        };
        playden::installs::GameMover game_mover(std::move(move_options));
        playden::installs::InstallService::Runners install_runners{{playden::Store::kSteam, &install_runner}};
        if (copy_runner) install_runners[playden::Store::kLocal] = copy_runner.get();
        playden::steam::InstallService installs(std::move(install_runners), std::move(install_volumes));
        installs.SetMoveRunner(&game_mover);
        // Installed games' records, for their updates. Set before the controller asks for details; its
        // Shutdown joins the details worker before the install service stops.
        details_source.SetInstalled([&installs](std::uint32_t app) -> std::optional<playden::installs::InstalledRecord> {
            const auto snapshot = installs.Snapshot();
            for (const auto& game : snapshot.installed)
                if (game.id == playden::GameId::Steam(app) && game.available) return game.record;
            return std::nullopt;
        });
        details_source.SetInstallRoot([&installs](std::uint32_t app) -> std::optional<std::string> {
            const auto snapshot = installs.Snapshot();
            for (const auto& game : snapshot.installed)
                if (game.id == playden::GameId::Steam(app) && game.available) return game.root;
            return std::nullopt;
        });
        playden::steam::CmClient servers_cm(paths.trust_store, library_storage);
        servers_cm.SetOwner("servers");
        servers_cm.SetSharedSession(steam_cm);
        playden::steam::CdnClient servers_cdn(paths.trust_store);
        playden::LocalSessionRepository servers_sessions(user_root, {}, &session_vault);
        playden::steam::CdnDownloadServerSource servers_source(servers_cm, servers_cdn, [&installs] {
            const auto snapshot = installs.Snapshot();
            const playden::steam::InstalledGame* newest = nullptr;
            for (const auto& game : snapshot.installed)
                if (game.available && game.id.steam_app() && (!newest || game.finished_at > newest->finished_at)) newest = &game;
            return newest ? *newest->id.steam_app() : 0u;
        });
        playden::steam::DownloadServerService servers(servers_source, servers_sessions);
        servers.SetLog([](std::uint32_t app, const char* step, const char* category, const char* detail) {
            char line[640];
            std::snprintf(line, sizeof(line), "app=%u step=%s result=%s detail=%s", app, step, category, detail);
            playden::app::AppendLogLine(log_root, "install.log", line);
        });
        // A games folder made over FTP is often root's and 0755: checked (and opened up) as a download starts.
        // Only the games folder itself: the primary volume's parent is /data, never chmodded.
        installs.SetFolderCheck([](const std::string& games_root) {
            const std::filesystem::path root(games_root);
            return playden::runtime::EnsureWritableFolders(
                {root},
                [](const std::function<bool()>& work) { return playden::ps5::WithRoot(work); },
                [](const std::string& line) { playden::app::AppendLogLine(log_root, "install.log", line); });
        });
        const auto auth_log = [](const std::string& line) { playden::app::AppendLogLine(log_root, "auth.log", "sign-in " + line); };
        playden::stores::Registry stores;
        stores.Add(std::make_unique<playden::stores::SteamAccount>(auth, sessions, auth_log));
        stores.Add(std::make_unique<playden::stores::SteamLibrarySource>(library_service));
        stores.Add(std::make_unique<playden::stores::SteamDetailsSource>(app_details));
        // Imported games: imports.json next to installs.json in the runtime state folder; their
        // folders in /data/playden-import and playden/import on each drive (a drive counts while mounted).
        playden::stores::local::LocalLibrarySource* local_library = nullptr;
        if (imports) {
            playden::stores::local::LocalLibraryConfig import_config;
            import_config.log = [](const std::string& line) { playden::app::AppendLogLine(log_root, "install.log", line); };
            // A drop-folder game's root-owned FTP files are opened (0777/0666) at Add and before each Play.
            playden::stores::local::DropFolderAccessOptions drop_access;
            drop_access.as_root = [](const std::function<bool()>& work) { return playden::ps5::WithRoot(work); };
            drop_access.log = import_config.log;
            import_config.open_drop_folder = playden::stores::local::MakeDropFolderOpener(std::move(drop_access));
            import_config.copy_into_playden = [&installs, runner = copy_runner.get()](
                                                  const playden::stores::local::ImportEntry& entry, const std::string& source,
                                                  const std::string& volume_id, std::uint32_t runtime_number) {
                return runner->Enqueue(installs, entry, source, volume_id, runtime_number);
            };
            import_config.move_into_playden = [&installs, runner = copy_runner.get()](
                                                  const playden::stores::local::ImportEntry& entry, const std::string& source,
                                                  std::uint32_t runtime_number) {
                return runner->Move(installs, entry, source, runtime_number);
            };
            import_config.remove_copy_folder = [runner = copy_runner.get()](const std::string& root) { return runner->Remove(root).empty(); };
            import_config.game_program = [](const std::string& folder, const std::string& program) {
                return playden::runtime::UnrealGameProgram(folder, program);
            };
            auto source = std::make_unique<playden::stores::local::LocalLibrarySource>(*imports, std::move(import_config));
            local_library = source.get();
            copy_runner->SetChanged([local_library] { local_library->Reload(); });
            stores.Add(std::move(source));
        }
        // Web access, phase 0: with /data/playden/dev/web-probe present, a bare listener on 7031 answers every
        // request with "ok" and logs each socket call to web.log, to learn whether a title can listen at all.
        playden::web::Server web_probe;
        const bool web_probe_on = !paths.runtime_data_root.empty() &&
                                  access((paths.runtime_data_root + "/dev/web-probe").c_str(), F_OK) == 0;
        if (web_probe_on) {
            const auto web_log = [](const std::string& line) { playden::app::AppendLogLine(log_root, "web.log", "probe " + line); };
            std::string netctl;
            const auto netctl_address = playden::ps5::ConsoleAddress(&netctl);
            const auto udp_address = playden::web::LocalAddress();
            web_log("address udp=" + (udp_address.empty() ? std::string("unknown") : udp_address) + " netctl=" +
                    (netctl_address.empty() ? std::string("unknown") : netctl_address) + " " + netctl);
            playden::web::ServerOptions probe;
            probe.first_port = probe.last_port = 7031;
            probe.verbose = true;
            probe.log = web_log;
            std::string error;
            if (web_probe.Start(probe, [](const playden::web::Request&, playden::web::Response& response) {
                    response.Send(200, "text/plain", "ok\n");
                }, &error))
                web_log("listening port=" + std::to_string(web_probe.port()));
        }
        // Web access: the logs page on the local network (off while the probe holds port 7031).
        playden::web::ServiceConfig web_config;
        web_config.paths = {log_root, "/data/homebrew", paths.runtime_data_root.empty() ? std::string() : paths.runtime_data_root + "/kits",
                            "/data/homebrew/" PLAYDEN_PS5_TITLE_ID};
        web_config.server.log = [](const std::string& line) { playden::app::AppendLogLine(log_root, "web.log", line); };
        web_config.address_fallback = [] { return playden::ps5::ConsoleAddress(); };
        playden::web::Service web(std::move(web_config));
        playden::homebrew::Controller services(simulation, stores, &persistence, &installs, &cloud, runtime_host.get(), auth_log);
        services.SetDownloadServers(&servers);
        if (!web_probe_on) services.SetWebAccess(&web);
        services.SetLocalLibrary(local_library);
        playden::steam::StoreDetailsService store_details(playden::homebrew::StoreFetch(paths.trust_store));
        services.SetStoreDetails(&store_details);
        services.SetSelfUpdate(&self_update);
        // A game bought or added to the library elsewhere: Steam pushes the new license list.
        services.SetLicenseChanges([steam_cm] { return playden::steam::CmLicenseChanges(*steam_cm); });
        services.SetShutdownLog([](const std::string& line) { playden::app::AppendLogLine(log_root, "launch.log", line); });
        // "Saved UI data is unavailable" names no file or reason: the log does.
        services.SetPersistenceLog([](const std::string& line) { playden::app::AppendLogLine(log_root, "launch.log", line); });
        services.SetStaySignedIn([&sessions](bool stay) {
            const auto moved = sessions.SetPersist(stay);
            playden::app::AppendLogLine(log_root, "auth.log", std::string("stay signed in ") + (stay ? "on" : "off") +
                                                                  " moved=" + (moved.ok() ? "1" : "0"));
        });
#endif
#if PLAYDEN_UI_SIMULATION
        playden::homebrew::Render renderer(paths.assets_root);
#else
        playden::homebrew::HttpArtworkBackend artwork_backend(paths.trust_store, art_cache_dir);
        // Games without store art show the icon inside their program, read from the game's files.
        playden::homebrew::ProgramIconArtworkBackend program_icons(artwork_backend);
        playden::homebrew::ArtworkService artwork(program_icons);
        playden::homebrew::Render renderer(paths.assets_root, &artwork);
#endif
        hui::ps5::Pad pad;
        pad.open();
        {
            // The controller Playden reads belongs to the initial user; log both
            // users so a controller that does nothing can be explained.
            int initial = -1;
            uint32_t foreground = 0;
            const int initial_rc = sceUserServiceGetInitialUser(&initial);
            const int foreground_rc = sceUserServiceGetForegroundUser(&foreground);
            char line[160];
            std::snprintf(line, sizeof(line), "pad users initial=%d rc=0x%08x foreground=%u rc=0x%08x", initial,
                          unsigned(initial_rc), unsigned(foreground), unsigned(foreground_rc));
            playden::app::AppendLogLine(log_root, "launch.log", line);
        }
        int keyboard_user = -1;
        sceUserServiceGetInitialUser(&keyboard_user);
        playden::app::AppendLogLine(log_root, "launch.log", "keyboard preload status=" + std::to_string(keyboard_module));
        char system_keyboard_line[64];
        std::snprintf(system_keyboard_line, sizeof(system_keyboard_line), "system keyboard preload status=0x%08x",
                      unsigned(system_keyboard_module));
        playden::app::AppendLogLine(log_root, "launch.log", system_keyboard_line);
        UsbKeyboard keyboard;
        playden::homebrew::KeyboardInput keyboard_input;
        // Text fields open the system keyboard, so Remote Play's keyboard can type into them;
        // Playden's own keyboard stays for a field the system keyboard refused.
        playden::ps5::SystemKeyboard system_keyboard;
        std::uint64_t offered_editor = 0;
        simulation.SetSystemKeyboardAvailable(system_keyboard_module == 0);
        hui::InputTracker tracker;
        std::array<hui::PadSample, 64> samples{};
        bool presented = false;
        bool was_connected = true;
        std::vector<playden::GameId> dev_prepares;  // imported games the request file asked to prepare
        std::uint64_t dev_prepares_until = 0;
        std::uint64_t last_render_us = 0;
        int slow_frames = 0;
        for (;;) {
            const auto now = hui::sys::monotonic_us();
            const auto count = pad.read(samples);
            const auto input = tracker.update(std::span<const hui::PadSample>(samples.data(), count), now);
            if (input.connected != was_connected) {
                simulation.SetControllerConnected(input.connected);
                was_connected = input.connected;
            }
            const auto& editor = simulation.state().editor;
            // The frame the dialog closes in carries the press that closed it (Done, Enter, Escape): it
            // does not reach Playden, or it would also press the row under the field.
            bool dialog_closed = false;
            if (system_keyboard.active()) {
                // The field went away (a sign-in finished, for example): so does the dialog.
                if (!editor.active || editor.serial != offered_editor) system_keyboard.Close();
                const auto state = system_keyboard.Poll();
                dialog_closed = state != playden::ps5::SystemKeyboard::State::kOpen;
                if (dialog_closed) simulation.SetSystemKeyboardOpen(false);
                if (state == playden::ps5::SystemKeyboard::State::kAccepted) {
                    simulation.SubmitEditorText(system_keyboard.Take());
                    // A value the field refused (a duplicate name, say) leaves it open: offer it again.
                    if (simulation.state().editor.active) {
                        offered_editor = 0;
                        simulation.SetSystemKeyboardOpen(true);
                    }
                }
                else if (state == playden::ps5::SystemKeyboard::State::kCancelled && editor.active) simulation.action("circle");
                if (state != playden::ps5::SystemKeyboard::State::kOpen)
                    playden::app::AppendLogLine(log_root, "launch.log", "system keyboard closed state=" + std::to_string(static_cast<int>(state)));
            } else if (editor.active && editor.serial != offered_editor && !input.is_held(hui::Action::confirm) &&
                       !input.is_held(hui::Action::menu)) {
                offered_editor = editor.serial;
                std::string refused;
                const bool opened = system_keyboard.Open(editor.purpose, editor.value, editor.secret, &refused);
                simulation.SetSystemKeyboardOpen(opened);
                playden::app::AppendLogLine(log_root, "launch.log", (opened ? std::string("system keyboard open") : "system keyboard refused") +
                                                                        (refused.empty() ? std::string() : " " + refused));
            }
            keyboard.open(keyboard_user, now, log_root);
            // A field waiting for the button that opened it to come up already belongs to the system keyboard.
            const bool awaiting_dialog = editor.active && editor.system_keyboard && editor.serial != offered_editor;
            if (system_keyboard.active() || awaiting_dialog || dialog_closed) {
                // The dialog owns input; keys typed meanwhile go to it, not to Playden. The USB keyboard's
                // state is still followed, so a key held across the close is not read as a new press.
                keyboard_input.Follow(keyboard.read());
            } else {
                dispatch(simulation, input);
                // A USB keyboard types into the open text box and also moves around Playden.
                for (const auto& command : keyboard_input.Update(keyboard.read(), simulation.state().editor.active, now)) {
                    if (!command.text.empty()) simulation.text(command.text);
                    else simulation.action(command.action);
                }
            }
            simulation.controller_sample(playden::homebrew::diagnostic_sample(input));
            const auto dispatched = hui::sys::monotonic_us();
#if !PLAYDEN_UI_SIMULATION
            services.Tick();
            // Until the UI has the saved settings, its defaults would replace them.
            if (simulation.PreferencesLoaded()) {
                downloads_slow_network.store(simulation.state().downloads_slow_network);
                {
                    std::lock_guard lock(download_server_mutex);
                    download_server = simulation.state().download_server;
                }
                const std::lock_guard<std::mutex> guard(content_language_lock);
                content_language = simulation.state().content_language;
            }
#endif
            const auto ticked = hui::sys::monotonic_us();
            if (simulation.state().quit_requested) break;
            const float frame_dt = last_render_us == 0 ? 1.0F / 60.0F :
                static_cast<float>(now - last_render_us) / 1000000.0F;
            last_render_us = now;
            renderer.draw(simulation, display.width(), display.height(), frame_dt);
            const auto drawn = hui::sys::monotonic_us();
            if (!display.swap()) break;
            // Frames over 50 ms are logged with where the time went (milliseconds), at most a few hundred per run.
            if (const auto swapped = hui::sys::monotonic_us(); presented && swapped - now > 50000 && slow_frames < 400) {
                ++slow_frames;
                std::string line = "slow frame ms=" + std::to_string((swapped - now) / 1000) +
                                   " input=" + std::to_string((dispatched - now) / 1000) +
                                   " tick=" + std::to_string((ticked - dispatched) / 1000) +
                                   " draw=" + std::to_string((drawn - ticked) / 1000) +
                                   " swap=" + std::to_string((swapped - drawn) / 1000);
#if !PLAYDEN_UI_SIMULATION
                if (const auto profile = services.LastTickProfile(); !profile.empty()) line += " [" + profile + "]";
#endif
                playden::app::AppendLogLine(log_root, "frames.log", line);
            }
            if (!presented) {
                hui::sys::hide_splash_screen();
                presented = true;
                playden::app::AppendLogLine(log_root, "launch.log", "first frame presented");
#if !PLAYDEN_UI_SIMULATION
                for (const auto& request : take_install_requests(native_access.runtime_ready)) {
                    using Action = playden::steam::InstallRequest::Action;
                    using playden::homebrew::ImportRequest;
                    using playden::homebrew::ImportRequestKind;
                    const char* action = "queued";
                    const auto& game = request.game;
                    if (request.action == Action::kCopy) {
                        action = "copy";
                        simulation.RequestImport({ImportRequestKind::CopyIntoPlayden, game, request.volume, {}, {}});
                    } else if (request.action == Action::kMove && game.store != playden::Store::kLocal) {
                        // An installed game to another drive.
                        const auto refused = installs.Move(game, request.volume);
                        action = refused.empty() ? "move-installed" : "move-refused";
                        if (!refused.empty())
                            playden::app::AppendLogLine(log_root, "install.log", "install-request move refused: " + refused);
                    } else if (request.action == Action::kMove) {
                        action = "move";
                        simulation.RequestImport({ImportRequestKind::MoveIntoPlayden, game, {}, {}, {}});
                    } else if (request.action == Action::kImport) {
                        // The same path as the Import sheet's add: the Local store checks the folder and program.
                        action = "import";
                        simulation.RequestImport({ImportRequestKind::Add, {}, request.volume, request.folder, request.program});
                    } else if (game.store == playden::Store::kLocal) {
                        if (request.action == Action::kUninstall) {
                            action = "remove";
                            simulation.RequestImport({ImportRequestKind::Remove, game, {}, {}, {}});
                        } else {
                            // Prepared once the Local store has said where the game's folder is.
                            action = "prepare";
                            dev_prepares.push_back(game);
                        }
                    } else if (request.action == Action::kUninstall) { action = "uninstall"; installs.Uninstall(game); }
                    else if (request.action == Action::kCancel) { action = "cancel"; installs.Cancel(game); }
                    else installs.Install(game);
                    playden::app::AppendLogLine(log_root, "install.log", "install-request " + std::string(action) +
                                               (request.action == Action::kImport
                                                    ? " volume=" + request.volume + " folder=" + request.folder +
                                                          " program=" + request.program
                                                    : " game=" + playden::ToKey(game)));
                }
                dev_prepares_until = now + 60ull * 1000000ull;
#endif
            }
#if !PLAYDEN_UI_SIMULATION
            // dev/install-request.txt's imported games to prepare, once their folder is known (a minute at most).
            if (!dev_prepares.empty()) {
                for (auto it = dev_prepares.begin(); it != dev_prepares.end();) {
                    if (!simulation.state().runtime_games.count(*it)) { ++it; continue; }
                    simulation.RequestRuntimePrepare(*it);
                    it = dev_prepares.erase(it);
                }
                if (now > dev_prepares_until) {
                    for (const auto& game : dev_prepares)
                        playden::app::AppendLogLine(log_root, "install.log", "install-request prepare dropped game=" +
                                                                                 playden::ToKey(game) + ": no folder to play from");
                    dev_prepares.clear();
                }
            }
#endif
            // Work advances only through the documented manual fixture clock.
            pad.tick(1.0F / 60.0F);
        }
#if !PLAYDEN_UI_SIMULATION
        playden::app::AppendLogLine(log_root, "launch.log", "quit requested");
        if (web_probe.running()) {
            web_probe.Stop();
            playden::app::AppendLogLine(log_root, "web.log", "probe stopped");
        }
        services.Shutdown();
        web.Shutdown();
        // The install service stops before the Local store goes (`stores` is destroyed before
        // `installs`): its copy runner's change callback calls into the Local store. Explicit here,
        // so it doesn't rest on the controller's shutdown or on declaration order.
        servers.Shutdown();
        installs.Shutdown();
        if (copy_runner) copy_runner->SetChanged({});
        playden::app::AppendLogLine(log_root, "launch.log", "shutdown artwork start");
        artwork.Shutdown();
        playden::app::AppendLogLine(log_root, "launch.log", "shutdown self-update start");
        self_update.Shutdown();
        playden::app::AppendLogLine(log_root, "launch.log", "shutdown done");
        if (self_update.Snapshot().restart_requested || self_update.Snapshot().rollback_requested) {
            // The relaunch payload starts first: it waits until Playden has gone, then opens it again.
            std::string elf, error;
            if (FILE* file = std::fopen((paths.title_root + "/self-update-relaunch.elf").c_str(), "rb")) {
                char buffer[4096];
                std::size_t n;
                while ((n = std::fread(buffer, 1, sizeof buffer, file)) > 0) elf.append(buffer, n);
                std::fclose(file);
            }
            const std::string request = "/data/playden/self-update/relaunch-title";
            FILE* title = std::fopen(request.c_str(), "w");
            const bool requested = title && std::fputs(PLAYDEN_PS5_TITLE_ID "\n", title) >= 0;
            if (title) std::fclose(title);
            const bool sent = !elf.empty() && requested && playden::ps5::SendToPayloadLoader(elf, &error);
            playden::app::AppendLogLine(log_root, "update.log", std::string("self-update: relaunch payload ") +
                                                                    (sent ? "sent" : "not sent: " + (elf.empty() ? std::string("missing") : error)));
            const bool swapped = self_update.ApplyAtQuit();
            playden::app::AppendLogLine(log_root, "update.log", std::string("self-update: quit ") +
                                                                    (swapped ? "with the new files in place" : "without changing the app"));
        }
#endif
    }
    display.close();
    hui::sys::quit();
}
