#include "runtime/settings_publication_journal.h"
#include "runtime/artifact_hash.h"
#include "runtime/publication_contract.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <csignal>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%d: %s\n", __LINE__, #x); std::exit(1); } } while (0)
#include "settings_publication_fixture.h"
// Rewrites a schema-2 fixture descriptor as legacy schema 1, with consistent metadata hashes.
SettingsPublicationOperation Legacy(SettingsPublicationOperation o) {
  auto& bytes = o.descriptor_bytes;
  bytes.replace(bytes.find("\"schema_version\":2"), 18, "\"schema_version\":1");
  bytes.erase(bytes.find(",\"game_root\"")); bytes += "}\n";
  PwTitlePublicationMetadata m{};
  CHECK(pw_title_publication_metadata_parse(reinterpret_cast<const uint8_t*>(o.metadata_bytes.data()),
        o.metadata_bytes.size(), &m) == PW_OK);
  Copy(m.descriptor_sha256, *HashArtifactBytes(bytes));
  auto metadata = SerializePublicationMetadata(m); CHECK(metadata); o.metadata_bytes = *metadata;
  return o;
}
int main() {
  std::string reason;
  CHECK(!FreezeSettingsPublicationOperation({}, &reason));
  CHECK(!DecodeSettingsPublicationOperation({}, &reason));
  CHECK(!DecodeSettingsPublicationOperation(playden::Bytes(kMaxSettingsPublicationOperationBytes + 1), &reason));
  auto request_source = Fixture();
  SettingsPublicationRequest request;
  request.transaction_id = request_source.transaction_id;
  request.kit_id = request_source.kit_id;
  request.install = request_source.install;
  request.plan = request_source.plan;
  request.registry_unix_time = request_source.registry_unix_time;
  auto request_bytes = EncodeSettingsPublicationRequest(request, &reason); CHECK(request_bytes);
  auto request_recovered = DecodeSettingsPublicationRequest(*request_bytes, &reason); CHECK(request_recovered);
  CHECK(EncodeSettingsPublicationRequest(*request_recovered, &reason) == request_bytes);
  CHECK(request_recovered->plan.profile_bytes == request.plan.profile_bytes);
  CHECK(request_recovered->install.launch_arguments == request.install.launch_arguments);
  for (std::size_t i = 0; i < request_bytes->size(); ++i) {
    auto cut = *request_bytes; cut.resize(i); CHECK(!DecodeSettingsPublicationRequest(cut, &reason));
    auto altered = *request_bytes; altered[i] ^= 1; CHECK(!DecodeSettingsPublicationRequest(altered, &reason));
  }
  auto request_extra = *request_bytes; request_extra.push_back(0);
  CHECK(!DecodeSettingsPublicationRequest(request_extra, &reason));
  {
    // A program that starts in the install root is a version 2 request; others keep the version 1 bytes.
    auto rooted = request;
    rooted.install.launch_working_directory.clear(); rooted.plan.launch_entry.working_directory.clear();
    rooted.install.launch_in_install_root = rooted.plan.launch_entry.in_install_root = true;
    auto rooted_bytes = EncodeSettingsPublicationRequest(rooted, &reason); CHECK(rooted_bytes);
    CHECK(std::string(rooted_bytes->begin() + 4, rooted_bytes->begin() + 9) == "PWRQ2");
    CHECK(std::string(request_bytes->begin() + 4, request_bytes->begin() + 9) == "PWRQ1");
    auto rooted_back = DecodeSettingsPublicationRequest(*rooted_bytes, &reason); CHECK(rooted_back);
    CHECK(rooted_back->install.launch_in_install_root && rooted_back->plan.launch_entry.in_install_root);
    CHECK(EncodeSettingsPublicationRequest(*rooted_back, &reason) == rooted_bytes);
    rooted.plan.launch_entry.in_install_root = false;
    CHECK(!EncodeSettingsPublicationRequest(rooted, &reason));
  }
  auto invalid_request = request; ++invalid_request.plan.settings_revision;
  CHECK(!EncodeSettingsPublicationRequest(invalid_request, &reason));
  invalid_request = request; invalid_request.transaction_id = "00000000-0000-0000-0000-000000000000";
  CHECK(!EncodeSettingsPublicationRequest(invalid_request, &reason));
  invalid_request = request; invalid_request.plan.profile_bytes.resize(8193, 'x');
  CHECK(!EncodeSettingsPublicationRequest(invalid_request, &reason));
  auto source = Fixture(); auto frozen = FreezeSettingsPublicationOperation(source, &reason);
  if (!frozen) std::fprintf(stderr, "%s\n", reason.c_str()); CHECK(frozen);
  auto encoded = EncodeSettingsPublicationOperation(*frozen, &reason); CHECK(encoded);
  auto decoded = DecodeSettingsPublicationOperation(*encoded, &reason); CHECK(decoded);
  CHECK(EncodeSettingsPublicationOperation(*decoded, &reason) == encoded);
  {
    // Starting in the install root survives a restart: version 3 decodes and encodes to the same bytes and digest.
    auto rooted = Fixture();
    rooted.install.launch_in_install_root = rooted.plan.launch_entry.in_install_root = true;
    auto rooted_frozen = FreezeSettingsPublicationOperation(rooted, &reason); CHECK(rooted_frozen);
    auto rooted_bytes = EncodeSettingsPublicationOperation(*rooted_frozen, &reason); CHECK(rooted_bytes);
    CHECK(std::string(rooted_bytes->begin() + 4, rooted_bytes->begin() + 9) == "PWOP3");
    CHECK(std::string(encoded->begin() + 4, encoded->begin() + 9) == "PWOP2");
    auto rooted_back = DecodeSettingsPublicationOperation(*rooted_bytes, &reason); CHECK(rooted_back);
    CHECK(rooted_back->install.launch_in_install_root && rooted_back->plan.launch_entry.in_install_root &&
          rooted_back->plan_sha256 == rooted_frozen->plan_sha256);
    CHECK(EncodeSettingsPublicationOperation(*rooted_back, &reason) == rooted_bytes);
  }
  auto publication = RebuildFrozenProfilePublication(*decoded, &reason); CHECK(publication);
  CHECK(publication->profile_references_validated && publication->profile_bytes == decoded->plan.profile_bytes);
  CHECK(publication->input_presets.size() == 1 && publication->input_presets[0].filename == "gamepad.input");
  CHECK(decoded->install.launch_arguments.empty() && decoded->plan.profile_bytes.find("arguments = -steam") != std::string::npos);
  CHECK(decoded->install.artifacts[0].identity == "e16ec8f" && decoded->install.last_failure->message == "prior message");
  CHECK(decoded->install.journal->step == "profile");
  // Native title ownership is independent of the recorded Steam download root.
  // Adoption must preserve existing launcher downloads and selected volumes.
  for (const auto* download_root : {"/data/homebrew/FAKE77031/games/220", "/mnt/usb0/playden/games/220"}) {
    auto adopted = Fixture(); adopted.install.install_path = download_root;
    auto replace = [&](std::string from, std::string to) {
      auto& profile = adopted.plan.profile_bytes;
      for (std::size_t pos = 0; (pos = profile.find(from, pos)) != std::string::npos; pos += to.size()) profile.replace(pos, from.size(), to);
    };
    std::string windows = download_root; std::replace(windows.begin(), windows.end(), '/', '\\');
    replace("Z:\\data\\homebrew\\FAKE78001\\games\\220", "Z:" + windows);
    Refresh(adopted);
    auto adopted_frozen = FreezeSettingsPublicationOperation(adopted, &reason);
    CHECK(adopted_frozen);
    auto adopted_bytes = EncodeSettingsPublicationOperation(*adopted_frozen, &reason); CHECK(adopted_bytes);
    auto adopted_roundtrip = DecodeSettingsPublicationOperation(*adopted_bytes, &reason); CHECK(adopted_roundtrip);
    CHECK(adopted_roundtrip->install.install_path == download_root);
    auto wrong_root = adopted; wrong_root.install.install_path = "/data/homebrew/FAKE78001/games/220";
    CHECK(!FreezeSettingsPublicationOperation(wrong_root, &reason)); // Still bind the exact recorded root.
    // Schema 1 cannot express an adopted root; it infers the per-title root.
    CHECK(!FreezeSettingsPublicationOperation(Legacy(adopted), &reason));
  }
  // Legacy schema-1 descriptors still bind the historical per-title root.
  CHECK(FreezeSettingsPublicationOperation(Legacy(Fixture()), &reason));
  auto changed = *frozen; ++changed.install.settings_revision;
  CHECK(!EncodeSettingsPublicationOperation(changed, &reason));
  changed = *frozen; changed.plan.profile_bytes += "\n";
  CHECK(!EncodeSettingsPublicationOperation(changed, &reason));
  CHECK(!RebuildFrozenProfilePublication(changed, &reason));
  changed = *frozen; changed.pending_receipt_bytes = changed.committed_receipt_bytes;
  CHECK(!EncodeSettingsPublicationOperation(changed, &reason));
  changed = *frozen; changed.plan_sha256[0] = 'z'; CHECK(!EncodeSettingsPublicationOperation(changed, &reason));
  changed = *frozen; changed.install.launch_arguments = "adoption-arguments";
  CHECK(!EncodeSettingsPublicationOperation(changed, &reason)); // Whole record is frozen, not only launch bytes.
  auto legacy = *encoded; CHECK(legacy[8] == '2'); legacy[8] = '1';
  CHECK(!DecodeSettingsPublicationOperation(legacy, &reason)); // Older draft wire formats cannot prove recovery ownership.
  auto extra = *encoded; extra.push_back(0); CHECK(!DecodeSettingsPublicationOperation(extra, &reason));
  for (std::size_t i = 0; i < encoded->size(); ++i) {
    auto truncated = *encoded; truncated.resize(i); CHECK(!DecodeSettingsPublicationOperation(truncated, &reason));
  }
  for (std::size_t i = 0; i < encoded->size(); ++i) {
    auto mutated = *encoded; mutated[i] ^= 1;
    CHECK(!DecodeSettingsPublicationOperation(mutated, &reason));
  }
  auto oversized = *encoded; std::fill(oversized.begin(), oversized.begin() + 4, 255);
  CHECK(!DecodeSettingsPublicationOperation(oversized, &reason)); // Length checked before allocation.
  changed = source; changed.transaction_id = "00000000-0000-0000-0000-000000000000";
  CHECK(!FreezeSettingsPublicationOperation(changed, &reason));
  changed = source; changed.plan.input_presets[0].first = "Gamepad.input"; Refresh(changed);
  CHECK(!FreezeSettingsPublicationOperation(changed, &reason));
  changed = source; changed.plan.profile_bytes += "[unsupported]\nkey = value\n"; Refresh(changed);
  CHECK(!FreezeSettingsPublicationOperation(changed, &reason));
  changed = source; changed.plan.registry = {{"Software\\Wine\\AppDefaults\\probe.exe", "Version", "\"win7\""}};
  Refresh(changed); CHECK(!FreezeSettingsPublicationOperation(changed, &reason)); // Missing mandatory original backup.
  changed.registry_backup = RegistryBackupBinding{"registry-backup-" + changed.transaction_id + ".v1", 0644, 4, *HashArtifactBytes("old!")};
  CHECK(FreezeSettingsPublicationOperation(changed, &reason));
  changed.registry_backup->filename = "../other"; CHECK(!FreezeSettingsPublicationOperation(changed, &reason));
  changed = source; changed.install.settings_revision = changed.plan.settings_revision = std::numeric_limits<std::uint64_t>::max();
  changed.previous_revision = changed.plan.settings_revision - 1; Refresh(changed); BindPrevious(changed);
  CHECK(FreezeSettingsPublicationOperation(changed, &reason));
  // The before-snapshot keeps any existing dxvk.conf, also an empty one or one the game
  // ships, bound by its digest.
  changed = source; changed.dxvk_before = {true, 0644, 0, *HashArtifactBytes(""), ""};
  CHECK(FreezeSettingsPublicationOperation(changed, &reason));
  const std::string shipped = "# The game's own DXVK settings\ndxgi.syncInterval = 1\n";
  changed.dxvk_before = {true, 0755, shipped.size(), *HashArtifactBytes(shipped), shipped};
  CHECK(FreezeSettingsPublicationOperation(changed, &reason));
  changed.dxvk_before.sha256 = *HashArtifactBytes("other"); CHECK(!FreezeSettingsPublicationOperation(changed, &reason));
  changed = source; changed.previous_revision = 6; CHECK(!FreezeSettingsPublicationOperation(changed, &reason));
  BindPrevious(changed); auto with_previous = FreezeSettingsPublicationOperation(changed, &reason); CHECK(with_previous);
  auto prior_bytes = EncodeSettingsPublicationOperation(*with_previous, &reason); CHECK(prior_bytes);
  auto prior_recovered = DecodeSettingsPublicationOperation(*prior_bytes, &reason); CHECK(prior_recovered);
  CHECK(prior_recovered->previous_receipt_bytes == changed.previous_receipt_bytes);
  CHECK(prior_recovered->previous_metadata_bytes == changed.previous_metadata_bytes);
  auto drift = *with_previous; drift.previous_metadata_bytes += "\n";
  CHECK(!EncodeSettingsPublicationOperation(drift, &reason));
  drift = *with_previous; drift.previous_receipt_bytes.clear(); CHECK(!EncodeSettingsPublicationOperation(drift, &reason));
  drift = *with_previous; drift.transaction_id = "00000000-0000-7000-8000-000000000002";
  CHECK(!FreezeSettingsPublicationOperation(drift, &reason));
  drift = changed; drift.previous_revision = 5; CHECK(!FreezeSettingsPublicationOperation(drift, &reason));
  drift = changed; drift.previous_receipt_bytes = with_previous->pending_receipt_bytes;
  CHECK(!FreezeSettingsPublicationOperation(drift, &reason));
  changed = source; changed.registry_unix_time = -1; CHECK(!FreezeSettingsPublicationOperation(changed, &reason));
  changed = source; changed.registry_unix_time = std::numeric_limits<std::int64_t>::max();
  auto timed = FreezeSettingsPublicationOperation(changed, &reason); CHECK(timed);
  auto timed_bytes = EncodeSettingsPublicationOperation(*timed, &reason); CHECK(timed_bytes);
  auto timed_recovered = DecodeSettingsPublicationOperation(*timed_bytes, &reason);
  CHECK(timed_recovered && timed_recovered->registry_unix_time == changed.registry_unix_time);
  drift = *timed; --drift.registry_unix_time; CHECK(!EncodeSettingsPublicationOperation(drift, &reason));
  changed = source; changed.previous_revision = 8; CHECK(!FreezeSettingsPublicationOperation(changed, &reason));
  changed = source;
  const std::string header = "# Generated by Playden from this game's settings.\n#";
  changed.plan.dxvk_config = header + std::string(256 * 1024 - header.size(), 'x');
  changed.dxvk_before = {true, 0644, changed.plan.dxvk_config.size(), *HashArtifactBytes(changed.plan.dxvk_config), changed.plan.dxvk_config};
  for (unsigned i = 1; i < 64; ++i) changed.plan.input_presets.emplace_back("pad" + std::to_string(i) + ".input", "[input]\nmode = keyboard\n;");
  for (auto& input : changed.plan.input_presets) input.second.resize(8192, ' ');
  Refresh(changed); auto large = FreezeSettingsPublicationOperation(changed, &reason);
  if (!large) std::fprintf(stderr, "%s\n", reason.c_str()); CHECK(large);
  auto large_bytes = EncodeSettingsPublicationOperation(*large, &reason);
  CHECK(large_bytes && large_bytes->size() > 1024 * 1024 && large_bytes->size() <= kMaxSettingsPublicationOperationBytes);
  CHECK(DecodeSettingsPublicationOperation(*large_bytes, &reason));
  changed.plan.dxvk_config += "x"; CHECK(!FreezeSettingsPublicationOperation(changed, &reason));
  namespace fs = std::filesystem;
  char pattern[] = "/tmp/playden-settings-operation-XXXXXX"; CHECK(mkdtemp(pattern));
  const auto root = fs::canonical(pattern);
  CHECK(LoadSettingsPublicationRequest(root, request.transaction_id).state == PublicationRecoveryState::kAbsent);
  CHECK(CreateSettingsPublicationRequest(root, request).ok());
  CHECK(!CreateSettingsPublicationRequest(root, request).ok()); // Never replace a request, even with identical bytes.
  auto loaded_request = LoadSettingsPublicationRequest(root, request.transaction_id);
  CHECK(loaded_request.state == PublicationRecoveryState::kValid && loaded_request.request);
  CHECK(EncodeSettingsPublicationRequest(*loaded_request.request, &reason) == request_bytes);
  const auto request_file = root / ("settings-request-" + request.transaction_id + ".v1");
  // Any mode of the app user's is read (#868); a second link still makes it corrupt.
  CHECK(chmod(request_file.c_str(), 0777) == 0);
  CHECK(LoadSettingsPublicationRequest(root, request.transaction_id).state == PublicationRecoveryState::kValid);
  const auto alias = root / "request-alias";
  CHECK(link(request_file.c_str(), alias.c_str()) == 0);
  CHECK(LoadSettingsPublicationRequest(root, request.transaction_id).state == PublicationRecoveryState::kCorrupt);
  CHECK(unlink(alias.c_str()) == 0);
  CHECK(chmod(request_file.c_str(), 0600) == 0);
  const std::string another_transaction = "00000000-0000-7000-8000-000000000099";
  const auto other_request = root / ("settings-request-" + another_transaction + ".v1");
  fs::copy_file(request_file, other_request);
  CHECK(LoadSettingsPublicationRequest(root, another_transaction).state == PublicationRecoveryState::kCorrupt);
  fs::remove(other_request); fs::create_hard_link(request_file, other_request);
  CHECK(LoadSettingsPublicationRequest(root, request.transaction_id).state == PublicationRecoveryState::kCorrupt);
  fs::remove(other_request); fs::create_symlink(request_file, other_request);
  CHECK(LoadSettingsPublicationRequest(root, another_transaction).state == PublicationRecoveryState::kCorrupt);
  fs::remove(other_request);
  CHECK(LoadSettingsPublicationRequest(root, "../escape").state == PublicationRecoveryState::kCorrupt);
  CHECK(LoadSettingsPublicationOperation(root).state == PublicationRecoveryState::kAbsent);
  CHECK(CreateSettingsPublicationOperation(root, *frozen).ok());
  auto loaded = LoadSettingsPublicationOperation(root);
  CHECK(loaded.state == PublicationRecoveryState::kValid && loaded.operation);
  CHECK(EncodeSettingsPublicationOperation(*loaded.operation, &reason) == encoded);
  CHECK(!CreateSettingsPublicationOperation(root, *frozen).ok());
  changed = source; changed.install.launch_arguments = "adoption-only";
  auto refrozen = FreezeSettingsPublicationOperation(changed, &reason); CHECK(refrozen);
  CHECK(!CreateSettingsPublicationOperation(root, *refrozen).ok());
  loaded = LoadSettingsPublicationOperation(root);
  CHECK(loaded.state == PublicationRecoveryState::kValid && loaded.operation->install.launch_arguments.empty());
  CHECK(chmod((root / "settings-operation.v1").c_str(), 0644) == 0);
  CHECK(LoadSettingsPublicationOperation(root).state == PublicationRecoveryState::kCorrupt);
  CHECK(chmod((root / "settings-operation.v1").c_str(), 0600) == 0);
  fs::create_hard_link(root / "settings-operation.v1", root / "alias");
  CHECK(LoadSettingsPublicationOperation(root).state == PublicationRecoveryState::kCorrupt); fs::remove(root / "alias");
  std::ofstream(root / "settings-operation.v1", std::ios::binary) << "partial";
  CHECK(LoadSettingsPublicationOperation(root).state == PublicationRecoveryState::kCorrupt);
  CHECK(!CreateSettingsPublicationOperation(root, *frozen).ok());
  fs::remove(root / "settings-operation.v1");
  fs::create_symlink("missing", root / "settings-operation.v1");
  CHECK(LoadSettingsPublicationOperation(root).state == PublicationRecoveryState::kCorrupt); fs::remove(root / "settings-operation.v1");
  changed = source; changed.plan.registry = {{"Software\\Wine\\AppDefaults\\probe.exe", "Version", "\"win7\""}};
  Refresh(changed);
  auto backup = CreatePublicationRegistryBackup(root, changed.transaction_id, "old!", 0644); CHECK(backup.ok());
  changed.registry_backup = backup.value(); auto backed = FreezeSettingsPublicationOperation(changed, &reason); CHECK(backed);
  CHECK(CreateSettingsPublicationOperation(root, *backed).ok());
  CHECK(LoadSettingsPublicationOperation(root).state == PublicationRecoveryState::kValid);
  fs::remove(root / backed->registry_backup->filename);
  CHECK(LoadSettingsPublicationOperation(root).state == PublicationRecoveryState::kCorrupt);
  fs::remove(root / "settings-operation.v1");
  CHECK(!CreateSettingsPublicationOperation(root, *backed).ok());
  CHECK(!fs::exists(root / "settings-operation.v1"));
  CHECK(chmod(root.c_str(), 0755) == 0);
  CHECK(LoadSettingsPublicationOperation(root).state == PublicationRecoveryState::kUnavailable);
  CHECK(!CreateSettingsPublicationOperation(root, *frozen).ok());
  CHECK(chmod(root.c_str(), 0700) == 0);
  CHECK(!CreateSettingsPublicationOperation(root / "not-created", *frozen).ok());
  CHECK(!fs::exists(root / "not-created"));
  // A write failure leaves only retained private temporary evidence, never a
  // valid operation. Reconstructing the store observes absent, not Ready.
  const auto child = fork(); CHECK(child >= 0);
  if (!child) {
    struct rlimit limit{}; if (getrlimit(RLIMIT_FSIZE, &limit)) _exit(2);
    limit.rlim_cur = 16; if (setrlimit(RLIMIT_FSIZE, &limit)) _exit(3);
    std::signal(SIGXFSZ, SIG_IGN);
    _exit(CreateSettingsPublicationOperation(root, *frozen).ok() ? 4 : 0);
  }
  int status = 0;
  CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
  CHECK(LoadSettingsPublicationOperation(root).state == PublicationRecoveryState::kAbsent);
  bool partial = false;
  for (const auto& entry : fs::directory_iterator(root)) {
    partial = true; struct stat info{};
    CHECK(lstat(entry.path().c_str(), &info) == 0 && S_ISREG(info.st_mode) && (info.st_mode & 07777) == 0600);
  }
  CHECK(partial);
  // Storage primitive only: commit/Ready authorization is independently tested
  // by the coordinator/OWNER suite. Never overwrite an archive destination.
  for (unsigned scenario = 0; scenario < 3; ++scenario) {
    const auto archive_root = root / ("archive-case-" + std::to_string(scenario));
    fs::create_directory(archive_root); CHECK(chmod(archive_root.c_str(), 0700) == 0);
    CHECK(CreateSettingsPublicationOperation(archive_root, *frozen).ok());
    const auto archive = archive_root / ("settings-operation-" + frozen->transaction_id + ".v1");
    if (scenario == 0) {
      std::ofstream(archive) << "foreign"; CHECK(chmod(archive.c_str(), 0600) == 0);
      CHECK(!ArchiveSettingsPublicationOperation(archive_root, *frozen).ok());
      CHECK(LoadSettingsPublicationOperation(archive_root).state == PublicationRecoveryState::kValid);
      std::ifstream file(archive); std::string bytes{std::istreambuf_iterator<char>(file), {}}; CHECK(bytes == "foreign");
    } else {
      if (scenario == 1) {
        const auto publisher = fork(); CHECK(publisher >= 0);
        if (publisher == 0) {
          ArchiveSettingsPublicationOperation(archive_root, *frozen, [] { _exit(77); return false; }); _exit(78);
        }
        int exit_status = 0; CHECK(waitpid(publisher, &exit_status, 0) == publisher && WIFEXITED(exit_status) && WEXITSTATUS(exit_status) == 77);
      }
      CHECK(ArchiveSettingsPublicationOperation(archive_root, *frozen).ok());
      CHECK(ArchiveSettingsPublicationOperation(archive_root, *frozen).ok());
      CHECK(LoadSettingsPublicationOperation(archive_root).state == PublicationRecoveryState::kAbsent);
      struct stat info{}; CHECK(lstat(archive.c_str(), &info) == 0 && info.st_nlink == 1 && (info.st_mode & 07777) == 0600);
      CHECK(chmod(archive.c_str(), 0644) == 0); CHECK(!ArchiveSettingsPublicationOperation(archive_root, *frozen).ok());
      CHECK(chmod(archive.c_str(), 0600) == 0);
      std::ofstream(archive, std::ios::binary) << "corrupt"; CHECK(!ArchiveSettingsPublicationOperation(archive_root, *frozen).ok());
    }
  }
  const auto intent_root = root / "intent"; fs::create_directory(intent_root); CHECK(chmod(intent_root.c_str(), 0700) == 0);
  CHECK(LoadPreparedSettingsPublicationOperation(intent_root, frozen->transaction_id).state == PublicationRecoveryState::kAbsent);
  CHECK(CreatePreparedSettingsPublicationOperation(intent_root, *frozen).ok());
  CHECK(!CreatePreparedSettingsPublicationOperation(intent_root, *frozen).ok());
  auto intent = LoadPreparedSettingsPublicationOperation(intent_root, frozen->transaction_id);
  CHECK(intent.state == PublicationRecoveryState::kValid && EncodeSettingsPublicationOperation(*intent.operation, &reason) == encoded);
  CHECK(LoadPreparedSettingsPublicationOperation(intent_root, "../invalid").state == PublicationRecoveryState::kCorrupt);
  CHECK(LoadPreparedSettingsPublicationOperation(intent_root, "00000000-0000-0000-0000-000000000000").state == PublicationRecoveryState::kCorrupt);
  const auto intent_name = intent_root / ("settings-intent-" + frozen->transaction_id + ".v1");
  CHECK(chmod(intent_name.c_str(), 0644) == 0);
  CHECK(LoadPreparedSettingsPublicationOperation(intent_root, frozen->transaction_id).state == PublicationRecoveryState::kCorrupt);
  CHECK(chmod(intent_name.c_str(), 0600) == 0);
  fs::create_hard_link(intent_name, intent_root / "alias");
  CHECK(LoadPreparedSettingsPublicationOperation(intent_root, frozen->transaction_id).state == PublicationRecoveryState::kCorrupt);
  fs::remove(intent_root / "alias");
  fs::rename(intent_name, intent_root / "retained");
  fs::create_symlink("retained", intent_name);
  CHECK(LoadPreparedSettingsPublicationOperation(intent_root, frozen->transaction_id).state == PublicationRecoveryState::kCorrupt);
  fs::remove(intent_name); fs::rename(intent_root / "retained", intent_name);
  const auto other_intent = "00000000-0000-7000-8000-000000000004";
  fs::rename(intent_name, intent_root / (std::string("settings-intent-") + other_intent + ".v1"));
  CHECK(LoadPreparedSettingsPublicationOperation(intent_root, other_intent).state == PublicationRecoveryState::kCorrupt);
  const auto fault_root = root / "intent-write-fault"; fs::create_directory(fault_root); CHECK(chmod(fault_root.c_str(), 0700) == 0);
  const auto intent_writer = fork(); CHECK(intent_writer >= 0);
  if (!intent_writer) {
    struct rlimit limit{16, 16}; CHECK(setrlimit(RLIMIT_FSIZE, &limit) == 0); std::signal(SIGXFSZ, SIG_IGN);
    _exit(CreatePreparedSettingsPublicationOperation(fault_root, *frozen).ok() ? 78 : 77);
  }
  int intent_status = 0; CHECK(waitpid(intent_writer, &intent_status, 0) == intent_writer && WIFEXITED(intent_status) && WEXITSTATUS(intent_status) == 77);
  CHECK(LoadPreparedSettingsPublicationOperation(fault_root, frozen->transaction_id).state == PublicationRecoveryState::kAbsent);
  CHECK(CreatePreparedSettingsPublicationOperation(fault_root, *frozen).ok());
  fs::remove_all(root);
  std::puts("settings publication operation tests passed");
}
