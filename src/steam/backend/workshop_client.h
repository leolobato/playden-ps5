// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "steam/backend/cm_client.h"
#include "steam/session_store.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace playden::steam {

// The Steam Workshop items the player subscribed to for one game (#856), read over the CM:
// PublishedFile.GetUserFiles#1 with type "mysubscriptions". Read-only.

inline constexpr std::size_t kWorkshopTitleBytes = 128;
inline constexpr std::size_t kWorkshopFilenameBytes = 260;
inline constexpr std::uint32_t kWorkshopPageSize = 100;
inline constexpr std::uint32_t kWorkshopMaxPages = 10;
inline constexpr std::size_t kWorkshopMaxItems = 1000;

struct WorkshopItem {
  std::uint64_t id = 0;
  std::string title;     // sanitised (SanitiseWorkshopText), at most kWorkshopTitleBytes
  std::string filename;  // sanitised, at most kWorkshopFilenameBytes
  std::uint64_t file_size = 0;
  std::string file_url;        // empty when Steam's URL was longer than Playden keeps (see has_file_url)
  bool has_file_url = false;   // Steam named a URL: a legacy item
  std::string preview_url;     // the item's preview picture; empty when Steam's URL was longer than Playden keeps
  std::uint64_t preview_size = 0;
  // hcontent_file: the item's manifest in the game's Workshop depot. For a legacy item it is the file's UGC handle
  // instead, which no depot holds (Left 4 Dead 2, #1319).
  std::uint64_t manifest = 0;
  std::uint32_t time_created = 0;
  std::uint32_t time_updated = 0;
  std::uint32_t consumer_appid = 0;
  std::uint64_t creator = 0;   // the author's SteamID
  std::uint32_t file_type = 0;
  bool banned = false;  // listed, but never downloaded (see PlanWorkshopSync)

  // An item with a depot manifest. Legacy items (from the old UGC system: Steam names a file_url, and their
  // hcontent_file is not a manifest) are listed but Playden does not download them. A URL decides it, as in
  // DepotDownloader: a depot item has none.
  bool legacy() const { return manifest == 0 || has_file_url || !file_url.empty(); }
  bool downloadable() const { return !legacy(); }
};

// Valid UTF-8 (each invalid sequence becomes U+FFFD), without control characters (C0, DEL, C1, U+2028/9),
// cut at `max_bytes` on a code point boundary. For third-party text that ends up in files and on screen.
std::string SanitiseWorkshopText(std::string_view text, std::size_t max_bytes);

// What one page held, for a log line without names or IDs.
struct WorkshopPageStats {
  std::uint32_t total = 0;      // what Steam says the whole list holds
  std::size_t received = 0;     // details on this page
  std::size_t kept = 0;         // listed and not banned
  std::size_t failed = 0;       // result other than 1 and 9: the list is incomplete
  std::size_t gone = 0;         // result 9 (file not found): the item was deleted from the Workshop
  std::size_t banned = 0;       // listed, flagged banned
  std::size_t legacy = 0;       // listed, from the old UGC system (WorkshopItem::legacy): never downloaded
  std::size_t other_app = 0;
  std::size_t not_item = 0;     // file_type other than 0 (collections, art, guides...)
  std::size_t invalid = 0;      // no ID, or damaged
};

// The request body for one page (1-based) of `steam_id`'s subscriptions for `app_id`.
std::vector<std::uint8_t> WorkshopSubscriptionsRequest(std::uint64_t steam_id, std::uint32_t app_id, std::uint32_t page);
// Parses one page. Items in the answer's order, filtered (see WorkshopPageStats): items of another app, other
// file types, failed and damaged details are left out; banned ones are listed with `banned` set. None when the
// answer itself is damaged (a bad item is only skipped).
std::optional<std::vector<WorkshopItem>> ParseWorkshopSubscriptions(const std::vector<std::uint8_t>& reply,
                                                                    std::uint32_t app_id, WorkshopPageStats* stats = nullptr);

struct WorkshopSubscriptions {
  CmError error = CmError::kProtocol;
  int eresult = 0;
  std::vector<WorkshopItem> items;  // unique IDs, in Steam's order
  std::uint32_t total = 0;          // Steam's count, before filtering
  std::uint32_t pages = 0;
  // The list may not name every subscription: it stopped at kWorkshopMaxPages / kWorkshopMaxItems, a page came
  // back empty before `total`, or Steam couldn't describe an item (a failed or damaged detail). Items Playden holds
  // that it doesn't name must stay.
  bool truncated = false;
  bool ok() const { return error == CmError::kNone; }
};

using WorkshopLog = std::function<void(const std::string& line)>;
// Runs `work` on one logged-on session, as CmClient::WithServiceSession does. The seam tests replace.
using WorkshopSessionRunner =
    std::function<CmError(const std::function<CmError(const CmClient::ServiceCall& call, std::uint64_t steam_id)>& work)>;

// Every page, 1.., until Steam's total is reached, a page comes back empty (cut short), or the caps. One log line.
WorkshopSubscriptions FetchWorkshopSubscriptions(const WorkshopSessionRunner& run, std::uint32_t app_id,
                                                 const std::atomic<bool>& cancel, const WorkshopLog& log = {});
WorkshopSubscriptions FetchWorkshopSubscriptions(CmClient& client, const StoredSession& session, std::uint32_t app_id,
                                                 const std::atomic<bool>& cancel, const WorkshopLog& log = {});

// The depot a game's Workshop items download from: appinfo depots/workshopdepot when it names one, else the
// app ID itself (as DepotDownloader). `appinfo` is the app's PICS text VDF.
std::uint32_t WorkshopDepotId(std::uint32_t app_id, std::string_view appinfo);

}  // namespace playden::steam
