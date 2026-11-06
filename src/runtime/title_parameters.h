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

}  // namespace playden::runtime
