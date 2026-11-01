#pragma once
#include "library/repository.h"
#include <string>
#include <utility>
#include <vector>

namespace playden {
class LibraryCache final : public LibraryRepository {
 public:
  explicit LibraryCache(Storage& storage) : storage_(storage) {}
  Result<LibrarySnapshot> Load(std::uint64_t account_id) const override;
  Result<void> Save(std::uint64_t account_id, const LibrarySnapshot& snapshot) const override;
  Result<void> Clear(std::uint64_t account_id) const override;
  Result<LibrarySnapshot> Load(const std::string& account_id) const;
  Result<void> Save(const std::string& account_id, const LibrarySnapshot& snapshot) const;
  Result<void> Clear(const std::string& account_id) const;
  Result<void> MergeGameTags(const std::string& account_id, const GameId& id,
                             std::vector<std::string> tags) const;
  Result<void> MergeGameTags(std::uint64_t account_id, const GameId& id,
                             std::vector<std::string> tags) const;
  // One read and one write for several games. A game that is missing or whose tags are not valid is skipped.
  Result<void> MergeGameTags(std::uint64_t account_id,
                             const std::vector<std::pair<GameId, std::vector<std::string>>>& updates) const;

 private:
  Storage& storage_;
};
}  // namespace playden
