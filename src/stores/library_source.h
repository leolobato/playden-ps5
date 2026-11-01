#pragma once
#include "library/service.h"
#include "library/store.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace playden::stores {

// One store's library, loaded off the presentation thread. Each request answers with LibraryEvents
// (loading, cached, complete, failure, cleared) carrying its request, account and generation; work
// for an older generation is dropped. A store without accounts takes account 0.
class LibrarySource {
 public:
  virtual ~LibrarySource() = default;
  virtual Store store() const = 0;
  // Zero means refused (stopped, busy or stale).
  virtual std::uint64_t Refresh(std::uint64_t account, std::uint64_t generation) = 0;
  virtual std::uint64_t LoadCached(std::uint64_t account, std::uint64_t generation) = 0;
  virtual std::uint64_t Clear(std::uint64_t account, std::uint64_t generation) = 0;
  // Steam reads licenses for the signed-in library. Zero when this store has none to read.
  virtual std::uint64_t EnsureLicenses(std::uint64_t /*account*/, std::uint64_t /*generation*/) { return 0; }
  virtual void Invalidate(std::uint64_t generation) = 0;
  virtual void Shutdown() = 0;
  virtual std::vector<LibraryEvent> Pump(std::size_t max_events) = 0;
};

}  // namespace playden::stores
