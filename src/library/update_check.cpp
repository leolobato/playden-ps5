// SPDX-License-Identifier: GPL-3.0-or-later
#include "library/update_check.h"

#include "library/store.h"

#include <algorithm>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace playden {
namespace {

constexpr std::size_t kMaximumBytes = 1024 * 1024;
constexpr std::size_t kMaximumGames = 20000;

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

std::string UpdateCheckStore::Path(std::uint64_t account, int version) const {
  return prefix_ + std::to_string(account) + ".v" + std::to_string(version);
}

bool operator==(const SavedGameUpdate& left, const SavedGameUpdate& right) {
  return left.newer == right.newer && left.new_dlc == right.new_dlc && left.download_bytes == right.download_bytes &&
         left.staged_bytes == right.staged_bytes && left.target == right.target && left.sized == right.sized;
}
bool operator==(const SavedUpdateCheck& left, const SavedUpdateCheck& right) {
  return left.checked_at == right.checked_at && left.games == right.games;
}

Result<void> UpdateCheckStore::Save(std::uint64_t account, const SavedUpdateCheck& check) const {
  if (!account) return Result<void>::Failure(StorageError::kInvalidPath, "Invalid update check account.");
  if (check.checked_at < 0 || check.games.size() > kMaximumGames)
    return Result<void>::Failure(StorageError::kInvalidData, "Update check cannot be saved.");
  std::string text = "version=2\nchecked_at=" + std::to_string(check.checked_at) + "\n";
  for (const auto& [id, game] : check.games) {
    if (!id.valid() || (!game.newer && !game.new_dlc)) continue;  // only games with something to download
    const int flags = (game.newer ? 1 : 0) | (game.new_dlc ? 2 : 0) | (game.sized ? 4 : 0);
    text += ToKey(id) + " " + std::to_string(flags) + " " + std::to_string(game.sized ? game.download_bytes : 0) + " " +
            std::to_string(game.sized ? game.staged_bytes : 0) + " " + std::to_string(game.target) + "\n";
  }
  const auto ready = storage_.EnsureRootDirectory();
  if (!ready.ok()) return ready;
  return storage_.WriteAtomicOpen(Path(account), Bytes(text.begin(), text.end()));
}

Result<std::optional<SavedUpdateCheck>> UpdateCheckStore::Load(std::uint64_t account) const {
  using Loaded = Result<std::optional<SavedUpdateCheck>>;
  if (!account) return Loaded::Failure(StorageError::kInvalidPath, "Invalid update check account.");
  int version = 2;
  auto read = storage_.Read(Path(account), kMaximumBytes);
  if (!read.ok() && read.error() == StorageError::kNotFound) {
    version = 1;
    read = storage_.Read(Path(account, 1), kMaximumBytes);
  }
  if (!read.ok()) {
    if (read.error() == StorageError::kNotFound) return Loaded::Success(std::nullopt);
    return Loaded::Failure(read.error(), read.message());
  }
  const auto invalid = [] { return Loaded::Failure(StorageError::kInvalidData, "Update check file is damaged."); };
  const std::string text(read.value().begin(), read.value().end());
  if (text.empty() || text.back() != '\n') return invalid();
  SavedUpdateCheck check;
  std::size_t start = 0;
  std::size_t line_number = 0;
  while (start < text.size()) {
    const auto end = text.find('\n', start);
    const std::string_view line(text.data() + start, end - start);
    start = end + 1;
    if (line_number++ == 0) {
      if (line != (version == 1 ? "version=1" : "version=2")) return invalid();
      continue;
    }
    if (line_number == 2) {
      constexpr std::string_view kPrefix = "checked_at=";
      if (line.substr(0, kPrefix.size()) != kPrefix) return invalid();
      const auto when = Decimal(line.substr(kPrefix.size()));
      if (!when || *when > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) return invalid();
      check.checked_at = static_cast<std::int64_t>(*when);
      continue;
    }
    std::vector<std::string_view> fields;
    for (std::size_t from = 0;;) {
      const auto space = line.find(' ', from);
      fields.push_back(line.substr(from, space == std::string_view::npos ? std::string_view::npos : space - from));
      if (space == std::string_view::npos) break;
      from = space + 1;
    }
    if (fields.size() != (version == 1 ? 3u : 5u)) return invalid();
    const auto id = ParseKey(fields[0]);
    const auto flags = Decimal(fields[1]);
    std::optional<std::uint64_t> numbers[3];
    for (std::size_t index = 2; index < fields.size(); ++index) numbers[index - 2] = Decimal(fields[index]);
    const std::uint64_t most = version == 1 ? 3 : 7;
    if (!id || !flags || (*flags & 3) == 0 || *flags > most || check.games.size() >= kMaximumGames) return invalid();
    if (std::any_of(fields.begin() + 2, fields.end(), [](std::string_view field) { return !Decimal(field); })) return invalid();
    // Version 1 held Steam's whole-depot estimate: only the flags are kept.
    SavedGameUpdate game{(*flags & 1) != 0, (*flags & 2) != 0};
    if (version == 2) {
      game.sized = (*flags & 4) != 0;
      game.download_bytes = game.sized ? *numbers[0] : 0;
      game.staged_bytes = game.sized ? *numbers[1] : 0;
      game.target = *numbers[2];
    }
    if (!check.games.emplace(*id, game).second) return invalid();
  }
  if (line_number < 2) return invalid();
  return Loaded::Success(std::move(check));
}

}  // namespace playden
