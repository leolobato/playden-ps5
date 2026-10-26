#pragma once

#include "steam/app_details_service.h"
#include "steam/backend/cm_client.h"
#include "steam/backend/content_language.h"

#include <functional>

namespace playden::steam {

// App details from PICS: totals of the depots Playden would install (Windows, the chosen content
// language, public branch) and whether a Windows launch entry exists. Sizes are Steam's per-depot
// figures, so they are an estimate until the manifests are fetched.
class CmAppDetailsSource final : public AppDetailsSource {
 public:
  explicit CmAppDetailsSource(CmClient& client) : client_(client) {}
  // The licenses the library last read: sizes then count only the depots they grant.
  void SetLicensedDepots(const LicensedDepots* licensed) { licensed_ = licensed; }
  // The content language sizes are counted for; it has to be the one an install would use or
  // the free-space check is made against the wrong depots. Read on the details worker when a
  // fetch starts, so a change in Settings reaches the next one.
  void SetContentLanguage(std::function<std::string()> language) { language_ = std::move(language); }
  std::vector<AppDetails> Fetch(const StoredSession& session, const std::vector<GameId>& ids,
                                const std::atomic<bool>& cancel) override;

 private:
  CmClient& client_;
  const LicensedDepots* licensed_ = nullptr;
  std::function<std::string()> language_;
};

// Pure part, for tests: turns one parsed app buffer into details. With `licensed`, sizes count
// only the depots those licenses grant, as an install selects them.
AppDetails DetailsFromAppInfo(std::uint32_t id, const std::string& buffer,
                              const std::optional<LicensedDepots::Grant>& licensed = std::nullopt,
                              std::string_view language = kDefaultContentLanguage);

}  // namespace playden::steam
