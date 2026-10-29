#pragma once

#include "platform/storage.h"
#include "steam/backend/cm_messages.h"
#include "steam/backend/http_client.h"
#include "steam/backend/ws_client.h"
#include "steam/auth_backend.h"
#include "steam/session_store.h"

#include <array>
#include <atomic>
#include <chrono>
#include <optional>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <memory>
#include <string>
#include <string_view>

namespace playden::steam {

enum class CmError {
  kNone,
  kCanceled,
  kNoSession,
  kNetwork,
  kRejected,  // logon refused; never treated as revoked credentials (the reference ignores it)
  kProtocol,
  kTimeout,
};

const char* CmErrorName(CmError error);

// Receives one line per CM logon and per refusal, for steam-cm.log: the owner, the step, the error
// and Steam's result code. Never a token, account name, the account's SteamID or an endpoint. Family
// sharing lines name the owning family member's account ID, so a shared game can be traced. Process-wide.
void SetCmLog(std::function<void(const std::string& line)> log);

// After Steam refuses a logon for a reason other than the credentials (busy, rate limited), or the
// CM directory answers with an HTTP error, every CmClient waits 1, then 5, then 20 minutes before
// trying again; Open() fails at once with kNetwork meanwhile. A successful logon ends it.
// Process-wide, since every CmClient logs on with the same account and device.
bool CmBackoffActive();
void ResetCmBackoff();

struct AcquisitionResult {
  CmError error = CmError::kProtocol;
  // App ID -> earliest acquisition (unix seconds) among this account's active licenses.
  std::map<std::uint32_t, std::uint32_t> acquired_at;
  // Apps and depots granted by those licenses' packages, for depot selection.
  std::set<std::uint32_t> owned_apps;
  std::set<std::uint32_t> owned_depots;
  // The apps of each active licensed package, for games the owned list leaves out.
  std::map<std::uint32_t, std::vector<std::uint32_t>> package_apps;
  std::map<std::uint32_t, std::vector<std::uint32_t>> package_depots;
  // Apps reached only through licenses borrowed from another Steam family member, not through one of the
  // account's own. They are also in owned_apps and package_apps.
  std::set<std::uint32_t> shared_apps;
  // Licensed packages Steam left out, sent without a token, or sent unreadable; their apps are not owned here.
  std::uint32_t skipped_packages = 0;
  bool ok() const { return error == CmError::kNone; }
};

// What the account's licenses granted at the last library load, shared with size estimates so a
// game's page counts only the depots an install would download. Safe from any thread.
class LicensedDepots {
 public:
  struct Grant {
    std::set<std::uint32_t> apps;
    std::set<std::uint32_t> depots;
  };
  void Set(const AcquisitionResult& result) {
    std::lock_guard<std::mutex> lock(mutex_);
    grant_ = Grant{result.owned_apps, result.owned_depots};
  }
  std::optional<Grant> Get() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return grant_;
  }

 private:
  mutable std::mutex mutex_;
  std::optional<Grant> grant_;
};

struct FreeLicenseResult {
  CmError error = CmError::kProtocol;
  std::int32_t eresult = cm::kEResultFail;  // Steam's answer, when it answered
  std::vector<std::uint32_t> granted_apps;
  std::vector<std::uint32_t> granted_packages;
  // After a grant: whether Steam pushed a newer license list within the wait, and how long it took.
  bool licenses_updated = false;
  std::uint32_t waited_ms = 0;
  bool ok() const { return error == CmError::kNone; }
};

struct TokenRenewal {
  CmError error = CmError::kProtocol;
  std::string access_token;
  std::optional<std::string> refresh_token;  // present only when Steam rotated it
  bool ok() const { return error == CmError::kNone; }
};

inline constexpr std::size_t kMaxAppInfoBatch = 50;

struct AppInfoBuffers {
  CmError error = CmError::kProtocol;
  std::map<std::uint32_t, std::string> buffers;  // app ID -> VDF text (trailing NULs removed)
  bool ok() const { return error == CmError::kNone; }
};

struct DepotRequest {
  std::uint32_t depot_id = 0;
  std::uint64_t manifest_gid = 0;
  // A depot the game borrows from another app (depotfromapp): an account that doesn't own that app is
  // refused its key, and the game installs without it.
  bool optional = false;
  // The app whose appinfo lists the depot (a DLC app for its own depots), asked for its key and
  // manifest request code; 0 means the call's app.
  std::uint32_t app_id = 0;
  // The branch the manifest belongs to, sent with the manifest request code; empty: public.
  std::string branch;
};
struct DepotGrant {
  std::uint32_t depot_id = 0;
  std::uint64_t manifest_gid = 0;
  std::array<std::uint8_t, 32> key{};
  std::uint64_t manifest_request_code = 0;  // short-lived; fetch the manifest soon after
};
struct DepotAccessResult {
  CmError error = CmError::kProtocol;
  std::vector<DepotGrant> grants;  // one per requested depot, in request order, except skipped ones
  std::vector<std::uint32_t> skipped;  // optional depots whose key the account was refused
  std::string detail;              // which step refused, with Steam's result code
  bool ok() const { return error == CmError::kNone; }
};

// The CM-only library facts. Implementations run synchronously on the caller's worker thread.
class AcquisitionSource {
 public:
  virtual ~AcquisitionSource() = default;
  virtual AcquisitionResult FetchAcquisitionDates(const StoredSession& session,
                                                  const std::atomic<bool>& cancel) = 0;
  // PICS app info, to name licensed games the owned list leaves out. None by default.
  virtual AppInfoBuffers FetchAppInfo(const StoredSession&, const std::vector<std::uint32_t>&,
                                      const std::atomic<bool>&) {
    return {};
  }
};

// Derives the stable logon device fields from a per-install seed, as the reference
// CMDeviceIdentity: SHA-256 login ID and SHA-1 hashed hardware keys. No hardware ID is read.
cm::DeviceIdentity MakeDeviceIdentity(std::string_view seed);
// Loads the per-install seed from storage, creating it on first use. A failed save still yields a
// seed that is stable for this run.
std::string LoadOrCreateDeviceSeed(const Storage& storage);

struct CmClientHooks {
  std::function<HttpResponse(const HttpRequest&, const std::atomic<bool>&)> directory;
  std::function<std::unique_ptr<MessageTransport>()> transport;
  std::function<std::uint64_t()> unix_now;
  std::uint32_t deadline_ms = 25'000;
};

// One logged-on CM connection that several CmClients share, as Steam's own client keeps one for the
// whole run. Calls take turns on it. Between calls a background thread answers the server and sends
// heartbeats, so the session stays logged on while Playden runs; it is opened again only when the
// credentials change or Steam drops it. Destroying the session logs off.
class CmSharedSession;
std::shared_ptr<CmSharedSession> MakeCmSharedSession();
// How many times Steam pushed a license list on the shared session whose packages differ from the
// account's previous one: a game bought, added to the library or removed while Playden runs. It
// starts at 0, and a new account's first list never counts. Safe from any thread.
std::uint64_t CmLicenseChanges(const CmSharedSession& shared);

// Without a shared session, a CM session per call: directory, connect (first four endpoints), hello,
// logon with the stored refresh token, the pushed license list, then package PICS in batches of 50.
// All output is error categories and app dates; no endpoint, address, token or account identity is
// reported.
class CmClient final : public AcquisitionSource {
 public:
  CmClient(std::string ca_bundle, const Storage& device_storage);
  CmClient(CmClientHooks hooks, cm::DeviceIdentity device);
  ~CmClient() override;

  // Names this client in steam-cm.log (cloud, details, library, install, auth).
  void SetOwner(std::string owner) { owner_ = std::move(owner); }
  // Runs every call on `shared` instead of a session of its own. Set before the first call.
  void SetSharedSession(std::shared_ptr<CmSharedSession> shared) { shared_ = std::move(shared); }

  AcquisitionResult FetchAcquisitionDates(const StoredSession& session,
                                          const std::atomic<bool>& cancel) override;

  // Renews the access token through the authenticated CM session
  // (Authentication.GenerateAccessTokenForApp#1), which accepts SteamClient refresh tokens that
  // the Web API refuses. A refused CM logon reports kRejected.
  TokenRenewal RenewAccessToken(const StoredSession& session, const std::atomic<bool>& cancel);

  // PICS app info buffers (text VDF) for up to kMaxAppInfoBatch apps, using app access tokens
  // where Steam grants them. Apps Steam does not describe are absent from the result.
  // Depot decryption keys and public-branch manifest request codes for one app's depots.
  // Any refused depot fails the whole call (kRejected): a partial install is never planned.
  DepotAccessResult FetchDepotAccess(const StoredSession& session, std::uint32_t app_id,
                                     const std::vector<DepotRequest>& depots,
                                     const std::atomic<bool>& cancel);

  AppInfoBuffers FetchAppInfo(const StoredSession& session, const std::vector<std::uint32_t>& app_ids,
                              const std::atomic<bool>& cancel) override;

  // Asks Steam to add free apps (free DLC) to the account in one ClientRequestFreeLicense. When it
  // grants anything, waits up to `license_wait_ms` (cancelable) for the license list Steam pushes
  // after the grant, so the next FetchAcquisitionDates on a shared session sees it; a wait that runs
  // out is not an error. Steam answering with an EResult other than OK is kRejected.
  FreeLicenseResult RequestFreeLicenses(const StoredSession& session, const std::vector<std::uint32_t>& app_ids,
                                        std::uint32_t license_wait_ms, const std::atomic<bool>& cancel);

  // A unified service call on an open session. When Steam refuses it, the result is kRejected and
  // `eresult` (optional) holds Steam's EResult.
  using ServiceCall = std::function<CmError(const std::string& method, const std::vector<std::uint8_t>& body,
                                            std::vector<std::uint8_t>* reply, int* eresult)>;
  // Opens one logged-on session that stays usable for `deadline_ms` and runs `work` with a service
  // call function and the logged-on SteamID. The session closes when `work` returns; the result is
  // the logon failure or whatever `work` returns.
  CmError WithServiceSession(const StoredSession& session, std::uint32_t deadline_ms, const std::atomic<bool>& cancel,
                             const std::function<CmError(const ServiceCall& call, std::uint64_t steam_id)>& work);

 private:
  friend class CmSharedSession;
  class Session;
  // The session one call runs on: its own, or the shared one held for the call's duration.
  struct Lease {
    std::unique_ptr<MessageTransport> owned_transport;
    std::unique_ptr<Session> owned_session;
    Session* session = nullptr;
    bool reused = false;
    std::unique_lock<std::mutex> hold;  // the shared session, while this call uses it
  };
  // Directory lookup, connect, hello and logon. On success the transport is connected and the
  // session logged on.
  CmError Open(const StoredSession& session, const std::atomic<bool>& cancel,
               std::chrono::steady_clock::time_point deadline,
               std::unique_ptr<MessageTransport>* transport, std::unique_ptr<Session>* opened);
  // A logged-on session: the shared one when it is logged on with the same credentials (opened
  // again otherwise), or a new one of this call's own.
  CmError Acquire(const StoredSession& session, const std::atomic<bool>& cancel,
                  std::chrono::steady_clock::time_point deadline, Lease* lease);
  // Ends the call: the shared session stays open unless the call left it unusable; a session of the
  // call's own logs off and closes.
  void Release(Lease* lease, CmError result);
  // Logs off and closes the lease's session, shared or not, and lets the shared one go.
  void CloseSession(Lease* lease);
  // Closes a session without a log-off, which would sign the device out of Steam.
  static void EndSession(Session* session, MessageTransport* transport,
                         const std::function<void(const std::string&)>& log);
  void Log(const std::string& line) const;
  CmClientHooks hooks_;
  std::string owner_ = "cm";
  std::shared_ptr<CmSharedSession> shared_;
  std::string ca_bundle_;
  const Storage* device_storage_ = nullptr;
  std::optional<cm::DeviceIdentity> device_;
  std::unique_ptr<HttpClient> http_;
};

// Adapts CM renewal to the auth worker: a refused CM logon means the credentials are gone; network
// trouble stays a network error so cache-only startup still applies.
AuthResult RenewThroughCm(CmClient& client, const StoredSession& session,
                          const std::atomic<bool>& cancel);

}  // namespace playden::steam
