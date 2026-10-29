#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>
#include <sys/stat.h>

namespace playden::platform {

enum class DirectoryBackend { kDescriptorRelative, kAbsolutePath };
enum class DirectoryPolicy { kApprovedTree, kPrivate, kPrivateTraversable, kUnownedTree };

// Operations assume a stable managed namespace and coordinated writers. The
// absolute-path backend checks components and observed identities, but is NOT a
// race-proof boundary against concurrent ancestor replacement. A failed
// post-operation check can mean effects occurred. Never retry cleanup through
// that context without reconciling its identity. No process CWD is changed.
//
// kPrivate requires current-EUID ownership and mode 0700 from the selected root
// downward; ancestors above that root may be public. kPrivateTraversable is
// kPrivate that also admits 0711 (others may pass through, never list), for the
// bottle's way to its user profile. A kPrivate folder only opens kPrivate
// children; a kPrivateTraversable one opens kPrivate or kPrivateTraversable
// children; neither opens other policies. kApprovedTree accepts
// existing directory ownership/modes, then refuses identity/owner/mode drift.
// kUnownedTree is kApprovedTree for a filesystem that records no owner or
// permissions (exFAT: every entry shows the mount owner and 0777): file
// operations skip owner and mode checks, and a rename is checked by device,
// type and size because such filesystems may renumber renamed files. All
// policies reject symlinks in every path component. Caller owns root authorization,
// stopped-title checks, journals, file-content validation and collision policy.
// Whether files created in `directory` keep the owner and permissions they are
// given: creates a probe file, sets it to 0600 and reads it back. Nothing when
// the probe cannot be written. On exFAT the answer is false.
std::optional<bool> RecordsOwnership(const std::filesystem::path& directory);
// Test hook: replaces the probe until reset with nullptr.
void SetOwnershipProbeForTesting(std::optional<bool> (*probe)(const std::filesystem::path&));

class ManagedDirectory {
 public:
  static DirectoryBackend DefaultBackend();
  static std::optional<ManagedDirectory> Open(
      const std::filesystem::path& absolute_path, DirectoryPolicy policy,
      DirectoryBackend backend = DefaultBackend());

  ManagedDirectory(ManagedDirectory&& other) noexcept;
  ManagedDirectory& operator=(ManagedDirectory&& other) noexcept;
  ~ManagedDirectory();
  ManagedDirectory(const ManagedDirectory&) = delete;
  ManagedDirectory& operator=(const ManagedDirectory&) = delete;

  // A folder it creates gets created_mode exactly (umask ignored); an existing
  // folder is never chmodded. kPrivate children are always created 0700,
  // kPrivateTraversable ones 0700 or 0711. (Startup's self-heal, runtime/self_heal.h,
  // tightens the app's own private state before anything opens it.)
  std::optional<ManagedDirectory> OpenChild(const std::string& name,
                                           DirectoryPolicy policy,
                                           bool create = false,
                                           mode_t created_mode = 0700) const;
  // Open/OpenChild never sync implicitly. After creation the caller must sync
  // the new directory and its parent before recording durable completion.
  bool Validate() const;
  // Returned file descriptors belong to the caller. Reads are nonblocking so
  // a FIFO cannot hang admission. expected_mode compares all permission bits.
  int OpenRegularFile(const std::string& name, bool require_owner = false,
                      std::optional<mode_t> expected_mode = std::nullopt) const;
  // Sets/verifies mode and ownership on the new file before returning it. No
  // payload has been written. Failure can leave an empty file for reconciliation.
  int CreateExclusiveFile(const std::string& name, mode_t mode) const;
  // Missing is false/ENOENT; only ordinary files/directories are admitted.
  bool InspectChild(const std::string& name, struct stat* result) const;
  // Caller admits source/destination content and chooses replacement. Existing
  // directories are never replaced. Source and existing destination must be
  // owned by the captured EUID. The caller must sync both parents afterward.
  // replace_existing=false is a checked absence policy, not an atomic
  // no-replace primitive against concurrent writers.
  bool RenameChild(const std::string& from, const ManagedDirectory& destination,
                   const std::string& to, bool replace_existing = true) const;
  bool RemoveRegularFile(const std::string& name) const;
  bool RemoveEmptyDirectory(const std::string& name) const;
  bool Sync() const;

  int fd() const { return fd_; }  // Borrowed, for fsync/fstat only; do not close.
  const std::filesystem::path& path() const { return path_; }
  const std::filesystem::path& root() const { return root_; }
  DirectoryPolicy policy() const { return policy_; }
  DirectoryBackend backend() const { return backend_; }

 private:
  struct Observation {
    std::filesystem::path path;
    struct stat identity;
    DirectoryPolicy check = DirectoryPolicy::kApprovedTree;  // kPrivate*: mode rechecked
  };
  ManagedDirectory() = default;
  bool ValidName(const std::string& name) const;
  bool Inspect(const std::string& name, struct stat* result) const;
  int OpenFile(const std::string& name, int flags, mode_t mode) const;
  bool Remove(const std::string& name, bool directory) const;
  int fd_ = -1;
  std::filesystem::path root_, path_;
  DirectoryPolicy policy_ = DirectoryPolicy::kApprovedTree;
  DirectoryBackend backend_ = DirectoryBackend::kAbsolutePath;
  uid_t uid_ = 0;
  std::vector<Observation> observations_;
};

}  // namespace playden::platform
