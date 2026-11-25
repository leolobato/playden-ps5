#include "runtime/publisher_runtime.h"

#include "7z.h"
#include "7zCrc.h"
#include "Bcj2.h"
#include "Bra.h"
#include "LzmaDec.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <memory>
#include <mutex>
#include <set>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace playden::runtime {
namespace {

namespace fs = std::filesystem;

constexpr unsigned char kSignature[6] = {'7', 'z', 0xBC, 0xAF, 0x27, 0x1C};
constexpr std::uint64_t kMaxScanBytes = 512ull << 20;
constexpr int kMaxSignatureHits = 32;
// Limits against a damaged installer: one header allocation, one LZMA dictionary, and the
// version file, which is decoded to memory.
constexpr std::size_t kMaxHeaderAllocation = 16u << 20;
constexpr std::uint64_t kMaxDictionary = 64u << 20;
constexpr std::uint64_t kMaxVersionFileBytes = 64u << 20;
constexpr std::size_t kMainInputBytes = 1u << 20;
constexpr std::size_t kSideInputBytes = 64u << 10;
constexpr std::size_t kSliceBytes = 1u << 20;  // most bytes per decode step (progress, cancel)
constexpr std::uint32_t kMethodLzma = 0x030101;
constexpr std::uint32_t kMethodBcj2 = 0x0303011B;
constexpr std::uint32_t kMethodBcjX86 = 0x03030103;
constexpr std::uint32_t kAttributeReparsePoint = 0x400;
constexpr std::uint32_t kAttributeUnixExtension = 0x8000;
constexpr const char* kStagingPrefix = ".staging-";

void* CappedAlloc(ISzAllocPtr, std::size_t size) { return size > kMaxHeaderAllocation ? nullptr : std::malloc(size); }
void* PlainAlloc(ISzAllocPtr, std::size_t size) { return std::malloc(size); }
void Free(ISzAllocPtr, void* address) { std::free(address); }
const ISzAlloc kHeaderAlloc = {CappedAlloc, Free};
const ISzAlloc kAlloc = {PlainAlloc, Free};

void InitCrc() {
  static std::once_flag once;
  std::call_once(once, [] { CrcGenerateTable(); });
}

std::string Lower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c); });
  return text;
}

bool ReadAt(int fd, std::uint64_t offset, void* buffer, std::size_t size) {
  auto* out = static_cast<unsigned char*>(buffer);
  while (size > 0) {
    const auto count = ::pread(fd, out, size, static_cast<off_t>(offset));
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) return false;
    out += count;
    offset += static_cast<std::uint64_t>(count);
    size -= static_cast<std::size_t>(count);
  }
  return true;
}

bool WriteAll(int fd, const unsigned char* data, std::size_t size) {
  while (size > 0) {
    const auto count = ::write(fd, data, size);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) return false;
    data += count;
    size -= static_cast<std::size_t>(count);
  }
  return true;
}

class Fd {
 public:
  explicit Fd(int fd = -1) : fd_(fd) {}
  ~Fd() { Close(); }
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
  int get() const { return fd_; }
  void Reset(int fd) {
    Close();
    fd_ = fd;
  }
  bool Close() {
    if (fd_ < 0) return true;
    const bool ok = ::close(fd_) == 0;
    fd_ = -1;
    return ok;
  }

 private:
  int fd_;
};

// The installer from `offset` on, as the 7z reader sees it: position 0 is the signature.
struct WindowStream {
  ISeekInStream vt;  // first member: the callbacks cast back to WindowStream
  int fd;
  std::uint64_t offset;
  std::uint64_t size;
  std::uint64_t position;
};

SRes WindowRead(ISeekInStreamPtr stream, void* buffer, std::size_t* size) {
  auto* window = reinterpret_cast<WindowStream*>(const_cast<ISeekInStream*>(stream));
  const auto left = window->position < window->size ? window->size - window->position : 0;
  const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(*size, left));
  *size = 0;
  if (count > 0 && !ReadAt(window->fd, window->offset + window->position, buffer, count)) return SZ_ERROR_READ;
  window->position += count;
  *size = count;
  return SZ_OK;
}

SRes WindowSeek(ISeekInStreamPtr stream, Int64* position, ESzSeek origin) {
  auto* window = reinterpret_cast<WindowStream*>(const_cast<ISeekInStream*>(stream));
  Int64 base = 0;
  if (origin == SZ_SEEK_CUR) base = static_cast<Int64>(window->position);
  else if (origin == SZ_SEEK_END) base = static_cast<Int64>(window->size);
  const Int64 target = base + *position;
  if (target < 0) return SZ_ERROR_PARAM;
  window->position = static_cast<std::uint64_t>(target);
  *position = target;
  return SZ_OK;
}

// An opened 7z archive inside the installer.
struct Archive {
  Archive() { SzArEx_Init(&db); }
  ~Archive() { SzArEx_Free(&db, &kHeaderAlloc); }
  Archive(const Archive&) = delete;
  Archive& operator=(const Archive&) = delete;
  CSzArEx db;
  std::uint64_t offset = 0;  // of the signature in the installer
};

bool TryOpen(int fd, std::uint64_t file_size, std::uint64_t offset, Archive* archive) {
  WindowStream window{{WindowRead, WindowSeek}, fd, offset, file_size - offset, 0};
  std::vector<Byte> buffer(64 * 1024);
  CLookToRead2 look;
  LookToRead2_CreateVTable(&look, False);
  look.realStream = &window.vt;
  look.buf = buffer.data();
  look.bufSize = buffer.size();
  LookToRead2_INIT(&look)
  SzArEx_Free(&archive->db, &kHeaderAlloc);
  SzArEx_Init(&archive->db);
  if (SzArEx_Open(&archive->db, &look.vt, &kHeaderAlloc, &kHeaderAlloc) != SZ_OK) return false;
  archive->offset = offset;
  return true;
}

// Scans for the 7z signature; the first hit that opens as an archive wins.
bool OpenEmbeddedArchive(int fd, std::uint64_t file_size, Archive* archive) {
  std::vector<unsigned char> chunk(1u << 20);
  const auto limit = std::min(file_size, kMaxScanBytes);
  int hits = 0;
  for (std::uint64_t at = 0; at < limit;) {
    const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(chunk.size(), file_size - at));
    if (count < sizeof(kSignature) || !ReadAt(fd, at, chunk.data(), count)) return false;
    for (std::size_t i = 0; i + sizeof(kSignature) <= count && at + i < limit; ++i) {
      if (std::memcmp(chunk.data() + i, kSignature, sizeof(kSignature)) != 0) continue;
      if (TryOpen(fd, file_size, at + i, archive)) return true;
      if (++hits >= kMaxSignatureHits) return false;
    }
    if (at + count >= file_size) break;
    at += count - (sizeof(kSignature) - 1);
  }
  return false;
}

bool Utf16ToUtf8(const std::vector<UInt16>& name, std::string* out) {
  out->clear();
  for (std::size_t i = 0; i < name.size() && name[i] != 0; ++i) {
    std::uint32_t c = name[i];
    if (c >= 0xD800 && c <= 0xDBFF) {
      if (i + 1 >= name.size() || name[i + 1] < 0xDC00 || name[i + 1] > 0xDFFF) return false;
      c = 0x10000 + ((c - 0xD800) << 10) + (name[++i] - 0xDC00);
    } else if (c >= 0xDC00 && c <= 0xDFFF) {
      return false;
    }
    if (c < 0x80) {
      *out += static_cast<char>(c);
    } else if (c < 0x800) {
      *out += static_cast<char>(0xC0 | (c >> 6));
      *out += static_cast<char>(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
      *out += static_cast<char>(0xE0 | (c >> 12));
      *out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
      *out += static_cast<char>(0x80 | (c & 0x3F));
    } else {
      *out += static_cast<char>(0xF0 | (c >> 18));
      *out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
      *out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
      *out += static_cast<char>(0x80 | (c & 0x3F));
    }
  }
  return true;
}

// One archive entry and, when it is extracted, its path below the version folder.
struct Entry {
  std::string path;
  bool directory = false;
  std::uint32_t folder = static_cast<std::uint32_t>(-1);  // -1: no data (empty file or folder)
  std::uint64_t size = 0;
  bool has_crc = false;
  std::uint32_t crc = 0;
  std::string target;  // "" = not extracted
};

bool ReadEntries(const Archive& archive, const PublisherRuntimeSpec& spec, std::vector<Entry>* entries,
                 std::string* reason) {
  const auto& db = archive.db;
  std::set<std::string> seen, targets;
  std::vector<UInt16> name;
  entries->assign(db.NumFiles, Entry());
  for (UInt32 i = 0; i < db.NumFiles; ++i) {
    auto& entry = (*entries)[i];
    name.assign(SzArEx_GetFileNameUtf16(&db, i, nullptr), 0);
    SzArEx_GetFileNameUtf16(&db, i, name.data());
    if (!Utf16ToUtf8(name, &entry.path) || !PublisherRuntimePathAllowed(entry.path))
      return (*reason = "the installer's archive holds an unsafe path: " + entry.path), false;
    if (!seen.insert(Lower(entry.path)).second)
      return (*reason = "the installer's archive holds a path twice: " + entry.path), false;
    if (SzBitWithVals_Check(&db.Attribs, i)) {
      const auto attributes = db.Attribs.Vals[i];
      const bool unix_link =
          (attributes & kAttributeUnixExtension) != 0 && ((attributes >> 16) & 0xF000) == 0xA000;
      if (unix_link || (attributes & kAttributeReparsePoint) != 0)
        return (*reason = "the installer's archive holds a link: " + entry.path), false;
    }
    entry.directory = SzArEx_IsDir(&db, i);
    entry.size = SzArEx_GetFileSize(&db, i);
    // Inside a solid folder, folders and empty files carry the folder's index with no bytes.
    entry.folder = entry.directory ? static_cast<std::uint32_t>(-1) : db.FileToFolder[i];
    entry.has_crc = SzBitWithVals_Check(&db.CRCs, i);
    if (entry.has_crc) entry.crc = db.CRCs.Vals[i];

    const auto slash = entry.path.find('/');
    const auto mapped = spec.folders.find(entry.path.substr(0, slash));
    if (mapped == spec.folders.end()) continue;
    if (!PublisherRuntimePathAllowed(mapped->second))
      return (*reason = "unsafe runtime folder: " + mapped->second), false;
    entry.target = mapped->second + (slash == std::string::npos ? "" : entry.path.substr(slash));
    if (!targets.insert(Lower(entry.target)).second)
      return (*reason = "two archive entries map to " + entry.target), false;
  }
  return true;
}

// Decoded output of one LZMA coder, read through a ring dictionary of at most the coder's
// dictionary size (smaller when the stream is shorter). The dictionary is kept across streams;
// it grows once to the size the largest folder needs, so the heap is not left with a trail of
// freed smaller ones.
class LzmaStream {
 public:
  explicit LzmaStream(std::size_t input_bytes) : input_(new Byte[input_bytes]), input_capacity_(input_bytes) {
    LzmaDec_CONSTRUCT(&decoder_)
  }
  ~LzmaStream() { LzmaDec_FreeProbs(&decoder_, &kAlloc); }
  LzmaStream(const LzmaStream&) = delete;
  LzmaStream& operator=(const LzmaStream&) = delete;

  bool Start(int fd, std::uint64_t offset, std::uint64_t packed, const Byte* props, unsigned props_size,
             std::uint64_t unpacked, std::uint64_t largest, std::string* reason) {
    CLzmaProps parsed;
    if (props_size != LZMA_PROPS_SIZE || LzmaProps_Decode(&parsed, props, props_size) != SZ_OK)
      return (*reason = "the installer's archive has bad LZMA properties"), false;
    const auto dictionary = std::max<std::uint64_t>(1, std::min<std::uint64_t>(parsed.dicSize, unpacked));
    if (dictionary > kMaxDictionary) return (*reason = "the installer's archive needs too much memory"), false;
    if (LzmaDec_AllocateProbs(&decoder_, props, props_size, &kAlloc) != SZ_OK)
      return (*reason = "not enough memory to unpack the installer"), false;
    if (dictionary > dictionary_capacity_) {
      const auto size = std::max(dictionary, std::min<std::uint64_t>({parsed.dicSize, largest, kMaxDictionary}));
      dictionary_.reset();
      dictionary_.reset(new (std::nothrow) Byte[size]);
      dictionary_capacity_ = dictionary_ ? static_cast<std::size_t>(size) : 0;
      if (!dictionary_) return (*reason = "not enough memory to unpack the installer"), false;
    }
    decoder_.dic = dictionary_.get();
    decoder_.dicBufSize = static_cast<SizeT>(dictionary);
    LzmaDec_Init(&decoder_);
    fd_ = fd;
    file_offset_ = offset;
    packed_left_ = packed;
    output_left_ = unpacked;
    input_position_ = input_size_ = 0;
    pending_ = nullptr;
    pending_size_ = 0;
    finished_ = false;
    return true;
  }

  // The next decoded bytes, valid until the next call; size 0 once the stream ended (checked).
  bool Next(const Byte** data, std::size_t* size) {
    *size = 0;
    if (output_left_ == 0) return finished_ || Finish();
    if (decoder_.dicPos == decoder_.dicBufSize) decoder_.dicPos = 0;
    const SizeT start = decoder_.dicPos;
    const SizeT limit = start + static_cast<SizeT>(std::min<std::uint64_t>(
                                    {decoder_.dicBufSize - start, output_left_, kSliceBytes}));
    const auto mode = limit - start == output_left_ ? LZMA_FINISH_END : LZMA_FINISH_ANY;
    while (decoder_.dicPos < limit) {
      if (!Refill()) return false;
      SizeT in = input_size_ - input_position_;
      const SizeT before = decoder_.dicPos;
      ELzmaStatus status;
      if (LzmaDec_DecodeToDic(&decoder_, limit, input_.get() + input_position_, &in, mode, &status) != SZ_OK)
        return false;
      input_position_ += in;
      status_ = status;
      if (decoder_.dicPos < limit && (status == LZMA_STATUS_FINISHED_WITH_MARK || (in == 0 && decoder_.dicPos == before)))
        return false;
    }
    *data = decoder_.dic + start;
    *size = limit - start;
    output_left_ -= *size;
    return output_left_ > 0 || Finish();
  }

  // Exactly `size` bytes into out, fewer only at the end of the stream.
  bool Read(Byte* out, std::size_t size, std::size_t* got) {
    *got = 0;
    while (*got < size) {
      if (pending_size_ == 0 && (!Next(&pending_, &pending_size_) || pending_size_ == 0)) return pending_size_ == 0 && finished_;
      const auto count = std::min(pending_size_, size - *got);
      std::memcpy(out + *got, pending_, count);
      pending_ += count;
      pending_size_ -= count;
      *got += count;
    }
    return true;
  }

  // Whether every byte was read and the packed data ended the stream.
  bool Ended() {
    const Byte* data = nullptr;
    std::size_t size = 0;
    return pending_size_ == 0 && Next(&data, &size) && size == 0;
  }

 private:
  bool Refill() {
    if (input_position_ < input_size_ || packed_left_ == 0) return true;
    const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(input_capacity_, packed_left_));
    if (!ReadAt(fd_, file_offset_, input_.get(), count)) return false;
    file_offset_ += count;
    packed_left_ -= count;
    input_position_ = 0;
    input_size_ = count;
    return true;
  }

  // Every byte was produced: the rest of the packed data must end the stream.
  bool Finish() {
    for (bool first = true;; first = false) {
      if (!Refill()) return false;
      SizeT in = input_size_ - input_position_;
      if (!first && in == 0) break;
      ELzmaStatus status;
      if (LzmaDec_DecodeToDic(&decoder_, decoder_.dicPos, input_.get() + input_position_, &in, LZMA_FINISH_END,
                              &status) != SZ_OK)
        return false;
      input_position_ += in;
      status_ = status;
      if (in == 0) break;
    }
    finished_ = input_position_ == input_size_ && packed_left_ == 0 &&
                (status_ == LZMA_STATUS_FINISHED_WITH_MARK || status_ == LZMA_STATUS_MAYBE_FINISHED_WITHOUT_MARK);
    return finished_;
  }

  CLzmaDec decoder_;
  std::unique_ptr<Byte[]> dictionary_;
  std::size_t dictionary_capacity_ = 0;
  std::unique_ptr<Byte[]> input_;
  std::size_t input_capacity_;
  std::size_t input_position_ = 0;
  std::size_t input_size_ = 0;
  int fd_ = -1;
  std::uint64_t file_offset_ = 0;
  std::uint64_t packed_left_ = 0;
  std::uint64_t output_left_ = 0;
  const Byte* pending_ = nullptr;
  std::size_t pending_size_ = 0;
  ELzmaStatus status_ = LZMA_STATUS_NOT_SPECIFIED;
  bool finished_ = false;
};

// Where a folder's decoded bytes go: split by file, each file's CRC checked at its end.
class FolderSink {
 public:
  struct File {
    const Entry* entry;
    fs::path path;           // empty: not written
    std::string* memory;     // or kept in memory
  };

  FolderSink(std::vector<File> files, std::uint32_t folder_crc, bool has_folder_crc,
             const PublisherRuntimeProgress& progress, const std::function<bool()>& canceled, std::uint64_t* done,
             std::uint64_t total)
      : files_(std::move(files)), folder_crc_(folder_crc), has_folder_crc_(has_folder_crc), progress_(progress),
        canceled_(canceled), done_(done), total_(total) {}

  bool Accept(const Byte* data, std::size_t size, std::string* reason) {
    if (canceled_ && canceled_()) return (*reason = "canceled"), false;
    folder_running_ = CrcUpdate(folder_running_, data, size);
    while (size > 0) {
      if (index_ >= files_.size()) return (*reason = "the installer's archive is damaged"), false;
      if (!Open(reason)) return false;
      const auto& file = files_[index_];
      const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(size, file.entry->size - written_));
      crc_ = CrcUpdate(crc_, data, count);
      if (fd_.get() >= 0 && !WriteAll(fd_.get(), data, count))
        return (*reason = "cannot write " + file.entry->target), false;
      if (file.memory) file.memory->append(reinterpret_cast<const char*>(data), count);
      if (fd_.get() >= 0 && done_) {
        *done_ += count;
        if (progress_) progress_(*done_, total_);
      }
      written_ += count;
      data += count;
      size -= count;
      if (written_ == file.entry->size && !Close(reason)) return false;
    }
    return true;
  }

  // After the folder's last byte: every file complete and the folder CRC right.
  bool Finish(std::string* reason) {
    while (index_ < files_.size() && files_[index_].entry->size == 0)
      if (!Open(reason) || !Close(reason)) return false;
    if (index_ != files_.size()) return (*reason = "the installer's archive is damaged"), false;
    if (has_folder_crc_ && CRC_GET_DIGEST(folder_running_) != folder_crc_)
      return (*reason = "the installer's archive is damaged (CRC)"), false;
    return true;
  }

 private:
  bool Open(std::string* reason) {
    if (opened_) return true;
    const auto& file = files_[index_];
    opened_ = true;
    written_ = 0;
    crc_ = CRC_INIT_VAL;
    if (!file.path.empty()) {
      fd_.Reset(::open(file.path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644));
      if (fd_.get() < 0 || ::fchmod(fd_.get(), 0644) != 0)
        return (*reason = "cannot create " + file.entry->target), false;
    }
    return true;
  }

  bool Close(std::string* reason) {
    const auto& file = files_[index_];
    const bool synced = fd_.get() < 0 || ::fsync(fd_.get()) == 0;
    if (!fd_.Close() || !synced) return (*reason = "cannot write " + file.entry->target), false;
    if (file.entry->has_crc && CRC_GET_DIGEST(crc_) != file.entry->crc)
      return (*reason = "the installer's archive is damaged (CRC of " + file.entry->path + ")"), false;
    ++index_;
    opened_ = false;
    return true;
  }

  std::vector<File> files_;
  std::uint32_t folder_crc_;
  bool has_folder_crc_;
  const PublisherRuntimeProgress& progress_;
  const std::function<bool()>& canceled_;
  std::uint64_t* done_;
  std::uint64_t total_;
  std::size_t index_ = 0;
  bool opened_ = false;
  std::uint64_t written_ = 0;
  std::uint32_t crc_ = CRC_INIT_VAL;
  std::uint32_t folder_running_ = CRC_INIT_VAL;
  Fd fd_;
};

// Decodes 7z folders (LZMA, the x86 BCJ filter over LZMA, or BCJ2 over three LZMA streams) through fixed buffers.
class FolderDecoder {
 public:
  FolderDecoder(int fd, const Archive& archive)
      : fd_(fd), archive_(archive), main_(kMainInputBytes), call_(kSideInputBytes), jump_(kSideInputBytes) {
    for (UInt32 i = 0; i < archive.db.db.NumFolders; ++i)
      largest_ = std::max<std::uint64_t>(largest_, SzAr_GetFolderUnpackSize(&archive.db.db, i));
  }

  bool Decode(std::uint32_t folder_index, FolderSink* sink, std::string* reason) {
    const auto& ar = archive_.db.db;
    const Byte* coders = ar.CodersData + ar.FoCodersOffsets[folder_index];
    CSzData data{coders, ar.FoCodersOffsets[folder_index + 1] - ar.FoCodersOffsets[folder_index]};
    CSzFolder folder;
    if (SzGetNextFolderItem(&folder, &data) != SZ_OK || data.Size != 0 ||
        folder.UnpackStream != ar.FoToMainUnpackSizeIndex[folder_index])
      return (*reason = "the installer's archive is damaged"), false;
    const UInt64* unpacked = ar.CoderUnpackSizes + ar.FoToCoderUnpackSizes[folder_index];
    const UInt64* packs = ar.PackPositions + ar.FoStartPackStreamIndex[folder_index];
    const std::uint64_t base = archive_.offset + archive_.db.dataPos;
    const auto pack_offset = [&](unsigned i) { return base + packs[i]; };
    const auto pack_size = [&](unsigned i) { return packs[i + 1] - packs[i]; };
    const auto props = [&](unsigned i) { return coders + folder.Coders[i].PropsOffset; };
    const auto is_lzma = [&](unsigned i) {
      return folder.Coders[i].MethodID == kMethodLzma && folder.Coders[i].NumStreams == 1;
    };
    const std::uint64_t total = SzAr_GetFolderUnpackSize(&ar, folder_index);

    if (folder.NumCoders == 1 && is_lzma(0) && folder.NumPackStreams == 1 && folder.PackStreams[0] == 0 &&
        folder.NumBonds == 0) {
      if (!main_.Start(fd_, pack_offset(0), pack_size(0), props(0), folder.Coders[0].PropsSize, total, largest_, reason))
        return false;
      for (;;) {
        const Byte* bytes = nullptr;
        std::size_t size = 0;
        if (!main_.Next(&bytes, &size)) return (*reason = "the installer's archive is damaged"), false;
        if (size == 0) break;
        if (!sink->Accept(bytes, size, reason)) return false;
      }
      return sink->Finish(reason);
    }

    // The x86 BCJ filter over one LZMA stream (as SzFolder_Decode2 expects it): coder 0 is LZMA on pack stream 0,
    // coder 1 the filter on its output. The filter converts all but the last few bytes it is given, so those wait
    // for the next block; the archive's last bytes stay as they are.
    if (folder.NumCoders == 2 && is_lzma(0) && folder.Coders[1].MethodID == kMethodBcjX86 &&
        folder.Coders[1].NumStreams == 1 && folder.Coders[1].PropsSize == 0 && folder.NumPackStreams == 1 &&
        folder.PackStreams[0] == 0 && folder.NumBonds == 1 && folder.Bonds[0].InIndex == 1 &&
        folder.Bonds[0].OutIndex == 0) {
      if (unpacked[0] != total) return (*reason = "the installer's archive is damaged"), false;
      if (!main_.Start(fd_, pack_offset(0), pack_size(0), props(0), folder.Coders[0].PropsSize, total, largest_, reason))
        return false;
      UInt32 state = Z7_BRANCH_CONV_ST_X86_STATE_INIT_VAL;
      UInt32 pc = 0;
      std::size_t kept = 0;
      std::uint64_t read = 0;
      while (read < total) {
        std::size_t got = 0;
        if (!main_.Read(output_.data() + kept, output_.size() - kept, &got) || got == 0)
          return (*reason = "the installer's archive is damaged"), false;
        read += got;
        const std::size_t filled = kept + got;
        const Byte* converted = z7_BranchConvSt_X86_Dec(output_.data(), filled, pc, &state);
        const std::size_t done = read < total ? static_cast<std::size_t>(converted - output_.data()) : filled;
        if (!sink->Accept(output_.data(), done, reason)) return false;
        pc += static_cast<UInt32>(done);
        kept = filled - done;
        std::memmove(output_.data(), output_.data() + done, kept);
      }
      if (!main_.Ended()) return (*reason = "the installer's archive is damaged"), false;
      return sink->Finish(reason);
    }

    // The 7z BCJ2 layout (as SzFolder_Decode2 expects it): coder 2 is the main stream (pack
    // stream 0), coder 1 the CALL stream (pack 2), coder 0 the JUMP stream (pack 3); the range
    // coder stream is pack stream 1, stored.
    const bool bcj2 = folder.NumCoders == 4 && is_lzma(0) && is_lzma(1) && is_lzma(2) &&
                      folder.Coders[3].MethodID == kMethodBcj2 && folder.Coders[3].NumStreams == 4 &&
                      folder.NumPackStreams == 4 && folder.PackStreams[0] == 2 && folder.PackStreams[1] == 6 &&
                      folder.PackStreams[2] == 1 && folder.PackStreams[3] == 0 && folder.NumBonds == 3 &&
                      folder.Bonds[0].InIndex == 5 && folder.Bonds[0].OutIndex == 0 &&
                      folder.Bonds[1].InIndex == 4 && folder.Bonds[1].OutIndex == 1 &&
                      folder.Bonds[2].InIndex == 3 && folder.Bonds[2].OutIndex == 2;
    if (!bcj2) {
      std::string methods;
      for (UInt32 i = 0; i < folder.NumCoders; ++i) {
        char id[16];
        std::snprintf(id, sizeof(id), "%s%X", i ? "+" : "", static_cast<unsigned>(folder.Coders[i].MethodID));
        methods += id;
      }
      return (*reason = "the installer's archive uses an unsupported 7z method (" + methods + ")"), false;
    }
    if ((unpacked[1] & 3) != 0 || (unpacked[0] & 3) != 0 || unpacked[2] + unpacked[1] + unpacked[0] != total)
      return (*reason = "the installer's archive is damaged"), false;
    if (!main_.Start(fd_, pack_offset(0), pack_size(0), props(2), folder.Coders[2].PropsSize, unpacked[2], largest_, reason) ||
        !call_.Start(fd_, pack_offset(2), pack_size(2), props(1), folder.Coders[1].PropsSize, unpacked[1], largest_, reason) ||
        !jump_.Start(fd_, pack_offset(3), pack_size(3), props(0), folder.Coders[0].PropsSize, unpacked[0], largest_, reason))
      return false;
    std::uint64_t rc_offset = pack_offset(1);
    std::uint64_t rc_left = pack_size(1);

    static const Byte kEmpty = 0;
    CBcj2Dec bcj2_state;
    Bcj2Dec_Init(&bcj2_state);
    for (unsigned i = 0; i < BCJ2_NUM_STREAMS; ++i) bcj2_state.bufs[i] = bcj2_state.lims[i] = &kEmpty;
    std::uint64_t produced = 0;
    const auto damaged = [&] { return (*reason = "the installer's archive is damaged"), false; };
    const auto next_output = [&] {
      bcj2_state.dest = output_.data();
      bcj2_state.destLim = output_.data() + std::min<std::uint64_t>(output_.size(), total - produced);
    };
    next_output();
    while (produced < total) {
      if (Bcj2Dec_Decode(&bcj2_state) != SZ_OK) return damaged();
      if (bcj2_state.dest == bcj2_state.destLim) {
        const auto size = static_cast<std::size_t>(bcj2_state.dest - output_.data());
        if (!sink->Accept(output_.data(), size, reason)) return false;
        produced += size;
        next_output();
        continue;
      }
      // The decoder stopped for input: refill the stream it waits for.
      const unsigned state = bcj2_state.state;
      if (state < BCJ2_NUM_STREAMS && bcj2_state.bufs[state] == bcj2_state.lims[state]) {
        std::size_t got = 0;
        const Byte* bytes = nullptr;
        if (state == BCJ2_STREAM_MAIN) {
          if (!main_.Next(&bytes, &got)) return damaged();
        } else if (state == BCJ2_STREAM_RC) {
          got = static_cast<std::size_t>(std::min<std::uint64_t>(rc_.size(), rc_left));
          if (!ReadAt(fd_, rc_offset, rc_.data(), got)) return damaged();
          rc_offset += got;
          rc_left -= got;
          bytes = rc_.data();
        } else {
          auto& buffer = state == BCJ2_STREAM_CALL ? call_buffer_ : jump_buffer_;
          if (!(state == BCJ2_STREAM_CALL ? call_ : jump_).Read(buffer.data(), buffer.size(), &got)) return damaged();
          bytes = buffer.data();
        }
        if (got == 0) return damaged();
        bcj2_state.bufs[state] = bytes;
        bcj2_state.lims[state] = bytes + got;
        continue;
      }
      return damaged();
    }
    for (unsigned i = 0; i < BCJ2_NUM_STREAMS; ++i)
      if (bcj2_state.bufs[i] != bcj2_state.lims[i]) return damaged();
    if (!main_.Ended() || !call_.Ended() || !jump_.Ended() || rc_left != 0 || !Bcj2Dec_IsMaybeFinished(&bcj2_state))
      return damaged();
    return sink->Finish(reason);
  }

 private:
  int fd_;
  const Archive& archive_;
  std::uint64_t largest_ = 0;  // folder output
  LzmaStream main_;
  LzmaStream call_;
  LzmaStream jump_;
  // On the heap: Playden's worker threads have small stacks.
  std::vector<Byte> call_buffer_ = std::vector<Byte>(kSideInputBytes);
  std::vector<Byte> jump_buffer_ = std::vector<Byte>(kSideInputBytes);
  std::vector<Byte> rc_ = std::vector<Byte>(kSideInputBytes);
  std::vector<Byte> output_ = std::vector<Byte>(kSliceBytes);
};

// Files of a folder, in stream order.
std::vector<const Entry*> FolderFiles(const Archive& archive, const std::vector<Entry>& entries, std::uint32_t folder) {
  std::vector<const Entry*> files;
  const auto& db = archive.db;
  const UInt32 end = folder + 1 < db.db.NumFolders ? db.FolderToFile[folder + 1] : db.NumFiles;
  for (UInt32 i = db.FolderToFile[folder]; i < end && i < db.NumFiles; ++i)
    if (entries[i].folder == folder) files.push_back(&entries[i]);
  return files;
}

bool FolderSizesMatch(const Archive& archive, const std::vector<const Entry*>& files, std::uint32_t folder) {
  std::uint64_t sum = 0;
  for (const auto* file : files) sum += file->size;
  return sum == SzAr_GetFolderUnpackSize(&archive.db.db, folder);
}

// The ordering of version folders: numerically, part by part.
std::optional<std::array<std::uint32_t, 4>> ParseVersion(const std::string& name) {
  std::array<std::uint32_t, 4> parts{};
  std::size_t at = 0;
  for (std::size_t i = 0; i < 4; ++i) {
    if (i > 0 && (at >= name.size() || name[at++] != '.')) return std::nullopt;
    const auto begin = at;
    std::uint64_t value = 0;
    while (at < name.size() && name[at] >= '0' && name[at] <= '9' && at - begin < 6)
      value = value * 10 + static_cast<std::uint64_t>(name[at++] - '0');
    if (at == begin || value > 0xFFFF) return std::nullopt;
    parts[i] = static_cast<std::uint32_t>(value);
  }
  if (at != name.size()) return std::nullopt;
  return parts;
}

bool IsDirectory(const fs::path& path) {
  struct stat info {};
  return ::lstat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

bool IsRegular(const fs::path& path) {
  struct stat info {};
  return ::lstat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode);
}

// mkdir -p with 0755 for the folders it creates; existing parts must be real folders.
bool MakeDirectories(const fs::path& path) {
  if (path.empty() || IsDirectory(path)) return true;
  if (path.has_parent_path() && path.parent_path() != path && !MakeDirectories(path.parent_path())) return false;
  if (::mkdir(path.c_str(), 0755) != 0 && errno != EEXIST) return false;
  if (!IsDirectory(path)) return false;
  return ::chmod(path.c_str(), 0755) == 0;
}

// Folders below root, each 0755; root must exist.
bool MakeBelow(const fs::path& root, const std::string& relative) {
  fs::path path = root;
  std::size_t begin = 0;
  while (begin <= relative.size()) {
    const auto end = relative.find('/', begin);
    path /= relative.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
    if (!IsDirectory(path)) {
      if (::mkdir(path.c_str(), 0755) != 0 || ::chmod(path.c_str(), 0755) != 0) return false;
    }
    if (end == std::string::npos) break;
    begin = end + 1;
  }
  return true;
}

bool WriteMarker(const fs::path& path) {
  Fd fd(::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0644));
  if (fd.get() < 0) return false;
  const bool ok = ::fchmod(fd.get(), 0644) == 0 && ::fsync(fd.get()) == 0;
  return fd.Close() && ok;
}

// A small regular file (links not followed), or nothing.
std::optional<std::string> ReadSmall(const fs::path& file, std::size_t limit) {
  struct stat info {};
  if (::lstat(file.c_str(), &info) != 0 || !S_ISREG(info.st_mode) || static_cast<std::uint64_t>(info.st_size) > limit)
    return std::nullopt;
  const int fd = ::open(file.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) return std::nullopt;
  std::string text(static_cast<std::size_t>(info.st_size), '\0');
  std::size_t at = 0;
  while (at < text.size()) {
    const auto count = ::read(fd, text.data() + at, text.size() - at);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) break;
    at += static_cast<std::size_t>(count);
  }
  ::close(fd);
  text.resize(at);
  return text;
}

// Best effort, through <file>.new and a rename.
void WriteSmall(const fs::path& file, const std::string& text) {
  const fs::path temporary = file.string() + ".new";
  const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0644);
  if (fd < 0) return;
  std::size_t at = 0;
  while (at < text.size()) {
    const auto count = ::write(fd, text.data() + at, text.size() - at);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) break;
    at += static_cast<std::size_t>(count);
  }
  ::close(fd);
  std::error_code ignored;
  if (at != text.size() || ::rename(temporary.c_str(), file.c_str()) != 0) fs::remove(temporary, ignored);
}

// A game's record: the version it uses and the pack it was set up with, one per line.
constexpr char kGameRecordPrefix[] = ".game-";

bool GameIdAllowed(const std::string& game) {
  if (game.empty() || game.size() > 32) return false;
  return std::all_of(game.begin(), game.end(), [](unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c == '-';
  });
}

struct GameRecord {
  std::string version;
  std::string pack;
};

std::optional<GameRecord> ReadGameRecord(const fs::path& file) {
  const auto text = ReadSmall(file, 256);
  if (!text) return std::nullopt;
  const auto first = text->find('\n');
  if (first == std::string::npos) return std::nullopt;
  const auto second = text->find('\n', first + 1);
  if (second == std::string::npos || second + 1 != text->size()) return std::nullopt;
  GameRecord record{text->substr(0, first), text->substr(first + 1, second - first - 1)};
  if (!ParseVersion(record.version) || record.pack.empty()) return std::nullopt;
  return record;
}

// Removes leftovers: every other version folder without a .complete marker, and complete ones
// beyond the newest other one, except a version a game made with this pack still names.
void KeepVersions(const fs::path& root, const std::string& current, const std::string& pack) {
  std::error_code error;
  std::set<std::string> named;
  if (!pack.empty()) {
    for (fs::directory_iterator it(root, error), end; !error && it != end; it.increment(error)) {
      const auto name = it->path().filename().string();
      if (name.rfind(kGameRecordPrefix, 0) != 0 || name.size() > 4 && name.compare(name.size() - 4, 4, ".new") == 0)
        continue;
      if (const auto record = ReadGameRecord(it->path()); record && record->pack == pack) named.insert(record->version);
    }
    error.clear();
  }
  std::vector<std::pair<std::array<std::uint32_t, 4>, fs::path>> complete;
  for (fs::directory_iterator it(root, error), end; !error && it != end; it.increment(error)) {
    const auto name = it->path().filename().string();
    const auto version = ParseVersion(name);
    if (!version || name == current) continue;
    if (IsRegular(it->path() / kPublisherRuntimeCompleteMarker)) {
      if (!named.count(name)) complete.emplace_back(*version, it->path());
    } else {
      std::error_code ignored;
      fs::remove_all(it->path(), ignored);
    }
  }
  std::sort(complete.begin(), complete.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
  for (std::size_t i = 1; i < complete.size(); ++i) {
    std::error_code ignored;
    fs::remove_all(complete[i].second, ignored);
  }
}

// What the last good run learned about the installer: its path, size and modification time, and the
// version it holds. A later run that finds the same installer and a complete version folder skips
// decoding the archive.
constexpr char kInstallerCacheName[] = ".installer-cache";

std::string InstallerStamp(const fs::path& installer, const struct stat& info) {
  return installer.string() + "\n" + std::to_string(static_cast<std::uint64_t>(info.st_size)) + "\n" +
         std::to_string(static_cast<std::int64_t>(info.st_mtime)) + "\n";
}

std::optional<std::string> CachedVersion(const fs::path& root, const std::string& stamp) {
  const auto text = ReadSmall(root / kInstallerCacheName, 4096);
  if (!text || text->size() <= stamp.size() || text->compare(0, stamp.size(), stamp) != 0) return std::nullopt;
  auto version = text->substr(stamp.size());
  while (!version.empty() && (version.back() == '\n' || version.back() == '\r')) version.pop_back();
  if (!ParseVersion(version)) return std::nullopt;
  return version;
}

// Best effort: a cache that can't be written only costs the next run a decode.
void WriteInstallerCache(const fs::path& root, const std::string& stamp, const std::string& version) {
  WriteSmall(root / kInstallerCacheName, stamp + version + "\n");
}

PublisherRuntimeResult Failure(std::string reason) {
  PublisherRuntimeResult result;
  result.reason = std::move(reason);
  return result;
}

}  // namespace

bool PublisherRuntimePathAllowed(std::string_view path) {
  if (path.empty() || path.front() == '/' || path.size() > 1024) return false;
  for (unsigned char c : path)
    if (c < 0x20 || c == ':' || c == '\\' || c == 0x7f) return false;
  std::size_t begin = 0;
  for (;;) {
    const auto end = path.find('/', begin);
    const auto part = path.substr(begin, end == std::string_view::npos ? std::string_view::npos : end - begin);
    if (part.empty() || part == "." || part == "..") return false;
    if (end == std::string_view::npos) return true;
    begin = end + 1;
  }
}

std::optional<std::string> PeFileVersion(std::string_view image) {
  const auto u16 = [&](std::uint64_t at) -> std::optional<std::uint32_t> {
    if (at + 2 > image.size()) return std::nullopt;
    return static_cast<std::uint32_t>(static_cast<unsigned char>(image[at]) |
                                      static_cast<unsigned char>(image[at + 1]) << 8);
  };
  const auto u32 = [&](std::uint64_t at) -> std::optional<std::uint32_t> {
    const auto low = u16(at), high = u16(at + 2);
    if (!low || !high) return std::nullopt;
    return *low | *high << 16;
  };
  if (image.size() < 0x40 || image.substr(0, 2) != "MZ") return std::nullopt;
  const auto pe = u32(0x3C);
  if (!pe || *pe + 24 > image.size() || image.substr(*pe, 4) != std::string_view("PE\0\0", 4)) return std::nullopt;
  const auto sections = u16(*pe + 6), optional_size = u16(*pe + 20), magic = u16(*pe + 24);
  if (!sections || !optional_size || !magic) return std::nullopt;
  const std::uint64_t optional = *pe + 24;
  std::uint64_t directories = 0;
  if (*magic == 0x10B) directories = optional + 96;
  else if (*magic == 0x20B) directories = optional + 112;
  else return std::nullopt;
  const auto count = u32(directories - 4);
  if (!count || *count < 3 || directories + 24 > optional + *optional_size) return std::nullopt;
  const auto resource_rva = u32(directories + 16);
  if (!resource_rva || *resource_rva == 0) return std::nullopt;

  const std::uint64_t section_table = optional + *optional_size;
  const auto to_offset = [&](std::uint32_t rva) -> std::optional<std::uint64_t> {
    for (std::uint32_t i = 0; i < *sections; ++i) {
      const std::uint64_t at = section_table + 40ull * i;
      const auto virtual_size = u32(at + 8), address = u32(at + 12), raw_size = u32(at + 16), raw = u32(at + 20);
      if (!virtual_size || !address || !raw_size || !raw) return std::nullopt;
      const auto span = std::max(*virtual_size, *raw_size);
      if (rva >= *address && rva - *address < span && rva - *address < *raw_size) return *raw + (rva - *address);
    }
    return std::nullopt;
  };
  const auto resources = to_offset(*resource_rva);
  if (!resources) return std::nullopt;
  // Resource tree: type (16 = RT_VERSION) -> first name -> first language -> data entry.
  const auto entry = [&](std::uint64_t directory, std::optional<std::uint32_t> id) -> std::optional<std::uint32_t> {
    const auto named = u16(directory + 12), ids = u16(directory + 14);
    if (!named || !ids) return std::nullopt;
    for (std::uint32_t i = 0; i < *named + *ids; ++i) {
      const auto name = u32(directory + 16 + 8ull * i), target = u32(directory + 20 + 8ull * i);
      if (!name || !target) return std::nullopt;
      if (!id || *name == *id) return *target;
    }
    return std::nullopt;
  };
  const auto type = entry(*resources, 16u);
  if (!type || !(*type & 0x80000000u)) return std::nullopt;
  const auto name = entry(*resources + (*type & 0x7FFFFFFFu), std::nullopt);
  if (!name || !(*name & 0x80000000u)) return std::nullopt;
  const auto language = entry(*resources + (*name & 0x7FFFFFFFu), std::nullopt);
  if (!language || (*language & 0x80000000u)) return std::nullopt;
  const auto data_rva = u32(*resources + *language);
  if (!data_rva) return std::nullopt;
  const auto info = to_offset(*data_rva);
  if (!info) return std::nullopt;

  // VS_VERSIONINFO: length, value length (52), type, L"VS_VERSION_INFO", padding, VS_FIXEDFILEINFO.
  static constexpr char kKey[] = "VS_VERSION_INFO";
  if (u16(*info + 2).value_or(0) != 52) return std::nullopt;
  for (std::size_t i = 0; i < sizeof(kKey); ++i)
    if (u16(*info + 6 + 2 * i).value_or(0xFFFF) != static_cast<unsigned char>(kKey[i])) return std::nullopt;
  const std::uint64_t fixed = *info + 40;
  if (u32(fixed).value_or(0) != 0xFEEF04BDu) return std::nullopt;
  const auto high = u32(fixed + 8), low = u32(fixed + 12);
  if (!high || !low) return std::nullopt;
  return std::to_string(*high >> 16) + "." + std::to_string(*high & 0xFFFF) + "." + std::to_string(*low >> 16) +
         "." + std::to_string(*low & 0xFFFF);
}

PublisherRuntimeResult EnsurePublisherRuntime(const PublisherRuntimeSpec& spec, const std::string& install_root,
                                              const std::string& runtime_root,
                                              const PublisherRuntimeProgress& progress,
                                              const std::function<bool()>& canceled,
                                              const PublisherRuntimeHolder& holder) {
  InitCrc();
  const fs::path cache_root(runtime_root);
  // The game's record, when it still holds: made with this pack, its version folder complete.
  const bool recorded = GameIdAllowed(holder.game) && !holder.pack.empty();
  const fs::path record_file = recorded ? cache_root / (kGameRecordPrefix + holder.game) : fs::path();
  std::optional<GameRecord> record;
  if (recorded) {
    record = ReadGameRecord(record_file);
    if (record && (record->pack != holder.pack || !IsRegular(cache_root / record->version / kPublisherRuntimeCompleteMarker)))
      record.reset();
  }
  // The recorded version, when the installer holds another one ("": the installer can't be read).
  const auto keep = [&](const std::string& installer_version) -> std::optional<PublisherRuntimeResult> {
    if (!record || record->version == installer_version) return std::nullopt;
    PublisherRuntimeResult result;
    result.ok = true;
    result.kept = true;
    result.version = record->version;
    result.version_dir = (cache_root / record->version).string();
    result.installer_version = installer_version;
    KeepVersions(cache_root, record->version, holder.pack);
    return result;
  };
  const auto fail = [&](std::string reason) {
    if (auto kept = keep({})) return *kept;
    return Failure(std::move(reason));
  };
  // A version in use: recorded for the game, then the other versions trimmed.
  const auto finish = [&](PublisherRuntimeResult result) {
    if (recorded) WriteSmall(record_file, result.version + "\n" + holder.pack + "\n");
    KeepVersions(cache_root, result.version, holder.pack);
    return result;
  };
  fs::path installer;
  for (const auto& candidate : spec.installers) {
    if (!PublisherRuntimePathAllowed(candidate)) continue;
    if (IsRegular(fs::path(install_root) / candidate)) {
      installer = fs::path(install_root) / candidate;
      break;
    }
  }
  if (installer.empty()) return fail("The " + spec.label + " installer was not found in the game.");
  Fd fd(::open(installer.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
  struct stat info {};
  if (fd.get() < 0 || ::fstat(fd.get(), &info) != 0 || !S_ISREG(info.st_mode))
    return fail("cannot read the " + spec.label + " installer");

  const auto stamp = InstallerStamp(installer, info);
  // A cached version answers when its folder is complete, or when the game keeps another one.
  if (const auto cached = CachedVersion(cache_root, stamp)) {
    if (auto kept = keep(*cached)) return *kept;
  }
  if (const auto cached = CachedVersion(cache_root, stamp);
      cached && IsRegular(cache_root / *cached / kPublisherRuntimeCompleteMarker)) {
    PublisherRuntimeResult result;
    result.ok = true;
    result.version = *cached;
    result.version_dir = (cache_root / *cached).string();
    return finish(std::move(result));
  }

  Archive archive;
  if (!OpenEmbeddedArchive(fd.get(), static_cast<std::uint64_t>(info.st_size), &archive))
    return fail("The " + spec.label + " installer holds no archive Playden can read.");
  std::vector<Entry> entries;
  std::string reason;
  if (!ReadEntries(archive, spec, &entries, &reason)) return fail(std::move(reason));

  FolderDecoder decoder(fd.get(), archive);
  // The version: decode just the version file's folder into memory.
  const auto version_entry = std::find_if(entries.begin(), entries.end(), [&](const Entry& entry) {
    return !entry.directory && entry.path == spec.version_file;
  });
  if (version_entry == entries.end() || version_entry->folder == static_cast<std::uint32_t>(-1) ||
      version_entry->size > kMaxVersionFileBytes)
    return fail("The " + spec.label + " installer has no usable " + spec.version_file + ".");
  std::string version_bytes;
  version_bytes.reserve(static_cast<std::size_t>(version_entry->size));
  {
    const auto files = FolderFiles(archive, entries, version_entry->folder);
    if (!FolderSizesMatch(archive, files, version_entry->folder)) return fail("the installer's archive is damaged");
    std::vector<FolderSink::File> sinks;
    for (const auto* file : files) sinks.push_back({file, {}, file == &*version_entry ? &version_bytes : nullptr});
    const auto& ar = archive.db.db;
    FolderSink sink(std::move(sinks), SzBitWithVals_Check(&ar.FolderCRCs, version_entry->folder)
                        ? ar.FolderCRCs.Vals[version_entry->folder] : 0,
                    SzBitWithVals_Check(&ar.FolderCRCs, version_entry->folder), progress, canceled, nullptr, 0);
    if (!decoder.Decode(version_entry->folder, &sink, &reason)) return fail(std::move(reason));
  }
  const auto version = PeFileVersion(version_bytes);
  version_bytes = std::string();
  if (!version) return fail("cannot read the version of " + spec.version_file);
  if (auto kept = keep(*version)) {
    WriteInstallerCache(cache_root, stamp, *version);  // the next run doesn't decode it again
    return *kept;
  }

  // From here on, a failed result still names the version.
  PublisherRuntimeResult result;
  result.version = *version;
  const fs::path& root = cache_root;
  const auto target = root / *version;
  result.version_dir = target.string();
  if (IsRegular(target / kPublisherRuntimeCompleteMarker)) {
    result.ok = true;
    WriteInstallerCache(root, stamp, *version);
    return finish(std::move(result));
  }

  const auto failure = [&](std::string text) {
    result.reason = std::move(text);
    return result;
  };
  if (!MakeDirectories(root)) return failure("cannot create " + root.string());
  std::error_code error;
  for (fs::directory_iterator it(root, error), end; !error && it != end; it.increment(error)) {
    if (it->path().filename().string().rfind(kStagingPrefix, 0) == 0) {
      std::error_code ignored;
      fs::remove_all(it->path(), ignored);
    }
  }
  const auto stage = root / (kStagingPrefix + *version);
  if (::mkdir(stage.c_str(), 0755) != 0 || ::chmod(stage.c_str(), 0755) != 0)
    return failure("cannot create " + stage.string());
  const auto abandon = [&](std::string text) {
    std::error_code ignored;
    fs::remove_all(stage, ignored);
    return failure(std::move(text));
  };

  // Folders and empty files first, then every folder holding an extracted file.
  std::uint64_t total = 0;
  std::set<std::uint32_t> folders;
  for (const auto& entry : entries) {
    if (entry.target.empty()) continue;
    const auto slash = entry.target.rfind('/');
    if (entry.directory) {
      if (!MakeBelow(stage, entry.target)) return abandon("cannot create " + entry.target);
      continue;
    }
    if (slash != std::string::npos && !MakeBelow(stage, entry.target.substr(0, slash)))
      return abandon("cannot create the folder of " + entry.target);
    if (entry.folder == static_cast<std::uint32_t>(-1)) {
      if (!WriteMarker(stage / entry.target)) return abandon("cannot create " + entry.target);
      continue;
    }
    total += entry.size;
    folders.insert(entry.folder);
  }
  std::uint64_t done = 0;
  if (progress) progress(0, total);
  for (const auto folder : folders) {
    if (canceled && canceled()) return abandon("canceled");
    const auto files = FolderFiles(archive, entries, folder);
    if (!FolderSizesMatch(archive, files, folder)) return abandon("the installer's archive is damaged");
    std::vector<FolderSink::File> sinks;
    for (const auto* file : files)
      sinks.push_back({file, file->target.empty() ? fs::path() : stage / file->target, nullptr});
    const auto& ar = archive.db.db;
    const bool has_crc = SzBitWithVals_Check(&ar.FolderCRCs, folder);
    FolderSink sink(std::move(sinks), has_crc ? ar.FolderCRCs.Vals[folder] : 0, has_crc, progress, canceled, &done,
                    total);
    if (!decoder.Decode(folder, &sink, &reason)) return abandon(reason);
  }

  if (!WriteMarker(stage / kPublisherRuntimeCompleteMarker)) return abandon("cannot record the runtime");
  fs::remove_all(target, error);
  if (error) return abandon("cannot replace " + target.string());
  fs::rename(stage, target, error);
  if (error) return abandon("cannot finish " + target.string());
  WriteInstallerCache(root, stamp, *version);
  result.ok = true;
  result.extracted = true;
  return finish(std::move(result));
}

}  // namespace playden::runtime
