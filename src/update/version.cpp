#include "update/version.h"

#ifndef PLAYDEN_VERSION
#error "PLAYDEN_VERSION must be defined by the build"
#endif

namespace playden::update {

std::string Version::ToString() const {
  return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch) +
         (beta ? "-beta." + std::to_string(beta) : "");
}

std::optional<Version> ParseVersion(std::string_view text) {
  if (!text.empty() && (text.front() == 'v' || text.front() == 'V')) text.remove_prefix(1);
  unsigned parts[3] = {};
  for (int i = 0; i < 3; ++i) {
    std::size_t digits = 0;
    unsigned value = 0;
    while (digits < text.size() && text[digits] >= '0' && text[digits] <= '9') {
      value = value * 10 + static_cast<unsigned>(text[digits] - '0');
      if (++digits > 6) return std::nullopt;
    }
    if (!digits) return std::nullopt;
    parts[i] = value;
    text.remove_prefix(digits);
    if (i < 2) {
      if (text.empty() || text.front() != '.') return std::nullopt;
      text.remove_prefix(1);
    }
  }
  unsigned beta = 0;
  if (!text.empty()) {
    if (text.substr(0, 6) != "-beta.") return std::nullopt;
    text.remove_prefix(6);
    if (text.empty() || text.size() > 6 || text.front() == '0') return std::nullopt;
    for (const char c : text) {
      if (c < '0' || c > '9') return std::nullopt;
      beta = beta * 10 + static_cast<unsigned>(c - '0');
    }
  }
  return Version{parts[0], parts[1], parts[2], beta};
}

Version CurrentVersion() {
  static const Version version = ParseVersion(PLAYDEN_VERSION).value_or(Version{});
  return version;
}

}  // namespace playden::update
