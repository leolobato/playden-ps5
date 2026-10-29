#include "runtime/layout_migration.h"
#include "runtime/game_install.h"
#include "runtime/private_directories.h"
#include "platform/storage.h"
#include "platform/directory_walk.h"
#include "steam/backend/json.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <vector>

namespace playden::runtime {
namespace fs = std::filesystem;
namespace {
constexpr const char* kMovedMarker = "MOVED.txt";
constexpr const char* kGamesMarker = "games-MOVED.txt";

enum Step { kNone, kUser, kLogs, kGames, kGamesMoved, kDone };

Step StepIndex(const std::string& step) {
  if (step == "user") return kUser;
  if (step == "logs") return kLogs;
  if (step == "games") return kGames;
  if (step == "games-moved") return kGamesMoved;
  if (step == "done") return kDone;
  // An interrupted revert left both layouts in use: resume the move from the start.
  if (step.rfind("revert-", 0) == 0) return kUser;
  return kNone;
}

void Log(const LayoutOps& ops, const std::string& line) {
  if (ops.log) ops.log("layout: " + line);
}

std::error_code Rename(const LayoutOps& ops, const fs::path& from, const fs::path& to) {
  if (ops.rename) return ops.rename(from, to);
  if (::rename(from.c_str(), to.c_str()) == 0) return {};
  return {errno, std::generic_category()};
}

bool AsRoot(const LayoutOps& ops, const std::function<bool()>& work) {
  return ops.as_root ? ops.as_root(work) : work();
}

bool SyncDirectory(const LayoutOps& ops, const fs::path& directory) {
  if (ops.sync) return ops.sync(directory);
  const int fd = platform::OpenDirectoryWalk(directory);
  if (fd < 0) return false;
  const bool ok = ::fsync(fd) == 0;
  const int error = errno;
  ::close(fd);
  errno = error;
  return ok;
}
std::error_code SyncRename(const LayoutOps& ops, const fs::path& from, const fs::path& to) {
  if (!SyncDirectory(ops, from.parent_path()) || !SyncDirectory(ops, to.parent_path()))
    return {errno ? errno : EIO, std::generic_category()};
  return {};
}

// Renames as the app user; a folder only root may write gets a second try as root.
std::error_code RenameEntry(const LayoutOps& ops, const fs::path& from, const fs::path& to) {
  auto error = Rename(ops, from, to);
  if ((error.value() == EACCES || error.value() == EPERM) && ops.as_root) {
    std::error_code rooted;
    const bool ran = ops.as_root([&] { rooted = Rename(ops, from, to); return !rooted; });
    if (!rooted && ran) error.clear();
    if (rooted) error = rooted;
  }
  return error ? error : SyncRename(ops, from, to);
}

std::string Describe(const std::error_code& error) {
  return "errno=" + std::to_string(error.value()) + " (" + error.message() + ")";
}

// 0: missing, 1: directory, 2: something else.
int Kind(const fs::path& path) {
  struct stat info{};
  if (::lstat(path.c_str(), &info) != 0) return errno == ENOENT ? 0 : 2;
  return S_ISDIR(info.st_mode) ? 1 : 2;
}

bool Exists(const fs::path& path) {
  struct stat info{};
  return ::lstat(path.c_str(), &info) == 0 || errno != ENOENT;
}

// Entries, without the markers this move leaves behind. Nothing when unreadable.
std::optional<std::vector<std::string>> Entries(const fs::path& directory) {
  std::vector<std::string> names;
  std::error_code error;
  for (fs::directory_iterator it(directory, error), end; !error && it != end; it.increment(error)) {
    const auto name = it->path().filename().string();
    if (name != kMovedMarker && name != kGamesMarker) names.push_back(name);
  }
  if (error) return std::nullopt;
  std::sort(names.begin(), names.end());
  return names;
}

// Where a log named `name` lands in `to`: its own name, else "name.moved", "name.moved.2", ...
// (each interrupted start can leave one more copy). Nothing when all are taken.
std::optional<fs::path> LogTarget(const fs::path& to, const std::string& name) {
  if (!Exists(to / name)) return to / name;
  for (int copy = 1; copy <= 100; ++copy) {
    auto target = to / (name + ".moved" + (copy == 1 ? std::string() : "." + std::to_string(copy)));
    if (!Exists(target)) return target;
  }
  return std::nullopt;
}

bool EmptyDirectory(const fs::path& directory) {
  const auto names = Entries(directory);
  return names && names->empty();
}

// Shared parents are sticky; the games root stays non-sticky for imported root-owned games.
bool EnsureSharedDirectory(const LayoutOps& ops, const fs::path& directory, mode_t mode = 01777) {
  const int kind = Kind(directory);
  if (kind == 1) return SyncDirectory(ops, directory) && SyncDirectory(ops, directory.parent_path());
  if (kind == 2) return false;
  const auto make = [&] {
    const int parent = platform::OpenDirectoryWalk(directory.parent_path());
    if (parent < 0) return false;
    const auto name = directory.filename().string();
    const bool made = platform::MakeWalkChild(parent, name.c_str(), mode) == 0;
    const int fd = made ? platform::OpenWalkChild(parent, name.c_str(),
        O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC) : -1;
    const bool ok = fd >= 0 && ::fchmod(fd, mode) == 0 &&
                    SyncDirectory(ops, directory) && SyncDirectory(ops, directory.parent_path());
    const int error = errno;
    if (fd >= 0) ::close(fd);
    ::close(parent);
    errno = error;
    return ok;
  };
  if (make()) return true;
  return (errno == EACCES || errno == EPERM) && AsRoot(ops, make) && Kind(directory) == 1;
}

void WriteMarker(const LayoutOps& ops, const fs::path& file, const std::string& text) {
  FILE* out = std::fopen(file.c_str(), "w");
  const bool written = out && std::fputs(text.c_str(), out) >= 0;
  if (out && std::fclose(out) != 0) Log(ops, "could not close " + file.string());
  if (!written) Log(ops, "could not write " + file.string() + ": " + std::strerror(errno));
}

std::string Escape(const std::string& text) {
  std::string out;
  for (const unsigned char c : text) {
    if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
    else if (c < 0x20) { char hex[8]; std::snprintf(hex, sizeof hex, "\\u%04x", c); out += hex; }
    else out += static_cast<char>(c);
  }
  return out;
}

bool PrepareState(const LayoutPaths& paths, std::string* error) {
  return EnsurePrivateDirectories(paths.boundary, paths.state_root, paths.state_root, error);
}

bool SaveJournal(const fs::path& state_root, const LayoutJournal& journal, std::string* error) {
  std::string text = "{\"step\":\"" + Escape(journal.step) + "\",\"from\":\"" + Escape(journal.from) +
                     "\",\"to\":\"" + Escape(journal.to) + "\",\"postponed\":" +
                     std::to_string(journal.postponed) + ",\"entries\":[";
  for (std::size_t i = 0; i < journal.entries.size(); ++i) {
    const auto& entry = journal.entries[i];
    if (i) text += ',';
    text += "{\"from\":\"" + Escape(entry.from) + "\",\"to\":\"" + Escape(entry.to) +
            "\",\"moved\":" + (entry.moved ? "true" : "false") + "}";
  }
  text += "]}\n";
  if (text.size() > 64 * 1024 || journal.entries.size() > 512) {
    if (error) *error = "layout journal has too many entries";
    return false;
  }
  const auto written = Storage(state_root.string()).WriteDurable(kLayoutJournal, Bytes(text.begin(), text.end()));
  if (!written.ok() && error) *error = written.message();
  return written.ok();
}

// Whether a game's publication receipt (publication-state.v1, a small JSON file) is committed.
// Read-only, without the publisher lease: the receipt is replaced whole, never edited in place.
bool ReceiptCommitted(const fs::path& path) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) return false;
  std::string text(8192, '\0');
  std::size_t done = 0;
  for (ssize_t count; done < text.size() && (count = ::read(fd, text.data() + done, text.size() - done)) > 0;)
    done += static_cast<std::size_t>(count);
  const bool whole = done < text.size();
  ::close(fd);
  text.resize(done);
  const auto root = whole ? steam::json::Parse(text) : std::nullopt;
  const auto* phase = root && root->is_object() ? root->Find("phase") : nullptr;
  return phase && phase->is_string() && phase->text() == "committed";
}

// The title of a pending launch session (StorageLaunchSessionJournal's PLAYDEN-LAUNCH-V1 file:
// its 4th line is the title ID in hex). Nothing when the file is not one.
std::optional<std::string> PendingLaunchTitle(const Bytes& bytes) {
  const std::string text(bytes.begin(), bytes.end());
  std::vector<std::string> lines;
  for (std::size_t start = 0; start <= text.size() && lines.size() < 4;) {
    const auto end = text.find('\n', start);
    if (end == std::string::npos) break;
    lines.push_back(text.substr(start, end - start));
    start = end + 1;
  }
  if (lines.size() < 4 || lines[0] != "PLAYDEN-LAUNCH-V1" || lines[3].empty() || lines[3].size() % 2 || lines[3].size() > 64)
    return std::nullopt;
  std::string title;
  for (std::size_t i = 0; i < lines[3].size(); i += 2) {
    unsigned value = 0;
    if (std::sscanf(lines[3].c_str() + i, "%2x", &value) != 1 || !std::isxdigit(static_cast<unsigned char>(lines[3][i])) ||
        !std::isxdigit(static_cast<unsigned char>(lines[3][i + 1])))
      return std::nullopt;
    title.push_back(static_cast<char>(value));
  }
  for (const char c : title)
    if (!std::isalnum(static_cast<unsigned char>(c))) return std::nullopt;
  return title;
}

// What would make the games rename strand an operation: a prepare/publication journal
// in the install records, an active settings operation in a game's control folder, or
// a launch session (StorageLaunchSessionJournal's pending file) for a game that may run.
// A pending session is only cleared once Playden sees its game end, which a game closed
// while Playden was not running never shows: its title is asked about (ops.title_running).
std::optional<std::string> PendingJournal(const LayoutPaths& paths, const LayoutOps& ops) {
  Storage storage(paths.state_root.string());
  const auto launch = storage.ReadDurable("launch-pending.v1", 64 * 1024);
  if (launch.ok()) {
    const auto title = PendingLaunchTitle(launch.value());
    const auto running = title && ops.title_running ? ops.title_running(*title) : std::nullopt;
    Log(ops, "pending launch title=" + (title ? *title : std::string("unreadable")) + " running=" +
                 (running ? (*running ? "yes" : "no") : "unknown"));
    if (!running || *running)
      return std::string("the last game you started from Playden may still be running or suspended. Close it, and "
                         "Playden moves them the next time it starts");
  } else if (launch.error() != StorageError::kNotFound) {
    return "the pending launch session can't be read (" + launch.message() + ")";
  }
  RuntimeInstallRepository repository(storage, "installs.json", {});
  const auto loaded = repository.Load();
  if (!loaded.ok()) return "the install records can't be read (" + loaded.message() + ")";
  // Only games inside the folder that moves can be stranded by it: installs on USB and M.2
  // drives keep their paths, so their journals don't hold the move back.
  // Records name console paths; the host folder is checked too.
  const auto moving_host = paths.app_root / "games";
  const auto moving = fs::path("/data") / moving_host.lexically_relative(paths.boundary);
  // The game's folder on the host, when it sits inside the folder that moves.
  const auto inside = [&](const std::string& install_path) -> std::optional<fs::path> {
    for (const auto& root : {moving, moving_host}) {
      const auto relative = fs::path(install_path).lexically_normal().lexically_relative(root);
      if (!relative.empty() && *relative.begin() != "..") return moving_host / relative;
    }
    return std::nullopt;
  };
  for (const auto& record : repository.Snapshot()) {
    const auto folder = inside(record.install_path);
    if (!folder) continue;
    // A record whose folder is gone (the game was installed again elsewhere) can't be stranded.
    if (Kind(*folder) != 1) {
      Log(ops, "install " + record.install_id + " skipped: its folder " + folder->string() + " is missing");
      continue;
    }
    // A game started or suspended from Home still uses its old paths, whatever Playden launched last.
    if (ops.title_running && !record.title_id.empty()) {
      const auto running = ops.title_running(record.title_id);
      if (!running || *running)
        return "the game " + record.title_id + " may still be running or suspended. Close it, and Playden moves "
               "them the next time it starts";
    }
    if (record.journal)
      return "install " + record.install_id + " has a pending " + record.journal->operation + " journal";
    // data_root is /data/playden/games/<id>; map it under this layout's boundary.
    const auto relative = fs::path(record.data_root).lexically_relative("/data");
    if (relative.empty() || *relative.begin() == "..") continue;
    // The last operation stays in the control folder after it commits; only one whose receipt
    // isn't committed is still in flight. (One being captured has a record journal, above.)
    const auto control = paths.boundary / relative / ".playden-control";
    if (Exists(control / "settings-operation.v1") && !ReceiptCommitted(control / "publication-state.v1"))
      return "install " + record.install_id + " has a pending settings publication";
  }
  return std::nullopt;
}

// Each intent is durable before the rename; its completion follows both parent syncs.
// A partial move only retries forward, so services never see an incomplete old user tree.
bool MoveEntries(const LayoutOps& ops, const fs::path& state, LayoutJournal& journal,
                 const fs::path& from, const fs::path& to, bool logs, std::string* reason) {
  const auto complete = [&](LayoutEntry& entry) {
    const auto source = from / entry.from, target = to / entry.to;
    const bool source_exists = Exists(source), target_exists = Exists(target);
    if (target_exists && (!source_exists || logs)) {
      const auto synced = SyncRename(ops, source, target);
      if (synced) { *reason = "rename parents could not be synced: " + Describe(synced); return false; }
    } else if (source_exists && !target_exists && !entry.moved) {
      const auto error = RenameEntry(ops, source, target);
      if (error) { *reason = "rename " + source.string() + " -> " + target.string() + " failed " + Describe(error); return false; }
    } else {
      *reason = "the recorded move of " + source.string() + " is incomplete or conflicts with " + target.string();
      return false;
    }
    entry.moved = true;
    return SaveJournal(state, journal, reason);
  };
  for (auto& entry : journal.entries)
    if (!complete(entry)) return false;
  if (Kind(from) == 0) return true;
  const auto names = Entries(from);
  if (!names) { *reason = from.string() + " can't be listed"; return false; }
  for (const auto& name : *names) {
    const auto slot = logs ? LogTarget(to, name) : Exists(to / name) ? std::nullopt : std::optional(to / name);
    if (!slot) { *reason = (from / name).string() + " and " + (to / name).string() + " both exist"; return false; }
    journal.entries.push_back({name, slot->filename().string(), false});
    if (!SaveJournal(state, journal, reason) || !complete(journal.entries.back())) return false;
  }
  return true;
}

std::string MovedText(const fs::path& to) {
  return "Playden 0.6 moved this folder's contents to " + to.string() + "\n";
}
}  // namespace

std::optional<LayoutJournal> LoadLayoutJournal(const fs::path& state_root, std::string* error) {
  if (error) error->clear();
  const auto read = Storage(state_root.string()).ReadDurable(kLayoutJournal, 64 * 1024);
  if (!read.ok()) {
    if (error && read.error() != StorageError::kNotFound) *error = read.message();
    return std::nullopt;
  }
  const auto root = steam::json::Parse(std::string_view(reinterpret_cast<const char*>(read.value().data()),
                                                        read.value().size()));
  const auto* step = root && root->is_object() ? root->Find("step") : nullptr;
  const auto* from = root && root->is_object() ? root->Find("from") : nullptr;
  const auto* to = root && root->is_object() ? root->Find("to") : nullptr;
  const auto* postponed = root && root->is_object() ? root->Find("postponed") : nullptr;
  if (!step || !step->is_string() || !from || !from->is_string() || !to || !to->is_string()) {
    if (error) *error = "layout journal is malformed";
    return std::nullopt;
  }
  LayoutJournal journal{step->text(), from->text(), to->text(), 0};
  if (postponed && postponed->is_number() && postponed->AsUint64() && *postponed->AsUint64() < 1000000)
    journal.postponed = static_cast<int>(*postponed->AsUint64());
  if (const auto* entries = root->Find("entries")) {
    if (!entries->is_array()) { if (error) *error = "layout entries are malformed"; return std::nullopt; }
    for (const auto& entry : entries->array()) {
      const auto* source = entry.Find("from"), *target = entry.Find("to"), *moved = entry.Find("moved");
      const auto name = [](const steam::json::Value* value) {
        return value && value->is_string() && !value->text().empty() && value->text() != "." && value->text() != ".." &&
               value->text().find('/') == std::string::npos && value->text().find('\0') == std::string::npos;
      };
      if (!name(source) || !name(target) || !moved || !moved->is_bool()) {
        if (error) *error = "layout entry is malformed";
        return std::nullopt;
      }
      journal.entries.push_back({source->text(), target->text(), moved->boolean()});
    }
  }
  if (StepIndex(journal.step) == kNone) { if (error) *error = "unknown layout step"; return std::nullopt; }
  return journal;
}

bool MarkLayoutDone(const fs::path& state_root, std::string* error) {
  std::string load_error;
  auto journal = LoadLayoutJournal(state_root, &load_error);
  if (!journal) {
    if (error) *error = load_error.empty() ? "no layout journal" : load_error;
    return false;
  }
  journal->step = "done";
  journal->postponed = 0;
  return SaveJournal(state_root, *journal, error);
}

LayoutResult MigrateLayout(const LayoutPaths& paths, const LayoutOps& ops) {
  const auto old_user = paths.app_root / "playden-data";
  const auto old_logs = paths.app_root / "logs";
  const auto old_games = paths.app_root / "games";
  LayoutResult result;
  bool uncertain = false;
  const auto finish = [&] {
    result.runtime_ready = result.user_moved && !uncertain;
    result.user_root = result.user_moved ? paths.user_root : old_user;
    result.log_root = result.logs_moved ? paths.log_root : old_logs;
    result.games_root = result.games_moved ? paths.games_root : old_games;
    if (!result.notice.empty()) Log(ops, "notice: " + result.notice);
    return result;
  };
  const auto hold = [&](const std::string& why) {
    Log(ops, why);
    result.notice = "Playden kept some data in its app folder: " + why;
    return finish();
  };

  std::string error;
  if (!PrepareState(paths, &error)) return hold("the runtime state folder is unavailable (" + error + ")");
  auto loaded = LoadLayoutJournal(paths.state_root, &error);
  if (!loaded && !error.empty()) return hold("the move record " + (paths.state_root / kLayoutJournal).string() +
                                             " can't be read (" + error + ")");
  LayoutJournal journal = loaded.value_or(LayoutJournal{});
  // A lost move record (runtime-state deleted or replaced by hand) after a move that plainly
  // happened: the app folder has no user data left and /data/playden/user has it. Written again,
  // or the user step would hold on Playden's own moved data. Data on both sides is still held.
  // It resumes at the logs step (moving what the app folder's logs hold now, as every later start
  // does), so a record lost between the user and logs steps loses no logs. Both folders must list.
  const auto old_entries = Kind(old_user) == 0 ? std::optional<std::vector<std::string>>(std::vector<std::string>{})
                                               : Entries(old_user);
  const auto user_entries = Kind(paths.user_root) == 1 ? Entries(paths.user_root) : std::nullopt;
  if (!loaded && old_entries && old_entries->empty() && user_entries && !user_entries->empty()) {
    LayoutJournal rebuilt{"logs", old_logs.string(), paths.log_root.string(), 0};
    if (!SaveJournal(paths.state_root, rebuilt, &error)) return hold("the move record can't be written (" + error + ")");
    Log(ops, "move record rebuilt step=logs");
    journal = rebuilt;
  }
  // Once the games step is done the games folder exists, shared: a fresh install has
  // none to move. One that came with the moved games keeps its mode.
  const auto ensure_games_root = [&] {
    return EnsureSharedDirectory(ops, paths.games_root, 0777);
  };
  const Step initial = StepIndex(journal.step);
  if (initial >= kGamesMoved) {
    result.user_moved = result.logs_moved = result.games_moved = true;
    if (!ensure_games_root()) { uncertain = true; return hold("the games root could not be created or synced"); }
    return finish();
  }
  // After an interrupted revert both places hold Playden's own files: merge them back.
  const bool reverted = journal.step.rfind("revert-", 0) == 0;
  const auto save = [&](const char* step, const fs::path& from, const fs::path& to, int postponed = 0) {
    LayoutJournal updated{step, from.string(), to.string(), postponed};
    if (!SaveJournal(paths.state_root, updated, &error)) return false;
    journal = updated;
    return true;
  };

  // User data, then logs: the stub folders stay for the pre-grant path checks.
  // A step is recorded before it starts; a target that already had files is refused
  // only before then, since afterwards its files are the move's own.
  struct Tree { Step step; const char* name; fs::path from, to; bool logs; bool* moved; };
  const Tree trees[] = {{kUser, "user", old_user, paths.user_root, false, &result.user_moved},
                        {kLogs, "logs", old_logs, paths.log_root, true, &result.logs_moved}};
  for (const auto& tree : trees) {
    if (initial > tree.step) { *tree.moved = true; continue; }
    if (journal.step != tree.name && !reverted) {
      const int kind = Kind(tree.to);
      if (kind == 2 || (kind == 1 && !EmptyDirectory(tree.to)))
        return hold(tree.to.string() + " already has files; " + tree.from.string() + " was kept. "
                    "Move them aside and restart Playden.");
    }
    if (!EnsureSharedDirectory(ops, tree.to)) return hold(tree.to.string() + " can't be created");
    if (journal.step != tree.name && !save(tree.name, tree.from, tree.to))
      return hold("the move record can't be written (" + error + ")");
    std::string reason;
    if (!MoveEntries(ops, paths.state_root, journal, tree.from, tree.to, tree.logs, &reason)) return hold(reason);
    if (Kind(tree.from) == 1) WriteMarker(ops, tree.from / kMovedMarker, MovedText(tree.to));
    Log(ops, "moved " + tree.from.string() + " to " + tree.to.string());
    *tree.moved = true;
  }

  if (journal.step != "games" && !save("games", old_games, paths.games_root))
    return hold("the move record can't be written (" + error + ")");
  if (ops.defer_games) return finish();
  const auto moved_games = [&] {
    if (!ensure_games_root() || !save("games-moved", old_games, paths.games_root)) {
      uncertain = true;
      return false;
    }
    result.games_moved = true;
    return true;
  };
  const int old_kind = Kind(old_games);
  if (old_kind == 0) {
    if (const auto error = SyncRename(ops, old_games, paths.games_root)) {
      uncertain = true;
      return hold("the games rename parents could not be synced: " + Describe(error));
    }
    // Nothing to move, or an interrupted start already renamed it.
    if (!moved_games()) return hold("the games move could not be made durable: " + error);
    return finish();
  }
  const auto postpone = [&](const std::string& why, bool tell_now) {
    const int count = journal.postponed + 1;
    if (!save("games", old_games, paths.games_root, count)) Log(ops, "the move record can't be written (" + error + ")");
    Log(ops, "games move postponed (" + std::to_string(count) + "): " + why);
    if (tell_now || count >= kLayoutPostponeNoticeAfter)
      result.notice = "Playden couldn't move your games to " + paths.games_root.string() + " yet: " + why;
    return finish();
  };
  if (old_kind != 1) {
    Log(ops, "games: " + old_games.string() + " is not a folder; nothing was moved");
    result.notice = old_games.string() + " is not a folder, so Playden left it in place.";
    return finish();
  }
  const int new_kind = Kind(paths.games_root);
  if (new_kind == 2 || (new_kind == 1 && !EmptyDirectory(paths.games_root))) {
    Log(ops, "games: " + paths.games_root.string() + " already has files; " + old_games.string() + " was kept");
    result.notice = paths.games_root.string() + " already has files, so Playden kept your games in " +
                    old_games.string() + ". Move them aside and restart Playden.";
    return finish();
  }
  if (const auto pending = PendingJournal(paths, ops)) return postpone(*pending, false);
  std::error_code rename_error;
  const bool ran = AsRoot(ops, [&] {
    rename_error = Rename(ops, old_games, paths.games_root);
    return !rename_error;
  });
  if (rename_error || !ran) {
    if (rename_error.value() == EXDEV)
      return postpone(paths.games_root.string() + " is on another drive; nothing was copied", true);
    return postpone("rename failed " + (rename_error ? Describe(rename_error) : std::string("(root unavailable)")),
                    true);
  }
  if (const auto error = SyncRename(ops, old_games, paths.games_root)) {
    uncertain = true;
    return postpone("the games rename parents could not be synced: " + Describe(error), true);
  }
  Log(ops, "moved " + old_games.string() + " to " + paths.games_root.string());
  if (!moved_games()) return hold("the games move could not be made durable: " + error);
  WriteMarker(ops, paths.app_root / kGamesMarker, MovedText(paths.games_root));
  return finish();
}

RevertResult RevertLayout(const LayoutPaths& paths, const LayoutOps& ops) {
  RevertResult result;
  std::string error;
  if (Kind(paths.state_root) == 0) {
    result.complete = true;  // never moved
    return result;
  }
  auto journal = LoadLayoutJournal(paths.state_root, &error);
  if (!journal) {
    if (!error.empty()) result.error = "the move record can't be read: " + error;
    else result.complete = true;  // never moved
    return result;
  }
  const auto save = [&](const char* step) {
    LayoutJournal updated = *journal;
    updated.step = step;
    if (!SaveJournal(paths.state_root, updated, &error)) {
      result.error = "the move record can't be written: " + error;
      return false;
    }
    return true;
  };
  const auto old_games = paths.app_root / "games";
  if (!save("revert-games")) return result;
  std::string problems;
  if (Kind(paths.games_root) == 1) {
    const int old_kind = Kind(old_games);
    if (old_kind == 0 || (old_kind == 1 && EmptyDirectory(old_games))) {
      std::error_code rename_error;
      const bool ran = AsRoot(ops, [&] {
        rename_error = Rename(ops, paths.games_root, old_games);
        return !rename_error;
      });
      if (!rename_error && ran) rename_error = SyncRename(ops, paths.games_root, old_games);
      if (rename_error || !ran) problems += "games rename failed " + Describe(rename_error) + "; ";
      else Log(ops, "moved " + paths.games_root.string() + " back to " + old_games.string());
    } else {
      problems += old_games.string() + " already has files; ";
    }
  }
  if (!problems.empty()) { result.error = problems; return result; }
  if (const auto error = SyncRename(ops, paths.games_root, old_games)) { result.error = Describe(error); return result; }
  std::remove((paths.app_root / kGamesMarker).c_str());

  struct Tree { const char* step; fs::path from, to; bool logs; };
  const Tree trees[] = {{"revert-logs", paths.log_root, paths.app_root / "logs", true},
                        {"revert-user", paths.user_root, paths.app_root / "playden-data", false}};
  for (const auto& tree : trees) {
    if (!save(tree.step)) return result;
    std::remove((tree.to / kMovedMarker).c_str());
    if (Kind(tree.from) != 1) continue;
    if (!EnsureSharedDirectory(ops, tree.to)) { result.error = tree.to.string() + " can't be created or synced"; return result; }
    const auto names = Entries(tree.from);
    if (!names) { result.error = tree.from.string() + " can't be listed"; return result; }
    for (const auto& name : *names) {
      const auto target =
          tree.logs ? LogTarget(tree.to, name) : Exists(tree.to / name) ? std::nullopt : std::optional(tree.to / name);
      if (!target) {
        problems += (tree.to / name).string() + " already exists; ";
        continue;
      }
      const auto rename_error = RenameEntry(ops, tree.from / name, *target);
      if (rename_error) problems += "rename " + (tree.from / name).string() + " failed " + Describe(rename_error) + "; ";
    }
    if (!problems.empty()) { result.error = problems; return result; }
    // A previous attempt may have renamed the last entry but failed its parent sync.
    if (!SyncDirectory(ops, tree.from) || !SyncDirectory(ops, tree.to)) {
      result.error = "the rollback folders could not be synced";
      return result;
    }
    Log(ops, "moved " + tree.from.string() + " back to " + tree.to.string());
  }
  if (!problems.empty()) {
    result.error = problems;
    Log(ops, "revert incomplete: " + problems);
    return result;
  }
  const auto removed = Storage(paths.state_root.string()).RemoveDurable(kLayoutJournal);
  if (!removed.ok()) {
    result.error = "the move record can't be removed: " + removed.message();
    return result;
  }
  result.complete = true;
  return result;
}

}  // namespace playden::runtime
