#pragma once
// The copy plan of an imported game: every folder and regular file of its folder, walked once
// before a "Copy into Playden" is queued and saved in the destination, so the copy, a resume after
// a restart and a cancel all work from the same list. The source is walked with
// platform/directory_walk.h only; a PS5 title can't read a link, so any link fails the plan.
//
// <dest>/.playden-copy/plan.json is one JSON document written one value per line, so it can be
// read back a line at a time (500 000 files would not fit a parsed document on the console):
//   {"version":1,"source":"/mnt/usb0/playden/import/Rally","program":"Bin/Rally.exe",
//    "runtime_number":4026531841,"total":938000000,"file_count":2,"dir_count":1,
//   "dirs":[
//   "Bin"
//   ],
//   "files":[
//   {"path":"Bin/Rally.exe","bytes":1000,"mtime":1760000000,"mtime_nsec":0},
//   {"path":"data.pak","bytes":937999000,"mtime":1760000000,"mtime_nsec":0}
//   ]}
// The folder .playden-copy is the copy's journal folder: RemoveManagedInstall treats a folder
// holding it as Playden's own, so a partial copy can be cancelled or uninstalled.
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace playden::stores::local {

inline constexpr const char* kCopyJournalDir = ".playden-copy";
inline constexpr const char* kCopyPlanFile = "plan.json";
inline constexpr std::size_t kMaxCopyFiles = 500000;
inline constexpr std::uint64_t kMaxCopyBytes = 1ull << 40;  // 1 TiB

struct CopyFile {
  std::string path;  // relative to the game folder, '/' separators
  std::uint64_t bytes = 0;
  std::int64_t mtime = 0;  // seconds
  std::int64_t mtime_nsec = 0;
};

struct CopyPlan {
  std::string source;   // the game folder (a path this process opens)
  std::string program;  // relative to the folder; must be one of `files`
  std::uint32_t runtime_number = 0;  // written to the install record's app_id; 0 when not known yet
  std::vector<std::string> dirs;  // parents before children
  std::vector<CopyFile> files;    // in copy order
  std::uint64_t total = 0;        // bytes of `files`
};

struct CopyPlanBuild {
  bool ok = false;
  CopyPlan plan;
  std::string failure;  // a sentence for the player, naming the path when one is at fault
  int error = 0;        // errno of the failure, when there was one
  std::vector<std::string> skipped;  // top-level names left out: Playden's own (for the log)
};

// Walks `source` (absolute): every folder and regular file, hidden ones included, no depth limit.
// A link anywhere fails it, naming the link; so do an unreadable entry and more than `max_files`
// files or `max_bytes` bytes. Other kinds of entries (sockets, devices) are left out, and so are
// Playden's own names at the top level (the copy journal, the install record and file list, the
// download journal): never copied over Playden's, and never deleted from a drop folder. The web page's
// temporary files are left out at any depth; its previous versions are copied with their files.
CopyPlanBuild BuildCopyPlan(const std::string& source, std::size_t max_files = kMaxCopyFiles,
                            std::uint64_t max_bytes = kMaxCopyBytes);

bool PlanHasFile(const CopyPlan& plan, const std::string& path);

// <root>/.playden-copy/plan.json, and <root>/.playden-files (the paths the copy may create, as the
// depot installer lists them, so removal never needs to list folders). Creates `root`; its parent
// must exist. Written to a temporary name, synced and renamed. Empty, or why it failed.
std::string SaveCopyPlan(const CopyPlan& plan, const std::string& root);
// <root>/.playden-files alone (SaveCopyPlan writes it too): the plan's paths, so RemoveManagedInstall
// removes exactly them. Empty, or why it failed.
std::string SaveFileList(const CopyPlan& plan, const std::string& root);
// The plan saved in `root`; nothing when it's missing or doesn't parse.
std::optional<CopyPlan> LoadCopyPlan(const std::string& root);
std::string CopyPlanPath(const std::string& root);

}  // namespace playden::stores::local
