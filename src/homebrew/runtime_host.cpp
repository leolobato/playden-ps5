// runtime_host.cpp - Worker-owned composition of the per-game runtime services.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: MIT
#include "homebrew/runtime_host.hpp"
#include "installs/install_record.h"

#include "platform/random_id.h"
#include "platform/storage.h"
#include "runtime/private_directories.h"
#include "runtime/prospero_profile.h"
#include "runtime/settings_publication_journal.h"
#include "runtime/dotnet_heap.h"
#include "runtime/recipe_ini.h"
#include "runtime/game_probe.h"
#include "runtime/install_script.h"
#include "runtime/layout_migration.h"
#include "runtime/redist_install.h"
#include "runtime/settings_publication_owner.h"
#include "runtime/session_storage.h"
#include "runtime/steam_stub.h"
#include "runtime/steamworks.h"
#include "runtime/self_heal.h"
#include "runtime/writable_folders.h"
#include "platform/managed_file_lock.h"
#include "steam/backend/cloud_mapping.h"
#include "steam/backend/json.h"
#include "stores/local/scanner.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#include <set>
#include <string_view>
#include <tuple>
#include <utility>

namespace playden::homebrew {
namespace rt = playden::runtime;
namespace {
// Stands in for the package layer while no verified kit is admitted. It reports
// unknown package state and refuses every action, so nothing is staged.
class UnavailablePackageInstaller final : public rt::PackageInstaller {
 public:
  explicit UnavailablePackageInstaller(std::string reason) : reason_(std::move(reason)) {}
  rt::PackageStatus Inspect(const rt::RuntimeInstall&) override { return {}; }
  rt::PackageActionResult StageTitle(const rt::RuntimeInstall&) override { return Refuse(); }
  rt::PackageActionResult CopyTemplate(const rt::RuntimeInstall&) override { return Refuse(); }
  rt::PackageActionResult ValidateArtifacts(const rt::RuntimeInstall&) override { return Refuse(); }
  rt::PackageActionResult PublishProfile(const rt::RuntimeInstall&) override { return Refuse(); }
 private:
  rt::PackageActionResult Refuse() const { return {rt::PackageActionState::kFailed, "kit-unavailable", reason_}; }
  std::string reason_;
};

constexpr int kMaxPreparationSteps = 64;
// A pending launch is polled at this interval, and Handoff trusts a preflight
// the worker made at most this long ago.
constexpr auto kLaunchPollInterval = std::chrono::seconds(2);
constexpr std::int64_t kPreflightFreshMs = 5000;
// The console's process list could not show whether the title runs (the reason is in
// runtime.log). Closing the game cannot help when no game runs; a restart usually does.
constexpr char kActivityUnknown[] =
    "The PS5 could not tell Playden whether the game is running. Restart the PS5, then try again.";

// A relocated game's republish refused because its old publication can't be verified against the
// new folder (VerifyMovedPublication, or the title's artifacts): only a new title gets past that.
// The capture step reports its moved-publication check under its own code, so its words tell.
bool UnverifiableMovedPublication(const std::string& code, const std::string& message) {
  return code == "publication-moved" || code == "publication-preflight-previous" || code == "artifact-invalid" ||
         (code == "publication-request-capture" && message.find("not the verified publication") != std::string::npos);
}

// A moved game whose publication can't be redone: Recreate bottle (confirmed) prepares it again.
constexpr char kRecreateHint[] = " Use Maintenance > Recreate bottle on the game page to prepare it again.";

// The game a request or record names: a default GameId stands for the Steam game its number is.
GameId GameOf(const GameId& game, std::uint32_t runtime_number) {
  return game == GameId{} && runtime_number ? GameId::Steam(runtime_number) : game;
}
// The game behind a record, for the callbacks named by game (settings, title icon).
GameId GameOf(const rt::RuntimeInstall& record) { return GameOf(record.game, record.runtime_number); }
// The game a request names.
GameId GameOf(const RuntimePrepareRequest& request) { return GameOf(request.game, request.runtime_number); }
// Whether a recipe is the record's game's: the same game (Steam: the app ID) and runtime number.
// A record copy that doesn't carry its game (read back from a journal) relies on the number, which
// the repository keeps to one game.
bool RecipeFits(const rt::GameRecipe& recipe, const rt::RuntimeInstall& record) {
  if (recipe.app_id != record.runtime_number) return false;
  return record.game == GameId{} || rt::RecipeGame(recipe) == record.game;
}

// Why the kit cannot run a recipe Playden ships (its requirements), for the player; empty when it
// can. The kit's own recipe is skipped: the kit's packager checked its template against it.
std::string KitLacksForRecipe(const rt::GameRecipe& recipe, const rt::VerifiedKitView& kit) {
  if (recipe.id == kit.recipe.id) return {};
  const auto unmet = rt::UnmetRecipeRequirements(recipe.requirements, kit);
  return unmet.empty() ? std::string()
                       : "The installed runtime kit lacks what this game needs (" + unmet + "); install a newer kit.";
}

// Staged, its registration submitted, and only waiting for ShadowMountPlus, which
// registers titles once no title runs: the game is ready to play from Home. A
// registration the console refused (a stale hook, a failed submission) is not.
bool AwaitingRegistration(const rt::RuntimeInstall& record) {
  return record.journal && (record.journal->step == "register" || record.journal->step == "await-native") &&
         (!record.last_failure || record.last_failure->code == "registration-uncertain");
}

// Rewrites the game_root= and prefix_root= lines of each <saves>/<id>/cloud-index.txt (the format
// steam/backend/cloud_sync.cpp WriteIndex writes) that lie inside `from` to the same place inside
// `to`, so a moved game keeps its quick-check index. Idempotent; returns how many changed.
std::size_t RebaseCloudIndexes(const std::filesystem::path& saves, const std::string& from, const std::string& to) {
  constexpr off_t kMaxIndexBytes = 16 * 1024 * 1024;
  std::size_t changed = 0;
  std::error_code error;
  for (std::filesystem::directory_iterator it(saves, error), end; !error && it != end; it.increment(error)) {
    std::error_code type_error;
    if (it->is_symlink(type_error) || !it->is_directory(type_error)) continue;
    const auto path = it->path() / "cloud-index.txt";
    const int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) continue;
    struct stat info{};
    std::string text;
    if (fstat(fd, &info) == 0 && S_ISREG(info.st_mode) && info.st_size <= kMaxIndexBytes) {
      text.resize(static_cast<std::size_t>(info.st_size));
      std::size_t done = 0;
      while (done < text.size()) {
        const auto count = read(fd, text.data() + done, text.size() - done);
        if (count <= 0) break;
        done += static_cast<std::size_t>(count);
      }
      text.resize(done);
    }
    close(fd);
    if (text.rfind("version=1\n", 0) != 0) continue;
    std::string rewritten;
    bool any = false;
    for (std::size_t start = 0; start < text.size();) {
      auto stop = text.find('\n', start);
      stop = stop == std::string::npos ? text.size() : stop + 1;
      std::string line = text.substr(start, stop - start);
      start = stop;
      for (const char* key : {"game_root=", "prefix_root="}) {
        const std::string_view name(key);
        if (line.compare(0, name.size(), name) != 0) continue;
        const bool newline = !line.empty() && line.back() == '\n';
        const auto value = line.substr(name.size(), line.size() - name.size() - (newline ? 1 : 0));
        if (const auto moved = rt::RebaseInstallPath(value, from, to)) {
          line = std::string(name) + *moved + (newline ? "\n" : "");
          any = true;
        }
      }
      rewritten += line;
    }
    if (!any) continue;
    const auto temporary = path.string() + ".playden-partial";
    const int out = open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, info.st_mode & 0777);
    if (out < 0) continue;
    std::size_t written = 0;
    while (written < rewritten.size()) {
      const auto count = write(out, rewritten.data() + written, rewritten.size() - written);
      if (count <= 0) break;
      written += static_cast<std::size_t>(count);
    }
    // The original's mode, whatever the umask took from it.
    const bool ok = written == rewritten.size() && fchmod(out, info.st_mode & 0777) == 0 && fsync(out) == 0;
    if (close(out) != 0 || !ok || rename(temporary.c_str(), path.c_str()) != 0) {
      unlink(temporary.c_str());
      continue;
    }
    ++changed;
  }
  return changed;
}
}  // namespace

// Newest wrapper session log under <data root>/library/logs, first 32 KiB and last 128 KiB.
// The first head and last tail bytes of a regular file, without following links; a
// marker line stands for what lies between them.
std::string FileTail(const std::filesystem::path& path, off_t tail, off_t head = 0) {
  const int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) return {};
  struct stat info{};
  std::string bytes;
  const auto read_range = [&](off_t start, off_t length) {
    std::string part(static_cast<std::size_t>(length), '\0');
    std::size_t done = 0;
    while (done < part.size()) {
      const auto count = pread(fd, part.data() + done, part.size() - done, start + static_cast<off_t>(done));
      if (count <= 0) break;
      done += static_cast<std::size_t>(count);
    }
    part.resize(done);
    return part;
  };
  if (fstat(fd, &info) == 0 && S_ISREG(info.st_mode)) {
    if (info.st_size > head + tail && head > 0)
      bytes = read_range(0, head) + "\n== ... " + std::to_string(info.st_size - head - tail) + " bytes left out ...\n";
    const off_t start = info.st_size > tail + head ? info.st_size - tail : (head > 0 ? 0 : std::max<off_t>(0, info.st_size - tail));
    bytes += read_range(start, info.st_size - start);
  }
  close(fd);
  return bytes;
}

// The newest regular file in folder whose name passes keep. Links are skipped.
template <typename Keep>
void Newest(const std::filesystem::path& folder, Keep keep, std::filesystem::path* newest,
            std::filesystem::file_time_type* newest_time) {
  namespace fs = std::filesystem;
  std::error_code error;
  for (fs::directory_iterator it(folder, error), end; !error && it != end; it.increment(error)) {
    if (!keep(it->path().filename().string()) || it->is_symlink(error) || !it->is_regular_file(error)) continue;
    const auto when = it->last_write_time(error);
    if (!error && (newest->empty() || when > *newest_time)) { *newest = it->path(); *newest_time = when; }
  }
}

// The subfolders of folder, links skipped.
std::vector<std::filesystem::path> Folders(const std::filesystem::path& folder) {
  namespace fs = std::filesystem;
  std::vector<fs::path> out;
  std::error_code error;
  for (fs::directory_iterator it(folder, error), end; !error && it != end && out.size() < 256; it.increment(error))
    if (!it->is_symlink(error) && it->is_directory(error)) out.push_back(it->path());
  return out;
}

// The newest session report of a game's bottle, followed by the newest log its engine wrote:
// Unity's AppData/LocalLow/<company>/<product>/Player.log or Unreal's
// AppData/Local/<project>/Saved/Logs/*.log in the bottle, or Unity 5's
// <program>_Data/output_log.txt beside the program. Those name the crash when the report
// stops short.
std::optional<std::string> NewestSessionLog(const std::filesystem::path& data_root,
                                            const std::filesystem::path& program) {
  namespace fs = std::filesystem;
  fs::path newest; fs::file_time_type newest_time{};
  Newest(data_root / "library/logs", [](const std::string& name) { return name.rfind("session-", 0) == 0; },
         &newest, &newest_time);
  if (newest.empty()) return std::nullopt;
  std::string text = "== " + newest.filename().string() + "\n" + FileTail(newest, 128 * 1024, 32 * 1024);
  // Every report in the folder, newest first, and the start of the one before the newest: a run
  // that wrote little, or a rotated report, still shows which runs there were and how they began.
  {
    std::vector<std::pair<fs::file_time_type, fs::path>> reports;
    std::error_code error;
    for (fs::directory_iterator it(data_root / "library/logs", error), end; !error && it != end && reports.size() < 64;
         it.increment(error))
      if (it->path().filename().string().rfind("session-", 0) == 0 && !it->is_symlink(error) && it->is_regular_file(error))
        reports.emplace_back(it->last_write_time(error), it->path());
    std::sort(reports.begin(), reports.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    text += "\n== reports";
    for (const auto& [when, path] : reports) {
      std::error_code size_error;
      const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(when.time_since_epoch()).count();
      text += "\n" + path.filename().string() + " bytes=" + std::to_string(fs::file_size(path, size_error)) +
              " time=" + std::to_string(seconds);
    }
    if (reports.size() > 1) text += "\n== " + reports[1].second.filename().string() + " (start)\n" + FileTail(reports[1].second, 0, 32 * 1024);
  }
  const auto user = data_root / "library/prefixes/game" / playden::steam::cloud::kPrefixUserFolder / "AppData";
  fs::path engine; fs::file_time_type engine_time{};
  for (const auto& company : Folders(user / "LocalLow"))
    for (const auto& product : Folders(company))
      Newest(product, [](const std::string& name) { return name == "Player.log"; }, &engine, &engine_time);
  for (const auto& project : Folders(user / "Local"))
    Newest(project / "Saved/Logs", [](const std::string& name) {
      return name.size() > 4 && name.compare(name.size() - 4, 4, ".log") == 0;
    }, &engine, &engine_time);
  if (!program.empty())
    Newest(program.parent_path() / (program.stem().string() + "_Data"),
           [](const std::string& name) { return name == "output_log.txt"; }, &engine, &engine_time);
  if (!engine.empty()) {
    const auto shown = engine.lexically_relative(data_root);
    text += "\n== " + (shown.empty() || *shown.begin() == ".." ? engine.parent_path().filename() / engine.filename() : shown).string() + "\n" +
            FileTail(engine, 64 * 1024);
  }
  return text;
}

std::optional<std::string> RandomUuid() { return playden::RandomUuid(); }

// Construction order resolves the ownership cycle:
//   repository -> records -> installer -> registry -> coordinator -> owner -> service.
// The repository's availability query consults the coordinator, which is built
// later; the repository constructor never calls it, and Load/Reserve/Adopt only
// run after the whole composition exists. The installer's publication callback and
// the service's driver dereference the owner at call time. Members are destroyed
// in reverse order, so no collaborator outlives what it references.
struct RuntimeHost::Composition {
  explicit Composition(const RuntimeHostConfig& config)
      : storage(config.state_root.string()),
        repository(storage, "installs.json",
                   [this](const std::string& title) {
                     return coordinator ? coordinator->Availability(title) : rt::NativeTitleAvailability{};
                   }),
        records(repository),
        registry(config.registry_transport) {}

  playden::Storage storage;
  // PS5 titles removed while Playden runs: the console keeps their registration until Playden
  // quits, so a new game reusing one fails publication ("ownership is unavailable or incoherent").
  std::set<std::string> retired_titles;
  std::map<std::string, LeftoverDelete> leftover_deletes;
  std::set<std::string> blocked_leftover_titles;
  bool leftover_journals_known = true;
  rt::RuntimeInstallRepository repository;
  rt::RepositoryInstallRecordStore records;
  std::optional<RuntimeKit> kit;
  std::unique_ptr<rt::PackageInstaller> installer;
  ps5::TitleRegistry registry;
  std::unique_ptr<ps5::Ps5NativeCoordinator> coordinator;
  std::unique_ptr<rt::SettingsPublicationOwner> owner;
  std::unique_ptr<rt::RuntimeInstallService> service;
  std::filesystem::path request_directory;
  std::map<std::string, std::string> admission;  // install id -> last admission diagnostic
  std::vector<rt::GameRecipe> recipes;
  std::vector<rt::SettingsPreset> presets;
  std::string settings_data_reason;
  std::unique_ptr<rt::StorageLaunchSessionJournal> launch_journal;
  std::unique_ptr<rt::FilesystemReconcileSessionSource> sessions;
  std::unique_ptr<rt::LaunchService> launch;
  // Last worker preflight per install, with its time, for Handoff.
  std::map<std::string, std::pair<rt::LaunchPreflight, std::int64_t>> preflights;
  // The old and new games root while the 0.6 games move is unfinished.
  std::optional<std::pair<std::string, std::string>> games_move;
};

RuntimeHost::RuntimeHost(RuntimeHostConfig config) : config_(std::move(config)) {
  if (!config_.uuid) config_.uuid = RandomUuid;
  if (!config_.unix_time) config_.unix_time = [] { return static_cast<std::int64_t>(std::time(nullptr)); };
  if (!config_.now_ms)
    config_.now_ms = [] {
      return static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
    };
  if (!config_.moved_games_root)
    config_.moved_games_root = [state = config_.state_root]() -> std::optional<std::pair<std::string, std::string>> {
      const auto journal = rt::LoadLayoutJournal(state);
      if (!journal || journal->step != "games-moved") return std::nullopt;
      return std::make_pair(journal->from, journal->to);
    };
  if (!config_.finish_games_move)
    config_.finish_games_move = [state = config_.state_root](std::string* error) { return rt::MarkLayoutDone(state, error); };
  application_thread_ = pthread_self();
}
RuntimeHost::~RuntimeHost() { Shutdown(); }

bool RuntimeHost::Start() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (started_) return true;
  pthread_attr_t attributes;
  if (pthread_attr_init(&attributes) != 0) return false;
  auto* thread = new pthread_t{};
  const bool ok = pthread_attr_setstacksize(&attributes, config_.stack_bytes) == 0 &&
                  pthread_create(thread, &attributes, &RuntimeHost::Entry, this) == 0;
  pthread_attr_destroy(&attributes);
  if (!ok) {
    delete thread;
    snapshot_.status = RuntimeHostStatus::kUnavailable;
    snapshot_.reason = "The runtime worker could not start.";
    ++snapshot_.revision;
    return false;
  }
  thread_ = thread;
  started_ = true;
  return true;
}

void RuntimeHost::Shutdown() {
  pthread_t* thread = nullptr;
  cancel_downloads_ = true;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
    thread = static_cast<pthread_t*>(thread_);
    thread_ = nullptr;
  }
  wake_.notify_all();
  published_.notify_all();
  if (thread) {
    pthread_join(*thread, nullptr);
    delete thread;
  }
}

bool RuntimeHost::WaitForStartup() {
  std::unique_lock<std::mutex> lock(mutex_);
  published_.wait(lock, [this] { return stopping_ || snapshot_.status != RuntimeHostStatus::kStarting; });
  return snapshot_.status == RuntimeHostStatus::kReady;
}

RuntimeSnapshot RuntimeHost::Snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return snapshot_;
}

std::shared_ptr<const RuntimeSnapshot> RuntimeHost::Shared() const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!shared_ || shared_revision_ != snapshot_.revision) {
    shared_ = std::make_shared<const RuntimeSnapshot>(snapshot_);
    shared_revision_ = snapshot_.revision;
  }
  return shared_;
}

// Every program of the folder wrapped in SteamStub, with the verdict and the form found, so a
// program the Program list greys out says why (#320).
void RuntimeHost::LogSteamStubPrograms(std::uint32_t app, const std::vector<rt::GameProgram>& programs) const {
  for (const auto& program : programs)
    if (program.steam_stub)
      Log("steamstub app=" + std::to_string(app) + " program=" + program.path +
          " supported=" + (program.steam_stub_supported ? "1" : "0") +
          " detail=" + (program.steam_stub_detail.empty() ? "unknown" : program.steam_stub_detail));
}

void RuntimeHost::Log(const std::string& line) const {
  if (config_.log) config_.log(line);
}

bool RuntimeHost::Stopping() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return stopping_;
}

void RuntimeHost::Prepare(RuntimePrepareRequest request) {
  request.game = GameOf(request.game, request.runtime_number);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) return;
    // A game asked for again keeps its place, with the newer request (launch options may have arrived).
    const auto queued = std::find_if(prepare_requests_.begin(), prepare_requests_.end(),
                                     [&](const auto& waiting) { return waiting.runtime_number == request.runtime_number; });
    if (queued != prepare_requests_.end()) {
      *queued = std::move(request);
    } else {
      prepare_order_[request.runtime_number] = ++next_work_order_;
      snapshot_.queued_prepares.push_back(request.runtime_number);
      prepare_requests_.push_back(std::move(request));
    }
    ++snapshot_.revision;
  }
  published_.notify_all();
  wake_.notify_all();
}

bool RuntimeHost::PrepareNext(std::uint32_t app_id) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto queued = std::find_if(prepare_requests_.begin(), prepare_requests_.end(),
                                     [&](const auto& waiting) { return waiting.runtime_number == app_id; });
    if (queued == prepare_requests_.end()) return false;
    // The others keep their order behind it.
    std::vector<std::uint64_t> orders;
    for (const auto& request : prepare_requests_) orders.push_back(prepare_order_.at(request.runtime_number));
    std::rotate(prepare_requests_.begin(), queued, queued + 1);
    for (std::size_t i = 0; i < prepare_requests_.size(); ++i)
      prepare_order_[prepare_requests_[i].runtime_number] = orders[i];
    snapshot_.queued_prepares.clear();
    for (const auto& waiting : prepare_requests_) snapshot_.queued_prepares.push_back(waiting.runtime_number);
    ++snapshot_.revision;
  }
  published_.notify_all();
  return true;
}

bool RuntimeHost::WithdrawPrepare(std::uint32_t app_id) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto queued = std::find_if(prepare_requests_.begin(), prepare_requests_.end(),
                                     [&](const auto& waiting) { return waiting.runtime_number == app_id; });
    if (queued == prepare_requests_.end()) return false;
    prepare_requests_.erase(queued);
    prepare_order_.erase(app_id);
    std::erase(snapshot_.queued_prepares, app_id);
    ++snapshot_.revision;
  }
  published_.notify_all();
  return true;
}

void RuntimeHost::DismissPrepared(std::uint32_t app_id, std::int64_t finished_at) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) return;
    prepared_dismissals_.push_back({app_id, finished_at});
  }
  wake_.notify_all();
}

void RuntimeHost::ClearPrepared() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) return;
    prepared_clear_requested_ = true;
  }
  wake_.notify_all();
}

namespace {
constexpr const char* kPreparedHistory = "prepared-history";
}  // namespace

void RuntimeHost::LoadPreparedHistory() {
  prepared_history_.clear();
  // stdio, not iostream: the console's libc has no locale-aware number parsing.
  FILE* file = std::fopen((config_.state_root / kPreparedHistory).c_str(), "r");
  if (!file) return;
  char line[64];
  // Oldest first: a long file keeps its newest entries.
  while (std::fgets(line, sizeof(line), file)) {
    char* end = nullptr;
    const auto app = std::strtoul(line, &end, 10);
    const auto finished = std::strtoll(end, nullptr, 10);
    if (app == 0 || app > UINT32_MAX || finished <= 0) continue;
    prepared_history_.push_back({static_cast<std::uint32_t>(app), static_cast<std::int64_t>(finished)});
    if (prepared_history_.size() > kMaxPrepared * 8) prepared_history_.erase(prepared_history_.begin());
  }
  std::fclose(file);
}

void RuntimeHost::SavePreparedHistory() {
  // Only what Queue could still list is kept.
  const auto now = config_.unix_time();
  std::erase_if(prepared_history_, [now](const PreparedGame& entry) {
    return entry.finished_at > now || now - entry.finished_at > kPreparedDays * 24 * 60 * 60;
  });
  if (prepared_history_.size() > kMaxPrepared * 8)
    prepared_history_.erase(prepared_history_.begin(),
                            prepared_history_.end() - static_cast<std::ptrdiff_t>(kMaxPrepared * 8));
  std::string text;
  for (const auto& entry : prepared_history_)
    text += std::to_string(entry.runtime_number) + "\t" + std::to_string(entry.finished_at) + "\n";
  const auto path = config_.state_root / kPreparedHistory;
  const auto temporary = path.string() + ".playden-partial";
  const int out = open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (out < 0) {
    Log("prepared history not saved: " + std::string(std::strerror(errno)));
    return;
  }
  std::size_t written = 0;
  while (written < text.size()) {
    const auto count = write(out, text.data() + written, text.size() - written);
    if (count <= 0) break;
    written += static_cast<std::size_t>(count);
  }
  const bool ok = written == text.size() && fsync(out) == 0;
  if (close(out) != 0 || !ok || rename(temporary.c_str(), path.c_str()) != 0) {
    unlink(temporary.c_str());
    Log("prepared history not saved");
  }
}

std::vector<PreparedGame> RuntimeHost::ListPrepared(const Composition* c) const {
  // A game uninstalled, being removed or being prepared again is not listed as Ready.
  std::set<std::uint32_t> ready;
  if (c)
    for (const auto& record : c->repository.Snapshot())
      if (record.state == rt::PreparationState::kReady && !record.journal) ready.insert(record.runtime_number);
  const auto now = config_.unix_time();
  std::vector<PreparedGame> listed;
  for (auto it = prepared_history_.rbegin(); it != prepared_history_.rend() && listed.size() < kMaxPrepared; ++it)
    if (ready.count(it->runtime_number) && it->finished_at <= now && now - it->finished_at <= kPreparedDays * 24 * 60 * 60)
      listed.push_back(*it);
  std::stable_sort(listed.begin(), listed.end(),
                   [](const PreparedGame& left, const PreparedGame& right) { return left.finished_at > right.finished_at; });
  return listed;
}

void RuntimeHost::PublishPrepared(const Composition* c) {
  auto listed = ListPrepared(c);
  Publish([&](RuntimeSnapshot& s) {
    if (s.preparing_app) std::erase_if(listed, [&s](const PreparedGame& entry) { return entry.runtime_number == *s.preparing_app; });
    if (s.prepared != listed) s.prepared = std::move(listed);
  });
}

void RuntimeHost::HoldPreparations(bool hold) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (prepares_held_ == hold) return;
    prepares_held_ = hold;
    // A new revision: a quit waiting for the queue to hold checks again whether anything still runs.
    ++snapshot_.revision;
  }
  published_.notify_all();
  wake_.notify_all();
}

void RuntimeHost::Play(std::uint32_t app_id, std::string setup_program) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) return;
    play_requests_.push_back({app_id, std::move(setup_program)});
  }
  wake_.notify_all();
}

void RuntimeHost::Remove(std::uint32_t app_id, bool delete_saves) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) return;
    remove_requests_.push_back({app_id, delete_saves, std::nullopt, ++next_work_order_});
  }
  wake_.notify_all();
}

void RuntimeHost::DeleteKeptBottle(std::uint32_t app_id) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) return;
    kept_delete_requests_.push_back(app_id);
  }
  wake_.notify_all();
}

bool RuntimeHost::DeleteLeftoverTitle(const std::string& title_id, std::string* error) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) return false;
    const auto found = std::find_if(snapshot_.leftover_titles.begin(), snapshot_.leftover_titles.end(),
                                    [&](const auto& title) { return title.title_id == title_id; });
    if (found == snapshot_.leftover_titles.end()) {
      if (error) *error = "This leftover PS5 title is no longer available.";
      return false;
    }
    if (snapshot_.deleting_leftover_title == title_id ||
        std::find(snapshot_.queued_leftover_deletes.begin(), snapshot_.queued_leftover_deletes.end(), title_id) !=
            snapshot_.queued_leftover_deletes.end()) return true;
    remove_requests_.push_back({0, false, LeftoverDelete{*found, {}, "retire"}, ++next_work_order_});
    snapshot_.queued_leftover_deletes.push_back(title_id);
    snapshot_.leftover_delete_failures.erase(title_id);
    ++snapshot_.revision;
  }
  published_.notify_all();
  wake_.notify_all();
  return true;
}

void RuntimeHost::ScanPrograms(std::uint32_t app_id, std::string install_path) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) return;
    scan_requests_.emplace_back(app_id, std::move(install_path));
  }
  wake_.notify_all();
}

void RuntimeHost::MeasureSaves() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) return;
    measure_requested_ = true;
  }
  wake_.notify_all();
}

void RuntimeHost::ReloadKit() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) return;
    kit_reload_requested_ = true;
  }
  wake_.notify_all();
}

void RuntimeHost::Maintain(std::uint32_t app_id, RuntimeMaintenance action) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) return;
    maintenance_requests_.emplace_back(app_id, action);
    snapshot_.maintenance[app_id] = {action, true, false, {}};
    ++snapshot_.revision;
  }
  wake_.notify_all();
}

void RuntimeHost::SetSteamAccount(std::string account_name, std::uint64_t steam_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  steam_account_name_ = std::move(account_name);
  steam_id_ = steam_id;
}

// A kit replaced after startup (an upload with --replace) keeps this Playden on the old kit's paths,
// which are gone: say so instead of failing on the first missing file.
std::string RuntimeHost::KitReplaced(const Composition& c) const {
  if (!c.kit || c.kit->manifest_sha256.empty() || !config_.package_operations.hash_file) return {};
  const auto current = config_.package_operations.hash_file(c.kit->view.root / "manifest.json");
  if (current && *current == c.kit->manifest_sha256) return {};
  return "The runtime kit changed while Playden was running. Quit Playden and start it again.";
}

// What a game's preparation steps work from: its recipe and effective settings, its folder, the bottle
// and the programs that may start.
struct RuntimeHost::GameFiles {
  const rt::RuntimeInstall& record;
  const rt::GameRecipe* recipe = nullptr;
  std::optional<rt::GameSettingsRecord> saved;
  rt::EffectiveSettings settings;
  std::filesystem::path root;  // the game folder, as this process sees it
  std::string prefix;          // the game's bottle, as this process sees it
  std::vector<std::string> programs;
  // The INI values to apply: the recipe's, or for a game still on the recipe Playden generated, those of the recipe
  // Playden now ships for it (they touch only the game's own files, which the generated recipe doesn't name).
  const std::vector<rt::IniValue>* ini = nullptr;
};

namespace {
// A steam_appid.txt's app ID (ParseSteamAppId); nothing when there is none or it holds something else.
std::optional<std::uint32_t> ReadSteamAppIdFile(const std::filesystem::path& path) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) return std::nullopt;
  struct stat info {};
  char buffer[64];
  ssize_t count = -1;
  if (::fstat(fd, &info) == 0 && S_ISREG(info.st_mode) && info.st_size < static_cast<off_t>(sizeof(buffer)))
    count = ::read(fd, buffer, sizeof(buffer));
  ::close(fd);
  if (count <= 0 || count >= static_cast<ssize_t>(sizeof(buffer))) return std::nullopt;
  std::string_view text(buffer, static_cast<std::size_t>(count));
  if (text.substr(0, 3) == "\xEF\xBB\xBF") text.remove_prefix(3);  // a UTF-8 byte order mark
  return rt::ParseSteamAppId(text);
}
}  // namespace

// Steam games: their own app ID. Other stores: the game's steam_appid.txt (the folder of the program that
// starts, then the game folder), else the one in Game settings, else none. The runtime number is never one.
SteamworksAppView RuntimeHost::SteamworksAppFor(const GameFiles& files) const {
  SteamworksAppView view;
  auto game = GameOf(files.record);
  // A record copy that doesn't carry its game (read back from a journal) is another store's when its
  // number is one of theirs: its number is not a Steam app ID.
  if (files.record.game == GameId{}) {
    std::lock_guard<std::mutex> lock(numbers_mutex_);
    for (const auto& [known, number] : numbers_)
      if (number == files.record.runtime_number) game = known;
  }
  if (const auto app = game.steam_app()) {
    view.app_id = *app;
    view.source = SteamworksAppView::Source::kStore;
    return view;
  }
  auto program = files.record.launch_path;
  std::replace(program.begin(), program.end(), '\\', '/');
  if (program.empty() && !files.programs.empty()) program = files.programs.front();
  std::vector<std::filesystem::path> places;
  if (!program.empty()) places.push_back((files.root / program).parent_path());
  if (places.empty() || places.front().lexically_normal() != files.root.lexically_normal()) places.push_back(files.root);
  for (const auto& folder : places)
    if (const auto app = ReadSteamAppIdFile(folder / "steam_appid.txt")) {
      view.app_id = *app;
      view.source = SteamworksAppView::Source::kGameFile;
      break;
    }
  const auto field = files.saved ? files.saved->steam_app_id : 0;
  if (view.app_id) {
    view.overridden = field && field != view.app_id;
  } else if (field) {
    view.app_id = field;
    view.source = SteamworksAppView::Source::kSettings;
  }
  return view;
}

// Generic step: the kit's media support in the bottle.
void RuntimeHost::MediaStep(Composition& c, const rt::RuntimeInstall& record) {
  if (!c.kit) return;
  std::string reason;
  // Media support is an upgrade for older bottles, not a condition to play: a game that doesn't use it
  // still starts, and the next Play tries again.
  if (!rt::ApplyMediaRegistry((c.kit->view.root / c.kit->view.runtime.path).string(),
                             (HostPath(record.data_root) / "library/prefixes/game").string(),
                             config_.unix_time(), &reason))
    Log("media upgrade app=" + std::to_string(record.runtime_number) + " skipped: " + reason);
}

// Steam-file step: a program wrapped in SteamStub can't start without the Steam client, whichever Steam
// library the game uses, so it is unpacked first (runtime/steam_stub.h).
std::string RuntimeHost::SteamStubStep(const GameFiles& files) {
  const auto& record = files.record;
  const auto unpacked = rt::UnpackSteamStubPrograms(files.root.string(), files.programs);
  if (!unpacked.found.empty() || !unpacked.ok) {
    std::string found, changed;
    for (const auto& path : unpacked.found) found += (found.empty() ? "" : ",") + path;
    for (const auto& path : unpacked.changed) changed += (changed.empty() ? "" : ",") + path;
    Log("steamstub app=" + std::to_string(record.runtime_number) + " found=" + found + " changed=" + (changed.empty() ? "-" : changed) +
        (unpacked.ok ? std::string() : " failed=" + unpacked.message));
  }
  return unpacked.ok ? std::string() : unpacked.message;
}

// Steam-file step: the game's steam_api DLLs in the state its "Steam features" setting asks for, emulated
// with its Steam app ID (SteamworksAppFor) and the signed-in account, or the game's own; then the legacy
// steam.dll of a game from before Steamworks.
std::string RuntimeHost::SteamworksStep(const GameFiles& files, bool steam_account_known) {
  const auto& record = files.record;
  const auto& root = files.root;
  const auto steam_app = SteamworksAppFor(files);
  const bool other_store = steam_app.source != SteamworksAppView::Source::kStore;
  if (other_store) {
    const char* source = steam_app.source == SteamworksAppView::Source::kGameFile ? "game-file"
                         : steam_app.source == SteamworksAppView::Source::kSettings ? "settings" : "none";
    Log("steamworks app=" + std::to_string(record.runtime_number) + " steam_app_id=" +
        (steam_app.app_id ? std::to_string(steam_app.app_id) : std::string("-")) + " source=" + source +
        (steam_app.overridden ? " overridden=settings" : ""));
    bool changed = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      const auto found = snapshot_.steamworks_apps.find(record.runtime_number);
      changed = found == snapshot_.steamworks_apps.end() || found->second != steam_app;
    }
    if (changed) Publish([&](RuntimeSnapshot& s) { s.steamworks_apps[record.runtime_number] = steam_app; });
  }
  // Without a Steam app ID there is nothing to emulate: the game keeps its own Steam files.
  const bool emulate = files.settings.values.at("steam_features").value == "emulate" && steam_app.app_id != 0;
  rt::SteamworksResult result;
  // The emulator's files name the signed-in account; before it is known they keep what Play wrote.
  const bool emulator_waits = emulate && !steam_account_known;
  if (emulator_waits) {
    Log("steamworks app=" + std::to_string(record.runtime_number) + " mode=emulate waits: the Steam account is not known yet");
  } else if (emulate) {
    rt::SteamworksIdentity identity;
    identity.app_id = steam_app.app_id;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      identity.account_name = steam_account_name_;
      identity.steam_id = steam_id_;
    }
    const bool facts = rt::ReadSteamworksFacts(root.string(), &identity);
    if (!facts)
      Log("steamworks app=" + std::to_string(record.runtime_number) + " no download facts: no DLC or depots listed");
    // DLC the account owns that the download didn't record. They stay in the facts, so a start before the
    // next license read still has them.
    if (config_.owned_dlc) {
      const auto listed = [](const std::vector<std::uint32_t>& ids, std::uint32_t id) {
        return std::find(ids.begin(), ids.end(), id) != ids.end();
      };
      std::vector<std::uint32_t> added;
      for (const auto dlc : config_.owned_dlc(identity.steam_id, steam_app.app_id))
        if (dlc != steam_app.app_id && !listed(identity.owned_dlc, dlc) && !listed(added, dlc)) added.push_back(dlc);
      if (!added.empty()) {
        identity.owned_dlc.insert(identity.owned_dlc.end(), added.begin(), added.end());
        const bool kept = !facts || other_store ||
                          rt::WriteSteamworksFacts(root.string(), identity.depots, identity.owned_dlc, identity.language);
        Log("steamworks app=" + std::to_string(record.runtime_number) + " owned dlc added=" + std::to_string(added.size()) +
            " total=" + std::to_string(identity.owned_dlc.size()) + (kept ? "" : " facts=write-failed"));
      }
    }
    const auto emulator = config_.steamworks_dir.empty() ? config_.runtime_dir + "/steamworks" : config_.steamworks_dir;
    // A game from another store keeps its folder as it shipped: no steam_appid.txt of Playden's would hide
    // the game's lack of one, so a later app ID in Game settings still reaches the emulator.
    identity.write_game_appid_file = !other_store;
    result = rt::EmulateSteamworks(root.string(), emulator, identity);
  } else {
    result = rt::RestoreSteamworks(root.string());
  }
  if (!emulator_waits) {
    std::string changed;
    for (const auto& path : result.changed) changed += (changed.empty() ? "" : ",") + path;
    Log("steamworks app=" + std::to_string(record.runtime_number) + " mode=" + (emulate ? "emulate" : "game") +
        " found=" + std::to_string(result.found.size()) + " changed=" + (changed.empty() ? "-" : changed) +
        (result.ok ? std::string() : " failed=" + result.message));
    if (!result.ok) return result.message;
  }
  // A game from before Steamworks (no steam_api DLL) checks for Steam through steam.dll instead
  // (GUN: "Failed to find Steam."); emulation gives it the minimal one, the game's mode takes it away.
  if (!emulator_waits && (!emulate || result.found.empty())) {
    const auto emulator = config_.steamworks_dir.empty() ? config_.runtime_dir + "/steamworks" : config_.steamworks_dir;
    const auto legacy = emulate ? rt::ProvideLegacySteam(root.string(), emulator, files.programs)
                                : rt::RemoveLegacySteam(root.string(), files.programs);
    if (!legacy.changed.empty() || !legacy.ok) {
      std::string added;
      for (const auto& path : legacy.changed) added += (added.empty() ? "" : ",") + path;
      Log("legacy steam app=" + std::to_string(record.runtime_number) + (emulate ? " added=" : " removed=") + added +
          (legacy.ok ? std::string() : " failed=" + legacy.message));
    }
    if (!legacy.ok) return legacy.message;
  }
  return {};
}

// Generic step: an Unreal Engine 3 game asks for its EULA through a second program the PS5 cannot start.
std::string RuntimeHost::Ue3EulaStep(const GameFiles& files) {
  const auto& record = files.record;
  const auto eula = rt::AcceptUe3Eula(files.root.string(), files.programs, files.prefix, config_.unix_time());
  if (eula.found)
    Log("ue3 eula app=" + std::to_string(record.runtime_number) + " guid=" + (eula.guid.empty() ? "-" : eula.guid) +
        " written=" + (eula.written ? "1" : "0") + (eula.ok ? std::string() : " failed=" + eula.message));
  if (!eula.ok) return "The game's EULA could not be marked accepted: " + eula.message + ".";
  return {};
}

// Generic step: Direct3D 12 through vkd3d-proton: its DLLs go into the bottle, where the profile's overrides
// load them.
std::string RuntimeHost::Direct3D12Step(Composition& c, const GameFiles& files) {
  if (!c.kit) return {};
  const auto& record = files.record;
  const auto& settings = files.settings;
  std::string reason;
  if (!rt::ApplyD3D12Library((c.kit->view.root / c.kit->view.runtime.path).string(),
                             (c.kit->view.root / c.kit->view.template_path).string(), files.prefix,
                             settings.vkd3d_proton, &reason)) {
    Log("d3d12 app=" + std::to_string(record.runtime_number) + " vkd3d-proton=" + (settings.vkd3d_proton ? "1" : "0") +
        " failed=" + reason);
    return std::string(settings.vkd3d_proton ? "vkd3d-proton could not be set up: "
                                             : "Wine's Direct3D 12 could not be put back: ") + reason + ".";
  } else if (settings.vkd3d_proton) {
    Log("d3d12 app=" + std::to_string(record.runtime_number) + " vkd3d-proton=1");
  }
  return {};
}

// Generic step: the bottle's Wine Mono path follows the kit's (a bottle copied from a kit under another id names
// that folder). Logged, never a refusal: only .NET programs need it, and the next Play tries again.
void RuntimeHost::KitMonoStep(Composition& c, const rt::RuntimeInstall& record, const char* when) {
  if (!c.kit || record.data_root.empty()) return;
  bool changed = false;
  std::string reason;
  const auto ok = rt::PointBottleAtKitMono((c.kit->view.root / c.kit->view.template_path).string(),
                                           (HostPath(record.data_root) / "library/prefixes/game").string(),
                                           config_.unix_time(), &changed, &reason);
  if (changed || !ok)
    Log(std::string("mono path app=") + std::to_string(record.runtime_number) + " when=" + when +
        (ok ? " pointed at kit " + c.kit->view.kit_id : " failed=" + reason));
}

// Generic step: the bottle's Visual C++ runtime key says the version of the kit's builtin DLLs, which Unreal's root
// launcher checks before it starts the game. Logged, never a refusal: most games never read it, and the next Play
// tries again.
void RuntimeHost::VcRuntimeStep(const GameFiles& files) {
  bool changed = false;
  std::string reason;
  const auto ok = rt::RaiseVcRuntimeVersion(files.prefix, config_.unix_time(), &changed, &reason);
  if (changed || !ok)
    Log("vc runtime app=" + std::to_string(files.record.runtime_number) +
        (ok ? std::string(" version=") + rt::kVcRuntimeVersion : " failed=" + reason));
}

// Generic step: vkd3d-proton's variables reach the game through the bottle's Environment key. Some change how
// it draws, so a bottle that exists but cannot take them stops the start; one not made yet gets them on the
// next Play.
std::string RuntimeHost::BottleEnvironmentStep(const GameFiles& files) {
  struct stat hive {};
  std::string reason;
  if (::lstat((files.prefix + "/user.reg").c_str(), &hive) == 0 &&
      !rt::ApplyBottleRegistryValues(rt::RenderBottleEnvironment(files.settings), files.prefix, config_.unix_time(),
                                     "user.reg", &reason)) {
    Log("bottle environment app=" + std::to_string(files.record.runtime_number) + " failed=" + reason);
    return "The game's vkd3d-proton variables could not be written: " + reason + ".";
  }
  return {};
}

// Steam-file step: registry keys Steam's install script writes on a game's first start (Fallout: New Vegas's
// launcher wants its install path there), and the redistributables those scripts run.
std::string RuntimeHost::InstallScriptsStep(const GameFiles& files) {
  const auto& record = files.record;
  const auto install_dir = rt::ProsperoWindowsPath(record.install_path);
  if (!install_dir) return {};
  const auto script = rt::ApplyInstallScripts(files.root.string(), *install_dir, files.prefix, config_.unix_time());
  if (!script.found.empty()) {
    std::string names;
    for (const auto& name : script.found) names += (names.empty() ? "" : ",") + name;
    Log("install script app=" + std::to_string(record.runtime_number) + " found=" + names +
        " values=" + std::to_string(script.values) + " skipped=" + std::to_string(script.skipped.size()) +
        (script.ok ? std::string() : " failed=" + script.message));
  }
  if (!script.ok) return "The game's install script could not be applied: " + script.message + ".";
  // The redistributables those scripts run (PhysX and the like): packages Playden can install
  // go into the bottle once; a package that fails is logged and the game still starts.
  const auto redist = rt::InstallRedistributables(files.root.string(), *install_dir, files.prefix, config_.unix_time());
  // Names and reasons come from the game's files: one line each, whatever they hold.
  const auto field = [](std::string text) {
    if (text.size() > 240) text.resize(240);
    for (auto& value : text)
      if (static_cast<unsigned char>(value) < 0x20 || value == 0x7f) value = '?';
    return text;
  };
  for (const auto& outcome : redist.outcomes)
    if (outcome.result != "already" && outcome.result != "covered")
      Log("redist app=" + std::to_string(record.runtime_number) + " script=" + field(outcome.script) + " name=" +
          field(outcome.name) + " result=" + outcome.result + " detail=" + field(outcome.detail) +
          (outcome.files ? " files=" + std::to_string(outcome.files) : std::string()));
  return {};
}

// Sets what MfcStep may do for the work in its scope (RedistWork), and puts the default back after it.
struct RuntimeHost::RedistWorkScope {
  RedistWork& work;
  RedistWorkScope(RedistWork& target, bool downloads, std::optional<std::uint32_t> play_app = std::nullopt)
      : work(target) {
    work = RedistWork{downloads, play_app, {}};
  }
  ~RedistWorkScope() { work = RedistWork{}; }
  RedistWorkScope(const RedistWorkScope&) = delete;
  RedistWorkScope& operator=(const RedistWorkScope&) = delete;
};

// Generic step: Microsoft's MFC for a game whose files import it (Wine has none; a Unity plugin of Moonlighter's
// stops the game without it), downloaded once per console and installed into the bottle once. An upgrade, not a
// condition: a failed download or install is logged, the game still starts, and the next Play tries again. Only
// work the player started downloads (redist_work_); start-up and other background work install from a kept copy
// or leave it for Play.
void RuntimeHost::MfcStep(const GameFiles& files) {
  const auto app = std::to_string(files.record.runtime_number);
  struct stat hive {};
  if (::lstat((files.prefix + "/system.reg").c_str(), &hive) != 0) return;  // the bottle comes later
  // Paths come from the game's files: one line each, whatever they hold.
  const auto field = [](std::string text) {
    if (text.size() > 240) text.resize(240);
    for (auto& value : text)
      if (static_cast<unsigned char>(value) < 0x20 || value == 0x7f) value = '?';
    return text;
  };
  const auto redist_for = [&](const char* architecture) -> const rt::VcRedist* {
    for (const auto& candidate : config_.vc_redists)
      if (candidate.architecture == architecture) return &candidate;
    return nullptr;
  };
  // The latest scan stands while the game's files are the ones it read, or while everything it found is
  // installed; otherwise the folder is walked again.
  const auto fingerprint = rt::GameFilesFingerprint(files.root.string());
  auto scan = rt::ReadMfcScan(files.prefix);
  const auto installed = [&](const std::string& program, const char* architecture) {
    const auto* redist = redist_for(architecture);
    return program.empty() || (redist && rt::MfcInstalled(*redist, files.prefix));
  };
  const bool reuse = scan && ((!fingerprint.empty() && scan->fingerprint == fingerprint) ||
                              ((!scan->x86_program.empty() || !scan->x64_program.empty()) &&
                               installed(scan->x86_program, "x86") && installed(scan->x64_program, "x64")));
  if (!reuse) {
    const auto began = std::chrono::steady_clock::now();
    const auto found = rt::FindMfcImports(files.root.string());
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began).count();
    Log("mfc scan app=" + app + " programs=" + std::to_string(found.programs) + " folders=" + std::to_string(found.folders) +
        " ms=" + std::to_string(ms) + (found.truncated ? " stopped early" : "") + " x86=" +
        (found.x86_program.empty() ? "-" : field(found.x86_program)) + " x64=" +
        (found.x64_program.empty() ? "-" : field(found.x64_program)));
    scan = rt::MfcScanRecord{fingerprint, found.x86_program, found.x64_program};
    if (!rt::WriteMfcScan(files.prefix, *scan)) Log("mfc scan app=" + app + " could not be kept in the bottle");
  }
  const auto cache = HostPath(config_.package_paths.console_data_root + "/playden/redist").string();
  for (const auto& [architecture, program] : {std::pair<const char*, std::string>{"x86", scan->x86_program},
                                              std::pair<const char*, std::string>{"x64", scan->x64_program}}) {
    if (program.empty()) continue;
    const auto* redist = redist_for(architecture);
    const auto line = "mfc upgrade app=" + app + " arch=" + architecture + " program=" + field(program);
    if (!redist) {
      Log(line + " skipped: no redistributable for it");
      continue;
    }
    if (rt::MfcInstalled(*redist, files.prefix)) continue;
    // A download that failed a moment ago (the same preparation, or a Play right after) isn't tried again yet;
    // a kept copy still installs.
    const auto began = std::chrono::steady_clock::now();
    const auto failed = redist_download_failed_.find(redist->sha256);
    const bool waits = failed != redist_download_failed_.end() && began - failed->second < config_.redist_retry_wait;
    const bool downloads = redist_work_.downloads && !waits && config_.fetch_range;
    bool shown = false;
    rt::RangeFetch fetch;
    if (downloads)
      fetch = [&](const std::string& url, std::uint64_t offset, std::uint64_t length, std::string* error) -> std::optional<std::string> {
        if (cancel_downloads_) {
          *error = "Playden is closing";
          return std::nullopt;
        }
        if (redist_work_.play_app && !shown) {
          shown = true;
          Publish([&](RuntimeSnapshot& s) { s.play_progress[*redist_work_.play_app] = kPlayDownloadingMfc; });
        }
        return config_.fetch_range(url, offset, length, cancel_downloads_, error);
      };
    const auto result = rt::InstallMfcRuntime(*redist, cache, files.prefix, fetch, config_.unix_time());
    if (shown) Publish([&](RuntimeSnapshot& s) { s.play_progress.erase(*redist_work_.play_app); });
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began).count();
    if (result.result == "already") continue;
    if (result.result == "installed") {
      redist_download_failed_.erase(redist->sha256);
      Log(line + " installed files=" + std::to_string(result.files) + " from=" + result.source + " ms=" + std::to_string(ms) +
          (result.cache_problem.empty() ? std::string() : " not kept: " + result.cache_problem));
      continue;
    }
    if (result.download_failed) redist_download_failed_[redist->sha256] = std::chrono::steady_clock::now();
    const bool not_downloaded = !fetch && !waits;  // no kept copy, and this work doesn't download
    if (not_downloaded)
      Log(line + " waits for Play: " + (redist_work_.downloads ? std::string("Playden can't download here")
                                                                 : std::string("start-up and background work don't download")));
    else if (waits && !result.download_failed)
      Log(line + " skipped: its download failed " +
          std::to_string(std::chrono::duration_cast<std::chrono::seconds>(began - failed->second).count()) +
          " s ago; it is tried again from " + std::to_string(config_.redist_retry_wait.count()) + " s");
    else
      Log(line + " skipped: " + result.detail);
    if (redist_work_.play_app)
      redist_work_.notice = result.download_failed || waits || not_downloaded ? kPlayMfcNotDownloaded : kPlayMfcNotInstalled;
  }
}

// Generic step: registry values the recipe carries (docs/community-recipes.md), written the way an install
// script's are.
std::string RuntimeHost::RecipeRegistryStep(const GameFiles& files) {
  const auto& registry = files.recipe->registry;
  if (registry.empty()) return {};
  std::string reason;
  const bool ok = rt::ApplyBottleRegistryValues(registry, files.prefix, config_.unix_time(), "user.reg", &reason);
  Log("recipe registry app=" + std::to_string(files.record.runtime_number) + " values=" + std::to_string(registry.size()) +
      (ok ? std::string() : " failed=" + reason));
  if (!ok) return "The recipe's registry values could not be applied: " + reason + ".";
  return {};
}

// Generic step: values the recipe puts in the game's own INI files (docs/community-recipes.md), only where the key
// is missing or empty, for a setting the game's launcher would write before starting the game.
std::string RuntimeHost::RecipeIniStep(const GameFiles& files) {
  if (!files.ini || files.ini->empty()) return {};
  const auto& ini = *files.ini;
  const auto applied = rt::ApplyRecipeIniValues(files.root.string(), ini);
  std::string changed, skipped;
  for (const auto& file : applied.changed) changed += (changed.empty() ? "" : ",") + file;
  for (const auto& file : applied.skipped) skipped += (skipped.empty() ? "" : ",") + file;
  Log("recipe ini app=" + std::to_string(files.record.runtime_number) + " values=" + std::to_string(ini.size()) +
      " changed=" + (changed.empty() ? "-" : changed) + (skipped.empty() ? std::string() : " skipped=" + skipped) +
      (applied.ok ? std::string() : " failed=" + applied.message));
  return applied.ok ? std::string() : applied.message;
}

// Generic step: self-contained .NET games reserve more address space than the console grants unless their
// garbage collector is given a heap limit (runtime/dotnet_heap.h).
std::string RuntimeHost::DotnetHeapStep(const GameFiles& files) {
  const auto dotnet = rt::LimitDotnetHeap(files.root.string());
  if (!dotnet.found.empty() || !dotnet.ok) {
    std::string limited;
    for (const auto& path : dotnet.changed) limited += (limited.empty() ? "" : ",") + path;
    Log("dotnet heap app=" + std::to_string(files.record.runtime_number) + " found=" + std::to_string(dotnet.found.size()) +
        " changed=" + (limited.empty() ? "-" : limited) + (dotnet.ok ? std::string() : " failed=" + dotnet.message));
  }
  return dotnet.ok ? std::string() : dotnet.message;
}

// The steps run in this order for every game, whatever its store: generic ones (media, EULA, Direct3D 12, Wine
// Mono, the bottle's Environment, MFC, the Visual C++ runtime key, the recipe's registry and INI values, the .NET
// heap) and Steam-file ones, which act only on the Steam files a game has (SteamStub, Steamworks and steam.dll,
// install scripts and redistributables).
std::string RuntimeHost::ApplyPreparationSteps(Composition& c, const rt::RuntimeInstall& record, bool steam_account_known) {
  MediaStep(c, record);
  GameFiles files{record};
  for (const auto& candidate : c.recipes)
    if (RecipeFits(candidate, record) && candidate.id == record.recipe_id) files.recipe = &candidate;
  if (!files.recipe) return {};  // publication reports the missing recipe
  files.ini = &files.recipe->ini;
  if (rt::IsGeneratedRecipeId(files.recipe->id))
    for (const auto& candidate : c.recipes)
      if (rt::RecipeGame(candidate) == GameOf(record) && candidate.app_id == record.runtime_number &&
          !rt::IsGeneratedRecipeId(candidate.id))
        files.ini = &candidate.ini;
  files.saved = config_.settings_record ? config_.settings_record(GameOf(record)) : std::nullopt;
  const rt::SettingsPreset* preset = nullptr;
  if (files.saved)
    for (const auto& candidate : c.presets)
      if (candidate.id == files.saved->preset_id) preset = &candidate;
  rt::SettingsError error;
  static const std::map<std::string, std::string> none;
  const auto kit = TitleKit(c, record);
  if (!rt::BuildEffectiveSettings(*files.recipe, preset, files.saved ? files.saved->overrides : none, kit, &files.settings,
                                  &error))
    return {};  // publication reports invalid settings
  files.root = config_.host_namespace_prefix.empty()
      ? std::filesystem::path(record.install_path)
      : config_.host_namespace_prefix / std::filesystem::path(record.install_path).relative_path();
  files.prefix = (HostPath(record.data_root) / "library/prefixes/game").string();
  for (const auto& entry : files.recipe->launch_entries) {
    auto program = entry.executable;
    std::replace(program.begin(), program.end(), '\\', '/');
    files.programs.push_back(std::move(program));
  }
  // A program picked from the game folder may not be one of the recipe's.
  if (auto own = record.launch_path; !own.empty()) {
    std::replace(own.begin(), own.end(), '\\', '/');
    if (std::find(files.programs.begin(), files.programs.end(), own) == files.programs.end())
      files.programs.push_back(std::move(own));
  }
  if (auto refused = SteamStubStep(files); !refused.empty()) return refused;
  if (auto refused = SteamworksStep(files, steam_account_known); !refused.empty()) return refused;
  if (auto refused = Ue3EulaStep(files); !refused.empty()) return refused;
  if (auto refused = Direct3D12Step(c, files); !refused.empty()) return refused;
  KitMonoStep(c, record, "prepare");
  if (auto refused = BottleEnvironmentStep(files); !refused.empty()) return refused;
  if (auto refused = InstallScriptsStep(files); !refused.empty()) return refused;
  MfcStep(files);
  // After the redistributables (install scripts, MFC): one that writes an older runtime key doesn't stay.
  VcRuntimeStep(files);
  if (auto refused = RecipeRegistryStep(files); !refused.empty()) return refused;
  if (auto refused = RecipeIniStep(files); !refused.empty()) return refused;
  return DotnetHeapStep(files);
}

void RuntimeHost::SetTitleNames(std::map<std::uint32_t, std::string> names) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_ || names == title_names_) return;
    title_names_ = std::move(names);
    title_names_changed_ = true;
  }
  wake_.notify_all();
}

void RuntimeHost::RecheckInstalls() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) return;
    installs_recheck_requested_ = true;
  }
  wake_.notify_all();
}

bool RuntimeHost::FilesChanged(const rt::RuntimeInstall& record) const {
  if (record.install_path.empty()) return false;
  struct stat info{};
  return lstat((HostPath(record.install_path) / installs::kFilesChangedName).c_str(), &info) == 0 && S_ISREG(info.st_mode);
}

void RuntimeHost::CheckActivity(std::uint32_t app_id) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) return;
    activity_requests_.push_back(app_id);
  }
  wake_.notify_all();
}

void RuntimeHost::CleanUpRequests() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) return;
    cleanup_requested_ = true;
  }
  wake_.notify_all();
}

std::size_t RuntimeHost::LeftoverRequests(Composition& c, bool remove) {
  static constexpr std::string_view prefix = "settings-request-", suffix = ".v1";
  std::set<std::string> referenced;
  for (const auto& record : c.repository.Snapshot())
    if (record.journal) referenced.insert(record.journal->operation_id);
  const auto& directory = c.request_directory;
  std::vector<std::string> leftovers;
  std::error_code error;
  for (std::filesystem::directory_iterator it(directory, error), end; !error && it != end; it.increment(error)) {
    const auto name = it->path().filename().string();
    if (name.size() <= prefix.size() + suffix.size() || name.compare(0, prefix.size(), prefix) != 0 ||
        name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0)
      continue;
    const auto transaction = name.substr(prefix.size(), name.size() - prefix.size() - suffix.size());
    if (!referenced.count(transaction)) leftovers.push_back(name);
  }
  if (!remove) return leftovers.size();
  playden::Storage storage(directory.string());
  std::size_t kept = 0;
  for (const auto& name : leftovers) {
    const auto removed = storage.RemoveDurable(name);
    Log("leftover request " + name + (removed.ok() ? " removed" : " kept: " + removed.message()));
    if (!removed.ok()) ++kept;
  }
  return kept;
}

bool RuntimeHost::Handoff() {
  {
    std::lock_guard<std::mutex> lock(launch_mutex_);
    if (!launch_ || launch_->Snapshot().state != rt::LaunchState::kReadyToHandoff) return false;
  }
  // The handoff writes the launch journal, whose ownership checks need the app user's effective
  // UID: root raised elsewhere waits until it is done. Taken before launch_mutex_, like the worker.
  const auto held = config_.hold_effective_uid ? config_.hold_effective_uid() : nullptr;
  rt::LaunchResult result;
  std::string install_id;
  {
    std::lock_guard<std::mutex> lock(launch_mutex_);
    if (!launch_ || !launch_composition_) return false;
    const auto before = launch_->Snapshot();
    if (before.state != rt::LaunchState::kReadyToHandoff) return false;
    install_id = before.install_id;
    result = launch_->Handoff();
    rt::LaunchSnapshot after;
    if (result.accepted) {
      after.install_id = install_id;
      after.session_id = result.session_id;
      after.state = rt::LaunchState::kLaunching;
    } else {
      // A refused handoff leaves the service's own state: still prepared, a
      // recorded failure, or launching when only the last write failed. None
      // of these read wrapper evidence on this thread.
      after = launch_->Snapshot();
    }
    PublishLaunch(*launch_composition_, after);
  }
  Log("play handoff install=" + install_id + " accepted=" + std::to_string(result.accepted) + " session=" +
      result.session_id + " code=" + result.code + " message=" + result.message);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    launch_poll_ = true;
  }
  wake_.notify_all();
  return true;
}

void* RuntimeHost::Entry(void* self) {
  static_cast<RuntimeHost*>(self)->Run();
  return nullptr;
}

void RuntimeHost::Publish(const std::function<void(RuntimeSnapshot&)>& change) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    change(snapshot_);
    ++snapshot_.revision;
  }
  published_.notify_all();
}

std::optional<std::uint32_t> RuntimeHost::RuntimeNumberFor(const GameId& game, bool* busy) {
  if (busy) *busy = false;
  if (const auto app = game.steam_app()) return app;
  if (!game.valid()) {
    Log("runtime number refused: invalid game " + ToKey(game));
    return std::nullopt;
  }
  {
    std::lock_guard<std::mutex> lock(numbers_mutex_);
    if (const auto found = numbers_.find(game); found != numbers_.end()) return found->second;
  }
  // A new number is written to installs.json, which takes the effective-UID hold: taken here first,
  // before the repository's lock. The worker holds it around startup and each batch while using the
  // repository, so a lookup already inside the repository would wait for the worker and the worker
  // for the lookup.
  std::shared_ptr<void> held;
  if (busy && config_.try_hold_effective_uid) {
    held = config_.try_hold_effective_uid(busy);
    if (*busy) return std::nullopt;
  } else if (config_.hold_effective_uid) {
    held = config_.hold_effective_uid();
  }
  std::uint32_t number = 0;
  {
    // The repository serializes this with the worker's own use of it (its writes included).
    std::lock_guard<std::mutex> lock(numbers_mutex_);
    if (const auto found = numbers_.find(game); found != numbers_.end()) return found->second;
    if (!numbers_repository_) {
      Log("runtime number for " + ToKey(game) + " refused: the runtime records are not loaded");
      return std::nullopt;
    }
    const auto allocated = numbers_repository_->RuntimeNumberFor(game);
    if (!allocated.ok()) {
      Log("runtime number for " + ToKey(game) + " refused: " + allocated.message());
      return std::nullopt;
    }
    number = allocated.value();
    numbers_.emplace(game, number);
  }
  Log("runtime number for " + ToKey(game) + " is " + std::to_string(number));
  Publish([&](RuntimeSnapshot& s) { s.runtime_games[number] = game; });
  return number;
}

void RuntimeHost::LearnNumber(const GameId& game, std::uint32_t runtime_number) {
  if (game.store == Store::kSteam || !game.valid()) return;
  {
    std::lock_guard<std::mutex> lock(numbers_mutex_);
    numbers_[game] = runtime_number;
  }
  Publish([&](RuntimeSnapshot& s) { s.runtime_games[runtime_number] = game; });
}

void RuntimeHost::ShareNumbers(rt::RuntimeInstallRepository* repository) {
  std::vector<rt::RuntimeInstall> records;
  {
    std::lock_guard<std::mutex> lock(numbers_mutex_);
    numbers_repository_ = repository;
    if (repository) records = repository->Snapshot();
  }
  for (const auto& record : records) LearnNumber(record.game, record.runtime_number);
}

rt::NativeActivity RuntimeHost::ActivityNow(std::uint32_t app_id, std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lock(mutex_);
  if (stopping_) return rt::NativeActivity::kUnknown;
  const auto before = snapshot_.activity_serial;
  activity_requests_.push_back(app_id);
  wake_.notify_all();
  const auto answered = [&] {
    const auto found = snapshot_.activity.find(app_id);
    return found != snapshot_.activity.end() && found->second.second > before;
  };
  if (!published_.wait_for(lock, timeout, [&] { return stopping_ || answered(); }) || !answered())
    return rt::NativeActivity::kUnknown;
  return snapshot_.activity.at(app_id).first;
}

RuntimeRelocationResult RuntimeHost::Relocate(RuntimeRelocation request, std::chrono::milliseconds timeout) {
  RuntimeRelocationResult busy;
  busy.outcome = RuntimeRelocationResult::Outcome::kBusy;
  busy.message = "Playden is busy with this game. Try again in a moment.";
  RuntimeRelocationResult closing = busy;
  closing.message = kRelocationClosing;
  auto pending = std::make_shared<PendingRelocation>();
  pending->request = std::move(request);
  std::unique_lock<std::mutex> lock(mutex_);
  if (stopping_ || closing_) return closing;
  relocation_requests_.push_back(pending);
  wake_.notify_all();
  const auto settled = [&] { return pending->result.has_value() || (stopping_ && !pending->started); };
  const bool answered = published_.wait_for(lock, timeout, [&] { return settled() || pending->started || closing_; });
  if (!answered || (!pending->started && !pending->result)) {
    // Not started yet: it never will.
    pending->abandoned = true;
    return answered ? closing : busy;
  }
  // Once started it runs to the end (the files may be moving): wait for its answer.
  published_.wait(lock, settled);
  if (!pending->result) return busy;
  return *pending->result;
}

void RuntimeHost::BeginClosing() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    closing_ = true;
  }
  published_.notify_all();
}

void RuntimeHost::ReportWork(std::uint64_t done, std::uint64_t total) {
  if (total == 0) return;
  const int percent = static_cast<int>(std::min<std::uint64_t>(done, total) * 100 / total);
  // Every publish rebuilds the UI's runtime state: only whole-percent changes go out.
  if (percent == work_percent_) return;
  work_percent_ = percent;
  Publish([percent](RuntimeSnapshot& s) { s.work_progress = static_cast<float>(percent) / 100.0F; });
}

void RuntimeHost::ClearWork() {
  if (work_percent_ < 0) return;
  work_percent_ = -1;
  Publish([](RuntimeSnapshot& s) { s.work_progress.reset(); });
}

// A setup that stopped before Ready, which Prepare starts again from its saved step (Retry) or from the start.
static bool SetupRestarts(const rt::RuntimeInstall& record) {
  return record.state == rt::PreparationState::kFailed || record.state == rt::PreparationState::kUnavailable ||
         record.state == rt::PreparationState::kDownloaded;
}

// Whether the install is bound to exactly the installed kit's runtime artifacts.
static bool ArtifactsBound(const auto& kit_artifacts, const rt::RuntimeInstall& record) {
  return record.artifacts.size() == kit_artifacts.size() &&
         std::all_of(kit_artifacts.begin(), kit_artifacts.end(), [&](const auto& artifact) {
           return std::any_of(record.artifacts.begin(), record.artifacts.end(), [&](const auto& bound) {
             return bound.artifact_id == artifact.artifact_id && bound.identity == artifact.identity &&
                    bound.sha256 == artifact.sha256;
           });
         });
}

void RuntimeHost::PublishInstalls(const Composition& composition, const std::string& only) {
  std::map<std::string, std::tuple<bool, bool, bool>> checked;  // title_current, runtime_current, setup_other_kit
  if (!only.empty()) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& view : snapshot_.installs)
      checked[view.install_id] = {view.title_current, view.runtime_current, view.setup_other_kit};
  }
  std::vector<RuntimeInstallView> installs;
  for (const auto& record : composition.repository.Snapshot()) {
    RuntimeInstallView view;
    view.install_id = record.install_id; view.runtime_number = record.runtime_number; view.title_id = record.title_id;
    view.game = GameOf(record.game, record.runtime_number);
    view.install_path = record.install_path;
    view.moving = record.journal && record.journal->operation == "remove-move";
    if (!record.data_root.empty()) view.prefix_root = record.data_root + "/library/prefixes/game";
    view.state = record.state;
    if (record.journal) view.step = record.journal->step;
    view.recovery_pending = record.journal.has_value();
    view.awaiting_registration = AwaitingRegistration(record);
    if (record.last_failure) {
      view.failure = record.last_failure->message;
      view.failure_code = record.last_failure->code;
    }
    if (const auto kept = checked.find(record.install_id); !only.empty() && record.install_id != only && kept != checked.end()) {
      std::tie(view.title_current, view.runtime_current, view.setup_other_kit) = kept->second;
    } else if (record.state == rt::PreparationState::kReady && composition.kit) {
      const bool files_current = composition.installer->TitleCurrent(record);
      view.title_current = files_current && composition.installer->TitleMetadataCurrent(record) && TitleNameCurrent(record);
      view.runtime_current = files_current && ArtifactsBound(composition.kit->view.artifacts, record);
    } else if (composition.kit && SetupRestarts(record) && !view.moving) {
      view.setup_other_kit = composition.installer->SetupFromOtherKit(record);
    }
    if (const auto found = composition.admission.find(record.install_id); found != composition.admission.end())
      view.admission = found->second;
    view.files_changed = record.state == rt::PreparationState::kReady && FilesChanged(record);
    view.settings_revision = record.settings_revision;
    view.launch_entry_id = record.launch_entry_id;
    view.launch_path = record.launch_path;
    view.launch_working_directory = record.launch_working_directory;
    view.launch_in_install_root = record.launch_in_install_root;
    std::error_code error;
    for (std::filesystem::directory_iterator it(SaveBackupRoot(record.install_id), error), end; !error && it != end;
         it.increment(error)) {
      const auto stamp = std::strtoll(it->path().filename().c_str(), nullptr, 10);
      if (stamp <= 0) continue;
      ++view.save_backups;
      view.latest_save_backup = std::max<std::int64_t>(view.latest_save_backup, stamp);
    }
    installs.push_back(std::move(view));
  }
  const bool known = composition.repository.Loaded();
  Publish([&](RuntimeSnapshot& s) {
    s.installs = std::move(installs);
    s.installs_known = known;
  });
  PublishPrepared(&composition);
}

bool RuntimeHost::TitleRuntimeCurrent(const Composition& c, const rt::RuntimeInstall& record) const {
  return c.kit && c.installer->TitleCurrent(record) && ArtifactsBound(c.kit->view.artifacts, record);
}

rt::KitCapabilities RuntimeHost::TitleKit(const Composition& c, const rt::RuntimeInstall& record) const {
  if (!c.kit) return {};
  const auto kit = rt::KitCapabilitiesFromManifest(c.kit->capabilities, true);
  // The title's markers are read only when the kit has something an older runtime would refuse.
  return kit.display_120hz || kit.profile_version_2 || kit.input_rumble || kit.display_framegen
             ? rt::KitForTitleRuntime(kit, TitleRuntimeCurrent(c, record))
             : kit;
}

// The complete OWNER protocol for the publish-profile step: persist the exact
// requested plan, arm OWNER, then capture, publish, register and commit Ready.
// Recovery journals resume their saved request or frozen operation; nothing is
// regenerated from newer settings.
rt::PackageActionResult RuntimeHost::DrivePublication(Composition& c, const rt::RuntimeInstall& record) {
  auto failed = [](std::string code, std::string message) {
    return rt::PackageActionResult{rt::PackageActionState::kFailed, std::move(code), std::move(message)};
  };
  if (!record.journal) return failed("publication-journal", "No preparation journal to publish.");
  const auto& prefix = config_.host_namespace_prefix;
  // Saved request, control setup and frozen publication journals all resume
  // through the persisted request; ResumeRequested hands a frozen publication
  // to Resume itself.
  if (record.journal->operation != "prepare") {
    const auto resumed = c.owner->ResumeRequested(record, c.request_directory, prefix);
    Log("publication resume journal=" + record.journal->operation + "/" + record.journal->step + " state=" +
        std::to_string(static_cast<int>(resumed.state)) + " code=" + resumed.code + " message=" + resumed.message);
    return resumed;
  }
  if (!c.kit) return failed("kit-unavailable", "No verified runtime kit is installed.");

  const rt::GameRecipe* recipe = nullptr;
  for (const auto& candidate : c.recipes)
    if (RecipeFits(candidate, record) && candidate.id == record.recipe_id) recipe = &candidate;
  if (!recipe) return failed("recipe-unavailable", "Playden no longer knows how to start this game; prepare it again.");
  rt::SettingsLaunchInput input;
  input.recipe = recipe;
  input.record = config_.settings_record
                     ? config_.settings_record(GameOf(record)).value_or(rt::GameSettingsRecord{})
                                         : rt::GameSettingsRecord{};
  for (const auto& preset : c.presets)
    if (preset.id == input.record.preset_id) input.preset = &preset;
  input.kit = TitleKit(c, record);
  input.target = {record.default_profile_id, record.install_path};
  input.runtime_dir = config_.runtime_dir;
  rt::SettingsLaunchPlan plan;
  std::string reason;
  if (input.record.revision != record.settings_revision)
    return failed("settings-changed", "The game's settings changed during preparation; prepare it again.");
  // The first start may come from the PS5 Home screen, so the game's Steam files are set up now.
  if (const auto steamworks = ApplyPreparationSteps(c, record); !steamworks.empty()) return failed("steamworks", steamworks);
  if (!rt::BuildSettingsLaunchPlan(input, &plan, &reason)) {
    Log("publication plan refused: " + reason);
    return failed("settings-plan", reason);
  }
  const auto transaction = config_.uuid();
  if (!transaction) return failed("random-unavailable", "No secure random source for the publication transaction.");
  rt::SettingsPublicationRequest request;
  request.transaction_id = *transaction;
  request.kit_id = c.kit->view.kit_id;
  request.install = record;
  request.plan = std::move(plan);
  request.registry_unix_time = config_.unix_time();
  const auto requested = c.owner->Request(request, c.request_directory);
  Log("publication request transaction=" + request.transaction_id + " state=" +
      std::to_string(static_cast<int>(requested.state)) + " code=" + requested.code + " message=" + requested.message);
  if (requested.state != rt::PackageActionState::kComplete) return requested;
  const auto armed = c.records.Find(record.install_id);
  if (!armed) return failed("publication-owner", "The armed OWNER record is missing.");
  const auto resumed = c.owner->ResumeRequested(*armed, c.request_directory, prefix);
  Log("publication resume state=" + std::to_string(static_cast<int>(resumed.state)) + " code=" + resumed.code +
      " message=" + resumed.message);
  return resumed;
}

// With no working folder set, the profile runs the program from its own folder
// (prospero_profile.cpp), so that is where dxvk.conf goes.
static std::string WorkingFolder(const std::string& working, std::string program, bool in_install_root) {
  if (!working.empty() && working != ".") return working;
  if (in_install_root) return {};
  std::replace(program.begin(), program.end(), '\\', '/');
  const auto slash = program.rfind('/');
  return slash == std::string::npos ? std::string() : program.substr(0, slash);
}

// The game root or its working folder when either isn't a folder (a stale record, an unplugged
// drive): nothing can be published for it. root is the host path of the record's install_path.
static std::optional<std::filesystem::path> MissingGameFolder(const std::filesystem::path& root,
                                                              const rt::RuntimeInstall& record) {
  const auto folder = WorkingFolder(record.launch_working_directory, record.launch_path, record.launch_in_install_root);
  const auto working = folder.empty() ? root : root / folder;
  struct stat info{};
  const auto is_directory = [&info](const std::filesystem::path& path) {
    return lstat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
  };
  if (!is_directory(root)) return root;
  if (!is_directory(working)) return working;
  return std::nullopt;
}

// The folders from the game root down to the program's working folder (where dxvk.conf
// goes) get the same treatment as the root: an owned, world-writable download folder
// becomes 0755. Folders that don't exist yet, links and folders owned by anyone else are
// left alone; publication then refuses them. The caller skips a root on a drive that
// keeps no owners.
std::string RuntimeHost::TightenWorkingFolder(const std::filesystem::path& root, const std::string& working) {
  if (working.empty() || working == ".") return {};
  auto folder = root;
  for (const auto& part : std::filesystem::path(working)) {
    if (part.empty() || part == "." || part == "..") return {};
    folder /= part;
    struct stat info{};
    if (lstat(folder.c_str(), &info) != 0 || !S_ISDIR(info.st_mode) || info.st_uid != geteuid()) return {};
    if ((info.st_mode & 07777) == 0700 || (info.st_mode & 07777) == 0755) continue;
    if (chmod(folder.c_str(), 0755) != 0) return "The game's working folder permissions could not be tightened.";
    char mode[8]; std::snprintf(mode, sizeof(mode), "%04o", unsigned(info.st_mode & 07777));
    Log("game folder mode " + std::string(mode) + " -> 0755 path=" + folder.string());
  }
  return {};
}

std::string RuntimeHost::EnsureWritable(const std::string& install_path, const std::string& working) {
  std::vector<std::filesystem::path> folders;
  for (const auto& folder : config_.writable_folders) folders.push_back(HostPath(folder));
  if (!install_path.empty()) {
    folders.push_back(HostPath(install_path));
    const std::filesystem::path relative(working);
    if (!working.empty() && working != "." && relative.is_relative() &&
        std::none_of(relative.begin(), relative.end(), [](const auto& part) { return part == ".."; }))
      folders.push_back(HostPath(install_path) / relative);
  }
  return rt::EnsureWritableFolders(folders, config_.as_root, [this](const std::string& line) { Log(line); },
                                    config_.host_namespace_prefix.empty() ? "/" : config_.host_namespace_prefix);
}

bool RuntimeHost::NeedsUnownedPublication(const Composition& c, const rt::RuntimeInstall& record) {
  if (!c.kit || record.state != rt::PreparationState::kReady || record.journal ||
      !rt::KitCapabilitiesFromManifest(c.kit->capabilities, true).game_root_unowned)
    return false;
  return rt::PublishedGameRootUnowned(record, config_.host_namespace_prefix) == std::optional<bool>(false);
}

bool RuntimeHost::DropUpgradeRequest(Composition& c, const std::string& install_id, const std::string& why) {
  const auto current = c.records.Find(install_id);
  if (!current || current->state != rt::PreparationState::kReady || !current->moved_from.empty() || !current->journal ||
      current->journal->operation != "settings-request" || current->journal->step != "capture-update")
    return false;
  // Only the saved request tells the upgrade (its plan at the record's revision) from a newer
  // settings update, whose request waits for Resume as before.
  const auto loaded = rt::LoadSettingsPublicationRequest(c.request_directory, current->journal->operation_id);
  if (loaded.state != rt::PublicationRecoveryState::kValid || !loaded.request ||
      loaded.request->plan.settings_revision != current->settings_revision)
    return false;
  const auto dropped = c.owner->DiscardRequested(*current, c.request_directory, config_.host_namespace_prefix);
  Log("settings update app=" + std::to_string(current->runtime_number) + " unowned upgrade request " + current->journal->operation_id +
      (dropped.state == rt::PackageActionState::kComplete ? " dropped" : " kept") + ": " + why +
      (dropped.code.empty() ? std::string{} : " code=" + dropped.code) +
      (dropped.message.empty() ? std::string{} : " " + dropped.message));
  if (dropped.state != rt::PackageActionState::kComplete) return false;
  PublishInstalls(c, install_id);
  return true;
}

void RuntimeHost::OpenGameFolders(const Composition& c, const rt::RuntimeInstall& record) {
  // Only under an unowned publication: an owned one fails the title's start check at 0777.
  if (!c.kit || record.install_path.empty() ||
      !rt::KitCapabilitiesFromManifest(c.kit->capabilities, true).game_root_unowned ||
      rt::PublishedGameRootUnowned(record, config_.host_namespace_prefix) != std::optional<bool>(true))
    return;
  const auto working = WorkingFolder(record.launch_working_directory, record.launch_path, record.launch_in_install_root);
  auto folder = HostPath(record.install_path);
  // Each folder through its descriptor, never through a link (the walk stops at one).
  const auto open_up = [this](int fd, const std::filesystem::path& path) {
    struct stat info{};
    if (fstat(fd, &info) != 0 || !S_ISDIR(info.st_mode) || info.st_uid != geteuid() || (info.st_mode & 07777) != 0755)
      return;
    const bool opened = fchmod(fd, 0777) == 0;
    Log("game folder mode 0755 -> 0777 path=" + path.string() + (opened ? "" : " failed errno=" + std::to_string(errno)));
  };
  const auto refused = [this](const std::filesystem::path& path) {
    Log("game folder open stops path=" + path.string() + " errno=" + std::to_string(errno) +
        (errno == ELOOP ? " (a link)" : ""));
  };
  int fd = rt::OpenFolderNoFollow(folder);
  if (fd < 0) return refused(folder);
  open_up(fd, folder);
  if (!working.empty() && working != ".")
    for (const auto& part : std::filesystem::path(working)) {
      if (part.empty() || part == "." || part == ".." || part.is_absolute()) break;
      const int child = rt::OpenChildFolderNoFollow(fd, folder, part.string());
      close(fd);
      fd = child;
      folder /= part;
      if (fd < 0) return refused(folder);
      open_up(fd, folder);
    }
  close(fd);
}

void RuntimeHost::SettleBottle(Composition& c, const rt::RuntimeInstall& record, const char* when) {
  const auto settled = c.installer->SettleBottleModes(record);
  if (settled.state != rt::PackageActionState::kComplete)
    Log(std::string("bottle modes ") + when + " install=" + record.install_id + " code=" + settled.code + " " +
        settled.message);
}

std::string RuntimeHost::UnlinkDxvkConfig(const rt::RuntimeInstall& record) {
  if (record.install_path.empty()) return {};
  const std::filesystem::path working(WorkingFolder(record.launch_working_directory, record.launch_path, record.launch_in_install_root));
  if (working.is_absolute() ||
      std::any_of(working.begin(), working.end(), [](const auto& part) { return part == ".."; }))
    return {};
  const auto path = HostPath(record.install_path) / working / "dxvk.conf";
  struct stat info{};
  if (lstat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode) || info.st_nlink <= 1) return {};
  const auto failed = [&](const char* step) {
    Log("dxvk.conf unlink app=" + std::to_string(record.runtime_number) + " failed at " + step + " errno=" + std::to_string(errno));
    return std::string("The game's dxvk.conf has more than one link and could not be replaced by a copy.");
  };
  constexpr off_t kMaxBytes = 256 * 1024;
  if (info.st_size > kMaxBytes) return failed("size");
  std::string bytes;
  {
    const int in = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    struct stat opened{};
    if (in < 0) return failed("open");
    if (fstat(in, &opened) != 0 || opened.st_ino != info.st_ino || opened.st_dev != info.st_dev) {
      close(in);
      return failed("identity");
    }
    char buffer[16384];
    for (;;) {
      const auto count = read(in, buffer, sizeof(buffer));
      if (count < 0 && errno == EINTR) continue;
      if (count < 0 || bytes.size() + static_cast<std::size_t>(std::max<ssize_t>(count, 0)) > std::size_t(kMaxBytes)) {
        close(in);
        return failed("read");
      }
      if (count == 0) break;
      bytes.append(buffer, static_cast<std::size_t>(count));
    }
    close(in);
  }
  const auto temporary = path.parent_path() / ".dxvk.conf.playden-unlink";
  unlink(temporary.c_str());
  const int out = open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (out < 0) return failed("create");
  std::size_t written = 0;
  while (written < bytes.size()) {
    const auto count = write(out, bytes.data() + written, bytes.size() - written);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) break;
    written += static_cast<std::size_t>(count);
  }
  const bool ok = written == bytes.size() && fchmod(out, info.st_mode & 0777) == 0 && fsync(out) == 0;
  if (close(out) != 0 || !ok || rename(temporary.c_str(), path.c_str()) != 0) {
    unlink(temporary.c_str());
    return failed("write");
  }
  Log("dxvk.conf unlink app=" + std::to_string(record.runtime_number) + " links=" + std::to_string(info.st_nlink) +
      " replaced by a copy path=" + path.string());
  return {};
}

std::string RuntimeHost::RepublishSettings(Composition& c, const rt::RuntimeInstall& record, bool* published,
                                           bool steam_account_known, bool* prepare_again,
                                           std::string* upgrade_deferred, std::string* code, bool force,
                                           const std::string& setup_program) {
  if (published) *published = false;
  if (code) code->clear();
  if (prepare_again) *prepare_again = false;
  if (upgrade_deferred) upgrade_deferred->clear();
  // A recipe this kit cannot run is refused before the game's files or bottle change.
  if (c.kit)
    for (const auto& candidate : c.recipes)
      if (RecipeFits(candidate, record) && candidate.id == record.recipe_id)
        if (const auto lacks = KitLacksForRecipe(candidate, c.kit->view); !lacks.empty()) return lacks;
  // Only the unowned upgrade of a game that did not move, with no newer settings: the game
  // played under its owned publication, which stays when the upgrade fails. A forced publication
  // (another runtime) reports its refusal instead.
  const auto saved_revision = config_.settings_record
      ? config_.settings_record(GameOf(record)).value_or(rt::GameSettingsRecord{}).revision : 0;
  const bool upgrade_only = !force && setup_program.empty() && record.setup_program.empty() && record.moved_from.empty() &&
                            saved_revision <= record.settings_revision && NeedsUnownedPublication(c, record);
  // A game folder that isn't there (a stale record, an unplugged drive) can't be published, and
  // arming the request first would leave a journal on the record: nothing is armed for it.
  if (upgrade_only || !record.moved_from.empty()) {
    if (const auto missing = MissingGameFolder(HostPath(record.install_path), record)) {
      if (!upgrade_only) return "The moved game's working folder is missing at its new place (" + missing->string() + ").";
      Log("settings update app=" + std::to_string(record.runtime_number) + " unowned upgrade skipped: game folder missing (" +
          missing->string() + ")");
      if (upgrade_deferred) *upgrade_deferred = "The game folder is missing.";
      return {};
    }
  }
  auto refused = PublishSavedSettings(c, record, published, steam_account_known, prepare_again, code, force, false,
                                      setup_program);
  if (refused.empty() || !upgrade_only) return refused;
  Log("settings update app=" + std::to_string(record.runtime_number) + " unowned upgrade deferred, owned publication kept: " +
      refused);
  if (prepare_again) *prepare_again = false;
  if (upgrade_deferred) *upgrade_deferred = refused;
  // One that left a journal waits for recovery like any other update (Play refuses it).
  const auto current = c.records.Find(record.install_id);
  if (!current || current->journal) return refused;
  // As before the upgrade existed: Play brings the Steam files in line; startup leaves them.
  return steam_account_known ? ApplyPreparationSteps(c, *current, true) : std::string{};
}

std::string RuntimeHost::PublishSavedSettings(Composition& c, const rt::RuntimeInstall& record, bool* published,
                                              bool steam_account_known, bool* prepare_again, std::string* code,
                                              bool force, bool defaults_published, const std::string& setup_program) {
  if (code) code->clear();
  // Refusals only a new preparation gets past (the title's recipe or settings can't be published again).
  const auto again = [prepare_again](std::string message) {
    if (prepare_again) *prepare_again = true;
    return message;
  };
  const auto stored = config_.settings_record ? config_.settings_record(GameOf(record)) : std::nullopt;
  auto saved = stored.value_or(rt::GameSettingsRecord{});
  // A forced publication republishes the title's own revision for another runtime, so it needs the saved settings of
  // that revision or a newer one: Playden defaults standing in for a missing or unreadable record are not they.
  if (force && (!stored ? !(defaults_published && record.settings_revision == 0) : saved.revision < record.settings_revision)) {
    Log("forced republish skipped app=" + std::to_string(record.runtime_number) + " saved=" +
        (stored ? std::to_string(saved.revision) : std::string("none")) +
        " title=" + std::to_string(record.settings_revision));
    return {};
  }
  // Run a program once: the saved settings with that program and the Mouse controller preset (its rows as the preset
  // sets them, so a recipe's or the player's controller choices don't hide the pointer) in their place. The title
  // keeps that profile until the next publication without one puts the saved settings back (RuntimeInstall::
  // setup_program), whatever their revision.
  const bool restoring = setup_program.empty() && !record.setup_program.empty();
  if (!setup_program.empty()) {
    auto& overrides = saved.overrides;
    std::erase_if(overrides, [](const auto& entry) {
      static const std::set<std::string> program = {"program", "architecture", "program_folder", "program_arguments",
                                                    "start_check"};
      return program.count(entry.first) || entry.first.rfind("button.", 0) == 0;
    });
    overrides["launch_option"] = rt::kProgramEntryId;
    overrides["program"] = setup_program;
    overrides["controller_preset"] = "mouse";
    for (const auto& [id, value] : rt::ControllerPresetInputs().at("mouse")) overrides[id] = value;
  }
  // A game whose folder moved (RuntimeInstall::moved_from) is published again at its revision, so
  // its descriptor, profile and dxvk.conf name the new folder.
  const bool moved = !record.moved_from.empty();
  // So is an owned publication the kit can replace by an unowned one (folders opened for mods).
  const bool unowned_upgrade = !moved && NeedsUnownedPublication(c, record);
  if (unowned_upgrade) Log("settings update app=" + std::to_string(record.runtime_number) + " publishes the game folder unowned");
  // "Steam features" only swaps the game's own files, so it follows the saved value at once.
  if (!force && setup_program.empty() && !restoring && saved.revision <= record.settings_revision && !moved &&
      !unowned_upgrade)
    return ApplyPreparationSteps(c, record, steam_account_known);
  if (!c.kit) return "No verified runtime kit is installed.";
  if (saved.revision < record.settings_revision)
    return again("The game's saved settings are older than its PS5 title's; prepare it again.");
  const rt::GameRecipe* recipe = nullptr;
  for (const auto& candidate : c.recipes)
    if (RecipeFits(candidate, record) && candidate.id == record.recipe_id &&
        std::to_string(candidate.version) == record.recipe_version)
      recipe = &candidate;
  if (!recipe) return again("Playden no longer knows how to start this game; prepare it again.");
  const rt::SettingsPreset* preset = nullptr;
  for (const auto& candidate : c.presets)
    if (candidate.id == saved.preset_id) preset = &candidate;
  // The run's buttons are the Mouse preset's (runtime/input/mouse.input): the recipe's and the settings preset's
  // bindings stay out of it, and the recipe's locks don't keep its program or controls.
  std::optional<rt::GameRecipe> run_recipe;
  std::optional<rt::SettingsPreset> run_preset;
  if (!setup_program.empty()) {
    const auto button = [](const auto& entry) { return entry.first.rfind("button.", 0) == 0; };
    run_recipe = *recipe;
    std::erase_if(run_recipe->defaults, button);
    std::erase_if(run_recipe->locked, [](const std::string& id) {
      static const std::set<std::string> controls = {"launch_option", "program", "controller_preset", "controller",
                                                     "pointer_stick", "pointer_speed"};
      return controls.count(id) || id.rfind("button.", 0) == 0;
    });
    recipe = &*run_recipe;
    if (preset) {
      run_preset = *preset;
      std::erase_if(run_preset->settings, button);
      preset = &*run_preset;
    }
  }
  const auto kit = TitleKit(c, record);
  // Another program or folder chosen in Game settings: the PS5 title and the bottle stay, and
  // the install record names the new program when this publication commits (Reconcile), so a
  // crash in between resumes with it. Callers run this only while the game is stopped.
  auto next = record;
  {
    rt::EffectiveSettings desired;
    rt::SettingsError ignored;
    const auto same = [](std::string left, std::string right) {
      std::replace(left.begin(), left.end(), '\\', '/');
      std::replace(right.begin(), right.end(), '\\', '/');
      return (left == "." ? std::string() : left) == (right == "." ? std::string() : right);
    };
    if (rt::BuildEffectiveSettings(*recipe, preset, saved.overrides, kit, &desired, &ignored) && desired.launch) {
      const auto& launch = *desired.launch;
      if (launch.id != record.launch_entry_id || !same(launch.executable, record.launch_path) ||
          !same(launch.working_directory, record.launch_working_directory) ||
          launch.in_install_root != record.launch_in_install_root) {
        const auto root = HostPath(record.install_path).string();
        const auto architecture = rt::GameProgramArchitecture(root, launch.executable);
        if (architecture.empty())
          return "The program " + launch.executable + " chosen in Game settings is missing or not a Windows program.";
        if (const auto refusal = rt::GameProgramRefusal(root, launch.executable, kit, rt::RecipeChoicesFrom(saved).try_anyway);
            !refusal.empty())
          return refusal;
        // Checked here, before a publication starts: one that fails part way leaves the title
        // waiting for recovery.
        const auto folder = WorkingFolder(launch.working_directory, launch.executable, launch.in_install_root);
        struct stat folder_info{};
        if (!folder.empty() && (lstat((HostPath(record.install_path) / folder).c_str(), &folder_info) != 0 ||
                                !S_ISDIR(folder_info.st_mode)))
          return "The program folder " + folder + " chosen in Game settings is missing.";
        // A kit that admits unowned game folders gets them as they are (mods, FTP edits).
        const auto records_ownership = kit.game_root_unowned ? std::optional<bool>(false)
                                                             : platform::RecordsOwnership(HostPath(record.install_path));
        if (records_ownership && *records_ownership)
          if (const auto refused = TightenWorkingFolder(HostPath(record.install_path),
                                                        WorkingFolder(launch.working_directory, launch.executable,
                                                                      launch.in_install_root));
              !refused.empty())
            return refused;
        next.launch_entry_id = launch.id;
        next.launch_path = launch.executable;
        next.launch_working_directory = launch.working_directory == "." ? std::string() : launch.working_directory;
        next.launch_in_install_root = launch.in_install_root && next.launch_working_directory.empty();
        // The recipe stays the one the game was prepared with: its graphics and engine arguments
        // came from the first program's files, and only a preparation sees Steam's launch options.
        Log("program switch app=" + std::to_string(record.runtime_number) + " from=" + record.launch_entry_id + ":" +
            record.launch_path + " to=" + next.launch_entry_id + ":" + next.launch_path + " recipe kept=" + record.recipe_id);
      }
    }
  }
  // An unowned folder is published as it is, so one made over FTP (root's, 0755) is opened up
  // before dxvk.conf is written into it, as preparation does.
  if (kit.game_root_unowned) {
    const auto working = WorkingFolder(next.launch_working_directory, next.launch_path, next.launch_in_install_root);
    if (const auto unwritable = EnsureWritable(next.install_path, working); !unwritable.empty()) return unwritable;
  }
  // SteamStub unpacking and the Steam emulator follow the program that starts now.
  if (const auto steamworks = ApplyPreparationSteps(c, next, steam_account_known); !steamworks.empty()) return steamworks;
  rt::SettingsLaunchInput input;
  input.recipe = recipe;
  input.record = saved;
  // The program and its folder are the install record's; the switch above moved them to the
  // settings' choice.
  auto& overrides = input.record.overrides;
  overrides["launch_option"] = next.launch_entry_id;
  if (next.launch_entry_id == rt::kProgramEntryId) {
    overrides["program"] = next.launch_path;
    // Its architecture comes from the file, whatever the sheet says now.
    const auto architecture = rt::GameProgramArchitecture(HostPath(next.install_path).string(), next.launch_path);
    if (!architecture.empty()) overrides["architecture"] = architecture;
  } else {
    overrides.erase("program");
  }
  // "." is the install root, empty the program's own folder (BuildEffectiveSettings).
  overrides["program_folder"] = next.launch_in_install_root ? std::string(".") : next.launch_working_directory;
  input.preset = preset;
  input.kit = kit;
  input.target = {next.default_profile_id, next.install_path};
  input.runtime_dir = config_.runtime_dir;
  rt::SettingsLaunchPlan plan;
  std::string reason;
  if (!rt::BuildSettingsLaunchPlan(input, &plan, &reason)) {
    Log("settings update plan refused app=" + std::to_string(record.runtime_number) + ": " + reason);
    return again("The settings could not be applied: " + reason);
  }
  if (plan.app_id != record.runtime_number || plan.recipe_id != record.recipe_id || plan.recipe_version != record.recipe_version ||
      plan.launch_entry_id != next.launch_entry_id || plan.profile_id != record.default_profile_id)
    return again("The settings no longer match this game's PS5 title; prepare it again.");
  // A forced publication runs only when the new runtime changes what the settings write (120 Hz output), so a
  // respin that changes nothing there keeps its publication, registration and bottle as they are.
  // So is a program's run, or the return from one, that publishes what the title already has.
  if (force || !setup_program.empty() || restoring) {
    // POSIX reads, as elsewhere here: the PS5 libc++ has no locale support for file streams.
    const auto read = [](const std::filesystem::path& path) {
      std::string bytes;
      const int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
      if (fd < 0) return bytes;
      char chunk[4096];
      for (ssize_t count; (count = ::read(fd, chunk, sizeof(chunk))) > 0;)
        bytes.append(chunk, static_cast<std::size_t>(count));
      close(fd);
      return bytes;
    };
    const auto profile = HostPath(record.data_root) / "library/profiles" / (plan.profile_id + ".profile");
    const auto config = HostPath(record.install_path) / WorkingFolder(next.launch_working_directory, next.launch_path,
                                                                                 next.launch_in_install_root) /
                        "dxvk.conf";
    if (read(profile) == plan.profile_bytes && read(config) == plan.dxvk_config)
      return restoring ? MarkSetupProgram(c, record.install_id, {}) : std::string{};
  }
  // Recorded before the program's profile is published, so a publication cut off part way still gets the saved
  // settings back.
  if (!setup_program.empty())
    if (const auto marked = MarkSetupProgram(c, record.install_id, setup_program); !marked.empty()) return marked;
  const auto transaction = config_.uuid();
  if (!transaction) return "No secure random source for the settings update.";
  rt::SettingsPublicationRequest request;
  request.transaction_id = *transaction;
  request.kit_id = c.kit->view.kit_id;
  request.install = next;
  request.install.settings_revision = saved.revision;  // the revision the update commits
  request.plan = std::move(plan);
  request.registry_unix_time = config_.unix_time();
  const auto requested = c.owner->Request(request, c.request_directory);
  Log("settings update request app=" + std::to_string(record.runtime_number) + " transaction=" + request.transaction_id +
      " revision=" + std::to_string(record.settings_revision) + "->" + std::to_string(saved.revision) + " state=" +
      std::to_string(static_cast<int>(requested.state)) + " code=" + requested.code + " message=" + requested.message);
  if (requested.state != rt::PackageActionState::kComplete) {
    if (code) *code = requested.code;
    return "The settings could not be applied: " + requested.message;
  }
  const auto armed = c.records.Find(record.install_id);
  if (!armed) return "The game's install record is missing.";
  const auto resumed = c.owner->ResumeRequested(*armed, c.request_directory, config_.host_namespace_prefix);
  Log("settings update resume app=" + std::to_string(record.runtime_number) + " state=" +
      std::to_string(static_cast<int>(resumed.state)) + " code=" + resumed.code + " message=" + resumed.message);
  // The title's start check now finds the new folder.
  if (moved && resumed.state == rt::PackageActionState::kComplete)
    if (const auto current = c.records.Find(record.install_id))
      c.admission[record.install_id] = c.installer->QuickAdmission(*current);
  // The upgrade alone is optional: one that failed before its publication began (the request
  // arm is still on the record) is dropped, and the game keeps playing under its owned publication.
  if (resumed.state != rt::PackageActionState::kComplete && unowned_upgrade && saved.revision == record.settings_revision)
    DropUpgradeRequest(c, record.install_id, resumed.message);
  PublishInstalls(c, record.install_id);
  // A failed update keeps its journal, so Play waits until Resume on the PS5 title finishes it.
  if (resumed.state != rt::PackageActionState::kComplete) {
    if (code) *code = resumed.code;
    return "The settings could not be applied: " + resumed.message;
  }
  if (published) *published = true;
  if (const auto current = c.records.Find(record.install_id)) OpenGameFolders(c, *current);
  if (moved) FinishGamesMove(c);
  if (restoring) return MarkSetupProgram(c, record.install_id, {});
  return {};
}

std::string RuntimeHost::MarkSetupProgram(Composition& c, const std::string& install_id, const std::string& program) {
  auto record = c.records.Find(install_id);
  if (!record) return "The game's install record is missing.";
  if (record->setup_program == program) return {};
  Log("setup program app=" + std::to_string(record->runtime_number) + " " +
      (program.empty() ? "done: " + record->setup_program : "runs once: " + program));
  record->setup_program = program;
  const auto written = c.repository.Update(*record);
  PublishInstalls(c, install_id);
  return written.ok() ? std::string{} : "The program's run could not be recorded: " + written.message();
}

bool RuntimeHost::MoveGames(Composition& c, std::string* error) {
  auto move = config_.moved_games_root ? config_.moved_games_root() : std::nullopt;
  auto journal = rt::LoadLayoutJournal(config_.state_root);
  const bool rename_pending = config_.layout_paths && journal && journal->step == "games";
  const auto console_path = [this](const std::filesystem::path& path) {
    if (config_.host_namespace_prefix.empty()) return path.string();
    return (std::filesystem::path("/") / path.lexically_relative(config_.host_namespace_prefix)).string();
  };
  if (rename_pending) move = std::make_pair(console_path(config_.layout_paths->app_root / "games"),
                                           console_path(config_.layout_paths->games_root));
  if (!move) return true;
  for (auto* path : {&move->first, &move->second})
    if (!config_.host_namespace_prefix.empty()) {
      const auto relative = std::filesystem::path(*path).lexically_relative(config_.host_namespace_prefix);
      if (!relative.empty() && *relative.begin() != "..") *path = (std::filesystem::path("/") / relative).string();
    }
  // All leases precede every activity check and survive the rename AND record rewrite.
  std::vector<platform::ManagedFileLock> leases;
  const auto postpone = [&](const std::string& why) {
    Log("layout games postponed: " + why);
    struct stat old_games{};
    if (rename_pending && ::lstat((config_.layout_paths->app_root / "games").c_str(), &old_games) == 0 &&
        S_ISDIR(old_games.st_mode)) return true;
    *error = "The games move could not finish: " + why;
    return false;
  };
  for (const auto& record : c.repository.Snapshot()) {
    if (!rt::RebaseInstallPath(record.install_path, move->first, move->second)) continue;
    const auto rebased = rt::RebaseInstallPath(record.install_path, move->first, move->second);
    // Missing game folders have nothing Home can be using at either location.
    bool present = false;
    for (const auto& path : {record.install_path, *rebased}) {
      std::error_code status_error;
      present |= std::filesystem::is_directory(HostPath(path), status_error);
      if (status_error && status_error != std::errc::no_such_file_or_directory)
        return postpone("the game folder " + path + " cannot be inspected: " + status_error.message());
    }
    if (!present) continue;
    const auto control_path = HostPath(record.data_root) / ".playden-control";
    auto control = platform::ManagedDirectory::Open(control_path, platform::DirectoryPolicy::kPrivate);
    auto lease = control ? platform::ManagedFileLock::Acquire(*control, "publication.lock", platform::LockMode::kPublisher)
                         : std::nullopt;
    if (!lease) return postpone("the publication lock for " + record.title_id + " is unavailable or held");
    leases.push_back(std::move(*lease));
  }
  if (rename_pending) {
    auto ops = config_.layout_operations;
    ops.defer_games = false;
    ops.as_root = config_.as_root;
    ops.log = [this](const std::string& line) { Log(line); };
    ops.title_running = [&c](const std::string& title) -> std::optional<bool> {
      const auto activity = c.coordinator->Activity(title);
      if (activity == rt::NativeActivity::kUnknown) return std::nullopt;
      return activity != rt::NativeActivity::kStopped;
    };
    const auto moved = rt::MigrateLayout(*config_.layout_paths, ops);
    if (!moved.runtime_ready) { *error = moved.notice; return false; }
    if (!moved.games_moved) return true;
  }
  if (!MoveGameRecords(c)) { *error = "The moved games' records could not be saved."; return false; }
  return true;
}

bool RuntimeHost::MoveGameRecords(Composition& c) {
  c.games_move = config_.moved_games_root ? config_.moved_games_root() : std::nullopt;
  if (!c.games_move) return true;
  if (!config_.host_namespace_prefix.empty()) {
    for (auto* path : {&c.games_move->first, &c.games_move->second}) {
      const auto relative = std::filesystem::path(*path).lexically_relative(config_.host_namespace_prefix);
      if (!relative.empty() && *relative.begin() != "..") *path = (std::filesystem::path("/") / relative).string();
    }
  }
  const auto& [from, to] = *c.games_move;
  // Records on other drives (USB, M.2) stay; the ones under the old root name the new one and
  // keep the old one until a publication names the new folder.
  std::vector<rt::RuntimeInstall> moved;
  for (auto record : c.repository.Snapshot()) {
    const auto path = rt::RebaseInstallPath(record.install_path, from, to);
    if (!path) continue;
    record.moved_from = record.install_path;
    record.install_path = *path;
    moved.push_back(std::move(record));
  }
  if (!moved.empty()) {
    const auto written = c.repository.UpdateAll(moved);
    Log("layout move records=" + std::to_string(moved.size()) + " from=" + from + " to=" + to + " " +
        (written.ok() ? std::string("rewritten") : "failed: " + written.message()));
    // Nothing is marked done while the records still name the old root.
    if (!written.ok()) { c.games_move.reset(); return false; }
  }
  const auto indexes = RebaseCloudIndexes(HostPath(to) / ".playden-saves", from, to);
  if (indexes) Log("layout move cloud indexes=" + std::to_string(indexes));
  return true;
}

void RuntimeHost::FinishGamesMove(Composition& c) {
  if (!c.games_move) return;
  // Games not prepared yet (or failed) keep their mark; their next preparation publishes the new folder.
  // So does a Ready one whose folder is gone (installed again elsewhere): it can't be republished
  // (RepublishSettings), and Uninstall or Recreate bottle clears its mark.
  for (const auto& record : c.repository.Snapshot())
    if (!record.moved_from.empty() && record.state == rt::PreparationState::kReady &&
        !MissingGameFolder(HostPath(record.install_path), record))
      return;
  std::string error;
  const bool done = config_.finish_games_move && config_.finish_games_move(&error);
  Log(std::string("layout move ") + (done ? "done" : "not marked done: " + error));
  if (done) c.games_move.reset();
}

RuntimeRelocationResult RuntimeHost::RunRelocate(Composition& c, const RuntimeRelocation& request) {
  using Outcome = RuntimeRelocationResult::Outcome;
  const auto answer = [](Outcome outcome, std::string message = {}) {
    RuntimeRelocationResult result;
    result.outcome = outcome;
    result.message = std::move(message);
    return result;
  };
  const auto app = std::to_string(request.runtime_number);
  // A queued preparation names the old folder and would move the game back there (RunPrepare's
  // drive move): it goes first, and the caller tries again later.
  if (request.runtime_number != 0) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (std::any_of(prepare_requests_.begin(), prepare_requests_.end(),
                    [&](const auto& waiting) { return waiting.runtime_number == request.runtime_number; })) {
      Log("relocate app=" + app + " waits: a preparation is queued");
      return answer(Outcome::kBusy, "Finish setting up the game first.");
    }
  }
  std::optional<rt::RuntimeInstall> found;
  if (request.runtime_number != 0)
    for (const auto& record : c.repository.Snapshot())
      if (record.runtime_number == request.runtime_number) found = record;
  if (!found) {
    // No PS5 title: nothing names the folder but the caller's own records. Preparations run on
    // this worker too, so none starts until the files are in place.
    if (!request.move_files) return answer(Outcome::kDone);
    const auto moved = request.move_files();
    Log("relocate app=" + app + " to=" + request.to + " no title " + (moved.empty() ? "moved" : "failed: " + moved));
    return moved.empty() ? answer(Outcome::kDone) : answer(Outcome::kFailed, moved);
  }
  auto record = *found;
  if (record.install_path != request.to) {
    if (record.state == rt::PreparationState::kRemoving)
      return answer(Outcome::kBusy, "This game's PS5 title is being removed. Uninstall or Play finishes that first.");
    // As the 0.6 games move: the lease first, then the title's activity, both held through the
    // move and the record write.
    const auto control_path = HostPath(record.data_root) / ".playden-control";
    auto control = platform::ManagedDirectory::Open(control_path, platform::DirectoryPolicy::kPrivate);
    auto lease = control ? platform::ManagedFileLock::Acquire(*control, "publication.lock", platform::LockMode::kPublisher)
                         : std::nullopt;
    if (!lease) {
      Log("relocate app=" + app + " waits: the publication lock for " + record.title_id + " is unavailable or held");
      return answer(Outcome::kBusy, "The game's PS5 title is in use. Close it, then try again.");
    }
    std::string refusal;
    if (record.state == rt::PreparationState::kReady) {
      refusal = MaintenanceRefusal(c, record);
    } else if (record.state == rt::PreparationState::kPreparing || record.state == rt::PreparationState::kRegistering) {
      // Staged titles name the old folder and nothing would republish them.
      refusal = "Finish setting up the game first.";
    } else if (record.journal) {
      refusal = "Finish the PS5 title's recovery first.";
    } else if (!record.title_id.empty()) {
      const auto activity = c.coordinator->Activity(record.title_id);
      if (activity == rt::NativeActivity::kUnknown) refusal = kActivityUnknown;
      else if (activity != rt::NativeActivity::kStopped) refusal = "Close the game first.";
    }
    if (!refusal.empty()) {
      Log("relocate app=" + app + " waits: " + refusal);
      return answer(Outcome::kBusy, refusal);
    }
    if (request.move_files) {
      if (const auto moved = request.move_files(); !moved.empty()) {
        Log("relocate app=" + app + " move failed: " + moved);
        return answer(Outcome::kFailed, moved);
      }
    }
    // One write names the new folder. A published title keeps the old one in moved_from until a
    // publication names the new folder; one never published has nothing to verify.
    const auto from = record.install_path;
    if (record.state == rt::PreparationState::kReady) record.moved_from = from;
    record.install_path = request.to;
    if (!request.volume_id.empty()) record.volume_id = request.volume_id;
    const auto written = c.repository.UpdateAll({record});
    Log("relocate app=" + app + " title=" + record.title_id + " from=" + from + " to=" + request.to + " " +
        (written.ok() ? std::string("record rewritten") : "failed: " + written.message()));
    PublishInstalls(c, record.install_id);
    if (!written.ok()) return answer(Outcome::kFailed, "The game's record could not be saved. Try again.");
    // The publication takes the lease itself.
  }
  const auto current = c.records.Find(record.install_id);
  if (!current || current->moved_from.empty() || current->state != rt::PreparationState::kReady)
    return answer(Outcome::kDone);
  // The marked republish, as after the 0.6 games move: same revision, the new folder in the
  // descriptor, profile and dxvk.conf; the old publication is checked against the new folder.
  if (const auto busy = MaintenanceRefusal(c, *current); !busy.empty()) {
    // Committed: Play and the next start publish it.
    Log("relocate app=" + app + " republish waits: " + busy);
    return answer(Outcome::kDone, busy);
  }
  bool published = false;
  std::string code;
  const auto refused = RepublishSettings(c, *current, &published, true, nullptr, nullptr, &code);
  if (refused.empty()) {
    Log("relocate app=" + app + " republished at " + request.to);
    return answer(Outcome::kDone);
  }
  if (!c.kit || !UnverifiableMovedPublication(code, refused)) {
    // Anything else (the title running or its activity unknown, the lease or the storage busy, no
    // kit): the record stays marked moved, and Play or the next start publishes it, as after the
    // 0.6 games move.
    Log("relocate app=" + app + " republish waits" + (code.empty() ? std::string() : " code=" + code) + ": " + refused);
    return answer(Outcome::kDone, refused);
  }
  // The moved publication can't be verified: as for a game installed again on another drive, the
  // old title goes, keeping its saves, and the game is prepared again where it is now.
  Log("relocate app=" + app + " republish failed code=" + code + ", removing the old title keeping saves: " + refused);
  auto result = answer(Outcome::kDone);
  result.prepare_again = true;
  Publish([&](RuntimeSnapshot& s) { s.removing_app = request.runtime_number; });
  const auto removed = RunRemove(c, request.runtime_number, false, true);
  PublishKeptBottles(c);
  Publish([&](RuntimeSnapshot& s) { s.removing_app.reset(); });
  if (!removed.empty()) {
    Log("relocate app=" + app + " old title removal stopped: " + removed);
    result.message = "The game's old PS5 title could not be removed yet. Play tries again.";
  }
  return result;
}

// Fresh facts for one launch, from the package layer, the bottle's start check
// and the console's inventory and activity. Runs on the worker; the application
// thread reuses the worker's latest result while it is fresh.
// A bottle opened up over FTP (a recursive chmod) fails the title's start check and Playden's own:
// its modes go back first. The title can't have started from it like that, but one that is running
// or suspended (a chmod while it played) keeps its bottle as it is: a mode change reads as ESTALE there.
void RuntimeHost::HealBottle(Composition& c, const std::string& data_root, const std::string& title_id) {
  const auto bottle = HostPath(data_root);
  if (!rt::BottleTooOpen(bottle)) return;
  if (!title_id.empty()) {
    const auto activity = c.coordinator->Activity(title_id);
    if (activity != rt::NativeActivity::kStopped) {
      Log("bottle heal skipped path=" + data_root + " title=" + title_id + " activity=" +
          std::to_string(static_cast<int>(activity)));
      return;
    }
  }
  const auto healed = rt::TightenBottleModes(bottle, [this](const std::string& line) { Log(line); });
  Log("bottle heal path=" + data_root + " tightened=" + std::to_string(healed.tightened) +
      " left=" + std::to_string(healed.foreign) + " failed=" + std::to_string(healed.failed));
}

rt::LaunchPreflight RuntimeHost::Preflight(Composition& c, const rt::RuntimeInstall& record) {
  if (!pthread_equal(pthread_self(), application_thread_)) {
    HealBottle(c, record.data_root, record.title_id);
    rt::LaunchPreflight p;
    p.capability_available = c.kit.has_value() && static_cast<bool>(config_.launch_title);
    const auto root = config_.host_namespace_prefix.empty()
        ? std::filesystem::path(record.install_path)
        : config_.host_namespace_prefix / std::filesystem::path(record.install_path).relative_path();
    struct stat info{};
    p.volume_mounted = lstat(root.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
    // One full inspection. Its publication check opens the title's publication the way
    // the title's start check does, so when it passes the start check passes; only a
    // refusal asks the diagnostic for its reason.
    const auto package = c.installer->Inspect(record);
    const auto admission = package.profile_published ? std::string("ok") : c.installer->AdmissionDiagnostic(record);
    c.admission[record.install_id] = admission;
    p.bottle_present = package.mutable_bottle_present && package.mutable_bottle_owned && admission == "ok";
    p.runtime_present = package.artifacts_valid;
    p.helper_valid = package.title_staged && package.manifest_matches && c.installer->TitleCurrent(record);
    p.profile_present = package.profile_published;
    p.settings_revision = package.profile_settings_revision;
    // The title's files and runtime were just checked above; the console's lists only
    // need to name this install's title (no second full inspection).
    const auto native = c.coordinator->QueryOwnership(record);
    p.title_owned = native.installed && native.owned_manifest_matches && !native.collision;
    p.inventory_confirmed = native.available && !native.scan_pending;
    p.activity = c.coordinator->Activity(record.title_id);
    c.preflights[record.install_id] = {p, config_.now_ms()};
    Log("play preflight install=" + record.install_id + " capability=" + std::to_string(p.capability_available) +
        " volume=" + std::to_string(p.volume_mounted) + " bottle=" + std::to_string(p.bottle_present) +
        " admission=" + admission + " runtime=" + std::to_string(p.runtime_present) +
        " helper=" + std::to_string(p.helper_valid) + " profile=" + std::to_string(p.profile_present) +
        " title=" + std::to_string(p.title_owned) + " inventory=" + std::to_string(p.inventory_confirmed) +
        " activity=" + std::to_string(static_cast<int>(p.activity)) + " revision=" + std::to_string(p.settings_revision));
    return p;
  }
  const auto cached = c.preflights.find(record.install_id);
  if (cached == c.preflights.end() || config_.now_ms() - cached->second.second > kPreflightFreshMs ||
      config_.now_ms() < cached->second.second)
    return {};
  return cached->second.first;
}

rt::LaunchEvidence RuntimeHost::Evidence(Composition& c, const rt::RuntimeInstall& record,
                                         const std::string& session_id) {
  rt::LaunchEvidence evidence;
  evidence.state = rt::LaunchEvidenceState::kNone;
  const auto listed = c.sessions->Read(record, session_id);
  if (!listed.ok) {
    Log("play evidence install=" + record.install_id + " unavailable code=" + listed.code + " message=" + listed.message);
    return evidence;
  }
  for (const auto& session : listed.sessions) {
    if (session.session_id != session_id) continue;
    evidence.session_id = session.session_id;
    evidence.install_id = session.install_id;
    evidence.title_id = session.title_id;
    evidence.runtime_bundle_id = session.runtime_bundle_id;
    evidence.settings_revision = session.settings_revision;
    evidence.exit_code = session.exit_code;
    // A Playden handoff must come back as a Playden session.
    evidence.valid = session.origin == rt::SessionOrigin::kPlayden;
    switch (session.phase) {
      case rt::WrapperSessionPhase::kStarting: evidence.state = rt::LaunchEvidenceState::kStarting; break;
      case rt::WrapperSessionPhase::kRunning: evidence.state = rt::LaunchEvidenceState::kRunning; break;
      case rt::WrapperSessionPhase::kExited: evidence.state = rt::LaunchEvidenceState::kExited; break;
      case rt::WrapperSessionPhase::kFailed:
        evidence.state = rt::LaunchEvidenceState::kFailed;
        evidence.code = "wrapper-start-failed";
        evidence.message = "The game did not start.";
        break;
    }
  }
  return evidence;
}

void RuntimeHost::PublishLaunch(Composition& c, const rt::LaunchSnapshot& launch) {
  RuntimeLaunchView view;
  view.install_id = launch.install_id;
  view.state = launch.state;
  view.code = launch.code;
  view.message = launch.message;
  view.exit_code = launch.exit_code;
  if (const auto record = c.records.Find(launch.install_id)) {
    view.runtime_number = record->runtime_number;
    view.game = GameOf(record->game, record->runtime_number);
    view.title_id = record->title_id;
  }
  Publish([&](RuntimeSnapshot& s) {
    s.handoff_ready = launch.state == rt::LaunchState::kReadyToHandoff;
    if (launch.state == rt::LaunchState::kIdle && launch.install_id.empty()) s.launch.reset();
    else s.launch = view;
  });
}

void RuntimeHost::RunPlay(Composition& c, std::uint32_t app_id, const std::string& setup_program) {
  // The player waits on this Play: Microsoft's MFC may be downloaded for it, with a progress line.
  RedistWorkScope redist(redist_work_, true, app_id);
  std::optional<rt::RuntimeInstall> record;
  for (const auto& candidate : c.repository.Snapshot())
    if (candidate.runtime_number == app_id) record = candidate;
  rt::LaunchResult result;
  rt::LaunchSnapshot snapshot;
  {
    std::lock_guard<std::mutex> lock(launch_mutex_);
    if (!c.launch) {
      result = rt::LaunchResult::Failure(rt::LaunchError::kPersistence, "Play is unavailable.");
    } else if (!record) {
      result = rt::LaunchResult::Failure(rt::LaunchError::kNotFound, "This game has no PS5 title yet.");
    } else if (const auto replaced = KitReplaced(c); !replaced.empty()) {
      result = rt::LaunchResult::Failure(rt::LaunchError::kPreflight, replaced);
    } else if (record->state == rt::PreparationState::kReady && FilesChanged(*record)) {
      // Its launch names the files from before the update: Prepare makes it again first.
      result = rt::LaunchResult::Failure(rt::LaunchError::kPreflight,
                                         "This game was updated. Press Play again to set it up for the new version.");
    } else if (const auto unwritable = EnsureWritable(record->install_path,
                                                      WorkingFolder(record->launch_working_directory, record->launch_path, record->launch_in_install_root));
               !unwritable.empty()) {
      result = rt::LaunchResult::Failure(rt::LaunchError::kPreflight, unwritable);
    } else {
      // Settings changed since the title's publication take effect now, while nothing of
      // the game runs: Home and Playden then start the same published profile.
      bool idle = false;
      if (c.launch_journal) {
        const auto pending = c.launch_journal->LoadPending();
        idle = pending.ok && !pending.session && !record->journal &&
               record->state == rt::PreparationState::kReady &&
               c.coordinator->Activity(record->title_id) == rt::NativeActivity::kStopped;
      }
      std::string settings;
      if (!setup_program.empty()) {
        // Another program for one run: its profile is published now, so nothing of the game may be running.
        if (!idle) {
          auto busy = MaintenanceRefusal(c, *record);
          settings = busy.empty() ? std::string("Close the game first.") : std::move(busy);
        } else if (!record->moved_from.empty()) {
          settings = "This game needs an update for Playden 0.6 first: press Play once.";
        } else {
          settings = RepublishSettings(c, *record, nullptr, true, nullptr, nullptr, nullptr, false, setup_program);
        }
        Log("play setup program app=" + std::to_string(app_id) + " program=" + setup_program + " " +
            (settings.empty() ? "published" : settings));
      } else if (!record->moved_from.empty()) {
        // Its folder moved in 0.6: the title starts only once its publication names the new one.
        if (!idle) {
          auto busy = MaintenanceRefusal(c, *record);
          settings = "This game needs an update for Playden 0.6 first: " + (busy.empty() ? "Close the game first." : busy);
        } else {
          Publish([&](RuntimeSnapshot& s) { s.play_progress[app_id] = kPlayUpdatingForLayout; });
          bool again = false;
          settings = RepublishSettings(c, *record, nullptr, true, &again);
          Publish([&](RuntimeSnapshot& s) { s.play_progress.erase(app_id); });
          if (!settings.empty())
            settings = "This game could not be updated for Playden 0.6: " + settings + (again ? kRecreateHint : "");
          Log("play moved app=" + std::to_string(app_id) + " " + (settings.empty() ? "republished" : settings));
        }
      } else if (idle) {
        settings = RepublishSettings(c, *record);
      }
      // Saves the game wrote and FTP edits made since, once nothing of the bottle is held and
      // only when Play goes ahead: a refused Play changes no modes.
      if (idle && settings.empty())
        if (const auto current = c.records.Find(record->install_id)) SettleBottle(c, *current, "play");
      result = settings.empty() ? c.launch->Request(record->install_id)
                                : rt::LaunchResult::Failure(rt::LaunchError::kPreflight, settings);
    }
    if (result.accepted) {
      snapshot = c.launch->Snapshot();
      PublishLaunch(c, snapshot);
    }
  }
  // A refused request leaves any pending session as it is; the refusal is
  // shown for this game only.
  Publish([&](RuntimeSnapshot& s) {
    ++s.plays_handled;
    if (result.accepted) s.play_refusals.erase(app_id);
    else s.play_refusals[app_id] = result.message.empty() ? "The game could not start." : result.message;
    // What it went ahead without (Microsoft's MFC), until a Play gets it.
    if (result.accepted && !redist_work_.notice.empty()) s.play_notices[app_id] = redist_work_.notice;
    else if (result.accepted) s.play_notices.erase(app_id);
  });
  Log("play request app=" + std::to_string(app_id) + " accepted=" + std::to_string(result.accepted) +
      " session=" + result.session_id + " code=" + result.code + " message=" + result.message);
  PublishInstalls(c);
}

bool RuntimeHost::PollLaunch(Composition& c) {
  std::lock_guard<std::mutex> lock(launch_mutex_);
  if (!c.launch) return false;
  auto snapshot = c.launch->Snapshot();
  if (snapshot.state == rt::LaunchState::kReadyToHandoff) {
    PublishLaunch(c, snapshot);
    return true;
  }
  const bool pending = c.launch_journal && [&] {
    const auto loaded = c.launch_journal->LoadPending();
    return loaded.ok && loaded.session.has_value();
  }();
  if (pending && snapshot.state == rt::LaunchState::kFailed) {
    // A failure read from bad or missing evidence says nothing about the title
    // itself: end the session only once the console shows the title stopped.
    const auto record = c.records.Find(snapshot.install_id);
    if (record && c.coordinator->Activity(record->title_id) != rt::NativeActivity::kStopped) {
      PublishLaunch(c, snapshot);
      return true;
    }
  }
  if (pending && (snapshot.state == rt::LaunchState::kExited || snapshot.state == rt::LaunchState::kFailed)) {
    const auto finished = c.launch->Finish();
    // The saves the game just wrote are Wine's (0600): opened to FTP again.
    if (finished.accepted && c.kit && rt::KitCapabilitiesFromManifest(c.kit->capabilities, true).bottle_profile_shared)
      if (const auto record = c.records.Find(snapshot.install_id);
          record && c.coordinator->Activity(record->title_id) == rt::NativeActivity::kStopped)
        SettleBottle(c, *record, "session");
    Log("play finished install=" + snapshot.install_id + " session=" + snapshot.session_id + " state=" +
        std::to_string(static_cast<int>(snapshot.state)) + " code=" + snapshot.code + " exit=" +
        (snapshot.exit_code ? std::to_string(*snapshot.exit_code) : std::string("unknown")) +
        " cleared=" + std::to_string(finished.accepted) + " " + finished.code);
    // A program's run ended (one closed from the PS5 menu is seen only now, after a restart): the game's own
    // settings go back at once, so its Home tile starts the game again.
    if (finished.accepted)
      if (const auto record = c.records.Find(snapshot.install_id);
          record && !record->setup_program.empty() && MaintenanceRefusal(c, *record).empty()) {
        const auto refused = RepublishSettings(c, *record, nullptr, false);
        if (!refused.empty()) Log("setup program app=" + std::to_string(record->runtime_number) + " kept: " + refused);
      }
    PublishLaunch(c, snapshot);
    return !finished.accepted;
  }
  PublishLaunch(c, snapshot);
  return pending;
}

namespace {
// JSON string body for a title name: quotes, backslashes and control characters escaped.
std::string JsonEscaped(const std::string& text) {
  std::string out;
  for (const unsigned char ch : text) {
    if (ch == '"' || ch == '\\') { out += '\\'; out += static_cast<char>(ch); }
    else if (ch < 0x20) { char code[8]; std::snprintf(code, sizeof(code), "\\u%04x", ch); out += code; }
    else out += static_cast<char>(ch);
  }
  return out;
}
}  // namespace

namespace {
// The "titleName" value of a param.json, or nothing.
std::optional<std::string> TitleNameOf(const std::string& param) {
  static constexpr std::string_view key = "\"titleName\"";
  const auto at = param.find(key);
  if (at == std::string::npos) return std::nullopt;
  auto cursor = param.find_first_not_of(" \t\r\n", at + key.size());
  if (cursor == std::string::npos || param[cursor] != ':') return std::nullopt;
  cursor = param.find_first_not_of(" \t\r\n", cursor + 1);
  if (cursor == std::string::npos || param[cursor] != '"') return std::nullopt;
  std::string value;
  for (auto i = cursor + 1; i < param.size(); ++i) {
    if (param[i] == '"') return value;
    if (param[i] == '\\' && i + 1 < param.size()) ++i;
    value += param[i];
  }
  return std::nullopt;
}
}  // namespace

std::optional<std::string> RuntimeHost::InstalledTitleName(const rt::RuntimeInstall& record) const {
  if (record.data_root.empty()) return std::nullopt;
  const auto root = config_.host_namespace_prefix.empty()
      ? std::filesystem::path(record.data_root)
      : config_.host_namespace_prefix / std::filesystem::path(record.data_root).relative_path();
  const auto path = root / "title-metadata/sce_sys/param.json";
  const int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) return std::nullopt;
  std::string bytes;
  char chunk[4096];
  for (ssize_t count; (count = read(fd, chunk, sizeof(chunk))) > 0 && bytes.size() < 64 * 1024;)
    bytes.append(chunk, static_cast<std::size_t>(count));
  close(fd);
  return TitleNameOf(bytes);
}

bool RuntimeHost::TitleNameCurrent(const rt::RuntimeInstall& record) const {
  std::string wanted;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = title_names_.find(record.runtime_number);
    if (found == title_names_.end() || found->second.empty()) return true;  // nothing asked for
    wanted = found->second;
  }
  const auto installed = InstalledTitleName(record);
  return installed && *installed == wanted;
}

rt::PackageActionResult RuntimeHost::WriteTitleMetadata(Composition& c, const rt::RuntimeInstall& record,
                                                       const std::string& title_name) {
  const auto failed = [](std::string message) {
    return rt::PackageActionResult{rt::PackageActionState::kFailed, "title-metadata", std::move(message)};
  };
  if (!c.kit) return failed("No verified runtime kit is installed.");
  const rt::KitTitleVariant* variant = rt::KitVariantFor(c.kit->view, record.title_id);
  if (!variant) return failed("The kit has no title for this game.");
  // The kit's param.json with the game's name in place of the kit's.
  std::string param;
  {
    const auto path = c.kit->view.root / variant->path / "sce_sys/param.json";
    const int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return failed("The kit's title parameters are unreadable.");
    char chunk[4096];
    for (ssize_t count; (count = read(fd, chunk, sizeof(chunk))) > 0 && param.size() < 64 * 1024;)
      param.append(chunk, static_cast<std::size_t>(count));
    close(fd);
  }
  // Replaces the value of the single "<key>": "<value>" entry; false for any other layout.
  const auto replace_value = [&param](std::string_view key, const std::string& value) {
    const std::string quoted = "\"" + std::string(key) + "\"";
    const auto at = param.find(quoted);
    if (at == std::string::npos || param.find(quoted, at + quoted.size()) != std::string::npos) return false;
    auto cursor = param.find_first_not_of(" \t\r\n", at + quoted.size());
    if (cursor == std::string::npos || param[cursor] != ':') return false;
    cursor = param.find_first_not_of(" \t\r\n", cursor + 1);
    if (cursor == std::string::npos || param[cursor] != '"') return false;
    const auto end = param.find('"', cursor + 1);
    if (end == std::string::npos) return false;
    param.replace(cursor + 1, end - cursor - 1, value);
    return true;
  };
  // The any-title shell is one title for every game: each copy gets its own title ID.
  if (variant->any_title &&
      !(replace_value("titleId", record.title_id) && replace_value("conceptId", record.title_id.substr(4)) &&
        replace_value("contentId", "UP9000-" + record.title_id + "_00-PROSPEROWIN00001")))
    return failed("The kit's title parameters have an unexpected layout.");
  const std::string name = title_name.empty() ? "Steam game " + std::to_string(record.runtime_number) : title_name;
  if (!replace_value("titleName", JsonEscaped(name)))
    Log("title metadata app=" + std::to_string(record.runtime_number) + " keeps the kit's name: unexpected layout");
  std::filesystem::path program;
  if (auto launch = record.launch_path; !launch.empty() && !record.install_path.empty()) {
    std::replace(launch.begin(), launch.end(), '\\', '/');
    program = HostPath(record.install_path) / launch;
  }
  const auto icon = config_.title_icon ? config_.title_icon(GameOf(record), program) : std::nullopt;
  Log("title metadata app=" + std::to_string(record.runtime_number) + " name=" + name + " icon=" +
      (icon ? std::to_string(icon->size()) + "B" : std::string("kit")));
  return c.installer->WriteTitleMetadata(record, param, icon);
}

namespace {
// Generated recipes live in the private runtime state, one file per game.
std::string GeneratedRecipeFile(const std::string& recipe_id) { return "recipe-" + recipe_id + ".json"; }
}  // namespace

// The recipe a game is prepared with: Playden's shipped one, otherwise one generated
// from Steam's launch options and saved so recovery and later settings changes use the
// same launch entries. Returns null with the reason for the player.
std::string RuntimeHost::RefreshChangedFiles(Composition& c, const RuntimePrepareRequest& request,
                                             const rt::RuntimeInstall& record) {
  const auto app = std::to_string(record.runtime_number);
  // A recipe Playden generated from the old files is made again from the new ones, with the launch options and
  // Game settings choices a fresh preparation uses: a program the player chose still starts while it is there,
  // and one that is gone is refused as a fresh preparation refuses it. A shipped recipe names its programs itself.
  if (rt::IsGeneratedRecipeId(record.recipe_id)) {
    std::string reason;
    const auto* recipe = RecipeForPrepare(c, request, &reason);
    if (!recipe) {
      Log("files changed app=" + app + " recipe refused: " + reason);
      return reason;
    }
    if (recipe->id != record.recipe_id || std::to_string(recipe->version) != record.recipe_version) {
      Log("files changed app=" + app + " recipe " + record.recipe_id + "/" + record.recipe_version + " became " + recipe->id +
          "/" + std::to_string(recipe->version));
      return std::string("This game's setup changed with its update.") + kRecreateHint;
    }
  }
  // Settings older than the title's can't be published over it (a forced publication skips them silently).
  if (const auto saved = config_.settings_record ? config_.settings_record(GameOf(record)) : std::nullopt;
      saved && saved->revision < record.settings_revision)
    return "The game's saved settings are older than its PS5 title's; prepare it again.";
  // Published again with the new recipe: a program that is gone moves to the one the recipe starts now.
  bool published = false;
  const auto refused = PublishSavedSettings(c, record, &published, true, nullptr, nullptr, true, true);
  const auto current = c.records.Find(record.install_id);
  Log("files changed app=" + app + " published=" + std::to_string(published) + " launch=" +
      (current ? current->launch_entry_id + ":" + current->launch_path : std::string("-")) +
      (refused.empty() ? std::string() : " refused=" + refused));
  if (!refused.empty()) return refused;
  // A marker that can't go only means the next Play sets the game up again (publishing nothing new).
  const auto marker = HostPath(record.install_path) / installs::kFilesChangedName;
  if (unlink(marker.c_str()) != 0 && errno != ENOENT)
    Log("files changed app=" + app + " marker unlink errno=" + std::to_string(errno));
  PublishInstalls(c, record.install_id);
  return {};
}

const rt::GameRecipe* RuntimeHost::RecipeForPrepare(Composition& c, const RuntimePrepareRequest& request,
                                                    std::string* reason) {
  for (const auto& candidate : c.recipes)
    if (rt::RecipeGame(candidate) == GameOf(request) && candidate.app_id == request.runtime_number &&
        !rt::IsGeneratedRecipeId(candidate.id))
      return &candidate;
  if (!c.settings_data_reason.empty()) { *reason = c.settings_data_reason; return nullptr; }
  const auto root = config_.host_namespace_prefix.empty()
      ? std::filesystem::path(request.install_path)
      : config_.host_namespace_prefix / std::filesystem::path(request.install_path).relative_path();
  // The program the player chose and "Try anyway" come from Game settings, so a game whose
  // Steam launch options don't work can still be started.
  const auto saved = config_.settings_record ? config_.settings_record(GameOf(request)) : std::nullopt;
  const auto choices = saved ? rt::RecipeChoicesFrom(*saved) : rt::RecipeChoices{};
  const auto kit = c.kit ? rt::KitCapabilitiesFromManifest(c.kit->capabilities, true) : rt::KitCapabilities{};
  // Steam's options all for other branches: the program the player chose goes first, and with no
  // choice yet, the options whose program is in the folder decide (the import rules: one clear
  // program starts the game, two or more are the player's choice).
  auto launches = request.launches;
  const auto same_program = [](const std::string& path, const std::string& other) {
    return std::equal(path.begin(), path.end(), other.begin(), other.end(), [](char a, char b) {
      return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
    });
  };
  const auto put_first = [&](const std::string& program) {
    std::stable_partition(launches.begin(), launches.end(), [&](const rt::LaunchEntry& launch) {
      const auto path = steam::RelativeGamePath(launch.executable);
      return path && same_program(*path, program);
    });
  };
  std::vector<SteamLaunchChoice> steam_choices;
  if (saved && !saved->steam_launch.empty()) {
    put_first(saved->steam_launch);
  } else if (request.launches_any_branch && choices.program.empty()) {
    for (const auto& launch : launches) {
      const auto path = steam::RelativeGamePath(launch.executable);
      if (!path || rt::GameProgramArchitecture(root.string(), *path).empty()) continue;
      if (std::none_of(steam_choices.begin(), steam_choices.end(),
                       [&](const SteamLaunchChoice& other) { return same_program(*path, other.path); }))
        steam_choices.push_back({*path, stores::local::IsLikelyHelper(*path)});
    }
    if (const auto clear = stores::local::ClearGameProgram(steam_choices)) put_first(steam_choices[*clear].path);
    if (steam_choices.size() < 2 || stores::local::ClearGameProgram(steam_choices)) steam_choices.clear();
  }
  if (!steam_choices.empty()) {
    *reason = "Choose the program that starts this game.";
    Log("prepare waits app=" + std::to_string(request.runtime_number) + " steam programs=" +
        std::to_string(steam_choices.size()));
    Publish([&](RuntimeSnapshot& s) {
      auto& setup = s.game_setups[request.runtime_number];
      setup.steam_choices = std::move(steam_choices);
      setup.draft.reset();
      setup.refusal_soft = false;
    });
    return nullptr;
  }
  rt::GameRecipe generated;
  bool soft = true;
  rt::GameRecipeDiagnostics diagnostics;
  const bool made = (!launches.empty() || !choices.program.empty()) &&
      rt::GenerateGameRecipe(GameOf(request), request.runtime_number, request.title_name, launches, root.string(),
                             &generated, reason, kit, choices, &soft, &diagnostics);
  // The same line an unpack writes, so a refused SteamStub program is found the same way.
  // A recipe made from another of Steam's programs says which one it skipped and why (The Saboteur, #320).
  if (!diagnostics.steam_stub_program.empty())
    Log("steamstub app=" + std::to_string(request.runtime_number) + " found=" + diagnostics.steam_stub_program +
        " changed=- " + (made ? std::string("skipped") : "failed=" + *reason) + " (" +
        (diagnostics.steam_stub_detail.empty() ? "unknown" : diagnostics.steam_stub_detail) + ")");
  if (!made && launches.empty() && choices.program.empty())
    *reason = request.launch_reason.empty() ? "Steam lists no Windows program this console can start for this game."
                                            : request.launch_reason;
  // A refused game gets the folder's programs and a draft for Game settings to start from; a
  // prepared one is scanned when its Program picker opens (ScanPrograms).
  std::vector<rt::GameProgram> programs;
  std::optional<rt::GameRecipe> draft;
  if (!made) {
    programs = rt::ListGamePrograms(root.string(), kit);
    LogSteamStubPrograms(request.runtime_number, programs);
    auto tried = choices;
    tried.try_anyway = true;
    rt::GameRecipe permissive;
    std::string ignored;
    if (!rt::GenerateGameRecipe(GameOf(request), request.runtime_number, request.title_name, launches, root.string(),
                                &permissive, &ignored, kit, tried)) {
      // No program yet: the sheet starts from the Program row.
      permissive = {};
      permissive.id = playden::GeneratedRecipeId(GameOf(request));
      permissive.version = 2;
      permissive.app_id = request.runtime_number;
      if (GameOf(request).store != Store::kSteam) permissive.game = GameOf(request);
      permissive.name = request.title_name;
      permissive.defaults["steam_features"] = "emulate";
    }
    draft = std::move(permissive);
    Log("prepare refused app=" + std::to_string(request.runtime_number) + " soft=" + std::to_string(soft) +
        " programs=" + std::to_string(programs.size()) + " " + *reason);
  }
  Publish([&](RuntimeSnapshot& s) {
    auto& setup = s.game_setups[request.runtime_number];
    setup.steam_choices.clear();
    if (!made) setup.programs = std::move(programs), setup.scanned = true;
    setup.draft = std::move(draft);
    setup.refusal_soft = !made && soft;
  });
  if (!made) return nullptr;
  const auto text = rt::SerializeGameRecipe(generated);
  const auto written = c.storage.WriteAtomicPrivate(GeneratedRecipeFile(generated.id), Bytes(text.begin(), text.end()));
  if (!written.ok()) {
    Log("generated recipe write failed app=" + std::to_string(request.runtime_number) + " message=" + written.message());
    *reason = "Playden could not save how to start this game.";
    return nullptr;
  }
  Log("generated recipe app=" + std::to_string(request.runtime_number) + " detected=" + generated.detected +
      " architecture=" + generated.architecture +
      " entries=" + std::to_string(generated.launch_entries.size()) + " executable=" +
      generated.launch_entries.front().executable);
  c.recipes.erase(std::remove_if(c.recipes.begin(), c.recipes.end(),
                                 [&](const rt::GameRecipe& recipe) { return recipe.id == generated.id; }),
                  c.recipes.end());
  c.recipes.push_back(std::move(generated));
  PublishRecipes(c);
  return &c.recipes.back();
}

void RuntimeHost::LoadGeneratedRecipes(Composition& c) {
  for (const auto& record : c.repository.Snapshot()) {
    if (!rt::IsGeneratedRecipeId(record.recipe_id)) continue;
    const auto bytes = c.storage.Read(GeneratedRecipeFile(record.recipe_id), 256 * 1024);
    rt::GameRecipe recipe;
    std::string reason;
    if (!bytes.ok() ||
        !rt::ParseGameRecipe(std::string(bytes.value().begin(), bytes.value().end()), &recipe, &reason) ||
        recipe.id != record.recipe_id || !RecipeFits(recipe, record) ||
        std::to_string(recipe.version) != record.recipe_version) {
      Log("generated recipe unavailable install=" + record.install_id + " " +
          (bytes.ok() ? reason : bytes.message()));
      continue;
    }
    c.recipes.push_back(std::move(recipe));
  }
  PublishRecipes(c);
}

void RuntimeHost::PublishRecipes(const Composition& c) {
  std::vector<rt::GameRecipe> generated;
  for (const auto& recipe : c.recipes)
    if (rt::IsGeneratedRecipeId(recipe.id)) generated.push_back(recipe);
  Publish([&](RuntimeSnapshot& s) { s.generated_recipes = std::move(generated); });
}

std::string RuntimeHost::RunPrepare(Composition& c, const RuntimePrepareRequest& request, bool* awaiting) {
  if (!c.leftover_journals_known) return "The PS5 title removal records cannot be read; restart Playden before preparing games.";
  if (!c.kit) return Snapshot().kit_reason;
  if (const auto replaced = KitReplaced(c); !replaced.empty()) return replaced;
  // Where the time before the first step goes: the recipe, the reservation and the title's metadata.
  const auto began = std::chrono::steady_clock::now();
  const auto elapsed_ms = [&began] {
    return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began).count());
  };
  // A game being started or played keeps its title and bottle as they are.
  if (c.launch_journal) {
    const auto pending = c.launch_journal->LoadPending();
    if (!pending.ok) return "The launch record cannot be read; restart Playden before preparing.";
    if (pending.session && pending.session->install_id == ToInstallId(GameOf(request.game, request.runtime_number)))
      return "Close the game before updating or repairing its PS5 title.";
  }
  // Before anything is written (or a managed folder opened): folders made over FTP may need a fix.
  if (const auto unwritable = EnsureWritable(request.install_path, {}); !unwritable.empty()) return unwritable;
  std::optional<rt::RuntimeInstall> existing;
  for (const auto& record : c.repository.Snapshot())
    if (record.runtime_number == request.runtime_number) existing = record;
  HealBottle(c, config_.package_paths.console_data_root + "/playden/games/" +
                    (existing ? existing->install_id : ToInstallId(GameOf(request.game, request.runtime_number))),
             existing ? existing->title_id : std::string());
  // A title being removed because its game moved drives is finished here; any other removal by Uninstall.
  const bool moving = existing && existing->state == rt::PreparationState::kRemoving && existing->journal &&
                      existing->journal->operation == "remove-move";
  if (existing && existing->state == rt::PreparationState::kRemoving && !moving)
    return "This game's PS5 title is still being removed. Uninstall the game again to finish.";
  // RunRemove runs inline, so removals_handled (the controller's uninstall progress) does not move.
  // A pending move finishes whatever the paths say now (the game may be back in its first folder).
  if (existing && (moving || (!existing->journal && existing->install_path != request.install_path))) {
    // Installed again on another drive (an uninstall that stopped part way can leave the title behind):
    // the old title starts files that are no longer the game's. It goes, keeping its saves, and the
    // game is prepared from where it is now; the saves come back after (RestoreKeptSaves), as with
    // Recreate bottle: the Windows user folders, not the bottle's registry or other folders.
    struct stat info{};
    if (lstat(HostPath(request.install_path).c_str(), &info) != 0 || !S_ISDIR(info.st_mode))
      return "The game's folder is missing; download it again.";
    Log("moved install app=" + std::to_string(request.runtime_number) + " title=" + existing->title_id + " from=" +
        existing->install_path + " to=" + request.install_path);
    // Recorded as a move, so a removal that stops part way is finished by the next Play or Prepare
    // and never turns into an uninstall of the game's new download.
    if (const auto removed = RunRemove(c, request.runtime_number, false, true); !removed.empty()) {
      Log("moved install app=" + std::to_string(request.runtime_number) + " removal stopped: " + removed);
      return "The game's old PS5 title could not be removed yet. Play tries again.";
    }
    existing.reset();
  }
  rt::InstallServiceResult started = rt::InstallServiceResult::Success();
  bool game_root_owned = true;
  const auto tighten_working = [&](const std::string& working) -> std::string {
    // The root was checked above; the working folder is known only now (it is where dxvk.conf goes).
    if (!working.empty())
      if (auto unwritable = EnsureWritable(request.install_path, working); !unwritable.empty()) return unwritable;
    return game_root_owned ? TightenWorkingFolder(HostPath(request.install_path), working) : std::string();
  };
  // A kit with game-root-unowned admits a game folder of any owner and mode (publication
  // metadata schema 3), so files a user adds or changes over FTP (mods) are left as they are.
  // Otherwise settings publication writes dxvk.conf into the game folder and requires that
  // folder to belong to the app user without group/other write access. Console
  // downloads are created world-writable; tighten an owned root to 0755. A root
  // owned by anyone else is refused, never taken over. A drive that records no
  // owners or permissions (exFAT) has nothing to check or tighten; the kit must
  // then admit such folders.
  const bool unowned_kit = rt::KitCapabilitiesFromManifest(c.kit->capabilities, true).game_root_unowned;
  if (unowned_kit) {
    struct stat info{};
    if (lstat(HostPath(request.install_path).c_str(), &info) != 0 || !S_ISDIR(info.st_mode))
      return "The game's folder is unavailable.";
    game_root_owned = false;
  } else {
    const auto root = config_.host_namespace_prefix.empty()
        ? std::filesystem::path(request.install_path)
        : config_.host_namespace_prefix / std::filesystem::path(request.install_path).relative_path();
    struct stat info{};
    if (lstat(root.c_str(), &info) != 0 || !S_ISDIR(info.st_mode))
      return "The game's folder is unavailable.";
    const auto records_ownership = platform::RecordsOwnership(root);
    if (!records_ownership) return "The game's folder can't be written.";
    if (!*records_ownership) {
      if (!rt::KitCapabilitiesFromManifest(c.kit->capabilities, true).game_root_unowned)
        return "This drive doesn't keep file owners, and the installed runtime kit can't start games from it. "
               "Install the game on console storage, or update the kit.";
      Log("game root without ownership path=" + request.install_path);
      game_root_owned = false;
    } else if (info.st_uid != geteuid()) {
      return "The game's folder belongs to another user.";
    } else if ((info.st_mode & 07777) != 0700 && (info.st_mode & 07777) != 0755) {
      if (chmod(root.c_str(), 0755) != 0) return "The game's folder permissions could not be tightened.";
      char mode[8]; std::snprintf(mode, sizeof(mode), "%04o", unsigned(info.st_mode & 07777));
      Log("game root mode " + std::string(mode) + " -> 0755 path=" + request.install_path);
    }
  }
  if (existing) {
    if (const auto refused = tighten_working(WorkingFolder(existing->launch_working_directory, existing->launch_path,
                                                                 existing->launch_in_install_root));
        !refused.empty())
      return refused;
  }
  // Reservations survive a failed allow-list or metadata write. Complete these
  // prerequisites again before a fresh start. Nothing is published yet (a title or
  // bottle left by another kit is refused first), so a reservation bound to an
  // earlier kit's artifacts is bound to this kit's.
  const auto initialize_reserved = [&](rt::RuntimeInstall& record) -> std::string {
    std::vector<rt::RuntimeArtifactIdentity> current;
    for (const auto& artifact : c.kit->view.artifacts)
      current.push_back({artifact.artifact_id, artifact.identity, artifact.sha256});
    const bool bound_current =
        record.artifacts.size() == current.size() &&
        std::all_of(current.begin(), current.end(), [&](const rt::RuntimeArtifactIdentity& wanted) {
          return std::any_of(record.artifacts.begin(), record.artifacts.end(), [&](const auto& bound) {
            return bound.artifact_id == wanted.artifact_id && bound.identity == wanted.identity &&
                   bound.sha256 == wanted.sha256;
          });
        });
    if (!bound_current) {
      if (!record.artifacts.empty()) Log("prepare rebinds app=" + std::to_string(record.runtime_number) + " to the kit's artifacts");
      record.artifacts = std::move(current);
      const auto bound = c.repository.Update(record);
      if (!bound.ok()) return "The kit artifacts could not be recorded: " + bound.message();
    }
    if (!c.installer->TitleMetadataCurrent(record)) {
      const auto written = WriteTitleMetadata(c, record, request.title_name);
      if (written.state != rt::PackageActionState::kComplete) return written.message;
    }
    return {};
  };
  if (existing) {
    // Startup may have failed to publish the list. Never install a rule shell
    // until its durable reservation is admitted by the helper.
    if (const auto listed = SyncTitleAllowList(c); !listed.empty()) return listed;
    if (existing->state == rt::PreparationState::kReady) {
      if (c.coordinator->Activity(existing->title_id) != rt::NativeActivity::kStopped)
        return "Close the game before updating or repairing its PS5 title.";
      // An interrupted settings update resumes its saved request; nothing is regenerated.
      if (existing->journal) {
        const auto resumed = DrivePublication(c, *existing);
        PublishInstalls(c);
        if (resumed.state == rt::PackageActionState::kComplete) return {};
        // An unowned upgrade's request that never reached its publication is dropped: the game
        // keeps its owned publication, and the update goes on below as for any Ready game.
        if (!DropUpgradeRequest(c, existing->install_id, resumed.message)) return resumed.message;
        existing = c.records.Find(existing->install_id);
        if (!existing) return "The game's install record is missing.";
      }
      bool changed = false;
      // Older titles carry the kit's generic name and icon: give them the game's.
      const bool metadata_current = c.installer->TitleMetadataCurrent(*existing) &&
                                    (request.title_name.empty() || InstalledTitleName(*existing) == request.title_name);
      if (!metadata_current) {
        const auto written = WriteTitleMetadata(c, *existing, request.title_name);
        if (written.state != rt::PackageActionState::kComplete) return written.message;
      }
      if (!c.installer->TitleCurrent(*existing) || !metadata_current) {
        // Same kit id and recipe; title files changed (a new wrapper build) or
        // the runtime was rebuilt (same artifacts, new bytes). The registration
        // and publication stay as they are. A rebuilt runtime is copied into the
        // title first and its new artifact hashes recorded only after that.
        auto target = *existing;
        bool runtime_upgrade = false;
        if (target.artifacts.size() == c.kit->view.artifacts.size()) {
          std::vector<rt::RuntimeArtifactIdentity> upgraded;
          bool same_set = true;
          for (const auto& artifact : c.kit->view.artifacts) {
            bool found = false;
            for (const auto& bound : target.artifacts)
              if (bound.artifact_id == artifact.artifact_id) {
                found = true;
                // The recipe must be the one this install was prepared from.
                if (artifact.artifact_id != "runtime" && bound.sha256 != artifact.sha256) same_set = false;
              }
            same_set = same_set && found;
            upgraded.push_back({artifact.artifact_id, artifact.identity, artifact.sha256});
          }
          const auto differs = [&] {
            for (const auto& wanted : upgraded) {
              bool equal = false;
              for (const auto& bound : target.artifacts)
                equal |= bound.artifact_id == wanted.artifact_id && bound.identity == wanted.identity &&
                         bound.sha256 == wanted.sha256;
              if (!equal) return true;
            }
            return false;
          };
          if (same_set && differs()) {
            target.artifacts = std::move(upgraded);
            runtime_upgrade = true;
          }
        }
        const auto refreshed = c.installer->RefreshTitle(target);
        if (runtime_upgrade && refreshed.state == rt::PackageActionState::kComplete) {
          const auto recorded = c.repository.Update(target);
          Log("runtime upgrade app=" + std::to_string(request.runtime_number) + " recorded=" + std::to_string(recorded.ok()));
          if (!recorded.ok()) return "The runtime was updated but could not be recorded; press Update again.";
          *existing = target;
        }
        Log("title refresh app=" + std::to_string(request.runtime_number) + " state=" +
            std::to_string(static_cast<int>(refreshed.state)) + " code=" + refreshed.code + " message=" + refreshed.message);
        PublishInstalls(c);
        if (refreshed.state != rt::PackageActionState::kComplete) return refreshed.message;
        changed = true;
      }
      // A game whose folder moved in 0.6 is published again for the new folder first, and an
      // owned publication again as unowned. A moved game whose publication can't be redone
      // (settings older than its title, a recipe Playden no longer has) stays as it is: the way
      // out is the player's confirmed choice.
      if (!existing->moved_from.empty() || NeedsUnownedPublication(c, *existing)) {
        bool again = false;
        std::string deferred;
        if (const auto refused = RepublishSettings(c, *existing, nullptr, true, &again, &deferred); !refused.empty())
          return again ? refused + kRecreateHint : refused;
        const auto install_id = existing->install_id;
        existing = c.records.Find(install_id);
        if (!existing) return "The game's install record is missing.";
        if (deferred.empty()) changed = true;
      }
      // A refreshed title may now admit the kit's modes for its bottle.
      SettleBottle(c, *existing, "update");
      if (const auto unlinked = UnlinkDxvkConfig(*existing); !unlinked.empty()) return unlinked;
      auto admission = c.installer->AdmissionDiagnostic(*existing);
      if (admission != "ok") {
        const auto repaired = c.installer->RepairBottle(*existing);
        admission = c.installer->AdmissionDiagnostic(*existing);
        Log("bottle repair app=" + std::to_string(request.runtime_number) + " state=" +
            std::to_string(static_cast<int>(repaired.state)) + " code=" + repaired.code + " admission=" + admission);
        if (repaired.state != rt::PackageActionState::kComplete) {
          c.admission[existing->install_id] = admission; PublishInstalls(c);
          return repaired.message;
        }
        changed = true;
      }
      c.admission[existing->install_id] = admission;
      PublishInstalls(c);
      if (admission != "ok") return "The PS5 title still fails its start check: " + admission;
      OpenGameFolders(c, *existing);
      // An update changed the game's files since its launch was set up: the launch is made again from them.
      if (FilesChanged(*existing)) {
        if (const auto refused = RefreshChangedFiles(c, request, *existing); !refused.empty()) return refused;
        changed = true;
        existing = c.records.Find(existing->install_id);
        if (!existing) return "The game's install record is missing.";
      }
      // The profile was built for the runtime the title had then; the kit's may take more keys (120 Hz output) or
      // fewer. Every Update that gets here publishes the saved settings again when they now write something else
      // (nothing otherwise), so an Update that failed after its title refresh is finished by the next one.
      {
        const auto current = c.records.Find(existing->install_id);
        if (!current) return "The game's install record is missing.";
        bool republished = false;
        const auto refused = RepublishSettings(c, *current, &republished, true, nullptr, nullptr, nullptr, true);
        Log("update republish app=" + std::to_string(request.runtime_number) + " published=" + std::to_string(republished) +
            (refused.empty() ? std::string() : " refused=" + refused));
        if (!refused.empty()) return refused;
        changed = changed || republished;
      }
      return changed ? std::string{} : "This game is already prepared.";
    }
    if (existing->install_path != request.install_path)
      return "The game's files moved since preparation started; recovery is not connected yet.";
    // A generated recipe that could not be read back is made again before a fresh start.
    const bool fresh_start = existing->state == rt::PreparationState::kFailed ||
                             existing->state == rt::PreparationState::kUnavailable ||
                             existing->state == rt::PreparationState::kDownloaded;
    if (fresh_start && rt::IsGeneratedRecipeId(existing->recipe_id) &&
        std::none_of(c.recipes.begin(), c.recipes.end(),
                     [&](const rt::GameRecipe& recipe) { return recipe.id == existing->recipe_id; })) {
      std::string reason;
      const auto* recipe = RecipeForPrepare(c, request, &reason);
      if (!recipe) return reason;
      if (recipe->id != existing->recipe_id || std::to_string(recipe->version) != existing->recipe_version)
        return "This game's setup changed; remove its PS5 title before preparing it again.";
    }
    // A setup that stopped before a kit swap left its title or bottle made with the earlier kit. Neither
    // Retry nor a fresh start finishes over it (both check them against this kit's), so each Try again
    // would fail the same way: Recreate bottle removes the title and prepares the game again.
    if (fresh_start && c.installer->SetupFromOtherKit(*existing)) {
      Log("prepare refused app=" + std::to_string(request.runtime_number) + ": title or bottle from another kit");
      return std::string("This game's setup was started with another runtime kit.") + kRecreateHint;
    }
    if (fresh_start)
      if (const auto initialized = initialize_reserved(*existing); !initialized.empty()) return initialized;
    if (existing->state == rt::PreparationState::kFailed || existing->state == rt::PreparationState::kUnavailable)
      started = c.service->Retry(existing->install_id);
    else if (existing->state == rt::PreparationState::kDownloaded)
      started = c.service->Prepare(existing->install_id);
    else if (!existing->journal)
      return "The preparation record has no journal to continue.";
    // Re-ensure the staged title (idempotent for a matching title; it also
    // repairs directory modes from earlier builds) before resuming.
    if (existing->state == rt::PreparationState::kPreparing || existing->state == rt::PreparationState::kRegistering) {
      // Only when the next step can run: each step stops while the title may run, and a
      // folder rebuilt here (one the player deleted, say) would put the title back on Home.
      const auto activity = c.coordinator->Activity(existing->title_id);
      if (activity == rt::NativeActivity::kUnknown) return kActivityUnknown;
      if (activity != rt::NativeActivity::kStopped) return "Close the game first.";
      const auto staged = c.installer->StageTitle(*existing);
      Log("resume title check state=" + std::to_string(static_cast<int>(staged.state)) + " code=" + staged.code +
          " message=" + staged.message);
    }
    // Otherwise this is an explicit resume of the saved journal: the service
    // routes it to the saved request or frozen operation, never a new plan, and
    // never resubmits an uncertain registration.
  } else {
    std::string reason;
    const rt::GameRecipe* recipe = RecipeForPrepare(c, request, &reason);
    if (!recipe) return reason;
    // Playden ships its recipes (runtime/recipes) and the kit carries a copy of one of them;
    // only that one must agree with the kit, as the kit's shell was packaged for it.
    if (recipe->id == c.kit->view.recipe.id && std::to_string(recipe->version) != c.kit->view.recipe.version)
      return "The installed runtime kit was built for another version of this game's setup.";
    if (const auto lacks = KitLacksForRecipe(*recipe, c.kit->view); !lacks.empty()) return lacks;
    if (recipe->launch_entries.empty()) return "Steam lists no Windows program this console can start for this game.";
    const auto settings = config_.settings_record ? config_.settings_record(GameOf(request)) : std::nullopt;
    // The title is made for the launch option the player chose, so its publication matches.
    rt::LaunchEntry launch = recipe->launch_entries.front();
    if (settings) {
      const rt::SettingsPreset* preset = nullptr;
      for (const auto& candidate : c.presets)
        if (candidate.id == settings->preset_id) preset = &candidate;
      rt::EffectiveSettings effective;
      rt::SettingsError ignored;
      if (rt::BuildEffectiveSettings(*recipe, preset, settings->overrides, rt::KitCapabilitiesFromManifest(c.kit->capabilities, true),
                                     &effective, &ignored) && effective.launch)
        launch = *effective.launch;
      // A program the player picked is checked on disk: it must be there, built as the sheet says.
      if (effective.launch && effective.launch->id == rt::kProgramEntryId) {
        const auto root = HostPath(request.install_path).string();
        const auto found = rt::GameProgramArchitecture(root, launch.executable);
        if (found.empty()) return "The program " + launch.executable + " chosen in Game settings is missing or not a Windows program.";
        if (found != effective.architecture)
          return "The program " + launch.executable + " is " + (found == "pe64" ? "64-bit" : "32-bit") +
                 " but Game settings says otherwise; choose it again in Game settings.";
        // "Don't start" holds for the picked program too, even when another launch option made the recipe.
        const auto refusal = rt::GameProgramRefusal(root, launch.executable, rt::KitCapabilitiesFromManifest(c.kit->capabilities, true),
                                                    rt::RecipeChoicesFrom(*settings).try_anyway);
        if (!refusal.empty()) return refusal;
      }
    }
    if (const auto refused = tighten_working(WorkingFolder(launch.working_directory, launch.executable, launch.in_install_root));
        !refused.empty())
      return refused;
    rt::ReserveRequest reserve;
    // Steam: "steam-<app>" and "playden-steam-<app>", as before.
    const auto game = GameOf(request.game, request.runtime_number);
    reserve.install_id = ToInstallId(game);
    reserve.runtime_number = request.runtime_number;
    reserve.game = game;
    reserve.default_profile_id = ProfileId(game);
    reserve.runtime_bundle_id = c.kit->view.runtime.bundle_id;
    reserve.recipe_id = recipe->id;
    reserve.recipe_version = std::to_string(recipe->version);
    reserve.settings_revision = settings ? settings->revision : 0;
    reserve.launch_entry_id = launch.id;
    reserve.launch_path = launch.executable;
    reserve.launch_working_directory = launch.working_directory;
    reserve.launch_in_install_root = launch.in_install_root;
    reserve.install_path = request.install_path;
    reserve.volume_id = request.volume_id;
    // Titles removed during this run go last: a kit with fixed titles still gets one back.
    std::vector<std::string> retired;
    for (auto& title : rt::KitTitleCandidates(c.kit->view)) {
      if (c.blocked_leftover_titles.count(title)) continue;
      (c.retired_titles.count(title) ? retired : reserve.kit_title_variants).push_back(std::move(title));
    }
    reserve.kit_title_variants.insert(reserve.kit_title_variants.end(), retired.begin(), retired.end());
    auto reserved = c.repository.Reserve(reserve);
    if (!reserved.ok()) {
      if (reserved.error() != rt::RepositoryError::kTitleVariantsExhausted)
        return "A PS5 title could not be reserved: " + reserved.message();
      // Every ID refused at once usually means one source cannot answer, not that every ID is taken:
      // ask the IDs no other game holds what each source says, until one is not clearly in use.
      std::set<std::string> held;
      for (const auto& install : c.repository.Snapshot()) held.insert(install.title_id);
      const auto log_prefix = "title reservation refused app=" + std::to_string(request.runtime_number) +
                              " candidates=" + std::to_string(reserve.kit_title_variants.size()) +
                              " held=" + std::to_string(held.size());
      std::size_t occupied = 0;
      std::string first_occupied;
      for (const auto& title : reserve.kit_title_variants) {
        if (held.count(title)) continue;
        const auto report = c.coordinator->ReportAvailability(title);
        const bool unknown = report.database == rt::TitleAvailability::kUnknown ||
                             report.folder == rt::TitleAvailability::kUnknown ||
                             report.shadowmount.state == ps5::RegistrationState::kUnavailable ||
                             report.activity == rt::NativeActivity::kUnknown;
        if (!unknown) {
          if (!occupied++) first_occupied = title + " " + report.Text();
          continue;
        }
        Log(log_prefix + " occupied=" + std::to_string(occupied) + " " + title + " " + report.Text());
        // Only a ShadowMountPlus that does not answer at all is fixed by loading it again.
        if (report.shadowmount.reason == "no-response")
          return "ShadowMountPlus isn't answering. Load it again (or restart the PS5 and jailbreak it again), then try again.";
        return "Playden could not check that a PS5 title is free for this game. Try again; if it still fails, "
               "open Logs in Settings for details.";
      }
      Log(log_prefix + " occupied=" + std::to_string(occupied) + (first_occupied.empty() ? "" : " first=" + first_occupied));
      return "Every PS5 title this runtime kit offers is in use. Uninstall a game, then try again.";
    }
    auto record = reserved.value();
    // Before the title exists: it may only start once its helper lists it.
    Log("prepare reserved app=" + std::to_string(request.runtime_number) + " title=" + record.title_id + " ms=" + elapsed_ms());
    if (const auto listed = SyncTitleAllowList(c); !listed.empty()) return listed;
    if (const auto initialized = initialize_reserved(record); !initialized.empty()) return initialized;
    Log("prepare metadata app=" + std::to_string(request.runtime_number) + " ms=" + elapsed_ms());
    PublishInstalls(c);
    started = c.service->Prepare(record.install_id);
  }
  PublishInstalls(c);
  Log("prepare start app=" + std::to_string(request.runtime_number) + " root=" + request.install_path +
      " ok=" + std::to_string(started.ok()) + " ms=" + elapsed_ms() + " message=" + started.message);
  if (!started.ok()) return started.message;
  const auto install_id = existing ? existing->install_id : ToInstallId(GameOf(request.game, request.runtime_number));
  bool checked_after_submit = false;
  for (int steps = 0; steps < kMaxPreparationSteps && !Stopping(); ++steps) {
    const auto current = c.records.Find(install_id);
    if (!current) return "The preparation record disappeared.";
    if (current->state == rt::PreparationState::kReady) {
      OpenGameFolders(c, *current);
      return {};
    }
    if (current->state != rt::PreparationState::kPreparing && current->state != rt::PreparationState::kRegistering)
      return current->last_failure ? current->last_failure->message : "Preparation stopped.";
    const auto before = current->journal ? current->journal->step : std::string{};
    const auto step_began = std::chrono::steady_clock::now();
    const auto step = c.service->Step(install_id);
    const auto step_ended = std::chrono::steady_clock::now();
    ClearWork();  // a step's progress ends with it
    PublishInstalls(c);
    const auto ms = [](auto from, auto to) {
      return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(to - from).count());
    };
    const auto after_step = c.records.Find(install_id);
    Log("prepare step app=" + std::to_string(request.runtime_number) + " from=" + before +
        " step_ms=" + ms(step_began, step_ended) + " publish_ms=" + ms(step_ended, std::chrono::steady_clock::now()) +
        " ok=" + std::to_string(step.ok()) + " error=" + std::to_string(static_cast<int>(step.error())) +
        " state=" + (after_step ? std::to_string(static_cast<int>(after_step->state)) : std::string("-")) +
        " to=" + (after_step && after_step->journal ? after_step->journal->step : std::string{}) +
        " code=" + (after_step && after_step->last_failure ? after_step->last_failure->code : std::string("-")) +
        " message=" + step.message);
    // A submitted registration is not polled: ShadowMountPlus registers titles only
    // while no title runs, Playden included. The console is asked once more right
    // away (a title may already be registered); otherwise preparation ends here and
    // the game is ready to play once the player leaves Playden. Later checks only
    // query the console and never resubmit an armed registration.
    const bool pending = !step.ok() && step.error() == rt::InstallServiceError::kRegistrationPending;
    if (!step.ok() && !pending) return step.message;
    const auto after = c.records.Find(install_id);
    if (pending || (after && after->journal && after->journal->step == "await-native")) {
      if (pending || checked_after_submit) {
        if (awaiting) *awaiting = true;
        return {};
      }
      checked_after_submit = true;
    }
  }
  return Stopping() ? "Preparation was interrupted." : "Preparation did not finish.";
}

namespace {
// Kept bottles: <games>/<install id>.kept, identified by this marker inside.
constexpr char kKeptMarker[] = "playden-kept.json";
constexpr char kKeptSuffix[] = ".kept";
// In a bottle once its kept saves were brought back: a kept copy still beside it is then older.
constexpr char kRestoredMarker[] = ".playden-saves-restored";
// Save backups: <games>/<install id>.saves/<Unix time>/users, the newest few kept.
constexpr char kSaveBackupSuffix[] = ".saves";
constexpr std::size_t kSaveBackupsKept = 3;

// "steam-<app id>" or "local-<uuid>": the install IDs a kept bottle may carry.
bool KeptInstallId(const std::string& id) { return ParseInstallId(id).has_value(); }

// Deletes a tree with lstat/unlink/rmdir: links are removed as links and never
// followed (Wine prefixes link z: to /). std::filesystem::remove_all crashed the
// console title on a retired title folder, so it is not used here. A missing root
// is not an error; the first failure is reported and the walk continues.
// `removed`, when given, is called after each entry the walk deletes (or fails to).
void RemoveTreeNoFollow(const std::string& path, std::error_code& error,
                        const std::function<void()>* removed = nullptr) {
  struct stat info{};
  if (lstat(path.c_str(), &info) != 0) {
    if (errno != ENOENT && !error) error = std::error_code(errno, std::generic_category());
    return;
  }
  if (S_ISDIR(info.st_mode)) {
    if (DIR* directory = opendir(path.c_str())) {
      while (const dirent* entry = readdir(directory)) {
        const std::string name = entry->d_name;
        if (name == "." || name == "..") continue;
        RemoveTreeNoFollow(path + "/" + name, error, removed);
      }
      closedir(directory);
    } else if (!error) {
      error = std::error_code(errno, std::generic_category());
    }
    if (rmdir(path.c_str()) != 0 && !error) error = std::error_code(errno, std::generic_category());
    if (removed) (*removed)();
    return;
  }
  if (unlink(path.c_str()) != 0 && !error) error = std::error_code(errno, std::generic_category());
  if (removed) (*removed)();
}
void RemoveTreeNoFollow(const std::filesystem::path& path, std::error_code& error,
                        const std::function<void()>* removed = nullptr) {
  error.clear();
  RemoveTreeNoFollow(path.string(), error, removed);
}

// Bytes of the regular files under a tree; links are not followed.
std::uint64_t SizeTreeNoFollow(const std::string& path, int depth = 0) {
  struct stat info{};
  if (depth > 24 || lstat(path.c_str(), &info) != 0) return 0;
  if (S_ISREG(info.st_mode)) return static_cast<std::uint64_t>(info.st_size);
  if (!S_ISDIR(info.st_mode)) return 0;
  std::uint64_t total = 0;
  if (DIR* directory = opendir(path.c_str())) {
    while (const dirent* entry = readdir(directory)) {
      const std::string name = entry->d_name;
      if (name != "." && name != "..") total += SizeTreeNoFollow(path + "/" + name, depth + 1);
    }
    closedir(directory);
  }
  return total;
}

// Entries under a tree, the root included, for deletion progress; links are not followed.
std::uint64_t CountTreeNoFollow(const std::string& path) {
  struct stat info{};
  if (lstat(path.c_str(), &info) != 0) return 0;
  std::uint64_t count = 1;
  if (S_ISDIR(info.st_mode))
    if (DIR* directory = opendir(path.c_str())) {
      while (const dirent* entry = readdir(directory)) {
        const std::string name = entry->d_name;
        if (name != "." && name != "..") count += CountTreeNoFollow(path + "/" + name);
      }
      closedir(directory);
    }
  return count;
}

// Bytes of the regular files under a tree; links are not followed.
std::uint64_t TreeBytes(const std::filesystem::path& root) {
  std::uint64_t total = 0;
  std::error_code error;
  std::size_t entries = 0;
  for (std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, error), end;
       !error && it != end && ++entries < 500000; it.increment(error)) {
    std::error_code ignored;
    if (it->is_symlink(ignored)) { it.disable_recursion_pending(); continue; }
    if (it->is_regular_file(ignored)) total += it->file_size(ignored);
  }
  return total;
}

// The game's own name: a PS5 title name may carry its store, "Half-Life 2 (Steam)" or "(GOG)", "(Epic)".
// Imported games carry none.
std::string WithoutStoreSuffix(std::string name) {
  for (const auto store : {Store::kSteam, Store::kGog, Store::kEpic}) {
    const auto suffix = " (" + std::string(Traits(store).name) + ")";
    if (name.size() > suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
      name.resize(name.size() - suffix.size());
      break;
    }
  }
  return name;
}

std::optional<KeptBottleView> ReadKeptMarker(const std::filesystem::path& kept, const std::string& install_id) {
  const int fd = open((kept / kKeptMarker).c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) return std::nullopt;
  std::string bytes(4096, '\0');
  const auto count = read(fd, bytes.data(), bytes.size());
  close(fd);
  if (count <= 0) return std::nullopt;
  bytes.resize(static_cast<std::size_t>(count));
  const auto json = playden::steam::json::Parse(bytes, {4096, 4, 64, 4096});
  if (!json) return std::nullopt;
  const auto* app = json->Find("app_id");
  const auto* name = json->Find("name");
  const auto* kept_at = json->Find("kept_at");
  const auto* size = json->Find("bytes");
  const auto* install = json->Find("install_id");
  // Written for games from other stores; absent means the Steam game app_id names.
  const auto* store = json->Find("store");
  const auto* game = json->Find("game");
  if (!app || !app->is_number() || !install || !install->is_string() || install->text() != install_id) return std::nullopt;
  if ((store == nullptr) != (game == nullptr) || (store && (!store->is_string() || !game->is_string()))) return std::nullopt;
  KeptBottleView view;
  view.install_id = install_id;
  view.runtime_number = static_cast<std::uint32_t>(std::strtoul(app->text().c_str(), nullptr, 10));
  if (name && name->is_string()) view.name = WithoutStoreSuffix(name->text());  // markers written before the name was stripped
  if (kept_at && kept_at->is_number()) view.kept_at = std::strtoll(kept_at->text().c_str(), nullptr, 10);
  if (size && size->is_number()) view.bytes = std::strtoull(size->text().c_str(), nullptr, 10);
  if (store) {
    const auto parsed = ParseStoreKey(store->text());
    if (!parsed) return std::nullopt;
    view.game = GameId{*parsed, game->text()};
    if (!view.game.valid() || ToInstallId(view.game) != install_id || view.runtime_number < rt::kFirstRuntimeNumber ||
        view.runtime_number > rt::kLastRuntimeNumber)
      return std::nullopt;
  } else {
    view.game = GameId::Steam(view.runtime_number);
    if (!view.game.valid() || ToInstallId(view.game) != install_id) return std::nullopt;
  }
  return view;
}

// Copies a saves tree into a private bottle: directories 0700, files keep their
// owner bits, links are skipped (Wine links point outside the bottle).
// Files with another link are skipped (logged): their content may be another file's (a private
// one linked into the profile), which the copy would make the profile's own.
bool CopySavesTree(const std::filesystem::path& from, const std::filesystem::path& to, std::string* error,
                   const std::function<void(const std::string&)>& log) {
  struct stat info{};
  if (lstat(to.c_str(), &info) != 0) {
    if (mkdir(to.c_str(), 0700) != 0) { *error = "cannot create " + to.filename().string(); return false; }
  } else if (!S_ISDIR(info.st_mode)) {
    *error = to.filename().string() + " is not a folder"; return false;
  }
  std::error_code iteration;
  for (std::filesystem::directory_iterator it(from, iteration), end; !iteration && it != end; it.increment(iteration)) {
    const auto source = it->path();
    const auto target = to / source.filename();
    if (lstat(source.c_str(), &info) != 0) continue;
    if (S_ISLNK(info.st_mode)) continue;
    if (S_ISDIR(info.st_mode)) {
      if (!CopySavesTree(source, target, error, log)) return false;
      continue;
    }
    if (!S_ISREG(info.st_mode)) continue;
    if (info.st_nlink != 1) {
      log("saves copy skips " + source.string() + ": it has " + std::to_string(info.st_nlink) + " links");
      continue;
    }
    const int input = open(source.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    struct stat opened{};
    if (input >= 0 && (fstat(input, &opened) != 0 || opened.st_ino != info.st_ino || opened.st_dev != info.st_dev ||
                       opened.st_nlink != 1)) {
      close(input);
      log("saves copy skips " + source.string() + ": it changed while being copied");
      continue;
    }
    const auto temporary = target.parent_path() / ("." + target.filename().string() + ".restore");
    unlink(temporary.c_str());
    const int output = open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                            (info.st_mode & 0700) | 0600);
    bool ok = input >= 0 && output >= 0;
    std::vector<char> buffer(64 * 1024);  // heap: console worker stacks are small
    while (ok) {
      const auto count = read(input, buffer.data(), buffer.size());
      if (count < 0 && errno == EINTR) continue;
      if (count <= 0) { ok = count == 0; break; }
      for (ssize_t offset = 0; ok && offset < count;) {
        const auto written = write(output, buffer.data() + offset, static_cast<std::size_t>(count - offset));
        if (written < 0 && errno == EINTR) continue;
        ok = written > 0; offset += written > 0 ? written : 0;
      }
    }
    ok = ok && fsync(output) == 0;
    if (input >= 0) close(input);
    if (output >= 0) ok = close(output) == 0 && ok;
    if (!ok || rename(temporary.c_str(), target.c_str()) != 0) {
      unlink(temporary.c_str());
      *error = "cannot copy " + source.filename().string();
      return false;
    }
  }
  if (iteration) { *error = "cannot list " + from.filename().string(); return false; }
  return true;
}
}  // namespace

std::string RuntimeHost::MaintenanceRefusal(Composition& c, const rt::RuntimeInstall& record) {
  if (record.state != rt::PreparationState::kReady) return "Prepare the game first.";
  if (record.journal) return "Finish the PS5 title's recovery first.";
  if (!c.launch_journal) return "Play is unavailable.";
  const auto pending = c.launch_journal->LoadPending();
  if (!pending.ok) return "The launch record cannot be read; restart Playden.";
  if (pending.session) return "Close the game first.";
  const auto activity = c.coordinator->Activity(record.title_id);
  if (activity == rt::NativeActivity::kUnknown) return kActivityUnknown;
  if (activity != rt::NativeActivity::kStopped) return "Close the game first.";
  return {};
}

std::filesystem::path RuntimeHost::SaveBackupRoot(const std::string& install_id) const {
  return HostPath(config_.package_paths.console_data_root + "/playden/games") / (install_id + kSaveBackupSuffix);
}

std::string RuntimeHost::RunMaintenance(Composition& c, std::uint32_t app, RuntimeMaintenance action, bool* ok) {
  *ok = false;
  std::optional<rt::RuntimeInstall> record;
  for (const auto& candidate : c.repository.Snapshot())
    if (candidate.runtime_number == app) record = candidate;
  if (!record) return "This game has no PS5 title yet.";
  if (const auto replaced = KitReplaced(c); !replaced.empty()) return replaced;
  if (const auto refused = MaintenanceRefusal(c, *record); !refused.empty()) {
    // Settings saved while the game runs reach it on the next Play from Playden.
    if (action == RuntimeMaintenance::kApplySettings) return "Changes apply the next time Playden starts the game.";
    return refused;
  }
  switch (action) {
    case RuntimeMaintenance::kApplySettings: {
      bool published = false;
      auto refused = RepublishSettings(c, *record, &published);
      if (!refused.empty()) return refused;
      *ok = true;
      return published ? "Settings applied." : "Settings are up to date.";
    }
    case RuntimeMaintenance::kClearShaderCache: {
      rt::LaunchEntry launch{record->launch_entry_id, {}, record->launch_path, record->launch_working_directory, {},
                             record->launch_in_install_root};
      std::string reason;
      if (!rt::ClearShaderCache(launch, HostPath(record->install_path).string(), &reason))
        return "The shader cache could not be cleared: " + reason + ".";
      *ok = true;
      return "Shader cache cleared. Shaders compile again on the next start.";
    }
    case RuntimeMaintenance::kRepair: {
      // An owned publication with a kit that admits unowned folders fails the start check once
      // the player opened the folder; it is published again as unowned, just below.
      const bool unowned_upgrade = NeedsUnownedPublication(c, *record);
      const auto working = WorkingFolder(record->launch_working_directory, record->launch_path, record->launch_in_install_root);
      if (const auto unwritable = EnsureWritable(record->install_path, working); !unwritable.empty())
        return unwritable;
      if (const auto unlinked = UnlinkDxvkConfig(*record); !unlinked.empty()) return unlinked;
      const auto repaired = c.installer->RepairBottle(*record);
      const auto admission = c.installer->AdmissionDiagnostic(*record);
      c.admission[record->install_id] = admission;
      Log("maintenance repair app=" + std::to_string(app) + " state=" + std::to_string(static_cast<int>(repaired.state)) +
          " code=" + repaired.code + " admission=" + admission);
      PublishInstalls(c, record->install_id);
      if (repaired.state != rt::PackageActionState::kComplete) return "The bottle could not be repaired: " + repaired.message;
      // A game whose folder moved fails the check until it is published again, just below.
      if (admission != "ok" && record->moved_from.empty() && !unowned_upgrade)
        return "The PS5 title still fails its start check: " + admission;
      std::string deferred;
      if (auto refused = RepublishSettings(c, *record, nullptr, true, nullptr, &deferred); !refused.empty())
        return refused;
      if (!record->moved_from.empty() || unowned_upgrade)
        if (const auto current = c.records.Find(record->install_id))
          if (const auto checked = c.installer->AdmissionDiagnostic(*current); checked != "ok")
            return (deferred.empty() ? std::string() : "The game folder could not be published for mods: " + deferred + " ") +
                   "The PS5 title still fails its start check: " + checked;
      *ok = true;
      if (const auto current = c.records.Find(record->install_id)) OpenGameFolders(c, *current);
      if (!deferred.empty()) return "Repaired, but the game folder could not be published for mods: " + deferred;
      if (!c.installer->TitleCurrent(*record) || !c.installer->TitleMetadataCurrent(*record))
        return "Repaired. The PS5 title also has an update: press Update on the game page.";
      return "Repaired. The game's start files and settings are in place.";
    }
    case RuntimeMaintenance::kBackupSaves: {
      const auto users = HostPath(record->data_root) / "library/prefixes/game/drive_c/users";
      struct stat info{};
      if (lstat(users.c_str(), &info) != 0 || !S_ISDIR(info.st_mode)) return "The game has no Windows user folders to back up.";
      const auto root = SaveBackupRoot(record->install_id);
      if (lstat(root.c_str(), &info) != 0 && mkdir(root.c_str(), 0700) != 0) return "The backup folder could not be made.";
      const auto stamp = std::to_string(config_.unix_time());
      const auto staging = root / ("." + stamp + ".partial");
      const auto target = root / stamp;
      std::error_code error;
      RemoveTreeNoFollow(staging, error);
      if (mkdir(staging.c_str(), 0700) != 0) return "The backup folder could not be made.";
      std::string reason;
      if (!CopySavesTree(users, staging / "users", &reason, [this](const std::string& line) { Log(line); }) ||
          rename(staging.c_str(), target.c_str()) != 0) {
        RemoveTreeNoFollow(staging, error);
        Log("maintenance backup failed app=" + std::to_string(app) + " " + reason);
        return "The saves could not be backed up" + (reason.empty() ? std::string(".") : ": " + reason + ".");
      }
      // Keep the newest few; older ones are removed (and so is any partial copy).
      std::vector<std::pair<long long, std::filesystem::path>> backups;
      for (std::filesystem::directory_iterator it(root, error), end; !error && it != end; it.increment(error)) {
        const auto name = it->path().filename().string();
        if (!name.empty() && name.front() == '.') { std::error_code ignored; RemoveTreeNoFollow(it->path(), ignored); continue; }
        backups.emplace_back(std::strtoll(name.c_str(), nullptr, 10), it->path());
      }
      std::sort(backups.begin(), backups.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
      for (std::size_t index = kSaveBackupsKept; index < backups.size(); ++index) {
        std::error_code ignored;
        RemoveTreeNoFollow(backups[index].second, ignored);
      }
      Log("maintenance backup app=" + std::to_string(app) + " stamp=" + stamp);
      PublishInstalls(c, record->install_id);
      *ok = true;
      return "Saves backed up. Playden keeps the newest " + std::to_string(kSaveBackupsKept) + " backups.";
    }
  }
  return {};
}

std::filesystem::path RuntimeHost::HostPath(const std::string& console_path) const {
  return config_.host_namespace_prefix.empty()
      ? std::filesystem::path(console_path)
      : config_.host_namespace_prefix / std::filesystem::path(console_path).relative_path();
}

std::string RuntimeHost::SyncTitleAllowList(Composition& c) {
  if (!c.kit || std::none_of(c.kit->view.variants.begin(), c.kit->view.variants.end(),
                             [](const rt::KitTitleVariant& variant) { return variant.any_title; }))
    return {};
  if (!config_.write_title_allow_list) return "This console can't let new PS5 titles start.";
  std::set<std::string> titles;
  for (const auto& record : c.repository.Snapshot())
    if (rt::KitVariantFor(c.kit->view, record.title_id)) titles.insert(record.title_id);
  std::string contents;
  for (const auto& title : titles) contents += title + "\n";
  std::string error;
  if (!config_.write_title_allow_list(contents, &error)) {
    Log("title allow-list write failed titles=" + std::to_string(titles.size()) + " error=" + error);
    return "The list of PS5 titles allowed to start could not be written: " + error;
  }
  Log("title allow-list titles=" + std::to_string(titles.size()));
  return {};
}

bool RuntimeHost::TitleListed(Composition& c, const std::string& title_id) {
  if (c.coordinator->Inventory(title_id) != rt::TitleAvailability::kFree) return true;
  struct stat info{};
  return lstat(HostPath("/user/app/" + title_id).c_str(), &info) == 0;
}

void RuntimeHost::PublishKeptBottles(Composition& c) {
  std::set<std::string> recorded;
  for (const auto& record : c.repository.Snapshot()) recorded.insert(record.install_id);
  std::vector<KeptBottleView> kept;
  const auto games = HostPath(config_.package_paths.console_data_root + "/playden/games");
  std::error_code error;
  for (std::filesystem::directory_iterator it(games, error), end; !error && it != end; it.increment(error)) {
    const auto name = it->path().filename().string();
    const std::size_t suffix = std::strlen(kKeptSuffix);
    if (name.size() <= suffix || name.compare(name.size() - suffix, suffix, kKeptSuffix) != 0) continue;
    const auto install_id = name.substr(0, name.size() - suffix);
    if (!KeptInstallId(install_id) || recorded.count(install_id)) continue;
    std::error_code ignored;
    if (it->is_symlink(ignored) || !it->is_directory(ignored)) continue;
    if (auto view = ReadKeptMarker(it->path(), install_id)) {
      // A game from another store keeps its number: installed again, it gets the same one back.
      if (const auto remembered = c.repository.RememberNumber(view->game, view->runtime_number); !remembered.ok()) {
        Log("kept bottle skipped install=" + install_id + ": " + remembered.message());
        continue;
      }
      view->data_root = config_.package_paths.console_data_root + "/playden/games/" + name;
      kept.push_back(std::move(*view));
    }
  }
  for (const auto& view : kept) LearnNumber(view.game, view.runtime_number);
  std::sort(kept.begin(), kept.end(), [](const auto& a, const auto& b) { return a.kept_at > b.kept_at; });
  Publish([&](RuntimeSnapshot& s) { s.kept_bottles = std::move(kept); });
}

std::optional<RuntimeHost::RemovalFailure> RuntimeHost::RetireTitle(
    Composition& c, const std::string& title_id, const std::string& operation_id,
    const rt::LeftoverTitle* expected) {
  const auto source = config_.homebrew_root / title_id;
  // ShadowMount must no longer discover the source before the native title goes.
  const auto manual = c.registry.RemoveManualSource(title_id);
  Log("remove manual source title=" + title_id + " ok=" + std::to_string(manual.ok) + " stage=" + manual.stage);
  struct stat info{};
  if (lstat(source.c_str(), &info) != 0) {
    if (errno == ENOENT) return std::nullopt;
    return RemovalFailure{"source-unavailable", "The PS5 title's folder cannot be checked."};
  }
  if (!S_ISDIR(info.st_mode) || info.st_uid != geteuid())
    return RemovalFailure{"source-foreign", "The PS5 title's folder is not Playden's; it was left as it is."};
  if (expected && (expected->device != static_cast<std::uint64_t>(info.st_dev) ||
                   expected->inode != static_cast<std::uint64_t>(info.st_ino)))
    return RemovalFailure{"source-changed", "The PS5 title's folder changed since it was found; it was left as it is."};
  std::string reason;
  const auto trash = config_.state_root / "trash";
  if (!rt::EnsurePrivateDirectories(config_.boundary, config_.state_root, trash, &reason))
    return RemovalFailure{"trash-unavailable", "Playden could not prepare its private trash: " + reason};
  auto homebrew = platform::ManagedDirectory::Open(config_.homebrew_root, platform::DirectoryPolicy::kApprovedTree);
  auto destination = platform::ManagedDirectory::Open(trash, platform::DirectoryPolicy::kApprovedTree);
  struct stat checked{};
  if (!homebrew || !destination || !homebrew->InspectChild(title_id, &checked) ||
      checked.st_dev != info.st_dev || checked.st_ino != info.st_ino || checked.st_uid != info.st_uid ||
      !homebrew->RenameChild(title_id, *destination, title_id + "-" + operation_id, false) ||
      !homebrew->Sync() || !destination->Sync())
    return RemovalFailure{"retire-failed", "The PS5 title's folder could not be moved away (error " +
                                           std::to_string(errno) + ")."};
  Log("remove retired title=" + title_id);
  return std::nullopt;
}

std::optional<RuntimeHost::RemovalFailure> RuntimeHost::UnregisterTitle(
    Composition& c, const std::string& title_id, const std::string& operation_id) {
  bool listed = TitleListed(c, title_id);
  TitleRemoverResult result;
  if (listed) {
    if (!config_.unregister_title)
      return RemovalFailure{"remover-unavailable", "Removing PS5 titles is not available in this build."};
    result = config_.unregister_title(title_id, operation_id);
    Log("remove unregister title=" + title_id + " sent=" + std::to_string(result.sent) + " code=" +
        (result.code ? std::to_string(*result.code) : std::string("none")) + " detail=" + result.detail);
    for (int poll = 0; listed && poll < config_.removal_confirm_polls && !Stopping(); ++poll) {
      listed = TitleListed(c, title_id);
      if (!listed) break;
      std::unique_lock<std::mutex> lock(mutex_);
      wake_.wait_for(lock, config_.removal_poll_interval, [this] { return stopping_; });
    }
    if (listed) listed = TitleListed(c, title_id);
  }
  if (!listed) return std::nullopt;
  if (!result.sent)
    return RemovalFailure{"remover-unavailable", "Playden could not start its title remover. Retry to try again."};
  if (result.code && *result.code != 0) {
    char code[16]; std::snprintf(code, sizeof(code), "0x%08x", static_cast<unsigned>(*result.code));
    return RemovalFailure{"unregister-refused", std::string("The console refused to remove the PS5 title (") + code + ")."};
  }
  return RemovalFailure{"unregister-unconfirmed",
                        "The console has not confirmed that the PS5 title is gone. Retry checks again."};
}

std::string RuntimeHost::DeleteRetiredTitle(const std::string& title_id, const std::string& operation_id) {
  const auto trash = config_.state_root / "trash" / (title_id + "-" + operation_id);
  struct stat info{};
  if (lstat(trash.c_str(), &info) != 0 && errno == ENOENT) return {};
  auto parent = platform::ManagedDirectory::Open(trash.parent_path(), platform::DirectoryPolicy::kPrivate);
  if (!parent || !parent->InspectChild(trash.filename().string(), &info) ||
      !S_ISDIR(info.st_mode) || info.st_uid != geteuid())
    return "The retired PS5 title's folder is not Playden's; it was left as it is.";
  const auto total = CountTreeNoFollow(trash.string());
  std::uint64_t done = 0;
  const std::function<void()> removed = [&] { ReportWork(++done, total); };
  std::error_code error;
  RemoveTreeNoFollow(trash, error, &removed);
  ClearWork();
  return error ? "The PS5 title's folder could not be deleted: " + error.message() : std::string{};
}

namespace {
std::string LeftoverDeleteFile(const std::string& title) { return "leftover-" + title + ".json"; }

bool LeftoverOperationId(const std::string& operation) {
  if (operation.size() != 36) return false;
  for (std::size_t i = 0; i < operation.size(); ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) { if (operation[i] != '-') return false; }
    else if (!((operation[i] >= '0' && operation[i] <= '9') ||
               (operation[i] >= 'a' && operation[i] <= 'f'))) return false;
  }
  return true;
}
}  // namespace

std::string RuntimeHost::SaveLeftoverDelete(Composition& c, const LeftoverDelete& request) {
  const auto& title = request.title;
  const auto text = "{\"schema\":1,\"title\":\"" + title.title_id + "\",\"operation_id\":\"" + request.operation_id +
      "\",\"step\":\"" + request.step + "\",\"device\":" + std::to_string(title.device) +
      ",\"inode\":" + std::to_string(title.inode) + ",\"name\":\"" + JsonEscaped(title.name) +
      "\",\"bytes\":" + std::to_string(title.bytes) + "}\n";
  // Storage's durable writer uses ManagedDirectory and creates the journal 0600 before writing.
  c.blocked_leftover_titles.insert(title.title_id);
  const auto saved = c.storage.WriteDurable(LeftoverDeleteFile(title.title_id), Bytes(text.begin(), text.end()));
  if (!saved.ok()) {
    // A failed sync may follow a successful rename: only a fresh journal read after restart may
    // decide which step is durable. Never continue using the previous in-memory step.
    c.leftover_deletes.erase(title.title_id);
    return "The removal could not be recorded; restart Playden before retrying: " + saved.message();
  }
  c.leftover_deletes[title.title_id] = request;
  return {};
}

void RuntimeHost::PublishLeftoverTitles(Composition& c) {
  auto titles = rt::FindLeftoverTitles(config_.homebrew_root, c.repository.Loaded() && c.leftover_journals_known, c.repository.Snapshot(),
                                      [&c](const std::string& title) { return c.coordinator->Activity(title); }, geteuid());
  // A retired title still waiting for the remover remains available to retry, even though its
  // source has left the scan root. Its private journal reserves the ID until completion.
  if (c.repository.Loaded())
    for (const auto& [id, request] : c.leftover_deletes) {
      const auto records = c.repository.Snapshot();
      if (std::any_of(records.begin(), records.end(), [&](const auto& record) { return record.title_id == id; }) ||
          c.coordinator->Activity(id) != rt::NativeActivity::kStopped) continue;
      if (std::none_of(titles.begin(), titles.end(), [&](const auto& title) { return title.title_id == id; }))
        titles.push_back(request.title);
    }
  std::sort(titles.begin(), titles.end(), [](const auto& a, const auto& b) { return a.title_id < b.title_id; });
  std::string ids;
  for (const auto& title : titles) ids += " " + title.title_id;
  Log("leftover titles found=" + std::to_string(titles.size()) + ids);
  Publish([&](RuntimeSnapshot& s) { s.leftover_titles = std::move(titles); });
}

std::string RuntimeHost::RunDeleteLeftover(Composition& c, LeftoverDelete request) {
  const auto id = request.title.title_id;
  const auto fail = [&](const std::string& message) {
    Log("leftover delete failed title=" + id + " step=" + request.step + " " + message);
    return message;
  };
  if (!c.leftover_journals_known) return fail("The PS5 title removal records cannot be read; restart Playden before retrying.");
  if (!c.repository.Loaded()) {
    const auto loaded = c.repository.Load();
    PublishInstalls(c);
    if (!loaded.ok()) return fail("Playden's records of PS5 titles cannot be read: " + loaded.message() + ".");
  }
  for (const auto& record : c.repository.Snapshot())
    if (record.title_id == id) return fail("This PS5 title now belongs to an installed game; it was left as it is.");
  if (const auto pending = c.leftover_deletes.find(id); pending != c.leftover_deletes.end()) request = pending->second;
  else if (c.blocked_leftover_titles.count(id))
    return fail("The removal record cannot be read; restart Playden before retrying.");
  auto homebrew = platform::ManagedDirectory::Open(config_.homebrew_root, platform::DirectoryPolicy::kApprovedTree);
  if (!homebrew) return fail("The PS5 title's folder cannot be checked.");
  const auto activity = c.coordinator->Activity(id);
  if (activity == rt::NativeActivity::kActive) return fail("Close the game before uninstalling it.");
  if (activity != rt::NativeActivity::kStopped) return fail(kActivityUnknown);
  const auto source = config_.homebrew_root / id;
  struct stat info{};
  if (lstat(source.c_str(), &info) == 0) {
    if (!S_ISDIR(info.st_mode) || info.st_uid != geteuid())
      return fail("The PS5 title's folder is not Playden's; it was left as it is.");
    if (request.step != "retire" || request.title.device != static_cast<std::uint64_t>(info.st_dev) ||
        request.title.inode != static_cast<std::uint64_t>(info.st_ino))
      return fail("The PS5 title's folder changed since it was found; it was left as it is.");
  } else if (errno != ENOENT) return fail("The PS5 title's folder cannot be checked.");
  else if (request.operation_id.empty()) return fail("This leftover PS5 title is no longer available.");
  if (!request.operation_id.empty()) {
    const auto trash = config_.state_root / "trash";
    const auto retired = trash / (id + "-" + request.operation_id);
    if (lstat(retired.c_str(), &info) == 0) {
      const auto parent = platform::ManagedDirectory::Open(trash, platform::DirectoryPolicy::kPrivate);
      if (!parent || !S_ISDIR(info.st_mode) || info.st_uid != geteuid() ||
          request.title.device != static_cast<std::uint64_t>(info.st_dev) ||
          request.title.inode != static_cast<std::uint64_t>(info.st_ino))
        return fail("The retired PS5 title's folder changed; it was left as it is.");
    } else if (errno != ENOENT) return fail("The retired PS5 title's folder cannot be checked.");
  }
  // Check the inventory before retiring: an unavailable remover must leave a listed title's source intact.
  if (TitleListed(c, id) && !config_.unregister_title)
    return fail("Removing PS5 titles is not available in this build.");
  if (request.operation_id.empty()) {
    const auto operation = config_.uuid();
    if (!operation || !LeftoverOperationId(*operation)) return fail("No secure random source for the removal.");
    request.operation_id = *operation;
    if (const auto error = SaveLeftoverDelete(c, request); !error.empty()) return fail(error);
  }
  if (request.step == "retire") {
    Publish([](RuntimeSnapshot& s) { s.deleting_leftover_step = "Removing PS5 title…"; });
    if (const auto error = RetireTitle(c, id, request.operation_id, &request.title)) return fail(error->message);
    request.step = "unregister";
    if (const auto error = SaveLeftoverDelete(c, request); !error.empty()) return fail(error);
  }
  if (request.step == "unregister") {
    Publish([](RuntimeSnapshot& s) { s.deleting_leftover_step = "Removing PS5 title…"; });
    if (Stopping()) return fail("The removal will continue when Playden starts again.");
    if (const auto error = UnregisterTitle(c, id, request.operation_id)) return fail(error->message);
    request.step = "trash";
    if (const auto error = SaveLeftoverDelete(c, request); !error.empty()) return fail(error);
  }
  // A restart or a retry still checks the native inventory before clearing the durable journal.
  if (TitleListed(c, id)) return fail("The console has not confirmed that the PS5 title is gone. Retry checks again.");
  Publish([](RuntimeSnapshot& s) { s.deleting_leftover_step = "Deleting title files…"; });
  if (const auto error = DeleteRetiredTitle(id, request.operation_id); !error.empty()) return fail(error);
  const auto cleared = c.storage.RemoveDurable(LeftoverDeleteFile(id));
  if (!cleared.ok()) {
    c.leftover_deletes.erase(id);
    return fail("The removal could not be completed; restart Playden before retrying: " + cleared.message());
  }
  c.leftover_deletes.erase(id);
  c.blocked_leftover_titles.erase(id);
  c.retired_titles.insert(id);
  Log("leftover delete complete title=" + id);
  return {};
}

void RuntimeHost::ResumeLeftoverDeletes(Composition& c) {
  // Every entry named like a journal counts, whatever it is: one that cannot be inspected still
  // reserves its title. A listing that stops early leaves the journals unknown.
  std::vector<std::string> names;
  DIR* directory = opendir(config_.state_root.c_str());
  if (!directory && errno != ENOENT) {
    c.leftover_journals_known = false;
    Log("leftover removal journals unavailable: errno " + std::to_string(errno));
    return;
  }
  if (directory) {
    for (;;) {
      errno = 0;
      const dirent* entry = readdir(directory);
      if (!entry) break;
      const std::string name = entry->d_name;
      if (name.rfind("leftover-", 0) == 0) names.push_back(name);
    }
    const int listing_error = errno;
    closedir(directory);
    if (listing_error != 0) {
      c.leftover_journals_known = false;
      Log("leftover removal journals unavailable: errno " + std::to_string(listing_error));
      return;
    }
  }
  for (const auto& entry_name : names) {
    // A journal is written before the removal changes anything, so a half-written one alone (.tmp) holds nothing.
    if (entry_name.size() > 4 && entry_name.compare(entry_name.size() - 4, 4, ".tmp") == 0) continue;
    const auto id = entry_name.size() == 23 && entry_name.substr(18) == ".json" ? entry_name.substr(9, 9) : std::string();
    if (!rt::IsManagedTitle(id)) {
      Log("leftover removal journal name not understood: " + entry_name);
      continue;
    }
    c.blocked_leftover_titles.insert(id);
    const auto directory = platform::ManagedDirectory::Open(config_.state_root, platform::DirectoryPolicy::kPrivate);
    struct stat journal_info{};
    if (!directory || !directory->InspectChild(entry_name, &journal_info) || !S_ISREG(journal_info.st_mode) ||
        journal_info.st_uid != geteuid() || (journal_info.st_mode & 07777) != 0600) {
      Log("leftover removal journal permissions invalid title=" + id); continue;
    }
    const auto bytes = c.storage.ReadDurable(entry_name, 8192);
    const auto json = bytes.ok() ? steam::json::Parse(std::string(bytes.value().begin(), bytes.value().end())) : std::nullopt;
    if (!json || !json->is_object()) { Log("leftover removal journal invalid title=" + id); continue; }
    const auto schema = json->Find("schema"), title = json->Find("title"), operation = json->Find("operation_id"),
               step = json->Find("step"), device = json->Find("device"), inode = json->Find("inode"),
               name = json->Find("name"), size = json->Find("bytes");
    if (json->object().size() != 8 || !schema || schema->AsUint64() != 1 || !title || !title->is_string() ||
        title->text() != id || !operation || !operation->is_string() || !LeftoverOperationId(operation->text()) ||
        !step || !step->is_string() || (step->text() != "retire" && step->text() != "unregister" && step->text() != "trash") ||
        !device || !device->AsUint64() || !inode || !inode->AsUint64() || !name || !name->is_string() ||
        name->text().size() > 4096 || !size || !size->AsUint64()) {
      Log("leftover removal journal invalid title=" + id); continue;
    }
    LeftoverDelete request;
    request.title = {id, config_.homebrew_root / id, name->text(), *size->AsUint64(), *device->AsUint64(), *inode->AsUint64()};
    request.operation_id = operation->text();
    request.step = step->text();
    c.leftover_deletes[id] = request;
  }
  // Each journal is reconciled before startup's trash housekeeping; failed ones retain their folder.
  const auto pending = c.leftover_deletes;
  for (const auto& [id, request] : pending) {
    Publish([&](RuntimeSnapshot& s) { s.deleting_leftover_title = id; });
    const auto error = RunDeleteLeftover(c, request);
    ClearWork();
    Publish([&](RuntimeSnapshot& s) {
      s.deleting_leftover_title.reset();
      s.deleting_leftover_step.clear();
      if (!error.empty()) s.leftover_delete_failures[id] = error;
      else s.leftover_delete_failures.erase(id);
      ++s.leftover_deletes_handled;
    });
  }
}

std::optional<RuntimeHost::RemovalFailure> RuntimeHost::KeepBottle(
    const std::filesystem::path& bottle, const std::string& install_id, const GameId& game,
    std::uint32_t runtime_number, const std::string& name, const std::string& operation_id) {
  const auto kept = bottle.parent_path() / (install_id + kKeptSuffix);
  std::error_code error;
  struct stat info{};
  // A Steam game's marker is as before; another store's also names its game.
  const auto identity = game.store == Store::kSteam ? std::string()
      : ",\"store\":\"" + std::string(Traits(game.store).key) + "\",\"game\":\"" + JsonEscaped(game.value) + "\"";
  const auto marker = "{\"schema\":1,\"install_id\":\"" + install_id + "\",\"app_id\":" +
                      std::to_string(runtime_number) + identity + ",\"name\":\"" + JsonEscaped(name) +
                      "\",\"kept_at\":" + std::to_string(config_.unix_time ? config_.unix_time() : std::int64_t(time(nullptr))) +
                      ",\"bytes\":" + std::to_string(TreeBytes(bottle)) + "}\n";
  const int fd = open((bottle / kKeptMarker).c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
  const bool written = fd >= 0 && write(fd, marker.data(), marker.size()) == static_cast<ssize_t>(marker.size()) &&
                       fsync(fd) == 0;
  if (fd >= 0) close(fd);
  if (!written) return RemovalFailure{"bottle-keep", "The game's saves could not be marked as kept."};
  const bool restored = lstat((bottle / kRestoredMarker).c_str(), &info) == 0;
  if (lstat(kept.c_str(), &info) == 0 && !restored) {
    // The kept copy's saves were never brought back into this bottle (a preparation after a move or
    // reinstall stopped before that): the kept copy holds them and stays. The newer bottle is set
    // aside beside it, never deleted.
    const auto aside = bottle.parent_path() / (install_id + ".set-aside-" + operation_id);
    Log("remove keeps the unrestored kept bottle install=" + install_id + "; newer bottle set aside");
    if (rename(bottle.c_str(), aside.c_str()) != 0)
      return RemovalFailure{"bottle-keep", "The game's newer bottle could not be set aside (error " + std::to_string(errno) + ")."};
  } else if (lstat(kept.c_str(), &info) == 0) {
    // An older kept copy whose saves this bottle already has: the bottle replaces it.
    Log("remove replaces older kept bottle install=" + install_id);
    RemoveTreeNoFollow(kept, error);
    if (error) return RemovalFailure{"bottle-keep", "An older copy of the game's saves could not be replaced."};
    if (rename(bottle.c_str(), kept.c_str()) != 0)
      return RemovalFailure{"bottle-keep", "The game's saves could not be kept (error " + std::to_string(errno) + ")."};
  } else if (rename(bottle.c_str(), kept.c_str()) != 0) {
    return RemovalFailure{"bottle-keep", "The game's saves could not be kept (error " + std::to_string(errno) + ")."};
  }
  return std::nullopt;
}

void RuntimeHost::KeepUnrecordedBottles(Composition& c) {
  // Every reason to doubt which bottles are unrecorded stops the whole pass: a kept bottle is
  // renamed away from where its game's title looks for it.
  // A leftover deletion journal, even an unreadable one, may stand for a retired title that still names a bottle.
  if (!c.repository.Loaded() || !c.leftover_journals_known || !c.blocked_leftover_titles.empty()) return;
  const auto records = c.repository.Snapshot();
  std::set<std::string> recorded_installs;
  for (const auto& record : records) recorded_installs.insert(record.install_id);
  // A PS5 title starts the bottle its game-launch.json names, running or not, whatever owns its folder and
  // whatever the records say about it: that bottle stays until the title is deleted (Storage lists it as a leftover).
  std::set<std::string> named_installs, named_roots;
  {
    std::error_code error;
    for (std::filesystem::directory_iterator it(config_.homebrew_root, error), end; !error && it != end;
         it.increment(error)) {
      const auto title = it->path().filename().string();
      if (!rt::IsManagedTitle(title)) continue;
      struct stat info{};
      if (lstat(it->path().c_str(), &info) != 0) {
        Log("unrecorded bottles not kept: " + title + " cannot be inspected");
        return;
      }
      if (!S_ISDIR(info.st_mode)) continue;
      const int fd = open((it->path() / "game-launch.json").c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
      std::string bytes;
      if (fd >= 0) {
        char chunk[4096];
        for (ssize_t count; (count = read(fd, chunk, sizeof(chunk))) > 0 && bytes.size() <= 8192;)
          bytes.append(chunk, static_cast<std::size_t>(count));
        close(fd);
      } else if (errno == ENOENT) {
        continue;  // no descriptor: the title names no bottle
      }
      const auto json = bytes.size() <= 8192 ? playden::steam::json::Parse(bytes) : std::nullopt;
      const auto* install = json ? json->Find("install_id") : nullptr;
      const auto* root = json ? json->Find("data_root") : nullptr;
      if (!install || !install->is_string() || !root || !root->is_string()) {
        Log("unrecorded bottles not kept: the game-launch.json of " + title + " cannot be read");
        return;
      }
      named_installs.insert(install->text());
      named_roots.insert(root->text());
    }
    if (error) {
      Log("unrecorded bottles not kept: " + config_.homebrew_root.string() + " cannot be listed");
      return;
    }
  }
  const auto console_games = config_.package_paths.console_data_root + "/playden/games";
  const auto games = HostPath(console_games);
  std::vector<std::string> found;
  {
    std::error_code error;
    for (std::filesystem::directory_iterator it(games, error), end; !error && it != end; it.increment(error)) {
      const auto install_id = it->path().filename().string();
      if (!KeptInstallId(install_id) || recorded_installs.count(install_id) || named_installs.count(install_id) ||
          named_roots.count(console_games + "/" + install_id))
        continue;
      struct stat info{};
      if (lstat(it->path().c_str(), &info) != 0 || !S_ISDIR(info.st_mode) || info.st_uid != geteuid()) continue;
      found.push_back(install_id);
    }
  }
  bool kept_any = false;
  for (const auto& install_id : found) {
    const auto game = ParseInstallId(install_id);
    if (!game) continue;
    std::uint32_t number = 0;
    if (const auto app = game->steam_app()) {
      number = *app;
    } else {
      const auto assigned = c.repository.RuntimeNumberFor(*game);
      if (!assigned.ok()) {
        Log("unrecorded bottle not kept install=" + install_id + ": " + assigned.message());
        // A failed write leaves the records unread: every later decision would rest on them.
        if (!c.repository.Loaded()) return;
        continue;
      }
      number = assigned.value();
    }
    const auto operation = config_.uuid();
    if (!operation) { Log("unrecorded bottles not kept: no secure random source"); return; }
    const auto bottle = games / install_id;
    std::string name;
    {
      const int fd = open((bottle / "title-metadata/sce_sys/param.json").c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
      if (fd >= 0) {
        std::string bytes;
        char chunk[4096];
        for (ssize_t count; (count = read(fd, chunk, sizeof(chunk))) > 0 && bytes.size() < 64 * 1024;)
          bytes.append(chunk, static_cast<std::size_t>(count));
        close(fd);
        name = WithoutStoreSuffix(TitleNameOf(bytes).value_or(std::string{}));
      }
    }
    if (const auto failed = KeepBottle(bottle, install_id, *game, number, name, *operation)) {
      Log("unrecorded bottle not kept install=" + install_id + ": " + failed->message);
      continue;
    }
    Log("unrecorded bottle kept install=" + install_id);
    kept_any = true;
  }
  if (kept_any) PublishKeptBottles(c);
}

// Removal of a game's PS5 title, journaled in its install record so an interrupted
// removal continues where it stopped: retire the title folder out of ShadowMount's
// scan root, unregister the title through the one-shot remover, keep or delete the
// bottle, then drop the record once the console no longer lists the title.
std::string RuntimeHost::RunRemove(Composition& c, std::uint32_t app, bool delete_saves, bool moving) {
  // After a failed write the records are unknown until read again, and an empty list would read
  // as "no PS5 title": the game's files would then be deleted under a title that still exists.
  if (!c.repository.Loaded()) {
    const auto loaded = c.repository.Load();
    PublishInstalls(c);
    if (!loaded.ok()) {
      Log("remove records unavailable: " + loaded.message());
      return "Playden's records of PS5 titles cannot be read: " + loaded.message() + ".";
    }
    Log("remove records read again installs=" + std::to_string(c.repository.Snapshot().size()));
  }
  std::optional<rt::RuntimeInstall> found;
  for (const auto& record : c.repository.Snapshot())
    if (record.runtime_number == app) found = record;
  if (!found) return {};  // no PS5 title: nothing to remove
  auto record = *found;
  if (!config_.unregister_title) return "Removing PS5 titles is not available in this build.";
  if (c.launch_journal) {
    const auto pending = c.launch_journal->LoadPending();
    if (!pending.ok) return "The launch record cannot be read; restart Playden before uninstalling.";
    if (pending.session && pending.session->install_id == record.install_id)
      return "Close the game before uninstalling it.";
  }
  const auto activity = c.coordinator->Activity(record.title_id);
  if (activity == rt::NativeActivity::kActive) return "Close the game before uninstalling it.";
  if (activity != rt::NativeActivity::kStopped) return kActivityUnknown;
  const bool resuming = record.state == rt::PreparationState::kRemoving && record.journal &&
                        record.journal->operation.rfind("remove", 0) == 0;
  // The game's name for the kept bottle, read before its title folder goes.
  std::string game_name;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (const auto named = title_names_.find(app); named != title_names_.end()) game_name = named->second;
  }
  if (game_name.empty()) game_name = InstalledTitleName(record).value_or(std::string{});
  game_name = WithoutStoreSuffix(std::move(game_name));
  auto save = [&](const char* where) -> std::string {
    const auto updated = c.repository.Update(record);
    PublishInstalls(c);
    if (!updated.ok()) {
      Log(std::string("remove record update failed at ") + where + ": " + updated.message());
      return "The removal could not be recorded: " + updated.message();
    }
    return {};
  };
  if (!resuming) {
    const auto operation = config_.uuid();
    if (!operation) return "No secure random source for the removal.";
    record.state = rt::PreparationState::kRemoving;
    record.journal = rt::PreparationJournal{*operation, delete_saves ? "remove-saves" : moving ? "remove-move" : "remove",
                                            "retire"};
    record.last_failure.reset();
    if (auto failed = save("start"); !failed.empty()) return failed;
  }
  // An uninstall that deletes saves takes over a removal its game's move started (that one keeps them).
  if (resuming && delete_saves && record.journal->operation == "remove-move") {
    record.journal->operation = "remove-saves";
    if (auto failed = save("adopt"); !failed.empty()) return failed;
  }
  delete_saves = record.journal->operation == "remove-saves";
  const auto fail = [&](const std::string& code, const std::string& message) {
    record.last_failure = rt::RuntimeFailure{code, message};
    save("failure");
    Log("remove failed install=" + record.install_id + " step=" + record.journal->step + " code=" + code);
    return message;
  };
  const auto advance = [&](const char* step) {
    record.journal->step = step;
    record.last_failure.reset();
    return save(step);
  };
  Log("remove install=" + record.install_id + " title=" + record.title_id + " step=" + record.journal->step +
      " delete_saves=" + std::to_string(delete_saves));
  const auto source = config_.homebrew_root / record.title_id;
  if (record.journal->step == "retire") {
    if (const auto failed = RetireTitle(c, record.title_id, record.journal->operation_id))
      return fail(failed->code, failed->message);
    if (auto failed = advance("unregister"); !failed.empty()) return failed;
  }
  if (record.journal->step == "unregister") {
    if (const auto failed = UnregisterTitle(c, record.title_id, record.journal->operation_id))
      return fail(failed->code, failed->message);
    if (auto failed = advance("bottle"); !failed.empty()) return failed;
  }
  if (record.journal->step == "bottle") {
    const auto bottle = HostPath(record.data_root);
    const auto kept = bottle.parent_path() / (record.install_id + kKeptSuffix);
    std::error_code error;
    struct stat info{};
    const bool present = lstat(bottle.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
    if (delete_saves) {
      // Deleting a bottle takes a while: its progress is the entries removed of those counted.
      const std::uint64_t total = CountTreeNoFollow(kept.string()) + (present ? CountTreeNoFollow(bottle.string()) : 0);
      std::uint64_t done = 0;
      const std::function<void()> removed = [&] { ReportWork(++done, total); };
      RemoveTreeNoFollow(kept, error, &removed);  // links are removed, never followed
      if (!error && present) RemoveTreeNoFollow(bottle, error, &removed);
      if (!error) RemoveTreeNoFollow(SaveBackupRoot(record.install_id), error);
      ClearWork();
      if (error) return fail("bottle-delete", "The game's saves and settings could not be deleted: " + error.message());
    } else if (present) {
      if (const auto failed = KeepBottle(bottle, record.install_id, GameOf(record), record.runtime_number, game_name,
                                         record.journal->operation_id))
        return fail(failed->code, failed->message);
    }
    if (auto failed = advance("erase"); !failed.empty()) return failed;
  }
  // Erase: both the console inventory and the title folder must be gone.
  rt::NativeTitleAvailability confirmed;
  confirmed.inventory = TitleListed(c, record.title_id) ? rt::TitleAvailability::kOccupied : rt::TitleAvailability::kFree;
  struct stat info{};
  confirmed.source_path = lstat(source.c_str(), &info) != 0 && errno == ENOENT ? rt::TitleAvailability::kFree
                                                                               : rt::TitleAvailability::kOccupied;
  // Saves an uninstall deletes after a move had already kept them (it took over past the bottle step):
  // gone before the record, so a failure keeps the journal and the next attempt deletes them again.
  if (delete_saves) {
    const auto kept = HostPath(record.data_root).parent_path() / (record.install_id + kKeptSuffix);
    std::error_code error;
    struct stat kept_info{};
    if (lstat(kept.c_str(), &kept_info) == 0) RemoveTreeNoFollow(kept, error);
    if (!error) RemoveTreeNoFollow(SaveBackupRoot(record.install_id), error);
    if (error) return fail("bottle-delete", "The game's saves and settings could not be deleted: " + error.message());
  }
  const auto erased = c.repository.EraseAfterUnregister(record.install_id, confirmed);
  if (!erased.ok()) return fail("erase", "The removal could not be completed: " + erased.message());
  // The title is gone; a stale entry would only let a later title with its ID start early.
  if (const auto listed = SyncTitleAllowList(c); !listed.empty()) Log("remove allow-list: " + listed);
  // A generated recipe is made again from Steam's launch options on the next prepare.
  if (rt::IsGeneratedRecipeId(record.recipe_id)) {
    c.storage.Remove(GeneratedRecipeFile(record.recipe_id));
    c.recipes.erase(std::remove_if(c.recipes.begin(), c.recipes.end(),
                                   [&](const rt::GameRecipe& recipe) { return recipe.id == record.recipe_id; }),
                    c.recipes.end());
    PublishRecipes(c);
  }
  c.admission.erase(record.install_id);
  // A normal uninstall's record is already erased; leftover failures keep their private journal.
  DeleteRetiredTitle(record.title_id, record.journal->operation_id);
  PublishInstalls(c);
  Log("remove complete install=" + record.install_id + " title=" + record.title_id);
  c.retired_titles.insert(record.title_id);
  return {};
}

std::string RuntimeHost::RestoreKeptSaves(Composition& c, std::uint32_t app) {
  std::optional<rt::RuntimeInstall> record;
  for (const auto& candidate : c.repository.Snapshot())
    if (candidate.runtime_number == app && (candidate.state == rt::PreparationState::kReady || AwaitingRegistration(candidate)))
      record = candidate;
  if (!record) return {};
  const auto bottle = HostPath(record->data_root);
  const auto kept = bottle.parent_path() / (record->install_id + kKeptSuffix);
  struct stat info{};
  if (lstat(kept.c_str(), &info) != 0 || !S_ISDIR(info.st_mode)) return {};
  const auto from = kept / "library/prefixes/game/drive_c/users";
  const auto to = bottle / "library/prefixes/game/drive_c/users";
  std::string reason;
  if (lstat(from.c_str(), &info) == 0 && S_ISDIR(info.st_mode)) {
    if (lstat(to.parent_path().c_str(), &info) != 0 || !S_ISDIR(info.st_mode))
      return "The game's new bottle has no drive C for its kept saves.";
    if (!CopySavesTree(from, to, &reason, [this](const std::string& line) { Log(line); })) {
      Log("restore kept saves failed app=" + std::to_string(app) + " " + reason);
      return "The saves kept from the previous install could not be brought back: " + reason;
    }
    // The restored profile is opened to FTP like the rest (bottle-profile-shared); the copy is private.
    if (c.kit && rt::KitCapabilitiesFromManifest(c.kit->capabilities, true).bottle_profile_shared)
      SettleBottle(c, *record, "restore");
  }
  {
    const int fd = open((bottle / kRestoredMarker).c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
    const bool marked = fd >= 0 && fsync(fd) == 0;
    if (fd >= 0) close(fd);
    if (!marked) return "The saves were brought back, but that could not be recorded.";
  }
  std::error_code error;
  RemoveTreeNoFollow(kept, error);
  Log("restore kept saves app=" + std::to_string(app) + " kept_removed=" + std::to_string(!error));
  PublishKeptBottles(c);
  return {};
}

std::string RuntimeHost::DeleteKept(Composition& c, std::uint32_t app) {
  // The kept bottle listed with this number; a Steam game's is named by its app ID.
  std::string install_id = ToInstallId(GameId::Steam(app));
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& view : snapshot_.kept_bottles)
      if (view.runtime_number == app) install_id = view.install_id;
  }
  const auto kept = HostPath(config_.package_paths.console_data_root + "/playden/games") / (install_id + kKeptSuffix);
  struct stat info{};
  if (lstat(kept.c_str(), &info) != 0) return {};
  if (!S_ISDIR(info.st_mode) || !ReadKeptMarker(kept, install_id)) return "This folder is not kept game data.";
  std::error_code error;
  RemoveTreeNoFollow(kept, error);  // links are removed, never followed
  if (!error) RemoveTreeNoFollow(SaveBackupRoot(install_id), error);
  return error ? "The kept saves could not be deleted: " + error.message() : std::string{};
}

void RuntimeHost::Run() {
  while (RunServices()) Log("kit reload: starting the runtime services again");
}

bool RuntimeHost::RunServices() {
  const auto hold = [this] { return config_.hold_effective_uid ? config_.hold_effective_uid() : nullptr; };
  auto startup_hold = hold();
  auto unavailable = [this](std::string reason) {
    Publish([&](RuntimeSnapshot& s) { s.status = RuntimeHostStatus::kUnavailable; s.reason = std::move(reason); });
  };
  std::string reason;
  const auto request_directory = config_.state_root / "requests";
  const bool private_ready =
      rt::EnsurePrivateDirectories(config_.boundary, config_.state_root, config_.state_root, &reason) &&
      rt::EnsurePrivateDirectories(config_.boundary, config_.state_root, request_directory, &reason);
  if (!private_ready) {
    Log("runtime unavailable: " + reason);
    unavailable("Private runtime state is unavailable: " + reason);
  }
  std::unique_ptr<Composition> composition;
  if (private_ready) {
    composition = std::make_unique<Composition>(config_);
    auto& c = *composition;
    c.request_directory = request_directory;
    if (!rt::LoadSettingsData(config_.runtime_dir, &c.presets, &c.recipes, &c.settings_data_reason) &&
        c.settings_data_reason.empty())
      c.settings_data_reason = "Game settings data is unavailable.";
    std::string kit_reason;
    c.kit = config_.admit_kit ? config_.admit_kit(&kit_reason) : std::nullopt;
    if (c.kit) {
      auto operations = config_.package_operations;
      operations.canceled = [this] { return Stopping(); };
      operations.progress = [this](std::uint64_t done, std::uint64_t total) { ReportWork(done, total); };
      operations.log = [this](const std::string& line) { Log(line); };
      auto installer = std::make_unique<rt::FilesystemPackageInstaller>(c.kit->view, config_.package_paths, operations);
      // Records prepared with a recipe Playden ships resolve whatever recipe the kit carries,
      // as long as the kit's template provides what the recipe requires.
      std::vector<std::string> shipped;
      for (const auto& recipe : c.recipes) {
        if (rt::IsGeneratedRecipeId(recipe.id)) continue;
        if (const auto lacks = KitLacksForRecipe(recipe, c.kit->view); lacks.empty()) shipped.push_back(recipe.id);
        else Log("recipe " + recipe.id + " refused kit=" + c.kit->view.kit_id + " " + lacks);
      }
      installer->AcceptShippedRecipes(std::move(shipped));
      installer->ShareBottleProfiles(rt::KitCapabilitiesFromManifest(c.kit->capabilities, true).bottle_profile_shared);
      c.installer = std::move(installer);
    } else {
      if (kit_reason.empty()) kit_reason = "No verified runtime kit is installed.";
      c.installer = std::make_unique<UnavailablePackageInstaller>(kit_reason);
    }
    auto native = config_.native;
    if (!native.log) native.log = [this](const std::string& line) { Log(line); };
    c.coordinator = std::make_unique<ps5::Ps5NativeCoordinator>(
        c.records, *c.installer, c.registry, config_.homebrew_root, std::move(native));
    c.owner = std::make_unique<rt::SettingsPublicationOwner>(
        c.records, *c.installer, *c.coordinator,
        c.kit && rt::KitCapabilitiesFromManifest(c.kit->capabilities, true).game_root_unowned,
        config_.host_namespace_prefix);
    c.owner->SetLog([this](const std::string& line) { Log(line); });
    c.service = std::make_unique<rt::RuntimeInstallService>(
        c.records, *c.installer, *c.coordinator,
        [&c](const rt::RuntimeInstall& record) {
          for (const auto& recipe : c.recipes)
            if (RecipeFits(recipe, record) && recipe.id == record.recipe_id &&
                std::to_string(recipe.version) == record.recipe_version)
              return rt::CompatibilityResult{true, {}};
          return rt::CompatibilityResult{false, "Playden no longer knows how to start this game; prepare it again."};
        },
        [this] { return Stopping(); },
        [this, &c](const rt::RuntimeInstall& record) { return DrivePublication(c, record); });
    Log("startup loading records");
    const auto loaded = c.repository.Load();
    std::string move_error;
    if (!loaded.ok() || !MoveGames(c, &move_error)) {
      Log("runtime records unavailable: " + loaded.message());
      unavailable(move_error.empty() ? "Runtime install records are unavailable: " + loaded.message() : move_error);
      composition.reset();
    } else {
      // Kept bottles first: their games' numbers are taken before anyone may ask for a new one.
      PublishKeptBottles(c);
      Log("startup kept bottles published");
      ShareNumbers(&c.repository);
      // Install records with a journal wait for an explicit recovery action. Leftover-title
      // deletion journals are resumed separately before their retired folders can be swept.
      Log("startup records loaded installs=" + std::to_string(c.repository.Snapshot().size()));
      LoadGeneratedRecipes(c);
      Log("startup generated recipes loaded");
      PublishInstalls(c);
      Log("startup installs published");
      ResumeLeftoverDeletes(c);
      PublishLeftoverTitles(c);
      KeepUnrecordedBottles(c);
      // Retired title folders left by an earlier removal are outside the scan
      // root already; deleting them is only housekeeping.
      if (c.leftover_journals_known) {
        std::error_code error;
        for (std::filesystem::directory_iterator it(config_.state_root / "trash", error), end; !error && it != end;
             it.increment(error)) {
          const auto entry = it->path().filename().string();
          const bool pending = std::any_of(c.blocked_leftover_titles.begin(), c.blocked_leftover_titles.end(),
                                           [&](const auto& title) { return entry.rfind(title + "-", 0) == 0; });
          if (pending) continue;
          std::error_code ignored;
          RemoveTreeNoFollow(it->path(), ignored);  // never follows links
        }
      }
      Log("startup trash emptied");
      if (const auto listed = SyncTitleAllowList(c); !listed.empty()) Log("startup allow-list: " + listed);
      if (c.kit)
        for (const auto& record : c.repository.Snapshot())
          if (record.state == rt::PreparationState::kReady) {
            // The quick form: what a game run changes and Repair fixes. Play and Update
            // run the title's whole start check.
            Log("admission check title=" + record.title_id + " quick");
            c.admission[record.install_id] = c.installer->QuickAdmission(record);
            Log("admission check title=" + record.title_id + " result=" + c.admission[record.install_id]);
          }
      PublishInstalls(c);
      if (config_.title_log)
        for (const auto& record : c.repository.Snapshot()) {
          const auto root = config_.host_namespace_prefix.empty()
              ? std::filesystem::path(record.data_root)
              : config_.host_namespace_prefix / std::filesystem::path(record.data_root).relative_path();
          const auto install = config_.host_namespace_prefix.empty()
              ? std::filesystem::path(record.install_path)
              : config_.host_namespace_prefix / std::filesystem::path(record.install_path).relative_path();
          const auto program = record.install_path.empty() || record.launch_path.empty()
              ? std::filesystem::path() : install / record.launch_path;
          if (const auto text = NewestSessionLog(root, program)) config_.title_log(record.title_id, *text);
        }
      // Play: the launch journal sits beside the install records. A session
      // pending from before a restart is polled, never handed off again.
      c.launch_journal = std::make_unique<rt::StorageLaunchSessionJournal>(c.storage);
      c.sessions = std::make_unique<rt::FilesystemReconcileSessionSource>(
          rt::HostDataRootForTests{config_.session_data_root});
      rt::LaunchDependencies launch;
      launch.preflight = [this, &c](const rt::RuntimeInstall& record) { return Preflight(c, record); };
      launch.evidence = [this, &c](const rt::RuntimeInstall& record, const std::string& session) {
        return Evidence(c, record, session);
      };
      launch.activity = [&c](const rt::RuntimeInstall& record) { return c.coordinator->Activity(record.title_id); };
      launch.backend = [this](const rt::RuntimeInstall& record, const std::string& session) {
        return config_.launch_title(record.title_id, {"session=" + session});
      };
      // Playden's own state is already durable; nothing waits to be written.
      launch.flush = [] { return rt::LaunchActionResult::Success(); };
      launch.uuid = [this] { return config_.uuid().value_or(std::string{}); };
      launch.now_ms = config_.now_ms;
      launch.is_application_thread = [this] { return pthread_equal(pthread_self(), application_thread_) != 0; };
      launch.evidence_wait_ms = config_.launch_evidence_wait_ms;
      {
        std::lock_guard<std::mutex> lock(launch_mutex_);
        c.launch = std::make_unique<rt::LaunchService>(c.records, *c.launch_journal, std::move(launch));
        launch_ = c.launch.get();
        launch_composition_ = &c;
      }
      if (PollLaunch(c)) {
        std::lock_guard<std::mutex> lock(mutex_);
        launch_poll_ = true;
      }
      // Games started from Home (and Playden's, which close Playden) left Wine's 0600 saves: with
      // bottle-profile-shared they are opened to FTP again once their title is stopped.
      if (c.kit && rt::KitCapabilitiesFromManifest(c.kit->capabilities, true).bottle_profile_shared) {
        const auto began = std::chrono::steady_clock::now();
        std::size_t settled = 0;
        for (const auto& record : c.repository.Snapshot())
          if (MaintenanceRefusal(c, record).empty()) { SettleBottle(c, record, "startup"); ++settled; }
        Log("startup bottle profiles settled=" + std::to_string(settled) + " ms=" +
            std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began).count()));
      }
      // Bottles from a kit with another folder name point Wine Mono at it: games started from Home find Mono only
      // once they name this kit's. Busy titles wait for Play.
      if (c.kit)
        for (const auto& record : c.repository.Snapshot())
          if (record.state == rt::PreparationState::kReady && MaintenanceRefusal(c, record).empty())
            KitMonoStep(c, record, "startup");
      // An unowned upgrade's request cut off before its publication began (a crash, a capture
      // refused on an earlier start) is dropped first: the game plays under its owned publication,
      // and the pass below tries the upgrade again. Moved games and newer settings keep theirs.
      for (const auto& record : c.repository.Snapshot())
        if (record.state == rt::PreparationState::kReady && record.journal && record.moved_from.empty() &&
            record.journal->operation == "settings-request" && record.journal->step == "capture-update")
          DropUpgradeRequest(c, record.install_id, "left by an earlier start");
      // Games whose folder moved with the games root (0.6) start from Home only once their
      // publication names the new folder: republished now, busy titles wait for Play.
      // So are games published owned while the kit admits unowned folders (folders opened for
      // mods). The Steam account is not known yet, so the emulator's files wait for Play.
      for (const auto& record : c.repository.Snapshot()) {
        if (record.state != rt::PreparationState::kReady) continue;
        const bool moved = !record.moved_from.empty();
        if (!moved && !NeedsUnownedPublication(c, record)) continue;
        const auto busy = c.kit ? MaintenanceRefusal(c, record) : std::string("no verified kit");
        bool published = false;
        std::string deferred;
        const auto refused = busy.empty() ? RepublishSettings(c, record, &published, false, nullptr, &deferred) : busy;
        Log(std::string(moved ? "startup moved app=" : "startup unowned app=") + std::to_string(record.runtime_number) + " " +
            (published ? std::string("republished")
             : !deferred.empty() ? "deferred: " + deferred
                                 : (busy.empty() ? "failed: " : "waits: ") + refused));
      }
      FinishGamesMove(c);
      // Settings saved while Playden was closed, or before a title was stopped, are published
      // now, so a start from the PS5 Home screen uses them too. Busy titles wait for Play.
      if (c.kit)
        for (const auto& record : c.repository.Snapshot()) {
          // Only newer settings: the Steam account is not known yet, so Steam files wait for Play.
          // So are they after a program's run (RuntimeInstall::setup_program), whatever their revision.
          const auto saved = config_.settings_record ? config_.settings_record(GameOf(record)) : std::nullopt;
          const bool newer = saved && saved->revision > record.settings_revision;
          if ((!newer && record.setup_program.empty()) || !record.moved_from.empty() ||
              !MaintenanceRefusal(c, record).empty()) continue;
          bool published = false;
          const auto refused = RepublishSettings(c, record, &published, false);
          if (published || !refused.empty())
            Log("startup settings app=" + std::to_string(record.runtime_number) + " " + (refused.empty() ? "applied" : refused));
        }
      const auto leftovers = LeftoverRequests(c, false);
      Publish([&](RuntimeSnapshot& s) { s.leftover_requests = leftovers; });
      const bool kit_available = c.kit.has_value();
      Log("runtime ready installs=" + std::to_string(c.repository.Snapshot().size()) + " kit=" +
          (kit_available ? c.kit->view.kit_id : "none (" + kit_reason + ")") +
          (c.settings_data_reason.empty() ? std::string{} : " settings_data=" + c.settings_data_reason));
      Publish([&](RuntimeSnapshot& s) {
        s.status = RuntimeHostStatus::kReady; s.reason.clear();
        s.kit_available = kit_available; s.kit_reason = kit_available ? std::string{} : kit_reason;
        s.kit = c.kit;
        // The UI reads the kit's identity and capabilities; its file and artifact lists (thousands of
        // entries) stay on the worker, or every Snapshot() on the application thread would copy them.
        if (s.kit) {
          s.kit->view.files.clear();
          s.kit->view.files.shrink_to_fit();
          s.kit->view.artifacts.clear();
          s.kit->view.artifacts.shrink_to_fit();
        }
      });
    }
  }
  if (private_ready) {
    LoadPreparedHistory();
    PublishPrepared(composition.get());
  }
  startup_hold.reset();
  bool reload = false;
  for (;;) {
    std::vector<PlayRequest> plays;
    std::vector<RemovalRequest> removals;
    std::vector<std::uint32_t> kept_deletes;
    std::vector<std::pair<std::uint32_t, RuntimeMaintenance>> maintenance;
    std::vector<std::pair<std::uint32_t, std::string>> scans;
    bool poll = false;
    bool cleanup = false;
    bool measure = false;
    std::vector<std::uint32_t> checks;
    bool names_changed = false;
    std::vector<PreparedGame> dismissals;
    bool clear_prepared = false;
    std::vector<std::shared_ptr<PendingRelocation>> relocations;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      // Preparation changes install records that Handoff reads on the
      // application thread, so it waits while a handoff is pending.
      const auto ready = [this] {
        return stopping_ || (!prepare_requests_.empty() && !snapshot_.handoff_ready && !prepares_held_) ||
               !play_requests_.empty() ||
               cleanup_requested_ || measure_requested_ || !activity_requests_.empty() || title_names_changed_ ||
               installs_recheck_requested_ ||
               (!remove_requests_.empty() && !snapshot_.handoff_ready) ||
               (!kept_delete_requests_.empty() && !snapshot_.handoff_ready) ||
               !scan_requests_.empty() || !prepared_dismissals_.empty() || prepared_clear_requested_ ||
               (!maintenance_requests_.empty() && !snapshot_.handoff_ready) ||
               (!relocation_requests_.empty() && !snapshot_.handoff_ready) ||
               (kit_reload_requested_ && !snapshot_.handoff_ready && !launch_poll_);
      };
      if (launch_poll_) wake_.wait_for(lock, kLaunchPollInterval, ready);
      else wake_.wait(lock, [&] { return ready() || launch_poll_; });
      if (stopping_) break;
      // Between batches, with no launch to hand off or follow: the services hold no work, and PS5
      // titles run their own copy of the runtime, so the kit's folder can change under them.
      if (kit_reload_requested_ && !snapshot_.handoff_ready && !launch_poll_) {
        kit_reload_requested_ = false;
        reload = true;
        break;
      }
      plays.swap(play_requests_);
      // Removal changes install records that Handoff reads, like preparation.
      if (!snapshot_.handoff_ready && !remove_requests_.empty() &&
          (prepare_requests_.empty() || prepares_held_ ||
           remove_requests_.front().order < prepare_order_.at(prepare_requests_.front().runtime_number))) {
        removals.push_back(std::move(remove_requests_.front()));
        remove_requests_.erase(remove_requests_.begin());
      }
      if (!snapshot_.handoff_ready) kept_deletes.swap(kept_delete_requests_);
      scans.swap(scan_requests_);
      // Maintenance changes the bottle and install record, like removal.
      if (!snapshot_.handoff_ready) maintenance.swap(maintenance_requests_);
      // A relocation rewrites the install record, like maintenance.
      if (!snapshot_.handoff_ready) relocations.swap(relocation_requests_);
      poll = launch_poll_;
      cleanup = cleanup_requested_;
      cleanup_requested_ = false;
      measure = measure_requested_;
      measure_requested_ = false;
      checks.swap(activity_requests_);
      names_changed = title_names_changed_ || installs_recheck_requested_;
      title_names_changed_ = false;
      installs_recheck_requested_ = false;
      dismissals.swap(prepared_dismissals_);
      clear_prepared = prepared_clear_requested_;
      prepared_clear_requested_ = false;
    }
    const auto batch_hold = hold();
    if (private_ready && (clear_prepared || !dismissals.empty())) {
      if (clear_prepared) prepared_history_.clear();
      for (const auto& dismissal : dismissals) std::erase(prepared_history_, dismissal);
      SavePreparedHistory();
      PublishPrepared(composition.get());
    }
    if (names_changed && composition) PublishInstalls(*composition);
    for (const auto& [app, install_path] : scans) {
      const auto kit = composition && composition->kit
          ? rt::KitCapabilitiesFromManifest(composition->kit->capabilities, true) : rt::KitCapabilities{};
      auto programs = rt::ListGamePrograms(HostPath(install_path).string(), kit);
      Log("program scan app=" + std::to_string(app) + " programs=" + std::to_string(programs.size()));
      LogSteamStubPrograms(app, programs);
      Publish([&](RuntimeSnapshot& s) {
        auto& setup = s.game_setups[app];
        setup.programs = std::move(programs);
        setup.scanned = true;
      });
    }
    for (const auto app : checks) {
      // Unknown unless the console says otherwise; no runtime means unknown too.
      auto activity = rt::NativeActivity::kUnknown;
      if (composition) {
        activity = rt::NativeActivity::kStopped;
        for (const auto& record : composition->repository.Snapshot())
          if (record.runtime_number == app && !record.title_id.empty()) activity = composition->coordinator->Activity(record.title_id);
      }
      Publish([&](RuntimeSnapshot& s) { s.activity[app] = {activity, ++s.activity_serial}; });
    }
    if (measure && composition) {
      std::map<std::uint32_t, std::uint64_t> sizes;
      constexpr const char* kUsers = "/library/prefixes/game/drive_c/users";
      for (const auto& record : composition->repository.Snapshot())
        if (!record.data_root.empty()) sizes[record.runtime_number] = SizeTreeNoFollow(HostPath(record.data_root + kUsers).string());
      const auto kept = Snapshot().kept_bottles;
      for (const auto& bottle : kept)
        if (!sizes.count(bottle.runtime_number)) sizes[bottle.runtime_number] = SizeTreeNoFollow(HostPath(bottle.data_root + kUsers).string());
      Publish([&](RuntimeSnapshot& s) { s.save_bytes = std::move(sizes); });
    }
    if (cleanup && composition) {
      const auto kept = LeftoverRequests(*composition, true);
      const auto left = LeftoverRequests(*composition, false);
      Log("leftover requests cleanup kept=" + std::to_string(kept) + " remaining=" + std::to_string(left));
      Publish([&](RuntimeSnapshot& s) { s.leftover_requests = left; });
    }
    for (const auto& play : plays) {
      const auto app = play.app;
      if (composition) {
        RunPlay(*composition, app, play.setup_program);
        poll = true;
      } else {
        Publish([&](RuntimeSnapshot& s) {
          s.play_refusals[app] = s.reason.empty() ? "The runtime services are unavailable." : s.reason;
          ++s.plays_handled;
        });
      }
    }
    {
      // A handoff that became ready during this pass waits for this batch's hold on the effective
      // UID (and its launch check expires): long work goes back to the queue, so the batch ends now.
      std::lock_guard<std::mutex> lock(mutex_);
      if (snapshot_.handoff_ready) {
        remove_requests_.insert(remove_requests_.begin(), removals.begin(), removals.end());
        kept_delete_requests_.insert(kept_delete_requests_.begin(), kept_deletes.begin(), kept_deletes.end());
        maintenance_requests_.insert(maintenance_requests_.begin(), maintenance.begin(), maintenance.end());
        relocation_requests_.insert(relocation_requests_.begin(), relocations.begin(), relocations.end());
        removals.clear();
        kept_deletes.clear();
        maintenance.clear();
        relocations.clear();
      }
    }
    for (const auto& request : removals) {
      const auto app = request.app;
      const auto delete_saves = request.delete_saves;
      if (request.leftover) {
        const auto& title = request.leftover->title.title_id;
        Publish([&](RuntimeSnapshot& s) {
          std::erase(s.queued_leftover_deletes, title);
          s.deleting_leftover_title = title;
          s.leftover_delete_failures.erase(title);
        });
        const auto refusal = composition ? RunDeleteLeftover(*composition, *request.leftover)
                                         : "The runtime services are unavailable.";
        if (composition) {
          PublishLeftoverTitles(*composition);
          KeepUnrecordedBottles(*composition);
        }
        ClearWork();
        Publish([&](RuntimeSnapshot& s) {
          s.deleting_leftover_title.reset();
          s.deleting_leftover_step.clear();
          if (!refusal.empty()) s.leftover_delete_failures[title] = refusal;
          ++s.leftover_deletes_handled;
        });
        continue;
      }
      std::string refusal;
      if (!composition) {
        const auto current = Snapshot();
        refusal = current.reason.empty() ? "The runtime services are unavailable." : current.reason;
      } else {
        Publish([&](RuntimeSnapshot& s) { s.removing_app = app; s.removal_failures.erase(app); });
        refusal = RunRemove(*composition, app, delete_saves);
        PublishKeptBottles(*composition);
        PublishLeftoverTitles(*composition);
      }
      Log("remove result app=" + std::to_string(app) + " " + (refusal.empty() ? "removed" : refusal));
      if (refusal.empty() && std::erase_if(prepared_history_, [app](const PreparedGame& entry) { return entry.runtime_number == app; })) {
        SavePreparedHistory();
        PublishPrepared(composition.get());
      }
      ClearWork();
      Publish([&](RuntimeSnapshot& s) {
        s.removing_app.reset();
        if (refusal.empty()) s.removal_failures.erase(app);
        else s.removal_failures[app] = refusal;
        ++s.removals_handled;
      });
    }
    for (const auto app : kept_deletes) {
      if (!composition) continue;
      const auto refusal = DeleteKept(*composition, app);
      Log("kept bottle delete app=" + std::to_string(app) + " " + (refusal.empty() ? "deleted" : refusal));
      PublishKeptBottles(*composition);
      if (!refusal.empty()) Publish([&](RuntimeSnapshot& s) { s.removal_failures[app] = refusal; });
    }
    for (const auto& [app, action] : maintenance) {
      bool ok = false;
      RedistWorkScope redist(redist_work_, true);  // the player asked for it
      const auto message = composition ? RunMaintenance(*composition, app, action, &ok)
                                       : std::string("The runtime services are unavailable.");
      Log("maintenance app=" + std::to_string(app) + " action=" + std::to_string(static_cast<int>(action)) +
          " ok=" + std::to_string(ok) + " " + message);
      ClearWork();
      Publish([&](RuntimeSnapshot& s) {
        s.maintenance[app] = {action, false, ok, message};
        ++s.maintenance_handled;
      });
    }
    for (const auto& pending : relocations) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pending->abandoned) continue;
        pending->started = true;
      }
      published_.notify_all();
      RuntimeRelocationResult result;
      if (!composition) {
        result.outcome = RuntimeRelocationResult::Outcome::kBusy;
        const auto current = Snapshot();
        result.message = current.reason.empty() ? "The runtime services are unavailable." : current.reason;
      } else {
        result = RunRelocate(*composition, pending->request);
      }
      ClearWork();
      Log("relocate result app=" + std::to_string(pending->request.runtime_number) + " outcome=" +
          std::to_string(static_cast<int>(result.outcome)) + (result.prepare_again ? " prepare-again" : "") +
          (result.message.empty() ? std::string() : " " + result.message));
      {
        std::lock_guard<std::mutex> lock(mutex_);
        pending->result = std::move(result);
      }
      published_.notify_all();
    }
    if (poll && composition) {
      const bool again = PollLaunch(*composition);
      std::lock_guard<std::mutex> lock(mutex_);
      launch_poll_ = again;
    }
    // One preparation per pass, taken from the front of the queue only as it starts, so PrepareNext
    // reorders everything not started yet. A handoff that became ready during this pass reads install
    // records on the application thread, and a quit to Home holds the queue: the queue waits for both.
    std::optional<RuntimePrepareRequest> next;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!prepare_requests_.empty() && !snapshot_.handoff_ready && !prepares_held_ && !stopping_ &&
          (remove_requests_.empty() ||
           prepare_order_.at(prepare_requests_.front().runtime_number) < remove_requests_.front().order)) {
        next = std::move(prepare_requests_.front());
        prepare_requests_.erase(prepare_requests_.begin());
        prepare_order_.erase(next->runtime_number);
        auto& queued = snapshot_.queued_prepares;
        queued.erase(std::remove(queued.begin(), queued.end(), next->runtime_number), queued.end());
        if (composition) {
          snapshot_.preparing_app = next->runtime_number;
          snapshot_.refusals.erase(next->runtime_number);
        }
        ++snapshot_.revision;
      }
    }
    if (next) {
      published_.notify_all();
      const auto& request = *next;
      std::string refusal;
      bool awaiting = false;
      if (!composition) {
        const auto current = Snapshot();
        refusal = current.reason.empty() ? "The runtime services are unavailable." : current.reason;
      } else {
        RedistWorkScope redist(redist_work_, true);  // Prepare, Update and Recreate: the player asked for them
        refusal = RunPrepare(*composition, request, &awaiting);
        // A resumed publication of a moved game may have been the last the 0.6 move waited for.
        FinishGamesMove(*composition);
        // A game prepared again gets back the saves it kept when it was uninstalled,
        // before it can first be started (from Home, while it waits for registration).
        if (refusal.empty()) refusal = RestoreKeptSaves(*composition, request.runtime_number);
        // Settings saved while the game was being prepared (after its profile was written) reach
        // the title as soon as it is ready; the check at startup ran before that.
        if (refusal.empty() && !awaiting)
          for (const auto& record : composition->repository.Snapshot()) {
            if (record.runtime_number != request.runtime_number) continue;
            bool published = false;
            const auto refused = RepublishSettings(*composition, record, &published);
            if (published || !refused.empty())
              Log("prepared settings app=" + std::to_string(record.runtime_number) + " " + (refused.empty() ? "applied" : refused));
          }
      }
      Log("prepare result app=" + std::to_string(request.runtime_number) + " " +
          (!refusal.empty() ? refusal : awaiting ? "awaiting registration" : "ready"));
      // Only a preparation that ends Ready is listed as done; one still waiting for the PS5 is listed
      // once a later check finds the title registered.
      if (composition && refusal.empty() && !awaiting) {
        prepared_history_.push_back({request.runtime_number, config_.unix_time()});
        SavePreparedHistory();
      }
      if (composition) {
        const auto leftovers = LeftoverRequests(*composition, false);
        Publish([&](RuntimeSnapshot& s) { s.leftover_requests = leftovers; });
      }
      ClearWork();
      // Listed as done as it leaves preparing_app (PublishPrepared leaves out the game being prepared).
      auto prepared = composition ? ListPrepared(composition.get()) : std::vector<PreparedGame>{};
      Publish([&](RuntimeSnapshot& s) {
        s.preparing_app.reset();
        ++s.prepares_handled;
        if (refusal.empty()) s.refusals.erase(request.runtime_number);
        else s.refusals[request.runtime_number] = refusal;
        if (composition) s.prepared = std::move(prepared);
      });
    }
  }
  {
    std::lock_guard<std::mutex> lock(launch_mutex_);
    launch_ = nullptr;
    launch_composition_ = nullptr;
  }
  ShareNumbers(nullptr);
  composition.reset();
  Publish([&](RuntimeSnapshot& s) {
    s.status = reload ? RuntimeHostStatus::kStarting : RuntimeHostStatus::kStopped;
    // Refused for want of a kit: the services starting again with one answer anew.
    if (reload) {
      s.refusals.clear();
      s.play_refusals.clear();
    }
  });
  return reload;
}

}  // namespace playden::homebrew
