// SPDX-License-Identifier: GPL-3.0-or-later
#include "web/pairing.h"

#include "runtime/artifact_hash.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

using namespace playden::web;
namespace fs = std::filesystem;

#define CHECK(condition)                                                                  \
  do {                                                                                    \
    if (!(condition)) {                                                                   \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
      std::exit(1);                                                                       \
    }                                                                                     \
  } while (false)

namespace {

std::string ReadText(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream text;
  text << in.rdbuf();
  return text.str();
}

void WriteText(const fs::path& path, const std::string& text) {
  { std::ofstream(path, std::ios::binary | std::ios::trunc) << text; }
  ::chmod(path.c_str(), 0600);
}

// Pairs one browser on a running Pairing: a code, then the right answer.
std::string PairOne(Pairing& pairing, bool remember) {
  std::int64_t wait = 0;
  std::string token;
  CHECK(pairing.Start(true, &wait));
  CHECK(pairing.Confirm(pairing.ActiveCode(), remember, &token) == Pairing::ConfirmResult::kPaired);
  return token;
}

std::string Work(Pairing& pairing) {
  std::string log;
  CHECK(pairing.DoStoreWork({}, &log) == Pairing::StoreWork::kDone);
  return log;
}

// Remembered browsers: kept as hashes across a new Pairing (a restart), for 90 days unused, until Forget all.
void RememberedBrowsers() {
  char pattern[] = "/tmp/playden-web-pairing-XXXXXX";
  const char* made = ::mkdtemp(pattern);
  CHECK(made != nullptr);
  const fs::path base = fs::canonical(made);
  const fs::path root = base / "web-access";
  const fs::path file = root / "browsers.v1";

  std::int64_t wall = 1'800'000'000;
  int sequence = 0;
  Pairing::Options options;
  options.now = [] { return std::int64_t{1000}; };
  options.wall_now = [&] { return wall; };
  options.random_hex = [&]() -> std::optional<std::string> {
    char text[33];
    std::snprintf(text, sizeof text, "%08x%024x", ++sequence * 104729, sequence);
    return std::string(text);
  };
  options.store_root = root.string();
  const auto day = std::int64_t{86'400};

  // A missing folder and file: nothing remembered yet, and the folder is made private.
  std::string remembered, session;
  {
    Pairing first(options);
    int changes = 0;
    first.SetStoreChanged([&] { ++changes; });
    Work(first);
    CHECK(fs::is_directory(root));
    struct stat info {};
    CHECK(::stat(root.c_str(), &info) == 0 && (info.st_mode & 0777) == 0700);
    CHECK(first.Count() == 0);
    remembered = PairOne(first, true);
    CHECK(changes == 1);  // a remembered browser asks for a write at once
    session = PairOne(first, false);
    CHECK(changes == 1);  // a browser paired for this session only never touches the file
    CHECK(first.Check(remembered) == Pairing::Browser::kRemembered);
    CHECK(first.Check(session) == Pairing::Browser::kSession);
    CHECK(first.Count() == 2);
    CHECK(Work(first).find("browsers saved") != std::string::npos);
    // Only the hash is kept, never the token.
    const auto stored = ReadText(file);
    CHECK(stored.find(remembered) == std::string::npos && stored.find(session) == std::string::npos);
    CHECK(stored.find(*playden::runtime::HashArtifactBytes(remembered)) != std::string::npos);
    CHECK(stored.find(*playden::runtime::HashArtifactBytes(session)) == std::string::npos);
    struct stat file_info {};
    CHECK(::stat(file.c_str(), &file_info) == 0 && (file_info.st_mode & 0777) == 0600);
  }

  // Restart: the remembered browser is still paired, the session one isn't.
  {
    Pairing second(options);
    CHECK(second.Check(remembered) == Pairing::Browser::kNone);  // not read yet
    CHECK(Work(second).find("remembered=1") != std::string::npos);
    CHECK(second.Check(remembered) == Pairing::Browser::kRemembered);
    CHECK(second.Check(session) == Pairing::Browser::kNone);
    CHECK(second.Count() == 1);
    // A token that isn't 64 hex digits is never hashed or matched.
    CHECK(second.Check(remembered.substr(1)) == Pairing::Browser::kNone);
    CHECK(second.Check(*playden::runtime::HashArtifactBytes(remembered)) == Pairing::Browser::kNone);

    // Uses within a day don't write; after a day the last use is written again.
    const auto before = ReadText(file);
    wall += day - 1;
    CHECK(second.Check(remembered) == Pairing::Browser::kRemembered);
    CHECK(Work(second).empty() && ReadText(file) == before);
    wall += 1;
    CHECK(second.Check(remembered) == Pairing::Browser::kRemembered);
    CHECK(Work(second).find("browsers saved") != std::string::npos && ReadText(file) != before);
  }

  // Each use starts the 90 days over: 89 days after that last write it's still paired; 90 days unused and it isn't.
  {
    wall += 89 * day;
    Pairing third(options);
    Work(third);
    CHECK(third.Check(remembered) == Pairing::Browser::kRemembered);
    Work(third);
  }
  {
    wall += 90 * day - 1;
    Pairing fourth(options);
    Work(fourth);
    CHECK(fourth.Count() == 1);
    wall += 1;
    CHECK(fourth.Count() == 0);
    CHECK(fourth.Check(remembered) == Pairing::Browser::kNone);
    Work(fourth);
    CHECK(ReadText(file).find(*playden::runtime::HashArtifactBytes(remembered)) == std::string::npos);
  }
  {
    // Expired entries are dropped on reading too.
    Pairing fifth(options);
    const auto kept = PairOne(fifth, true);
    Work(fifth);
    wall += 90 * day;
    Pairing sixth(options);
    CHECK(Work(sixth).find("remembered=0") != std::string::npos);
    CHECK(sixth.Check(kept) == Pairing::Browser::kNone);
  }

  // Forget all: this session's browsers and the remembered ones, in the file too.
  {
    Pairing seventh(options);
    Work(seventh);
    const auto kept = PairOne(seventh, true);
    const auto here = PairOne(seventh, false);
    Work(seventh);
    seventh.ForgetAll();
    CHECK(seventh.Count() == 0);
    CHECK(seventh.Check(kept) == Pairing::Browser::kNone && seventh.Check(here) == Pairing::Browser::kNone);
    Work(seventh);
    Pairing eighth(options);
    Work(eighth);
    CHECK(eighth.Check(kept) == Pairing::Browser::kNone && eighth.Count() == 0);

    // Forget all before the file was read forgets what the file holds.
    const auto again = PairOne(eighth, true);
    Work(eighth);
    Pairing ninth(options);
    ninth.ForgetAll();
    Work(ninth);
    CHECK(ninth.Check(again) == Pairing::Browser::kNone);
    Pairing tenth(options);
    Work(tenth);
    CHECK(tenth.Check(again) == Pairing::Browser::kNone);
  }

  // The cap: the browser used least recently goes, remembered or not.
  {
    auto capped = options;
    capped.max_tokens = 2;
    Pairing pairing(capped);
    Work(pairing);
    const auto a = PairOne(pairing, true);
    wall += 10;
    const auto b = PairOne(pairing, false);
    wall += 10;
    CHECK(pairing.Check(a) == Pairing::Browser::kRemembered);
    wall += 10;
    const auto c = PairOne(pairing, true);
    CHECK(pairing.Check(a) == Pairing::Browser::kRemembered);
    CHECK(pairing.Check(b) == Pairing::Browser::kNone);
    CHECK(pairing.Check(c) == Pairing::Browser::kRemembered);
    wall += 10;
    const auto d = PairOne(pairing, false);  // forgets a, the remembered one used least recently: the file changes
    CHECK(pairing.Check(a) == Pairing::Browser::kNone);
    Work(pairing);
    Pairing restarted(capped);
    Work(restarted);
    CHECK(restarted.Check(a) == Pairing::Browser::kNone && restarted.Check(c) == Pairing::Browser::kRemembered);
    CHECK(restarted.Check(d) == Pairing::Browser::kNone);
    restarted.ForgetAll();
    Work(restarted);
  }

  // A corrupt file: nothing is remembered, pairing still works, and the next write replaces it.
  for (const char* damaged : {"garbage\n", "playden-web-browsers 1\nnot-a-hash 12\n", "playden-web-browsers 2\n",
                              "playden-web-browsers 1\n0123", "",
                              // A last use too large for a number.
                              "playden-web-browsers 1\naaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa 9999999999999999999\n"}) {
    WriteText(file, damaged);
    Pairing pairing(options);
    CHECK(Work(pairing).find("refused") != std::string::npos);
    CHECK(pairing.Count() == 0);
    const auto fresh = PairOne(pairing, true);
    Work(pairing);
    Pairing restarted(options);
    CHECK(Work(restarted).find("remembered=1") != std::string::npos);
    CHECK(restarted.Check(fresh) == Pairing::Browser::kRemembered);
    restarted.ForgetAll();
    Work(restarted);
  }
  // A file too large is refused the same way.
  WriteText(file, std::string("playden-web-browsers 1\n") + std::string(8192, 'a'));
  {
    Pairing pairing(options);
    CHECK(Work(pairing).find("refused") != std::string::npos && pairing.Count() == 0);
  }
  // The file missing again (deleted by hand): nothing remembered, and no complaint.
  fs::remove(file);
  {
    Pairing pairing(options);
    const auto log = Work(pairing);
    CHECK(log.find("remembered=0") != std::string::npos && log.find("refused") == std::string::npos);
  }

  // The file lock is busy (a game's setup): nothing is read or written until it's free.
  {
    Pairing pairing(options);
    Work(pairing);
    const auto token = PairOne(pairing, true);
    std::string log;
    CHECK(pairing.DoStoreWork([] { return std::shared_ptr<void>(); }, &log) == Pairing::StoreWork::kBusy);
    CHECK(!fs::exists(file) || ReadText(file).find(*playden::runtime::HashArtifactBytes(token)) == std::string::npos);
    CHECK(pairing.DoStoreWork([] { return std::make_shared<int>(0); }, &log) == Pairing::StoreWork::kDone);
    CHECK(ReadText(file).find(*playden::runtime::HashArtifactBytes(token)) != std::string::npos);
  }

  // A write that fails is owed until it works (Forget all must not come undone at the next start).
  {
    Pairing pairing(options);
    Work(pairing);
    const auto token = PairOne(pairing, true);
    fs::remove(file);
    fs::create_directory(file);  // the file's name taken by a folder: the rename onto it fails
    std::string log;
    CHECK(pairing.DoStoreWork({}, &log) == Pairing::StoreWork::kFailed && log.find("not saved") != std::string::npos);
    fs::remove(file);
    CHECK(pairing.DoStoreWork({}, &log) == Pairing::StoreWork::kDone && log.find("browsers saved") != std::string::npos);
    CHECK(ReadText(file).find(*playden::runtime::HashArtifactBytes(token)) != std::string::npos);
    pairing.ForgetAll();
    Work(pairing);
  }

  // A browser paired before the file is read can push a remembered one past the cap: the file forgets it too.
  {
    std::vector<std::string> kept;
    {
      Pairing filler(options);
      Work(filler);
      for (std::size_t i = 0; i < options.max_tokens; ++i) {
        kept.push_back(PairOne(filler, true));
        wall += 10;
      }
      Work(filler);
    }
    Pairing pairing(options);
    const auto here = PairOne(pairing, false);  // before the file is read
    Work(pairing);
    CHECK(pairing.Count() == options.max_tokens && pairing.Check(here) == Pairing::Browser::kSession);
    CHECK(pairing.Check(kept.front()) == Pairing::Browser::kNone);
    Pairing restarted(options);
    Work(restarted);
    CHECK(restarted.Check(kept.front()) == Pairing::Browser::kNone);
    CHECK(restarted.Check(kept.back()) == Pairing::Browser::kRemembered);
    restarted.ForgetAll();
    Work(restarted);
  }

  // A last use in the future (the clock went back) is written as now, so restarts can't keep an unused browser.
  {
    WriteText(file, "playden-web-browsers 1\n" + std::string(64, 'b') + " " + std::to_string(wall + 200 * day) + "\n");
    {
      Pairing pairing(options);
      CHECK(Work(pairing).find("browsers saved") != std::string::npos);
      CHECK(pairing.Count() == 1);
    }
    CHECK(ReadText(file).find(std::to_string(wall)) != std::string::npos);
    wall += 90 * day;
    Pairing later(options);
    Work(later);
    CHECK(later.Count() == 0);
  }

  // No folder to keep them in: remembering falls back to this session.
  {
    auto nowhere = options;
    nowhere.store_root = "relative/web-access";
    Pairing pairing(nowhere);
    const auto token = PairOne(pairing, true);
    CHECK(Work(pairing).find("unavailable") != std::string::npos);
    CHECK(pairing.Check(token) == Pairing::Browser::kRemembered);
    CHECK(Work(pairing).empty());  // it doesn't try again
    auto none = options;
    none.store_root.clear();
    Pairing memory(none);
    const auto only = PairOne(memory, true);
    CHECK(memory.Check(only) == Pairing::Browser::kSession);
  }

  fs::remove_all(base);
}

}  // namespace

int main() {
  std::int64_t now = 1000;
  int sequence = 0;
  Pairing::Options options;
  options.now = [&] { return now; };
  options.random_hex = [&]() -> std::optional<std::string> {
    char text[33];
    std::snprintf(text, sizeof text, "%08x%024x", ++sequence * 7919, sequence);
    return std::string(text);
  };
  Pairing pairing(options);
  std::int64_t wait = 0;
  std::string token;

  // Nothing to confirm yet, and no code on the TV.
  CHECK(pairing.Confirm("000000", false, &token) == Pairing::ConfirmResult::kNoCode);
  CHECK(pairing.ActiveCode().empty());
  CHECK(!pairing.Paired("") && !pairing.Paired("x"));

  // A code: six digits; asking again keeps it unless a new one is wanted.
  CHECK(pairing.Start(false, &wait));
  const auto code = pairing.ActiveCode();
  CHECK(code.size() == 6 && code.find_first_not_of("0123456789") == std::string::npos);
  CHECK(pairing.Start(false, &wait) && pairing.ActiveCode() == code);
  CHECK(pairing.Start(true, &wait) && pairing.ActiveCode() != code);

  // Right: a token that stays valid, and the code is used up.
  const auto second = pairing.ActiveCode();
  CHECK(pairing.Confirm(second, false, &token) == Pairing::ConfirmResult::kPaired);
  CHECK(token.size() == 64 && pairing.Paired(token));
  CHECK(pairing.ActiveCode().empty());
  CHECK(pairing.Confirm(second, false, nullptr) == Pairing::ConfirmResult::kNoCode);

  // Expiry.
  CHECK(pairing.Start(false, &wait));
  now += options.code_lifetime_ms;
  CHECK(pairing.ActiveCode().empty());
  CHECK(pairing.Paired(token));

  // Wrong tries count across new codes: asking for another doesn't give more guesses.
  const auto wrong_of = [](const std::string& right) { return right == "000000" ? std::string("000001") : std::string("000000"); };
  for (int i = 0; i < options.max_wrong_tries - 1; ++i) {
    CHECK(pairing.Start(true, &wait));
    CHECK(pairing.Confirm(wrong_of(pairing.ActiveCode()), false, nullptr) == Pairing::ConfirmResult::kWrong);
  }
  CHECK(pairing.Start(true, &wait));
  const auto last = pairing.ActiveCode();
  CHECK(pairing.Confirm(wrong_of(last), false, nullptr) == Pairing::ConfirmResult::kLocked);
  CHECK(pairing.ActiveCode().empty());
  CHECK(!pairing.Start(true, &wait) && wait == options.lockout_ms);
  CHECK(pairing.Confirm(last, false, nullptr) == Pairing::ConfirmResult::kLocked);
  now += options.lockout_ms;
  CHECK(pairing.Start(false, &wait));

  // Cancel from the TV.
  pairing.Cancel();
  CHECK(pairing.ActiveCode().empty());

  // At most a few browsers: the oldest goes first.
  std::string first_token;
  for (std::size_t i = 0; i <= options.max_tokens; ++i) {
    CHECK(pairing.Start(true, &wait));
    std::string minted;
    CHECK(pairing.Confirm(pairing.ActiveCode(), false, &minted) == Pairing::ConfirmResult::kPaired);
    if (i == 0) first_token = minted;
    CHECK(pairing.Paired(minted));
  }
  CHECK(!pairing.Paired(first_token));

  // No random source: no code, and no wait to report.
  Pairing::Options broken = options;
  broken.random_hex = []() -> std::optional<std::string> { return std::nullopt; };
  Pairing none(broken);
  CHECK(!none.Start(false, &wait) && wait == 0);
  RememberedBrowsers();
  std::puts("web_pairing: ok");
}
