// SPDX-License-Identifier: GPL-3.0-or-later
#include "library/play_history.h"

#include "library/store.h"

#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace playden {
namespace {

constexpr std::size_t kMaximumBytes = 1024 * 1024;
// The longest line (an imported game's key, two 20-digit numbers) is under 90 bytes: the most games fit the read.
constexpr std::size_t kMaximumGames = 10000;
constexpr std::string_view kHeader = "version=1";

// Plain decimal, no sign or leading zero (but "0"); the console's libc has no stream parsing.
std::optional<std::uint64_t> Decimal(std::string_view text) {
  if (text.empty() || text.size() > 20 || (text.size() > 1 && text[0] == '0')) return std::nullopt;
  std::uint64_t value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') return std::nullopt;
    const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
    if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) return std::nullopt;
    value = value * 10 + digit;
  }
  return value;
}

}  // namespace

bool operator==(const PlayRecord& left, const PlayRecord& right) {
  return left.last_played == right.last_played && left.played_seconds == right.played_seconds;
}

bool RecordPlay(PlayHistory& history, GameId id, std::int64_t when, std::uint64_t seconds) {
  if (!id.valid() || when <= 0) return false;
  const auto found = history.find(id);
  if (found == history.end() && history.size() >= kMaximumGames) return false;
  auto& record = history[std::move(id)];
  const PlayRecord before = record;
  if (when > record.last_played) record.last_played = when;
  if (seconds > std::numeric_limits<std::uint64_t>::max() - record.played_seconds)
    record.played_seconds = std::numeric_limits<std::uint64_t>::max();
  else
    record.played_seconds += seconds;
  return !(record == before);
}

Result<void> PlayHistoryStore::Save(const PlayHistory& history) const {
  std::string text = std::string(kHeader) + "\n";
  for (const auto& [id, record] : history) {
    if (!id.valid() || record.last_played <= 0) continue;
    text += ToKey(id) + " " + std::to_string(record.last_played) + " " + std::to_string(record.played_seconds) + "\n";
  }
  // Through the managed tree: no link anywhere in the path, and open to FTP like the rest of the user folder (#868).
  return Storage(storage_).OpenTree().WriteDurable(kFile, Bytes(text.begin(), text.end()));
}

Result<PlayHistory> PlayHistoryStore::Load() const {
  const auto read = Storage(storage_).OpenTree().ReadDurable(kFile, kMaximumBytes);
  if (!read.ok()) {
    if (read.error() == StorageError::kNotFound) return Result<PlayHistory>::Success({});
    return Result<PlayHistory>::Failure(read.error(), read.message());
  }
  const std::string text(read.value().begin(), read.value().end());
  // Empty: a save a power loss stopped before it reached the disk. Nothing to keep; the next save writes it again.
  if (text.find_first_not_of('\0') == std::string::npos) return Result<PlayHistory>::Success({});
  const auto first = text.find('\n');
  if (std::string_view(text).substr(0, first) != kHeader)
    return Result<PlayHistory>::Failure(StorageError::kInvalidData, "Play history file has another version.");
  PlayHistory history;
  std::size_t start = first == std::string::npos ? text.size() : first + 1;
  while (start < text.size()) {
    auto end = text.find('\n', start);
    // A last line without its newline was cut short: it is skipped like any line that doesn't read.
    if (end == std::string::npos) break;
    const std::string_view line(text.data() + start, end - start);
    start = end + 1;
    const auto first_space = line.find(' ');
    const auto second_space = first_space == std::string_view::npos ? first_space : line.find(' ', first_space + 1);
    if (second_space == std::string_view::npos) continue;
    const auto id = ParseKey(line.substr(0, first_space));
    const auto when = Decimal(line.substr(first_space + 1, second_space - first_space - 1));
    const auto seconds = Decimal(line.substr(second_space + 1));
    if (!id || !when || !seconds || *when == 0 ||
        *when > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
      continue;
    if (history.size() >= kMaximumGames) break;
    history[*id] = {static_cast<std::int64_t>(*when), *seconds};
  }
  return Result<PlayHistory>::Success(std::move(history));
}

}  // namespace playden
