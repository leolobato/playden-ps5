// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "library/model.h"
#include "platform/storage.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace playden {

// A store's collections for one account (#536), saved apart from its library so that a store that can't read them
// keeps the library cache as it was. Sign-out removes them with the library.
class CollectionsRepository {
 public:
  virtual ~CollectionsRepository() = default;
  virtual Result<std::vector<LibraryCollection>> Load(std::uint64_t account_id) const = 0;
  virtual Result<void> Save(std::uint64_t account_id, const std::vector<LibraryCollection>& collections) const = 0;
  virtual Result<void> Clear(std::uint64_t account_id) const = 0;
};

// Files are <file_prefix><decimal account>.cache: Steam's "collections-", GOG's "collections-gog-". The file is
// checked on read (magic, account, sizes, IDs, checksum); a damaged one reads as invalid data.
class CollectionsCache final : public CollectionsRepository {
 public:
  explicit CollectionsCache(Storage& storage, std::string file_prefix = "collections-")
      : storage_(storage), file_prefix_(std::move(file_prefix)) {}
  Result<std::vector<LibraryCollection>> Load(std::uint64_t account_id) const override;
  Result<void> Save(std::uint64_t account_id, const std::vector<LibraryCollection>& collections) const override;
  Result<void> Clear(std::uint64_t account_id) const override;

  static constexpr std::size_t kMaximumCollections = 10000;
  static constexpr std::size_t kMaximumMembers = 200000;  // all collections together
  static constexpr std::size_t kMaximumNameBytes = 1024;

 private:
  std::string Path(std::uint64_t account) const { return file_prefix_ + std::to_string(account) + ".cache"; }
  Storage& storage_;
  std::string file_prefix_;
};

}  // namespace playden
