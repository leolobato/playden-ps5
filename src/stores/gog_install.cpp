// SPDX-License-Identifier: GPL-3.0-or-later
#include "stores/gog_install.h"

#include "gog/info_file.h"
#include "gog/install_script.h"
#include "gog/languages.h"
#include "installs/install_record.h"
#include "steam/backend/depot_installer.h"
#include "steam/install_volumes.h"
#include "stores/gog_update.h"

#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <ctime>
#include <limits>
#include <utility>
#include <vector>

namespace playden::stores {
namespace {

using Error = gog::GogInstaller::Outcome::Error;
using installs::InstallStep;

constexpr const char* kCantDownload = "This game can't be downloaded.";
constexpr const char* kCantUpdate = "This game can't be updated.";

// The install's own work on an explicit stack: the token call makes a TLS connection, and the info file and scripts
// are parsed as JSON; the install service's worker has the platform's default stack, small on the console.
bool RunOnStack(std::size_t stack, const std::function<void()>& work) {
  pthread_attr_t attr;
  if (pthread_attr_init(&attr) != 0) return false;
  bool ok = pthread_attr_setstacksize(&attr, stack) == 0;
  pthread_t thread{};
  ok = ok && pthread_create(&thread, &attr,
                            [](void* argument) -> void* {
                              (*static_cast<const std::function<void()>*>(argument))();
                              return nullptr;
                            },
                            const_cast<std::function<void()>*>(&work)) == 0;
  pthread_attr_destroy(&attr);
  if (ok) pthread_join(thread, nullptr);
  return ok;
}

std::string FoldCase(std::string text) {
  for (auto& c : text)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  return text;
}

bool IsRegular(const std::string& path) {
  struct stat info {};
  return ::lstat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode);
}

// The names at the top of `root`, as the listing spells them; empty when it can't be read.
std::vector<std::string> TopNames(const std::string& root) {
  std::vector<std::string> names;
  DIR* listing = ::opendir(root.c_str());
  if (!listing) return names;
  while (const auto* entry = ::readdir(listing)) {
    const std::string name = entry->d_name;
    if (name != "." && name != "..") names.push_back(name);
  }
  ::closedir(listing);
  return names;
}

// "/data/playden-games/gog-1" -> "Z:\data\playden-games\gog-1": prospero-win maps Z: to the console root, and the
// host's Wine sees its own root the same way.
std::string WindowsApp(const std::string& root) {
  std::string path = "Z:";
  for (const char c : root) path += c == '/' ? '\\' : c;
  return path;
}

const char* InstallerFailure(Error error) {
  switch (error) {
    case Error::kNetwork: return "Network problem. Resume to try again.";
    case Error::kCorrupt: return "Downloaded content failed verification. Resume to try again.";
    case Error::kNoSpace: return "Not enough free space.";
    case Error::kStorage: return "Could not write the game files.";
    case Error::kProtocol: return "GOG returned unexpected data.";
    case Error::kNoBuild: return "GOG has no Windows version of this game to install.";
    case Error::kNotOwned: return "GOG says this account doesn't own this game.";
    case Error::kNone:
    case Error::kCanceled:
    case Error::kSignedOut: break;
  }
  return "GOG returned unexpected data.";
}

void SyncFolder(const std::string& folder) {
  const int descriptor = ::open(folder.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (descriptor >= 0) {
    (void)::fsync(descriptor);
    ::close(descriptor);
  }
}

// `text` at `path` through a synced temporary file renamed over it, then the folder synced: the install record,
// written last, marks the install complete; an update's record and state are written the same way.
bool WriteSynced(const std::string& folder, const std::string& path, const std::string& text) {
  const std::string temporary = path + ".tmp";
  FILE* file = std::fopen(temporary.c_str(), "wb");
  if (!file) return false;
  bool ok = std::fwrite(text.data(), 1, text.size(), file) == text.size();
  ok = std::fflush(file) == 0 && ok;
  ok = ::fsync(::fileno(file)) == 0 && ok;
  ok = std::fclose(file) == 0 && ok;
  ok = ok && std::rename(temporary.c_str(), path.c_str()) == 0;
  if (!ok) {
    std::remove(temporary.c_str());
    return false;
  }
  SyncFolder(folder);
  return true;
}

bool WriteRecord(const std::string& root, const std::string& text) {
  return WriteSynced(root, root + "/" + installs::kInstallRecordName, text);
}

// The base product's play tasks, from goggame-<product>.info at the top of `folder`; `info_result` says what was found.
std::vector<installs::RecordedLaunch> ReadLaunches(const std::string& folder, std::uint64_t product,
                                                   const char** info_result) {
  std::vector<installs::RecordedLaunch> launches;
  const auto info_path = gog::FindInfoFile(folder, product);
  const auto info = info_path ? gog::ReadInfo(*info_path) : std::nullopt;
  if (info)
    for (const auto& option : gog::LaunchesFromInfo(*info))
      launches.push_back({option.executable, option.arguments, option.working_directory, option.description});
  if (info_result) *info_result = info ? "ok" : info_path ? "unreadable" : "missing";
  return launches;
}

// .playden-download holds only the installer's journal files: each goes, then the folder.
bool RemoveDownloadFolder(const std::string& root) {
  const std::string folder = root + "/" + gog::kDownloadDirName;
  struct stat info {};
  if (::lstat(folder.c_str(), &info) != 0) return errno == ENOENT;
  if (!S_ISDIR(info.st_mode)) return false;
  for (const auto& name : TopNames(folder)) {
    const std::string path = folder + "/" + name;
    if (std::remove(path.c_str()) != 0 && errno != ENOENT) return false;
  }
  return ::rmdir(folder.c_str()) == 0 || errno == ENOENT;
}

// `excluded` less what `products` hold: a record never names a product as both installed and left out.
std::vector<std::uint64_t> LeftOut(const std::vector<std::uint64_t>& excluded,
                                   const std::vector<std::uint64_t>& products) {
  std::vector<std::uint64_t> left_out;
  for (const auto id : excluded)
    if (std::find(products.begin(), products.end(), id) == products.end()) left_out.push_back(id);
  return left_out;
}

// The DLC sidecar's text: one decimal per line, in `excluded`'s (sorted) order; empty for none.
std::string FormatDlcSidecar(const std::vector<std::uint64_t>& excluded) {
  std::string text;
  for (const auto id : excluded) text += std::to_string(id) + "\n";
  return text;
}

enum class Sidecar { kRead, kMissing, kBad };

// The DLC sidecar at `path` into `ids` (sorted, once each). kBad: it can't be read, is too long or holds a line that
// isn't a whole decimal.
Sidecar ReadDlcSidecar(const std::string& path, std::vector<std::uint64_t>* ids) {
  ids->clear();
  struct stat info {};
  if (::lstat(path.c_str(), &info) != 0) return errno == ENOENT ? Sidecar::kMissing : Sidecar::kBad;
  if (!S_ISREG(info.st_mode)) return Sidecar::kBad;
  FILE* file = std::fopen(path.c_str(), "rb");
  if (!file) return Sidecar::kBad;
  // Up to kMaxGogRecordProducts IDs of at most 20 digits, with their newlines; one byte more says it is longer.
  constexpr std::size_t kLimit = installs::kMaxGogRecordProducts * 21;
  std::string text(kLimit + 1, '\0');
  const auto size = std::fread(&text[0], 1, text.size(), file);
  const bool failed = std::ferror(file) != 0;
  std::fclose(file);
  if (failed || size > kLimit) return Sidecar::kBad;
  text.resize(size);
  std::size_t start = 0;
  while (start < text.size()) {
    const auto end = text.find('\n', start);
    if (end == std::string::npos || end == start || end - start > 20) return Sidecar::kBad;
    std::uint64_t id = 0;
    for (std::size_t at = start; at < end; ++at) {
      const char c = text[at];
      if (c < '0' || c > '9') return Sidecar::kBad;
      const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
      if (id > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) return Sidecar::kBad;
      id = id * 10 + digit;
    }
    ids->push_back(id);
    start = end + 1;
  }
  std::sort(ids->begin(), ids->end());
  ids->erase(std::unique(ids->begin(), ids->end()), ids->end());
  return Sidecar::kRead;
}

bool Contains(const std::vector<std::uint64_t>& ids, std::uint64_t id) {
  return std::find(ids.begin(), ids.end(), id) != ids.end();
}

}  // namespace

std::vector<std::uint64_t> GogExcludedDlc(const installs::InstallOptions& options, bool update,
                                          const std::optional<installs::InstalledRecord>& record, std::uint64_t base) {
  std::vector<std::uint64_t> excluded;
  // GOG DLC only: the install service keeps a job's to its game's store.
  for (const auto& dlc : options.excluded_dlc)
    if (dlc.store == Store::kGog)
      if (const auto id = dlc.gog_product()) excluded.push_back(*id);
  if (update && !options.dlc_choice && record && record->gog)
    excluded.insert(excluded.end(), record->gog->excluded.begin(), record->gog->excluded.end());
  const auto dropped = [base](std::uint64_t id) { return id == 0 || id == base; };
  excluded.erase(std::remove_if(excluded.begin(), excluded.end(), dropped), excluded.end());
  std::sort(excluded.begin(), excluded.end());
  excluded.erase(std::unique(excluded.begin(), excluded.end()), excluded.end());
  if (excluded.size() > installs::kMaxGogRecordProducts) excluded.resize(installs::kMaxGogRecordProducts);
  return excluded;
}

GogInstallRunner::GogInstallRunner(gog::Session& session, std::function<gog::Transport()> make_transport,
                                   std::function<std::string()> content_language,
                                   std::function<void(const std::string&)> log, FreeBytes free_bytes)
    : session_(session),
      make_transport_(std::move(make_transport)),
      content_language_(std::move(content_language)),
      log_(std::move(log)),
      free_bytes_(std::move(free_bytes)) {}

void GogInstallRunner::Log(const std::string& line) const {
  if (log_) log_(line);
}

std::uint64_t GogInstallRunner::Free(const std::string& path) const {
  if (free_bytes_) return free_bytes_(path).value_or(0);
  // As the Steam installer: a drive that can't be read counts as full, unless the platform can't read any.
  if (const auto space = steam::QueryVolumeSpace(path)) return space->free_bytes;
  return steam::VolumeSpaceSupported() ? 0 : std::numeric_limits<std::uint64_t>::max();
}

std::string GogInstallRunner::DownloadServerName() const {
  std::lock_guard<std::mutex> lock(endpoint_mutex_);
  if (endpoint_ == "fastly" || endpoint_ == "gcore") return "GOG · " + endpoint_;
  return "GOG";
}

installs::InstallRunner::Result GogInstallRunner::Install(
    const GameId& id, const std::string& root, const installs::InstallOptions& options, const std::atomic<bool>& cancel,
    const std::function<void(std::uint64_t, std::uint64_t, std::uint64_t)>& progress,
    const std::function<void(InstallStep)>& step) {
  Result result;
  const auto product = id.store == Store::kGog && id.valid() ? id.gog_product() : std::nullopt;
  if (!product || root.empty() || root.front() != '/') {
    result.failure = kCantDownload;
    return result;
  }
  if (step) step(InstallStep::kDownloading);
  {
    std::lock_guard<std::mutex> lock(endpoint_mutex_);
    endpoint_.clear();
  }
  if (!RunOnStack(gog::kPlannerStackBytes, [&] { result = Run(id, *product, root, options, cancel, progress); })) {
    Log("gog install game=" + ToKey(id) + " step=plan result=no_thread");
    result = Result{};
    result.failure = InstallerFailure(Error::kNetwork);
  }
  return result;
}

bool GogInstallRunner::SessionReady(const std::atomic<bool>& cancel, const std::string& game, Result* result) {
  auto token = session_.Token(cancel);
  const auto status = token.status;
  token.Wipe();
  using Status = gog::AccessToken::Status;
  if (status == Status::kOk) return true;
  if (status == Status::kCanceled || cancel.load()) {
    result->canceled = true;
    return false;
  }
  const bool network = status == Status::kNetwork;
  Log(game + " step=plan result=" + (network ? "network" : "signed_out"));
  result->retry_later = true;
  result->waiting = network ? kWaitingForGog : kWaitingForSignIn;
  return false;
}

gog::GogInstaller GogInstallRunner::MakeInstaller() {
  gog::GogInstaller::Hooks hooks;
  hooks.make_transport = make_transport_;
  hooks.token = [this](const std::atomic<bool>& flag) { return session_.Token(flag); };
  hooks.invalidate = [this](std::uint64_t generation, std::string_view token) {
    session_.InvalidateAccessToken(generation, token);
  };
  hooks.free_bytes = [this](const std::string& path) { return Free(path); };
  hooks.pause = tuning_.pause;
  hooks.now_seconds = tuning_.now_seconds
                          ? tuning_.now_seconds
                          : std::function<std::int64_t()>([] { return static_cast<std::int64_t>(std::time(nullptr)); });
  hooks.log = log_;
  hooks.endpoint = [this](const char* endpoint) {
    std::lock_guard<std::mutex> lock(endpoint_mutex_);
    if (endpoint_ != endpoint) endpoint_ = endpoint;
  };
  hooks.parallel = tuning_.parallel;
  return gog::GogInstaller(std::move(hooks));
}

// A download that stopped: true when `error` ends the run, with `result` saying how. kNone goes on.
bool GogInstallRunner::Stopped(Error error, const std::atomic<bool>& cancel, Result* result) {
  switch (error) {
    case Error::kNone: return false;
    case Error::kCanceled:
      result->canceled = true;
      return true;
    case Error::kSignedOut:
      // Also a session GOG revoked mid-download: the journal stays for the next sign-in.
      if (cancel.load()) {
        result->canceled = true;
        return true;
      }
      result->retry_later = true;
      result->waiting = kWaitingForSignIn;
      return true;
    case Error::kNetwork:
      // GOG or its CDN out of reach (a builds fetch, five failed tries of a chunk): the job waits and tries again by
      // itself, keeping the journal, as a Steam download does.
      if (cancel.load()) {
        result->canceled = true;
        return true;
      }
      result->retry_later = true;
      result->waiting = kWaitingForGog;
      return true;
    default:
      result->failure = InstallerFailure(error);
      return true;
  }
}

installs::InstallRunner::Result GogInstallRunner::Run(
    const GameId& id, std::uint64_t product, const std::string& root, const installs::InstallOptions& options,
    const std::atomic<bool>& cancel, const std::function<void(std::uint64_t, std::uint64_t, std::uint64_t)>& progress) {
  Result result;
  const std::string game = "gog install game=" + ToKey(id);
  // A session first: without one the job waits, keeping its place and what it downloaded.
  if (!SessionReady(cancel, game, &result)) return result;
  std::string language = options.language;
  if (language.empty() && content_language_) language = content_language_();
  if (language.empty()) language = "english";
  Log(game + " step=plan result=ok language=" + language);

  auto installer = MakeInstaller();
  gog::GogInstaller::Target target;
  target.product = product;
  target.root = root;
  target.language = language;
  target.excluded = GogExcludedDlc(options, /*update=*/false, std::nullopt, product);
  const auto outcome = installer.Run(target, cancel, [&progress](const gog::GogInstaller::Progress& state) {
    if (progress) progress(state.done, state.total, state.held);
  });
  {
    char line[320];
    std::snprintf(line, sizeof(line),
                  " step=download result=%s files=%zu bytes=%llu corrupt=%d stalls=%d links=%d redone=%d parallel=%d",
                  outcome.error == Error::kNone ? "ok" : gog::InstallerErrorName(outcome.error), outcome.files,
                  static_cast<unsigned long long>(outcome.install_bytes), outcome.stats.corrupt_retries,
                  outcome.stats.stalls, outcome.stats.link_fetches, outcome.stats.redone_files, outcome.stats.parallel);
    Log(game + line);
  }
  if (Stopped(outcome.error, cancel, &result)) return result;
  // The record names the language the plan holds (a resume keeps its own), as a Steam code.
  std::string steam_language = gog::SteamCodeFor(outcome.language);
  if (steam_language.empty()) steam_language = language;
  return Finish(id, product, root, steam_language, outcome, target.excluded);
}

void GogInstallRunner::RunScripts(const std::string& game, const std::string& root,
                                  const std::vector<std::uint64_t>& products, const std::string& gog_language) {
  const auto names = TopNames(root);
  std::size_t scripts = 0;
  for (const auto installed : products) {
    const auto wanted = FoldCase("goggame-" + std::to_string(installed) + ".script");
    std::string path;
    for (const auto& name : names)
      if (FoldCase(name) == wanted && IsRegular(root + "/" + name)) {
        path = root + "/" + name;
        break;
      }
    if (path.empty()) continue;
    ++scripts;
    const auto script = gog::ReadScript(path);
    if (!script) {
      Log(game + " step=script product=" + std::to_string(installed) + " result=unreadable");
      continue;
    }
    gog::ScriptContext context;
    context.game_root = root;
    context.support_root = root + "/" + gog::kGogDirName + "/support/" + std::to_string(installed);
    context.product = std::to_string(installed);
    context.gog_language = gog_language;
    context.windows_app = WindowsApp(root);
    std::size_t left_out = 0;
    // Copies never go over a file that is there (supportData's overwrite aside): a player's file stays.
    const auto steps = gog::PlanScript(*script, context, /*skip_copies=*/false, &left_out);
    const auto run = gog::RunScript(steps);
    Log(game + " step=script product=" + std::to_string(installed) + " result=" + (run.failed ? "failed" : "ok") +
        " steps=" + std::to_string(steps.size()) + " changed=" + std::to_string(run.changed) +
        " skipped=" + std::to_string(left_out + run.skipped) + " failed=" + std::to_string(run.failed));
  }
  if (scripts == 0) Log(game + " step=script result=none");
}

installs::InstallRunner::Result GogInstallRunner::Finish(const GameId& id, std::uint64_t product,
                                                         const std::string& root, const std::string& steam_language,
                                                         const gog::GogInstaller::Outcome& outcome,
                                                         const std::vector<std::uint64_t>& excluded) {
  Result result;
  const std::string game = "gog install game=" + ToKey(id);

  // 1. The base game's play tasks.
  const char* info_result = "missing";
  const auto launches = ReadLaunches(root, product, &info_result);

  // 2. Each product's install script, once with its copies.
  RunScripts(game, root, outcome.products, outcome.language);

  // 3. The record, last.
  installs::GogRecord record;
  record.build_id = outcome.build_id;
  record.generation = outcome.generation;
  record.products = outcome.products;
  record.language = outcome.language;
  record.meta_id = outcome.meta_id;
  record.excluded = LeftOut(excluded, outcome.products);
  const auto text =
      installs::FormatGogInstallRecord(id, outcome.install_bytes, outcome.files, steam_language, record, launches);
  const bool written = WriteRecord(root, text);
  Log(game + " step=record result=" + (written ? "ok" : "failed") + " files=" + std::to_string(outcome.files) +
      " bytes=" + std::to_string(outcome.install_bytes) + " info=" + info_result +
      " launches=" + std::to_string(launches.size()));
  if (!written) {
    result.failure = InstallerFailure(Error::kStorage);
    return result;
  }

  // 4. The journal: the record says the install is complete.
  if (!RemoveDownloadFolder(root)) Log(game + " step=record result=journal_kept");
  result.ok = true;
  result.install_bytes = outcome.install_bytes;
  return result;
}

installs::InstallRunner::Result GogInstallRunner::Update(
    const GameId& id, const std::string& root, const installs::InstallOptions& options, const std::atomic<bool>& cancel,
    const std::function<void(std::uint64_t, std::uint64_t, std::uint64_t)>& progress,
    const std::function<void(InstallStep)>& step, const std::function<bool()>& may_apply) {
  // An update keeps the installed language, and GOG has no branches Playden installs: options choose only the DLC.
  Result result;
  const auto product = id.store == Store::kGog && id.valid() ? id.gog_product() : std::nullopt;
  if (!product || root.empty() || root.front() != '/') {
    result.failure = kCantUpdate;
    return result;
  }
  {
    std::lock_guard<std::mutex> lock(endpoint_mutex_);
    endpoint_.clear();
  }
  if (!RunOnStack(gog::kPlannerStackBytes,
                  [&] { result = RunUpdate(id, *product, root, options, cancel, progress, step, may_apply); })) {
    Log("gog update game=" + ToKey(id) + " step=plan result=no_thread");
    result = Result{};
    result.failure = InstallerFailure(Error::kNetwork);
  }
  return result;
}

installs::InstallRunner::Result GogInstallRunner::RunUpdate(
    const GameId& id, std::uint64_t product, const std::string& root, const installs::InstallOptions& options,
    const std::atomic<bool>& cancel,
    const std::function<void(std::uint64_t, std::uint64_t, std::uint64_t)>& progress,
    const std::function<void(InstallStep)>& step, const std::function<bool()>& may_apply) {
  Result result;
  const std::string game = "gog update game=" + ToKey(id);
  const auto waiting_for_game = [&] {
    Log(game + " step=apply result=game_running");
    Result waiting;
    waiting.retry_later = true;
    waiting.waiting = kWaitingForGame;
    return waiting;
  };

  // (a) Putting its files in place already: it finishes without a session or the network, once the game is closed.
  const auto state = installs::ReadUpdateState(root);
  if (state && state->phase != installs::UpdatePhase::kStaging) {
    if (may_apply && !may_apply()) return waiting_for_game();
    if (step) step(InstallStep::kApplying);
    return Apply(id, root, cancel);
  }

  // (b) Staging: the installed build's record, a session, then the changed files beside the game.
  const auto record = installs::ReadInstallRecord(root + "/" + installs::kInstallRecordName);
  if (!record || !record->gog) {
    Log(game + " step=plan result=no_record");
    result.failure = kCantUpdate;
    return result;
  }
  // The language the game has, as a Steam code: it selects the depots, so an update keeps it.
  std::string steam_language = gog::SteamCodeFor(record->gog->language);
  if (steam_language.empty()) steam_language = record->language;
  if (steam_language.empty()) steam_language = "english";
  // The DLC left out: the record's too, unless the update changes the game's DLC.
  const auto excluded = GogExcludedDlc(options, /*update=*/true, record, product);

  const std::string work = root + "/" + installs::kUpdateDirName;
  const std::string state_path = work + "/" + installs::kUpdateStateName;
  const std::string dlc_path = work + "/" + kGogUpdateDlcName;
  // A work folder staged for another DLC choice is never reused: Stage reuses its plan and staged files as they are,
  // and a full staging would go in place at once. Its sidecar names the choice; without one (a build before the
  // sidecar) it was the record's. Still staging here, so the discard can't stop an update halfway.
  {
    struct stat info {};
    if (::lstat(work.c_str(), &info) == 0) {
      std::vector<std::uint64_t> planned;
      const auto read = ReadDlcSidecar(dlc_path, &planned);
      if (read == Sidecar::kMissing) planned = GogExcludedDlc({}, /*update=*/true, record, product);
      if (read == Sidecar::kBad || planned != excluded) {
        if (!DiscardGogUpdate(root)) {
          Log(game + " step=plan result=storage");
          result.failure = InstallerFailure(Error::kStorage);
          return result;
        }
        Log(game + " step=plan result=restaged");
      }
    }
  }
  installs::UpdateState staging_state;
  staging_state.phase = installs::UpdatePhase::kStaging;
  staging_state.language = steam_language;
  // Everything staged already (the game was running when it finished): putting it in place needs no session.
  if (GogUpdateStaged(root)) {
    Log(game + " step=stage result=ready");
  } else {
    if (!SessionReady(cancel, game, &result)) return result;
    {
      struct stat info {};
      if ((::mkdir(work.c_str(), 0777) != 0 && errno != EEXIST) || ::lstat(work.c_str(), &info) != 0 ||
          !S_ISDIR(info.st_mode)) {
        Log(game + " step=plan result=storage");
        result.failure = InstallerFailure(Error::kStorage);
        return result;
      }
    }
    const bool created = !installs::ReadUpdateState(root);
    if (created && !WriteSynced(work, state_path, installs::FormatUpdateState(staging_state))) {
      Log(game + " step=plan result=storage");
      result.failure = InstallerFailure(Error::kStorage);
      return result;
    }
    // The DLC choice this staging is planned with, before Stage plans anything.
    if ((created || !IsRegular(dlc_path)) && !WriteSynced(work, dlc_path, FormatDlcSidecar(excluded))) {
      Log(game + " step=plan result=storage");
      result.failure = InstallerFailure(Error::kStorage);
      return result;
    }
    Log(game + " step=plan result=ok language=" + steam_language);

    // Without an installed plan the first Stage hashes the game's files before anything downloads.
    const bool checking = !IsRegular(work + "/" + gog::kUpdateStagedName) &&
                          !gog::ReadPlanHeader(root + "/" + gog::kPlanPath).has_value();
    std::atomic<bool> downloading{!checking};
    if (step) step(checking ? InstallStep::kCheckingFiles : InstallStep::kDownloading);
    // The record says a Stage finished: gone before this one, written again only once it has.
    const std::string record_path = work + "/" + kGogUpdateRecordName;
    if (std::remove(record_path.c_str()) == 0) {
      SyncFolder(work);
    } else if (errno != ENOENT) {
      Log(game + " step=plan result=storage");
      result.failure = InstallerFailure(Error::kStorage);
      return result;
    }
    auto installer = MakeInstaller();
    gog::GogInstaller::Target target;
    target.product = product;
    target.root = root;
    target.language = steam_language;
    target.excluded = excluded;
    const auto outcome = installer.Stage(target, cancel, [&](const gog::GogInstaller::Progress& state) {
      if (!downloading.exchange(true) && step) step(InstallStep::kDownloading);
      if (progress) progress(state.done, state.total, state.held);
    });
    {
      // The DLC the game had (installed or turned off) that this run keeps on but the plan doesn't hold: no longer
      // owned, or not in the meta. A count only.
      std::size_t dlc_missing = 0;
      if (outcome.error == Error::kNone) {
        auto known = record->gog->products;
        known.insert(known.end(), record->gog->excluded.begin(), record->gog->excluded.end());
        std::sort(known.begin(), known.end());
        known.erase(std::unique(known.begin(), known.end()), known.end());
        for (const auto id : known)
          if (id != product && !Contains(excluded, id) && !Contains(outcome.products, id)) ++dlc_missing;
      }
      char line[352];
      std::snprintf(line, sizeof(line),
                    " step=stage result=%s files=%zu changed=%llu removed=%llu hashed=%llu bytes=%llu corrupt=%d "
                    "links=%d parallel=%d dlc_missing=%zu",
                    outcome.error == Error::kNone ? "ok" : gog::InstallerErrorName(outcome.error), outcome.files,
                    static_cast<unsigned long long>(outcome.staged_files),
                    static_cast<unsigned long long>(outcome.removed_files),
                    static_cast<unsigned long long>(outcome.hashed_files),
                    static_cast<unsigned long long>(outcome.download_bytes), outcome.stats.corrupt_retries,
                    outcome.stats.link_fetches, outcome.stats.parallel, dlc_missing);
      Log(game + line);
    }
    if (Stopped(outcome.error, cancel, &result)) return result;

    // (c) The new record, before the phase leaves staging: an apply that is interrupted finishes from it. Its play
    // tasks come from the new info file when the update staged one, else from the installed one.
    const char* info_result = "missing";
    std::vector<installs::RecordedLaunch> launches;
    const std::string staging = work + "/" + gog::kUpdateStagingName;
    if (gog::FindInfoFile(staging, product)) launches = ReadLaunches(staging, product, &info_result);
    else launches = ReadLaunches(root, product, &info_result);
    installs::GogRecord gog_record;
    gog_record.build_id = outcome.build_id;
    gog_record.generation = outcome.generation;
    gog_record.products = outcome.products;
    gog_record.language = outcome.language;
    gog_record.meta_id = outcome.meta_id;
    // Never a product the plan holds (a reused plan keeps its own products).
    gog_record.excluded = LeftOut(excluded, outcome.products);
    const auto text = installs::FormatGogInstallRecord(id, outcome.install_bytes, outcome.files, record->language,
                                                       gog_record, launches);
    const bool written = WriteSynced(work, record_path, text);
    Log(game + " step=record result=" + (written ? "ok" : "failed") + " files=" + std::to_string(outcome.files) +
        " bytes=" + std::to_string(outcome.install_bytes) + " info=" + info_result +
        " launches=" + std::to_string(launches.size()));
    if (!written) {
      result.failure = InstallerFailure(Error::kStorage);
      return result;
    }
  }

  // Every folder the new build needs can be made: once replacing starts the update can only finish, and something the
  // game wrote where a folder goes would stop it there with the game locked.
  if (!GogUpdateFoldersClear(root)) {
    Log(game + " step=apply result=blocked");
    result.failure = InstallerFailure(Error::kStorage);
    return result;
  }
  // (d) From here the game's files change: not while it runs.
  if (may_apply && !may_apply()) return waiting_for_game();
  // (e) Replacing, then the files in place.
  auto replacing = staging_state;
  if (const auto saved = installs::ReadUpdateState(root)) replacing = *saved;
  replacing.phase = installs::UpdatePhase::kReplacing;
  if (!WriteSynced(work, state_path, installs::FormatUpdateState(replacing))) {
    Log(game + " step=apply result=storage");
    result.failure = InstallerFailure(Error::kStorage);
    return result;
  }
  if (step) step(InstallStep::kApplying);
  return Apply(id, root, cancel);
}

installs::InstallRunner::Result GogInstallRunner::Apply(const GameId& id, const std::string& root,
                                                        const std::atomic<bool>& cancel) {
  Result result;
  const std::string game = "gog update game=" + ToKey(id);
  GogUpdateHooks hooks;
  // Each installed product's script, as an install runs it: the products and language from the new plan, now in place.
  hooks.after_replace = [this, &game](const std::string& folder) {
    const auto header = gog::ReadPlanHeader(folder + "/" + gog::kPlanPath);
    if (!header) {
      Log(game + " step=script result=no_plan");
      return;
    }
    RunScripts(game, folder, header->products, header->language);
  };
  const auto applied = ApplyGogUpdate(root, cancel, hooks);
  Log(game + " step=apply result=" + (applied.error == Error::kNone ? "ok" : gog::InstallerErrorName(applied.error)) +
      " replaced=" + std::to_string(applied.replaced) + " deleted=" + std::to_string(applied.deleted) +
      " skipped=" + std::to_string(applied.skipped));
  switch (applied.error) {
    case Error::kNone:
      Log(game + " step=finish result=ok bytes=" + std::to_string(applied.install_bytes));
      result.ok = true;
      result.install_bytes = applied.install_bytes;
      return result;
    case Error::kCanceled:
      result.canceled = true;
      return result;
    case Error::kProtocol:
      result.failure = kCantUpdate;
      return result;
    default:
      result.failure = InstallerFailure(Error::kStorage);
      return result;
  }
}

std::string GogInstallRunner::DiscardUpdate(const std::string& root) {
  std::string failure;
  const bool discarded = DiscardGogUpdate(root, &failure);
  Log(std::string("gog update-discard result=") + (discarded ? "ok" : "failed"));
  return discarded ? std::string() : (failure.empty() ? "The update's files could not be deleted." : failure);
}

std::string GogInstallRunner::Remove(const std::string& root) {
  std::string failure;
  const bool removed = steam::RemoveManagedInstall(root, &failure);
  Log(std::string("gog install-removal result=") + (removed ? "ok" : "failed"));
  return removed ? std::string() : (failure.empty() ? "The game's files could not be deleted." : failure);
}

}  // namespace playden::stores
