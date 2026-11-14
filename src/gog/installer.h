// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Downloads a GOG Windows build into a game folder (GOG_PROTOCOL.md §4-6), below the install runner: a gen 2 build as
// chunks, a gen 1 build as byte ranges of its products' main.bin. Run plans the install once, saves the plan, then
// downloads chunks and ranges on its own workers and checks every file, so a run stopped at any point continues from
// the chunks and ranges it had.
//
// The folder (<root>):
// - .playden-download/ is made before anything else is written, so RemoveManagedInstall always knows the folder as
//   Playden's: Cancel and Uninstall can delete a partial install. It holds gog-journal, append-only: "c\t<file>\t<unit>"
//   once a chunk or range was written (and a chunk's hashes passed), "f\t<file>" once a file was finished and checked.
//   A torn last line is ignored.
// - .playden-gog/files is the plan, line by line: a header
//   "v2\t<generation>\t<build>\t<meta>\t<GOG language>\t<products, comma-separated>\t<total size>\t<total download>\t
//   <timestamp or ->", then per entry "D\t<path>", or "F\t<path>\t<source>\t<size>\t<md5|->\t<sha256|->\t<units>"
//   followed by one line per unit: "C\t<compressed md5>\t<compressed size>\t<md5>\t<size>" for a gen 2 chunk, or
//   "R\t<blob offset>\t<length>" for a byte range of a gen 1 main.bin (at most kGen1RangeBytes). A file's units are all
//   chunks or all ranges. The source is a product ID (whose secure link serves the chunks, or for gen 1 whose main.bin
//   holds the ranges), or dep:<id> for a dependency from the public store (DOSBox, ScummVM, and the __redist
//   installers; gen 2 chunks in a gen 1 plan too).
//   For gen 1 the meta is the MD5 of repository.json, the timestamp the build's legacy_build_id, and the total download
//   counts a range by its length. A Phase 3 plan's "v1\t<build>\t<meta>\t…\t<total compressed>" header reads as
//   generation 2. A saved plan is reused as it is: a resume never fetches builds or manifests again, and never moves
//   to a newer build in the middle of a download.
// - .playden-gog/meta/<meta> keeps the build meta (zlib, checked against its MD5) or the gen 1 repository.json (plain
//   JSON, named by its MD5); .playden-gog/manifests/<hash> the gen 2 depot manifests (zlib, checked) and
//   .playden-gog/manifests/v1-<name> the gen 1 ones (plain JSON; GOG gives no hash, so they are fetched again only when
//   planning again), for verify and updates; .playden-gog/support/<product>/ the support files outside app/.
// - .playden-gog/redist/<ID>/ holds every __redist installer the build names (gen 2 meta dependencies, gen 1 redist
//   depots), and .playden-gog/redist.list says which program of each to run with which arguments (gog/dependencies.h).
//   The list is written with each plan, before the file list.
// - .playden-files lists every placed path ("d\t<path>", "f\t<path>"), redist files too, written before the plan.
//
// The installer writes no install record and leaves .playden-download in place; the runner writes the record. A gen 1
// repository's support commands (GOG's Galaxy helper) are never run.
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
    // A chunk came from this CDN endpoint: "fastly", "gcore" or "other" (EndpointLogName), never a host. Called on
    // the download workers; may be empty.
    std::function<void(const char* endpoint)> endpoint;
    int parallel = kDefaultParallel;
  };

  struct Target {
    std::uint64_t product = 0;
    std::string root;      // absolute; made when missing
    std::string language;  // Playden's content language, Steam's name ("english")
  };

  // Download bytes done and in all (compressed chunks, and gen 1 ranges by their length), and the installed bytes already finished when the run started.
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
      kProtocol,   // also a gen 1 repository Playden can't read, or one of another product or build
      kNoBuild,    // no default-branch Windows build
      kNotOwned,
    };
    Error error = Error::kProtocol;
    std::uint64_t install_bytes = 0;
    std::size_t files = 0;
    int generation = 0;          // the plan's: 1 or 2
    std::string build_id;        // the builds item's build_id
    std::string meta_id;         // gen 2: the build meta's MD5; gen 1: repository.json's
    std::uint64_t timestamp = 0; // gen 1: the build's legacy_build_id; 0 for gen 2
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
