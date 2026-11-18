// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace playden::runtime {

// param.json attribute3 bit of a title that supports variable refresh rate (VRR). With it the system drives a
// VRR display at the rate the game shows frames, also at 120 Hz, without "Apply to Unsupported Games".
inline constexpr std::uint64_t kTitleAttribute3VariableRefresh = 0x40000;

// The title's attribute3, or nothing when param.json has no single plain decimal "attribute3".
std::optional<std::uint64_t> TitleAttribute3(const std::string& param_json);

// Sets or clears the VRR bit in param.json's single plain decimal "attribute3"; the rest of the text is kept as it
// is. False, with param_json unchanged, for any other layout.
bool SetTitleVariableRefresh(std::string* param_json, bool supported);

// Removes param.json's single top-level plain decimal "downloadDataSize" entry, the rest of the text kept as it is.
// The system gives a title with one a download-data area of that many MiB (/download0, which a game title never
// uses): it makes /user/download/<ID>/download0.dat when the title is registered, fully allocated, and keeps the
// size it registered with until the title is registered again (#1001). True when param_json has no such key
// afterwards, also when it had none. False, with param_json unchanged, for any other layout.
bool RemoveTitleDownloadData(std::string* param_json);

// Whether param.json may ask for a download-data area: a "downloadDataSize" key anywhere in the text, a top-level
// one however it is spelled, or text that is not a JSON object.
bool TitleRequestsDownloadData(const std::string& param_json);

}  // namespace playden::runtime
