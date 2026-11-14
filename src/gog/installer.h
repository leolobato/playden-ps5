// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Downloads a GOG gen 2 Windows build into a game folder (GOG_PROTOCOL.md §4-6), below the install runner. Run plans
// the install once, saves the plan, then downloads chunks on its own workers and checks every one, so a run stopped at
// any point continues from the chunks it had.
//
// The folder (<root>):
// - .playden-download/ is made before anything else is written, so RemoveManagedInstall always knows the folder as
//   Playden's: Cancel and Uninstall can delete a partial install. It holds gog-journal, append-only: "c\t<file>\t<chunk>"
//   once a chunk was written and both its hashes passed, "f\t<file>" once a file was finished and checked. A torn last
//   line is ignored.
// - .playden-gog/files is the plan, line by line: a header
//   "v1\t<build>\t<meta>\t<GOG language>\t<products, comma-separated>\t<total size>\t<total compressed>", then per entry
//   "D\t<path>", or "F\t<path>\t<source>\t<size>\t<md5|->\t<sha256|->\t<chunks>" followed by one
//   "C\t<compressed md5>\t<compressed size>\t<md5>\t<size>" line per chunk. The source is a product ID, or dep:<id> for
//   a game-folder dependency (DOSBox, ScummVM). A saved plan is reused as it is: a resume never fetches builds or
//   manifests again, and never moves to a newer build in the middle of a download.
// - .playden-gog/meta/<meta> and .playden-gog/manifests/<hash> keep the build meta and depot manifests (zlib, as GOG
//   serves them, each checked against its MD5) for verify and updates; .playden-gog/support/<product>/ the support
//   files outside app/.
// - .playden-files lists every placed path ("d\t<path>", "f\t<path>"), written before the plan.
//
// The installer writes no install record and leaves .playden-download in place; the runner writes the record.
//
// Log lines name the step, counts, sizes, HTTP statuses and the endpoint names "fastly" and "gcore": never a URL, a
// token, a secure link's parameters or the account.

#include "gog/http.h"
#include "gog/session.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace playden::gog {

// The same names as the Steam installer's (DepotInstaller::kJournalDir, kFileList), which RemoveManagedInstall reads.
inline constexpr char kDownloadDirName[] = ".playden-download";
inline constexpr char kFileListName[] = ".playden-files";
inline constexpr char kGogDirName[] = ".playden-gog";
inline constexpr char kPlanPath[] = ".playden-gog/files";
inline constexpr char kJournalPath[] = ".playden-download/gog-journal";
// Free space kept beyond the download, as steam::kInstallSpaceMargin.
inline constexpr std::uint64_t kGogSpaceMargin = 64ull * 1024 * 1024;
inline constexpr int kDefaultParallel = 4;
inline constexpr int kMaxParallel = 16;
// Explicit stacks (the console's default is small): the planner parses JSON and makes TLS connections.
inline constexpr std::size_t kWorkerStackBytes = 256 * 1024;
inline constexpr std::size_t kPlannerStackBytes = 1024 * 1024;
// A secure link is fetched again this long before its expires_at.
inline constexpr std::int64_t kLinkRefreshSeconds = 600;
// This many stall timeouts within the window take one worker off, down to one.
inline constexpr int kStallsToSlowDown = 3;
inline constexpr std::int64_t kStallWindowSeconds = 60;
// A chunk that still gets 401 or 403 after this many fresh links fails with kNetwork.
inline constexpr int kMaxLinkRefreshesPerChunk = 2;

class GogInstaller {
 public:
  struct Hooks {
    // A transport for one thread: called once by the planner and once per download worker.
    std::function<Transport()> make_transport;
    // The session's access token (Session::Token).
    std::function<AccessToken(const std::atomic<bool>& cancel)> token;
    // An API refused this token (Session::InvalidateAccessToken).
    std::function<void(std::uint64_t generation, std::string_view token)> invalidate;
    // Free bytes on the drive holding `path`.
    std::function<std::uint64_t(const std::string& path)> free_bytes;
    // Waits before a chunk retry; SleepPause when empty.
    std::function<void(std::uint32_t ms, const std::atomic<bool>& cancel)> pause;
    // Wall-clock Unix seconds (a secure link's expires_at is one), also for the stall window.
    std::function<std::int64_t()> now_seconds;
    std::function<void(const std::string& line)> log;
    int parallel = kDefaultParallel;
  };

  struct Target {
    std::uint64_t product = 0;
    std::string root;      // absolute; made when missing
    std::string language;  // Playden's content language, Steam's name ("english")
  };

  // Compressed download bytes done and in all, and the installed bytes already finished when the run started.
  struct Progress {
    std::uint64_t done = 0;
    std::uint64_t total = 0;
    std::uint64_t held = 0;
  };

  struct Outcome {
    enum class Error {
      kNone,
      kCanceled,   // the journal is kept: the next run continues
      kSignedOut,  // no GOG session, or GOG refused the token twice
      kNetwork,    // try again later
      kCorrupt,    // a chunk or a file kept failing its checks
      kNoSpace,
      kStorage,
      kProtocol,
      kGen1Only,   // the default build is gen 1 (Phase 4)
      kNoBuild,    // no default-branch Windows build
      kNotOwned,
    };
    Error error = Error::kProtocol;
    std::uint64_t install_bytes = 0;
    std::size_t files = 0;
    std::string build_id;
    std::string meta_id;
    std::vector<std::uint64_t> products;
    std::string language;  // the GOG code installed ("en-US")
    struct Stats {
      int corrupt_retries = 0;  // chunk attempts that failed a check and were tried again
      int stalls = 0;
      int link_fetches = 0;     // secure and open links fetched
      int redone_files = 0;     // files fetched again after their whole-file check failed
      int parallel = 0;         // workers active at the end
    } stats;
  };

  explicit GogInstaller(Hooks hooks);
  Outcome Run(const Target& target, const std::atomic<bool>& cancel,
              const std::function<void(const Progress&)>& progress = {});

 private:
  Hooks hooks_;
};

const char* InstallerErrorName(GogInstaller::Outcome::Error error);

}  // namespace playden::gog
