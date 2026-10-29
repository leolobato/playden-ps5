#include "steam/backend/binary_kv.h"
#include "steam/backend/cm_client.h"
#include "steam/backend/cm_messages.h"
#include "steam/backend/proto_wire.h"

#include <cassert>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace playden::steam;

namespace {

std::string g_root;

std::vector<std::uint8_t> Fixture(const std::string& name) {
  std::ifstream in(g_root + "/" + name, std::ios::binary);
  assert(in && "missing fixture");
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// Matches fixtures/steam/proto/generate_cm_fixtures.py.
constexpr const char* kSeed = "6F1C2B8A-3D4E-4F50-9A6B-7C8D9E0F1A2B";

void RequestsMatchProtocBytes() {
  assert(cm::EncodeHello() == Fixture("hello_request.pb"));
  const auto device = MakeDeviceIdentity(kSeed);
  assert(device.machine_name == "Playden" && device.login_id != 0);
  assert(cm::EncodeLogon("synthetic_user", "synthetic.refresh.token", 0, device) ==
         Fixture("logon_request.pb"));
  assert(cm::EncodePicsPackagesRequest({{100, 7001}, {200, 0}}) == Fixture("pics_request.pb"));
  assert(cm::EncodeHeartBeat().empty());
}

void ResponsesDecode() {
  cm::LogonResponse logon;
  assert(cm::DecodeLogonResponse(Fixture("logon_response.pb"), &logon));
  assert(logon.eresult == cm::kEResultOk && logon.heartbeat_seconds == 12 && logon.cell_id == 7);

  cm::LicenseList list;
  assert(cm::DecodeLicenseList(Fixture("license_list.pb"), &list));
  assert(list.eresult == cm::kEResultOk && list.licenses.size() == 5);
  assert(list.licenses[0].package_id == 100 && list.licenses[0].time_created == 1600000000 &&
         list.licenses[0].owner_id == 4242 && list.licenses[0].access_token == 7001);
  assert(list.licenses[2].flags == 8);
  assert(list.licenses[4].minute_limit == 60 && list.licenses[4].minutes_used == 60);

  cm::PicsResponse part;
  assert(cm::DecodePicsResponse(Fixture("pics_response_part1.pb"), &part));
  assert(part.response_pending && part.packages.size() == 1 && part.packages[0].package_id == 100);
  PackageContents contents;
  assert(ParsePackageBuffer(part.packages[0].buffer.data(), part.packages[0].buffer.size(), 100,
                            &contents));
  assert((contents.app_ids == std::vector<std::uint32_t>{10, 20}));
  assert(cm::DecodePicsResponse(Fixture("pics_response_part2.pb"), &part) && !part.response_pending);
}

void LogonResponseFallsBackToLegacyAndDefaultHeartbeat() {
  // eresult 5 only: no heartbeat fields -> 9 seconds.
  cm::LogonResponse logon;
  assert(cm::DecodeLogonResponse({0x08, 0x05}, &logon));
  assert(logon.eresult == cm::kEResultInvalidPassword && logon.heartbeat_seconds == 9);
  // legacy 20 only.
  assert(cm::DecodeLogonResponse({0x08, 0x01, 0x10, 0x14}, &logon) && logon.heartbeat_seconds == 20);
  // eresult with the wrong wire type fails.
  assert(!cm::DecodeLogonResponse({0x0d, 1, 0, 0, 0}, &logon));
}

void FramesRoundTripAndRejectMalformedInput() {
  cm::Header header;
  header.steam_id = cm::kAnonymousSteamId;
  header.session_id = -3;
  header.jobid_source = 9;
  const auto frame = cm::EncodeFrame(cm::kEMsgClientHello, header, {1, 2, 3});
  cm::Frame decoded;
  assert(cm::DecodeFrame(frame.data(), frame.size(), &decoded));
  assert(decoded.emsg == cm::kEMsgClientHello && decoded.header.steam_id == cm::kAnonymousSteamId);
  assert(decoded.header.session_id == -3 && decoded.header.jobid_source == 9u);
  assert(!decoded.header.jobid_target && !decoded.header.eresult);
  assert((decoded.body == std::vector<std::uint8_t>{1, 2, 3}));

  auto plain = frame;
  plain[3] &= 0x7f;  // clear the protobuf flag
  assert(!cm::DecodeFrame(plain.data(), plain.size(), &decoded));
  assert(!cm::DecodeFrame(frame.data(), 7, &decoded));
  auto long_header = frame;
  long_header[4] = 0xff;
  assert(!cm::DecodeFrame(long_header.data(), long_header.size(), &decoded));

  const auto server = Fixture("server_pics_part1.bin");
  assert(cm::DecodeFrame(server.data(), server.size(), &decoded));
  assert(decoded.emsg == cm::kEMsgClientPICSProductInfoResponse && decoded.header.jobid_target == 1u);
  assert(decoded.header.steam_id == 76561197960269970ull && decoded.header.session_id == 77);
}

// A borrowed license carries the lender's account ID, the Borrowed flag and a license type.
void LicenseOwnerFlagsAndTypeDecode() {
  proto::Writer license;
  license.AddVarint(1, 77);
  license.AddFixed32(2, 1700000000);
  license.AddVarint(7, 0x4000);
  license.AddVarint(9, 10);
  license.AddVarint(12, 9999);
  license.AddVarint(17, 31337);
  proto::Writer body;
  body.AddVarint(1, cm::kEResultOk);
  body.AddMessage(2, license);
  cm::LicenseList list;
  assert(cm::DecodeLicenseList(body.bytes(), &list) && list.licenses.size() == 1);
  const auto& decoded = list.licenses[0];
  assert(decoded.package_id == 77 && decoded.time_created == 1700000000 && decoded.flags == 0x4000);
  assert(decoded.license_type == 10 && decoded.owner_id == 9999 && decoded.access_token == 31337);
  // A license type that is not a varint is malformed.
  proto::Writer bad_entry;
  bad_entry.AddVarint(1, 77);
  bad_entry.AddFixed32(9, 10);
  proto::Writer bad;
  bad.AddMessage(2, bad_entry);
  assert(!cm::DecodeLicenseList(bad.bytes(), &list));
}

void TruncatedMessagesFail() {
  auto licenses = Fixture("license_list.pb");
  licenses.resize(licenses.size() - 3);
  cm::LicenseList list;
  assert(!cm::DecodeLicenseList(licenses, &list));

  std::vector<std::vector<std::uint8_t>> frames;
  assert(cm::SplitMultiPayload({2, 0, 0, 0, 7, 8, 1, 0, 0, 0, 9}, &frames) && frames.size() == 2);
  assert(!cm::SplitMultiPayload({2, 0, 0, 0, 7}, &frames));
  assert(!cm::SplitMultiPayload({1, 0, 0}, &frames));
}

// CMsgClientRequestFreeLicense (5572) and its response (5573): field numbers from SteamKit2's
// steammessages_clientserver.proto. Proto2 sends the repeated IDs unpacked; packed ones decode too.
void FreeLicenseRequestAndResponse() {
  assert(cm::kEMsgClientRequestFreeLicense == 5572 && cm::kEMsgClientRequestFreeLicenseResponse == 5573);
  // appids = 2, unpacked varints: 0x10 <id>.
  assert(cm::EncodeFreeLicenseRequest({5001, 7}) ==
         (std::vector<std::uint8_t>{0x10, 0x89, 0x27, 0x10, 0x07}));
  assert(cm::EncodeFreeLicenseRequest({}).empty());

  playden::steam::proto::Writer unpacked;
  unpacked.AddVarint(1, cm::kEResultOk);
  unpacked.AddVarint(2, 900);
  unpacked.AddVarint(3, 5001);
  unpacked.AddVarint(3, 5002);
  cm::FreeLicenseResponse response;
  assert(cm::DecodeFreeLicenseResponse(unpacked.bytes(), &response));
  assert(response.eresult == cm::kEResultOk && response.granted_packages == std::vector<std::uint32_t>{900} &&
         (response.granted_apps == std::vector<std::uint32_t>{5001, 5002}));

  playden::steam::proto::Writer packed;
  packed.AddBytes(3, std::vector<std::uint8_t>{0x89, 0x27, 0x07});
  assert(cm::DecodeFreeLicenseResponse(packed.bytes(), &response));
  // eresult absent: the proto default, Fail.
  assert(response.eresult == cm::kEResultFail && response.granted_packages.empty() &&
         (response.granted_apps == std::vector<std::uint32_t>{5001, 7}));

  // A truncated packed run or a wrong wire type fails.
  playden::steam::proto::Writer truncated;
  truncated.AddBytes(3, std::vector<std::uint8_t>{0x89});
  assert(!cm::DecodeFreeLicenseResponse(truncated.bytes(), &response));
  playden::steam::proto::Writer wrong;
  wrong.AddString(1, "x");
  assert(!cm::DecodeFreeLicenseResponse(wrong.bytes(), &response));
}

// CContentServerDirectory_GetManifestRequestCode_Request: app_id = 1, depot_id = 2, manifest_id = 3,
// app_branch = 4 (branch_password_hash = 5 is never sent).
void ManifestRequestCodeNamesTheBranch() {
  const std::vector<std::uint8_t> head{0x08, 0xB8, 0x03, 0x10, 0xB9, 0x03, 0x18, 0x0B};
  auto with = [&](const std::string& branch) {
    auto bytes = head;
    bytes.push_back(0x22);
    bytes.push_back(static_cast<std::uint8_t>(branch.size()));
    bytes.insert(bytes.end(), branch.begin(), branch.end());
    return bytes;
  };
  assert(cm::EncodeManifestRequestCodeRequest(440, 441, 11) == with("public"));
  assert(cm::EncodeManifestRequestCodeRequest(440, 441, 11, "") == with("public"));
  assert(cm::EncodeManifestRequestCodeRequest(440, 441, 11, "4.04") == with("4.04"));
}

}  // namespace

int main(int argc, char** argv) {
  assert(argc == 2);
  g_root = argv[1];
  RequestsMatchProtocBytes();
  ResponsesDecode();
  LogonResponseFallsBackToLegacyAndDefaultHeartbeat();
  FramesRoundTripAndRejectMalformedInput();
  LicenseOwnerFlagsAndTypeDecode();
  TruncatedMessagesFail();
  FreeLicenseRequestAndResponse();
  ManifestRequestCodeNamesTheBranch();
}
