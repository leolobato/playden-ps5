// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// The console can mount USB drives the other way round, or on another port, so /mnt/usb<N> names a place and not a
// drive. These say when two paths are one folder on whichever mount its drive is on.
#include <string>
#include <string_view>

namespace playden::platform {

inline constexpr int kUsbMounts = 8;  // /mnt/usb0 to /mnt/usb7

// An install volume ID of a USB mount: "usb0" to "usb7".
inline bool IsUsbVolumeId(std::string_view id) {
  return id.size() == 4 && id.substr(0, 3) == "usb" && id[3] >= '0' && id[3] < '0' + kUsbMounts;
}

// path with its USB mount's number left out (/mnt/usb1/playden/games/730 is /mnt/usb/playden/games/730), or as it
// is when it isn't on one. Two paths with the same answer are one folder.
inline std::string WithoutUsbMountNumber(const std::string& path) {
  constexpr std::string_view kMount = "/mnt/usb";
  if (path.size() <= kMount.size() || path.compare(0, kMount.size(), kMount) != 0 || path[kMount.size()] < '0' ||
      path[kMount.size()] >= '0' + kUsbMounts || (path.size() > kMount.size() + 1 && path[kMount.size() + 1] != '/'))
    return path;
  return std::string(kMount) + path.substr(kMount.size() + 1);
}

// The same folder on USB mount `number`; path has to be on a USB mount.
inline std::string OnUsbMount(const std::string& path, int number) {
  constexpr std::string_view kMount = "/mnt/usb";
  return std::string(kMount) + std::to_string(number) + WithoutUsbMountNumber(path).substr(kMount.size());
}

// A path in the form a title searches the USB mounts for: /mnt/usb/<folder>, which is never a mount itself.
inline bool IsUsbSearchPath(const std::string& path) { return path.rfind("/mnt/usb/", 0) == 0; }

// The USB mount path is on (/mnt/usb1), or nothing when it isn't on one.
inline std::string UsbMountOf(const std::string& path) {
  return WithoutUsbMountNumber(path) == path ? std::string() : path.substr(0, std::string_view("/mnt/usb0").size());
}

// Where a published path really is for a game at install_path: its mount in place of the search form.
inline std::string OnGameMount(const std::string& published, const std::string& install_path) {
  const auto mount = UsbMountOf(install_path);
  if (!IsUsbSearchPath(published) || mount.empty()) return published;
  return mount + published.substr(std::string_view("/mnt/usb").size());
}

}  // namespace playden::platform
