#pragma once
#include "runtime/game_install.h"
#include <optional>
#include <string>

namespace playden::ps5 {
struct InstalledTitleIdentity {
  std::string title_id;
  std::string content_id;
  std::string metadata_path;
};
struct TitleDatabaseObservation {
  runtime::TitleAvailability inventory = runtime::TitleAvailability::kUnknown;
  std::optional<InstalledTitleIdentity> identity;
  // Why an occupied row gave no identity, for logs.
  std::string reason;
};
// Worker-only read-only observation. A failed/open-incomplete query never means
// a free title. Any matching row occupies an ID, even if its metadata is invalid.
// Concurrent calls refuse as unknown; the private SQLite reader is serialized.
TitleDatabaseObservation ReadTitleDatabase(const std::string& path, const std::string& title_id);
TitleDatabaseObservation ReadNativeTitleDatabase(const std::string& title_id);
}  // namespace playden::ps5
