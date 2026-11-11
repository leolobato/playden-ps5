#include "steam/backend/cm_client.h"

#include "steam/backend/app_info_http.h"
#include "steam/backend/auth_messages.h"
#include "steam/backend/binary_kv.h"
#include "steam/backend/gzip.h"
#include "steam/backend/json.h"
#include "steam/backend/token_fingerprint.h"

#include <mbedtls/sha1.h>
#include <mbedtls/sha256.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace playden::steam {
namespace {

using Clock = std::chrono::steady_clock;
constexpr std::size_t kMaxEndpoints = 4;
constexpr std::size_t kPicsBatch = 50;
constexpr std::uint32_t kMultiDepth = 16;
constexpr std::size_t kMaxJobBytes = 16 * 1024 * 1024;
constexpr std::size_t kDirectoryBodyLimit = 256 * 1024;
constexpr const char* kDeviceSeedFile = "steam-device-id";
// ELicenseFlags that make a license unavailable: renewal failed, pending, expired, cancelled,
// fraud, not activated, pending refund, cancelled by partner. Borrowed (0x4000) is not among them: a
// license another Steam family member shares is usable here.
constexpr std::uint32_t kUnavailableLicense =
    0x02 | 0x04 | 0x08 | 0x10 | 0x20 | 0x400 | 0x800 | 0x2000 | 0x40000;
constexpr std::uint32_t kRenewLicense = 0x1;
constexpr std::uint32_t kBorrowedLicense = 0x4000;
// At most this many per-license family lines per dump, so a huge account cannot rotate the log away.
constexpr std::size_t kMaxFamilyLicenseLines = 300;
// At most this many package IDs on one family PICS line.
constexpr std::size_t kMaxFamilyIds = 100;

std::string Hex(const unsigned char* data, std::size_t size) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string text;
  text.reserve(size * 2);
  for (std::size_t index = 0; index < size; ++index) {
    text.push_back(kDigits[data[index] >> 4]);
    text.push_back(kDigits[data[index] & 15]);
  }
  return text;
}

bool ValidSeed(std::string_view seed) {
  if (seed.size() < 16 || seed.size() > 64) return false;
  return std::all_of(seed.begin(), seed.end(), [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-';
  });
}

// Endpoints are host:port values from the directory; anything else could alter the URL.
bool ValidEndpoint(const std::string& endpoint) {
  if (endpoint.empty() || endpoint.size() > 255) return false;
  return std::all_of(endpoint.begin(), endpoint.end(), [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           c == '-' || c == '.' || c == ':';
  });
}

// Package 0 counts: older accounts hold Valve's games there (Team Fortress 2's content, issue #58), and the
// reference reads every package in the license list.
bool OwnLicense(const cm::License& license, std::uint32_t account_id) {
  return (license.owner_id == 0 || license.owner_id == account_id) && (license.flags & kBorrowedLicense) == 0;
}

// Why an unusable license is left out, or nullptr when it is usable: own or borrowed from a family member.
const char* UnusableLicense(const cm::License& license) {
  if (license.flags & kUnavailableLicense) return "flags";
  if (license.minute_limit > 0 && license.minutes_used >= license.minute_limit) return "minutes";
  return nullptr;
}

std::string HexFlags(std::uint32_t flags) {
  char text[16];
  std::snprintf(text, sizeof(text), "0x%x", flags);
  return text;
}

// Comma-separated IDs, at most kMaxFamilyIds of them, then how many more there were.
std::string IdList(const std::vector<std::uint32_t>& ids) {
  std::string text;
  for (std::size_t index = 0; index < ids.size() && index < kMaxFamilyIds; ++index)
    text += (index ? "," : "") + std::to_string(ids[index]);
  if (ids.size() > kMaxFamilyIds) text += ",+" + std::to_string(ids.size() - kMaxFamilyIds);
  return text.empty() ? "none" : text;
}

// FNV-1a over what the per-license family lines show, to dump them only when the list changed.
std::uint64_t LicenseListHash(const std::vector<cm::License>& licenses, std::uint32_t account_id) {
  std::uint64_t hash = 0xcbf29ce484222325ull;
  auto mix = [&hash](std::uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
      hash ^= (value >> shift) & 0xff;
      hash *= 0x100000001b3ull;
    }
  };
  mix(account_id);
  for (const auto& license : licenses) {
    mix(license.package_id);
    mix(license.owner_id);
    mix(license.flags);
    mix(license.license_type);
    mix(license.time_created);
    mix(static_cast<std::uint32_t>(license.minute_limit) ^
        (static_cast<std::uint64_t>(static_cast<std::uint32_t>(license.minutes_used)) << 32));
    mix(license.access_token != 0);
  }
  return hash;
}

// The license list whose per-license family lines were last written, process-wide: a library refresh
// with the same list writes only the summary.
std::mutex g_family_mutex;
std::uint64_t g_family_dumped = 0;

bool ShouldDumpLicenses(std::uint64_t hash) {
  std::lock_guard<std::mutex> lock(g_family_mutex);
  if (hash == g_family_dumped) return false;
  g_family_dumped = hash;
  return true;
}

// The family summary of one license-list load and, when the list changed, a line per license that isn't
// plainly the account's own. Account IDs stay out of the log: an owner is "self", "0" (none) or "family-<n>",
// numbered by ascending ID, which is enough to tell owners apart.
void LogLicenses(const std::function<void(const std::string&)>& log, const std::vector<cm::License>& licenses,
                 const std::map<std::uint32_t, const cm::License*>& chosen, std::uint32_t account_id) {
  std::size_t own = 0, borrowed = 0, dropped = 0;
  std::map<std::uint32_t, std::size_t> owners;  // other account IDs
  std::size_t bits[32] = {};
  for (const auto& license : licenses) {
    if (UnusableLicense(license)) ++dropped;
    else if (OwnLicense(license, account_id)) ++own;
    else ++borrowed;
    if (license.owner_id != 0 && license.owner_id != account_id) ++owners[license.owner_id];
    for (int bit = 0; bit < 32; ++bit) bits[bit] += (license.flags >> bit) & 1;
  }
  const auto hash = LicenseListHash(licenses, account_id);
  const bool dump = ShouldDumpLicenses(hash);
  std::map<std::uint32_t, std::string> owner_labels;
  for (const auto& [owner, count] : owners)
    owner_labels.emplace(owner, "family-" + std::to_string(owner_labels.size() + 1));
  const auto owner_label = [&](std::uint32_t owner) -> std::string {
    if (owner == 0) return "0";
    return owner == account_id ? "self" : owner_labels.at(owner);
  };
  std::string owner_text, flag_text;
  std::size_t listed = 0;
  for (const auto& [owner, count] : owners) {
    if (listed++ == 16) {
      owner_text += ",+" + std::to_string(owners.size() - 16);
      break;
    }
    owner_text += (owner_text.empty() ? "" : ",") + owner_labels.at(owner) + ":" + std::to_string(count);
  }
  for (int bit = 0; bit < 32; ++bit)
    if (bits[bit])
      flag_text += (flag_text.empty() ? "" : ",") + HexFlags(1u << bit) + ":" + std::to_string(bits[bit]);
  log("family step=licenses total=" + std::to_string(licenses.size()) + " own=" + std::to_string(own) +
      " borrowed=" + std::to_string(borrowed) + " dropped=" + std::to_string(dropped) +
      " owners=" + (owner_text.empty() ? "none" : owner_text) + " flags=" + (flag_text.empty() ? "none" : flag_text) +
      " changed=" + (dump ? "y" : "n"));
  if (!dump) return;
  // Another owner, any flag but Renew, or left out. Licenses from another account or marked Borrowed come
  // first, so the cap never hides them behind an old account's flagged licenses.
  std::size_t lines = 0, left_out = 0;
  auto detail = [&](const cm::License& license) {
    if (lines == kMaxFamilyLicenseLines) {
      ++left_out;
      return;
    }
    ++lines;
    std::string verdict;
    if (const auto* reason = UnusableLicense(license)) {
      verdict = std::string("result=dropped reason=") + reason;
      if (license.flags & kUnavailableLicense) verdict += ":" + HexFlags(license.flags & kUnavailableLicense);
    } else if (chosen.at(license.package_id) != &license) {
      verdict = "result=superseded reason=" +
                std::string(OwnLicense(*chosen.at(license.package_id), account_id) ? "own" : "duplicate");
    } else {
      verdict = std::string("result=kept reason=") + (OwnLicense(license, account_id) ? "own" : "borrowed");
    }
    log("family step=license package=" + std::to_string(license.package_id) +
        " owner=" + owner_label(license.owner_id) + " flags=" + HexFlags(license.flags) +
        " type=" + std::to_string(license.license_type) + " created=" + std::to_string(license.time_created) +
        " token=" + (license.access_token ? "y" : "n") + " " + verdict);
  };
  auto family = [account_id](const cm::License& license) {
    return (license.owner_id != 0 && license.owner_id != account_id) || (license.flags & kBorrowedLicense);
  };
  for (const auto& license : licenses)
    if (family(license)) detail(license);
  for (const auto& license : licenses)
    if (!family(license) && ((license.flags & ~kRenewLicense) != 0 || UnusableLicense(license))) detail(license);
  if (left_out) log("family step=license truncated=" + std::to_string(left_out));
}

std::uint32_t RemainingMs(Clock::time_point deadline) {
  const auto remaining =
      std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
  return remaining <= 0 ? 0 : static_cast<std::uint32_t>(remaining);
}

std::mutex g_log_mutex;
std::function<void(const std::string&)> g_log;

void SharedLog(const std::string& line) {
  std::lock_guard<std::mutex> lock(g_log_mutex);
  if (g_log) g_log("owner=shared " + line);
}

// Friends kept per session, at most: a full list holds up to 2000; the rest is room for persona states of others.
constexpr std::size_t kMaxKnownFriends = 4000;
// Persona state messages logged one by one after a logon, then a summary at most this often.
constexpr std::uint64_t kLoggedPersonaMessages = 3;
constexpr auto kPersonaLogInterval = std::chrono::seconds(60);

// Process-wide backoff after Steam refuses a logon for a reason that isn't the credentials.
std::mutex g_backoff_mutex;
int g_refusals = 0;
Clock::time_point g_not_before{};

void NoteRefusal() {
  static constexpr int kMinutes[] = {1, 5, 20};
  std::lock_guard<std::mutex> lock(g_backoff_mutex);
  g_not_before = Clock::now() + std::chrono::minutes(kMinutes[std::min(g_refusals, 2)]);
  ++g_refusals;
}

// Fingerprints of refresh tokens Steam refused at logon for the credentials. Asking again with the
// same token only earns RateLimitExceeded; a new token (Sign in again, rotation) is tried normally.
std::set<std::string> g_refused_tokens;

void NoteLogon() {
  std::lock_guard<std::mutex> lock(g_backoff_mutex);
  g_refusals = 0;
  g_not_before = {};
}

CmError FromTransport(HttpError error) {
  switch (error) {
    case HttpError::kNone: return CmError::kNone;
    case HttpError::kCanceled: return CmError::kCanceled;
    case HttpError::kTimeout: return CmError::kTimeout;
    case HttpError::kProtocolError:
    case HttpError::kResponseTooLarge: return CmError::kProtocol;
    default: return CmError::kNetwork;
  }
}

}  // namespace

const char* CmErrorName(CmError error) {
  switch (error) {
    case CmError::kNone: return "none";
    case CmError::kCanceled: return "canceled";
    case CmError::kNoSession: return "no_session";
    case CmError::kNetwork: return "network";
    case CmError::kRejected: return "rejected";
    case CmError::kProtocol: return "protocol";
    case CmError::kTimeout: return "timeout";
  }
  return "protocol";
}

const char* CmStageName(CmStage stage) {
  switch (stage) {
    case CmStage::kNone: return "none";
    case CmStage::kBackoff: return "backoff";
    case CmStage::kDirectory: return "directory";
    case CmStage::kConnect: return "connect";
    case CmStage::kHello: return "hello";
    case CmStage::kLogon: return "logon";
    case CmStage::kRequest: return "request";
  }
  return "none";
}

std::string CmFailureText(const CmFailure& failure) {
  if (failure.stage == CmStage::kNone) return {};
  std::string text = std::string("stage=") + CmStageName(failure.stage);
  if (failure.transport != HttpError::kNone) text += std::string(" transport=") + HttpErrorName(failure.transport);
  if (failure.transport_code != 0) text += " curl=" + std::to_string(failure.transport_code);
  if (failure.eresult != 0) text += " eresult=" + std::to_string(failure.eresult);
  if (failure.refused) text += " refused=1";
  return text;
}

void SetCmLog(std::function<void(const std::string& line)> log) {
  std::lock_guard<std::mutex> lock(g_log_mutex);
  g_log = std::move(log);
}

bool CmBackoffActive() {
  std::lock_guard<std::mutex> lock(g_backoff_mutex);
  return Clock::now() < g_not_before;
}

void ResetCmBackoff() {
  NoteLogon();
  std::lock_guard<std::mutex> lock(g_backoff_mutex);
  g_refused_tokens.clear();
}

cm::DeviceIdentity MakeDeviceIdentity(std::string_view seed) {
  cm::DeviceIdentity device;
  const std::string login_input = "login-id:" + std::string(seed);
  unsigned char digest[32] = {};
  mbedtls_sha256(reinterpret_cast<const unsigned char*>(login_input.data()), login_input.size(),
                 digest, 0);
  device.login_id = static_cast<std::uint32_t>(digest[0]) << 24 |
                    static_cast<std::uint32_t>(digest[1]) << 16 |
                    static_cast<std::uint32_t>(digest[2]) << 8 | static_cast<std::uint32_t>(digest[3]);
  if (device.login_id == 0) device.login_id = 1;
  device.machine_name = "Playden";
  auto append = [&device](std::string_view text) {
    device.machine_id.insert(device.machine_id.end(), text.begin(), text.end());
  };
  device.machine_id.push_back(0x00);
  append("MessageObject");
  device.machine_id.push_back(0x00);
  for (const char* key : {"BB3", "FF2", "3B3"}) {
    const std::string input = std::string(key) + ":" + std::string(seed);
    unsigned char sha1[20] = {};
    mbedtls_sha1(reinterpret_cast<const unsigned char*>(input.data()), input.size(), sha1);
    device.machine_id.push_back(0x01);
    append(key);
    device.machine_id.push_back(0x00);
    append(Hex(sha1, sizeof(sha1)));
    device.machine_id.push_back(0x00);
  }
  device.machine_id.push_back(0x08);
  device.machine_id.push_back(0x08);
  return device;
}

std::string LoadOrCreateDeviceSeed(const Storage& storage) {
  // The auth and library CM clients share one install identity; create it once.
  static std::mutex mutex;
  std::lock_guard<std::mutex> lock(mutex);
  const auto saved = storage.Read(kDeviceSeedFile, 256);
  if (saved.ok()) {
    std::string text(saved.value().begin(), saved.value().end());
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
      text.pop_back();
    if (ValidSeed(text)) return text;
  }
  // A seed that could not be saved is still this run's identity: the sign-in and the CM logons that share this
  // storage must name the same machine.
  static std::map<const Storage*, std::string> unsaved;
  if (const auto kept = unsaved.find(&storage); kept != unsaved.end()) return kept->second;
  // A per-install value only has to differ from other installs, so hash several local sources
  // rather than depend on an entropy device the console may not provide.
  const auto wall = std::chrono::system_clock::now().time_since_epoch().count();
  const auto steady = Clock::now().time_since_epoch().count();
  const auto thread = std::hash<std::thread::id>{}(std::this_thread::get_id());
  const int local = 0;
  const std::string input = std::to_string(wall) + ":" + std::to_string(steady) + ":" +
                            std::to_string(thread) + ":" +
                            std::to_string(reinterpret_cast<std::uintptr_t>(&local));
  unsigned char digest[32] = {};
  mbedtls_sha256(reinterpret_cast<const unsigned char*>(input.data()), input.size(), digest, 0);
  const std::string seed = Hex(digest, 16);
  if (!storage.EnsureRootDirectory().ok() ||
      !storage.WriteAtomicPrivate(kDeviceSeedFile, Bytes(seed.begin(), seed.end())).ok())
    unsaved[&storage] = seed;
  return seed;
}

// One connected CM conversation. Frames are handled as they arrive; callers wait for state.
class CmClient::Session {
 public:
  Session(MessageTransport& transport, Clock::time_point deadline, const std::atomic<bool>& cancel)
      : transport_(transport), deadline_(deadline), cancel_(&cancel) {}

  // A kept session serves a later call with that call's deadline and cancel flag.
  void Rebind(Clock::time_point deadline, const std::atomic<bool>& cancel) {
    deadline_ = deadline;
    cancel_ = &cancel;
  }
  bool usable() const { return fatal_ == CmError::kNone && !logged_off_ && !logon_pending_ && logon_; }
  std::int32_t heartbeat_seconds() const { return heartbeat_seconds_; }
  // Steam's EResult for a refused logon, or for being logged off, when it sent one.
  std::optional<std::int32_t> logon_eresult() const {
    return logon_ ? std::optional<std::int32_t>(logon_->eresult) : std::nullopt;
  }
  std::optional<std::int32_t> logged_off_eresult() const { return logged_off_eresult_; }
  bool logged_off() const { return logged_off_; }

  CmError Send(std::uint32_t emsg, const std::vector<std::uint8_t>& body,
               std::optional<std::uint64_t> jobid = std::nullopt) {
    cm::Header header;
    header.steam_id = steam_id_;
    header.session_id = session_id_;
    header.jobid_source = jobid;
    const auto frame = cm::EncodeFrame(emsg, header, body);
    if (frame.empty()) return CmError::kProtocol;
    const auto remaining = RemainingMs(deadline_);
    if (remaining == 0) return CmError::kTimeout;
    return FromTransport(transport_.Send(frame, remaining, *cancel_));
  }

  // Waits until ready() holds, sending heartbeats on the server interval once logged on.
  template <typename Ready>
  CmError WaitFor(Ready ready) {
    for (;;) {
      if (fatal_ != CmError::kNone) return fatal_;
      if (ready()) return CmError::kNone;
      if (logged_off_) return CmError::kNetwork;
      if (cancel_->load(std::memory_order_acquire)) return CmError::kCanceled;
      if (const auto sent = SendPersonaState(); sent != CmError::kNone) return sent;
      if (!friend_requests_.empty() && logon_ && logon_->eresult == cm::kEResultOk)
        if (const auto sent = RequestFriendData(); sent != CmError::kNone) return sent;
      if (heartbeat_seconds_ > 0 && Clock::now() >= next_heartbeat_) {
        if (const auto sent = Send(cm::kEMsgClientHeartBeat, cm::EncodeHeartBeat());
            sent != CmError::kNone)
          return sent;
        next_heartbeat_ = Clock::now() + std::chrono::seconds(heartbeat_seconds_);
      }
      const auto remaining = RemainingMs(deadline_);
      if (remaining == 0) return CmError::kTimeout;
      std::vector<std::uint8_t> message;
      const auto received =
          transport_.Receive(&message, std::min<std::uint32_t>(remaining, 250), *cancel_);
      if (received == HttpError::kTimeout) continue;
      if (received != HttpError::kNone) return FromTransport(received);
      Handle(message.data(), message.size(), 0);
    }
  }

  CmError Logon(const StoredSession& stored, const cm::DeviceIdentity& device) {
    logon_pending_ = true;
    const auto body = cm::EncodeLogon(stored.account_name, stored.refresh_token,
                                      stored.cell_id.value_or(0), device);
    if (body.empty()) return CmError::kProtocol;
    if (const auto sent = Send(cm::kEMsgClientLogon, body); sent != CmError::kNone) return sent;
    if (const auto waited = WaitFor([this] { return logon_.has_value(); });
        waited != CmError::kNone)
      return waited;
    logon_pending_ = false;
    // Only a refusal of the credentials is kRejected (callers treat it as signed out); Steam being
    // busy or rate limiting is a network condition.
    if (logon_->eresult != cm::kEResultOk)
      return cm::CredentialRefusal(logon_->eresult) ? CmError::kRejected : CmError::kNetwork;
    heartbeat_seconds_ = logon_->heartbeat_seconds;
    next_heartbeat_ = Clock::now() + std::chrono::seconds(heartbeat_seconds_);
    logon_at_ = Clock::now();
    return SendPersonaState();
  }

  // With friends on and logged on: sends the persona state the shared session asks for when it is not the one last
  // sent (after each logon, and after a change while logged on). 0 sends nothing. A send that failed is tried again
  // on the next turn.
  CmError SendPersonaState() {
    const auto state = persona_state_ ? persona_state_->load(std::memory_order_acquire) : 0;
    if (!friends_enabled_ || state == 0 || state == sent_persona_state_ || !logon_ ||
        logon_->eresult != cm::kEResultOk || logon_pending_)
      return CmError::kNone;
    const auto sent = Send(cm::kEMsgClientChangeStatus, cm::EncodeChangeStatus(state));
    SharedLog(std::string("friends change_status ") +
              (state == cm::kPersonaStateInvisible ? "invisible" : state == cm::kPersonaStateOnline ? "online"
                                                                                                     : "other") +
              " result=" + CmErrorName(sent));
    if (sent == CmError::kNone) sent_persona_state_ = state;
    return sent;
  }

  CmError Licenses(std::vector<cm::License>* licenses) {
    if (const auto waited = WaitFor([this] { return licenses_.has_value(); });
        waited != CmError::kNone)
      return waited;
    *licenses = licenses_->licenses;
    return CmError::kNone;
  }

  // Requests one batch and collects every response part until response_pending is false.
  CmError Pics(const std::vector<cm::PackageRequest>& batch,
               std::vector<cm::PicsResponse>* parts) {
    return PicsBody(cm::EncodePicsPackagesRequest(batch), parts);
  }

  CmError PicsBody(const std::vector<std::uint8_t>& body, std::vector<cm::PicsResponse>* parts) {
    const std::uint64_t jobid = ++job_counter_;
    job_ = Job{jobid, {}, 0, false, CmError::kNone};
    if (body.empty()) return CmError::kProtocol;
    if (const auto sent = Send(cm::kEMsgClientPICSProductInfoRequest, body, jobid);
        sent != CmError::kNone)
      return sent;
    if (const auto waited = WaitFor([this] { return job_ && job_->done; });
        waited != CmError::kNone)
      return waited;
    const auto error = job_->error;
    *parts = std::move(job_->parts);
    job_.reset();
    return error;
  }

  // Calls a unified service method and waits for its single response body.
  CmError ServiceMethod(const std::string& name, const std::vector<std::uint8_t>& body,
                        std::vector<std::uint8_t>* reply) {
    return Call(cm::kEMsgServiceMethodCallFromClient, name, body, cm::kEMsgServiceMethodResponse, reply);
  }

  // Sends one job request and waits for its single response of the expected EMsg.
  CmError Call(std::uint32_t emsg, const std::string& job_name, const std::vector<std::uint8_t>& body,
               std::uint32_t response_emsg, std::vector<std::uint8_t>* reply) {
    const std::uint64_t jobid = ++job_counter_;
    service_ = ServiceJob{jobid, false, CmError::kNone, {}, response_emsg};
    cm::Header header;
    header.steam_id = steam_id_;
    header.session_id = session_id_;
    header.jobid_source = jobid;
    header.target_job_name = job_name;
    const auto frame = cm::EncodeFrame(emsg, header, body);
    if (frame.empty()) return CmError::kProtocol;
    const auto remaining = RemainingMs(deadline_);
    if (remaining == 0) return CmError::kTimeout;
    if (const auto sent = FromTransport(transport_.Send(frame, remaining, *cancel_));
        sent != CmError::kNone)
      return sent;
    if (const auto waited = WaitFor([this] { return service_ && service_->done; });
        waited != CmError::kNone)
      return waited;
    const auto error = service_->error;
    last_eresult_ = service_->eresult;
    *reply = std::move(service_->body);
    service_.reset();
    return error;
  }

  // EResult of the last refused call, or kEResultOk.
  int last_eresult() const { return last_eresult_; }

  std::uint64_t steam_id() const { return steam_id_; }

  // The license list Steam last pushed, and how many it has pushed: one after logon, and another
  // whenever the account's licenses change (a game bought or added to the library).
  const std::optional<cm::LicenseList>& licenses() const { return licenses_; }
  std::uint64_t license_serial() const { return license_serial_; }

  // Waits at most `wait` for a license list newer than `serial`. kTimeout when none came; the
  // call's own deadline is kept for what follows.
  CmError WaitForLicensesAfter(std::uint64_t serial, Clock::duration wait) {
    const auto saved = deadline_;
    deadline_ = Clock::now() + wait;
    const auto waited = WaitFor([this, serial] { return license_serial_ > serial; });
    deadline_ = saved;
    return waited;
  }

  // An idle turn until the deadline: answers the server and sends a heartbeat when due. kTimeout
  // means the session is still fine.
  CmError Idle() { return WaitFor([] { return false; }); }

  // The shared session only, with online-steam: keeps the friends list Steam pushes after logon and asks
  // for the friends' persona states. Set before the logon.
  void EnableFriends() { friends_enabled_ = true; }
  // With friends on: the persona state to show friends (ClientChangeStatus after the logon and after each change),
  // read on every turn. It belongs to the shared session, which outlives this one.
  void FollowPersonaState(const std::atomic<std::uint32_t>* state) { persona_state_ = state; }
  // Changes whenever the friends or their persona states change.
  std::uint64_t friends_serial() const { return friends_serial_; }
  CmFriends Friends() const {
    CmFriends out;
    out.account = friends_listed_ ? steam_id_ : 0;
    for (const auto& [id, known] : friends_)
      if (known.relationship == cm::kFriendRelationshipFriend) out.friends.push_back(known.person);
    return out;
  }

 private:
  struct ServiceJob {
    std::uint64_t id = 0;
    bool done = false;
    CmError error = CmError::kNone;
    std::vector<std::uint8_t> body;
    std::uint32_t response_emsg = cm::kEMsgServiceMethodResponse;
    int eresult = cm::kEResultOk;
  };
  int last_eresult_ = cm::kEResultOk;
  struct Job {
    std::uint64_t id = 0;
    std::vector<cm::PicsResponse> parts;
    std::size_t bytes = 0;
    bool done = false;
    CmError error = CmError::kNone;
  };

  struct KnownFriend {
    CmFriend person;
    std::uint32_t relationship = 0;
  };
  // What persona states carried since the last log line. No names or IDs, only counts.
  struct PersonaStats {
    std::uint64_t messages = 0;
    std::uint64_t entries = 0;
    std::uint64_t name = 0;
    std::uint64_t state = 0;
    std::uint64_t app = 0;
    std::uint64_t lobby = 0;
    std::uint64_t avatar = 0;
    std::uint64_t rich = 0;
    std::uint32_t flags = 0;
  };

  KnownFriend* Known(std::uint64_t id) {
    if (const auto found = friends_.find(id); found != friends_.end()) return &found->second;
    if (friends_.size() >= kMaxKnownFriends) return nullptr;
    auto& known = friends_[id];
    known.person.steam_id = id;
    return &known;
  }

  // Steam pushes the list after logon (and again on a change); the persona states are asked for once per
  // friend added, never again on a timer.
  void HandleFriendsList(const std::vector<std::uint8_t>& body) {
    cm::FriendsList list;
    if (!cm::DecodeFriendsList(body, &list)) {
      SharedLog("friends list unreadable");
      return;
    }
    if (!list.incremental)
      for (auto& entry : friends_) entry.second.relationship = 0;
    for (const auto& relation : list.friends) {
      if (!cm::IndividualSteamId(relation.steam_id)) continue;
      auto* known = Known(relation.steam_id);
      if (!known) continue;
      const bool added = known->relationship != cm::kFriendRelationshipFriend;
      known->relationship = relation.relationship;
      if (relation.relationship == cm::kFriendRelationshipFriend && (added || !list.incremental))
        friend_requests_.insert(relation.steam_id);
    }
    friends_listed_ = true;
    ++friends_serial_;
    std::size_t count = 0;
    for (const auto& entry : friends_) count += entry.second.relationship == cm::kFriendRelationshipFriend;
    SharedLog("friends list friends=" + std::to_string(count) + " incremental=" + (list.incremental ? "1" : "0"));
  }

  // The IDs stay pending until the request went out, so a send that ran out of time is tried on the next turn.
  CmError RequestFriendData() {
    const std::vector<std::uint64_t> ids(friend_requests_.begin(), friend_requests_.end());
    const auto body = cm::EncodeRequestFriendData(cm::kFriendDataRequested, ids);
    if (body.empty()) return CmError::kProtocol;
    const auto sent = Send(cm::kEMsgClientRequestFriendData, body);
    SharedLog("friends request ids=" + std::to_string(ids.size()) + " result=" + CmErrorName(sent));
    if (sent == CmError::kNone) friend_requests_.clear();
    return sent;
  }

  // A group the update carries (status_flags names it, or one of its fields came) replaces the friend's;
  // a missing field in it is cleared. Groups not carried stay. A name is only ever replaced by a name.
  void HandlePersonaState(const std::vector<std::uint8_t>& body) {
    cm::PersonaState state;
    if (!cm::DecodePersonaState(body, &state)) {
      SharedLog("friends persona unreadable");
      return;
    }
    const std::uint32_t flags = state.status_flags.value_or(0);
    bool changed = false;
    for (auto& entry : state.friends) {
      ++persona_stats_.entries;
      persona_stats_.name += entry.player_name.has_value();
      persona_stats_.state += entry.persona_state.has_value();
      persona_stats_.app += entry.game_app_id.has_value() || entry.game_id.has_value();
      persona_stats_.lobby += entry.lobby_id.has_value() && *entry.lobby_id != 0;
      persona_stats_.avatar += entry.avatar_hash.has_value();
      persona_stats_.rich += entry.rich_presence.has_value();
      if (entry.steam_id == steam_id_ || !cm::IndividualSteamId(entry.steam_id)) continue;
      auto* known = Known(entry.steam_id);
      if (!known) continue;
      auto& person = known->person;
      if ((flags & cm::kPersonaFlagStatus) || entry.persona_state) person.persona_state = entry.persona_state.value_or(0);
      if (entry.player_name) person.name = std::move(*entry.player_name);
      if ((flags & cm::kPersonaFlagPresence) || entry.avatar_hash) {
        const auto& hash = entry.avatar_hash;
        const bool real = hash && hash->size() == 20 &&
                          std::any_of(hash->begin(), hash->end(), [](std::uint8_t byte) { return byte != 0; });
        person.avatar_hash = real ? Hex(hash->data(), hash->size()) : std::string{};
      }
      if ((flags & (cm::kPersonaFlagPresence | cm::kPersonaFlagGameExtraInfo)) || entry.game_app_id || entry.game_id ||
          entry.lobby_id || entry.game_name) {
        person.game_id = entry.game_id.value_or(0);
        person.app_id = entry.game_app_id.value_or(0);
        // A Steam game's GameID is its app ID with type 0 (bits 24-31).
        if (person.app_id == 0 && person.game_id != 0 && ((person.game_id >> 24) & 0xff) == 0)
          person.app_id = static_cast<std::uint32_t>(person.game_id & 0xffffff);
        person.game_name = entry.game_name.value_or(std::string{});
        person.lobby_id = entry.lobby_id.value_or(0);
      }
      if ((flags & cm::kPersonaFlagRichPresence) || entry.rich_presence)
        person.rich_presence = entry.rich_presence ? std::move(*entry.rich_presence)
                                                   : std::vector<std::pair<std::string, std::string>>{};
      changed = true;
    }
    if (changed) ++friends_serial_;
    ++persona_stats_.messages;
    ++persona_messages_;
    persona_stats_.flags = flags;
    LogPersonaStats();
  }

  // The first few messages after a logon one by one, then a summary at most once a minute while they keep
  // coming: enough to tell whether Steam keeps sending updates.
  void LogPersonaStats() {
    const auto now = Clock::now();
    if (persona_messages_ > kLoggedPersonaMessages && now < next_persona_log_) return;
    next_persona_log_ = now + kPersonaLogInterval;
    const auto since = std::chrono::duration_cast<std::chrono::seconds>(now - logon_at_).count();
    SharedLog("friends persona messages=" + std::to_string(persona_stats_.messages) +
              " entries=" + std::to_string(persona_stats_.entries) + " name=" + std::to_string(persona_stats_.name) +
              " state=" + std::to_string(persona_stats_.state) + " app=" + std::to_string(persona_stats_.app) +
              " lobby=" + std::to_string(persona_stats_.lobby) + " avatar=" + std::to_string(persona_stats_.avatar) +
              " rich=" + std::to_string(persona_stats_.rich) + " flags=" + HexFlags(persona_stats_.flags) +
              " total=" + std::to_string(persona_messages_) + " since_logon_s=" + std::to_string(since));
    persona_stats_ = {};
  }

  void Handle(const std::uint8_t* data, std::size_t size, std::uint32_t depth) {
    if (depth >= kMultiDepth) {
      fatal_ = CmError::kProtocol;
      return;
    }
    cm::Frame frame;
    if (!cm::DecodeFrame(data, size, &frame)) return;  // non-protobuf or unknown frames are ignored

    if (frame.emsg == cm::kEMsgClientLogOnResponse && logon_pending_ && session_id_ == 0 &&
        frame.header.steam_id != 0 && frame.header.session_id != 0) {
      steam_id_ = frame.header.steam_id;
      session_id_ = frame.header.session_id;
    }
    switch (frame.emsg) {
      case cm::kEMsgMulti: {
        cm::Multi multi;
        if (!cm::DecodeMulti(frame.body, &multi)) {
          fatal_ = CmError::kProtocol;
          return;
        }
        std::vector<std::uint8_t> payload;
        if (multi.size_unzipped > 0) {
          if (multi.size_unzipped > kMaxGunzipBytes ||
              !Gunzip(multi.message_body.data(), multi.message_body.size(), multi.size_unzipped,
                      &payload)) {
            fatal_ = CmError::kProtocol;
            return;
          }
        } else {
          payload = std::move(multi.message_body);
        }
        std::vector<std::vector<std::uint8_t>> frames;
        if (!cm::SplitMultiPayload(payload, &frames)) {
          fatal_ = CmError::kProtocol;
          return;
        }
        for (const auto& inner : frames) {
          Handle(inner.data(), inner.size(), depth + 1);
          if (fatal_ != CmError::kNone) return;
        }
        return;
      }
      case cm::kEMsgClientLogOnResponse: {
        cm::LogonResponse response;
        if (logon_pending_ && cm::DecodeLogonResponse(frame.body, &response)) logon_ = response;
        break;
      }
      case cm::kEMsgClientLicenseList: {
        cm::LicenseList list;
        if (!cm::DecodeLicenseList(frame.body, &list)) {
          fatal_ = CmError::kProtocol;
          return;
        }
        if (list.eresult && *list.eresult != cm::kEResultOk) {
          fatal_ = CmError::kRejected;
          return;
        }
        licenses_ = std::move(list);
        ++license_serial_;
        break;
      }
      case cm::kEMsgClientFriendsList:
        if (friends_enabled_) HandleFriendsList(frame.body);
        break;
      case cm::kEMsgClientPersonaState:
        if (friends_enabled_) HandlePersonaState(frame.body);
        break;
      case cm::kEMsgClientLoggedOff: {
        logged_off_ = true;
        std::int32_t eresult = cm::kEResultFail;
        if (cm::DecodeLoggedOff(frame.body, &eresult)) logged_off_eresult_ = eresult;
        break;
      }
      default:
        break;
    }

    if (service_ && !service_->done && frame.header.jobid_target &&
        *frame.header.jobid_target == service_->id) {
      service_->done = true;
      if (frame.header.eresult && *frame.header.eresult != cm::kEResultOk) {
        service_->error = CmError::kRejected;
        service_->eresult = static_cast<int>(*frame.header.eresult);
      }
      else if (frame.emsg != service_->response_emsg)
        service_->error = CmError::kProtocol;
      else
        service_->body = std::move(frame.body);
      return;
    }

    // Job responses: jobid_target defaults to all-ones when absent, so require presence.
    if (!job_ || job_->done || !frame.header.jobid_target ||
        *frame.header.jobid_target != job_->id)
      return;
    if (frame.header.eresult && *frame.header.eresult != cm::kEResultOk) {
      job_->done = true;
      job_->error = CmError::kRejected;
      return;
    }
    job_->bytes += frame.body.size();
    cm::PicsResponse part;
    if (frame.emsg != cm::kEMsgClientPICSProductInfoResponse || job_->bytes > kMaxJobBytes ||
        !cm::DecodePicsResponse(frame.body, &part)) {
      job_->done = true;
      job_->error = CmError::kProtocol;
      return;
    }
    const bool pending = part.response_pending;
    job_->parts.push_back(std::move(part));
    if (!pending) job_->done = true;
  }

  MessageTransport& transport_;
  Clock::time_point deadline_;
  const std::atomic<bool>* cancel_;
  std::optional<std::int32_t> logged_off_eresult_;
  std::uint64_t steam_id_ = cm::kAnonymousSteamId;
  std::int32_t session_id_ = 0;
  bool logon_pending_ = false;
  std::optional<cm::LogonResponse> logon_;
  std::optional<cm::LicenseList> licenses_;
  std::uint64_t license_serial_ = 0;
  bool logged_off_ = false;
  std::int32_t heartbeat_seconds_ = 0;
  Clock::time_point next_heartbeat_{};
  std::uint64_t job_counter_ = 0;
  std::optional<Job> job_;
  std::optional<ServiceJob> service_;
  CmError fatal_ = CmError::kNone;
  Clock::time_point logon_at_{};
  bool friends_enabled_ = false;
  const std::atomic<std::uint32_t>* persona_state_ = nullptr;
  std::uint32_t sent_persona_state_ = 0;  // what this logon last sent; 0 for nothing yet
  bool friends_listed_ = false;
  std::map<std::uint64_t, KnownFriend> friends_;
  std::set<std::uint64_t> friend_requests_;  // friends whose persona states are still to ask for
  std::uint64_t friends_serial_ = 0;
  PersonaStats persona_stats_;
  std::uint64_t persona_messages_ = 0;
  Clock::time_point next_persona_log_{};
};

CmClient::CmClient(std::string ca_bundle, const Storage& device_storage)
    : ca_bundle_(std::move(ca_bundle)), device_storage_(&device_storage) {}

CmClient::CmClient(CmClientHooks hooks, cm::DeviceIdentity device)
    : hooks_(std::move(hooks)), device_(std::move(device)) {}

class CmSharedSession {
 public:
  explicit CmSharedSession(CmSessionOptions options) : options_(options), pump_([this] { Pump(); }) {}
  ~CmSharedSession() {
    {
      std::lock_guard<std::mutex> lock(stop_mutex_);
      stop_ = true;
    }
    stop_wake_.notify_all();
    if (pump_.joinable()) pump_.join();
    std::lock_guard<std::mutex> lock(mutex_);
    Drop();
  }

 private:
  friend class CmClient;
  friend std::uint64_t CmLicenseChanges(const CmSharedSession& shared);
  friend std::uint64_t CmFriendChanges(const CmSharedSession& shared);
  friend CmFriends CmFriendsOf(const CmSharedSession& shared);
  friend bool CmSharedSessionLoggedOn(const CmSharedSession& shared);
  friend void SetCmPersonaState(CmSharedSession& shared, std::uint32_t state);
  // Keeps an idle session alive: answers the server and sends heartbeats on its interval. A call
  // waiting for the session never waits more than one short turn.
  void Pump() {
    static const std::atomic<bool> kNoCancel{false};
    std::unique_lock<std::mutex> stop_lock(stop_mutex_);
    while (!stop_) {
      stop_lock.unlock();
      {
        std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
        if (lock && session_) {
          session_->Rebind(Clock::now() + std::chrono::milliseconds(100), kNoCancel);
          const auto idle = session_->Idle();
          if (idle != CmError::kTimeout && idle != CmError::kNone) {
            Drop();
          } else {
            NoteLicenses();
            NoteFriends();
          }
        }
      }
      stop_lock.lock();
      stop_wake_.wait_for(stop_lock, std::chrono::milliseconds(100), [this] { return stop_; });
    }
  }
  // Compares the license list the session last received with the account's previous one. Steam
  // pushes a new list when a license is added or removed; the first list after a logon only
  // repeats what the library fetch already reads, unless it changed while the session was closed.
  // Called with mutex_ held, whenever a call or the pump has let the session read frames.
  void NoteLicenses() {
    if (!session_ || session_->license_serial() == noted_serial_ || !session_->licenses()) return;
    noted_serial_ = session_->license_serial();
    std::set<std::uint32_t> packages;
    for (const auto& license : session_->licenses()->licenses) packages.insert(license.package_id);
    const auto account = session_->steam_id();
    if (account == licensed_account_ && packages != licensed_packages_)
      license_changes_.fetch_add(1, std::memory_order_acq_rel);
    licensed_account_ = account;
    licensed_packages_ = std::move(packages);
  }
  // Publishes the session's friends when they changed. Called with mutex_ held, where NoteLicenses is.
  void NoteFriends() {
    if (!session_ || session_->friends_serial() == noted_friends_serial_) return;
    noted_friends_serial_ = session_->friends_serial();
    auto friends = session_->Friends();
    {
      std::lock_guard<std::mutex> lock(friends_mutex_);
      friends_ = std::move(friends);
    }
    friend_changes_.fetch_add(1, std::memory_order_acq_rel);
  }
  void Drop() {
    NoteLicenses();
    NoteFriends();
    noted_serial_ = 0;
    noted_friends_serial_ = 0;
    logged_on_.store(false, std::memory_order_release);
    if (transport_) CmClient::EndSession(session_.get(), transport_.get(), [](const std::string& line) {
      std::lock_guard<std::mutex> lock(g_log_mutex);
      if (g_log) g_log("owner=shared " + line);
    });
    session_.reset();
    transport_.reset();
    key_.clear();
  }

  std::mutex mutex_;  // held by a call for its duration, and by the pump for one short turn
  std::unique_ptr<MessageTransport> transport_;
  std::unique_ptr<CmClient::Session> session_;
  std::string key_;
  std::uint64_t noted_serial_ = 0;
  std::uint64_t licensed_account_ = 0;
  std::set<std::uint32_t> licensed_packages_;
  std::atomic<std::uint64_t> license_changes_{0};
  const CmSessionOptions options_;
  std::uint64_t noted_friends_serial_ = 0;
  mutable std::mutex friends_mutex_;  // only around friends_, never held with mutex_ waiting
  CmFriends friends_;
  std::atomic<std::uint64_t> friend_changes_{0};
  std::atomic<bool> logged_on_{false};
  std::atomic<std::uint32_t> persona_state_{0};
  std::mutex stop_mutex_;
  std::condition_variable stop_wake_;
  bool stop_ = false;
  std::thread pump_;
};

std::shared_ptr<CmSharedSession> MakeCmSharedSession(CmSessionOptions options) {
  auto shared = std::make_shared<CmSharedSession>(options);
  // Steam sends friends' online states only to a session that set a status itself: without ClientChangeStatus every
  // friend's persona state comes without a status, and the Friends tab shows them all offline.
  SetCmPersonaState(*shared, options.friends ? cm::kPersonaStateOnline : 0);
  return shared;
}

std::uint64_t CmLicenseChanges(const CmSharedSession& shared) {
  return shared.license_changes_.load(std::memory_order_acquire);
}

std::uint64_t CmFriendChanges(const CmSharedSession& shared) {
  return shared.friend_changes_.load(std::memory_order_acquire);
}

CmFriends CmFriendsOf(const CmSharedSession& shared) {
  std::lock_guard<std::mutex> lock(shared.friends_mutex_);
  return shared.friends_;
}

bool CmSharedSessionLoggedOn(const CmSharedSession& shared) {
  return shared.logged_on_.load(std::memory_order_acquire);
}

void SetCmPersonaState(CmSharedSession& shared, std::uint32_t state) {
  shared.persona_state_.store(state, std::memory_order_release);
}

void CmClient::EndSession(Session* session, MessageTransport* transport,
                          const std::function<void(const std::string&)>& log) {
  // No ClientLogOff: as the Steam client, the connection just closes so the saved sign-in stays valid.
  if (session && session->logged_off())
    log("logged_off eresult=" + std::to_string(session->logged_off_eresult().value_or(0)));
  if (transport) transport->Close();
}

CmClient::~CmClient() = default;

void CmClient::Log(const std::string& line) const {
  std::lock_guard<std::mutex> lock(g_log_mutex);
  if (g_log) g_log("owner=" + owner_ + " " + line);
}

void CmClient::CloseSession(Lease* lease) {
  if (lease->hold.owns_lock()) {
    shared_->Drop();
    lease->hold.unlock();
  } else if (lease->owned_transport) {
    EndSession(lease->owned_session.get(), lease->owned_transport.get(), [this](const std::string& line) { Log(line); });
  }
  lease->owned_session.reset();
  lease->owned_transport.reset();
  lease->session = nullptr;
  lease->reused = false;
}

CmError CmClient::Acquire(const StoredSession& stored, const std::atomic<bool>& cancel,
                          Clock::time_point deadline, Lease* lease) {
  last_failure_ = {};
  if (!shared_) {
    lease->reused = false;
    const auto opened = Open(stored, cancel, deadline, &lease->owned_transport, &lease->owned_session);
    lease->session = lease->owned_session.get();
    if (opened != CmError::kNone) CloseSession(lease);
    return opened;
  }
  // Another call (a library license scan can take a minute) may hold the session: waiting for it
  // stays cancelable, and this call's deadline starts once it has the session.
  const auto wait_start = Clock::now();
  lease->hold = std::unique_lock<std::mutex>(shared_->mutex_, std::try_to_lock);
  while (!lease->hold.owns_lock()) {
    if (cancel.load(std::memory_order_acquire)) return CmError::kCanceled;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    lease->hold.try_lock();
  }
  deadline += Clock::now() - wait_start;
  const std::string key = std::to_string(stored.steam_id) + ":" + stored.refresh_token;
  if (shared_->session_ && key == shared_->key_ && shared_->session_->usable()) {
    shared_->session_->Rebind(deadline, cancel);
    // A persona state changed while the session sat between calls goes out before this call's work; a session that
    // can't send it is opened again below (which sends it after the logon).
    if (shared_->session_->SendPersonaState() == CmError::kNone) {
      lease->session = shared_->session_.get();
      lease->reused = true;
      shared_->logged_on_.store(true, std::memory_order_release);
      return CmError::kNone;
    }
  }
  // Another account or a rotated token: the old session ends before the new logon.
  shared_->Drop();
  lease->reused = false;
  const auto opened = Open(stored, cancel, deadline, &shared_->transport_, &shared_->session_);
  if (opened != CmError::kNone) {
    CloseSession(lease);
    return opened;
  }
  shared_->key_ = key;
  lease->session = shared_->session_.get();
  shared_->logged_on_.store(true, std::memory_order_release);
  return CmError::kNone;
}

void CmClient::Release(Lease* lease, CmError result) {
  if (lease->hold.owns_lock()) {
    // The shared session stays for the next call unless this one left it in an unknown state.
    const bool clean = result == CmError::kNone || result == CmError::kRejected;
    if (!clean || !lease->session || !lease->session->usable()) {
      shared_->Drop();
    } else {
      shared_->NoteLicenses();
      shared_->NoteFriends();
    }
    lease->session = nullptr;
    lease->hold.unlock();
    return;
  }
  CloseSession(lease);
}

CmError CmClient::Open(const StoredSession& stored, const std::atomic<bool>& cancel,
                       Clock::time_point deadline, std::unique_ptr<MessageTransport>* transport,
                       std::unique_ptr<Session>* opened) {
  if (cancel.load(std::memory_order_acquire)) return CmError::kCanceled;
  if (stored.account_name.empty() || stored.refresh_token.empty() || stored.steam_id == 0)
    return CmError::kNoSession;
  if (!device_) {
    if (!device_storage_) return CmError::kProtocol;
    device_ = MakeDeviceIdentity(LoadOrCreateDeviceSeed(*device_storage_));
  }
  if (CmBackoffActive()) {
    Log("step=backoff error=network");
    last_failure_.stage = CmStage::kBackoff;
    return CmError::kNetwork;
  }
  const auto fingerprint = TokenFingerprint(stored.refresh_token);
  {
    std::lock_guard<std::mutex> lock(g_backoff_mutex);
    if (g_refused_tokens.count(fingerprint)) {
      last_failure_.stage = CmStage::kLogon;
      return CmError::kRejected;
    }
  }

  HttpRequest request;
  request.url =
      "https://api.steampowered.com/ISteamDirectory/GetCMListForConnect/v1/"
      "?cmtype=websockets&maxcount=16&format=json";
  request.headers = {{"User-Agent", "Playden/1.0"}};
  request.timeout_ms = std::max<std::uint32_t>(1, std::min<std::uint32_t>(10'000, RemainingMs(deadline)));
  request.max_response_bytes = kDirectoryBodyLimit;
  HttpResponse response;
  if (hooks_.directory) {
    response = hooks_.directory(request, cancel);
  } else {
    if (ca_bundle_.empty()) return CmError::kProtocol;
    if (!http_) http_ = std::make_unique<HttpClient>(ca_bundle_);
    response = http_->Perform(request, cancel);
  }
  if (!response.ok() || response.status_code != 200) {
    if (response.error == HttpError::kCanceled) return CmError::kCanceled;
    last_failure_.stage = CmStage::kDirectory;
    last_failure_.transport = response.error;
    last_failure_.transport_code = response.transport_code;
    // The transport's category and curl's code, never the address: a blocked name shows as name_resolution_failed.
    Log("step=directory error=network http=" + std::to_string(response.status_code) +
        (response.ok() ? std::string{}
                       : std::string(" transport=") + HttpErrorName(response.error) +
                             (response.transport_code ? " curl=" + std::to_string(response.transport_code) : std::string{})));
    // An HTTP answer that isn't 200 (429, 5xx) is Steam turning us away, not a lost network.
    if (response.ok()) {
      NoteRefusal();
      last_failure_.refused = true;
    }
    return CmError::kNetwork;
  }
  const std::string_view body(response.body.empty() ? ""
                                                    : reinterpret_cast<const char*>(response.body.data()),
                              response.body.size());
  const auto root = json::Parse(body, json::Limits{kDirectoryBodyLimit, 8, 4096, 4096});
  const auto* payload = root ? root->Find("response") : nullptr;
  const auto* list = payload ? payload->Find("serverlist") : nullptr;
  std::vector<std::string> endpoints;
  if (list && list->is_array()) {
    for (const auto& server : list->array()) {
      const auto* endpoint = server.Find("endpoint");
      if (endpoint && endpoint->is_string() && ValidEndpoint(endpoint->text()))
        endpoints.push_back(endpoint->text());
      if (endpoints.size() == kMaxEndpoints) break;
    }
  }
  if (endpoints.empty()) {
    last_failure_.stage = CmStage::kDirectory;
    Log("step=directory error=protocol");
    return CmError::kProtocol;
  }

  *transport = hooks_.transport ? hooks_.transport() : std::make_unique<WebSocketClient>(ca_bundle_);
  if (!*transport) return CmError::kProtocol;
  CmError connect_error = CmError::kNetwork;
  HttpError connect_transport = HttpError::kNone;
  bool connected = false;
  for (const auto& endpoint : endpoints) {
    const auto remaining = RemainingMs(deadline);
    if (remaining == 0) {
      connect_error = CmError::kTimeout;
      break;
    }
    connect_transport = (*transport)->Connect("wss://" + endpoint + "/cmsocket/", remaining, cancel);
    connect_error = FromTransport(connect_transport);
    if (connect_error == CmError::kNone) {
      connected = true;
      break;
    }
    if (connect_error == CmError::kCanceled) break;
  }
  if (!connected) {
    if (connect_error != CmError::kCanceled) {
      last_failure_.stage = CmStage::kConnect;
      last_failure_.transport = connect_transport;
      // The last endpoint's transport category (never the endpoint): name_resolution_failed for a blocked name.
      Log(std::string("step=connect error=") + CmErrorName(connect_error) +
          (connect_transport != HttpError::kNone ? std::string(" transport=") + HttpErrorName(connect_transport)
                                                 : std::string{}) +
          " endpoints=" + std::to_string(endpoints.size()));
    }
    return connect_error;
  }

  *opened = std::make_unique<Session>(**transport, deadline, cancel);
  // Only the shared session keeps friends; a call's own session never asks for them.
  if (shared_ && opened == &shared_->session_ && shared_->options_.friends) {
    (*opened)->EnableFriends();
    (*opened)->FollowPersonaState(&shared_->persona_state_);
  }
  if (const auto hello = (*opened)->Send(cm::kEMsgClientHello, cm::EncodeHello());
      hello != CmError::kNone) {
    last_failure_.stage = CmStage::kHello;
    Log(std::string("step=hello error=") + CmErrorName(hello));
    return hello;
  }
  const auto logon = (*opened)->Logon(stored, *device_);
  if (logon == CmError::kNone) {
    NoteLogon();
    Log("step=logon result=ok " + fingerprint);
  } else if (logon != CmError::kCanceled) {
    const auto eresult = (*opened)->logon_eresult();
    const auto off = (*opened)->logged_off_eresult();
    last_failure_.stage = CmStage::kLogon;
    last_failure_.eresult = eresult.value_or(off.value_or(0));
    Log(std::string("step=logon error=") + CmErrorName(logon) +
        " eresult=" + std::to_string(eresult.value_or(0)) + " " + fingerprint +
        (off ? " logged_off_eresult=" + std::to_string(*off) : std::string{}));
    // Refused for a reason other than the credentials: Steam is busy or limiting us. Steam may
    // answer the logon with a log-off instead of a logon response.
    const auto refusal = eresult ? eresult : off;
    if (refusal && *refusal != cm::kEResultOk && !cm::CredentialRefusal(*refusal)) {
      NoteRefusal();
      last_failure_.refused = true;
    }
    if (logon == CmError::kRejected) {
      std::lock_guard<std::mutex> lock(g_backoff_mutex);
      g_refused_tokens.insert(fingerprint);
    }
  }
  return logon;
}

TokenRenewal CmClient::RenewAccessToken(const StoredSession& stored,
                                        const std::atomic<bool>& cancel) {
  TokenRenewal renewal;
  const auto deadline = Clock::now() + std::chrono::milliseconds(hooks_.deadline_ms);
  Lease lease;
  renewal.error = Acquire(stored, cancel, deadline, &lease);
  auto& session = lease.session;
  if (renewal.error == CmError::kNone) {
    auth::GenerateAccessTokenForAppRequest request;
    request.refresh_token = stored.refresh_token;
    request.steam_id = stored.steam_id;
    std::vector<std::uint8_t> body;
    std::vector<std::uint8_t> reply;
    if (!auth::Encode(request, &body).ok()) {
      renewal.error = CmError::kProtocol;
    } else {
      renewal.error = session->ServiceMethod("Authentication.GenerateAccessTokenForApp#1", body, &reply);
      // A refused method call is not a refused logon; keep kRejected for the latter.
      if (renewal.error == CmError::kRejected) renewal.error = CmError::kProtocol;
      auth::GenerateAccessTokenForAppResponse decoded;
      if (renewal.error == CmError::kNone) {
        if (!auth::Decode(reply, &decoded).ok() || decoded.access_token.empty()) {
          renewal.error = CmError::kProtocol;
        } else {
          renewal.access_token = std::move(decoded.access_token);
          renewal.refresh_token = std::move(decoded.refresh_token);
        }
      }
    }
  }
  // A rotated refresh token changes the key, so the next call with it logs on again.
  Release(&lease, renewal.error);
  return renewal;
}

DepotAccessResult CmClient::FetchDepotAccess(const StoredSession& stored, std::uint32_t app_id,
                                            const std::vector<DepotRequest>& depots,
                                            const std::atomic<bool>& cancel) {
  DepotAccessResult result;
  if (app_id == 0 || depots.empty() || depots.size() > 64) return result;  // kProtocol
  const auto deadline = Clock::now() + std::chrono::milliseconds(hooks_.deadline_ms);
  Lease lease;
  auto finish = [&](CmError error) {
    Release(&lease, error);
    result.error = error;
    if (error != CmError::kNone) result.grants.clear();
    return result;
  };
  if (const auto open = Acquire(stored, cancel, deadline, &lease); open != CmError::kNone)
    return finish(open);
  auto& session = lease.session;
  // Each depot's key and manifest request code result, with Steam's EResult, for every install.
  for (const auto& depot : depots) {
    // A DLC app's own depot is asked for under that app. A borrowed one Steam refuses under the app that lists
    // it is asked again under the game before it is skipped.
    std::vector<std::uint32_t> ask_under{depot.app_id ? depot.app_id : app_id};
    if (depot.optional && ask_under[0] != app_id) ask_under.push_back(app_id);
    std::string answer;
    const auto answered = [&](std::uint32_t depot_app, const std::string& text) {
      answer += (answer.empty() ? "" : " then ") + std::string("app=") + std::to_string(depot_app) + " " + text;
    };
    const auto settle = [&] { result.answers.emplace_back(depot.depot_id, answer); };
    bool granted = false;
    for (std::size_t attempt = 0; attempt < ask_under.size() && !granted; ++attempt) {
      const std::uint32_t depot_app = ask_under[attempt];
      const bool last = attempt + 1 == ask_under.size();
      auto note = [this, depot_app](const char* step, std::uint32_t depot_id, const std::string& outcome) {
        Log(std::string("step=") + step + " app=" + std::to_string(depot_app) + " depot=" + std::to_string(depot_id) +
            " " + outcome);
      };
      DepotGrant grant;
      grant.depot_id = depot.depot_id;
      grant.manifest_gid = depot.manifest_gid;
      std::vector<std::uint8_t> reply;
      if (const auto call = session->Call(cm::kEMsgClientGetDepotDecryptionKey, {},
                                          cm::EncodeDepotKeyRequest(depot.depot_id, depot_app),
                                          cm::kEMsgClientGetDepotDecryptionKeyResponse, &reply);
          call != CmError::kNone) {
        // Steam's EResult rides only on a refusal; other failures never reached it.
        const auto text = std::string("error=") + CmErrorName(call) +
                          (call == CmError::kRejected ? " eresult=" + std::to_string(session->last_eresult()) : "");
        note("depot_key", depot.depot_id, text);
        answered(depot_app, "key " + text);
        settle();
        return finish(call);
      }
      cm::DepotKeyResponse key;
      if (!cm::DecodeDepotKeyResponse(reply, &key)) {
        note("depot_key", depot.depot_id, "error=protocol");
        answered(depot_app, "key error=protocol");
        settle();
        return finish(CmError::kProtocol);
      }
      if (key.eresult == cm::kEResultAccessDenied && depot.optional) {
        // The last refusal skips the depot (Slay the Spire's 877621 for an account without the app it comes from).
        // Any other result is no answer about the licence: it stops the install, so an update keeps the files.
        note("depot_key", depot.depot_id,
             std::string("result=") + (last ? "skipped" : "refused") + " eresult=" + std::to_string(key.eresult));
        answered(depot_app, "key=refused eresult=" + std::to_string(key.eresult));
        continue;
      }
      if (key.eresult != cm::kEResultOk) {
        note("depot_key", depot.depot_id, "result=refused eresult=" + std::to_string(key.eresult));
        answered(depot_app, "key=refused eresult=" + std::to_string(key.eresult));
        settle();
        result.detail = "depot key refused eresult=" + std::to_string(key.eresult);
        return finish(CmError::kRejected);
      }
      if (key.depot_id != depot.depot_id || key.key.size() != grant.key.size()) {
        note("depot_key", depot.depot_id, "error=protocol");
        answered(depot_app, "key error=protocol");
        settle();
        return finish(CmError::kProtocol);
      }
      note("depot_key", depot.depot_id, "result=ok");
      std::copy(key.key.begin(), key.key.end(), grant.key.begin());
      reply.clear();
      if (const auto call = session->ServiceMethod(
              "ContentServerDirectory.GetManifestRequestCode#1",
              cm::EncodeManifestRequestCodeRequest(depot_app, depot.depot_id, depot.manifest_gid, depot.branch), &reply);
          call != CmError::kNone) {
        const auto eresult = std::to_string(session->last_eresult());
        const bool refused = call == CmError::kRejected;
        const auto text = std::string("error=") + CmErrorName(call) + (refused ? " eresult=" + eresult : "");
        note("manifest_code", depot.depot_id, text);
        answered(depot_app, "key=ok code " + text);
        // A borrowed depot's request code denied is a refusal like its key's.
        if (refused && depot.optional && session->last_eresult() == cm::kEResultAccessDenied) continue;
        settle();
        if (refused) result.detail = "manifest request code refused eresult=" + eresult;
        return finish(call);
      }
      if (!cm::DecodeManifestRequestCodeResponse(reply, &grant.manifest_request_code) ||
          grant.manifest_request_code == 0) {
        note("manifest_code", depot.depot_id, "result=refused code=0");
        answered(depot_app, "key=ok code=0");
        settle();
        result.detail = "manifest request code refused";
        return finish(CmError::kRejected);
      }
      note("manifest_code", depot.depot_id, "result=ok");
      answered(depot_app, "key=ok code=ok");
      result.grants.push_back(grant);
      granted = true;
    }
    if (!granted) {
      answer += " skipped";
      result.skipped.push_back(depot.depot_id);
    }
    settle();
  }
  return finish(CmError::kNone);
}

CmError CmClient::WithServiceSession(const StoredSession& stored, std::uint32_t deadline_ms, const std::atomic<bool>& cancel,
                                     const std::function<CmError(const ServiceCall& call, std::uint64_t steam_id)>& work) {
  const auto deadline = Clock::now() + std::chrono::milliseconds(deadline_ms);
  Lease lease;
  if (const auto open = Acquire(stored, cancel, deadline, &lease); open != CmError::kNone) {
    Release(&lease, open);
    return open;
  }
  auto& session = lease.session;
  const ServiceCall call = [&session](const std::string& method, const std::vector<std::uint8_t>& body,
                                      std::vector<std::uint8_t>* reply, int* eresult) {
    const auto error = session->ServiceMethod(method, body, reply);
    if (eresult) *eresult = session->last_eresult();
    return error;
  };
  const auto result = work(call, session->steam_id());
  Release(&lease, result);
  return result;
}

AppInfoBuffers CmClient::FetchAppInfo(const StoredSession& stored,
                                     const std::vector<std::uint32_t>& app_ids,
                                     const std::atomic<bool>& cancel) {
  return FetchAppInfo(stored, app_ids, cancel, true);
}

AppInfoBuffers CmClient::FetchAppInfoPics(const StoredSession& stored, const std::vector<std::uint32_t>& app_ids,
                                          const std::atomic<bool>& cancel) {
  return FetchAppInfo(stored, app_ids, cancel, false);
}

AppInfoBuffers CmClient::FetchAppInfo(const StoredSession& stored, const std::vector<std::uint32_t>& app_ids,
                                     const std::atomic<bool>& cancel, bool by_hash) {
  AppInfoBuffers result;
  last_failure_ = {};
  if (app_ids.empty() || app_ids.size() > kMaxAppInfoBatch) return result;  // kProtocol
  const auto deadline = Clock::now() + std::chrono::milliseconds(hooks_.deadline_ms);
  Lease lease;
  bool reused = false;
  auto finish = [&](CmError error) {
    Release(&lease, error);
    result.error = error;
    if (error != CmError::kNone) result.buffers.clear();
    return result;
  };
  if (const auto open = Acquire(stored, cancel, deadline, &lease); open != CmError::kNone)
    return finish(open);
  reused = lease.reused;
  auto& session = lease.session;

  std::vector<std::uint8_t> reply;
  if (const auto call = session->Call(cm::kEMsgClientPICSAccessTokenRequest, {},
                                      cm::EncodePicsAccessTokenRequest(app_ids),
                                      cm::kEMsgClientPICSAccessTokenResponse, &reply);
      call != CmError::kNone) {
    // A kept session the server dropped while idle: one fresh try, since this call only reads.
    if (reused && (call == CmError::kNetwork || call == CmError::kTimeout)) {
      Log(std::string("step=app_info at=access_token error=") + CmErrorName(call) + " reused=1 retry=1");
      CloseSession(&lease);
      return FetchAppInfo(stored, app_ids, cancel, by_hash);
    }
    const auto error = call == CmError::kRejected ? CmError::kProtocol : call;
    last_failure_.stage = CmStage::kRequest;
    if (call == CmError::kRejected) last_failure_.eresult = session->last_eresult();
    if (call != CmError::kCanceled)
      Log(std::string("step=app_info at=access_token error=") + CmErrorName(call) + " reused=" + (reused ? "1" : "0") +
          (call == CmError::kRejected ? " eresult=" + std::to_string(session->last_eresult()) : std::string{}));
    return finish(error);
  }
  std::vector<cm::AppRequest> tokens;
  std::vector<std::uint32_t> denied;
  if (!cm::DecodePicsAccessTokenResponse(reply, &tokens, &denied)) {
    last_failure_.stage = CmStage::kRequest;
    Log("step=app_info at=access_token error=protocol decode=0");
    return finish(CmError::kProtocol);
  }
  std::vector<cm::AppRequest> requests;
  for (const auto id : app_ids) {
    cm::AppRequest request{id, 0};
    for (const auto& token : tokens)
      if (token.app_id == id) request.access_token = token.access_token;
    requests.push_back(request);
  }
  std::vector<cm::PicsResponse> parts;
  if (const auto pics = session->PicsBody(cm::EncodePicsAppsRequest(requests), &parts);
      pics != CmError::kNone) {
    last_failure_.stage = CmStage::kRequest;
    if (pics != CmError::kCanceled)
      Log(std::string("step=app_info at=pics error=") + CmErrorName(pics) + " reused=" + (reused ? "1" : "0"));
    return finish(pics);
  }
  const std::set<std::uint32_t> wanted(app_ids.begin(), app_ids.end());
  std::map<std::uint32_t, std::string> why;
  std::map<std::uint32_t, const cm::PicsApp*> offloaded;
  std::uint32_t http_min_size = 0;
  std::string http_host;
  for (const auto& part : parts) {
    if (part.http_min_size) http_min_size = part.http_min_size;
    if (!part.http_host.empty()) http_host = part.http_host;
    for (const auto app : part.unknown_apps)
      if (wanted.count(app)) why.emplace(app, "unknown");
    for (const auto& app : part.apps) {
      if (!wanted.count(app.app_id)) continue;
      if (app.missing_token || app.buffer.empty()) {
        why[app.app_id] = app.missing_token ? "missing_token" : !app.sha.empty() ? "http" : "empty";
        if (!app.missing_token && !app.sha.empty()) offloaded[app.app_id] = &app;
        continue;
      }
      std::string text(app.buffer.begin(), app.buffer.end());
      while (!text.empty() && text.back() == '\0') text.pop_back();
      result.buffers[app.app_id] = std::move(text);
    }
  }
  // The CM is done with: the session goes back before any HTTP.
  finish(CmError::kNone);

  // Steam answers most apps with only the SHA-1 and size of their VDF, fetched by hash from its HTTP host.
  for (const auto& [id, app] : offloaded) result.hashed[id] = HashedAppInfo{app->sha, app->size};
  result.http_host = http_host;
  AppInfoHttpStats http;
  if (by_hash && !result.hashed.empty()) {
    const auto make_perform = [this]() -> AppInfoPerform {
      if (hooks_.app_info) return hooks_.app_info;
      if (ca_bundle_.empty()) return nullptr;
      auto client = std::make_shared<HttpClient>(ca_bundle_);
      return [client](const HttpRequest& request, const std::atomic<bool>& stop) { return client->Perform(request, stop); };
    };
    for (auto& [id, text] : FetchAppInfoByHash(make_perform, http_host, result.hashed, kAppInfoHttpWorkers, cancel, &http)) {
      result.hashed.erase(id);
      result.buffers[id] = std::move(text);
    }
  }

  for (const auto id : app_ids) {
    if (result.buffers.count(id)) continue;
    auto reason = why.count(id) ? why[id] : std::string("absent");
    if (std::find(denied.begin(), denied.end(), id) != denied.end()) reason = "token_denied";
    result.unanswered[id] = std::move(reason);
  }
  if (!offloaded.empty() || !result.unanswered.empty()) {
    // One line per batch Steam didn't answer in full, so the log tells a refused token from an HTTP-only answer.
    std::map<std::string, std::size_t> counts;
    for (const auto& [id, reason] : result.unanswered) ++counts[reason];
    std::string line = "step=app_info requested=" + std::to_string(app_ids.size()) +
                       " answered=" + std::to_string(result.buffers.size()) + " parts=" + std::to_string(parts.size()) +
                       " tokens=" + std::to_string(tokens.size()) + " denied=" + std::to_string(denied.size()) +
                       " offloaded=" + std::to_string(offloaded.size());
    if (by_hash)
      line += " http_fetched=" + std::to_string(http.fetched) + " http_failed=" + std::to_string(http.failed) +
              " http_skipped=" + std::to_string(http.skipped) + " http_connections=" + std::to_string(http.new_connections) +
              " http_ms=" + std::to_string(http.total_ms);
    if (!http.error.empty()) line += " http_error=" + http.error;
    for (const auto& [reason, count] : counts) line += " " + reason + "=" + std::to_string(count);
    if (http_min_size) line += " http_min_size=" + std::to_string(http_min_size);
    if (!http_host.empty()) line += " http_host=" + http_host.substr(0, 64);
    Log(line);
  }
  return result;
}

FreeLicenseResult CmClient::RequestFreeLicenses(const StoredSession& stored, const std::vector<std::uint32_t>& app_ids,
                                                std::uint32_t license_wait_ms, const std::atomic<bool>& cancel) {
  FreeLicenseResult result;
  if (app_ids.empty() || app_ids.size() > kMaxAppInfoBatch) return result;  // kProtocol
  const auto deadline = Clock::now() + std::chrono::milliseconds(hooks_.deadline_ms);
  Lease lease;
  auto finish = [&](CmError error) {
    Release(&lease, error);
    result.error = error;
    return result;
  };
  if (const auto open = Acquire(stored, cancel, deadline, &lease); open != CmError::kNone) return finish(open);
  Session& session = *lease.session;
  // The list from logon first: a grant's list is told apart from it by the serial.
  std::vector<cm::License> licenses;
  if (const auto waited = session.Licenses(&licenses); waited != CmError::kNone) return finish(waited);
  const auto serial = session.license_serial();
  std::vector<std::uint8_t> reply;
  if (const auto call = session.Call(cm::kEMsgClientRequestFreeLicense, {}, cm::EncodeFreeLicenseRequest(app_ids),
                                     cm::kEMsgClientRequestFreeLicenseResponse, &reply);
      call != CmError::kNone) {
    if (call == CmError::kRejected) result.eresult = session.last_eresult();
    Log(std::string("step=free_license error=") + CmErrorName(call) +
        (call == CmError::kRejected ? " eresult=" + std::to_string(result.eresult) : ""));
    return finish(call);
  }
  cm::FreeLicenseResponse response;
  if (!cm::DecodeFreeLicenseResponse(reply, &response)) {
    Log("step=free_license error=protocol");
    return finish(CmError::kProtocol);
  }
  result.eresult = response.eresult;
  result.granted_apps = std::move(response.granted_apps);
  result.granted_packages = std::move(response.granted_packages);
  Log("step=free_license requested=" + std::to_string(app_ids.size()) + " eresult=" + std::to_string(result.eresult) +
      " granted_apps=" + std::to_string(result.granted_apps.size()) +
      " granted_packages=" + std::to_string(result.granted_packages.size()));
  if (result.eresult != cm::kEResultOk) return finish(CmError::kRejected);
  if (!result.granted_apps.empty() || !result.granted_packages.empty()) {
    const auto start = Clock::now();
    const auto waited = session.WaitForLicensesAfter(serial, std::chrono::milliseconds(license_wait_ms));
    result.waited_ms = static_cast<std::uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count());
    result.licenses_updated = waited == CmError::kNone;
    // Running out of time leaves the session fine; anything else ends the call.
    if (waited != CmError::kNone && waited != CmError::kTimeout) return finish(waited);
  }
  return finish(CmError::kNone);
}

AcquisitionResult CmClient::FetchAcquisitionDates(const StoredSession& stored,
                                                  const std::atomic<bool>& cancel) {
  AcquisitionResult result;
  const auto deadline = Clock::now() + std::chrono::milliseconds(hooks_.deadline_ms);
  Lease lease;
  auto finish = [&](CmError error) {
    Release(&lease, error);
    result.error = error;
    if (error != CmError::kNone) {
      result.acquired_at.clear();
      result.owned_apps.clear();
      result.owned_depots.clear();
      result.package_apps.clear();
      result.package_depots.clear();
      result.shared_apps.clear();
    }
    return result;
  };
  if (const auto open = Acquire(stored, cancel, deadline, &lease); open != CmError::kNone)
    return finish(open);
  Session& session = *lease.session;
  std::vector<cm::License> licenses;
  if (const auto waited = session.Licenses(&licenses); waited != CmError::kNone)
    return finish(waited);

  const auto adopted = session.steam_id();
  const auto account_id = static_cast<std::uint32_t>(
      adopted != cm::kAnonymousSteamId ? adopted : stored.steam_id);
  // One license per package: the account's own wins over one borrowed from a family member, whatever
  // the list order, so its token and date are used.
  std::map<std::uint32_t, const cm::License*> chosen;
  for (const auto& license : licenses) {
    if (UnusableLicense(license)) continue;
    auto [entry, inserted] = chosen.emplace(license.package_id, &license);
    if (!inserted && !OwnLicense(*entry->second, account_id) && OwnLicense(license, account_id))
      entry->second = &license;
  }
  LogLicenses([this](const std::string& line) { Log(line); }, licenses, chosen, account_id);
  std::vector<cm::PackageRequest> packages;
  std::set<std::uint32_t> borrowed_packages;
  for (const auto& [id, license] : chosen) {
    packages.push_back({id, license->access_token});
    if (!OwnLicense(*license, account_id)) borrowed_packages.insert(id);
  }

  std::map<std::uint32_t, std::vector<std::uint32_t>> package_apps;
  // What the borrowed packages did in PICS, for the family log.
  std::vector<std::uint32_t> borrowed_missing_token, borrowed_unparsable, borrowed_absent;
  std::set<std::uint32_t> borrowed_apps, borrowed_depots;
  for (std::size_t start = 0; start < packages.size(); start += kPicsBatch) {
    // Each batch gets its own deadline: an account with thousands of licenses needs hundreds of
    // batches, more than one deadline covers (issue #47). A stalled batch still times out.
    if (start > 0) session.Rebind(Clock::now() + std::chrono::milliseconds(hooks_.deadline_ms), cancel);
    const std::vector<cm::PackageRequest> batch(
        packages.begin() + static_cast<std::ptrdiff_t>(start),
        packages.begin() + static_cast<std::ptrdiff_t>(std::min(start + kPicsBatch, packages.size())));
    std::vector<cm::PicsResponse> parts;
    if (const auto pics = session.Pics(batch, &parts); pics != CmError::kNone) return finish(pics);
    std::set<std::uint32_t> wanted;
    for (const auto& package : batch) wanted.insert(package.package_id);
    std::set<std::uint32_t> received, answered;
    for (const auto& part : parts) {
      for (const auto& package : part.packages) {
        if (!wanted.count(package.package_id)) continue;
        const bool borrowed = borrowed_packages.count(package.package_id) > 0;
        // A borrowed package's first answer, for the family log.
        const bool first_answer = borrowed && answered.insert(package.package_id).second;
        if (first_answer && package.missing_token) borrowed_missing_token.push_back(package.package_id);
        if (package.missing_token) continue;
        PackageContents contents;
        // One odd license must not block every install (issue #1): skip it, its apps stay unowned.
        if (!ParsePackageBuffer(package.buffer.data(), package.buffer.size(), package.package_id, &contents)) {
          if (first_answer) borrowed_unparsable.push_back(package.package_id);
          continue;
        }
        auto& apps = package_apps[package.package_id];
        apps.insert(apps.end(), contents.app_ids.begin(), contents.app_ids.end());
        auto& depots = result.package_depots[package.package_id];
        depots.insert(depots.end(), contents.depot_ids.begin(), contents.depot_ids.end());
        result.owned_apps.insert(contents.app_ids.begin(), contents.app_ids.end());
        result.owned_depots.insert(contents.depot_ids.begin(), contents.depot_ids.end());
        // Steam2-era packages list their content depots among the apps (Counter-Strike: Source's 241), as
        // depots were apps then: an ID in either list grants that depot, as DepotDownloader checks. Steam
        // gives apps and depots IDs from one pool, so an app ID never names another app's depot.
        result.owned_depots.insert(contents.app_ids.begin(), contents.app_ids.end());
        if (borrowed) {
          borrowed_apps.insert(contents.app_ids.begin(), contents.app_ids.end());
          borrowed_depots.insert(contents.depot_ids.begin(), contents.depot_ids.end());
        }
        received.insert(package.package_id);
      }
    }
    for (const auto id : wanted)
      if (borrowed_packages.count(id) && !answered.count(id)) borrowed_absent.push_back(id);
    result.skipped_packages += static_cast<std::uint32_t>(wanted.size() - received.size());
  }

  // Apps any of the account's own packages grant are its own, even when a family member shares them too.
  std::set<std::uint32_t> own_apps;
  for (const auto& [id, apps] : package_apps)
    if (!borrowed_packages.count(id)) own_apps.insert(apps.begin(), apps.end());
  for (const auto app : borrowed_apps)
    if (!own_apps.count(app)) result.shared_apps.insert(app);
  {
    std::uint32_t borrowed_received = 0;
    for (const auto& entry : package_apps) borrowed_received += borrowed_packages.count(entry.first) ? 1 : 0;
    Log("family step=pics borrowed_packages=" + std::to_string(borrowed_packages.size()) +
        " received=" + std::to_string(borrowed_received) +
        " missing_token=" + std::to_string(borrowed_missing_token.size()) +
        " unparsable=" + std::to_string(borrowed_unparsable.size()) +
        " absent=" + std::to_string(borrowed_absent.size()) + " apps=" + std::to_string(borrowed_apps.size()) +
        " depots=" + std::to_string(borrowed_depots.size()) + " shared_apps=" +
        std::to_string(result.shared_apps.size()));
    if (!borrowed_missing_token.empty()) Log("family step=pics missing_token_packages=" + IdList(borrowed_missing_token));
    if (!borrowed_unparsable.empty()) Log("family step=pics unparsable_packages=" + IdList(borrowed_unparsable));
    if (!borrowed_absent.empty()) Log("family step=pics absent_packages=" + IdList(borrowed_absent));
  }
  // Nothing usable at all is a reply this client does not understand, not a few odd licenses.
  if (!packages.empty() && result.skipped_packages == packages.size()) return finish(CmError::kProtocol);

  const auto now = hooks_.unix_now
                       ? hooks_.unix_now()
                       : static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::system_clock::now().time_since_epoch()).count());
  for (const auto& [id, license] : chosen) {
    if (license->time_created == 0 || license->time_created > now) continue;
    const auto found = package_apps.find(id);
    if (found == package_apps.end()) continue;
    const bool borrowed = borrowed_packages.count(id) > 0;
    for (const auto app : found->second) {
      // A borrowed license dates only an app the account doesn't own itself.
      if (borrowed && own_apps.count(app)) continue;
      auto [entry, inserted] = result.acquired_at.emplace(app, license->time_created);
      if (!inserted) entry->second = std::min(entry->second, license->time_created);
    }
  }
  result.package_apps = std::move(package_apps);
  return finish(CmError::kNone);
}

AuthResult RenewThroughCm(CmClient& client, const StoredSession& session,
                          const std::atomic<bool>& cancel) {
  auto renewal = client.RenewAccessToken(session, cancel);
  switch (renewal.error) {
    case CmError::kNone: break;
    case CmError::kCanceled: return {AuthError::kCanceled, std::nullopt};
    case CmError::kNoSession:
    case CmError::kRejected: return {AuthError::kCredentialsRevoked, std::nullopt};
    case CmError::kNetwork:
    case CmError::kTimeout: return {AuthError::kNetwork, std::nullopt};
    case CmError::kProtocol: return {AuthError::kProtocol, std::nullopt};
  }
  const auto now = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::system_clock::now().time_since_epoch()).count());
  auto token_ok = [&](const std::string& token) {
    const auto claims = InspectTokenClaims(token);
    return claims && claims->steam_id == session.steam_id && claims->expires_at_unix_seconds > now;
  };
  if (!token_ok(renewal.access_token) || (renewal.refresh_token && !token_ok(*renewal.refresh_token)))
    return {AuthError::kProtocol, std::nullopt};
  auto updated = session;
  updated.access_token = std::move(renewal.access_token);
  if (renewal.refresh_token) updated.refresh_token = std::move(*renewal.refresh_token);
  return {AuthError::kNone, std::move(updated)};
}

}  // namespace playden::steam
