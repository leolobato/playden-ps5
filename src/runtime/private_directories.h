#pragma once
#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>
#include <sys/types.h>
namespace playden::runtime {
// Folders of a private tree that may also be 0711 (kPrivateTraversable): others can
// pass through them, never list them. Absolute paths at or under private_root; the
// ones EnsurePrivateDirectories creates get created_mode (0700 or 0711). Empty:
// the whole tree is 0700.
struct PrivateTraversal {
  std::vector<std::filesystem::path> folders;
  mode_t created_mode = 0700;
};
// Creates directories as the current effective user, without credential changes.
// boundary is an existing trusted directory; private_root and descendants must
// belong to that user and have mode 0700 (or 0711 for traversal folders).
// Existing foreign/public directories in the private tree are rejected, never
// chmodded or taken over (startup's self-heal, runtime/self_heal.h, only takes extra
// bits away from the app's own). Missing folders between boundary and private_root are
// created 01777.
bool EnsurePrivateDirectories(const std::filesystem::path& boundary,
                              const std::filesystem::path& private_root,
                              const std::filesystem::path& directory,
                              std::string* error,
                              const PrivateTraversal& traversal = {});
bool IsPrivateDirectoryTree(const std::filesystem::path& private_root,
                            const std::filesystem::path& directory,
                            const PrivateTraversal& traversal = {});
// traversable: also admits 0711.
bool IsPrivateDirectory(const std::filesystem::path& directory, bool traversable = false);
// Sets an app-owned private folder (0700 or 0711) to mode, 0700 or 0711, through a descriptor
// that is not a link. Missing, and folders of other modes or owners, are left: true. False only
// when the change itself failed.
bool SetTraversableMode(const std::filesystem::path& folder, mode_t mode);
// Opens a tree to everyone (FTP runs as root, which the console gives no permission bypass):
// app-owned folders become 0777, app-owned regular files 0666 with their execute bits kept.
// Links are never followed, entries of other owners are neither changed nor entered, files
// with more than one link are left alone, and the walk is bounded. Every ancestor and child
// opens by descriptor without following links, including on the console via the syscall gateway.
// Listed credential subtrees stay 0700/0600. ok: the root is missing, or an owned folder that could be listed and
// nothing changed under the walk; truncated: the walk stopped at its bound; skipped counts
// entries that could not be opened or changed, linked the files with other links.
struct SharedTreeResult {
  bool ok = false;
  bool truncated = false;
  bool changed_under = false;
  std::size_t changed = 0;
  std::size_t skipped = 0;
  std::size_t linked = 0;
};
// Launcher credentials under the Windows profile. Empty until a launcher writes credentials
// here; game saves and emulator settings are not sign-in tokens.
const std::vector<std::filesystem::path>& PrivateProfilePaths();
bool IsPrivateProfilePath(const std::filesystem::path& relative,
                          const std::vector<std::filesystem::path>& private_paths);
// before_open runs before each entry opens (race tests).
struct SharedTreeOptions {
  std::vector<std::filesystem::path> private_paths = PrivateProfilePaths();
  std::function<void(const std::filesystem::path&)> before_open;
};
SharedTreeResult OpenTreeToEveryone(const std::filesystem::path& root, const SharedTreeOptions& options = {});
// Whether folders are opened relative to a descriptor (*at calls): not on the console.
bool DescriptorRelativeOpens();
// A folder opened O_DIRECTORY|O_NOFOLLOW and checked to be the entry lstat saw. -1: errno ELOOP
// for a link, ENOTDIR, ESTALE when it changed in between.
int OpenFolderNoFollow(const std::filesystem::path& path);
// parent_path/name, a folder, opened without following a link: by openat(parent_fd) unless
// by_path, else by path once the parent's path still names parent_fd, checked to be the entry
// lstat saw. -1: errno ELOOP for a link, ENOTDIR, ESTALE when the parent or the entry changed.
int OpenChildFolderNoFollow(int parent_fd, const std::filesystem::path& parent_path, const std::string& name,
                            bool by_path = !DescriptorRelativeOpens());
}
