#include "steam/backend/depot_installer.h"

#include "steam/backend/depot_chunk.h"
#include "steam/backend/gzip.h"
#include "steam/install_volumes.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <limits>
#include <condition_variable>
#include <mutex>
#include <pthread.h>
#include <charconv>
#include <map>
#include <memory>
#include <set>

namespace playden::steam {
namespace {
// An imported game's copy journal (stores/local/copy_plan.h kCopyJournalDir).
constexpr const char* kCopyJournalDir = ".playden-copy";

using Clock = std::chrono::steady_clock;
struct ScopeExit {
  std::function<void()> fn;
  ~ScopeExit() { fn(); }
};
// Accumulate full clock precision, converting only when a snapshot is emitted.
template <class Fn>
auto Timed(Clock::duration& elapsed, Fn fn) -> decltype(fn()) {
  const auto start = Clock::now();
  ScopeExit end{[&] { elapsed += Clock::now() - start; }};
  return fn();
}
std::uint64_t Milliseconds(Clock::duration elapsed) {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
}

std::string Hex(const std::array<std::uint8_t, 20>& bytes) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string text;
  for (const auto byte : bytes) {
    text.push_back(kDigits[byte >> 4]);
    text.push_back(kDigits[byte & 15]);
  }
  return text;
}

std::string Lower(std::string text) {
  for (auto& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return text;
}

bool MakeDirs(const std::string& path, std::uint64_t* created = nullptr) {
  if (path.empty()) return false;
  std::string current;
  std::size_t start = 0;
  if (path[0] == '/') { current = "/"; start = 1; }
  while (start <= path.size()) {
    const auto slash = path.find('/', start);
    const auto part = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
    if (!part.empty()) {
      if (current.size() > 1 || (current.size() == 1 && current[0] != '/')) current += '/';
      current += part;
      if (mkdir(current.c_str(), 0777) == 0) {
        if (created) ++*created;
      } else {
        struct stat info {};
        if (stat(current.c_str(), &info) != 0 || !S_ISDIR(info.st_mode)) return false;
      }
    }
    if (slash == std::string::npos) break;
    start = slash + 1;
  }
  return true;
}

std::string Parent(const std::string& path) {
  const auto slash = path.rfind('/');
  return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

bool WriteAt(const std::string& path, std::uint64_t file_size, std::uint64_t offset,
             const std::vector<std::uint8_t>& data) {
  const int fd = open(path.c_str(), O_WRONLY | O_CREAT, 0666);
  if (fd < 0) return false;
  bool ok = true;
  struct stat info {};
  if (fstat(fd, &info) != 0) ok = false;
  if (ok && static_cast<std::uint64_t>(info.st_size) != file_size &&
      ftruncate(fd, static_cast<off_t>(file_size)) != 0)
    ok = false;
  std::size_t written = 0;
  while (ok && written < data.size()) {
    const auto result = pwrite(fd, data.data() + written, data.size() - written,
                               static_cast<off_t>(offset + written));
    if (result <= 0) ok = false;
    else written += static_cast<std::size_t>(result);
  }
  if (close(fd) != 0) ok = false;
  return ok;
}

bool EnsureFileSize(const std::string& path, std::uint64_t size) {
  const int fd = open(path.c_str(), O_WRONLY | O_CREAT, 0666);
  if (fd < 0) return false;
  const bool ok = ftruncate(fd, static_cast<off_t>(size)) == 0;
  return close(fd) == 0 && ok;
}

std::uint64_t DefaultFreeBytes(const std::string& path) {
  const auto space = QueryVolumeSpace(path);
  if (space) return space->free_bytes;
  return VolumeSpaceSupported() ? 0 : std::numeric_limits<std::uint64_t>::max();
}

// Deletes a tree without following links. `error` keeps the first failure's errno.
// A folder's entries other than "." and "..", read before any is deleted: deleting while reading
// can make readdir skip entries. False when the folder cannot be opened.
bool ListEntries(const std::string& path, std::vector<std::string>& names) {
  DIR* dir = opendir(path.c_str());
  if (!dir) return false;
  while (dirent* entry = readdir(dir)) {
    std::string name = entry->d_name;
    if (name != "." && name != "..") names.push_back(std::move(name));
  }
  closedir(dir);
  return true;
}

bool RemoveTree(const std::string& path, int* error = nullptr) {
  const auto failed = [error] {
    if (error && *error == 0) *error = errno;
    return false;
  };
  struct stat info {};
  if (lstat(path.c_str(), &info) != 0) return errno == ENOENT || failed();
  if (!S_ISDIR(info.st_mode)) return unlink(path.c_str()) == 0 || failed();
  std::vector<std::string> names;
  if (!ListEntries(path, names)) return failed();
  bool ok = true;
  for (const auto& name : names) ok = RemoveTree(path + "/" + name, error) && ok;
  return (rmdir(path.c_str()) == 0 || failed()) && ok;
}

// Bytes the filesystem has allocated to a file, at most its size; 0 when absent.
std::uint64_t AllocatedBytes(const std::string& path, std::uint64_t size) {
  struct stat info {};
  if (stat(path.c_str(), &info) != 0 || info.st_blocks <= 0) return 0;
  return std::min<std::uint64_t>(size, static_cast<std::uint64_t>(info.st_blocks) * 512);
}

bool Exists(const std::string& path) {
  struct stat info {};
  return stat(path.c_str(), &info) == 0;
}

bool SameFile(const ManifestFile& left, const ManifestFile& right) {
  if (left.size != right.size || left.flags != right.flags || left.link_target != right.link_target ||
      left.sha_content != right.sha_content || left.chunks.size() != right.chunks.size())
    return false;
  for (std::size_t index = 0; index < left.chunks.size(); ++index)
    if (left.chunks[index].sha != right.chunks[index].sha) return false;
  return true;
}

InstallError FromCm(CmError error) {
  switch (error) {
    case CmError::kNone: return InstallError::kNone;
    case CmError::kCanceled: return InstallError::kCanceled;
    case CmError::kNoSession: return InstallError::kNoSession;
    case CmError::kRejected: return InstallError::kRejected;
    case CmError::kNetwork:
    case CmError::kTimeout: return InstallError::kNetwork;
    case CmError::kProtocol: return InstallError::kProtocol;
  }
  return InstallError::kProtocol;
}

struct Placement {
  std::size_t depot = 0;
  const ManifestFile* file = nullptr;
};

std::string JournalKey(std::uint32_t depot, const std::string& path, std::uint64_t offset) {
  return std::to_string(depot) + "\t" + std::to_string(offset) + "\t" + path;
}

}  // namespace

DownloadTuning ReadDownloadTuning(const std::string& path) {
  DownloadTuning tuning;
  FILE* input = std::fopen(path.c_str(), "r");
  if (!input) return tuning;
  ScopeExit close{[&] { std::fclose(input); }};
  char line[128];
  bool seen_slots = false, seen_ahead = false;
  while (std::fgets(line, sizeof(line), input)) {
    std::string text(line);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
    if (text.empty()) continue;
    const auto equals = text.find('=');
    if (equals == std::string::npos) return {};
    int value = 0;
    const auto parsed = std::from_chars(text.data() + equals + 1, text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) return {};
    const auto key = text.substr(0, equals);
    if (key == "slots" && !seen_slots) {
      seen_slots = true;
      tuning.slots = std::clamp(value, 1, 8);
    } else if (key == "ahead" && !seen_ahead && value >= 1 && value <= 65536) {
      seen_ahead = true;
      tuning.ahead_files = value;
    } else return {};
  }
  if (std::ferror(input)) return {};
  return tuning;
}

const char* InstallErrorName(InstallError error) {
  switch (error) {
    case InstallError::kNone: return "none";
    case InstallError::kCanceled: return "canceled";
    case InstallError::kNoSession: return "no_session";
    case InstallError::kRejected: return "rejected";
    case InstallError::kNetwork: return "network";
    case InstallError::kCorrupt: return "corrupt";
    case InstallError::kNoSpace: return "no_space";
    case InstallError::kStorage: return "storage";
    case InstallError::kProtocol: return "protocol";
  }
  return "protocol";
}

DepotInstaller::DepotInstaller(CmClient& cm, CdnClient& cdn) {
  hooks_.access = [&cm](const StoredSession& session, std::uint32_t app_id,
                        const std::vector<DepotRequest>& depots, const std::atomic<bool>& cancel) {
    return cm.FetchDepotAccess(session, app_id, depots, cancel);
  };
  // Which check a manifest that arrived failed, for the install log; empty after a network failure.
  auto manifest_problem = std::make_shared<std::string>();
  hooks_.servers = [&cdn, manifest_problem](std::uint32_t cell, const std::atomic<bool>& cancel,
                                            std::vector<ContentServer>* out) {
    manifest_problem->clear();  // a manifest problem from an earlier run must not hide why this list failed
    return cdn.Servers(cell, cancel, out);
  };
  hooks_.manifest = [&cdn, manifest_problem](const ContentServer& server, const DepotGrant& grant,
                                             const std::atomic<bool>& cancel, DepotManifest* out) {
    manifest_problem->clear();
    std::vector<std::uint8_t> zip;
    const auto fetched = cdn.Manifest(server, grant.depot_id, grant.manifest_gid, grant.manifest_request_code,
                                      cancel, &zip);
    if (fetched == CdnError::kCanceled) return InstallError::kCanceled;
    if (fetched != CdnError::kNone) return InstallError::kNetwork;
    const auto corrupt = [&](std::string what) {
      *manifest_problem = "depot=" + std::to_string(grant.depot_id) + " bytes=" + std::to_string(zip.size()) + " " + what;
      return InstallError::kCorrupt;
    };
    std::vector<std::uint8_t> raw;
    if (!UnzipFirstEntry(zip, &InflateRaw, &raw)) return corrupt("unzip");
    if (!ParseDepotManifest(raw, out)) return corrupt("parse raw=" + std::to_string(raw.size()));
    if (out->filenames_encrypted && !DecryptManifestFilenames(out, grant.key, &SymmetricDecrypt))
      return corrupt("filenames");
    if (out->depot_id != grant.depot_id || out->gid != grant.manifest_gid)
      return corrupt("ids got_depot=" + std::to_string(out->depot_id) + " got_gid=" + std::to_string(out->gid));
    std::string reason;
    if (!ValidateDepotManifest(*out, &reason)) return corrupt("invalid " + reason);
    return InstallError::kNone;
  };
  const auto ca_bundle = cdn.ca_bundle();
  hooks_.make_fetcher = [ca_bundle] {
    auto client = std::make_shared<CdnClient>(ca_bundle);
    InstallerHooks::ChunkFetcher fetcher;
    fetcher.fetch = [client](const ContentServer& server, std::uint32_t depot, const std::string& sha,
                              std::size_t cap, const std::atomic<bool>& cancel, std::vector<std::uint8_t>* out) {
      return client->Chunk(server, depot, sha, cap, cancel, out);
    };
    fetcher.last_failure = [client] { return client->last_failure(); };
    fetcher.stats = [client] { return client->stats(); };
    return fetcher;
  };
  hooks_.cdn_stats = [&cdn] { return cdn.stats(); };
  hooks_.free_bytes = &DefaultFreeBytes;
  hooks_.last_failure = [&cdn, manifest_problem] {
    return manifest_problem->empty() ? cdn.last_failure() : *manifest_problem;
  };
  hooks_.pause = [](int milliseconds, const std::atomic<bool>& cancel) {
    for (int waited = 0; waited < milliseconds && !cancel.load(); waited += 100) usleep(100 * 1000);
  };
}

DepotInstaller::DepotInstaller(InstallerHooks hooks) : hooks_(std::move(hooks)) {
  if (!hooks_.free_bytes) hooks_.free_bytes = &DefaultFreeBytes;
}

InstallOutcome DepotInstaller::Run(const StoredSession& session, const InstallTarget& target,
                                   const std::atomic<bool>& cancel,
                                   const std::function<void(const InstallProgress&)>& on_progress) {
  InstallOutcome outcome;
  InstallStats stats;
  const int slots = std::clamp(tuning_.slots, 1, 8);
  const int ahead_files = std::clamp(tuning_.ahead_files, 1, 65536);
  stats.slots = slots; stats.ahead_files = ahead_files;
  CdnStats worker_cdn;
  bool workers_running = false;
  CdnStats coordinator_cdn;
  const auto initial_cdn = hooks_.cdn_stats ? hooks_.cdn_stats() : CdnStats{};
  Clock::duration fetch{}, process{}, create{}, write{}, journal_time{}, backoff{};
  auto phase_start = Clock::now();
  Clock::time_point download_start{}, last_stats{};
  bool loop_finished = false;
  auto emit = [&](const char* phase) {
    if (!hooks_.stats) return;
    stats.fetch_ms = Milliseconds(fetch); stats.process_ms = Milliseconds(process);
    stats.create_ms = Milliseconds(create); stats.write_ms = Milliseconds(write);
    stats.journal_ms = Milliseconds(journal_time); stats.backoff_ms = Milliseconds(backoff);
    if (download_start != Clock::time_point{} && !loop_finished)
      stats.download_ms = Milliseconds(Clock::now() - download_start);
    {
      const auto current = workers_running ? coordinator_cdn : (hooks_.cdn_stats ? hooks_.cdn_stats() : CdnStats{});
#define CDN_DELTA(field) stats.field = current.field - initial_cdn.field
      CDN_DELTA(requests); CDN_DELTA(failures); CDN_DELTA(resolve_ms); CDN_DELTA(connect_ms);
      CDN_DELTA(tls_ms); CDN_DELTA(first_byte_ms); CDN_DELTA(total_ms); CDN_DELTA(new_connections); CDN_DELTA(bytes);
#undef CDN_DELTA
#define CDN_ADD(field) stats.field += worker_cdn.field
      CDN_ADD(requests); CDN_ADD(failures); CDN_ADD(resolve_ms); CDN_ADD(connect_ms);
      CDN_ADD(tls_ms); CDN_ADD(first_byte_ms); CDN_ADD(total_ms); CDN_ADD(new_connections); CDN_ADD(bytes);
#undef CDN_ADD
    }
    hooks_.stats(phase, stats);
  };
  auto fail = [&outcome](InstallError error, const char* detail) {
    outcome.error = error;
    outcome.detail = detail;
    return outcome;
  };
  if (target.app_id == 0 || target.install_root.empty() || target.install_root[0] != '/' ||
      target.depots.empty() || target.install_root.find("/..") != std::string::npos)
    return fail(InstallError::kProtocol, "invalid install target");
  if (cancel.load()) return fail(InstallError::kCanceled, "canceled");
  auto trace = [this](const char* step) { if (hooks_.trace) hooks_.trace(step); };
  trace("access");

  std::vector<DepotRequest> requests;
  for (const auto& depot : target.depots) {
    if (!depot.manifest_gid || *depot.manifest_gid == 0) return fail(InstallError::kProtocol, "depot without manifest");
    requests.push_back({depot.id, *depot.manifest_gid, depot.depot_from_app.has_value()});
  }
  const auto access = hooks_.access(session, target.app_id, requests, cancel);
  if (!access.ok()) {
    outcome.error = FromCm(access.error);
    outcome.detail = access.detail.empty() ? "depot access failed" : access.detail;
    return outcome;
  }
  if (access.grants.empty() || access.grants.size() + access.skipped.size() != requests.size())
    return fail(InstallError::kProtocol, "incomplete depot access");
  outcome.skipped_depots = access.skipped;

  stats.access_ms = Milliseconds(Clock::now() - phase_start);
  phase_start = Clock::now();
  std::vector<ContentServer> servers;
  const auto listed = hooks_.servers(session.cell_id.value_or(0), cancel, &servers);
  if (listed == CdnError::kCanceled) return fail(InstallError::kCanceled, "canceled");
  if (listed != CdnError::kNone || servers.empty()) {
    outcome.error = InstallError::kNetwork;
    outcome.detail = "no content servers";
    if (hooks_.last_failure) outcome.detail += " " + hooks_.last_failure();
    return outcome;
  }

  stats.servers_ms = Milliseconds(Clock::now() - phase_start);
  phase_start = Clock::now();
  trace("manifests");
  // Manifests, in mounting order.
  std::vector<DepotManifest> manifests(access.grants.size());
  for (std::size_t index = 0; index < access.grants.size(); ++index) {
    InstallError fetched = InstallError::kNetwork;
    bool network_failed = false;
    // A server that fails or sends a manifest that does not parse (a busy CDN can answer 502, or with a
    // cut-off body) is followed by another, after 1, 2, 4 and 8 s, as for chunks.
    const std::size_t attempts = std::min<std::size_t>(std::max<std::size_t>(servers.size() * 2, 4), 6);
    for (std::size_t attempt = 0; attempt < attempts; ++attempt) {
      fetched = hooks_.manifest(servers[attempt % servers.size()], access.grants[index], cancel, &manifests[index]);
      if (fetched != InstallError::kNetwork && fetched != InstallError::kCorrupt) break;
      network_failed = network_failed || fetched == InstallError::kNetwork;
      if (hooks_.pause && attempt + 1 < attempts) hooks_.pause(1000 << std::min<std::size_t>(attempt, 3), cancel);
      if (cancel.load()) return fail(InstallError::kCanceled, "canceled");
    }
    // A manifest that only ever came back broken is corrupt; one that some server could not send at all is a
    // network problem, which Resume or a later try gets past.
    if (fetched == InstallError::kCorrupt && network_failed) fetched = InstallError::kNetwork;
    if (fetched != InstallError::kNone) {
      outcome.error = fetched;
      outcome.detail = "manifest unavailable";
      if (hooks_.last_failure) outcome.detail += " " + hooks_.last_failure();
      return outcome;
    }
  }

  stats.manifests_ms = Milliseconds(Clock::now() - phase_start);
  phase_start = Clock::now();
  trace("merge");
  // Later depots replace the same path from earlier ones; identical copies stay with the first.
  std::map<std::string, Placement> placements;
  for (std::size_t depot = 0; depot < manifests.size(); ++depot) {
    for (const auto& file : manifests[depot].files) {
      const auto key = Lower(file.path);
      const auto found = placements.find(key);
      if (found != placements.end()) {
        if (SameFile(*found->second.file, file)) continue;
        if (found->second.file->is_directory() != file.is_directory()) return fail(InstallError::kCorrupt, "conflicting depots");
      }
      placements[key] = {depot, &file};
    }
  }

  InstallProgress progress;
  for (const auto& [key, placement] : placements) {
    if (placement.file->is_directory() || placement.file->is_symlink()) continue;
    progress.install_total += placement.file->size;
    ++progress.files_total;
    for (const auto& chunk : placement.file->chunks) progress.download_total += chunk.compressed_size;
  }

  trace("journal");
  // Journal of chunks already written in a previous run.
  const std::string journal_dir = target.install_root + "/" + kJournalDir;
  const std::string journal_path = journal_dir + "/journal";
  if (!MakeDirs(journal_dir)) return fail(InstallError::kStorage, "cannot create install folder");
  // Every path this install may create, so removal never has to list directories (the console's
  // readdir is unreliable).
  {
    const std::string list_path = target.install_root + "/" + kFileList;
    FILE* list = std::fopen((list_path + ".tmp").c_str(), "w");
    if (!list) return fail(InstallError::kStorage, "cannot write file list");
    for (const auto& [key, placement] : placements)
      if (!placement.file->is_symlink())
        std::fprintf(list, "%c\t%s\n", placement.file->is_directory() ? 'd' : 'f', placement.file->path.c_str());
    if (std::fclose(list) != 0 || std::rename((list_path + ".tmp").c_str(), list_path.c_str()) != 0)
      return fail(InstallError::kStorage, "cannot write file list");
  }
  std::string journal_header = "#v2 ";
  for (std::size_t index = 0; index < access.grants.size(); ++index) {
    if (index) journal_header += ',';
    journal_header += std::to_string(access.grants[index].depot_id) + ':' +
                      std::to_string(access.grants[index].manifest_gid);
  }
  std::set<std::string> done;
  bool has_header = false, nonempty_journal = false, reset_journal = false, first_line = true;
  bool unterminated_journal = false;
  if (FILE* input = std::fopen(journal_path.c_str(), "r")) {
    const auto read_line = [&](std::string entry) {
      while (!entry.empty() && (entry.back() == '\n' || entry.back() == '\r')) entry.pop_back();
      const bool at_start = first_line;
      first_line = false;
      if (entry.empty()) return;
      nonempty_journal = true;
      if (entry[0] == '#') {
        // Version headers must occur exactly once, on the first line. Other comments are ignored.
        const bool version_prefix = entry.compare(0, 2, "#v") == 0 || entry.compare(0, 2, "#V") == 0;
        const bool version_line = version_prefix && (entry.size() == 2 ||
            (entry.size() > 2 && std::isdigit(static_cast<unsigned char>(entry[2]))));
        if (version_line) {
          if (!at_start || has_header || entry != journal_header) reset_journal = true;
          has_header = true;
        }
        return;
      }
      done.insert(std::move(entry));
    };
    char buffer[8192];
    std::string line;
    bool oversized = false;
    const auto max_line = std::max<std::size_t>(journal_header.size() + 2, sizeof(buffer));
    while (std::fgets(buffer, sizeof(buffer), input)) {
      const std::string part(buffer);
      if (line.size() + part.size() > max_line) oversized = true;
      if (!oversized) line += part;
      if (!part.empty() && part.back() == '\n') {
        if (oversized) { reset_journal = true; nonempty_journal = true; first_line = false; }
        else read_line(std::move(line));
        line.clear(); oversized = false;
      }
    }
    if (oversized) { reset_journal = true; nonempty_journal = true; }
    else if (!line.empty()) { read_line(std::move(line)); unterminated_journal = true; }
    const bool read_ok = std::ferror(input) == 0;
    const bool closed = std::fclose(input) == 0;
    if (!read_ok || !closed) return fail(InstallError::kStorage, "cannot read journal");
  } else if (Exists(journal_path)) return fail(InstallError::kStorage, "cannot read journal");
  if (reset_journal || !nonempty_journal) {
    done.clear();
    const auto temporary = journal_path + ".tmp";
    FILE* header = std::fopen(temporary.c_str(), "w");
    if (!header) return fail(InstallError::kStorage, "cannot write journal");
    const bool written = std::fprintf(header, "%s\n", journal_header.c_str()) >= 0;
    const bool closed = std::fclose(header) == 0;
    if (!written || !closed || std::rename(temporary.c_str(), journal_path.c_str()) != 0) {
      std::remove(temporary.c_str());
      return fail(InstallError::kStorage, "cannot write journal");
    }
    unterminated_journal = false;
    if (reset_journal) trace("journal-reset");
  } else if (!has_header) trace("journal-legacy");

  // Space still needed: per file, the smaller of its unwritten chunks and its unallocated bytes.
  // A resumed install on a preallocating filesystem already holds its files at full size.
  std::uint64_t remaining_bytes = 0;
  std::map<std::string, std::uint64_t> allocated;
  for (const auto& [key, placement] : placements) {
    if (placement.file->is_directory() || placement.file->is_symlink()) continue;
    const auto depot_id = manifests[placement.depot].depot_id;
    const auto path = target.install_root + "/" + placement.file->path;
    const auto held = AllocatedBytes(path, placement.file->size);
    allocated[key] = held;
    progress.held_bytes += held;
    bool pending = placement.file->chunks.empty() && !Exists(path);
    std::uint64_t unwritten = pending ? placement.file->size : 0;
    for (const auto& chunk : placement.file->chunks) {
      if (done.count(JournalKey(depot_id, placement.file->path, chunk.offset))) {
        progress.download_done += chunk.compressed_size;
        ++stats.chunks_skipped;
      } else {
        unwritten += chunk.original_size;
        ++stats.chunks_pending;
        stats.bytes_pending += chunk.compressed_size;
        pending = true;
      }
    }
    const auto needed = std::min(unwritten, placement.file->size - held);
    if (remaining_bytes > std::numeric_limits<std::uint64_t>::max() - needed)
      return fail(InstallError::kNoSpace, "install size exceeds available space");
    remaining_bytes += needed;
    if (pending) ++stats.files_pending;
  }
  trace("space");
  const auto available_bytes = hooks_.free_bytes(target.install_root);
  if (available_bytes < kInstallSpaceMargin || remaining_bytes > available_bytes - kInstallSpaceMargin)
    return fail(InstallError::kNoSpace, "not enough free space");
  if (on_progress) on_progress(progress);

  stats.prepare_ms = Milliseconds(Clock::now() - phase_start);
  stats.files_total = progress.files_total;
  stats.download_total = progress.download_total;
  emit("shape");
  trace("write");
  FILE* journal = std::fopen(journal_path.c_str(), "a");
  if (!journal) return fail(InstallError::kStorage, "cannot write journal");
  // A valid final line need not have a newline; separate it from newly appended chunk keys.
  if (unterminated_journal && (std::fprintf(journal, "\n") < 0 || std::fflush(journal) != 0)) {
    std::fclose(journal);
    return fail(InstallError::kStorage, "cannot write journal");
  }
  download_start = last_stats = Clock::now();
  ScopeExit end_stats{[&] { emit("end"); }};
  auto report_progress = [&] {
    const auto now = Clock::now();
    if (now - last_stats >= std::chrono::seconds(60)) { emit("progress"); last_stats = now; }
  };
  struct FileWork {
    Placement placement;
    bool prepared = false;
    std::size_t pending = 0;
    std::uint64_t allocated = 0;  // counted in progress.held_bytes
  };
  struct ChunkWork { std::size_t file; const ManifestChunk* chunk; std::string journal_key; };
  std::vector<FileWork> files;
  std::vector<ChunkWork> chunks;
  for (const auto& [key, placement] : placements) {
    if (placement.file->is_symlink()) continue;
    const auto index = files.size();
    const auto held = allocated.find(key);
    files.push_back({placement, false, 0, held == allocated.end() ? 0 : held->second});
    for (const auto& chunk : placement.file->chunks) {
      auto entry = JournalKey(access.grants[placement.depot].depot_id, placement.file->path, chunk.offset);
      if (done.count(entry)) continue;
      chunks.push_back({index, &chunk, std::move(entry)});
      ++files.back().pending;
    }
  }
  struct Worker {
    InstallerHooks::ChunkFetcher fetcher;
    pthread_t thread{};
    bool started = false, busy = false, ready = false;
    std::size_t work = 0;
    std::vector<std::uint8_t> plain;
    std::uint64_t compressed_bytes = 0, retries = 0, corrupt_retries = 0;
    Clock::duration fetch{}, process{}, backoff{};
    CdnStats cdn;
    std::function<void()> run;
  };
  std::vector<std::unique_ptr<Worker>> workers;
  std::mutex mutex;
  std::condition_variable changed;
  std::atomic<bool> stop{false};
  std::size_t next_chunk = 0, occupied = 0, waiting = 0;
  std::uint32_t peak_active = 0, peak_waiting = 0;
  InstallError first_error = InstallError::kNone;
  std::string first_detail;
  const auto fail_locked = [&](InstallError error, std::string detail) {
    if (first_error == InstallError::kNone) {
      first_error = error;
      first_detail = std::move(detail);
    }
    stop.store(true);
    changed.notify_all();
  };
  const auto publish_failure = [&](InstallError error, std::string detail) {
    std::lock_guard<std::mutex> lock(mutex);
    fail_locked(error, std::move(detail));
  };
  // Workers publish their own counters under this lock. Never query their clients here.
  const auto snapshot = [&] {
    std::lock_guard<std::mutex> lock(mutex);
    fetch = {}; process = {}; backoff = {}; worker_cdn = {};
    stats.peak_active = peak_active; stats.peak_waiting = peak_waiting;
    stats.chunk_retries = stats.corrupt_retries = 0;
    for (const auto& worker : workers) {
      fetch += worker->fetch; process += worker->process; backoff += worker->backoff;
      stats.chunk_retries += worker->retries; stats.corrupt_retries += worker->corrupt_retries;
#define SUM_CDN(field) worker_cdn.field += worker->cdn.field
      SUM_CDN(requests); SUM_CDN(failures); SUM_CDN(resolve_ms); SUM_CDN(connect_ms); SUM_CDN(tls_ms);
      SUM_CDN(first_byte_ms); SUM_CDN(total_ms); SUM_CDN(new_connections); SUM_CDN(bytes);
#undef SUM_CDN
    }
  };
  const auto join_workers = [&] {
    stop.store(true);
    changed.notify_all();
    for (auto& worker : workers) {
      if (worker->started) { pthread_join(worker->thread, nullptr); worker->started = false; }
    }
    workers_running = false;
    snapshot();
  };
  ScopeExit joined{join_workers};
  ScopeExit close_journal{[&] { if (journal) std::fclose(journal); }};
  coordinator_cdn = hooks_.cdn_stats ? hooks_.cdn_stats() : CdnStats{};
  workers_running = true;
  if (!chunks.empty()) trace("first-chunk");
  pthread_attr_t attr;
  const bool attr_ready = pthread_attr_init(&attr) == 0;
  const bool stack_ready = attr_ready && pthread_attr_setstacksize(&attr, 256 * 1024) == 0;
  if (!stack_ready && !chunks.empty()) publish_failure(InstallError::kNetwork, "cannot start workers");
  for (int index = 0; stack_ready && index < slots && !chunks.empty() && !stop.load(); ++index) {
    auto worker = std::make_unique<Worker>();
#if defined(__cpp_exceptions)
    try {
#endif
      if (hooks_.make_fetcher) worker->fetcher = hooks_.make_fetcher();
      else { worker->fetcher.fetch = hooks_.chunk; worker->fetcher.last_failure = hooks_.last_failure; }
#if defined(__cpp_exceptions)
    } catch (...) { publish_failure(InstallError::kNetwork, "cannot create fetcher"); break; }
#endif
    if (!worker->fetcher.fetch) { publish_failure(InstallError::kNetwork, "cannot create fetcher"); break; }
    auto* self = worker.get();
    self->run = [&, self] {
      // Destroy the HTTP client on the same thread that used it, after publishing its counters.
      ScopeExit release_fetcher{[self] { self->fetcher = {}; }};
      std::size_t server_cursor = 0;
      Clock::duration fetch_time{}, process_time{}, backoff_time{};
      std::uint64_t retries = 0, corrupt_retries = 0;
#if defined(__cpp_exceptions)
      try {
#endif
        while (!stop.load()) {
          {
            std::unique_lock<std::mutex> lock(mutex);
            changed.wait(lock, [&] { return stop.load() || !self->busy; });
            if (stop.load() || next_chunk == chunks.size()) break;
            self->work = next_chunk++;
            self->busy = true;
            ++occupied;
            peak_active = std::max(peak_active, static_cast<std::uint32_t>(occupied));
          }
          const auto& work = chunks[self->work];
          const auto& placement = files[work.file].placement;
          const auto& grant = access.grants[placement.depot];
          const auto& chunk = *work.chunk;
          InstallError result = InstallError::kNetwork;
          std::string detail;
          std::vector<std::uint8_t> plain;
          std::uint64_t compressed_bytes = 0;
          const auto attempts = static_cast<std::size_t>(kChunkAttempts) * std::min<std::size_t>(servers.size(), 2);
          for (std::size_t attempt = 0; attempt < attempts && !stop.load(); ++attempt) {
            if (attempt > 0) ++retries;
            std::vector<std::uint8_t> encrypted;
            const auto fetched = Timed(fetch_time, [&] {
              return self->fetcher.fetch(servers[server_cursor % servers.size()], grant.depot_id, Hex(chunk.sha),
                                         static_cast<std::size_t>(chunk.compressed_size) + 4096, stop, &encrypted);
            });
            if (fetched == CdnError::kCanceled || stop.load()) { result = InstallError::kCanceled; break; }
            if (fetched != CdnError::kNone) {
              ++server_cursor;
              result = InstallError::kNetwork;
              if (hooks_.pause && attempt + 1 < attempts)
                Timed(backoff_time, [&] { hooks_.pause(1000 << std::min<std::size_t>(attempt, 3), stop); });
              continue;
            }
            const auto processed = Timed(process_time, [&] {
              return ProcessDepotChunk(encrypted, grant.key, chunk.original_size, chunk.crc, chunk.sha, &plain);
            });
            if (processed == ChunkError::kNone) {
              compressed_bytes = encrypted.size(); result = InstallError::kNone; break;
            }
            ++server_cursor;
            if (attempt + 1 < attempts) ++corrupt_retries;
            result = InstallError::kCorrupt;
          }
          if (stop.load()) result = InstallError::kCanceled;
          if (result != InstallError::kNone) {
            detail = result == InstallError::kCanceled ? "canceled" : "chunk unavailable";
            if (result == InstallError::kNetwork && self->fetcher.last_failure) {
              const auto failure = self->fetcher.last_failure();
              if (!failure.empty()) detail += " " + failure;
            }
          }
          const auto cdn = self->fetcher.stats ? self->fetcher.stats() : CdnStats{};
          {
            std::lock_guard<std::mutex> lock(mutex);
            self->fetch = fetch_time; self->process = process_time; self->backoff = backoff_time;
            self->retries = retries; self->corrupt_retries = corrupt_retries; self->cdn = cdn;
            if (result != InstallError::kNone) { fail_locked(result, std::move(detail)); break; }
            self->plain = std::move(plain); self->compressed_bytes = compressed_bytes;
            self->ready = true;
            ++waiting;
            peak_waiting = std::max(peak_waiting, static_cast<std::uint32_t>(waiting));
            changed.notify_all();
          }
        }
#if defined(__cpp_exceptions)
      } catch (...) { publish_failure(InstallError::kNetwork, "chunk worker failed"); }
#endif
    };
    workers.push_back(std::move(worker));
    if (pthread_create(&self->thread, &attr, [](void* context) -> void* {
          static_cast<Worker*>(context)->run(); return nullptr;
        }, self) != 0) {
      publish_failure(InstallError::kNetwork, "cannot start workers"); break;
    }
    self->started = true;
  }
  if (attr_ready) pthread_attr_destroy(&attr);
  const auto prepare_file = [&](std::size_t index) {
    auto& work = files[index];
    if (work.prepared) return true;
    const auto& file = *work.placement.file;
    const auto path = target.install_root + "/" + file.path;
    if (file.is_directory()) {
      if (!Timed(create, [&] { return MakeDirs(path, &stats.folders_created); })) return false;
    } else {
      if (!Timed(create, [&] { return MakeDirs(Parent(path), &stats.folders_created); })) return false;
      const bool existed = Exists(path);
      if (file.chunks.empty() || !existed) {
        if (!Timed(create, [&] { return EnsureFileSize(path, file.size); })) return false;
        if (!existed) ++stats.files_created;
        const auto now_held = AllocatedBytes(path, file.size);
        if (now_held > work.allocated) { progress.held_bytes += now_held - work.allocated; work.allocated = now_held; }
      }
    }
    work.prepared = true;
    return true;
  };
  std::size_t prepare_next = 0, front_file = 0, written_chunks = 0;
  while (true) {
    if (cancel.load()) publish_failure(InstallError::kCanceled, "canceled");
    Worker* ready = nullptr;
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (first_error != InstallError::kNone) break;
      for (auto& worker : workers) if (worker->ready) {
        ready = worker.get(); ready->ready = false; --waiting; break;
      }
    }
    snapshot();
    report_progress();
    if (ready) {
      const auto& work = chunks[ready->work];
      const auto& file = *files[work.file].placement.file;
      if (!prepare_file(work.file)) { publish_failure(InstallError::kStorage, "cannot create file"); break; }
      if (stats.chunks_done == 0) trace("first-write");
      const auto path = target.install_root + "/" + file.path;
      if (!Timed(write, [&] { return WriteAt(path, file.size, work.chunk->offset, ready->plain); })) {
        publish_failure(InstallError::kStorage, "cannot write file"); break;
      }
      const bool recorded = Timed(journal_time, [&] {
        return std::fprintf(journal, "%s\n", work.journal_key.c_str()) >= 0 && std::fflush(journal) == 0;
      });
      if (!recorded) { publish_failure(InstallError::kStorage, "cannot write journal"); break; }
      ++stats.chunks_done; ++written_chunks; --files[work.file].pending;
      stats.compressed_bytes += ready->compressed_bytes;
      stats.original_bytes += work.chunk->original_size;
      progress.download_done += work.chunk->compressed_size;
      // Writing into a sparse file allocates it; a preallocated file is already counted.
      auto& written = files[work.file];
      const auto grow = std::min<std::uint64_t>(work.chunk->original_size, file.size - written.allocated);
      written.allocated += grow;
      progress.held_bytes += grow;
      if (on_progress) on_progress(progress);
      // Release the occupied slot only after its output has been written and freed.
      std::vector<std::uint8_t>().swap(ready->plain);
      {
        std::lock_guard<std::mutex> lock(mutex);
        ready->busy = false; --occupied; changed.notify_all();
      }
      continue;
    }
    while (front_file < files.size() && files[front_file].prepared && files[front_file].pending == 0) ++front_file;
    while (prepare_next < files.size() && files[prepare_next].prepared) ++prepare_next;
    if (prepare_next < files.size() && prepare_next - std::min(prepare_next, front_file) < static_cast<std::size_t>(ahead_files)) {
      if (!prepare_file(prepare_next++)) { publish_failure(InstallError::kStorage, "cannot create file"); break; }
      continue;
    }
    if (written_chunks == chunks.size() && prepare_next == files.size()) break;
    std::unique_lock<std::mutex> lock(mutex);
    changed.wait_for(lock, std::chrono::milliseconds(50), [&] { return stop.load() || waiting != 0; });
  }
  join_workers();
  if (first_error != InstallError::kNone) {
    outcome.error = first_error; outcome.detail = first_detail; outcome.progress = progress;
    return outcome;
  }
  std::fclose(journal);
  journal = nullptr;
  stats.download_ms = Milliseconds(Clock::now() - download_start);
  loop_finished = true;

  // Install record, then drop the journal: the record marks a complete, verified install.
  const std::string record_path = target.install_root + "/" + kInstallRecord;
  const std::string temporary = record_path + ".tmp";
  FILE* record = std::fopen(temporary.c_str(), "w");
  if (!record) return fail(InstallError::kStorage, "cannot write install record");
  // Steam records are written as before; another store's game adds "store" and "game".
  std::string identity;
  if (target.game.valid() && target.game.store != Store::kSteam)
    identity = ",\"store\":\"" + std::string(Traits(target.game.store).key) + "\",\"game\":\"" + target.game.value + "\"";
  std::fprintf(record, "{\"version\":1,\"app_id\":%u%s,\"install_bytes\":%llu,\"files\":%u,\"depots\":[",
               target.app_id, identity.c_str(), static_cast<unsigned long long>(progress.install_total), progress.files_total);
  for (std::size_t index = 0; index < access.grants.size(); ++index)
    std::fprintf(record, "%s{\"id\":%u,\"manifest\":\"%llu\"}", index ? "," : "", access.grants[index].depot_id,
                 static_cast<unsigned long long>(access.grants[index].manifest_gid));
  std::fprintf(record, "]}\n");
  const bool closed = std::fclose(record) == 0;
  if (!closed || std::rename(temporary.c_str(), record_path.c_str()) != 0)
    return fail(InstallError::kStorage, "cannot write install record");
  std::remove(journal_path.c_str());
  rmdir(journal_dir.c_str());
  outcome.error = InstallError::kNone;
  outcome.progress = progress;
  return outcome;
}

bool RemoveManagedInstall(const std::string& install_root, std::string* failure) {
  const auto fail = [failure](std::string reason) {
    if (failure) *failure = std::move(reason);
    return false;
  };
  if (install_root.size() < 2 || install_root[0] != '/' || install_root.find("/..") != std::string::npos)
    return fail("The game's folder path is not valid.");
  if (!Exists(install_root)) return true;
  // The copy of an imported game keeps its plan in .playden-copy (stores/local/copy_plan.h).
  const std::string copy_dir = install_root + "/" + kCopyJournalDir;
  if (!Exists(install_root + "/" + DepotInstaller::kInstallRecord) &&
      !Exists(install_root + "/" + DepotInstaller::kJournalDir) && !Exists(copy_dir))
    return fail("The game's folder holds no Playden install record, so it was left as it is.");
  // Remove exactly the paths the install recorded, deepest first, then Playden's own files.
  std::vector<std::string> files;
  std::vector<std::string> directories;
  if (FILE* list = std::fopen((install_root + "/" + DepotInstaller::kFileList).c_str(), "r")) {
    char line[8192];
    while (std::fgets(line, sizeof(line), list)) {
      std::string entry(line);
      while (!entry.empty() && (entry.back() == '\n' || entry.back() == '\r')) entry.pop_back();
      if (entry.size() < 3 || entry[1] != '\t') continue;
      const auto path = entry.substr(2);
      if (path.empty() || path[0] == '/' || path.find("..") != std::string::npos) continue;
      (entry[0] == 'd' ? directories : files).push_back(install_root + "/" + path);
    }
    std::fclose(list);
  }
  for (const auto& file : files) std::remove(file.c_str());
  std::sort(directories.begin(), directories.end(),
            [](const std::string& left, const std::string& right) { return left.size() > right.size(); });
  for (const auto& directory : directories) rmdir(directory.c_str());
  // Parent folders of recorded files that the manifest did not list explicitly.
  std::vector<std::string> parents;
  for (const auto& file : files)
    for (auto parent = Parent(file); parent.size() > install_root.size(); parent = Parent(parent)) parents.push_back(parent);
  std::sort(parents.begin(), parents.end(),
            [](const std::string& left, const std::string& right) { return left.size() > right.size(); });
  for (const auto& parent : parents) rmdir(parent.c_str());
  // Files the list did not cover (Playden's journals, files the game or other tools wrote) go in a walk.
  // The install record goes last: a folder that could not be emptied stays Playden's, so
  // uninstalling it again tries again.
  int error = 0;
  bool emptied = true;
  std::vector<std::string> names;
  if (ListEntries(install_root, names)) {
    for (const auto& name : names)
      if (name != DepotInstaller::kInstallRecord) emptied = RemoveTree(install_root + "/" + name, &error) && emptied;
  } else {
    emptied = false;
    error = errno;
  }
  if (emptied) {
    if (std::remove((install_root + "/" + DepotInstaller::kInstallRecord).c_str()) != 0 && errno != ENOENT) {
      emptied = false;
      error = errno;
    } else if (rmdir(install_root.c_str()) != 0) {
      emptied = false;
      error = errno;
    }
  }
  if (emptied) return true;
  return fail("Some of the game's files could not be deleted (" + std::string(std::strerror(error)) + ").");
}

}  // namespace playden::steam
