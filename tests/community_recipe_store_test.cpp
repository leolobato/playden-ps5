// community_recipe_store_test.cpp - The community recipes kept on disk: objects by content,
// objects.lst, release indexes, the current pointer and garbage collection. Also built as
// community_recipe_store_native_policy_test with the console's file rules (no *at calls).
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "community/recipe_release.h"
#include "community/recipe_store.h"

#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <set>
#include <string>

#ifdef PLAYDEN_TEST_AT_UNAVAILABLE
// The console title refuses *at calls with a real folder descriptor: none may be needed.
#include <cerrno>
extern "C" int openat(int, const char*, int, ...) { errno = EINVAL; return -1; }
extern "C" int mkdirat(int, const char*, mode_t) { errno = EINVAL; return -1; }
extern "C" int fstatat(int, const char*, struct stat*, int) { errno = EINVAL; return -1; }
extern "C" int unlinkat(int, const char*, int) { errno = EINVAL; return -1; }
extern "C" int renameat(int, const char*, int, const char*) { errno = EINVAL; return -1; }
#endif

using namespace playden::community;
namespace fs = std::filesystem;

namespace {

int failures = 0;
#define CHECK(cond)                                                                 \
  do {                                                                              \
    if (!(cond)) {                                                                  \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                                   \
    }                                                                               \
  } while (0)

std::string Slurp(const fs::path& path) {
  std::string text;
  if (std::FILE* file = std::fopen(path.c_str(), "rb")) {
    char buffer[1024];
    std::size_t count = 0;
    while ((count = std::fread(buffer, 1, sizeof buffer, file)) > 0) text.append(buffer, count);
    std::fclose(file);
  }
  return text;
}

void Spit(const fs::path& path, const std::string& text) {
  if (std::FILE* file = std::fopen(path.c_str(), "wb")) {
    std::fwrite(text.data(), 1, text.size(), file);
    std::fclose(file);
  }
}

unsigned Mode(const fs::path& path) {
  struct stat info{};
  return lstat(path.c_str(), &info) == 0 ? static_cast<unsigned>(info.st_mode & 07777) : 0;
}

// An index listing the given revisions as recipes of app 10.
std::string Index(const std::vector<std::string>& revisions) {
  std::string apps;
  int n = 0;
  for (const auto& revision : revisions) {
    if (!apps.empty()) apps += ",";
    apps += R"({"id":"community-10-r)" + std::to_string(n++) + R"(","revision":")" + revision + R"("})";
  }
  return R"({"format_version":1,"zip_sha256":")" + std::string(64, 'a') + R"(","apps":{"10":[)" + apps + "]}}";
}

}  // namespace

int main() {
  char pattern[] = "/tmp/playden-recipe-store-XXXXXX";
  CHECK(mkdtemp(pattern));
  const fs::path base = fs::canonical(pattern);
  const fs::path user = base / "user";
  fs::create_directory(user);
  const fs::path root = user / "community-recipes";

  RecipeStore store(root);
  std::string error;
  CHECK(!store.ReadObject(std::string(64, '0')));  // not prepared: nothing
  CHECK(store.Prepare(&error));
  CHECK(Mode(root) == 0700 && Mode(root / "objects") == 0700 && Mode(root / "releases") == 0700);
  CHECK(store.Prepare(&error));  // again: fine

  const std::string a = R"({"recipe":"a"})", b = R"({"recipe":"b"})", c = R"({"recipe":"c"})";
  const std::string ra = Sha256Hex(a), rb = Sha256Hex(b), rc = Sha256Hex(c);
  CHECK(IsRevision(ra) && IsRevision(rb));
  CHECK(store.WriteObjects({a, b}, &error));
  CHECK(store.ReadObject(ra) == a && store.ReadObject(rb) == b);
  CHECK(Mode(root / "objects" / (ra + ".json")) == 0600);
  CHECK(Slurp(root / "objects.lst") == ra + "\n" + rb + "\n");
  // Written again: nothing changes, nothing listed twice.
  CHECK(store.WriteObjects({b, a}, &error));
  CHECK(store.ListedObjects().size() == 2);
  // By prefix: the first 32 hex characters.
  CHECK(store.FindObjectByPrefix(ra.substr(0, 32)) == ra);
  CHECK(!store.FindObjectByPrefix(rc.substr(0, 32)));
  CHECK(!store.FindObjectByPrefix(ra.substr(0, 31)));
  CHECK(!store.FindObjectByPrefix("../../../../etc/passwd"));
  CHECK(!store.ReadObject("../objects.lst"));
  CHECK(!store.ReadObject(ra.substr(0, 32)));
  // A changed object no longer matches its name: refused, and written again when fetched again.
  Spit(root / "objects" / (rb + ".json"), R"({"recipe":"B"})");
  CHECK(!store.ReadObject(rb));
  CHECK(!store.FindObjectByPrefix(rb.substr(0, 32)));
  CHECK(store.WriteObjects({b}, &error));
  CHECK(store.ReadObject(rb) == b);
  // A link in place of an object is refused.
  fs::remove(root / "objects" / (rb + ".json"));
  fs::create_symlink(root / "objects" / (ra + ".json"), root / "objects" / (rb + ".json"));
  CHECK(!store.ReadObject(rb));
  fs::remove(root / "objects" / (rb + ".json"));
  CHECK(store.WriteObjects({b}, &error));

  // Releases and the pointer.
  CHECK(!store.ReadCurrent());
  CHECK(store.WriteRelease("r1", Index({ra}), &error));
  CHECK(store.WriteCurrent({"r1", 1000}, &error));
  CHECK(store.ReadCurrent() && store.ReadCurrent()->tag == "r1" && store.ReadCurrent()->fetched_at == 1000);
  CHECK(store.ReadRelease("r1") == Index({ra}));
  CHECK(!store.WriteRelease("../r2", "{}", &error));
  CHECK(!store.WriteCurrent({".hidden", 1}, &error));
  CHECK(!store.ReadRelease("../current"));

  // A refresh stopped before the pointer: the new objects and index are there, r1 stays current.
  store.before_write = [](const std::string& step) { return step != "current"; };
  CHECK(store.WriteObjects({c}, &error));
  CHECK(store.WriteRelease("r2", Index({rb, rc}), &error));
  CHECK(!store.WriteCurrent({"r2", 2000}, &error));
  CHECK(store.ReadCurrent()->tag == "r1");
  store.before_write = nullptr;
  // Stopped before objects.lst: no object is written either (listed first, so never lost to GC).
  store.before_write = [](const std::string& step) { return step != "objects.lst"; };
  CHECK(!store.WriteObjects({R"({"recipe":"d"})"}, &error));
  CHECK(!store.ReadObject(Sha256Hex(R"({"recipe":"d"})")));
  store.before_write = nullptr;

  // Garbage collection: the current release's objects stay; keep holds a full revision or a prefix.
  CHECK(store.CollectGarbage({}) == 2);  // b and c: only r1 (a) is current
  CHECK(store.ReadObject(ra) && !store.ReadObject(rb) && !store.ReadObject(rc));
  CHECK(Slurp(root / "objects.lst") == ra + "\n");
  CHECK(store.WriteObjects({b, c}, &error));
  CHECK(store.CollectGarbage({rb, rc.substr(0, 32)}) == 0);
  CHECK(store.ReadObject(rb) && store.ReadObject(rc));
  // Switching to r2: a is no longer current, b and c are; a goes once nothing keeps it.
  CHECK(store.WriteCurrent({"r2", 2000}, &error));
  CHECK(store.CollectGarbage({ra.substr(0, 32)}) == 0);
  CHECK(store.ReadObject(ra));
  CHECK(store.CollectGarbage({}) == 1);
  CHECK(!store.ReadObject(ra) && store.ReadObject(rb) && store.ReadObject(rc));
  CHECK(store.RemoveRelease("r1") && !store.ReadRelease("r1"));
  // A current index that can't be read: nothing is deleted.
  CHECK(store.WriteObjects({a}, &error));
  Spit(root / "releases" / "r2.json", "damaged");
  CHECK(store.CollectGarbage({}) == 0);
  CHECK(store.ReadObject(ra));

  // Votes.
  CHECK(!store.ReadVotes());
  CHECK(store.WriteVotes(R"({"recipes":{}})", &error));
  CHECK(store.ReadVotes() == std::string(R"({"recipes":{}})"));

  // Playden closing: a write stops between objects, garbage collection between files, and what
  // collection did not reach stays listed.
  {
    RecipeStore closing(user / "closing");
    CHECK(closing.Prepare(&error));
    std::atomic<bool> stop{false};
    int objects = 0;
    closing.before_write = [&](const std::string& step) {
      if (step == "object" && ++objects == 1) stop = true;  // set while the first object is written
      return true;
    };
    error.clear();
    CHECK(!closing.WriteObjects({a, b, c}, &error, &stop) && error == "cancelled");
    CHECK(objects == 1 && closing.ReadObject(ra) && !closing.ReadObject(rb) && !closing.ReadObject(rc));
    closing.before_write = nullptr;
    stop = false;
    CHECK(closing.WriteObjects({b, c}, &error, &stop));
    stop = true;
    CHECK(closing.CollectGarbage({}, &stop) == 0);  // no current release: all could go, none did
    CHECK(closing.ReadObject(ra) && closing.ReadObject(rb) && closing.ListedObjects().size() == 3);
    stop = false;
    CHECK(closing.CollectGarbage({}, &stop) == 3 && closing.ListedObjects().empty());
  }

  // A public (not private) folder is refused rather than used.
  RecipeStore other(user / "other");
  fs::create_directory(user / "other");
  chmod((user / "other").c_str(), 0755);
  CHECK(!other.Prepare(&error));

  fs::remove_all(base);
  if (failures) std::fprintf(stderr, "%d failure(s)\n", failures);
  else std::printf("community recipe store ok\n");
  return failures ? 1 : 0;
}
