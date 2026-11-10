// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace playden::runtime {

// "pe32" or "pe64" from a Windows executable's PE header; empty when it is neither, or when the path is not a
// regular file (a symbolic link, a folder, a FIFO) or can't be read. Reads only the DOS and PE headers.
inline std::string ExecutableArchitecture(const std::string& path) {
  // O_NONBLOCK: opening a FIFO must not wait for a writer; it is then refused as not a regular file.
  const int file = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
  if (file < 0) return {};
  struct stat info {};
  if (::fstat(file, &info) != 0 || !S_ISREG(info.st_mode)) {
    ::close(file);
    return {};
  }
  const auto read_at = [file](off_t offset, unsigned char* buffer, std::size_t size) {
    std::size_t done = 0;
    while (done < size) {
      const auto count = ::pread(file, buffer + done, size - done, offset + static_cast<off_t>(done));
      if (count < 0 && errno == EINTR) continue;
      if (count <= 0) return false;
      done += static_cast<std::size_t>(count);
    }
    return true;
  };
  unsigned char dos[64];
  unsigned char header[26];
  std::string architecture;
  if (read_at(0, dos, sizeof(dos)) && dos[0] == 'M' && dos[1] == 'Z') {
    const std::uint32_t pe = dos[0x3c] | (dos[0x3d] << 8) | (dos[0x3e] << 16) |
                             (static_cast<std::uint32_t>(dos[0x3f]) << 24);
    if (pe < 64u * 1024u * 1024u && read_at(static_cast<off_t>(pe), header, sizeof(header)) &&
        header[0] == 'P' && header[1] == 'E' && header[2] == 0 && header[3] == 0) {
      const unsigned machine = header[4] | (header[5] << 8);
      const unsigned magic = header[24] | (header[25] << 8);
      if (machine == 0x014c && magic == 0x010b) architecture = "pe32";
      if (machine == 0x8664 && magic == 0x020b) architecture = "pe64";
    }
  }
  ::close(file);
  return architecture;
}

}  // namespace playden::runtime
