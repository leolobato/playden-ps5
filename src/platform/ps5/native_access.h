#pragma once

#include <string>
#include <vector>

namespace playden::ps5 {

enum class NativePathKind {
  kMissing,
  kFile,
  kDirectory,
  kOther,
};

enum class NativePathAccess {
  kRead,
  kReadWrite,
};

// Native calls stay outside this portable bootstrap. The production adapter may
// use package metadata or a durable marker for source_identity; callers must not
// assume that two mount views expose matching device/inode values.
struct NativeAccessOps {
  void* context = nullptr;
  int (*initialize_user_service)(void*) = nullptr;
  int (*prepare_graphics_access)(void*) = nullptr;
  int (*request_data_access)(void*) = nullptr;
  NativePathKind (*path_kind)(void*, const char*) = nullptr;
  bool (*path_accessible)(void*, const char*, NativePathAccess) = nullptr;
  bool (*read_source_identity)(void*, const char*, std::string*) = nullptr;
  bool (*prepare_runtime_root)(void*, const char*, std::string*) = nullptr;
};

struct NativeAccessOptions {
  std::string title_id;
  std::string sandbox_root = "/app0";
  std::string stable_homebrew_root = "/data/homebrew";
  std::string managed_runtime_root = "/data/playden";
  std::string managed_runtime_parent = "/data";
  // Set only when durable install metadata proves this launcher has no prior
  // account/log state. Directory creation belongs to the owning services.
  bool known_fresh_state = false;
  // Optional durable package identity. When empty, it is captured from the
  // sandbox root before the grant can invalidate that mount view.
  std::string source_identity;
  // Helpers requiring a single-threaded target must run before UserService,
  // which may start background threads. Initialize it before returning to UI.
  bool user_service_after_grant = false;
  // Visible paths do not prove the grant ran: ShadowMountPlus 1.7beta4+
  // mounts /data into the sandbox, where the private trees stay closed.
  bool require_grant = false;
};

// Package paths: the title, its assets and trust store, and the stub
// playden-data/logs folders. Granted starts need the stubs only as readable
// folders, and take a missing one (startup makes it after the grant); the
// fallback without the grant still checks them as writable (its live launcher
// only shows "Root access is missing"). After the grant, the
// 0.6 layout move fills user_root, user_log_root and games_root (each at the
// new place once its step has moved, else the old in-app one). Not granted:
// they stay empty and the package paths are used.
struct NativePaths {
  std::string title_root;
  std::string assets_root;
  std::string trust_store;
  std::string steam_state_root;
  std::string log_root;
  std::string runtime_data_root;
  std::string user_root;
  std::string user_log_root;
  std::string games_root;
};

// Where services keep user data, logs and console-storage games this start.
inline const std::string& EffectiveUserRoot(const NativePaths& paths) {
  return paths.user_root.empty() ? paths.steam_state_root : paths.user_root;
}
inline const std::string& EffectiveLogRoot(const NativePaths& paths) {
  return paths.user_log_root.empty() ? paths.log_root : paths.user_log_root;
}
inline std::string EffectiveGamesRoot(const NativePaths& paths) {
  return paths.games_root.empty() ? paths.title_root + "/games" : paths.games_root;
}

struct NativeAccessResult {
  bool launcher_ready = false;
  bool runtime_ready = false;
  // The data grant succeeded (or wasn't needed); without runtime_ready the data, not the grant, failed.
  bool granted = false;
  bool already_accessible = false;
  NativePaths paths;
  std::string error;
  std::string notice;  // one line for the user about the data layout, when set
  // Uninstall Playden (#868) had removed the PS5 titles and was removing files when Playden stopped: startup
  // finished it instead of making Playden's folders again. Nothing else starts; the "removed" screen shows.
  bool playden_removed = false;
  bool playden_removal_complete = false;
  std::vector<std::string> playden_removal_notes;
};

// Orders the portable parts of startup around the root-view change. Successful
// preparation is cached. A failed grant can be retried without repeating
// completed UserService/graphics preparation or mutating persistent storage.
class NativeAccessBootstrap {
 public:
  NativeAccessBootstrap(NativeAccessOps ops, NativeAccessOptions options);
  NativeAccessResult Prepare();

 private:
  NativeAccessResult PrepareAccess();
  NativeAccessResult LauncherFallback(const std::string& error,
                                      const NativePaths* stable = nullptr);

  NativeAccessOps ops_;
  NativeAccessOptions options_;
  bool user_initialized_ = false;
  bool graphics_prepared_ = false;
  bool source_identity_captured_ = false;
  std::string source_identity_;
  bool complete_ = false;
  NativeAccessResult complete_result_;
};

}  // namespace playden::ps5
