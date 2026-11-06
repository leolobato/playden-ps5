// community_sharing_store_test.cpp - The install secret (sharing-id) and the vote memory
// (votes-cast.json) on disk, when to ask for a vote, and that the secret never survives into a log
// line. Also built as community_sharing_store_native_policy_test with the console's file rules
// (no *at calls).
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "community/install_secret.h"
#include "community/vote_state.h"
#include "diagnostics/redaction.h"

#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

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

using namespace playden;
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
  chmod(path.c_str(), 0600);
}

unsigned Mode(const fs::path& path) {
  struct stat info{};
  return lstat(path.c_str(), &info) == 0 ? static_cast<unsigned>(info.st_mode & 07777) : 0;
}

const std::string kRevA(64, 'a');
const std::string kRevB(64, 'b');
const std::string kId = "community-steam-274520-120hz-left-stick-pointer";

void Secret(const fs::path& base) {
  const std::string root = (base / "community-recipes").string();
  std::string error, note;
  const auto first = LoadOrCreateInstallSecret(root, &error, &note);
  CHECK(first && IsInstallSecret(*first));
  CHECK(note.empty());
  CHECK(Mode(root) == 0700);
  CHECK(Mode(fs::path(root) / kInstallSecretFileName) == 0600);
  CHECK(Slurp(fs::path(root) / kInstallSecretFileName) == *first + "\n");
  // Kept: the same secret on every load.
  const auto again = LoadOrCreateInstallSecret(root, &error);
  CHECK(again && *again == *first);
  // Without the trailing new line too.
  Spit(fs::path(root) / kInstallSecretFileName, *first);
  CHECK(LoadOrCreateInstallSecret(root, &error).value_or("") == *first);
  // Reset: a new one, kept from then on.
  const auto reset = ResetInstallSecret(root, &error);
  CHECK(reset && IsInstallSecret(*reset) && *reset != *first);
  CHECK(LoadOrCreateInstallSecret(root, &error).value_or("") == *reset);
  // A malformed file is replaced, and the note never holds its content.
  for (const char* bad : {"", "XYZ", "0123456789ABCDEF0123456789ABCDEF", "0123456789abcdef0123456789abcdef0",
                          "0123456789abcdef 123456789abcdef"}) {
    Spit(fs::path(root) / kInstallSecretFileName, bad);
    note.clear();
    const auto replaced = LoadOrCreateInstallSecret(root, &error, &note);
    CHECK(replaced && IsInstallSecret(*replaced));
    CHECK(!note.empty());
    if (*bad) CHECK(note.find(bad) == std::string::npos);
  }
  // A link in its place is refused, not followed or replaced.
  const fs::path link = fs::path(root) / kInstallSecretFileName;
  fs::remove(link);
  Spit(base / "elsewhere", "0123456789abcdef0123456789abcdef");
  CHECK(symlink((base / "elsewhere").c_str(), link.c_str()) == 0);
  error.clear();
  CHECK(!LoadOrCreateInstallSecret(root, &error));
  CHECK(!error.empty());
  CHECK(fs::is_symlink(link));
  fs::remove(link);
  // Bad roots.
  CHECK(!LoadOrCreateInstallSecret("relative/root", &error));
  CHECK(!LoadOrCreateInstallSecret(root + "/../community-recipes", &error));
  CHECK(!ResetInstallSecret("", &error));
  // A public folder is not taken over.
  const fs::path open_root = base / "open";
  fs::create_directory(open_root);
  chmod(open_root.c_str(), 0777);
  CHECK(!LoadOrCreateInstallSecret(open_root.string(), &error));

  // Random hex.
  const auto a = RandomHex(16), b = RandomHex(16);
  CHECK(a && b && a->size() == 32 && *a != *b && IsInstallSecret(*a));
  CHECK(!RandomHex(0) && !RandomHex(65));
  CHECK(RandomHex(64).value_or("").size() == 128);
}

void Redaction() {
  const std::string secret = "0123456789abcdef0123456789abcdef";
  const std::string tag = "fedcba9876543210fedcba9876543210";  // a recipe revision tag: also 32 hex
  using diagnostics::RedactSecrets;
  CHECK(RedactSecrets("Authorization: Bearer " + secret) == "Authorization: Bearer <redacted>");
  CHECK(RedactSecrets("authorization: bearer " + secret + "\nnext") == "authorization: bearer <redacted>\nnext");
  CHECK(RedactSecrets("{\"h\":\"Bearer " + secret + "\"}") == "{\"h\":\"Bearer <redacted>\"}");
  CHECK(RedactSecrets("BEARER\t" + secret + " tail") == "BEARER\t<redacted> tail");
  // The known secret anywhere, the file's content in a dump for instance.
  CHECK(RedactSecrets("sharing-id: " + secret + "\n", secret) == "sharing-id: <redacted>\n");
  CHECK(RedactSecrets(secret + secret, secret) == "<redacted><redacted>");
  // Left alone: revision tags, words that only contain "bearer", a Bearer with no token.
  CHECK(RedactSecrets("recipe_version " + tag) == "recipe_version " + tag);
  CHECK(RedactSecrets("recipe_version " + tag, secret) == "recipe_version " + tag);
  CHECK(RedactSecrets("forbearer " + tag) == "forbearer " + tag);
  CHECK(RedactSecrets("Bearer") == "Bearer");
  CHECK(RedactSecrets("Bearer ") == "Bearer ");
  CHECK(RedactSecrets("short", "abc") == "short");
  CHECK(RedactSecrets("") == "");
}

void Votes(const fs::path& base) {
  const std::string root = (base / "community-recipes").string();
  std::string error, note;
  auto state = LoadVoteState(root, &error, &note);
  CHECK(state && state->voting == VotingPreference::kUnset && state->votes.empty() && state->asked.empty());
  CHECK(note.empty());

  // When to ask how a game ran: any game, three minutes or a shorter "Did it start?", two prompts at most.
  const std::string settings = FeedbackSettingsFingerprint("base=playden-default");
  const std::string changed = FeedbackSettingsFingerprint("base=playden-default\nresolution=1280x720");
  CHECK(settings.size() == 16 && settings != changed && settings == FeedbackSettingsFingerprint("base=playden-default"));
  VoteState fresh;
  CHECK(NextFeedbackQuestion(fresh, "steam-10", "0.19", settings, 180, true) == FeedbackQuestion::kHowDidItRun);
  CHECK(NextFeedbackQuestion(fresh, "steam-10", "0.19", settings, 179, true) == FeedbackQuestion::kDidItStart);
  CHECK(NextFeedbackQuestion(fresh, "steam-10", "0.19", settings, 3600, false) == FeedbackQuestion::kNone);
  CHECK(NextFeedbackQuestion(fresh, "", "0.19", settings, 3600, true) == FeedbackQuestion::kNone);
  fresh.NoteFeedback("steam-10", "0.19", settings, false);  // Not now
  CHECK(NextFeedbackQuestion(fresh, "steam-10", "0.19", settings, 3600, true) == FeedbackQuestion::kHowDidItRun);
  fresh.NoteFeedback("steam-10", "0.19", settings, false);
  CHECK(NextFeedbackQuestion(fresh, "steam-10", "0.19", settings, 3600, true) == FeedbackQuestion::kNone);
  CHECK(NextFeedbackQuestion(fresh, "steam-10", "0.19", changed, 30, true) == FeedbackQuestion::kDidItStart);
  CHECK(NextFeedbackQuestion(fresh, "steam-10", "0.20", settings, 3600, true) == FeedbackQuestion::kHowDidItRun);
  fresh.NoteFeedback("steam-10", "0.19", changed, true);  // a report for the new settings
  CHECK(fresh.feedback.size() == 1 && fresh.feedback[0].settings == changed && fresh.feedback[0].prompts == 0 &&
        fresh.feedback[0].answered);
  CHECK(NextFeedbackQuestion(fresh, "steam-10", "0.19", changed, 3600, true) == FeedbackQuestion::kNone);

  // Record replaces the revision's vote and counts as asked.
  state->voting = VotingPreference::kOn;
  state->Record(CastVote{kId, kRevA, "up", ""});
  state->Record(CastVote{kId, kRevA, "down", "slow"});
  state->Record(CastVote{kId, kRevB, "up", ""});
  state->MarkAsked("community-steam-10-other", kRevA);
  state->MarkAsked("community-steam-10-other", kRevA);
  state->NoteFeedback("steam-10", "0.19", settings, false);
  CHECK(state->votes.size() == 2);
  CHECK(state->Find(kId, kRevA) && state->Find(kId, kRevA)->vote == "down" && state->Find(kId, kRevA)->reason == "slow");
  CHECK(state->WasAsked(kId, kRevA) && state->WasAsked(kId, kRevB) && state->asked.size() == 3);

  CHECK(SaveVoteState(root, *state, &error));
  CHECK(Mode(fs::path(root) / kVoteStateFileName) == 0600);
  const std::string saved = Slurp(fs::path(root) / kVoteStateFileName);
  CHECK(saved == "{\"format_version\":1,\"voting\":\"on\",\"votes\":[{\"recipe_id\":\"" + kId + "\",\"revision\":\"" +
                     kRevA + "\",\"vote\":\"down\",\"reason\":\"slow\"},{\"recipe_id\":\"" + kId +
                     "\",\"revision\":\"" + kRevB + "\",\"vote\":\"up\"}],\"asked\":[{\"recipe_id\":\"" + kId +
                     "\",\"revision\":\"" + kRevA + "\"},{\"recipe_id\":\"" + kId + "\",\"revision\":\"" + kRevB +
                     "\"},{\"recipe_id\":\"community-steam-10-other\",\"revision\":\"" + kRevA + "\"}],\"feedback\":[{\"game\":"
                     "\"steam-10\",\"kit\":\"0.19\",\"settings\":\"" + settings + "\",\"prompts\":1,\"answered\":false}]}\n");
  const auto back = LoadVoteState(root, &error);
  CHECK(back && back->voting == VotingPreference::kOn && back->votes.size() == 2 && back->asked.size() == 3 &&
        back->feedback.size() == 1 && back->feedback[0].prompts == 1);
  CHECK(back && SerializeVoteState(*back) == saved);

  state->voting = VotingPreference::kOff;
  CHECK(SaveVoteState(root, *state, &error));
  CHECK(LoadVoteState(root, &error)->voting == VotingPreference::kOff);

  // Bad entries are skipped and never written.
  const auto parsed = ParseVoteState(
      "{\"format_version\":1,\"voting\":\"maybe\",\"votes\":[{\"recipe_id\":\"" + kId + "\",\"revision\":\"short\","
      "\"vote\":\"up\"},{\"recipe_id\":\"" + kId + "\",\"revision\":\"" + kRevA + "\",\"vote\":\"up\",\"reason\":"
      "\"slow\"},{\"recipe_id\":\"" + kId + "\",\"revision\":\"" + kRevA + "\",\"vote\":\"down\",\"reason\":\"crash\"},"
      "7],\"asked\":[{\"recipe_id\":\"a\\\"b\",\"revision\":\"" + kRevA + "\"}]}");
  CHECK(parsed && parsed->voting == VotingPreference::kUnset && parsed->votes.size() == 1 &&
        parsed->votes[0].reason == "crash" && parsed->asked.size() == 1);
  VoteState dirty;
  dirty.votes.push_back(CastVote{"bad\"id", kRevA, "up", ""});
  dirty.asked.emplace_back(kId, "nothex");
  dirty.feedback.push_back(FeedbackSeen{"Steam 10", "0.19", settings, 1, false});
  CHECK(SerializeVoteState(dirty) == "{\"format_version\":1,\"voting\":\"unset\",\"votes\":[],\"asked\":[],\"feedback\":[]}\n");
  // A file from before feedback reads with none.
  CHECK(ParseVoteState("{\"format_version\":1,\"voting\":\"on\",\"votes\":[],\"asked\":[]}")->feedback.empty());
  CHECK(!ParseVoteState("[]") && !ParseVoteState("{\"format_version\":2}") && !ParseVoteState("nope"));

  // A malformed file reads as empty, with a note.
  Spit(fs::path(root) / kVoteStateFileName, "{broken");
  note.clear();
  const auto reset = LoadVoteState(root, &error, &note);
  CHECK(reset && reset->votes.empty() && !note.empty());

  // Bounded.
  VoteState many;
  for (std::size_t i = 0; i < kVoteStateMaxEntries + 5; ++i) {
    char revision[65];
    std::snprintf(revision, sizeof revision, "%064zx", i);
    many.Record(CastVote{kId, revision, "up", ""});
  }
  CHECK(many.votes.size() == kVoteStateMaxEntries && many.asked.size() == kVoteStateMaxEntries);
  CHECK(many.votes.front().revision != std::string(63, '0') + "0");  // the oldest went first
  CHECK(SaveVoteState(root, many, &error));
  const auto big = LoadVoteState(root, &error);
  CHECK(big && big->votes.size() == kVoteStateMaxEntries);

  // Valid votes.
  CHECK(IsValidVote("up", "") && IsValidVote("down", "") && IsValidVote("down", "controls"));
  CHECK(!IsValidVote("up", "slow") && !IsValidVote("down", "bored") && !IsValidVote("", ""));

  CHECK(!LoadVoteState("relative", &error) && !SaveVoteState("relative", VoteState{}, &error));
}

}  // namespace

int main() {
  char pattern[] = "/tmp/playden-sharing-store-XXXXXX";
  const char* made = mkdtemp(pattern);
  CHECK(made);
  if (!made) return 1;
  const fs::path base = fs::canonical(made);
  Secret(base);
  Redaction();
  Votes(base);
  std::error_code ignored;
  fs::remove_all(base, ignored);
  if (failures) std::fprintf(stderr, "%d check(s) failed\n", failures);
  return failures ? 1 : 0;
}
