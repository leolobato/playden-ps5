#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace playden::runtime {

// A zstd-compressed tar archive (.tzst), read as a stream: the publisher pack's archives are
// tens of megabytes and the console's worker stacks and memory are small.
struct TarEntry {
  std::string path;  // as stored (ustar prefix, pax path or GNU long name applied), without a trailing '/'
  char type = '0';   // '0' a regular file, '5' a directory
  std::uint64_t size = 0;
};

// Calls on_entry for each file or directory; for a file, on_data then receives its content in
// chunks. Links, devices and FIFOs are refused, as are damaged headers, a stream that ends
// early and a callback that returns false. Returns false with a reason (for logs).
bool ReadTzst(const std::string& path, const std::function<bool(const TarEntry&)>& on_entry,
              const std::function<bool(const char*, std::size_t)>& on_data, std::string* reason);

}  // namespace playden::runtime
