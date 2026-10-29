#include "update/version.h"

#ifndef PLAYDEN_VERSION
#error "PLAYDEN_VERSION must be defined by the build"
#endif
#ifndef PLAYDEN_BUILD_COMMIT
#define PLAYDEN_BUILD_COMMIT ""
#endif

namespace playden::update {

std::string Version::ToString() const {
  return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch) +
         (beta ? "-beta." + std::to_string(beta) : "") +
         (nightly() ? "-nightly." + std::to_string(nightly_date) + "." + std::to_string(nightly_build) : "");
}

namespace {

// Digits only, no leading zero, at most `max_digits` long.
bool ParseCount(std::string_view text, std::size_t max_digits, unsigned* value) {
  if (text.empty() || text.size() > max_digits || text.front() == '0') return false;
  *value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') return false;
    *value = *value * 10 + static_cast<unsigned>(c - '0');
  }
  return true;
}

}  // namespace

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
  Version version{parts[0], parts[1], parts[2]};
  if (text.empty()) return version;
  if (text.substr(0, 6) == "-beta.") {
    if (!ParseCount(text.substr(6), 6, &version.beta)) return std::nullopt;
    return version;
  }
  // -nightly.YYYYMMDD.N, with a plausible date.
  if (text.substr(0, 9) != "-nightly.") return std::nullopt;
  text.remove_prefix(9);
  const auto dot = text.find('.');
  if (dot != 8 || !ParseCount(text.substr(0, 8), 8, &version.nightly_date) ||
      !ParseCount(text.substr(9), 3, &version.nightly_build))
    return std::nullopt;
  const unsigned month = version.nightly_date / 100 % 100, day = version.nightly_date % 100;
  if (version.nightly_date < 20000101 || month < 1 || month > 12 || day < 1 || day > 31) return std::nullopt;
  return version;
}

Version CurrentVersion() {
  static const Version version = ParseVersion(PLAYDEN_VERSION).value_or(Version{});
  return version;
}

std::string BuildCommit() { return PLAYDEN_BUILD_COMMIT; }

}  // namespace playden::update
