// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "steam/backend/vdf.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace playden::runtime {

// Steam Workshop items (#856) for Valve's Steam client (Steam features = Online). With the game in the bottle's Steam
// library (Steam library folder = Steam's folder), the client keeps the game's Workshop state in
// <W>/appworkshop_<appid>.acf, W being the bottle's steamapps/workshop folder (steam/backend/workshop_items.h). It
// downloads nothing while that file lists no installed items, and the game then sees none. Playden adds the items it
// holds to the file's WorkshopItemsInstalled; from there the client keeps every item the account subscribes to.

// A file larger than this is not read, so never changed.
inline constexpr std::size_t kWorkshopAcfMaxBytes = 1u << 20;
// More items than this are refused whole.
inline constexpr std::size_t kWorkshopAcfMaxItems = 4096;

// "appworkshop_<appid>.acf"
std::string WorkshopAcfName(std::uint32_t app_id);

// One item Playden holds in W/content/<appid>/<id>.
struct WorkshopAcfItem {
  std::uint64_t id = 0;
  std::uint64_t bytes = 0;
  std::uint32_t time_updated = 0;
  std::uint64_t manifest = 0;
};

// A VdfNode tree (the root's children are the top-level pairs) as Valve writes text KeyValues: tab indents, braces on
// their own lines, "key"<tab><tab>"value", a line end after each line. Backslash, quote, line feed and tab are escaped
// as ParseVdf reads them, so ParseVdf gives the same tree back.
std::string SerializeVdf(const steam::VdfNode& root);

// The appworkshop file with the items added to WorkshopItemsInstalled as {size, timeupdated, manifest}. An item the
// file already lists is never changed (the client may have updated it); every other key keeps its value and its place.
// When an item is added, NeedsDownload becomes "0" and SizeOnDisk the sum of the installed items' sizes (either is
// added when missing). Nothing to add: `existing` as it is. Without a file, a new one in the client's format with the
// items; with no items, none. Nullopt (leave the file alone): it doesn't parse, its root isn't one AppWorkshop
// dict with this app ID, its WorkshopItemsInstalled isn't a dict, it or the file the merge makes is over
// kWorkshopAcfMaxBytes, or there are more than kWorkshopAcfMaxItems items.
std::optional<std::string> MergeAppWorkshopAcf(std::optional<std::string_view> existing, std::uint32_t app_id,
                                               const std::vector<WorkshopAcfItem>& items);

// The file without the given items in WorkshopItemsInstalled (Playden deleted their folders), SizeOnDisk worked out
// again; WorkshopItemDetails and every other key stay as they are. None of them listed: `existing` as it is. Nullopt as
// MergeAppWorkshopAcf refuses a file.
std::optional<std::string> DropFromAppWorkshopAcf(std::string_view existing, std::uint32_t app_id,
                                                  const std::set<std::uint64_t>& ids);

// The items the file lists as installed with a manifest (Valve's client downloaded them), in its order; at most
// kWorkshopAcfMaxItems. Empty for a file MergeAppWorkshopAcf would refuse.
std::vector<WorkshopAcfItem> ParseAppWorkshopInstalled(std::string_view existing, std::uint32_t app_id);

enum class WorkshopAcfOutcome {
  kNoItems,    // nothing to add: the file isn't read or written
  kWritten,    // written with the items added
  kUnchanged,  // it lists every item already
  kLeftAlone,  // a file Playden can't read or doesn't understand (a link, too big, another format): left as it is
  kFailed,     // W is not a real folder (or one on the way), more than kWorkshopAcfMaxItems items, or the write failed
};
const char* WorkshopAcfOutcomeName(WorkshopAcfOutcome outcome);

// ParseAppWorkshopInstalled of W/appworkshop_<appid>.acf, read only; empty when it is missing or can't be read.
std::vector<WorkshopAcfItem> ReadAppWorkshopInstalled(const std::string& workshop_folder, std::uint32_t app_id);
// DropFromAppWorkshopAcf on W/appworkshop_<appid>.acf, written as ApplyAppWorkshopAcf writes. No file: kNoItems.
WorkshopAcfOutcome RemoveFromAppWorkshopAcf(const std::string& workshop_folder, std::uint32_t app_id,
                                            const std::set<std::uint64_t>& ids);

// MergeAppWorkshopAcf on W/appworkshop_<appid>.acf, W opened through ManagedDirectory (no link anywhere on the way,
// never created). Written atomically (a temporary file renamed over it), 0644, only when it changes.
WorkshopAcfOutcome ApplyAppWorkshopAcf(const std::string& workshop_folder, std::uint32_t app_id,
                                       const std::vector<WorkshopAcfItem>& items);

}  // namespace playden::runtime
