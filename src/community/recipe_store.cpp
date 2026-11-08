// recipe_store.cpp - The community recipes kept on disk, by content.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "community/recipe_store.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <memory>

#include "community/recipe_release.h"
#include "platform/managed_directory.h"
#include "platform/storage.h"
#include "runtime/private_directories.h"
#include "update/release.h"

namespace playden::community {
namespace {

using platform::DirectoryPolicy;
using platform::ManagedDirectory;

constexpr const char* kObjects = "objects";
constexpr const char* kReleases = "releases";
constexpr const char* kObjectList = "objects.lst";
constexpr const char* kCurrent = "current";
constexpr const char* kVotes = "votes.json";

std::string ObjectPath(std::string_view revision) { return std::string(kObjects) + "/" + std::string(revision) + ".json"; }
std::string ReleasePath(std::string_view tag) { return std::string(kReleases) + "/" + std::string(tag) + ".json"; }

}  // namespace

bool SafeReleaseTag(std::string_view tag) { return tag.size() <= 64 && update::SafeFolderName(tag); }

RecipeStore::RecipeStore(std::filesystem::path root) : root_(std::move(root)) {}

bool RecipeStore::Prepare(std::string* error) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::string reason;
  if (!root_.is_absolute() || root_.lexically_normal() != root_ || !root_.has_parent_path()) {
    if (error) *error = "community recipes folder must be an absolute normalized path";
    return false;
  }
  for (const char* child : {kObjects, kReleases}) {
    if (!runtime::EnsurePrivateDirectories(root_.parent_path(), root_, root_ / child, &reason)) {
      if (error) *error = "community recipes folder unavailable: " + reason;
      return false;
    }
  }
  prepared_ = true;
  return true;
}

bool RecipeStore::OpenExisting() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (prepared_) return true;
  if (!root_.is_absolute() || root_.lexically_normal() != root_ || !root_.has_parent_path()) return false;
  auto directory = ManagedDirectory::Open(root_, DirectoryPolicy::kPrivate);
  if (!directory || !directory->OpenChild(kObjects, DirectoryPolicy::kPrivate)) return false;
  prepared_ = true;
  return true;
}

std::optional<std::string> RecipeStore::ReadFile(const std::string& relative, std::size_t limit) const {
  if (!prepared_) return std::nullopt;
  auto directory = ManagedDirectory::Open(root_, DirectoryPolicy::kPrivate);
  if (!directory) return std::nullopt;
  std::filesystem::path path(relative);
  for (const auto& part : path.parent_path()) {
    auto child = directory->OpenChild(part.string(), DirectoryPolicy::kPrivate);
    if (!child) return std::nullopt;
    directory = std::move(child);
  }
  const int fd = directory->OpenRegularFile(path.filename().string(), true);
  if (fd < 0) return std::nullopt;
  struct stat info{};
  if (fstat(fd, &info) != 0 || info.st_size < 0 || static_cast<std::uint64_t>(info.st_size) > limit) {
    close(fd);
    return std::nullopt;
  }
  std::string bytes;
  bytes.reserve(static_cast<std::size_t>(info.st_size));
  // On the heap: the worker's stack is small.
  auto buffer = std::make_unique<char[]>(16 << 10);
  for (;;) {
    const ssize_t count = read(fd, buffer.get(), 16 << 10);
    if (count < 0 && (errno == EINTR || errno == EAGAIN)) continue;
    if (count < 0 || bytes.size() + static_cast<std::size_t>(count) > limit) {
      close(fd);
      return std::nullopt;
    }
    if (count == 0) break;
    bytes.append(buffer.get(), static_cast<std::size_t>(count));
  }
  close(fd);
  return bytes;
}

bool RecipeStore::WriteFile(const std::string& relative, std::string_view bytes, const char* step,
                            std::string* error) {
  if (!prepared_) {
    if (error) *error = "community recipes folder is not prepared";
    return false;
  }
  if (before_write && !before_write(step)) {
    if (error) *error = std::string("stopped before writing ") + step;
    return false;
  }
  const auto result = Storage(root_.string()).WriteDurable(relative, Bytes(bytes.begin(), bytes.end()));
  if (!result.ok()) {
    if (error) *error = "cannot write " + relative + ": " + result.message();
    return false;
  }
  return true;
}

std::vector<std::string> RecipeStore::ReadList() const {
  std::vector<std::string> revisions;
  const auto text = ReadFile(kObjectList, 1 << 20);  // 16,000 revisions
  if (!text) return revisions;
  std::set<std::string> seen;
  std::size_t start = 0;
  while (start < text->size()) {
    auto end = text->find('\n', start);
    if (end == std::string::npos) end = text->size();
    const std::string line = text->substr(start, end - start);
    if (IsRevision(line) && seen.insert(line).second) revisions.push_back(line);
    start = end + 1;
  }
  return revisions;
}

std::vector<std::string> RecipeStore::ListedObjects() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return ReadList();
}

std::optional<std::string> RecipeStore::ReadObjectUnlocked(std::string_view revision) const {
  if (!IsRevision(revision)) return std::nullopt;
  auto bytes = ReadFile(ObjectPath(revision), kMaxRecipeBytes);
  if (!bytes || Sha256Hex(*bytes) != revision) return std::nullopt;
  return bytes;
}

std::optional<std::string> RecipeStore::ReadObject(std::string_view revision) const {
  // No lock: a refresh writing objects must not hold up the UI thread (see recipe_store.h).
  return ReadObjectUnlocked(revision);
}

std::optional<std::string> RecipeStore::FindObjectByPrefix(std::string_view prefix32) const {
  if (!IsRevisionPrefix(prefix32)) return std::nullopt;
  // No lock: objects.lst is replaced by rename, and each object's hash is checked.
  for (const auto& revision : ReadList())
    if (std::string_view(revision).substr(0, 32) == prefix32 && ReadObjectUnlocked(revision)) return revision;
  return std::nullopt;
}

bool RecipeStore::WriteObjects(const std::vector<std::string>& contents, std::string* error,
                               const std::atomic<bool>* cancel) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto listed = ReadList();
  std::set<std::string> known(listed.begin(), listed.end());
  std::vector<std::pair<std::string, const std::string*>> objects;
  for (const auto& bytes : contents) {
    if (bytes.size() > kMaxRecipeBytes) continue;
    std::string revision = Sha256Hex(bytes);
    if (!IsRevision(revision)) {
      if (error) *error = "cannot hash a recipe";
      return false;
    }
    if (known.insert(revision).second) listed.push_back(revision);
    objects.emplace_back(std::move(revision), &bytes);
  }
  // The list first: an object written after a stop is still found by garbage collection.
  if (listed.size() != ReadList().size()) {
    std::string text;
    for (const auto& revision : listed) text += revision + "\n";
    if (!WriteFile(kObjectList, text, "objects.lst", error)) return false;
  }
  for (const auto& [revision, bytes] : objects) {
    if (cancel && *cancel) {
      if (error) *error = "cancelled";
      return false;
    }
    if (ReadObjectUnlocked(revision)) continue;  // content-addressed: already there
    if (!WriteFile(ObjectPath(revision), *bytes, "object", error)) return false;
  }
  return true;
}

std::optional<std::string> RecipeStore::ReadRelease(std::string_view tag) const {
  if (!SafeReleaseTag(tag)) return std::nullopt;
  std::lock_guard<std::mutex> lock(mutex_);
  return ReadFile(ReleasePath(tag), kMaxIndexBytes);
}

bool RecipeStore::WriteRelease(std::string_view tag, std::string_view index_json, std::string* error) {
  if (!SafeReleaseTag(tag) || index_json.size() > kMaxIndexBytes) {
    if (error) *error = "release tag or index unusable";
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  return WriteFile(ReleasePath(tag), index_json, "release", error);
}

bool RecipeStore::RemoveRelease(std::string_view tag) {
  if (!SafeReleaseTag(tag)) return false;
  std::lock_guard<std::mutex> lock(mutex_);
  if (!prepared_) return false;
  return Storage(root_.string()).RemoveDurable(ReleasePath(tag)).ok();
}

std::optional<std::string> RecipeStore::ReadVotes() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return ReadFile(kVotes, kMaxSummaryBytes);
}

bool RecipeStore::WriteVotes(std::string_view summary_json, std::string* error) {
  if (summary_json.size() > kMaxSummaryBytes) return false;
  std::lock_guard<std::mutex> lock(mutex_);
  return WriteFile(kVotes, summary_json, "votes", error);
}

std::optional<CurrentRelease> RecipeStore::ReadCurrentLocked() const {
  const auto text = ReadFile(kCurrent, 256);
  if (!text) return std::nullopt;
  const auto space = text->find(' ');
  if (space == std::string::npos) return std::nullopt;
  CurrentRelease current;
  current.tag = text->substr(0, space);
  std::string number = text->substr(space + 1);
  while (!number.empty() && (number.back() == '\n' || number.back() == '\r')) number.pop_back();
  if (!SafeReleaseTag(current.tag) || number.empty() || number.size() > 18) return std::nullopt;
  for (char c : number)
    if (c < '0' || c > '9') return std::nullopt;
  current.fetched_at = std::strtoll(number.c_str(), nullptr, 10);
  return current;
}

std::optional<CurrentRelease> RecipeStore::ReadCurrent() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return ReadCurrentLocked();
}

bool RecipeStore::WriteCurrent(const CurrentRelease& current, std::string* error) {
  if (!SafeReleaseTag(current.tag) || current.fetched_at < 0) {
    if (error) *error = "release tag unusable";
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  return WriteFile(kCurrent, current.tag + " " + std::to_string(current.fetched_at) + "\n", "current", error);
}

std::size_t RecipeStore::CollectGarbage(const std::set<std::string>& keep, const std::atomic<bool>* cancel) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!prepared_) return 0;
  // The current release's objects stay whatever keep says. Without a readable current index
  // nothing is deleted: a damaged file must not empty the cache.
  std::set<std::string> current;
  if (const auto pointer = ReadCurrentLocked()) {
    const auto index_json = ReadFile(ReleasePath(pointer->tag), kMaxIndexBytes);
    const auto index = index_json ? ParseRecipeIndex(*index_json, nullptr) : std::nullopt;
    if (!index) return 0;
    for (const auto& [game, listed] : index->games)
      for (const auto& entry : listed.recipes) current.insert(entry.revision);
  } else {
    struct stat info{};
    auto directory = ManagedDirectory::Open(root_, DirectoryPolicy::kPrivate);
    if (!directory || directory->InspectChild(kCurrent, &info) || errno != ENOENT) return 0;
  }
  const auto listed = ReadList();
  std::vector<std::string> remaining;
  std::size_t removed = 0;
  const Storage storage(root_.string());
  for (const auto& revision : listed) {
    // Stopping (Playden closes): what is left stays listed, for the next collection.
    if ((cancel && *cancel) || current.count(revision) || keep.count(revision) || keep.count(revision.substr(0, 32))) {
      remaining.push_back(revision);
      continue;
    }
    if (storage.RemoveDurable(ObjectPath(revision)).ok()) ++removed;
    else remaining.push_back(revision);
  }
  if (remaining.size() != listed.size()) {
    std::string text;
    for (const auto& revision : remaining) text += revision + "\n";
    std::string error;
    WriteFile(kObjectList, text, "objects.lst", &error);
  }
  return removed;
}

}  // namespace playden::community
