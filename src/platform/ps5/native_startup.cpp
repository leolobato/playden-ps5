#include "platform/ps5/native_startup.h"
#include "platform/ps5/native_access_posix.h"
#include "platform/ps5/title_activity.h"
#include "platform/ps5/title_allow_list.h"
#include "runtime/kit_trust.h"
#include "runtime/layout_migration.h"
#include "runtime/self_heal.h"
#include "runtime/writable_folders.h"
#include <EGL/egl.h>
#include "steam/backend/native_network.h"
#include "app/diagnostic_log.h"

#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <vector>

extern "C" int sceUserServiceInitialize(const void*);
#if PLAYDEN_NATIVE_ACCESS_SOURCE
extern "C" {
#include "pw_lapy_elevation.h"
}
#endif

namespace playden::ps5 {
namespace {
struct StartupContext {
  std::string title_id;
  NativeAccessPosixBackend* filesystem = nullptr;
  std::string source_identity;
  std::string grant_diagnostic;
};

int InitializeUser(void* data) {
  auto& context = *static_cast<StartupContext*>(data);
  const auto ops = context.filesystem->Ops();
  std::string identity;
  const std::string stable = "/data/homebrew/" + context.title_id;
  const std::string logs = ops.read_source_identity(ops.context, stable.c_str(), &identity) &&
      !context.source_identity.empty() && identity == context.source_identity ?
      stable + "/logs" : "/app0/logs";
  app::AppendLogLine(logs, "native-startup.log", "user service initialization starting");
  const auto result = static_cast<std::uint32_t>(sceUserServiceInitialize(nullptr));
  app::AppendLogLine(logs, "native-startup.log", "user service result=" + std::to_string(result));
  if (result != 0 && result != 0x80960003U) return static_cast<int>(result);
  return 0;
}

int PrepareGraphics(void* data) {
  (void)data;
  app::AppendLogLine("/app0/logs", "native-startup.log", "graphics entry point probe starting");
  // The linked EGL implementation returns its static display from this call.
  // eglInitialize/Display::open creates graphics threads; defer that until main
  // after the helper has verified and cloned a single-threaded title credential.
  const int result = eglGetDisplay(EGL_DEFAULT_DISPLAY) != EGL_NO_DISPLAY ? 0 : EIO;
  app::AppendLogLine("/app0/logs", "native-startup.log", "graphics entry point result=" + std::to_string(result));
  return result;
}

void ReportSessionAccess(const std::string& state_root, const std::string& log_root, const char* stage) {
  const std::string file = state_root + "/session.v1";
  struct stat metadata{};
  const int status = stat(file.c_str(), &metadata);
  const int status_error = status ? errno : 0;
  const int descriptor = open(file.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
  const int open_error = descriptor < 0 ? errno : 0;
  if (descriptor >= 0) close(descriptor);
  app::AppendLogLine(log_root, "native-startup.log",
      std::string("session access stage=") + stage + " file=" + file +
      " uid=" + std::to_string(getuid()) + " euid=" + std::to_string(geteuid()) +
      " gid=" + std::to_string(getgid()) + " egid=" + std::to_string(getegid()) +
      " stat_error=" + std::to_string(status_error) +
      " file_uid=" + std::to_string(metadata.st_uid) +
      " file_gid=" + std::to_string(metadata.st_gid) +
      " mode=" + std::to_string(metadata.st_mode & 0777) +
      " open_error=" + std::to_string(open_error));
}

// After the grant, before the layout move has run this start: the session file
// is at /data/playden/user once an earlier start moved it, else in the app folder.
// The line stays in the app's logs: writing into /data/playden/logs before the
// move would make that folder look taken.
void ReportGrantedSessionAccess(const std::string& title_id, const char* stage) {
  const std::string stable = "/data/homebrew/" + title_id;
  struct stat metadata{};
  const bool moved = stat("/data/playden/user/session.v1", &metadata) == 0;
  ReportSessionAccess(moved ? "/data/playden/user" : stable + "/playden-data", stable + "/logs", stage);
}

int RequestData(void* data) {
#if PLAYDEN_NATIVE_ACCESS_SOURCE
  auto& context = *static_cast<StartupContext*>(data);
  app::AppendLogLine("/app0/logs", "native-startup.log", "data grant starting");
  // Each failure names its stage: the startup screen tells the player which step to fix.
  if (!steam::internal::InitializeNativeNetworkForBootstrap()) {
    context.grant_diagnostic = "helper stage=network status=unavailable error=" + std::to_string(ENETDOWN);
    return ENETDOWN;
  }
  const auto ops = context.filesystem->Ops();
  std::string helper;
  for (const auto& root : {std::string("/app0"), "/data/homebrew/" + context.title_id}) {
    std::string identity;
    if (!context.source_identity.empty() &&
        ops.read_source_identity(ops.context, root.c_str(), &identity) &&
        identity == context.source_identity &&
        ops.path_kind(ops.context, (root + "/lapy.elf").c_str()) == NativePathKind::kFile) {
      helper = root + "/lapy.elf";
      break;
    }
  }
  if (helper.empty()) {
    context.grant_diagnostic = "helper stage=find-helper status=unavailable error=" + std::to_string(ENOENT);
    return ENOENT;
  }
  const uid_t original_effective_uid = geteuid();
  ReportSessionAccess("/app0/playden-data", "/app0/logs", "before-grant");
  struct pw_lapy_elevation_diagnostics diagnostic{};
  const int grant = pw_lapy_elevation_request_at_detailed(
      static_cast<std::int32_t>(getpid()), helper.c_str(), &diagnostic);
  const int grant_error = diagnostic.error ? diagnostic.error : errno;
  context.grant_diagnostic = "helper stage=" + std::string(diagnostic.stage ? diagnostic.stage : "unknown") +
      " status=" + (diagnostic.helper_status_valid ? std::to_string(diagnostic.helper_status) : "unavailable") +
      " error=" + std::to_string(diagnostic.error);
  if (grant != 0) return grant_error ? grant_error : EIO;
  ReportGrantedSessionAccess(context.title_id, "after-grant");
  // The filesystem grant must not change ownership of the launcher's private
  // session/settings files. Let the kernel clone/update the credential normally;
  // keep the granted root view and verify effective UID before services start.
  if (geteuid() != original_effective_uid && seteuid(original_effective_uid) != 0) {
    const int restore_error = errno ? errno : EIO;
    context.grant_diagnostic += "; effective uid restoration failed error=" + std::to_string(restore_error);
    return restore_error;
  }
  if (geteuid() != original_effective_uid) return EACCES;
  ReportGrantedSessionAccess(context.title_id, "after-uid-restore");
  // The helper may still be settling the process after its cooperative reply.
  struct timespec delay{1, 0};
  while (nanosleep(&delay, &delay) != 0)
    if (errno != EINTR) return errno ? errno : EIO;
  return 0;
#else
  (void)data;
  return ENOSYS;
#endif
}
}

NativeAccessResult PrepareNativeStartup(const std::string& title_id) {
  StartupContext context{title_id, nullptr, {}, {}};
  NativeAccessPosixBackend filesystem(title_id,
      {&context, InitializeUser, PrepareGraphics, RequestData});
  context.filesystem = &filesystem;
  NativeAccessOptions options;
  options.title_id = title_id;
  options.user_service_after_grant = true;
  options.require_grant = true;
  const auto ops = filesystem.Ops();
  ops.read_source_identity(ops.context, "/app0", &context.source_identity);
  auto startup_ops = filesystem.Ops();
  startup_ops.prepare_runtime_root = [](void*, const char* root, std::string* error) {
    *error = runtime::PrepareRuntimeRoot(root, WithRoot, [](const std::string& line) {
      app::AppendLogLine("/app0/logs", "native-startup.log", line);
    });
    return error->empty();
  };
  NativeAccessBootstrap bootstrap(startup_ops, options);
  auto result = bootstrap.Prepare();
  if (!result.runtime_ready && !context.grant_diagnostic.empty())
    result.error += "; " + context.grant_diagnostic;
  // The 0.6 layout: user data, logs and games leave the app folder once. Still
  // single-threaded here; only user data and logs move. The runtime worker moves
  // games under publication leases before download services receive their volume paths.
  std::vector<std::string> layout_lines;
  if (result.runtime_ready && !result.paths.runtime_data_root.empty()) {
    namespace fs = std::filesystem;
    const fs::path data_root = result.paths.runtime_data_root;
    const auto layout_log = [&layout_lines](const std::string& line) { layout_lines.push_back(line); };
    // A recursive chmod over FTP leaves the private state and the kit too open to be accepted:
    // given back their modes before anything reads them.
    runtime::TightenPrivateModes(data_root, layout_log);
    runtime::TightenKitModes(runtime::kDevelopmentKitRoot, WithRoot, layout_log);
    // The app folder's empty stubs, before the move writes MOVED.txt into them. A failure is only
    // logged: the move takes a missing one.
    runtime::PrepareAppFolderStubs(result.paths.title_root, WithRoot,
                                   [&layout_lines](const std::string& line) { layout_lines.push_back(line); });
    runtime::LayoutPaths layout;
    layout.app_root = result.paths.title_root;
    layout.user_root = data_root / "user";
    layout.log_root = data_root / "logs";
    layout.games_root = data_root.parent_path() / "playden-games";
    layout.state_root = data_root / "runtime-state";
    layout.boundary = data_root.parent_path();
    runtime::LayoutOps layout_ops;
    layout_ops.defer_games = true;
    layout_ops.as_root = [](const std::function<bool()>& work) { return WithRoot(work); };
    layout_ops.log = [&layout_lines](const std::string& line) { layout_lines.push_back(line); };
    const auto moved = runtime::MigrateLayout(layout, layout_ops);
    result.paths.user_root = moved.user_root.string();
    result.paths.user_log_root = moved.log_root.string();
    result.paths.games_root = moved.games_root.string();
    result.notice = moved.notice;
    if (!moved.runtime_ready) {
      result.runtime_ready = false;
      result.error = moved.notice;
    }
    // The drop folder for games copied over FTP (/data/playden-import, 0777). Still single-threaded;
    // a failure is only logged: games can still be imported from a drive.
    if (result.runtime_ready)
      runtime::PrepareDropFolder(data_root.parent_path() / "playden-import", WithRoot,
                                 [&layout_lines](const std::string& line) { layout_lines.push_back(line); });
  }
  const std::string logs = result.launcher_ready ? EffectiveLogRoot(result.paths) : "/app0/logs";
  for (const auto& line : layout_lines) app::AppendLogLine(logs, "native-startup.log", line);
  if (!result.paths.user_root.empty())
    ReportSessionAccess(result.paths.user_root, logs, "after-layout");
  if (!context.grant_diagnostic.empty())
    app::AppendLogLine(logs, "native-startup.log", context.grant_diagnostic);
  app::AppendLogLine(logs, "native-startup.log",
      "bootstrap launcher_ready=" + std::to_string(result.launcher_ready) +
      " runtime_ready=" + std::to_string(result.runtime_ready) + " error=" + result.error);
  return result;
}
}
