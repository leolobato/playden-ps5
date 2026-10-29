#include "update/update_service.h"

#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <utility>

#include "steam/backend/json.h"
#include "update/app_swap.h"
#include "update/file_ops.h"
#include "update/zip_extract.h"

namespace playden::update {

struct UpdateService::Shared {
  mutable std::mutex mutex;
  UpdateSnapshot snapshot;
  std::atomic<bool> cancel{false};
  std::atomic<bool> busy{false};
  std::function<void()> queued;  // one job asked for while another ran (under mutex)
  bool thread_started = false;
  pthread_t thread{};
};

namespace {

void* RunJob(void* opaque) {
  std::unique_ptr<std::function<void()>> work(static_cast<std::function<void()>*>(opaque));
  (*work)();
  return nullptr;
}

std::string Quote(const std::string& text) {
  std::string out = "\"";
  for (char c : text) {
    if (c == '\n') { out += "\\n"; continue; }
    if (c == '"' || c == '\\') out += '\\';
    if (static_cast<unsigned char>(c) >= 0x20) out += c;
    else out += ' ';
  }
  return out + "\"";
}

// The version of the kit in kit_root, from its runtime's artifact name; none for a kit that names none
// (kits before 0.10) or a manifest that can't be read.
std::optional<KitVersion> InstalledKitVersion(const std::string& kit_root) {
  std::string text;
  if (!ReadSmallFile(kit_root + "/manifest.json", &text, 8 << 20)) return std::nullopt;
  steam::json::Limits limits;
  limits.max_bytes = 8 << 20;
  limits.max_values = 1 << 18;
  limits.max_string_bytes = 1 << 20;
  const auto manifest = steam::json::Parse(text, limits);
  const auto* runtime = manifest && manifest->is_object() ? manifest->Find("runtime") : nullptr;
  const auto* artifact = runtime && runtime->is_object() ? runtime->Find("artifact_revision") : nullptr;
  if (!artifact || !artifact->is_string()) return std::nullopt;
  return KitVersionFromArtifact(artifact->text());
}

std::string Gigabytes(std::uint64_t bytes) {
  char text[32];
  std::snprintf(text, sizeof text, "%.1f GB", static_cast<double>(bytes) / 1e9);
  return text;
}

// An app file's mode on the console: 0777 for executables, 0666 for everything else, whatever the
// zip has. Someone may install a build by hand over a self-updated app, with FTP or the web file
// manager. They run as other users, and root has no DAC bypass on /data, so they can only write over
// files that everyone can write. Nothing in Playden needs the app files to be read-only.
std::optional<unsigned> AppMode(const std::string& relative, unsigned) {
  const auto slash = relative.rfind('/');
  const std::string name = slash == std::string::npos ? relative : relative.substr(slash + 1);
  const auto ends = [&](const char* suffix) {
    const std::string tail = suffix;
    return name.size() > tail.size() && name.compare(name.size() - tail.size(), tail.size(), tail) == 0;
  };
  if (name == "eboot.bin" || ends(".prx") || ends(".elf")) return 0777u;
  return 0666u;
}

}  // namespace

const char* UpdatePhaseName(UpdatePhase phase) {
  switch (phase) {
    case UpdatePhase::kIdle: return "idle";
    case UpdatePhase::kChecking: return "checking";
    case UpdatePhase::kUpToDate: return "up-to-date";
    case UpdatePhase::kAvailable: return "available";
    case UpdatePhase::kDownloading: return "downloading";
    case UpdatePhase::kUnpacking: return "unpacking";
    case UpdatePhase::kStaged: return "staged";
    case UpdatePhase::kFailed: return "failed";
  }
  return "?";
}

const char* KitInstallPhaseName(KitInstallPhase phase) {
  switch (phase) {
    case KitInstallPhase::kIdle: return "idle";
    case KitInstallPhase::kChecking: return "checking";
    case KitInstallPhase::kNotNeeded: return "not-needed";
    case KitInstallPhase::kUnavailable: return "unavailable";
    case KitInstallPhase::kAvailable: return "available";
    case KitInstallPhase::kDownloading: return "downloading";
    case KitInstallPhase::kUnpacking: return "unpacking";
    case KitInstallPhase::kInstalled: return "installed";
    case KitInstallPhase::kFailed: return "failed";
  }
  return "?";
}

UpdateService::UpdateService(UpdateConfig config) : config_(std::move(config)), include_betas_(config_.include_betas), shared_(std::make_shared<Shared>()) {
  if (!config_.as_root) config_.as_root = [](const std::function<bool()>& work) { return work(); };
  if (!config_.now) config_.now = [] { return static_cast<std::int64_t>(time(nullptr)); };
  shared_->snapshot.current = config_.current;
}

UpdateService::~UpdateService() { Shutdown(); }

void UpdateService::Log(const std::string& line) const {
  if (config_.log) config_.log("self-update: " + line);
}

void UpdateService::Set(const std::function<void(UpdateSnapshot&)>& change) {
  std::lock_guard<std::mutex> lock(shared_->mutex);
  change(shared_->snapshot);
  ++shared_->snapshot.revision;
}

UpdateSnapshot UpdateService::Snapshot() const {
  std::lock_guard<std::mutex> lock(shared_->mutex);
  return shared_->snapshot;
}

bool UpdateService::Busy() const { return shared_->busy; }

void UpdateService::AcknowledgeFinished() {
  Set([](UpdateSnapshot& s) {
    s.finished_message.clear();
    s.finished_failed = false;
    s.whats_new = false;
  });
}

std::string UpdateService::PageFor(const std::string& tag) const {
  return config_.releases_page.empty() ? std::string() : config_.releases_page + "/tag/" + tag;
}

// The running version's notes, saved when the update to it was confirmed; without them, its page.
void UpdateService::LoadCurrentNotes() {
  std::string notes, url;
  std::string text;
  if (ReadSmallFile(config_.work_root + "/release-notes.json", &text, 64 << 10)) {
    if (const auto saved = steam::json::Parse(text); saved && saved->is_object()) {
      const auto* version = saved->Find("version");
      if (version && version->is_string() && ParseVersion(version->text()) == config_.current) {
        if (const auto* value = saved->Find("notes"); value && value->is_string()) notes = value->text();
        if (const auto* value = saved->Find("url"); value && value->is_string()) url = value->text();
      }
    }
  }
  // A nightly has no release page: its notes are on the page that lists the nightly builds.
  if (url.empty()) url = config_.current.nightly() ? config_.nightly_page : PageFor("v" + config_.current.ToString());
  Set([&](UpdateSnapshot& s) {
    s.current_notes = notes;
    s.current_notes_url = url;
  });
}

void UpdateService::Fail(const std::string& message, const std::string& detail) {
  Log("failed: " + message + (detail.empty() ? "" : " (" + detail + ")"));
  Set([&](UpdateSnapshot& s) {
    s.phase = UpdatePhase::kFailed;
    s.error = message;
    s.part.clear();
  });
}

std::string UpdateService::StatePath() const { return config_.work_root + "/state.json"; }

bool UpdateService::SaveState(const std::string& phase, const std::string& extra) {
  const std::string text = "{\"phase\":" + Quote(phase) + extra + "}\n";
  const bool ok = WriteFileAtomically(StatePath(), text);
  if (!ok) Log("cannot save " + StatePath());
  return ok;
}

std::string UpdateService::StagedFields() const {
  const auto snapshot = Snapshot();
  return ",\"version\":" + Quote(staged_version_.ToString()) + ",\"from\":" + Quote(config_.current.ToString()) +
         ",\"stage\":" + Quote(staged_folder_) + ",\"kit\":" + Quote(staged_kit_) + ",\"kit_id\":" +
         Quote(staged_kit_id_) + ",\"notes\":" + Quote(snapshot.notes) + ",\"notes_url\":" + Quote(snapshot.notes_url);
}

bool UpdateService::PrepareWorkRoot() {
  if (IsFolder(config_.work_root) && access(config_.work_root.c_str(), W_OK) == 0) return true;
  config_.as_root([&] { return MakeFolders(config_.work_root) && chmod(config_.work_root.c_str(), 0777) == 0; });
  const bool ok = IsFolder(config_.work_root) && access(config_.work_root.c_str(), W_OK) == 0;
  if (!ok) Log("cannot create " + config_.work_root);
  return ok;
}

// A kit left by going back, or replaced by the kit-only install (perhaps uploaded over FTP, so
// root's files): it goes as root here, at startup, before the runtime and the UI run (root is
// process-wide). Once, after a kit was replaced; a large kit takes a few seconds.
void UpdateService::RemoveReplacedKits() {
  for (const auto& name : ListFolder(config_.kits_root)) {
    if (name.compare(0, 10, ".replaced-") != 0) continue;
    const std::string path = config_.kits_root + "/" + name;
    if (!IsFolder(path)) continue;
    if (ListFolder(path).empty() && rmdir(path.c_str()) == 0) continue;
    config_.as_root([&] {
      RemoveTree(path);
      return !Exists(path);
    });
    if (Exists(path)) Log("could not remove " + path);
  }
}

// The kit under an id it had before, which the Playden that used that id left when it updated to this
// one (the new kit went in next to it). Once the kit is installed under its own id, the old folder
// becomes the kit this version's update replaced, ".previous-<old id>-<time>" with the newest
// backup's time: going back puts it back, and the next update removes it. As root, at startup,
// before the runtime and the UI run (root is process-wide).
void UpdateService::RetireRenamedKits() {
  if (config_.kit_id.empty() || !Exists(config_.kits_root + "/" + config_.kit_id + "/manifest.json")) return;
  for (const auto& id : config_.renamed_kit_ids) {
    const std::string old_kit = config_.kits_root + "/" + id;
    if (id.empty() || id == config_.kit_id || !IsFolder(old_kit)) continue;
    const auto backup = LatestBackup();
    const std::string stamp = backup ? backup->path.substr(backup->path.rfind('-') + 1) : std::to_string(config_.now());
    std::string previous = config_.kits_root + "/.previous-" + id + "-" + stamp;
    if (Exists(previous)) previous = config_.kits_root + "/.previous-" + id + "-" + std::to_string(config_.now());
    const bool moved = config_.as_root([&] { return !Exists(previous) && rename(old_kit.c_str(), previous.c_str()) == 0; });
    Log(moved ? "kit folder " + id + " retired as " + previous : "kit folder " + id + " could not be retired");
  }
}

// The app user's leftovers of a confirmed update, on the worker and without root: the stage, the
// downloads, the replaced entries and the files of a kit left by going back.
void UpdateService::CleanUpLeftovers() {
  std::vector<std::string> paths;
  for (const char* name : {"/stage", "/downloads", "/replaced"})
    if (Exists(config_.work_root + name)) paths.push_back(config_.work_root + name);
  for (const auto& name : ListFolder(config_.kits_root))
    if (name.compare(0, 10, ".replaced-") == 0) paths.push_back(config_.kits_root + "/" + name);
  if (paths.empty()) return;
  Run([paths] {
    for (const auto& path : paths) RemoveTree(path);
  });
}

bool UpdateService::RemoveOwnedTree(const std::string& path) {
  RemoveTree(path);
  if (Exists(path)) config_.as_root([&] { RemoveTree(path); return true; });
  return !Exists(path);
}

void UpdateService::Run(std::function<void()> work) {
  {
    // A job asked for while another runs waits for it (the startup check behind the cleanup of a
    // confirmed update, an install behind a check); a third request replaces nothing.
    std::lock_guard<std::mutex> lock(shared_->mutex);
    if (shared_->busy) {
      if (!shared_->queued) shared_->queued = std::move(work);
      return;
    }
    shared_->busy = true;
  }
  if (shared_->thread_started) {
    pthread_join(shared_->thread, nullptr);
    shared_->thread_started = false;
  }
  shared_->cancel = false;
  auto shared = shared_;
  auto* job = new std::function<void()>([shared, work = std::move(work)] {
    work();
    for (;;) {
      std::function<void()> next;
      {
        std::lock_guard<std::mutex> lock(shared->mutex);
        next = std::move(shared->queued);
        shared->queued = nullptr;
        if (!next) {
          shared->busy = false;
          return;
        }
      }
      if (!shared->cancel) next();
    }
  });
  pthread_attr_t attributes;
  pthread_attr_init(&attributes);
  pthread_attr_setstacksize(&attributes, config_.worker_stack_bytes);
  const int rc = pthread_create(&shared_->thread, &attributes, RunJob, job);
  pthread_attr_destroy(&attributes);
  if (rc != 0) {
    delete job;
    {
      std::lock_guard<std::mutex> lock(shared_->mutex);
      shared_->busy = false;
      shared_->queued = nullptr;
    }
    Fail("Playden could not start the update.", "pthread_create " + std::to_string(rc));
    return;
  }
  shared_->thread_started = true;
}

void UpdateService::Shutdown() {
  shared_->cancel = true;
  if (shared_->thread_started) {
    pthread_join(shared_->thread, nullptr);
    shared_->thread_started = false;
  }
}

void UpdateService::Cancel() { shared_->cancel = true; }

void UpdateService::RequestRollback() {
  Set([&](UpdateSnapshot& s) {
    if (!s.previous.empty() && s.phase != UpdatePhase::kDownloading && s.phase != UpdatePhase::kUnpacking)
      s.rollback_requested = true;
  });
}

std::optional<UpdateService::Backup> UpdateService::LatestBackup() const {
  std::optional<Backup> latest;
  std::string latest_name;
  for (const auto& name : ListFolder(config_.work_root + "/backup")) {
    const auto dash = name.rfind('-');
    const auto version = dash == std::string::npos ? std::nullopt : ParseVersion(name.substr(0, dash));
    const std::string path = config_.work_root + "/backup/" + name;
    if (!version || !Exists(path + "/eboot.bin")) continue;
    // Names end in the swap time; the newest has the largest.
    const auto stamp = name.substr(dash + 1);
    if (!latest || stamp.size() > latest_name.size() || (stamp.size() == latest_name.size() && stamp > latest_name)) {
      latest = Backup{path, *version};
      latest_name = stamp;
    }
  }
  return latest;
}

namespace {
// 0.6.0 moved user data, logs and games out of the app folder; an older version finds none of it.
constexpr Version kLayoutBoundary{0, 6, 0};
}  // namespace

std::string UpdateService::LayoutRefusal(const Version& to) const {
  if (!(to < kLayoutBoundary)) return {};
  if (!(config_.current < kLayoutBoundary)) return "this version uses the 0.6 data layout";
  if (config_.layout_journal.empty()) return {};
  // Moved (or being moved) when the move record is there; unknown counts as moved.
  struct stat info{};
  if (::lstat(config_.layout_journal.c_str(), &info) == 0) return "the 0.6 layout move record exists";
  if (errno != ENOENT) return "the 0.6 layout move record can't be checked (errno=" + std::to_string(errno) + ")";
  return {};
}

void UpdateService::FindPrevious() {
  auto backup = LatestBackup();
  if (backup) {
    const auto refusal = LayoutRefusal(backup->version);
    if (!refusal.empty()) {
      Log("go back to " + backup->version.ToString() + " not offered: it can't read the 0.6 data layout (" +
          refusal + ")");
      backup.reset();
    }
  }
  Set([&](UpdateSnapshot& s) {
    s.previous = backup && backup->version != config_.current ? backup->version.ToString() : std::string();
  });
}

void UpdateService::RequestRestart() {
  Set([&](UpdateSnapshot& s) {
    if (s.phase == UpdatePhase::kStaged) s.restart_requested = true;
  });
}

DownloadOptions UpdateService::Downloads(const std::string& part) {
  DownloadOptions options;
  options.fetch = config_.fetch;
  options.user_agent = "Playden/" + config_.current.ToString();
  options.hash_file = config_.hash_file;
  options.cancel = &shared_->cancel;
  options.wait_before_retry = config_.wait_before_retry;
  options.log = [this](const std::string& line) { Log(line); };
  if (part == "kit-only")
    options.progress = [this](std::uint64_t done, std::uint64_t total) {
      Set([&](UpdateSnapshot& s) { s.kit.done = done; s.kit.total = total; });
    };
  else if (!part.empty())
    options.progress = [this, part](std::uint64_t done, std::uint64_t total) {
      Set([&](UpdateSnapshot& s) { s.part = part; s.done = done; s.total = total; });
    };
  return options;
}

std::string UpdateService::ReadyKit(const KitPart& kit) const {
  if (!config_.verify_kit) return {};
  const std::string installed = config_.kits_root + "/" + kit.kit_id;
  const std::string staged = config_.kits_root + "/.next-" + kit.kit_id + "-" + kit.revision;
  for (const auto& folder : {installed, staged}) {
    const auto sha = config_.verify_kit(folder);
    if (sha && *sha == kit.manifest_sha256) return folder;
  }
  return {};
}

void UpdateService::Start() {
  FindPrevious();
  LoadCurrentNotes();
  RemoveReplacedKits();
  RetireRenamedKits();
  std::string text;
  if (!ReadSmallFile(StatePath(), &text)) {
    CleanUpLeftovers();  // what a start closed before its cleanup finished left behind
    return;
  }
  const auto state = steam::json::Parse(text);
  if (!state || !state->is_object()) {
    Log("unreadable state, removed");
    unlink(StatePath().c_str());
    return;
  }
  const auto field = [&](const char* name) -> std::string {
    const auto* value = state->Find(name);
    return value && value->is_string() ? value->text() : std::string();
  };
  const std::string phase = field("phase");
  if (phase == "applied") {
    ConfirmApplied(text);
    return;
  }
  if (phase == "staged") {
    const auto version = ParseVersion(field("version"));
    const std::string stage = field("stage");
    const std::string kit = field("kit");
    const bool kit_present = kit.empty() || (config_.verify_kit && config_.verify_kit(kit));
    if (version && config_.current < *version && !stage.empty() && Exists(stage + "/eboot.bin") && kit_present) {
      staged_folder_ = stage;
      staged_version_ = *version;
      staged_kit_ = kit;
      staged_kit_id_ = field("kit_id");
      Log("update " + version->ToString() + " is staged at " + stage + (kit.empty() ? "" : " with kit " + kit));
      Set([&](UpdateSnapshot& s) {
        s.phase = UpdatePhase::kStaged;
        s.available = *version;
        s.notes = field("notes");
        s.notes_url = field("notes_url");
      });
      return;
    }
    Log("staged update no longer applies, removed");
    RemoveOwnedTree(stage);
    if (!kit.empty()) RemoveOwnedTree(kit);
    unlink(StatePath().c_str());
  }
}

void UpdateService::ConfirmApplied(const std::string& text) {
  const auto state = steam::json::Parse(text);
  const auto field = [&](const char* name) -> std::string {
    const auto* value = state->Find(name);
    return value && value->is_string() ? value->text() : std::string();
  };
  const auto version = ParseVersion(field("version"));
  const std::string backup = field("backup");
  const std::string previous_kit = field("previous_kit");
  const std::string kit_id = field("kit_id");
  const bool rolled_back = field("rolled_back") == "yes";
  if (version && *version == config_.current) {
    Log((rolled_back ? "going back to " : "update to ") + version->ToString() + " confirmed");
    if (!rolled_back) {
      // Kept for Settings › Release notes until the next update.
      WriteFileAtomically(config_.work_root + "/release-notes.json",
                          "{\"version\":" + Quote(version->ToString()) + ",\"notes\":" + Quote(field("notes")) +
                              ",\"url\":" + Quote(field("notes_url")) + "}\n");
      LoadCurrentNotes();
    }
    Set([&](UpdateSnapshot& s) {
      s.whats_new = !rolled_back;
      s.finished_message = rolled_back ? "Playden went back to version " + version->ToString() + "."
                                       : "Playden was updated to " + version->ToString() + ".";
      s.finished_failed = false;
    });
    // Only the app user's own leftovers go now. The previous app and kit stay to go back by hand;
    // older ones are root-owned and are removed at the next update's quit, when nothing else runs.
    if (!rolled_back) unlink((config_.work_root + "/declined-version").c_str());
    // Normally done at quit; again here in case the process died right after the swap. Root is
    // process-wide, so it is raised now, while Start runs before the runtime and the UI: raised from
    // the worker it failed the runtime's ownership checks at the same time ("Games can't start").
    config_.as_root([&] {
      return RefreshRegisteredCopies(config_.installed_root + "/sce_sys", config_.appmeta_root,
                                     config_.registered_app_root, [this](const std::string& line) { Log(line); });
    });
    // Confirmed: a start closed before the cleanup below finishes does not confirm again; the next
    // start finds the leftovers without a state and cleans them up.
    unlink(StatePath().c_str());
    CleanUpLeftovers();
    return;
  }
  Log((rolled_back ? "going back to " : "update to ") + field("version") + " did not take effect; running " +
      config_.current.ToString());
  // The swap was interrupted (the executable moves last, so this version still runs): put back the
  // entries that were replaced, then the kit this version uses.
  const std::string stage = field("stage");
  if (!backup.empty() && !stage.empty() && IsFolder(backup))
    config_.as_root([&] {
      const bool undone = UndoSwap(config_.installed_root, stage, backup, [this](const std::string& line) { Log(line); });
      rmdir(backup.c_str());  // empty now, unless something could not move back
      return undone;
    });
  if (!previous_kit.empty() && !kit_id.empty() && Exists(previous_kit)) {
    const std::string installed_kit = config_.kits_root + "/" + kit_id;
    // The kit that replaced it goes back where it came from (a staged kit stays ready for a retry).
    std::string displaced = field("next_kit");
    if (displaced.empty() || Exists(displaced))
      displaced = config_.kits_root + "/.failed-" + kit_id + "-" + std::to_string(config_.now());
    const bool restored = config_.as_root([&] {
      const bool moved_aside = Exists(installed_kit);
      if (moved_aside && rename(installed_kit.c_str(), displaced.c_str()) != 0) return false;
      if (rename(previous_kit.c_str(), installed_kit.c_str()) == 0) return true;
      if (moved_aside) rename(displaced.c_str(), installed_kit.c_str());  // keep a working kit in place
      return false;
    });
    Log(restored ? "previous kit restored" : "previous kit could not be restored");
    if (restored && displaced.find("/.failed-") != std::string::npos) RemoveOwnedTree(displaced);
  }
  Set([&](UpdateSnapshot& s) {
    s.finished_message = (rolled_back ? "Going back to Playden " : "The update to Playden ") + field("version") +
                         " did not finish. Playden " + config_.current.ToString() + " is still installed.";
    s.finished_failed = true;
  });
  unlink(StatePath().c_str());
  FindPrevious();
}

bool UpdateService::SetIncludeBetas(bool enabled) { return include_betas_.exchange(enabled) != enabled; }

void UpdateService::Check(bool user_asked) {
  {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    const auto phase = shared_->snapshot.phase;
    if (phase == UpdatePhase::kDownloading || phase == UpdatePhase::kUnpacking || phase == UpdatePhase::kStaged)
      return;
  }
  Run([this, user_asked] { DoCheck(user_asked); });
}

void UpdateService::Install() {
  Run([this] {
    // A track change can arrive while a download is being cancelled or a check is queued.
    if (release_ && (release_->prerelease || release_->version.beta) && !include_betas_.load()) {
      DoCheck(false);
      return;
    }
    if (!release_ || !manifest_) DoCheck(false);
    if (Snapshot().phase != UpdatePhase::kAvailable) return;
    DoInstall();
  });
}

void UpdateService::DoCheck(bool user_asked) {
  Set([&](UpdateSnapshot& s) {
    s.phase = UpdatePhase::kChecking;
    s.user_check = user_asked;
    s.error.clear();
  });
  auto options = Downloads("");
  options.max_attempts = 2;
  std::string error;
  const bool local = !config_.local_release_dir.empty() && Exists(config_.local_release_dir + "/release.json");
  const auto read_local = [&](const std::string& name) -> std::optional<std::string> {
    std::string text;
    if (ReadSmallFile(config_.local_release_dir + "/" + name, &text, 2 << 20)) return text;
    error = "cannot read " + name;
    return std::nullopt;
  };
  if (local) Log("reading the release from " + config_.local_release_dir);
  const bool include_betas = include_betas_.load();
  std::optional<Release> release;
  std::optional<std::vector<Release>> releases;  // every release, when read: betas, or a kit zip left out
  if (include_betas) {
    releases = FetchReleases(&error);
    if (!releases) return Fail("Playden could not read the releases on GitHub.", error);
    for (const auto& candidate : *releases)
      if (!candidate.draft && (!release || release->version < candidate.version)) release = candidate;
  } else {
    const auto body = local ? read_local("release.json")
                            : FetchSmall(config_.latest_release_url, 2 << 20, options, "application/vnd.github+json", &error);
    if (!body) return Fail("Playden could not reach GitHub to look for updates.", error);
    release = ParseRelease(*body, &error);
    if (!release) return Fail("The latest release on GitHub could not be read.", error);
    if (release->draft || release->prerelease || release->version.beta) release.reset();
  }
  if (!release || !(config_.current < release->version)) {
    Log("up to date: running " + config_.current.ToString());
    release_.reset();
    manifest_.reset();
    kit_zip_.reset();
    Set([&](UpdateSnapshot& s) {
      s.phase = UpdatePhase::kUpToDate;
      s.available = release ? release->version : config_.current;
    });
    return;
  }
  const auto* asset = release->Find(kUpdateManifestAsset);
  if (!asset) return Fail("Release " + release->tag + " has no update information. Install it by hand.", "no asset");
  const auto manifest_text = local ? read_local(kUpdateManifestAsset)
                                   : FetchSmall(asset->url, 256 << 10, options, "application/octet-stream", &error);
  if (!manifest_text) return Fail("Playden could not read the update information.", error);
  const auto manifest = ParseUpdateManifest(*manifest_text, &error);
  if (!manifest) return Fail("The update information of " + release->tag + " is not valid.", error);
  if (manifest->app_version != release->version || manifest->title_id != config_.title_id)
    return Fail("The update information does not match release " + release->tag + ".", "version or title");
  if (config_.current < manifest->min_from)
    return Fail("Playden " + release->version.ToString() + " cannot update from this version. Install it by hand.",
                "min_from " + manifest->min_from.ToString());
  const auto* app = release->Find(manifest->app_asset);
  const auto* kit = release->Find(manifest->kit.asset);
  // A release whose kit did not change may leave the kit zip out; it is needed only when the
  // console has no copy of that kit, and then an older release has the same zip.
  const bool kit_needed = ReadyKit(manifest->kit).empty();
  if (kit_needed && !kit) {
    if (!releases) {
      std::string ignored;
      releases = FetchReleases(&ignored);
    }
    if (releases)
      for (const auto& older : *releases) {
        if (older.draft || release->version < older.version) continue;
        if ((kit = older.Find(manifest->kit.asset))) {
          Log("update " + release->tag + " takes its kit " + manifest->kit.asset + " from " + older.tag);
          break;
        }
      }
  }
  if (!app || (kit_needed && !kit)) return Fail("Release " + release->tag + " is missing a download.", "asset");
  if (app->sha256.empty() || (kit_needed && kit->sha256.empty()))
    return Fail("Release " + release->tag + " has no checksums. Install it by hand.", "digest");
  release_ = *release;
  manifest_ = *manifest;
  if (kit_needed) kit_zip_ = *kit;
  else kit_zip_.reset();
  Log("update " + release->tag + " available: app " + std::to_string(app->size) + " bytes, kit " +
      (kit_needed ? std::to_string(kit->size) + " bytes" : "already installed"));
  std::string declined;
  ReadSmallFile(config_.work_root + "/declined-version", &declined, 64);
  const auto declined_version = ParseVersion(declined.substr(0, declined.find('\n')));
  Set([&](UpdateSnapshot& s) {
    s.phase = UpdatePhase::kAvailable;
    s.quiet = declined_version && *declined_version == release->version;
    s.available = release->version;
    s.notes = manifest->notes;
    s.notes_url = release->page.empty() ? PageFor(release->tag) : release->page;
    s.kit_needed = kit_needed;
    s.download_bytes = app->size + (kit_needed ? kit->size : 0);
  });
}

void UpdateService::DoInstall() {
  const auto& release = *release_;
  const auto& manifest = *manifest_;
  if (!PrepareWorkRoot() || !MakeFolders(config_.work_root + "/downloads"))
    return Fail("Playden could not create its update folder.", config_.work_root);
  const auto* app = release.Find(manifest.app_asset);
  const auto* kit = kit_zip_ ? &*kit_zip_ : release.Find(manifest.kit.asset);
  std::string kit_folder = ReadyKit(manifest.kit);
  const bool kit_needed = kit_folder.empty();
  // Zip, unpacked files and some room to spare.
  if (!app || (kit_needed && !kit)) return Fail("Release " + release.tag + " is missing a download.", "asset");
  const std::uint64_t needed = app->size * 3 + (kit_needed ? kit->size * 4 : 0);
  if (config_.free_bytes) {
    const auto free = config_.free_bytes(config_.work_root);
    if (free && *free < needed)
      return Fail("The update needs " + Gigabytes(needed) + " of free space; " + Gigabytes(*free) + " is free.",
                  "free space");
  }
  if (kit_needed) {
    if (!StageKit(*kit, manifest.kit, false)) return;
    kit_folder = ReadyKit(manifest.kit);
  }
  if (!StageApp(release, manifest)) return;
  staged_version_ = release.version;
  // The installed kit already matches: nothing to swap for it.
  staged_kit_ = kit_folder == config_.kits_root + "/" + manifest.kit.kit_id ? std::string() : kit_folder;
  staged_kit_id_ = manifest.kit.kit_id;
  if (!SaveState("staged", StagedFields())) return Fail("Playden could not save the update state.", StatePath());
  Log("update " + release.version.ToString() + " staged");
  Set([&](UpdateSnapshot& s) {
    s.phase = UpdatePhase::kStaged;
    s.part.clear();
  });
}

bool UpdateService::StageKit(const ReleaseAsset& asset, const KitPart& kit, bool kit_only) {
  const std::string local_zip = config_.local_release_dir + "/" + asset.name;
  const bool local = !config_.local_release_dir.empty() && Exists(local_zip);
  const std::string zip = local ? local_zip : config_.work_root + "/downloads/" + asset.name;
  // The update reports in the snapshot's own fields, the kit-only install in snapshot.kit.
  const auto step = [&](bool unpacking, std::uint64_t total) {
    Set([&](UpdateSnapshot& s) {
      if (kit_only) {
        s.kit.phase = unpacking ? KitInstallPhase::kUnpacking : KitInstallPhase::kDownloading;
        s.kit.done = 0;
        s.kit.total = total;
        return;
      }
      s.phase = unpacking ? UpdatePhase::kUnpacking : UpdatePhase::kDownloading;
      s.part = "kit";
      s.done = 0;
      s.total = total;
    });
  };
  const auto cancelled = [&] {
    Set([&](UpdateSnapshot& s) {
      if (kit_only) s.kit.phase = KitInstallPhase::kAvailable;
      else { s.phase = UpdatePhase::kAvailable; s.part.clear(); }
    });
  };
  const auto fail = [&](const std::string& message, const std::string& detail) {
    if (kit_only) FailKit(message, detail);
    else Fail(message, detail);
  };
  step(false, asset.size);
  const auto download = local ? DownloadResult{true, false, {}}
                              : DownloadAsset(asset.url, asset.size, asset.sha256, zip, Downloads(kit_only ? "kit-only" : "kit"));
  if (!download.ok) {
    if (download.cancelled) {
      cancelled();
      return false;
    }
    fail("The kit download failed.", download.error);
    return false;
  }
  step(true, 0);
  const std::string staging = config_.kits_root + "/.staging-" + kit.kit_id + "-" + kit.revision;
  RemoveOwnedTree(staging);
  config_.as_root([&] {
    // A first start has no kits folder: made like the kit upload tool makes it.
    if (!IsFolder(config_.kits_root) && (!MakeFolders(config_.kits_root) || chmod(config_.kits_root.c_str(), 0755) != 0))
      return false;
    return MakeFolders(staging) && chmod(staging.c_str(), 0777) == 0;
  });
  if (!IsFolder(staging) || access(staging.c_str(), W_OK) != 0) {
    fail("Playden could not create the kit folder.", staging);
    return false;
  }
  ZipExtractOptions unzip;
  unzip.inner_prefix = kit.prefix;
  unzip.cancel = &shared_->cancel;
  unzip.fix_mode = [this](const std::string& path, unsigned mode) {
    return config_.as_root([&] { return chmod(path.c_str(), mode) == 0; });
  };
  unzip.log = [this](const std::string& line) { Log(line); };
  unzip.progress = [this, kit_only](std::uint64_t done, std::uint64_t total) {
    Set([&](UpdateSnapshot& s) {
      if (kit_only) { s.kit.done = done; s.kit.total = total; }
      else { s.done = done; s.total = total; }
    });
  };
  const auto unpacked = ExtractZip(zip, staging, unzip);
  if (!unpacked.ok) {
    RemoveOwnedTree(staging);
    if (unpacked.cancelled) {
      cancelled();
      return false;
    }
    fail("The kit could not be unpacked.", unpacked.error);
    return false;
  }
  Log("kit unpacked: " + std::to_string(unpacked.files) + " files, " + std::to_string(unpacked.rewritten) +
      " written again, " + std::to_string(unpacked.mode_fixed) + " root-owned given their mode");
  const auto sha = config_.verify_kit ? config_.verify_kit(staging) : std::nullopt;
  if (!sha || *sha != kit.manifest_sha256) {
    RemoveOwnedTree(staging);
    fail("The new kit did not pass its check.", sha ? "manifest " + *sha : "not a complete kit");
    return false;
  }
  // Ready next to the installed kit; it replaces it when Playden quits for the update.
  const std::string final_folder = config_.kits_root + "/.next-" + kit.kit_id + "-" + kit.revision;
  const std::string replaced = final_folder + ".old";
  const bool moved = config_.as_root([&] {
    if (Exists(final_folder) && rename(final_folder.c_str(), replaced.c_str()) != 0) return false;
    return rename(staging.c_str(), final_folder.c_str()) == 0;
  });
  RemoveOwnedTree(replaced);
  if (!moved) {
    RemoveOwnedTree(staging);
    fail("Playden could not put the new kit in place.", final_folder);
    return false;
  }
  if (!local) unlink(zip.c_str());
  return true;
}

void UpdateService::FailKit(const std::string& message, const std::string& detail) {
  Log("kit install failed: " + message + (detail.empty() ? "" : " (" + detail + ")"));
  Set([&](UpdateSnapshot& s) {
    s.kit.phase = KitInstallPhase::kFailed;
    s.kit.error = message;
  });
}

// Every release, newest first: GET .../releases next to the configured .../releases/latest. Console
// testing reads release.json from local_release_dir as the only one.
std::optional<std::vector<Release>> UpdateService::FetchReleases(std::string* error) {
  if (!config_.local_release_dir.empty() && Exists(config_.local_release_dir + "/release.json")) {
    std::string text;
    if (!ReadSmallFile(config_.local_release_dir + "/release.json", &text, 2 << 20)) {
      *error = "cannot read release.json";
      return std::nullopt;
    }
    auto release = ParseRelease(text, error);
    if (!release) return std::nullopt;
    return std::vector<Release>{std::move(*release)};
  }
  std::string url = config_.latest_release_url;
  if (url.size() > 7 && url.compare(url.size() - 7, 7, "/latest") == 0) url.erase(url.size() - 7);
  auto options = Downloads("");
  options.max_attempts = 2;
  const auto body = FetchSmall(url + "?per_page=100", 4 << 20, options, "application/vnd.github+json", error);
  if (!body) return std::nullopt;
  return ParseReleases(*body, error);
}

void UpdateService::CheckKit() {
  if (config_.kit_id.empty() || config_.kit_manifest_sha256.empty()) return;
  Run([this] { DoCheckKit(); });
}

void UpdateService::InstallKit() {
  Run([this] {
    if (!kit_asset_ || !kit_part_) DoCheckKit();
    if (Snapshot().kit.phase != KitInstallPhase::kAvailable) return;
    DoInstallKit();
  });
}

void UpdateService::DoCheckKit() {
  const auto phase = Snapshot().phase;
  // A staged update brings its own kit when Playden restarts; one downloading may too.
  if (phase == UpdatePhase::kStaged || phase == UpdatePhase::kDownloading || phase == UpdatePhase::kUnpacking) return;
  const std::string installed = config_.kits_root + "/" + config_.kit_id;
  const auto installed_sha = config_.verify_kit ? config_.verify_kit(installed) : std::nullopt;
  if (installed_sha && *installed_sha == config_.kit_manifest_sha256) {
    Set([](UpdateSnapshot& s) { s.kit = {}; s.kit.phase = KitInstallPhase::kNotNeeded; });
    return;
  }
  Log("kit check: " + (installed_sha ? "the installed kit is another one (" + *installed_sha + ")"
                                     : std::string("no complete kit is installed")));
  Set([](UpdateSnapshot& s) {
    s.kit = {};
    s.kit.phase = KitInstallPhase::kChecking;
  });
  const auto unavailable = [&](const std::string& why) {
    Log("kit check: " + why);
    Set([](UpdateSnapshot& s) { s.kit.phase = KitInstallPhase::kUnavailable; });
  };
  std::string error;
  const auto releases = FetchReleases(&error);
  if (!releases) return FailKit("Playden could not reach GitHub to download its runtime kit.", error);
  // The running version's release names its kit. A nightly has no release: the newest release before it
  // stands in, and is used only if it names the same kit (checked below).
  const Release* own = nullptr;
  for (const auto& release : *releases) {
    if (release.draft) continue;
    if (release.version == config_.current) own = &release;
    else if (config_.current.nightly() && release.version < config_.current &&
             (!own || (own->version != config_.current && own->version < release.version)))
      own = &release;
  }
  if (!own) return unavailable("no release has version " + config_.current.ToString());
  const auto* manifest_asset = own->Find(kUpdateManifestAsset);
  if (!manifest_asset) return unavailable("release " + own->tag + " has no update information");
  auto options = Downloads("");
  options.max_attempts = 2;
  const bool local = !config_.local_release_dir.empty() && Exists(config_.local_release_dir + "/" + kUpdateManifestAsset);
  std::optional<std::string> manifest_text;
  if (!local) {
    manifest_text = FetchSmall(manifest_asset->url, 256 << 10, options, "application/octet-stream", &error);
  } else if (std::string text; ReadSmallFile(config_.local_release_dir + "/" + kUpdateManifestAsset, &text, 256 << 10)) {
    manifest_text = std::move(text);
  } else {
    error = std::string("cannot read ") + kUpdateManifestAsset;
  }
  if (!manifest_text) return FailKit("Playden could not read the kit information.", error);
  const auto manifest = ParseUpdateManifest(*manifest_text, &error);
  if (!manifest) return FailKit("The kit information of " + own->tag + " is not valid.", error);
  if (manifest->kit.kit_id != config_.kit_id || manifest->kit.manifest_sha256 != config_.kit_manifest_sha256)
    return unavailable("release " + own->tag + " names kit " + manifest->kit.kit_id + " " + manifest->kit.manifest_sha256);
  // Only a missing or older kit is replaced: one as new or newer (a development kit, or the same version
  // built here) stays. A kit whose version can't be read is older than 0.10, or unreadable.
  if (installed_sha) {
    const auto installed_version = InstalledKitVersion(installed);
    const auto offered_version = ParseKitVersion(manifest->kit.revision);
    if (installed_version && offered_version && !(*installed_version < *offered_version)) {
      Log("kit check: the installed kit is not older than " + manifest->kit.revision + ", kept");
      Set([](UpdateSnapshot& s) { s.kit = {}; s.kit.phase = KitInstallPhase::kNotNeeded; });
      return;
    }
  }
  // A release whose kit did not change may leave the kit zip out: an older release has the same zip.
  const Release* source = own;
  const ReleaseAsset* zip = own->Find(manifest->kit.asset);
  for (const auto& release : *releases) {
    if (zip) break;
    if (release.draft || config_.current < release.version) continue;
    if ((zip = release.Find(manifest->kit.asset))) source = &release;
  }
  if (!zip) return unavailable("no release has " + manifest->kit.asset);
  if (zip->sha256.empty()) return FailKit("The kit on GitHub has no checksum. Install it by hand.", source->tag);
  const bool staged = !ReadyKit(manifest->kit).empty();
  kit_asset_ = *zip;
  kit_part_ = manifest->kit;
  Log("kit " + manifest->kit.asset + " available from " + source->tag + ": " +
      (staged ? std::string("already on the console") : std::to_string(zip->size) + " bytes"));
  Set([&](UpdateSnapshot& s) {
    s.kit.phase = KitInstallPhase::kAvailable;
    s.kit.replaces = installed_sha.has_value();
    s.kit.version = source->version.ToString();
    s.kit.download_bytes = staged ? 0 : zip->size;
  });
}

void UpdateService::DoInstallKit() {
  const auto phase = Snapshot().phase;
  // A staged update's kit may be the same one: taking it would leave the update without its kit.
  if (phase == UpdatePhase::kStaged || phase == UpdatePhase::kDownloading || phase == UpdatePhase::kUnpacking) {
    Log("kit install skipped: an update brings the kit");
    return;
  }
  const auto& kit = *kit_part_;
  const std::string installed = config_.kits_root + "/" + kit.kit_id;
  std::string folder = ReadyKit(kit);
  if (folder.empty()) {
    if (!PrepareWorkRoot() || !MakeFolders(config_.work_root + "/downloads"))
      return FailKit("Playden could not create its download folder.", config_.work_root);
    const std::uint64_t needed = kit_asset_->size * 4;  // zip, unpacked files and some room to spare
    if (config_.free_bytes) {
      const auto free = config_.free_bytes(config_.work_root);
      if (free && *free < needed)
        return FailKit("The kit needs " + Gigabytes(needed) + " of free space; " + Gigabytes(*free) + " is free.",
                       "free space");
    }
    if (!StageKit(*kit_asset_, kit, true)) return;
    folder = ReadyKit(kit);
    if (folder.empty()) return FailKit("The new kit did not pass its check.", "not ready after unpacking");
  }
  if (folder != installed) {
    // No game can use the kit in place (none was admitted), so it is swapped now; the one it
    // replaces is a ".replaced-" leftover, removed at a later start.
    const std::string aside = config_.kits_root + "/.replaced-" + kit.kit_id + "-" + std::to_string(config_.now());
    const bool moved = config_.as_root([&] {
      if (Exists(installed) && rename(installed.c_str(), aside.c_str()) != 0) return false;
      if (rename(folder.c_str(), installed.c_str()) == 0) return true;
      if (Exists(aside)) rename(aside.c_str(), installed.c_str());
      return false;
    });
    if (!moved) return FailKit("Playden could not put the kit in place.", installed);
  }
  Log("kit " + kit.kit_id + " " + kit.revision + " installed");
  Set([](UpdateSnapshot& s) {
    s.kit.phase = KitInstallPhase::kInstalled;
    s.kit.done = s.kit.total;
  });
}

bool UpdateService::StageApp(const Release& release, const UpdateManifest& manifest) {
  const auto* asset = release.Find(manifest.app_asset);
  const std::string local_zip = config_.local_release_dir + "/" + asset->name;
  const bool local = !config_.local_release_dir.empty() && Exists(local_zip);
  const std::string zip = local ? local_zip : config_.work_root + "/downloads/" + asset->name;
  Set([&](UpdateSnapshot& s) {
    s.phase = UpdatePhase::kDownloading;
    s.part = "app";
    s.done = 0;
    s.total = asset->size;
  });
  const auto download = local ? DownloadResult{true, false, {}}
                              : DownloadAsset(asset->url, asset->size, asset->sha256, zip, Downloads("app"));
  if (!download.ok) {
    if (download.cancelled) {
      Set([](UpdateSnapshot& s) { s.phase = UpdatePhase::kAvailable; s.part.clear(); });
      return false;
    }
    Fail("The Playden download failed.", download.error);
    return false;
  }
  Set([&](UpdateSnapshot& s) {
    s.phase = UpdatePhase::kUnpacking;
    s.part = "app";
    s.done = 0;
    s.total = 0;
  });
  const std::string stage = config_.work_root + "/stage/" + release.version.ToString();
  RemoveOwnedTree(stage);
  if (!MakeFolders(stage)) {
    Fail("Playden could not create its update folder.", stage);
    return false;
  }
  ZipExtractOptions unzip;
  unzip.inner_prefix = manifest.app_prefix;
  unzip.cancel = &shared_->cancel;
  unzip.fix_mode = [this](const std::string& path, unsigned mode) {
    return config_.as_root([&] { return chmod(path.c_str(), mode) == 0; });
  };
  unzip.mode_for = AppMode;
  unzip.folder_mode = 0777u;  // for the same reason as AppMode
  unzip.log = [this](const std::string& line) { Log(line); };
  unzip.progress = [this](std::uint64_t done, std::uint64_t total) {
    Set([&](UpdateSnapshot& s) { s.done = done; s.total = total; });
  };
  const auto unpacked = ExtractZip(zip, stage, unzip);
  if (!unpacked.ok) {
    RemoveOwnedTree(stage);
    if (unpacked.cancelled) {
      Set([](UpdateSnapshot& s) { s.phase = UpdatePhase::kAvailable; s.part.clear(); });
      return false;
    }
    Fail("The Playden download could not be unpacked.", unpacked.error);
    return false;
  }
  std::string param_text;
  const auto param = ReadSmallFile(stage + "/sce_sys/param.json", &param_text)
                         ? steam::json::Parse(param_text)
                         : std::nullopt;
  const auto* title = param && param->is_object() ? param->Find("titleId") : nullptr;
  const auto* content = param && param->is_object() ? param->Find("contentVersion") : nullptr;
  const auto content_version = content && content->is_string() ? ParseVersion(content->text()) : std::nullopt;
  std::string full_text;
  const bool has_full = ReadSmallFile(stage + "/playden-version", &full_text, 64);
  if (!full_text.empty() && full_text.back() == '\n') full_text.pop_back();
  const auto full_version = has_full ? ParseVersion(full_text)
                                    : (Exists(stage + "/playden-version") ? std::nullopt : content_version);
  const Version numeric{release.version.major, release.version.minor, release.version.patch};
  if (!title || !title->is_string() || title->text() != config_.title_id || !content_version ||
      *content_version != numeric || !full_version || *full_version != release.version ||
      !Exists(stage + "/eboot.bin")) {
    RemoveOwnedTree(stage);
    Fail("The downloaded Playden is not version " + release.version.ToString() + ".", "param.json");
    return false;
  }
  if (!local) unlink(zip.c_str());
  staged_folder_ = stage;
  return true;
}

// Puts the newest backup's app and the previous kit back, as one transaction, when Playden quits.
bool UpdateService::RollBackAtQuit() {
  const auto backup = LatestBackup();
  if (!backup) return false;
  if (const auto refusal = LayoutRefusal(backup->version); !refusal.empty()) {
    Log("going back refused: " + backup->version.ToString() + " can't read the 0.6 data layout (" + refusal + ")");
    return false;
  }
  const std::string stamp = std::to_string(config_.now());
  const std::string replaced = config_.work_root + "/replaced/" + config_.current.ToString() + "-" + stamp;
  // The kit that update replaced: ".previous-<kit_id>-<time>" with the backup's time. An update
  // that kept the kit left none, and going back keeps the installed kit.
  const std::string backup_stamp = backup->path.substr(backup->path.rfind('-') + 1);
  std::string previous_kit, kit_id;
  for (const auto& name : ListFolder(config_.kits_root)) {
    const std::string suffix = "-" + backup_stamp;
    if (name.compare(0, 10, ".previous-") != 0 || name.size() <= 10 + suffix.size() ||
        name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0)
      continue;
    // A kit retired under an earlier id is the one the backed-up app used: an update from that app may also have
    // replaced a kit under this version's id at the same time, which the older app never reads.
    const std::string id = name.substr(10, name.size() - 10 - suffix.size());
    const bool renamed = std::find(config_.renamed_kit_ids.begin(), config_.renamed_kit_ids.end(), id) !=
                         config_.renamed_kit_ids.end();
    if (!previous_kit.empty() && !renamed) continue;
    kit_id = id;
    previous_kit = config_.kits_root + "/" + name;
    if (renamed) break;
  }
  const std::string installed_kit = kit_id.empty() ? std::string() : config_.kits_root + "/" + kit_id;
  const std::string replaced_kit = config_.kits_root + "/.replaced-" + kit_id + "-" + stamp;
  const auto log = [this](const std::string& line) { Log(line); };
  // Saved before anything moves: if the process dies part way, the next start puts it back.
  const std::string going_back = ",\"version\":" + Quote(backup->version.ToString()) + ",\"from\":" +
                                 Quote(config_.current.ToString()) + ",\"rolled_back\":\"yes\"";
  if (!SaveState("applied", going_back + ",\"backup\":" + Quote(replaced) + ",\"stage\":" + Quote(backup->path) +
                                ",\"kit_id\":" + Quote(kit_id) + ",\"previous_kit\":" +
                                Quote(previous_kit.empty() ? std::string() : replaced_kit) + ",\"next_kit\":" +
                                Quote(previous_kit))) {
    Log("going back failed: cannot save the state");
    return false;
  }
  SwapResult swap;
  bool kit_swapped = false;
  config_.as_root([&] {
    // A previous kit under an id this version's kit no longer has (RetireRenamedKits) goes back to
    // its own folder; this version's kit stays where it is, unused by the older Playden.
    bool moved_aside = false;
    if (!previous_kit.empty()) {
      if (Exists(installed_kit)) {
        if (rename(installed_kit.c_str(), replaced_kit.c_str()) != 0) {
          swap.error = "cannot move the current kit aside";
          return false;
        }
        moved_aside = true;
      }
      if (rename(previous_kit.c_str(), installed_kit.c_str()) != 0) {
        if (moved_aside) rename(replaced_kit.c_str(), installed_kit.c_str());
        swap.error = "cannot put the previous kit back";
        return false;
      }
      kit_swapped = true;
    }
    MakeFolders(config_.work_root + "/replaced");
    swap = RestoreApp(config_.installed_root, backup->path, replaced, log);
    if (!swap.ok) {
      if (kit_swapped) {
        rename(installed_kit.c_str(), previous_kit.c_str());
        if (moved_aside) rename(replaced_kit.c_str(), installed_kit.c_str());
        kit_swapped = false;
      }
      return false;
    }
    if (!RefreshRegisteredCopies(config_.installed_root + "/sce_sys", config_.appmeta_root,
                                 config_.registered_app_root, log))
      Log("some Home screen copies were not refreshed");
    // The version left behind is not kept: going back is one step. Its kit is the app user's files;
    // they are removed on the next start, so Playden reopens at once.
    RemoveTree(backup->path);
    return true;
  });
  if (!swap.ok) {
    Log("going back failed: " + swap.error);
    unlink(StatePath().c_str());
    return false;
  }
  SaveState("applied", going_back);
  WriteFileAtomically(config_.work_root + "/declined-version", config_.current.ToString() + "\n");
  Log("went back to " + backup->version.ToString() + (kit_swapped ? " with the previous kit" : ""));
  return true;
}

bool UpdateService::ApplyAtQuit() {
  if (Snapshot().rollback_requested) return RollBackAtQuit();
  if (!Snapshot().restart_requested || staged_folder_.empty()) return false;
  const std::string stamp = std::to_string(config_.now());
  const std::string backup = config_.work_root + "/backup/" + config_.current.ToString() + "-" + stamp;
  const std::string installed_kit = staged_kit_id_.empty() ? std::string() : config_.kits_root + "/" + staged_kit_id_;
  const std::string previous_kit = config_.kits_root + "/.previous-" + staged_kit_id_ + "-" + stamp;
  const auto log = [this](const std::string& line) { Log(line); };
  const auto applied = [&](bool kit_moved) {
    return ",\"version\":" + Quote(staged_version_.ToString()) + ",\"from\":" + Quote(config_.current.ToString()) +
           ",\"backup\":" + Quote(backup) + ",\"stage\":" + Quote(staged_folder_) + ",\"notes\":" +
           Quote(Snapshot().notes) + ",\"notes_url\":" + Quote(Snapshot().notes_url) + ",\"kit_id\":" +
           Quote(staged_kit_id_) + ",\"previous_kit\":" + Quote(kit_moved ? previous_kit : std::string()) +
           ",\"next_kit\":" + Quote(kit_moved ? staged_kit_ : std::string());
  };
  // Saved before anything moves: if the process dies part way, the next start sees which version
  // runs and finishes or undoes the swap.
  if (!SaveState("applied", applied(!staged_kit_.empty()))) {
    Log("swap failed: cannot save the state");
    return false;
  }
  SwapResult swap;
  bool kit_swapped = false;
  config_.as_root([&] {
    // One previous version is kept. Older backups and kits are mostly root-owned (copied over FTP):
    // they go now, as root, while no other thread runs.
    for (const auto& name : ListFolder(config_.work_root + "/backup")) RemoveTree(config_.work_root + "/backup/" + name);
    // Its kit goes too, whether or not this update brings a new one: going back from this update
    // restores only the kit this update replaces.
    for (const auto& name : ListFolder(config_.kits_root))
      if (name.compare(0, 10, ".previous-") == 0) RemoveTree(config_.kits_root + "/" + name);
    // One transaction: the kit first, then the app; a failed app swap puts the kit back.
    if (!staged_kit_.empty()) {
      if (!Exists(staged_kit_)) {
        swap.error = "staged kit is missing";
        return false;
      }
      if (Exists(installed_kit) && rename(installed_kit.c_str(), previous_kit.c_str()) != 0) {
        swap.error = "cannot move the installed kit aside";
        return false;
      }
      if (rename(staged_kit_.c_str(), installed_kit.c_str()) != 0) {
        rename(previous_kit.c_str(), installed_kit.c_str());
        swap.error = "cannot move the new kit in";
        return false;
      }
      kit_swapped = true;
    }
    MakeFolders(config_.work_root + "/backup");
    swap = SwapApp(config_.installed_root, staged_folder_, backup, log);
    if (!swap.ok) {
      if (kit_swapped) {
        rename(installed_kit.c_str(), staged_kit_.c_str());
        if (Exists(previous_kit)) rename(previous_kit.c_str(), installed_kit.c_str());
        kit_swapped = false;
      }
      return false;
    }
    if (!RefreshRegisteredCopies(config_.installed_root + "/sce_sys", config_.appmeta_root,
                                 config_.registered_app_root, log))
      Log("some Home screen copies were not refreshed");
    return true;
  });
  if (!swap.ok) {
    Log("swap failed: " + swap.error);
    SaveState("staged", StagedFields());  // still staged: the next quit tries again
    return false;
  }
  RemoveOwnedTree(staged_folder_);
  SaveState("applied", applied(kit_swapped && Exists(previous_kit)));
  Log("swapped to " + staged_version_.ToString() + " (" + std::to_string(swap.moved) + " entries" +
      (kit_swapped ? ", new kit" : "") + "), backup " + backup);
  return true;
}

}  // namespace playden::update
