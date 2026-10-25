#include "platform/ps5/native_coordinator.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>
using namespace playden::runtime;
using namespace playden::ps5;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%d: %s\n", __LINE__, #x); std::exit(1); } } while (0)
struct Records : InstallRecordStore {
  RuntimeInstall value;
  std::optional<RuntimeInstall> Find(const std::string& id) const override { return id == value.install_id ? std::optional<RuntimeInstall>(value) : std::nullopt; }
  std::vector<RuntimeInstall> Snapshot() const override { return {value}; }
  RecordUpdateResult Update(const RuntimeInstall&) override { CHECK(false); return {}; }
};
struct Package : PackageInstaller {
  PackageStatus value;
  PackageStatus Inspect(const RuntimeInstall&) override { return value; }
  PackageActionResult StageTitle(const RuntimeInstall&) override { CHECK(false); return {}; }
  PackageActionResult CopyTemplate(const RuntimeInstall&) override { CHECK(false); return {}; }
  PackageActionResult ValidateArtifacts(const RuntimeInstall&) override { CHECK(false); return {}; }
  PackageActionResult PublishProfile(const RuntimeInstall&) override { CHECK(false); return {}; }
};
int main() {
  char name[] = "/tmp/playden-native-coordinator-XXXXXX"; CHECK(mkdtemp(name));
  const auto temporary = std::filesystem::canonical(name), root = temporary / "data/homebrew";
  CHECK(ReadTitleSource(root, "FAKE78001").availability == TitleAvailability::kUnknown);
  std::filesystem::create_directories(root);
  CHECK(ReadTitleSource(root, "FAKE78001").availability == TitleAvailability::kFree);
  Records records; Package package;
  auto& record = records.value;
  record.install_id = "install-220"; record.title_id = "FAKE78001"; record.runtime_number = 220;
  record.data_root = "/data/playden/games/install-220"; record.default_profile_id = "main";
  record.runtime_bundle_id = "wine-tested"; record.recipe_id = "hl2"; record.recipe_version = "1";
  record.launch_entry_id = "game"; record.launch_path = "hl2.exe"; record.install_path = "/data/games/220";
  record.launch_working_directory = ""; record.volume_id = "internal";
  CHECK(ValidateRuntimeInstallRecord(record));
  std::string response = R"({"status":0,"games":[]})";
  unsigned mutations = 0;
  TitleRegistry registry([&](const std::string& route, const std::string&) -> std::optional<std::string> {
    if (route == "/api/v1/games") return response;
    ++mutations; return R"({"status":0})";
  });
  TitleDatabaseObservation database{TitleAvailability::kFree, {}};
  NativeActivity activity = NativeActivity::kStopped;
  Ps5NativeCoordinator native(records, package, registry, root, {
      [&](const auto&) { return database; }, [&](const auto&) { return activity; }});
  auto available = native.Availability(record.title_id);
  CHECK(available.inventory == TitleAvailability::kFree && available.source_path == TitleAvailability::kFree);
  CHECK(native.Activity(record.title_id) == NativeActivity::kStopped);
  CHECK(native.ReportAvailability(record.title_id).Text() == "app.db=free folder=free shadowmount=absent activity=stopped");
  response = "{}";
  const auto report = native.ReportAvailability(record.title_id);
  CHECK(report.shadowmount.state == RegistrationState::kUnavailable && report.database == TitleAvailability::kFree);
  CHECK(report.Text() == "app.db=free folder=free shadowmount=unavailable(status) activity=stopped");
  response = R"({"status":0,"games":[{"title_id":"FAKE78001","installed":false,"managed":false,"mounted":false,"source_available":false,"source_type":"folder","runtime_path":"/data/homebrew/FAKE78001","path":"/data/homebrew/FAKE78001"}]})";
  available = native.Availability(record.title_id);
  CHECK(available.inventory == TitleAvailability::kFree && available.source_path == TitleAvailability::kFree);
  for (const auto occupied : {TitleAvailability::kOccupied, TitleAvailability::kUnknown}) {
    database.inventory = occupied;
    CHECK(native.Availability(record.title_id).inventory != TitleAvailability::kFree);
  }
  database.inventory = TitleAvailability::kFree;
  for (const auto busy : {NativeActivity::kActive, NativeActivity::kUnknown}) {
    activity = busy;
    CHECK(native.Availability(record.title_id).inventory != TitleAvailability::kFree);
  }
  activity = NativeActivity::kStopped;
  std::filesystem::create_directories(root / record.title_id);
  CHECK(native.Availability(record.title_id).source_path == TitleAvailability::kOccupied);
  std::filesystem::remove(root / record.title_id);
  response = R"({"status":0,"games":[]})";
  auto source = root / record.title_id; std::filesystem::create_directories(source / "sce_sys");
  CHECK(chmod(source.c_str(), 0700) == 0); CHECK(chmod((source / "sce_sys").c_str(), 0700) == 0);
  std::ofstream(source / "sce_sys/param.json") << R"({"titleId":"FAKE78001","contentId":"UP9000-FAKE78001_00-PROSPEROWIN00001"})";
  package.value.known = true; package.value.title_staged = true; package.value.manifest_matches = true; package.value.artifacts_valid = true;
  package.value.title_source_path = source.string();
  auto status = native.Query(record); CHECK(status.available && status.source_available && status.owned_manifest_matches && !status.installed && !status.collision);
  // ShadowMount must read the source: an owned 0755 tree is still Playden's,
  // but a group/other-writable one is not.
  CHECK(chmod(source.c_str(), 0755) == 0); CHECK(chmod((source / "sce_sys").c_str(), 0755) == 0);
  status = native.Query(record); CHECK(status.owned_manifest_matches && !status.collision);
  CHECK(chmod(source.c_str(), 0777) == 0); CHECK(native.Query(record).collision);
  CHECK(chmod(source.c_str(), 0755) == 0); CHECK(chmod((source / "sce_sys").c_str(), 0775) == 0);
  CHECK(native.Query(record).collision);
  CHECK(chmod(source.c_str(), 0700) == 0); CHECK(chmod((source / "sce_sys").c_str(), 0700) == 0);
  CHECK(native.Availability(record.title_id).source_path == TitleAvailability::kOccupied);
  CHECK(native.Register(record).state == RegistrationResultState::kFailed && mutations == 0);
  record.journal = PreparationJournal{"11111111-2222-3333-4444-555555555555", "settings-publication", "register"};
  CHECK(native.Register(record).state == RegistrationResultState::kFailed && mutations == 0);
  record.state = PreparationState::kRegistering;
  record.journal->operation_id = "transaction";
  CHECK(native.Register(record).state == RegistrationResultState::kFailed && mutations == 0);
  record.journal->operation_id = "11111111-2222-3333-4444-555555555555";
  CHECK(native.Register(record).state == RegistrationResultState::kFailed && mutations == 0);
  package.value.template_owned = true; package.value.mutable_bottle_present = true; package.value.mutable_bottle_owned = true;
  CHECK(native.Register(record).state == RegistrationResultState::kScanQueued && mutations == 2);
  activity = NativeActivity::kUnknown; CHECK(native.Register(record).state == RegistrationResultState::kFailed && mutations == 2); activity = NativeActivity::kStopped;
  database = {TitleAvailability::kOccupied, InstalledTitleIdentity{record.title_id, "UP9000-FAKE78001_00-PROSPEROWIN00001", "/user/app/FAKE78001/sce_sys"}};
  response = R"({"status":0,"games":[{"title_id":"FAKE78001","installed":true,"managed":true,"source_available":true,"source_type":"folder","path":"/data/homebrew/FAKE78001"}]})";
  status = native.Query(record); CHECK(status.available && status.installed && !status.collision);
  CHECK(native.Register(record).state == RegistrationResultState::kAccepted && mutations == 2);
  const auto stale_arm = record;
  record.journal->operation_id = "11111111-2222-3333-4444-666666666666";
  CHECK(native.Register(stale_arm).state == RegistrationResultState::kFailed && mutations == 2);
  database.identity->content_id = "foreign"; CHECK(native.Query(record).collision); CHECK(native.Register(record).state == RegistrationResultState::kFailed && mutations == 2);
  database.identity->content_id = "UP9000-FAKE78001_00-PROSPEROWIN00001";
  database.inventory = TitleAvailability::kFree; CHECK(!native.Query(record).available); database.inventory = TitleAvailability::kOccupied;
  const auto ready_response = response;
  response = R"({"status":0,"games":[{"title_id":"FAKE78001","installed":true,"managed":true,"source_available":true,"source_type":"folder","path":"/data/homebrew/FAKE78001"},{"title_id":"FAKE78001","installed":true,"managed":true,"source_available":true,"source_type":"folder","path":"/data/foreign"}]})";
  CHECK(native.Query(record).collision);
  CHECK(native.Register(record).state == RegistrationResultState::kFailed && mutations == 2);
  response = ready_response;
  record.journal->step = "await-native";
  CHECK(native.Register(record).state == RegistrationResultState::kFailed && mutations == 2);
  record.journal->step = "register";
  package.value.manifest_matches = false; CHECK(native.Query(record).collision); package.value.manifest_matches = true;
  // A package inspected in another mapped namespace cannot authorize this source.
  package.value.title_source_path = (temporary / "elsewhere/data/homebrew/FAKE78001").string();
  CHECK(native.Query(record).collision);
  package.value.title_source_path = source.string();
  response = "{}"; CHECK(!native.Query(record).available); CHECK(native.Availability("FAKE78002").source_path == TitleAvailability::kUnknown);
  std::filesystem::remove_all(source); std::filesystem::create_symlink("/", source);
  CHECK(ReadTitleSource(root, record.title_id).availability != TitleAvailability::kFree);
  CHECK(!native.Query(record).available);
  CHECK(native.Activity("../unsafe") == NativeActivity::kUnknown);
  std::filesystem::remove_all(temporary);
  // The default reader logs an unknown result once per reason, not on every check.
  std::vector<std::string> lines;
  const auto logged = LoggedTitleActivity([&](const std::string& line) { lines.push_back(line); });
  CHECK(logged("FAKE78001") == NativeActivity::kUnknown && logged("FAKE78001") == NativeActivity::kUnknown);
  CHECK(logged("FAKE78002") == NativeActivity::kUnknown);
  CHECK(lines.size() == 2 && lines[0] == "title activity unknown title=FAKE78001 reason=not a PS5 build");
  std::puts("native coordinator tests passed");
}
