// SPDX-License-Identifier: GPL-3.0-or-later
// A GOG build's __redist installers (gog/dependencies): where their files go in the game folder, their arguments and
// bitness from the recorded public dependency store, XNA's recorded depot manifest placed through a PathSet, and the
// redist list written and read back: P and S lines, a link, a list over 64 KiB, 65 entries, a torn line, a wrong
// header, programs that leave the redist folder. Usage: gog_dependencies_test <fixtures/gog> <scratch folder>.
#include "gog/content.h"
#include "gog/dependencies.h"
#include "gog/depot_reader.h"
#include "gog/inflate.h"
#include "gog/path_rules.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#define CHECK(condition)                                                                  \
  do {                                                                                    \
    if (!(condition)) {                                                                   \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
      std::exit(1);                                                                       \
    }                                                                                     \
  } while (false)

using namespace playden::gog;
namespace fs = std::filesystem;

namespace {

std::string g_fixtures;
std::string g_scratch;

std::string ReadFile(const std::string& path) {
  FILE* file = std::fopen(path.c_str(), "rb");
  CHECK(file != nullptr);
  std::string bytes;
  char buffer[4096];
  for (std::size_t got; (got = std::fread(buffer, 1, sizeof(buffer), file)) > 0;) bytes.append(buffer, got);
  std::fclose(file);
  return bytes;
}

void WriteFile(const std::string& path, const std::string& bytes) {
  FILE* file = std::fopen(path.c_str(), "wb");
  CHECK(file != nullptr);
  CHECK(std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size());
  CHECK(std::fclose(file) == 0);
}

std::string Placed(std::string_view raw) {
  const auto placed = RedistPlace(raw);
  CHECK(placed.has_value());
  return *placed;
}

void TestPlace() {
  CHECK(Placed("__redist/XNA/xnafx31_redist.msi") == ".playden-gog/redist/XNA/xnafx31_redist.msi");
  CHECK(Placed("__REDIST\\DirectX\\DXSETUP.exe") == ".playden-gog/redist/DirectX/DXSETUP.exe");
  CHECK(Placed("__Redist/MSVC2008/vcredist_x86.exe") == ".playden-gog/redist/MSVC2008/vcredist_x86.exe");
  CHECK(Placed("/__redist/openAL/oalinst.exe") == ".playden-gog/redist/openAL/oalinst.exe");
  // Without the prefix: the prefix is still added, nothing lands in the game folder.
  CHECK(Placed("MSVC2005/vcredist_x86.exe") == ".playden-gog/redist/MSVC2005/vcredist_x86.exe");
  CHECK(Placed("__redistx/a.exe") == ".playden-gog/redist/__redistx/a.exe");
  CHECK(Placed("__redist") == ".playden-gog/redist/__redist");
  // Refused: traversal, absolute, control characters, Playden's own names, an empty rest.
  for (const char* raw : {"__redist/../x", "__redist/a/../../x", "//etc/passwd", "C:/x.exe", "__redist/a\tb.exe",
                          "__redist/a\nb", "__redist/.playden-files", ".playden-gog/redist/x", "", "__redist/",
                          "__redist//x"})
    CHECK(!RedistPlace(raw));
  CHECK(!RedistPlace(std::string("__redist/a\0b", 12)));
  // Within kMaxPathBytes as GOG names it, past it once placed.
  const std::string part(252, 'a');
  const std::string near = part + "/" + part + "/" + part + "/" + part;
  CHECK(NormalizePath("__redist/" + near) && !RedistPlace("__redist/" + near));

  CHECK(IsRedistPlacement(".playden-gog/redist/XNA/x.msi"));
  PathRefusal why = PathRefusal::kNone;
  CHECK(!IsRedistPlacement("XNA/x.msi", &why) && why == PathRefusal::kReserved);
  CHECK(!IsRedistPlacement(".playden-gog/support/1/x", &why) && why == PathRefusal::kReserved);
  CHECK(!IsRedistPlacement(".PLAYDEN-GOG/redist/XNA/x.msi"));
  CHECK(!IsRedistPlacement(".playden-gog/redist/../x", &why) && why == PathRefusal::kDotComponent);
  CHECK(!IsRedistPlacement(".playden-gog/redist//x"));
  CHECK(!IsRedistPlacement(".playden-gog/redist/a\\b"));  // NormalizePath would turn it into another path
  CHECK(!IsRedistPlacement(".playden-gog/redist/"));
}

std::string InflateFixture(const std::string& name) {
  const auto zlib = ReadFile(g_fixtures + "/" + name);
  ZlibStream stream(kMaxDependencyManifestBytes);
  std::string out;
  CHECK(stream.Feed(reinterpret_cast<const std::uint8_t*>(zlib.data()), zlib.size(),
                    [&out](const std::uint8_t* data, std::size_t size) {
                      out.append(reinterpret_cast<const char*>(data), size);
                      return true;
                    }) == ZlibStream::Status::kOk);
  CHECK(stream.Finish() == ZlibStream::Status::kOk);
  return out;
}

void TestManifest() {
  const auto dependencies = ParseDependencyManifest(InflateFixture("dependencies-manifest.zlib"));
  CHECK(dependencies.has_value() && dependencies->size() == 69);
  const auto find = [&dependencies](const std::string& id) -> const Dependency& {
    for (const auto& dependency : *dependencies)
      if (dependency.id == id) return dependency;
    std::fprintf(stderr, "no %s\n", id.c_str());
    std::exit(1);
  };
  const auto& msvc2005 = find("MSVC2005");
  CHECK(msvc2005.arguments == "/q:a /c:\"VCREDI~3.EXE /q:a /c:\"\"msiexec /i vcredist.msi /qb\"\" \"");
  CHECK(msvc2005.usable_arguments && msvc2005.bitness.empty() && RedistDependency(msvc2005));
  const auto& xna = find("XNA");
  CHECK(xna.arguments == "/qn" && xna.exe_path == "__redist/XNA/xnafx31_redist.msi" && RedistDependency(xna));
  const auto& directx = find("DirectX");
  CHECK(directx.arguments == "/silent" && directx.usable_arguments);
  const auto& msvc2015 = find("MSVC2015_x64");
  CHECK((msvc2015.bitness == std::vector<std::string>{"64"}));
  // ISI really has empty arguments: usable, and told apart from unusable ones.
  const auto& isi = find("ISI");
  CHECK(isi.arguments.empty() && isi.usable_arguments);
  const auto& dosbox = find("DOSBox074_2CS");
  CHECK(!RedistDependency(dosbox) && dosbox.arguments.empty() && dosbox.usable_arguments);
  std::size_t redist = 0;
  for (const auto& dependency : *dependencies) redist += RedistDependency(dependency);
  CHECK(redist == 49);

  // Arguments Playden can't keep are cleared, and the entry stays (never "missing").
  const auto entry = [](const std::string& executable) {
    return R"({"dependencyId":"Odd","manifest":"11111111111111111111111111111111","size":1,"compressedSize":1,)"
           R"("executable":)" + executable + "}";
  };
  for (const std::string& executable :
       {std::string(R"({"path":"__redist/Odd/setup.exe","arguments":"/q\u0007"})"),
        std::string(R"({"path":"__redist/Odd/setup.exe","arguments":"/q\t/x"})"),
        std::string(R"({"path":"__redist/Odd/setup.exe","arguments":"/q \u00e9"})"),
        std::string(R"({"path":"__redist/Odd/setup.exe","arguments":7})"),
        R"({"path":"__redist/Odd/setup.exe","arguments":")" + std::string(1025, 'a') + "\"}"}) {
    const auto parsed = ParseDependencyManifest("{\"depots\":[" + entry(executable) + "]}");
    CHECK(parsed && parsed->size() == 1);
    CHECK(!parsed->front().usable_arguments && parsed->front().arguments.empty());
    CHECK(parsed->front().exe_path == "__redist/Odd/setup.exe");
  }
  const auto longest = ParseDependencyManifest(
      "{\"depots\":[" + entry(R"({"path":"__redist/Odd/setup.exe","arguments":")" + std::string(1024, 'a') + "\"}") +
      "]}");
  CHECK(longest && longest->front().usable_arguments && longest->front().arguments.size() == 1024);
  // Malformed bitness is dropped, the entry kept.
  const auto bitness = ParseDependencyManifest(
      R"({"depots":[{"dependencyId":"Odd","manifest":"11111111111111111111111111111111","size":1,"compressedSize":1,)"
      R"("osBitness":"64"}]})");
  CHECK(bitness && bitness->size() == 1 && bitness->front().bitness.empty());

  // XNA's recorded depot manifest: its one file is the program the repository names, placed under the redist folder,
  // next to a game file of the same name without a clash.
  PathSet paths;
  CHECK(paths.Add("xnafx31_redist.msi", PathKind::kFile, 1, false, 0).ok());
  std::vector<std::string> placed;
  DepotItemReader reader([&](const DepotItem& item) {
    const auto at = RedistPlace(item.path);
    CHECK(at.has_value());
    const auto added = paths.AddRedist(*at, PathKind::kFile, 1 + placed.size());
    CHECK(added.ok() && !added.replaced);
    placed.push_back(added.path);
    return true;
  });
  const auto manifest = ReadFile(g_fixtures + "/dependency-depot-XNA.json");
  CHECK(reader.Feed(reinterpret_cast<const std::uint8_t*>(manifest.data()), manifest.size()) ==
        DepotItemReader::Status::kOk);
  CHECK(reader.Finish() == DepotItemReader::Status::kOk);
  CHECK((placed == std::vector<std::string>{".playden-gog/redist/XNA/xnafx31_redist.msi"}));
  CHECK(FoldCase(placed.front()) == FoldCase(*RedistPlace(xna.exe_path)));
  // The same path in another case is one file (exFAT), and keeps its folder spelling; a link is refused.
  const auto again = paths.AddRedist(".playden-gog/redist/xna/XNAFX31_REDIST.MSI", PathKind::kFile, 9);
  CHECK(again.ok() && again.replaced == std::optional<std::size_t>(1) &&
        again.path == ".playden-gog/redist/XNA/XNAFX31_REDIST.MSI");
  CHECK(paths.AddRedist(".playden-gog/redist/XNA/link", PathKind::kLink, 10).refusal == PathRefusal::kLink);
  CHECK(paths.AddRedist(".playden-gog/redist/XNA/xnafx31_redist.msi/x", PathKind::kFile, 11).refusal ==
        PathRefusal::kClash);
  CHECK(!paths.AddRedist("XNA/x.msi", PathKind::kFile, 12).ok());
}

RedistEntry Package(const std::string& id, const std::string& program, const std::string& arguments) {
  RedistEntry entry;
  entry.id = id;
  entry.package = true;
  entry.program = program;
  entry.arguments = arguments;
  return entry;
}

RedistEntry Skip(const std::string& id, RedistSkip skip) {
  RedistEntry entry;
  entry.id = id;
  entry.skip = skip;
  return entry;
}

std::string MakeRoot(const std::string& name) {
  const auto root = g_scratch + "/" + name;
  fs::create_directories(root + "/.playden-gog");
  return root;
}

RedistList::Status ReadText(const std::string& name, const std::string& text) {
  const auto root = MakeRoot(name);
  WriteFile(root + "/" + kRedistListPath, text);
  const auto list = ReadRedistList(root);
  if (list.status != RedistList::Status::kOk) CHECK(list.entries.empty());
  return list.status;
}

void TestList() {
  const auto root = MakeRoot("round-trip");
  CHECK(ReadRedistList(root).status == RedistList::Status::kMissing);
  CHECK(ReadRedistList(g_scratch + "/nothing-here").status == RedistList::Status::kMissing);
  const std::string msvc2005 = "/q:a /c:\"VCREDI~3.EXE /q:a /c:\"\"msiexec /i vcredist.msi /qb\"\" \"";
  const std::vector<RedistEntry> entries{
      Package("XNA", ".playden-gog/redist/XNA/xnafx31_redist.msi", "/qn"),
      Package("MSVC2005", ".playden-gog/redist/MSVC2005/vcredist_x86.exe", msvc2005),
      Package("ISI", ".playden-gog/redist/ISI/scriptinterpreter.exe", ""),
      Skip("Gone", RedistSkip::kMissing),
      Skip("NoExe", RedistSkip::kNoProgram),
      Skip("Odd", RedistSkip::kRefused),
  };
  CHECK(WriteRedistList(root, entries) == 0);
  CHECK(!fs::exists(root + "/.playden-gog/redist.list.tmp"));
  CHECK(ReadFile(root + "/" + kRedistListPath) ==
        "gog-redist\tv1\n"
        "P\tXNA\t.playden-gog/redist/XNA/xnafx31_redist.msi\t/qn\n"
        "P\tMSVC2005\t.playden-gog/redist/MSVC2005/vcredist_x86.exe\t" + msvc2005 + "\n"
        "P\tISI\t.playden-gog/redist/ISI/scriptinterpreter.exe\t\n"
        "S\tGone\tmissing\nS\tNoExe\tno_program\nS\tOdd\trefused\n");
  const auto read = ReadRedistList(root);
  CHECK(read.status == RedistList::Status::kOk && read.entries.size() == entries.size());
  for (std::size_t index = 0; index < entries.size(); ++index) {
    const auto& got = read.entries[index];
    const auto& want = entries[index];
    CHECK(got.id == want.id && got.package == want.package);
    if (want.package) CHECK(got.program == want.program && got.arguments == want.arguments);
    else CHECK(got.skip == want.skip);
  }
  // A plan with no __redist dependencies writes the header alone; writing again replaces the list.
  CHECK(WriteRedistList(root, {}) == 0);
  CHECK(ReadFile(root + "/" + kRedistListPath) == "gog-redist\tv1\n");
  const auto empty = ReadRedistList(root);
  CHECK(empty.status == RedistList::Status::kOk && empty.entries.empty());

  // The writer refuses what the reader would refuse.
  CHECK(WriteRedistList(root, {Package("XNA", "../x.msi", "/qn")}) == EINVAL);
  CHECK(WriteRedistList(root, {Package("XNA", ".playden-gog/redist/XNA/x.msi", "/q\t")}) == EINVAL);
  CHECK(WriteRedistList(root, {Package("../XNA", ".playden-gog/redist/XNA/x.msi", "/qn")}) == EINVAL);
  CHECK(WriteRedistList(root, std::vector<RedistEntry>(65, Skip("A", RedistSkip::kMissing))) == EINVAL);
  // 64 entries, each valid on its own, past 64 KiB together.
  const std::string folder_name(200, 'a');
  const auto long_program = ".playden-gog/redist/" + folder_name + "/" + folder_name + "/" + folder_name + "/x.exe";
  CHECK(IsRedistPlacement(long_program));
  CHECK(WriteRedistList(root, {Package("A", long_program, std::string(1000, 'b'))}) == 0);
  CHECK(WriteRedistList(root, std::vector<RedistEntry>(64, Package("A", long_program, std::string(1000, 'b')))) ==
        EINVAL);
  CHECK(WriteRedistList(root, {}) == 0);
  CHECK(WriteRedistList("relative", {}) == EINVAL);
  CHECK(WriteRedistList(g_scratch + "/no-folder", {}) == ENOENT);
  CHECK(ReadFile(root + "/" + kRedistListPath) == "gog-redist\tv1\n");  // untouched by the refusals
  // A link at the temporary name is removed, never written through.
  const auto target = g_scratch + "/link-target";
  WriteFile(target, "keep\n");
  CHECK(symlink(target.c_str(), (root + "/.playden-gog/redist.list.tmp").c_str()) == 0);
  CHECK(WriteRedistList(root, {Skip("A", RedistSkip::kMissing)}) == 0);
  CHECK(ReadFile(target) == "keep\n" && ReadRedistList(root).entries.size() == 1);

  // 64 entries are fine; 65 are refused.
  std::string many = "gog-redist\tv1\n";
  for (int i = 0; i < 64; ++i) many += "S\tD" + std::to_string(i) + "\tmissing\n";
  CHECK(ReadText("sixty-four", many) == RedistList::Status::kOk);
  CHECK(ReadText("sixty-five", many + "S\tD64\tmissing\n") == RedistList::Status::kRefused);
  // Over 64 KiB, even in few lines.
  CHECK(ReadText("large", "gog-redist\tv1\nP\tA\t.playden-gog/redist/A/x.exe\t" + std::string(65 * 1024, 'a') +
                              "\n") == RedistList::Status::kRefused);
  // Torn, wrong headers, bad lines.
  CHECK(ReadText("torn", "gog-redist\tv1\nS\tA\tmissing\nS\tB\tmiss") == RedistList::Status::kRefused);
  CHECK(ReadText("header-torn", "gog-redist\tv1") == RedistList::Status::kRefused);
  CHECK(ReadText("empty", "") == RedistList::Status::kRefused);
  CHECK(ReadText("header-v2", "gog-redist\tv2\n") == RedistList::Status::kRefused);
  CHECK(ReadText("header-none", "S\tA\tmissing\n") == RedistList::Status::kRefused);
  CHECK(ReadText("header-crlf", "gog-redist\tv1\r\n") == RedistList::Status::kRefused);
  for (const char* line :
       {"P\tA\t.playden-gog/redist/A/../../../x.exe\t/q", "P\tA\t../x.exe\t/q", "P\tA\t/abs/x.exe\t/q",
        "P\tA\tA/x.exe\t/q", "P\tA\t.playden-gog/redist/A/x.exe", "P\tA\t.playden-gog/redist/A/x.exe\t/q\textra",
        "P\tA\t.playden-gog/redist/A/x.exe\t/q\x01", "P\tA\t.playden-gog/redist/A/x.exe\t/q\x7f",
        "S\tA\tgone", "S\tA", "S\t../A\tmissing", "S\t\tmissing", "X\tA\tmissing", "", "S\tA\tmissing\r"})
    CHECK(ReadText("bad-line", std::string("gog-redist\tv1\nS\tOk\tmissing\n") + line + "\n") ==
          RedistList::Status::kRefused);

  // A link in the list's place, or .playden-gog itself a link: refused, the target never read.
  const auto linked = MakeRoot("linked");
  const auto real = g_scratch + "/real-list";
  WriteFile(real, "gog-redist\tv1\nS\tA\tmissing\n");
  CHECK(symlink(real.c_str(), (linked + "/" + kRedistListPath).c_str()) == 0);
  CHECK(ReadRedistList(linked).status == RedistList::Status::kRefused);
  const auto folder_linked = g_scratch + "/folder-linked";
  fs::create_directories(folder_linked);
  CHECK(symlink((root + "/.playden-gog").c_str(), (folder_linked + "/.playden-gog").c_str()) == 0);
  CHECK(ReadRedistList(folder_linked).status == RedistList::Status::kRefused);
  // A folder in the list's place.
  const auto folder = MakeRoot("folder");
  fs::create_directories(folder + "/" + kRedistListPath);
  CHECK(ReadRedistList(folder).status == RedistList::Status::kRefused);
}

}  // namespace

int main(int argc, char** argv) {
  CHECK(argc == 3);
  g_fixtures = argv[1];
  fs::remove_all(argv[2]);
  fs::create_directories(argv[2]);
  g_scratch = fs::canonical(argv[2]).string();
  TestPlace();
  TestManifest();
  TestList();
  fs::remove_all(g_scratch);
  std::printf("gog dependencies ok\n");
  return 0;
}
