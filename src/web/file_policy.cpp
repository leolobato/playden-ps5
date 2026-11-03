// SPDX-License-Identifier: GPL-3.0-or-later
#include "web/file_policy.h"

namespace playden::web {
namespace {

// Files holding a sign-in that can sit in a game folder. Add each new credential file a game folder gets here.
constexpr const char* kSecretNames[] = {
    "playden_online.txt",     // the account's refresh token, for the online bridge
    "socialclub_scauth.txt",  // the Rockstar sign-in
};

}  // namespace

std::string LowerAscii(std::string_view text) {
  std::string out(text);
  for (auto& c : out)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  return out;
}

bool ValidUtf8(std::string_view text) {
  std::size_t i = 0;
  while (i < text.size()) {
    const auto first = static_cast<unsigned char>(text[i]);
    std::size_t more = 0;
    unsigned value = 0;
    if (first < 0x80) { ++i; continue; }
    if (first >= 0xc2 && first <= 0xdf) { more = 1; value = first & 0x1f; }
    else if (first >= 0xe0 && first <= 0xef) { more = 2; value = first & 0x0f; }
    else if (first >= 0xf0 && first <= 0xf4) { more = 3; value = first & 0x07; }
    else return false;
    if (text.size() - i <= more) return false;
    for (std::size_t j = 1; j <= more; ++j) {
      const auto next = static_cast<unsigned char>(text[i + j]);
      if ((next & 0xc0) != 0x80) return false;
      value = (value << 6) | (next & 0x3f);
    }
    if ((more == 2 && (value < 0x800 || (value >= 0xd800 && value <= 0xdfff))) ||
        (more == 3 && (value < 0x10000 || value > 0x10ffff)))
      return false;
    i += more + 1;
  }
  return true;
}

bool HiddenName(std::string_view name) {
  const auto lower = LowerAscii(name);
  if (lower.rfind(".playden", 0) == 0) return true;
  for (const char* secret : kSecretNames)
    if (lower == secret) return true;
  return false;
}

bool ReadOnlyFolderName(std::string_view name) { return LowerAscii(name) == "steam_settings"; }

bool UsableName(std::string_view name) {
  if (name.empty() || name == "." || name == ".." || name.size() > kMaxNameBytes) return false;
  for (const unsigned char c : name)
    if (c < 0x20 || c == 0x7f || c == '/' || c == '\\') return false;
  return ValidUtf8(name);
}

bool NewName(std::string_view name) { return UsableName(name) && name.size() <= kMaxNewNameBytes && !HiddenName(name); }

std::optional<std::vector<std::string>> SplitRelativePath(std::string_view path) {
  std::vector<std::string> parts;
  if (path.empty()) return parts;
  std::size_t start = 0;
  while (true) {
    const auto slash = path.find('/', start);
    const auto part = path.substr(start, slash == std::string_view::npos ? slash : slash - start);
    if (!UsableName(part) || HiddenName(part)) return std::nullopt;
    parts.emplace_back(part);
    if (slash == std::string_view::npos) return parts;
    start = slash + 1;
  }
}

bool ReadOnlyPath(const std::vector<std::string>& parts) {
  for (const auto& part : parts)
    if (ReadOnlyFolderName(part)) return true;
  return false;
}

bool PrivatePath(const std::vector<std::string>& parts, const std::vector<std::string>& private_paths) {
  for (const auto& path : private_paths) {
    const auto split = SplitRelativePath(path);
    if (!split || split->empty() || split->size() > parts.size()) continue;
    bool within = true;
    for (std::size_t i = 0; i < split->size(); ++i) within = within && LowerAscii((*split)[i]) == LowerAscii(parts[i]);
    if (within) return true;
  }
  return false;
}

}  // namespace playden::web
