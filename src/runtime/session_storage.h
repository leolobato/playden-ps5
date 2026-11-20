#pragma once

#include "platform/storage.h"
#include "platform/managed_directory.h"
#include "runtime/session_reconciler.h"

#include <cstddef>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>

namespace playden::runtime {

// Host-only mapping of the canonical console /data mount. RuntimeInstall
// identity remains canonical; arbitrary record roots are never accepted.
struct HostDataRootForTests {
  std::filesystem::path path;
};

class FilesystemReconcileSessionSource final : public ReconcileSessionSource {
public:
  static constexpr std::size_t kMaxSessionBytes = 4096;
  static constexpr std::size_t kMaxMarkerBytes = 4096;
  static constexpr std::size_t kMaxDirectoryEntries = 1024;

  FilesystemReconcileSessionSource();
  explicit FilesystemReconcileSessionSource(HostDataRootForTests mapping);

  SessionEnumeration Enumerate(const RuntimeInstall &record,
                               std::size_t limit) override;
  // One session record by id, with the same ownership checks and no bound on
  // how many other records exist. A record not written yet is an empty result.
  SessionEnumeration Read(const RuntimeInstall &record,
                          const std::string &session_id);

  // When the install's title last ran a game (#446): the modification time (unix seconds) of its newest session
  // record that reached running or exited, from Home or from Playden. The title writes each phase by replacing the
  // record, so for an ended session that is when it ended. Unlike Enumerate this reads as much as it can: a record
  // it can't read, a temporary file or a record of another install is skipped, and there is no record limit (only
  // kMaxPlayedEntries entries are looked at). A record modified after `not_after` (the clock was set back since) is
  // passed over. Nothing when no record qualifies or the folder can't be opened.
  static constexpr std::size_t kMaxPlayedEntries = 8192;
  std::optional<std::int64_t> LastPlayed(const RuntimeInstall &record,
                                         std::int64_t not_after = std::numeric_limits<std::int64_t>::max());
  // The same for one session record by id (the one a Playden launch named).
  std::optional<std::int64_t> SessionPlayed(const RuntimeInstall &record,
                                            const std::string &session_id);

  // Historical runtime/revision identities are returned unchanged so the
  // reconciler can reject each one without hiding later records. This adapter
  // does not archive old records; reaching a bound or finding temporary/lock
  // evidence leaves the whole install pending for explicit recovery.

private:
  std::optional<platform::ManagedDirectory> OpenSessions(const RuntimeInstall &record,
                                               SessionEnumeration *failure);
  std::filesystem::path host_data_root_;
};

// Storage must name a caller-provisioned private 0700 root dedicated to
// reconciliation journals, separate from the wrapper's logs/sessions tree.
class DurableReconcileJournal final : public ReconcileJournal {
public:
  static constexpr std::size_t kMaxJournalBytes = 16 * 1024;

  explicit DurableReconcileJournal(Storage &storage);
  ReconcileJournalLoad Load(const std::string &session_id) override;
  ReconcileJournalResult Save(const ReconcileEntry &entry) override;

private:
  Storage &storage_;
};

} // namespace playden::runtime
