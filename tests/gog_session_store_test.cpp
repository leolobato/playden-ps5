// SPDX-License-Identifier: GPL-3.0-or-later
// The GOG session file: private folder and file, refused links and modes, corrupt files removed, "Stay signed in"
// off, and no token left behind after a failed write. Synthetic tokens and accounts only.
#include "gog/session_store.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <functional>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#ifdef PLAYDEN_TEST_AT_UNAVAILABLE
// The console title refuses *at calls with a real folder descriptor: none may be needed.
extern "C" int openat(int, const char*, int, ...) { errno = EINVAL; return -1; }
extern "C" int mkdirat(int, const char*, mode_t) { errno = EINVAL; return -1; }
extern "C" int fstatat(int, const char*, struct stat*, int) { errno = EINVAL; return -1; }
extern "C" int unlinkat(int, const char*, int) { errno = EINVAL; return -1; }
extern "C" int renameat(int, const char*, int, const char*) { errno = EINVAL; return -1; }
#endif

using namespace playden;
using playden::gog::LoadStatus;
using playden::gog::SessionStore;
using playden::gog::StoredSession;
namespace fs = std::filesystem;

#define CHECK(condition)                                                                  \
  do {                                                                                    \
    if (!(condition)) {                                                                   \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
      std::exit(1);                                                                       \
    }                                                                                     \
  } while (false)

namespace {

fs::path g_base;
int g_roots = 0;

const std::string kToken = "SyntheticRefreshToken-0123456789abcdef_ABCDEF.~+/=!";

StoredSession Session(const std::string& token = kToken) {
  StoredSession session;
  session.refresh_token = token;
  session.user_id = "48151623421234567";
  session.username = "Synthetic Player";
  return session;
}

// A fresh user root, shared as on the console (user/ is 01777 there; 0755 here is enough to show it isn't required
// to be private).
fs::path FreshRoot() {
  const fs::path root = g_base / ("root-" + std::to_string(++g_roots));
  fs::create_directories(root);
  CHECK(::chmod(root.c_str(), 0755) == 0);
  return root;
}

SessionStore::Options Options(const fs::path& root, bool persist = true, SessionStore::Writer write = {}) {
  SessionStore::Options options;
  options.user_root = root.string();
  options.persist = persist;
  options.write = std::move(write);
  return options;
}

mode_t ModeOf(const fs::path& path) {
  struct stat info{};
  CHECK(::lstat(path.c_str(), &info) == 0);
  return info.st_mode & 07777;
}

bool Exists(const fs::path& path) {
  struct stat info{};
  return ::lstat(path.c_str(), &info) == 0;
}

bool IsLink(const fs::path& path) {
  struct stat info{};
  return ::lstat(path.c_str(), &info) == 0 && S_ISLNK(info.st_mode);
}

std::string ReadAll(const fs::path& path) {
  FILE* file = std::fopen(path.c_str(), "rb");
  CHECK(file != nullptr);
  std::string text;
  char chunk[4096];
  for (std::size_t count; (count = std::fread(chunk, 1, sizeof(chunk), file)) > 0;) text.append(chunk, count);
  std::fclose(file);
  return text;
}

void WriteRaw(const fs::path& path, const Bytes& bytes, mode_t mode = 0600) {
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, mode);
  CHECK(fd >= 0);
  CHECK(::write(fd, bytes.data(), bytes.size()) == static_cast<ssize_t>(bytes.size()));
  CHECK(::close(fd) == 0);
  CHECK(::chmod(path.c_str(), mode) == 0);
}

void WriteText(const fs::path& path, const std::string& text, mode_t mode = 0600) {
  WriteRaw(path, Bytes(text.begin(), text.end()), mode);
}

// Every regular file in folder (links are not followed) whose bytes contain the token.
int FilesWithToken(const fs::path& folder, const std::string& token = kToken) {
  if (!Exists(folder)) return 0;
  int found = 0;
  for (const auto& entry : fs::directory_iterator(folder)) {
    if (!fs::is_regular_file(entry.symlink_status())) continue;
    if (ReadAll(entry.path()).find(token) != std::string::npos) ++found;
  }
  return found;
}

std::size_t EntryCount(const fs::path& folder) {
  std::size_t count = 0;
  for (const auto& entry : fs::directory_iterator(folder)) {
    (void)entry;
    ++count;
  }
  return count;
}

// The test's own CRC-32 for hand-built files; pinned by the golden file below.
std::uint32_t Crc32(const std::uint8_t* data, std::size_t size) {
  std::uint32_t crc = 0xFFFFFFFFu;
  for (std::size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) crc = (crc & 1u) ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
  }
  return ~crc;
}

void Put16(Bytes& out, std::size_t value) {
  out.push_back(static_cast<std::uint8_t>(value));
  out.push_back(static_cast<std::uint8_t>(value >> 8));
}

void Put32(Bytes& out, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) out.push_back(static_cast<std::uint8_t>(value >> shift));
}

// A file with any fields, checked or not, and a correct header.
Bytes Raw(const std::string& token, const std::string& user_id, const std::string& username) {
  Bytes payload;
  for (const auto* text : {&token, &user_id, &username}) {
    Put16(payload, text->size());
    payload.insert(payload.end(), text->begin(), text->end());
  }
  Bytes out = {'P', 'D', 'G', 'O', 'G', 'S', 0, 1, 1};
  Put32(out, static_cast<std::uint32_t>(payload.size()));
  Put32(out, Crc32(payload.data(), payload.size()));
  out.insert(out.end(), payload.begin(), payload.end());
  return out;
}

void FormatIsPinned() {
  StoredSession tiny;
  tiny.refresh_token = "abc";
  tiny.user_id = "1";
  tiny.username = "u";
  // CRC-32 of the payload computed independently (Python zlib.crc32): 0x338b1051.
  const Bytes golden = {'P', 'D', 'G', 'O', 'G', 'S', 0, 1, 1,  11, 0, 0, 0,  0x51, 0x10, 0x8b, 0x33,
                        3, 0, 'a', 'b', 'c', 1, 0, '1', 1, 0, 'u'};
  CHECK(gog::EncodeSession(tiny) == golden);
  CHECK(Raw("abc", "1", "u") == golden);
  StoredSession decoded;
  CHECK(gog::DecodeSession(golden, &decoded));
  CHECK(decoded.refresh_token == "abc" && decoded.user_id == "1" && decoded.username == "u");

  // Out-of-range fields are not encoded.
  auto bad = Session();
  bad.user_id = "012";
  CHECK(gog::EncodeSession(bad).empty());
  bad = Session();
  bad.refresh_token = "has space";
  CHECK(gog::EncodeSession(bad).empty());
  bad = Session();
  bad.refresh_token = std::string(4097, 'a');
  CHECK(gog::EncodeSession(bad).empty());
  bad = Session();
  bad.user_id = "123456789012345678901";
  CHECK(gog::EncodeSession(bad).empty());
  bad = Session();
  bad.username = std::string(257, 'n');
  CHECK(gog::EncodeSession(bad).empty());
  bad = Session();
  bad.username.clear();
  CHECK(gog::EncodeSession(bad).empty());
  // The limits themselves are fine.
  auto edge = Session(std::string(4096, '~'));
  edge.user_id = "18446744073709551615";
  edge.username = std::string(256, 'n');
  CHECK(!gog::EncodeSession(edge).empty());

  // Wipe clears every field.
  auto wiped = Session();
  wiped.Wipe();
  CHECK(wiped.refresh_token.empty() && wiped.user_id.empty() && wiped.username.empty());
}

void RoundTrip() {
  const auto root = FreshRoot();
  {
    SessionStore store(Options(root));
    // Nothing yet: no folder is made just by looking.
    CHECK(store.Load().status == LoadStatus::kNone);
    CHECK(!Exists(root / "gog"));
    const auto saved = store.Save(Session());
    CHECK(saved.ok && !saved.storage_full);
  }
  CHECK(ModeOf(root / "gog") == 0777);  // open: FTP can delete the sign-in (#868)
  CHECK(ModeOf(root / "gog/session") == 0600);
  CHECK(EntryCount(root / "gog") == 1);
  SessionStore restarted(Options(root));
  const auto loaded = restarted.Load();
  CHECK(loaded.status == LoadStatus::kOk);
  CHECK(loaded.session.refresh_token == kToken);
  CHECK(loaded.session.user_id == "48151623421234567");
  CHECK(loaded.session.username == "Synthetic Player");
  CHECK(loaded.reason.find(kToken) == std::string::npos);

  // A new session replaces the file.
  CHECK(restarted.Save(Session("NewerToken-xyz")).ok);
  SessionStore again(Options(root));
  CHECK(again.Load().session.refresh_token == "NewerToken-xyz");
  CHECK(FilesWithToken(root / "gog") == 0);
}

void Refusals() {
  // The gog folder is a link to a folder elsewhere: nothing is read, written or followed.
  {
    const auto root = FreshRoot();
    const auto outside = root / "outside";
    fs::create_directory(outside);
    CHECK(::chmod(outside.c_str(), 0700) == 0);
    WriteRaw(outside / "session", gog::EncodeSession(Session()));
    fs::create_directory_symlink(outside, root / "gog");
    SessionStore store(Options(root));
    CHECK(store.Load().status == LoadStatus::kUnavailable);
    CHECK(!store.Save(Session("OtherToken")).ok);
    CHECK(!store.Clear().ok);
    CHECK(!store.RemoveLeftovers());
    CHECK(!store.SetPersist(false).ok);
    CHECK(IsLink(root / "gog"));
    CHECK(EntryCount(outside) == 1 && FilesWithToken(outside) == 1);
    CHECK(FilesWithToken(outside, "OtherToken") == 0);
  }
  // The session file is a link: refused, and neither it nor its target is touched.
  {
    const auto root = FreshRoot();
    const auto outside = root / "outside";
    fs::create_directory(outside);
    WriteRaw(outside / "target", gog::EncodeSession(Session()));
    fs::create_directory(root / "gog");
    CHECK(::chmod((root / "gog").c_str(), 0700) == 0);
    fs::create_symlink(outside / "target", root / "gog/session");
    SessionStore store(Options(root));
    CHECK(store.Load().status == LoadStatus::kUnavailable);
    CHECK(!store.Save(Session("OtherToken")).ok);
    CHECK(!store.Clear().ok);
    CHECK(IsLink(root / "gog/session"));
    CHECK(ReadAll(outside / "target").find(kToken) != std::string::npos);
    CHECK(FilesWithToken(root / "gog", "OtherToken") == 0);
    CHECK(EntryCount(root / "gog") == 1);
  }
  // A gog folder of the app user's at another mode is Playden's own (#868): used as it is, never chmodded,
  // and the session in it is still owner-only.
  {
    const auto root = FreshRoot();
    fs::create_directory(root / "gog");
    CHECK(::chmod((root / "gog").c_str(), 0755) == 0);
    SessionStore store(Options(root));
    CHECK(store.Save(Session()).ok);
    SessionStore restarted(Options(root));
    CHECK(restarted.Load().status == LoadStatus::kOk);
    CHECK(ModeOf(root / "gog") == 0755);
    CHECK(ModeOf(root / "gog/session") == 0600);
  }
  // A session file of another mode is refused and kept.
  {
    const auto root = FreshRoot();
    fs::create_directory(root / "gog");
    CHECK(::chmod((root / "gog").c_str(), 0700) == 0);
    WriteRaw(root / "gog/session", gog::EncodeSession(Session()), 0644);
    SessionStore store(Options(root));
    CHECK(store.Load().status == LoadStatus::kUnavailable);
    CHECK(Exists(root / "gog/session"));
  }
  // A user root that isn't absolute and normalized is refused.
  {
    SessionStore relative(Options("relative/user"));
    CHECK(relative.Load().status == LoadStatus::kUnavailable);
    CHECK(!relative.Save(Session()).ok);
    SessionStore dotted(Options(g_base / "x/../y"));
    CHECK(dotted.Load().status == LoadStatus::kUnavailable);
  }
  // An invalid session is never written.
  {
    const auto root = FreshRoot();
    SessionStore store(Options(root));
    auto bad = Session();
    bad.username = "two\nlines";
    const auto saved = store.Save(bad);
    CHECK(!saved.ok && saved.reason == "invalid_session");
    CHECK(!Exists(root / "gog"));
    CHECK(store.Load().status == LoadStatus::kNone);
  }
}

void CorruptFiles() {
  const Bytes good = gog::EncodeSession(Session());
  std::vector<std::pair<const char*, Bytes>> cases;
  {
    auto bytes = good;
    bytes[0] = 'X';
    cases.emplace_back("bad magic", bytes);
  }
  {
    auto bytes = good;
    bytes[8] = 2;
    cases.emplace_back("wrong version", bytes);
  }
  {
    auto bytes = good;
    bytes[13] ^= 0xFF;
    cases.emplace_back("bad crc", bytes);
  }
  {
    auto bytes = good;
    bytes[bytes.size() - 1] ^= 0x01;
    cases.emplace_back("payload changed", bytes);
  }
  {
    auto bytes = good;
    bytes.pop_back();
    cases.emplace_back("truncated", bytes);
  }
  cases.emplace_back("header only", Bytes(good.begin(), good.begin() + 17));
  cases.emplace_back("empty", Bytes());
  {
    auto bytes = good;
    bytes.push_back(0);
    cases.emplace_back("trailing byte", bytes);
  }
  {
    // A valid session with a trailing byte the length covers (CRC fixed up): still trailing data.
    auto bytes = Raw(kToken, "48151623421234567", "Synthetic Player");
    bytes.push_back('!');
    const std::uint32_t size = static_cast<std::uint32_t>(bytes.size() - 17);
    for (int i = 0; i < 4; ++i) bytes[9 + i] = static_cast<std::uint8_t>(size >> (8 * i));
    const auto crc = Crc32(bytes.data() + 17, size);
    for (int i = 0; i < 4; ++i) bytes[13 + i] = static_cast<std::uint8_t>(crc >> (8 * i));
    cases.emplace_back("trailing payload byte", bytes);
  }
  {
    auto bytes = good;
    bytes.resize(gog::kMaxSessionFileBytes + 1, 'A');
    cases.emplace_back("oversized", bytes);
  }
  cases.emplace_back("user id 012", Raw(kToken, "012", "Synthetic Player"));
  cases.emplace_back("user id letters", Raw(kToken, "12a", "Synthetic Player"));
  cases.emplace_back("username newline", Raw(kToken, "48151623421234567", "Synthetic\nPlayer"));
  cases.emplace_back("token space", Raw("Synthetic Token", "48151623421234567", "Synthetic Player"));
  cases.emplace_back("empty token", Raw("", "48151623421234567", "Synthetic Player"));

  for (const auto& [name, bytes] : cases) {
    StoredSession decoded;
    if (gog::DecodeSession(bytes, &decoded)) {
      std::fprintf(stderr, "decoded a corrupt file: %s\n", name);
      CHECK(false);
    }
    const auto root = FreshRoot();
    fs::create_directory(root / "gog");
    CHECK(::chmod((root / "gog").c_str(), 0700) == 0);
    WriteRaw(root / "gog/session", bytes);
    WriteRaw(root / "gog/.session.journal-7-3", good);
    SessionStore store(Options(root));
    const auto loaded = store.Load();
    if (loaded.status != LoadStatus::kCorrupt) {
      std::fprintf(stderr, "not refused as corrupt: %s\n", name);
      CHECK(false);
    }
    CHECK(loaded.session.refresh_token.empty());
    CHECK(EntryCount(root / "gog") == 0);
    // The next start shows signed out.
    SessionStore restarted(Options(root));
    CHECK(restarted.Load().status == LoadStatus::kNone);
  }
}

void PersistOff() {
  // Save keeps the session in memory and writes nothing; a restart finds nothing.
  {
    const auto root = FreshRoot();
    SessionStore store(Options(root, false));
    CHECK(store.Save(Session()).ok);
    CHECK(!Exists(root / "gog/session"));
    CHECK(FilesWithToken(root / "gog") == 0);
    // The same process still has it.
    const auto same = store.Load();
    CHECK(same.status == LoadStatus::kOk && same.session.refresh_token == kToken);
    SessionStore restarted(Options(root, false));
    CHECK(restarted.Load().status == LoadStatus::kNone);
    SessionStore persisting(Options(root, true));
    CHECK(persisting.Load().status == LoadStatus::kNone);
  }
  // Load deletes a file written earlier, unread, with its leftovers.
  {
    const auto root = FreshRoot();
    {
      SessionStore earlier(Options(root, true));
      CHECK(earlier.Save(Session()).ok);
    }
    WriteRaw(root / "gog/.session.journal-9-9", gog::EncodeSession(Session()));
    SessionStore store(Options(root, false));
    CHECK(!store.persist());
    CHECK(store.Load().status == LoadStatus::kNone);
    CHECK(EntryCount(root / "gog") == 0);
  }
  // Corrupt bytes are deleted unread too.
  {
    const auto root = FreshRoot();
    fs::create_directory(root / "gog");
    CHECK(::chmod((root / "gog").c_str(), 0700) == 0);
    WriteText(root / "gog/session", "not a session");
    SessionStore store(Options(root, false));
    CHECK(store.Load().status == LoadStatus::kNone);
    CHECK(EntryCount(root / "gog") == 0);
  }
  // SetPersist(true) writes the memory session; SetPersist(false) deletes the file and keeps the memory copy.
  {
    const auto root = FreshRoot();
    SessionStore store(Options(root, false));
    CHECK(store.Save(Session()).ok);
    CHECK(!Exists(root / "gog/session"));
    CHECK(store.SetPersist(true).ok);
    CHECK(store.persist());
    CHECK(ModeOf(root / "gog/session") == 0600);
    {
      SessionStore restarted(Options(root, true));
      CHECK(restarted.Load().session.refresh_token == kToken);
    }
    CHECK(store.SetPersist(false).ok);
    CHECK(!store.persist());
    CHECK(!Exists(root / "gog/session"));
    CHECK(FilesWithToken(root / "gog") == 0);
    const auto kept = store.Load();
    CHECK(kept.status == LoadStatus::kOk && kept.session.refresh_token == kToken);
    // On again without a session in memory: nothing to write.
    CHECK(store.Clear().ok);
    CHECK(store.SetPersist(true).ok);
    CHECK(!Exists(root / "gog/session"));
  }
  // SetPersist(false) before any Load: the file's session moves to memory, the file goes.
  {
    const auto root = FreshRoot();
    {
      SessionStore earlier(Options(root, true));
      CHECK(earlier.Save(Session()).ok);
    }
    SessionStore store(Options(root, true));
    CHECK(store.SetPersist(false).ok);
    CHECK(!Exists(root / "gog/session"));
    CHECK(store.Load().session.refresh_token == kToken);
  }
}

void ClearRemovesFile() {
  const auto root = FreshRoot();
  SessionStore store(Options(root));
  CHECK(store.Save(Session()).ok);
  WriteRaw(root / "gog/.session.journal-4-2", gog::EncodeSession(Session()));
  CHECK(store.Clear().ok);
  CHECK(EntryCount(root / "gog") == 0);
  CHECK(store.Load().status == LoadStatus::kNone);
  SessionStore restarted(Options(root));
  CHECK(restarted.Load().status == LoadStatus::kNone);

  // A missing folder is fine for every call, and none of them creates it.
  const auto empty = FreshRoot();
  SessionStore none(Options(empty));
  CHECK(none.Load().status == LoadStatus::kNone);
  CHECK(none.Clear().ok);
  CHECK(none.RemoveLeftovers());
  CHECK(none.SetPersist(false).ok);
  CHECK(!Exists(empty / "gog"));
  // So is a missing user root for removal; saving needs it.
  SessionStore gone(Options(g_base / "missing-user-root"));
  CHECK(gone.Load().status == LoadStatus::kNone);
  CHECK(gone.Clear().ok);
  CHECK(!gone.Save(Session()).ok);
  CHECK(!Exists(g_base / "missing-user-root"));
}

// A writer that creates a temporary file holding the encoded session by path, as a failed WriteDurable leaves it.
void LeaveJournal(const fs::path& folder, const std::string& name, const Bytes& bytes) {
  WriteRaw(folder / name, bytes);
}

enum class Fault { kWrite, kRename, kSync };

SessionStore::Writer FaultyWriter(const fs::path& folder, Fault fault) {
  return [folder, fault](const Storage& storage, const std::string& name, const Bytes& bytes) -> Result<void> {
    CHECK(name == "session");
    switch (fault) {
      case Fault::kWrite:
        LeaveJournal(folder, ".session.journal-1-0", bytes);
        errno = ENOSPC;
        return Result<void>::Failure(StorageError::kWriteFailed, "synthetic write failure");
      case Fault::kRename:
        LeaveJournal(folder, ".session.journal-1-0", bytes);
        errno = EIO;
        return Result<void>::Failure(StorageError::kRenameFailed, "synthetic rename failure");
      case Fault::kSync: {
        CHECK(storage.WriteDurable(name, bytes).ok());
        LeaveJournal(folder, ".session.journal-1-1", bytes);
        errno = EIO;
        return Result<void>::Failure(StorageError::kSyncFailed, "synthetic sync failure");
      }
    }
    return Result<void>::Failure(StorageError::kWriteFailed, "unreachable");
  };
}

enum class After { kStartup, kSignOut, kStaySignedInOff };

void FailedWriteLeavesNoToken(Fault fault, After after) {
  const auto root = FreshRoot();
  const auto folder = root / "gog";
  SessionStore store(Options(root, true, FaultyWriter(folder, fault)));
  const auto saved = store.Save(Session());
  CHECK(!saved.ok);
  CHECK(saved.storage_full == (fault == Fault::kWrite));
  CHECK(saved.reason == (fault == Fault::kWrite ? "write_failed" : fault == Fault::kRename ? "rename_failed" : "sync_failed"));
  CHECK(saved.reason.find(kToken) == std::string::npos);
  // The failed save cleaned up after itself, and kept the session for this process.
  CHECK(FilesWithToken(folder) == 0);
  const auto same = store.Load();
  CHECK(same.status == LoadStatus::kOk && same.session.refresh_token == kToken);

  // A crash before that cleanup leaves the temporary files: plant them as the writer did, and one more.
  const Bytes encoded = gog::EncodeSession(Session());
  LeaveJournal(folder, fault == Fault::kSync ? ".session.journal-1-1" : ".session.journal-1-0", encoded);
  LeaveJournal(folder, ".session.journal-999-0", encoded);
  CHECK(FilesWithToken(folder) == 2);

  switch (after) {
    case After::kStartup: {
      SessionStore restarted(Options(root, true));
      CHECK(restarted.Load().status == LoadStatus::kNone);
      break;
    }
    case After::kSignOut:
      CHECK(store.Clear().ok);
      CHECK(store.Load().status == LoadStatus::kNone);
      break;
    case After::kStaySignedInOff:
      CHECK(store.SetPersist(false).ok);
      break;
  }
  CHECK(FilesWithToken(folder) == 0);
  CHECK(EntryCount(folder) == 0);
}

void FaultInjection() {
  for (const auto fault : {Fault::kWrite, Fault::kRename, Fault::kSync})
    for (const auto after : {After::kStartup, After::kSignOut, After::kStaySignedInOff})
      FailedWriteLeavesNoToken(fault, after);
}

void LinkedLeftoverIsKept() {
  const auto root = FreshRoot();
  const auto outside = root / "outside";
  fs::create_directory(outside);
  WriteText(outside / "target", "keep me");
  fs::create_directory(root / "gog");
  CHECK(::chmod((root / "gog").c_str(), 0700) == 0);
  fs::create_symlink(outside / "target", root / "gog/.session.journal-x");
  WriteRaw(root / "gog/.session.journal-2-0", gog::EncodeSession(Session()));
  WriteText(root / "gog/unrelated", "other file");
  SessionStore store(Options(root));
  CHECK(!store.RemoveLeftovers());
  CHECK(IsLink(root / "gog/.session.journal-x"));
  CHECK(ReadAll(outside / "target") == "keep me");
  // The regular leftover beside it still goes; other names are not touched.
  CHECK(!Exists(root / "gog/.session.journal-2-0"));
  CHECK(Exists(root / "gog/unrelated"));
  // Sign-out can't report success while it stays.
  CHECK(!store.Clear().ok);
  fs::remove(root / "gog/.session.journal-x");

  // A folder with the prefix is a failure too, and stays.
  fs::create_directory(root / "gog/.session.journal-dir");
  CHECK(!store.RemoveLeftovers());
  CHECK(Exists(root / "gog/.session.journal-dir"));
  fs::remove(root / "gog/.session.journal-dir");
  CHECK(store.RemoveLeftovers());
}

void ForgetIfNotPersisted() {
  const auto root = FreshRoot();
  {
    SessionStore store(Options(root));
    CHECK(store.Save(Session()).ok);
  }
  WriteRaw(root / "gog/.session.journal-3-1", gog::EncodeSession(Session()));
  CHECK(SessionStore::ForgetIfNotPersisted(root.string()));
  CHECK(EntryCount(root / "gog") == 0);
  // Nothing there: a no-op, and no folder is made.
  const auto empty = FreshRoot();
  CHECK(SessionStore::ForgetIfNotPersisted(empty.string()));
  CHECK(!Exists(empty / "gog"));
}

}  // namespace

int main(int argc, char** argv) {
  // A folder under the build tree, given by ctest, so no component is a link (macOS /tmp is one).
  CHECK(argc == 2);
  const fs::path parent(argv[1]);
  fs::remove_all(parent);
  fs::create_directories(parent);
  g_base = fs::canonical(parent);
  FormatIsPinned();
  RoundTrip();
  Refusals();
  CorruptFiles();
  PersistOff();
  ClearRemovesFile();
  FaultInjection();
  LinkedLeftoverIsKept();
  ForgetIfNotPersisted();
  fs::remove_all(g_base);
  std::printf("gog session store ok\n");
  return 0;
}
