// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "steam/backend/cm_client.h"
#include "steam/backend/http_client.h"
#include "steam/session_store.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace playden::steam {

// Diagnostics only: what IFamilyGroupsService says about the account's Steam family, to compare with the
// licenses the CM sent. Nothing here adds games or affects a library load.

using FamilyPerform = std::function<HttpResponse(const HttpRequest&, const std::atomic<bool>&)>;
using FamilyLog = std::function<void(const std::string& line)>;

struct FamilyMember {
  std::uint64_t steamid = 0;
  std::optional<std::uint64_t> role;
};

// GetFamilyGroupForUser. family_groupid 0 means the account is in no family.
struct FamilyGroup {
  std::uint64_t family_groupid = 0;
  bool not_member = false;
  std::optional<std::uint64_t> role;
  std::vector<FamilyMember> members;
};

struct SharedLibraryApp {
  std::uint32_t appid = 0;
  std::vector<std::uint64_t> owners;
  std::uint64_t exclude_reason = 0;  // 0: shared; anything else is Steam's reason it is not
};

// GetSharedLibraryApps.
struct SharedLibrary {
  std::vector<SharedLibraryApp> apps;
};

// Lenient: unknown fields are ignored, IDs may be JSON numbers or strings, missing fields stay at their
// defaults. Empty when the body is not a JSON object with a "response" object.
std::optional<FamilyGroup> ParseFamilyGroup(std::string_view body);
std::optional<SharedLibrary> ParseSharedLibraryApps(std::string_view body);

// Asks Steam about the account's family and its shared library and writes "family webapi ..." lines.
// `acquisitions` (when ok) tells which shared apps have a license here. Every failure becomes a log line.
// The URL, the token and response bodies are never logged.
void RunFamilyDiagnostics(const StoredSession& session, const AcquisitionResult* acquisitions,
                          const FamilyPerform& perform, const FamilyLog& log, const std::atomic<bool>& cancel);

// What the probe found, kept so the comparison with licenses can run after both finish.
struct FamilyProbe {
  enum class Outcome { kCanceled, kFailed, kNoGroup, kGroup };
  Outcome outcome = Outcome::kFailed;
  FamilyGroup group;
  std::optional<SharedLibrary> library;
};

// The two requests of RunFamilyDiagnostics, logging the group and any failure. Split so a caller can run
// the requests while the license list loads and compare afterwards with LogFamilySharedApps.
FamilyProbe ProbeFamily(const StoredSession& session, const FamilyPerform& perform, const FamilyLog& log,
                        const std::atomic<bool>& cancel);
void LogFamilySharedApps(const FamilyProbe& probe, const AcquisitionResult* acquisitions, std::uint64_t elapsed_ms,
                         const FamilyLog& log);

}  // namespace playden::steam
