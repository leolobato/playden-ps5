// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "library/model.h"
#include "steam/backend/cm_client.h"
#include "steam/session_store.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace playden::steam {

// The player's Steam collections (#536), read over the CM: CloudConfigStore.Download#1 for the library namespace
// (1), which holds one "user-collections.<id>" entry per collection with a JSON value. Read-only.

// What one read found, for a log line without names or IDs.
struct CollectionsReadStats {
  std::size_t reply_bytes = 0;
  std::size_t entries = 0;      // every entry in the namespace
  std::size_t collections = 0;  // kept
  std::size_t deleted = 0;      // entries marked deleted
  std::size_t special = 0;      // "favorite" and "hidden": Playden keeps its own
  std::size_t dynamic = 0;      // filled by Steam from a saved filter: left out
  std::size_t invalid = 0;      // an ID, name or value Playden can't keep
};

// The request body: the library namespace from version 0 (everything, not changes since a version).
std::vector<std::uint8_t> LibraryCollectionsRequest();
// Parses Download's answer. The collections are in the order the answer gives them, members as Steam games.
// None when the answer itself is damaged (a bad entry is only skipped).
std::optional<std::vector<LibraryCollection>> ParseLibraryCollections(const std::vector<std::uint8_t>& reply,
                                                                      CollectionsReadStats* stats = nullptr);

using CollectionsLog = std::function<void(const std::string& line)>;
// Opens a CM session for `session`, asks for the collections, and parses them. None when Steam can't be reached,
// refuses the call, or answers something damaged; `log` (optional) gets one line either way.
std::optional<std::vector<LibraryCollection>> FetchLibraryCollections(CmClient& client, const StoredSession& session,
                                                                      const std::atomic<bool>& cancel,
                                                                      const CollectionsLog& log = {});

}  // namespace playden::steam
