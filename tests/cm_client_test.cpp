#include "steam/backend/cm_client.h"
#include "steam/backend/proto_wire.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

using namespace playden;
using namespace playden::steam;
using namespace std::chrono_literals;

namespace {

std::string g_root;
constexpr const char* kSeed = "6F1C2B8A-3D4E-4F50-9A6B-7C8D9E0F1A2B";
constexpr std::uint64_t kAccountSteamId = 76561197960269970ull;  // account 4242

std::vector<std::uint8_t> Fixture(const std::string& name) {
  std::ifstream in(g_root + "/" + name, std::ios::binary);
  assert(in && "missing fixture");
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::vector<std::uint8_t> ServerFrame(std::uint32_t emsg, const std::vector<std::uint8_t>& body,
                                      std::optional<std::int32_t> eresult = std::nullopt,
                                      std::optional<std::uint64_t> jobid_target = std::nullopt) {
  cm::Header header;
  header.steam_id = kAccountSteamId;
  header.session_id = 77;
  header.eresult = eresult;
  header.jobid_target = jobid_target;
  return cm::EncodeFrame(emsg, header, body);
}

std::vector<std::uint8_t> LogonResponseBody(std::int32_t eresult, std::int32_t heartbeat) {
  proto::Writer body;
  body.AddVarint(1, static_cast<std::uint32_t>(eresult));
  body.AddVarint(3, static_cast<std::uint32_t>(heartbeat));
  return body.bytes();
}

struct Script {
  std::mutex mutex;
  int failing_connects = 0;
  int connects = 0;
  bool closed = false;
  std::vector<cm::Frame> sent;
  std::deque<std::pair<std::chrono::steady_clock::time_point, std::vector<std::uint8_t>>> inbox;
  // What the fake server answers.
  std::vector<std::vector<std::uint8_t>> on_logon;
  std::vector<std::vector<std::uint8_t>> on_pics;
  std::vector<std::vector<std::uint8_t>> on_service;
  std::map<std::uint32_t, std::vector<std::vector<std::uint8_t>>> on_emsg;
  std::vector<std::vector<std::uint8_t>> raw_sent;
  std::chrono::milliseconds pics_delay{0};
  // When set, answers each PICS request instead of on_pics.
  std::function<std::vector<std::uint8_t>(const cm::Frame&)> pics_reply;

  void Queue(std::vector<std::uint8_t> frame, std::chrono::milliseconds delay = 0ms) {
    inbox.emplace_back(std::chrono::steady_clock::now() + delay, std::move(frame));
  }
};

class FakeTransport final : public MessageTransport {
 public:
  explicit FakeTransport(std::shared_ptr<Script> script) : script_(std::move(script)) {}
  HttpError Connect(const std::string& url, std::uint32_t, const std::atomic<bool>&) override {
    std::lock_guard<std::mutex> lock(script_->mutex);
    assert(url.rfind("wss://", 0) == 0 && url.find("/cmsocket/") != std::string::npos);
    ++script_->connects;
    if (script_->failing_connects-- > 0) return HttpError::kConnectionFailed;
    return HttpError::kNone;
  }
  HttpError Send(const std::vector<std::uint8_t>& message, std::uint32_t,
                 const std::atomic<bool>&) override {
    std::lock_guard<std::mutex> lock(script_->mutex);
    cm::Frame frame;
    assert(cm::DecodeFrame(message.data(), message.size(), &frame));
    if (frame.emsg == cm::kEMsgClientLogon)
      for (const auto& reply : script_->on_logon) script_->Queue(reply);
    if (frame.emsg == cm::kEMsgClientPICSProductInfoRequest && script_->pics_reply)
      script_->Queue(script_->pics_reply(frame), script_->pics_delay);
    else if (frame.emsg == cm::kEMsgClientPICSProductInfoRequest)
      for (const auto& reply : script_->on_pics) script_->Queue(reply, script_->pics_delay);
    if (frame.emsg == cm::kEMsgServiceMethodCallFromClient)
      for (const auto& reply : script_->on_service) script_->Queue(reply);
    if (const auto found = script_->on_emsg.find(frame.emsg); found != script_->on_emsg.end())
      for (const auto& reply : found->second) script_->Queue(reply);
    script_->raw_sent.push_back(message);
    script_->sent.push_back(std::move(frame));
    return HttpError::kNone;
  }
  HttpError Receive(std::vector<std::uint8_t>* message, std::uint32_t wait_ms,
                    const std::atomic<bool>& cancel) override {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
    for (;;) {
      if (cancel) return HttpError::kCanceled;
      {
        std::lock_guard<std::mutex> lock(script_->mutex);
        if (!script_->inbox.empty() &&
            script_->inbox.front().first <= std::chrono::steady_clock::now()) {
          *message = std::move(script_->inbox.front().second);
          script_->inbox.pop_front();
          return HttpError::kNone;
        }
      }
      if (std::chrono::steady_clock::now() >= deadline) return HttpError::kTimeout;
      std::this_thread::sleep_for(1ms);
    }
  }
  void Close() override {
    std::lock_guard<std::mutex> lock(script_->mutex);
    script_->closed = true;
  }

 private:
  std::shared_ptr<Script> script_;
};

HttpResponse Directory(const std::string& json) {
  HttpResponse response;
  response.status_code = 200;
  response.body.assign(json.begin(), json.end());
  return response;
}

constexpr const char* kTwoEndpoints =
    R"({"response":{"serverlist":[{"endpoint":"cm1.example.invalid:27022"},)"
    R"({"endpoint":"bad/endpoint?x"},{"endpoint":"cm2.example.invalid:443"}],"success":true}})";

CmClient MakeClient(std::shared_ptr<Script> script, std::uint32_t deadline_ms = 3000,
                    std::string directory = kTwoEndpoints) {
  CmClientHooks hooks;
  hooks.directory = [directory](const HttpRequest& request, const std::atomic<bool>&) {
    assert(request.url.find("ISteamDirectory/GetCMListForConnect") != std::string::npos);
    return Directory(directory);
  };
  hooks.transport = [script] { return std::make_unique<FakeTransport>(script); };
  hooks.unix_now = [] { return std::uint64_t{1700000000}; };
  hooks.deadline_ms = deadline_ms;
  return CmClient(hooks, MakeDeviceIdentity(kSeed));
}

StoredSession Session() {
  StoredSession session;
  session.steam_id = kAccountSteamId;
  session.account_name = "synthetic_user";
  session.refresh_token = "synthetic.refresh.token";
  return session;
}

std::shared_ptr<Script> HappyScript() {
  auto script = std::make_shared<Script>();
  script->on_logon = {Fixture("server_multi_logon.bin")};
  script->on_pics = {Fixture("server_pics_part1.bin"), Fixture("server_pics_part2.bin")};
  return script;
}

void AcquisitionDatesFromGzipMultiLicensesAndMultipartPics() {
  auto script = HappyScript();
  script->failing_connects = 1;  // the first endpoint fails; the next valid one is used
  auto client = MakeClient(script);
  std::atomic<bool> cancel{false};
  const auto result = client.FetchAcquisitionDates(Session(), cancel);
  assert(result.ok());
  // Package 100 (1600000000) holds apps 10, 20; package 200 (1500000000) holds 20, 30. Packages
  // 300 (expired) and 500 (minutes used up) are left out. Package 400 is another owner's, shared with
  // this account, but Steam doesn't describe it.
  assert(result.acquired_at.size() == 3 && result.skipped_packages == 1 && result.shared_apps.empty());
  assert(result.acquired_at.at(10) == 1600000000u);
  assert(result.acquired_at.at(20) == 1500000000u);
  assert(result.acquired_at.at(30) == 1500000000u);

  std::lock_guard<std::mutex> lock(script->mutex);
  assert(script->connects == 2 && script->closed);
  assert(script->sent.size() == 3);  // closed without a log-off
  assert(script->sent[0].emsg == cm::kEMsgClientHello);
  assert(script->sent[0].header.steam_id == cm::kAnonymousSteamId);
  assert(script->sent[1].emsg == cm::kEMsgClientLogon);
  assert(script->sent[1].body == Fixture("logon_request.pb"));
  assert(script->sent[1].header.session_id == 0);
  // After logon the frame header carries the adopted session identity and the job ID.
  assert(script->sent[2].emsg == cm::kEMsgClientPICSProductInfoRequest);
  assert(script->sent[2].body == cm::EncodePicsPackagesRequest({{100, 7001}, {200, 0}, {400, 0}}));
  assert(script->sent[2].header.steam_id == kAccountSteamId);
  assert(script->sent[2].header.session_id == 77);
  assert(script->sent[2].header.jobid_source == 1u);
}

void RejectedLogonIsReportedWithoutDates() {
  auto script = std::make_shared<Script>();
  script->on_logon = {ServerFrame(cm::kEMsgClientLogOnResponse,
                                  LogonResponseBody(cm::kEResultInvalidPassword, 9))};
  auto client = MakeClient(script);
  std::atomic<bool> cancel{false};
  const auto result = client.FetchAcquisitionDates(Session(), cancel);
  assert(result.error == CmError::kRejected && result.acquired_at.empty());
}

void FailedLicenseListIsRejected() {
  auto script = std::make_shared<Script>();
  proto::Writer licenses;
  licenses.AddVarint(1, cm::kEResultFail);
  script->on_logon = {ServerFrame(cm::kEMsgClientLogOnResponse, LogonResponseBody(1, 9)),
                      ServerFrame(cm::kEMsgClientLicenseList, licenses.bytes())};
  auto client = MakeClient(script);
  std::atomic<bool> cancel{false};
  assert(client.FetchAcquisitionDates(Session(), cancel).error == CmError::kRejected);
}

// One odd license must not block every install (issue #1): a package Steam leaves out or sends without its token is
// skipped, and only its apps stay unowned.
void PackagesSteamCannotDescribeAreSkipped() {
  std::atomic<bool> cancel{false};
  // Package 100 is left out; the reply holds package 200 only, not pending.
  auto left_out = HappyScript();
  left_out->on_pics = {ServerFrame(cm::kEMsgClientPICSProductInfoResponse, Fixture("pics_response_part2.pb"), std::nullopt, 1)};
  auto client = MakeClient(left_out);
  const auto partial = client.FetchAcquisitionDates(Session(), cancel);
  assert(partial.ok() && partial.skipped_packages == 2);  // and the shared package 400
  assert(partial.owned_apps == (std::set<std::uint32_t>{20, 30}));
  assert(partial.acquired_at.size() == 2 && partial.acquired_at.at(20) == 1500000000u);

  // Package 100 comes back marked as missing its token, with no buffer.
  auto tokenless = HappyScript();
  auto body = Fixture("pics_response_part2.pb");
  proto::Writer entry;
  entry.AddVarint(1, 100);
  entry.AddVarint(3, 1);
  proto::Writer missing;
  missing.AddMessage(3, entry);
  body.insert(body.end(), missing.bytes().begin(), missing.bytes().end());
  tokenless->on_pics = {ServerFrame(cm::kEMsgClientPICSProductInfoResponse, body, std::nullopt, 1)};
  auto other = MakeClient(tokenless);
  const auto skipped = other.FetchAcquisitionDates(Session(), cancel);
  assert(skipped.ok() && skipped.skipped_packages == 2 && skipped.owned_apps == (std::set<std::uint32_t>{20, 30}));

  // A reply with none of the packages is still a protocol error.
  auto empty = HappyScript();
  empty->on_pics = {ServerFrame(cm::kEMsgClientPICSProductInfoResponse, {}, std::nullopt, 1)};
  auto third = MakeClient(empty);
  const auto none = third.FetchAcquisitionDates(Session(), cancel);
  assert(none.error == CmError::kProtocol && none.owned_apps.empty());
}

void JobErrorResultFailsTheBatch() {
  auto script = HappyScript();
  script->on_pics = {ServerFrame(cm::kEMsgClientPICSProductInfoResponse, {}, cm::kEResultFail, 1)};
  auto client = MakeClient(script);
  std::atomic<bool> cancel{false};
  assert(client.FetchAcquisitionDates(Session(), cancel).error == CmError::kRejected);
}

void SilentServerTimesOut() {
  auto script = std::make_shared<Script>();
  auto client = MakeClient(script, 150);
  std::atomic<bool> cancel{false};
  const auto start = std::chrono::steady_clock::now();
  assert(client.FetchAcquisitionDates(Session(), cancel).error == CmError::kTimeout);
  assert(std::chrono::steady_clock::now() - start < 1s);
}

// Thousands of licenses take hundreds of PICS batches (issue #47): each batch has its own deadline, so
// batches that together take longer than one deadline still finish.
void EveryPackageBatchGetsItsOwnDeadline() {
  auto script = std::make_shared<Script>();
  // Package 200 plus 100 more that Steam leaves out: three batches of at most 50.
  proto::Writer licenses;
  proto::Writer owned;
  owned.AddVarint(1, 200);
  owned.AddFixed32(2, 1500000000);
  licenses.AddMessage(2, owned);
  for (std::uint32_t id = 1000; id < 1100; ++id) {
    proto::Writer license;
    license.AddVarint(1, id);
    licenses.AddMessage(2, license);
  }
  script->on_logon = {ServerFrame(cm::kEMsgClientLogOnResponse, LogonResponseBody(1, 9)),
                      ServerFrame(cm::kEMsgClientLicenseList, licenses.bytes())};
  script->pics_reply = [](const cm::Frame& request) {
    return ServerFrame(cm::kEMsgClientPICSProductInfoResponse, Fixture("pics_response_part2.pb"), std::nullopt,
                       request.header.jobid_source);
  };
  script->pics_delay = 200ms;
  auto client = MakeClient(script, 500);
  std::atomic<bool> cancel{false};
  const auto start = std::chrono::steady_clock::now();
  const auto result = client.FetchAcquisitionDates(Session(), cancel);
  assert(std::chrono::steady_clock::now() - start > 500ms);
  assert(result.ok() && result.skipped_packages == 100);
  assert(result.owned_apps == (std::set<std::uint32_t>{20, 30}));
  std::lock_guard<std::mutex> lock(script->mutex);
  assert(std::count_if(script->sent.begin(), script->sent.end(), [](const cm::Frame& frame) {
           return frame.emsg == cm::kEMsgClientPICSProductInfoRequest;
         }) == 3);
}

// Package 0 is the base license on older accounts and holds Valve's games (issue #58): its apps and depots count.
std::vector<std::uint8_t> PackageBuffer(std::uint32_t package_id, const std::vector<std::uint32_t>& apps,
                                        const std::vector<std::uint32_t>& depots) {
  std::vector<std::uint8_t> kv = {1, 0, 0, 0};
  auto text = [&](const std::string& value) {
    kv.insert(kv.end(), value.begin(), value.end());
    kv.push_back(0);
  };
  auto section = [&](const std::string& name) {
    kv.push_back(0);
    text(name);
  };
  auto ids = [&](const char* name, const std::vector<std::uint32_t>& values) {
    section(name);
    for (std::size_t index = 0; index < values.size(); ++index) {
      kv.push_back(2);
      text(std::to_string(index));
      for (int shift = 0; shift < 32; shift += 8) kv.push_back(static_cast<std::uint8_t>(values[index] >> shift));
    }
    kv.push_back(8);
  };
  section(std::to_string(package_id));
  ids("appids", apps);
  ids("depotids", depots);
  kv.push_back(8);  // ends the package section
  kv.push_back(8);  // and the root
  return kv;
}

void PackageZeroCounts() {
  auto script = std::make_shared<Script>();
  proto::Writer licenses;
  licenses.AddVarint(1, 1);
  for (const auto& [id, created] :
       {std::pair<std::uint32_t, std::uint32_t>{0, 1240000000}, {8538, 1300000000}, {7, 1100000000}}) {
    proto::Writer license;
    license.AddVarint(1, id);
    license.AddFixed32(2, created);
    license.AddVarint(7, 0x200);
    licenses.AddMessage(2, license);
  }
  script->on_logon = {ServerFrame(cm::kEMsgClientLogOnResponse, LogonResponseBody(1, 9)),
                      ServerFrame(cm::kEMsgClientLicenseList, licenses.bytes())};
  script->pics_reply = [](const cm::Frame& request) {
    proto::Writer body;
    for (const auto& [id, apps, depots] :
         {std::tuple<std::uint32_t, std::vector<std::uint32_t>, std::vector<std::uint32_t>>{0, {440, 220}, {441, 232251}},
          {8538, {440}, {232251, 232253}},
          // A Steam2-era package lists Counter-Strike: Source's content depot 241 among its apps.
          {7, {240, 241}, {209, 232331}}}) {
      proto::Writer package;
      package.AddVarint(1, id);
      package.AddBytes(5, PackageBuffer(id, apps, depots));
      body.AddMessage(3, package);
    }
    return ServerFrame(cm::kEMsgClientPICSProductInfoResponse, body.bytes(), std::nullopt, request.header.jobid_source);
  };
  auto client = MakeClient(script);
  std::atomic<bool> cancel{false};
  const auto result = client.FetchAcquisitionDates(Session(), cancel);
  assert(result.ok() && result.skipped_packages == 0);
  assert(result.owned_apps == (std::set<std::uint32_t>{220, 240, 241, 440}));
  // An ID among a package's apps grants that depot too, as DepotDownloader checks.
  assert(result.owned_depots == (std::set<std::uint32_t>{209, 220, 240, 241, 440, 441, 232251, 232253, 232331}));
  assert(result.acquired_at.at(440) == 1240000000u && result.acquired_at.at(220) == 1240000000u);
}

// A license in a synthetic list; owner 4242 is the signed-in account.
struct TestLicense {
  std::uint32_t package_id = 0;
  std::uint32_t created = 0;
  std::uint32_t flags = 0;
  std::uint32_t owner_id = 4242;
  std::uint64_t token = 0;
  std::int32_t minute_limit = 0;
  std::int32_t minutes_used = 0;
};

using PackageContentsMap = std::map<std::uint32_t, std::pair<std::vector<std::uint32_t>, std::vector<std::uint32_t>>>;

// A script whose logon pushes `licenses` and whose PICS answers every package in `contents` it was asked for.
std::shared_ptr<Script> LicenseScript(const std::vector<TestLicense>& licenses, PackageContentsMap contents) {
  auto script = std::make_shared<Script>();
  proto::Writer list;
  list.AddVarint(1, 1);
  for (const auto& entry : licenses) {
    proto::Writer license;
    license.AddVarint(1, entry.package_id);
    if (entry.created) license.AddFixed32(2, entry.created);
    if (entry.minute_limit) license.AddVarint(4, static_cast<std::uint32_t>(entry.minute_limit));
    if (entry.minutes_used) license.AddVarint(5, static_cast<std::uint32_t>(entry.minutes_used));
    if (entry.flags) license.AddVarint(7, entry.flags);
    license.AddVarint(12, entry.owner_id);
    if (entry.token) license.AddVarint(17, entry.token);
    list.AddMessage(2, license);
  }
  script->on_logon = {ServerFrame(cm::kEMsgClientLogOnResponse, LogonResponseBody(1, 9)),
                      ServerFrame(cm::kEMsgClientLicenseList, list.bytes())};
  script->pics_reply = [contents](const cm::Frame& request) {
    proto::Writer body;
    for (const auto& [id, granted] : contents) {
      proto::Writer package;
      package.AddVarint(1, id);
      package.AddBytes(5, PackageBuffer(id, granted.first, granted.second));
      body.AddMessage(3, package);
    }
    return ServerFrame(cm::kEMsgClientPICSProductInfoResponse, body.bytes(), std::nullopt, request.header.jobid_source);
  };
  return script;
}

std::vector<std::uint8_t> PicsRequestSent(Script& script) {
  std::lock_guard<std::mutex> lock(script.mutex);
  for (const auto& frame : script.sent)
    if (frame.emsg == cm::kEMsgClientPICSProductInfoRequest) return frame.body;
  return {};
}

struct LogCapture {
  std::mutex mutex;
  std::vector<std::string> lines;
  LogCapture() {
    SetCmLog([this](const std::string& line) {
      std::lock_guard<std::mutex> lock(mutex);
      lines.push_back(line);
    });
  }
  ~LogCapture() { SetCmLog(nullptr); }
  std::vector<std::string> Take() {
    std::lock_guard<std::mutex> lock(mutex);
    auto taken = std::move(lines);
    lines.clear();
    return taken;
  }
};

bool HasLine(const std::vector<std::string>& lines, const std::string& line) {
  return std::find(lines.begin(), lines.end(), line) != lines.end();
}

std::size_t CountPrefix(const std::vector<std::string>& lines, const std::string& prefix) {
  return static_cast<std::size_t>(std::count_if(lines.begin(), lines.end(), [&](const std::string& line) {
    return line.rfind(prefix, 0) == 0;
  }));
}

// Steam Families: a license another member shares (another owner, the Borrowed flag) counts, and its apps
// that no own license grants are shared. Every other unavailable flag and used-up minutes still drop it.
void BorrowedLicensesAreShared() {
  LogCapture log;
  auto script = LicenseScript(
      {{100, 1600000000, 0, 4242, 7001},
       {600, 1550000000, 0x4000, 9999, 6601},
       {700, 1500000000, 0x4000 | 0x8, 9999, 7701},       // expired
       {800, 1500000000, 0x4000, 8888, 8801, 60, 60},     // minutes used up
       {900, 1450000000, 0x2000, 4242}},                  // pending refund
      {{100, {{10, 20}, {101}}}, {600, {{20, 40}, {201, 401}}}});
  auto client = MakeClient(script);
  std::atomic<bool> cancel{false};
  const auto result = client.FetchAcquisitionDates(Session(), cancel);
  assert(result.ok() && result.skipped_packages == 0);
  assert(PicsRequestSent(*script) == cm::EncodePicsPackagesRequest({{100, 7001}, {600, 6601}}));
  assert(result.owned_apps == (std::set<std::uint32_t>{10, 20, 40}));
  // App IDs count as depots too (Steam2-era packages), so the apps are among them.
  assert(result.owned_depots == (std::set<std::uint32_t>{10, 20, 40, 101, 201, 401}));
  assert(result.shared_apps == (std::set<std::uint32_t>{40}));
  // App 20 is the account's own: the borrowed license's earlier date doesn't apply to it.
  assert(result.acquired_at.at(10) == 1600000000u && result.acquired_at.at(20) == 1600000000u);
  assert(result.acquired_at.at(40) == 1550000000u);
  assert(result.package_apps.at(600) == (std::vector<std::uint32_t>{20, 40}));

  const auto lines = log.Take();
  assert(HasLine(lines, "owner=cm family step=licenses total=5 own=1 borrowed=1 dropped=3 owners=8888:1,9999:2 "
                        "flags=0x8:1,0x2000:1,0x4000:3 changed=y"));
  assert(HasLine(lines, "owner=cm family step=license package=600 owner=9999 flags=0x4000 type=0 created=1550000000 "
                        "token=y result=kept reason=borrowed"));
  assert(HasLine(lines, "owner=cm family step=license package=700 owner=9999 flags=0x4008 type=0 created=1500000000 "
                        "token=y result=dropped reason=flags:0x8"));
  assert(HasLine(lines, "owner=cm family step=license package=800 owner=8888 flags=0x4000 type=0 created=1500000000 "
                        "token=y result=dropped reason=minutes"));
  assert(HasLine(lines, "owner=cm family step=license package=900 owner=4242 flags=0x2000 type=0 created=1450000000 "
                        "token=n result=dropped reason=flags:0x2000"));
  assert(CountPrefix(lines, "owner=cm family step=license ") == 4);  // package 100 is plainly the account's own
  assert(HasLine(lines, "owner=cm family step=pics borrowed_packages=1 received=1 missing_token=0 unparsable=0 "
                        "absent=0 apps=2 depots=2 shared_apps=1"));
}

// The same package through the account's own license and a borrowed one: the own license's token and date
// count, and its apps are not shared, whichever comes first in the list.
void OwnLicenseWinsOverBorrowedInEitherOrder() {
  const TestLicense own{100, 1600000000, 0, 4242, 7001};
  for (const bool borrowed_first : {false, true}) {
    LogCapture log;
    const TestLicense borrowed{100, 1400000000 + (borrowed_first ? 1u : 0u), 0x4000, 9999, 8001};
    auto script = LicenseScript(borrowed_first ? std::vector<TestLicense>{borrowed, own}
                                               : std::vector<TestLicense>{own, borrowed},
                                {{100, {{10}, {101}}}});
    auto client = MakeClient(script);
    std::atomic<bool> cancel{false};
    const auto result = client.FetchAcquisitionDates(Session(), cancel);
    assert(result.ok());
    assert(PicsRequestSent(*script) == cm::EncodePicsPackagesRequest({{100, 7001}}));
    assert(result.owned_apps == (std::set<std::uint32_t>{10}) && result.shared_apps.empty());
    assert(result.acquired_at.at(10) == 1600000000u);
    const auto lines = log.Take();
    assert(HasLine(lines, "owner=cm family step=license package=100 owner=9999 flags=0x4000 type=0 created=" +
                              std::to_string(borrowed.created) + " token=y result=superseded reason=own"));
    assert(HasLine(lines, "owner=cm family step=pics borrowed_packages=0 received=0 missing_token=0 unparsable=0 "
                          "absent=0 apps=0 depots=0 shared_apps=0"));
  }
}

// Borrowed packages PICS doesn't describe are named, and their apps stay unowned.
void BorrowedPackagesPicsSkipsAreLogged() {
  LogCapture log;
  auto script = LicenseScript({{100, 1600000000, 0, 4242, 7001},
                               {610, 1550000000, 0x4000, 9999, 0},
                               {620, 1550000000, 0x4000, 9999, 0},
                               {630, 1550000000, 0x4000, 9999, 0}},
                              {{100, {{10}, {101}}}});
  script->pics_reply = [](const cm::Frame& request) {
    proto::Writer body;
    proto::Writer own;
    own.AddVarint(1, 100);
    own.AddBytes(5, PackageBuffer(100, {10}, {101}));
    body.AddMessage(3, own);
    proto::Writer tokenless;  // 610: missing its token
    tokenless.AddVarint(1, 610);
    tokenless.AddVarint(3, 1);
    body.AddMessage(3, tokenless);
    proto::Writer garbled;  // 620: unreadable buffer; 630 left out
    garbled.AddVarint(1, 620);
    garbled.AddBytes(5, std::vector<std::uint8_t>{1, 2, 3});
    body.AddMessage(3, garbled);
    return ServerFrame(cm::kEMsgClientPICSProductInfoResponse, body.bytes(), std::nullopt, request.header.jobid_source);
  };
  auto client = MakeClient(script);
  std::atomic<bool> cancel{false};
  const auto result = client.FetchAcquisitionDates(Session(), cancel);
  assert(result.ok() && result.skipped_packages == 3 && result.shared_apps.empty());
  assert(result.owned_apps == (std::set<std::uint32_t>{10}));
  const auto lines = log.Take();
  assert(HasLine(lines, "owner=cm family step=pics borrowed_packages=3 received=0 missing_token=1 unparsable=1 "
                        "absent=1 apps=0 depots=0 shared_apps=0"));
  assert(HasLine(lines, "owner=cm family step=pics missing_token_packages=610"));
  assert(HasLine(lines, "owner=cm family step=pics unparsable_packages=620"));
  assert(HasLine(lines, "owner=cm family step=pics absent_packages=630"));
}

// The per-license lines are written once per distinct list, so library refreshes don't rotate the log away,
// and at most 300 at a time.
void FamilyLicenseLinesAreCappedAndOnlyOnChange() {
  std::vector<TestLicense> licenses;
  for (std::uint32_t id = 5000; id < 5305; ++id) licenses.push_back({id, 1500000000, 0x80, 0});
  auto script = LicenseScript(licenses, {{5000, {{50}, {51}}}});
  std::atomic<bool> cancel{false};
  LogCapture log;
  {
    auto client = MakeClient(script);
    assert(client.FetchAcquisitionDates(Session(), cancel).ok());
  }
  auto lines = log.Take();
  assert(HasLine(lines, "owner=cm family step=licenses total=305 own=305 borrowed=0 dropped=0 owners=none "
                        "flags=0x80:305 changed=y"));
  assert(CountPrefix(lines, "owner=cm family step=license package=") == 300);
  assert(HasLine(lines, "owner=cm family step=license truncated=5"));
  // The same list again: only the summary.
  {
    auto client = MakeClient(script);
    assert(client.FetchAcquisitionDates(Session(), cancel).ok());
  }
  lines = log.Take();
  assert(HasLine(lines, "owner=cm family step=licenses total=305 own=305 borrowed=0 dropped=0 owners=none "
                        "flags=0x80:305 changed=n"));
  assert(CountPrefix(lines, "owner=cm family step=license ") == 0);
  // A changed list is written again, a shared license first even past the cap.
  licenses.push_back({5400, 1500000000, 0x4000, 9999, 1});
  auto changed = LicenseScript(licenses, {{5000, {{50}, {51}}}});
  {
    auto client = MakeClient(changed);
    assert(client.FetchAcquisitionDates(Session(), cancel).ok());
  }
  lines = log.Take();
  assert(CountPrefix(lines, "owner=cm family step=license package=") == 300);
  assert(HasLine(lines, "owner=cm family step=license package=5400 owner=9999 flags=0x4000 type=0 created=1500000000 "
                        "token=y result=kept reason=borrowed"));
  assert(HasLine(lines, "owner=cm family step=license truncated=6"));
}

void CancelStopsWaiting() {
  auto script = std::make_shared<Script>();
  auto client = MakeClient(script, 5000);
  std::atomic<bool> cancel{false};
  std::thread canceler([&cancel] {
    std::this_thread::sleep_for(50ms);
    cancel = true;
  });
  const auto start = std::chrono::steady_clock::now();
  assert(client.FetchAcquisitionDates(Session(), cancel).error == CmError::kCanceled);
  assert(std::chrono::steady_clock::now() - start < 1s);
  canceler.join();
}

void HeartbeatIsSentOnTheServerInterval() {
  auto script = HappyScript();
  // Logon with a one-second heartbeat; the license push arrives only after it.
  script->on_logon = {ServerFrame(cm::kEMsgClientLogOnResponse, LogonResponseBody(1, 1))};
  auto client = MakeClient(script, 4000);
  std::thread pusher([script] {
    std::this_thread::sleep_for(1300ms);
    std::lock_guard<std::mutex> lock(script->mutex);
    auto licenses = Fixture("license_list.pb");
    script->Queue(ServerFrame(cm::kEMsgClientLicenseList, licenses));
  });
  std::atomic<bool> cancel{false};
  const auto result = client.FetchAcquisitionDates(Session(), cancel);
  pusher.join();
  assert(result.ok());
  std::lock_guard<std::mutex> lock(script->mutex);
  bool heartbeat = false;
  for (const auto& frame : script->sent) heartbeat |= frame.emsg == cm::kEMsgClientHeartBeat;
  assert(heartbeat);
}

void MissingSessionOrEndpointsFailEarly() {
  auto script = HappyScript();
  auto client = MakeClient(script);
  std::atomic<bool> cancel{false};
  StoredSession empty;
  assert(client.FetchAcquisitionDates(empty, cancel).error == CmError::kNoSession);
  auto no_endpoints = MakeClient(script, 3000, R"({"response":{"serverlist":[]}})");
  assert(no_endpoints.FetchAcquisitionDates(Session(), cancel).error == CmError::kProtocol);
  std::lock_guard<std::mutex> lock(script->mutex);
  assert(script->connects == 0);
}

void AccessTokenRenewsThroughAuthenticatedServiceMethod() {
  auto script = std::make_shared<Script>();
  script->on_logon = {ServerFrame(cm::kEMsgClientLogOnResponse, LogonResponseBody(1, 9))};
  proto::Writer reply;
  reply.AddString(1, "renewed.access.token");
  script->on_service = {ServerFrame(cm::kEMsgServiceMethodResponse, reply.bytes(), std::nullopt, 1)};
  auto client = MakeClient(script);
  std::atomic<bool> cancel{false};
  const auto renewal = client.RenewAccessToken(Session(), cancel);
  assert(renewal.ok() && renewal.access_token == "renewed.access.token" && !renewal.refresh_token);
  std::lock_guard<std::mutex> lock(script->mutex);
  // The session closes after the call without a log-off, which would sign the device out.
  assert(script->closed && script->sent.back().emsg == cm::kEMsgServiceMethodCallFromClient);
  const auto& call = script->sent.back();
  assert(call.emsg == cm::kEMsgServiceMethodCallFromClient);
  assert(call.header.jobid_source == 1u && call.header.session_id == 77);
  const auto& raw_call = script->raw_sent.back();
  const std::string raw(raw_call.begin(), raw_call.end());
  assert(raw.find("Authentication.GenerateAccessTokenForApp#1") != std::string::npos);
  assert(raw.find("synthetic.refresh.token") != std::string::npos);
}

void RefusedServiceCallIsNotARefusedLogon() {
  auto script = std::make_shared<Script>();
  script->on_logon = {ServerFrame(cm::kEMsgClientLogOnResponse, LogonResponseBody(1, 9))};
  script->on_service = {ServerFrame(cm::kEMsgServiceMethodResponse, {}, cm::kEResultAccessDenied, 1)};
  auto client = MakeClient(script);
  std::atomic<bool> cancel{false};
  assert(client.RenewAccessToken(Session(), cancel).error == CmError::kProtocol);
  auto refused = std::make_shared<Script>();
  refused->on_logon = {ServerFrame(cm::kEMsgClientLogOnResponse, LogonResponseBody(cm::kEResultInvalidPassword, 9))};
  auto refused_client = MakeClient(refused);
  assert(refused_client.RenewAccessToken(Session(), cancel).error == CmError::kRejected);
}

int CountSent(const std::shared_ptr<Script>& script, std::uint32_t emsg) {
  std::lock_guard<std::mutex> lock(script->mutex);
  return static_cast<int>(std::count_if(script->sent.begin(), script->sent.end(),
                                        [emsg](const cm::Frame& frame) { return frame.emsg == emsg; }));
}

CmError Ping(CmClient& client, const std::atomic<bool>& cancel) {
  return client.WithServiceSession(Session(), 3000, cancel, [](const CmClient::ServiceCall& call, std::uint64_t) {
    std::vector<std::uint8_t> reply;
    return call("Cloud.Ping#1", {}, &reply, nullptr);
  });
}

void SharedSessionServesEveryClientWithOneLogon() {
  auto script = std::make_shared<Script>();
  script->on_logon = {ServerFrame(cm::kEMsgClientLogOnResponse, LogonResponseBody(1, 1))};
  {
    auto shared = MakeCmSharedSession();
    auto cloud = MakeClient(script);
    auto details = MakeClient(script);
    cloud.SetSharedSession(shared);
    details.SetSharedSession(shared);
    std::atomic<bool> cancel{false};
    for (std::uint64_t job = 1; job <= 4; ++job) {
      {
        std::lock_guard<std::mutex> lock(script->mutex);
        script->on_service = {ServerFrame(cm::kEMsgServiceMethodResponse, {}, std::nullopt, job)};
      }
      assert(Ping(job % 2 ? cloud : details, cancel) == CmError::kNone);
    }
    // Idle for longer than the server's heartbeat interval: the session is kept alive meanwhile.
    std::this_thread::sleep_for(1600ms);
    assert(CountSent(script, cm::kEMsgClientHeartBeat) >= 1);
    {
      std::lock_guard<std::mutex> lock(script->mutex);
      script->on_service = {ServerFrame(cm::kEMsgServiceMethodResponse, {}, std::nullopt, 5)};
    }
    assert(Ping(cloud, cancel) == CmError::kNone);
    assert(CountSent(script, cm::kEMsgClientLogon) == 1);
    std::lock_guard<std::mutex> lock(script->mutex);
    assert(script->connects == 1 && !script->closed);
  }
  // The last owner gone, the shared session closes without a log-off.
  assert(CountSent(script, cm::kEMsgClientLogOff) == 0);
  std::lock_guard<std::mutex> lock(script->mutex);
  assert(script->closed);
}

void SharedSessionLogsOnAgainForNewCredentials() {
  auto script = std::make_shared<Script>();
  script->on_logon = {ServerFrame(cm::kEMsgClientLogOnResponse, LogonResponseBody(1, 9))};
  script->on_service = {ServerFrame(cm::kEMsgServiceMethodResponse, {}, std::nullopt, 1)};
  auto shared = MakeCmSharedSession();
  auto client = MakeClient(script);
  client.SetSharedSession(shared);
  std::atomic<bool> cancel{false};
  assert(Ping(client, cancel) == CmError::kNone);
  auto rotated = Session();
  rotated.refresh_token = "synthetic.rotated.token";
  const auto result = client.WithServiceSession(rotated, 3000, cancel, [](const CmClient::ServiceCall& call, std::uint64_t) {
    std::vector<std::uint8_t> reply;
    return call("Cloud.Ping#1", {}, &reply, nullptr);
  });
  assert(result == CmError::kNone);
  // The old session closed before the new logon.
  assert(CountSent(script, cm::kEMsgClientLogon) == 2 && CountSent(script, cm::kEMsgClientLogOff) == 0);
}

std::vector<std::uint8_t> LicenseListFrame(const std::vector<std::uint32_t>& packages) {
  proto::Writer licenses;
  for (const auto package : packages) {
    proto::Writer license;
    license.AddVarint(1, package);
    license.AddFixed32(2, 1500000000);
    licenses.AddMessage(2, license);
  }
  return ServerFrame(cm::kEMsgClientLicenseList, licenses.bytes());
}

bool WaitForLicenseChanges(const CmSharedSession& shared, std::uint64_t wanted) {
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (CmLicenseChanges(shared) != wanted && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(5ms);
  return CmLicenseChanges(shared) == wanted;
}

// Steam pushes the license list again when a license is added (a free game added to the library on
// another device). Only a different set of packages counts, also across a new logon.
void SharedSessionCountsLicenseChanges() {
  auto script = std::make_shared<Script>();
  script->on_logon = {ServerFrame(cm::kEMsgClientLogOnResponse, LogonResponseBody(1, 9)), LicenseListFrame({100, 200})};
  script->on_service = {ServerFrame(cm::kEMsgServiceMethodResponse, {}, std::nullopt, 1)};
  auto shared = MakeCmSharedSession();
  auto client = MakeClient(script);
  client.SetSharedSession(shared);
  std::atomic<bool> cancel{false};
  assert(Ping(client, cancel) == CmError::kNone);
  // The first list after the first logon is what the library already reads.
  assert(CmLicenseChanges(*shared) == 0);
  {
    std::lock_guard<std::mutex> lock(script->mutex);
    script->Queue(LicenseListFrame({200, 100}));
  }
  std::this_thread::sleep_for(400ms);
  assert(CmLicenseChanges(*shared) == 0);
  // Pushed while the session is idle: the pump reads it.
  {
    std::lock_guard<std::mutex> lock(script->mutex);
    script->Queue(LicenseListFrame({100, 200, 300}));
  }
  assert(WaitForLicenseChanges(*shared, 1));
  // A rotated token logs on again; the list it gets differs from the last one seen.
  {
    std::lock_guard<std::mutex> lock(script->mutex);
    script->on_logon = {ServerFrame(cm::kEMsgClientLogOnResponse, LogonResponseBody(1, 9)), LicenseListFrame({100, 300})};
  }
  auto rotated = Session();
  rotated.refresh_token = "synthetic.rotated.token";
  assert(client.WithServiceSession(rotated, 3000, cancel, [](const CmClient::ServiceCall& call, std::uint64_t) {
    std::vector<std::uint8_t> reply;
    return call("Cloud.Ping#1", {}, &reply, nullptr);
  }) == CmError::kNone);
  assert(WaitForLicenseChanges(*shared, 2));
  assert(CountSent(script, cm::kEMsgClientLogon) == 2);
}

// A call waiting while another holds the shared session (a long library license scan, issue #47)
// can be canceled, and its deadline starts only once it has the session.
void WaitingForTheSharedSessionIsCancelableAndUntimed() {
  auto script = std::make_shared<Script>();
  script->on_logon = {ServerFrame(cm::kEMsgClientLogOnResponse, LogonResponseBody(1, 9))};
  script->on_service = {ServerFrame(cm::kEMsgServiceMethodResponse, {}, std::nullopt, 1)};
  auto shared = MakeCmSharedSession();
  auto holder = MakeClient(script);
  auto waiter = MakeClient(script);
  holder.SetSharedSession(shared);
  waiter.SetSharedSession(shared);
  std::atomic<bool> held{false};
  std::atomic<bool> no_cancel{false};
  std::thread scan([&] {
    assert(holder.WithServiceSession(Session(), 3000, no_cancel, [&](const CmClient::ServiceCall&, std::uint64_t) {
      held = true;
      std::this_thread::sleep_for(600ms);
      return CmError::kNone;
    }) == CmError::kNone);
  });
  while (!held) std::this_thread::sleep_for(1ms);

  std::atomic<bool> cancel{false};
  std::thread canceler([&cancel] {
    std::this_thread::sleep_for(50ms);
    cancel = true;
  });
  const auto start = std::chrono::steady_clock::now();
  assert(Ping(waiter, cancel) == CmError::kCanceled);
  assert(std::chrono::steady_clock::now() - start < 300ms);
  canceler.join();

  // Waits out the rest of the scan, longer than its own 200 ms deadline, then still gets its answer.
  const auto result = waiter.WithServiceSession(Session(), 200, no_cancel, [](const CmClient::ServiceCall& call, std::uint64_t) {
    std::vector<std::uint8_t> reply;
    return call("Cloud.Ping#1", {}, &reply, nullptr);
  });
  assert(result == CmError::kNone);
  scan.join();
}

void WithoutKeepAliveEachCallLogsOnAgain() {
  auto script = std::make_shared<Script>();
  script->on_logon = {ServerFrame(cm::kEMsgClientLogOnResponse, LogonResponseBody(1, 9))};
  script->on_service = {ServerFrame(cm::kEMsgServiceMethodResponse, {}, std::nullopt, 1)};
  auto client = MakeClient(script);
  std::atomic<bool> cancel{false};
  assert(Ping(client, cancel) == CmError::kNone);
  assert(Ping(client, cancel) == CmError::kNone);
  assert(CountSent(script, cm::kEMsgClientLogon) == 2 && CountSent(script, cm::kEMsgClientLogOff) == 0);
}

void RefusedTokenIsNotTriedAgain() {
  ResetCmBackoff();
  auto script = std::make_shared<Script>();
  script->on_logon = {ServerFrame(cm::kEMsgClientLogOnResponse, LogonResponseBody(cm::kEResultAccessDenied, 9))};
  auto client = MakeClient(script);
  std::atomic<bool> cancel{false};
  assert(client.FetchAcquisitionDates(Session(), cancel).error == CmError::kRejected);
  assert(client.FetchAcquisitionDates(Session(), cancel).error == CmError::kRejected);
  {
    std::lock_guard<std::mutex> lock(script->mutex);
    assert(script->connects == 1);  // the second call never reached Steam
  }
  auto renewed = Session();
  renewed.refresh_token = "synthetic.new.token";
  assert(client.FetchAcquisitionDates(renewed, cancel).error == CmError::kRejected);  // tried: a new token
  std::lock_guard<std::mutex> lock(script->mutex);
  assert(script->connects == 2);
  ResetCmBackoff();
}

void BusyLogonBacksOffEveryClient() {
  ResetCmBackoff();
  std::vector<std::string> lines;
  std::mutex lines_mutex;
  SetCmLog([&](const std::string& line) {
    std::lock_guard<std::mutex> lock(lines_mutex);
    lines.push_back(line);
  });
  auto script = std::make_shared<Script>();
  constexpr std::int32_t kRateLimitExceeded = 84;
  script->on_logon = {ServerFrame(cm::kEMsgClientLogOnResponse, LogonResponseBody(kRateLimitExceeded, 9))};
  auto client = MakeClient(script);
  client.SetOwner("cloud");
  std::atomic<bool> cancel{false};
  // Steam being busy is not a refusal of the credentials.
  assert(client.RenewAccessToken(Session(), cancel).error == CmError::kNetwork);
  assert(CmBackoffActive());
  auto other = MakeClient(script);
  assert(other.FetchAcquisitionDates(Session(), cancel).error == CmError::kNetwork);
  {
    std::lock_guard<std::mutex> lock(script->mutex);
    assert(script->connects == 1);  // the second client didn't even connect
  }
  {
    std::lock_guard<std::mutex> lock(lines_mutex);
    assert(lines.size() == 2);
    assert(lines[0].rfind("owner=cloud step=logon error=network eresult=84 fp=", 0) == 0);
    assert(lines[1] == "owner=cm step=backoff error=network");
    for (const auto& line : lines) assert(line.find("synthetic") == std::string::npos);
  }
  SetCmLog(nullptr);
  ResetCmBackoff();
  assert(!CmBackoffActive());
}

void LogOffInsteadOfLogonResponseBacksOff() {
  ResetCmBackoff();
  auto script = std::make_shared<Script>();
  proto::Writer off;
  off.AddVarint(1, cm::kEResultTryAnotherCM);
  script->on_logon = {ServerFrame(cm::kEMsgClientLoggedOff, off.bytes())};
  auto client = MakeClient(script);
  std::atomic<bool> cancel{false};
  assert(client.FetchAcquisitionDates(Session(), cancel).error == CmError::kNetwork);
  assert(CmBackoffActive());
  ResetCmBackoff();
}

void DirectoryErrorBacksOff() {
  ResetCmBackoff();
  auto script = HappyScript();
  CmClientHooks hooks;
  hooks.directory = [](const HttpRequest&, const std::atomic<bool>&) {
    HttpResponse response;
    response.status_code = 429;
    return response;
  };
  hooks.transport = [script] { return std::make_unique<FakeTransport>(script); };
  hooks.deadline_ms = 3000;
  CmClient client(hooks, MakeDeviceIdentity(kSeed));
  std::atomic<bool> cancel{false};
  assert(client.FetchAcquisitionDates(Session(), cancel).error == CmError::kNetwork);
  assert(CmBackoffActive());
  ResetCmBackoff();
}

void AppInfoUsesAccessTokensAndCollectsMultipartBuffers() {
  auto script = std::make_shared<Script>();
  script->on_logon = {ServerFrame(cm::kEMsgClientLogOnResponse, LogonResponseBody(1, 9))};
  proto::Writer token;
  token.AddVarint(1, 10);
  token.AddVarint(2, 555);
  proto::Writer tokens;
  tokens.AddMessage(3, token);
  script->on_emsg[cm::kEMsgClientPICSAccessTokenRequest] = {
      ServerFrame(cm::kEMsgClientPICSAccessTokenResponse, tokens.bytes(), std::nullopt, 1)};
  auto app_part = [](std::uint32_t id, const std::string& text, bool pending) {
    proto::Writer app;
    app.AddVarint(1, id);
    std::vector<std::uint8_t> buffer(text.begin(), text.end());
    buffer.push_back(0);
    buffer.push_back(0);
    app.AddBytes(5, buffer);
    proto::Writer response;
    response.AddMessage(1, app);
    if (pending) response.AddVarint(6, 1);
    return response.bytes();
  };
  script->on_emsg[cm::kEMsgClientPICSProductInfoRequest] = {
      ServerFrame(cm::kEMsgClientPICSProductInfoResponse, app_part(10, "\"appinfo\" { }", true), std::nullopt, 2),
      ServerFrame(cm::kEMsgClientPICSProductInfoResponse, app_part(20, "\"appinfo\" { \"x\" \"y\" }", false), std::nullopt, 2)};
  auto client = MakeClient(script);
  std::atomic<bool> cancel{false};
  const auto result = client.FetchAppInfo(Session(), {10, 20, 30}, cancel);
  assert(result.ok() && result.buffers.size() == 2);
  assert(result.buffers.at(10) == "\"appinfo\" { }");  // trailing NULs removed
  assert(result.buffers.at(20).find("\"x\"") != std::string::npos);
  std::lock_guard<std::mutex> lock(script->mutex);
  const auto& request = script->sent.back();
  assert(request.emsg == cm::kEMsgClientPICSProductInfoRequest && request.header.jobid_source == 2u);
  // The granted token rides along with app 10 only.
  assert(request.body == cm::EncodePicsAppsRequest({{10, 555}, {20, 0}, {30, 0}}));
  assert(client.FetchAppInfo(Session(), {}, cancel).error == CmError::kProtocol);
}

std::shared_ptr<Script> DepotScript(std::int32_t key_result, std::uint64_t code) {
  auto script = std::make_shared<Script>();
  script->on_logon = {ServerFrame(cm::kEMsgClientLogOnResponse, LogonResponseBody(1, 9))};
  proto::Writer key;
  key.AddVarint(1, static_cast<std::uint32_t>(key_result));
  key.AddVarint(2, 441);
  key.AddBytes(3, std::vector<std::uint8_t>(32, 0x5a));
  script->on_emsg[cm::kEMsgClientGetDepotDecryptionKey] = {
      ServerFrame(cm::kEMsgClientGetDepotDecryptionKeyResponse, key.bytes(), std::nullopt, 1)};
  proto::Writer request_code;
  request_code.AddVarint(1, code);
  script->on_service = {ServerFrame(cm::kEMsgServiceMethodResponse, request_code.bytes(), std::nullopt, 2)};
  return script;
}

void DepotAccessReturnsKeysAndManifestCodes() {
  LogCapture log;
  auto script = DepotScript(cm::kEResultOk, 987654321);
  auto client = MakeClient(script);
  std::atomic<bool> cancel{false};
  const auto access = client.FetchDepotAccess(Session(), 440, {{441, 11}}, cancel);
  assert(access.ok() && access.grants.size() == 1);
  assert(access.grants[0].depot_id == 441 && access.grants[0].manifest_gid == 11);
  assert(access.grants[0].key[0] == 0x5a && access.grants[0].manifest_request_code == 987654321u);
  {
    std::lock_guard<std::mutex> lock(script->mutex);
    const auto& last_call = script->raw_sent.back();
    const std::string raw(last_call.begin(), last_call.end());
    assert(raw.find("ContentServerDirectory.GetManifestRequestCode#1") != std::string::npos);
    assert(raw.find("public") != std::string::npos);
  }
  auto refused = MakeClient(DepotScript(cm::kEResultAccessDenied, 1));
  assert(refused.FetchDepotAccess(Session(), 440, {{441, 11}}, cancel).error == CmError::kRejected);
  // A depot borrowed from another app the account doesn't own is skipped, not a refusal (Slay the Spire's 877621).
  auto borrowed = MakeClient(DepotScript(cm::kEResultAccessDenied, 1));
  const auto skipped = borrowed.FetchDepotAccess(Session(), 440, {{441, 11, true}}, cancel);
  assert(skipped.ok() && skipped.grants.empty() && skipped.skipped == std::vector<std::uint32_t>{441});
  auto no_code = MakeClient(DepotScript(cm::kEResultOk, 0));
  assert(no_code.FetchDepotAccess(Session(), 440, {{441, 11}}, cancel).error == CmError::kRejected);
  // Steam refusing the manifest request code: its EResult is in the detail.
  auto code_refused_script = DepotScript(cm::kEResultOk, 1);
  code_refused_script->on_service = {
      ServerFrame(cm::kEMsgServiceMethodResponse, {}, cm::kEResultAccessDenied, 2)};
  auto code_refused = MakeClient(code_refused_script);
  const auto denied = code_refused.FetchDepotAccess(Session(), 440, {{441, 11}}, cancel);
  assert(denied.error == CmError::kRejected && denied.detail == "manifest request code refused eresult=15");
  // Every depot's key and request code result is logged, with Steam's EResult when refused.
  std::vector<std::string> lines;
  for (auto& line : log.Take())
    if (line.find("step=depot_key") != std::string::npos || line.find("step=manifest_code") != std::string::npos)
      lines.push_back(std::move(line));
  assert(lines == (std::vector<std::string>{
                      "owner=cm step=depot_key app=440 depot=441 result=ok",
                      "owner=cm step=manifest_code app=440 depot=441 result=ok",
                      "owner=cm step=depot_key app=440 depot=441 result=refused eresult=15",
                      "owner=cm step=depot_key app=440 depot=441 result=skipped eresult=15",
                      "owner=cm step=depot_key app=440 depot=441 result=ok",
                      "owner=cm step=manifest_code app=440 depot=441 result=refused code=0",
                      "owner=cm step=depot_key app=440 depot=441 result=ok",
                      "owner=cm step=manifest_code app=440 depot=441 error=rejected eresult=15",
                  }));
}

std::vector<std::uint8_t> FreeLicenseReply(std::int32_t eresult, const std::vector<std::uint32_t>& apps,
                                           const std::vector<std::uint32_t>& packages, std::uint64_t job) {
  proto::Writer body;
  body.AddVarint(1, static_cast<std::uint32_t>(eresult));
  for (const auto package : packages) body.AddVarint(2, package);
  for (const auto app : apps) body.AddVarint(3, app);
  return ServerFrame(cm::kEMsgClientRequestFreeLicenseResponse, body.bytes(), std::nullopt, job);
}

// Free DLC: one ClientRequestFreeLicense; after a grant the call waits for the license list Steam pushes,
// so the license read that follows on the shared session sees the new license.
void FreeLicenseGrantWaitsForTheNewLicenseList() {
  auto script = std::make_shared<Script>();
  script->on_logon = {ServerFrame(cm::kEMsgClientLogOnResponse, LogonResponseBody(1, 9)), LicenseListFrame({100})};
  // Steam may push the new list before its answer: it still counts.
  script->on_emsg[cm::kEMsgClientRequestFreeLicense] = {LicenseListFrame({100, 900}),
                                                        FreeLicenseReply(cm::kEResultOk, {5001}, {900}, 1)};
  auto shared = MakeCmSharedSession();
  auto client = MakeClient(script);
  client.SetSharedSession(shared);
  std::atomic<bool> cancel{false};
  const auto granted = client.RequestFreeLicenses(Session(), {5001, 5002}, 5000, cancel);
  assert(granted.ok() && granted.eresult == cm::kEResultOk && granted.licenses_updated && granted.waited_ms < 1000);
  assert(granted.granted_apps == std::vector<std::uint32_t>{5001} &&
         granted.granted_packages == std::vector<std::uint32_t>{900});
  {
    std::lock_guard<std::mutex> lock(script->mutex);
    const auto request = std::find_if(script->sent.begin(), script->sent.end(), [](const cm::Frame& frame) {
      return frame.emsg == cm::kEMsgClientRequestFreeLicense;
    });
    assert(request != script->sent.end() && request->body == cm::EncodeFreeLicenseRequest({5001, 5002}));
    assert(request->header.jobid_source == 1u);
  }

  // Granted, but no new list within the wait: still a success, and the shared session stays open.
  {
    std::lock_guard<std::mutex> lock(script->mutex);
    script->on_emsg[cm::kEMsgClientRequestFreeLicense] = {FreeLicenseReply(cm::kEResultOk, {5002}, {}, 2)};
    script->on_service = {ServerFrame(cm::kEMsgServiceMethodResponse, {}, std::nullopt, 3)};
  }
  const auto start = std::chrono::steady_clock::now();
  const auto late = client.RequestFreeLicenses(Session(), {5002}, 200, cancel);
  assert(late.ok() && !late.licenses_updated && late.waited_ms >= 150);
  assert(std::chrono::steady_clock::now() - start < 2s);
  assert(Ping(client, cancel) == CmError::kNone);
  assert(CountSent(script, cm::kEMsgClientLogon) == 1);

  // Nothing granted (paid DLC): no wait.
  {
    std::lock_guard<std::mutex> lock(script->mutex);
    script->on_emsg[cm::kEMsgClientRequestFreeLicense] = {FreeLicenseReply(cm::kEResultOk, {}, {}, 4)};
  }
  const auto refused = client.RequestFreeLicenses(Session(), {5003}, 5000, cancel);
  assert(refused.ok() && refused.granted_apps.empty() && !refused.licenses_updated && refused.waited_ms == 0);

  // Steam failing the request is kRejected with its EResult, in the body or the header.
  {
    std::lock_guard<std::mutex> lock(script->mutex);
    script->on_emsg[cm::kEMsgClientRequestFreeLicense] = {FreeLicenseReply(cm::kEResultFail, {}, {}, 5)};
  }
  const auto failed = client.RequestFreeLicenses(Session(), {5003}, 5000, cancel);
  assert(failed.error == CmError::kRejected && failed.eresult == cm::kEResultFail);
  {
    std::lock_guard<std::mutex> lock(script->mutex);
    script->on_emsg[cm::kEMsgClientRequestFreeLicense] = {
        ServerFrame(cm::kEMsgClientRequestFreeLicenseResponse, {}, cm::kEResultAccessDenied, 6)};
  }
  const auto denied = client.RequestFreeLicenses(Session(), {5003}, 5000, cancel);
  assert(denied.error == CmError::kRejected && denied.eresult == cm::kEResultAccessDenied);
  assert(client.RequestFreeLicenses(Session(), {}, 5000, cancel).error == CmError::kProtocol);
}

// The wait after a grant ends at once when the install is canceled.
void FreeLicenseWaitIsCancelable() {
  auto script = std::make_shared<Script>();
  script->on_logon = {ServerFrame(cm::kEMsgClientLogOnResponse, LogonResponseBody(1, 9)), LicenseListFrame({100})};
  script->on_emsg[cm::kEMsgClientRequestFreeLicense] = {FreeLicenseReply(cm::kEResultOk, {5001}, {}, 1)};
  auto client = MakeClient(script);
  std::atomic<bool> cancel{false};
  std::thread canceler([&cancel] {
    std::this_thread::sleep_for(100ms);
    cancel = true;
  });
  const auto start = std::chrono::steady_clock::now();
  const auto result = client.RequestFreeLicenses(Session(), {5001}, 10000, cancel);
  canceler.join();
  assert(result.error == CmError::kCanceled && std::chrono::steady_clock::now() - start < 2s);
}

// A DLC app's own depot: its key and manifest request code are asked for under the DLC app, not the game.
// Another version's manifest: its request code names that branch.
void DepotAccessNamesTheBranch() {
  auto script = DepotScript(cm::kEResultOk, 987654321);
  auto client = MakeClient(script);
  std::atomic<bool> cancel{false};
  DepotRequest request{441, 11};
  request.branch = "4.04";
  assert(client.FetchDepotAccess(Session(), 440, {request}, cancel).ok());
  std::lock_guard<std::mutex> lock(script->mutex);
  const auto& code = script->sent.back();
  assert(code.emsg == cm::kEMsgServiceMethodCallFromClient);
  assert(code.body == cm::EncodeManifestRequestCodeRequest(440, 441, 11, "4.04"));
  const std::string body(code.body.begin(), code.body.end());
  assert(body.find("\x22\x04" "4.04") != std::string::npos && body.find("public") == std::string::npos);
}

void DepotAccessAsksEachDepotUnderItsOwnApp() {
  LogCapture log;
  auto script = DepotScript(cm::kEResultOk, 987654321);
  auto client = MakeClient(script);
  std::atomic<bool> cancel{false};
  DepotRequest request{441, 11};
  request.app_id = 5001;
  assert(client.FetchDepotAccess(Session(), 440, {request}, cancel).ok());
  {
    std::lock_guard<std::mutex> lock(script->mutex);
    const auto key = std::find_if(script->sent.begin(), script->sent.end(), [](const cm::Frame& frame) {
      return frame.emsg == cm::kEMsgClientGetDepotDecryptionKey;
    });
    assert(key != script->sent.end() && key->body == cm::EncodeDepotKeyRequest(441, 5001));
    const auto& code = script->sent.back();
    assert(code.emsg == cm::kEMsgServiceMethodCallFromClient);
    assert(code.body == cm::EncodeManifestRequestCodeRequest(5001, 441, 11));
    // Without a branch the request names the public one.
    const std::string body(code.body.begin(), code.body.end());
    assert(body.find("\x22\x06public") != std::string::npos);
  }
  bool named = false;
  for (const auto& line : log.Take()) named |= line == "owner=cm step=depot_key app=5001 depot=441 result=ok";
  assert(named);
}

void DeviceSeedPersistsPerInstall() {
  const auto root = std::filesystem::temp_directory_path() / "playden-cm-seed-test";
  std::filesystem::remove_all(root);
  Storage storage(root.string());
  const auto first = LoadOrCreateDeviceSeed(storage);
  const auto second = LoadOrCreateDeviceSeed(storage);
  assert(first.size() == 32 && first == second);
  assert(MakeDeviceIdentity(first).machine_id == MakeDeviceIdentity(second).machine_id);
  assert(MakeDeviceIdentity(first).login_id != MakeDeviceIdentity(kSeed).login_id);
  std::filesystem::remove_all(root);
  // A seed that cannot be saved (the root's parent is missing) is still the same for the rest of the run, so a
  // sign-in and the CM logons sharing the storage name one machine.
  Storage unwritable((root / "missing-parent" / "root").string());
  const auto unsaved = LoadOrCreateDeviceSeed(unwritable);
  assert(unsaved.size() == 32 && LoadOrCreateDeviceSeed(unwritable) == unsaved);
  assert(!std::filesystem::exists(root));
}

}  // namespace

int main(int argc, char** argv) {
  assert(argc == 2);
  g_root = argv[1];
  // A token one test refused stays refused for the process, so every test starts clean.
  ResetCmBackoff();
  AcquisitionDatesFromGzipMultiLicensesAndMultipartPics();
  PackageZeroCounts();
  ResetCmBackoff();
  BorrowedLicensesAreShared();
  ResetCmBackoff();
  OwnLicenseWinsOverBorrowedInEitherOrder();
  ResetCmBackoff();
  BorrowedPackagesPicsSkipsAreLogged();
  ResetCmBackoff();
  FamilyLicenseLinesAreCappedAndOnlyOnChange();
  ResetCmBackoff();
  RejectedLogonIsReportedWithoutDates();
  ResetCmBackoff();
  FailedLicenseListIsRejected();
  ResetCmBackoff();
  PackagesSteamCannotDescribeAreSkipped();
  ResetCmBackoff();
  JobErrorResultFailsTheBatch();
  ResetCmBackoff();
  SilentServerTimesOut();
  ResetCmBackoff();
  EveryPackageBatchGetsItsOwnDeadline();
  ResetCmBackoff();
  CancelStopsWaiting();
  ResetCmBackoff();
  HeartbeatIsSentOnTheServerInterval();
  ResetCmBackoff();
  MissingSessionOrEndpointsFailEarly();
  ResetCmBackoff();
  AccessTokenRenewsThroughAuthenticatedServiceMethod();
  ResetCmBackoff();
  RefusedServiceCallIsNotARefusedLogon();
  ResetCmBackoff();
  AppInfoUsesAccessTokensAndCollectsMultipartBuffers();
  ResetCmBackoff();
  DepotAccessReturnsKeysAndManifestCodes();
  ResetCmBackoff();
  DepotAccessAsksEachDepotUnderItsOwnApp();
  DepotAccessNamesTheBranch();
  ResetCmBackoff();
  FreeLicenseGrantWaitsForTheNewLicenseList();
  ResetCmBackoff();
  FreeLicenseWaitIsCancelable();
  ResetCmBackoff();
  DeviceSeedPersistsPerInstall();
  ResetCmBackoff();
  SharedSessionServesEveryClientWithOneLogon();
  ResetCmBackoff();
  SharedSessionLogsOnAgainForNewCredentials();
  SharedSessionCountsLicenseChanges();
  ResetCmBackoff();
  WaitingForTheSharedSessionIsCancelableAndUntimed();
  ResetCmBackoff();
  WithoutKeepAliveEachCallLogsOnAgain();
  ResetCmBackoff();
  RefusedTokenIsNotTriedAgain();
  ResetCmBackoff();
  BusyLogonBacksOffEveryClient();
  ResetCmBackoff();
  DirectoryErrorBacksOff();
  ResetCmBackoff();
  LogOffInsteadOfLogonResponseBacksOff();
}
