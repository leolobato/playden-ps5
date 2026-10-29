#include "update/file_ops.h"
#include "update/zip_extract.h"
#include "zip_fixture.h"

#include <sys/stat.h>

#include <cerrno>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace playden::update;
using zip_fixture::Entry;
#define CHECK(value) do { if (!(value)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #value); std::exit(1); } } while(false)

namespace {
std::string root;
int counter = 0;

// Writes the zip, unpacks it into a fresh folder and returns the result; *dest names the folder.
ZipExtractResult Run(const std::vector<Entry>& entries, ZipExtractOptions options, std::string* dest = nullptr) {
  const std::string zip = root + "/case" + std::to_string(++counter) + ".zip";
  const std::string out = root + "/out" + std::to_string(counter);
  CHECK(zip_fixture::Write(zip, zip_fixture::Build(entries)));
  CHECK(MakeFolders(out));
  if (dest) *dest = out;
  return ExtractZip(zip, out, options);
}

std::string Read(const std::string& path) {
  std::string text;
  CHECK(ReadSmallFile(path, &text, 256u << 20));
  return text;
}

unsigned Mode(const std::string& path) {
  struct stat st{};
  CHECK(stat(path.c_str(), &st) == 0);
  return st.st_mode & 07777;
}
}  // namespace

int main() {
  root = zip_fixture::TempFolder("zip");
  CHECK(!root.empty());

  CHECK(SafeEntryName("a/b.txt"));
  CHECK(SafeEntryName("a/b/"));
  for (const char* bad : {"", "/etc/passwd", "../x", "a/../b", "a/./b", "a//b", "a\\b", "C:x", "a/\x01"})
    CHECK(!SafeEntryName(bad));

  // Only the inner folder is unpacked, without its prefix; modes and folders follow the zip.
  {
    std::string out;
    ZipExtractOptions options;
    options.inner_prefix = "top/app/";
    std::vector<std::pair<std::uint64_t, std::uint64_t>> progress;
    options.progress = [&](std::uint64_t done, std::uint64_t total) { progress.push_back({done, total}); };
    const auto result = Run({{"top/", "", 040755}, {"top/README.md", "readme"}, {"top/app/", "", 040755},
                             {"top/app/eboot.bin", "elf", 0100755}, {"top/app/sce_sys/param.json", "{}"},
                             {"top/app/empty/", "", 040755}},
                            options, &out);
    CHECK(result.ok && result.files == 2 && result.folders == 1 && result.bytes == 5);
    CHECK(Read(out + "/eboot.bin") == "elf" && Mode(out + "/eboot.bin") == 0755);
    CHECK(Read(out + "/sce_sys/param.json") == "{}" && Mode(out + "/sce_sys/param.json") == 0644);
    CHECK(IsFolder(out + "/empty") && !Exists(out + "/README.md"));
    CHECK(progress.size() == 2 && progress.back().first == 5 && progress.back().second == 5);
  }
  // mode_for decides file modes and may refuse a file.
  {
    std::string out;
    ZipExtractOptions options;
    options.mode_for = [](const std::string& name, unsigned) -> std::optional<unsigned> {
      if (name == "x.elf") return 0755u;
      return 0600u;
    };
    CHECK(Run({{"x.elf", "1"}, {"y.txt", "2", 0100777}}, options, &out).ok);
    CHECK(Mode(out + "/x.elf") == 0755 && Mode(out + "/y.txt") == 0600);
    options.mode_for = [](const std::string&, unsigned) -> std::optional<unsigned> { return std::nullopt; };
    CHECK(!Run({{"x", "1"}}, options).ok);
  }
  // A zip without unix modes gets 0644 files.
  {
    std::string out;
    Entry plain{"plain.txt", "p"};
    plain.unix_made = false;
    CHECK(Run({plain}, {}, &out).ok && Mode(out + "/plain.txt") == 0644);
  }
  // Refusals: nothing unsafe gets written.
  {
    std::string out;
    CHECK(!Run({{"ok.txt", "1"}, {"../evil", "x"}}, {}, &out).ok);
    CHECK(ListFolder(out).empty());
    CHECK(!Run({{"a", "1"}, {"a", "2"}}, {}).ok);                    // duplicate
    CHECK(!Run({{"dir/", ""}, {"dir", "x"}}, {}).ok);                // duplicate after normalising
    CHECK(!Run({{"link", "target", 0120777}}, {}, &out).ok);         // symlink
    CHECK(!Run({{"fifo", "", 0010644}}, {}).ok);                     // special file
    Entry encrypted{"secret", "x"};
    encrypted.flags = 1;
    CHECK(!Run({encrypted}, {}).ok);
    ZipExtractOptions small;
    small.max_total_bytes = 3;
    CHECK(!Run({{"a", "12"}, {"b", "34"}}, small).ok);
    ZipExtractOptions few;
    few.max_entries = 1;
    CHECK(!Run({{"a", "1"}, {"b", "2"}}, few).ok);
    ZipExtractOptions bad_prefix;
    bad_prefix.inner_prefix = "../";
    CHECK(!Run({{"a", "1"}}, bad_prefix).ok);
  }
  // A file whose mode can't be set (created as root while another thread held euid 0 on the PS5) is
  // deleted and written again; one that never takes its mode fails the unpack.
  {
    std::string out;
    int calls = 0;
    ZipExtractOptions options;
    options.set_mode = [&calls](int fd, unsigned mode) {
      if (++calls == 2) { errno = EPERM; return -1; }  // the second file's first try
      return fchmod(fd, mode);
    };
    std::vector<std::string> lines;
    options.log = [&lines](const std::string& line) { lines.push_back(line); };
    const auto result = Run({{"a.dll", "aa"}, {"b.dll", "bb"}, {"c.dll", "cc"}}, options, &out);
    CHECK(result.ok && result.files == 3 && result.rewritten == 1 && calls == 4);
    CHECK(Read(out + "/b.dll") == "bb" && Mode(out + "/b.dll") == 0644);
    CHECK(lines.size() == 1 && lines[0].find("b.dll chmod=-1") != std::string::npos);
    options.set_mode = [](int, unsigned) { errno = EPERM; return -1; };
    const auto stuck = Run({{"a.dll", "aa"}}, options);
    CHECK(!stuck.ok && stuck.error.find("a.dll") != std::string::npos);
    // With a root hook, a file the app user can't chmod gets its mode from it, without rewriting.
    std::vector<std::string> fixed;
    options.fix_mode = [&fixed](const std::string& path, unsigned mode) {
      fixed.push_back(path);
      return chmod(path.c_str(), mode) == 0;
    };
    const auto rescued = Run({{"a.dll", "aa"}, {"run.elf", "e", 0100755}}, options, &out);
    CHECK(rescued.ok && rescued.mode_fixed == 2 && rescued.rewritten == 0 && fixed.size() == 2);
    CHECK(Mode(out + "/a.dll") == 0644 && Mode(out + "/run.elf") == 0755);
    // A root hook that fails falls back to writing the file again.
    options.fix_mode = [](const std::string&, unsigned) { return false; };
    CHECK(!Run({{"a.dll", "aa"}}, options).ok);
  }
  // folder_mode sets every created folder's mode, also folders the zip only implies, whatever the umask.
  {
    std::string out;
    ZipExtractOptions options;
    options.folder_mode = 0777;
    const mode_t old_umask = umask(022);
    const auto result = Run({{"a/b/c.txt", "c"}, {"d/", "", 040755}, {"d/e/", "", 040700}}, options, &out);
    umask(old_umask);
    CHECK(result.ok && result.files == 1 && result.folders == 2);
    CHECK(Mode(out + "/a") == 0777 && Mode(out + "/a/b") == 0777);
    CHECK(Mode(out + "/d") == 0777 && Mode(out + "/d/e") == 0777);
    // Without it (the kit unpack) folders keep mkdir's 0777 less the umask.
    umask(022);
    const auto plain = Run({{"a/b/c.txt", "c"}, {"d/", "", 040777}}, {}, &out);
    umask(old_umask);
    CHECK(plain.ok && Mode(out + "/a") == 0755 && Mode(out + "/a/b") == 0755 && Mode(out + "/d") == 0755);
    // A folder the app user can't chmod (made while another thread held root) gets its mode as root.
    options.set_folder_mode = [](const std::string&, unsigned) { errno = EPERM; return -1; };
    std::vector<std::string> fixed;
    options.fix_mode = [&fixed](const std::string& path, unsigned mode) {
      fixed.push_back(path);
      return chmod(path.c_str(), mode) == 0;
    };
    umask(022);
    const auto rescued = Run({{"x/y.txt", "y"}}, options, &out);
    CHECK(rescued.ok && fixed.size() == 1 && fixed[0] == out + "/x" && Mode(out + "/x") == 0777);
    // Without a working root hook the unpack fails.
    options.fix_mode = [](const std::string&, unsigned) { return false; };
    const auto stuck = Run({{"x/y.txt", "y"}}, options);
    umask(old_umask);
    CHECK(!stuck.ok && stuck.error.find("x/y.txt") != std::string::npos);
  }
  // A damaged file fails its CRC check.
  {
    Entry damaged{"data.bin", "payload"};
    damaged.corrupt_crc = true;
    const auto result = Run({damaged}, {});
    CHECK(!result.ok && result.error.find("data.bin") != std::string::npos);
  }
  // Not a zip, a truncated zip, and a destination that is not empty.
  {
    const std::string junk = root + "/junk.zip";
    CHECK(zip_fixture::Write(junk, "not a zip"));
    CHECK(MakeFolders(root + "/junk-out"));
    CHECK(!ExtractZip(junk, root + "/junk-out", {}).ok);
    std::string whole = zip_fixture::Build({{"a", std::string(1000, 'a')}});
    CHECK(zip_fixture::Write(junk, whole.substr(0, whole.size() - 30)));
    CHECK(!ExtractZip(junk, root + "/junk-out", {}).ok);
    CHECK(zip_fixture::Write(junk, whole));
    CHECK(zip_fixture::Write(root + "/junk-out/existing", "x"));
    CHECK(!ExtractZip(junk, root + "/junk-out", {}).ok);
  }
  // A file larger than the write buffer streams through, and cancel stops the run.
  {
    std::string out;
    std::string big(5 * 1024 * 1024 + 17, '\0');
    for (std::size_t i = 0; i < big.size(); ++i) big[i] = static_cast<char>(i * 31 % 251);
    CHECK(Run({{"big.bin", big}}, {}, &out).ok);
    CHECK(Read(out + "/big.bin") == big);
    std::atomic<bool> cancel{false};
    ZipExtractOptions options;
    options.cancel = &cancel;
    options.progress = [&](std::uint64_t, std::uint64_t) { cancel = true; };
    const auto result = Run({{"1", "a"}, {"2", "b"}, {"3", "c"}}, options, &out);
    CHECK(!result.ok && result.cancelled && result.files == 1);
  }
  // FindInnerPrefix locates "<top>/<inner>/" by a marker file.
  {
    const std::string zip = root + "/prefix.zip";
    CHECK(zip_fixture::Write(zip, zip_fixture::Build({{"playden-ps5-0.3.0/tools/x.py", ""},
                                                      {"playden-ps5-0.3.0/FAKE77031/sce_sys/param.json", "{}"}})));
    CHECK(FindInnerPrefix(zip, "sce_sys/param.json") == "playden-ps5-0.3.0/FAKE77031/");
    CHECK(FindInnerPrefix(zip, "manifest.json").empty());
  }
  RemoveTree(root);
  std::puts("ok");
  return 0;
}
