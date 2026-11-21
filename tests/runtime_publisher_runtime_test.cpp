#include "runtime/publisher_runtime.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(condition)                                                                  \
  do {                                                                                    \
    if (!(condition)) {                                                                   \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
      std::exit(1);                                                                       \
    }                                                                                     \
  } while (false)

namespace fs = std::filesystem;
using namespace playden::runtime;

namespace {

// The fixtures (tests/fixtures/publisher-runtime, made by make-fixtures.py) are plain 7z
// archives; each test wraps one in a made-up installer prefix.
fs::path g_fixtures;

std::string ReadFile(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  CHECK(in.good());
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void WriteFile(const fs::path& path, const std::string& bytes) {
  fs::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << bytes;
  CHECK(out.good());
}

std::string Fixture(const std::string& name) { return ReadFile(g_fixtures / name); }

// A self-extractor's head: "MZ", a stray 7z signature that is no archive, padding.
std::string InstallerPrefix() {
  std::string prefix = "MZ";
  prefix.resize(200, '\x90');
  prefix += std::string("7z\xBC\xAF\x27\x1C", 6);
  prefix += "not an archive header";
  prefix.resize(4096, '\0');
  return prefix;
}

std::uint32_t Crc32(const char* data, std::size_t size) {
  std::uint32_t crc = 0xFFFFFFFFu;
  for (std::size_t i = 0; i < size; ++i) {
    crc ^= static_cast<unsigned char>(data[i]);
    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

std::uint64_t Le64(const std::string& bytes, std::size_t at) {
  std::uint64_t value = 0;
  for (int i = 7; i >= 0; --i) value = value << 8 | static_cast<unsigned char>(bytes[at + i]);
  return value;
}

void PutLe32(std::string* bytes, std::size_t at, std::uint32_t value) {
  for (int i = 0; i < 4; ++i) (*bytes)[at + i] = static_cast<char>(value >> (8 * i));
}

std::string Utf16(const std::string& text) {
  std::string out;
  for (char c : text) {
    out += c;
    out += '\0';
  }
  return out;
}

// Renames an entry in an archive with an uncompressed header (same length) and fixes the CRCs.
std::string PatchName(std::string archive, const std::string& from, const std::string& to) {
  CHECK(from.size() == to.size());
  const auto header = 32 + Le64(archive, 12);
  const auto header_size = Le64(archive, 20);
  const auto at = archive.find(Utf16(from), header);
  CHECK(at != std::string::npos && at + from.size() * 2 <= header + header_size);
  archive.replace(at, from.size() * 2, Utf16(to));
  PutLe32(&archive, 28, Crc32(archive.data() + header, header_size));
  PutLe32(&archive, 8, Crc32(archive.data() + 12, 20));
  return archive;
}

PublisherRuntimeSpec Spec() {
  PublisherRuntimeSpec spec;
  spec.label = "Example Runtime";
  spec.installers = {"Redist/Example-Setup.exe", "Setup/Example-Setup.exe"};
  spec.version_file = "x86/runtime.dll";
  spec.folders = {{"x86", "x86"}, {"x64", "lib64/Example Runtime"}};
  return spec;
}

struct Scratch {
  Scratch() {
    char pattern[] = "/tmp/playden-publisher-runtime-XXXXXX";
    CHECK(::mkdtemp(pattern));
    root = pattern;
  }
  ~Scratch() {
    std::error_code ignored;
    fs::remove_all(root, ignored);
  }
  fs::path install() const { return root / "game"; }
  fs::path runtime() const { return root / "runtime" / "examplepub"; }
  void Install(const std::string& archive, const std::string& where = "Redist/Example-Setup.exe") const {
    WriteFile(install() / where, InstallerPrefix() + archive);
  }
  PublisherRuntimeResult Ensure(const PublisherRuntimeSpec& spec = Spec(), const PublisherRuntimeProgress& progress = {},
                                const std::function<bool()>& canceled = {}) const {
    return EnsurePublisherRuntime(spec, install().string(), runtime().string(), progress, canceled);
  }
  fs::path root;
};

unsigned Mode(const fs::path& path) {
  struct stat info {};
  CHECK(::lstat(path.c_str(), &info) == 0);
  return info.st_mode & 07777;
}

bool HasStaging(const fs::path& root) {
  std::error_code error;
  for (fs::directory_iterator it(root, error), end; !error && it != end; it.increment(error))
    if (it->path().filename().string().rfind(".staging-", 0) == 0) return true;
  return false;
}

// The tree every fixture holds: x86 -> x86, x64 -> lib64/Example Runtime, other/ skipped.
void CheckTree(const fs::path& version_dir, const std::string& version) {
  CHECK(PeFileVersion(ReadFile(version_dir / "x86/runtime.dll")) == version);
  CHECK(ReadFile(version_dir / "x86/readme.txt") == "hello from x86\n");
  CHECK(fs::is_regular_file(version_dir / "x86/empty.txt") && fs::file_size(version_dir / "x86/empty.txt") == 0);
  const auto x64 = version_dir / "lib64/Example Runtime";
  CHECK(PeFileVersion(ReadFile(x64 / "lib/helper.dll")) == std::string("9.8.7.6"));  // PE32+
  CHECK(fs::file_size(x64 / "locales/en.pak") == 900);
  CHECK(fs::is_directory(x64 / "emptydir"));
  CHECK(!fs::exists(version_dir / "x64") && !fs::exists(version_dir / "other"));
  CHECK(fs::is_regular_file(version_dir / kPublisherRuntimeCompleteMarker));
  for (const auto& item : fs::recursive_directory_iterator(version_dir))
    CHECK(Mode(item.path()) == (item.is_directory() ? 0755u : 0644u));
  CHECK(Mode(version_dir) == 0755);
}

void TestBcj2ExtractionAndReuse() {
  Scratch scratch;
  scratch.Install(Fixture("bcj2.7z"));
  WriteFile(scratch.runtime() / ".staging-0.0.0.1/left", "over");
  std::uint64_t last_done = 0, last_total = 0;
  int calls = 0;
  const auto result = scratch.Ensure(Spec(), [&](std::uint64_t done, std::uint64_t total) {
    CHECK(done >= last_done && done <= total);
    last_done = done;
    last_total = total;
    ++calls;
  });
  CHECK(result.ok && result.extracted && result.reason.empty());
  CHECK(result.version == "1.2.3.4");
  CHECK(result.version_dir == (scratch.runtime() / "1.2.3.4").string());
  CheckTree(result.version_dir, "1.2.3.4");
  CHECK(calls > 1 && last_done == last_total);
  std::uint64_t sum = 0;
  for (const auto& item : fs::recursive_directory_iterator(result.version_dir))
    if (item.is_regular_file()) sum += item.file_size();
  CHECK(sum == last_total);
  CHECK(!HasStaging(scratch.runtime()));

  // Already complete: reused as it is.
  WriteFile(fs::path(result.version_dir) / "x86/readme.txt", "changed");
  const auto again = scratch.Ensure();
  CHECK(again.ok && !again.extracted && again.version == "1.2.3.4");
  CHECK(ReadFile(fs::path(result.version_dir) / "x86/readme.txt") == "changed");

  // The installer is not read again while it is unchanged (same path, size and time) and the
  // version folder is complete: even garbage in its place still answers.
  {
    const auto installer = scratch.install() / "Redist/Example-Setup.exe";
    const auto stamp = fs::last_write_time(installer);
    const auto size = fs::file_size(installer);
    WriteFile(installer, std::string(size, 'G'));
    fs::last_write_time(installer, stamp);
    const auto cached = scratch.Ensure();
    CHECK(cached.ok && !cached.extracted && cached.version == "1.2.3.4" && cached.version_dir == result.version_dir);
    // A changed installer is read again (and found damaged).
    WriteFile(installer, std::string(size + 1, 'G'));
    CHECK(!scratch.Ensure().ok);
    scratch.Install(Fixture("bcj2.7z"));
  }

  // A version folder without .complete is extracted again.
  fs::remove(fs::path(result.version_dir) / kPublisherRuntimeCompleteMarker);
  const auto redo = scratch.Ensure();
  CHECK(redo.ok && redo.extracted);
  CheckTree(redo.version_dir, "1.2.3.4");
}

void TestSolidAndRetention() {
  Scratch scratch;
  fs::create_directories(scratch.runtime() / "notes");
  scratch.Install(Fixture("bcj2.7z"));
  CHECK(scratch.Ensure().ok);
  scratch.Install(Fixture("solid.7z"));
  const auto solid = scratch.Ensure();
  CHECK(solid.ok && solid.extracted && solid.version == "1.2.3.5");
  CheckTree(solid.version_dir, "1.2.3.5");
  CHECK(fs::exists(scratch.runtime() / "1.2.3.4") && fs::exists(scratch.runtime() / "1.2.3.5"));

  fs::create_directories(scratch.runtime() / "1.0.0.0");  // unfinished leftover
  scratch.Install(Fixture("plain.7z"));
  const auto plain = scratch.Ensure();
  CHECK(plain.ok && plain.extracted && plain.version == "1.2.3.6");
  CheckTree(plain.version_dir, "1.2.3.6");
  CHECK(ReadFile(fs::path(plain.version_dir) / "x86/ab/c.txt") == "c\n");
  CHECK(fs::exists(scratch.runtime() / "1.2.3.6") && fs::exists(scratch.runtime() / "1.2.3.5"));
  CHECK(!fs::exists(scratch.runtime() / "1.2.3.4") && !fs::exists(scratch.runtime() / "1.0.0.0"));
  CHECK(fs::is_directory(scratch.runtime() / "notes"));
}

// The x86 BCJ filter over LZMA (Social Club 2.4.0.302's installer, GTA V Enhanced). The fixture's one file runs
// past Playden's 1 MiB decode block with a converted CALL across the block's end, so the bytes the filter holds
// back must carry over and the code after them needs the right position; the file's CRC is checked.
void TestBcjExtraction() {
  Scratch scratch;
  scratch.Install(Fixture("bcj.7z"));
  const auto result = scratch.Ensure();
  if (!result.ok) std::fprintf(stderr, "reason: %s\n", result.reason.c_str());
  CHECK(result.ok && result.extracted && result.version == "1.2.3.9");
  const auto dll = fs::path(result.version_dir) / "x86/runtime.dll";
  CHECK(PeFileVersion(ReadFile(dll)) == std::string("1.2.3.9"));
  CHECK(fs::file_size(dll) == (1u << 20) + 3 + 16 * 1024);
  CHECK(fs::is_regular_file(fs::path(result.version_dir) / kPublisherRuntimeCompleteMarker));
}

void TestInstallerLookup() {
  Scratch scratch;
  auto missing = scratch.Ensure();
  CHECK(!missing.ok && missing.reason.find("not found") != std::string::npos);
  CHECK(!fs::exists(scratch.runtime()));

  scratch.Install(Fixture("bcj2.7z"), "Setup/Example-Setup.exe");  // the second location
  CHECK(scratch.Ensure().ok);

  Scratch empty;
  WriteFile(empty.install() / "Redist/Example-Setup.exe", InstallerPrefix());
  const auto none = empty.Ensure();
  CHECK(!none.ok && none.reason.find("no archive") != std::string::npos);

  auto spec = Spec();
  spec.version_file = "x86/missing.dll";
  scratch.Install(Fixture("bcj2.7z"));
  const auto no_version = scratch.Ensure(spec);
  CHECK(!no_version.ok && no_version.reason.find("x86/missing.dll") != std::string::npos);
}

void TestRefusals() {
  const auto refused = [](const std::string& archive, const std::string& expected) {
    Scratch scratch;
    scratch.Install(archive);
    const auto result = scratch.Ensure();
    if (result.ok || result.reason.find(expected) == std::string::npos)
      std::fprintf(stderr, "reason: %s\n", result.reason.c_str());
    CHECK(!result.ok && result.reason.find(expected) != std::string::npos);
    CHECK(!fs::exists(scratch.runtime()) || fs::is_empty(scratch.runtime()));
  };
  const auto plain = Fixture("plain.7z");
  refused(PatchName(plain, "x86/ab/c.txt", "x86/../c.txt"), "unsafe path");
  refused(PatchName(plain, "x64/zz.bin", "/x6/zz.bin"), "unsafe path");
  refused(PatchName(plain, "x64/zz.bin", "C:4/zz.bin"), "unsafe path");
  refused(PatchName(plain, "x64/zz.bin", "x64\\zz.bin"), "unsafe path");
  refused(PatchName(plain, "x86/b.txt", "x86/A.txt"), "path twice");
  refused(Fixture("link.7z"), "link");
  refused(Fixture("lzma2.7z"), "unsupported 7z method");

  // Two archive folders mapped onto one target.
  Scratch scratch;
  scratch.Install(Fixture("bcj2.7z"));
  auto spec = Spec();
  spec.folders = {{"x86", "same"}, {"x64", "same"}};
  const auto clash = scratch.Ensure(spec);
  CHECK(!clash.ok && clash.reason.find("map to") != std::string::npos);
}

void TestDamagedArchive() {
  auto archive = Fixture("plain.7z");
  // Files are packed in path order: the middle of the packed data is x64/lib/helper.dll, not
  // the version file. The version is read, extraction starts and fails on that file.
  const auto middle = 32 + Le64(archive, 12) / 2;
  archive[middle] = static_cast<char>(archive[middle] ^ 0x55);
  Scratch scratch;
  scratch.Install(archive);
  const auto result = scratch.Ensure();
  CHECK(!result.ok && result.version == "1.2.3.6");
  CHECK(result.reason.find("damaged") != std::string::npos);
  CHECK(!fs::exists(scratch.runtime() / "1.2.3.6"));
  CHECK(!HasStaging(scratch.runtime()));
}

void TestCancel() {
  Scratch scratch;
  scratch.Install(Fixture("bcj2.7z"));
  bool started = false;
  const auto result = scratch.Ensure(
      Spec(), [&](std::uint64_t, std::uint64_t) { started = true; }, [&] { return started; });
  CHECK(!result.ok && result.reason == "canceled");
  CHECK(!fs::exists(scratch.runtime() / "1.2.3.4"));
  CHECK(!HasStaging(scratch.runtime()));
}

void TestPeFileVersion() {
  CHECK(!PeFileVersion(""));
  CHECK(!PeFileVersion(std::string(4096, '\0')));
  std::string mz(4096, '\0');
  mz[0] = 'M';
  mz[1] = 'Z';
  CHECK(!PeFileVersion(mz));
  Scratch scratch;
  scratch.Install(Fixture("bcj2.7z"));
  const auto result = scratch.Ensure();
  CHECK(result.ok);
  const auto image = ReadFile(fs::path(result.version_dir) / "x86/runtime.dll");
  CHECK(PeFileVersion(image) == std::string("1.2.3.4"));
  CHECK(!PeFileVersion(image.substr(0, 0x200)));  // resources cut off
  for (std::size_t size = 0; size < 0x400; size += 7) PeFileVersion(image.substr(0, size));
}

void TestPathRules() {
  CHECK(PublisherRuntimePathAllowed("x86"));
  CHECK(PublisherRuntimePathAllowed("x64/locales/en-US.pak"));
  CHECK(PublisherRuntimePathAllowed("lib64/Example Runtime/a b.dll"));
  CHECK(!PublisherRuntimePathAllowed(""));
  CHECK(!PublisherRuntimePathAllowed("/x86/a"));
  CHECK(!PublisherRuntimePathAllowed("x86/../a"));
  CHECK(!PublisherRuntimePathAllowed(".."));
  CHECK(!PublisherRuntimePathAllowed("x86/./a"));
  CHECK(!PublisherRuntimePathAllowed("x86//a"));
  CHECK(!PublisherRuntimePathAllowed("x86/"));
  CHECK(!PublisherRuntimePathAllowed("C:/a"));
  CHECK(!PublisherRuntimePathAllowed("x86\\a"));
  CHECK(!PublisherRuntimePathAllowed(std::string("x86/a\nb")));
}

}  // namespace

int main(int argc, char** argv) {
  CHECK(argc == 2);
  g_fixtures = argv[1];
  TestPathRules();
  TestPeFileVersion();
  TestBcj2ExtractionAndReuse();
  TestSolidAndRetention();
  TestBcjExtraction();
  TestInstallerLookup();
  TestRefusals();
  TestDamagedArchive();
  TestCancel();
  std::puts("runtime_publisher_runtime_test: ok");
  return 0;
}
