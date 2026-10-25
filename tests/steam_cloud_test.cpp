#include "steam/backend/app_info.h"
#include "steam/backend/cloud_mapping.h"
#include "steam/backend/cloud_messages.h"
#include "steam/backend/cloud_sync.h"
#include "steam/backend/steam_cloud_runner.h"
#include "steam/backend/proto_wire.h"

#include <mbedtls/sha1.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <unistd.h>
#include <vector>
#include "steam_id.h"

using namespace playden;
using namespace playden::steam;
using namespace playden::steam::cloud;

#define CHECK(condition)                                                                  \
  do {                                                                                    \
    if (!(condition)) {                                                                   \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
      std::exit(1);                                                                       \
    }                                                                                     \
  } while (false)

namespace {

constexpr std::uint32_t kApp = 4242;
constexpr std::uint64_t kSteamId = 76561197960287930ULL;

std::vector<std::uint8_t> Bytes(const std::string& text) { return std::vector<std::uint8_t>(text.begin(), text.end()); }

// Steam's transfer paths arrive URL-encoded; the fake encodes names the same way.
std::string Encode(const std::string& text) {
  std::string out;
  for (const char value : text) {
    if (value == ' ' || value == '%') {
      char escaped[4];
      std::snprintf(escaped, sizeof(escaped), "%%%02X", static_cast<unsigned char>(value));
      out += escaped;
    } else {
      out += value;
    }
  }
  return out;
}

std::string Decode(const std::string& text) {
  std::string out;
  for (std::size_t index = 0; index < text.size(); ++index) {
    if (text[index] == '%' && index + 2 < text.size()) {
      out += static_cast<char>(std::strtol(text.substr(index + 1, 2).c_str(), nullptr, 16));
      index += 2;
    } else {
      out += text[index];
    }
  }
  return out;
}

std::vector<std::uint8_t> Sha1(const std::vector<std::uint8_t>& data) {
  std::vector<std::uint8_t> digest(20);
  mbedtls_sha1(data.data(), data.size(), digest.data());
  return digest;
}

std::string ReadText(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

void WriteText(const std::filesystem::path& path, const std::string& text) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << text;
}

// A single stored zip entry, as Steam sends a compressed Cloud file (method 0 keeps the test simple).
std::vector<std::uint8_t> StoredZip(const std::string& name, const std::vector<std::uint8_t>& data) {
  std::vector<std::uint8_t> zip;
  const auto u16 = [&](std::uint16_t value) { zip.push_back(value & 0xff); zip.push_back(value >> 8); };
  const auto u32 = [&](std::uint32_t value) { for (int shift = 0; shift < 32; shift += 8) zip.push_back((value >> shift) & 0xff); };
  std::uint32_t crc = 0xffffffffu;
  for (const auto byte : data) {
    crc ^= byte;
    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
  }
  crc = ~crc;
  u32(0x04034b50); u16(20); u16(0); u16(0); u16(0); u16(0); u32(crc); u32(data.size()); u32(data.size());
  u16(name.size()); u16(0);
  zip.insert(zip.end(), name.begin(), name.end());
  zip.insert(zip.end(), data.begin(), data.end());
  const auto central = zip.size();
  u32(0x02014b50); u16(20); u16(20); u16(0); u16(0); u16(0); u16(0); u32(crc); u32(data.size()); u32(data.size());
  u16(name.size()); u16(0); u16(0); u16(0); u16(0); u32(0); u32(0);
  zip.insert(zip.end(), name.begin(), name.end());
  const auto central_size = zip.size() - central;
  u32(0x06054b50); u16(0); u16(0); u16(1); u16(1); u32(central_size); u32(central); u16(0);
  return zip;
}

const char kAppInfo[] = R"VDF("appinfo"
{
	"appid"		"4242"
	"common" { "name" "Cloud Game" "type" "Game" }
	"ufs"
	{
		"quota"		"1000000"
		"maxnumfiles"		"100"
		"savefiles"
		{
			"0" { "root" "WinMyDocuments" "path" "My Games/Cloud Game" "pattern" "*.sav" }
			"1" { "root" "gameinstall" "path" "cfg\\" "pattern" "config.cfg" }
			"2" { "root" "WinAppDataRoaming" "path" "Cloud Game/{64BitSteamID}" "pattern" "*" "recursive" "1" }
			"3" { "root" "MacAppSupport" "path" "Cloud Game" "pattern" "*" "platforms" { "1" "MacOS" } }
			"4" { "root" "LinuxHome" "path" ".cloudgame" "pattern" "*.sav" }
		}
		"rootoverrides"
		{
			"0" { "root" "LinuxHome" "os" "Windows" "useinstead" "WinSavedGames" "addpath" "Cloud Game\\" }
			"1" { "root" "WinMyDocuments" "os" "MacOS" "useinstead" "MacHome" }
		}
	}
}
)VDF";

// ---- An in-memory Steam Cloud behind fake CM and HTTP seams. ----
struct FakeCloud {
  struct File {
    std::vector<std::uint8_t> data;
    std::uint64_t time = 0;
    bool zipped = false;
  };
  std::map<std::string, File> files;
  std::uint64_t change = 10;
  std::uint64_t steam_id = kSteamId;
  bool declares_ufs = true;
  bool corrupt_downloads = false;
  long download_status = 200;  // another status refuses every download
  int download_transport_code = 0;  // nonzero fails every download in transport with this curl code
  std::uint64_t next_batch = 700;
  std::map<std::string, std::vector<std::uint8_t>> pending;  // uploads in the open batch
  std::vector<std::string> pending_deletes;
  int service_calls = 0;
  std::vector<std::string> methods;

  std::vector<std::uint8_t> Changelist() const {
    proto::Writer body(8 * 1024 * 1024);
    body.AddVarint(1, change);
    body.AddString(4, "%WinMyDocuments%My Games/Cloud Game");
    for (const auto& [name, file] : files) {
      proto::Writer item;
      const std::string prefix = "%WinMyDocuments%My Games/Cloud Game/";
      if (name.rfind(prefix, 0) == 0) {
        item.AddString(1, name.substr(prefix.size()));
        item.AddVarint(7, 0);
      } else {
        item.AddString(1, name);
      }
      item.AddBytes(2, Sha1(file.data));
      item.AddVarint(3, file.time);
      item.AddVarint(4, file.data.size());
      body.AddMessage(2, item);
    }
    return body.bytes();
  }

  CmError Call(const std::string& method, const std::vector<std::uint8_t>& request, std::vector<std::uint8_t>* reply, int* eresult) {
    ++service_calls;
    methods.push_back(method);
    if (eresult) *eresult = 1;
    proto::Reader reader(request);
    proto::Field field;
    std::map<std::uint32_t, std::vector<proto::Field>> fields;
    while (reader.Next(&field)) fields[field.number].push_back(field);
    const auto text = [&](std::uint32_t number) { return fields[number].empty() ? std::string() : fields[number].front().AsString(); };
    const auto number = [&](std::uint32_t id) { return fields[id].empty() ? 0ULL : fields[id].front().value; };
    CHECK(number(1) == kApp || method == kClientCommitFileUpload);
    if (method == kGetAppFileChangelist) {
      *reply = Changelist();
    } else if (method == kClientFileDownload) {
      const auto found = files.find(text(2));
      if (found == files.end()) return CmError::kRejected;
      proto::Writer body;
      const auto wire = found->second.zipped ? StoredZip("f", found->second.data) : found->second.data;
      body.AddVarint(2, wire.size());
      body.AddVarint(3, found->second.data.size());
      body.AddBytes(4, Sha1(found->second.data));
      body.AddVarint(5, found->second.time);
      body.AddString(7, "cloud.test");
      body.AddString(8, "/dl/" + Encode(text(2)));
      body.AddVarint(9, 1);
      proto::Writer header;
      header.AddString(1, "x-test-signature");
      header.AddString(2, "ok");
      body.AddMessage(10, header);
      *reply = body.bytes();
    } else if (method == kBeginAppUploadBatch) {
      pending.clear();
      pending_deletes.clear();
      proto::Writer body;
      body.AddVarint(1, ++next_batch);
      body.AddVarint(4, change + 1);
      *reply = body.bytes();
    } else if (method == kClientBeginFileUpload) {
      CHECK(number(13) == next_batch);
      CHECK(number(2) == number(3));
      proto::Writer body;
      proto::Writer block;
      block.AddString(1, "cloud.test");
      block.AddString(2, "/up/" + Encode(text(6)));
      block.AddVarint(3, 1);
      block.AddVarint(4, 4);  // PUT
      block.AddVarint(6, 0);
      block.AddVarint(7, number(3));
      body.AddMessage(2, block);
      *reply = body.bytes();
    } else if (method == kClientCommitFileUpload) {
      proto::Writer body;
      body.AddVarint(1, number(1) != 0 && pending.contains(text(4)) ? 1 : 0);
      *reply = body.bytes();
    } else if (method == kClientDeleteFile) {
      pending_deletes.push_back(text(2));
      reply->clear();
    } else if (method == kCompleteAppUploadBatchBlocking) {
      if (number(3) == 1) {
        for (auto& [name, data] : pending) files[name] = {data, 2'000, false};
        for (const auto& name : pending_deletes) files.erase(name);
        ++change;
      }
      pending.clear();
      pending_deletes.clear();
      reply->clear();
    } else {
      return CmError::kProtocol;
    }
    return CmError::kNone;
  }

  HttpResponse Http(const HttpRequest& request) {
    HttpResponse response;
    response.status_code = 200;
    const std::string base = "https://cloud.test";
    CHECK(request.url.rfind(base, 0) == 0);
    const auto path = Decode(request.url.substr(base.size()));
    if (path.rfind("/dl/", 0) == 0) {
      CHECK(request.method == HttpMethod::kGet);
      CHECK(request.headers.size() == 1 && request.headers[0].first == "x-test-signature");
      const auto& file = files.at(path.substr(4));
      response.body = file.zipped ? StoredZip("f", file.data) : file.data;
      if (corrupt_downloads && !response.body.empty()) response.body.back() ^= 0xff;
      if (download_status != 200) {
        response.status_code = download_status;
        response.body.clear();
      }
      if (download_transport_code != 0) {
        response.error = HttpError::kProtocolError;
        response.transport_code = download_transport_code;
        response.status_code = 0;
        response.body.clear();
      }
    } else if (path.rfind("/up/", 0) == 0) {
      CHECK(request.method == HttpMethod::kPut);
      pending[path.substr(4)] = request.body;
    } else {
      response.status_code = 404;
    }
    return response;
  }

  CloudHooks Hooks() {
    CloudHooks hooks;
    hooks.session = [this](const StoredSession&, std::uint32_t, const std::atomic<bool>&,
                           const std::function<CmError(const CmClient::ServiceCall&, std::uint64_t)>& work) {
      const CmClient::ServiceCall call = [this](const std::string& method, const std::vector<std::uint8_t>& body,
                                                std::vector<std::uint8_t>* reply, int* eresult) {
        return Call(method, body, reply, eresult);
      };
      return work(call, steam_id);
    };
    hooks.app_info = [this](const StoredSession&, std::uint32_t app, const std::atomic<bool>&, CmError* error) -> std::optional<CloudSaveInfo> {
      *error = CmError::kNone;
      auto info = ParseAppInfo(app, kAppInfo);
      CHECK(info.has_value());
      if (!declares_ufs) info->cloud = {};
      else info->cloud.patterns.pop_back();  // the Linux-rooted location is covered by the mapping test
      return info->cloud;
    };
    hooks.http = [this](const HttpRequest& request, const std::atomic<bool>&) { return Http(request); };
    hooks.now = [] { return std::int64_t{1'800'000'000}; };
    return hooks;
  }
};

void MessagesRoundTrip() {
  // Requests carry the documented field numbers.
  {
    const auto request = EncodeChangelistRequest(kApp, 0);
    proto::Reader reader(request);
    proto::Field field;
    CHECK(reader.Next(&field) && field.number == 1 && field.value == kApp);
    CHECK(reader.Next(&field) && field.number == 2 && field.value == 0);
  }
  // A changelist resolves path prefixes only when an index is present and validates entries.
  proto::Writer body;
  body.AddVarint(1, 77);
  body.AddString(4, "%GameInstall%cfg");
  proto::Writer first;
  first.AddString(1, "config.cfg");
  first.AddBytes(2, Sha1(Bytes("a")));
  first.AddVarint(4, 1);
  first.AddVarint(7, 0);
  body.AddMessage(2, first);
  proto::Writer bare;
  bare.AddString(1, "remote.dat");
  bare.AddBytes(2, Sha1(Bytes("b")));
  bare.AddVarint(4, 1);
  bare.AddVarint(9, 1);
  body.AddMessage(2, bare);
  proto::Writer gone;
  gone.AddString(1, "old.dat");
  gone.AddVarint(5, 2);
  body.AddMessage(2, gone);
  auto list = DecodeChangelistResponse(body.bytes());
  CHECK(list && list->change_number == 77 && list->files.size() == 3);
  CHECK(list->files[0].name == "%GameInstall%cfg/config.cfg");
  CHECK(list->files[1].name == "remote.dat" && list->files[1].reupload_requested);
  CHECK(list->files[2].state == RemoteState::kDeleted);

  proto::Writer delta;
  delta.AddVarint(1, 1);
  delta.AddVarint(3, 1);
  CHECK(!DecodeChangelistResponse(delta.bytes()));  // deltas are refused
  proto::Writer missing;
  missing.AddVarint(1, 1);
  proto::Writer nosha;
  nosha.AddString(1, "x");
  nosha.AddVarint(4, 1);
  missing.AddMessage(2, nosha);
  CHECK(!DecodeChangelistResponse(missing.bytes()));  // a present file needs its SHA-1
  proto::Writer duplicate;
  duplicate.AddVarint(1, 1);
  duplicate.AddMessage(2, first);
  duplicate.AddMessage(2, first);
  duplicate.AddString(4, "p");
  CHECK(!DecodeChangelistResponse(duplicate.bytes()));
  proto::Writer bad_index;
  bad_index.AddVarint(1, 1);
  bad_index.AddMessage(2, first);  // index 0 with no prefixes
  CHECK(!DecodeChangelistResponse(bad_index.bytes()));

  proto::Writer begin;
  begin.AddVarint(1, 55);
  begin.AddVarint(4, 78);
  const auto batch = DecodeBeginBatchResponse(begin.bytes());
  CHECK(batch && batch->batch_id == 55 && batch->app_change_number == 78);
  proto::Writer commit;
  commit.AddVarint(1, 1);
  CHECK(DecodeCommitFileUploadResponse(commit.bytes()) == std::optional<bool>(true));
}

void ParsesUfsAndMaps() {
  const auto info = ParseAppInfo(kApp, kAppInfo);
  CHECK(info && info->cloud.declared && info->cloud.quota == 1'000'000 && info->cloud.max_num_files == 100);
  // The Mac-only location is dropped; the Linux one is overridden to Saved Games for Windows.
  CHECK(info->cloud.patterns.size() == 4);
  const auto& linux_override = info->cloud.patterns[3];
  CHECK(linux_override.root == SavePathRoot::kWinSavedGames && linux_override.path == "Cloud Game/.cloudgame");
  CHECK(linux_override.upload_root == SavePathRoot::kLinuxHome && linux_override.upload_path == ".cloudgame");
  CHECK(info->cloud.patterns[1].upload_path == "cfg/");

  CHECK(GlobMatch("*.sav", "SLOT1.SAV") && GlobMatch("save?.dat", "save7.dat") && !GlobMatch("*.sav", "slot.sav.bak"));
  CHECK(GlobMatch("*", "") && GlobMatch("a*b*c", "aXXbYYc") && !GlobMatch("a*b", "ac"));

  // Without an account the {64BitSteamID} location cannot be mapped; the Linux-rooted upload too.
  const auto unsigned_mapping = BuildSaveMapping(info->cloud, kApp, 0);
  CHECK(!unsigned_mapping.usable());

  auto windows_only = info->cloud;
  windows_only.patterns.pop_back();  // drop the Linux-rooted one
  const auto mapping = BuildSaveMapping(windows_only, kApp, kSteamId);
  CHECK(mapping.usable());
  const SavePaths paths(mapping);
  auto local = paths.LocalFor("%WinMyDocuments%My Games/Cloud Game/slot1.sav");
  CHECK(local && local->root == SaveRoot::kPrefix && local->path == "drive_c/users/prospero/Documents/My Games/Cloud Game/slot1.sav");
  CHECK(paths.LocalFor("%WinMyDocuments%/my games/cloud game/SLOT1.SAV")->key() == local->key());  // case and separator
  CHECK(paths.LocalFor("%SteamCloudDocuments%My Games/Cloud Game/slot1.sav")->key() == local->key());  // alias
  CHECK(!paths.LocalFor("%WinMyDocuments%My Games/Cloud Game/notes.txt"));  // pattern
  CHECK(!paths.LocalFor("%WinMyDocuments%My Games/Cloud Game/sub/slot1.sav"));  // not recursive
  local = paths.LocalFor("%GameInstall%cfg/config.cfg");
  CHECK(local && local->root == SaveRoot::kGame && local->path == "cfg/config.cfg");
  local = paths.LocalFor("%WinAppDataRoaming%Cloud Game/" + std::to_string(kSteamId) + "/deep/state.bin");
  CHECK(local && local->path == "drive_c/users/prospero/AppData/Roaming/Cloud Game/" + std::to_string(kSteamId) + "/deep/state.bin");
  local = paths.LocalFor("remote.dat");
  CHECK(local && local->path == "drive_c/Program Files (x86)/Steam/userdata/0/4242/remote/remote.dat");
  CHECK(!paths.LocalFor("%Unknown%thing.sav"));  // bare-name fallback never takes a %Root% name
  std::string error;
  CHECK(!paths.LocalFor("%WinMyDocuments%../escape.sav", &error) && !error.empty());

  // Local slots map back to the names Steam uses.
  CHECK(paths.RemoteFor({SaveRoot::kPrefix, "drive_c/users/prospero/Documents/My Games/Cloud Game/slot2.sav"}) ==
        std::optional<std::string>("%WinMyDocuments%My Games/Cloud Game/slot2.sav"));
  CHECK(paths.RemoteFor({SaveRoot::kGame, "cfg/config.cfg"}) == std::optional<std::string>("%GameInstall%cfg/config.cfg"));
  CHECK(!paths.RemoteFor({SaveRoot::kGame, "cfg/other.cfg"}));
}

void SyncEndToEnd() {
  const auto base = std::filesystem::temp_directory_path() / ("playden-cloud-" + std::to_string(::getpid()));
  std::filesystem::remove_all(base);
  GameSaveLocation location{kApp, (base / "game").string(), (base / "prefix").string(), (base / "state").string()};
  std::filesystem::create_directories(location.game_root);
  const auto docs = base / "prefix/drive_c/users/prospero/Documents/My Games/Cloud Game";
  const StoredSession session{kSteamId, "account", "refresh", "access", {}};
  const std::atomic<bool> cancel{false};

  FakeCloud cloud;
  cloud.files["%WinMyDocuments%My Games/Cloud Game/slot1.sav"] = {Bytes("cloud slot one"), 1'000, false};
  cloud.files["%GameInstall%cfg/config.cfg"] = {Bytes("volume=5"), 1'100, true};  // arrives zipped
  cloud.files["remote.dat"] = {Bytes("api storage"), 1'200, false};
  CloudSync sync(cloud.Hooks());

  // First sync: everything comes down; downloads need no account confirmation.
  auto plan = sync.Check(session, location, cancel);
  if (plan.status != PlanStatus::kReady)
    std::fprintf(stderr, "status=%d message=%s downloads=%zu uploads=%zu\n", static_cast<int>(plan.status), plan.message.c_str(),
                 plan.downloads, plan.uploads);
  CHECK(plan.status == PlanStatus::kReady && plan.downloads == 3 && plan.uploads == 0 && plan.requires_account_confirmation);
  auto outcome = sync.Apply(session, location, plan, Resolution::kAutomatic, false, cancel);
  if (!outcome.ok) std::fprintf(stderr, "apply: %s downloaded=%zu\n", outcome.message.c_str(), outcome.downloaded);
  CHECK(outcome.ok && outcome.downloaded == 3);
  CHECK(ReadText(docs / "slot1.sav") == "cloud slot one");
  CHECK(ReadText(base / "game/cfg/config.cfg") == "volume=5");
  CHECK(ReadText(base / "prefix/drive_c/Program Files (x86)/Steam/userdata/0/4242/remote/remote.dat") == "api storage");
  CHECK(std::filesystem::exists(base / "state/cloud-baseline.txt"));

  plan = sync.Check(session, location, cancel);
  CHECK(plan.status == PlanStatus::kUpToDate && plan.message == "Saves are up to date");

  // The first upload from this console that would replace Cloud saves asks (#69: a game that missed
  // the downloaded saves writes fresh ones); every upload of that sync is part of the one choice.
  WriteText(docs / "slot1.sav", "local progress");
  WriteText(docs / "slot2.sav", "new slot");
  plan = sync.Check(session, location, cancel);
  CHECK(plan.status == PlanStatus::kConflict && plan.first_overwrite && plan.conflicts == 2 && plan.uploads == 0 &&
        !plan.requires_account_confirmation);
  CHECK(!sync.Apply(session, location, plan, Resolution::kAutomatic, false, cancel).ok);
  CHECK(cloud.files.at("%WinMyDocuments%My Games/Cloud Game/slot1.sav").data == Bytes("cloud slot one"));
  outcome = sync.Apply(session, location, plan, Resolution::kUseLocal, false, cancel);
  CHECK(outcome.ok && outcome.uploaded == 2);
  CHECK(cloud.files.at("%WinMyDocuments%My Games/Cloud Game/slot1.sav").data == Bytes("local progress"));
  CHECK(cloud.files.at("%WinMyDocuments%My Games/Cloud Game/slot2.sav").data == Bytes("new slot"));
  CHECK(cloud.change == 11);
  // The Cloud copy it replaced is kept with the backups; the new slot had none.
  CHECK(ReadText(base / "state/backup/1800000000/cloud/prefix/drive_c/users/prospero/Documents/My Games/Cloud Game/slot1.sav") ==
        "cloud slot one");
  CHECK(!std::filesystem::exists(base / "state/backup/1800000000/cloud/prefix/drive_c/users/prospero/Documents/My Games/Cloud Game/slot2.sav"));
  // The Saves page shows both sides as they are after the upload.
  CHECK(outcome.local_files == outcome.remote_files && outcome.remote_files == 4);
  CHECK(sync.Check(session, location, cancel).status == PlanStatus::kUpToDate);

  // From then on a local change uploads without asking.
  WriteText(docs / "slot1.sav", "more progress");
  plan = sync.Check(session, location, cancel);
  CHECK(plan.status == PlanStatus::kReady && plan.uploads == 1 && !plan.first_overwrite);
  outcome = sync.Apply(session, location, plan, Resolution::kAutomatic, false, cancel);
  CHECK(outcome.ok && outcome.uploaded == 1);
  CHECK(cloud.files.at("%WinMyDocuments%My Games/Cloud Game/slot1.sav").data == Bytes("more progress"));

  // Both sides change: a conflict the player settles; local copies are backed up first.
  WriteText(docs / "slot1.sav", "local again");
  cloud.files["%WinMyDocuments%My Games/Cloud Game/slot1.sav"] = {Bytes("other device"), 3'000, false};
  ++cloud.change;
  plan = sync.Check(session, location, cancel);
  CHECK(plan.status == PlanStatus::kConflict && plan.conflicts == 1);
  CHECK(plan.local_files >= 1 && plan.remote_files >= 1 && plan.remote_latest == 3'000);
  CHECK(!sync.Apply(session, location, plan, Resolution::kAutomatic, false, cancel).ok);
  outcome = sync.Apply(session, location, plan, Resolution::kUseCloud, false, cancel);
  CHECK(outcome.ok && outcome.downloaded == 1);
  CHECK(ReadText(docs / "slot1.sav") == "other device");
  CHECK(ReadText(base / "state/backup/1800000000/prefix/drive_c/users/prospero/Documents/My Games/Cloud Game/slot1.sav") ==
        "local again");

  // Cloud changed between check and apply: nothing is written.
  WriteText(docs / "slot2.sav", "edited");
  plan = sync.Check(session, location, cancel);
  ++cloud.change;
  outcome = sync.Apply(session, location, plan, Resolution::kAutomatic, false, cancel);
  CHECK(!outcome.ok && outcome.message.find("changed") != std::string::npos);
  CHECK(cloud.files.at("%WinMyDocuments%My Games/Cloud Game/slot2.sav").data == Bytes("new slot"));

  // A different Steam account does not trust this baseline: differing files become choices.
  cloud.steam_id = kSteamId + 1;
  const StoredSession other{kSteamId + 1, "other", "refresh", "access", {}};
  plan = sync.Check(other, location, cancel);
  CHECK(plan.requires_account_confirmation && plan.status == PlanStatus::kConflict);
  cloud.steam_id = kSteamId;

  // Saves made here before any sync: uploading them waits for the player to confirm the account.
  {
    GameSaveLocation fresh{kApp, (base / "fresh-game").string(), (base / "fresh-prefix").string(), (base / "fresh-state").string()};
    FakeCloud empty;
    CloudSync first(empty.Hooks());
    WriteText(base / "fresh-prefix/drive_c/users/prospero/Documents/My Games/Cloud Game/mine.sav", "offline progress");
    auto fresh_plan = first.Check(session, fresh, cancel);
    CHECK(fresh_plan.status == PlanStatus::kNeedsAccount && fresh_plan.uploads == 1);
    auto result = first.Apply(session, fresh, fresh_plan, Resolution::kAutomatic, false, cancel);
    CHECK(!result.ok && empty.files.empty());
    result = first.Apply(session, fresh, fresh_plan, Resolution::kAutomatic, true, cancel);
    CHECK(result.ok && result.uploaded == 1 && empty.files.contains("%WinMyDocuments%My Games/Cloud Game/mine.sav"));
    // Once a baseline exists for this account, later uploads need no confirmation.
    WriteText(base / "fresh-prefix/drive_c/users/prospero/Documents/My Games/Cloud Game/mine.sav", "more progress");
    fresh_plan = first.Check(session, fresh, cancel);
    CHECK(fresh_plan.status == PlanStatus::kReady && !fresh_plan.requires_account_confirmation);
  }

  // A damaged download is refused and the local file is left alone.
  cloud.files["%WinMyDocuments%My Games/Cloud Game/slot3.sav"] = {Bytes("third"), 4'000, false};
  ++cloud.change;
  plan = sync.Check(session, location, cancel);
  cloud.corrupt_downloads = true;
  outcome = sync.Apply(session, location, plan, Resolution::kUseCloud, true, cancel);
  CHECK(!outcome.ok && outcome.message.find("damaged") != std::string::npos);
  CHECK(outcome.detail.find("file=%WinMyDocuments%My Games/Cloud Game/slot3.sav") != std::string::npos &&
        outcome.detail.find("sha1_match=0") != std::string::npos);
  CHECK(!std::filesystem::exists(docs / "slot3.sav"));
  cloud.corrupt_downloads = false;

  // A download the server refuses: the player reads a short message, the log gets the file and the HTTP status.
  plan = sync.Check(session, location, cancel);
  cloud.download_status = 403;
  outcome = sync.Apply(session, location, plan, Resolution::kUseCloud, true, cancel);
  CHECK(!outcome.ok && outcome.message == "A Cloud save could not be downloaded. Try again.");
  CHECK(outcome.detail.find("file=%WinMyDocuments%My Games/Cloud Game/slot3.sav http error=") != std::string::npos &&
        outcome.detail.find(" status=403 ") != std::string::npos && outcome.detail.find("x-test-signature") == std::string::npos);
  cloud.download_status = 200;

  // A download that fails in transport: the log names curl's code, which several failures share one error for.
  plan = sync.Check(session, location, cancel);
  cloud.download_transport_code = 35;
  outcome = sync.Apply(session, location, plan, Resolution::kUseCloud, true, cancel);
  CHECK(!outcome.ok && outcome.message == "A Cloud save could not be downloaded. Try again.");
  CHECK(outcome.detail.find(" http error=protocol_error curl=35 status=0 ") != std::string::npos);
  cloud.download_transport_code = 0;

  // A game without Steam Cloud says so.
  cloud.declares_ufs = false;
  plan = sync.Check(session, location, cancel);
  CHECK(plan.status == PlanStatus::kUnsupported && plan.message == "This game does not use Steam Cloud.");

  std::filesystem::remove_all(base);
}

void ManagedPrefixAndPlanBinding() {
  const auto base = std::filesystem::temp_directory_path() /
                    ("playden-cloud-managed-" + std::to_string(getpid()));
  std::filesystem::remove_all(base);
  const CloudLocation legacy{SteamId(kApp), (base / "game").string(), (base / "cloud").string()};
  CHECK(SteamCloudRunner::SaveLocation(legacy).prefix_root == legacy.state_root + "/prefix");
  CloudLocation managed = legacy;
  managed.prefix_root = (base / "library/prefixes/game").string();
  const auto mapping = SteamCloudRunner::SaveLocation(managed);
  CHECK(mapping.prefix_root == managed.prefix_root);
  CHECK(mapping.state_root == managed.state_root);
  const std::string relative = "/drive_c/users/prospero/Documents/My Games/Cloud Game/mine.sav";
  WriteText(managed.prefix_root + relative, "managed progress");
  FakeCloud remote;
  SteamCloudRunner runner(remote.Hooks());
  const StoredSession session{kSteamId, "user", "refresh", "access", {}};
  std::atomic<bool> cancel{false};
  // A game without its bottle yet (not set up): a check is fine, a transfer is refused, so nothing
  // lands in a folder the game never reads (#72).
  remote.files["%WinMyDocuments%My Games/Cloud Game/cloud.sav"] = {Bytes("cloud progress"), 1'000, false};
  auto report = runner.Check(session, legacy, cancel);
  CHECK(report.downloads == 1);
  report = runner.Apply(session, legacy, CloudResolution::kUseCloud, true, cancel);
  CHECK(report.state == CloudState::kFailed && report.message.find("Press Play") != std::string::npos);
  CHECK(!std::filesystem::exists(base / "cloud/prefix/drive_c/users/prospero/Documents/My Games/Cloud Game/cloud.sav"));
  remote.files.clear();
  report = runner.Check(session, managed, cancel);
  CHECK(report.uploads == 1);
  CloudLocation replacement = managed;
  replacement.prefix_root = (base / "other-bottle").string();
  std::filesystem::copy(managed.prefix_root, replacement.prefix_root,
                        std::filesystem::copy_options::recursive);
  report = runner.Apply(session, replacement, CloudResolution::kUseLocal, true, cancel);
  CHECK(report.state == CloudState::kFailed);
  CHECK(remote.files.empty());
  // A refused location switch invalidates the stale plan, including when the
  // old location is supplied again. Only a new check authorizes a transfer.
  report = runner.Apply(session, managed, CloudResolution::kUseLocal, true, cancel);
  CHECK(report.state == CloudState::kFailed);
  runner.Check(session, managed, cancel);
  report = runner.Apply(session, managed, CloudResolution::kUseLocal, true, cancel);
  CHECK(report.state == CloudState::kSynced);
  CHECK(remote.files.at("%WinMyDocuments%My Games/Cloud Game/mine.sav").data == Bytes("managed progress"));
  CHECK(!std::filesystem::exists(base / "cloud/prefix"));
  std::filesystem::remove_all(base);
}

void QuickCheckAndIndex() {
  const auto base = std::filesystem::temp_directory_path() / ("playden-cloud-quick-" + std::to_string(::getpid()));
  std::filesystem::remove_all(base);
  GameSaveLocation location{kApp, (base / "game").string(), (base / "prefix").string(), (base / "state").string()};
  std::filesystem::create_directories(location.game_root);
  const auto docs = base / "prefix/drive_c/users/prospero/Documents/My Games/Cloud Game";
  const StoredSession session{kSteamId, "account", "refresh", "access", {}};
  const std::atomic<bool> cancel{false};
  FakeCloud cloud;
  cloud.files["%WinMyDocuments%My Games/Cloud Game/slot1.sav"] = {Bytes("same"), 1'000, false};
  CloudSync sync(cloud.Hooks());

  // No index yet: nothing to compare with.
  CHECK(sync.QuickCheck(session, {location}, cancel) == std::vector<Drift>{Drift::kUnknown});

  // Already in step before any sync: the up-to-date check records a baseline, so a later local
  // change uploads without asking to confirm the account.
  WriteText(docs / "slot1.sav", "same");
  auto plan = sync.Check(session, location, cancel);
  CHECK(plan.status == PlanStatus::kUpToDate);
  CHECK(std::filesystem::exists(base / "state/cloud-baseline.txt") && std::filesystem::exists(base / "state/cloud-index.txt"));
  CHECK(sync.QuickCheck(session, {location}, cancel) == std::vector<Drift>{Drift::kUnchanged});

  // Local change (new size), remote change (new change number), both.
  WriteText(docs / "slot1.sav", "local progress");
  CHECK(sync.QuickCheck(session, {location}, cancel) == std::vector<Drift>{Drift::kLocal});
  ++cloud.change;
  CHECK(sync.QuickCheck(session, {location}, cancel) == std::vector<Drift>{Drift::kBoth});
  --cloud.change;
  plan = sync.Check(session, location, cancel);
  // This console never uploaded for the game: the first replacement of a Cloud save is asked.
  CHECK(plan.status == PlanStatus::kConflict && plan.first_overwrite && plan.conflicts == 1 && !plan.requires_account_confirmation);
  std::vector<std::pair<std::uint64_t, std::uint64_t>> steps;
  auto outcome = sync.Apply(session, location, plan, Resolution::kUseLocal, false, cancel,
                            [&](std::uint64_t done, std::uint64_t total) { steps.emplace_back(done, total); });
  CHECK(outcome.ok && outcome.uploaded == 1);
  CHECK(steps.size() == 2 && steps.front().first == 0 && steps.back().first == 14 && steps.back().second == 14);
  // The Apply refreshed the index: in step again.
  CHECK(sync.QuickCheck(session, {location}, cancel) == std::vector<Drift>{Drift::kUnchanged});
  cloud.files["%WinMyDocuments%My Games/Cloud Game/slot2.sav"] = {Bytes("other device"), 3'000, false};
  ++cloud.change;
  // A new file in a save folder is a local change too; a game without an index stays unknown.
  WriteText(docs / "slot3.sav", "new here");
  GameSaveLocation fresh{kApp, (base / "fresh-game").string(), (base / "fresh-prefix").string(), (base / "fresh-state").string()};
  CHECK((sync.QuickCheck(session, {location, fresh}, cancel) == std::vector<Drift>{Drift::kBoth, Drift::kUnknown}));
  std::filesystem::remove(docs / "slot3.sav");
  CHECK(sync.QuickCheck(session, {location}, cancel) == std::vector<Drift>{Drift::kRemote});
  // Another signed-in account: the index is not this account's.
  cloud.steam_id = kSteamId + 1;
  const StoredSession other{kSteamId + 1, "other", "refresh", "access", {}};
  CHECK(sync.QuickCheck(other, {location}, cancel) == std::vector<Drift>{Drift::kUnknown});
  cloud.steam_id = kSteamId;

  // Deleting the local saves backs them up first and leaves Cloud alone.
  std::string message;
  CHECK(sync.RemoveLocal(location, &message) && message == "1 save deleted");
  CHECK(!std::filesystem::exists(docs / "slot1.sav"));
  CHECK(ReadText(base / "state/backup/1800000000/prefix/drive_c/users/prospero/Documents/My Games/Cloud Game/slot1.sav") ==
        "local progress");
  CHECK(cloud.files.count("%WinMyDocuments%My Games/Cloud Game/slot1.sav") == 1);
  CHECK(!sync.RemoveLocal(fresh, &message));
  std::filesystem::remove_all(base);
}

void MissingGameFolder() {
  // An uninstalled game keeps its bottle but not its own folder: Cloud files that belong in the game
  // folder are never downloaded, uploaded or deleted.
  const auto base = std::filesystem::temp_directory_path() / ("playden-cloud-kept-" + std::to_string(::getpid()));
  std::filesystem::remove_all(base);
  GameSaveLocation kept{kApp, "", (base / "prefix").string(), (base / "state").string()};
  const auto docs = base / "prefix/drive_c/users/prospero/Documents/My Games/Cloud Game";
  WriteText(docs / "slot1.sav", "kept slot");
  const StoredSession session{kSteamId, "account", "refresh", "access", {}};
  const std::atomic<bool> cancel{false};
  FakeCloud cloud;
  cloud.files["%WinMyDocuments%My Games/Cloud Game/slot1.sav"] = {Bytes("kept slot"), 1'000, false};
  cloud.files["%GameInstall%cfg/config.cfg"] = {Bytes("volume=5"), 1'100, false};
  CloudSync sync(cloud.Hooks());
  auto plan = sync.Check(session, kept, cancel);
  CHECK(plan.status == PlanStatus::kUpToDate && plan.downloads == 0 && plan.uploads == 0 && plan.remote_files == 1);
  for (const auto& decision : plan.decisions)
    if (decision.remote_name.rfind("%GameInstall%", 0) == 0) CHECK(decision.action == Action::kUnavailable);
  const auto outcome = sync.Apply(session, kept, plan, Resolution::kUseLocal, true, cancel);
  CHECK(outcome.ok && outcome.uploaded == 0 && outcome.removed_remote == 0);
  CHECK(cloud.files.count("%GameInstall%cfg/config.cfg") == 1 && !std::filesystem::exists("/cfg/config.cfg"));
  CHECK(sync.QuickCheck(session, {kept}, cancel) == std::vector<Drift>{Drift::kUnchanged});
  // Reinstalled: the game folder is back, so the index no longer describes this location.
  GameSaveLocation reinstalled = kept;
  reinstalled.game_root = (base / "game").string();
  std::filesystem::create_directories(reinstalled.game_root);
  CHECK(sync.QuickCheck(session, {reinstalled}, cancel) == std::vector<Drift>{Drift::kUnknown});
  std::filesystem::remove_all(base);
}

}  // namespace

int main() {
  ManagedPrefixAndPlanBinding();
  MessagesRoundTrip();
  ParsesUfsAndMaps();
  SyncEndToEnd();
  QuickCheckAndIndex();
  MissingGameFolder();
  std::puts("steam cloud tests passed");
}
