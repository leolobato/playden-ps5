// SPDX-License-Identifier: GPL-3.0-or-later
#include "stores/gog_install.h"

#include "gog/info_file.h"
#include "gog/install_script.h"
#include "gog/languages.h"
#include "installs/install_record.h"
#include "steam/backend/depot_installer.h"
#include "steam/install_volumes.h"

#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

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
    case Error::kGen1Only: return "Playden can't install this GOG game yet: it uses an older GOG download format.";
    case Error::kNoBuild: return "GOG has no Windows version of this game to install.";
    case Error::kNotOwned: return "GOG says this account doesn't own this game.";
    case Error::kNone:
    case Error::kCanceled:
    case Error::kSignedOut: break;
  }
  return "GOG returned unexpected data.";
}

// The record through a synced temporary file renamed over it, then the folder synced: written last, it marks the
// install complete.
bool WriteRecord(const std::string& root, const std::string& text) {
  const std::string path = root + "/" + installs::kInstallRecordName;
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
  const int folder = ::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (folder >= 0) {
    (void)::fsync(folder);
    ::close(folder);
  }
  return true;
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

}  // namespace

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

installs::InstallRunner::Result GogInstallRunner::Run(
    const GameId& id, std::uint64_t product, const std::string& root, const installs::InstallOptions& options,
    const std::atomic<bool>& cancel, const std::function<void(std::uint64_t, std::uint64_t, std::uint64_t)>& progress) {
  Result result;
  const std::string game = "gog install game=" + ToKey(id);
  // A session first: without one the job waits, keeping its place and what it downloaded.
  {
    auto token = session_.Token(cancel);
    const auto status = token.status;
    token.Wipe();
    using Status = gog::AccessToken::Status;
    if (status == Status::kCanceled || (status != Status::kOk && cancel.load())) {
      result.canceled = true;
      return result;
    }
    if (status != Status::kOk) {
      const bool network = status == Status::kNetwork;
      Log(game + " step=plan result=" + (network ? "network" : "signed_out"));
      result.retry_later = true;
      result.waiting = network ? kWaitingForGog : kWaitingForSignIn;
      return result;
    }
  }
  std::string language = options.language;
  if (language.empty() && content_language_) language = content_language_();
  if (language.empty()) language = "english";
  Log(game + " step=plan result=ok language=" + language);

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
  gog::GogInstaller installer(std::move(hooks));
  gog::GogInstaller::Target target;
  target.product = product;
  target.root = root;
  target.language = language;
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
  switch (outcome.error) {
    case Error::kNone: break;
    case Error::kCanceled:
      result.canceled = true;
      return result;
    case Error::kSignedOut:
      // Also a session GOG revoked mid-download: the journal stays for the next sign-in.
      if (cancel.load()) {
        result.canceled = true;
        return result;
      }
      result.retry_later = true;
      result.waiting = kWaitingForSignIn;
      return result;
    case Error::kNetwork:
      // GOG or its CDN out of reach (a builds fetch, five failed tries of a chunk): the job waits and tries again by
      // itself, keeping the journal, as a Steam download does.
      if (cancel.load()) {
        result.canceled = true;
        return result;
      }
      result.retry_later = true;
      result.waiting = kWaitingForGog;
      return result;
    default:
      result.failure = InstallerFailure(outcome.error);
      return result;
  }
  // The record names the language the plan holds (a resume keeps its own), as a Steam code.
  std::string steam_language = gog::SteamCodeFor(outcome.language);
  if (steam_language.empty()) steam_language = language;
  return Finish(id, product, root, steam_language, outcome);
}

installs::InstallRunner::Result GogInstallRunner::Finish(const GameId& id, std::uint64_t product,
                                                         const std::string& root, const std::string& steam_language,
                                                         const gog::GogInstaller::Outcome& outcome) {
  Result result;
  const std::string game = "gog install game=" + ToKey(id);

  // 1. The base game's play tasks.
  std::vector<installs::RecordedLaunch> launches;
  const auto info_path = gog::FindInfoFile(root, product);
  const auto info = info_path ? gog::ReadInfo(*info_path) : std::nullopt;
  if (info)
    for (const auto& option : gog::LaunchesFromInfo(*info))
      launches.push_back({option.executable, option.arguments, option.working_directory, option.description});
  const char* info_result = info ? "ok" : info_path ? "unreadable" : "missing";

  // 2. Each product's install script, once with its copies.
  {
    const auto names = TopNames(root);
    std::size_t scripts = 0;
    for (const auto installed : outcome.products) {
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
      context.gog_language = outcome.language;
      context.windows_app = WindowsApp(root);
      std::size_t left_out = 0;
      const auto steps = gog::PlanScript(*script, context, /*skip_copies=*/false, &left_out);
      const auto run = gog::RunScript(steps);
      Log(game + " step=script product=" + std::to_string(installed) + " result=" + (run.failed ? "failed" : "ok") +
          " steps=" + std::to_string(steps.size()) + " changed=" + std::to_string(run.changed) +
          " skipped=" + std::to_string(left_out + run.skipped) + " failed=" + std::to_string(run.failed));
    }
    if (scripts == 0) Log(game + " step=script result=none");
  }

  // 3. The record, last.
  installs::GogRecord record;
  record.build_id = outcome.build_id;
  record.generation = 2;
  record.products = outcome.products;
  record.language = outcome.language;
  record.meta_id = outcome.meta_id;
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

std::string GogInstallRunner::Remove(const std::string& root) {
  std::string failure;
  const bool removed = steam::RemoveManagedInstall(root, &failure);
  Log(std::string("gog install-removal result=") + (removed ? "ok" : "failed"));
  return removed ? std::string() : (failure.empty() ? "The game's files could not be deleted." : failure);
}

}  // namespace playden::stores
