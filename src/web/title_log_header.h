// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace playden::web {

// The first line of a title log Playden copies to the logs folder: which game the log belongs to, so a log
// outlives its title (an uninstalled game, or a title ID a later game reuses) under the right name.
//   PLAYDEN_TITLE_LOG/1 install=steam-271590 name=Grand Theft Auto V Legacy
// The name comes last and runs to the end of the line. tools/collect_logs.py reads the same line.
inline constexpr std::string_view kTitleLogHeader = "PLAYDEN_TITLE_LOG/1 ";
inline constexpr std::size_t kTitleLogNameBytes = 512;
inline constexpr std::size_t kTitleLogInstallBytes = 128;

struct TitleLogGame {
  std::string install;  // steam-271590, gog-<id>, local-<uuid>
  std::string name;     // empty: unknown
};

namespace title_log_detail {
inline bool InstallChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
}
inline bool ValidInstall(std::string_view install) {
  if (install.empty() || install.size() > kTitleLogInstallBytes) return false;
  for (const char c : install) if (!InstallChar(c)) return false;
  return true;
}
}  // namespace title_log_detail

// The header line, newline included; empty for an install ID it can't carry. Control characters in the name
// become spaces, and a name over the limit is cut at a character boundary, so it stays one line.
inline std::string TitleLogHeader(std::string_view install, std::string_view name) {
  if (!title_log_detail::ValidInstall(install)) return {};
  std::string clean;
  for (const char c : name) {
    const auto u = static_cast<unsigned char>(c);
    clean += u < 0x20 || u == 0x7f ? ' ' : c;
  }
  if (clean.size() > kTitleLogNameBytes) {
    std::size_t cut = kTitleLogNameBytes;
    while (cut > 0 && (static_cast<unsigned char>(clean[cut]) & 0xC0) == 0x80) --cut;
    clean.resize(cut);
  }
  while (!clean.empty() && clean.back() == ' ') clean.pop_back();
  return std::string(kTitleLogHeader) + "install=" + std::string(install) + " name=" + clean + "\n";
}

// The game a title log's first line names; nothing for a log without the header (one an older Playden copied)
// or a malformed one.
inline std::optional<TitleLogGame> ParseTitleLogHeader(std::string_view text) {
  if (text.substr(0, kTitleLogHeader.size()) != kTitleLogHeader) return std::nullopt;
  const auto end = text.find('\n');
  if (end == std::string_view::npos) return std::nullopt;
  auto line = text.substr(kTitleLogHeader.size(), end - kTitleLogHeader.size());
  if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
  constexpr std::string_view kInstall = "install=", kName = " name=";
  if (line.substr(0, kInstall.size()) != kInstall) return std::nullopt;
  const auto name_at = line.find(kName);
  if (name_at == std::string_view::npos) return std::nullopt;
  TitleLogGame game{std::string(line.substr(kInstall.size(), name_at - kInstall.size())),
                    std::string(line.substr(name_at + kName.size()))};
  if (!title_log_detail::ValidInstall(game.install) || game.name.size() > kTitleLogNameBytes) return std::nullopt;
  for (const unsigned char c : game.name) if (c < 0x20 || c == 0x7f) return std::nullopt;
  return game;
}

// The header's line length (0 without one): what to skip before reading the log the kit wrote.
inline std::size_t TitleLogHeaderLength(std::string_view text) {
  if (!ParseTitleLogHeader(text)) return 0;
  return text.find('\n') + 1;
}

}  // namespace playden::web
