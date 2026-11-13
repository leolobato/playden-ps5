#include "homebrew/artwork.hpp"
#include "homebrew/program_icon.hpp"
#include "homebrew/title_icon.hpp"
#include "homebrew/render.hpp"
#include "homebrew/simulation.hpp"
#include "gfx/gl_program.hpp"
#include <SDL.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <thread>
#include "steam_id.h"

using namespace playden::homebrew;
using namespace std::chrono_literals;
namespace {
struct Backend final : ArtworkBackend {
    std::atomic<bool> release{false};
    std::optional<ArtworkImage> Load(const ArtworkKey& key, const std::atomic<bool>& cancel) override {
        while (!release && !cancel) std::this_thread::sleep_for(1ms);
        if (cancel) return std::nullopt;
        // The program's icon, a 32x32 orange disc on a transparent ground, as the poster the real backend makes.
        if (ProgramIconPath(key.source)) {
            ArtworkImage icon{32,32,std::vector<std::uint8_t>(32*32*4,0)};
            for (int y=0;y<32;++y) for (int x=0;x<32;++x)
                if ((x-16)*(x-16)+(y-16)*(y-16)<15*15) {
                    auto* p=&icon.rgba[(y*32+x)*4]; p[0]=240; p[1]=140; p[2]=30; p[3]=255;
                }
            return ProgramIconArt(icon,kProgramIconPosterWidth,kProgramIconPosterHeight);
        }
        if (key.game_id == SteamId(44)) return std::nullopt;
        ArtworkImage result{64,96,{}};
        result.rgba.resize(64*96*4);
        for (std::size_t i=0;i<result.rgba.size();i+=4) {
            const auto stripe = static_cast<unsigned char>((i/4/64)%16 < 8 ? 220 : 100);
            result.rgba[i] = key.game_id == SteamId(42) ? stripe : 25;
            result.rgba[i+1] = key.game_id == SteamId(43) ? stripe : 25;
            result.rgba[i+2] = 70;
            result.rgba[i+3] = 255;
        }
        return result;
    }
};
}
int main(int argc,char** argv) {
    assert(argc==2);
    assert(SDL_Init(SDL_INIT_VIDEO)==0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS,SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
    auto* window=SDL_CreateWindow("Synthetic asynchronous artwork",0,0,1920,1080,SDL_WINDOW_OPENGL|SDL_WINDOW_HIDDEN);
    assert(window);
    auto context=SDL_GL_CreateContext(window); assert(context);
    SDL_GL_SetSwapInterval(0);
    hui::gfx::set_glsl_prefix("#version 410 core\n");
    const std::filesystem::path output(argv[1]);
    std::filesystem::create_directories(output);
    {
        Backend backend;
        ArtworkService service(backend);
        Render renderer(PLAYDEN_HOMEBREW_ASSETS_DIR,&service);
        Simulation model({}, {},ModelMode::Live);
        model.SetWallClock(1700000000);
        model.UpdateAccount({AuthState::SignedIn,"123","Synthetic account",{},{},true});
        assert(model.UpdateLibrary({{{SteamId(42),"Red cover",{},{},{}},{SteamId(43),"Green cover",{},{},{}},{SteamId(44),"Unavailable cover",{},{},{}}},false,{},false,true,1700000000}));
        assert(model.action("r1"));
        for(int i=0;i<16;++i) renderer.draw(model,1920,1080);
        assert(renderer.diagnostics().live_artwork_texture_count==0);
        assert(renderer.capture_png(output/"pending.png",1920,1080));
        // Navigation remains active while the worker is waiting.
        assert(model.action("right"));
        backend.release=true;
        const auto deadline=std::chrono::steady_clock::now()+3s;
        while(renderer.diagnostics().live_artwork_texture_count<2 && std::chrono::steady_clock::now()<deadline) {
            renderer.draw(model,1920,1080);
            std::this_thread::sleep_for(1ms);
        }
        assert(renderer.diagnostics().live_artwork_texture_count==2);
        assert(renderer.diagnostics().live_artwork_gpu_bytes==2*64*96*4);
        for(int i=0;i<16;++i) renderer.draw(model,1920,1080);
        assert(renderer.capture_png(output/"ready.png",1920,1080));
        // A game whose cover failed shows the icon inside its program once it is prepared (#213).
        LiveRuntimeGame prepared;
        prepared.game_id=SteamId(44);
        prepared.status="Ready";
        prepared.program_icon=ProgramIconSource("/data/playden-games/44/Game.exe");
        model.UpdateRuntime(RuntimeState::Ready,{prepared});
        const auto icon_deadline=std::chrono::steady_clock::now()+3s;
        while(renderer.diagnostics().live_artwork_texture_count<3 && std::chrono::steady_clock::now()<icon_deadline) {
            renderer.draw(model,1920,1080);
            std::this_thread::sleep_for(1ms);
        }
        assert(renderer.diagnostics().live_artwork_texture_count==3);
        for(int i=0;i<16;++i) renderer.draw(model,1920,1080);
        assert(renderer.capture_png(output/"program-icon.png",1920,1080));
        const auto generation=model.state().account_generation;
        model.UpdateAccount({AuthState::SignedOut,{},{},{},{},true});
        // Same-account replacement in one render interval must still invalidate resources.
        model.UpdateAccount({AuthState::SignedIn,"123","Synthetic account",{},{},true});
        assert(model.state().account_generation>generation);
        renderer.draw(model,1920,1080);
        assert(renderer.diagnostics().live_artwork_texture_count==0);
        service.Shutdown();
    }
    {
        // GOG only: Steam signed out, GOG signed in. The GOG covers load all the same, and the game page shows its
        // hero, though Steam has no account.
        Backend backend;
        backend.release=true;
        ArtworkService service(backend);
        Render renderer(PLAYDEN_HOMEBREW_ASSETS_DIR,&service);
        Simulation model({}, {},ModelMode::Live);
        model.SetGogStoreEnabled(true);
        model.SetWallClock(1700000000);
        model.UpdateAccount({AuthState::SignedOut,{},{},{},{},true});
        AccountUpdate gog{AuthState::SignedIn,"00000000000000aa","Synthetic GOG account",{},{},false};
        gog.store=playden::Store::kGog;
        model.UpdateAccount(gog);
        LibraryUpdate update;
        for (std::uint64_t id : {1207658924ULL, 1207658925ULL}) {
            playden::GameSummary game;
            game.id=playden::GameId::Gog(id);
            game.title="Synthetic GOG game "+std::to_string(id);
            const char digit=id==1207658924ULL ? 'a' : 'c';
            game.artwork_source="https://images.gog.com/"+std::string(64,digit)+".jpg?namespace=gamesdb";
            game.hero_source="https://images.gog.com/"+std::string(64,digit)+"_bg_crop_1920x655.jpg?namespace=gamesdb";
            update.games.push_back(game);
        }
        update.cache_available=true;
        update.retrieved_at=1700000000;
        update.source=playden::Store::kGog;
        assert(model.UpdateLibrary(update));
        assert(model.state().account().id.empty());
        assert(model.action("r1") && model.state().navigation.route==Route::Library);
        const auto deadline=std::chrono::steady_clock::now()+3s;
        while(renderer.diagnostics().live_artwork_texture_count<2 && std::chrono::steady_clock::now()<deadline) {
            renderer.draw(model,1920,1080);
            std::this_thread::sleep_for(1ms);
        }
        assert(renderer.diagnostics().live_artwork_texture_count==2);
        for(int i=0;i<16;++i) renderer.draw(model,1920,1080);
        assert(renderer.capture_png(output/"gog-only.png",1920,1080));
        // The game page: its hero joins the covers.
        assert(model.action("details:" + playden::ToKey(playden::GameId::Gog(1207658924))));
        assert(model.state().navigation.detail_id == playden::GameId::Gog(1207658924));
        const auto hero_deadline=std::chrono::steady_clock::now()+3s;
        while(renderer.diagnostics().live_artwork_texture_count<3 && std::chrono::steady_clock::now()<hero_deadline) {
            renderer.draw(model,1920,1080);
            std::this_thread::sleep_for(1ms);
        }
        assert(renderer.diagnostics().live_artwork_texture_count==3);
        for(int i=0;i<16;++i) renderer.draw(model,1920,1080);
        assert(renderer.capture_png(output/"gog-only-details.png",1920,1080));
        service.Shutdown();
    }
    SDL_GL_DeleteContext(context);
    SDL_DestroyWindow(window);
    SDL_Quit();
}
