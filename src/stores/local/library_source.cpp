#include "stores/local/library_source.h"

#include "stores/local/copy_plan.h"

#include <cerrno>
#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <utility>

namespace playden::stores::local {
namespace {

std::string Join(const std::string& base, const std::string& relative) {
  if (relative.empty()) return base;
  if (base.empty() || base == "/") return "/" + relative;
  return base + "/" + relative;
}

bool IsFolder(const std::string& path) {
  struct stat info {};
  return stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

// The same rule the install volumes use: a drive's folder counts only while a file system other
// than its parent's is mounted there.
bool IsMountPoint(const std::string& path) {
  struct stat mount {}, parent {};
  const auto slash = path.find_last_of('/');
  if (slash == std::string::npos) return false;
  const auto parent_path = slash == 0 ? std::string("/") : path.substr(0, slash);
  if (lstat(path.c_str(), &mount) != 0 || !S_ISDIR(mount.st_mode) || stat(parent_path.c_str(), &parent) != 0)
    return false;
  return mount.st_dev != parent.st_dev;
}

const char* ProgramWords(ProgramCheck check) {
  switch (check) {
    case ProgramCheck::kOk: return "";
    case ProgramCheck::kInvalidPath: return "The program's path is not inside the game's folder.";
    case ProgramCheck::kMissing: return "The program is not in the game's folder.";
    case ProgramCheck::kNotRegularFile: return "The program is not a file.";
    case ProgramCheck::kUnsupported: return "The game's folder can't be read on this console.";
    case ProgramCheck::kFailed: return "The game's folder could not be read.";
  }
  return "The program could not be checked.";
}

const char* ImportWords(ImportError error) {
  switch (error) {
    case ImportError::kDuplicate: return "This folder is already in the library.";
    case ImportError::kBounds: return "The library has as many imported games as it can hold.";
    case ImportError::kInvalid: return "The game's folder, program or name can't be used.";
    case ImportError::kNotFound: return "The game is not in the library.";
    case ImportError::kWrongState: return "Cancel the game's copy first.";
    case ImportError::kNotLoaded: return "Playden's list of imported games could not be read.";
    default: return "Playden's list of imported games could not be saved.";
  }
}

}  // namespace

std::vector<ImportVolume> ConsoleImportVolumes() {
  std::vector<ImportVolume> volumes;
  for (const auto& root : ImportRoots())
    volumes.push_back({root.volume, root.mount, root.folder, std::string(root.volume) != ImportStore::kDropFolderVolume});
  return volumes;
}

LocalLibrarySource::LocalLibrarySource(ImportStore& store, LocalLibraryConfig config)
    : store_(store), config_(std::move(config)) {
  // Imports load, the drives are checked, and the import folders are scanned.
  Job first;
  first.command = Command::kScan;
  first.request = ++request_;
  pending_.push_back(std::move(first));
  scan_waiting_ = true;
  auto snapshot = std::make_shared<LocalSnapshot>();
  snapshot->scanning = true;
  snapshot_ = std::move(snapshot);
  worker_ = std::thread([this] { Run(); });
}

LocalLibrarySource::~LocalLibrarySource() { Shutdown(); }

void LocalLibrarySource::Shutdown() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
  }
  wake_.notify_all();
  if (worker_.joinable()) worker_.join();
}

std::uint64_t LocalLibrarySource::Submit(Job job) {
  std::uint64_t request = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_ || pending_.size() >= 64) return 0;
    request = job.request = ++request_;
    pending_.push_back(std::move(job));
  }
  wake_.notify_one();
  return request;
}

std::uint64_t LocalLibrarySource::Refresh(std::uint64_t account, std::uint64_t generation) {
  Job job;
  job.command = Command::kList;
  job.account = account;
  job.generation = generation;
  return Submit(std::move(job));
}

std::uint64_t LocalLibrarySource::Clear(std::uint64_t account, std::uint64_t generation) {
  Job job;
  job.command = Command::kClear;
  job.account = account;
  job.generation = generation;
  return Submit(std::move(job));
}

std::uint64_t LocalLibrarySource::LookForGames() {
  std::uint64_t request = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) return 0;
    // A look that hasn't started yet answers this one too.
    if (scan_waiting_)
      for (const auto& job : pending_)
        if (job.command == Command::kScan) return job.request;
    if (pending_.size() >= 64) return 0;
    Job job;
    job.command = Command::kScan;
    request = job.request = ++request_;
    pending_.push_back(std::move(job));
    scan_waiting_ = true;  // with the job, so the worker can't take it first
    auto next = std::make_shared<LocalSnapshot>(*snapshot_);
    next->scanning = true;
    ++next->revision;
    snapshot_ = std::move(next);
  }
  wake_.notify_one();
  return request;
}

void LocalLibrarySource::CheckVolumes() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (check_waiting_ || scan_waiting_) return;
    check_waiting_ = true;
  }
  Job job;
  job.command = Command::kCheckVolumes;
  if (!Submit(std::move(job))) {
    std::lock_guard<std::mutex> lock(mutex_);
    check_waiting_ = false;
  }
}

std::uint64_t LocalLibrarySource::Add(const std::string& volume, const std::string& folder, const std::string& program) {
  Job job;
  job.command = Command::kAdd;
  job.volume = volume;
  job.folder = folder;
  job.program = program;
  return Submit(std::move(job));
}

std::uint64_t LocalLibrarySource::Remove(const std::string& id) {
  Job job;
  job.command = Command::kRemove;
  job.id = id;
  return Submit(std::move(job));
}

std::uint64_t LocalLibrarySource::OpenForPlay(const std::string& id) {
  Job job;
  job.command = Command::kOpen;
  job.id = id;
  return Submit(std::move(job));
}

std::uint64_t LocalLibrarySource::CopyIntoPlayden(const std::string& id, const std::string& volume_id,
                                                  std::uint32_t runtime_number) {
  Job job;
  job.command = Command::kCopy;
  job.id = id;
  job.volume = volume_id;
  job.runtime_number = runtime_number;
  return Submit(std::move(job));
}

std::uint64_t LocalLibrarySource::MoveIntoPlayden(const std::string& id, std::uint32_t runtime_number) {
  Job job;
  job.command = Command::kMove;
  job.id = id;
  job.runtime_number = runtime_number;
  return Submit(std::move(job));
}

void LocalLibrarySource::Reload() {
  Job job;
  job.command = Command::kReload;
  Submit(std::move(job));
}

std::uint64_t LocalLibrarySource::Reconcile(std::function<installs::InstallSnapshot()> installs,
                                            std::function<void(const GameId&)> cancel_job,
                                            std::function<std::uint32_t(const GameId&)> runtime_number) {
  if (!installs) return 0;
  Job job;
  job.command = Command::kReconcile;
  job.installs = std::move(installs);
  job.cancel_job = std::move(cancel_job);
  job.number_for = std::move(runtime_number);
  return Submit(std::move(job));
}

void LocalLibrarySource::SetRuntimeNumber(std::function<std::uint32_t(const GameId&)> runtime_number) {
  std::lock_guard<std::mutex> lock(mutex_);
  runtime_number_ = std::move(runtime_number);
}

std::vector<LibraryEvent> LocalLibrarySource::Pump(std::size_t max_events) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<LibraryEvent> result;
  while (!events_.empty() && result.size() < max_events) {
    result.push_back(std::move(events_.front()));
    events_.pop_front();
  }
  return result;
}

std::shared_ptr<const LocalSnapshot> LocalLibrarySource::Snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return snapshot_;
}

std::vector<installs::InstalledLocation> LocalLibrarySource::Locations() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return snapshot_->locations;
}

void LocalLibrarySource::Run() {
  for (;;) {
    Job job;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      wake_.wait(lock, [this] { return stopping_ || !pending_.empty(); });
      if (stopping_) return;
      // A scan can take a while on a full drive: library loads, adds and removes go first.
      auto next = std::find_if(pending_.begin(), pending_.end(), [](const Job& waiting) {
        return waiting.command != Command::kScan && waiting.command != Command::kCheckVolumes;
      });
      if (next == pending_.end()) next = pending_.begin();
      job = std::move(*next);
      pending_.erase(next);
      if (job.command == Command::kScan) scan_waiting_ = false;
      if (job.command == Command::kCheckVolumes) check_waiting_ = false;
    }
    Execute(job);
  }
}

void LocalLibrarySource::EnsureLoaded() {
  if (store_.loaded() || load_tried_) return;
  load_tried_ = true;
  const auto loaded = store_.Load();
  load_error_ = loaded.ok() ? std::string{} : loaded.message();
  if (!loaded.ok()) Log("imports load failed: " + loaded.message());
  ++games_revision_;
}

void LocalLibrarySource::Execute(const Job& job) {
  EnsureLoaded();
  switch (job.command) {
    case Command::kList:
    case Command::kClear: {
      LibraryEvent event;
      event.request_id = job.request;
      event.account_id = job.account;
      event.account_generation = job.generation;
      if (job.command == Command::kClear) {
        event.kind = LibraryEventKind::kCleared;
      } else if (!store_.loaded()) {
        event.kind = LibraryEventKind::kFailure;
        event.error = steam::LibraryError::kPersistence;
        event.cache_error = StorageError::kInvalidData;
      } else {
        event.kind = LibraryEventKind::kComplete;
        for (const auto& game : store_.Games()) {
          GameSummary summary;
          summary.id = GameId::Local(game.id);
          summary.title = game.title;
          if (game.added_at) summary.added_at_unix_seconds = game.added_at;
          event.games.push_back(std::move(summary));
        }
        event.cache_available = true;
      }
      std::lock_guard<std::mutex> lock(mutex_);
      events_.push_back(std::move(event));
      return;
    }
    case Command::kScan:
      // The games and the drives are published before the folders are walked.
      CheckPresence();
      Project();
      Scan();
      Project();
      return;
    case Command::kCheckVolumes: {
      const bool changed = CheckPresence();
      if (changed) Scan();
      Project();
      return;
    }
    case Command::kAdd: {
      const auto* volume = Volume(job.volume);
      if (!volume) return Outcome(job, false, {}, "Playden doesn't know the drive \"" + job.volume + "\".");
      if (CheckPresence()) Scan();
      if (!present_.count(volume->volume)) return Outcome(job, false, {}, "The game's drive is not connected.");
      if (!IsSafeRelativePath(job.folder)) return Outcome(job, false, {}, "The game's folder can't be used.");
      // A bare name is a folder in the drive's import folder.
      const auto folder = job.folder.find('/') == std::string::npos ? Join(volume->folder, job.folder) : job.folder;
      const auto path = Join(volume->mount, folder);
      // Opened first: a root-owned folder FTP made may not even be listable by the app user.
      if (const auto opened = OpenDropFolder(*volume, path); !opened.empty()) return Outcome(job, false, {}, opened);
      auto program = job.program;
      const auto check = CheckProgram(HostPath(path), program);
      if (check != ProgramCheck::kOk) return Outcome(job, false, {}, ProgramWords(check));
      if (config_.game_program)
        if (auto game = config_.game_program(HostPath(path), program);
            !game.empty() && game != program && CheckProgram(HostPath(path), game) == ProgramCheck::kOk) {
          Log("import program volume=" + job.volume + " folder=" + folder + " launcher=" + program + " game=" + game);
          program = std::move(game);
        }
      std::string title;
      for (const auto& candidate : candidates_)
        if (candidate.volume == job.volume && candidate.folder == folder) title = candidate.title;
      if (title.empty()) title = TitleForFolder(path);
      const auto added = store_.Add(job.volume, folder, title, program);
      if (!added.ok()) {
        Log("import add failed volume=" + job.volume + " folder=" + folder + ": " + added.message());
        return Outcome(job, false, {}, ImportWords(added.error()));
      }
      Log("import added id=" + added.value().id + " volume=" + job.volume + " folder=" + folder +
          " program=" + program);
      for (auto& candidate : candidates_)
        if (candidate.volume == job.volume && candidate.folder == folder) candidate.imported_id = added.value().id;
      // Numbered here, on this worker, before the game is listed: the main thread never waits for it.
      WarmRuntimeNumber(job.number_for, added.value().id);
      ++games_revision_;
      return Outcome(job, true, added.value().id, {});
    }
    case Command::kOpen: {
      const auto game = store_.Find(job.id);
      const auto* volume = game ? Volume(game->volume) : nullptr;
      // Only a game playing from the drop folder has files to open.
      if (!game || !volume || game->state == ImportState::kCopied) return Outcome(job, true, job.id, {});
      const auto opened = OpenDropFolder(*volume, Join(volume->mount, game->folder));
      return Outcome(job, opened.empty(), job.id, opened);
    }
    case Command::kCopy: {
      const auto game = store_.Find(job.id);
      const auto* volume = game ? Volume(game->volume) : nullptr;
      if (!game) return Outcome(job, false, job.id, ImportWords(ImportError::kNotFound));
      if (!config_.copy_into_playden) return Outcome(job, false, job.id, "Playden can't copy games here.");
      if (game->state == ImportState::kCopied) return Outcome(job, false, job.id, "The game is already in Playden.");
      if (game->state != ImportState::kInPlace) return Outcome(job, false, job.id, "The game is already being copied.");
      if (CheckPresence()) Scan();
      if (!volume || !present_.count(volume->volume)) return Outcome(job, false, job.id, "The game's drive is not connected.");
      const auto path = Join(volume->mount, game->folder);
      // A drop-folder game's files may be root-owned (FTP): opened first, as before a Play.
      if (const auto opened = OpenDropFolder(*volume, path); !opened.empty()) return Outcome(job, false, job.id, opened);
      const auto failure = config_.copy_into_playden(*game, HostPath(path), job.volume, job.runtime_number);
      if (!failure.empty()) {
        Log("import copy not queued id=" + job.id + ": " + failure);
        return Outcome(job, false, job.id, failure);
      }
      return Outcome(job, true, job.id, {});
    }
    case Command::kMove: {
      const auto game = store_.Find(job.id);
      const auto* volume = game ? Volume(game->volume) : nullptr;
      if (!game) return Outcome(job, false, job.id, ImportWords(ImportError::kNotFound));
      if (!config_.move_into_playden) return Outcome(job, false, job.id, "Playden can't move games here.");
      if (game->state == ImportState::kCopied) return Outcome(job, false, job.id, "The game is already in Playden.");
      if (game->state == ImportState::kCopyQueued) return Outcome(job, false, job.id, "The game is already being copied.");
      if (!volume || game->volume != ImportStore::kDropFolderVolume)
        return Outcome(job, false, job.id, "Only games in the drop folder can be moved. Copy it instead.");
      const auto path = Join(volume->mount, game->folder);
      // A move that stopped after its rename (copying) is finished as it is; a new one opens the
      // files first, as before a Play (FTP may have left root-owned ones).
      if (game->state == ImportState::kInPlace)
        if (const auto opened = OpenDropFolder(*volume, path); !opened.empty()) return Outcome(job, false, job.id, opened);
      const auto failure = config_.move_into_playden(*game, HostPath(path), job.runtime_number);
      ++games_revision_;
      Project();
      if (!failure.empty()) {
        Log("import move not done id=" + job.id + ": " + failure);
        return Outcome(job, false, job.id, failure);
      }
      return Outcome(job, true, job.id, {});
    }
    case Command::kReload:
      ++games_revision_;
      Project();
      return;
    case Command::kReconcile: {
      if (!store_.loaded()) return Outcome(job, false, {}, ImportWords(ImportError::kNotLoaded));
      const auto service = job.installs();
      bool changed = false;
      const auto installed = [&](const GameId& id) {
        return std::any_of(service.installed.begin(), service.installed.end(),
                           [&](const installs::InstalledGame& game) { return game.id == id; });
      };
      const auto available = [&](const GameId& id) {
        return std::any_of(service.installed.begin(), service.installed.end(),
                           [&](const installs::InstalledGame& game) { return game.id == id && game.available; });
      };
      const auto queued = [&](const GameId& id) {
        return std::any_of(service.jobs.begin(), service.jobs.end(),
                           [&](const installs::InstallJobView& view) { return view.id == id; });
      };
      std::set<std::string> numbered;  // asked for already, in this pass
      for (const auto& game : store_.Games()) {
        if (game.state != ImportState::kCopyQueued && game.state != ImportState::kCopying) continue;
        const auto id = GameId::Local(game.id);
        if (available(id)) {
          // The copy committed its install record; Playden stopped before imports.json said so.
          if (game.state == ImportState::kCopyQueued) store_.StartCopy(game.id);
          const auto finished = store_.FinishCopy(game.id);
          Log("import copy reconciled id=" + game.id + (finished.ok() ? " copied" : " failed: " + finished.message()));
          changed = true;
        } else if (installed(id)) {
          // Listed on a drive that isn't there (a copy queued there, or its folder missing): nothing
          // says whether it finished, so it stays as it is until the drive is back.
          Log("import copy reconcile waits id=" + game.id + ": its install is unavailable");
        } else if (!queued(id)) {
          // A move renames the drop folder away: one whose rename happened is finished, never put
          // back in place (its folder is in Playden now).
          const auto* volume = Volume(game.volume);
          if (volume && game.volume == ImportStore::kDropFolderVolume && config_.move_into_playden) {
            struct stat info {};
            const auto source = HostPath(Join(volume->mount, game.folder));
            if (::lstat(source.c_str(), &info) != 0 && errno == ENOENT) {
              const auto number = job.number_for ? job.number_for(id) : 0u;
              numbered.insert(game.id);
              const auto failure = config_.move_into_playden(game, source, number);
              Log("import move reconciled id=" + game.id + (failure.empty() ? " copied" : ": " + failure));
              changed = true;
              continue;
            }
          }
          store_.CancelCopy(game.id, "The copy stopped. Copy it again.");
          Log("import copy reconciled id=" + game.id + " in_place (no copy job)");
          changed = true;
        }
      }
      // A job left from a copy that failed or was put back in place (its entry moved first).
      for (const auto& view : service.jobs) {
        if (view.id.store != Store::kLocal || !job.cancel_job) continue;
        const auto game = store_.Find(view.id.value);
        if (game && game->state != ImportState::kInPlace) continue;
        Log("import copy job cancelled id=" + view.id.value + " (not being copied)");
        job.cancel_job(view.id);
      }
      // Copy plan folders no copy owns: a stop between the plan being saved and its job queued
      // (Enqueue), or a copy put back in place above. Its game is gone or plays in place, the
      // service has no job or install for it. Enqueue runs on this worker too, so none is half made.
      if (config_.remove_copy_folder)
        for (const auto& volume : service.volumes) {
          if (!volume.available || volume.games_root.empty()) continue;
          DIR* folder = ::opendir(volume.games_root.c_str());
          if (!folder) continue;
          std::vector<std::string> roots;
          while (const auto* item = ::readdir(folder)) {
            const auto key = ParseKey(item->d_name);
            if (!key || key->store != Store::kLocal || queued(*key) || installed(*key)) continue;
            const auto game = store_.Find(key->value);
            if (game && game->state != ImportState::kInPlace) continue;
            const auto root = volume.games_root + "/" + item->d_name;
            struct stat journal {};
            if (::lstat((root + "/" + kCopyJournalDir).c_str(), &journal) == 0 && S_ISDIR(journal.st_mode))
              roots.push_back(root);
          }
          ::closedir(folder);
          for (const auto& root : roots) {
            const bool removed = config_.remove_copy_folder(root);
            Log("import copy folder " + std::string(removed ? "removed" : "not removed") + " root=" + root +
                " (no copy owns it)");
          }
        }
      for (const auto& game : store_.Games())
        if (!numbered.count(game.id)) WarmRuntimeNumber(job.number_for, game.id);
      if (changed) ++games_revision_;
      return Outcome(job, true, {}, {});
    }
    case Command::kRemove: {
      const auto removed = store_.Remove(job.id);
      if (!removed.ok()) {
        Log("import remove failed id=" + job.id + ": " + removed.message());
        return Outcome(job, false, job.id, ImportWords(removed.error()));
      }
      Log("import removed id=" + job.id);
      for (auto& candidate : candidates_)
        if (candidate.imported_id == job.id) candidate.imported_id.clear();
      ++games_revision_;
      return Outcome(job, true, job.id, {});
    }
  }
}

void LocalLibrarySource::WarmRuntimeNumber(const std::function<std::uint32_t(const GameId&)>& given, const std::string& id) {
  std::function<std::uint32_t(const GameId&)> number_for = given;
  if (!number_for) {
    std::lock_guard<std::mutex> lock(mutex_);
    number_for = runtime_number_;
  }
  if (number_for) number_for(GameId::Local(id));
}

void LocalLibrarySource::Outcome(const Job& job, bool ok, std::string id, std::string message) {
  outcomes_.push_back({job.request, ok, std::move(id), std::move(message)});
  if (outcomes_.size() > kMaxOutcomes) outcomes_.erase(outcomes_.begin());
  Project();
}

bool LocalLibrarySource::CheckPresence() {
  std::set<std::string> present;
  for (const auto& volume : config_.volumes) {
    const bool here = config_.present ? config_.present(volume)
        : volume.require_mount_point ? IsMountPoint(HostPath(volume.mount)) : IsFolder(HostPath(volume.mount));
    if (here) present.insert(volume.volume);
  }
  if (present == present_) return false;
  std::string line = "import volumes present=";
  for (const auto& volume : present) line += volume + ",";
  Log(line);
  present_ = std::move(present);
  return true;
}

void LocalLibrarySource::Scan() {
  std::vector<ImportCandidate> candidates;
  for (const auto& volume : config_.volumes) {
    if (!present_.count(volume.volume)) continue;
    const auto root = Join(volume.mount, volume.folder);
    const auto listed = ScanImportRoot(HostPath(root));
    if (listed.status != ScanStatus::kOk) {
      Log("import scan root=" + root + " failed errno=" + std::to_string(listed.error));
      continue;
    }
    for (const auto& name : listed.folders) {
      ImportCandidate candidate;
      candidate.volume = volume.volume;
      candidate.folder = Join(volume.folder, name);
      candidate.path = Join(root, name);
      const auto scan = ScanGameFolder(HostPath(candidate.path));
      candidate.status = scan.status;
      candidate.title = scan.title.empty() ? TitleForFolder(candidate.path) : scan.title;
      candidate.programs = scan.programs;
      candidate.total_bytes = scan.total_bytes;
      candidate.file_count = scan.file_count;
      candidate.ascii_ok = IsPrintableAscii(candidate.path);
      candidate.partial = scan.limit_reached || scan.deeper_folders;
      if (const auto known = store_.FindByFolder(candidate.volume, candidate.folder)) candidate.imported_id = known->id;
      candidates.push_back(std::move(candidate));
    }
  }
  Log("import scan candidates=" + std::to_string(candidates.size()));
  candidates_ = std::move(candidates);
}

void LocalLibrarySource::Project() {
  auto next = std::make_shared<LocalSnapshot>();
  next->loaded = store_.loaded();
  next->load_error = load_error_;
  next->games = store_.Games();
  next->present_volumes = present_;
  next->candidates = candidates_;
  next->outcomes = outcomes_;
  next->games_revision = games_revision_;
  for (const auto& game : next->games) {
    // A game copied into Playden plays from Playden's own folder (a managed install).
    if (game.state == ImportState::kCopied) continue;
    const auto* volume = Volume(game.volume);
    if (!volume) continue;
    installs::InstalledLocation location;
    location.game = GameId::Local(game.id);
    location.root = Join(volume->mount, game.folder);
    location.volume_id = game.volume;
    location.ownership = installs::InstalledLocation::Ownership::kExternal;
    location.available = present_.count(game.volume) && IsFolder(HostPath(location.root));
    if (location.available && !IsPrintableAscii(location.root)) {
      location.available = false;
      location.reason = kNotAsciiReason;
    }
    next->locations.push_back(std::move(location));
  }
  std::lock_guard<std::mutex> lock(mutex_);
  next->scanning = scan_waiting_;
  next->revision = snapshot_->revision + 1;
  snapshot_ = std::move(next);
}

const ImportVolume* LocalLibrarySource::Volume(const std::string& id) const {
  for (const auto& volume : config_.volumes)
    if (volume.volume == id) return &volume;
  return nullptr;
}

std::string LocalLibrarySource::HostPath(const std::string& console_path) const {
  return config_.host_prefix.empty() ? console_path : config_.host_prefix + console_path;
}

std::string LocalLibrarySource::OpenDropFolder(const ImportVolume& volume, const std::string& path) {
  if (volume.volume != ImportStore::kDropFolderVolume || !config_.open_drop_folder) return {};
  return config_.open_drop_folder(path);
}

void LocalLibrarySource::Log(const std::string& line) const {
  if (config_.log) config_.log(line);
}

}  // namespace playden::stores::local
