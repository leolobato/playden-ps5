// SPDX-License-Identifier: GPL-3.0-or-later
#include "gog/installer.h"

#include "gog/api.h"
#include "gog/chunk_fetch.h"
#include "gog/content.h"
#include "gog/depot_reader.h"
#include "gog/hash.h"
#include "gog/inflate.h"
#include "gog/path_rules.h"
#include "gog/product_id.h"
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
constexpr std::uint64_t kMaxChunksPerFile = 1u << 24;
constexpr std::size_t kReadBufferBytes = 1024 * 1024;
constexpr std::size_t kMaxLineBytes = 64 * 1024;
// A build meta or manifest on the CDN: within the streaming client's cap.
constexpr std::size_t kMaxCompressedMetaBytes = steam::kMaxHttpStreamResponseBytes;
constexpr char kMetaDir[] = ".playden-gog/meta";
constexpr char kManifestDir[] = ".playden-gog/manifests";
constexpr char kItemsPath[] = ".playden-gog/items.tmp";
constexpr char kDependencySourcePrefix[] = "dep:";

// ---- Small helpers ----------------------------------------------------------------------------------------------

std::string Join(const std::string& root, std::string_view relative) {
  std::string path = root;
  path += '/';
  path += relative;
  return path;
}

bool ParseU64(std::string_view text, std::uint64_t* out) {
  if (text.empty() || text.size() > 20) return false;
  std::uint64_t value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') return false;
    const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
    if (value > (UINT64_MAX - digit) / 10) return false;
    value = value * 10 + digit;
  }
  *out = value;
  return true;
}

std::vector<std::string_view> Split(std::string_view line, char separator) {
  std::vector<std::string_view> fields;
  std::size_t start = 0;
  for (;;) {
    const auto at = line.find(separator, start);
    if (at == std::string_view::npos) {
      fields.push_back(line.substr(start));
      return fields;
    }
    fields.push_back(line.substr(start, at - start));
    start = at + 1;
  }
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
  for (const auto part : Split(std::string_view(root).substr(1), '/'))
    if (part.empty() || part == "." || part == "..") return false;
  return true;
}

// A plan path: relative, no empty, "." or ".." component.
bool ValidPlanPath(std::string_view path) {
  if (path.empty() || path.size() > kMaxPathBytes + 64 || path.front() == '/') return false;
  for (const auto part : Split(path, '/'))
    if (part.empty() || part == "." || part == "..") return false;
  for (const char c : path)
    if (static_cast<unsigned char>(c) < 0x20) return false;
  return true;
}

bool Exists(const std::string& path) {
  struct stat info {};
  return lstat(path.c_str(), &info) == 0;
}

Error WriteFailure() { return errno == ENOSPC ? Error::kNoSpace : Error::kStorage; }

// Reads lines through a heap buffer; `complete` is false for a last line without its newline.
class LineReader {
 public:
  explicit LineReader(const std::string& path) : file_(std::fopen(path.c_str(), "rb")), buffer_(new char[4096]) {}
  ~LineReader() {
    if (file_) std::fclose(file_);
  }
  LineReader(const LineReader&) = delete;
  LineReader& operator=(const LineReader&) = delete;
  bool open() const { return file_ != nullptr; }
  bool Next(std::string* line, bool* complete) {
    line->clear();
    *complete = false;
    if (!file_) return false;
    while (std::fgets(buffer_.get(), 4096, file_)) {
      const std::size_t length = std::strlen(buffer_.get());
      line->append(buffer_.get(), length);
      if (!line->empty() && line->back() == '\n') {
        line->pop_back();
        *complete = true;
        return true;
      }
      if (line->size() > kMaxLineBytes) return true;  // too long: the caller refuses it
    }
    return !line->empty();
  }
  bool failed() const { return file_ && std::ferror(file_); }

 private:
  FILE* file_;
  std::unique_ptr<char[]> buffer_;
};

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

std::string Md5OrDash(const std::string& hash) { return hash.empty() ? "-" : hash; }

// ---- The plan -----------------------------------------------------------------------------------------------------

struct PlanHeader {
  std::string build_id;
  std::string meta_id;
  std::string language;
  std::vector<std::uint64_t> products;
  std::uint64_t total_size = 0;
  std::uint64_t total_compressed = 0;
};

struct PlanEntry {
  bool directory = false;
  std::string path;
  std::string source;
  std::uint64_t size = 0;
  std::string md5;
  std::string sha256;
  std::vector<DepotChunk> chunks;
  std::uint64_t compressed() const {
    std::uint64_t total = 0;
    for (const auto& chunk : chunks) total += chunk.compressed_size;
    return total;
  }
};

std::string HeaderLine(const PlanHeader& header) {
  std::string line = "v1\t" + header.build_id + "\t" + header.meta_id + "\t" + header.language + "\t";
  for (std::size_t i = 0; i < header.products.size(); ++i) {
    if (i) line += ',';
    line += std::to_string(header.products[i]);
  }
  line += "\t" + std::to_string(header.total_size) + "\t" + std::to_string(header.total_compressed);
  return line;
}

bool ParseHeader(const std::string& line, PlanHeader* header) {
  const auto fields = Split(line, '\t');
  if (fields.size() != 7 || fields[0] != "v1") return false;
  std::uint64_t number = 0;
  if (!CanonicalId(fields[1], &number) || !IsMd5Hex(fields[2]) || !PlainLanguageCode(fields[3])) return false;
  header->build_id = std::string(fields[1]);
  header->meta_id = std::string(fields[2]);
  header->language = std::string(fields[3]);
  header->products.clear();
  for (const auto product : Split(fields[4], ',')) {
    if (!CanonicalId(product, &number)) return false;
    header->products.push_back(number);
  }
  return ParseU64(fields[5], &header->total_size) && ParseU64(fields[6], &header->total_compressed);
}

bool ValidSource(std::string_view source) {
  std::uint64_t id = 0;
  if (CanonicalId(source, &id)) return true;
  const std::string_view prefix = kDependencySourcePrefix;
  return source.size() > prefix.size() && source.substr(0, prefix.size()) == prefix &&
         ValidDependencyId(source.substr(prefix.size()));
}

std::string EntryLine(const PlanEntry& entry) {
  if (entry.directory) return "D\t" + entry.path;
  return "F\t" + entry.path + "\t" + entry.source + "\t" + std::to_string(entry.size) + "\t" + Md5OrDash(entry.md5) +
         "\t" + Md5OrDash(entry.sha256) + "\t" + std::to_string(entry.chunks.size());
}

std::string ChunkLine(const DepotChunk& chunk) {
  return "C\t" + chunk.compressed_md5 + "\t" + std::to_string(chunk.compressed_size) + "\t" + chunk.md5 + "\t" +
         std::to_string(chunk.size);
}

// Reads the entries of a plan (or of items.tmp, which has the same entry lines).
class EntryReader {
 public:
  explicit EntryReader(const std::string& path) : lines_(path) {}
  bool open() const { return lines_.open(); }
  bool Header(PlanHeader* header) {
    std::string line;
    bool complete = false;
    return lines_.Next(&line, &complete) && complete && ParseHeader(line, header);
  }
  enum class Next { kEntry, kEnd, kBad };
  Next Read(PlanEntry* entry) {
    std::string line;
    bool complete = false;
    if (!lines_.Next(&line, &complete)) return lines_.failed() ? Next::kBad : Next::kEnd;
    if (!complete) return Next::kBad;
    const auto fields = Split(line, '\t');
    *entry = PlanEntry();
    if (fields.size() == 2 && fields[0] == "D") {
      if (!ValidPlanPath(fields[1])) return Next::kBad;
      entry->directory = true;
      entry->path = std::string(fields[1]);
      return Next::kEntry;
    }
    if (fields.size() != 7 || fields[0] != "F" || !ValidPlanPath(fields[1]) || !ValidSource(fields[2]))
      return Next::kBad;
    entry->path = std::string(fields[1]);
    entry->source = std::string(fields[2]);
    std::uint64_t count = 0;
    if (!ParseU64(fields[3], &entry->size) || !ParseU64(fields[6], &count) || count > kMaxChunksPerFile)
      return Next::kBad;
    if (fields[4] != "-") {
      if (!IsMd5Hex(fields[4])) return Next::kBad;
      entry->md5 = std::string(fields[4]);
    }
    if (fields[5] != "-") {
      if (!IsSha256Hex(fields[5])) return Next::kBad;
      entry->sha256 = std::string(fields[5]);
    }
    std::uint64_t sum = 0;
    for (std::uint64_t i = 0; i < count; ++i) {
      if (!lines_.Next(&line, &complete) || !complete) return Next::kBad;
      const auto parts = Split(line, '\t');
      DepotChunk chunk;
      if (parts.size() != 5 || parts[0] != "C" || !IsMd5Hex(parts[1]) || !IsMd5Hex(parts[3]) ||
          !ParseU64(parts[2], &chunk.compressed_size) || !ParseU64(parts[4], &chunk.size) ||
          chunk.compressed_size == 0 || chunk.compressed_size > kMaxChunkBytes || chunk.size > kMaxChunkBytes)
        return Next::kBad;
      chunk.compressed_md5 = std::string(parts[1]);
      chunk.md5 = std::string(parts[3]);
      sum += chunk.size;
      entry->chunks.push_back(std::move(chunk));
    }
    if (sum != entry->size) return Next::kBad;
    return Next::kEntry;
  }

 private:
  LineReader lines_;
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

// Saves the zlib file GOG names by its MD5 at `dest`, from the first source that serves it whole. A file already
// there with that MD5 is kept. kChunkAttempts in all, rotating through the sources.
FetchKind FetchToFile(Transport& transport, const std::vector<Source>& sources, const std::string& md5,
                      const std::string& dest, const ChunkPause& pause, const std::atomic<bool>& cancel,
                      const Logger& log, const char* what) {
  std::string existing;
  if (Exists(dest) && FileMd5(dest, &existing) && existing == md5) return FetchKind::kOk;
  if (sources.empty()) return FetchKind::kNetwork;
  const auto temporary = dest + ".tmp";
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
    request.max_response_bytes = kMaxCompressedMetaBytes;
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
      if (hash.FinalHex() == md5) {
        if (std::rename(temporary.c_str(), dest.c_str()) != 0) return FetchKind::kStorage;
        return FetchKind::kOk;
      }
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
  Planner(const Hooks& hooks, const GogInstaller::Target& target, const ChunkPause& pause,
          const std::atomic<bool>& cancel, const Logger& log)
      : hooks_(hooks), target_(target), pause_(pause), cancel_(cancel), log_(log) {}

  Error Plan() {
    transport_ = hooks_.make_transport ? hooks_.make_transport() : Transport();
    if (!transport_) return Error::kNetwork;
    for (const char* folder : {kMetaDir, kManifestDir})
      if (!MakeDirectory(Join(target_.root, folder))) return Error::kStorage;

    BuildChoice choice;
    if (const auto error = Builds(&choice); error != Error::kNone) return error;
    Meta meta;
    if (const auto error = LoadMeta(choice, &meta); error != Error::kNone) return error;
    std::set<std::uint64_t> owned;
    if (const auto error = Owned(&owned); error != Error::kNone) return error;
    if (!owned.count(target_.product)) {
      log_("gog plan owned result=not_owned");
      return Error::kNotOwned;
    }
    const auto selection = SelectDepots(meta, owned, target_.language);
    for (const auto& endpoint : choice.meta) {
      const auto url = FillUrl(endpoint, "");
      if (!url) continue;
      if (auto base = CdnBase(*url)) cdns_.push_back({EndpointLogName(endpoint.name), std::move(*base)});
    }
    if (cdns_.empty()) return Error::kProtocol;
    log_("gog plan build generation=2 depots=%zu products=%zu size=%llu", selection.depots.size(),
         selection.products.size(), static_cast<unsigned long long>(selection.size));

    const auto items_path = Join(target_.root, kItemsPath);
    items_ = std::fopen(items_path.c_str(), "wb");
    if (!items_) return WriteFailure();
    Error error = Error::kNone;
    for (const auto& depot : selection.depots) {
      error = Depot("/content-system/v2/meta/", depot.manifest, depot.product, std::to_string(depot.product));
      if (error != Error::kNone) break;
    }
    if (error == Error::kNone && !meta.dependencies.empty()) error = Dependencies(meta);
    if (std::fclose(items_) != 0 && error == Error::kNone) error = WriteFailure();
    items_ = nullptr;
    if (error == Error::kNone) error = items_error_;
    if (error == Error::kNone) {
      log_("gog plan entries=%zu links_skipped=%zu clashes=%zu refused=%zu other=%zu", next_, links_, clashes_,
           refused_, other_);
      PlanHeader header;
      header.build_id = choice.build_id;
      header.meta_id = choice.meta_id;
      header.language = selection.language.empty() ? "en-US" : selection.language;
      header.products = selection.products;
      error = WritePlan(&header);
    }
    std::remove(items_path.c_str());
    return error;
  }

 private:
  Error Builds(BuildChoice* choice) {
    const auto answer = FetchBuilds(transport_, target_.product, cancel_);
    if (!answer.answered) return answer.canceled || cancel_.load() ? Error::kCanceled : Error::kNetwork;
    log_("gog plan builds status=%ld", answer.status);
    if (answer.status == 404) return Error::kNoBuild;
    if (answer.status == 429 || answer.status >= 500) return Error::kNetwork;
    if (answer.status != 200) return Error::kProtocol;
    *choice = ChooseBuild(answer.body);
    switch (choice->kind) {
      case BuildChoice::Kind::kGen2: return Error::kNone;
      case BuildChoice::Kind::kGen1Only: log_("gog plan build generation=1"); return Error::kGen1Only;
      case BuildChoice::Kind::kNone: log_("gog plan build none"); return Error::kNoBuild;
      case BuildChoice::Kind::kProtocol: return Error::kProtocol;
    }
    return Error::kProtocol;
  }

  Error LoadMeta(const BuildChoice& choice, Meta* meta) {
    std::vector<Source> sources;
    for (const auto& endpoint : choice.meta)
      if (auto url = FillUrl(endpoint, "")) sources.push_back({EndpointLogName(endpoint.name), std::move(*url)});
    const auto path = Join(target_.root, std::string(kMetaDir) + "/" + choice.meta_id);
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

  // One depot manifest: saved, then read item by item into the path set and items.tmp.
  Error Depot(const std::string& folder, const std::string& hash, std::uint64_t product, const std::string& source) {
    const auto path = Join(target_.root, std::string(kManifestDir) + "/" + hash);
    const auto fetched = FetchToFile(transport_, Sources(folder, hash), hash, path, pause_, cancel_, log_, "manifest");
    if (fetched != FetchKind::kOk) return FromFetch(fetched);
    DepotItemReader reader([&](const DepotItem& item) { return Item(item, product, source); });
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

  bool Item(const DepotItem& item, std::uint64_t product, const std::string& source) {
    PathKind kind = PathKind::kFile;
    switch (item.type) {
      case DepotItem::Type::kFile: kind = PathKind::kFile; break;
      case DepotItem::Type::kDirectory: kind = PathKind::kDirectory; break;
      case DepotItem::Type::kLink: kind = PathKind::kLink; break;
      case DepotItem::Type::kOther: ++other_; return true;
    }
    const auto added = paths_.Add(item.path, kind, product, item.support, next_);
    if (!added.ok()) {
      if (added.refusal == PathRefusal::kLink) ++links_;
      else if (added.refusal == PathRefusal::kClash) ++clashes_;
      else ++refused_;
      return true;
    }
    if (added.replaced && *added.replaced < dropped_.size()) dropped_[*added.replaced] = true;
    dropped_.push_back(false);
    ++next_;
    PlanEntry entry;
    entry.directory = kind == PathKind::kDirectory;
    entry.path = added.path;
    entry.source = source;
    entry.size = item.size;
    entry.md5 = item.md5;
    entry.sha256 = item.sha256;
    entry.chunks = item.chunks;
    std::string text = EntryLine(entry);
    text += '\n';
    for (const auto& chunk : entry.chunks) text += ChunkLine(chunk) + '\n';
    if (std::fwrite(text.data(), 1, text.size(), items_) != text.size()) {
      items_error_ = WriteFailure();
      return false;
    }
    return true;
  }

  Error Dependencies(const Meta& meta) {
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
    const auto path = Join(target_.root, std::string(kManifestDir) + "/" + id);
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
    for (const auto& wanted : meta.dependencies) {
      const auto found = std::find_if(dependencies->begin(), dependencies->end(),
                                      [&wanted](const Dependency& dependency) { return dependency.id == wanted; });
      if (found == dependencies->end()) {
        ++missing;
        continue;
      }
      if (!GameFolderDependency(*found)) {
        ++redist;  // __redist installers wait for Phase 4
        continue;
      }
      ++game_folder;
      const auto error = Depot("/content-system/v2/dependencies/meta/", found->manifest, target_.product,
                               kDependencySourcePrefix + found->id);
      if (error != Error::kNone) return error;
    }
    log_("gog plan dependencies game_folder=%zu redist_skipped=%zu missing=%zu", game_folder, redist, missing);
    return Error::kNone;
  }

  // items.tmp, without the entries a later depot replaced, becomes the plan and .playden-files.
  Error WritePlan(PlanHeader* header) {
    const auto items_path = Join(target_.root, kItemsPath);
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
        header->total_compressed += entry.compressed();
      }
    }
    // The old journal's indexes belong to another plan.
    if (std::remove(Join(target_.root, kJournalPath).c_str()) != 0 && errno != ENOENT) return Error::kStorage;
    DurableFile plan(Join(target_.root, kPlanPath));
    DurableFile list(Join(target_.root, kFileListName));
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
      for (const auto& chunk : entry.chunks) plan.Line(ChunkLine(chunk));
      list.Line((entry.directory ? "d\t" : "f\t") + entry.path);
    }
    // The file list first: once the plan exists, everything it may place is listed.
    if (const auto error = list.Commit(); error != Error::kNone) return error;
    if (const auto error = plan.Commit(); error != Error::kNone) return error;
    log_("gog plan saved files=%zu size=%llu compressed=%llu", files,
         static_cast<unsigned long long>(header->total_size),
         static_cast<unsigned long long>(header->total_compressed));
    return Error::kNone;
  }

  const Hooks& hooks_;
  const GogInstaller::Target& target_;
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
    const auto fields = Split(line, '\t');
    std::uint64_t file = 0, chunk = 0;
    if (fields.size() == 2 && fields[0] == "f" && ParseU64(fields[1], &file) && file < (1ull << 39))
      state.files.insert(file);
    else if (fields.size() == 3 && fields[0] == "c" && ParseU64(fields[1], &file) && file < (1ull << 39) &&
             ParseU64(fields[2], &chunk) && chunk < kMaxChunksPerFile)
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

// The chunk bases of each source: a secure link per product, one open link for the dependency store. One fetch at a
// time per source; a worker that saw an old version waits for the fetch in progress instead of starting another.
class Links {
 public:
  Links(const Hooks& hooks, const Logger& log, const std::atomic<bool>& cancel) : hooks_(hooks), log_(log), cancel_(cancel) {}

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
      *out = FetchSecureLink(transport, token.token, product, cancel_);
      log_("gog secure_link status=%ld result=%s", out->status, LinkName(out->kind));
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
  std::vector<DepotChunk> chunks;
  std::vector<std::uint64_t> offsets;
  std::vector<bool> done;
  std::size_t remaining = 0;
  bool redone = false;
};

struct Task {
  std::shared_ptr<FileWork> file;
  std::size_t chunk = 0;
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
    for (std::size_t chunk = 0; chunk < file->chunks.size(); ++chunk)
      if (!file->done[chunk]) queue_.push_back({file, chunk});
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

  // A file whose chunks are all written: its size set and its whole-file hashes checked. On a mismatch it is
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
    if (error == Error::kNone && ((!file->md5.empty() && file->chunks.size() > 1) || !file->sha256.empty())) {
      Md5 md5;
      Sha256 sha;
      const bool want_md5 = !file->md5.empty() && file->chunks.size() > 1;
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
      if (file->chunks.empty()) {
        // An empty file the manifest gives another hash: nothing to fetch again.
        log_("gog file check failed chunks=0");
        lock.unlock();
        Stop(Error::kCorrupt);
        return;
      }
      file->redone = true;
      ++stats_.redone_files;
      log_("gog file check failed chunks=%zu: fetching it again", file->chunks.size());
      std::fill(file->done.begin(), file->done.end(), false);
      file->remaining = file->chunks.size();
      for (std::size_t chunk = 0; chunk < file->chunks.size(); ++chunk) {
        done_ -= std::min(done_, file->chunks[chunk].compressed_size);
        queue_.push_back({file, chunk});
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
        if (!file.done[task.chunk]) {
          file.done[task.chunk] = true;
          --file.remaining;
          done_ += file.chunks[task.chunk].compressed_size;
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

  // One chunk, with fresh links when the CDN refuses the one in use.
  Error Fetch(Transport& transport, const Task& task) {
    const auto& file = *task.file;
    const auto& chunk = file.chunks[task.chunk];
    std::optional<std::uint64_t> stale;
    for (int refresh = 0;; ++refresh) {
      Endpoints endpoints;
      std::uint64_t version = 0;
      if (const auto error = links_.Get(transport, file.source, stale, &endpoints, &version); error != Error::kNone)
        return error;
      ChunkFile out(file.path);
      const auto result = FetchChunk(
          transport, *endpoints, chunk, file.offsets[task.chunk],
          [&out](std::uint64_t offset, const std::uint8_t* data, std::size_t size) { return out.Write(offset, data, size); },
          pause_, cancel_);
      endpoints.reset();
      const int close_error = out.Close();
      Account(result);
      if (result.kind == ChunkResult::Kind::kExpired) {
        log_("gog chunk refused status=%ld endpoint=%s: fetching the link again", result.status,
             result.endpoint.c_str());
        if (refresh >= kMaxLinkRefreshesPerChunk) return Error::kNetwork;
        stale = version;
        continue;
      }
      if (result.kind != ChunkResult::Kind::kOk) {
        log_("gog chunk failed result=%s attempts=%d status=%ld endpoint=%s", ChunkResultName(result.kind),
             result.attempts, result.status, result.endpoint.c_str());
        return FromChunk(result.kind);
      }
      if (close_error != 0) return close_error == ENOSPC ? Error::kNoSpace : Error::kStorage;
      if (result.attempts > 1)
        log_("gog chunk ok attempts=%d corrupt=%d stalls=%d endpoint=%s", result.attempts, result.corrupt,
             result.stalls, result.endpoint.c_str());
      if (hooks_.endpoint) hooks_.endpoint(result.endpoint.c_str());
      return journal_.Chunk(file.index, task.chunk);
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
    case Error::kGen1Only: return "gen1_only";
    case Error::kNoBuild: return "no_build";
    case Error::kNotOwned: return "not_owned";
  }
  return "protocol";
}

GogInstaller::GogInstaller(Hooks hooks) : hooks_(std::move(hooks)) {}

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
  const auto plan_path = Join(target.root, kPlanPath);
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
          Planner planner(hooks_, target, pause, cancel, log);
          error = planner.Plan();
        }))
      return finish(Error::kNetwork);
    if (error != Error::kNone) return finish(error);
    EntryReader reader(plan_path);
    if (!reader.open() || !reader.Header(&header)) return finish(Error::kStorage);
  }
  outcome.build_id = header.build_id;
  outcome.meta_id = header.meta_id;
  outcome.language = header.language;
  outcome.products = header.products;
  outcome.install_bytes = header.total_size;

  // 5-6. What the journal already has, and the space the rest needs.
  const auto journal_path = Join(target.root, kJournalPath);
  auto state = LoadJournal(journal_path);
  std::uint64_t held = 0, done = 0, missing = 0, pending_chunks = 0, unfinished = 0;
  std::set<std::string> sources;
  std::unordered_set<std::uint64_t> finished;
  {
    EntryReader reader(plan_path);
    PlanHeader again;
    if (!reader.open() || !reader.Header(&again)) return finish(Error::kStorage);
    PlanEntry entry;
    for (std::uint64_t index = 0;; ++index) {
      const auto next = reader.Read(&entry);
      if (next == EntryReader::Next::kEnd) break;
      if (next == EntryReader::Next::kBad) {
        // A damaged plan is made again on the next run, with a new journal.
        std::remove(plan_path.c_str());
        log("gog plan damaged");
        return finish(Error::kStorage);
      }
      if (entry.directory) continue;
      ++outcome.files;
      const auto path = Join(target.root, entry.path);
      struct stat info {};
      const bool present = lstat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode);
      const bool marked = state.files.count(index) > 0;
      if (marked && present && static_cast<std::uint64_t>(info.st_size) == entry.size) {
        finished.insert(index);
        held += entry.size;
        done += entry.compressed();
        continue;
      }
      ++unfinished;
      // A finished file that is gone or has another size is fetched whole again; so is a file in progress that is
      // gone.
      const bool redo = marked || !present;
      for (std::size_t chunk = 0; chunk < entry.chunks.size(); ++chunk) {
        const auto key = ChunkKey(index, chunk);
        if (!redo && state.chunks.count(key)) {
          held += entry.chunks[chunk].size;
          done += entry.chunks[chunk].compressed_size;
        } else {
          state.chunks.erase(key);
          missing += entry.chunks[chunk].size;
          ++pending_chunks;
          sources.insert(Links::Key(entry.source));
        }
      }
    }
  }
  const auto free = hooks_.free_bytes(target.root);
  log("gog download files=%zu finished=%zu chunks_left=%llu", outcome.files, finished.size(),
      static_cast<unsigned long long>(pending_chunks));
  if (missing > UINT64_MAX - kGogSpaceMargin || missing + kGogSpaceMargin > free) {
    log("gog download space short need=%llu free=%llu", static_cast<unsigned long long>(missing),
        static_cast<unsigned long long>(free));
    return finish(Error::kNoSpace);
  }
  const Progress start{done, header.total_compressed, held};
  if (progress) progress(start);

  Journal journal;
  if (const auto error = journal.Open(journal_path); error != Error::kNone) return finish(error);
  Links links(hooks_, log, cancel);

  // 8. The links each source needs, before any chunk.
  const int parallel = std::max(1, std::min(kMaxParallel, hooks_.parallel));
  Download download(hooks_, pause, cancel, log, journal, links, outcome.stats);
  download.AddDone(done);
  if (pending_chunks > 0) {
    Error error = Error::kNone;
    if (!RunOnStack(kWorkerStackBytes, [&] {
          Transport transport = hooks_.make_transport ? hooks_.make_transport() : Transport();
          if (!transport) {
            error = Error::kNetwork;
            return;
          }
          for (const auto& source : sources) {
            Endpoints endpoints;
            std::uint64_t version = 0;
            error = links.Get(transport, source, std::nullopt, &endpoints, &version);
            if (error != Error::kNone) return;
          }
        }))
      error = Error::kNetwork;
    outcome.stats.link_fetches = links.fetches();
    if (error != Error::kNone) return finish(error);
  }
  // Workers also for a file whose chunks are all journaled: a failed whole-file check fetches it again.
  if (unfinished > 0) {
    if (!download.Start(parallel)) return finish(Error::kNetwork);
    log("gog download start parallel=%d", parallel);
  }

  // 7. Feed the workers in plan order.
  auto last_sync = std::chrono::steady_clock::now();
  Error journal_error = Error::kNone;
  const auto tick = [&] {
    if (cancel.load()) download.Stop(Error::kCanceled);
    const auto now = std::chrono::steady_clock::now();
    if (now - last_sync >= std::chrono::seconds(2)) {
      last_sync = now;
      if (const auto error = journal.Sync(); error != Error::kNone && journal_error == Error::kNone) {
        journal_error = error;
        download.Stop(error);
      }
    }
    if (progress) progress({std::min(download.done(), header.total_compressed), header.total_compressed, held});
  };
  {
    EntryReader reader(plan_path);
    PlanHeader again;
    bool readable = reader.open() && reader.Header(&again);
    PlanEntry entry;
    for (std::uint64_t index = 0; readable && !download.stopped(); ++index) {
      if (cancel.load()) {
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
        if (!MakeFolders(target.root, entry.path, true)) download.Stop(WriteFailure());
        continue;
      }
      if (finished.count(index)) continue;
      if (!MakeFolders(target.root, entry.path, false)) {
        download.Stop(WriteFailure());
        break;
      }
      auto file = std::make_shared<FileWork>();
      file->index = index;
      file->path = Join(target.root, entry.path);
      struct stat info {};
      if (lstat(file->path.c_str(), &info) == 0 && !S_ISREG(info.st_mode)) {
        log("gog download a folder or link is in a file's place");
        download.Stop(Error::kStorage);
        break;
      }
      file->source = entry.source;
      file->size = entry.size;
      file->md5 = entry.md5;
      file->sha256 = entry.sha256;
      file->chunks = std::move(entry.chunks);
      file->done.assign(file->chunks.size(), false);
      std::uint64_t offset = 0;
      for (std::size_t chunk = 0; chunk < file->chunks.size(); ++chunk) {
        file->offsets.push_back(offset);
        offset += file->chunks[chunk].size;
        if (state.chunks.count(ChunkKey(index, chunk))) file->done[chunk] = true;
        else ++file->remaining;
      }
      if (file->remaining == 0) {
        // No chunks (an empty file), or every chunk already written: only its size and check are left.
        download.Finish(file, false);
        continue;
      }
      if (!download.Admit(file, tick)) break;
    }
    if (!readable) download.Stop(Error::kStorage);
  }
  download.Drain(tick);
  download.Join();
  outcome.stats.link_fetches = links.fetches();
  outcome.stats.parallel = unfinished > 0 ? download.active_limit() : 0;
  const auto closed = journal.Close();
  if (progress) progress({std::min(download.done(), header.total_compressed), header.total_compressed, held});
  Error error = download.error();
  if (error == Error::kNone && cancel.load()) error = Error::kCanceled;
  if (error == Error::kNone) error = closed;
  if (error == Error::kNone)
    log("gog download done files=%zu size=%llu", outcome.files, static_cast<unsigned long long>(header.total_size));
  return finish(error);
}

}  // namespace playden::gog
