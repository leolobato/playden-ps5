#pragma once
// recipe_store.h - The community recipes kept on disk, by content:
//   <root>/objects/<revision>.json   each recipe file, named by the sha256 of its bytes
//   <root>/objects.lst               the revisions of every object written (the console title can't
//                                    list folders); garbage collection reads it
//   <root>/releases/<tag>.json       a release's index.json, as downloaded
//   <root>/votes.json                the last vote totals (summary.json), when there are any
//   <root>/current                   "<tag> <fetched at, unix seconds>": the release in use
// The root (console: /data/playden/user/community-recipes) is private to Playden (0700, files
// 0600) and every write goes through platform::ManagedDirectory (platform::Storage's durable
// writes: a temporary file, then rename). A refresh writes objects.lst first, then the objects,
// the release index and votes, and replaces current last, so a refresh cut short leaves the
// previous release in use. Thread-safe: one mutex around every file operation.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace playden::community {

struct CurrentRelease {
  std::string tag;
  std::int64_t fetched_at = 0;  // unix seconds of the last successful refresh
};

class RecipeStore {
 public:
  // root: an absolute normalized folder whose parent exists (the user data folder).
  explicit RecipeStore(std::filesystem::path root);

  // Creates root, objects/ and releases/ (0700) when missing and checks they are private. Every
  // other call fails until this succeeded once.
  bool Prepare(std::string* error);
  bool prepared() const { return prepared_; }

  // The object's bytes when its sha256 equals revision (full 64 hex); nothing otherwise.
  std::optional<std::string> ReadObject(std::string_view revision) const;
  // The full revision of a listed object whose name starts with prefix32 (32 hex) and whose bytes
  // still match it; nothing when there is none.
  std::optional<std::string> FindObjectByPrefix(std::string_view prefix32) const;
  // Writes the objects not already present (their name is their sha256), after adding them to
  // objects.lst.
  bool WriteObjects(const std::vector<std::string>& contents, std::string* error);

  std::optional<std::string> ReadRelease(std::string_view tag) const;
  bool WriteRelease(std::string_view tag, std::string_view index_json, std::string* error);
  bool RemoveRelease(std::string_view tag);

  std::optional<std::string> ReadVotes() const;
  bool WriteVotes(std::string_view summary_json, std::string* error);

  std::optional<CurrentRelease> ReadCurrent() const;
  bool WriteCurrent(const CurrentRelease& current, std::string* error);

  // Deletes every object listed in objects.lst that is neither in keep (full revisions or 32-hex
  // prefixes) nor listed by the current release's index, then rewrites objects.lst. Returns the
  // number of objects removed. Must not run while a refresh is writing (its new objects are not
  // listed by current yet): CommunityRecipeService serializes the two.
  std::size_t CollectGarbage(const std::set<std::string>& keep);

  // The revisions objects.lst lists.
  std::vector<std::string> ListedObjects() const;

  const std::filesystem::path& root() const { return root_; }

  // Test hook: called with the file about to be written ("objects.lst", "object", "release",
  // "votes", "current"); returning false fails that write as if Playden had stopped there.
  std::function<bool(const std::string& step)> before_write;

 private:
  std::optional<std::string> ReadFile(const std::string& relative, std::size_t limit) const;
  bool WriteFile(const std::string& relative, std::string_view bytes, const char* step, std::string* error);
  std::vector<std::string> ReadList() const;
  std::set<std::string> CurrentRevisions() const;
  std::optional<std::string> ReadObjectLocked(std::string_view revision) const;
  std::optional<CurrentRelease> ReadCurrentLocked() const;

  std::filesystem::path root_;
  std::atomic<bool> prepared_{false};
  mutable std::mutex mutex_;
};

// A release tag safe as a file name: update::SafeFolderName, at most 64 characters.
bool SafeReleaseTag(std::string_view tag);

}  // namespace playden::community
