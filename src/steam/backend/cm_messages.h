#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Reduced Steam CM (connection manager) messages: the protobuf frame, hello, logon, heartbeat,
// Multi, license list and package PICS. Field numbers follow the pinned reference .proto files in
// fixtures/steam/proto; requests encode fields in ascending number order, like protoc.
namespace playden::steam::cm {

enum EMsg : std::uint32_t {
  kEMsgMulti = 1,
  kEMsgServiceMethodResponse = 147,
  kEMsgServiceMethodCallFromClient = 151,
  kEMsgClientHeartBeat = 703,
  kEMsgClientLogOff = 706,
  kEMsgClientChangeStatus = 716,
  kEMsgClientLogOnResponse = 751,
  kEMsgClientLoggedOff = 757,
  kEMsgClientPersonaState = 766,
  kEMsgClientFriendsList = 767,
  kEMsgClientLicenseList = 780,
  kEMsgClientRequestFriendData = 815,
  kEMsgClientGetDepotDecryptionKey = 5438,
  kEMsgClientGetDepotDecryptionKeyResponse = 5439,
  kEMsgClientLogon = 5514,
  kEMsgClientRequestFreeLicense = 5572,
  kEMsgClientRequestFreeLicenseResponse = 5573,
  kEMsgClientPICSProductInfoRequest = 8903,
  kEMsgClientPICSProductInfoResponse = 8904,
  kEMsgClientPICSAccessTokenRequest = 8905,
  kEMsgClientPICSAccessTokenResponse = 8906,
  kEMsgClientHello = 9805,
};

enum EResult : std::int32_t {
  kEResultOk = 1,
  kEResultFail = 2,
  kEResultInvalidPassword = 5,
  kEResultAccessDenied = 15,
  kEResultLogonSessionReplaced = 34,
  kEResultTryAnotherCM = 48,
};

// A logon refusal that means the stored credentials are no good (Steam's EResult numbers):
// invalid password, access denied, revoked, expired, logon denied, bad auth code, bad two-factor
// code. Any other refusal (busy, rate limited, try another CM) is about Steam, not the account.
inline bool CredentialRefusal(std::int32_t eresult) {
  switch (eresult) {
    case 5: case 15: case 26: case 27: case 63: case 65: case 88: return true;
    default: return false;
  }
}

inline constexpr std::uint32_t kProtoMask = 0x80000000u;
inline constexpr std::uint32_t kProtocolVersion = 65580;
inline constexpr std::uint32_t kClientPackageVersion = 1771;
// Individual account, desktop instance, public universe, account 0: the pre-logon identity.
inline constexpr std::uint64_t kAnonymousSteamId = 0x0110000100000000ull;
inline constexpr std::size_t kMaxFrameBytes = 8 * 1024 * 1024;
inline constexpr std::size_t kMaxRepeatedEntries = 100'000;

struct Header {
  std::uint64_t steam_id = 0;
  std::int32_t session_id = 0;
  std::optional<std::uint64_t> jobid_source;
  std::optional<std::uint64_t> jobid_target;
  std::string target_job_name;  // unified service method, e.g. "Authentication.X#1"
  std::optional<std::int32_t> eresult;  // absent means success; the proto default is Fail
};

struct Frame {
  std::uint32_t emsg = 0;
  Header header;
  std::vector<std::uint8_t> body;
};

std::vector<std::uint8_t> EncodeFrame(std::uint32_t emsg, const Header& header,
                                      const std::vector<std::uint8_t>& body);
// Accepts only protobuf frames (mask bit set) within kMaxFrameBytes.
bool DecodeFrame(const std::uint8_t* data, std::size_t size, Frame* frame);

struct DeviceIdentity {
  std::uint32_t login_id = 0;  // sent as obfuscated_private_ip; logons sharing it replace each other
  std::string machine_name;
  std::vector<std::uint8_t> machine_id;  // binary KeyValues "MessageObject"
};

std::vector<std::uint8_t> EncodeHello();
std::vector<std::uint8_t> EncodeLogon(const std::string& account_name,
                                      const std::string& refresh_token, std::uint32_t cell_id,
                                      const DeviceIdentity& device);
std::vector<std::uint8_t> EncodeHeartBeat();

struct LogonResponse {
  std::int32_t eresult = kEResultFail;
  std::int32_t heartbeat_seconds = 9;
  std::uint32_t cell_id = 0;
};
bool DecodeLogonResponse(const std::vector<std::uint8_t>& body, LogonResponse* response);
bool DecodeLoggedOff(const std::vector<std::uint8_t>& body, std::int32_t* eresult);

struct Multi {
  std::uint32_t size_unzipped = 0;
  std::vector<std::uint8_t> message_body;
};
bool DecodeMulti(const std::vector<std::uint8_t>& body, Multi* multi);
// Splits a Multi payload of (u32 LE size, frame) entries. A truncated tail fails the whole payload.
bool SplitMultiPayload(const std::vector<std::uint8_t>& payload,
                       std::vector<std::vector<std::uint8_t>>* frames);

struct License {
  std::uint32_t package_id = 0;
  std::uint32_t time_created = 0;
  std::int32_t minute_limit = 0;
  std::int32_t minutes_used = 0;
  std::uint32_t flags = 0;
  std::uint32_t license_type = 0;  // ELicenseType, for diagnostics only
  std::uint32_t owner_id = 0;
  std::uint64_t access_token = 0;
};
struct LicenseList {
  std::optional<std::int32_t> eresult;
  std::vector<License> licenses;
};
bool DecodeLicenseList(const std::vector<std::uint8_t>& body, LicenseList* list);

struct PackageRequest {
  std::uint32_t package_id = 0;
  std::uint64_t access_token = 0;
};
std::vector<std::uint8_t> EncodePicsPackagesRequest(const std::vector<PackageRequest>& packages);

struct PicsPackage {
  std::uint32_t package_id = 0;
  bool missing_token = false;
  std::vector<std::uint8_t> buffer;
};
struct PicsApp {
  std::uint32_t app_id = 0;
  bool missing_token = false;
  bool only_public = false;
  std::vector<std::uint8_t> buffer;  // text VDF, possibly NUL-terminated
  std::vector<std::uint8_t> sha;     // SHA-1 of the VDF; with no buffer, the copy over HTTP is named by it
  std::uint32_t size = 0;
};
struct PicsResponse {
  std::vector<PicsPackage> packages;
  std::vector<PicsApp> apps;
  std::vector<std::uint32_t> unknown_apps;
  bool response_pending = false;
  // Apps whose VDF is at least this big may come without a buffer, to be fetched from http_host instead.
  std::uint32_t http_min_size = 0;
  std::string http_host;
};

struct AppRequest {
  std::uint32_t app_id = 0;
  std::uint64_t access_token = 0;
};
std::vector<std::uint8_t> EncodePicsAppsRequest(const std::vector<AppRequest>& apps);
std::vector<std::uint8_t> EncodePicsAccessTokenRequest(const std::vector<std::uint32_t>& app_ids);
// App ID -> access token, for apps Steam granted tokens to (denied apps are absent, listed in `denied`).
bool DecodePicsAccessTokenResponse(const std::vector<std::uint8_t>& body,
                                   std::vector<AppRequest>* tokens,
                                   std::vector<std::uint32_t>* denied = nullptr);
bool DecodePicsResponse(const std::vector<std::uint8_t>& body, PicsResponse* response);

std::vector<std::uint8_t> EncodeDepotKeyRequest(std::uint32_t depot_id, std::uint32_t app_id);
struct DepotKeyResponse {
  std::int32_t eresult = kEResultFail;
  std::uint32_t depot_id = 0;
  std::vector<std::uint8_t> key;
};
bool DecodeDepotKeyResponse(const std::vector<std::uint8_t>& body, DepotKeyResponse* response);

// ContentServerDirectory.GetManifestRequestCode#1 for a manifest of `branch` (empty: public). No
// branch password is sent: password branches are not offered.
std::vector<std::uint8_t> EncodeManifestRequestCodeRequest(std::uint32_t app_id, std::uint32_t depot_id,
                                                           std::uint64_t manifest_id, std::string_view branch = "public");
bool DecodeManifestRequestCodeResponse(const std::vector<std::uint8_t>& body, std::uint64_t* code);

// CMsgClientRequestFreeLicense: asks Steam to add free apps (free DLC) to the account.
std::vector<std::uint8_t> EncodeFreeLicenseRequest(const std::vector<std::uint32_t>& app_ids);
struct FreeLicenseResponse {
  std::int32_t eresult = kEResultFail;  // the proto default
  std::vector<std::uint32_t> granted_packages;
  std::vector<std::uint32_t> granted_apps;
};
// Accepts the repeated IDs packed or unpacked.
bool DecodeFreeLicenseResponse(const std::vector<std::uint8_t>& body, FreeLicenseResponse* response);

// CMsgClientFriendsList (767), pushed after logon and on every change: incremental = 1, repeated friends = 2
// {ulfriendid = 1 fixed64, efriendrelationship = 2}.
inline constexpr std::uint32_t kFriendRelationshipFriend = 3;
struct FriendRelation {
  std::uint64_t steam_id = 0;
  std::uint32_t relationship = 0;
};
struct FriendsList {
  bool incremental = false;
  std::vector<FriendRelation> friends;
};
bool DecodeFriendsList(const std::vector<std::uint8_t>& body, FriendsList* list);
// An individual account (not a group or clan): the account type and universe bits of its SteamID.
inline bool IndividualSteamId(std::uint64_t steam_id) { return (steam_id >> 52) == 0x011; }

// EClientPersonaStateFlag: the groups a persona state carries.
inline constexpr std::uint32_t kPersonaFlagStatus = 1;
inline constexpr std::uint32_t kPersonaFlagPlayerName = 2;
inline constexpr std::uint32_t kPersonaFlagPresence = 16;
inline constexpr std::uint32_t kPersonaFlagGameExtraInfo = 256;
inline constexpr std::uint32_t kPersonaFlagRichPresence = 4096;
// What Playden asks for: status, name, presence (avatar, game), game extra info and rich presence, as the bridge.
inline constexpr std::uint32_t kFriendDataRequested = kPersonaFlagStatus | kPersonaFlagPlayerName |
                                                      kPersonaFlagPresence | kPersonaFlagGameExtraInfo |
                                                      kPersonaFlagRichPresence;

// CMsgClientPersonaState (766): status_flags = 1, repeated friends = 2. Each field is present only when sent.
struct PersonaFriend {
  std::uint64_t steam_id = 0;                           // 1, fixed64
  std::optional<std::uint32_t> persona_state = {};      // 2
  std::optional<std::uint32_t> game_app_id = {};        // 3, game_played_app_id
  std::optional<std::string> player_name = {};          // 15
  std::optional<std::vector<std::uint8_t>> avatar_hash = {};  // 31, SHA-1 bytes
  std::optional<std::string> game_name = {};            // 55
  std::optional<std::uint64_t> game_id = {};            // 56, fixed64
  std::optional<std::uint64_t> lobby_id = {};           // 73, game_lobby_id, fixed64
  // 71, rich_presence {key = 1, value = 2}; present when any entry came.
  std::optional<std::vector<std::pair<std::string, std::string>>> rich_presence = {};
};
struct PersonaState {
  std::optional<std::uint32_t> status_flags;
  std::vector<PersonaFriend> friends;
};
bool DecodePersonaState(const std::vector<std::uint8_t>& body, PersonaState* state);

// CMsgClientRequestFriendData (815): persona_state_requested = 1, repeated friends = 2 (fixed64, unpacked).
std::vector<std::uint8_t> EncodeRequestFriendData(std::uint32_t requested, const std::vector<std::uint64_t>& steam_ids);

// CMsgClientChangeStatus (716): persona_state = 1 (EPersonaState, 1 = Online, 7 = Invisible).
inline constexpr std::uint32_t kPersonaStateOnline = 1;
inline constexpr std::uint32_t kPersonaStateInvisible = 7;
std::vector<std::uint8_t> EncodeChangeStatus(std::uint32_t persona_state);

}  // namespace playden::steam::cm
