#pragma once

#include "library/model.h"
#include "platform/storage.h"

#include <cstdint>
#include <vector>

namespace playden {

struct LibrarySnapshot {
  // 5 names games by store and value (other stores than Steam); 6 marks demos.
  std::uint32_t schema_version = 4;
  std::uint64_t retrieved_at_unix_seconds = 0;
  std::vector<GameSummary> games;
  bool metadata_partial = false;
};

class LibraryRepository {
 public:
  virtual ~LibraryRepository() = default;
  virtual Result<LibrarySnapshot> Load(std::uint64_t account_id) const = 0;
  virtual Result<void> Save(std::uint64_t account_id,
                            const LibrarySnapshot& snapshot) const = 0;
  virtual Result<void> Clear(std::uint64_t account_id) const = 0;
};

}  // namespace playden
