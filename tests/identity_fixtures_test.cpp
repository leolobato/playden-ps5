// Files written before games were named by store (tests/fixtures/identity, synthetic data in the
// exact formats of those writers: the binary ones were made with them) load with the new code
// and are written back byte for byte; games from other stores round-trip in the new forms.
#include "library/cache.h"
#include "runtime/game_install.h"
#include "runtime/game_settings.h"
#include "runtime/settings_publication_journal.h"
#include "steam/install_service.h"

#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>

#define CHECK(condition)                                                                  \
  do {                                                                                    \
    if (!(condition)) {                                                                   \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
      std::exit(1);                                                                       \
    }                                                                                     \
  } while (false)

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using playden::GameId;

namespace {

fs::path fixtures;

const GameId kLocalA = GameId::Local("0f8fad5b-d9cb-469f-a165-70867728950e");
const GameId kLocalB = GameId::Local("7c9e6679-7425-40de-944b-e07fc1f90ae7");
const GameId kLocalC = GameId::Local("1b4e28ba-2fa1-41d2-883f-0016d3cca427");
const GameId kLocalD = GameId::Local("9a1d1f2e-0000-4000-8000-000000000001");

std::string Read(const fs::path& path) {
  std::string text;
  FILE* file = std::fopen(path.c_str(), "rb");
  CHECK(file != nullptr);
  char buffer[4096];
  for (std::size_t count; (count = std::fread(buffer, 1, sizeof(buffer), file)) > 0;) text.append(buffer, count);
  std::fclose(file);
  return text;
}

void Write(const fs::path& path, const std::string& text) {
  fs::create_directories(path.parent_path());
  FILE* file = std::fopen(path.c_str(), "wb");
  CHECK(file != nullptr);
  CHECK(std::fwrite(text.data(), 1, text.size(), file) == text.size());
  CHECK(std::fclose(file) == 0);
}

playden::Bytes Bytes(const std::string& text) { return playden::Bytes(text.begin(), text.end()); }

void SetTime(const fs::path& path, long seconds) {
  const timeval times[2] = {{seconds, 0}, {seconds, 0}};
  CHECK(utimes(path.c_str(), times) == 0);
}

struct TemporaryDirectory {
  TemporaryDirectory() {
    auto pattern = (fs::temp_directory_path() / "playden-identity-XXXXXX").string();
    CHECK(::mkdtemp(pattern.data()) != nullptr);
    path = fs::canonical(pattern);
    CHECK(chmod(path.c_str(), 0700) == 0);
  }
  ~TemporaryDirectory() { fs::remove_all(path); }
  fs::path path;
};

// Library cache: schema 4 is read as Steam and written back as it was; schema 5 names stores.
void LibraryCacheKeepsSteamBytes() {
  TemporaryDirectory directory;
  const std::string account = "76561190000000042";
  const auto name = "library-" + account + ".cache";
  const auto original = Read(fixtures / name);
  Write(directory.path / name, original);
  playden::Storage storage(directory.path.string());
  playden::LibraryCache cache(storage);
  auto loaded = cache.Load(account);
  CHECK(loaded.ok() && loaded.value().schema_version == 4 && loaded.value().games.size() == 3);
  CHECK(loaded.value().games[0].id == GameId::Steam(220) && loaded.value().games[1].id == GameId::Steam(400));
  CHECK(loaded.value().games[2].id == GameId::Steam(4294967295u));
  CHECK(loaded.value().games[1].added_at_unix_seconds == 1500000000u);
  CHECK(cache.Save(account, loaded.value()).ok());
  CHECK(Read(directory.path / name) == original);

  // Schema 4 holds Steam app IDs only; schema 5 holds any store's games.
  auto mixed = loaded.value();
  mixed.games.push_back({kLocalA, "Imported", "", "", ""});
  mixed.games.back().added_at_unix_seconds = 1760000001;
  CHECK(!cache.Save(account, mixed).ok());
  CHECK(Read(directory.path / name) == original);
  mixed.schema_version = 5;
  CHECK(cache.Save(account, mixed).ok());
  auto restored = cache.Load(account);
  CHECK(restored.ok() && restored.value().schema_version == 5 && restored.value().games.size() == 4);
  CHECK(restored.value().games[3].id == kLocalA && restored.value().games[3].title == "Imported");
  CHECK(restored.value().games[3].added_at_unix_seconds == 1760000001u);
  CHECK(restored.value().games[2].id == GameId::Steam(4294967295u));
  const auto schema5 = Read(directory.path / name);
  CHECK(cache.Save(account, restored.value()).ok() && Read(directory.path / name) == schema5);
  // Not a game: a Steam value with a leading zero, or an unknown store.
  auto bad = mixed;
  bad.games.back().id = GameId{playden::Store::kSteam, "0440"};
  CHECK(!cache.Save(account, bad).ok());
  bad.games.back().id = GameId{playden::Store::kGog, "1207658924"};
  CHECK(!cache.Save(account, bad).ok());
}

class Sessions final : public playden::SessionRepository {
 public:
  playden::Result<std::optional<playden::StoredSession>> Load() const override {
    return playden::Result<std::optional<playden::StoredSession>>::Success(std::nullopt);
  }
  playden::Result<void> Save(const playden::StoredSession&) const override {
    return playden::Result<void>::Success();
  }
  playden::Result<void> Clear() const override { return playden::Result<void>::Success(); }
};

class Runner final : public playden::steam::SessionInstallRunner {
 public:
  explicit Runner(playden::SessionRepository& sessions) : SessionInstallRunner(sessions) {}
  std::atomic<int> installs{0};
  Result InstallApp(const playden::StoredSession&, std::uint32_t, const std::string&,
                 const playden::steam::InstallOptions&, const std::atomic<bool>&,
                 const std::function<void(std::uint64_t, std::uint64_t, std::uint64_t)>&,
                 const std::function<void(playden::steam::InstallStep)>&) override {
    ++installs;
    return {false, false, "unexpected", 0};
  }
  std::string Remove(const std::string&) override { return {}; }
};

template <typename Ready>
void Await(playden::steam::InstallService& service, Ready ready) {
  const auto deadline = std::chrono::steady_clock::now() + 6s;
  while (!ready(service.Snapshot())) {
    CHECK(std::chrono::steady_clock::now() < deadline);
    std::this_thread::sleep_for(1ms);
  }
}

const playden::steam::InstallJobView* JobOf(const playden::steam::InstallSnapshot& snapshot, const GameId& id) {
  for (const auto& job : snapshot.jobs)
    if (job.id == id) return &job;
  return nullptr;
}

std::vector<playden::steam::InstallVolume> Volumes(const fs::path& root) {
  return {{"internal", "Console storage", (root / "games").string(), true},
          {"usb1", "USB drive", (root / "usb1").string(), true}};
}

// .playden-games and .playden-downloads-dismissed: Steam lines are written back byte for byte
// (installed, unavailable, queued and paused); Local lines use their key.
void RegistryKeepsSteamLines() {
  using namespace playden::steam;
  TemporaryDirectory directory;
  const auto games = directory.path / "games";
  fs::create_directories(directory.path / "usb1");
  const auto registry = Read(fixtures / ".playden-games");
  const auto dismissed = Read(fixtures / ".playden-downloads-dismissed");
  Write(games / ".playden-games", registry);
  Write(games / ".playden-downloads-dismissed", dismissed);
  // 220 finished at the dismissed time; 400's drive has no folder for it (unavailable).
  Write(games / "220/.playden-install.json", Read(fixtures / ".playden-install.json"));
  SetTime(games / "220/.playden-install.json", 1760000000);
  Sessions sessions;
  Runner runner(sessions);
  {
    InstallService service(runner, Volumes(directory.path));
    const auto snapshot = service.Snapshot();
    CHECK(snapshot.installed.size() == 2 && snapshot.installed[0].id == GameId::Steam(220));
    CHECK(snapshot.installed[0].install_bytes == 5000000000u && snapshot.installed[0].available);
    CHECK(snapshot.installed[1].id == GameId::Steam(400) && !snapshot.installed[1].available);
    CHECK(JobOf(snapshot, GameId::Steam(900)) && JobOf(snapshot, GameId::Steam(900))->state == InstallJobState::kPaused);
    // The queued one waits for a sign-in, which rewrites the registry.
    Await(service, [](const InstallSnapshot& s) {
      const auto* job = JobOf(s, GameId::Steam(70));
      return job && job->state == InstallJobState::kQueued && job->failure == "Waiting for the Steam sign-in.";
    });
    service.DismissCompleted(GameId::Steam(220));
  }
  CHECK(Read(games / ".playden-games") == registry);
  CHECK(Read(games / ".playden-downloads-dismissed") == dismissed);
  CHECK(runner.installs == 0);

  // Local games: installed (one finished and dismissed), on an absent folder, paused and queued.
  TemporaryDirectory local;
  const auto local_games = local.path / "games";
  fs::create_directories(local.path / "usb1");
  const auto c_key = playden::ToKey(kLocalC);
  Write(local_games / ".playden-games",
        playden::ToKey(kLocalA) + "\tusb1\t777\n" + playden::ToKey(kLocalB) + "\tinternal\t0\tpaused\n" +
            playden::ToKey(kLocalD) + "\tinternal\t0\n" + c_key + "\tinternal\t5\n");
  Write(local_games / (c_key + "/.playden-install.json"),
        "{\"version\":1,\"app_id\":4026531842,\"store\":\"local\",\"game\":\"" + kLocalC.value +
            "\",\"install_bytes\":4096,\"files\":1,\"depots\":[]}\n");
  SetTime(local_games / (c_key + "/.playden-install.json"), 1760000500);
  Write(local_games / ".playden-downloads-dismissed", c_key + "\t1760000500\n");
  {
    InstallService service(runner, Volumes(local.path));
    const auto snapshot = service.Snapshot();
    CHECK(snapshot.installed.size() == 2 && snapshot.installed[0].id == kLocalA && !snapshot.installed[0].available);
    CHECK(snapshot.installed[1].id == kLocalC && snapshot.installed[1].install_bytes == 4096);
    CHECK(snapshot.completed.empty());  // dismissed
    CHECK(JobOf(snapshot, kLocalB) && JobOf(snapshot, kLocalB)->state == InstallJobState::kPaused);
    // Only Steam games download: a queued Local job fails instead of reaching the Steam runner.
    Await(service, [](const InstallSnapshot& s) {
      const auto* job = JobOf(s, kLocalD);
      return job && job->state == InstallJobState::kFailed;
    });
    service.DismissCompleted(kLocalC);
  }
  CHECK(runner.installs == 0);
  CHECK(Read(local_games / ".playden-games") ==
        playden::ToKey(kLocalA) + "\tusb1\t777\n" + c_key + "\tinternal\t4096\n" + playden::ToKey(kLocalB) +
            "\tinternal\t0\tpaused\n" + playden::ToKey(kLocalD) + "\tinternal\t0\tpaused\n");
  CHECK(Read(local_games / ".playden-downloads-dismissed") == c_key + "\t1760000500\n");
  // Malformed keys are dropped: leading zeros, upper-case UUIDs, other stores.
  TemporaryDirectory junk;
  fs::create_directories(junk.path / "usb1");
  Write(junk.path / "games/.playden-games",
        "0220\tinternal\t1\nlocal-0F8FAD5B-D9CB-469F-A165-70867728950E\tinternal\t1\ngog-1\tinternal\t1\n");
  {
    InstallService service(runner, Volumes(junk.path));
    CHECK(service.Snapshot().installed.empty() && service.Snapshot().jobs.empty());
  }
}

// installs.json (a game moved with the 0.6 layout, one that starts in its install root).
void InstallRecordsKeepBytes() {
  using namespace playden::runtime;
  TemporaryDirectory directory;
  const auto original = Read(fixtures / "installs.json");
  Write(directory.path / "installs.json", original);
  playden::Storage storage(directory.path.string());
  RuntimeInstallRepository repository(storage, "installs.json", [](const std::string&) {
    return NativeTitleAvailability{TitleAvailability::kFree, TitleAvailability::kFree};
  });
  CHECK(repository.Load().ok());
  const auto moved = repository.Find("steam-220");
  const auto rooted = repository.Find("steam-400");
  CHECK(moved && moved->game == GameId::Steam(220) && moved->moved_from == "/data/homebrew/FAKE77031/games/220");
  CHECK(rooted && rooted->game == GameId::Steam(400) && rooted->launch_in_install_root);
  CHECK(repository.Save().ok());
  CHECK(Read(directory.path / "installs.json") == original);
  CHECK(repository.RuntimeNumberFor(GameId::Steam(220)).value() == 220u);
  CHECK(Read(directory.path / "installs.json") == original);  // Steam needs no allocator
  // A kept bottle's number comes back to its game and never goes to another.
  CHECK(repository.RememberNumber(kLocalA, 0xF0000005u).ok());
  CHECK(repository.RememberNumber(kLocalA, 0xF0000005u).ok());
  CHECK(repository.RememberNumber(kLocalB, 0xF0000005u).error() == RepositoryError::kConflict);
  CHECK(repository.RememberNumber(kLocalA, 0xF0000006u).error() == RepositoryError::kConflict);
  CHECK(repository.RememberNumber(kLocalB, 220).error() == RepositoryError::kInvalidRecord);
  CHECK(repository.RememberNumber(GameId::Steam(220), 220).ok());
  CHECK(repository.RuntimeNumberFor(kLocalA).value() == 0xF0000005u);
  CHECK(Read(directory.path / "installs.json") == original);  // nothing new was allocated
  CHECK(repository.RuntimeNumberFor(kLocalB).value() == 0xF0000000u);
}

// game-settings.json: Steam keys as before (numeric order), Local keys by ToKey.
void GameSettingsKeepKeys() {
  using namespace playden::runtime;
  TemporaryDirectory directory;
  const auto path = directory.path / "game-settings.json";
  const auto original = Read(fixtures / "game-settings.json");
  Write(path, original);
  CHECK(chmod(path.c_str(), 0600) == 0);
  std::map<GameId, GameSettingsRecord> records;
  std::string reason;
  CHECK(LoadGameSettingsFile(path.string(), &records, &reason) && records.size() == 3);
  CHECK(records.at(GameId::Steam(220)).overrides.at("program") == "bin/hl2.exe");
  CHECK(records.at(GameId::Steam(400)).preset_id == "playden-default");
  CHECK(records.count(GameId::Steam(4294967295u)));
  CHECK(SaveGameSettingsFile(path.string(), records, &reason));
  CHECK(Read(path) == original);
  records[kLocalA] = {"", {{"program", "Game.exe"}}, 1};
  CHECK(SaveGameSettingsFile(path.string(), records, &reason));
  const auto text = Read(path);
  CHECK(text.rfind(original.substr(0, original.size() - 3), 0) == 0);  // Steam entries first, unchanged
  CHECK(text.find(",\"local-0f8fad5b-d9cb-469f-a165-70867728950e\":{\"preset\":\"\",\"revision\":1,") !=
        std::string::npos);
  std::map<GameId, GameSettingsRecord> back;
  CHECK(LoadGameSettingsFile(path.string(), &back, &reason) && back == records);
  // Keys earlier Playdens read the same way still load; malformed Local keys do not.
  Write(path, "{\"version\":1,\"games\":{\"0220\":{\"preset\":\"\",\"revision\":2,\"overrides\":{}},"
              "\"local-0F8FAD5B-D9CB-469F-A165-70867728950E\":{\"preset\":\"\",\"revision\":2,\"overrides\":{}},"
              "\"local-\":{\"preset\":\"\",\"revision\":2,\"overrides\":{}}}}\n");
  CHECK(LoadGameSettingsFile(path.string(), &back, &reason) && back.size() == 1 && back.count(GameId::Steam(220)));
}

// Recipes: a generated Steam recipe is read and written as before; a Local one names its game.
void RecipesNameTheirGame() {
  using namespace playden::runtime;
  const auto original = Read(fixtures / "recipe-steam-220-auto.json");
  GameRecipe recipe;
  std::string reason;
  CHECK(ParseGameRecipe(original, &recipe, &reason));
  CHECK(recipe.id == "steam-220-auto" && IsGeneratedRecipeId(recipe.id) && RecipeGame(recipe) == GameId::Steam(220));
  CHECK(SerializeGameRecipe(recipe) == original);
  CHECK(GeneratedRecipeId(220) == playden::GeneratedRecipeId(GameId::Steam(220)));

  auto local = recipe;
  local.id = playden::GeneratedRecipeId(kLocalA);
  local.app_id = 0xF0000000u;
  local.game = kLocalA;
  CHECK(local.id == "local-0f8fad5b-d9cb-469f-a165-70867728950e-auto" && IsGeneratedRecipeId(local.id));
  const auto text = SerializeGameRecipe(local);
  CHECK(text.find("\"app_id\":4026531840,\"store\":\"local\",\"game\":\"0f8fad5b-d9cb-469f-a165-70867728950e\",") !=
        std::string::npos);
  GameRecipe back;
  CHECK(ParseGameRecipe(text, &back, &reason) && RecipeGame(back) == kLocalA && back.app_id == 0xF0000000u);
  CHECK(SerializeGameRecipe(back) == text);
  // A recipe for runtime number 4026531840 with no game is a Steam recipe, never the Local game's.
  auto steam_number = local;
  steam_number.game = {};
  CHECK(RecipeGame(steam_number) == GameId::Steam(0xF0000000u));
  // Half an identity, an unknown store or an invalid game is refused.
  auto refuse = [&](const std::string& from, const std::string& to) {
    auto changed = text;
    changed.replace(changed.find(from), from.size(), to);
    GameRecipe ignored;
    return !ParseGameRecipe(changed, &ignored, &reason);
  };
  CHECK(refuse(",\"store\":\"local\"", ""));
  CHECK(refuse("\"store\":\"local\"", "\"store\":\"gog\""));
  CHECK(refuse("\"game\":\"0f8fad5b", "\"game\":\"0F8FAD5B"));
  CHECK(!IsGeneratedRecipeId("local-0F8FAD5B-D9CB-469F-A165-70867728950E-auto"));
  CHECK(!IsGeneratedRecipeId("local--auto") && !IsGeneratedRecipeId("steam--auto") && IsGeneratedRecipeId("steam-1-auto"));
  // Generated for a Local game: its id, game and runtime number.
  const auto root = fs::temp_directory_path() / ("playden-identity-recipe-" + std::to_string(::getpid()));
  fs::create_directories(root);
  std::string pe(512, '\0');
  pe[0] = 'M'; pe[1] = 'Z'; pe[0x3c] = 0x40;
  pe.replace(0x40, 4, std::string("PE\0\0", 4));
  pe[0x44] = 0x4c; pe[0x45] = 0x01;  // i386
  pe[0x58] = 0x0b; pe[0x59] = 0x01;  // PE32
  Write(root / "Game.exe", pe);
  GameRecipe generated;
  const bool made = GenerateGameRecipe(kLocalA, 0xF0000000u, "Imported", {{"", "", "Game.exe", "", ""}}, root.string(),
                                       &generated, &reason);
  fs::remove_all(root);
  CHECK(made && generated.id == playden::GeneratedRecipeId(kLocalA) && RecipeGame(generated) == kLocalA &&
        generated.app_id == 0xF0000000u);
}

// A pending settings publication written by the journal (PWOP2, and PWOP3 for a game that starts
// in its install root) still decodes, rebuilds and encodes to the same bytes.
void PendingPublicationsRecover() {
  using namespace playden::runtime;
  for (const char* name : {"settings-operation-pwop2.bin", "settings-operation-pwop3.bin"}) {
    const auto bytes = Bytes(Read(fixtures / name));
    std::string reason;
    const auto operation = DecodeSettingsPublicationOperation(bytes, &reason);
    CHECK(operation);
    CHECK(operation->install.runtime_number == 220 && operation->install.install_id == "install-220");
    CHECK(operation->install.launch_in_install_root == (std::string(name).find("pwop3") != std::string::npos));
    CHECK(RebuildFrozenProfilePublication(*operation, &reason));
    CHECK(EncodeSettingsPublicationOperation(*operation, &reason) == bytes);
  }
}

}  // namespace

int main(int argc, char** argv) {
  CHECK(argc == 2);
  fixtures = argv[1];
  LibraryCacheKeepsSteamBytes();
  RegistryKeepsSteamLines();
  InstallRecordsKeepBytes();
  GameSettingsKeepKeys();
  RecipesNameTheirGame();
  PendingPublicationsRecover();
  std::puts("identity_fixtures_test passed");
}
