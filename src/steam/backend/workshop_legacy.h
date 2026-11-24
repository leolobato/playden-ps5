// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "steam/backend/http_client.h"
#include "steam/backend/workshop_client.h"
#include "steam/backend/workshop_items.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace playden::steam {

// Workshop items from Steam's old UGC system (#1319, Left 4 Dead 2): no depot manifest, one file at the item's
// file_url on Steam's user content servers. The URL ends in the file's SHA-1, which the download is checked against:
//   https://cdn.steamusercontent.com/ugc/<UGC handle>/<SHA-1, 40 hex digits>/
// The game asks Steam for the file itself (ISteamRemoteStorage::UGCDownload): with Valve's client it lands in the
// game's own folder (left4dead2/addons/workshop/<id>.vpk); with gbe_fork, which serves it from mods.json, Playden's
// copy in the bottle is what the game receives.

// The emulator reports a mod's file size as an int32 (mods.json primary_filesize): a larger file would be cut short.
inline constexpr std::uint64_t kLegacyWorkshopMaxBytes = 0x7fffffffull;
// Bytes asked for per request (a Range): within the HTTP client's streamed-body cap.
inline constexpr std::uint64_t kLegacyWorkshopPieceBytes = 32ull << 20;

struct LegacyWorkshopFile {
  std::string url;
  std::string sha1;  // lowercase hex, from the URL
  std::string name;  // the file's name: the last component of Steam's filename
  std::uint64_t bytes = 0;
};

// The file an old-style item downloads, when Playden can fetch it: a UGC handle (its version), https on
// steamusercontent.com (or a subdomain), a /ugc/<number>/<SHA-1>/ path, a size from 1 byte to kLegacyWorkshopMaxBytes,
// and a name that is a plain file name
// (no folder, not "." or "..", no .playden- prefix, no control characters, at most 255 bytes) once Steam's folders
// are cut off. None otherwise: the item stays "can't be downloaded".
std::optional<LegacyWorkshopFile> LegacyWorkshopFileOf(const WorkshopItem& item);

// The item's preview picture, fetched beside its file (the game shows it in its add-on list): checked the same way
// against its preview_url, at most kLegacyWorkshopPreviewMaxBytes, saved as kLegacyWorkshopPreviewName. None when the
// item isn't fetchable, has no usable preview, or its own file already has that name.
inline constexpr const char* kLegacyWorkshopPreviewName = "preview.jpg";
inline constexpr std::uint64_t kLegacyWorkshopPreviewMaxBytes = 16ull << 20;
std::optional<LegacyWorkshopFile> LegacyWorkshopPreviewOf(const WorkshopItem& item);

// What carries one HTTP request (HttpClient::Perform; tests replace it).
using LegacyWorkshopPerform = std::function<HttpResponse(const HttpRequest&, const std::atomic<bool>&)>;

// Downloads the item's file into staging_root/<name> in pieces, then checks its size and SHA-1, then its preview
// picture the same way (a preview that fails is left out, never the item), and writes the file list and tree
// DepotInstaller leaves (DepotInstaller::kFileList, kFileTree), so the sync puts it in place the same
// way. Each piece must come back as the Range asked for (206 with that Content-Range; 200 only for a whole file in one
// piece). Transport trouble and server errors (5xx, 408, 429) are kNetwork, which waits for the network; a file the
// server refuses or doesn't have (other 4xx) is kRejected; a server that keeps answering with other bytes than asked,
// or a wrong size or SHA-1, is kCorrupt: both fail only this item. A full drive is kNoSpace; other write failures
// kStorage; a cancel, also while the file is checked, kCanceled.
WorkshopDownloadResult DownloadLegacyWorkshopItem(const LegacyWorkshopPerform& perform, const WorkshopItem& item,
                                                  const std::string& staging_root, const std::atomic<bool>& cancel,
                                                  const WorkshopProgressFn& progress,
                                                  std::uint64_t piece_bytes = kLegacyWorkshopPieceBytes);

}  // namespace playden::steam
