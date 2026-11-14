// SPDX-License-Identifier: GPL-3.0-or-later
#include "stores/gog_update.h"

#include "gog/dependencies.h"
#include "gog/path_rules.h"
#include "gog/plan_file.h"
#include "installs/install_record.h"
#include "steam/backend/depot_installer.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace playden::stores {
namespace {

using Error = gog::GogInstaller::Outcome::Error;

constexpr char kSupportPrefix[] = ".playden-gog/support/";
constexpr char kMeasureDir[] = ".playden-gog/measure";
constexpr char kWorkGogDir[] = "gog";
constexpr char kWorkJournal[] = "gog-journal";

std::string Join(const std::string& root, const std::string& path) { return root + "/" + path; }

std::string Parent(const std::string& path) {
  const auto slash = path.rfind('/');
  return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

bool Present(const std::string& path) {
  struct stat info {};
  return ::lstat(path.c_str(), &info) == 0;
}

bool IsDirectory(const std::string& path) {
  struct stat info {};
  return ::lstat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

bool IsRegular(const std::string& path) {
  struct stat info {};
  return ::lstat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode);
}

// Whether every folder above <base>/<path> is a real folder (lstat: a link isn't). Nothing is moved or deleted through
// a link, which could lead out of the game's folder.
bool RealParents(const std::string& base, const std::string& path) {
  for (auto parent = Parent(path); !parent.empty(); parent = Parent(parent))
    if (!IsDirectory(Join(base, parent))) return false;
  return true;
}

void SyncFolder(const std::string& folder) {
  const int descriptor = ::open(folder.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (descriptor < 0) return;
  (void)::fsync(descriptor);
  ::close(descriptor);
}

// `text` at `path` through a synced temporary file renamed over it, then its folder synced.
bool WriteSynced(const std::string& path, const std::string& text) {
  const std::string temporary = path + ".tmp";
  FILE* file = std::fopen(temporary.c_str(), "wb");
  if (!file) return false;
  bool ok = text.empty() || std::fwrite(text.data(), 1, text.size(), file) == text.size();
  ok = std::fflush(file) == 0 && ok;
  ok = ::fsync(::fileno(file)) == 0 && ok;
  ok = std::fclose(file) == 0 && ok;
  ok = ok && std::rename(temporary.c_str(), path.c_str()) == 0;
  if (!ok) {
    std::remove(temporary.c_str());
    return false;
  }
  SyncFolder(Parent(path));
  return true;
}

// A folder's entry names, read before any is changed (changing while reading can make readdir skip entries). False
// when it can't be opened.
bool Names(const std::string& folder, std::vector<std::string>* names) {
  DIR* listing = ::opendir(folder.c_str());
  if (!listing) return false;
  while (const auto* entry = ::readdir(listing)) {
    const std::string name = entry->d_name;
    if (name != "." && name != "..") names->push_back(name);
  }
  ::closedir(listing);
  return true;
}

// Deletes a tree without following links.
bool RemoveTree(const std::string& path) {
  struct stat info {};
  if (::lstat(path.c_str(), &info) != 0) return errno == ENOENT;
  if (!S_ISDIR(info.st_mode)) return ::unlink(path.c_str()) == 0 || errno == ENOENT;
  std::vector<std::string> names;
  if (!Names(path, &names)) return false;
  bool ok = true;
  for (const auto& name : names) ok = RemoveTree(Join(path, name)) && ok;
  return (::rmdir(path.c_str()) == 0 || errno == ENOENT) && ok;
}

// A folder that must be a real one: made when missing.
bool EnsureFolder(const std::string& path) {
  if (::mkdir(path.c_str(), 0777) != 0 && errno != EEXIST) return false;
  return IsDirectory(path);
}

void DeepestFirst(std::vector<std::string>* folders) {
  std::sort(folders->begin(), folders->end());
  folders->erase(std::unique(folders->begin(), folders->end()), folders->end());
  std::stable_sort(folders->begin(), folders->end(),
                   [](const std::string& left, const std::string& right) { return left.size() > right.size(); });
}

struct ListedPath {
  char kind = 'f';
  std::string path;
};

// A file list ("f\t<path>", "d\t<path>") or, with `plain`, the staged list (a path per line), keeping the paths an
// update may touch and counting the rest in `skipped`. A torn last line is left out. False when it can't be read.
bool ReadList(const std::string& path, bool plain, std::vector<ListedPath>* entries, std::uint64_t* skipped) {
  gog::LineReader reader(path);
  if (!reader.open()) return false;
  std::string line;
  bool complete = false;
  while (reader.Next(&line, &complete)) {
    if (!complete) continue;
    ListedPath entry;
    if (plain) {
      entry.path = line;
    } else {
      if (line.size() < 3 || line[1] != '\t' || (line[0] != 'f' && line[0] != 'd')) {
        ++*skipped;
        continue;
      }
      entry.kind = line[0];
      entry.path = line.substr(2);
    }
    if (!GogUpdatePath(entry.path)) {
      ++*skipped;
      continue;
    }
    entries->push_back(std::move(entry));
  }
  return !reader.failed();
}

// The two versions' lists, keyed as exFAT compares names.
struct Lists {
  std::vector<ListedPath> old_list, new_list;
  std::unordered_set<std::string> old_files;  // keys of the old version's files
  std::unordered_set<std::string> new_files;  // keys of the new version's files
  std::unordered_set<std::string> new_dirs;
  std::unordered_map<std::string, std::string> new_spelling;  // key -> the path as the new version names it
  std::uint64_t skipped = 0;
};

bool ReadLists(const std::string& work, Lists* lists) {
  if (!ReadList(Join(work, gog::kUpdateOldName), false, &lists->old_list, &lists->skipped) ||
      !ReadList(Join(work, gog::kUpdateFilesName), false, &lists->new_list, &lists->skipped))
    return false;
  for (const auto& entry : lists->old_list)
    if (entry.kind == 'f') lists->old_files.insert(gog::FoldCase(entry.path));
  for (const auto& entry : lists->new_list) {
    const auto key = gog::FoldCase(entry.path);
    (entry.kind == 'd' ? lists->new_dirs : lists->new_files).insert(key);
    lists->new_spelling[key] = entry.path;
  }
  return true;
}

// Makes <root>/<path> and the folders above it, from the top. Where one of them is something else, only a file the old
// version has and the new one doesn't is deleted to make way; anything else (a file the game wrote, a link) stays and
// the folder can't be made. With `apply` false nothing changes: it only says whether the folder could be made.
bool MakeGameDirs(const std::string& root, const std::string& path, const Lists& lists, bool apply = true) {
  std::size_t start = 0;
  while (start <= path.size()) {
    const auto slash = path.find('/', start);
    const auto prefix = path.substr(0, slash);
    const auto full = Join(root, prefix);
    struct stat info {};
    const bool present = ::lstat(full.c_str(), &info) == 0;
    if (present && !S_ISDIR(info.st_mode)) {
      const auto key = gog::FoldCase(prefix);
      if (S_ISLNK(info.st_mode) || !S_ISREG(info.st_mode) || lists.new_files.count(key) || !lists.old_files.count(key))
        return false;
      if (!apply) return true;  // the old version's file goes; nothing is under it
      if (::unlink(full.c_str()) != 0) return false;
    } else if (!present && !apply) {
      return true;
    }
    if (apply && ::mkdir(full.c_str(), 0777) != 0 && !IsDirectory(full)) return false;
    if (slash == std::string::npos) break;
    start = slash + 1;
  }
  return true;
}

// Clears the way for a file at <root>/<path> where the old version had a folder: the old version's files under it that
// the new one doesn't have go, then the folders under it (empty ones the game made too) and the folder itself. The
// whole tree is looked at first: anything else in there (a file the game wrote, a file the new version lists, a link)
// keeps every file where it is, and the update stops there. Nothing is followed through a link. With `apply` false
// nothing changes: it only says whether the folder could be cleared. Read by listing the folders, since the lists
// can't name what the game wrote.
bool ClearFolderForFile(const std::string& root, const std::string& path, const Lists& lists, bool apply = true) {
  if (!RealParents(root, path) || !IsDirectory(Join(root, path))) return false;
  std::vector<std::string> files, folders, pending{path};
  while (!pending.empty()) {
    const auto folder = std::move(pending.back());
    pending.pop_back();
    std::vector<std::string> names;
    if (!Names(Join(root, folder), &names)) return false;
    for (const auto& name : names) {
      const auto child = folder + "/" + name;
      struct stat info {};
      if (::lstat(Join(root, child).c_str(), &info) != 0) {
        if (errno == ENOENT) continue;  // gone since the listing
        return false;
      }
      if (S_ISDIR(info.st_mode)) {
        folders.push_back(child);
        pending.push_back(child);
        continue;
      }
      const auto key = gog::FoldCase(child);
      if (!S_ISREG(info.st_mode) || !lists.old_files.count(key) || lists.new_files.count(key)) return false;
      files.push_back(child);
    }
  }
  if (!apply) return true;
  for (const auto& file : files)
    if (std::remove(Join(root, file).c_str()) != 0 && errno != ENOENT) return false;
  DeepestFirst(&folders);
  for (const auto& folder : folders)
    if (::rmdir(Join(root, folder).c_str()) != 0 && errno != ENOENT) return false;
  return ::rmdir(Join(root, path).c_str()) == 0;
}

// Whether an old version's folder stays: the new version lists it in the same spelling, or in another spelling that
// is the same folder on disk (a filesystem that ignores case).
bool FolderStays(const std::string& root, const Lists& lists, const std::string& path) {
  const auto key = gog::FoldCase(path);
  if (!lists.new_dirs.count(key)) return false;
  const auto spelling = lists.new_spelling.find(key);
  if (spelling == lists.new_spelling.end() || spelling->second == path) return true;
  struct stat old_info {}, new_info {};
  if (::lstat(Join(root, path).c_str(), &old_info) != 0 || ::lstat(Join(root, spelling->second).c_str(), &new_info) != 0)
    return true;
  return old_info.st_dev == new_info.st_dev && old_info.st_ino == new_info.st_ino;
}

bool SameFileOnDisk(const std::string& left, const std::string& right) {
  struct stat a {}, b {};
  if (::lstat(left.c_str(), &a) != 0 || ::lstat(right.c_str(), &b) != 0) return true;  // nothing to tell apart
  return a.st_dev == b.st_dev && a.st_ino == b.st_ino;
}

// `from` renamed to `to` when it is still there.
bool MoveIfPresent(const std::string& from, const std::string& to) {
  if (!Present(from)) return true;
  if (!IsRegular(from)) return false;
  return std::rename(from.c_str(), to.c_str()) == 0;
}

// Each file in `from` moved into `to` by name. A name `to` has already is kept (metas and gen 2 manifests are named by
// their content's hash) and the copy in `from` goes; a gen 1 manifest ("v1-<name>", named by GOG without a hash) is
// replaced by the new build's.
bool MoveNames(const std::string& from, const std::string& to) {
  if (!Present(from)) return true;
  if (!IsDirectory(from) || !EnsureFolder(to)) return false;
  std::vector<std::string> names;
  if (!Names(from, &names)) return false;
  for (const auto& name : names) {
    const auto source = Join(from, name);
    if (!IsRegular(source)) continue;  // the sweep at the end takes it
    const auto target = Join(to, name);
    if (Present(target) && name.compare(0, 3, "v1-") != 0) {
      if (std::remove(source.c_str()) != 0 && errno != ENOENT) return false;
    } else if (std::rename(source.c_str(), target.c_str()) != 0) {
      return false;
    }
  }
  return true;
}

bool ValidRoot(const std::string& root) {
  return root.size() >= 2 && root.front() == '/' && root.back() != '/' && root.find("/..") == std::string::npos;
}

}  // namespace

bool RemoveGogMeasure(const std::string& root) {
  // RemoveTree lstat's only the last component: the game folder and .playden-gog must be real folders, or the delete
  // could follow a link out of the game's folder.
  if (!ValidRoot(root) || !IsDirectory(root)) return false;
  const auto gog_dir = Join(root, gog::kGogDirName);
  if (!Present(gog_dir)) return true;
  if (!IsDirectory(gog_dir)) return false;
  return RemoveTree(Join(root, kMeasureDir));
}

bool GogUpdatePath(std::string_view path) {
  if (path.empty() || path.size() > gog::kMaxPathBytes || !gog::ValidPlanPath(path)) return false;
  const auto first = gog::FoldCase(path.substr(0, path.find('/')));
  if (first.compare(0, 8, ".playden") != 0) return true;
  constexpr std::string_view kSupport = kSupportPrefix;
  if (path.size() > kSupport.size() && path.compare(0, kSupport.size(), kSupport) == 0) return true;
  return gog::IsRedistPlacement(path);
}

bool GogUpdateStaged(const std::string& root) {
  if (!ValidRoot(root) || !IsDirectory(root)) return false;
  const auto state = installs::ReadUpdateState(root);
  if (!state || state->phase != installs::UpdatePhase::kStaging) return false;
  const auto work = Join(root, gog::kUpdateDirName);
  if (!IsDirectory(work) || !IsRegular(Join(work, gog::kUpdateStagedName)) ||
      !IsRegular(Join(work, gog::kUpdateOldName)) || !IsRegular(Join(work, gog::kUpdateFilesName)))
    return false;
  const auto record = installs::ReadInstallRecord(Join(work, kGogUpdateRecordName));
  if (!record || !record->gog) return false;
  const auto header = gog::ReadPlanHeader(gog::UpdateLayout(root).plan);
  return header && header->build_id == record->gog->build_id && header->generation == record->gog->generation;
}

bool GogUpdateFoldersClear(const std::string& root) {
  if (!ValidRoot(root)) return false;
  const auto work = Join(root, gog::kUpdateDirName);
  if (!IsDirectory(work)) return false;
  Lists lists;
  std::vector<ListedPath> staged;
  std::uint64_t skipped = 0;
  if (!ReadLists(work, &lists) || !ReadList(Join(work, gog::kUpdateStagedName), true, &staged, &skipped)) return false;
  for (const auto& entry : staged) {
    if (!Parent(entry.path).empty() && !MakeGameDirs(root, Parent(entry.path), lists, false)) return false;
    // A folder where the file now goes: only the old version's files may be in it.
    if (IsDirectory(Join(root, entry.path)) && !ClearFolderForFile(root, entry.path, lists, false)) return false;
  }
  for (const auto& entry : lists.new_list)
    if (entry.kind == 'd' && !MakeGameDirs(root, entry.path, lists, false)) return false;
  return true;
}

GogApplyResult ApplyGogUpdate(const std::string& root, const std::atomic<bool>& cancel, const GogUpdateHooks& hooks) {
  GogApplyResult result;
  const auto fail = [&result](Error error) {
    result.error = error;
    return result;
  };
  if (!ValidRoot(root) || !IsDirectory(root)) return fail(Error::kProtocol);
  auto state = installs::ReadUpdateState(root);
  if (!state || state->phase == installs::UpdatePhase::kStaging) return fail(Error::kProtocol);
  const auto work = Join(root, gog::kUpdateDirName);
  if (!IsDirectory(work)) return fail(Error::kStorage);
  const auto staging = Join(work, gog::kUpdateStagingName);
  const auto staged_path = Join(work, gog::kUpdateStagedName);
  const auto set_phase = [&](installs::UpdatePhase phase) {
    state->phase = phase;
    return WriteSynced(Join(work, installs::kUpdateStateName), installs::FormatUpdateState(*state));
  };

  // The old list goes once the deletion is done; after that the lists aren't needed.
  const bool deleted = !Present(Join(work, gog::kUpdateOldName));
  Lists lists;
  std::vector<ListedPath> staged;
  if (!deleted) {
    if (!ReadLists(work, &lists)) return fail(Error::kStorage);
    result.skipped += lists.skipped;
    if (!ReadList(staged_path, true, &staged, &result.skipped)) return fail(Error::kStorage);
  }

  // 1. Replacing: each staged file over the game's. One already moved (no staged copy left) is done.
  if (state->phase == installs::UpdatePhase::kReplacing && !deleted) {
    for (const auto& entry : staged) {
      if (cancel.load()) return fail(Error::kCanceled);
      const auto source = Join(staging, entry.path);
      if (!Present(source)) continue;
      if (!IsRegular(source) || !RealParents(staging, entry.path)) {
        ++result.skipped;
        continue;
      }
      const auto parent = Parent(entry.path);
      if (!parent.empty() && !MakeGameDirs(root, parent, lists)) return fail(Error::kStorage);
      const auto target = Join(root, entry.path);
      if (IsDirectory(target) && !ClearFolderForFile(root, entry.path, lists)) return fail(Error::kStorage);
      if (std::rename(source.c_str(), target.c_str()) != 0) return fail(Error::kStorage);
      ++result.replaced;
      if (hooks.replaced) hooks.replaced(entry.path);
    }
    // Folders the new version lists, empty ones included.
    for (const auto& entry : lists.new_list)
      if (entry.kind == 'd' && !MakeGameDirs(root, entry.path, lists)) return fail(Error::kStorage);
    if (!set_phase(installs::UpdatePhase::kDeleting)) return fail(Error::kStorage);
  }

  // 2. Deleting: the old version's files the new one doesn't have; one the new version names in another spelling goes
  // too, unless it is the same file (a filesystem that ignores case).
  if (!deleted) {
    // Decided from the lists, not from this call's work: a finish carried on after an interruption still marks it.
    bool changed = !staged.empty();
    std::vector<std::string> folders;
    for (const auto& entry : lists.old_list) {
      if (cancel.load()) return fail(Error::kCanceled);
      const auto key = gog::FoldCase(entry.path);
      if (entry.kind == 'd') {
        if (!FolderStays(root, lists, entry.path)) folders.push_back(entry.path);
        continue;
      }
      const auto old_path = Join(root, entry.path);
      if (lists.new_files.count(key)) {
        const auto spelling = lists.new_spelling.find(key);
        if (spelling == lists.new_spelling.end() || spelling->second == entry.path ||
            SameFileOnDisk(old_path, Join(root, spelling->second)))
          continue;
      } else if (lists.new_dirs.count(key)) {
        continue;  // made a folder while replacing
      }
      changed = true;
      if (IsDirectory(old_path)) continue;
      // A folder above it became a link: what it leads to is not the game's.
      if (!RealParents(root, entry.path)) continue;
      if (std::remove(old_path.c_str()) == 0) {
        ++result.deleted;
      } else if (errno != ENOENT && errno != ENOTDIR) {
        return fail(Error::kStorage);
      }
      for (auto parent = Parent(entry.path); !parent.empty(); parent = Parent(parent)) folders.push_back(parent);
    }
    // Folders left empty, deepest first; a folder the new version has, or holding anything, stays.
    DeepestFirst(&folders);
    for (const auto& folder : folders)
      if (GogUpdatePath(folder) && !FolderStays(root, lists, folder) && RealParents(root, folder))
        ::rmdir(Join(root, folder).c_str());
    // The game's launch was set up from the old files: it is made again before the game next starts.
    if (changed && !WriteSynced(Join(root, installs::kFilesChangedName), "v1\n")) return fail(Error::kStorage);
    if (std::remove(Join(work, gog::kUpdateOldName).c_str()) != 0 && errno != ENOENT) return fail(Error::kStorage);
  }

  // 3. The new build's plan, metas, manifests, redist list and file list into place.
  const auto work_gog = Join(work, kWorkGogDir);
  const auto live_gog = Join(root, gog::kGogDirName);
  if (!EnsureFolder(live_gog)) return fail(Error::kStorage);
  if (!MoveIfPresent(Join(work_gog, "files"), Join(root, gog::kPlanPath)) ||
      !MoveNames(Join(work_gog, "meta"), Join(live_gog, "meta")) ||
      !MoveNames(Join(work_gog, "manifests"), Join(live_gog, "manifests")) ||
      !MoveIfPresent(Join(work_gog, "redist.list"), Join(root, gog::kRedistListPath)) ||
      !MoveIfPresent(Join(work, gog::kUpdateFilesName), Join(root, gog::kFileListName)))
    return fail(Error::kStorage);
  // A measure of the old build is stale now.
  (void)RemoveGogMeasure(root);

  // 4. The install scripts, then the record: it says the game holds the new build.
  if (hooks.after_replace) hooks.after_replace(root);
  if (!MoveIfPresent(Join(work, kGogUpdateRecordName), Join(root, installs::kInstallRecordName)))
    return fail(Error::kStorage);
  SyncFolder(root);

  // 5. The work folder: the staged files by their list (the console's readdir is unreliable), the journal, gog/, the
  // lists and the runner's DLC sidecar, and the state last. Without the state the game is updated; a folder the sweep
  // leaves goes at uninstall.
  {
    std::vector<ListedPath> leftovers;
    std::uint64_t ignored = 0;
    if (ReadList(staged_path, true, &leftovers, &ignored)) {
      std::vector<std::string> folders;
      for (const auto& entry : leftovers) {
        if (!RealParents(staging, entry.path)) continue;
        std::remove(Join(staging, entry.path).c_str());
        for (auto parent = Parent(entry.path); !parent.empty(); parent = Parent(parent)) folders.push_back(parent);
      }
      DeepestFirst(&folders);
      for (const auto& folder : folders) ::rmdir(Join(staging, folder).c_str());
    }
  }
  ::rmdir(staging.c_str());
  std::remove(Join(work, kWorkJournal).c_str());
  (void)RemoveTree(work_gog);
  for (const char* name : {gog::kUpdateStagedName, gog::kUpdatePathsName, gog::kUpdateFilesName, gog::kUpdateOldName,
                           kGogUpdateDlcName})
    std::remove(Join(work, name).c_str());
  if (std::remove(Join(work, installs::kUpdateStateName).c_str()) != 0 && errno != ENOENT)
    return fail(Error::kStorage);
  (void)RemoveTree(work);

  const auto record = installs::ReadInstallRecord(Join(root, installs::kInstallRecordName));
  result.install_bytes = record ? record->install_bytes : 0;
  result.error = Error::kNone;
  return result;
}

bool DiscardGogUpdate(const std::string& root, std::string* failure) {
  return steam::DiscardManagedUpdate(root, failure);
}

}  // namespace playden::stores
