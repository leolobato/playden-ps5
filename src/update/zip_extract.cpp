#include "update/zip_extract.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <memory>
#include <set>
#include <vector>

#include "miniz.h"
#include "update/file_ops.h"

namespace playden::update {
namespace {

constexpr std::size_t kWriteBuffer = 1 << 20;
constexpr unsigned kTypeMask = 0170000, kRegular = 0100000, kFolder = 0040000;

struct Sink {
  int fd = -1;
  std::vector<std::uint8_t> buffer;
  std::size_t used = 0;
  bool failed = false;
  const std::atomic<bool>* cancel = nullptr;
};

bool Flush(Sink& sink) {
  std::size_t written = 0;
  while (written < sink.used) {
    const ssize_t n = write(sink.fd, sink.buffer.data() + written, sink.used - written);
    if (n <= 0) { sink.failed = true; return false; }
    written += static_cast<std::size_t>(n);
  }
  sink.used = 0;
  return true;
}

size_t Collect(void* opaque, mz_uint64, const void* data, size_t size) {
  auto& sink = *static_cast<Sink*>(opaque);
  if (sink.cancel && sink.cancel->load()) return 0;
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  std::size_t left = size;
  while (left) {
    const std::size_t take = std::min(left, sink.buffer.size() - sink.used);
    std::memcpy(sink.buffer.data() + sink.used, bytes, take);
    sink.used += take;
    bytes += take;
    left -= take;
    if (sink.used == sink.buffer.size() && !Flush(sink)) return 0;
  }
  return size;
}

struct Archive {
  mz_zip_archive zip{};
  bool open = false;
  ~Archive() { if (open) mz_zip_reader_end(&zip); }
};

std::string ZipError(mz_zip_archive& zip) { return mz_zip_get_error_string(mz_zip_get_last_error(&zip)); }

}  // namespace

bool SafeEntryName(const std::string& name) {
  if (name.empty() || name.front() == '/') return false;
  std::size_t start = 0;
  while (start <= name.size()) {
    const auto slash = name.find('/', start);
    const auto end = slash == std::string::npos ? name.size() : slash;
    const std::string part = name.substr(start, end - start);
    // A trailing "/" (folder entry) leaves one empty last part, which is fine.
    if (part.empty() && end != name.size()) return false;
    if (part == "." || part == "..") return false;
    if (slash == std::string::npos) break;
    start = slash + 1;
  }
  for (unsigned char c : name)
    if (c < 0x20 || c == 0x7f || c == '\\' || c == ':') return false;
  return true;
}

std::string FindInnerPrefix(const std::string& zip_path, const std::string& marker) {
  Archive archive;
  if (!mz_zip_reader_init_file(&archive.zip, zip_path.c_str(), 0)) return {};
  archive.open = true;
  const std::string tail = "/" + marker;
  const mz_uint count = mz_zip_reader_get_num_files(&archive.zip);
  for (mz_uint i = 0; i < count; ++i) {
    char name[1024];
    const mz_uint length = mz_zip_reader_get_filename(&archive.zip, i, name, sizeof name);
    if (!length || length >= sizeof name) continue;
    const std::string entry(name);
    if (entry.size() <= tail.size() || entry.compare(entry.size() - tail.size(), tail.size(), tail) != 0) continue;
    const std::string prefix = entry.substr(0, entry.size() - marker.size());
    // Exactly two folders: "<top>/<inner>/".
    if (std::count(prefix.begin(), prefix.end(), '/') == 2 && SafeEntryName(prefix)) return prefix;
  }
  return {};
}

ZipExtractResult ExtractZip(const std::string& zip_path, const std::string& dest, const ZipExtractOptions& options) {
  ZipExtractResult result;
  auto fail = [&](std::string reason) {
    result.error = std::move(reason);
    if (options.log) options.log("unzip failed: " + result.error);
    return result;
  };
  auto cancelled = [&] { return options.cancel && options.cancel->load(); };
  if (!options.inner_prefix.empty() && (options.inner_prefix.back() != '/' || !SafeEntryName(options.inner_prefix)))
    return fail("bad inner folder");
  if (!IsFolder(dest) || !ListFolder(dest).empty()) return fail("destination is not an empty folder");

  Archive archive;
  if (!mz_zip_reader_init_file(&archive.zip, zip_path.c_str(), 0)) return fail("not a zip: " + ZipError(archive.zip));
  archive.open = true;
  const mz_uint count = mz_zip_reader_get_num_files(&archive.zip);
  if (count > options.max_entries) return fail("too many entries");

  // First pass: check every entry under the prefix before anything is written.
  struct Entry {
    mz_uint index;
    std::string relative;
    bool folder;
    unsigned mode;
    std::uint64_t size;
  };
  std::vector<Entry> entries;
  std::set<std::string> seen;
  std::uint64_t total = 0;
  for (mz_uint i = 0; i < count; ++i) {
    mz_zip_archive_file_stat stat{};
    if (!mz_zip_reader_file_stat(&archive.zip, i, &stat)) return fail("unreadable entry " + std::to_string(i));
    const std::string name = stat.m_filename;
    if (!SafeEntryName(name)) return fail("unsafe entry name: " + name);
    if (name.compare(0, options.inner_prefix.size(), options.inner_prefix) != 0) continue;
    std::string relative = name.substr(options.inner_prefix.size());
    if (relative.empty()) continue;
    const bool folder = stat.m_is_directory || relative.back() == '/';
    if (folder) relative.pop_back();
    if (!seen.insert(relative).second) return fail("duplicate entry: " + relative);
    if (stat.m_is_encrypted || (stat.m_bit_flag & 1)) return fail("encrypted entry: " + relative);
    if (!stat.m_is_supported) return fail("unsupported compression: " + relative);
    // Unix modes live in the high half of the external attributes (made by unix, version 3).
    const unsigned unix_mode = (stat.m_version_made_by >> 8) == 3 ? (stat.m_external_attr >> 16) & 0xffff : 0;
    const unsigned type = unix_mode & kTypeMask;
    if (type && type != (folder ? kFolder : kRegular)) return fail("not a regular file or folder: " + relative);
    unsigned mode = 0777;
    if (!folder) {
      const unsigned zip_mode = unix_mode & 07777 ? unix_mode & 07777 : 0644;
      if (options.mode_for) {
        const auto chosen = options.mode_for(relative, zip_mode);
        if (!chosen) return fail("file not expected here: " + relative);
        mode = *chosen & 07777;
      } else {
        mode = zip_mode;
      }
      total += stat.m_uncomp_size;
      if (total > options.max_total_bytes) return fail("too large when unpacked");
    }
    entries.push_back({i, relative, folder, mode, stat.m_uncomp_size});
  }
  // A file's folders are created even when the zip has no folder entries for them.
  std::set<std::string> made;
  const auto make_folder = [&](const std::string& relative) {
    for (std::size_t i = 0; i <= relative.size(); ++i) {
      if (i != relative.size() && relative[i] != '/') continue;
      const std::string part = relative.substr(0, i);
      if (!made.insert(part).second) continue;
      const std::string path = dest + "/" + part;
      if (mkdir(path.c_str(), 0777) != 0 && errno != EEXIST) return false;
      if (!IsFolder(path)) return false;
      if (!options.folder_mode) continue;
      const unsigned want = *options.folder_mode & 07777;
      struct stat st{};
      if (stat(path.c_str(), &st) != 0) return false;
      if ((st.st_mode & 07777) == want) continue;
      const int chmod_rc = options.set_folder_mode ? options.set_folder_mode(path, want) : chmod(path.c_str(), want);
      // Created while another thread held root, like the files below.
      if (chmod_rc != 0 && !(errno == EPERM && options.fix_mode && options.fix_mode(path, want))) {
        if (options.log) options.log("unzip: cannot set the mode of folder " + part + " errno=" + std::to_string(errno));
        return false;
      }
      if (stat(path.c_str(), &st) != 0 || (st.st_mode & 07777) != want) return false;
    }
    return true;
  };
  Sink sink;
  sink.buffer.resize(kWriteBuffer);
  sink.cancel = options.cancel;
  for (const auto& entry : entries) {
    if (cancelled()) { result.cancelled = true; return fail("cancelled"); }
    const std::string path = dest + "/" + entry.relative;
    if (entry.folder) {
      if (!make_folder(entry.relative)) return fail("cannot create folder " + entry.relative);
      ++result.folders;
      continue;
    }
    const auto slash = entry.relative.rfind('/');
    if (slash != std::string::npos && !make_folder(entry.relative.substr(0, slash)))
      return fail("cannot create folder for " + entry.relative);
    bool written = false;
    for (int attempt = 0; attempt < 3 && !written; ++attempt) {
      // A new file each time: one created with someone else's owner can't take our mode.
      if (attempt > 0) unlink(path.c_str());
      sink.fd = open(path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0666);
      if (sink.fd < 0) return fail("cannot create " + entry.relative + " errno=" + std::to_string(errno));
      sink.used = 0;
      sink.failed = false;
      const bool extracted = mz_zip_reader_extract_to_callback(&archive.zip, entry.index, Collect, &sink, 0) &&
                             Flush(sink);
      if (!extracted) {
        const std::string reason = sink.failed ? "write failed errno=" + std::to_string(errno) : ZipError(archive.zip);
        close(sink.fd);
        if (cancelled()) { result.cancelled = true; return fail("cancelled"); }
        return fail("cannot unpack " + entry.relative + ": " + reason);
      }
      const int chmod_rc = options.set_mode ? options.set_mode(sink.fd, entry.mode) : fchmod(sink.fd, entry.mode);
      const int chmod_errno = chmod_rc ? errno : 0;
      struct stat st{};
      int stat_rc = fstat(sink.fd, &st);
      close(sink.fd);
      const auto good = [&] {
        return stat_rc == 0 && static_cast<std::uint64_t>(st.st_size) == entry.size && (st.st_mode & 07777) == entry.mode;
      };
      written = chmod_rc == 0 && good();
      // Created while another thread held root: the bytes are right, only root can set the mode.
      if (!written && chmod_rc != 0 && chmod_errno == EPERM && stat_rc == 0 &&
          static_cast<std::uint64_t>(st.st_size) == entry.size && options.fix_mode &&
          options.fix_mode(path, entry.mode)) {
        stat_rc = stat(path.c_str(), &st);
        written = good();
        if (written) ++result.mode_fixed;
        continue;
      }
      if (!written && options.log) {
        char line[512];
        std::snprintf(line, sizeof line,
                      "unzip check failed (attempt %d): %s chmod=%d errno=%d uid=%u size=%lld want=%llu mode=%o want=%o",
                      attempt + 1, entry.relative.c_str(), chmod_rc, chmod_errno, (unsigned)st.st_uid,
                      (long long)st.st_size, (unsigned long long)entry.size, (unsigned)(st.st_mode & 07777),
                      entry.mode);
        options.log(line);
      }
      if (!written && attempt < 2) ++result.rewritten;
    }
    if (!written) return fail("file did not keep its size or mode: " + entry.relative);
    ++result.files;
    result.bytes += entry.size;
    if (options.progress) options.progress(result.bytes, total);
  }
  result.ok = true;
  return result;
}

}  // namespace playden::update
