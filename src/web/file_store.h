// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "platform/managed_directory.h"
#include "web/file_policy.h"

#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include <sys/stat.h>
#include <sys/types.h>

namespace playden::web {

// One named folder of a game: the only places the Files tab reaches.
struct FileRoot {
  std::string path;         // absolute, as this process opens it
  std::string create_from;  // existing folder below which a missing root is made; empty: never made
  mode_t folder_mode = 0700;
  mode_t file_mode = 0600;
  bool game_folder = false;  // may be on a file system with no owners (exFAT): asked at the first write
  std::vector<std::string> private_paths;  // '/'-separated, relative to the root: hidden like Playden's own files
};

// An answer other than success: an HTTP status and words for the player. status 0: success.
struct FileError {
  int status = 0;
  std::string message;
  bool ok() const { return status == 0; }
};

struct PreviousVersion {
  std::uint64_t size = 0;
  std::int64_t modified = 0;
};

struct FileEntry {
  std::string name;
  bool directory = false;
  std::uint64_t size = 0;
  std::int64_t modified = 0;
  bool read_only = false;
  std::optional<PreviousVersion> previous;
  // Only a previous version is left (a crash between a replace's two renames): it can be restored.
  bool recoverable = false;
};

inline constexpr std::size_t kMaxListedEntries = 5000;

struct FileListing {
  bool exists = false;
  bool read_only = false;
  bool truncated = false;
  std::size_t unshown = 0;  // names the page can't show (invalid UTF-8) and files with another hard link
  std::vector<FileEntry> entries;
};

struct OpenedFile {
  int fd = -1;  // the caller's to close
  std::uint64_t size = 0;
  std::string name;
};

// What a walk over a folder found, or what its delete did.
struct TreeSummary {
  std::uint64_t files = 0;    // not counting a previous version beside its file
  std::uint64_t folders = 0;  // the folder itself too
  std::uint64_t bytes = 0;
  std::uint64_t left = 0;     // what a delete leaves: see DeleteTree
};

// A file read and hashed for the editor.
struct FileText {
  std::string text;  // as UTF-8, with any byte order mark left out
  std::string encoding = "utf-8";
  bool bom = false;
  std::string sha256;
};
inline constexpr std::size_t kMaxTextBytes = 1024 * 1024;

// A folder Play searches for a game's redistributables (runtime/redist_names.h), as far as the page needs.
struct RedistFolderInfo {
  std::string path;  // relative to the game folder
  std::size_t entries = 0;
  std::uint64_t bytes = 0;            // of the files directly in it
  std::vector<std::string> sample;    // a few of its names
  std::string physx_installer;        // what Play would install from here, relative to the game folder; empty: none
};

// What a move does when the destination already has that name (compared without regard to case).
enum class MoveClash {
  kAsk,       // nothing changes: 409, and the summary names what is there
  kReplace,   // a file replaces the one there, which is kept as its previous version; a folder merges into the one there
  kKeepBoth,  // the moved file or folder takes a free numbered name: "save (2).dat"
};

// What a move did, or why it asks.
struct MoveSummary {
  std::string name;          // the name it has in the destination
  std::uint64_t files = 0;   // moved
  std::uint64_t folders = 0;
  std::uint64_t left = 0;    // stayed where it was, with the folders that hold it: what DeleteTree leaves
  bool clash = false;        // kAsk only: the destination has that name (status 409)
  bool clash_folder = false; // and it is a folder
};

// How far a move across roots is. Totals are what a summary of the source found before the copy began, so bytes can
// pass total_bytes when a file grows meanwhile.
struct MoveProgress {
  std::uint64_t bytes = 0;  // copied so far, previous versions too
  std::uint64_t total_bytes = 0;
  std::uint64_t files = 0;  // moved so far
  std::uint64_t total_files = 0;
  std::string name;         // the file being copied
};
// Called as a move across roots goes on, on the thread that moves. False stops the move within a chunk: the file being
// copied stays where it was, and a second run goes on with what is left.
using MoveObserver = std::function<bool(const MoveProgress&)>;

struct UploadOptions {
  std::uint64_t length = 0;
  bool overwrite = false;
  std::optional<std::string> if_sha256;  // the file must still have this hash at the commit
};

// Reads up to size bytes of the upload: the count, 0 at the end, -1 when the client went away.
using UploadReader = std::function<std::ptrdiff_t(char* data, std::size_t size)>;
// Writes bytes to the client; false when it went away.
using ByteWriter = std::function<bool(const char* data, std::size_t size)>;

// Reads and changes files under FileRoots. Called from the web server's workers, at the same time.
//
// Root is process-wide and has no permission bypass on /data: while another thread has raised it, opening a private
// path fails, and a file made then belongs to root. So every path operation runs under try_hold, which keeps root
// from being raised (ps5::TryHoldEffectiveUid). It is never held while bytes move, because the runtime worker and
// durable storage wait on that same lock. The runtime worker holds it for a whole batch, such as a game's setup:
// file operations then answer 503.
//
// This protects against a request that tries to leave a root. It doesn't protect against a program writing on the
// console at the same time (FTP runs as root, games as this user): a folder swapped for a link in the middle of a
// request can redirect it. Each step refuses a link it sees, and a failed check never cleans up through that context.
class FileStore {
 public:
  struct Options {
    // Keeps root from being raised while the result lives; null while another thread holds it. Empty: no such lock.
    std::function<std::shared_ptr<void>()> try_hold;
    int hold_retry_ms = 100;
    int hold_tries = 50;
    // Bytes that may still be written where this path is; nothing when unknown (the upload goes ahead).
    std::function<std::optional<std::uint64_t>(const std::string& path)> free_space;
    // Random text for temporary names. Empty: RandomUuid.
    std::function<std::optional<std::string>()> random_id;
    // Seconds since 1970, for the age of leftovers. Empty: the system clock.
    std::function<std::int64_t()> now;
    std::int64_t stale_seconds = 3600;
    // Test hook: called before each step of a commit with its name; false fails the step as a disk error would.
    std::function<bool(const std::string& step)> fault;
  };

  explicit FileStore(Options options);

  FileError List(const FileRoot& root, const std::string& path, FileListing* out);
  FileError OpenForRead(const FileRoot& root, const std::string& path, bool previous, OpenedFile* out);
  FileError ReadText(const FileRoot& root, const std::string& path, FileText* out);
  // A complete upload, or a refusal before anything changed (the old file stays). *replaced: a file was replaced.
  FileError Upload(const FileRoot& root, const std::string& path, const UploadOptions& options, const UploadReader& read,
                   bool* replaced);
  // One new folder, in a folder that is there. With parents: makes each level that's missing, and a folder already
  // there is fine; a file in the way is 409 and named.
  FileError MakeFolder(const FileRoot& root, const std::string& path, bool parents = false);
  // A file (with its previous version) or an empty folder.
  FileError Delete(const FileRoot& root, const std::string& path);
  // A folder and everything in it that the page can show. What stays, with the folders that hold it: hidden and secret
  // names, read-only and private folders, links, files with another hard link, names the page can't show, temporary
  // files still in use. A file or folder the console won't let this user remove stays too. Locks one batch at a time,
  // so a long delete keeps nothing else waiting for long. Stopped by an error, a second run goes on with what is left.
  FileError DeleteTree(const FileRoot& root, const std::string& path, TreeSummary* out);
  // What DeleteTree would do, changing nothing.
  FileError SummarizeTree(const FileRoot& root, const std::string& path, TreeSummary* out);
  // Moves or renames a file or folder (with any previous versions) to folder/name, in the same root or another root of
  // the same game. In one root it renames, so a folder goes whole. Across roots it copies each file the page can show
  // into a temporary there, publishes it whole and deletes the source after that: a stop leaves each file whole on
  // one side or the other, and a second run goes on with what is left. What stays is what DeleteTree leaves. A path
  // that holds or is a private path, or passes through a read-only folder, is refused on either side.
  FileError Move(const FileRoot& from, const std::string& path, const FileRoot& to, const std::string& folder,
                 const std::string& name, MoveClash clash, MoveSummary* out, const MoveObserver& observe = {});
  // Swaps a file with its previous version, or brings back a previous version with no file.
  FileError Restore(const FileRoot& root, const std::string& path);
  // A folder as a zip, its entries named from the folder's own name down. Before any byte goes out, a folder too
  // large for a zip is 413. *started: bytes went out, so a later failure can only cut the connection.
  FileError WriteZip(const FileRoot& root, const std::string& path, std::int64_t modified, const ByteWriter& write,
                     const std::function<bool()>& begin, bool* started);
  // The game folder's redistributable folders that exist, by Play's own rules.
  FileError RedistFolders(const FileRoot& root, std::vector<RedistFolderInfo>* out);
  // Removes the leftovers of an upload that could not be cleaned up at the time, when the lock is free now.
  void CleanUpDeferred();
  std::size_t DeferredCleanups();

  // Runs work under try_hold, with the same retries as a file operation; 503 when it stays busy.
  FileError WithHold(const std::function<FileError()>& work);

 private:
  std::shared_ptr<void> Hold();
  FileError Locate(const FileRoot& root, const std::vector<std::string>& parts, bool writing, bool create_root,
                   std::optional<platform::ManagedDirectory>* out);
  std::optional<platform::DirectoryPolicy> WritePolicy(const FileRoot& root);
  bool Step(const std::string& step);
  FileError WalkTree(const FileRoot& root, const std::string& path, bool remove, TreeSummary* out);
  std::int64_t Now() const;
  std::optional<std::string> RandomText();
  void Sweep(const platform::ManagedDirectory& folder);
  void DeferCleanup(const FileRoot& root, std::string folder, std::string name, platform::DirectoryPolicy policy);
  bool RemoveTemporary(const std::string& folder, const std::string& name, platform::DirectoryPolicy policy);
  struct MoveFrame;
  struct MoveTally;
  FileError MoveEntries(const FileRoot& from, const FileRoot& to, std::vector<MoveFrame>* stack, MoveSummary* out, MoveTally* tally);
  FileError RenameFile(const platform::ManagedDirectory& from, const std::string& name, const platform::ManagedDirectory& to,
                       const std::string& target, bool replace);
  FileError CopyFile(const FileRoot& from, const std::vector<std::string>& source, const FileRoot& to,
                     const std::vector<std::string>& target, bool replace, MoveTally* tally);
  FileError StageCopy(const FileRoot& to, const std::vector<std::string>& folder, int fd, std::uint64_t size,
                      std::string* temporary, std::string* folder_path, platform::DirectoryPolicy* policy, MoveTally* tally);
  void DropTemporary(const FileRoot& root, const std::string& folder_path, const std::string& temporary,
                     platform::DirectoryPolicy policy);
  FileError CaseRename(const platform::ManagedDirectory& folder, const std::string& name, const std::string& target);
  bool CommitUpload(const FileRoot& root, const std::vector<std::string>& parts, const std::string& temporary,
                    const UploadOptions& options, const struct stat* expected, bool* replaced, FileError* error);

  Options options_;
  std::mutex state_mutex_;
  // One commit at a time: its renames are separate steps, and a no-replace rename is only a check.
  std::mutex commit_mutex_;
  std::map<std::string, bool> records_ownership_;  // game folder path -> file system keeps owners
  std::set<std::string> active_uploads_;           // absolute paths of temporary files being written
  struct Deferred {
    std::string folder;
    std::string name;
    platform::DirectoryPolicy policy;
  };
  std::vector<Deferred> deferred_;
};

}  // namespace playden::web
