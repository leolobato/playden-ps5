// SPDX-License-Identifier: GPL-3.0-or-later
#include "gog/installer.h"

#include "gog/api.h"
#include "gog/chunk_fetch.h"
#include "gog/content.h"
#include "gog/dependencies.h"
#include "gog/depot_reader.h"
#include "gog/hash.h"
#include "gog/inflate.h"
#include "gog/manifest_v1.h"
#include "gog/path_rules.h"
#include "gog/plan_file.h"
#include "gog/product_id.h"
#include "gog/update_diff.h"
#include "platform/secret.h"

#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <unordered_set>
#include <utility>

namespace playden::gog {
namespace {

using Error = GogInstaller::Outcome::Error;
using Hooks = GogInstaller::Hooks;

constexpr std::size_t kMaxActiveFiles = 64;
constexpr std::size_t kMaxQueuedChunks = 256;
constexpr std::size_t kReadBufferBytes = 1024 * 1024;
// A build meta or manifest on the CDN: within the streaming client's cap.
constexpr std::size_t kMaxCompressedMetaBytes = steam::kMaxHttpStreamResponseBytes;

// ---- Small helpers ----------------------------------------------------------------------------------------------

std::string Join(const std::string& root, std::string_view relative) {
  std::string path = root;
  path += '/';
  path += relative;
  return path;
}

// A real folder, never a link: lstat, not stat.
bool IsRealDirectory(const std::string& path) {
  struct stat info {};
  return lstat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

bool MakeDirectory(const std::string& path) {
  if (mkdir(path.c_str(), 0777) != 0 && errno != EEXIST) return false;
  return IsRealDirectory(path);
}

// Every folder of `relative` below root (the last component too when `include_last`), each a real folder.
bool MakeFolders(const std::string& root, std::string_view relative, bool include_last) {
  std::size_t at = 0;
  for (;;) {
    const auto slash = relative.find('/', at);
    if (slash == std::string_view::npos) break;
    if (!MakeDirectory(Join(root, relative.substr(0, slash)))) return false;
    at = slash + 1;
  }
  return !include_last || MakeDirectory(Join(root, relative));
}

// The game folder and the folders above it. Above it a folder may be a link the system made (a mount); the game
// folder itself must be a real one.
bool MakeRoot(const std::string& root) {
  for (std::size_t slash = root.find('/', 1); slash != std::string::npos; slash = root.find('/', slash + 1)) {
    const auto parent = root.substr(0, slash);
    struct stat info {};
    if (stat(parent.c_str(), &info) == 0) {
      if (!S_ISDIR(info.st_mode)) return false;
      continue;
    }
    if (mkdir(parent.c_str(), 0777) != 0 && errno != EEXIST) return false;
  }
  return MakeDirectory(root);
}

bool ValidRoot(const std::string& root) {
  if (root.size() < 2 || root[0] != '/' || root.back() == '/') return false;
  for (const auto part : SplitFields(std::string_view(root).substr(1), '/'))
    if (part.empty() || part == "." || part == "..") return false;
  return true;
}

bool Exists(const std::string& path) {
  struct stat info {};
  return lstat(path.c_str(), &info) == 0;
}

Error WriteFailure() { return errno == ENOSPC ? Error::kNoSpace : Error::kStorage; }

// A file written through a temporary one: synced, then renamed over the name.
class DurableFile {
 public:
  explicit DurableFile(std::string path) : path_(std::move(path)), temporary_(path_ + ".tmp") {
    file_ = std::fopen(temporary_.c_str(), "wb");
    if (!file_) error_ = WriteFailure();
  }
  ~DurableFile() {
    if (file_) std::fclose(file_);
    if (!committed_) std::remove(temporary_.c_str());
  }
  DurableFile(const DurableFile&) = delete;
  DurableFile& operator=(const DurableFile&) = delete;
  void Line(const std::string& line) {
    if (error_ != Error::kNone) return;
    if (std::fwrite(line.data(), 1, line.size(), file_) != line.size() || std::fputc('\n', file_) == EOF)
      error_ = WriteFailure();
  }
  void Bytes(const std::uint8_t* data, std::size_t size) {
    if (error_ != Error::kNone) return;
    if (std::fwrite(data, 1, size, file_) != size) error_ = WriteFailure();
  }
  Error Commit() {
    if (error_ == Error::kNone && (std::fflush(file_) != 0 || fsync(fileno(file_)) != 0)) error_ = WriteFailure();
    if (std::fclose(file_) != 0 && error_ == Error::kNone) error_ = WriteFailure();
    file_ = nullptr;
    if (error_ == Error::kNone && std::rename(temporary_.c_str(), path_.c_str()) != 0) error_ = WriteFailure();
    committed_ = error_ == Error::kNone;
    return error_;
  }

 private:
  std::string path_;
  std::string temporary_;
  FILE* file_ = nullptr;
  Error error_ = Error::kNone;
  bool committed_ = false;
};

// Streams a file through `consume` in heap-sized pieces.
bool ReadFile(const std::string& path, const std::function<bool(const std::uint8_t*, std::size_t)>& consume) {
  const int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) return false;
  std::unique_ptr<std::uint8_t[]> buffer(new std::uint8_t[kReadBufferBytes]);
  bool ok = true;
  for (;;) {
    const auto got = read(fd, buffer.get(), kReadBufferBytes);
    if (got < 0) {
      if (errno == EINTR) continue;
      ok = false;
      break;
    }
    if (got == 0) break;
    if (!consume(buffer.get(), static_cast<std::size_t>(got))) {
      ok = false;
      break;
    }
  }
  close(fd);
  return ok;
}

bool FileMd5(const std::string& path, std::string* md5) {
  Md5 hash;
  if (!ReadFile(path, [&hash](const std::uint8_t* data, std::size_t size) {
        hash.Update(data, size);
        return true;
      }))
    return false;
  *md5 = hash.FinalHex();
  return true;
}

// Runs `work` on a thread with an explicit stack and waits for it.
bool RunOnStack(std::size_t stack, const std::function<void()>& work) {
  pthread_attr_t attr;
  if (pthread_attr_init(&attr) != 0) return false;
  bool ok = pthread_attr_setstacksize(&attr, stack) == 0;
  pthread_t thread{};
  const std::function<void()>* context = &work;
  ok = ok && pthread_create(&thread, &attr,
                            [](void* argument) -> void* {
                              (*static_cast<const std::function<void()>*>(argument))();
                              return nullptr;
                            },
                            const_cast<std::function<void()>*>(context)) == 0;
  pthread_attr_destroy(&attr);
  if (ok) pthread_join(thread, nullptr);
  return ok;
}

class Logger {
 public:
  explicit Logger(const std::function<void(const std::string&)>& log) : log_(log) {}
  void operator()(const char* format, ...) const __attribute__((format(printf, 2, 3))) {
    if (!log_) return;
    char line[256];
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(line, sizeof(line), format, arguments);
    va_end(arguments);
    log_(line);
  }

 private:
  const std::function<void(const std::string&)>& log_;
};

// ---- Streamed CDN files (metas and manifests) ---------------------------------------------------------------------

struct Source {
  std::string name;  // EndpointLogName
  std::string url;
};

enum class FetchKind { kOk, kCorrupt, kNetwork, kCanceled, kStorage, kNoSpace };

Error FromFetch(FetchKind kind) {
  switch (kind) {
    case FetchKind::kOk: return Error::kNone;
    case FetchKind::kCorrupt: return Error::kCorrupt;
    case FetchKind::kNetwork: return Error::kNetwork;
    case FetchKind::kCanceled: return Error::kCanceled;
    case FetchKind::kStorage: return Error::kStorage;
    case FetchKind::kNoSpace: return Error::kNoSpace;
  }
  return Error::kNetwork;
}

// Streams the body of the first source that serves it whole into `temporary` (at most `cap` bytes), kChunkAttempts in
// all, rotating through the sources. With `expected`, the body's MD5 must be that one, else the next try; without
// one, any 200 is taken. *md5 gets the body's MD5. Nothing is left at `temporary` unless the result is kOk.
FetchKind FetchBody(Transport& transport, const std::vector<Source>& sources, const std::string& expected,
                    std::size_t cap, const std::string& temporary, std::string* md5, const ChunkPause& pause,
                    const std::atomic<bool>& cancel, const Logger& log, const char* what) {
  if (sources.empty()) return FetchKind::kNetwork;
  FetchKind last = FetchKind::kNetwork;
  for (int attempt = 0; attempt < kChunkAttempts; ++attempt) {
    if (cancel.load()) return FetchKind::kCanceled;
    if (attempt > 0) {
      pause(kChunkRetryPauseMs * static_cast<std::uint32_t>(attempt), cancel);
      if (cancel.load()) return FetchKind::kCanceled;
    }
    const auto& source = sources[static_cast<std::size_t>(attempt) % sources.size()];
    FILE* file = std::fopen(temporary.c_str(), "wb");
    if (!file) return errno == ENOSPC ? FetchKind::kNoSpace : FetchKind::kStorage;
    Md5 hash;
    int write_errno = 0;
    steam::HttpRequest request;
    request.url = source.url;
    request.timeout_ms = 0;
    request.connect_timeout_ms = kChunkConnectTimeoutMs;
    request.stall_timeout_ms = kChunkStallTimeoutMs;
    request.max_response_bytes = cap;
    request.headers.emplace_back("User-Agent", kUserAgent);
    request.body_sink = [&](const std::uint8_t* data, std::size_t size) {
      hash.Update(data, size);
      if (std::fwrite(data, 1, size, file) != size) {
        write_errno = errno ? errno : EIO;
        return false;
      }
      return true;
    };
    const auto response = transport(request, cancel);
    bool synced = std::fflush(file) == 0 && fsync(fileno(file)) == 0;
    if (!synced && write_errno == 0) write_errno = errno ? errno : EIO;
    if (std::fclose(file) != 0 && write_errno == 0) write_errno = errno ? errno : EIO;
    const long status = response.ok() ? response.status_code : 0;
    if (write_errno != 0) {
      std::remove(temporary.c_str());
      return write_errno == ENOSPC ? FetchKind::kNoSpace : FetchKind::kStorage;
    }
    if (cancel.load() || response.error == steam::HttpError::kCanceled) {
      std::remove(temporary.c_str());
      return FetchKind::kCanceled;
    }
    if (response.ok() && status == 200) {
      *md5 = hash.FinalHex();
      if (expected.empty() || *md5 == expected) return FetchKind::kOk;
      last = FetchKind::kCorrupt;
      log("gog plan %s retry endpoint=%s reason=hash", what, source.name.c_str());
    } else {
      last = FetchKind::kNetwork;
      log("gog plan %s retry endpoint=%s status=%ld reason=%s", what, source.name.c_str(), status,
          response.ok() ? "status" : steam::HttpErrorName(response.error));
    }
    std::remove(temporary.c_str());
  }
  return last;
}

// Saves the zlib file GOG names by its MD5 at `dest`, from the first source that serves it whole. A file already
// there with that MD5 is kept.
FetchKind FetchToFile(Transport& transport, const std::vector<Source>& sources, const std::string& md5,
                      const std::string& dest, const ChunkPause& pause, const std::atomic<bool>& cancel,
                      const Logger& log, const char* what) {
  std::string existing;
  if (Exists(dest) && FileMd5(dest, &existing) && existing == md5) return FetchKind::kOk;
  const auto temporary = dest + ".tmp";
  std::string got;
  const auto kind = FetchBody(transport, sources, md5, kMaxCompressedMetaBytes, temporary, &got, pause, cancel, log, what);
  if (kind != FetchKind::kOk) return kind;
  if (std::rename(temporary.c_str(), dest.c_str()) != 0) {
    std::remove(temporary.c_str());
    return FetchKind::kStorage;
  }
  return FetchKind::kOk;
}

// Saves a file GOG gives no hash for (gen 1): streamed to `<folder>/<temporary name>.tmp`, then renamed to
// `<folder>/<final name>`, where the final name is `name` or, when `name` is empty, the body's MD5 (*md5).
FetchKind FetchUnhashedToFile(Transport& transport, const std::vector<Source>& sources, std::size_t cap,
                              const std::string& folder, const std::string& temporary_name, const std::string& name,
                              std::string* md5, const ChunkPause& pause, const std::atomic<bool>& cancel,
                              const Logger& log, const char* what) {
  const auto temporary = folder + "/" + temporary_name + ".tmp";
  const auto kind = FetchBody(transport, sources, std::string(), cap, temporary, md5, pause, cancel, log, what);
  if (kind != FetchKind::kOk) return kind;
  const auto dest = folder + "/" + (name.empty() ? *md5 : name);
  if (std::rename(temporary.c_str(), dest.c_str()) != 0) {
    std::remove(temporary.c_str());
    return FetchKind::kStorage;
  }
  return FetchKind::kOk;
}

enum class InflateKind { kOk, kStorage, kCorrupt, kTooLarge, kRefused };

// Inflates a saved zlib file through `out`.
InflateKind InflateFile(const std::string& path, std::uint64_t cap, const ZlibStream::Output& out) {
  ZlibStream stream(cap);
  ZlibStream::Status status = ZlibStream::Status::kOk;
  const bool read = ReadFile(path, [&](const std::uint8_t* data, std::size_t size) {
    status = stream.Feed(data, size, out);
    return status == ZlibStream::Status::kOk;
  });
  if (status == ZlibStream::Status::kOk) status = stream.Finish();
  switch (status) {
    case ZlibStream::Status::kOk: return read ? InflateKind::kOk : InflateKind::kStorage;
    case ZlibStream::Status::kTooLarge: return InflateKind::kTooLarge;
    case ZlibStream::Status::kRefused: return InflateKind::kRefused;
    default: return read ? InflateKind::kCorrupt : InflateKind::kStorage;
  }
}

// The CDN base of a build meta URL: everything before /content-system/v2/meta/.
std::optional<std::string> CdnBase(const std::string& meta_url) {
  static constexpr std::string_view kMarker = "/content-system/v2/meta/";
  const auto at = meta_url.find(kMarker);
  if (at == std::string::npos || at < 9) return std::nullopt;
  return meta_url.substr(0, at);
}

// The CDN base of a gen 1 repository URL: everything before /content-system/v1/manifests/.
std::optional<std::string> CdnBaseV1(const std::string& repository_url) {
  static constexpr std::string_view kMarker = "/content-system/v1/manifests/";
  const auto at = repository_url.find(kMarker);
  if (at == std::string::npos || at < 9) return std::nullopt;
  return repository_url.substr(0, at);
}

std::string LastSegment(const std::string& url) {
  const auto end = url.find_first_of("?#");
  const auto path = url.substr(0, end);
  const auto slash = path.rfind('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

// ---- Planning -----------------------------------------------------------------------------------------------------

Error FromToken(AccessToken::Status status) {
  switch (status) {
    case AccessToken::Status::kOk: return Error::kNone;
    case AccessToken::Status::kNetwork: return Error::kNetwork;
    case AccessToken::Status::kRevoked:
    case AccessToken::Status::kSignedOut: return Error::kSignedOut;
    case AccessToken::Status::kCanceled: return Error::kCanceled;
  }
  return Error::kSignedOut;
}

class Planner {
 public:
  Planner(const Hooks& hooks, const GogInstaller::Target& target, const PlanLayout& layout, const ChunkPause& pause,
          const std::atomic<bool>& cancel, const Logger& log)
      : hooks_(hooks), target_(target), layout_(layout), pause_(pause), cancel_(cancel), log_(log) {}

  Error Plan() {
    transport_ = hooks_.make_transport ? hooks_.make_transport() : Transport();
    if (!transport_) return Error::kNetwork;
    if (!MakeLayoutFolders()) return Error::kStorage;
    if (!target_.seed_list.empty()) SeedFromFileList();

    BuildChoice choice;
    if (const auto error = Builds(&choice); error != Error::kNone) return error;
    PlanHeader header;
    header.build_id = choice.build_id;
    const auto& items_path = layout_.items;
    Error error = choice.kind == BuildChoice::Kind::kGen1 ? PlanV1(choice, &header) : PlanV2(choice, &header);
    if (items_) {
      if (std::fclose(items_) != 0 && error == Error::kNone) error = WriteFailure();
      items_ = nullptr;
    }
    if (error == Error::kNone) error = items_error_;
    if (error == Error::kNone) {
      log_("gog plan entries=%zu links_skipped=%zu clashes=%zu refused=%zu other=%zu", next_, links_, clashes_,
           refused_, other_);
      if (header.language.empty()) header.language = "en-US";
      error = WritePlan(&header);
    }
    std::remove(items_path.c_str());
    return error;
  }

 private:
  // The layout's folders below the game folder, each a real folder: the meta and manifest folders, and the folder
  // of every file the planner writes. A layout path outside the game folder is refused.
  bool MakeLayoutFolders() const {
    const auto make = [this](const std::string& path, bool include_last) {
      const auto& root = target_.root;
      if (path.size() <= root.size() + 1 || path.compare(0, root.size(), root) != 0 || path[root.size()] != '/')
        return false;
      const std::string_view relative = std::string_view(path).substr(root.size() + 1);
      return ValidPlanPath(relative) && MakeFolders(root, relative, include_last);
    };
    return make(layout_.meta_dir, true) && make(layout_.manifest_dir, true) && make(layout_.items, false) &&
           make(layout_.plan, false) && make(layout_.redist_list, false) && make(layout_.file_list, false);
  }

  // The installed spellings, before the first depot: every path of the installed file list and the folders above
  // each, so a build that changes only a name's case keeps the name the drive has. A list that is missing or doesn't
  // read seeds nothing; a torn last line is ignored.
  void SeedFromFileList() {
    LineReader reader(target_.seed_list);
    std::string line;
    bool complete = false;
    std::size_t seeds = 0;
    while (reader.Next(&line, &complete) && complete) {
      if (line.size() < 3 || line[1] != '\t' || (line[0] != 'd' && line[0] != 'f')) continue;
      const std::string_view path = std::string_view(line).substr(2);
      if (!ValidPlanPath(path)) continue;
      paths_.Seed(path, line[0] == 'd' ? PathKind::kDirectory : PathKind::kFile);
      ++seeds;
    }
    log_("gog plan seeded paths=%zu", seeds);
  }

  Error OpenItems() {
    items_ = std::fopen(layout_.items.c_str(), "wb");
    return items_ ? Error::kNone : WriteFailure();
  }

  // A gen 2 build: its meta, then each depot's manifest and the game-folder dependencies.
  Error PlanV2(const BuildChoice& choice, PlanHeader* header) {
    Meta meta;
    if (const auto error = LoadMeta(choice, &meta); error != Error::kNone) return error;
    std::set<std::uint64_t> owned;
    if (const auto error = OwnedIncluded(&owned); error != Error::kNone) return error;
    const auto selection = SelectDepots(meta, owned, target_.language);
    for (const auto& endpoint : choice.meta) {
      const auto url = FillUrl(endpoint, "");
      if (!url) continue;
      if (auto base = CdnBase(*url)) cdns_.push_back({EndpointLogName(endpoint.name), std::move(*base)});
    }
    if (cdns_.empty()) return Error::kProtocol;
    log_("gog plan build generation=2 depots=%zu products=%zu size=%llu", selection.depots.size(),
         selection.products.size(), static_cast<unsigned long long>(selection.size));
    header->generation = 2;
    header->meta_id = choice.meta_id;
    header->language = selection.language;
    header->products = selection.products;

    if (const auto error = OpenItems(); error != Error::kNone) return error;
    for (const auto& depot : selection.depots) {
      const auto error =
          Depot("/content-system/v2/meta/", depot.manifest, depot.product, std::to_string(depot.product));
      if (error != Error::kNone) return error;
    }
    if (!meta.dependencies.empty()) return Dependencies(meta.dependencies);
    return Error::kNone;
  }

  // A gen 1 build: repository.json (saved by its MD5, the plan's meta), then each depot's plain manifest, its files
  // as ranges of their product's main.bin, then the game-folder dependencies from the gen 2 dependency store.
  Error PlanV1(const BuildChoice& choice, PlanHeader* header) {
    std::vector<Source> sources;
    for (const auto& endpoint : choice.repository) {
      auto url = FillUrl(endpoint, "");
      if (!url) continue;
      if (auto base = CdnBaseV1(*url)) cdns_.push_back({EndpointLogName(endpoint.name), std::move(*base)});
      sources.push_back({EndpointLogName(endpoint.name), std::move(*url)});
    }
    if (cdns_.empty()) return Error::kProtocol;
    const auto& meta_dir = layout_.meta_dir;
    std::string md5;
    const auto fetched = FetchUnhashedToFile(transport_, sources, kMaxRepositoryV1Bytes, meta_dir, "repository.json",
                                             std::string(), &md5, pause_, cancel_, log_, "repository");
    if (fetched != FetchKind::kOk) return FromFetch(fetched);
    std::string body;
    if (!ReadFile(meta_dir + "/" + md5, [&body](const std::uint8_t* data, std::size_t size) {
          if (body.size() + size > kMaxRepositoryV1Bytes) return false;
          body.append(reinterpret_cast<const char*>(data), size);
          return true;
        }))
      return Error::kStorage;
    const auto repository = ParseRepositoryV1(body);
    if (!repository || repository->root_product != target_.product || repository->timestamp != choice.timestamp) {
      log_("gog plan repository result=%s", repository ? "other_build" : "unreadable");
      return Error::kProtocol;
    }
    std::set<std::uint64_t> owned;
    if (const auto error = OwnedIncluded(&owned); error != Error::kNone) return error;
    // A depot an excluded DLC shares with the base game is the base game's (SelectDepotsV1 takes the first product of
    // the depot's gameIDs that the install takes).
    const auto selection = SelectDepotsV1(*repository, owned, target_.language);
    // Base first, then each DLC in product order, as gen 2 does: a later depot's file wins (PathSet), so a DLC's file
    // replaces the base game's. The repository may list a DLC depot before a base one.
    std::vector<const RepositoryDepotV1*> depots;
    for (const auto product : selection.products)
      for (const auto& depot : selection.depots)
        if (depot.selected_product == product) depots.push_back(&depot);
    log_("gog plan build generation=1 depots=%zu products=%zu size=%llu", depots.size(), selection.products.size(),
         static_cast<unsigned long long>(selection.size));
    log_("gog plan support_commands=%zu skipped", repository->support_commands);
    header->generation = 1;
    header->meta_id = md5;
    header->timestamp = choice.timestamp;
    header->language = selection.language;
    header->products = selection.products;

    if (const auto error = OpenItems(); error != Error::kNone) return error;
    for (const auto* depot : depots) {
      const auto error = DepotV1(*depot, choice.timestamp);
      if (error != Error::kNone) return error;
    }
    if (!selection.dependencies.empty()) return Dependencies(selection.dependencies);
    return Error::kNone;
  }

  // One gen 1 depot manifest: saved as v1-<name>, then read item by item. Its URL names the depot's first gameIDs
  // entry; its files belong to the product it was selected for (an unowned DLC listed first is not that one).
  Error DepotV1(const RepositoryDepotV1& depot, std::uint64_t timestamp) {
    const auto product = depot.selected_product;
    const auto path = "/content-system/v1/manifests/" + std::to_string(depot.products.front()) + "/windows/" +
                      std::to_string(timestamp) + "/" + depot.manifest;
    std::vector<Source> sources;
    for (const auto& cdn : cdns_) sources.push_back({cdn.name, cdn.url + path});
    const auto& folder = layout_.manifest_dir;
    const auto name = "v1-" + depot.manifest;
    std::string md5;
    // Within the streaming client's cap; the reader checks kMaxDepotManifestBytes too.
    constexpr std::size_t kCap = static_cast<std::size_t>(
        std::min<std::uint64_t>(kMaxDepotManifestBytes, steam::kMaxHttpStreamResponseBytes));
    const auto fetched =
        FetchUnhashedToFile(transport_, sources, kCap, folder, name, name, &md5, pause_, cancel_, log_, "manifest");
    if (fetched != FetchKind::kOk) return FromFetch(fetched);
    auto reader = DepotItemReaderV1([&](const DepotItem& item) {
      return Item(item, product, item.blob_product ? std::to_string(item.blob_product) : std::to_string(product), 1);
    });
    DepotItemReader::Status status = DepotItemReader::Status::kOk;
    const bool read = ReadFile(folder + "/" + name, [&](const std::uint8_t* data, std::size_t size) {
      status = reader.Feed(data, size);
      return status == DepotItemReader::Status::kOk;
    });
    if (items_error_ != Error::kNone) return items_error_;
    if (!read && status == DepotItemReader::Status::kOk) return Error::kStorage;
    if (status == DepotItemReader::Status::kOk) status = reader.Finish();
    if (status != DepotItemReader::Status::kOk) {
      log_("gog plan manifest result=%s", DepotReaderStatusName(status));
      return Error::kProtocol;
    }
    return Error::kNone;
  }

  Error Builds(BuildChoice* choice) {
    const auto answer = FetchBuilds(transport_, target_.product, cancel_);
    if (!answer.answered) return answer.canceled || cancel_.load() ? Error::kCanceled : Error::kNetwork;
    log_("gog plan builds status=%ld", answer.status);
    if (answer.status == 404) return Error::kNoBuild;
    if (answer.status == 429 || answer.status >= 500) return Error::kNetwork;
    if (answer.status != 200) return Error::kProtocol;
    *choice = ChooseBuild(answer.body);
    switch (choice->kind) {
      case BuildChoice::Kind::kGen2:
      case BuildChoice::Kind::kGen1: return Error::kNone;
      case BuildChoice::Kind::kNone: log_("gog plan build none"); return Error::kNoBuild;
      case BuildChoice::Kind::kProtocol: return Error::kProtocol;
    }
    return Error::kProtocol;
  }

  Error LoadMeta(const BuildChoice& choice, Meta* meta) {
    std::vector<Source> sources;
    for (const auto& endpoint : choice.meta)
      if (auto url = FillUrl(endpoint, "")) sources.push_back({EndpointLogName(endpoint.name), std::move(*url)});
    const auto path = layout_.meta_dir + "/" + choice.meta_id;
    const auto fetched = FetchToFile(transport_, sources, choice.meta_id, path, pause_, cancel_, log_, "meta");
    if (fetched != FetchKind::kOk) return FromFetch(fetched);
    std::string inflated;
    const auto kind = InflateFile(path, kMaxMetaBytes, [&inflated](const std::uint8_t* data, std::size_t size) {
      inflated.append(reinterpret_cast<const char*>(data), size);
      return true;
    });
    if (kind == InflateKind::kStorage) return Error::kStorage;
    if (kind != InflateKind::kOk) return Error::kProtocol;
    auto parsed = ParseMeta(inflated);
    if (!parsed || parsed->base_product != target_.product) return Error::kProtocol;
    *meta = std::move(*parsed);
    return Error::kNone;
  }

  // The owned products the install takes: the owned list (mandatory: without it nothing is planned), the base game
  // in it, then the DLC the player turned off taken out. The base game is never taken out.
  Error OwnedIncluded(std::set<std::uint64_t>* owned) {
    if (const auto error = Owned(owned); error != Error::kNone) return error;
    if (!owned->count(target_.product)) {
      log_("gog plan owned result=not_owned");
      return Error::kNotOwned;
    }
    std::size_t excluded = 0;
    for (const auto id : target_.excluded)
      if (id != target_.product) excluded += owned->erase(id);
    // A count only: never the products.
    if (!target_.excluded.empty()) log_("gog plan dlc excluded=%zu", excluded);
    return Error::kNone;
  }

  Error Owned(std::set<std::uint64_t>* owned) {
    for (int round = 0; round < 2; ++round) {
      if (!hooks_.token) return Error::kSignedOut;
      auto token = hooks_.token(cancel_);
      if (token.status != AccessToken::Status::kOk) return FromToken(token.status);
      const auto result = FetchOwned(transport_, token.token, cancel_);
      log_("gog plan owned status=%ld", result.status);
      switch (result.kind) {
        case OwnedResult::Kind::kOk:
          owned->insert(result.ids.begin(), result.ids.end());
          return Error::kNone;
        case OwnedResult::Kind::kRejected:
          // Once: the session drops the token and the next one is a fresh one. A second refusal is a sign-out.
          if (round == 0 && hooks_.invalidate) hooks_.invalidate(token.generation, token.token);
          continue;
        case OwnedResult::Kind::kNetwork: return cancel_.load() ? Error::kCanceled : Error::kNetwork;
        case OwnedResult::Kind::kProtocol: return Error::kProtocol;
      }
    }
    return Error::kSignedOut;
  }

  std::vector<Source> Sources(const std::string& folder, const std::string& hash) const {
    std::vector<Source> sources;
    const auto galaxy = GalaxyPath(hash);
    if (!galaxy) return sources;
    for (const auto& cdn : cdns_) sources.push_back({cdn.name, cdn.url + folder + *galaxy});
    return sources;
  }

  // One depot manifest: saved, then read item by item into the path set and items.tmp. A `redist` depot's paths go
  // under .playden-gog/redist/ (RedistPlace).
  Error Depot(const std::string& folder, const std::string& hash, std::uint64_t product, const std::string& source,
              bool redist = false) {
    const auto path = layout_.manifest_dir + "/" + hash;
    const auto fetched = FetchToFile(transport_, Sources(folder, hash), hash, path, pause_, cancel_, log_, "manifest");
    if (fetched != FetchKind::kOk) return FromFetch(fetched);
    DepotItemReader reader([&](const DepotItem& item) { return Item(item, product, source, 2, redist); });
    DepotItemReader::Status status = DepotItemReader::Status::kOk;
    const auto kind = InflateFile(path, kMaxDepotManifestBytes, [&](const std::uint8_t* data, std::size_t size) {
      status = reader.Feed(data, size);
      return status == DepotItemReader::Status::kOk;
    });
    if (items_error_ != Error::kNone) return items_error_;
    if (kind == InflateKind::kStorage) return Error::kStorage;
    if (kind != InflateKind::kOk && kind != InflateKind::kRefused) return Error::kProtocol;
    if (status == DepotItemReader::Status::kOk) status = reader.Finish();
    if (status != DepotItemReader::Status::kOk) {
      log_("gog plan manifest result=%s", DepotReaderStatusName(status));
      return Error::kProtocol;
    }
    return Error::kNone;
  }

  // One manifest item into the path set and items.tmp. `generation` is the manifest's: a gen 1 file becomes ranges
  // of its source's main.bin, at most kGen1RangeBytes each, the last one shorter; an empty file has none. A `redist`
  // item is placed by RedistPlace; the one at program_key_ is the package's program.
  bool Item(const DepotItem& item, std::uint64_t product, const std::string& source, int generation,
            bool redist = false) {
    PathKind kind = PathKind::kFile;
    switch (item.type) {
      case DepotItem::Type::kFile: kind = PathKind::kFile; break;
      case DepotItem::Type::kDirectory: kind = PathKind::kDirectory; break;
      case DepotItem::Type::kLink: kind = PathKind::kLink; break;
      case DepotItem::Type::kOther: ++other_; return true;
    }
    PathSet::Added added;
    if (!redist) {
      added = paths_.Add(item.path, kind, product, item.support, next_, generation);
    } else if (kind == PathKind::kLink) {
      added.refusal = PathRefusal::kLink;
    } else if (const auto placed = RedistPlace(item.path)) {
      added = paths_.AddRedist(*placed, kind, next_);
    } else {
      added.refusal = PathRefusal::kReserved;
    }
    if (!added.ok()) {
      if (added.refusal == PathRefusal::kLink) ++links_;
      else if (added.refusal == PathRefusal::kClash) ++clashes_;
      else ++refused_;
      return true;
    }
    if (added.replaced && *added.replaced < dropped_.size()) dropped_[*added.replaced] = true;
    if (redist && kind == PathKind::kFile && !program_key_.empty() && FoldCase(added.path) == program_key_)
      program_path_ = added.path;
    dropped_.push_back(false);
    ++next_;
    PlanEntry entry;
    entry.directory = kind == PathKind::kDirectory;
    entry.path = added.path;
    entry.source = source;
    entry.size = item.size;
    entry.md5 = item.md5;
    entry.sha256 = item.sha256;
    if (generation == 1 && !entry.directory) {
      for (std::uint64_t at = 0; at < item.size; at += kGen1RangeBytes)
        entry.units.ranges.push_back({item.offset + at, std::min(kGen1RangeBytes, item.size - at)});
    } else {
      entry.units.chunks = item.chunks;
    }
    const std::string text = EntryText(entry);
    if (std::fwrite(text.data(), 1, text.size(), items_) != text.size()) {
      items_error_ = WriteFailure();
      return false;
    }
    return true;
  }

  // The build's dependencies by ID, from the public dependency store (gen 2 manifests and chunks, for a gen 1 build
  // too): game-folder ones into the game folder, __redist installers under .playden-gog/redist/, each with its line
  // in the redist list. IDs the store doesn't list are counted and listed as missing.
  Error Dependencies(const std::vector<std::string>& named_ids) {
    std::vector<std::string> wanted_ids;
    for (const auto& id : named_ids)
      if (std::find(wanted_ids.begin(), wanted_ids.end(), id) == wanted_ids.end()) wanted_ids.push_back(id);
    const auto answer = FetchDependencyRepository(transport_, cancel_);
    if (!answer.answered) return answer.canceled || cancel_.load() ? Error::kCanceled : Error::kNetwork;
    log_("gog plan dependencies status=%ld", answer.status);
    if (answer.status == 429 || answer.status >= 500) return Error::kNetwork;
    if (answer.status != 200) return Error::kProtocol;
    const auto repository = ParseDependencyRepository(answer.body);
    if (!repository) return Error::kProtocol;
    const auto id = LastSegment(repository->manifest_url);
    if (!IsMd5Hex(id)) return Error::kProtocol;
    std::vector<Source> sources{{"other", repository->manifest_url}};
    for (const auto& cdn : cdns_)
      if (const auto galaxy = GalaxyPath(id)) {
        auto url = cdn.url + "/content-system/v2/dependencies/meta/" + *galaxy;
        if (url != repository->manifest_url) sources.push_back({cdn.name, std::move(url)});
        else sources.front().name = cdn.name;
      }
    const auto path = layout_.manifest_dir + "/" + id;
    const auto fetched = FetchToFile(transport_, sources, id, path, pause_, cancel_, log_, "dependencies");
    if (fetched != FetchKind::kOk) return FromFetch(fetched);
    std::string inflated;
    const auto kind =
        InflateFile(path, kMaxDependencyManifestBytes, [&inflated](const std::uint8_t* data, std::size_t size) {
          inflated.append(reinterpret_cast<const char*>(data), size);
          return true;
        });
    if (kind == InflateKind::kStorage) return Error::kStorage;
    if (kind != InflateKind::kOk) return Error::kProtocol;
    const auto dependencies = ParseDependencyManifest(inflated);
    if (!dependencies) return Error::kProtocol;
    std::size_t game_folder = 0, redist = 0, missing = 0;
    for (const auto& wanted : wanted_ids) {
      const auto found = std::find_if(dependencies->begin(), dependencies->end(),
                                      [&wanted](const Dependency& dependency) { return dependency.id == wanted; });
      if (found == dependencies->end()) {
        ++missing;
        if (ValidDependencyId(wanted)) redist_.push_back(Skipped(wanted, RedistSkip::kMissing));
        continue;
      }
      if (!RedistDependency(*found)) {
        ++game_folder;
        const auto error = Depot("/content-system/v2/dependencies/meta/", found->manifest, target_.product,
                                 kDependencySourcePrefix + found->id);
        if (error != Error::kNone) return error;
        continue;
      }
      // Every __redist installer is downloaded, those Wine provides too: Play decides which to run.
      ++redist;
      const auto program = RedistPlace(found->exe_path);
      program_key_ = program ? FoldCase(*program) : std::string();
      program_path_.clear();
      const auto error = Depot("/content-system/v2/dependencies/meta/", found->manifest, target_.product,
                               kDependencySourcePrefix + found->id, true);
      program_key_.clear();
      if (error != Error::kNone) return error;
      if (!program || !found->usable_arguments) {
        redist_.push_back(Skipped(found->id, RedistSkip::kRefused));
      } else if (program_path_.empty()) {
        redist_.push_back(Skipped(found->id, RedistSkip::kNoProgram));
      } else {
        RedistEntry entry;
        entry.id = found->id;
        entry.package = true;
        entry.program = program_path_;
        entry.arguments = found->arguments;
        redist_.push_back(std::move(entry));
      }
    }
    log_("gog plan dependencies game_folder=%zu redist=%zu missing=%zu", game_folder, redist, missing);
    return Error::kNone;
  }

  static RedistEntry Skipped(const std::string& id, RedistSkip skip) {
    RedistEntry entry;
    entry.id = id;
    entry.skip = skip;
    return entry;
  }

  // items.tmp, without the entries a later depot replaced, becomes the plan and .playden-files.
  Error WritePlan(PlanHeader* header) {
    const auto& items_path = layout_.items;
    std::size_t files = 0;
    {
      EntryReader reader(items_path);
      if (!reader.open()) return Error::kStorage;
      PlanEntry entry;
      for (std::size_t index = 0;; ++index) {
        const auto next = reader.Read(&entry);
        if (next == EntryReader::Next::kEnd) break;
        if (next == EntryReader::Next::kBad || index >= dropped_.size()) return Error::kStorage;
        if (dropped_[index] || entry.directory) continue;
        ++files;
        header->total_size += entry.size;
        header->total_compressed += entry.download();
      }
    }
    // The old journal's indexes belong to another plan.
    if (!layout_.journal.empty() && std::remove(layout_.journal.c_str()) != 0 && errno != ENOENT)
      return Error::kStorage;
    // The redist list before the file list and the plan: a saved plan always has its own (header only without
    // __redist dependencies), and planning again replaces the old one.
    if (const int failure = WriteRedistListFile(layout_.redist_list, redist_); failure != 0) {
      log_("gog plan redist list result=%s", failure == EINVAL ? "refused" : "failed");
      return failure == EINVAL ? Error::kProtocol : failure == ENOSPC ? Error::kNoSpace : Error::kStorage;
    }
    DurableFile plan(layout_.plan);
    DurableFile list(layout_.file_list);
    plan.Line(HeaderLine(*header));
    EntryReader reader(items_path);
    if (!reader.open()) return Error::kStorage;
    PlanEntry entry;
    for (std::size_t index = 0;; ++index) {
      const auto next = reader.Read(&entry);
      if (next == EntryReader::Next::kEnd) break;
      if (next == EntryReader::Next::kBad) return Error::kStorage;
      if (dropped_[index]) continue;
      plan.Line(EntryLine(entry));
      for (const auto& chunk : entry.units.chunks) plan.Line(ChunkLine(chunk));
      for (const auto& range : entry.units.ranges) plan.Line(RangeLine(range));
      list.Line((entry.directory ? "d\t" : "f\t") + entry.path);
    }
    // The file list first: once the plan exists, everything it may place is listed.
    if (const auto error = list.Commit(); error != Error::kNone) return error;
    if (const auto error = plan.Commit(); error != Error::kNone) return error;
    log_("gog plan saved files=%zu size=%llu download=%llu", files,
         static_cast<unsigned long long>(header->total_size),
         static_cast<unsigned long long>(header->total_compressed));
    return Error::kNone;
  }

  const Hooks& hooks_;
  const GogInstaller::Target& target_;
  const PlanLayout& layout_;
  const ChunkPause& pause_;
  const std::atomic<bool>& cancel_;
  const Logger& log_;
  Transport transport_;
  std::vector<Source> cdns_;  // name and base, in the meta's endpoint order
  PathSet paths_;
  std::vector<bool> dropped_;
  std::size_t next_ = 0;
  FILE* items_ = nullptr;
  Error items_error_ = Error::kNone;
  std::size_t links_ = 0, clashes_ = 0, refused_ = 0, other_ = 0;
  std::vector<RedistEntry> redist_;  // the redist list, in the build's dependency order
  std::string program_key_;          // the folded placed program of the redist depot being read
  std::string program_path_;         // its planned path, once an item matched
};

// ---- The journal --------------------------------------------------------------------------------------------------

std::uint64_t ChunkKey(std::uint64_t file, std::uint64_t chunk) { return (file << 24) | chunk; }

struct JournalState {
  std::unordered_set<std::uint64_t> files;   // finished file indexes
  std::unordered_set<std::uint64_t> chunks;  // ChunkKey of written and checked chunks
};

JournalState LoadJournal(const std::string& path) {
  JournalState state;
  LineReader reader(path);
  std::string line;
  bool complete = false;
  while (reader.Next(&line, &complete)) {
    if (!complete) break;  // a torn last line
    const auto fields = SplitFields(line, '\t');
    std::uint64_t file = 0, chunk = 0;
    if (fields.size() == 2 && fields[0] == "f" && ParsePlanNumber(fields[1], &file) && file < (1ull << 39))
      state.files.insert(file);
    else if (fields.size() == 3 && fields[0] == "c" && ParsePlanNumber(fields[1], &file) && file < (1ull << 39) &&
             ParsePlanNumber(fields[2], &chunk) && chunk < kMaxChunksPerFile)
      state.chunks.insert(ChunkKey(file, chunk));
  }
  return state;
}

class Journal {
 public:
  Error Open(const std::string& path) {
    file_ = std::fopen(path.c_str(), "ab");
    return file_ ? Error::kNone : WriteFailure();
  }
  ~Journal() {
    if (file_) std::fclose(file_);
  }
  Error Chunk(std::size_t file, std::size_t chunk) {
    char line[64];
    std::snprintf(line, sizeof(line), "c\t%zu\t%zu\n", file, chunk);
    return Write(line);
  }
  Error File(std::size_t file) {
    char line[48];
    std::snprintf(line, sizeof(line), "f\t%zu\n", file);
    return Write(line);
  }
  Error Sync() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!file_ || !dirty_) return Error::kNone;
    dirty_ = false;
    if (std::fflush(file_) != 0 || fsync(fileno(file_)) != 0) return WriteFailure();
    return Error::kNone;
  }
  Error Close() {
    const auto error = Sync();
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_ && std::fclose(file_) != 0 && error == Error::kNone) {
      file_ = nullptr;
      return WriteFailure();
    }
    file_ = nullptr;
    return error;
  }

 private:
  Error Write(const char* line) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!file_) return Error::kStorage;
    if (std::fputs(line, file_) == EOF) return WriteFailure();
    dirty_ = true;
    return Error::kNone;
  }
  std::mutex mutex_;
  FILE* file_ = nullptr;
  bool dirty_ = false;
};

// ---- Links --------------------------------------------------------------------------------------------------------

using Endpoints = std::shared_ptr<const std::vector<CdnEndpoint>>;

Endpoints Share(std::vector<CdnEndpoint> endpoints) {
  // The copy shared by the workers wipes the signature when the last one lets go.
  return Endpoints(new std::vector<CdnEndpoint>(std::move(endpoints)), [](const std::vector<CdnEndpoint>* list) {
    auto* owned = const_cast<std::vector<CdnEndpoint>*>(list);
    LinkResult::WipeEndpoints(*owned);
    delete owned;
  });
}

// The chunk and range bases of each source: a secure link per product (a gen 1 plan's for its build's main.bin), one
// open link for the dependency store. One fetch at a time per source; a worker that saw an old version waits for the
// fetch in progress instead of starting another.
class Links {
 public:
  Links(const Hooks& hooks, const Logger& log, const std::atomic<bool>& cancel, int generation,
        std::uint64_t timestamp)
      : hooks_(hooks), log_(log), cancel_(cancel), generation_(generation), timestamp_(timestamp) {}

  static std::string Key(const std::string& source) {
    return source.rfind(kDependencySourcePrefix, 0) == 0 ? std::string(kDependencySourcePrefix) : source;
  }

  // The current endpoints of `source`, fetching them when there are none yet, when `stale_version` is the version the
  // caller found refused, or when the link is near its expiry.
  Error Get(Transport& transport, const std::string& source, std::optional<std::uint64_t> stale_version,
            Endpoints* endpoints, std::uint64_t* version) {
    const auto key = Key(source);
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
      auto& entry = entries_[key];
      if (entry.fetching) {
        changed_.wait(lock);
        continue;
      }
      const bool refused = stale_version && *stale_version == entry.version;
      const bool expiring = entry.endpoints && entry.clock_refresh && hooks_.now_seconds &&
                            entry.expires_at - hooks_.now_seconds() < kLinkRefreshSeconds;
      if (entry.endpoints && !refused && !expiring) {
        *endpoints = entry.endpoints;
        *version = entry.version;
        return Error::kNone;
      }
      entry.fetching = true;
      lock.unlock();
      LinkResult result;
      const auto error = Fetch(transport, key, &result);
      lock.lock();
      auto& done = entries_[key];
      done.fetching = false;
      changed_.notify_all();
      if (error != Error::kNone) return error;
      ++fetches_;
      done.endpoints = Share(std::move(result.endpoints));
      done.expires_at = result.expires_at;
      const std::int64_t now = hooks_.now_seconds ? hooks_.now_seconds() : 0;
      // A link that is already near its expiry when it comes (a wrong clock) is fetched again only when refused.
      done.clock_refresh = result.expires_at != 0 && result.expires_at - now >= kLinkRefreshSeconds;
      ++done.version;
      *endpoints = done.endpoints;
      *version = done.version;
      return Error::kNone;
    }
  }

  int fetches() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return fetches_;
  }

 private:
  struct Entry {
    Endpoints endpoints;
    std::int64_t expires_at = 0;
    bool clock_refresh = false;
    std::uint64_t version = 0;
    bool fetching = false;
  };

  Error Fetch(Transport& transport, const std::string& key, LinkResult* out) {
    if (key == kDependencySourcePrefix) {
      *out = FetchOpenLink(transport, cancel_);
      log_("gog open_link status=%ld result=%s", out->status, LinkName(out->kind));
      return LinkError(out->kind);
    }
    std::uint64_t product = 0;
    if (!CanonicalId(key, &product)) return Error::kProtocol;
    for (int round = 0; round < 2; ++round) {
      if (!hooks_.token) return Error::kSignedOut;
      auto token = hooks_.token(cancel_);
      if (token.status != AccessToken::Status::kOk) return FromToken(token.status);
      *out = generation_ == 1 ? FetchSecureLinkV1(transport, token.token, product, timestamp_, cancel_)
                              : FetchSecureLink(transport, token.token, product, cancel_);
      log_("gog secure_link generation=%d status=%ld result=%s", generation_, out->status, LinkName(out->kind));
      if (out->kind != LinkResult::Kind::kRejected) return LinkError(out->kind);
      if (round == 0 && hooks_.invalidate) hooks_.invalidate(token.generation, token.token);
    }
    return Error::kSignedOut;
  }

  static const char* LinkName(LinkResult::Kind kind) {
    switch (kind) {
      case LinkResult::Kind::kOk: return "ok";
      case LinkResult::Kind::kRejected: return "rejected";
      case LinkResult::Kind::kForbidden: return "forbidden";
      case LinkResult::Kind::kNetwork: return "network";
      case LinkResult::Kind::kProtocol: return "protocol";
      case LinkResult::Kind::kCanceled: return "canceled";
    }
    return "network";
  }

  static Error LinkError(LinkResult::Kind kind) {
    switch (kind) {
      case LinkResult::Kind::kOk: return Error::kNone;
      case LinkResult::Kind::kRejected: return Error::kSignedOut;
      case LinkResult::Kind::kForbidden: return Error::kNotOwned;
      case LinkResult::Kind::kNetwork: return Error::kNetwork;
      case LinkResult::Kind::kProtocol: return Error::kProtocol;
      case LinkResult::Kind::kCanceled: return Error::kCanceled;
    }
    return Error::kNetwork;
  }

  const Hooks& hooks_;
  const Logger& log_;
  const std::atomic<bool>& cancel_;
  const int generation_;
  const std::uint64_t timestamp_;
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  std::map<std::string, Entry> entries_;
  int fetches_ = 0;
};

// ---- Downloads ----------------------------------------------------------------------------------------------------

struct FileWork {
  std::size_t index = 0;
  std::string path;  // absolute
  std::string source;
  std::uint64_t size = 0;
  std::string md5;
  std::string sha256;
  Units units;
  bool whole_md5 = false;  // a gen 1 product file: its MD5 is checked however many ranges it has, even none
  std::vector<std::uint64_t> offsets;  // where each unit starts in the file: the running sums
  std::vector<bool> done;
  std::size_t remaining = 0;
  bool redone = false;
};

struct Task {
  std::shared_ptr<FileWork> file;
  std::size_t unit = 0;
};

Error FromChunk(ChunkResult::Kind kind) {
  switch (kind) {
    case ChunkResult::Kind::kOk: return Error::kNone;
    case ChunkResult::Kind::kCorrupt: return Error::kCorrupt;
    case ChunkResult::Kind::kExpired: return Error::kNetwork;
    case ChunkResult::Kind::kNetwork: return Error::kNetwork;
    case ChunkResult::Kind::kCanceled: return Error::kCanceled;
    case ChunkResult::Kind::kStorage: return Error::kStorage;
    case ChunkResult::Kind::kNoSpace: return Error::kNoSpace;
  }
  return Error::kNetwork;
}

// Writes a chunk's bytes into its file, opened once per attempt by absolute path; links are refused.
class ChunkFile {
 public:
  explicit ChunkFile(const std::string& path) : path_(path) {}
  ~ChunkFile() { Close(); }
  bool Write(std::uint64_t offset, const std::uint8_t* data, std::size_t size) {
    if (fd_ < 0) {
      fd_ = open(path_.c_str(), O_WRONLY | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0666);
      if (fd_ < 0) return false;
    }
    while (size > 0) {
      const auto written = pwrite(fd_, data, size, static_cast<off_t>(offset));
      if (written < 0) {
        if (errno == EINTR) continue;
        return false;
      }
      if (written == 0) {
        errno = EIO;
        return false;
      }
      data += written;
      offset += static_cast<std::uint64_t>(written);
      size -= static_cast<std::size_t>(written);
    }
    return true;
  }
  // 0, or the errno of a failed close.
  int Close() {
    if (fd_ < 0) return 0;
    const int result = close(fd_);
    fd_ = -1;
    return result == 0 ? 0 : (errno ? errno : EIO);
  }

 private:
  const std::string& path_;
  int fd_ = -1;
};

class Download {
 public:
  Download(const Hooks& hooks, const ChunkPause& pause, const std::atomic<bool>& cancel, const Logger& log,
           Journal& journal, Links& links, GogInstaller::Outcome::Stats& stats)
      : hooks_(hooks), pause_(pause), cancel_(cancel), log_(log), journal_(journal), links_(links), stats_(stats) {}

  // Starts the workers; false when none could start.
  bool Start(int parallel) {
    active_limit_ = parallel;
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) return false;
    const bool stack = pthread_attr_setstacksize(&attr, kWorkerStackBytes) == 0;
    for (int index = 0; stack && index < parallel; ++index) {
      auto worker = std::make_unique<Worker>();
      worker->owner = this;
      worker->id = index;
      if (pthread_create(&worker->thread, &attr,
                         [](void* context) -> void* {
                           auto* self = static_cast<Worker*>(context);
                           self->owner->Work(self->id);
                           return nullptr;
                         },
                         worker.get()) != 0)
        break;
      workers_.push_back(std::move(worker));
    }
    pthread_attr_destroy(&attr);
    return !workers_.empty();
  }

  void Join() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      feeding_done_ = true;
    }
    changed_.notify_all();
    for (auto& worker : workers_) pthread_join(worker->thread, nullptr);
    workers_.clear();
  }

  // Waits for room, then queues the file's chunks that are not done yet. False once the download stops.
  bool Admit(std::shared_ptr<FileWork> file, const std::function<void()>& tick) {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stop_ && (queue_.size() >= kMaxQueuedChunks || active_files_ >= kMaxActiveFiles)) {
      changed_.wait_for(lock, std::chrono::milliseconds(250));
      lock.unlock();
      tick();
      lock.lock();
    }
    if (stop_) return false;
    ++active_files_;
    for (std::size_t unit = 0; unit < file->units.count(); ++unit)
      if (!file->done[unit]) queue_.push_back({file, unit});
    changed_.notify_all();
    return true;
  }

  // Waits until every queued chunk is done or the download stops.
  void Drain(const std::function<void()>& tick) {
    std::unique_lock<std::mutex> lock(mutex_);
    feeding_done_ = true;
    changed_.notify_all();
    while (!stop_ && (!queue_.empty() || in_flight_ > 0 || active_files_ > 0)) {
      changed_.wait_for(lock, std::chrono::milliseconds(250));
      lock.unlock();
      tick();
      lock.lock();
    }
  }

  // A file whose units are all written: its size set and its whole-file hashes checked (a gen 2 file's MD5 only when it
  // has more than one chunk, each chunk having its own; a gen 1 file's always). On a mismatch it is
  // fetched once more, then the download fails with kCorrupt. Called by the worker that wrote its last chunk, or by
  // the feeder for a file the journal already had whole.
  void Finish(const std::shared_ptr<FileWork>& file, bool admitted) {
    Error error = Error::kNone;
    const int fd = open(file->path.c_str(), O_WRONLY | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0666);
    if (fd < 0) {
      error = WriteFailure();
    } else {
      if (ftruncate(fd, static_cast<off_t>(file->size)) != 0) error = WriteFailure();
      if (close(fd) != 0 && error == Error::kNone) error = WriteFailure();
    }
    bool matches = true;
    const bool want_md5 = !file->md5.empty() && (file->whole_md5 || file->units.count() > 1);
    if (error == Error::kNone && (want_md5 || !file->sha256.empty())) {
      Md5 md5;
      Sha256 sha;
      const bool want_sha = !file->sha256.empty();
      if (!ReadFile(file->path, [&](const std::uint8_t* data, std::size_t size) {
            if (want_md5) md5.Update(data, size);
            if (want_sha) sha.Update(data, size);
            return !cancel_.load();
          })) {
        error = cancel_.load() ? Error::kCanceled : Error::kStorage;
      } else {
        matches = (!want_md5 || md5.FinalHex() == file->md5) && (!want_sha || sha.FinalHex() == file->sha256);
      }
    }
    if (error == Error::kNone && !matches) {
      std::unique_lock<std::mutex> lock(mutex_);
      if (file->redone) {
        log_("gog file check failed again");
        lock.unlock();
        Stop(Error::kCorrupt);
        return;
      }
      if (file->units.count() == 0) {
        // An empty file the manifest gives another hash: nothing to fetch again.
        log_("gog file check failed units=0");
        lock.unlock();
        Stop(Error::kCorrupt);
        return;
      }
      file->redone = true;
      ++stats_.redone_files;
      log_("gog file check failed units=%zu: fetching it again", file->units.count());
      std::fill(file->done.begin(), file->done.end(), false);
      file->remaining = file->units.count();
      for (std::size_t unit = 0; unit < file->units.count(); ++unit) {
        done_ -= std::min(done_, file->units.download(unit));
        queue_.push_back({file, unit});
      }
      if (!admitted) ++active_files_;
      changed_.notify_all();
      return;
    }
    if (error == Error::kNone) error = journal_.File(file->index);
    if (error != Error::kNone) {
      Stop(error);
      return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (admitted) --active_files_;
    changed_.notify_all();
  }

  void Stop(Error error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!stop_) {
      stop_ = true;
      error_ = error;
    }
    changed_.notify_all();
  }

  Error error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return error_;
  }
  bool stopped() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stop_;
  }
  std::uint64_t done() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return done_;
  }
  void AddDone(std::uint64_t bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    done_ += bytes;
  }
  int active_limit() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_limit_;
  }

 private:
  struct Worker {
    Download* owner = nullptr;
    int id = 0;
    pthread_t thread{};
  };

  void Work(int id) {
    Transport transport = hooks_.make_transport ? hooks_.make_transport() : Transport();
    if (!transport) {
      Stop(Error::kNetwork);
      return;
    }
    for (;;) {
      Task task;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        changed_.wait(lock, [&] {
          return stop_ || (!queue_.empty() && id < active_limit_) ||
                 (feeding_done_ && queue_.empty() && in_flight_ == 0);
        });
        if (stop_ || queue_.empty()) return;
        task = std::move(queue_.front());
        queue_.pop_front();
        ++in_flight_;
      }
      if (cancel_.load()) {
        Release();
        Stop(Error::kCanceled);
        return;
      }
      const auto error = Fetch(transport, task);
      bool last = false;
      if (error == Error::kNone) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& file = *task.file;
        if (!file.done[task.unit]) {
          file.done[task.unit] = true;
          --file.remaining;
          done_ += file.units.download(task.unit);
        }
        last = file.remaining == 0;
      }
      if (error == Error::kNone && last) Finish(task.file, true);
      Release();
      if (error != Error::kNone) {
        Stop(error);
        return;
      }
    }
  }

  void Release() {
    std::lock_guard<std::mutex> lock(mutex_);
    --in_flight_;
    changed_.notify_all();
  }

  // One chunk or range, with fresh links when the CDN refuses the one in use.
  Error Fetch(Transport& transport, const Task& task) {
    const auto& file = *task.file;
    std::optional<std::uint64_t> stale;
    for (int refresh = 0;; ++refresh) {
      Endpoints endpoints;
      std::uint64_t version = 0;
      if (const auto error = links_.Get(transport, file.source, stale, &endpoints, &version); error != Error::kNone)
        return error;
      ChunkFile out(file.path);
      const ChunkWrite write = [&out](std::uint64_t offset, const std::uint8_t* data, std::size_t size) {
        return out.Write(offset, data, size);
      };
      const auto result =
          file.units.chunks.empty()
              ? FetchRange(transport, *endpoints, file.units.ranges[task.unit].offset,
                           file.units.ranges[task.unit].length, file.offsets[task.unit], write, pause_, cancel_)
              : FetchChunk(transport, *endpoints, file.units.chunks[task.unit], file.offsets[task.unit], write, pause_,
                           cancel_);
      endpoints.reset();
      const int close_error = out.Close();
      Account(result);
      const char* what = file.units.chunks.empty() ? "range" : "chunk";
      if (result.kind == ChunkResult::Kind::kExpired) {
        log_("gog %s refused status=%ld endpoint=%s: fetching the link again", what, result.status,
             result.endpoint.c_str());
        if (refresh >= kMaxLinkRefreshesPerChunk) return Error::kNetwork;
        stale = version;
        continue;
      }
      if (result.kind != ChunkResult::Kind::kOk) {
        log_("gog %s failed result=%s attempts=%d status=%ld endpoint=%s", what, ChunkResultName(result.kind),
             result.attempts, result.status, result.endpoint.c_str());
        return FromChunk(result.kind);
      }
      if (close_error != 0) return close_error == ENOSPC ? Error::kNoSpace : Error::kStorage;
      if (result.attempts > 1)
        log_("gog %s ok attempts=%d corrupt=%d stalls=%d endpoint=%s", what, result.attempts, result.corrupt,
             result.stalls, result.endpoint.c_str());
      if (hooks_.endpoint) hooks_.endpoint(result.endpoint.c_str());
      return journal_.Chunk(file.index, task.unit);
    }
  }

  // Counts retries and stalls; three stalls within the window take one worker off.
  void Account(const ChunkResult& result) {
    std::lock_guard<std::mutex> lock(mutex_);
    stats_.corrupt_retries += result.kind == ChunkResult::Kind::kOk ? result.corrupt : std::max(0, result.corrupt - 1);
    stats_.stalls += result.stalls;
    if (result.stalls == 0) return;
    const std::int64_t now = hooks_.now_seconds ? hooks_.now_seconds() : 0;
    for (int i = 0; i < result.stalls; ++i) stall_times_.push_back(now);
    while (!stall_times_.empty() && now - stall_times_.front() > kStallWindowSeconds) stall_times_.pop_front();
    if (static_cast<int>(stall_times_.size()) >= kStallsToSlowDown && active_limit_ > 1) {
      --active_limit_;
      stall_times_.clear();
      log_("gog download parallel=%d", active_limit_);
    }
  }

  const Hooks& hooks_;
  const ChunkPause& pause_;
  const std::atomic<bool>& cancel_;
  const Logger& log_;
  Journal& journal_;
  Links& links_;
  GogInstaller::Outcome::Stats& stats_;

  mutable std::mutex mutex_;
  std::condition_variable changed_;
  std::deque<Task> queue_;
  std::size_t in_flight_ = 0;
  std::size_t active_files_ = 0;
  bool feeding_done_ = false;
  bool stop_ = false;
  Error error_ = Error::kNone;
  int active_limit_ = 0;
  std::deque<std::int64_t> stall_times_;
  std::uint64_t done_ = 0;
  std::vector<std::unique_ptr<Worker>> workers_;
};

// ---- One plan's download ------------------------------------------------------------------------------------------

// What a download covers: the plan's file entries (or those `wanted` picks), written under `dest_root` with their
// progress in `journal`. An install's makes the plan's folders too and counts what it held at the start in progress;
// an update's progress counts only its own files.
struct DownloadJob {
  std::string plan;
  std::string journal;
  std::string dest_root;
  std::string drive;  // the game folder: free_bytes is asked for it
  bool folders = false;
  bool report_held = false;
  std::function<bool(const PlanEntry&)> wanted;  // empty: every file entry
};

// Run's and Stage's download of a saved plan: Scan reads the journal and the plan (what is finished, what is left),
// EnoughSpace checks the drive, Fetch gets the links each source needs and then feeds the workers in plan order.
class PlanDownload {
 public:
  PlanDownload(const Hooks& hooks, const ChunkPause& pause, const std::atomic<bool>& cancel, const Logger& log,
               const PlanHeader& header, DownloadJob job)
      : hooks_(hooks), pause_(pause), cancel_(cancel), log_(log), header_(header), job_(std::move(job)) {}

  // A plan that doesn't read is removed: it is made again next time, with a new journal.
  Error Scan() {
    state_ = LoadJournal(job_.journal);
    EntryReader reader(job_.plan);
    PlanHeader again;
    if (!reader.open() || !reader.Header(&again)) return Error::kStorage;
    PlanEntry entry;
    for (std::uint64_t index = 0;; ++index) {
      const auto next = reader.Read(&entry);
      if (next == EntryReader::Next::kEnd) break;
      if (next == EntryReader::Next::kBad) {
        std::remove(job_.plan.c_str());
        log_("gog plan damaged");
        return Error::kStorage;
      }
      if (entry.directory) continue;
      ++files_;
      if (job_.wanted && !job_.wanted(entry)) continue;
      ++wanted_files_;
      wanted_size_ += entry.size;
      wanted_download_ += entry.download();
      const auto path = Join(job_.dest_root, entry.path);
      struct stat info {};
      const bool present = lstat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode);
      const bool marked = state_.files.count(index) > 0;
      if (marked && present && static_cast<std::uint64_t>(info.st_size) == entry.size) {
        finished_.insert(index);
        held_ += entry.size;
        done_ += entry.download();
        continue;
      }
      ++unfinished_;
      // A finished file that is gone or has another size is fetched whole again; so is a file in progress that is
      // gone.
      const bool redo = marked || !present;
      for (std::size_t unit = 0; unit < entry.units.count(); ++unit) {
        const auto key = ChunkKey(index, unit);
        if (!redo && state_.chunks.count(key)) {
          held_ += entry.units.size(unit);
          done_ += entry.units.download(unit);
        } else {
          state_.chunks.erase(key);
          missing_ += entry.units.size(unit);
          ++pending_chunks_;
          sources_.insert(Links::Key(entry.source));
        }
      }
    }
    total_ = job_.wanted ? wanted_download_ : header_.total_compressed;
    log_("gog download files=%zu finished=%zu units_left=%llu", wanted_files_, finished_.size(),
         static_cast<unsigned long long>(pending_chunks_));
    return Error::kNone;
  }

  // The bytes the unfinished units write, plus kGogSpaceMargin, against the drive's free bytes.
  bool EnoughSpace() const {
    const auto free = hooks_.free_bytes(job_.drive);
    if (missing_ > UINT64_MAX - kGogSpaceMargin || missing_ + kGogSpaceMargin > free) {
      log_("gog download space short need=%llu free=%llu", static_cast<unsigned long long>(missing_),
           static_cast<unsigned long long>(free));
      return false;
    }
    return true;
  }

  Error Fetch(const std::function<void(const GogInstaller::Progress&)>& progress,
              GogInstaller::Outcome::Stats& stats) {
    const std::uint64_t held = job_.report_held ? held_ : 0;
    if (progress) progress({done_, total_, held});

    Journal journal;
    if (const auto error = journal.Open(job_.journal); error != Error::kNone) return error;
    Links links(hooks_, log_, cancel_, header_.generation, header_.timestamp);

    // The links each source needs, before any chunk.
    const int parallel = std::max(1, std::min(kMaxParallel, hooks_.parallel));
    Download download(hooks_, pause_, cancel_, log_, journal, links, stats);
    download.AddDone(done_);
    if (pending_chunks_ > 0) {
      Error error = Error::kNone;
      if (!RunOnStack(kWorkerStackBytes, [&] {
            Transport transport = hooks_.make_transport ? hooks_.make_transport() : Transport();
            if (!transport) {
              error = Error::kNetwork;
              return;
            }
            for (const auto& source : sources_) {
              Endpoints endpoints;
              std::uint64_t version = 0;
              error = links.Get(transport, source, std::nullopt, &endpoints, &version);
              if (error != Error::kNone) return;
            }
          }))
        error = Error::kNetwork;
      stats.link_fetches = links.fetches();
      if (error != Error::kNone) return error;
    }
    // Workers also for a file whose chunks are all journaled: a failed whole-file check fetches it again.
    if (unfinished_ > 0) {
      if (!download.Start(parallel)) return Error::kNetwork;
      log_("gog download start parallel=%d", parallel);
    }

    // Feed the workers in plan order.
    auto last_sync = std::chrono::steady_clock::now();
    Error journal_error = Error::kNone;
    const auto tick = [&] {
      if (cancel_.load()) download.Stop(Error::kCanceled);
      const auto now = std::chrono::steady_clock::now();
      if (now - last_sync >= std::chrono::seconds(2)) {
        last_sync = now;
        if (const auto error = journal.Sync(); error != Error::kNone && journal_error == Error::kNone) {
          journal_error = error;
          download.Stop(error);
        }
      }
      if (progress) progress({std::min(download.done(), total_), total_, held});
    };
    Feed(download, tick);
    download.Drain(tick);
    download.Join();
    stats.link_fetches = links.fetches();
    stats.parallel = unfinished_ > 0 ? download.active_limit() : 0;
    const auto closed = journal.Close();
    if (progress) progress({std::min(download.done(), total_), total_, held});
    Error error = download.error();
    if (error == Error::kNone && cancel_.load()) error = Error::kCanceled;
    if (error == Error::kNone) error = closed;
    return error;
  }

  std::size_t files() const { return files_; }  // the plan's file entries
  std::size_t wanted_files() const { return wanted_files_; }
  std::uint64_t wanted_size() const { return wanted_size_; }
  std::uint64_t wanted_download() const { return wanted_download_; }

 private:
  void Feed(Download& download, const std::function<void()>& tick) {
    EntryReader reader(job_.plan);
    PlanHeader again;
    bool readable = reader.open() && reader.Header(&again);
    PlanEntry entry;
    for (std::uint64_t index = 0; readable && !download.stopped(); ++index) {
      if (cancel_.load()) {
        download.Stop(Error::kCanceled);
        break;
      }
      const auto next = reader.Read(&entry);
      if (next == EntryReader::Next::kEnd) break;
      if (next == EntryReader::Next::kBad) {
        readable = false;
        break;
      }
      if (entry.directory) {
        if (job_.folders && !MakeFolders(job_.dest_root, entry.path, true)) download.Stop(WriteFailure());
        continue;
      }
      if (finished_.count(index) || (job_.wanted && !job_.wanted(entry))) continue;
      if (!MakeFolders(job_.dest_root, entry.path, false)) {
        download.Stop(WriteFailure());
        break;
      }
      auto file = std::make_shared<FileWork>();
      file->index = index;
      file->path = Join(job_.dest_root, entry.path);
      struct stat info {};
      if (lstat(file->path.c_str(), &info) == 0 && !S_ISREG(info.st_mode)) {
        log_("gog download a folder or link is in a file's place");
        download.Stop(Error::kStorage);
        break;
      }
      file->source = entry.source;
      file->size = entry.size;
      file->md5 = entry.md5;
      file->sha256 = entry.sha256;
      file->whole_md5 = header_.generation == 1 && !DependencySource(entry.source);
      file->units = std::move(entry.units);
      file->done.assign(file->units.count(), false);
      std::uint64_t offset = 0;
      for (std::size_t unit = 0; unit < file->units.count(); ++unit) {
        file->offsets.push_back(offset);
        offset += file->units.size(unit);
        if (state_.chunks.count(ChunkKey(index, unit))) file->done[unit] = true;
        else ++file->remaining;
      }
      if (file->remaining == 0) {
        // No units (an empty file), or every unit already written: only its size and check are left.
        download.Finish(file, false);
        continue;
      }
      if (!download.Admit(file, tick)) break;
    }
    if (!readable) download.Stop(Error::kStorage);
  }

  const Hooks& hooks_;
  const ChunkPause& pause_;
  const std::atomic<bool>& cancel_;
  const Logger& log_;
  const PlanHeader& header_;
  const DownloadJob job_;
  JournalState state_;
  std::size_t files_ = 0, wanted_files_ = 0;
  std::uint64_t wanted_size_ = 0, wanted_download_ = 0;
  std::uint64_t held_ = 0, done_ = 0, missing_ = 0, pending_chunks_ = 0, unfinished_ = 0, total_ = 0;
  std::set<std::string> sources_;
  std::unordered_set<std::uint64_t> finished_;
};

// ---- An update's lists --------------------------------------------------------------------------------------------

bool IsRegularFile(const std::string& path) {
  struct stat info {};
  return lstat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode);
}

// Adds the "d\t<path>" and "f\t<path>" lines of a file list. A list that is missing adds nothing.
bool AddListLines(const std::string& path, std::set<std::string>* lines) {
  if (!Exists(path)) return true;
  LineReader reader(path);
  if (!reader.open()) return false;
  std::string line;
  bool complete = false;
  while (reader.Next(&line, &complete)) {
    if (!complete) break;  // a torn last line
    if (line.size() >= 3 && line[1] == '\t' && (line[0] == 'd' || line[0] == 'f') &&
        ValidPlanPath(std::string_view(line).substr(2)))
      lines->insert(line);
  }
  return !reader.failed();
}

// Writes `old` (the live file list as it is) and `paths` (what either version lists, and what an earlier run of this
// update listed: never fewer), each through a temporary file.
Error WriteOldAndPaths(const std::string& root, const std::string& work, const PlanLayout& layout) {
  const auto live_list = Join(root, kFileListName);
  const auto old_path = Join(work, kUpdateOldName);
  const auto paths_path = Join(work, kUpdatePathsName);
  {
    DurableFile old_list(old_path);
    if (Exists(live_list) && !ReadFile(live_list, [&old_list](const std::uint8_t* data, std::size_t size) {
          old_list.Bytes(data, size);
          return true;
        }))
      return Error::kStorage;
    if (const auto error = old_list.Commit(); error != Error::kNone) return error;
  }
  std::set<std::string> lines;
  for (const auto& list : {paths_path, old_path, layout.file_list})
    if (!AddListLines(list, &lines)) return Error::kStorage;
  DurableFile paths(paths_path);
  for (const auto& line : lines) paths.Line(line);
  return paths.Commit();
}

// The installed plan's files the new plan lacks, by FoldCase(path), as DiffPlans counts them; 0 without an installed
// plan that reads.
std::uint64_t CountRemoved(const std::string& old_plan, const std::string& new_plan) {
  std::unordered_set<std::string> kept;
  PlanEntry entry;
  {
    EntryReader reader(new_plan);
    PlanHeader header;
    if (!reader.open() || !reader.Header(&header)) return 0;
    while (reader.Read(&entry) == EntryReader::Next::kEntry)
      if (!entry.directory) kept.insert(FoldCase(entry.path));
  }
  EntryReader reader(old_plan);
  PlanHeader header;
  if (!reader.open() || !reader.Header(&header)) return 0;
  std::uint64_t removed = 0;
  for (;;) {
    const auto next = reader.Read(&entry);
    if (next == EntryReader::Next::kBad) return 0;
    if (next == EntryReader::Next::kEnd) break;
    if (!entry.directory && !kept.count(FoldCase(entry.path))) ++removed;
  }
  return removed;
}

}  // namespace

const char* InstallerErrorName(Error error) {
  switch (error) {
    case Error::kNone: return "none";
    case Error::kCanceled: return "canceled";
    case Error::kSignedOut: return "signed_out";
    case Error::kNetwork: return "network";
    case Error::kCorrupt: return "corrupt";
    case Error::kNoSpace: return "no_space";
    case Error::kStorage: return "storage";
    case Error::kProtocol: return "protocol";
    case Error::kNoBuild: return "no_build";
    case Error::kNotOwned: return "not_owned";
  }
  return "protocol";
}

GogInstaller::GogInstaller(Hooks hooks) : hooks_(std::move(hooks)) {}

GogInstaller::Outcome GogInstaller::Plan(const Target& target, const PlanLayout& layout,
                                         const std::atomic<bool>& cancel) {
  Outcome outcome;
  const Logger log(hooks_.log);
  const ChunkPause pause = hooks_.pause ? ChunkPause(hooks_.pause) : ChunkPause(SleepPause);
  const auto finish = [&](Error error) {
    outcome.error = error;
    log("gog plan result=%s", InstallerErrorName(error));
    return outcome;
  };
  // An installed game's folder: never made here.
  if (target.product == 0 || !ValidRoot(target.root) || !IsRealDirectory(target.root)) return finish(Error::kStorage);
  if (cancel.load()) return finish(Error::kCanceled);
  Error error = Error::kNone;
  if (!RunOnStack(kPlannerStackBytes, [&] {
        Planner planner(hooks_, target, layout, pause, cancel, log);
        error = planner.Plan();
      }))
    return finish(Error::kNetwork);
  if (error != Error::kNone) return finish(error);
  EntryReader reader(layout.plan);
  PlanHeader header;
  if (!reader.open() || !reader.Header(&header)) return finish(Error::kStorage);
  PlanEntry entry;
  for (;;) {
    const auto next = reader.Read(&entry);
    if (next == EntryReader::Next::kEnd) break;
    if (next == EntryReader::Next::kBad) return finish(Error::kStorage);
    if (!entry.directory) ++outcome.files;
  }
  outcome.generation = header.generation;
  outcome.build_id = header.build_id;
  outcome.meta_id = header.meta_id;
  outcome.timestamp = header.timestamp;
  outcome.language = header.language;
  outcome.products = header.products;
  outcome.install_bytes = header.total_size;
  return finish(Error::kNone);
}

GogInstaller::Outcome GogInstaller::Run(const Target& target, const std::atomic<bool>& cancel,
                                        const std::function<void(const Progress&)>& progress) {
  Outcome outcome;
  const Logger log(hooks_.log);
  const ChunkPause pause = hooks_.pause ? ChunkPause(hooks_.pause) : ChunkPause(SleepPause);
  const auto finish = [&](Error error) {
    outcome.error = error;
    log("gog install result=%s", InstallerErrorName(error));
    return outcome;
  };
  if (target.product == 0 || !ValidRoot(target.root)) return finish(Error::kStorage);
  if (!hooks_.free_bytes) return finish(Error::kStorage);

  // 1. The folder and .playden-download before anything else.
  if (!MakeRoot(target.root) || !MakeDirectory(Join(target.root, kDownloadDirName)) ||
      !MakeDirectory(Join(target.root, kGogDirName)))
    return finish(WriteFailure());
  if (cancel.load()) return finish(Error::kCanceled);

  // 2-4. A saved plan, or a new one.
  const auto layout = InstallLayout(target.root);
  const auto& plan_path = layout.plan;
  PlanHeader header;
  bool have_plan = false;
  if (Exists(plan_path)) {
    EntryReader reader(plan_path);
    have_plan = reader.open() && reader.Header(&header);
    if (!have_plan) {
      log("gog plan saved one unreadable: planning again");
      std::remove(plan_path.c_str());
    }
  }
  if (have_plan) {
    log("gog plan reused");
  } else {
    Error error = Error::kNone;
    if (!RunOnStack(kPlannerStackBytes, [&] {
          Planner planner(hooks_, target, layout, pause, cancel, log);
          error = planner.Plan();
        }))
      return finish(Error::kNetwork);
    if (error != Error::kNone) return finish(error);
    EntryReader reader(plan_path);
    if (!reader.open() || !reader.Header(&header)) return finish(Error::kStorage);
  }
  outcome.generation = header.generation;
  outcome.build_id = header.build_id;
  outcome.meta_id = header.meta_id;
  outcome.timestamp = header.timestamp;
  outcome.language = header.language;
  outcome.products = header.products;
  outcome.install_bytes = header.total_size;

  // 5-8. What the journal already has, the space the rest needs, then the rest.
  DownloadJob job;
  job.plan = plan_path;
  job.journal = layout.journal;
  job.dest_root = target.root;
  job.drive = target.root;
  job.folders = true;
  job.report_held = true;
  PlanDownload download(hooks_, pause, cancel, log, header, std::move(job));
  if (const auto error = download.Scan(); error != Error::kNone) return finish(error);
  outcome.files = download.files();
  if (!download.EnoughSpace()) return finish(Error::kNoSpace);
  const auto error = download.Fetch(progress, outcome.stats);
  if (error == Error::kNone)
    log("gog download done files=%zu size=%llu", outcome.files, static_cast<unsigned long long>(header.total_size));
  return finish(error);
}

GogInstaller::StageOutcome GogInstaller::Stage(const Target& target, const std::atomic<bool>& cancel,
                                               const std::function<void(const Progress&)>& progress) {
  StageOutcome outcome;
  const Logger log(hooks_.log);
  const ChunkPause pause = hooks_.pause ? ChunkPause(hooks_.pause) : ChunkPause(SleepPause);
  const auto finish = [&](Error error) {
    outcome.error = error;
    log("gog update step=stage result=%s files=%zu changed=%llu removed=%llu bytes=%llu parallel=%d",
        InstallerErrorName(error), outcome.files, static_cast<unsigned long long>(outcome.staged_files),
        static_cast<unsigned long long>(outcome.removed_files),
        static_cast<unsigned long long>(outcome.download_bytes), outcome.stats.parallel);
    return outcome;
  };
  // 1. An installed game's folder (never made here), and the update's work folders, each a real folder.
  const auto& root = target.root;
  if (target.product == 0 || !ValidRoot(root) || !IsRealDirectory(root) ||
      !IsRegularFile(Join(root, kInstallRecordName)) || !hooks_.free_bytes)
    return finish(Error::kStorage);
  const auto work = Join(root, kUpdateDirName);
  const auto staging = Join(work, kUpdateStagingName);
  if (!MakeDirectory(work) || !MakeDirectory(Join(work, "gog")) || !MakeDirectory(staging))
    return finish(WriteFailure());
  if (cancel.load()) return finish(Error::kCanceled);

  // 2. A saved update plan (with its file list), or a new one seeded with the installed spellings.
  const auto layout = UpdateLayout(root);
  const auto staged_path = Join(work, kUpdateStagedName);
  PlanHeader header;
  bool have_plan = false;
  if (Exists(layout.plan)) {
    EntryReader reader(layout.plan);
    have_plan = reader.open() && reader.Header(&header) && IsRegularFile(layout.file_list);
    if (!have_plan) {
      log("gog update plan saved one unreadable: planning again");
      std::remove(layout.plan.c_str());
    }
  }
  if (have_plan) {
    log("gog update plan reused");
  } else {
    // A staged list names the files of the plan it was made from.
    if (std::remove(staged_path.c_str()) != 0 && errno != ENOENT) return finish(Error::kStorage);
    Target planned = target;
    planned.seed_list = Join(root, kFileListName);
    Error error = Error::kNone;
    if (!RunOnStack(kPlannerStackBytes, [&] {
          Planner planner(hooks_, planned, layout, pause, cancel, log);
          error = planner.Plan();
        }))
      error = Error::kNetwork;
    log("gog update step=plan result=%s", InstallerErrorName(error));
    if (error != Error::kNone) return finish(error);
    EntryReader reader(layout.plan);
    if (!reader.open() || !reader.Header(&header)) return finish(Error::kStorage);
  }
  outcome.generation = header.generation;
  outcome.build_id = header.build_id;
  outcome.meta_id = header.meta_id;
  outcome.timestamp = header.timestamp;
  outcome.language = header.language;
  outcome.products = header.products;
  outcome.install_bytes = header.total_size;

  // 3. The lists, before the first staged chunk: old, paths, then staged from the diff. Once staged exists it is the
  // update's: a resume fetches what it names and never diffs again.
  const auto installed_plan = InstallLayout(root).plan;
  bool diffed = false;
  if (!Exists(staged_path)) {
    if (cancel.load()) return finish(Error::kCanceled);
    if (const auto error = WriteOldAndPaths(root, work, layout); error != Error::kNone) return finish(error);
    DurableFile staged(staged_path);
    const auto diff = DiffPlans(installed_plan, layout.plan, root, cancel,
                                [&staged](std::uint64_t, const PlanEntry& entry, bool changed) {
                                  if (changed && !entry.directory) staged.Line(entry.path);
                                });
    const char* result = diff.canceled ? "canceled" : diff.readable ? "ok" : "damaged";
    log("gog update step=diff result=%s changed=%llu removed=%llu hashed=%llu bytes=%llu", result,
        static_cast<unsigned long long>(diff.changed_files), static_cast<unsigned long long>(diff.removed_files),
        static_cast<unsigned long long>(diff.hashed_files), static_cast<unsigned long long>(diff.download_bytes));
    if (diff.canceled) return finish(Error::kCanceled);
    if (!diff.readable) {
      // Planned again next time.
      std::remove(layout.plan.c_str());
      return finish(Error::kStorage);
    }
    if (const auto error = staged.Commit(); error != Error::kNone) return finish(error);
    outcome.removed_files = diff.removed_files;
    outcome.hashed_files = diff.hashed_files;
    diffed = true;
  }
  std::unordered_set<std::string> staged;
  {
    LineReader reader(staged_path);
    if (!reader.open()) return finish(Error::kStorage);
    std::string line;
    bool complete = false;
    while (reader.Next(&line, &complete)) {
      if (!complete) return finish(Error::kStorage);
      staged.insert(line);
    }
    if (reader.failed()) return finish(Error::kStorage);
  }
  if (!diffed) outcome.removed_files = CountRemoved(installed_plan, layout.plan);

  // 4-6. The staged files the journal hasn't finished, the space they need, then the download into staging/.
  DownloadJob job;
  job.plan = layout.plan;
  job.journal = layout.journal;
  job.dest_root = staging;
  job.drive = root;
  job.wanted = [&staged](const PlanEntry& entry) { return staged.count(entry.path) > 0; };
  PlanDownload download(hooks_, pause, cancel, log, header, std::move(job));
  if (const auto error = download.Scan(); error != Error::kNone) return finish(error);
  outcome.files = download.files();
  outcome.staged_files = download.wanted_files();
  outcome.staged_bytes = download.wanted_size();
  outcome.download_bytes = download.wanted_download();
  // 7. Nothing staged: nothing to download.
  if (outcome.staged_files == 0) {
    if (progress) progress({0, 0, 0});
    return finish(Error::kNone);
  }
  if (!download.EnoughSpace()) return finish(Error::kNoSpace);
  return finish(download.Fetch(progress, outcome.stats));
}

}  // namespace playden::gog
