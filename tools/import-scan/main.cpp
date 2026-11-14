// import-scan: what Playden's import scanner sees in a Windows game folder, on the host.
//
//   import-scan <game folder>      the folder's title, its programs ranked (helpers flagged), its
//                                  entries listed, how long the scan took, and whether it could play in
//                                  place (printable-ASCII path)
//   import-scan --root <folder>    the same for every folder inside an import folder
#include "stores/local/scanner.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

namespace {
using namespace playden::stores::local;

const char* StatusName(ScanStatus status) {
  switch (status) {
    case ScanStatus::kOk: return "ok";
    case ScanStatus::kMissing: return "missing";
    case ScanStatus::kNotFolder: return "not a folder";
    case ScanStatus::kUnsupported: return "unsupported";
    case ScanStatus::kFailed: return "failed";
  }
  return "unknown";
}

std::string Bytes(std::uint64_t bytes) {
  char text[32];
  if (bytes >= 1024ull * 1024 * 1024) std::snprintf(text, sizeof text, "%.1f GB", bytes / (1024.0 * 1024 * 1024));
  else if (bytes >= 1024ull * 1024) std::snprintf(text, sizeof text, "%.1f MB", bytes / (1024.0 * 1024));
  else std::snprintf(text, sizeof text, "%llu bytes", static_cast<unsigned long long>(bytes));
  return text;
}

int Report(const std::string& folder) {
  const auto started = std::chrono::steady_clock::now();
  const auto scan = ScanGameFolder(folder);
  const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
  std::printf("%s\n", folder.c_str());
  if (scan.status != ScanStatus::kOk) {
    std::printf("  status: %s (%s)\n", StatusName(scan.status), std::strerror(scan.error));
    return 1;
  }
  std::printf("  title: %s\n", scan.title.c_str());
  std::printf("  entries: %zu in %.1f ms%s%s\n", scan.entries, elapsed,
              scan.limit_reached ? " (stopped at the entry limit)" : "",
              scan.deeper_folders ? " (deeper folders not searched)" : "");
  std::printf("  ascii_ok: %s\n", scan.printable_path ? "yes (can play in place)" : "no (copy or move only)");
  if (scan.unreadable) std::printf("  unreadable entries: %zu\n", scan.unreadable);
  std::printf("  programs: %zu\n", scan.programs.size());
  for (std::size_t index = 0; index < scan.programs.size(); ++index) {
    const auto& program = scan.programs[index];
    std::printf("    %2zu. %s  %s%s%s\n", index + 1, program.path.c_str(), Bytes(program.bytes).c_str(),
                program.helper ? "  [installer or tool]" : "", program.printable ? "" : "  [not ASCII]");
  }
  return 0;
}
}  // namespace

int main(int argc, char** argv) {
  if (argc == 3 && std::strcmp(argv[1], "--root") == 0) {
    const std::string root = argv[2];
    const auto listed = ScanImportRoot(root);
    if (listed.status != ScanStatus::kOk) {
      std::fprintf(stderr, "%s: %s (%s)\n", root.c_str(), StatusName(listed.status), std::strerror(listed.error));
      return 1;
    }
    int failed = 0;
    for (const auto& name : listed.folders) failed |= Report(root + "/" + name);
    if (listed.limit_reached) std::printf("(more folders than the scanner lists)\n");
    return failed;
  }
  if (argc != 2 || argv[1][0] == '-') {
    std::fprintf(stderr, "usage: import-scan <game folder>\n       import-scan --root <import folder>\n");
    return 2;
  }
  return Report(argv[1]);
}
