#include "homebrew/render.hpp"
#include "app/library_controller.h"
#include "app/persistence_controller.h"
#include <algorithm>
#include <cstdlib>
#include <unistd.h>
#include <chrono>
#include <thread>
#include "homebrew/simulation.hpp"
#include "gfx/gl_program.hpp"
#include <SDL.h>
#include <cassert>
#include <filesystem>
#include "steam_id.h"

namespace {
using namespace playden;
struct Sessions final : SessionRepository {
  Result<std::optional<StoredSession>> Load() const override {
    return Result<std::optional<StoredSession>>::Success(StoredSession{123,"Synthetic account","synthetic-refresh","synthetic-access",{}});
  }
  Result<void> Save(const StoredSession&) const override { return Result<void>::Success(); }
  Result<void> Clear() const override { return Result<void>::Success(); }
};
struct Backend final : steam::LibraryBackend {
  std::vector<GameSummary> games;
  steam::LibraryResult Fetch(const StoredSession&,const std::atomic<bool>&) override {
    return {steam::LibraryError::kNone,games,false};
  }
};
struct Cache final : LibraryRepository {
  mutable std::optional<LibrarySnapshot> snapshot;
  Result<LibrarySnapshot> Load(std::uint64_t id) const override {
    assert(id==123);
    return snapshot ? Result<LibrarySnapshot>::Success(*snapshot)
        : Result<LibrarySnapshot>::Failure(StorageError::kNotFound,"missing");
  }
  Result<void> Save(std::uint64_t id,const LibrarySnapshot& value) const override {
    assert(id==123);snapshot=value;return Result<void>::Success();
  }
  Result<void> Clear(std::uint64_t) const override { snapshot.reset();return Result<void>::Success(); }
};
}

// Settings: Up and Down stay in a section; another section is reached through the rail.
static void enter_settings_section(playden::homebrew::Simulation& model, int index) {
  using playden::homebrew::Route;
  if (model.state().navigation.route != Route::Settings || !model.state().navigation.modals.empty()) return;
  const auto& rows = model.view().rows;
  const std::string wanted = rows[static_cast<std::size_t>(index)].section;
  const auto here = [&] { return rows[static_cast<std::size_t>(model.view().focused_row)].section; };
  if (here() == wanted) return;
  std::vector<std::string> order;
  for (const auto& row : rows) if (order.empty() || order.back() != row.section) order.push_back(row.section);
  const auto position = [&](const std::string& section) { return std::find(order.begin(), order.end(), section) - order.begin(); };
  model.action("left");
  for (int step = 0; step < 16 && here() != wanted; ++step) model.action(position(here()) < position(wanted) ? "down" : "up");
  model.action("right");
}
int main(int argc,char** argv) {
  using namespace playden::homebrew;
  if(argc!=2) return 2;
  assert(SDL_Init(SDL_INIT_VIDEO)==0);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,4);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,1);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,SDL_GL_CONTEXT_PROFILE_CORE);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS,SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
  auto* window=SDL_CreateWindow("Synthetic live library verification",0,0,1920,1080,
                                 SDL_WINDOW_OPENGL|SDL_WINDOW_HIDDEN);
  assert(window);
  auto context=SDL_GL_CreateContext(window); assert(context);
  hui::gfx::set_glsl_prefix("#version 410 core\n");
  const std::filesystem::path output(argv[1]);
  std::filesystem::create_directories(output);
  {
    Simulation model({}, {},ModelMode::Live);
    model.SetWallClock(1700000000);
    model.UpdateAccount({AuthState::SignedIn,"123","Synthetic account",{},{},true});
    playden::GameSummary first{SteamId(42),"Synthetic library title",{},"Known synthetic description","Example studio"};
    first.playtime_minutes=91; first.last_played_unix_seconds=1699827200;
    Sessions sessions;Backend backend;Cache cache;
    backend.games={first,{SteamId(43),"Café on the coast",{},{},{}},{SteamId(44),"Αθήνα",{},{},{}}};
    playden::LibraryService service(backend,sessions,&cache,[]{return 1700000000;});
    playden::app::LibraryController controller(model,service);
    auto settle=[&] {
      const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
      do { controller.Tick(); if(!model.state().library_loading) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      } while(std::chrono::steady_clock::now()<deadline);
      assert(false && "graphical library service deadline");
    };
    controller.Activate(123,false);settle();
    assert(model.state().games.size()==3 && model.state().account().cache_available);
    Render renderer(PLAYDEN_HOMEBREW_ASSETS_DIR);
    auto capture=[&](const char* name) {
      for(int i=0;i<16;++i) renderer.draw(model,1920,1080);
      assert(renderer.capture_png(output/name,1920,1080));
    };
    capture("home.png");
    {
      Simulation missing({}, {}, ModelMode::Live);
      missing.ShowRootAccessMissing("native data grant failed (code 2); helper stage=request status=unavailable error=2");
      for(int i=0;i<16;++i) renderer.draw(missing,1920,1080);
      assert(renderer.capture_png(output/"root-access-missing.png",1920,1080));
      assert(renderer.diagnostics().focused_control_key=="root-quit");
    }
    assert(model.action("r1") && model.state().navigation.route==Route::Library);
    capture("library.png");
    assert(model.action("search"));model.text("CAFÉ");assert(model.action("done"));
    assert(model.visible_games().size()==1 && model.visible_games().front()->id==SteamId(43));
    capture("search.png");
    assert(model.action("search"));model.text(" discarded");assert(model.action("circle"));
    assert(model.state().library.query=="CAFÉ" && model.visible_games().size()==1);
    assert(model.action("search") && model.action("clear"));model.text("no result");assert(model.action("done"));
    assert(model.visible_games().empty());
    assert(model.action("search") && model.action("clear") && model.action("done"));
    assert(model.visible_games().size()==3);
    // The first Library focus can be its sidebar. Move right into its grid.
    if(model.state().navigation.library_rail_focused) assert(model.action("right"));
    assert(model.action("cross") && model.state().navigation.route==Route::Details);
    capture("details.png");
    {
      // Playden's own update: the offer with its release page as a QR code, the download on
      // Downloads, and "What's new" after the update.
      Simulation updating({}, {}, ModelMode::Live);
      updating.SetWallClock(1700000000);
      updating.UpdateAccount({AuthState::SignedIn,"123","Synthetic account",{},{},true});
      assert(updating.UpdateLibrary({backend.games,false,{},false,false,{}}));
      auto capture_update=[&](const char* name) {
        for(int i=0;i<16;++i) renderer.draw(updating,1920,1080);
        assert(renderer.capture_png(output/name,1920,1080));
      };
      LiveSelfUpdate offer;
      offer.current="0.2.0"; offer.phase="available"; offer.available="0.3.0";
      offer.notes="Playden updates itself from GitHub releases. Downloads resume after a restart, and the Saves tab "
                  "shows which games are in sync.";
      offer.notes_url="https://github.com/leolobato/playden-ps5/releases/tag/v0.3.0";
      offer.download_bytes=486'000'000; offer.kit_needed=true;
      updating.UpdateSelfUpdate(offer);
      assert(!updating.state().navigation.modals.empty() && !updating.state().navigation.modals.back().link.empty());
      capture_update("update-available.png");
      assert(updating.action("cross"));
      offer.phase="downloading"; offer.part="kit"; offer.done=208'000'000; offer.total=437'000'000;
      offer.bytes_per_second=26'400'000; offer.eta_seconds=8;
      updating.UpdateSelfUpdate(offer);
      for(int i=0;i<4 && updating.state().navigation.route!=Route::Downloads;++i) assert(updating.action("r1"));
      capture_update("update-downloading.png");
      LiveSelfUpdate fresh;
      fresh.current="0.3.0"; fresh.current_notes=offer.notes;
      fresh.current_notes_url=offer.notes_url;
      fresh.finished_message="Playden was updated to 0.3.0."; fresh.whats_new=true;
      updating.UpdateSelfUpdate(fresh);
      assert(updating.state().navigation.modals.back().title=="What's new in Playden 0.3.0");
      capture_update("whats-new.png");
      assert(updating.action("cross") && updating.state().navigation.modals.empty());
      fresh.finished_message.clear(); fresh.whats_new=false; fresh.current_notes.clear();
      updating.UpdateSelfUpdate(fresh);
      for(int i=0;i<4 && updating.state().navigation.route!=Route::Settings;++i) assert(updating.action("r1"));
      assert(updating.action("focus:settings-release-notes") && updating.action("cross"));
      capture_update("release-notes-short.png");
      // About: every fact in full, the kit's notes under its rows.
      while(!updating.state().navigation.modals.empty()) assert(updating.action("circle"));
      fresh.current="0.8.0-nightly.20261027.2"; fresh.commit="e7a30f21e8";
      updating.UpdateSelfUpdate(fresh);
      updating.SetKitInfo({"playden-kit",std::string(64,'d'),std::string(64,'e'),std::string(40,'f'),true,{},"0.13-dev.20261024.3"});
      assert(updating.action("focus:settings-about") && updating.action("cross"));
      assert(updating.view().surface=="about" && updating.view().rows.size()==5);
      assert(updating.view().rows[2].value=="playden-kit 0.13-dev.20261024.3 · dddddddddd" &&
             updating.view().rows[2].subtitle=="This version of Playden expects eeeeeeeeee");
      assert(updating.action("down") && updating.action("down") && updating.action("down"));
      assert(updating.view().rows[static_cast<std::size_t>(updating.view().focused_row)].key=="about-kit-source");
      capture_update("about.png");
    }
    {
      // Steam's options name no clear program: setting up asks which one starts the game.
      Simulation live({}, {}, ModelMode::Live);
      live.UpdateAccount({AuthState::SignedIn,"123","Synthetic account",{},{},true});
      assert(live.UpdateLibrary({backend.games,false,{},false,false,{}}));
      live.UpdateInstalls({}, {{SteamId(42), 600'000'000}});
      LiveRuntimeGame waiting; waiting.game_id=SteamId(42); waiting.status="Not prepared"; waiting.can_prepare=true;
      live.UpdateRuntime(RuntimeState::Ready, {waiting});
      assert(live.action("details:42"));
      playden::homebrew::LiveGameSetup setup;
      setup.steam_choices={{"x64/Launcher64.exe",false},{"x86/Launcher.exe",false},{"Redist/Setup.exe",true}};
      live.SetGameSetups({{SteamId(42),setup}});
      assert(live.view().surface=="steam-programs");
      for(int i=0;i<16;++i) renderer.draw(live,1920,1080);
      assert(renderer.capture_png(output/"steam-programs.png",1920,1080));
      assert(live.action("focus:cancel") && live.action("cross"));
      for(int i=0;i<16;++i) renderer.draw(live,1920,1080);
      assert(renderer.capture_png(output/"steam-programs-play.png",1920,1080));
    }
    {
      // An installed, prepared game: Play is the same height as the other actions,
      // in either state, and the uninstall dialog's choice fits its button.
      Simulation live({}, {}, ModelMode::Live);
      live.SetWallClock(1700000000);
      live.UpdateAccount({AuthState::SignedIn,"123","Synthetic account",{},{},true});
      assert(live.UpdateLibrary({backend.games,false,{},false,false,{}}));
      live.UpdateAppDetails(SteamId(42), 300'000'000, 600'000'000, true);
      live.UpdateInstalls({}, {{SteamId(42), 600'000'000}});
      LiveRuntimeGame ready; ready.game_id= SteamId(42); ready.status="Ready"; ready.ready=true; ready.can_play=true; ready.has_title=true;
      live.UpdateRuntime(RuntimeState::Ready, {ready});
      assert(live.action("details:42"));
      auto capture_live=[&](const char* name) {
        for(int i=0;i<16;++i) renderer.draw(live,1920,1080);
        assert(renderer.capture_png(output/name,1920,1080));
      };
      capture_live("details-play.png");
      ready.can_play=false; ready.play_status="Starting…";
      live.UpdateRuntime(RuntimeState::Ready, {ready});
      capture_live("details-play-disabled.png");
      {
        LiveRuntimeGame copying; copying.game_id= SteamId(42); copying.status="Preparing"; copying.has_title=true;
        copying.detail="Copying the Windows environment"; copying.progress=0.42F;
        live.UpdateRuntime(RuntimeState::Ready, {copying});
        capture_live("details-preparing-progress.png");
      }
      {
        // Prepared and waiting for the PS5: Finish setup explains quitting to Home.
        LiveRuntimeGame waiting; waiting.game_id= SteamId(42); waiting.status="Waiting for the PS5"; waiting.has_title=true;
        waiting.awaiting_registration=true; waiting.can_prepare=true;
        waiting.detail="Waiting for the PS5, which adds the game to Home when Playden quits";
        live.UpdateRuntime(RuntimeState::Ready, {waiting});
        capture_live("details-ready-to-play.png");
        live.NotifyReadyToPlay(SteamId(42));
        capture_live("ready-to-play-prompt.png");
        assert(live.action("focus:ready-later") && live.action("cross"));
      }
      // The details state gallery covers button widths, hint colours and the seventh fact.
      for (const auto& [status, filename] : std::vector<std::pair<std::string, std::string>>{
               {"Not prepared", "details-not-prepared.png"}, {"Kit update available", "details-kit-update.png"},
               {"Needs repair", "details-needs-repair.png"}, {"Needs recovery", "details-interrupted.png"},
               {"Failed", "details-failed.png"}, {"Registration pending", "details-not-confirmed.png"},
               {"Removal incomplete", "details-removing.png"}, {"No kit", "details-no-kit.png"}}) {
        LiveRuntimeGame state; state.game_id= SteamId(42); state.status=status;
        state.can_prepare=status!="No kit"; state.has_title=status!="No kit" && status!="Not prepared";
        if (status=="Needs recovery" || status=="Failed") state.detail="insufficient verified free space for package copy";
        live.UpdateRuntime(RuntimeState::Ready, {state});
        capture_live(filename.c_str());
      }
      ready.can_play=true; ready.play_status.clear();
      live.UpdateRuntime(RuntimeState::Ready, {ready});
      assert(live.action("focus:details-more") && live.action("cross"));
      assert(live.action("focus:uninstall") && live.action("cross"));
      capture_live("uninstall-dialog.png");
      assert(live.action("focus:uninstall-delete-saves") && live.action("cross"));
      capture_live("uninstall-dialog-delete-saves.png");
      // Uninstalled with its saves kept, on a console with two drives: Details says so beside Install.
      while(!live.state().navigation.modals.empty()) assert(live.action("circle"));
      live.UpdateVolumes({{"internal","Internal",true,{},{}},{"usb0","USB drive",true,{},{}}}, "usb0");
      live.UpdateInstalls({}, {});
      live.UpdateRuntime(RuntimeState::Ready, {});
      live.UpdateKeptBottles({{SteamId(42), "Synthetic library title", 1700000000, 450'000'000}});
      assert(live.action("details:42"));
      capture_live("details-kept.png");
      live.UpdateVolumes({{"internal","Console storage",true,674'000'000'000ull,156'000'000'000ull},
                          {"m2","M.2 SSD",true,1'000'000'000'000ull,249'000'000'000ull}}, "m2");
      capture_live("details-free-space.png");
      // Too little room on the default drive: Install opens the sheet, and another drive is for this game only.
      live.UpdateVolumes({{"internal","Console storage",true,674'000'000'000ull,156'000'000'000ull},
                          {"m2","M.2 SSD",true,1'000'000'000'000ull,64'000'000ull}}, "m2");
      assert(live.action("focus:details-install") && live.action("cross"));
      assert(live.TakeInstallRequests().empty());
      capture_live("install-low-space.png");
      assert(live.action("focus:storage-picker") && live.action("cross"));
      capture_live("install-picker.png");
      assert(live.action("focus:volume-internal") && live.action("cross"));
      capture_live("install-other-drive.png");
      assert(live.TakeInstallRequests().empty());
      assert(live.action("circle"));
      // Settings: kept game data is one row that opens the manager with a checkbox per game.
      live.UpdateKeptBottles({{SteamId(42), "Synthetic library title", 1700000000, 450'000'000},
                              {SteamId(43), "Café on the coast", 1699000000, 1'200'000'000}});
      while(live.state().navigation.route!=Route::Settings) assert(live.action("r1"));
      assert(live.action("focus:settings-kept"));
      capture_live("settings-kept.png");
      assert(live.action("cross"));
      assert(live.action("focus:kept-toggle-43") && live.action("cross"));
      capture_live("kept-manager.png");
      // Leftover title folders: the title ID and size stay beside Delete, which always asks first.
      while(!live.state().navigation.modals.empty()) assert(live.action("circle"));
      live.UpdateLeftoverTitles({{"FAKE78001", "A Short Hike", 12'000'000}, {"FAKE78002", "Portal", 8'000'000}});
      assert(live.action("focus:settings-leftovers"));
      capture_live("settings-leftover-titles.png");
      assert(live.action("cross"));
      assert(live.action("focus:leftover-toggle-FAKE78002"));
      assert(live.action("cross"));
      capture_live("leftover-titles-manager.png");
      assert(live.action("focus:leftover-selection-delete"));
      assert(live.action("cross"));
      assert(live.state().navigation.focus_key == "cancel");
      capture_live("leftover-title-confirm.png");
      assert(live.action("circle"));
      assert(live.action("circle"));
      live.UpdateLeftoverDeletions({{"FAKE78001", "A Short Hike", "Deleting title files…", ""},
                                   {"FAKE78002", "Portal", "", ""}});
      live.action("l1"); live.action("l1");
      assert(live.state().navigation.route == Route::Downloads);
      capture_live("queue-leftover-titles.png");
      live.action("r1"); live.action("r1");
      assert(live.state().navigation.route == Route::Settings);
      live.UpdateLeftoverTitles({});
      live.UpdateLeftoverDeletions({});
      // Settings > Storage: the default drive's usage above the rows, then the installed games list.
      while(!live.state().navigation.modals.empty()) assert(live.action("circle"));
      live.UpdateVolumes({{"internal","Console storage",true,825'000'000'000ull,512'000'000'000ull}}, "internal");
      live.UpdateInstalls({}, {{SteamId(42), 62'700'000'000ull, "internal"}, {SteamId(43), 8'400'000'000ull, "internal"},
                               {SteamId(44), 1'200'000'000ull, "internal"}});
      LiveRuntimeGame playing; playing.game_id= SteamId(44); playing.status="Ready"; playing.ready=true; playing.play_status="Playing";
      LiveRuntimeGame titled=ready; titled.uninstall_status.clear();
      live.UpdateRuntime(RuntimeState::Ready, {titled, playing});
      assert(live.action("focus:settings-storage"));
      capture_live("settings-storage-drive.png");
      assert(live.action("focus:settings-installed"));
      capture_live("settings-storage.png");
      assert(live.action("cross"));
      assert(live.action("focus:installed-toggle-43") && live.action("cross"));
      capture_live("installed-games.png");
      // A batch: the list closes and Queue follows it; one game is uninstalled, the other stops on Cloud.
      assert(live.action("focus:installed-toggle-42") && live.action("cross"));
      assert(live.action("focus:installed-selection-uninstall") && live.action("cross"));
      capture_live("installed-games-confirm.png");
      assert(live.action("focus:confirm-destructive") && live.action("cross"));
      titled.uninstall_status="Removing PS5 title…";
      live.UpdateRuntime(RuntimeState::Ready, {titled, playing});
      live.UpdatePreparationQueue({std::nullopt, {}, {}, {}, {{SteamId(42), "Syncing saves…"}, {SteamId(43), ""}}});
      capture_live("installed-games-queued.png");
      live.action("l1");
      live.action("l1");
      assert(live.state().navigation.route==Route::Downloads);
      capture_live("queue-uninstalls.png");
      live.NotifyUninstallStopped(SteamId(43), "Cloud saves could not be synced. Fix them in Saves, then try again.");
      live.UpdatePreparationQueue({std::nullopt, {}, {}, {}, {{SteamId(42), "Removing PS5 title…"}}});
      capture_live("queue-uninstalls-stopped.png");
      titled.uninstall_status.clear();
      live.UpdateRuntime(RuntimeState::Ready, {playing});
      live.UpdatePreparationQueue({});
      live.UpdateInstalls({}, {{SteamId(43), 8'400'000'000ull, "internal"}, {SteamId(44), 1'200'000'000ull, "internal"}});
      live.NotifyUninstallFinished(SteamId(42));
      capture_live("queue-uninstalls-done.png");
      live.action("r1");
      live.action("r1");
      assert(live.state().navigation.route==Route::Settings);
      // With every Storage row shown the list scrolls; back on the first row the usage card shows again.
      while(!live.state().navigation.modals.empty()) assert(live.action("circle"));
      live.SetRuntimeLeftovers(1);
      assert(live.action("focus:settings-runtime"));
      capture_live("settings-storage-bottom.png");
      while(live.state().navigation.focus_key!="settings-storage") assert(live.action("up"));
      capture_live("settings-storage-top-again.png");
      // A drive without a space report that is disconnected: one line and its state.
      live.UpdateVolumes({{"internal","USB drive",false,{},{}}}, "internal");
      capture_live("settings-storage-unknown.png");
    }
    {
      // A long list: Sort and Select all stay on top and the footer stays visible mid-list.
      Simulation many({}, {}, ModelMode::Live);
      many.SetWallClock(1700000000);
      many.UpdateAccount({AuthState::SignedIn,"123","Synthetic account",{},{},true});
      std::vector<playden::GameSummary> summaries;
      std::vector<LiveInstalledGame> installed;
      for(int index=0; index<56; ++index) {
        playden::GameSummary summary{static_cast<playden::GameId>(SteamId(5000+index)),"Synthetic game "+std::to_string(index+1),{},{},{}};
        if(index%3) summary.last_played_unix_seconds=1700000000-static_cast<std::uint64_t>(index)*86400*5;
        summaries.push_back(summary);
        installed.push_back({static_cast<playden::GameId>(SteamId(5000+index)), (60ull-index)*700'000'000ull, "internal"});
      }
      assert(many.UpdateLibrary({summaries,false,{},false,false,{}}));
      many.UpdateVolumes({{"internal","M.2 SSD",true,1'000'200'000'000ull,90'300'000'000ull}}, "internal");
      many.UpdateInstalls({}, installed);
      while(many.state().navigation.route!=Route::Settings) assert(many.action("r1"));
      assert(many.action("focus:settings-installed") && many.action("cross"));
      assert(many.action("focus:installed-toggle-5003") && many.action("cross"));
      assert(many.action("focus:installed-toggle-5005") && many.action("cross"));
      for(int step=0; step<14; ++step) assert(many.action("down"));
      for(int i=0;i<24;++i) renderer.draw(many,1920,1080);
      assert(renderer.capture_png(output/"installed-games-long.png",1920,1080));
    }
    {
      // Rate and remaining time share the downloaded-size baseline, aligned to the card's right edge.
      Simulation downloads({}, {}, ModelMode::Live);
      downloads.UpdateAccount({AuthState::SignedIn,"123","Synthetic account",{},{},true});
      assert(downloads.UpdateLibrary({backend.games,false,{},false,false,{}}));
      LiveInstallJob job{SteamId(42), JobState::Downloading, 47ULL << 30, 59ULL << 30, {}, "internal"};
      job.bytes_per_second = 40'055'603; // 38.2 MiB/s in the UI's existing size convention.
      job.eta_seconds = 300;
      downloads.UpdateInstalls({job}, {});
      while(downloads.state().navigation.route != Route::Downloads) assert(downloads.action("r1"));
      auto capture_download = [&](const char* name) {
        for(int i=0;i<16;++i) renderer.draw(downloads,1920,1080);
        assert(renderer.capture_png(output/name,1920,1080));
      };
      capture_download("download-rate-eta.png");
      job.state = JobState::Paused;
      downloads.UpdateInstalls({job}, {});
      capture_download("download-rate-paused.png");
      downloads.UpdateAppDetails(SteamId(42), 25ULL << 30, 35ULL << 30, true);
      downloads.UpdateVolumes({{"internal", "Console storage", true, 100ULL << 30, 20ULL << 30}}, "internal");
      capture_download("download-reservation-over-capacity.png");
    }
    model.UpdateAccount({AuthState::Offline,"123","Synthetic account",{},{},true});
    controller.Activate(123,true);settle();
    assert(model.state().games.size()==3 && model.state().library_offline);
    capture("offline.png");
    // Exercise actual private persistence and renderer restoration with synthetic data.
    auto pattern=(std::filesystem::temp_directory_path()/"playden-graphical-persistence-XXXXXX").string();
    std::vector<char> temporary(pattern.begin(),pattern.end());temporary.push_back(0);
    const char* made=mkdtemp(temporary.data());assert(made);
    const std::filesystem::path private_root(made);
    {
      playden::Storage storage(private_root.string());playden::PreferencesStore preferences(storage);
      playden::OrganizationStore organization(storage);
      {
        playden::app::PersistenceService persistence(preferences,organization);
        playden::app::PersistenceController bridge(model,persistence);bridge.Activate(123);
        persistence.WaitIdle();bridge.Tick();
        assert(model.OrganizationAccount()==playden::kDeviceOrganization);
        assert(model.action("square") && model.state().library.favorites.contains(SteamId(43)));
        assert(model.action("circle"));
        for(int n=0;n<4 && model.state().navigation.route!=Route::Settings;++n)assert(model.action("r1"));
        const auto& rows=model.view().rows;
        const auto target=std::find_if(rows.begin(),rows.end(),[](const ViewRow& row){return row.action=="toggle-motion";});
        assert(target!=rows.end());const int index=static_cast<int>(target-rows.begin());
        enter_settings_section(model,index);
        for(int n=0;n<80 && model.view().focused_row!=index;++n)assert(model.action(model.view().focused_row<index?"down":"up"));
        assert(model.view().focused_row==index && model.action("cross"));
        bridge.FlushAndShutdown();
      }
      Simulation restored({}, {},ModelMode::Live);
      restored.UpdateAccount({AuthState::Offline,"123","Synthetic account",{},{},true});
      assert(restored.UpdateLibrary({backend.games,false,{},true,true,1700000000}));
      playden::app::PersistenceService persistence(preferences,organization);
      playden::app::PersistenceController bridge(restored,persistence);bridge.Activate(123);
      const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
      while((!restored.state().library.favorites.contains(SteamId(43)) || !restored.state().reduced_motion) && std::chrono::steady_clock::now()<deadline) {
        bridge.Tick();std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      assert(restored.state().reduced_motion && restored.state().library.favorites.contains(SteamId(43)));
      assert(restored.action("r1"));
      for(int i=0;i<16;++i)renderer.draw(restored,1920,1080);
      assert(renderer.capture_png(output/"restored.png",1920,1080));
      bridge.FlushAndShutdown();
    }
    std::filesystem::remove_all(private_root);
  }
  SDL_GL_DeleteContext(context); SDL_DestroyWindow(window); SDL_Quit();
}
