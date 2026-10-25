#include "installs/install_service.h"
#include "steam/install_volumes.h"

#include <sys/stat.h>

#include <algorithm>
#include <chrono>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace playden::installs {
using steam::DownloadEstimate;
using steam::kInstallSpaceMargin;
using steam::PendingInstallBytes;
using steam::QueryVolumeSpace;
using steam::VolumeSpaceSupported;

std::vector<InstallRequest> ParseInstallRequests(const std::string& text) {
  std::vector<InstallRequest> requests;
  std::string_view remaining(text);
  while (!remaining.empty() && requests.size() < 64) {
    const auto newline = remaining.find('\n');
    auto line = remaining.substr(0, newline);
    remaining = newline == std::string_view::npos ? std::string_view{} : remaining.substr(newline + 1);
    bool malformed = false;
    // The next word; a quoted one runs to its closing quote and must be followed by a space or the end.
    const auto token = [&line, &malformed]() {
      const auto first = line.find_first_not_of(" \t\r\v\f");
      if (first == std::string_view::npos) { line = {}; return std::string_view{}; }
      line.remove_prefix(first);
      if (line.front() == '"') {
        const auto close = line.find('"', 1);
        if (close == std::string_view::npos || close == 1) { malformed = true; line = {}; return std::string_view{}; }
        const auto word = line.substr(1, close - 1);
        line.remove_prefix(close + 1);
        if (!line.empty() && line.find_first_of(" \t\r\v\f") != 0) malformed = true;
        return word;
      }
      const auto end = line.find_first_of(" \t\r\v\f");
      const auto word = line.substr(0, end);
      line = end == std::string_view::npos ? std::string_view{} : line.substr(end);
      return word;
    };
    auto id = token();
    if (id.empty()) continue;
    InstallRequest request;
    if (id == "import") {
      request.action = InstallRequest::Action::kImport;
      request.volume = std::string(token());
      request.folder = std::string(token());
      request.program = std::string(token());
      if (malformed || request.volume.empty() || request.folder.empty() || request.program.empty() || !token().empty() ||
          malformed)
        continue;
      requests.push_back(std::move(request));
      continue;
    }
    if (id == "copy") {
      // copy local-<uuid> [volume]
      request.action = InstallRequest::Action::kCopy;
      const auto game = ParseKey(token());
      request.volume = std::string(token());
      if (malformed || !game || game->store != Store::kLocal || !token().empty() || malformed) continue;
      request.game = *game;
      requests.push_back(std::move(request));
      continue;
    }
    if (id == "move") {
      // move local-<uuid>: a drop-folder game into Playden's games folder on the console storage
      request.action = InstallRequest::Action::kMove;
      const auto game = ParseKey(token());
      if (malformed || !game || game->store != Store::kLocal || !token().empty() || malformed) continue;
      request.game = *game;
      requests.push_back(std::move(request));
      continue;
    }
    if (id == "uninstall" || id == "cancel") {
      request.action = id == "uninstall" ? InstallRequest::Action::kUninstall : InstallRequest::Action::kCancel;
      id = token();
    }
    if (malformed || id.empty() || !token().empty() || malformed) continue;
    if (id.rfind("local-", 0) == 0) {
      // Imported games have no download to cancel.
      const auto game = ParseKey(id);
      if (!game || game->store != Store::kLocal || request.action == InstallRequest::Action::kCancel) continue;
      request.game = *game;
      requests.push_back(std::move(request));
      continue;
    }
    const auto parsed = std::from_chars(id.data(), id.data() + id.size(), request.app);
    if (parsed.ec != std::errc{} || parsed.ptr != id.data() + id.size() || request.app == 0) continue;
    request.game = GameId::Steam(request.app);
    requests.push_back(std::move(request));
  }
  return requests;
}

namespace {
// Every install Completed would list, newest first, without the `kMaxCompleted` cap.
std::vector<CompletedDownload> AllCompletedDownloads(const std::vector<InstalledGame>& installed,
                                                     const std::vector<std::pair<GameId, std::int64_t>>& dismissed,
                                                     std::int64_t now) {
  std::vector<CompletedDownload> completed;
  for (const auto& game : installed) {
    if (game.finished_at <= 0 || game.finished_at > now || now - game.finished_at > kCompletedDays * 24 * 60 * 60) continue;
    if (std::find(dismissed.begin(), dismissed.end(), std::make_pair(game.id, game.finished_at)) != dismissed.end()) continue;
    completed.push_back({game.id, game.install_bytes, game.volume_id, game.finished_at});
  }
  std::stable_sort(completed.begin(), completed.end(), [](const CompletedDownload& left, const CompletedDownload& right) {
    return left.finished_at > right.finished_at;
  });
  return completed;
}
}  // namespace

std::vector<CompletedDownload> CompletedDownloads(const std::vector<InstalledGame>& installed,
                                                  const std::vector<std::pair<GameId, std::int64_t>>& dismissed,
                                                  std::int64_t now) {
  auto completed = AllCompletedDownloads(installed, dismissed, now);
  if (completed.size() > kMaxCompleted) completed.resize(kMaxCompleted);
  return completed;
}

namespace {
std::int64_t RateNowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}

constexpr const char* kRecord = ".playden-install.json";
constexpr const char* kJournal = ".playden-download";
constexpr const char* kUpdateStaging = ".playden-update";
constexpr const char* kRegistry = ".playden-games";
constexpr const char* kDefaultVolume = ".playden-default-volume";
constexpr const char* kDismissed = ".playden-downloads-dismissed";
constexpr const char* kVolumeUnavailable = "The game's storage is not available.";
// What Downloads says while a job waits to try again, when its runner says nothing.
constexpr const char* kWaiting = "Waiting. It tries again by itself.";

bool Exists(const std::string& path) {
  struct stat info {};
  return stat(path.c_str(), &info) == 0;
}

// The install record is written when an install finishes, so its time is the finish time.
std::int64_t RecordTime(const std::string& path) {
  struct stat info {};
  return stat(path.c_str(), &info) == 0 ? static_cast<std::int64_t>(info.st_mtime) : 0;
}

std::int64_t WallNow() {
  return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::uint64_t RecordedBytes(const std::string& record_path) {
  FILE* file = std::fopen(record_path.c_str(), "r");
  if (!file) return 0;
  char text[4096] = {};
  const auto read = std::fread(text, 1, sizeof(text) - 1, file);
  std::fclose(file);
  const std::string_view view(text, read);
  const auto key = view.find("\"install_bytes\":");
  if (key == std::string_view::npos) return 0;
  const char* begin = view.data() + key + std::strlen("\"install_bytes\":");
  std::uint64_t value = 0;
  std::from_chars(begin, view.data() + view.size(), value);
  return value;
}

// Volume ids are short identifiers written into the registry; anything else is refused.
bool ValidVolumeId(std::string_view id) {
  if (id.empty() || id.size() > 32) return false;
  for (const char value : id)
    if (!((value >= 'a' && value <= 'z') || (value >= '0' && value <= '9') || value == '-' || value == '_')) return false;
  return true;
}

struct RegistryEntry {
  GameId id;
  std::string volume_id;
  std::uint64_t install_bytes = 0;
  bool queued = false;  // written for a job in Downloads: "<id>\t<volume>\t0"
  bool paused = false;  // "<id>\t<volume>\t0\tpaused": the player paused it, or it failed
  bool removing = false;  // "<id>\t<volume>\t<install bytes>\tremoving": uninstalled, folder not deleted yet
  // "<id>\t<volume>\t0\tupdate" (or "update-paused"), after the game's own line: an update in Downloads.
  bool update_job = false;
  bool update_paused = false;
};

// One line per game: "<key>" (legacy, primary volume) or "<key>\t<volume id>\t<install bytes>",
// followed by "\tpaused" for a job that should wait for Resume after a restart, or "\tremoving" for
// an uninstall whose folder is not deleted yet, or "\tupdate"/"\tupdate-paused" for an installed
// game's update job (a second line for the game). The key is ToKey: a Steam game's app ID,
// "local-<uuid>" for an imported game.
std::optional<RegistryEntry> ParseRegistryLine(std::string_view line) {
  RegistryEntry entry;
  const auto first_tab = line.find('\t');
  const auto id = ParseKey(line.substr(0, first_tab));
  if (!id) return std::nullopt;
  entry.id = *id;
  if (first_tab == std::string_view::npos) return entry;
  const auto rest = line.substr(first_tab + 1);
  const auto second_tab = rest.find('\t');
  const auto volume = rest.substr(0, second_tab);
  if (!ValidVolumeId(volume)) return std::nullopt;
  entry.volume_id = std::string(volume);
  if (second_tab != std::string_view::npos) {
    const auto fields = rest.substr(second_tab + 1);
    const auto third_tab = fields.find('\t');
    const auto bytes = fields.substr(0, third_tab);
    std::from_chars(bytes.data(), bytes.data() + bytes.size(), entry.install_bytes);
    entry.queued = entry.install_bytes == 0;
    const auto flag = third_tab == std::string_view::npos ? std::string_view{} : fields.substr(third_tab + 1);
    entry.paused = flag == "paused";
    entry.removing = flag == "removing";
    entry.update_job = flag == "update" || flag == "update-paused";
    entry.update_paused = flag == "update-paused";
  }
  return entry;
}

}  // namespace

InstallService::InstallService(InstallRunner& steam, std::string games_root)
    : InstallService(Runners{{Store::kSteam, &steam}},
                     std::vector<InstallVolume>{{kPrimaryVolumeId, "Console storage", std::move(games_root), true}}) {}

InstallService::InstallService(InstallRunner& steam, std::vector<InstallVolume> volumes)
    : InstallService(Runners{{Store::kSteam, &steam}}, std::move(volumes)) {}

InstallService::InstallService(Runners runners, std::vector<InstallVolume> volumes)
    : runners_(std::move(runners)), volumes_(std::move(volumes)) {
  if (volumes_.empty()) volumes_.push_back({kPrimaryVolumeId, "Console storage", "games", true});
  volumes_.front().available = true;
  default_volume_id_ = volumes_.front().id;
  Scan();
  worker_ = std::thread([this] { Run(); });
}

InstallService::~InstallService() { Shutdown(); }

InstallRunner* InstallService::RunnerFor(Store store) const {
  const auto found = runners_.find(store);
  return found == runners_.end() ? nullptr : found->second;
}

void InstallService::ClearWait(Job& job) {
  if (job.waiting) job.view.failure.clear();
  job.waiting = false;
  job.not_before = {};
  job.retry_delay = std::chrono::milliseconds{0};
}

void InstallService::RetryWaiting(Store store) {
  std::lock_guard<std::mutex> lock(mutex_);
  bool woken = false;
  for (auto& job : jobs_)
    if (job.waiting && job.view.id.store == store && job.view.state == InstallJobState::kQueued) {
      // What Downloads says while it waits stays until the job runs.
      job.not_before = {};
      job.retry_delay = std::chrono::milliseconds{0};
      woken = true;
    }
  if (woken) wake_.notify_all();
}

void InstallService::SetRetryDelays(std::chrono::milliseconds first, std::chrono::milliseconds most) {
  std::lock_guard<std::mutex> lock(mutex_);
  first_retry_ = first;
  most_retry_ = std::max(first, most);
}

const InstallVolume* InstallService::FindVolume(const std::string& id) const {
  for (const auto& volume : volumes_)
    if (volume.id == id) return &volume;
  return nullptr;
}

std::string InstallService::RootOn(const std::string& volume_id, GameId id) const {
  const auto* volume = FindVolume(volume_id);
  return volume && id.valid() ? volume->games_root + "/" + ToKey(id) : std::string();
}

std::string InstallService::LockedRootFor(GameId id) const {
  for (const auto& job : jobs_)
    if (job.view.id == id) return RootOn(job.view.volume_id, id);
  for (const auto& game : installed_)
    if (game.id == id) return RootOn(game.volume_id, id);
  return RootOn(default_volume_id_, id);
}

std::string InstallService::RootFor(GameId id) {
  std::lock_guard<std::mutex> lock(mutex_);
  return LockedRootFor(id);
}

// The primary volume keeps a registry of app IDs Playden manages and the volume of each;
// directory listing is avoided because the console's readdir did not return entries reliably.
void InstallService::Scan() {
  const auto& primary = volumes_.front().games_root;
  std::vector<RegistryEntry> entries;
  if (FILE* registry = std::fopen((primary + "/" + kRegistry).c_str(), "r")) {
    char line[160];
    while (std::fgets(line, sizeof(line), registry) && entries.size() < kMaxJobs * 4) {
      const auto length = std::strcspn(line, "\r\n");
      auto entry = ParseRegistryLine(std::string_view(line, length));
      if (!entry) continue;
      if (entry->volume_id.empty()) entry->volume_id = volumes_.front().id;
      const auto existing = std::find_if(entries.begin(), entries.end(), [&](const RegistryEntry& other) { return other.id == entry->id; });
      if (existing == entries.end()) {
        entries.push_back(std::move(*entry));
      } else if (entry->update_job) {
        existing->update_job = true;
        existing->update_paused = entry->update_paused;
      }
    }
    std::fclose(registry);
  }
  if (FILE* file = std::fopen((primary + "/" + kDefaultVolume).c_str(), "r")) {
    char line[64] = {};
    if (std::fgets(line, sizeof(line), file)) {
      const std::string id(line, std::strcspn(line, "\r\n"));
      if (const auto* volume = FindVolume(id); volume && volume->available) default_volume_id_ = id;
    }
    std::fclose(file);
  }
  for (const auto& entry : entries) {
    const auto* volume = FindVolume(entry.volume_id);
    if (entry.removing) {
      // An uninstall Playden stopped before the folder was gone carries on; one on an absent
      // drive waits for the drive.
      InstalledGame game{entry.id, entry.install_bytes, RootOn(entry.volume_id, entry.id), entry.volume_id,
                         volume && volume->available};
      uninstalling_.push_back(game);
      if (game.available) removals_.push_back({entry.id.store, game.root, std::nullopt, game});
      continue;
    }
    if (!volume || !volume->available) {
      // Keep games on an absent drive listed so they come back when it does.
      installed_.push_back({entry.id, entry.install_bytes, RootOn(entry.volume_id, entry.id), entry.volume_id, false});
      continue;
    }
    const auto root = RootOn(entry.volume_id, entry.id);
    const bool staged_update = Exists(root + "/" + kUpdateStaging);
    if (Exists(root + "/" + kRecord)) {
      installed_.push_back({entry.id, RecordedBytes(root + "/" + kRecord), root, entry.volume_id, true,
                            RecordTime(root + "/" + kRecord)});
      // Its update in Downloads comes back as the player left it.
      if (entry.update_job && jobs_.size() < kMaxJobs) {
        Job job;
        job.view.id = entry.id;
        job.view.update = true;
        job.view.volume_id = entry.volume_id;
        job.view.state = entry.update_paused ? InstallJobState::kPaused : InstallJobState::kQueued;
        if (entry.update_paused) job.view.failure = "Interrupted. Resume to continue.";
        jobs_.push_back(job);
      }
    } else if (staged_update && !Exists(root + "/" + kJournal) && jobs_.size() < kMaxJobs) {
      // An update stopped while its files were swapped in, which removes the install record first: it
      // finishes by itself.
      Job job;
      job.view.id = entry.id;
      job.view.update = true;
      job.view.volume_id = entry.volume_id;
      job.view.state = InstallJobState::kQueued;
      jobs_.push_back(job);
    } else if ((Exists(root + "/" + kJournal) || entry.queued) && jobs_.size() < kMaxJobs) {
      // A job that failed or waited before writing anything (no journal yet) stays in
      // Downloads too, so a failure while Steam connects doesn't lose the queued game. A job that
      // was queued or downloading when Playden stopped (a game started, or Playden restarted)
      // carries on by itself; one the player paused, or that failed, waits for Resume.
      Job job;
      job.view.id = entry.id;
      if (entry.queued && !entry.paused) {
        job.view.state = InstallJobState::kQueued;
      } else {
        job.view.state = InstallJobState::kPaused;
        job.view.failure = "Interrupted. Resume to continue.";
      }
      job.view.volume_id = entry.volume_id;
      jobs_.push_back(job);
    } else if (entry.install_bytes > 0) {
      // The drive is there but the game's folder isn't (moved aside or renamed): keep the game
      // listed as unavailable, so it comes back with its folder instead of being forgotten at
      // the next save.
      installed_.push_back({entry.id, entry.install_bytes, root, entry.volume_id, false});
    }
  }
  std::sort(installed_.begin(), installed_.end(),
            [](const InstalledGame& left, const InstalledGame& right) { return left.id < right.id; });
  // "<key>\t<finish time>" per finished download the player took off Completed.
  if (FILE* file = std::fopen((primary + "/" + kDismissed).c_str(), "r")) {
    char line[128];
    while (std::fgets(line, sizeof(line), file) && dismissed_.size() < kMaxJobs * 4) {
      const std::string_view text(line, std::strcspn(line, "\r\n"));
      const auto tab = text.find('\t');
      if (tab == std::string_view::npos) continue;
      const auto id = ParseKey(text.substr(0, tab));
      const auto time = text.substr(tab + 1);
      long long finished = 0;
      const auto parsed = std::from_chars(time.data(), time.data() + time.size(), finished);
      if (id && parsed.ec == std::errc{} && parsed.ptr == time.data() + time.size() && finished > 0)
        dismissed_.emplace_back(*id, static_cast<std::int64_t>(finished));
    }
    std::fclose(file);
  }
  ++revision_;
}

void InstallService::SaveRegistry() {
  if (!default_volume_saved_) SaveDefaultVolume();
  const auto& primary = volumes_.front().games_root;
  mkdir(primary.c_str(), 0777);
  const auto path = primary + "/" + kRegistry;
  const auto temporary = path + ".tmp";
  FILE* registry = std::fopen(temporary.c_str(), "w");
  if (!registry) return;
  for (const auto& game : installed_)
    std::fprintf(registry, "%s\t%s\t%llu\n", ToKey(game.id).c_str(), game.volume_id.c_str(),
                 static_cast<unsigned long long>(game.install_bytes));
  for (const auto& job : jobs_) {
    const bool waits = job.view.state == InstallJobState::kPaused || job.view.state == InstallJobState::kFailed;
    const char* flag = job.view.update ? (waits ? "\tupdate-paused" : "\tupdate") : waits ? "\tpaused" : "";
    std::fprintf(registry, "%s\t%s\t0%s\n", ToKey(job.view.id).c_str(), job.view.volume_id.c_str(), flag);
  }
  for (const auto& game : uninstalling_)
    std::fprintf(registry, "%s\t%s\t%llu\tremoving\n", ToKey(game.id).c_str(), game.volume_id.c_str(),
                 static_cast<unsigned long long>(game.install_bytes));
  if (std::fclose(registry) == 0) std::rename(temporary.c_str(), path.c_str());
}

void InstallService::SaveDefaultVolume() {
  const auto& primary = volumes_.front().games_root;
  mkdir(primary.c_str(), 0777);
  const auto path = primary + "/" + kDefaultVolume;
  const auto temporary = path + ".tmp";
  FILE* file = std::fopen(temporary.c_str(), "w");
  // A full disk refuses the write; the choice still holds for this session and is saved again
  // with the registry.
  bool saved = file && std::fprintf(file, "%s\n", default_volume_id_.c_str()) > 0;
  if (file) saved = std::fclose(file) == 0 && saved;
  saved = saved && std::rename(temporary.c_str(), path.c_str()) == 0;
  if (!saved) std::remove(temporary.c_str());
  default_volume_saved_ = saved;
}

void InstallService::SaveDismissed() {
  // Only dismissals that still hide an install are kept.
  dismissed_.erase(std::remove_if(dismissed_.begin(), dismissed_.end(), [this](const std::pair<GameId, std::int64_t>& entry) {
    return std::none_of(installed_.begin(), installed_.end(), [&entry](const InstalledGame& game) {
      return game.id == entry.first && game.finished_at == entry.second;
    });
  }), dismissed_.end());
  const auto& primary = volumes_.front().games_root;
  mkdir(primary.c_str(), 0777);
  const auto path = primary + "/" + kDismissed;
  const auto temporary = path + ".tmp";
  FILE* file = std::fopen(temporary.c_str(), "w");
  if (!file) return;
  for (const auto& [id, finished] : dismissed_)
    std::fprintf(file, "%s\t%lld\n", ToKey(id).c_str(), static_cast<long long>(finished));
  if (std::fclose(file) == 0) std::rename(temporary.c_str(), path.c_str());
}

void InstallService::Dismiss(GameId id) {
  for (const auto& game : installed_) {
    if (game.id != id || game.finished_at <= 0) continue;
    const std::pair<GameId, std::int64_t> entry{id, game.finished_at};
    if (std::find(dismissed_.begin(), dismissed_.end(), entry) == dismissed_.end()) dismissed_.push_back(entry);
  }
}

void InstallService::DismissCompleted(GameId id) {
  std::lock_guard<std::mutex> lock(mutex_);
  Dismiss(id);
  SaveDismissed();
  ++revision_;
}

void InstallService::ClearCompleted() {
  std::lock_guard<std::mutex> lock(mutex_);
  // Every finished download, not only the `kMaxCompleted` the list shows: the older ones would
  // move up into the list.
  for (const auto& completed : AllCompletedDownloads(installed_, dismissed_, WallNow())) Dismiss(completed.id);
  SaveDismissed();
  ++revision_;
}

InstallService::Job* InstallService::Find(GameId id) {
  for (auto& job : jobs_)
    if (job.view.id == id) return &job;
  return nullptr;
}

void InstallService::Publish() {
  ++revision_;
  SaveRegistry();
}

void InstallService::Install(GameId id, std::optional<std::uint64_t> estimated_bytes, const std::string& volume_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  // Only a store with a runner installs.
  if (stopping_ || !id.valid() || !RunnerFor(id.store) || Find(id) || jobs_.size() >= kMaxJobs) return;
  if (std::any_of(installed_.begin(), installed_.end(), [id](const InstalledGame& game) { return game.id == id; })) return;
  // A game whose files are still being deleted can be installed again once they are gone.
  if (std::any_of(uninstalling_.begin(), uninstalling_.end(), [id](const InstalledGame& game) { return game.id == id; })) return;
  Job job;
  job.view.id = id;
  const auto& target = volume_id.empty() ? default_volume_id_ : volume_id;
  job.view.volume_id = target;
  job.estimated_bytes = estimated_bytes;
  const auto* volume = FindVolume(target);
  const auto space = volume && volume->available ? QueryVolumeSpace(volume->games_root) : std::nullopt;
  if (!volume || !volume->available) {
    job.view.state = InstallJobState::kFailed;
    job.view.failure = "The storage is disconnected.";
  } else if (!space) {
    if (VolumeSpaceSupported()) {
      job.view.state = InstallJobState::kFailed;
      job.view.failure = "Free space is unavailable. Check the storage and try again.";
    }
  } else {
    auto available = space->free_bytes;
    // Free space already counts what other downloads have written; reserve only the rest.
    for (const auto& pending : jobs_) {
      if (pending.view.volume_id != target || pending.cancel_requested ||
          pending.view.state == InstallJobState::kFailed) continue;
      const auto reserved = PendingInstallBytes(pending.estimated_bytes, pending.view.held_bytes);
      available -= std::min(available, reserved);
    }
    if (available < kInstallSpaceMargin || estimated_bytes.value_or(0) > available - kInstallSpaceMargin) {
      job.view.state = InstallJobState::kFailed;
      job.view.failure = "Not enough free space after reserving space for other downloads.";
    }
  }
  jobs_.push_back(job);
  Publish();
  wake_.notify_all();
}

void InstallService::Pause(GameId id) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto* job = Find(id);
  if (!job) return;
  if (active_ == id) active_cancel_.store(true);
  job->view.state = InstallJobState::kPaused;
  job->rate.Reset(); job->view.bytes_per_second.reset(); job->view.eta_seconds.reset();
  job->view.failure.clear();
  job->waiting = false;
  Publish();
  wake_.notify_all();
}

void InstallService::Resume(GameId id) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto* job = Find(id);
  if (!job || job->view.state == InstallJobState::kRunning) return;
  // The player asks for it now: a job that waited to try again tries at once.
  ClearWait(*job);
  job->view.state = InstallJobState::kQueued;
  job->view.failure.clear();
  Publish();
  wake_.notify_all();
}

void InstallService::DownloadNow(GameId id) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = std::find_if(jobs_.begin(), jobs_.end(), [id](const Job& job) { return job.view.id == id; });
  if (found == jobs_.end() || found->view.state == InstallJobState::kRunning || found->cancel_requested) return;
  Job job = *found;
  jobs_.erase(found);
  ClearWait(job);
  job.view.state = InstallJobState::kQueued;
  job.view.failure.clear();
  jobs_.push_front(job);
  // The running download stops and goes back in the queue right behind it, so it resumes next.
  if (active_ && *active_ != id) {
    const auto running = std::find_if(jobs_.begin(), jobs_.end(), [this](const Job& job) { return job.view.id == *active_; });
    if (running != jobs_.end() && !running->cancel_requested) {
      Job stopped = *running;
      jobs_.erase(running);
      stopped.view.state = InstallJobState::kQueued;
      stopped.view.failure.clear();
      jobs_.insert(jobs_.begin() + 1, stopped);
      active_cancel_.store(true);
    }
  }
  Publish();
  wake_.notify_all();
}

void InstallService::MoveQueued(GameId id, bool up) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto movable = [this](const Job& job) {
    return job.view.state == InstallJobState::kQueued && !job.cancel_requested && active_ != job.view.id;
  };
  const auto found = std::find_if(jobs_.begin(), jobs_.end(), [id](const Job& job) { return job.view.id == id; });
  if (found == jobs_.end() || !movable(*found)) return;
  auto other = found;
  while (up ? other != jobs_.begin() : other + 1 != jobs_.end()) {
    if (up) --other; else ++other;
    if (!movable(*other)) continue;
    std::iter_swap(found, other);
    Publish();
    wake_.notify_all();
    return;
  }
}

void InstallService::Cancel(GameId id) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto* job = Find(id);
  if (!job) return;
  const bool installed = std::any_of(installed_.begin(), installed_.end(), [id](const InstalledGame& game) { return game.id == id; });
  // An update whose files were being swapped in can only finish.
  if (job->view.update && !installed) return;
  job->cancel_requested = true;
  if (active_ == id) {
    active_cancel_.store(true);
  } else {
    const auto* volume = FindVolume(job->view.volume_id);
    const auto root = volume && volume->available ? RootOn(job->view.volume_id, id) : std::string();
    const bool update = job->view.update;
    jobs_.erase(std::remove_if(jobs_.begin(), jobs_.end(), [id](const Job& entry) { return entry.view.id == id; }),
                jobs_.end());
    // A canceled update keeps the installed game and deletes only what it staged.
    if (!root.empty()) QueueRemoval({id.store, root, std::nullopt, std::nullopt, update});
  }
  Publish();
  wake_.notify_all();
}

void InstallService::Uninstall(GameId id) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = std::find_if(installed_.begin(), installed_.end(), [id](const InstalledGame& game) { return game.id == id; });
  if (found == installed_.end()) return;
  // Nothing can be deleted on an absent drive. A game whose folder is missing from a present
  // drive is only forgotten: whatever its path names now isn't this install, so nothing is removed.
  const auto* volume = FindVolume(found->volume_id);
  if (!found->available && (!volume || !volume->available)) return;
  const auto game = *found;
  installed_.erase(found);
  updates_.erase(id);
  // Its update goes with it; a running one stops first, then the folder is deleted.
  if (auto* job = Find(id)) {
    if (active_ == id) {
      job->cancel_requested = true;
      active_cancel_.store(true);
    } else {
      jobs_.erase(std::remove_if(jobs_.begin(), jobs_.end(), [id](const Job& entry) { return entry.view.id == id; }),
                  jobs_.end());
    }
  }
  // Its runner still hears of a game whose folder is missing, to forget what it keeps of the game.
  if (game.available) QueueRemoval({id.store, RootOn(game.volume_id, id), std::nullopt, game});
  else QueueRemoval({id.store, {}, id, std::nullopt});
  Publish();
  wake_.notify_all();
}

void InstallService::Update(GameId id) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (stopping_ || !RunnerFor(id.store) || Find(id) || jobs_.size() >= kMaxJobs || !updates_.count(id)) return;
  const auto found = std::find_if(installed_.begin(), installed_.end(), [id](const InstalledGame& game) { return game.id == id; });
  if (found == installed_.end() || !found->available) return;
  Job job;
  job.view.id = id;
  job.view.update = true;
  job.view.volume_id = found->volume_id;
  jobs_.push_back(job);
  Publish();
  wake_.notify_all();
}

void InstallService::CheckUpdates() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (stopping_) return;
  check_updates_ = true;
  wake_.notify_all();
}

bool InstallService::Adopt(GameId id, const std::string& volume_id, std::uint64_t install_bytes) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (std::any_of(installed_.begin(), installed_.end(), [id](const InstalledGame& game) { return game.id == id; }))
    return true;
  const auto* volume = FindVolume(volume_id);
  if (stopping_ || !id.valid() || !RunnerFor(id.store) || Find(id) || !volume || !volume->available) return false;
  if (std::any_of(uninstalling_.begin(), uninstalling_.end(), [id](const InstalledGame& game) { return game.id == id; }))
    return false;
  const auto root = RootOn(volume_id, id);
  if (!Exists(root + "/" + kRecord)) return false;
  installed_.push_back({id, install_bytes, root, volume_id, true, RecordTime(root + "/" + kRecord)});
  std::sort(installed_.begin(), installed_.end(),
            [](const InstalledGame& left, const InstalledGame& right) { return left.id < right.id; });
  Publish();
  return true;
}

// Callers hold the lock. An uninstall does not wait for the running download, which can take
// hours: the download steps back into the queue, keeping its place, and carries on afterwards.
// Other removals (a canceled download's folder, a game to forget) wait for it.
void InstallService::QueueRemoval(Removal removal) {
  const bool uninstall = removal.game.has_value();
  if (uninstall) uninstalling_.push_back(*removal.game);
  removals_.push_back(std::move(removal));
  if (!uninstall || !active_) return;
  if (auto* running = Find(*active_); running && running->view.state == InstallJobState::kRunning && !running->cancel_requested) {
    running->view.state = InstallJobState::kQueued;
    running->rate.Reset(); running->view.bytes_per_second.reset(); running->view.eta_seconds.reset();
    active_cancel_.store(true);
  }
}

std::vector<RemovalFailure> InstallService::TakeRemovalFailures() {
  std::lock_guard<std::mutex> lock(mutex_);
  return std::exchange(removal_failures_, {});
}

void InstallService::SetDefaultVolume(const std::string& volume_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto* volume = FindVolume(volume_id);
  if (!volume || !volume->available || volume_id == default_volume_id_) return;
  default_volume_id_ = volume_id;
  SaveDefaultVolume();
  ++revision_;
}

InstallSnapshot InstallService::Snapshot() {
  std::lock_guard<std::mutex> lock(mutex_);
  InstallSnapshot snapshot;
  const auto now = RateNowMs();
  for (auto& job : jobs_) {
    DownloadEstimate estimate;
    if (job.view.state == InstallJobState::kRunning)
      estimate = job.rate.Shown(now, job.view.done_bytes, job.view.total_bytes);
    if (job.view.bytes_per_second != estimate.bytes_per_second || job.view.eta_seconds != estimate.eta_seconds) {
      job.view.bytes_per_second = estimate.bytes_per_second; job.view.eta_seconds = estimate.eta_seconds;
      ++revision_; // Time-only telemetry changes do not write the install registry.
    }
  }
  snapshot.revision = revision_;
  for (const auto& job : jobs_) snapshot.jobs.push_back(job.view);
  snapshot.installed = installed_;
  for (auto& game : snapshot.installed) game.update_available = updates_.count(game.id) > 0;
  for (const auto& game : uninstalling_) snapshot.uninstalling.push_back(game.id);
  snapshot.completed = CompletedDownloads(installed_, dismissed_, WallNow());
  snapshot.volumes = volumes_;
  snapshot.default_volume_id = default_volume_id_;
  snapshot.default_volume_saved = default_volume_saved_;
  return snapshot;
}

void InstallService::Shutdown() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_ && !worker_.joinable()) return;
    stopping_ = true;
    active_cancel_.store(true);
  }
  wake_.notify_all();
  if (worker_.joinable()) worker_.join();
}

void InstallService::Run() {
  for (;;) {
    std::optional<GameId> next;
    InstallRunner* runner = nullptr;
    std::string next_root;
    std::string next_games_root;
    std::optional<Removal> removal;
    std::map<Store, std::vector<InstalledGame>> update_checks;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      // The first queued job whose wait is over runs; jobs waiting to try again are skipped, and the
      // worker sleeps until the earliest of them or a request (Install, Pause, Resume, Cancel,
      // Shutdown) wakes it.
      for (;;) {
        if (stopping_) return;
        if (!removals_.empty() || check_updates_) break;
        const auto now = std::chrono::steady_clock::now();
        std::optional<std::chrono::steady_clock::time_point> earliest;
        bool ready = false;
        for (const auto& job : jobs_) {
          if (job.view.state != InstallJobState::kQueued) continue;
          if (job.not_before <= now) { ready = true; break; }
          if (!earliest || job.not_before < *earliest) earliest = job.not_before;
        }
        if (ready) break;
        if (earliest) wake_.wait_until(lock, *earliest);
        else wake_.wait(lock);
      }
      if (!removals_.empty()) {
        removal = removals_.front();
        removals_.pop_front();
      } else if (check_updates_) {
        check_updates_ = false;
        active_cancel_.store(false);  // Shutdown stops the check
        for (const auto& game : installed_)
          if (game.available && RunnerFor(game.id.store)) update_checks[game.id.store].push_back(game);
      } else {
        const auto now = std::chrono::steady_clock::now();
        for (auto& job : jobs_) {
          if (job.view.state != InstallJobState::kQueued || job.not_before > now) continue;
          const auto* volume = FindVolume(job.view.volume_id);
          runner = RunnerFor(job.view.id.store);
          if (!runner) {
            // A store without a runner can't install; its job in the registry waits as failed.
            job.view.state = InstallJobState::kFailed;
            job.view.failure = "This game can't be downloaded.";
            Publish();
            continue;
          }
          if (!volume || !volume->available) {
            job.view.state = InstallJobState::kFailed;
            job.view.failure = kVolumeUnavailable;
            Publish();
            continue;
          }
          job.view.state = InstallJobState::kRunning;
          // Trying again: what it said while it waited no longer applies.
          if (job.waiting) { job.waiting = false; job.view.failure.clear(); }
          job.rate.Reset();
          job.view.bytes_per_second.reset(); job.view.eta_seconds.reset();
          job.view.step = InstallStep::kStarting;
          job.view.step_since = std::chrono::steady_clock::now();
          next = job.view.id;
          next_root = RootOn(job.view.volume_id, job.view.id);
          next_games_root = volume->games_root;
          active_ = next;
          active_cancel_.store(false);
          Publish();
          break;
        }
      }
    }
    if (!update_checks.empty()) {
      for (const auto& [store, games] : update_checks) {
        auto* checker = RunnerFor(store);
        const auto found = checker ? checker->FindUpdates(games, active_cancel_) : std::nullopt;
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) return;
        // Not known now: what the last check found stands.
        if (!found) continue;
        for (const auto& game : games) updates_.erase(game.id);
        for (const auto id : *found)
          if (std::any_of(installed_.begin(), installed_.end(), [id](const InstalledGame& game) { return game.id == id; }))
            updates_.insert(id);
        ++revision_;
      }
      continue;
    }
    if (removal) {
      std::string failure;
      if (auto* remover = RunnerFor(removal->store)) {
        if (removal->discard_update) remover->DiscardUpdate(removal->root);
        else if (removal->forget) remover->Forget(*removal->forget);
        else failure = remover->Remove(removal->root);
      } else if (removal->game) {
        failure = "Playden can't delete this game's files.";
      }
      std::lock_guard<std::mutex> lock(mutex_);
      if (const auto& game = removal->game) {
        uninstalling_.erase(std::remove_if(uninstalling_.begin(), uninstalling_.end(),
                                           [&](const InstalledGame& entry) { return entry.id == game->id; }),
                            uninstalling_.end());
        if (!failure.empty()) {
          // The folder keeps its install record until everything else is gone, so the game is
          // listed again and uninstalling it again tries again.
          if (!Find(game->id)) {
            installed_.push_back(*game);
            std::sort(installed_.begin(), installed_.end(),
                      [](const InstalledGame& left, const InstalledGame& right) { return left.id < right.id; });
          }
          removal_failures_.push_back({game->id, failure});
        }
      }
      Publish();
      continue;
    }
    if (!next) continue;

    // The volume's folder may have been made over FTP and not be writable by the app user.
    if (folder_check_) {
      const std::string unwritable = folder_check_(next_games_root);
      if (!unwritable.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        active_.reset();
        if (auto* job = Find(*next); job && job->view.state == InstallJobState::kRunning) {
          ClearWait(*job);
          job->view.state = InstallJobState::kFailed;
          job->view.failure = unwritable;
        }
        Publish();
        continue;
      }
    }

    const auto result = runner->Install(*next, next_root, active_cancel_,
                                        [this, id = *next](std::uint64_t done, std::uint64_t total, std::uint64_t held) {
                                          std::lock_guard<std::mutex> lock(mutex_);
                                          if (auto* job = Find(id)) {
                                            // Under way: a later wait starts short again.
                                            job->retry_delay = std::chrono::milliseconds{0};
                                            if (job->view.state == InstallJobState::kRunning) job->rate.Record(done, RateNowMs());
                                            job->view.done_bytes = done;
                                            job->view.total_bytes = total;
                                            job->view.held_bytes = held;
                                            ++revision_;  // progress only; the registry is unchanged
                                          }
                                        },
                                        [this, id = *next](InstallStep step) {
                                          std::lock_guard<std::mutex> lock(mutex_);
                                          if (auto* job = Find(id)) job->retry_delay = std::chrono::milliseconds{0};
                                          if (auto* job = Find(id); job && job->view.step != step) {
                                            job->view.step = step;
                                            job->view.step_since = std::chrono::steady_clock::now();
                                            ++revision_;
                                          }
                                        });

    bool remove_files = false;
    bool discard_update = false;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      active_.reset();
      auto* job = Find(*next);
      if (job && result.ok) {
        // Installed, even when a cancel arrived meanwhile: it came too late (a copy may already have
        // deleted its source), so the game stays and can be uninstalled. An update replaces the game's
        // entry, unless the game is being uninstalled.
        const auto volume_id = job->view.volume_id;
        jobs_.erase(std::remove_if(jobs_.begin(), jobs_.end(), [&](const Job& entry) { return entry.view.id == *next; }),
                    jobs_.end());
        updates_.erase(*next);
        const bool removing = std::any_of(uninstalling_.begin(), uninstalling_.end(),
                                          [&](const InstalledGame& game) { return game.id == *next; });
        installed_.erase(std::remove_if(installed_.begin(), installed_.end(), [&](const InstalledGame& game) { return game.id == *next; }),
                         installed_.end());
        if (!removing) {
          installed_.push_back({*next, result.install_bytes, next_root, volume_id, true, WallNow()});
          std::sort(installed_.begin(), installed_.end(),
                    [](const InstalledGame& left, const InstalledGame& right) { return left.id < right.id; });
        }
      } else if (job && job->cancel_requested) {
        discard_update = job->view.update;
        jobs_.erase(std::remove_if(jobs_.begin(), jobs_.end(), [&](const Job& entry) { return entry.view.id == *next; }),
                    jobs_.end());
        remove_files = true;
      } else if (job && job->view.state == InstallJobState::kRunning && result.canceled && stopping_) {
        // Playden is closing: the download stays queued, so the next start carries on with it.
        job->view.state = InstallJobState::kQueued;
      } else if (job && job->view.state == InstallJobState::kRunning && !result.ok && !result.canceled && result.retry_later) {
        // Not yet (no sign-in, or the store is out of reach, as it often is for a moment after Playden
        // starts): the job keeps its place and tries again after a wait that doubles each time, while
        // the worker goes on with the other jobs.
        job->view.state = InstallJobState::kQueued;
        job->view.failure = result.waiting.empty() ? kWaiting : result.waiting;
        job->waiting = true;
        job->retry_delay = job->retry_delay.count() == 0 ? first_retry_ : std::min(job->retry_delay * 2, most_retry_);
        job->not_before = std::chrono::steady_clock::now() + job->retry_delay;
        job->rate.Reset(); job->view.bytes_per_second.reset(); job->view.eta_seconds.reset();
      } else if (job && job->view.state == InstallJobState::kRunning) {
        // A Pause or Resume that arrived while the run was stopping already chose the state.
        ClearWait(*job);
        job->view.state = result.canceled ? InstallJobState::kPaused : InstallJobState::kFailed;
        job->view.failure = result.canceled ? std::string() : result.failure;
      }
      Publish();
    }
    if (remove_files && discard_update) runner->DiscardUpdate(next_root);
    else if (remove_files) runner->Remove(next_root);
  }
}

}  // namespace playden::installs
