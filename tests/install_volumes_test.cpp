#include "platform/usb_mount.h"
#include "steam/install_volumes.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <unistd.h>

using namespace playden::steam;

#define CHECK(condition)                                                                  \
  do {                                                                                    \
    if (!(condition)) {                                                                   \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
      std::exit(1);                                                                       \
    }                                                                                     \
  } while (false)

int main() {
  const auto base = std::filesystem::temp_directory_path() / ("playden-volumes-" + std::to_string(::getpid()));
  std::filesystem::remove_all(base);
  std::filesystem::create_directories(base / "app0");
  std::filesystem::create_directories(base / "ext1");
  std::filesystem::create_directories(base / "locked");

  const std::vector<VolumeCandidate> candidates = {
      {"internal", "Console storage", (base / "app0").string(), (base / "app0" / "games").string()},
      {"m2", "M.2 SSD", (base / "ext1").string(), (base / "ext1" / "playden" / "games").string()},
      {"usb0", "USB drive", (base / "absent").string(), (base / "absent" / "games").string()},
      {"locked", "Read-only drive", (base / "locked").string(), (base / "locked" / "playden" / "games").string()},
  };
  std::filesystem::permissions(base / "locked", std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec);

  const auto volumes = DiscoverInstallVolumes(candidates);
  // The primary is always offered; a missing mount is skipped; an unwritable one is offered unavailable.
  CHECK(volumes.size() == 3);
  CHECK(volumes[0].id == "internal" && volumes[0].available);
  CHECK(volumes[1].id == "m2" && volumes[1].available);
  CHECK(std::filesystem::is_directory(base / "ext1" / "playden" / "games"));
  CHECK(!std::filesystem::exists(base / "ext1" / "playden" / "games" / ".playden-probe"));
  CHECK(volumes[2].id == "locked");
  if (::geteuid() != 0) CHECK(!volumes[2].available);  // root ignores the permission bits

  // A drive whose root only root may write (a console-formatted M.2, issue #57): the folders are
  // made with root rights and left open to the app user, which then writes there itself.
  int root_calls = 0;
  const auto locked_mode = std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec;
  const AsRoot as_root = [&](const std::function<bool()>& work) {
    ++root_calls;
    std::filesystem::permissions(base / "locked", std::filesystem::perms::owner_all);
    const bool ok = work();
    std::filesystem::permissions(base / "locked", locked_mode);
    return ok;
  };
  const auto rooted = DiscoverInstallVolumes(candidates, as_root);
  CHECK(rooted.size() == 3 && rooted[1].available && rooted[2].id == "locked" && rooted[2].available);
  CHECK(root_calls == (::geteuid() != 0 ? 1 : 0));  // only the drive the app user could not write
  if (::geteuid() != 0) {
    for (const auto& folder : {base / "locked" / "playden", base / "locked" / "playden" / "games"})
      CHECK((std::filesystem::status(folder).permissions() & std::filesystem::perms::all) == std::filesystem::perms::all);
    CHECK(!std::filesystem::exists(base / "locked" / "playden" / "games" / ".playden-probe"));
  }
  // Without root rights the drive stays offered but unavailable.
  std::filesystem::create_directories(base / "locked2");
  std::filesystem::permissions(base / "locked2", locked_mode);
  const auto refused = DiscoverInstallVolumes(
      {candidates[0], {"locked2", "Locked", (base / "locked2").string(), (base / "locked2" / "playden" / "games").string()}},
      [](const std::function<bool()>&) { return false; });
  CHECK(refused.size() == 2);
  if (::geteuid() != 0) CHECK(!refused[1].available && !std::filesystem::exists(base / "locked2" / "playden"));
  std::filesystem::permissions(base / "locked2", std::filesystem::perms::owner_all);

  // A folder on the same filesystem as its parent is an empty mount point, not a drive:
  // it is skipped and nothing is created inside it.
  std::filesystem::create_directories(base / "unmounted");
  const auto unmounted = DiscoverInstallVolumes({
      candidates[0],
      {"extended", "Extended storage", (base / "unmounted").string(), (base / "unmounted" / "playden" / "games").string(), true},
  });
  CHECK(unmounted.size() == 1 && unmounted[0].id == "internal");
  CHECK(!std::filesystem::exists(base / "unmounted" / "playden"));
  // The filesystem root is a mount point on any host, so a required mount point there is offered.
  const auto mounted = DiscoverInstallVolumes({candidates[0], {"root", "Root", "/", (base / "root-games").string(), true}});
  CHECK(mounted.size() == 2 && mounted[1].id == "root");

  // The console candidates start with console storage under /app0; drives must be mounted.
  const auto console = ConsoleVolumeCandidates();
  CHECK(!console.empty() && console[0].id == InstallService::kPrimaryVolumeId && console[0].games_root == "/app0/games");
  CHECK(!console[0].require_mount_point);
  for (std::size_t index = 1; index < console.size(); ++index) CHECK(console[index].require_mount_point);
  // After the grant, console storage is the games root outside the app folder.
  const auto granted = ConsoleVolumeCandidates("/data/homebrew/FAKE77031", "/data/playden-games");
  CHECK(granted.size() == console.size() && granted[0].id == InstallService::kPrimaryVolumeId);
  CHECK(granted[0].games_root == "/data/playden-games" && granted[0].mount == "/data" && !granted[0].require_mount_point);
  CHECK(ConsoleVolumeCandidates("/data/homebrew/FAKE77031")[0].games_root == "/data/homebrew/FAKE77031/games");
  for (std::size_t index = 1; index < granted.size(); ++index)
    CHECK(granted[index].games_root == console[index].games_root);

  const auto space = QueryVolumeSpace(base.string());
  CHECK(space && space->total_bytes > 0 && space->free_bytes <= space->total_bytes);
  // A new game's directories may not exist yet; query the existing volume ancestor.
  const auto missing = QueryVolumeSpace((base / "absent" / "new-game").string());
  CHECK(missing && missing->total_bytes == space->total_bytes);
  CHECK(!std::filesystem::exists(base / "absent"));
  CHECK(!QueryVolumeSpace(""));
  // Console storage leaves out what the PS5 keeps for itself; never below zero.
  CHECK(detail::WithoutConsoleReserve({100ull << 30, 20ull << 30}, kConsoleStorageReserve).free_bytes == 12ull << 30);
  CHECK(detail::WithoutConsoleReserve({100ull << 30, 5ull << 30}, kConsoleStorageReserve).free_bytes == 0 &&
        detail::WithoutConsoleReserve({100ull << 30, 5ull << 30}, kConsoleStorageReserve).total_bytes == 100ull << 30);
  CHECK(QueryVolumeSpace(base.string() + "//./"));
  CHECK(!QueryVolumeSpace(base.string() + "/../games"));
  CHECK(QueryVolumeSpace("."));
  CHECK(!QueryVolumeSpace(std::string("/tmp\0ignored", 12)));
  CHECK(!QueryVolumeSpace("/playden-unknown-volume-" + std::to_string(::getpid()) + "/games"));
  std::FILE* regular = std::fopen((base / "file").c_str(), "w");
  CHECK(regular && std::fclose(regular) == 0);
  CHECK(!QueryVolumeSpace((base / "file" / "games").string()));

  const auto converted = detail::VolumeSpaceFromBlocks(65536, 15261605, 3799429);
  CHECK(converted && converted->total_bytes == 1000184545280ull && converted->free_bytes == 248999378944ull);
  const auto reserved = detail::VolumeSpaceFromBlocks(4096, 100, -1);
  CHECK(reserved && reserved->total_bytes == 409600 && reserved->free_bytes == 0);
  CHECK(!detail::VolumeSpaceFromBlocks(0, 100, 10));
  CHECK(!detail::VolumeSpaceFromBlocks(-1, 100, 10));
  CHECK(!detail::VolumeSpaceFromBlocks(4096, 0, 0));
  CHECK(!detail::VolumeSpaceFromBlocks(4096, 10, 11));
  CHECK(!detail::VolumeSpaceFromBlocks(4096, std::numeric_limits<std::uint64_t>::max(), 1));
  for (const auto* root : {"/mnt/ext0", "/mnt/ext1", "/mnt/usb0", "/mnt/usb1"}) {
    CHECK(detail::VolumeMountMatches(root, root));
    CHECK(detail::VolumeMountMatches(std::string(root) + "/playden/games/new-game", root));
    CHECK(!detail::VolumeMountMatches(root, "/mnt"));
    CHECK(!detail::VolumeMountMatches(std::string(root) + "/playden", "/mnt"));
    CHECK(!detail::VolumeMountMatches(root, "/mnt/ext2"));
  }
  CHECK(detail::VolumeMountMatches("/data/playden/games", "/data"));
  CHECK(detail::VolumeMountMatches("/mnt/ext10", "/mnt"));
  using playden::platform::IsUsbVolumeId;
  using playden::platform::OnUsbMount;
  using playden::platform::WithoutUsbMountNumber;
  CHECK(IsUsbVolumeId("usb0") && IsUsbVolumeId("usb7") && !IsUsbVolumeId("usb8") && !IsUsbVolumeId("usb") &&
        !IsUsbVolumeId("usb10") && !IsUsbVolumeId("m2"));
  CHECK(WithoutUsbMountNumber("/mnt/usb1/playden/games/730") == "/mnt/usb/playden/games/730");
  CHECK(WithoutUsbMountNumber("/mnt/usb1") == "/mnt/usb");
  for (const char* other : {"/mnt/usb/x", "/mnt/usb10/x", "/mnt/usb8/x", "/mnt/usbx/x", "/mnt/ext1/x", "/data/mnt/usb1/x", "/mnt/usb1x"})
    CHECK(WithoutUsbMountNumber(other) == other);
  CHECK(OnUsbMount("/mnt/usb1/playden/games/730", 3) == "/mnt/usb3/playden/games/730");

  std::filesystem::permissions(base / "locked", std::filesystem::perms::owner_all);
  std::filesystem::remove_all(base);
  std::puts("install volumes tests passed");
}
