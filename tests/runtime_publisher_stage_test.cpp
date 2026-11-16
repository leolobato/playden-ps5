#include "runtime/artifact_hash.h"
#include "runtime/publisher_stage.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(condition)                                                                  \
  do {                                                                                    \
    if (!(condition)) {                                                                   \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
      std::exit(1);                                                                       \
    }                                                                                     \
  } while (false)

namespace fs = std::filesystem;
using namespace playden::runtime;

namespace {

// A made-up publisher: an x86 proxy that keeps the game's original, a helper per arch, a marker
// in a hidden folder and an ini.
const std::string kManifest = R"({
  "version": 1,
  "account": {
    "name": "Example Publisher",
    "display_fields": ["name"],
    "stage_fields": ["token"],
    "stage_pattern": { "charset": "base64", "min": 8, "max": 64 },
    "max_bytes": 4096
  },
  "runtime": {
    "label": "Example Runtime",
    "installers": ["Redist/Example-Setup.exe"],
    "version_file": "x86/runtime.dll",
    "folders": { "x86": "x86" }
  },
  "titles": {
    "4000": { "arch": "x86", "args": "-one -two=4000" },
    "4001": { "arch": "x64", "args": "" }
  },
  "files": [
    { "source": "proxy32.dll", "target": "proxy.dll", "keep_original": "proxy_real.dll", "arch": "x86" },
    { "source": "helper32.dll", "target": "helper.dll", "arch": "x86" },
    { "source": "helper64.dll", "target": "x64/helper.dll", "arch": "x64" },
    { "text": "", "target": ".examplepub/marker" }
  ],
  "ini": { "target": "helper.ini", "lines": ["args=${title.args}", "root=${runtime}\\x86", "debug=0"] },
  "credential_file": "credential.txt",
  "status": { "file": ".examplepub/status", "expired": "state=stale" }
})";

std::string Read(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), {}};
}

void Write(const fs::path& path, const std::string& bytes) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary | std::ios::trunc) << bytes;
}

unsigned Mode(const fs::path& path) {
  struct stat info {};
  CHECK(::lstat(path.c_str(), &info) == 0);
  return info.st_mode & 07777;
}

struct Fixture {
  fs::path root, pack, title;
  PublisherStageManifest manifest;
  Fixture() {
    char name[] = "/tmp/playden-publisher-stage-XXXXXX";
    CHECK(::mkdtemp(name));
    root = fs::canonical(name);
    pack = root / "pack";
    title = root / "game/Title";
    Write(pack / "proxy32.dll", "proxy-32");
    Write(pack / "helper32.dll", "helper-32");
    Write(pack / "helper64.dll", "helper-64");
    Write(title / "proxy.dll", "game-proxy");
    Write(title / "game.exe", "game");
    std::string reason;
    CHECK(ParsePublisherStageManifest(kManifest, &manifest, &reason));
  }
  ~Fixture() {
    std::error_code ignored;
    fs::remove_all(root, ignored);
  }
  PublisherStageResult Apply(const std::string& app = "4000") {
    return ApplyPublisherStage(manifest, pack.string(), title.string(), app, "Z:\\data\\runtime\\1.0");
  }
};

bool Has(const std::vector<std::string>& list, const std::string& value) {
  for (const auto& item : list)
    if (item == value) return true;
  return false;
}

void FirstStagePlacesEverything() {
  Fixture f;
  const auto result = f.Apply();
  CHECK(result.ok);
  CHECK((result.changed ==
         std::vector<std::string>{"proxy_real.dll", "proxy.dll", "helper.dll", ".examplepub/marker", "helper.ini"}));
  CHECK(Read(f.title / "proxy.dll") == "proxy-32");
  CHECK(Read(f.title / "proxy_real.dll") == "game-proxy");
  CHECK(Read(f.title / "helper.dll") == "helper-32");
  CHECK(fs::is_regular_file(f.title / ".examplepub/marker") && Read(f.title / ".examplepub/marker").empty());
  CHECK(Mode(f.title / ".examplepub") == 0755);
  CHECK(Read(f.title / "helper.ini") == "args=-one -two=4000\r\nroot=Z:\\data\\runtime\\1.0\\x86\r\ndebug=0\r\n");
  CHECK(!fs::exists(f.title / "credential.txt"));  // only Play writes it

  // Staging again changes nothing: the proxy is already in place, nothing moves.
  const auto again = f.Apply();
  CHECK(again.ok && again.changed.empty());
  CHECK(Read(f.title / "proxy_real.dll") == "game-proxy");
}

void ExistingFolderModesStay() {
  Fixture f;
  fs::create_directories(f.title / ".examplepub");
  CHECK(::chmod((f.title / ".examplepub").c_str(), 0700) == 0);
  CHECK(::chmod(f.title.c_str(), 0700) == 0);
  CHECK(f.Apply().ok);
  CHECK(Mode(f.title / ".examplepub") == 0700 && Mode(f.title) == 0700);
}

void GameUpdateReplacesTheKeptOriginal() {
  Fixture f;
  CHECK(f.Apply().ok);
  // The game updated its file over the proxy: the update becomes the kept original.
  Write(f.title / "proxy.dll", "game-proxy-2");
  const auto result = f.Apply();
  CHECK(result.ok && (result.changed == std::vector<std::string>{"proxy_real.dll", "proxy.dll"}));
  CHECK(Read(f.title / "proxy.dll") == "proxy-32" && Read(f.title / "proxy_real.dll") == "game-proxy-2");
  // A changed ini is written again.
  Write(f.title / "helper.ini", "edited");
  const auto ini = f.Apply();
  CHECK(ini.ok && (ini.changed == std::vector<std::string>{"helper.ini"}));
}

// A pack update brings a new proxy: the old one (recorded as Playden's) is replaced in place and the
// game's original stays kept; restore removes an old proxy the same way.
void PackUpdateKeepsTheOriginal() {
  Fixture f;
  CHECK(f.Apply().ok);
  const auto record = Read(f.title / kPublisherStageRecordName);
  CHECK(record.find("\"pack\":\"pack\"") != std::string::npos);
  CHECK(record.find("\"proxy.dll\":\"" + HashArtifactBytes("proxy-32").value() + "\"") != std::string::npos);
  Write(f.pack / "proxy32.dll", "proxy-32-v2");
  const auto result = f.Apply();
  CHECK(result.ok && (result.changed == std::vector<std::string>{"proxy.dll"}));
  CHECK(Read(f.title / "proxy.dll") == "proxy-32-v2" && Read(f.title / "proxy_real.dll") == "game-proxy");
  CHECK(Read(f.title / kPublisherStageRecordName).find(HashArtifactBytes("proxy-32-v2").value()) != std::string::npos);
  // The pack changes again before the next stage: restore still knows the placed proxy.
  Write(f.pack / "proxy32.dll", "proxy-32-v3");
  const auto restored = RestorePublisherStage(f.manifest, f.pack.string(), f.title.string(), "4000");
  CHECK(restored.ok && Read(f.title / "proxy.dll") == "game-proxy" && !fs::exists(f.title / "proxy_real.dll"));
  CHECK(!fs::exists(f.title / kPublisherStageRecordName));
}

// The game updates its file over a recorded proxy: the update replaces the kept original.
void GameUpdateOverRecordedProxy() {
  Fixture f;
  CHECK(f.Apply().ok);
  Write(f.pack / "proxy32.dll", "proxy-32-v2");  // a pack update at the same time
  Write(f.title / "proxy.dll", "game-proxy-2");
  const auto result = f.Apply();
  CHECK(result.ok && (result.changed == std::vector<std::string>{"proxy_real.dll", "proxy.dll"}));
  CHECK(Read(f.title / "proxy.dll") == "proxy-32-v2" && Read(f.title / "proxy_real.dll") == "game-proxy-2");
}

// No record, but a kept original exists: the target is taken as Playden's and the original stays.
void MissingRecordKeepsTheOriginal() {
  Fixture f;
  Write(f.title / "proxy.dll", "older-proxy");
  Write(f.title / "proxy_real.dll", "game-proxy");
  const auto result = f.Apply();
  CHECK(result.ok && result.changed.front() == "proxy.dll");
  CHECK(Read(f.title / "proxy.dll") == "proxy-32" && Read(f.title / "proxy_real.dll") == "game-proxy");
  // A damaged record counts as none.
  Write(f.title / kPublisherStageRecordName, "{damaged");
  Write(f.title / "proxy.dll", "older-proxy");
  CHECK(f.Apply().ok && Read(f.title / "proxy_real.dll") == "game-proxy");
}

void ArchFiltersFiles() {
  Fixture f;
  const auto result = f.Apply("4001");
  CHECK(result.ok);
  CHECK(Read(f.title / "x64/helper.dll") == "helper-64" && !fs::exists(f.title / "helper.dll"));
  CHECK(Read(f.title / "proxy.dll") == "game-proxy" && !fs::exists(f.title / "proxy_real.dll"));
  CHECK(Read(f.title / "helper.ini").rfind("args=\r\n", 0) == 0);
  // Restore of the x64 title leaves the x86 entries alone.
  const auto restored = RestorePublisherStage(f.manifest, f.pack.string(), f.title.string(), "4001");
  CHECK(restored.ok && !fs::exists(f.title / "x64/helper.dll") && Read(f.title / "proxy.dll") == "game-proxy");
}

// The x86 and the x64 proxy share a target and its kept original: each title gets its own, and its
// restore puts the game's file back.
void ArchesShareATarget() {
  Fixture f;
  const auto marker = std::string("{ \"text\": \"\", \"target\": \".examplepub/marker\" }");
  const auto text = std::string(kManifest).replace(
      kManifest.find(marker), marker.size(),
      "{ \"source\": \"proxy64.dll\", \"target\": \"proxy.dll\", \"keep_original\": \"proxy_real.dll\", \"arch\": \"x64\" }, " +
          marker);
  std::string reason;
  CHECK(ParsePublisherStageManifest(text, &f.manifest, &reason));
  Write(f.pack / "proxy64.dll", "proxy-64");
  CHECK(f.Apply("4001").ok);
  CHECK(Read(f.title / "proxy.dll") == "proxy-64" && Read(f.title / "proxy_real.dll") == "game-proxy");
  const auto restored = RestorePublisherStage(f.manifest, f.pack.string(), f.title.string(), "4001");
  CHECK(restored.ok && Read(f.title / "proxy.dll") == "game-proxy" && !fs::exists(f.title / "proxy_real.dll"));
  CHECK(f.Apply("4000").ok);
  CHECK(Read(f.title / "proxy.dll") == "proxy-32" && Read(f.title / "proxy_real.dll") == "game-proxy");
  const auto back = RestorePublisherStage(f.manifest, f.pack.string(), f.title.string(), "4000");
  CHECK(back.ok && Read(f.title / "proxy.dll") == "game-proxy" && !fs::exists(f.title / "proxy_real.dll"));
}

void UnlistedTitleIsRefused() {
  Fixture f;
  const auto result = f.Apply("4002");
  CHECK(!result.ok && result.message.find("4002") != std::string::npos && result.changed.empty());
  CHECK(Read(f.title / "proxy.dll") == "game-proxy" && !fs::exists(f.title / "helper.ini"));
}

void RestorePutsOriginalsBack() {
  Fixture f;
  CHECK(f.Apply().ok);
  std::string value = "c2VjcmV0dG9rZW4=";
  std::string reason;
  CHECK(WritePublisherCredential(f.manifest, f.title.string(), value, &reason));
  Write(f.title / ".examplepub/status", "state=stale");  // the game's, not ours
  Write(f.title / "other.dll", "game-other");
  const auto result = RestorePublisherStage(f.manifest, f.pack.string(), f.title.string(), "4000");
  CHECK(result.ok);
  CHECK(Read(f.title / "proxy.dll") == "game-proxy" && !fs::exists(f.title / "proxy_real.dll"));
  CHECK(!fs::exists(f.title / "helper.dll") && !fs::exists(f.title / ".examplepub/marker"));
  CHECK(!fs::exists(f.title / "helper.ini") && !fs::exists(f.title / "credential.txt"));
  CHECK(Has(result.changed, "proxy.dll") && Has(result.changed, "credential.txt"));
  // Nothing else: the game's files, the status file and the folders stay.
  CHECK(Read(f.title / "game.exe") == "game" && Read(f.title / "other.dll") == "game-other");
  CHECK(fs::is_regular_file(f.title / ".examplepub/status") && fs::is_directory(f.title / ".examplepub"));
  // Restoring again is a no-op.
  const auto again = RestorePublisherStage(f.manifest, f.pack.string(), f.title.string(), "4000");
  CHECK(again.ok && again.changed.empty());
}

void RestoreKeepsWhatTheGameChanged() {
  Fixture f;
  CHECK(f.Apply().ok);
  Write(f.title / "proxy.dll", "game-proxy-3");  // updated by the game after staging
  Write(f.title / "helper.dll", "game-helper");  // the game ships its own now
  const auto result = RestorePublisherStage(f.manifest, f.pack.string(), f.title.string(), "4000");
  CHECK(result.ok);
  CHECK(Read(f.title / "proxy.dll") == "game-proxy-3" && Read(f.title / "proxy_real.dll") == "game-proxy");
  CHECK(Read(f.title / "helper.dll") == "game-helper");
}

void LinksAreRefused() {
  {
    // A target that is a link is never written through.
    Fixture f;
    Write(f.root / "outside.dll", "outside");
    CHECK(::symlink((f.root / "outside.dll").c_str(), (f.title / "helper.dll").c_str()) == 0);
    const auto result = f.Apply();
    CHECK(!result.ok && result.message.find("helper.dll") != std::string::npos);
    CHECK(Read(f.root / "outside.dll") == "outside");
  }
  {
    // A linked folder on the way is not followed.
    Fixture f;
    fs::create_directories(f.root / "elsewhere");
    CHECK(::symlink((f.root / "elsewhere").c_str(), (f.title / ".examplepub").c_str()) == 0);
    const auto result = f.Apply();
    CHECK(!result.ok && fs::is_empty(f.root / "elsewhere"));
    std::string value = "c2VjcmV0dG9rZW4=", reason;
    CHECK(WritePublisherCredential(f.manifest, f.title.string(), value, &reason));  // not in that folder
  }
  {
    // A linked title folder is refused.
    Fixture f;
    CHECK(::symlink(f.title.c_str(), (f.root / "linked").c_str()) == 0);
    CHECK(!ApplyPublisherStage(f.manifest, f.pack.string(), (f.root / "linked").string(), "4000", "Z:\\r").ok);
    std::string value = "c2VjcmV0dG9rZW4=", reason;
    CHECK(!WritePublisherCredential(f.manifest, (f.root / "linked").string(), value, &reason));
    CHECK(value.find_first_not_of('\0') == std::string::npos);  // wiped even on refusal
  }
  {
    // A credential file that is a link is refused.
    Fixture f;
    Write(f.root / "outside.txt", "outside");
    CHECK(::symlink((f.root / "outside.txt").c_str(), (f.title / "credential.txt").c_str()) == 0);
    std::string value = "c2VjcmV0dG9rZW4=", reason;
    CHECK(!WritePublisherCredential(f.manifest, f.title.string(), value, &reason));
    CHECK(Read(f.root / "outside.txt") == "outside");
    CHECK(!RemovePublisherCredential(f.manifest, f.title.string(), &reason));
  }
}

void CredentialIsPrivateAndRemovable() {
  Fixture f;
  ::umask(022);
  std::string value = "c2VjcmV0dG9rZW4=", reason;
  CHECK(WritePublisherCredential(f.manifest, f.title.string(), value, &reason));
  CHECK(value.find_first_not_of('\0') == std::string::npos);
  CHECK(Read(f.title / "credential.txt") == "c2VjcmV0dG9rZW4=\n" && Mode(f.title / "credential.txt") == 0600);
  // A wider file left behind is replaced by a private one.
  CHECK(::chmod((f.title / "credential.txt").c_str(), 0644) == 0);
  value = "b3RoZXJ0b2tlbg==";
  CHECK(WritePublisherCredential(f.manifest, f.title.string(), value, &reason));
  CHECK(Read(f.title / "credential.txt") == "b3RoZXJ0b2tlbg==\n" && Mode(f.title / "credential.txt") == 0600);
  CHECK(RemovePublisherCredential(f.manifest, f.title.string(), &reason) && !fs::exists(f.title / "credential.txt"));
  CHECK(RemovePublisherCredential(f.manifest, f.title.string(), &reason));  // missing is fine
}

void MissingPackFileFails() {
  Fixture f;
  fs::remove(f.pack / "helper32.dll");
  const auto result = f.Apply();
  CHECK(!result.ok && result.message.find("helper32.dll") != std::string::npos);
}

// A game file already identical to the pack's is the game's: it isn't recorded, restore keeps it.
void IdenticalGameFileIsNotRecorded() {
  Fixture f;
  Write(f.title / "helper.dll", "helper-32");
  CHECK(f.Apply().ok);
  CHECK(Read(f.title / kPublisherStageRecordName).find("\"helper.dll\"") == std::string::npos);
  const auto restored = RestorePublisherStage(f.manifest, f.pack.string(), f.title.string(), "4000");
  CHECK(restored.ok && Read(f.title / "helper.dll") == "helper-32");
  CHECK(!fs::exists(f.title / "proxy_real.dll") && Read(f.title / "proxy.dll") == "game-proxy");
}

// Without a manifest the record alone takes everything back, credential included.
void RestoreFromTheRecordAlone() {
  Fixture f;
  CHECK(f.Apply().ok);
  std::string value = "c2VjcmV0dG9rZW4=", reason;
  CHECK(WritePublisherCredential(f.manifest, f.title.string(), value, &reason));
  Write(f.title / "other.dll", "game-other");
  const auto result = RestorePublisherRecord(f.title.string());
  CHECK(result.ok);
  CHECK(Read(f.title / "proxy.dll") == "game-proxy" && !fs::exists(f.title / "proxy_real.dll"));
  CHECK(!fs::exists(f.title / "helper.dll") && !fs::exists(f.title / "helper.ini"));
  CHECK(!fs::exists(f.title / "credential.txt") && !fs::exists(f.title / kPublisherStageRecordName));
  CHECK(Read(f.title / "other.dll") == "game-other" && Read(f.title / "game.exe") == "game");
  CHECK(RestorePublisherRecord(f.title.string()).ok);  // again: nothing to do
  CHECK(RestorePublisherRecord((f.root / "missing").string()).ok);
}

// A newer pack drops entries: restore still takes back what the record lists.
void RestoreTakesBackEntriesTheManifestDropped() {
  Fixture f;
  CHECK(f.Apply().ok);
  f.manifest.files.clear();
  const auto result = RestorePublisherStage(f.manifest, f.pack.string(), f.title.string(), "4000");
  CHECK(result.ok);
  CHECK(Read(f.title / "proxy.dll") == "game-proxy" && !fs::exists(f.title / "proxy_real.dll"));
  CHECK(!fs::exists(f.title / "helper.dll") && !fs::exists(f.title / kPublisherStageRecordName));
}

}  // namespace

int main() {
  FirstStagePlacesEverything();
  ExistingFolderModesStay();
  GameUpdateReplacesTheKeptOriginal();
  PackUpdateKeepsTheOriginal();
  GameUpdateOverRecordedProxy();
  MissingRecordKeepsTheOriginal();
  ArchFiltersFiles();
  ArchesShareATarget();
  UnlistedTitleIsRefused();
  RestorePutsOriginalsBack();
  RestoreKeepsWhatTheGameChanged();
  LinksAreRefused();
  CredentialIsPrivateAndRemovable();
  MissingPackFileFails();
  IdenticalGameFileIsNotRecorded();
  RestoreFromTheRecordAlone();
  RestoreTakesBackEntriesTheManifestDropped();
  std::puts("runtime_publisher_stage_test passed");
  return 0;
}
