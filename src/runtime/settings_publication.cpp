#include "runtime/settings_publication.h"

#include "platform/usb_mount.h"
#include "runtime/publication_contract.h"
#include "runtime/artifact_hash.h"
extern "C" {
#include "pw_title_publication_native.h"
#include "pw_wine_launch.h"
}
#include "platform/managed_file_lock.h"
#include "platform/usb_mount.h"
#include <cerrno>
#include <atomic>
#include <charconv>
#include <cstring>
#include <unistd.h>

namespace playden::runtime {
namespace {
using platform::ManagedDirectory;
using platform::DirectoryPolicy;
PackageActionResult Failure(const char* code, const char* message) {
  return {PackageActionState::kFailed, code, message};
}
bool Complete(const PackageActionResult& result) { return result.state == PackageActionState::kComplete; }
std::filesystem::path Map(const std::filesystem::path& prefix, const std::string& canonical) {
  return prefix.empty() ? std::filesystem::path(canonical) : prefix / std::filesystem::path(canonical).relative_path();
}
bool PathsMatch(const SettingsPublicationOperation& o, const PublicationPaths& p, const std::filesystem::path& prefix) {
  if (!prefix.empty() && (!prefix.is_absolute() || prefix != prefix.lexically_normal())) return false;
  const auto profiles = o.install.data_root + "/library/profiles/";
  return p.profile == Map(prefix, profiles + o.plan.profile_id + ".profile") &&
      p.catalog == Map(prefix, profiles + "profiles.lst") &&
      p.input_directory == Map(prefix, o.install.data_root + "/library/input") &&
      p.descriptor == Map(prefix, "/data/homebrew/" + o.install.title_id + "/game-launch.json") &&
      p.metadata == Map(prefix, profiles + ".playden-publication");
}
// The publisher still holds its lease: the descriptor is open and is the file the
// control folder names publication.lock. Owner, mode and folder chains are not
// re-proven at every step; only Playden writes these folders.
bool LeaseHeld(int borrowed, const std::filesystem::path& control) {
  return platform::ManagedFileLock::Holds(borrowed, control / "publication.lock");
}
DirectoryPolicy GameDirectoryPolicy(bool unowned) {
  return unowned ? DirectoryPolicy::kUnownedTree : DirectoryPolicy::kApprovedTree;
}
// The game's working folder belongs to the app user with 0700/0755, unless its
// filesystem records no owner or permissions.
bool OwnedGameDirectory(const struct stat& info, bool unowned) {
  return S_ISDIR(info.st_mode) && (unowned || (info.st_uid == geteuid() &&
      ((info.st_mode & 07777) == 0700 || (info.st_mode & 07777) == 0755)));
}
// The operation publishes its game folder unowned (metadata schema 3).
bool PublishesUnowned(const SettingsPublicationOperation& o) {
  PwTitlePublicationMetadata m{};
  return pw_title_publication_metadata_parse(reinterpret_cast<const uint8_t*>(o.metadata_bytes.data()),
      o.metadata_bytes.size(), &m) == PW_OK && m.game_root_unowned != 0;
}
// Reads a regular file (never through a symbolic link) up to maximum bytes, and
// syncs it so what an apply builds on is durable. The file read is the one that
// was statted (same device, inode and size); owner, mode, link count and times
// are not checked, since only Playden writes these folders. The mode is returned
// for writers that keep it.
std::optional<FrozenFileSnapshot> ReadPublicationFile(const ManagedDirectory& directory,
    const std::string& name, std::size_t maximum) {
  struct stat before{}, opened{}, after{};
  if (!directory.InspectChild(name, &before)) {
    if (errno == ENOENT && directory.Sync()) return FrozenFileSnapshot{};
    return std::nullopt;
  }
  auto same = [](const struct stat& a, const struct stat& b) {
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_size == b.st_size;
  };
  if (!S_ISREG(before.st_mode) || before.st_size < 0 || static_cast<std::uint64_t>(before.st_size) > maximum)
    return std::nullopt;
  int fd = directory.OpenRegularFile(name);
  if (fd < 0) return std::nullopt;
  bool valid = fstat(fd, &opened) == 0 && same(before, opened);
  std::string bytes; char buffer[4096];
  while (valid) {
    const auto count = read(fd, buffer, sizeof(buffer));
    if (count < 0 && errno == EINTR) continue;
    if (count < 0 || static_cast<std::size_t>(count) > maximum - bytes.size()) { valid = false; break; }
    if (!count) break;
    bytes.append(buffer, count);
  }
  valid = valid && fstat(fd, &opened) == 0 && same(before, opened) &&
      directory.InspectChild(name, &after) && same(before, after) &&
      bytes.size() == static_cast<std::uint64_t>(before.st_size) && fsync(fd) == 0;
  const bool closed = close(fd) == 0;
  if (!valid || !closed || !directory.Sync()) return std::nullopt;
  return FrozenFileSnapshot{true, static_cast<unsigned>(before.st_mode & 07777), bytes.size(), {}, std::move(bytes)};
}
bool WritePublicationFile(const ManagedDirectory& directory, const std::string& name,
    const std::string& bytes, const FrozenFileSnapshot& before, const std::function<bool()>& held) {
  static std::atomic<unsigned long> sequence{0}; std::string temporary; int fd = -1;
  for (unsigned attempt = 0; attempt < 128; ++attempt) {
    temporary = "." + name + ".publication-" + std::to_string(getpid()) + "-" + std::to_string(sequence++);
    fd = directory.CreateExclusiveFile(temporary, 0600);
    if (fd >= 0 || errno != EEXIST) break;
  }
  if (fd < 0) return false;
  std::size_t offset = 0; bool valid = true;
  while (offset < bytes.size()) {
    const auto count = write(fd, bytes.data() + offset, bytes.size() - offset);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) { valid = false; break; }
    offset += static_cast<std::size_t>(count);
  }
  const bool synced = valid && fsync(fd) == 0; const bool closed = close(fd) == 0;
  if (!synced || !closed || !held()) return false;
  auto observed = ReadPublicationFile(directory, name, kMaxSettingsPublicationOperationBytes);
  if (!observed || observed->present != before.present ||
      (before.present && (observed->bytes != before.bytes || observed->mode != before.mode))) return false;
  return held() && directory.RenameChild(temporary, directory, name, before.present) && directory.Sync() && held();
}
struct ReceiptRead {
  bool ok = false;
  bool absent = false;
  std::string bytes;
};
ReceiptRead ReadReceipt(const std::filesystem::path& control, const ManagedDirectory& directory) {
  constexpr const char* name = "publication-state.v1";
  struct stat before{}, after{};
  if (!directory.InspectChild(name, &before)) {
    const bool absent = errno == ENOENT && directory.Sync();
    return {absent, absent, {}};
  }
  if (!S_ISREG(before.st_mode) || before.st_size <= 0 || before.st_size > 8192) return {};
  Storage storage(control.string()); auto read = storage.ReadDurable(name, 8192);
  if (!read.ok() || !directory.InspectChild(name, &after) || before.st_dev != after.st_dev ||
      before.st_ino != after.st_ino || before.st_size != after.st_size) return {};
  std::string bytes(read.value().begin(), read.value().end()); PwTitlePublication parsed{};
  if (pw_title_publication_parse(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), &parsed) != PW_OK) return {};
  return {true, false, std::move(bytes)};
}
// The game folder a title's live descriptor names, or nothing when it can't be read.
std::optional<std::string> LiveGameRoot(const std::string& title_id, const std::filesystem::path& prefix) {
  const auto host = prefix.string(); PwTitlePublicationPlatform platform{};
  platform.host_root = host.empty() ? nullptr : host.c_str(); platform.owner_uid = geteuid();
  PwTitlePublicationDescriptor descriptor{}; char error[256]{};
  if (pw_title_publication_read_descriptor(("/data/homebrew/" + title_id + "/game-launch.json").c_str(), title_id.c_str(),
        &platform, &descriptor, error, sizeof(error)) != PW_OK)
    return std::nullopt;
  return std::string(descriptor.launch.game_root);
}
// Whether the previous publication of `title_id` is the one a moved game folder left behind.
bool NamesMovedRoot(const GameRootMove* moved, const std::string& title_id, const std::filesystem::path& prefix) {
  if (!moved) return false;
  const auto root = LiveGameRoot(title_id, prefix);
  return root && (*root == moved->from || *root == platform::WithoutUsbMountNumber(moved->from));
}
// An owned publication (schema 2) that an unowned one replaces. The player may have opened the
// game folder since (mods over FTP), which the owned rule refuses, so it is checked as a folder
// moved onto itself, with the unowned rule (VerifyMovedPublication).
bool OwnedReplacedByUnowned(const std::string& previous_metadata, bool unowned) {
  PwTitlePublicationMetadata m{};
  return unowned && !previous_metadata.empty() &&
      pw_title_publication_metadata_parse(reinterpret_cast<const uint8_t*>(previous_metadata.data()),
                                          previous_metadata.size(), &m) == PW_OK && m.game_root_unowned == 0;
}
GameRootMove InPlace(const RuntimeInstall& r) { return {r.install_path, r.install_path}; }
}
PackageActionResult VerifyCommittedPublicationReceipt(const RuntimeInstall& r, const std::filesystem::path& prefix) {
  const auto refuse = [](const char* message) { return Failure("publication-receipt", message); };
  if (!prefix.empty() && (!prefix.is_absolute() || prefix != prefix.lexically_normal())) return refuse("Invalid canonical namespace.");
  const auto control = Map(prefix, r.data_root + "/.playden-control");
  auto directory = ManagedDirectory::Open(control, DirectoryPolicy::kPrivate);
  if (!directory) return refuse("Private publication control directory is unavailable.");
  auto receipt = ReadReceipt(control, *directory);
  if (!receipt.ok || receipt.absent) return refuse("The publication receipt is missing, corrupt or unavailable.");
  PwTitlePublication parsed{};
  if (pw_title_publication_parse(reinterpret_cast<const uint8_t*>(receipt.bytes.data()), receipt.bytes.size(), &parsed) != PW_OK ||
      parsed.phase != PW_TITLE_PUBLICATION_COMMITTED || parsed.install_id != r.install_id || parsed.title_id != r.title_id ||
      parsed.settings_revision != r.settings_revision)
    return refuse("The publication receipt is not committed for this install at its revision.");
  auto profiles = ManagedDirectory::Open(Map(prefix, r.data_root + "/library/profiles"), DirectoryPolicy::kPrivate);
  auto metadata = profiles ? ReadPublicationFile(*profiles, ".playden-publication", 32768) : std::nullopt;
  PwTitlePublicationMetadata m{};
  if (!metadata || !metadata->present || HashArtifactBytes(metadata->bytes) != std::optional<std::string>(parsed.publication_sha256) ||
      pw_title_publication_metadata_parse(reinterpret_cast<const uint8_t*>(metadata->bytes.data()), metadata->bytes.size(), &m) != PW_OK ||
      m.install_id != r.install_id || m.title_id != r.title_id || m.data_root != r.data_root || m.settings_revision != r.settings_revision)
    return refuse("The published metadata does not bind the committed receipt.");
  return {PackageActionState::kComplete, {}, {}};
}
std::optional<bool> PublishedGameRootUnowned(const RuntimeInstall& r, const std::filesystem::path& prefix) {
  if (!prefix.empty() && (!prefix.is_absolute() || prefix != prefix.lexically_normal())) return std::nullopt;
  auto profiles = ManagedDirectory::Open(Map(prefix, r.data_root + "/library/profiles"), DirectoryPolicy::kPrivate);
  auto bytes = profiles ? ReadPublicationFile(*profiles, ".playden-publication", 32768) : std::nullopt;
  PwTitlePublicationMetadata m{};
  if (!bytes || !bytes->present || pw_title_publication_metadata_parse(
          reinterpret_cast<const uint8_t*>(bytes->bytes.data()), bytes->bytes.size(), &m) != PW_OK)
    return std::nullopt;
  return m.game_root_unowned != 0;
}
std::optional<std::string> PublishedGameRoot(const RuntimeInstall& r, const std::filesystem::path& prefix) {
  return LiveGameRoot(r.title_id, prefix);
}
std::optional<std::string> PublishedProfileDigest(const RuntimeInstall& r, const std::filesystem::path& prefix) {
  if (!prefix.empty() && (!prefix.is_absolute() || prefix != prefix.lexically_normal())) return std::nullopt;
  auto profiles = ManagedDirectory::Open(Map(prefix, r.data_root + "/library/profiles"), DirectoryPolicy::kPrivate);
  auto bytes = profiles ? ReadPublicationFile(*profiles, ".playden-publication", 32768) : std::nullopt;
  PwTitlePublicationMetadata m{};
  if (!bytes || !bytes->present || pw_title_publication_metadata_parse(
          reinterpret_cast<const uint8_t*>(bytes->bytes.data()), bytes->bytes.size(), &m) != PW_OK)
    return std::nullopt;
  return std::string(m.profile_sha256);
}
std::optional<GameRootMove> GameRootMoveOf(const RuntimeInstall& record) {
  if (record.moved_from.empty() || record.moved_from == record.install_path) return std::nullopt;
  return GameRootMove{record.moved_from, record.install_path};
}
PackageActionResult VerifyMovedPublication(const RuntimeInstall& r, const GameRootMove& move,
    const std::string& receipt_bytes, const std::string& metadata_bytes, const std::filesystem::path& prefix,
    bool unowned) {
  const auto refuse = [](const char* message) { return Failure("publication-moved", message); };
  if (!prefix.empty() && (!prefix.is_absolute() || prefix != prefix.lexically_normal()))
    return refuse("Invalid canonical namespace.");
  PwTitlePublication receipt{}; PwTitlePublicationMetadata m{};
  const auto metadata_digest = HashArtifactBytes(metadata_bytes);
  if (pw_title_publication_parse(reinterpret_cast<const uint8_t*>(receipt_bytes.data()), receipt_bytes.size(), &receipt) != PW_OK ||
      receipt.phase != PW_TITLE_PUBLICATION_COMMITTED || receipt.install_id != r.install_id || receipt.title_id != r.title_id ||
      !metadata_digest || *metadata_digest != receipt.publication_sha256)
    return refuse("The moved game's publication receipt does not bind its metadata.");
  if (pw_title_publication_metadata_parse(reinterpret_cast<const uint8_t*>(metadata_bytes.data()), metadata_bytes.size(), &m) != PW_OK ||
      m.install_id != r.install_id || m.title_id != r.title_id || m.data_root != r.data_root ||
      m.settings_revision != receipt.settings_revision)
    return refuse("The moved game's publication metadata names another install.");
  const auto host = prefix.string(); PwTitlePublicationPlatform platform{};
  platform.host_root = host.empty() ? nullptr : host.c_str(); platform.owner_uid = geteuid();
  PwTitlePublicationDescriptor descriptor{}; char error[256]{};
  // Published naming no mount, the old folder is its search form too.
  const auto published_from = [&] {
    const auto root = LiveGameRoot(r.title_id, prefix);
    return root && platform::IsUsbSearchPath(*root) ? platform::WithoutUsbMountNumber(move.from) : move.from;
  }();
  const auto descriptor_digest = [&] {
    return HashArtifactBytes(std::string_view(reinterpret_cast<const char*>(descriptor.bytes), descriptor.length));
  };
  if (pw_title_publication_read_descriptor(("/data/homebrew/" + r.title_id + "/game-launch.json").c_str(), r.title_id.c_str(),
        &platform, &descriptor, error, sizeof(error)) != PW_OK ||
      descriptor.launch.app_id != r.runtime_number || descriptor.launch.install_id != r.install_id ||
      descriptor.launch.data_root != r.data_root || descriptor.launch.game_root != published_from ||
      descriptor_digest() != std::optional<std::string>(m.descriptor_sha256))
    return refuse("The moved game's descriptor does not match its publication.");
  // Profile and catalog live in the private data root, which did not move.
  auto profiles = ManagedDirectory::Open(Map(prefix, r.data_root + "/library/profiles"), DirectoryPolicy::kPrivate);
  auto profile_bytes = profiles ? ReadPublicationFile(*profiles, std::string(m.profile_id) + ".profile", 256 * 1024) : std::nullopt;
  auto catalog = profiles ? ReadPublicationFile(*profiles, "profiles.lst", 4096) : std::nullopt;
  PwGameProfile profile{};
  if (!profile_bytes || !profile_bytes->present || HashArtifactBytes(profile_bytes->bytes) != std::optional<std::string>(m.profile_sha256) ||
      pw_game_profile_parse(reinterpret_cast<const uint8_t*>(profile_bytes->bytes.data()), profile_bytes->bytes.size(), &profile) != PW_OK ||
      profile.app.id != std::string(m.profile_id) || !catalog || !catalog->present ||
      HashArtifactBytes(catalog->bytes) != std::optional<std::string>(m.catalog_sha256))
    return refuse("The moved game's profile does not match its publication.");
  // The profile's program and folder were inside the old game folder; the folder is checked at its new place.
  char working[1024]{}, executable[1024]{};
  const auto bottle = r.data_root + "/library/prefixes/game";
  if (pw_wine_launch_host_dir(bottle.c_str(), profile.app.working_directory, working, sizeof(working)) ||
      pw_wine_launch_host_dir(bottle.c_str(), profile.app.executable, executable, sizeof(executable)) ||
      std::string(m.dxvk_path) != std::string(working) + "/dxvk.conf" ||
      !RebaseInstallPath(executable, published_from, move.to))
    return refuse("The moved game's profile names a program outside its game folder.");
  const auto moved_working = RebaseInstallPath(working, published_from, move.to);
  struct stat info{};
  if (!moved_working || lstat(Map(prefix, *moved_working).c_str(), &info) != 0 ||
      !OwnedGameDirectory(info, unowned || m.game_root_unowned != 0))
    return refuse("The moved game's working folder is missing at its new place.");
  return {PackageActionState::kComplete, {}, {}};
}
PackageActionResult PublishSettingsOperation(const SettingsPublicationOperation& o,
    const PublicationPaths& paths, PublicationDependencies& deps) {
  std::string reason; auto encoded = EncodeSettingsPublicationOperation(o, &reason);
  if (!encoded || !PathsMatch(o, paths, deps.host_namespace_prefix))
    return Failure("publication-invalid", "Frozen operation or canonical publication paths are invalid.");
  if (!deps.activity || !deps.block_owner || !deps.preflight || !deps.apply_files ||
      !deps.verify_files || !deps.reconcile_committed)
    return Failure("publication-unavailable", "Required publication adapters are unavailable.");
  const auto control = Map(deps.host_namespace_prefix, o.install.data_root + "/.playden-control");
  auto directory = ManagedDirectory::Open(control, DirectoryPolicy::kPrivate);
  if (!directory) return Failure("publication-control", "Private publication control directory is unavailable.");
  auto lease = platform::ManagedFileLock::Acquire(*directory, "publication.lock", platform::LockMode::kPublisher);
  if (!lease) return Failure(errno == EWOULDBLOCK || errno == EAGAIN ? "publication-busy" : "publication-lock",
                             "Publisher lease could not be acquired.");
  auto held = [&] { return LeaseHeld(lease->fd(), control); };
  auto checkpoint = [&](PublicationStep step) { return (!deps.after_step || deps.after_step(step)) && held(); };
  // The one "game stopped" check of an apply, under the lease and before any effect.
  // A title started from Home by this kit holds the lease shared while it runs, so
  // the exclusive lease alone keeps it from starting until the apply ends.
  if (deps.activity(o.install) != NativeActivity::kStopped)
    return Failure("publication-not-stopped", "The title is not confirmed stopped.");
  auto journal = LoadSettingsPublicationOperation(control);
  bool fresh = journal.state == PublicationRecoveryState::kAbsent;
  PackageActionResult result;
  if (!fresh) {
    if (journal.state != PublicationRecoveryState::kValid || !journal.operation)
      return Failure("publication-journal", "Operation recovery is corrupt or unavailable.");
    auto recovered = EncodeSettingsPublicationOperation(*journal.operation, &reason);
    if (!recovered) return Failure("publication-journal", "Operation recovery is invalid.");
    if (*recovered != *encoded) {
      const auto& previous = *journal.operation;
      if (!deps.authorize_retirement || o.previous_receipt_bytes != previous.committed_receipt_bytes ||
          o.previous_metadata_bytes != previous.metadata_bytes || o.previous_revision != previous.plan.settings_revision ||
          o.transaction_id == previous.transaction_id)
        return Failure("publication-drift", "A different frozen operation must be reconciled first.");
      const auto profile_root = previous.install.data_root + "/library/profiles/";
      PublicationPaths previous_paths{
        Map(deps.host_namespace_prefix, profile_root + previous.plan.profile_id + ".profile"),
        Map(deps.host_namespace_prefix, profile_root + "profiles.lst"),
        Map(deps.host_namespace_prefix, previous.install.data_root + "/library/input"),
        Map(deps.host_namespace_prefix, "/data/homebrew/" + previous.install.title_id + "/game-launch.json"),
        Map(deps.host_namespace_prefix, profile_root + ".playden-publication")};
      // A journal left by a game folder that moved names the old folder, which no longer exists.
      // Compared without the USB mount's number: published naming no mount, the drive may have changed places first.
      if (deps.moved_root && platform::WithoutUsbMountNumber(previous.install.install_path) ==
                                 platform::WithoutUsbMountNumber(deps.moved_root->from))
        result = VerifyMovedPublication(o.install, *deps.moved_root, previous.committed_receipt_bytes, previous.metadata_bytes,
                                        deps.host_namespace_prefix, PublishesUnowned(o));
      // Published naming no mount: its drive changed places since, with nothing published, and the journal names the old one.
      else if (previous.install.install_path != o.install.install_path &&
               platform::WithoutUsbMountNumber(previous.install.install_path) == platform::WithoutUsbMountNumber(o.install.install_path))
        result = VerifyMovedPublication(o.install, GameRootMove{previous.install.install_path, o.install.install_path},
                                        previous.committed_receipt_bytes, previous.metadata_bytes, deps.host_namespace_prefix,
                                        PublishesUnowned(o));
      else if (OwnedReplacedByUnowned(previous.metadata_bytes, PublishesUnowned(o)))
        result = VerifyMovedPublication(o.install, InPlace(o.install), previous.committed_receipt_bytes,
                                        previous.metadata_bytes, deps.host_namespace_prefix, true);
      else
        result = deps.verify_files(previous, previous_paths, lease->fd(), true);
      if (!Complete(result)) return result;
      result = deps.authorize_retirement(previous, o); if (!Complete(result)) return result;
      if (!ArchiveSettingsPublicationOperation(control, previous).ok() || !held())
        return Failure("publication-archive", "Previous journal archival requires fresh recovery.");
      fresh = true;
      if (!checkpoint(PublicationStep::kPreviousArchived))
        return Failure("publication-interrupted", "Previous journal archival requires fresh recovery.");
    }
  }
  result = deps.block_owner(o);
  if (!Complete(result)) return result;
  if (!checkpoint(PublicationStep::kOwnerBlocked))
    return Failure("publication-interrupted", "OWNER persistence requires fresh recovery.");
  auto receipt = ReadReceipt(control, *directory);
  if (!receipt.ok) return Failure("publication-receipt", "Publication receipt is corrupt or unavailable.");
  bool committed = !receipt.absent && receipt.bytes == o.committed_receipt_bytes;
  bool pending = !receipt.absent && receipt.bytes == o.pending_receipt_bytes;
  const bool before = o.previous_receipt_bytes.empty() ? receipt.absent :
      !receipt.absent && receipt.bytes == o.previous_receipt_bytes;
  if ((!pending && !committed && !before) || ((pending || committed) && fresh))
    return Failure("publication-collision", "Receipt does not prove this operation's recovery ownership.");
  if (!pending && !committed) {
    result = deps.preflight(o, paths, lease->fd());
    if (!Complete(result)) return result;
    if (!held()) return Failure("publication-lock", "Publisher lease changed during preflight.");
  }
  if (fresh) {
    auto created = CreateSettingsPublicationOperation(control, o);
    if (!created.ok()) return Failure("publication-journal", "Operation persistence failed; reload before effects.");
  }
  if (!checkpoint(PublicationStep::kJournalDurable))
    return Failure("publication-interrupted", "Operation persistence requires fresh recovery.");
  auto write_receipt = [&](const std::string& bytes) {
    if (!held()) return false;
    Storage storage(control.string()); Bytes data(bytes.begin(), bytes.end());
    if (!storage.WriteDurable("publication-state.v1", data).ok() || !held()) return false;
    auto observed = ReadReceipt(control, *directory);
    return observed.ok && !observed.absent && observed.bytes == bytes;
  };
  if (!committed) {
    if (!write_receipt(o.pending_receipt_bytes))
      return Failure("publication-pending", "Pending receipt persistence failed; reload before effects.");
    if (!checkpoint(PublicationStep::kPendingDurable))
      return Failure("publication-interrupted", "Pending publication requires fresh recovery.");
    result = deps.apply_files(o, paths, lease->fd());
    if (!Complete(result)) return result;
    if (!checkpoint(PublicationStep::kFilesApplied))
      return Failure("publication-interrupted", "Publication file effects require fresh recovery.");
    result = deps.verify_files(o, paths, lease->fd(), false);
    if (!Complete(result)) return result;
    auto still_pending = ReadReceipt(control, *directory);
    if (!still_pending.ok || still_pending.bytes != o.pending_receipt_bytes || !held())
      return Failure("publication-changed", "Publication receipt or lease changed during verification.");
    if (!checkpoint(PublicationStep::kPendingVerified))
      return Failure("publication-interrupted", "Pending verification requires fresh recovery.");
    if (!write_receipt(o.committed_receipt_bytes))
      return Failure("publication-commit", "Committed receipt persistence failed; reload before reconciliation.");
    if (!checkpoint(PublicationStep::kCommitDurable))
      return Failure("publication-interrupted", "Commit persistence requires fresh recovery.");
  }
  result = deps.verify_files(o, paths, lease->fd(), true);
  if (!Complete(result)) return result;
  auto observed = ReadReceipt(control, *directory);
  if (!observed.ok || observed.bytes != o.committed_receipt_bytes || !held())
    return Failure("publication-changed", "Committed publication or lease changed.");
  if (!checkpoint(PublicationStep::kCommitVerified))
    return Failure("publication-interrupted", "Committed verification requires fresh OWNER reconciliation.");
  return deps.reconcile_committed(o);
}
PackageActionResult VerifySettingsPublicationFiles(const SettingsPublicationOperation& o,
    const PublicationPaths& paths, int borrowed, bool committed, const std::filesystem::path& prefix) {
  std::string reason;
  if (!EncodeSettingsPublicationOperation(o, &reason) || !PathsMatch(o, paths, prefix))
    return Failure("publication-invalid", "Frozen verification operation or canonical paths are invalid.");
  PwTitlePublicationDescriptor descriptor{}; char error[256]{};
  if (pw_title_publication_descriptor_parse(reinterpret_cast<const uint8_t*>(o.descriptor_bytes.data()),
        o.descriptor_bytes.size(), o.install.title_id.c_str(), &descriptor, error, sizeof(error)) != PW_OK)
    return Failure("publication-descriptor", "Frozen descriptor is invalid.");
  const auto digest = HashArtifactBytes(o.metadata_bytes);
  if (!digest) return Failure("publication-hash", "Metadata hashing is unavailable.");
  const auto host = prefix.string(); PwTitlePublicationPlatform platform{};
  platform.host_root = host.empty() ? nullptr : host.c_str(); platform.owner_uid = geteuid();
  const auto mount = platform::UsbMountOf(o.install.install_path);
  platform.known_game_mount = mount.empty() ? nullptr : mount.c_str();
  const auto& receipt = committed ? o.committed_receipt_bytes : o.pending_receipt_bytes;
  PwTitlePublicationContents contents{};
  if (pw_title_publication_verify_locked(&descriptor, &platform, borrowed,
        reinterpret_cast<const uint8_t*>(receipt.data()), receipt.size(), digest->c_str(), &contents, error, sizeof(error)) != PW_OK)
    return {PackageActionState::kFailed, "publication-verification", error};
  return {PackageActionState::kComplete, {}, {}};
}
PackageActionResult ApplyPendingPublicationRegistry(const SettingsPublicationOperation& o,
    int borrowed, const std::filesystem::path& prefix) {
  std::string reason;
  auto encoded = EncodeSettingsPublicationOperation(o, &reason);
  if (!encoded || (!prefix.empty() && (!prefix.is_absolute() || prefix != prefix.lexically_normal())))
    return Failure("publication-invalid", "Frozen registry operation or namespace is invalid.");
  const auto control = Map(prefix, o.install.data_root + "/.playden-control");
  auto directory = ManagedDirectory::Open(control, DirectoryPolicy::kPrivate);
  auto held = [&] { return directory && LeaseHeld(borrowed, control); };
  if (!held()) return Failure("publication-lock", "Registry publisher descriptor is unavailable or changed.");
  auto journal = LoadSettingsPublicationOperation(control);
  auto recovered = journal.operation ? EncodeSettingsPublicationOperation(*journal.operation, &reason) : std::nullopt;
  auto receipt = ReadReceipt(control, *directory);
  if (journal.state != PublicationRecoveryState::kValid || !recovered || *recovered != *encoded ||
      !receipt.ok || receipt.absent || receipt.bytes != o.pending_receipt_bytes || !held())
    return Failure("publication-registry-ownership", "Registry effects require the exact durable pending operation.");
  if (o.plan.registry.empty()) return {PackageActionState::kComplete, {}, {}};
  if (!o.registry_backup) return Failure("publication-registry-backup", "Registry snapshot binding is missing.");
  auto backup = LoadPublicationRegistryBackup(control, o.transaction_id, *o.registry_backup);
  if (backup.state != PublicationRecoveryState::kValid)
    return Failure("publication-registry-backup", "Immutable registry backup is unavailable.");
  auto intended = backup.bytes;
  if (!ApplyRegistryValues(&intended, o.plan.registry, o.registry_unix_time, &reason) ||
      intended.size() > kMaxPublicationRegistryBackupBytes)
    return Failure("publication-registry-render", "Frozen registry edits cannot be safely rendered.");
  const auto prefix_path = Map(prefix, o.install.data_root + "/library/prefixes/game");
  auto bottle = ManagedDirectory::Open(prefix_path, DirectoryPolicy::kPrivateTraversable);
  if (!bottle) return Failure("publication-registry-directory", "Private bottle directory is unavailable.");
  Storage storage(prefix_path.string());
  storage.AllowTraversableRoot();
  auto read = [&](std::string* bytes, unsigned* mode) {
    struct stat before{}, after{};
    if (!bottle->InspectChild("user.reg", &before) || !S_ISREG(before.st_mode) || before.st_size < 0 ||
        static_cast<std::uint64_t>(before.st_size) > kMaxPublicationRegistryBackupBytes) return false;
    auto observed = storage.ReadDurable("user.reg", kMaxPublicationRegistryBackupBytes);
    if (!observed.ok() || !bottle->InspectChild("user.reg", &after) ||
        before.st_dev != after.st_dev || before.st_ino != after.st_ino || before.st_size != after.st_size ||
        observed.value().size() != static_cast<std::uint64_t>(before.st_size)) return false;
    bytes->assign(observed.value().begin(), observed.value().end()); *mode = before.st_mode & 07777;
    return true;
  };
  std::string current; unsigned mode = 0;
  if (!read(&current, &mode) || !held())
    return Failure("publication-registry-read", "Registry identity or durability could not be verified.");
  const bool original = current == backup.bytes && mode == o.registry_backup->source_mode;
  const bool applied = current == intended && mode == 0600;
  if (!original && !applied)
    return Failure("publication-registry-collision", "Live registry matches neither frozen original nor intended bytes.");
  if (!applied) {
    Bytes bytes(intended.begin(), intended.end());
    if (!storage.WriteDurable("user.reg", bytes).ok())
      return Failure("publication-registry-write", "Registry publication failed; pending evidence must be recovered.");
  }
  if (!held() || !read(&current, &mode) || current != intended || mode != 0600 || !held())
    return Failure("publication-registry-verify", "Published registry could not be verified durably.");
  return {PackageActionState::kComplete, {}, {}};
}
PackageActionResult ApplyPendingPublicationDxvk(const SettingsPublicationOperation& o,
    int borrowed, const std::filesystem::path& prefix) {
  std::string reason;
  auto encoded = EncodeSettingsPublicationOperation(o, &reason);
  if (!encoded || (!prefix.empty() && (!prefix.is_absolute() || prefix != prefix.lexically_normal())))
    return Failure("publication-invalid", "Frozen DXVK operation or namespace is invalid.");
  const auto control = Map(prefix, o.install.data_root + "/.playden-control");
  auto directory = ManagedDirectory::Open(control, DirectoryPolicy::kPrivate);
  auto held = [&] { return directory && LeaseHeld(borrowed, control); };
  if (!held()) return Failure("publication-lock", "Registry publisher descriptor is unavailable or changed.");
  auto journal = LoadSettingsPublicationOperation(control);
  auto recovered = journal.operation ? EncodeSettingsPublicationOperation(*journal.operation, &reason) : std::nullopt;
  auto receipt = ReadReceipt(control, *directory);
  if (journal.state != PublicationRecoveryState::kValid || !recovered || *recovered != *encoded ||
      !receipt.ok || receipt.absent || receipt.bytes != o.pending_receipt_bytes || !held())
    return Failure("publication-dxvk-ownership", "Registry effects require the exact durable pending operation.");
  PwTitlePublicationMetadata metadata{};
  if (pw_title_publication_metadata_parse(reinterpret_cast<const uint8_t*>(o.metadata_bytes.data()),
      o.metadata_bytes.size(), &metadata) != PW_OK)
    return Failure("publication-invalid", "Frozen DXVK metadata is invalid.");
  const auto target = Map(prefix, platform::OnGameMount(metadata.dxvk_path, o.install.install_path));
  const bool unowned = metadata.game_root_unowned != 0;
  auto game = ManagedDirectory::Open(target.parent_path(), GameDirectoryPolicy(unowned));
  struct stat game_info{};
  if (!game || lstat(target.parent_path().c_str(), &game_info) || !OwnedGameDirectory(game_info, unowned))
    return Failure("publication-dxvk-directory", "Owned game working directory is unavailable.");
  auto read = [&](FrozenFileSnapshot* snapshot) {
    auto observed = ReadPublicationFile(*game, "dxvk.conf", 256 * 1024);
    if (!observed) return false;
    *snapshot = std::move(*observed); return true;
  };
  FrozenFileSnapshot current;
  if (!read(&current) || !held()) return Failure("publication-dxvk-read", "DXVK identity or durability could not be verified.");
  const bool desired_present = !o.plan.dxvk_config.empty();
  // The title admits 0600 or 0644; a game's own file in another mode is replaced as 0644.
  const unsigned desired_mode = o.dxvk_before.present && o.dxvk_before.mode == 0600 ? 0600 : 0644;
  // An unowned folder reports one fixed mode for every file; only bytes count there.
  const auto mode_matches = [&](unsigned mode, unsigned wanted) { return unowned || mode == wanted; };
  // Original by bytes alone: a mode change is no reason to stop, the write sets the
  // mode the title admits.
  const bool original = current.present == o.dxvk_before.present && (!current.present ||
      current.bytes == o.dxvk_before.bytes);
  const bool applied = current.present == desired_present && (!current.present ||
      (current.bytes == o.plan.dxvk_config && mode_matches(current.mode, desired_mode)));
  if (!original && !applied)
    return Failure("publication-dxvk-collision", "Live DXVK matches neither frozen original nor intended bytes.");
  if (!applied && !desired_present) {
    if (!held() || !game->RemoveRegularFile("dxvk.conf") || !game->Sync())
      return Failure("publication-dxvk-remove", "DXVK removal failed; recover the pending operation.");
  } else if (!applied) {
    static std::atomic<unsigned long> sequence{0}; std::string temporary; int fd = -1;
    for (unsigned attempt = 0; attempt < 128; ++attempt) {
      temporary = ".dxvk.conf.publication-" + std::to_string(getpid()) + "-" + std::to_string(sequence++);
      fd = game->CreateExclusiveFile(temporary, desired_mode);
      if (fd >= 0 || errno != EEXIST) break;
    }
    if (fd < 0) return Failure("publication-dxvk-write", "Exclusive DXVK temporary creation failed.");
    std::size_t offset = 0; bool valid = true;
    while (offset < o.plan.dxvk_config.size()) {
      const auto count = write(fd, o.plan.dxvk_config.data() + offset, o.plan.dxvk_config.size() - offset);
      if (count < 0 && errno == EINTR) continue;
      if (count <= 0) { valid = false; break; }
      offset += static_cast<std::size_t>(count);
    }
    const bool synced = valid && fsync(fd) == 0; const bool closed = close(fd) == 0;
    if (!synced || !closed || !held() || !game->RenameChild(temporary, *game, "dxvk.conf", current.present) || !game->Sync())
      return Failure("publication-dxvk-write", "DXVK publication failed; pending effects require recovery.");
  }
  if (!held() || !read(&current) || current.present != desired_present ||
      (current.present && (current.bytes != o.plan.dxvk_config || !mode_matches(current.mode, desired_mode))) || !held())
    return Failure("publication-dxvk-verify", "DXVK intended presence/bytes/mode could not be verified durably.");
  return {PackageActionState::kComplete, {}, {}};
}
PackageActionResult ApplyPendingPublicationFiles(const SettingsPublicationOperation& o,
    const PublicationPaths& paths, int borrowed, const std::filesystem::path& prefix,
    const std::function<bool(const std::filesystem::path&)>& after_effect) {
  std::string reason;
  if (!EncodeSettingsPublicationOperation(o, &reason) || !PathsMatch(o, paths, prefix))
    return Failure("publication-invalid", "Frozen file operation or canonical paths are invalid.");
  auto result = ApplyPendingPublicationRegistry(o, borrowed, prefix);
  if (!Complete(result)) return result;
  const auto control = Map(prefix, o.install.data_root + "/.playden-control");
  auto directory = ManagedDirectory::Open(control, DirectoryPolicy::kPrivate);
  auto held = [&] { return directory && LeaseHeld(borrowed, control); };
  auto checkpoint = [&](const std::filesystem::path& path) { return (!after_effect || after_effect(path)) && held(); };
  if (!checkpoint(Map(prefix, o.install.data_root + "/library/prefixes/game/user.reg")))
    return Failure("publication-interrupted", "Registry effect requires fresh recovery.");
  result = ApplyPendingPublicationDxvk(o, borrowed, prefix);
  if (!Complete(result)) return result;
  PwTitlePublicationMetadata current{}, previous{};
  if (pw_title_publication_metadata_parse(reinterpret_cast<const uint8_t*>(o.metadata_bytes.data()),
      o.metadata_bytes.size(), &current) != PW_OK || (!o.previous_metadata_bytes.empty() &&
      pw_title_publication_metadata_parse(reinterpret_cast<const uint8_t*>(o.previous_metadata_bytes.data()),
      o.previous_metadata_bytes.size(), &previous) != PW_OK))
    return Failure("publication-invalid", "Frozen publication metadata is invalid.");
  if (!checkpoint(Map(prefix, platform::OnGameMount(current.dxvk_path, o.install.install_path)))) return Failure("publication-interrupted", "DXVK effect requires fresh recovery.");
  auto library = ManagedDirectory::Open(Map(prefix, o.install.data_root + "/library"), DirectoryPolicy::kPrivateTraversable);
  if (!library) return Failure("publication-directory", "Owned private library is unavailable.");
  auto profiles = library->OpenChild("profiles", DirectoryPolicy::kPrivate, true);
  if (!profiles || !profiles->Sync() || !library->Sync() || !checkpoint(paths.profile.parent_path()))
    return Failure("publication-directory", "Private profile directory requires recovery.");
  auto inputs = library->OpenChild("input", DirectoryPolicy::kPrivate, true);
  if (!inputs || !inputs->Sync() || !library->Sync() || !checkpoint(paths.input_directory))
    return Failure("publication-directory", "Private input directory requires recovery.");
  auto title = ManagedDirectory::Open(paths.descriptor.parent_path(), DirectoryPolicy::kApprovedTree);
  struct stat title_info{};
  if (!title || lstat(paths.descriptor.parent_path().c_str(), &title_info) || title_info.st_uid != geteuid() ||
      ((title_info.st_mode & 07777) != 0700 && (title_info.st_mode & 07777) != 0755))
    return Failure("publication-directory", "Owned staged title directory is unavailable.");
  auto publish = [&](const ManagedDirectory& parent, const std::filesystem::path& path,
                     const std::string& bytes, const std::string& prior_hash) -> PackageActionResult {
    if (!held()) return Failure("publication-lock", "Publisher ownership changed before file effects.");
    auto observed = ReadPublicationFile(parent, path.filename().string(), kMaxSettingsPublicationOperationBytes);
    if (!observed) return Failure("publication-file-read", "Publication file identity or durability is unavailable.");
    const bool intended = observed->present && observed->mode == 0600 && observed->bytes == bytes;
    const auto digest = observed->present ? HashArtifactBytes(observed->bytes) : std::nullopt;
    // A file the previous committed set published must still be those bytes or the
    // intended ones (the resume rule). A new destination takes whatever an earlier
    // apply left there (a preset switched away from and back to): these files are
    // Playden's own output, and an interrupted write leaves the old or new file.
    const bool original = prior_hash.empty() || (observed->present && digest && *digest == prior_hash);
    if (!original && !intended)
      return Failure("publication-file-collision", "Publication file matches neither owned prior nor intended bytes.");
    if (!intended && !WritePublicationFile(parent, path.filename().string(), bytes, *observed, held))
      return Failure("publication-file-write", "Publication file write failed; recover retained pending evidence.");
    auto verified = ReadPublicationFile(parent, path.filename().string(), kMaxSettingsPublicationOperationBytes);
    if (!verified || !verified->present || verified->mode != 0600 || verified->bytes != bytes || !held())
      return Failure("publication-file-verify", "Publication file readback could not be verified.");
    if (!checkpoint(path)) return Failure("publication-interrupted", "Durable publication file requires fresh recovery.");
    return {PackageActionState::kComplete, {}, {}};
  };
  result = publish(*profiles, paths.profile, o.plan.profile_bytes,
      previous.profile_id == o.plan.profile_id ? previous.profile_sha256 : "");
  if (!Complete(result)) return result;
  for (const auto& input : o.plan.input_presets) {
    std::string prior;
    for (std::size_t i = 0; i < previous.input_count; ++i)
      if (input.first == previous.inputs[i].name) prior = previous.inputs[i].sha256;
    result = publish(*inputs, paths.input_directory / input.first, input.second, prior);
    if (!Complete(result)) return result;
  }
  result = publish(*profiles, paths.catalog, o.catalog_bytes, previous.catalog_sha256);
  if (!Complete(result)) return result;
  result = publish(*title, paths.descriptor, o.descriptor_bytes, previous.descriptor_sha256);
  if (!Complete(result)) return result;
  std::string previous_digest;
  if (!o.previous_metadata_bytes.empty()) {
    auto digest = HashArtifactBytes(o.previous_metadata_bytes);
    if (!digest) return Failure("publication-hash", "Prior metadata hashing is unavailable.");
    previous_digest = *digest;
  }
  return publish(*profiles, paths.metadata, o.metadata_bytes, previous_digest);
}
PackageActionResult PreflightSettingsPublication(const SettingsPublicationOperation& o,
    const PublicationPaths& paths, int borrowed, const std::filesystem::path& prefix, const GameRootMove* moved) {
  std::string reason;
  if (!EncodeSettingsPublicationOperation(o, &reason) || !PathsMatch(o, paths, prefix))
    return Failure("publication-invalid", "Frozen preflight operation or canonical paths are invalid.");
  auto data = ManagedDirectory::Open(Map(prefix, o.install.data_root), DirectoryPolicy::kPrivateTraversable);
  auto control = data ? data->OpenChild(".playden-control", DirectoryPolicy::kPrivate) : std::nullopt;
  auto held = [&] { return control && LeaseHeld(borrowed, control->path()); };
  if (!held()) return Failure("publication-lock", "Preflight publisher descriptor is unavailable or changed.");
  auto receipt = ReadReceipt(control->path(), *control);
  const bool initial = o.previous_receipt_bytes.empty();
  if (!receipt.ok || (initial ? !receipt.absent : receipt.absent || receipt.bytes != o.previous_receipt_bytes))
    return Failure("publication-preflight-receipt", "Preflight requires the exact previous receipt or initial absence.");
  auto library = data->OpenChild("library", DirectoryPolicy::kPrivateTraversable);
  auto title = ManagedDirectory::Open(paths.descriptor.parent_path(), DirectoryPolicy::kApprovedTree);
  struct stat title_info{};
  if (!library || !title || lstat(title->path().c_str(), &title_info) || title_info.st_uid != geteuid() ||
      ((title_info.st_mode & 07777) != 0700 && (title_info.st_mode & 07777) != 0755))
    return Failure("publication-preflight-directory", "Owned library or staged title directory is unavailable.");
  PwTitlePublicationMetadata previous{}, desired{};
  if (pw_title_publication_metadata_parse(reinterpret_cast<const uint8_t*>(o.metadata_bytes.data()),
      o.metadata_bytes.size(), &desired) != PW_OK)
    return Failure("publication-invalid", "Frozen preflight metadata is invalid.");
  // Destinations without a previous committed version are not refused: an earlier
  // apply's leftovers (presets switched A -> B -> A, an interrupted first setup) are
  // Playden's own files and are written over.
  if (!initial && NamesMovedRoot(moved, o.install.title_id, prefix)) {
    auto checked = VerifyMovedPublication(o.install, *moved, o.previous_receipt_bytes, o.previous_metadata_bytes, prefix,
                                          desired.game_root_unowned != 0);
    if (!Complete(checked)) return {PackageActionState::kFailed, "publication-preflight-previous", checked.message};
  } else if (!initial && OwnedReplacedByUnowned(o.previous_metadata_bytes, desired.game_root_unowned != 0)) {
    auto checked = VerifyMovedPublication(o.install, InPlace(o.install), o.previous_receipt_bytes, o.previous_metadata_bytes,
                                          prefix, true);
    if (!Complete(checked)) return {PackageActionState::kFailed, "publication-preflight-previous", checked.message};
  } else if (!initial) {
    const auto host = prefix.string(); PwTitlePublicationPlatform platform{};
    platform.host_root = host.empty() ? nullptr : host.c_str(); platform.owner_uid = geteuid();
    const auto mount = platform::UsbMountOf(o.install.install_path);
    platform.known_game_mount = mount.empty() ? nullptr : mount.c_str();
    PwTitlePublicationDescriptor descriptor{}; PwTitlePublicationContents contents{}; char error[256]{};
    auto hash = HashArtifactBytes(o.previous_metadata_bytes);
    if (!hash || pw_title_publication_metadata_parse(reinterpret_cast<const uint8_t*>(o.previous_metadata_bytes.data()),
        o.previous_metadata_bytes.size(), &previous) != PW_OK ||
        pw_title_publication_read_descriptor(("/data/homebrew/" + o.install.title_id + "/game-launch.json").c_str(),
        o.install.title_id.c_str(), &platform, &descriptor, error, sizeof(error)) != PW_OK ||
        descriptor.launch.app_id != o.install.runtime_number ||
        pw_title_publication_verify_locked(&descriptor, &platform, borrowed,
        reinterpret_cast<const uint8_t*>(o.previous_receipt_bytes.data()), o.previous_receipt_bytes.size(),
        hash->c_str(), &contents, error, sizeof(error)) != PW_OK)
      return {PackageActionState::kFailed, "publication-preflight-previous", error[0] ? error : "Previous complete publication is invalid."};
  }
  const auto dxvk = Map(prefix, platform::OnGameMount(desired.dxvk_path, o.install.install_path));
  const bool unowned = desired.game_root_unowned != 0;
  auto game = ManagedDirectory::Open(dxvk.parent_path(), GameDirectoryPolicy(unowned));
  struct stat game_info{};
  if (!game || lstat(game->path().c_str(), &game_info) || !OwnedGameDirectory(game_info, unowned))
    return Failure("publication-preflight-directory", "Owned game working directory is unavailable.");
  // By bytes, as the DXVK effect decides "original". The registry comparison below
  // keeps the mode, which the registry effect's resume rule also compares.
  auto before = ReadPublicationFile(*game, "dxvk.conf", 256 * 1024);
  if (!before || before->present != o.dxvk_before.present || (before->present && before->bytes != o.dxvk_before.bytes))
    return Failure("publication-preflight-snapshot", "Live DXVK disagrees with the frozen before snapshot.");
  // A dxvk.conf Playden did not write (one the game ships) is kept in the frozen
  // before-snapshot, then written over or removed as the title's contract requires.
  auto prefixes = library->OpenChild("prefixes", DirectoryPolicy::kPrivateTraversable);
  auto bottle = prefixes ? prefixes->OpenChild("game", DirectoryPolicy::kPrivateTraversable) : std::nullopt;
  auto registry = bottle ? ReadPublicationFile(*bottle, "user.reg", kMaxPublicationRegistryBackupBytes) : std::nullopt;
  if (!registry || !registry->present)
    return Failure("publication-preflight-registry", "Owned bottle registry is unavailable.");
  if (o.registry_backup) {
    auto backup = LoadPublicationRegistryBackup(control->path(), o.transaction_id, *o.registry_backup);
    if (backup.state != PublicationRecoveryState::kValid || registry->bytes != backup.bytes ||
        registry->mode != o.registry_backup->source_mode)
      return Failure("publication-preflight-snapshot", "Live registry disagrees with its immutable before backup.");
  }
  auto rendered = registry->bytes;
  if (!ApplyRegistryValues(&rendered, o.plan.registry, o.registry_unix_time, &reason) ||
      rendered.size() > kMaxPublicationRegistryBackupBytes)
    return Failure("publication-preflight-registry", "Frozen registry edits cannot be rendered against the original registry.");
  if (!held()) return Failure("publication-lock", "Publisher ownership changed during preflight.");
  return {PackageActionState::kComplete, {}, {}};
}
Result<SettingsPublicationOperation> CaptureSettingsPublicationOperation(const RuntimeInstall& r,
    const SettingsLaunchPlan& plan, const std::string& kit_id, const std::string& transaction,
    std::int64_t unix_time, int borrowed, const std::filesystem::path& prefix, const GameRootMove* moved,
    bool unowned_game_roots) {
  auto fail = [](const std::string& message) {
    return Result<SettingsPublicationOperation>::Failure(StorageError::kInvalidData, message);
  };
  if (!ValidateRuntimeInstallRecord(r) || unix_time < 0 ||
      (!prefix.empty() && (!prefix.is_absolute() || prefix != prefix.lexically_normal())))
    return fail("Invalid record, timestamp or canonical namespace.");
  SettingsPublicationOperation o; o.install = r; o.plan = plan; o.kit_id = kit_id;
  o.transaction_id = transaction; o.registry_unix_time = unix_time;
  o.catalog_bytes = plan.profile_id + ".profile\n";
  // Schema 2 binds the recorded download root, which is independent of the native title. Schema 3 binds a USB
  // game's folder on no mount in particular: the plan's profile says which this is.
  PwGameProfile planned{}; char planned_working[1024]{};
  const auto planned_bottle = r.data_root + "/library/prefixes/game";
  const bool searches_usb = pw_game_profile_parse(reinterpret_cast<const uint8_t*>(plan.profile_bytes.data()), plan.profile_bytes.size(), &planned) == PW_OK &&
      !pw_wine_launch_host_dir(planned_bottle.c_str(), planned.app.working_directory, planned_working, sizeof(planned_working)) &&
      platform::IsUsbSearchPath(planned_working);
  o.descriptor_bytes = std::string("{\"schema_version\":") + (searches_usb ? "3" : "2") + ",\"app_id\":" + std::to_string(r.runtime_number) +
      ",\"settings_revision\":" + std::to_string(plan.settings_revision);
  auto field = [&](const char* name, const std::string& value) {
    o.descriptor_bytes += ",\""; o.descriptor_bytes += name; o.descriptor_bytes += "\":\"";
    for (unsigned char c : value) {
      if (c < 32 || c > 126) return false;
      if (c == '"' || c == '\\') o.descriptor_bytes += '\\';
      o.descriptor_bytes += static_cast<char>(c);
    }
    o.descriptor_bytes += '"'; return true;
  };
  if (!field("title_id", r.title_id) || !field("install_id", r.install_id) || !field("data_root", r.data_root) ||
      !field("default_profile_id", plan.profile_id) || !field("runtime_bundle_id", r.runtime_bundle_id) ||
      !field("recipe_id", plan.recipe_id) || !field("recipe_version", plan.recipe_version) || !field("exit_policy", r.exit_policy) ||
      !field("game_root", searches_usb ? platform::WithoutUsbMountNumber(r.install_path) : r.install_path))
    return fail("Descriptor identities cannot be represented safely.");
  o.descriptor_bytes += "}\n";
  PwGameProfile profile{}; char working[1024]{};
  const auto bottle_path = r.data_root + "/library/prefixes/game";
  if (pw_game_profile_parse(reinterpret_cast<const uint8_t*>(plan.profile_bytes.data()), plan.profile_bytes.size(), &profile) != PW_OK ||
      pw_wine_launch_host_dir(bottle_path.c_str(), profile.app.working_directory, working, sizeof(working)))
    return fail("Frozen profile cannot produce its canonical working directory.");
  auto bind = [](auto& target, const std::string& value) {
    if (value.size() >= sizeof(target)) return false;
    std::memcpy(target, value.c_str(), value.size() + 1); return true;
  };
  auto hash = [&](auto& target, const std::string& bytes) {
    auto digest = HashArtifactBytes(bytes); return digest && bind(target, *digest);
  };
  // Schema 3 (game_root_unowned) lets the kit's launch admission accept a game folder of any
  // owner and mode: every folder when the kit can, else one on a drive that records no owner
  // or permissions (exFAT).
  bool unowned = unowned_game_roots || searches_usb;
  if (!unowned) {
    const auto records_ownership = platform::RecordsOwnership(Map(prefix, working));
    if (!records_ownership) return fail("The game's working directory cannot be checked.");
    unowned = !*records_ownership;
  }
  PwTitlePublicationMetadata m{}; m.schema_version = unowned ? 3 : 2;
  m.game_root_unowned = unowned ? 1 : 0; m.settings_revision = plan.settings_revision;
  if (!bind(m.install_id, r.install_id) || !bind(m.title_id, r.title_id) || !bind(m.data_root, r.data_root) ||
      !bind(m.profile_id, plan.profile_id) || !bind(m.runtime_bundle_id, r.runtime_bundle_id) ||
      !bind(m.recipe_id, plan.recipe_id) || !bind(m.recipe_version, plan.recipe_version) || !bind(m.kit_id, kit_id) ||
      !hash(m.profile_sha256, plan.profile_bytes) || !hash(m.descriptor_sha256, o.descriptor_bytes) ||
      !hash(m.catalog_sha256, o.catalog_bytes) || plan.input_presets.size() > 64 ||
      !bind(m.dxvk_path, std::string(working) + "/dxvk.conf")) return fail("Publication identities or hashes exceed the contract.");
  m.input_count = plan.input_presets.size();
  for (std::size_t i = 0; i < plan.input_presets.size(); ++i)
    if (!bind(m.inputs[i].name, plan.input_presets[i].first) || !hash(m.inputs[i].sha256, plan.input_presets[i].second))
      return fail("Input preset cannot be bound to completion metadata.");
  m.dxvk_present = !plan.dxvk_config.empty();
  if (m.dxvk_present && !hash(m.dxvk_sha256, plan.dxvk_config)) return fail("DXVK hashing failed.");
  const std::string executable = profile.app.executable;
  const auto basename = executable.substr(executable.find_last_of('\\') + 1);
  m.registry_count = 2;
  m.registry[0].kind = m.registry[1].kind = PW_TITLE_PUBLICATION_REGISTRY_ABSENT;
  if (!bind(m.registry[0].key, "Software\\Wine\\AppDefaults\\" + basename) ||
      !bind(m.registry[0].name, "Version") || !bind(m.registry[1].key, "Control Panel\\Desktop") ||
      !bind(m.registry[1].name, "LogPixels")) return fail("Owned registry identities exceed the contract.");
  auto equal = [](const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
      auto lower = [](unsigned char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; };
      if (lower(a[i]) != lower(b[i])) return false;
    }
    return true;
  };
  unsigned seen = 0;
  for (const auto& value : plan.registry) {
    int index = -1;
    for (int i = 0; i < 2; ++i) if (equal(value.key, m.registry[i].key) && equal(value.name, m.registry[i].name)) index = i;
    if (index < 0 || (seen & (1u << index))) return fail("Registry plan contains an unsupported or duplicate owned target.");
    seen |= 1u << index;
    auto& target = m.registry[index];
    if (value.data.empty()) continue;
    if (index == 0 && value.data.size() >= 2 && value.data.front() == '"' && value.data.back() == '"') {
      target.kind = PW_TITLE_PUBLICATION_REGISTRY_STRING;
      if (!bind(target.string_value, value.data.substr(1, value.data.size() - 2))) return fail("Registry string exceeds its bound.");
    } else if (index == 1 && value.data.size() == 14 && value.data.compare(0, 6, "dword:") == 0) {
      target.kind = PW_TITLE_PUBLICATION_REGISTRY_DWORD;
      auto parsed = std::from_chars(value.data.data() + 6, value.data.data() + 14, target.dword, 16);
      if (parsed.ec != std::errc{} || parsed.ptr != value.data.data() + 14) return fail("Registry DWORD is not canonical.");
    } else return fail("Registry plan has an unsupported value type.");
  }
  auto metadata = SerializePublicationMetadata(m);
  if (!metadata) return fail("Completion metadata is invalid."); o.metadata_bytes = *metadata;
  auto data = ManagedDirectory::Open(Map(prefix, r.data_root), DirectoryPolicy::kPrivateTraversable);
  auto control = data ? data->OpenChild(".playden-control", DirectoryPolicy::kPrivate) : std::nullopt;
  auto held = [&] { return control && LeaseHeld(borrowed, control->path()); };
  if (!held()) return fail("Qualified publisher descriptor/private tree is unavailable.");
  auto receipt = ReadReceipt(control->path(), *control);
  if (!receipt.ok) return fail("Before receipt is corrupt or unavailable.");
  auto library = data->OpenChild("library", DirectoryPolicy::kPrivateTraversable);
  if (!library) return fail("Private library is unavailable.");
  if (!receipt.absent) {
    PwTitlePublication parsed{}; PwTitlePublicationDescriptor descriptor{}; PwTitlePublicationContents contents{};
    const auto host = prefix.string(); PwTitlePublicationPlatform platform{};
    platform.host_root = host.empty() ? nullptr : host.c_str(); platform.owner_uid = geteuid(); char error[256]{};
    auto profiles = library->OpenChild("profiles", DirectoryPolicy::kPrivate);
    auto prior = profiles ? ReadPublicationFile(*profiles, ".playden-publication", 32768) : std::nullopt;
    if (prior && prior->present && NamesMovedRoot(moved, r.title_id, prefix)) {
      auto checked = VerifyMovedPublication(r, *moved, receipt.bytes, prior->bytes, prefix, unowned);
      if (!Complete(checked) || pw_title_publication_parse(reinterpret_cast<const uint8_t*>(receipt.bytes.data()),
                                                           receipt.bytes.size(), &parsed) != PW_OK)
        return fail("Before state is not the verified publication of the moved game folder: " + checked.message);
    } else if (prior && prior->present && OwnedReplacedByUnowned(prior->bytes, unowned)) {
      auto checked = VerifyMovedPublication(r, InPlace(r), receipt.bytes, prior->bytes, prefix, true);
      if (!Complete(checked) || pw_title_publication_parse(reinterpret_cast<const uint8_t*>(receipt.bytes.data()),
                                                           receipt.bytes.size(), &parsed) != PW_OK)
        return fail("Before state is not the verified publication of this game folder: " + checked.message);
    } else if (const auto known_mount = platform::UsbMountOf(r.install_path);
        pw_title_publication_parse(reinterpret_cast<const uint8_t*>(receipt.bytes.data()), receipt.bytes.size(), &parsed) != PW_OK ||
        parsed.phase != PW_TITLE_PUBLICATION_COMMITTED || !prior || !prior->present ||
        pw_title_publication_read_descriptor(("/data/homebrew/" + r.title_id + "/game-launch.json").c_str(), r.title_id.c_str(),
        &platform, &descriptor, error, sizeof(error)) != PW_OK || descriptor.launch.app_id != r.runtime_number ||
        (platform.known_game_mount = known_mount.empty() ? nullptr : known_mount.c_str(), false) ||
        pw_title_publication_verify_locked(&descriptor, &platform, borrowed,
        reinterpret_cast<const uint8_t*>(receipt.bytes.data()), receipt.bytes.size(), parsed.publication_sha256,
        &contents, error, sizeof(error)) != PW_OK)
      return fail("Before state is not a verified committed complete publication.");
    o.previous_revision = parsed.settings_revision; o.previous_receipt_bytes = receipt.bytes; o.previous_metadata_bytes = prior->bytes;
  }
  auto game = ManagedDirectory::Open(Map(prefix, platform::OnGameMount(working, r.install_path)), GameDirectoryPolicy(m.game_root_unowned != 0));
  auto dxvk = game ? ReadPublicationFile(*game, "dxvk.conf", 256 * 1024) : std::nullopt;
  if (!dxvk) return fail("Before DXVK snapshot cannot be verified.");
  o.dxvk_before = *dxvk;
  if (dxvk->present) {
    auto digest = HashArtifactBytes(dxvk->bytes); if (!digest) return fail("Before DXVK hashing failed."); o.dxvk_before.sha256 = *digest;
  }
  auto prefixes = library->OpenChild("prefixes", DirectoryPolicy::kPrivateTraversable);
  auto bottle = prefixes ? prefixes->OpenChild("game", DirectoryPolicy::kPrivateTraversable) : std::nullopt;
  auto registry = bottle ? ReadPublicationFile(*bottle, "user.reg", kMaxPublicationRegistryBackupBytes) : std::nullopt;
  if (!registry || !registry->present) return fail("Before registry cannot be verified.");
  auto intended_registry = registry->bytes; std::string reason;
  if (!ApplyRegistryValues(&intended_registry, plan.registry, unix_time, &reason) || intended_registry.size() > kMaxPublicationRegistryBackupBytes)
    return fail("Registry plan cannot be applied to its captured original.");
  if (!plan.registry.empty()) {
    auto digest = HashArtifactBytes(registry->bytes); if (!digest) return fail("Before registry hashing failed.");
    o.registry_backup = RegistryBackupBinding{"registry-backup-" + transaction + ".v1", registry->mode, registry->size, *digest};
  }
  auto frozen = FreezeSettingsPublicationOperation(o, &reason);
  if (!frozen || !held()) return fail(frozen ? "Publisher ownership changed during capture." : reason);
  if (o.registry_backup) {
    // Preflight reads this backup. A backup left by an earlier capture of this
    // same transaction (for example one refused by preflight) is accepted
    // only when it verifies against this binding and equals the live original
    // captured now, i.e. exactly what this capture would write. Any other file
    // remains orphan evidence and refuses capture.
    const auto existing = LoadPublicationRegistryBackup(control->path(), transaction, *o.registry_backup);
    const bool identical = existing.state == PublicationRecoveryState::kValid && existing.bytes == registry->bytes;
    if (!identical) {
      auto backup = CreatePublicationRegistryBackup(control->path(), transaction, registry->bytes, registry->mode);
      if (!backup.ok() || backup.value().filename != o.registry_backup->filename || backup.value().sha256 != o.registry_backup->sha256 ||
          backup.value().size != o.registry_backup->size || backup.value().source_mode != o.registry_backup->source_mode)
        return fail("Exclusive registry backup preparation failed; preserve orphan evidence.");
    }
    if (!held()) return fail("Publisher ownership changed during capture.");
  }
  const auto profiles = r.data_root + "/library/profiles/";
  PublicationPaths paths{Map(prefix, profiles + plan.profile_id + ".profile"), Map(prefix, profiles + "profiles.lst"),
      Map(prefix, r.data_root + "/library/input"), Map(prefix, "/data/homebrew/" + r.title_id + "/game-launch.json"),
      Map(prefix, profiles + ".playden-publication")};
  auto preflight = PreflightSettingsPublication(*frozen, paths, borrowed, prefix, moved);
  if (!Complete(preflight) || !held()) return fail(preflight.message.empty() ? "Captured operation preflight failed." : preflight.message);
  return Result<SettingsPublicationOperation>::Success(std::move(*frozen));
}
}  // namespace playden::runtime
