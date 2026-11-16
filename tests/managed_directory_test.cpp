#include "platform/managed_directory.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "line %d: %s (errno %d)\n", __LINE__, #x, errno); std::exit(1); } } while (0)
namespace fs = std::filesystem;
using playden::platform::ManagedDirectory;
using playden::platform::DirectoryBackend;
using playden::platform::DirectoryPolicy;

namespace {
struct Fixture {
  fs::path root;
  Fixture() {
    char pattern[] = "/tmp/playden-managed-directory-XXXXXX";
    CHECK(::mkdtemp(pattern));
    root = fs::canonical(pattern);
  }
  ~Fixture() { fs::remove_all(root); }
};

void ReadWriteRenameAndRemove(DirectoryBackend backend) {
  Fixture f;
  CHECK(::chmod(f.root.c_str(), 0755) == 0);
  auto root = ManagedDirectory::Open(f.root, DirectoryPolicy::kApprovedTree, backend);
  CHECK(root && root->Validate() && root->backend() == backend);
  auto private_dir = root->OpenChild("private", DirectoryPolicy::kPrivate, true);
  CHECK(private_dir && private_dir->root() == f.root / "private");
  auto child = private_dir->OpenChild("nested", DirectoryPolicy::kPrivate, true);
  CHECK(child && child->root() == private_dir->root());
  CHECK(!private_dir->OpenChild("public", DirectoryPolicy::kApprovedTree, true));
  struct stat info{};
  CHECK(::fstat(child->fd(), &info) == 0 && info.st_uid == ::geteuid() && (info.st_mode & 07777) == 0700);
  const int file = child->CreateExclusiveFile("temporary", 0600);
  CHECK(file >= 0);
  CHECK(::fstat(file, &info) == 0 && S_ISREG(info.st_mode) && info.st_uid == ::geteuid() && (info.st_mode & 07777) == 0600);
  CHECK(::write(file, "hello", 5) == 5 && ::fsync(file) == 0 && ::close(file) == 0);
  CHECK(child->CreateExclusiveFile("temporary", 0600) < 0 && errno == EEXIST);
  CHECK(child->RenameChild("temporary", *private_dir, "record"));
  CHECK(child->Sync() && private_dir->Sync());
  int input = private_dir->OpenRegularFile("record", true, 0600);
  char bytes[6]{};
  CHECK(input >= 0 && ::read(input, bytes, sizeof(bytes)) == 5 && std::string(bytes) == "hello");
  CHECK(::close(input) == 0);
  CHECK(private_dir->OpenRegularFile("record", true, 0644) < 0);
  CHECK(private_dir->InspectChild("record", &info) && S_ISREG(info.st_mode));
  CHECK(!private_dir->RemoveEmptyDirectory("record"));
  CHECK(!private_dir->RemoveRegularFile("nested"));
  CHECK(private_dir->RemoveRegularFile("record") && private_dir->RemoveEmptyDirectory("nested") && private_dir->Sync());
  CHECK(!fs::exists(f.root / "private/record"));
  auto moved = std::move(*private_dir);
  CHECK(moved.Validate() && !private_dir->Validate());
}

void RejectTraversalLinksAndSpecials(DirectoryBackend backend) {
  Fixture f;
  auto root = ManagedDirectory::Open(f.root, DirectoryPolicy::kPrivate, backend);
  CHECK(root);
  for (const std::string& name : std::vector<std::string>{"", ".", "..", "../outside", "a/b", "/absolute", "a\\b", std::string("x\0y", 3)}) {
    CHECK(!root->OpenChild(name, DirectoryPolicy::kPrivate, true));
    CHECK(root->CreateExclusiveFile(name, 0600) < 0);
    CHECK(root->OpenRegularFile(name) < 0);
    CHECK(!root->RemoveRegularFile(name));
  }
  CHECK(!ManagedDirectory::Open("relative", DirectoryPolicy::kApprovedTree, backend));
  CHECK(!ManagedDirectory::Open(f.root / "../other", DirectoryPolicy::kApprovedTree, backend));
  CHECK(!ManagedDirectory::Open(f.root.string() + "//real", DirectoryPolicy::kApprovedTree, backend));
  CHECK(!ManagedDirectory::Open(f.root.string() + "/", DirectoryPolicy::kApprovedTree, backend));
  CHECK(!ManagedDirectory::Open(fs::path(f.root.string() + std::string("/a\0b", 4)), DirectoryPolicy::kApprovedTree, backend));
  CHECK(root->CreateExclusiveFile(std::string(8192, 'x'), 0600) < 0 && errno == ENAMETOOLONG);
  CHECK(::mkfifo((f.root / "fifo").c_str(), 0600) == 0);
  CHECK(root->OpenRegularFile("fifo") < 0);
  struct stat info{};
  CHECK(!root->InspectChild("fifo", &info) && !root->RemoveRegularFile("fifo"));
  fs::create_directory(f.root / "real");
  fs::create_directory_symlink("real", f.root / "linked");
  fs::create_symlink("fifo", f.root / "file-link");
  CHECK(!ManagedDirectory::Open(f.root / "linked", DirectoryPolicy::kApprovedTree, backend));
  CHECK(!root->OpenChild("linked", DirectoryPolicy::kApprovedTree));
  CHECK(root->OpenRegularFile("file-link") < 0 && !root->RemoveRegularFile("file-link"));
  CHECK(root->CreateExclusiveFile("bad-mode", 04700) < 0);
  CHECK(!fs::exists(f.root / "bad-mode"));
}

void RejectPolicyAndIdentityDrift(DirectoryBackend backend, bool replacement_link) {
  Fixture f;
  fs::create_directory(f.root / "parent");
  CHECK(::chmod((f.root / "parent").c_str(), 0755) == 0);
  CHECK(!ManagedDirectory::Open(f.root / "parent", DirectoryPolicy::kPrivate, backend));
  auto parent = ManagedDirectory::Open(f.root / "parent", DirectoryPolicy::kApprovedTree, backend);
  CHECK(parent);
  auto child = parent->OpenChild("child", DirectoryPolicy::kPrivate, true);
  CHECK(child);
  CHECK(::chmod(child->path().c_str(), 0755) == 0);
  CHECK(!child->Validate() && child->CreateExclusiveFile("forbidden", 0600) < 0);
  CHECK(::chmod(child->path().c_str(), 0700) == 0);
  CHECK(child->Validate());
  CHECK(::chmod(parent->path().c_str(), 0700) == 0);
  CHECK(!child->Validate());
  CHECK(::chmod(parent->path().c_str(), 0755) == 0);
  fs::rename(f.root / "parent", f.root / "original");
  fs::create_directory(f.root / "replacement");
  if (replacement_link) fs::create_directory_symlink("replacement", f.root / "parent");
  else fs::rename(f.root / "replacement", f.root / "parent");
  CHECK(!parent->Validate() && !child->Validate());
  CHECK(parent->CreateExclusiveFile("forbidden", 0600) < 0);
  CHECK(!parent->OpenChild("forbidden", DirectoryPolicy::kPrivate, true));
  CHECK(!parent->Sync());
  CHECK(fs::is_empty(replacement_link ? f.root / "replacement" : f.root / "parent"));
  CHECK(fs::is_empty(f.root / "original/child"));
}

void RenameAdmission(DirectoryBackend backend) {
  Fixture f;
  auto root = ManagedDirectory::Open(f.root, DirectoryPolicy::kPrivate, backend);
  CHECK(root);
  auto stage = root->OpenChild("staged", DirectoryPolicy::kPrivate, true);
  CHECK(stage && root->RenameChild("staged", *root, "published", false));
  CHECK(!stage->Validate());
  auto reopened = root->OpenChild("published", DirectoryPolicy::kPrivate);
  CHECK(reopened);
  for (const char* name : {"source", "existing"}) {
    const int fd = root->CreateExclusiveFile(name, 0600);
    CHECK(fd >= 0 && ::close(fd) == 0);
  }
  CHECK(!root->RenameChild("source", *root, "existing", false) && errno == EEXIST);
  CHECK(fs::exists(f.root / "source"));
  CHECK(!root->RenameChild("source", *root, "published"));
  CHECK(root->RenameChild("source", *root, "existing"));
  auto other_backend = ManagedDirectory::Open(f.root, DirectoryPolicy::kPrivate,
      backend == DirectoryBackend::kAbsolutePath ? DirectoryBackend::kDescriptorRelative : DirectoryBackend::kAbsolutePath);
#if defined(__PROSPERO__) || (defined(PLAYDEN_PLATFORM_PS5) && PLAYDEN_PLATFORM_PS5)
  CHECK(!other_backend && errno == ENOTSUP);
#else
  CHECK(other_backend && !root->RenameChild("existing", *other_backend, "other"));
#endif
  CHECK(fs::exists(f.root / "existing") && !fs::exists(f.root / "other"));
  auto removed = root->OpenChild("removed", DirectoryPolicy::kPrivate, true);
  CHECK(removed);
  fs::rename(f.root / "removed", f.root / "moved");
  fs::create_directory(f.root / "removed");
  CHECK(!root->RenameChild("existing", *removed, "record"));
  CHECK(fs::exists(f.root / "existing") && fs::is_empty(f.root / "removed"));
}

void ForeignOwnershipWhenPrivileged(DirectoryBackend backend) {
  if (::geteuid() != 0) return;  // Exercise in privileged CI; no credential changes.
  Fixture f;
  auto root = ManagedDirectory::Open(f.root, DirectoryPolicy::kPrivate, backend);
  CHECK(root);
  const int file = root->CreateExclusiveFile("foreign", 0600);
  CHECK(file >= 0 && ::fchown(file, 1, static_cast<gid_t>(-1)) == 0 && ::close(file) == 0);
  CHECK(root->OpenRegularFile("foreign", true) < 0);
  CHECK(!root->RemoveRegularFile("foreign"));
  CHECK(!root->RenameChild("foreign", *root, "renamed"));
  CHECK(fs::exists(f.root / "foreign"));
  fs::create_directory(f.root / "foreign-dir");
  CHECK(::chmod((f.root / "foreign-dir").c_str(), 0700) == 0);
  CHECK(::chown((f.root / "foreign-dir").c_str(), 1, static_cast<gid_t>(-1)) == 0);
  CHECK(!root->OpenChild("foreign-dir", DirectoryPolicy::kPrivate));
  auto approved = ManagedDirectory::Open(f.root / "foreign-dir", DirectoryPolicy::kApprovedTree, backend);
  CHECK(approved); // Read-only kit roots may belong to another user.
  CHECK(::chown((f.root / "foreign-dir").c_str(), 0, static_cast<gid_t>(-1)) == 0);
  CHECK(!approved->Validate());
}

// kPrivateTraversable admits 0700 and 0711 only, owned by the user; it opens
// private children, and a kPrivate parent never opens it.
void PrivateTraversableAccepts0711Only(DirectoryBackend backend) {
  Fixture f;
  const auto mode = [](const fs::path& path) {
    struct stat info{};
    CHECK(::lstat(path.c_str(), &info) == 0);
    return info.st_mode & 07777;
  };
  const auto chain = f.root / "chain";
  fs::create_directory(chain);
  for (mode_t admitted : {0700, 0711}) {
    CHECK(::chmod(chain.c_str(), admitted) == 0);
    auto opened = ManagedDirectory::Open(chain, DirectoryPolicy::kPrivateTraversable, backend);
    CHECK(opened && opened->policy() == DirectoryPolicy::kPrivateTraversable && opened->root() == chain);
  }
  for (mode_t refused : {0755, 0751, 0701, 0710, 0733, 0777, 0500}) {
    CHECK(::chmod(chain.c_str(), refused) == 0);
    CHECK(!ManagedDirectory::Open(chain, DirectoryPolicy::kPrivateTraversable, backend) && errno == EACCES);
  }
  CHECK(::chmod(chain.c_str(), 0711) == 0);
  CHECK(!ManagedDirectory::Open(chain, DirectoryPolicy::kPrivate, backend));
  auto root = ManagedDirectory::Open(f.root, DirectoryPolicy::kApprovedTree, backend);
  CHECK(root);
  auto traversable = root->OpenChild("chain", DirectoryPolicy::kPrivateTraversable);
  CHECK(traversable && traversable->root() == chain);
  CHECK(!root->OpenChild("chain", DirectoryPolicy::kPrivate) && errno == EACCES);

  // Created 0711 or 0700 whatever the umask; other modes are refused before mkdir.
  const auto previous = ::umask(077);
  auto library = traversable->OpenChild("library", DirectoryPolicy::kPrivateTraversable, true, 0711);
  CHECK(library && mode(chain / "library") == 0711 && library->root() == chain);
  auto closed = traversable->OpenChild("closed", DirectoryPolicy::kPrivateTraversable, true, 0700);
  CHECK(closed && mode(chain / "closed") == 0700);
  CHECK(!traversable->OpenChild("wide", DirectoryPolicy::kPrivateTraversable, true, 0755) && errno == EINVAL);
  CHECK(!traversable->OpenChild("wide", DirectoryPolicy::kPrivate, true, 0711) && errno == EINVAL);
  CHECK(!fs::exists(chain / "wide"));
  ::umask(previous);

  // A traversable folder opens kPrivate children (exactly 0700) but no other policy.
  auto profiles = library->OpenChild("profiles", DirectoryPolicy::kPrivate, true);
  CHECK(profiles && mode(chain / "library/profiles") == 0700 && profiles->root() == chain);
  CHECK(!library->OpenChild("public", DirectoryPolicy::kApprovedTree, true) && errno == EINVAL);
  CHECK(!library->OpenChild("public", DirectoryPolicy::kUnownedTree, true) && errno == EINVAL);
  CHECK(!fs::exists(chain / "library/public"));
  // A kPrivate folder stays strict: no traversable child, even an existing 0711 one.
  fs::create_directory(chain / "library/profiles/open");
  CHECK(::chmod((chain / "library/profiles/open").c_str(), 0711) == 0);
  CHECK(!profiles->OpenChild("open", DirectoryPolicy::kPrivateTraversable) && errno == EINVAL);
  CHECK(!profiles->OpenChild("open", DirectoryPolicy::kPrivate) && errno == EACCES);
  CHECK(!profiles->OpenChild("made", DirectoryPolicy::kPrivateTraversable, true, 0711));
  CHECK(!fs::exists(chain / "library/profiles/made"));

  // Mode drift is caught by Validate, also between the two admitted modes (identity).
  CHECK(::chmod((chain / "library").c_str(), 0755) == 0);
  CHECK(!library->Validate() && !profiles->Validate());
  CHECK(!traversable->OpenChild("library", DirectoryPolicy::kPrivateTraversable) && errno == EACCES);
  CHECK(::chmod((chain / "library").c_str(), 0700) == 0);
  CHECK(!library->Validate());
  auto reopened = traversable->OpenChild("library", DirectoryPolicy::kPrivateTraversable);
  CHECK(reopened && reopened->Validate());

  // Renames into a traversable folder admit 0700 or 0711 folders; into kPrivate, 0700 only.
  CHECK(::chmod((chain / "closed").c_str(), 0711) == 0);
  CHECK(::chmod((chain / "library").c_str(), 0711) == 0);
  reopened = traversable->OpenChild("library", DirectoryPolicy::kPrivateTraversable);
  auto private_profiles = reopened ? reopened->OpenChild("profiles", DirectoryPolicy::kPrivate) : std::nullopt;
  CHECK(private_profiles);
  CHECK(!traversable->RenameChild("closed", *private_profiles, "moved") && errno == EACCES);
  CHECK(traversable->RenameChild("closed", *reopened, "game"));
  CHECK(mode(chain / "library/game") == 0711);

  if (::geteuid() == 0) {  // Wrong owner, in privileged CI only.
    fs::create_directory(f.root / "foreign");
    CHECK(::chmod((f.root / "foreign").c_str(), 0711) == 0);
    CHECK(::chown((f.root / "foreign").c_str(), 1, static_cast<gid_t>(-1)) == 0);
    CHECK(!ManagedDirectory::Open(f.root / "foreign", DirectoryPolicy::kPrivateTraversable, backend));
    CHECK(!root->OpenChild("foreign", DirectoryPolicy::kPrivateTraversable));
  }
}
}

// kUnownedTree is used for game folders on drives without owners or permissions.
// The host filesystem keeps modes, so this checks that the policy's operations
// work and that links and specials are still refused; the probe sees ownership.
void UnownedTreeOperations(DirectoryBackend backend) {
  Fixture f;
  CHECK(::chmod(f.root.c_str(), 0777) == 0);
  CHECK(playden::platform::RecordsOwnership(f.root) == std::optional<bool>(true));
  CHECK(!fs::exists(f.root / (".playden-ownership-probe-" + std::to_string(::getpid()))));
  CHECK(!playden::platform::RecordsOwnership(f.root / "missing"));
  auto root = ManagedDirectory::Open(f.root, DirectoryPolicy::kUnownedTree, backend);
  CHECK(root && root->policy() == DirectoryPolicy::kUnownedTree);
  const int file = root->CreateExclusiveFile(".dxvk.conf.tmp", 0644);
  CHECK(file >= 0 && ::write(file, "x", 1) == 1 && ::close(file) == 0);
  { std::ofstream existing(f.root / "dxvk.conf"); existing << "old"; }
  CHECK(::chmod((f.root / "dxvk.conf").c_str(), 0666) == 0);
  CHECK(root->RenameChild(".dxvk.conf.tmp", *root, "dxvk.conf", true));
  const int read = root->OpenRegularFile("dxvk.conf");
  char byte = 0;
  CHECK(read >= 0 && ::read(read, &byte, 1) == 1 && byte == 'x' && ::close(read) == 0);
  fs::create_symlink("dxvk.conf", f.root / "linked.conf");
  CHECK(root->OpenRegularFile("linked.conf") < 0 && !root->RemoveRegularFile("linked.conf"));
  CHECK(root->RemoveRegularFile("dxvk.conf") && !fs::exists(f.root / "dxvk.conf"));
}

// kOpenTree (#868): Playden's own data, any mode as long as the app user owns it, so FTP (root,
// with no permission bypass on /data) can delete it. Folders it creates are 0777; a private
// child still demands 0700, and a private parent never opens an open child.
void OpenTreeAcceptsAnyModeOwnedOnly(DirectoryBackend backend) {
  Fixture f;
  const auto mode = [](const fs::path& path) {
    struct stat info{};
    CHECK(::lstat(path.c_str(), &info) == 0);
    return info.st_mode & 07777;
  };
  const auto state = f.root / "state";
  fs::create_directory(state);
  for (mode_t any : {0700, 0711, 0755, 0777, 01777, 0500}) {
    CHECK(::chmod(state.c_str(), any) == 0);
    auto opened = ManagedDirectory::Open(state, DirectoryPolicy::kOpenTree, backend);
    CHECK(opened && opened->policy() == DirectoryPolicy::kOpenTree && opened->root() == state);
  }
  CHECK(::chmod(state.c_str(), 0700) == 0);
  auto open = ManagedDirectory::Open(state, DirectoryPolicy::kOpenTree, backend);
  CHECK(open);
  auto made = open->OpenChild("requests", DirectoryPolicy::kOpenTree, true);
  CHECK(made && mode(state / "requests") == 0777 && made->root() == state);
  auto deep = made->OpenChild("deeper", DirectoryPolicy::kOpenTree, true);
  CHECK(deep && mode(state / "requests/deeper") == 0777);
  auto secret = open->OpenChild("secret", DirectoryPolicy::kPrivate, true);
  CHECK(secret && mode(state / "secret") == 0700 && secret->root() == state / "secret");
  CHECK(!secret->OpenChild("child", DirectoryPolicy::kOpenTree, true) && errno == EINVAL);
  // A 0666 file (what FTP and the app write now) is created and read back.
  const int file = open->CreateExclusiveFile("installs.json", 0666);
  CHECK(file >= 0 && ::close(file) == 0 && mode(state / "installs.json") == 0666);
  const int read = open->OpenRegularFile("installs.json", true);
  CHECK(read >= 0 && ::close(read) == 0);
  // From an approved parent, the child becomes the open tree's root.
  auto parent = ManagedDirectory::Open(f.root, DirectoryPolicy::kApprovedTree, backend);
  CHECK(parent);
  auto child = parent->OpenChild("state", DirectoryPolicy::kOpenTree);
  CHECK(child && child->root() == state);
  auto created = parent->OpenChild("art", DirectoryPolicy::kOpenTree, true);
  CHECK(created && mode(f.root / "art") == 0777);
  // Links are still refused, and a mode change under an open handle is drift like any other.
  fs::create_symlink(state, f.root / "linked");
  CHECK(!ManagedDirectory::Open(f.root / "linked", DirectoryPolicy::kOpenTree, backend));
  CHECK(!parent->OpenChild("linked", DirectoryPolicy::kOpenTree));
  CHECK(::chmod(state.c_str(), 0777) == 0);
  CHECK(!open->Validate() && errno == ESTALE);
  if (::geteuid() == 0) {
    CHECK(::chown(state.c_str(), 1, static_cast<gid_t>(-1)) == 0);
    CHECK(!ManagedDirectory::Open(state, DirectoryPolicy::kOpenTree, backend) && errno == EACCES);
  }
}

int main() {
#if defined(__PROSPERO__) || (defined(PLAYDEN_PLATFORM_PS5) && PLAYDEN_PLATFORM_PS5)
  CHECK(ManagedDirectory::DefaultBackend() == DirectoryBackend::kAbsolutePath);
  const std::vector<DirectoryBackend> backends{DirectoryBackend::kAbsolutePath};
#else
  CHECK(ManagedDirectory::DefaultBackend() == DirectoryBackend::kDescriptorRelative);
  const std::vector<DirectoryBackend> backends{DirectoryBackend::kDescriptorRelative, DirectoryBackend::kAbsolutePath};
#endif
  for (auto backend : backends) {
    ReadWriteRenameAndRemove(backend);
    RejectTraversalLinksAndSpecials(backend);
    RejectPolicyAndIdentityDrift(backend, false);
    RejectPolicyAndIdentityDrift(backend, true);
    RenameAdmission(backend);
    ForeignOwnershipWhenPrivileged(backend);
    PrivateTraversableAccepts0711Only(backend);
    UnownedTreeOperations(backend);
    OpenTreeAcceptsAnyModeOwnedOnly(backend);
  }
  std::printf("managed directory: %zu backends, identity, policy and file operations pass\n", backends.size());
}
