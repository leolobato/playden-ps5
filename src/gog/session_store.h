// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The GOG session on disk: <user root>/gog/session, 0600 in a 0700 folder, with no link in any component.
// Nothing here logs; callers log "session save result=<kind>" only, never a token, the user ID or the username.
//
// File: magic "PDGOGS\0\1", version u8 = 1, payload length u32 LE, CRC-32 of the payload u32 LE, then the payload:
// three u16-LE-length-prefixed strings, the refresh token, the user ID and the username, and nothing after them.

#include "platform/storage.h"

#include <cstddef>
#include <functional>
#include <mutex>
#include <string>

namespace playden::gog {

struct StoredSession {
  std::string refresh_token;  // 1-4096 bytes of printable ASCII, no space
  std::string user_id;        // canonical decimal: 1-20 digits, no leading zero
  std::string username;       // 1-256 bytes, no control characters

  // Copied, never moved: a moved-from short string keeps its bytes in its inline buffer, so each copy wipes its own.
  StoredSession() = default;
  StoredSession(const StoredSession&) = default;
  StoredSession& operator=(const StoredSession&) = default;
  // Zeroes all three fields (platform::ZeroizeSecret).
  void Wipe();
  ~StoredSession() { Wipe(); }
};

enum class LoadStatus {
  kNone,         // nothing stored: signed out
  kOk,           // session holds the stored session
  kCorrupt,      // the file was unreadable as a session; it and its temporary files were removed
  kUnavailable,  // the folder or the file is unsafe (a link, wrong mode or owner) or can't be read; nothing changed
};

struct LoadResult {
  LoadStatus status = LoadStatus::kNone;
  StoredSession session;      // kOk only
  bool storage_full = false;  // never set by Load (it writes nothing); kept for callers that share the field
  std::string reason;         // short, for the log: no path, token or account data
};

struct SaveResult {
  bool ok = false;
  bool storage_full = false;  // the write failed with ENOSPC (or EDQUOT): console storage is full
  std::string reason;         // short, for the log: no path, token or account data
};

class SessionStore {
 public:
  using Writer = std::function<Result<void>(const Storage& storage, const std::string& name, const Bytes& bytes)>;

  struct Options {
    std::string user_root;  // absolute, normalized, no link: /data/playden/user on the console
    bool persist = true;    // "Stay signed in"
    Writer write;           // test hook; empty means Storage::WriteDurable
  };

  explicit SessionStore(Options options);
  ~SessionStore();
  SessionStore(const SessionStore&) = delete;
  SessionStore& operator=(const SessionStore&) = delete;

  // Startup: removes leftover temporary files first. A session saved in this process (kept in memory) wins over the
  // file. With persist off, the file is deleted unread and only the memory copy is returned (kNone without one).
  LoadResult Load();
  // Keeps the session in memory, then writes it when persist is on. A failed write removes the session file and the
  // temporary files, so the next start shows signed out; the memory copy keeps serving this process.
  SaveResult Save(const StoredSession& session);
  // Sign-out: wipes the memory copy and deletes the file and the temporary files. ok only once all of them are gone.
  SaveResult Clear();
  // On: writes the memory copy when there is one. Off: deletes the file and the temporary files, keeping the memory
  // copy (read from the file first when there is none yet).
  SaveResult SetPersist(bool persist);
  bool persist() const;

  // Removes every regular file in <user root>/gog whose name starts with ".session.journal-" (what a failed
  // Storage::WriteDurable leaves). Never follows a link: a link or folder with that name is a failure and stays.
  // A missing folder is success, and nothing is created.
  bool RemoveLeftovers();

  // Deletes the session file and the temporary files, for a start where "Stay signed in" is off and the GOG store is
  // not in use. A missing folder is success, and nothing is created.
  static bool ForgetIfNotPersisted(const std::string& user_root);

 private:
  bool RemoveLeftoversLocked();
  bool RemoveFileLocked();
  SaveResult WriteLocked(const StoredSession& session);
  LoadResult ReadLocked();

  mutable std::mutex mutex_;
  std::string user_root_;
  bool persist_;
  Writer write_;
  bool has_memory_ = false;
  StoredSession memory_;
};

// The file's bytes for a session, or empty when a field is out of range. Exposed for tests.
Bytes EncodeSession(const StoredSession& session);
// False for anything but exactly one well-formed session. Exposed for tests.
bool DecodeSession(const Bytes& bytes, StoredSession* session);

inline constexpr char kSessionFileName[] = "session";
inline constexpr char kSessionFolderName[] = "gog";
inline constexpr char kSessionLeftoverPrefix[] = ".session.journal-";
inline constexpr std::size_t kMaxSessionFileBytes = 16 * 1024;

}  // namespace playden::gog
