// SPDX-License-Identifier: GPL-3.0-or-later
// GOG updates (gog/update_diff, gog/plan_file, the planner's layouts, GogInstaller::Stage, stores/gog_update): synthetic
// plans written with plan_file.h against files on disk, and synthetic builds (gog_synthetic_game.h) planned through the
// stub CDN into an update or measure layout beside a live install, then staged beside it; hand-made work folders and
// Stage's output put in place. Usage: gog_update_test <fixtures/gog>
// <scratch folder>.

#include "gog/chunk_fetch.h"
#include "gog/dependencies.h"
#include "gog/installer.h"
#include "gog/path_rules.h"
#include "gog/plan_file.h"
#include "gog/update_diff.h"
#include "gog_cdn_stub.h"
#include "gog_synthetic_game.h"
#include "installs/install_record.h"
#include "steam/backend/depot_installer.h"
#include "stores/gog_update.h"

#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace gog_test;
using playden::gog::DiffPlans;
using playden::gog::DiffResult;
using playden::gog::GogInstaller;
using playden::gog::PlanEntry;
using playden::gog::PlanHeader;
using Error = GogInstaller::Outcome::Error;
namespace fs = std::filesystem;
namespace gog = playden::gog;

std::string g_fixtures;
std::string g_scratch;

constexpr std::int64_t kNow = 1800000000;

std::string Root(const std::string& name) {
  const auto root = g_scratch + "/" + name;
  fs::create_directories(root);
  return root;
}

void WriteBytes(const std::string& path, const std::string& bytes) {
  fs::create_directories(fs::path(path).parent_path());
  FILE* file = std::fopen(path.c_str(), "wb");
  CHECK(file && std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size() && std::fclose(file) == 0);
}

bool Exists(const std::string& path) {
  struct stat info {};
  return lstat(path.c_str(), &info) == 0;
}

// ---- Synthetic plans ----------------------------------------------------------------------------------------------

// A gen 2 file of `bytes` in chunks of `pieces` (one chunk when empty), with its MD5 when `file_md5`.
PlanEntry Gen2File(const std::string& path, const std::string& bytes, bool file_md5 = true,
                   std::vector<std::size_t> pieces = {}, const std::string& source = "1900000001") {
  PlanEntry entry;
  entry.path = path;
  entry.source = source;
  entry.size = bytes.size();
  if (file_md5 && !bytes.empty()) entry.md5 = Md5Hex(bytes);
  if (pieces.empty() && !bytes.empty()) pieces.push_back(bytes.size());
  std::size_t at = 0;
  for (const auto piece : pieces) {
    const auto raw = bytes.substr(at, piece);
    at += piece;
    gog::DepotChunk chunk;
    chunk.md5 = Md5Hex(raw);
    chunk.size = raw.size();
    chunk.compressed_md5 = Md5Hex(raw + "z");
    chunk.compressed_size = raw.size() / 2 + 1;
    entry.units.chunks.push_back(chunk);
  }
  CHECK(at == bytes.size());
  return entry;
}

// A gen 1 file: ranges of its product's main.bin, its whole MD5.
PlanEntry Gen1File(const std::string& path, const std::string& bytes, std::uint64_t offset = 100) {
  PlanEntry entry;
  entry.path = path;
  entry.source = "1900000001";
  entry.size = bytes.size();
  entry.md5 = Md5Hex(bytes);
  for (std::uint64_t at = 0; at < bytes.size(); at += gog::kGen1RangeBytes)
    entry.units.ranges.push_back(
        {offset + at, std::min<std::uint64_t>(gog::kGen1RangeBytes, bytes.size() - at)});
  return entry;
}

PlanEntry Dir(const std::string& path) {
  PlanEntry entry;
  entry.directory = true;
  entry.path = path;
  return entry;
}

PlanHeader Header(int generation, const std::string& build = "1000000000000000101") {
  PlanHeader header;
  header.generation = generation;
  header.build_id = build;
  header.meta_id = std::string(32, 'a');
  header.language = "en-US";
  header.products = {kBase};
  header.timestamp = generation == 1 ? kTimestampV1 : 0;
  return header;
}

// Writes the plan, its totals from the entries.
void WritePlan(const std::string& path, PlanHeader header, const std::vector<PlanEntry>& entries) {
  for (const auto& entry : entries) {
    if (entry.directory) continue;
    header.total_size += entry.size;
    header.total_compressed += entry.download();
  }
  std::string text = gog::HeaderLine(header) + "\n";
  for (const auto& entry : entries) text += gog::EntryText(entry);
  WriteBytes(path, text);
  // The test's own plans must read: a plan that doesn't would turn an old-plan case into a hashing one.
  gog::EntryReader reader(path);
  PlanHeader again;
  CHECK(reader.open() && reader.Header(&again));
  PlanEntry entry;
  std::size_t count = 0;
  for (gog::EntryReader::Next next; (next = reader.Read(&entry)) == gog::EntryReader::Next::kEntry;) ++count;
  CHECK(count == entries.size());
}

// Same-sized bytes that hash to nothing a plan names: an old-plan case never reads them.
std::string Unrelated(std::size_t size) { return std::string(size, '#'); }

struct Diffed {
  DiffResult result;
  std::map<std::string, bool> changed;  // by path
  std::vector<std::uint64_t> indexes;
};

Diffed Diff(const std::string& old_plan, const std::string& new_plan, const std::string& root,
            const std::atomic<bool>* cancel = nullptr,
            const std::function<void(const PlanEntry&)>& seen = {}) {
  static const std::atomic<bool> kNever{false};
  Diffed diffed;
  diffed.result = DiffPlans(old_plan, new_plan, root, cancel ? *cancel : kNever,
                            [&](std::uint64_t index, const PlanEntry& entry, bool changed) {
                              diffed.indexes.push_back(index);
                              diffed.changed[entry.path] = changed;
                              if (seen) seen(entry);
                            });
  return diffed;
}

const std::string kA = RandomBytes(3000, 11);
const std::string kB = RandomBytes(5000, 12);
const std::string kC = RandomBytes(700, 13);

// ---- The diff -----------------------------------------------------------------------------------------------------

void TestIdentical() {
  const auto root = Root("identical");
  const std::vector<PlanEntry> entries{Dir("logs"), Gen2File("bin/a.exe", kA), Gen2File("data/b.dat", kB, true, {2000, 3000}),
                                       Gen2File("empty.txt", "")};
  WritePlan(root + "/old", Header(2), entries);
  WritePlan(root + "/new", Header(2), entries);
  WriteBytes(root + "/bin/a.exe", Unrelated(kA.size()));
  WriteBytes(root + "/data/b.dat", Unrelated(kB.size()));
  WriteBytes(root + "/empty.txt", "");
  const auto diffed = Diff(root + "/old", root + "/new", root);
  const auto& r = diffed.result;
  CHECK(r.readable && r.old_plan && !r.canceled);
  CHECK(r.changed_files == 0 && r.unchanged_files == 3 && r.removed_files == 0 && r.hashed_files == 0);
  CHECK(r.staged_bytes == 0 && r.download_bytes == 0 && r.install_bytes == kA.size() + kB.size());
  CHECK(!diffed.changed.at("logs") && !diffed.changed.at("bin/a.exe") && !diffed.changed.at("empty.txt"));
  CHECK((diffed.indexes == std::vector<std::uint64_t>{0, 1, 2, 3}));
}

void TestOneMd5Changed() {
  const auto root = Root("md5-changed");
  const auto new_b = RandomBytes(kB.size(), 99);
  WritePlan(root + "/old", Header(2), {Gen2File("a", kA), Gen2File("b", kB)});
  const auto changed = Gen2File("b", new_b);
  WritePlan(root + "/new", Header(2), {Gen2File("a", kA), changed});
  WriteBytes(root + "/a", Unrelated(kA.size()));
  WriteBytes(root + "/b", Unrelated(kB.size()));
  const auto diffed = Diff(root + "/old", root + "/new", root);
  const auto& r = diffed.result;
  CHECK(r.old_plan && r.hashed_files == 0 && r.changed_files == 1 && r.unchanged_files == 1);
  CHECK(!diffed.changed.at("a") && diffed.changed.at("b"));
  CHECK(r.staged_bytes == new_b.size() && r.download_bytes == changed.download());
}

// The same MD5 claimed for another size is another file.
void TestSameMd5OtherSize() {
  const auto root = Root("md5-size");
  WritePlan(root + "/old", Header(2), {Gen2File("a", kA)});
  auto longer = Gen2File("a", kA + "x");
  longer.md5 = Md5Hex(kA);
  WritePlan(root + "/new", Header(2), {longer});
  WriteBytes(root + "/a", Unrelated(kA.size() + 1));
  const auto r = Diff(root + "/old", root + "/new", root).result;
  CHECK(r.old_plan && r.changed_files == 1 && r.unchanged_files == 0 && r.hashed_files == 0);
}

// No file MD5 on either side: the chunk lists decide.
void TestChunkLists() {
  const auto root = Root("chunks");
  const auto old_a = Gen2File("a", kA, false, {1000, 2000});
  WritePlan(root + "/old", Header(2), {old_a});
  WriteBytes(root + "/a", Unrelated(kA.size()));
  WritePlan(root + "/same", Header(2), {Gen2File("a", kA, false, {1000, 2000})});
  auto r = Diff(root + "/old", root + "/same", root).result;
  CHECK(r.old_plan && r.unchanged_files == 1 && r.changed_files == 0 && r.hashed_files == 0);
  auto other = Gen2File("a", kA, false, {1000, 2000});
  other.units.chunks[1].md5 = Md5Hex("another second chunk");
  WritePlan(root + "/other", Header(2), {other});
  r = Diff(root + "/old", root + "/other", root).result;
  CHECK(r.changed_files == 1 && r.unchanged_files == 0);
  // The same bytes cut in other chunks: nothing both sides can answer.
  WritePlan(root + "/recut", Header(2), {Gen2File("a", kA, false, {2000, 1000})});
  CHECK(Diff(root + "/old", root + "/recut", root).result.changed_files == 1);
  // SHA-256 decides before the chunk lists when both have one.
  auto sha_old = Gen2File("s", kC, false, {300, 400});
  sha_old.sha256 = Sha256Hex(kC);
  auto sha_new = Gen2File("s", kC, false, {300, 400});
  sha_new.sha256 = Sha256Hex(kC + "other");
  CHECK(!gog::SameContent(sha_old, sha_new));
  sha_new.sha256 = sha_old.sha256;
  CHECK(gog::SameContent(sha_old, sha_new));
}

// Gen 1 (whole MD5, ranges) against gen 2 (one chunk, no file MD5), both ways.
void TestGenerations() {
  const auto root = Root("generations");
  WritePlan(root + "/gen1", Header(1), {Gen1File("a", kA), Gen1File("b", kB)});
  WritePlan(root + "/gen2", Header(2), {Gen2File("a", kA, false), Gen2File("b", RandomBytes(kB.size(), 5), false)});
  WriteBytes(root + "/a", Unrelated(kA.size()));
  WriteBytes(root + "/b", Unrelated(kB.size()));
  auto diffed = Diff(root + "/gen1", root + "/gen2", root);
  CHECK(diffed.result.old_plan && diffed.result.hashed_files == 0);
  CHECK(!diffed.changed.at("a") && diffed.changed.at("b"));
  diffed = Diff(root + "/gen2", root + "/gen1", root);
  CHECK(!diffed.changed.at("a") && diffed.changed.at("b"));
  // A gen 2 file of several chunks without a file MD5 has no content MD5: changed against gen 1.
  WritePlan(root + "/gen2-chunks", Header(2), {Gen2File("a", kA, false, {1000, 2000})});
  CHECK(Diff(root + "/gen1", root + "/gen2-chunks", root).changed.at("a"));
}

void TestAddedAndRemoved() {
  const auto root = Root("added-removed");
  WritePlan(root + "/old", Header(2), {Gen2File("a", kA), Gen2File("gone.dat", kB)});
  WritePlan(root + "/new", Header(2), {Gen2File("a", kA), Gen2File("new.dat", kC)});
  WriteBytes(root + "/a", Unrelated(kA.size()));
  WriteBytes(root + "/new.dat", Unrelated(kC.size()));
  const auto diffed = Diff(root + "/old", root + "/new", root);
  CHECK(!diffed.changed.at("a") && diffed.changed.at("new.dat"));
  CHECK(diffed.result.removed_files == 1 && diffed.result.changed_files == 1 && diffed.result.unchanged_files == 1);
}

// An unchanged entry whose disk file is gone, has another size, or is a folder is fetched again.
void TestDiskFiles() {
  const auto root = Root("disk");
  const std::vector<PlanEntry> entries{Gen2File("missing", kA), Gen2File("short", kB), Gen2File("folder", kC),
                                       Gen2File("fine", kC)};
  WritePlan(root + "/old", Header(2), entries);
  WritePlan(root + "/new", Header(2), entries);
  WriteBytes(root + "/short", Unrelated(kB.size() - 1));
  fs::create_directories(root + "/folder");
  WriteBytes(root + "/fine", Unrelated(kC.size()));
  const auto diffed = Diff(root + "/old", root + "/new", root);
  CHECK(diffed.changed.at("missing") && diffed.changed.at("short") && diffed.changed.at("folder"));
  CHECK(!diffed.changed.at("fine") && diffed.result.changed_files == 3 && diffed.result.removed_files == 0);
}

// exFAT ignores case: 'Data/File.dat' and 'data/file.dat' are one entry.
void TestCaseFolded() {
  const auto root = Root("case");
  WritePlan(root + "/old", Header(2), {Gen2File("Data/File.dat", kA)});
  WritePlan(root + "/new", Header(2), {Gen2File("data/file.dat", kA)});
  WriteBytes(root + "/data/file.dat", Unrelated(kA.size()));
  const auto diffed = Diff(root + "/old", root + "/new", root);
  CHECK(diffed.result.old_plan && !diffed.changed.at("data/file.dat"));
  CHECK(diffed.result.removed_files == 0 && diffed.result.unchanged_files == 1);
}

// No old plan (or one that doesn't read): disk files of the right size are hashed.
void TestNoOldPlan() {
  const auto root = Root("no-old");
  WritePlan(root + "/new", Header(2),
            {Gen2File("match", kA), Gen2File("differs", kB), Gen2File("missing", kC),
             Gen2File("chunks", kB, false, {2500, 2500}), Gen2File("one-chunk", kC, false), Gen2File("empty", "")});
  WriteBytes(root + "/match", kA);
  WriteBytes(root + "/differs", Unrelated(kB.size()));
  WriteBytes(root + "/chunks", kB);  // right bytes, but nothing to check them against
  WriteBytes(root + "/one-chunk", kC);
  WriteBytes(root + "/empty", "");
  for (const auto& old_plan : {root + "/none", root + "/damaged"}) {
    if (old_plan == root + "/damaged") WriteBytes(old_plan, gog::HeaderLine(Header(2)) + "\nF\tmatch\tnot-a-source\n");
    const auto diffed = Diff(old_plan, root + "/new", root);
    const auto& r = diffed.result;
    CHECK(r.readable && !r.old_plan && !r.canceled);
    CHECK(!diffed.changed.at("match") && diffed.changed.at("differs") && diffed.changed.at("missing"));
    CHECK(diffed.changed.at("chunks") && !diffed.changed.at("one-chunk") && !diffed.changed.at("empty"));
    CHECK(r.hashed_files == 3 && r.unchanged_files == 3 && r.changed_files == 3 && r.removed_files == 0);
  }
}

void TestSupportAndRedist() {
  const auto root = Root("support-redist");
  const std::string support = ".playden-gog/support/1900000001/save/slot.sav";
  const std::string redist = ".playden-gog/redist/XNA/xnafx31_redist.msi";
  WritePlan(root + "/old", Header(2),
            {Gen2File(support, kA), Gen2File(redist, kB, true, {}, "dep:XNA")});
  WritePlan(root + "/new", Header(2),
            {Gen2File(support, kA), Gen2File(redist, RandomBytes(kB.size(), 7), true, {}, "dep:XNA")});
  WriteBytes(root + "/" + support, Unrelated(kA.size()));
  WriteBytes(root + "/" + redist, Unrelated(kB.size()));
  const auto diffed = Diff(root + "/old", root + "/new", root);
  CHECK(diffed.result.old_plan && !diffed.changed.at(support) && diffed.changed.at(redist));
}

// A Phase 3 plan's "v1" header reads as gen 2.
void TestPhase3Header() {
  const auto root = Root("phase3");
  WritePlan(root + "/new", Header(2), {Gen2File("a", kA)});
  const auto entry = Gen2File("a", kA);
  WriteBytes(root + "/old", "v1\t1000000000000000100\t" + std::string(32, 'b') + "\ten-US\t1900000001\t" +
                                std::to_string(kA.size()) + "\t" + std::to_string(entry.download()) + "\n" +
                                gog::EntryText(entry));
  const auto header = gog::ReadPlanHeader(root + "/old");
  CHECK(header && header->generation == 2 && header->build_id == "1000000000000000100" && header->timestamp == 0);
  WriteBytes(root + "/a", Unrelated(kA.size()));
  const auto r = Diff(root + "/old", root + "/new", root).result;
  CHECK(r.old_plan && r.unchanged_files == 1 && r.hashed_files == 0);
  CHECK(!gog::ReadPlanHeader(root + "/nothing"));
}

// Cancel while a file is hashed: the diff stops in the middle of it and reports it, and never reports that file. The
// file is the plan's last (so only the hash's own check can stop it) and sparse: 2 GiB of holes.
void TestCancel() {
  const auto root = Root("cancel");
  constexpr std::uint64_t kBig = 2ull * 1024 * 1024 * 1024;
  PlanEntry big;
  big.path = "big.bin";
  big.source = "1900000001";
  big.size = kBig;
  big.md5 = Md5Hex("not these bytes");
  for (std::uint64_t at = 0; at < kBig; at += gog::kMaxChunkBytes) {
    gog::DepotChunk chunk;
    chunk.md5 = Md5Hex("chunk" + std::to_string(at));
    chunk.compressed_md5 = chunk.md5;
    chunk.size = gog::kMaxChunkBytes;
    chunk.compressed_size = 1024;
    big.units.chunks.push_back(chunk);
  }
  WritePlan(root + "/new", Header(2), {Gen2File("first", kA), big});
  WriteBytes(root + "/first", kA);
  {
    FILE* file = std::fopen((root + "/big.bin").c_str(), "wb");
    CHECK(file && ftruncate(fileno(file), static_cast<off_t>(kBig)) == 0 && std::fclose(file) == 0);
  }
  std::atomic<bool> cancel{false};
  bool big_seen = false;
  std::thread canceler;
  const auto diffed = Diff(root + "/none", root + "/new", root, &cancel, [&](const PlanEntry& entry) {
    if (entry.path == "first")
      canceler = std::thread([&cancel] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        cancel = true;
      });
    if (entry.path == "big.bin") big_seen = true;
  });
  canceler.join();
  CHECK(diffed.result.canceled && !diffed.result.readable && !big_seen);
  CHECK(diffed.result.hashed_files == 2);
}

void TestUpdateTarget() {
  const auto expect = [](const std::string& text) {
    const auto hex = Sha256Hex(text);
    std::uint64_t value = std::stoull(hex.substr(0, 16), nullptr, 16);
    return value == 0 ? 1 : value;
  };
  const std::vector<std::uint64_t> base{1900000001};
  CHECK(gog::UpdateTarget("1000000000000000101", 2, base) == expect("gog\n2\n1000000000000000101\n1900000001"));
  CHECK(gog::UpdateTarget("1000000000000000101", 1, base) == expect("gog\n1\n1000000000000000101\n1900000001"));
  CHECK(gog::UpdateTarget("1000000000000000101", 1, base) != gog::UpdateTarget("1000000000000000101", 2, base));
  CHECK(gog::UpdateTarget("1000000000000000102", 2, base) != gog::UpdateTarget("1000000000000000101", 2, base));
  // The products it takes: sorted, without repeats; another DLC is another update.
  CHECK(gog::UpdateTarget("1000000000000000101", 2, {1900000002, 1900000001, 1900000002}) ==
        expect("gog\n2\n1000000000000000101\n1900000001,1900000002"));
  CHECK(gog::UpdateTarget("1000000000000000101", 2, {1900000001, 1900000002}) !=
        gog::UpdateTarget("1000000000000000101", 2, base));
  CHECK(gog::UpdateTarget("", 2, {}) != 0);
}

// ---- Seeds --------------------------------------------------------------------------------------------------------

void TestSeeds() {
  gog::PathSet paths;
  paths.Seed("Data/Sub/File.dat", gog::PathKind::kFile);
  paths.Seed("Logs", gog::PathKind::kDirectory);
  paths.Seed("Thing", gog::PathKind::kFile);
  auto added = paths.Add("data/sub/file.dat", gog::PathKind::kFile, kBase, false, 0);
  CHECK(added.ok() && added.path == "Data/Sub/File.dat" && !added.replaced);
  added = paths.Add("DATA/other.dat", gog::PathKind::kFile, kBase, false, 1);
  CHECK(added.ok() && added.path == "Data/other.dat");
  added = paths.Add("logs/today.txt", gog::PathKind::kFile, kBase, false, 2);
  CHECK(added.ok() && added.path == "Logs/today.txt");
  // A seeded file is no entry: a folder in its place is no clash and takes its spelling.
  added = paths.Add("thing/inside.txt", gog::PathKind::kFile, kBase, false, 3);
  CHECK(added.ok() && added.path == "Thing/inside.txt" && !added.replaced);
  // A seeded folder is no entry either: a file there is no clash.
  gog::PathSet other;
  other.Seed("Saves/slot.sav", gog::PathKind::kFile);
  added = other.Add("saves", gog::PathKind::kFile, kBase, false, 0);
  CHECK(added.ok() && added.path == "Saves" && !added.replaced);
  CHECK(other.entries() == 1);
  // Unseeded paths are as before.
  added = other.Add("New/a.txt", gog::PathKind::kFile, kBase, false, 1);
  CHECK(added.ok() && added.path == "New/a.txt");
}

// ---- Planning into a layout ---------------------------------------------------------------------------------------

struct Harness {
  CdnStub stub;
  std::atomic<bool> cancel{false};
  std::atomic<bool> revoked{false};
  std::atomic<std::uint64_t> free{1ull << 40};
  int parallel = 4;
  std::vector<std::uint64_t> excluded;  // the DLC Install, Plan and Stage leave out
  std::mutex mutex;
  std::vector<std::string> logs;
  GogInstaller::Progress last_progress;

  Harness() {
    stub.SetOwned("{\"owned\":[1900000001,1900000002]}");
    stub.SetSecureLink(ReadFile(g_fixtures + "/secure-link-200.json"));
    stub.SetSecureLinkV1(ReadFile(g_fixtures + "/secure-link-gen1-200.json"));
    stub.SetOpenLink(ReadFile(g_fixtures + "/dependencies-open-link.json"));
  }

  GogInstaller::Hooks Hooks() {
    GogInstaller::Hooks hooks;
    hooks.make_transport = [this] { return stub.Transport(); };
    hooks.token = [this](const std::atomic<bool>&) {
      gog::AccessToken token;
      token.status = revoked.load() ? gog::AccessToken::Status::kRevoked : gog::AccessToken::Status::kOk;
      if (!revoked.load()) token.token = AccessText(0);
      token.generation = 1;
      return token;
    };
    hooks.free_bytes = [this](const std::string& path) {
      CHECK(path.find("/.playden-update") == std::string::npos);
      return free.load();
    };
    hooks.parallel = parallel;
    hooks.pause = [](std::uint32_t, const std::atomic<bool>&) {};
    hooks.now_seconds = [] { return kNow; };
    hooks.log = [this](const std::string& line) {
      std::lock_guard<std::mutex> lock(mutex);
      logs.push_back(line);
    };
    return hooks;
  }

  GogInstaller::Outcome Install(const std::string& root) {
    GogInstaller installer(Hooks());
    GogInstaller::Target target{kBase, root, "english"};
    target.excluded = excluded;
    return installer.Run(target, cancel);
  }

  GogInstaller::Outcome Plan(const std::string& root, const gog::PlanLayout& layout, bool seed) {
    GogInstaller installer(Hooks());
    GogInstaller::Target target{kBase, root, "english"};
    if (seed) target.seed_list = root + "/.playden-files";
    target.excluded = excluded;
    return installer.Plan(target, layout, cancel);
  }

  // Installs with Run and writes the record the runner would.
  void InstallGame(const std::string& root) {
    CHECK(Install(root).error == Error::kNone);
    WriteBytes(root + "/" + gog::kInstallRecordName, "{\"synthetic\":true}\n");
  }

  GogInstaller::StageOutcome Stage(const std::string& root) {
    GogInstaller installer(Hooks());
    GogInstaller::Target target{kBase, root, "english"};
    target.excluded = excluded;
    return installer.Stage(target, cancel, [this](const GogInstaller::Progress& progress) {
      CHECK(progress.done <= progress.total && progress.held == 0);
      last_progress = progress;
    });
  }

  bool Logged(const std::string& text) {
    std::lock_guard<std::mutex> lock(mutex);
    for (const auto& line : logs)
      if (line.find(text) != std::string::npos) return true;
    return false;
  }

  // The chunk hashes asked for, secure-link and dependency ones.
  std::multiset<std::string> Chunks() const {
    std::multiset<std::string> hashes;
    for (const auto& request : stub.requests())
      if (request.kind == Kind::kChunk || request.kind == Kind::kDependencyChunk) hashes.insert(request.hash);
    return hashes;
  }

  // No log line names a path or the scratch folder.
  void CheckLogs() {
    std::lock_guard<std::mutex> lock(mutex);
    for (const auto& line : logs) {
      CHECK(line.find(g_scratch) == std::string::npos);
      CHECK(line.find("Data/") == std::string::npos && line.find("data/") == std::string::npos);
      CHECK(line.find("example.invalid") == std::string::npos && line.find("synthetic-access") == std::string::npos);
      CHECK(line.find("https://") == std::string::npos && line.find(".dat") == std::string::npos);
      CHECK(line.find("slot.sav") == std::string::npos && line.find("xnafx") == std::string::npos);
    }
  }
};

// Everything under root but `skip` (a top-level name): names and bytes, folders marked.
std::map<std::string, std::string> Snapshot(const std::string& root, const std::string& skip) {
  std::map<std::string, std::string> files;
  for (auto it = fs::recursive_directory_iterator(root); it != fs::recursive_directory_iterator(); ++it) {
    const auto relative = fs::relative(it->path(), root).string();
    if (relative == skip) {
      it.disable_recursion_pending();
      continue;
    }
    files[relative] = it->is_directory() ? std::string("<folder>") : ReadFile(it->path().string());
  }
  return files;
}

const std::string kExe = RandomBytes(40 * 1024, 31);
const std::string kData = RandomBytes(30 * 1024, 32);
const std::string kNewData = RandomBytes(20 * 1024, 33);
const std::string kXna = RandomBytes(10 * 1024, 34);

Game BuildA() {
  Game game;
  GameDepot main;
  main.files.push_back({"DepotFile", "bin\\Game.exe", kExe});
  main.files.push_back({"DepotFile", "Data\\File.dat", kData});
  main.files.push_back({"DepotFile", "save\\slot.sav", "slot\n", {}, true});
  game.depots.push_back(main);
  return game;
}

// B: the exe changed, the data folder spelled in lower case, a new file, and an XNA redist.
Game BuildB() {
  Game game;
  game.build_id = "1000000000000000202";
  GameDepot main;
  main.files.push_back({"DepotFile", "bin\\Game.exe", kExe + "patched"});
  main.files.push_back({"DepotFile", "data\\file.dat", kData});
  main.files.push_back({"DepotFile", "data\\new.dat", kNewData});
  main.files.push_back({"DepotFile", "save\\slot.sav", "slot\n", {}, true});
  game.depots.push_back(main);
  game.dependencies = {"XNA"};
  return game;
}

Published PublishXna(CdnStub& stub, const std::string& bytes = kXna) {
  StoreDependency xna;
  xna.id = "XNA";
  xna.exe_path = "__redist/XNA/xnafx31_redist.msi";
  xna.arguments_json = "\"/qn\"";
  xna.files.push_back({"DepotFile", "__redist/XNA/xnafx31_redist.msi", bytes});
  return PublishDependencyStore(stub, {xna});
}

void TestPlanIntoUpdateLayout() {
  Harness harness;
  Publish(harness.stub, BuildA());
  const auto root = g_scratch + "/layout-update";
  CHECK(harness.Install(root).error == Error::kNone);
  const auto before = Snapshot(root, gog::kUpdateDirName);
  CHECK(before.count(".playden-gog/files") && before.count(".playden-files") && before.count(".playden-gog/redist.list"));
  CHECK(before.count(".playden-download/gog-journal"));

  PublishXna(harness.stub);
  Publish(harness.stub, BuildB());
  harness.stub.ClearRequests();
  const auto layout = gog::UpdateLayout(root);
  const auto outcome = harness.Plan(root, layout, true);
  CHECK(outcome.error == Error::kNone && outcome.build_id == "1000000000000000202" && outcome.generation == 2);
  CHECK(outcome.files == 5);
  // Nothing was downloaded, and the live install is as it was.
  CHECK(harness.stub.Count(Kind::kChunk) == 0 && harness.stub.Count(Kind::kDependencyChunk) == 0);
  CHECK(Snapshot(root, gog::kUpdateDirName) == before);

  CHECK(layout.plan == root + "/.playden-update/gog/files" && layout.file_list == root + "/.playden-update/files");
  CHECK(layout.journal == root + "/.playden-update/gog-journal" && !Exists(layout.items));
  const auto header = gog::ReadPlanHeader(layout.plan);
  CHECK(header && header->build_id == "1000000000000000202");
  const auto plan = ReadFile(layout.plan);
  // The installed spelling is kept: 'data\' in B plans into the 'Data/' folder on the drive.
  CHECK(plan.find("F\tData/File.dat\t") != std::string::npos && plan.find("F\tData/new.dat\t") != std::string::npos);
  CHECK(plan.find("data/") == std::string::npos);
  const auto list = ReadFile(layout.file_list);
  CHECK(list.find("f\tData/new.dat\n") != std::string::npos);
  CHECK(list.find("f\t.playden-gog/redist/XNA/xnafx31_redist.msi\n") != std::string::npos);
  CHECK(ReadFile(layout.redist_list) ==
        "gog-redist\tv1\nP\tXNA\t.playden-gog/redist/XNA/xnafx31_redist.msi\t/qn\n");
  CHECK(Exists(root + "/.playden-update/gog/meta") && Exists(root + "/.playden-update/gog/manifests"));

  // The two plans diffed: the exe and the new data file changed, the rest kept (the support file too).
  const auto diffed = Diff(gog::InstallLayout(root).plan, layout.plan, root);
  CHECK(diffed.result.readable && diffed.result.old_plan && diffed.result.removed_files == 0);
  CHECK(diffed.changed.at("bin/Game.exe") && diffed.changed.at("Data/new.dat"));
  CHECK(diffed.changed.at(".playden-gog/redist/XNA/xnafx31_redist.msi"));
  CHECK(!diffed.changed.at("Data/File.dat") && !diffed.changed.at(".playden-gog/support/1900000001/save/slot.sav"));
  CHECK(diffed.result.changed_files == 3 && diffed.result.unchanged_files == 2);
  CHECK(diffed.result.staged_bytes == kExe.size() + 7 + kNewData.size() + kXna.size());

  // Planning again replaces the update's journal, never the install's.
  WriteBytes(layout.journal, "f\t0\n");
  CHECK(harness.Plan(root, layout, true).error == Error::kNone);
  CHECK(!Exists(layout.journal) && Snapshot(root, gog::kUpdateDirName) == before);
  harness.CheckLogs();
}

// Without seeds, B's own spelling: the test above bites. A measure's layout under .playden-gog/measure, no journal.
void TestPlanWithoutSeeds() {
  Harness harness;
  Publish(harness.stub, BuildA());
  const auto root = g_scratch + "/layout-measure";
  CHECK(harness.Install(root).error == Error::kNone);
  const auto before = Snapshot(root, ".playden-gog");
  const auto live_gog = Snapshot(root + "/.playden-gog", "measure");
  PublishXna(harness.stub);
  Publish(harness.stub, BuildB());
  const auto layout = gog::MeasureLayout(root);
  CHECK(layout.journal.empty() && layout.file_list == root + "/.playden-gog/measure/file-list");
  const auto outcome = harness.Plan(root, layout, false);
  CHECK(outcome.error == Error::kNone && outcome.build_id == "1000000000000000202");
  const auto plan = ReadFile(layout.plan);
  CHECK(plan.find("F\tdata/file.dat\t") != std::string::npos && plan.find("Data/") == std::string::npos);
  CHECK(Exists(layout.redist_list) && Exists(layout.file_list) && !Exists(layout.items));
  CHECK(Snapshot(root, ".playden-gog") == before);
  CHECK(Snapshot(root + "/.playden-gog", "measure") == live_gog);
  CHECK(!Exists(root + "/.playden-update"));
  harness.CheckLogs();
}

// A gen 1 build planned into the update layout: its repository and manifests stay there too.
void TestPlanGen1IntoLayout() {
  Harness harness;
  Publish(harness.stub, BuildA());
  const auto root = g_scratch + "/layout-gen1";
  CHECK(harness.Install(root).error == Error::kNone);
  const auto before = Snapshot(root, gog::kUpdateDirName);
  GameV1 game;
  GameDepotV1 depot;
  depot.files.push_back({"bin\\Game.exe", kExe});
  depot.files.push_back({"DATA\\File.dat", kData});
  game.depots.push_back(depot);
  const auto published = PublishV1(harness.stub, game);
  const auto layout = gog::UpdateLayout(root);
  const auto outcome = harness.Plan(root, layout, true);
  CHECK(outcome.error == Error::kNone && outcome.generation == 1 && outcome.build_id == "15751");
  CHECK(Snapshot(root, gog::kUpdateDirName) == before);
  CHECK(Exists(layout.meta_dir + "/" + published.repository_md5));
  bool manifest = false;
  for (const auto& entry : fs::directory_iterator(layout.manifest_dir))
    manifest = manifest || entry.path().filename().string().rfind("v1-", 0) == 0;
  CHECK(manifest);
  CHECK(ReadFile(layout.plan).find("F\tData/File.dat\t") != std::string::npos);
  // Gen 2 to gen 1: the same bytes are kept.
  const auto diffed = Diff(gog::InstallLayout(root).plan, layout.plan, root);
  CHECK(!diffed.changed.at("bin/Game.exe") && !diffed.changed.at("Data/File.dat"));
  CHECK(diffed.result.removed_files == 1);  // the support file
  harness.CheckLogs();
}

// Plan never makes a game folder.
void TestPlanNeedsFolder() {
  Harness harness;
  Publish(harness.stub, BuildA());
  const auto root = g_scratch + "/not-installed";
  CHECK(harness.Plan(root, gog::UpdateLayout(root), false).error == Error::kStorage);
  CHECK(!Exists(root));
}


// ---- Staging an update --------------------------------------------------------------------------------------------

const std::string kKeep = RandomBytes(12 * 1024, 41);
const std::string kChange = RandomBytes(18 * 1024, 42);
const std::string kChanged = RandomBytes(24 * 1024, 43);  // B's change.dat: three chunks
const std::string kRemove = RandomBytes(6 * 1024, 44);
const std::string kAdd = RandomBytes(9 * 1024, 45);

Game ChangesA() {
  Game game;
  GameDepot main;
  main.files.push_back({"DepotFile", "keep.dat", kKeep});
  main.files.push_back({"DepotFile", "change.dat", kChange});
  main.files.push_back({"DepotFile", "remove.dat", kRemove});
  game.depots.push_back(main);
  return game;
}

// B keeps one file, changes one (into three chunks), removes one and adds one.
Game ChangesB(int lie_chunk = -1) {
  Game game;
  game.build_id = "1000000000000000202";
  GameDepot main;
  main.files.push_back({"DepotFile", "keep.dat", kKeep});
  GameFile change{"DepotFile", "change.dat", kChanged, {8 * 1024, 8 * 1024, 8 * 1024}};
  change.lie_chunk = lie_chunk;
  main.files.push_back(change);
  main.files.push_back({"DepotFile", "add.dat", kAdd});
  game.depots.push_back(main);
  return game;
}

std::string Work(const std::string& root) { return root + "/" + gog::kUpdateDirName; }
std::string Staging(const std::string& root) { return Work(root) + "/staging"; }
std::string Contents(const std::string& path) { return Exists(path) ? ReadFile(path) : std::string("<missing>"); }

std::multiset<std::string> Hashes(const std::vector<gog::DepotChunk>& chunks) {
  std::multiset<std::string> hashes;
  for (const auto& chunk : chunks) hashes.insert(chunk.compressed_md5);
  return hashes;
}

std::uint64_t Download(const std::vector<gog::DepotChunk>& chunks) {
  std::uint64_t total = 0;
  for (const auto& chunk : chunks) total += chunk.compressed_size;
  return total;
}

std::multiset<std::string> Union(std::multiset<std::string> a, const std::multiset<std::string>& b) {
  a.insert(b.begin(), b.end());
  return a;
}

// A folder's entry names as the drive stores them.
std::set<std::string> Names(const std::string& folder) {
  std::set<std::string> names;
  for (const auto& entry : fs::directory_iterator(folder)) names.insert(entry.path().filename().string());
  return names;
}

void TestStageChanges() {
  Harness harness;
  Publish(harness.stub, ChangesA());
  const auto root = g_scratch + "/stage-changes";
  harness.InstallGame(root);
  const auto before = Snapshot(root, gog::kUpdateDirName);
  CHECK(before.at(".playden-files") == "f\tkeep.dat\nf\tchange.dat\nf\tremove.dat\n");
  CHECK(before.count(".playden-gog/redist.list") && before.count(gog::kInstallRecordName));
  const auto b = Publish(harness.stub, ChangesB());
  harness.stub.ClearRequests();

  const auto outcome = harness.Stage(root);
  CHECK(outcome.error == Error::kNone && outcome.build_id == "1000000000000000202" && outcome.generation == 2);
  CHECK(outcome.meta_id == b.meta_id && outcome.language == "en-US" && outcome.files == 3);
  CHECK(outcome.install_bytes == kKeep.size() + kChanged.size() + kAdd.size());
  CHECK(outcome.staged_files == 2 && outcome.staged_bytes == kChanged.size() + kAdd.size());
  CHECK(outcome.removed_files == 1 && outcome.hashed_files == 0);
  const auto& change = b.chunks.at("change.dat");
  const auto& add = b.chunks.at("add.dat");
  CHECK(outcome.download_bytes == Download(change) + Download(add));
  // Only the changed and added files' chunks.
  CHECK(harness.Chunks() == Union(Hashes(change), Hashes(add)));
  CHECK(harness.last_progress.done == outcome.download_bytes && harness.last_progress.total == outcome.download_bytes);
  CHECK(Contents(Staging(root) + "/change.dat") == kChanged && Contents(Staging(root) + "/add.dat") == kAdd);
  CHECK((Names(Staging(root)) == std::set<std::string>{"add.dat", "change.dat"}));
  // The installed game is as it was, byte for byte.
  CHECK(Snapshot(root, gog::kUpdateDirName) == before);
  CHECK(Contents(Work(root) + "/staged") == "change.dat\nadd.dat\n");
  CHECK(Contents(Work(root) + "/old") == before.at(".playden-files"));
  CHECK(Contents(Work(root) + "/files") == "f\tkeep.dat\nf\tchange.dat\nf\tadd.dat\n");
  CHECK(Contents(Work(root) + "/paths") == "f\tadd.dat\nf\tchange.dat\nf\tkeep.dat\nf\tremove.dat\n");
  CHECK(Exists(Work(root) + "/gog-journal") && Exists(Work(root) + "/gog/files"));
  for (const char* name : {"staged.tmp", "old.tmp", "paths.tmp", "files.tmp", "gog/items.tmp"})
    CHECK(!Exists(Work(root) + "/" + name));
  CHECK(harness.Logged("gog update step=diff result=ok changed=2 removed=1 hashed=0"));
  CHECK(harness.Logged("gog update step=stage result=none files=3 changed=2 removed=1"));
  harness.CheckLogs();
}

// Cancel after the first chunk of the three-chunk changed file; a second Stage goes on from there.
void TestStageResume() {
  Harness harness;
  harness.parallel = 1;
  Publish(harness.stub, ChangesA());
  const auto root = g_scratch + "/stage-resume";
  harness.InstallGame(root);
  const auto before = Snapshot(root, gog::kUpdateDirName);
  const auto b = Publish(harness.stub, ChangesB());
  const auto& change = b.chunks.at("change.dat");
  harness.stub.ClearRequests();
  harness.stub.SetFault([&](const Request& request) -> std::optional<Fault> {
    if (request.hash != change.at(1).compressed_md5 || harness.cancel.load()) return std::nullopt;
    harness.cancel = true;
    Fault fault;
    fault.response = Failure(HttpError::kCanceled);
    return fault;
  });
  CHECK(harness.Stage(root).error == Error::kCanceled);
  CHECK(harness.stub.CountHash(change.at(0).compressed_md5) == 1);
  const auto staged = Contents(Work(root) + "/staged");
  CHECK(staged == "change.dat\nadd.dat\n");
  CHECK(ReadFile(Work(root) + "/gog-journal").find("c\t1\t0\n") != std::string::npos);
  CHECK(Snapshot(root, gog::kUpdateDirName) == before);

  // The player deletes an unchanged game file meanwhile: what is staged stays as it was.
  std::remove((root + "/keep.dat").c_str());
  harness.stub.SetFault({});
  harness.cancel = false;
  const int builds = harness.stub.Count(Kind::kBuilds);
  const auto outcome = harness.Stage(root);
  CHECK(outcome.error == Error::kNone && outcome.staged_files == 2 && outcome.removed_files == 1);
  CHECK(harness.stub.Count(Kind::kBuilds) == builds && harness.Logged("gog update plan reused"));
  CHECK(harness.stub.CountHash(change.at(0).compressed_md5) == 1);  // not asked for twice
  CHECK(harness.stub.CountHash(change.at(1).compressed_md5) == 2);
  CHECK(harness.stub.CountHash(change.at(2).compressed_md5) == 1);
  CHECK(harness.stub.CountHash(b.chunks.at("keep.dat").at(0).compressed_md5) == 0);
  CHECK(Contents(Work(root) + "/staged") == staged);
  CHECK(Contents(Staging(root) + "/change.dat") == kChanged && Contents(Staging(root) + "/add.dat") == kAdd);
  CHECK(!Exists(Staging(root) + "/keep.dat"));
  harness.CheckLogs();
}

// A second Stage after one that finished reuses the plan and asks the CDN for nothing.
void TestStageReused() {
  Harness harness;
  Publish(harness.stub, ChangesA());
  const auto root = g_scratch + "/stage-reused";
  harness.InstallGame(root);
  Publish(harness.stub, ChangesB());
  CHECK(harness.Stage(root).error == Error::kNone);
  const auto lists = Snapshot(Work(root), "staging");
  harness.stub.ClearRequests();
  const auto outcome = harness.Stage(root);
  CHECK(outcome.error == Error::kNone && outcome.build_id == "1000000000000000202" && outcome.staged_files == 2);
  CHECK(outcome.removed_files == 1 && outcome.hashed_files == 0 && outcome.files == 3);
  CHECK(harness.stub.requests().empty());
  CHECK(harness.Logged("gog update plan reused"));
  CHECK(Contents(Staging(root) + "/change.dat") == kChanged);
  // The lists are as the first Stage wrote them (the journal only grew).
  auto now = Snapshot(Work(root), "staging");
  CHECK(now.at("gog-journal").rfind(lists.at("gog-journal"), 0) == 0);
  now.erase("gog-journal");
  auto earlier = lists;
  earlier.erase("gog-journal");
  CHECK(now == earlier);
  harness.CheckLogs();
}

// A support file, an app/ support file and a __redist package: each staged at the path the new plan places it.
void TestStageSupportAndRedist() {
  Harness harness;
  Game a;
  GameDepot main;
  main.files.push_back({"DepotFile", "bin\\Game.exe", kExe});
  main.files.push_back({"DepotFile", "save\\slot.sav", "slot\n", {}, true});
  main.files.push_back({"DepotFile", "app\\dosbox.conf", "[dosbox]\n", {}, true});
  a.depots.push_back(main);
  a.dependencies = {"XNA"};
  PublishXna(harness.stub);
  Publish(harness.stub, a);
  const auto root = g_scratch + "/stage-support";
  harness.InstallGame(root);
  const auto before = Snapshot(root, gog::kUpdateDirName);
  CHECK(before.count(".playden-gog/support/1900000001/save/slot.sav") && before.count("dosbox.conf"));
  CHECK(before.count(".playden-gog/redist/XNA/xnafx31_redist.msi"));

  Game b = a;
  b.build_id = "1000000000000000202";
  b.depots[0].files[1].bytes = "slot two\n";
  b.depots[0].files[2].bytes = "[dosbox]\nmemsize=64\n";
  const auto new_xna = RandomBytes(kXna.size(), 77);
  const auto store = PublishXna(harness.stub, new_xna);
  const auto published = Publish(harness.stub, b);
  harness.stub.ClearRequests();
  const auto outcome = harness.Stage(root);
  CHECK(outcome.error == Error::kNone && outcome.staged_files == 3 && outcome.removed_files == 0);
  CHECK(Contents(Work(root) + "/staged") ==
        ".playden-gog/support/1900000001/save/slot.sav\ndosbox.conf\n.playden-gog/redist/XNA/xnafx31_redist.msi\n");
  CHECK(Contents(Staging(root) + "/.playden-gog/support/1900000001/save/slot.sav") == "slot two\n");
  CHECK(Contents(Staging(root) + "/dosbox.conf") == "[dosbox]\nmemsize=64\n");
  CHECK(Contents(Staging(root) + "/.playden-gog/redist/XNA/xnafx31_redist.msi") == new_xna);
  CHECK(!Exists(Staging(root) + "/bin") && !Exists(Staging(root) + "/app"));
  CHECK(harness.stub.CountHash(published.chunks.at("bin\\Game.exe").at(0).compressed_md5) == 0);
  CHECK(harness.stub.Count(Kind::kDependencyChunk) == 1 &&
        harness.stub.CountHash(store.chunks.at("__redist/XNA/xnafx31_redist.msi").at(0).compressed_md5) == 1);
  CHECK(harness.stub.Count(Kind::kChunk) == 2);
  // The new redist list waits in the update's plan folder; the live one is unchanged.
  CHECK(Contents(Work(root) + "/gog/redist.list") ==
        "gog-redist\tv1\nP\tXNA\t.playden-gog/redist/XNA/xnafx31_redist.msi\t/qn\n");
  CHECK(Snapshot(root, gog::kUpdateDirName) == before);
  harness.CheckLogs();
}

// Too little space for the staged files and the margin: nothing is downloaded. Just enough is enough.
void TestStageNoSpace() {
  Harness harness;
  Publish(harness.stub, ChangesA());
  const auto root = g_scratch + "/stage-space";
  harness.InstallGame(root);
  Publish(harness.stub, ChangesB());
  harness.stub.ClearRequests();
  const std::uint64_t need = kChanged.size() + kAdd.size() + gog::kGogSpaceMargin;
  harness.free = need - 1;
  const auto outcome = harness.Stage(root);
  CHECK(outcome.error == Error::kNoSpace && outcome.staged_files == 2);
  CHECK(harness.stub.Count(Kind::kChunk) == 0 && harness.stub.Count(Kind::kSecureLink) == 0);
  CHECK(Exists(Work(root) + "/staged"));
  harness.free = need;
  CHECK(harness.Stage(root).error == Error::kNone);
  CHECK(Contents(Staging(root) + "/add.dat") == kAdd);
}

// A chunk whose hash never matches is tried kChunkAttempts times, then the stage is corrupt.
void TestStageCorrupt() {
  Harness harness;
  harness.parallel = 1;
  Publish(harness.stub, ChangesA());
  const auto root = g_scratch + "/stage-corrupt";
  harness.InstallGame(root);
  const auto before = Snapshot(root, gog::kUpdateDirName);
  const auto b = Publish(harness.stub, ChangesB(1));
  harness.stub.ClearRequests();
  CHECK(harness.Stage(root).error == Error::kCorrupt);
  const auto& change = b.chunks.at("change.dat");
  CHECK(harness.stub.CountHash(change.at(1).compressed_md5) == gog::kChunkAttempts);
  const auto journal = ReadFile(Work(root) + "/gog-journal");
  CHECK(journal.find("c\t1\t0\n") != std::string::npos && journal.find("c\t1\t1\n") == std::string::npos);
  CHECK(Snapshot(root, gog::kUpdateDirName) == before);
}

// GOG revokes the session in the middle: signed out, the journal and staging kept. Uninstalled then, both versions'
// files go.
void TestStageSignedOut() {
  Harness harness;
  harness.parallel = 1;
  Publish(harness.stub, ChangesA());
  const auto root = g_scratch + "/stage-revoked";
  harness.InstallGame(root);
  const auto b = Publish(harness.stub, ChangesB());
  const auto& change = b.chunks.at("change.dat");
  harness.stub.ClearRequests();
  harness.stub.SetFault([&](const Request& request) -> std::optional<Fault> {
    if (request.hash != change.at(1).compressed_md5) return std::nullopt;
    harness.revoked = true;
    Fault fault;
    fault.response = Answer(401, "");
    return fault;
  });
  CHECK(harness.Stage(root).error == Error::kSignedOut);
  CHECK(ReadFile(Work(root) + "/gog-journal").find("c\t1\t0\n") != std::string::npos);
  CHECK(Exists(Staging(root) + "/change.dat") && Exists(Work(root) + "/paths"));
  std::string failure;
  CHECK(playden::steam::RemoveManagedInstall(root, &failure));
  CHECK(!Exists(root));
  harness.CheckLogs();
}

// Gen 1 A to gen 2 B where one file's MD5 differs: that file is staged.
void TestStageGen1ToGen2() {
  Harness harness;
  GameV1 a;
  GameDepotV1 depot;
  depot.files.push_back({"bin\\Game.exe", kExe});
  depot.files.push_back({"Data\\File.dat", kData});
  a.depots.push_back(depot);
  PublishV1(harness.stub, a);
  const auto root = g_scratch + "/stage-gen1";
  harness.InstallGame(root);
  CHECK(gog::ReadPlanHeader(root + "/" + gog::kPlanPath)->generation == 1);
  const auto before = Snapshot(root, gog::kUpdateDirName);
  Game b;
  b.build_id = "1000000000000000202";
  GameDepot main;
  main.files.push_back({"DepotFile", "bin\\Game.exe", kExe});
  main.files.push_back({"DepotFile", "Data\\File.dat", kNewData});
  b.depots.push_back(main);
  const auto published = Publish(harness.stub, b);
  harness.stub.ClearRequests();
  const auto outcome = harness.Stage(root);
  CHECK(outcome.error == Error::kNone && outcome.generation == 2 && outcome.staged_files == 1);
  CHECK(Contents(Work(root) + "/staged") == "Data/File.dat\n");
  CHECK(harness.Chunks() == Hashes(published.chunks.at("Data\\File.dat")));
  CHECK(harness.stub.Count(Kind::kRange) == 0);
  CHECK(Contents(Staging(root) + "/Data/File.dat") == kNewData);
  CHECK(Snapshot(root, gog::kUpdateDirName) == before);
}

// B spells the installed 'Data/' folder 'data/': staged under the installed spelling.
void TestStageSpelling() {
  Harness harness;
  Publish(harness.stub, BuildA());
  const auto root = g_scratch + "/stage-spelling";
  harness.InstallGame(root);
  Game b = BuildB();
  b.dependencies.clear();
  b.depots[0].files[1].bytes = kNewData + "changed";
  Publish(harness.stub, b);
  harness.stub.ClearRequests();
  const auto outcome = harness.Stage(root);
  CHECK(outcome.error == Error::kNone && outcome.staged_files == 3);
  CHECK(Contents(Work(root) + "/staged") == "bin/Game.exe\nData/File.dat\nData/new.dat\n");
  CHECK((Names(Staging(root)) == std::set<std::string>{"Data", "bin"}));
  CHECK((Names(Staging(root) + "/Data") == std::set<std::string>{"File.dat", "new.dat"}));
  CHECK(Contents(Staging(root) + "/Data/File.dat") == kNewData + "changed");
  CHECK(Contents(Work(root) + "/files").find("f\tData/new.dat\n") != std::string::npos);
  harness.CheckLogs();
}

// The same build again: nothing staged, nothing downloaded.
void TestStageSameBuild() {
  Harness harness;
  Publish(harness.stub, ChangesA());
  const auto root = g_scratch + "/stage-same";
  harness.InstallGame(root);
  const auto before = Snapshot(root, gog::kUpdateDirName);
  harness.stub.ClearRequests();
  const auto outcome = harness.Stage(root);
  CHECK(outcome.error == Error::kNone && outcome.staged_files == 0 && outcome.staged_bytes == 0);
  CHECK(outcome.removed_files == 0 && outcome.download_bytes == 0 && outcome.files == 3);
  CHECK(outcome.build_id == "1000000000000000101");
  CHECK(harness.Chunks().empty() && harness.stub.Count(Kind::kSecureLink) == 0);
  CHECK(Contents(Work(root) + "/staged").empty());
  CHECK(Names(Staging(root)).empty());
  CHECK(Snapshot(root, gog::kUpdateDirName) == before);
}

// No install record, or no folder: refused before anything is written.
void TestStageNeedsInstall() {
  Harness harness;
  Publish(harness.stub, ChangesA());
  const auto root = g_scratch + "/stage-no-record";
  CHECK(harness.Install(root).error == Error::kNone);
  harness.stub.ClearRequests();
  CHECK(harness.Stage(root).error == Error::kStorage);
  CHECK(!Exists(Work(root)) && harness.stub.requests().empty());
  CHECK(harness.Stage(g_scratch + "/stage-nothing").error == Error::kStorage);
  CHECK(!Exists(g_scratch + "/stage-nothing"));
  // A record that is a folder is no record.
  fs::create_directories(root + "/" + gog::kInstallRecordName);
  CHECK(harness.Stage(root).error == Error::kStorage && !Exists(Work(root)));
  // A work folder that is a link is never written through.
  const auto linked = g_scratch + "/stage-linked";
  harness.InstallGame(linked);
  const auto elsewhere = g_scratch + "/stage-elsewhere";
  fs::create_directories(elsewhere);
  CHECK(symlink(elsewhere.c_str(), Work(linked).c_str()) == 0);
  CHECK(harness.Stage(linked).error == Error::kStorage && Names(elsewhere).empty());
}

// A saved update plan that doesn't read is planned again, and the staged list made from it goes with it.
void TestStageDamagedPlan() {
  Harness harness;
  Publish(harness.stub, ChangesA());
  const auto root = g_scratch + "/stage-damaged";
  harness.InstallGame(root);
  Publish(harness.stub, ChangesB());
  CHECK(harness.Stage(root).error == Error::kNone);
  WriteBytes(Work(root) + "/gog/files", "not a plan\n");
  WriteBytes(Work(root) + "/staged", "keep.dat\n");
  harness.stub.ClearRequests();
  const auto outcome = harness.Stage(root);
  CHECK(outcome.error == Error::kNone && outcome.staged_files == 2 && harness.stub.Count(Kind::kBuilds) == 1);
  CHECK(Contents(Work(root) + "/staged") == "change.dat\nadd.dat\n");
  CHECK(Contents(Staging(root) + "/change.dat") == kChanged && !Exists(Staging(root) + "/keep.dat"));
  CHECK(Contents(Work(root) + "/paths") == "f\tadd.dat\nf\tchange.dat\nf\tkeep.dat\nf\tremove.dat\n");
}

// ---- Putting an update in place -----------------------------------------------------------------------------------

using playden::installs::UpdatePhase;
using playden::stores::ApplyGogUpdate;
using playden::stores::GogApplyResult;
using playden::stores::GogUpdateHooks;

const std::atomic<bool> kNoCancel{false};

std::string Lines(const std::vector<std::string>& lines) {
  std::string text;
  for (const auto& line : lines) text += line + "\n";
  return text;
}

std::string RecordText(const std::string& build, std::uint64_t bytes, const std::string& program) {
  playden::installs::GogRecord record;
  record.build_id = build;
  record.products = {kBase};
  record.language = "en-US";
  record.meta_id = std::string(32, build == "1000000000000000202" ? 'b' : 'a');
  return playden::installs::FormatGogInstallRecord(playden::GameId::Gog(kBase), bytes, 3, "english", record,
                                                   {{program, "", "", "Synthetic Game"}});
}

void WriteState(const std::string& root, UpdatePhase phase) {
  playden::installs::UpdateState state;
  state.phase = phase;
  WriteBytes(Work(root) + "/" + playden::installs::kUpdateStateName, playden::installs::FormatUpdateState(state));
}

std::optional<UpdatePhase> Phase(const std::string& root) {
  const auto state = playden::installs::ReadUpdateState(root);
  return state ? std::optional<UpdatePhase>(state->phase) : std::nullopt;
}

// A work folder as Stage and the runner leave it: the old and new file lists, the staged files, the new plan, metas,
// manifests, redist list and record, and update.json at `phase`. The installed game gets the old list, plan, meta,
// redist list and record.
void HandMade(const std::string& root, const std::vector<std::string>& old_list,
              const std::vector<std::string>& new_list,
              const std::vector<std::pair<std::string, std::string>>& staged, UpdatePhase phase) {
  WriteBytes(root + "/.playden-install.json", RecordText("1000000000000000101", 1, "old.exe"));
  WriteBytes(root + "/.playden-files", Lines(old_list));
  WriteBytes(root + "/.playden-gog/files", "plan A\n");
  WriteBytes(root + "/.playden-gog/meta/" + std::string(32, 'a'), "meta A");
  WriteBytes(root + "/.playden-gog/meta/" + std::string(32, 'c'), "meta shared");
  WriteBytes(root + "/.playden-gog/manifests/m-old", "manifest A");
  WriteBytes(root + "/.playden-gog/manifests/v1-depot.json", "gen 1 manifest A");
  WriteBytes(root + "/.playden-gog/redist.list", "gog-redist\tv1\n");
  WriteBytes(Work(root) + "/old", Lines(old_list));
  WriteBytes(Work(root) + "/files", Lines(new_list));
  std::vector<std::string> names;
  for (const auto& [path, bytes] : staged) {
    names.push_back(path);
    WriteBytes(Staging(root) + "/" + path, bytes);
  }
  fs::create_directories(Staging(root));
  WriteBytes(Work(root) + "/staged", Lines(names));
  WriteBytes(Work(root) + "/paths", Lines(old_list) + Lines(new_list));
  WriteBytes(Work(root) + "/gog-journal", "f\t0\n");
  WriteBytes(Work(root) + "/gog/files", "plan B\n");
  WriteBytes(Work(root) + "/gog/meta/" + std::string(32, 'b'), "meta B");
  WriteBytes(Work(root) + "/gog/meta/" + std::string(32, 'c'), "meta shared, again");
  WriteBytes(Work(root) + "/gog/manifests/m-new", "manifest B");
  WriteBytes(Work(root) + "/gog/manifests/v1-depot.json", "gen 1 manifest B");
  WriteBytes(Work(root) + "/gog/redist.list", "gog-redist\tv1\nP\tXNA\t.playden-gog/redist/XNA/x.msi\t/qn\n");
  WriteBytes(Work(root) + "/record", RecordText("1000000000000000202", 4242, "new.exe"));
  WriteState(root, phase);
}

ino_t Inode(const std::string& path) {
  struct stat info {};
  CHECK(lstat(path.c_str(), &info) == 0);
  return info.st_ino;
}

// The finished update's folder: the new plan, lists, metas, manifests and record in place, no work folder.
void CheckSwapped(const std::string& root) {
  CHECK(!Exists(Work(root)));
  CHECK(Contents(root + "/.playden-gog/files") == "plan B\n");
  CHECK(Contents(root + "/.playden-gog/redist.list") == "gog-redist\tv1\nP\tXNA\t.playden-gog/redist/XNA/x.msi\t/qn\n");
  CHECK(Contents(root + "/.playden-gog/meta/" + std::string(32, 'a')) == "meta A");
  CHECK(Contents(root + "/.playden-gog/meta/" + std::string(32, 'b')) == "meta B");
  CHECK(Contents(root + "/.playden-gog/meta/" + std::string(32, 'c')) == "meta shared");  // the name is kept
  CHECK(Contents(root + "/.playden-gog/manifests/m-old") == "manifest A");
  CHECK(Contents(root + "/.playden-gog/manifests/m-new") == "manifest B");
  // A gen 1 manifest's name isn't its content's hash: the new build's replaces it.
  CHECK(Contents(root + "/.playden-gog/manifests/v1-depot.json") == "gen 1 manifest B");
  const auto record = playden::installs::ReadInstallRecord(root + "/.playden-install.json");
  CHECK(record && record->gog && record->gog->build_id == "1000000000000000202" && record->install_bytes == 4242);
  CHECK(record->launches.size() == 1 && record->launches[0].executable == "new.exe");
  CHECK(!Exists(root + "/.playden-install.json.tmp") && !Exists(root + "/.playden-files.tmp"));
}

const std::vector<std::string> kOldList{"f\tkeep.dat",      "f\tchange.dat",   "f\tremove.dat", "d\tempty",
                                        "f\tbecomes",       "f\tfolder/a.dat", "f\tfolder/b.dat",
                                        "f\tgone/deep/x.dat", "f\tData/old.dat"};
const std::vector<std::string> kNewList{"f\tkeep.dat",         "f\tchange.dat", "f\tadd.dat", "d\tlogs",
                                        "f\tbecomes/inner.dat", "f\tfolder",     "f\tData/new.dat"};

void WriteOldGame(const std::string& root) {
  WriteBytes(root + "/keep.dat", "keep");
  WriteBytes(root + "/change.dat", "change A");
  WriteBytes(root + "/remove.dat", "remove");
  fs::create_directories(root + "/empty");
  WriteBytes(root + "/becomes", "a file in A");
  WriteBytes(root + "/folder/a.dat", "a");
  WriteBytes(root + "/folder/b.dat", "b");
  WriteBytes(root + "/gone/deep/x.dat", "x");
  WriteBytes(root + "/Data/old.dat", "old");
  // Written by the game: never in a list.
  WriteBytes(root + "/user.cfg", "player's");
  WriteBytes(root + "/Data/save.txt", "player's save");
}

const std::vector<std::pair<std::string, std::string>> kStagedFiles{
    {"change.dat", "change B"}, {"add.dat", "added"}, {"becomes/inner.dat", "inner"}, {"folder", "a file in B"},
    {"Data/new.dat", "new"}};

// Changed files replaced, new ones added, removed ones deleted; a file becomes a folder and a folder of old files a
// file. Unchanged files keep their inode, the game's own files stay, and lines outside an update's paths are skipped.
void TestApplyReplaces() {
  const auto root = Root("apply-replaces");
  WriteOldGame(root);
  auto staged = kStagedFiles;
  // Playden's own files are never put in place from a staged list.
  staged.push_back({".playden-install.json", "{\"evil\":true}"});
  staged.push_back({".PLAYDEN-files", "f\tevil\n"});
  HandMade(root, kOldList, kNewList, staged, UpdatePhase::kReplacing);
  WriteBytes(Work(root) + "/staged", Contents(Work(root) + "/staged") + "../outside\n");
  const auto keep = Inode(root + "/keep.dat");
  std::vector<std::string> seen;
  int scripts = 0;
  GogUpdateHooks hooks;
  hooks.replaced = [&](const std::string& path) { seen.push_back(path); };
  hooks.after_replace = [&](const std::string& folder) {
    ++scripts;
    CHECK(folder == root);
    // The new plan and files are in place, the record not yet.
    CHECK(Contents(root + "/.playden-gog/files") == "plan B\n" && Contents(root + "/change.dat") == "change B");
    CHECK(Contents(root + "/.playden-install.json").find("1000000000000000101") != std::string::npos);
  };
  const auto result = ApplyGogUpdate(root, kNoCancel, hooks);
  CHECK(result.error == Error::kNone && result.install_bytes == 4242 && scripts == 1);
  CHECK(result.replaced == 5 && result.deleted == 3 && result.skipped == 3);
  CHECK((seen == std::vector<std::string>{"change.dat", "add.dat", "becomes/inner.dat", "folder", "Data/new.dat"}));
  CHECK(Contents(root + "/keep.dat") == "keep" && Inode(root + "/keep.dat") == keep);
  CHECK(Contents(root + "/change.dat") == "change B" && Contents(root + "/add.dat") == "added");
  CHECK(Contents(root + "/becomes/inner.dat") == "inner" && Contents(root + "/folder") == "a file in B");
  CHECK(Contents(root + "/Data/new.dat") == "new" && !Exists(root + "/Data/old.dat"));
  CHECK(!Exists(root + "/remove.dat") && !Exists(root + "/gone") && !Exists(root + "/empty"));
  CHECK(fs::is_directory(root + "/logs"));
  CHECK(Contents(root + "/user.cfg") == "player's" && Contents(root + "/Data/save.txt") == "player's save");
  CHECK(Contents(root + "/.playden-files") == Lines(kNewList));
  CHECK(Exists(root + "/" + playden::installs::kFilesChangedName));
  CHECK(!Exists(g_scratch + "/outside"));
  CheckSwapped(root);
  // Done: nothing left to apply.
  CHECK(ApplyGogUpdate(root, kNoCancel).error == Error::kProtocol);
}

// Something the game wrote where a folder now goes: kStorage, the phase still replacing. Once it goes, apply finishes.
// The runner's check before replacing sees the same thing.
void TestApplyBlocked() {
  const auto root = Root("apply-blocked");
  WriteOldGame(root);
  HandMade(root, kOldList, kNewList, kStagedFiles, UpdatePhase::kReplacing);
  WriteBytes(root + "/logs", "the game's own log file");
  CHECK(!playden::stores::GogUpdateFoldersClear(root));
  const auto blocked = ApplyGogUpdate(root, kNoCancel);
  CHECK(blocked.error == Error::kStorage && Phase(root) == UpdatePhase::kReplacing);
  CHECK(Contents(root + "/logs") == "the game's own log file");
  CHECK(Contents(root + "/.playden-install.json").find("1000000000000000101") != std::string::npos);
  CHECK(!Exists(root + "/" + playden::installs::kFilesChangedName));
  std::remove((root + "/logs").c_str());
  CHECK(playden::stores::GogUpdateFoldersClear(root));
  const auto result = ApplyGogUpdate(root, kNoCancel);
  CHECK(result.error == Error::kNone && fs::is_directory(root + "/logs"));
  CHECK(Contents(root + "/becomes/inner.dat") == "inner" && !Exists(root + "/remove.dat"));
  CheckSwapped(root);
}

// A folder that becomes a file, holding something the game wrote (a file, then a link): the runner's check before
// replacing is false, and an apply stops with the old version's files in that folder untouched. An empty folder the
// game made and an old file already gone (an apply interrupted mid-clear) don't stop it.
void TestApplyFolderBecomesFileBlocked() {
  const auto root = Root("apply-folder-blocked");
  WriteOldGame(root);
  HandMade(root, kOldList, kNewList, kStagedFiles, UpdatePhase::kStaging);
  WriteBytes(root + "/folder/save.txt", "player's save");
  CHECK(!playden::stores::GogUpdateFoldersClear(root));
  WriteState(root, UpdatePhase::kReplacing);
  const auto blocked = ApplyGogUpdate(root, kNoCancel);
  CHECK(blocked.error == Error::kStorage && Phase(root) == UpdatePhase::kReplacing);
  CHECK(Contents(root + "/folder/a.dat") == "a" && Contents(root + "/folder/b.dat") == "b");
  CHECK(Contents(root + "/folder/save.txt") == "player's save");
  std::remove((root + "/folder/save.txt").c_str());
  WriteBytes(g_scratch + "/folder-outside/x", "outside");
  CHECK(symlink((g_scratch + "/folder-outside").c_str(), (root + "/folder/link").c_str()) == 0);
  CHECK(!playden::stores::GogUpdateFoldersClear(root));
  CHECK(ApplyGogUpdate(root, kNoCancel).error == Error::kStorage);
  CHECK(Contents(root + "/folder/a.dat") == "a" && Contents(g_scratch + "/folder-outside/x") == "outside");
  std::remove((root + "/folder/link").c_str());
  fs::create_directories(root + "/folder/cache/empty");
  std::remove((root + "/folder/a.dat").c_str());
  CHECK(playden::stores::GogUpdateFoldersClear(root));
  const auto result = ApplyGogUpdate(root, kNoCancel);
  CHECK(result.error == Error::kNone && Contents(root + "/folder") == "a file in B");
  CHECK(Contents(root + "/becomes/inner.dat") == "inner" && !Exists(root + "/remove.dat"));
  CheckSwapped(root);
}

// A measure's folder goes, but never through a link: with .playden-gog a link, or the game folder one, what they lead
// to stays.
void TestRemoveMeasure() {
  const auto root = Root("remove-measure");
  WriteBytes(root + "/.playden-gog/measure/meta/x", "measure");
  WriteBytes(root + "/.playden-gog/files", "plan A\n");
  CHECK(playden::stores::RemoveGogMeasure(root));
  CHECK(!Exists(root + "/.playden-gog/measure") && Contents(root + "/.playden-gog/files") == "plan A\n");
  CHECK(playden::stores::RemoveGogMeasure(root));  // nothing left to delete

  const auto linked = Root("remove-measure-linked");
  WriteBytes(g_scratch + "/measure-outside/measure/x", "outside");
  CHECK(symlink((g_scratch + "/measure-outside").c_str(), (linked + "/.playden-gog").c_str()) == 0);
  CHECK(!playden::stores::RemoveGogMeasure(linked));
  CHECK(Contents(g_scratch + "/measure-outside/measure/x") == "outside");

  const auto link_root = g_scratch + "/remove-measure-link-root";
  WriteBytes(g_scratch + "/measure-target/.playden-gog/measure/x", "outside");
  CHECK(symlink((g_scratch + "/measure-target").c_str(), link_root.c_str()) == 0);
  CHECK(!playden::stores::RemoveGogMeasure(link_root));
  CHECK(Contents(g_scratch + "/measure-target/.playden-gog/measure/x") == "outside");
}

// Cancel after the first renamed file: the phase stays, and the next apply finishes. A restart with update.json at
// deleting (every file already in place) finishes too, and still marks the files changed; so does one stopped while
// finishing.
void TestApplyInterrupted() {
  {
    const auto root = Root("apply-canceled");
    WriteOldGame(root);
    HandMade(root, kOldList, kNewList, kStagedFiles, UpdatePhase::kReplacing);
    std::atomic<bool> cancel{false};
    GogUpdateHooks hooks;
    hooks.replaced = [&](const std::string&) { cancel = true; };
    const auto stopped = ApplyGogUpdate(root, cancel, hooks);
    CHECK(stopped.error == Error::kCanceled && stopped.replaced == 1 && Phase(root) == UpdatePhase::kReplacing);
    CHECK(Contents(root + "/change.dat") == "change B" && !Exists(root + "/add.dat") && Exists(root + "/remove.dat"));
    CHECK(Contents(root + "/.playden-gog/files") == "plan A\n");
    cancel = false;
    std::vector<std::string> seen;
    hooks.replaced = [&](const std::string& path) { seen.push_back(path); };
    const auto result = ApplyGogUpdate(root, cancel, hooks);
    CHECK(result.error == Error::kNone && result.replaced == 4 && seen.front() == "add.dat");
    CHECK(Contents(root + "/change.dat") == "change B" && Contents(root + "/add.dat") == "added");
    CHECK(!Exists(root + "/remove.dat") && Exists(root + "/" + playden::installs::kFilesChangedName));
    CheckSwapped(root);
  }
  {
    // Replacing finished, then Playden stopped: every staged file is in place and update.json says deleting.
    // Made by hand at deleting: the staged files are in the game and staging/ is empty.
    const auto again = Root("apply-restart");
    WriteOldGame(again);
    HandMade(again, kOldList, kNewList, kStagedFiles, UpdatePhase::kDeleting);
    for (const auto& [path, bytes] : kStagedFiles) {
      if (path == "becomes/inner.dat") {
        std::remove((again + "/becomes").c_str());
      } else if (path == "folder") {
        fs::remove_all(again + "/folder");
      }
      WriteBytes(again + "/" + path, bytes);
      std::remove((Staging(again) + "/" + path).c_str());
    }
    const auto result = ApplyGogUpdate(again, kNoCancel);
    CHECK(result.error == Error::kNone && result.replaced == 0 && result.deleted == 3);
    CHECK(!Exists(again + "/remove.dat") && !Exists(again + "/gone") && !Exists(again + "/Data/old.dat"));
    CHECK(Exists(again + "/" + playden::installs::kFilesChangedName));
    CHECK(Contents(again + "/user.cfg") == "player's");
    CheckSwapped(again);
  }
  {
    // Stopped while finishing: the deletion done (no `old`), the plan and record already in place, update.json still
    // there. Apply carries on with what is left and leaves the record as it is.
    const auto root = Root("apply-finishing");
    WriteBytes(root + "/keep.dat", "keep");
    HandMade(root, {"f\tkeep.dat"}, {"f\tkeep.dat"}, {}, UpdatePhase::kDeleting);
    CHECK(std::remove((Work(root) + "/old").c_str()) == 0);
    CHECK(std::rename((Work(root) + "/gog/files").c_str(), (root + "/.playden-gog/files").c_str()) == 0);
    CHECK(std::rename((Work(root) + "/record").c_str(), (root + "/.playden-install.json").c_str()) == 0);
    int scripts = 0;
    GogUpdateHooks hooks;
    hooks.after_replace = [&](const std::string&) { ++scripts; };
    const auto result = ApplyGogUpdate(root, kNoCancel, hooks);
    CHECK(result.error == Error::kNone && result.replaced == 0 && result.deleted == 0 && scripts == 1);
    CHECK(result.install_bytes == 4242 && !Exists(root + "/" + playden::installs::kFilesChangedName));
    CheckSwapped(root);
  }
}

// The same build again (nothing staged, nothing removed): no marker; the lists are still swapped and a leftover measure
// goes. A deletion alone marks the files changed, and so does a staged file alone.
void TestApplyMarker() {
  const std::vector<std::string> list{"f\tkeep.dat"};
  {
    const auto root = Root("apply-nothing");
    WriteBytes(root + "/keep.dat", "keep");
    HandMade(root, list, list, {}, UpdatePhase::kReplacing);
    WriteBytes(root + "/.playden-gog/measure/files", "measured");
    const auto result = ApplyGogUpdate(root, kNoCancel);
    CHECK(result.error == Error::kNone && result.replaced == 0 && result.deleted == 0);
    CHECK(!Exists(root + "/" + playden::installs::kFilesChangedName));
    CHECK(!Exists(root + "/.playden-gog/measure") && Contents(root + "/keep.dat") == "keep");
    CheckSwapped(root);
  }
  {
    const auto root = Root("apply-only-deletes");
    WriteBytes(root + "/keep.dat", "keep");
    WriteBytes(root + "/remove.dat", "remove");
    HandMade(root, {"f\tkeep.dat", "f\tremove.dat"}, list, {}, UpdatePhase::kReplacing);
    CHECK(ApplyGogUpdate(root, kNoCancel).error == Error::kNone);
    CHECK(!Exists(root + "/remove.dat") && Exists(root + "/" + playden::installs::kFilesChangedName));
  }
  {
    // A changed file alone, carried on at deleting with it already in place: the staged list still marks it.
    const auto root = Root("apply-only-changes");
    WriteBytes(root + "/keep.dat", "keep B");
    HandMade(root, list, list, {{"keep.dat", "keep B"}}, UpdatePhase::kDeleting);
    std::remove((Staging(root) + "/keep.dat").c_str());
    const auto result = ApplyGogUpdate(root, kNoCancel);
    CHECK(result.error == Error::kNone && result.replaced == 0 && result.deleted == 0);
    CHECK(Contents(root + "/keep.dat") == "keep B" && Exists(root + "/" + playden::installs::kFilesChangedName));
  }
}

// Support and redist files are updated, a redist package the new build drops goes with its folder, and nothing is
// deleted through a link.
void TestApplySupportAndRedist() {
  const auto root = Root("apply-support");
  const std::string support = ".playden-gog/support/1900000001/save/slot.sav";
  const std::string xna = ".playden-gog/redist/XNA/x.msi";
  const std::string old_redist = ".playden-gog/redist/OLD/setup.exe";
  WriteBytes(root + "/" + support, "slot A");
  WriteBytes(root + "/" + xna, "xna A");
  WriteBytes(root + "/" + old_redist, "old setup");
  WriteBytes(root + "/linked/real.dat", "kept");
  const auto elsewhere = Root("apply-support-elsewhere");
  WriteBytes(elsewhere + "/victim.dat", "not the game's");
  CHECK(symlink(elsewhere.c_str(), (root + "/through").c_str()) == 0);
  HandMade(root, {"f\t" + support, "f\t" + xna, "f\t" + old_redist, "f\tthrough/victim.dat"},
           {"f\t" + support, "f\t" + xna, "f\t.playden-gog/redist/NEW/setup.exe"},
           {{support, "slot B"}, {xna, "xna B"}, {".playden-gog/redist/NEW/setup.exe", "new setup"}},
           UpdatePhase::kReplacing);
  const auto result = ApplyGogUpdate(root, kNoCancel);
  CHECK(result.error == Error::kNone && result.replaced == 3 && result.deleted == 1);
  CHECK(Contents(root + "/" + support) == "slot B" && Contents(root + "/" + xna) == "xna B");
  CHECK(Contents(root + "/.playden-gog/redist/NEW/setup.exe") == "new setup");
  CHECK(!Exists(root + "/" + old_redist) && !Exists(root + "/.playden-gog/redist/OLD"));
  CHECK(Contents(elsewhere + "/victim.dat") == "not the game's");
  CheckSwapped(root);
}

// Discard: refused once replacing has started (the work folder stays whole); at staging the work folder goes, the
// staged .playden-gog files with it, and the game is as it was.
void TestDiscard() {
  const auto root = Root("discard");
  WriteOldGame(root);
  const std::string support = ".playden-gog/support/1900000001/save/slot.sav";
  auto staged = kStagedFiles;
  staged.push_back({support, "slot B"});
  HandMade(root, kOldList, kNewList, staged, UpdatePhase::kReplacing);
  std::string failure;
  CHECK(!playden::stores::DiscardGogUpdate(root, &failure) && !failure.empty());
  CHECK(Contents(Staging(root) + "/" + support) == "slot B" && Exists(Work(root) + "/record"));
  WriteState(root, UpdatePhase::kDeleting);
  CHECK(!playden::stores::DiscardGogUpdate(root));
  WriteState(root, UpdatePhase::kStaging);
  const auto before = Snapshot(root, gog::kUpdateDirName);
  CHECK(playden::stores::DiscardGogUpdate(root, &failure));
  CHECK(!Exists(Work(root)) && Snapshot(root, gog::kUpdateDirName) == before);
  CHECK(ApplyGogUpdate(root, kNoCancel).error == Error::kProtocol);
}

// The runner's DLC sidecar in the work folder goes with it: once the update is in place, and when it is discarded.
void TestDlcSidecarGoes() {
  const auto applied = Root("dlc-sidecar-applied");
  WriteOldGame(applied);
  HandMade(applied, kOldList, kNewList, kStagedFiles, UpdatePhase::kReplacing);
  WriteBytes(Work(applied) + "/" + playden::stores::kGogUpdateDlcName, "1900000002\n");
  CHECK(ApplyGogUpdate(applied, kNoCancel).error == Error::kNone);
  CheckSwapped(applied);
  const auto discarded = Root("dlc-sidecar-discarded");
  WriteOldGame(discarded);
  HandMade(discarded, kOldList, kNewList, kStagedFiles, UpdatePhase::kStaging);
  WriteBytes(Work(discarded) + "/" + playden::stores::kGogUpdateDlcName, "");
  CHECK(playden::stores::DiscardGogUpdate(discarded));
  CHECK(!Exists(Work(discarded)));
}

// What Stage left, put in place the way the runner does: the record first, then the phase, then apply.
GogApplyResult ApplyStaged(const std::string& root) {
  WriteBytes(Work(root) + "/record", RecordText("1000000000000000202", 4242, "new.exe"));
  WriteState(root, UpdatePhase::kReplacing);
  return ApplyGogUpdate(root, kNoCancel);
}

// Stage A to B, then apply: B's files, list and plan, A's removed file gone, the unchanged one's inode kept, a file the
// game wrote kept; the update's meta and manifests join the install's.
void TestApplyStageOutput() {
  Harness harness;
  Publish(harness.stub, ChangesA());
  const auto root = g_scratch + "/apply-staged";
  harness.InstallGame(root);
  WriteBytes(root + "/player.sav", "player");
  const auto keep = Inode(root + "/keep.dat");
  const auto b = Publish(harness.stub, ChangesB());
  CHECK(harness.Stage(root).error == Error::kNone);
  const auto new_plan = ReadFile(Work(root) + "/gog/files");
  const auto new_list = ReadFile(Work(root) + "/files");
  const auto result = ApplyStaged(root);
  CHECK(result.error == Error::kNone && result.replaced == 2 && result.deleted == 1 && result.skipped == 0);
  CHECK(Contents(root + "/keep.dat") == kKeep && Inode(root + "/keep.dat") == keep);
  CHECK(Contents(root + "/change.dat") == kChanged && Contents(root + "/add.dat") == kAdd);
  CHECK(!Exists(root + "/remove.dat") && Contents(root + "/player.sav") == "player");
  CHECK(Contents(root + "/.playden-gog/files") == new_plan && Contents(root + "/.playden-files") == new_list);
  CHECK(Exists(root + "/.playden-gog/meta/" + b.meta_id) && !Exists(Work(root)));
  CHECK(Exists(root + "/" + playden::installs::kFilesChangedName));
  CHECK(gog::ReadPlanHeader(root + "/" + gog::kPlanPath)->build_id == "1000000000000000202");
  // The next update starts from B's plan: the same build again stages nothing.
  CHECK(harness.Stage(root).staged_files == 0);
}

// Stage output with the installed 'Data/' spelling, support files and a redist package, put in place.
void TestApplyStagedSpellingAndSupport() {
  {
    Harness harness;
    Publish(harness.stub, BuildA());
    const auto root = g_scratch + "/apply-spelling";
    harness.InstallGame(root);
    Game b = BuildB();
    b.dependencies.clear();
    b.depots[0].files[1].bytes = kNewData + "changed";
    Publish(harness.stub, b);
    CHECK(harness.Stage(root).error == Error::kNone);
    CHECK(ApplyStaged(root).error == Error::kNone);
    CHECK((Names(root + "/Data") == std::set<std::string>{"File.dat", "new.dat"}));
    CHECK(Names(root).count("Data") == 1 && Names(root).count("data") == 0);
    CHECK(Contents(root + "/Data/File.dat") == kNewData + "changed" && Contents(root + "/Data/new.dat") == kNewData);
    CHECK(Contents(root + "/bin/Game.exe") == kExe + "patched");
  }
  {
    Harness harness;
    Game a;
    GameDepot main;
    main.files.push_back({"DepotFile", "bin\\Game.exe", kExe});
    main.files.push_back({"DepotFile", "save\\slot.sav", "slot\n", {}, true});
    a.depots.push_back(main);
    a.dependencies = {"XNA"};
    PublishXna(harness.stub);
    Publish(harness.stub, a);
    const auto root = g_scratch + "/apply-staged-support";
    harness.InstallGame(root);
    Game b = a;
    b.build_id = "1000000000000000202";
    b.depots[0].files[1].bytes = "slot two\n";
    const auto new_xna = RandomBytes(kXna.size(), 78);
    PublishXna(harness.stub, new_xna);
    Publish(harness.stub, b);
    CHECK(harness.Stage(root).error == Error::kNone);
    const auto result = ApplyStaged(root);
    CHECK(result.error == Error::kNone && result.replaced == 2 && result.skipped == 0);
    CHECK(Contents(root + "/.playden-gog/support/1900000001/save/slot.sav") == "slot two\n");
    CHECK(Contents(root + "/.playden-gog/redist/XNA/xnafx31_redist.msi") == new_xna);
    CHECK(Contents(root + "/bin/Game.exe") == kExe && !Exists(Work(root)));
  }
}

// ---- DLC turned off and on again ----------------------------------------------------------------------------------

const std::string kDlcExtra = RandomBytes(14 * 1024, 51);
const std::string kShared = RandomBytes(8 * 1024, 52);     // the base game's shared/shared.dat
const std::string kDlcShared = RandomBytes(9 * 1024, 53);  // the DLC's version of the same path, which wins
const std::string kDlcInfo = "{\"gameId\":\"1900000002\",\"rootGameId\":\"1900000001\"}\n";
const std::string kDlcScript = "{\"actions\":[],\"productId\":\"1900000002\"}\n";

// The base game, and a DLC with its own file, a support file, its goggame-<id> files and, with `shared`, a file the
// base game has too.
Game DlcGame(bool shared) {
  Game game;
  GameDepot base;
  base.files.push_back({"DepotFile", "bin\\Game.exe", kExe});
  if (shared) base.files.push_back({"DepotFile", "shared\\shared.dat", kShared});
  game.depots.push_back(base);
  GameDepot dlc;
  dlc.product = kDlc;
  dlc.files.push_back({"DepotFile", "dlc\\extra.dat", kDlcExtra});
  dlc.files.push_back({"DepotFile", "save\\dlc.sav", "dlc save\n", {}, true});
  if (shared) dlc.files.push_back({"DepotFile", "shared\\shared.dat", kDlcShared});
  game.depots.push_back(dlc);
  GameDepot dlc_gog;
  dlc_gog.product = kDlc;
  dlc_gog.gog = true;
  dlc_gog.files.push_back({"DepotFile", "goggame-1900000002.info", kDlcInfo});
  dlc_gog.files.push_back({"DepotFile", "goggame-1900000002.script", kDlcScript});
  game.depots.push_back(dlc_gog);
  return game;
}

// The DLC's files, as the drive and the file list hold them.
const char* const kDlcPaths[] = {"dlc/extra.dat", ".playden-gog/support/1900000002/save/dlc.sav",
                                 "goggame-1900000002.info", "goggame-1900000002.script"};

void CheckDlcInstalled(const std::string& root) {
  CHECK(Contents(root + "/dlc/extra.dat") == kDlcExtra);
  CHECK(Contents(root + "/.playden-gog/support/1900000002/save/dlc.sav") == "dlc save\n");
  CHECK(Contents(root + "/goggame-1900000002.info") == kDlcInfo);
  CHECK(Contents(root + "/goggame-1900000002.script") == kDlcScript);
  const auto list = ReadFile(root + "/.playden-files");
  for (const char* path : kDlcPaths) CHECK(list.find(std::string("f\t") + path + "\n") != std::string::npos);
  CHECK((gog::ReadPlanHeader(root + "/" + gog::kPlanPath)->products == std::vector<std::uint64_t>{kBase, kDlc}));
}

void CheckDlcGone(const std::string& root) {
  for (const char* path : kDlcPaths) CHECK(!Exists(root + "/" + path));
  CHECK(!Exists(root + "/dlc") && !Exists(root + "/.playden-gog/support/1900000002"));
  const auto list = ReadFile(root + "/.playden-files");
  CHECK(list.find("dlc/") == std::string::npos && list.find("1900000002") == std::string::npos);
  CHECK((gog::ReadPlanHeader(root + "/" + gog::kPlanPath)->products == std::vector<std::uint64_t>{kBase}));
  CHECK(Contents(root + "/bin/Game.exe") == kExe);
}

// A plan that leaves the DLC out holds only the base game's files and names only the base product; the base game is
// never left out.
void TestPlanExcludesDlc() {
  Harness harness;
  Publish(harness.stub, DlcGame(false));
  const auto root = g_scratch + "/plan-dlc-off";
  CHECK(harness.Install(root).error == Error::kNone);
  CheckDlcInstalled(root);
  CHECK(!harness.Logged("gog plan dlc excluded="));
  const auto before = Snapshot(root, gog::kUpdateDirName);
  const auto layout = gog::UpdateLayout(root);

  harness.excluded = {kDlc};
  auto outcome = harness.Plan(root, layout, true);
  CHECK(outcome.error == Error::kNone && (outcome.products == std::vector<std::uint64_t>{kBase}));
  auto header = gog::ReadPlanHeader(layout.plan);
  CHECK(header && (header->products == std::vector<std::uint64_t>{kBase}));
  auto plan = ReadFile(layout.plan);
  CHECK(plan.find("F\tbin/Game.exe\t") != std::string::npos);
  CHECK(plan.find("dlc/") == std::string::npos && plan.find("1900000002") == std::string::npos);
  CHECK(outcome.files == 1 && outcome.install_bytes == kExe.size());
  CHECK(harness.Logged("gog plan dlc excluded=1"));
  CHECK(Snapshot(root, gog::kUpdateDirName) == before);

  harness.excluded = {kBase};
  outcome = harness.Plan(root, layout, true);
  CHECK(outcome.error == Error::kNone && (outcome.products == std::vector<std::uint64_t>{kBase, kDlc}));
  header = gog::ReadPlanHeader(layout.plan);
  CHECK(header && (header->products == std::vector<std::uint64_t>{kBase, kDlc}));
  plan = ReadFile(layout.plan);
  CHECK(plan.find("F\tbin/Game.exe\t") != std::string::npos && plan.find("F\tdlc/extra.dat\t") != std::string::npos);
  CHECK(harness.Logged("gog plan dlc excluded=0"));

  // The owned list stays mandatory: without it nothing is planned, never the base game alone.
  harness.excluded = {kDlc};
  harness.stub.SetOwned("not json");
  CHECK(harness.Plan(root, layout, true).error == Error::kProtocol);
  harness.CheckLogs();
}

// Gen 1: a depot an excluded DLC shares with the base game (the DLC first in its gameIDs) stays, as the base game's.
void TestPlanGen1ExcludesDlc() {
  Harness harness;
  Publish(harness.stub, BuildA());
  const auto root = g_scratch + "/plan-gen1-dlc-off";
  CHECK(harness.Install(root).error == Error::kNone);
  GameV1 game;
  GameDepotV1 base;
  base.files.push_back({"bin\\Game.exe", kExe});
  game.depots.push_back(base);
  GameDepotV1 dlc;
  dlc.products = {kDlc};
  dlc.files.push_back({"dlc\\extra.dat", kDlcExtra});
  game.depots.push_back(dlc);
  GameDepotV1 shared;
  shared.products = {kDlc, kBase};
  GameFileV1 shared_file{"shared\\shared.dat", kShared};
  shared_file.no_url = true;  // its source is the product the depot was selected for
  shared.files.push_back(shared_file);
  GameFileV1 shared_support{"save\\shared.sav", "shared save\n"};
  shared_support.support = true;
  shared.files.push_back(shared_support);
  game.depots.push_back(shared);
  PublishV1(harness.stub, game);
  const auto layout = gog::UpdateLayout(root);

  // With the DLC the shared depot is the DLC's.
  auto outcome = harness.Plan(root, layout, true);
  CHECK(outcome.error == Error::kNone && outcome.generation == 1);
  CHECK((outcome.products == std::vector<std::uint64_t>{kBase, kDlc}));
  auto plan = ReadFile(layout.plan);
  CHECK(plan.find("F\tdlc/extra.dat\t") != std::string::npos);
  CHECK(plan.find("F\tshared/shared.dat\t1900000002\t") != std::string::npos);
  CHECK(plan.find("F\t.playden-gog/support/1900000002/save/shared.sav\t") != std::string::npos);

  harness.excluded = {kDlc};
  outcome = harness.Plan(root, layout, true);
  CHECK(outcome.error == Error::kNone && (outcome.products == std::vector<std::uint64_t>{kBase}));
  plan = ReadFile(layout.plan);
  CHECK(plan.find("dlc/extra.dat") == std::string::npos && plan.find("support/1900000002") == std::string::npos);
  CHECK(plan.find("F\tbin/Game.exe\t") != std::string::npos);
  CHECK(plan.find("F\tshared/shared.dat\t1900000001\t") != std::string::npos);
  CHECK(plan.find("F\t.playden-gog/support/1900000001/save/shared.sav\t") != std::string::npos);
  CHECK(outcome.install_bytes == kExe.size() + kShared.size() + 12);
  harness.CheckLogs();
}

// Installed with the DLC, then staged without it: nothing new is downloaded, the DLC's files are removed, and once in
// place its folder, support files and goggame-<id> files are gone and the file list no longer names them.
void TestStageDlcRemoved() {
  Harness harness;
  Publish(harness.stub, DlcGame(false));
  const auto root = g_scratch + "/stage-dlc-removed";
  harness.InstallGame(root);
  CheckDlcInstalled(root);
  harness.stub.ClearRequests();
  harness.excluded = {kDlc};
  const auto outcome = harness.Stage(root);
  CHECK(outcome.error == Error::kNone && outcome.staged_files == 0 && outcome.staged_bytes == 0);
  CHECK(outcome.removed_files == 4 && outcome.files == 1);
  CHECK((outcome.products == std::vector<std::uint64_t>{kBase}));
  CHECK(harness.Chunks().empty() && Contents(Work(root) + "/staged").empty());
  CheckDlcInstalled(root);  // nothing changes before the apply
  const auto result = ApplyStaged(root);
  CHECK(result.error == Error::kNone && result.replaced == 0 && result.deleted == 4 && result.skipped == 0);
  CheckDlcGone(root);
  CHECK(!Exists(Work(root)));
  harness.CheckLogs();
}

// A path the base game and the DLC both have (the DLC's won): without the DLC the base game's version is staged and
// put in place.
void TestStageDlcFileOverridesBase() {
  Harness harness;
  Publish(harness.stub, DlcGame(true));
  const auto root = g_scratch + "/stage-dlc-override";
  harness.InstallGame(root);
  CheckDlcInstalled(root);
  CHECK(Contents(root + "/shared/shared.dat") == kDlcShared);
  harness.excluded = {kDlc};
  const auto outcome = harness.Stage(root);
  CHECK(outcome.error == Error::kNone && outcome.staged_files == 1 && outcome.staged_bytes == kShared.size());
  CHECK(outcome.removed_files == 4 && outcome.files == 2);
  CHECK(Contents(Work(root) + "/staged") == "shared/shared.dat\n");
  CHECK(Contents(Staging(root) + "/shared/shared.dat") == kShared);
  CHECK(Contents(root + "/shared/shared.dat") == kDlcShared);
  const auto result = ApplyStaged(root);
  CHECK(result.error == Error::kNone && result.replaced == 1 && result.deleted == 4);
  CHECK(Contents(root + "/shared/shared.dat") == kShared);
  CheckDlcGone(root);
  CHECK(ReadFile(root + "/.playden-files").find("f\tshared/shared.dat\n") != std::string::npos);
  harness.CheckLogs();
}

// Installed without the DLC, then staged with it: its files are staged and put in place.
void TestStageDlcAdded() {
  Harness harness;
  Publish(harness.stub, DlcGame(false));
  const auto root = g_scratch + "/stage-dlc-added";
  harness.excluded = {kDlc};
  harness.InstallGame(root);
  CheckDlcGone(root);
  harness.excluded.clear();
  const auto outcome = harness.Stage(root);
  CHECK(outcome.error == Error::kNone && outcome.staged_files == 4 && outcome.removed_files == 0);
  CHECK((outcome.products == std::vector<std::uint64_t>{kBase, kDlc}));
  CHECK(outcome.staged_bytes == kDlcExtra.size() + 9 + kDlcInfo.size() + kDlcScript.size());
  CHECK(Contents(Staging(root) + "/dlc/extra.dat") == kDlcExtra);
  CHECK(!harness.Logged("gog plan dlc excluded=0"));
  const auto result = ApplyStaged(root);
  CHECK(result.error == Error::kNone && result.replaced == 4 && result.deleted == 0);
  CheckDlcInstalled(root);
  harness.CheckLogs();
}
}  // namespace

int main(int argc, char** argv) {
  CHECK(argc == 3);
  g_fixtures = argv[1];
  g_scratch = argv[2];
  std::error_code ignored;
  fs::remove_all(g_scratch, ignored);
  CHECK(fs::create_directories(g_scratch));
  CHECK(std::string(gog::kUpdateDirName) == playden::installs::kUpdateDirName);
  CHECK(gog::InstallLayout("/g").plan == std::string("/g/") + gog::kPlanPath);
  CHECK(gog::InstallLayout("/g").journal == std::string("/g/") + gog::kJournalPath);
  CHECK(gog::InstallLayout("/g").file_list == std::string("/g/") + gog::kFileListName);
  CHECK(gog::InstallLayout("/g").redist_list == std::string("/g/") + gog::kRedistListPath);
  CHECK(std::string(gog::kInstallRecordName) == playden::steam::DepotInstaller::kInstallRecord);

  TestIdentical();
  TestOneMd5Changed();
  TestSameMd5OtherSize();
  TestChunkLists();
  TestGenerations();
  TestAddedAndRemoved();
  TestDiskFiles();
  TestCaseFolded();
  TestNoOldPlan();
  TestSupportAndRedist();
  TestPhase3Header();
  TestCancel();
  TestUpdateTarget();
  TestSeeds();
  TestPlanIntoUpdateLayout();
  TestPlanWithoutSeeds();
  TestPlanGen1IntoLayout();
  TestPlanNeedsFolder();
  TestStageChanges();
  TestStageResume();
  TestStageReused();
  TestStageSupportAndRedist();
  TestStageNoSpace();
  TestStageCorrupt();
  TestStageSignedOut();
  TestStageGen1ToGen2();
  TestStageSpelling();
  TestStageSameBuild();
  TestStageNeedsInstall();
  TestStageDamagedPlan();
  TestApplyReplaces();
  TestApplyBlocked();
  TestApplyFolderBecomesFileBlocked();
  TestRemoveMeasure();
  TestApplyInterrupted();
  TestApplyMarker();
  TestApplySupportAndRedist();
  TestDiscard();
  TestDlcSidecarGoes();
  TestApplyStageOutput();
  TestApplyStagedSpellingAndSupport();
  TestPlanExcludesDlc();
  TestPlanGen1ExcludesDlc();
  TestStageDlcRemoved();
  TestStageDlcFileOverridesBase();
  TestStageDlcAdded();
  fs::remove_all(g_scratch, ignored);
  std::puts("gog_update: ok");
  return 0;
}
