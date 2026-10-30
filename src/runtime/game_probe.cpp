#include "runtime/game_probe.h"

#include "runtime/steam_stub.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace playden::runtime {
namespace {

namespace fs = std::filesystem;

std::string Lower(std::string text) {
  for (auto& value : text)
    if (value >= 'A' && value <= 'Z') value = static_cast<char>(value - 'A' + 'a');
  return text;
}

bool EndsWith(const std::string& text, std::string_view suffix) {
  return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// A program file opened without following links. Reads are positioned, so the import
// table is found through the section table wherever it sits in a large program.
class Program {
 public:
  explicit Program(const std::string& path) : file_(::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC)) {}
  ~Program() { if (file_ >= 0) ::close(file_); }
  Program(const Program&) = delete;
  Program& operator=(const Program&) = delete;

  bool ReadAt(std::uint64_t offset, void* buffer, std::size_t size) const {
    if (file_ < 0) return false;
    auto* bytes = static_cast<unsigned char*>(buffer);
    std::size_t done = 0;
    while (done < size) {
      const auto count = ::pread(file_, bytes + done, size - done, static_cast<off_t>(offset + done));
      if (count < 0 && errno == EINTR) continue;
      if (count <= 0) return false;
      done += static_cast<std::size_t>(count);
    }
    return true;
  }
  // Up to size bytes at offset; returns how many were read.
  std::size_t ReadSome(std::uint64_t offset, unsigned char* buffer, std::size_t size) const {
    if (file_ < 0) return 0;
    for (;;) {
      const auto count = ::pread(file_, buffer, size, static_cast<off_t>(offset));
      if (count < 0 && errno == EINTR) continue;
      return count < 0 ? 0 : static_cast<std::size_t>(count);
    }
  }

 private:
  int file_;
};

std::uint32_t U32(const unsigned char* bytes) {
  return bytes[0] | (bytes[1] << 8) | (bytes[2] << 16) | (static_cast<std::uint32_t>(bytes[3]) << 24);
}
std::uint16_t U16(const unsigned char* bytes) { return static_cast<std::uint16_t>(bytes[0] | (bytes[1] << 8)); }

struct Section {
  std::string name;
  std::uint32_t address = 0;
  std::uint32_t size = 0;
  std::uint32_t raw = 0;
  std::uint32_t flags = 0;
};

// The console's memory pages are 16 KB; Windows lays sections out on 4 KB ones.
constexpr std::uint32_t kConsolePage = 0x4000;
constexpr std::uint32_t kSharedWritable = 0x10000000u | 0x80000000u;  // IMAGE_SCN_MEM_SHARED | _WRITE

struct PeFacts {
  bool valid = false;
  bool clr = false;
  std::string entry_section;
  // A writable section shared between processes that doesn't fill whole console pages.
  bool unaligned_shared = false;
  bool pe64 = false;                 // PE32+ (x64), otherwise PE32 (x86)
  std::vector<std::string> imports;  // lower case
  std::vector<std::string> delay_imports;  // lower case; read only when asked
};

PeFacts ReadPe(const std::string& path, bool delay_imports = false) {
  PeFacts facts;
  Program program(path);
  unsigned char dos[64];
  if (!program.ReadAt(0, dos, sizeof(dos)) || dos[0] != 'M' || dos[1] != 'Z') return facts;
  const std::uint32_t pe = U32(dos + 0x3c);
  if (pe > 64u * 1024u * 1024u) return facts;
  unsigned char header[24];
  if (!program.ReadAt(pe, header, sizeof(header)) || std::memcmp(header, "PE\0\0", 4) != 0) return facts;
  const unsigned sections = U16(header + 6);
  const unsigned optional_size = U16(header + 20);
  if (sections == 0 || sections > 96 || optional_size < 96 || optional_size > 1024) return facts;
  std::vector<unsigned char> optional(optional_size);
  if (!program.ReadAt(pe + 24, optional.data(), optional.size())) return facts;
  const unsigned magic = U16(optional.data());
  if (magic != 0x10b && magic != 0x20b) return facts;
  const std::uint32_t entry = U32(optional.data() + 16);
  const std::size_t directories = magic == 0x20b ? 112 : 96;
  const auto directory = [&](unsigned index) -> std::uint32_t {
    const auto at = directories + index * 8;
    return at + 8 <= optional.size() ? U32(optional.data() + at) : 0;
  };
  const std::uint32_t import_rva = directory(1);
  const std::uint32_t delay_rva = directory(13);
  facts.clr = directory(14) != 0;
  facts.pe64 = magic == 0x20b;

  std::vector<unsigned char> table(sections * 40u);
  if (!program.ReadAt(pe + 24 + optional_size, table.data(), table.size())) return facts;
  std::vector<Section> list;
  for (unsigned i = 0; i < sections; ++i) {
    const auto* row = table.data() + i * 40;
    Section section;
    section.name.assign(reinterpret_cast<const char*>(row), strnlen(reinterpret_cast<const char*>(row), 8));
    section.address = U32(row + 12);
    section.size = std::max(U32(row + 8), U32(row + 16));
    section.raw = U32(row + 20);
    section.flags = U32(row + 36);
    const auto mapped = (U32(row + 8) + 0xfffu) & ~0xfffu;
    if ((section.flags & kSharedWritable) == kSharedWritable &&
        (section.address % kConsolePage != 0 || mapped % kConsolePage != 0))
      facts.unaligned_shared = true;
    list.push_back(section);
  }
  const auto file_offset = [&](std::uint32_t rva) -> std::optional<std::uint64_t> {
    for (const auto& section : list)
      if (rva >= section.address && rva - section.address < section.size)
        return static_cast<std::uint64_t>(section.raw) + (rva - section.address);
    return std::nullopt;
  };
  for (const auto& section : list)
    if (entry >= section.address && entry - section.address < section.size) facts.entry_section = section.name;
  facts.valid = true;

  auto descriptor = import_rva ? file_offset(import_rva) : std::nullopt;
  for (unsigned count = 0; descriptor && count < 512; ++count, *descriptor += 20) {
    unsigned char row[20];
    if (!program.ReadAt(*descriptor, row, sizeof(row))) break;
    const std::uint32_t name_rva = U32(row + 12);
    if (name_rva == 0) break;
    const auto name_at = file_offset(name_rva);
    if (!name_at) break;
    char name[128] = {};
    const auto got = program.ReadSome(*name_at, reinterpret_cast<unsigned char*>(name), sizeof(name) - 1);
    facts.imports.push_back(Lower(std::string(name, strnlen(name, got))));
  }
  // Delay-load descriptors (32 bytes each): the DLL's name at +4, an address relative to the image when
  // bit 0 of the attributes is set (the only form linkers have written since Visual C++ 7).
  auto delay = delay_imports && delay_rva ? file_offset(delay_rva) : std::nullopt;
  for (unsigned count = 0; delay && count < 512; ++count, *delay += 32) {
    unsigned char row[32];
    if (!program.ReadAt(*delay, row, sizeof(row))) break;
    const std::uint32_t name_rva = U32(row + 4);
    if (name_rva == 0) break;
    if (!(U32(row) & 1)) continue;
    const auto name_at = file_offset(name_rva);
    if (!name_at) continue;
    char name[128] = {};
    const auto got = program.ReadSome(*name_at, reinterpret_cast<unsigned char*>(name), sizeof(name) - 1);
    facts.delay_imports.push_back(Lower(std::string(name, strnlen(name, got))));
  }
  return facts;
}

// Graphics library names a program loads at run time (Unity, GameMaker and others call
// LoadLibrary instead of importing). ASCII and UTF-16, any case.
const std::array<std::pair<const char*, const char*>, 9>& Libraries() {
  static const std::array<std::pair<const char*, const char*>, 9> list = {{
      {"d3d8.dll", "d3d8"}, {"d3d9.dll", "d3d9"}, {"d3d10.dll", "d3d10"}, {"d3d10_1.dll", "d3d10"},
      {"d3d11.dll", "d3d11"}, {"dxgi.dll", "dxgi"}, {"d3d12.dll", "d3d12"}, {"opengl32.dll", "opengl"},
      {"vulkan-1.dll", "vulkan"}}};
  return list;
}

void AddImports(const PeFacts& facts, std::set<std::string>* apis) {
  for (const auto& name : facts.imports) {
    for (const auto& [library, api] : Libraries())
      if (name == library) apis->insert(api);
    if (name == "ddraw.dll") apis->insert("ddraw");
  }
}

// Engines known only by a name inside the program: SIL (FTL) reads SIL_WINDOWS_* settings.
// An XNA game names the framework among its .NET assembly references; Wine Mono runs it on FNA, which draws
// through SDL and OpenGL (Celeste).
constexpr std::pair<const char*, const char*> kEngineMarks[] = {{"sil_windows_use_", "sil"},
                                                                {"microsoft.xna.framework", "xna"}};

// An installer the game ships beside Unreal's Engine folder (VC_redist.x64.exe) is not the game.
constexpr const char* kInstallerWords[] = {"redist", "dxsetup", "uninst", "prereq"};

// Unreal Engine 5 names Nanite's console variables (as UTF-16, scanned in lower case); Unreal 4 has no Nanite.
constexpr char kUnreal5Mark[] = "r\0.\0n\0a\0n\0i\0t\0e\0";

void ScanNames(const std::string& path, std::set<std::string>* apis, std::string* engine = nullptr,
               bool* unreal5 = nullptr) {
  Program program(path);
  std::vector<std::string> needles;
  std::vector<const char*> found_as;
  for (const auto& [library, api] : Libraries()) {
    needles.emplace_back(library);
    found_as.push_back(api);
    std::string wide;
    for (const char* at = library; *at; ++at) wide += *at, wide += '\0';
    needles.push_back(wide);
    found_as.push_back(api);
  }
  const std::string_view unreal5_mark(kUnreal5Mark, sizeof(kUnreal5Mark) - 1);
  constexpr std::size_t kChunk = 1 << 20;
  constexpr std::size_t kOverlap = 32;
  std::vector<unsigned char> buffer(kChunk + kOverlap);  // heap: console worker stacks are small
  std::size_t kept = 0;
  for (std::uint64_t offset = 0; offset < kProbeScanBytes;) {
    const auto count = program.ReadSome(offset, buffer.data() + kept, kChunk);
    if (count == 0) break;
    const auto size = kept + count;
    for (std::size_t i = kept; i < size; ++i)
      if (buffer[i] >= 'A' && buffer[i] <= 'Z') buffer[i] = static_cast<unsigned char>(buffer[i] - 'A' + 'a');
    const std::string_view text(reinterpret_cast<const char*>(buffer.data()), size);
    for (std::size_t n = 0; n < needles.size(); ++n)
      if (text.find(needles[n]) != std::string_view::npos) apis->insert(found_as[n]);
    if (engine && engine->empty())
      for (const auto& [mark, name] : kEngineMarks)
        if (text.find(mark) != std::string_view::npos) *engine = name;
    if (unreal5 && !*unreal5 && text.find(unreal5_mark) != std::string_view::npos) *unreal5 = true;
    offset += count;
    kept = std::min(kOverlap, size);
    std::memmove(buffer.data(), buffer.data() + size - kept, kept);
  }
}

// The major version of the Unity that built a game, from the start of its data folder's first serialized file
// (globalgamemanagers from Unity 5, mainData before): the header holds the version as text ("5.6.2f1") at offset
// 20 from Unity 3.5 on. 0 when it can't be read.
int UnityMajorVersion(const fs::path& data) {
  for (const char* name : {"globalgamemanagers", "mainData"}) {
    Program file((data / name).string());
    unsigned char head[64] = {};
    const auto count = file.ReadSome(0, head, sizeof(head));
    if (count <= 20) continue;
    // digits '.' digits '.' digits, a release letter, digits and the string's end (5.6.2f1, 4.7.2p3).
    std::size_t at = 20;
    int major = 0;
    while (at < count && head[at] >= '0' && head[at] <= '9' && major < 10000) major = major * 10 + (head[at++] - '0');
    if (at == 20 || at >= count || head[at++] != '.') continue;
    const auto digits = [&] {
      const auto start = at;
      while (at < count && head[at] >= '0' && head[at] <= '9') ++at;
      return at > start;
    };
    if (!digits() || at >= count || head[at++] != '.' || !digits() || at >= count ||
        std::string_view("abfpx").find(static_cast<char>(head[at++])) == std::string_view::npos || !digits() ||
        at >= count || head[at] != '\0')
      continue;
    return major;
  }
  return 0;
}

// Entries of a folder, without following links: name as spelled -> is a directory.
std::vector<std::pair<std::string, bool>> Entries(const fs::path& folder) {
  std::vector<std::pair<std::string, bool>> out;
  std::error_code error;
  for (fs::directory_iterator it(folder, error), last; !error && it != last && out.size() < 4096; it.increment(error)) {
    const auto status = it->symlink_status(error);
    if (error) { error.clear(); continue; }
    if (fs::is_symlink(status)) continue;
    out.emplace_back(it->path().filename().string(), fs::is_directory(status));
  }
  return out;
}

std::optional<std::string> Find(const std::vector<std::pair<std::string, bool>>& entries, std::string_view wanted,
                                bool directory) {
  for (const auto& [name, is_directory] : entries)
    if (is_directory == directory && Lower(name) == wanted) return name;
  return std::nullopt;
}

// Unreal's own tools beside the game in <Project>/Binaries/Win64: never the game.
constexpr const char* kUnrealToolWords[] = {"crashreport", "cefsubprocess", "epicwebhelper", "-cmd.exe"};

bool UnrealTool(const std::string& lower) {
  return std::any_of(std::begin(kInstallerWords), std::end(kInstallerWords),
                     [&](const char* word) { return lower.find(word) != std::string::npos; }) ||
         std::any_of(std::begin(kUnrealToolWords), std::end(kUnrealToolWords),
                     [&](const char* word) { return lower.find(word) != std::string::npos; });
}

// The Unreal program under root: <Project>/Binaries/Win64/<Project>-Win64-Shipping.exe, searched a few levels
// deep. With shipping false, a game that ships it under another name (TS2Prototype/Binaries/Win64/WondersOfSodor.exe)
// is found instead, as the only program in a Binaries/Win64 folder or the one named like the root program
// (launcher_stem, lower case) or the project. Returns the path relative to root, or empty.
std::string FindUnrealProgram(const fs::path& root, const std::string& relative, int depth, bool shipping,
                              const std::string& launcher_stem, std::size_t launcher_folder) {
  if (depth > 4) return {};
  const auto entries = Entries(root / relative);
  const auto lower_relative = Lower(relative);
  const auto path_of = [&](const std::string& name) { return relative.empty() ? name : relative + "/" + name; };
  if (shipping) {
    for (const auto& [name, directory] : entries)
      if (!directory && EndsWith(Lower(name), "-shipping.exe") && lower_relative.find("binaries/win64") != std::string::npos)
        return path_of(name);
  } else if (EndsWith(lower_relative, "binaries/win64")) {
    // The project is the folder above Binaries, below the launcher's folder.
    std::string project;
    const fs::path parts(lower_relative.substr(std::min(launcher_folder, lower_relative.size())));
    for (auto at = parts.begin(); at != parts.end(); ++at)
      if (std::next(at) != parts.end() && *std::next(at) == "binaries") project = at->string();
    std::vector<std::string> programs;
    for (const auto& [name, directory] : entries) {
      const auto lower = Lower(name);
      if (!directory && EndsWith(lower, ".exe") && !UnrealTool(lower)) programs.push_back(name);
    }
    // Only in a project's folder: Unreal 3 keeps its own program in a Binaries/Win64 at the root.
    if (!project.empty() && programs.size() == 1) return path_of(programs.front());
    for (const auto& name : programs) {
      const auto stem = Lower(name.substr(0, name.size() - 4));
      if (!project.empty() && (stem == launcher_stem || stem == project)) return path_of(name);
    }
  }
  for (const auto& [name, directory] : entries) {
    if (!directory) continue;
    const auto lower = Lower(name);
    if (lower == "engine" || lower == "content" || lower == "saved" || lower == "plugins") continue;
    const auto found = FindUnrealProgram(root, path_of(name), depth + 1, shipping, launcher_stem, launcher_folder);
    if (!found.empty()) return found;
  }
  return {};
}

// A *-Shipping.exe anywhere first, then a program named otherwise.
std::string FindShipping(const fs::path& root, const std::string& relative, const std::string& launcher_stem) {
  const auto launcher_folder = relative.empty() ? 0 : relative.size() + 1;
  auto found = FindUnrealProgram(root, relative, 0, true, launcher_stem, launcher_folder);
  return found.empty() ? FindUnrealProgram(root, relative, 0, false, launcher_stem, launcher_folder) : found;
}

// An Unreal root program: <Game>.exe beside the Engine folder, not an installer the game ships there.
bool UnrealLauncher(const std::vector<std::pair<std::string, bool>>& here, const std::string& lower_file) {
  return !EndsWith(lower_file, "-shipping.exe") && Find(here, "engine", true) &&
         std::none_of(std::begin(kInstallerWords), std::end(kInstallerWords),
                      [&](const char* word) { return lower_file.find(word) != std::string::npos; });
}

std::string LowerStem(const std::string& lower_file) {
  return EndsWith(lower_file, ".exe") ? lower_file.substr(0, lower_file.size() - 4) : lower_file;
}

// Microsoft's MFC library, which Wine doesn't provide: mfc140.dll, mfc140u.dll, mfcm140.dll, mfcm140u.dll (and
// whatever later 140 builds add).
bool MfcLibrary(const std::string& lower) {
  return (lower.rfind("mfc140", 0) == 0 || lower.rfind("mfcm140", 0) == 0) && EndsWith(lower, ".dll");
}

bool MfcScanDone(const MfcImports& out) {
  return out.truncated || (!out.x86_program.empty() && !out.x64_program.empty());
}

void ScanMfc(const fs::path& root, const std::string& relative, int depth, MfcImports* out, std::size_t* seen) {
  if (out->folders >= kMfcScanFolders) {
    out->truncated = true;
    return;
  }
  ++out->folders;
  const auto entries = Entries(relative.empty() ? root : root / relative);
  for (const auto& [name, directory] : entries) {
    if (MfcScanDone(*out)) return;
    if (directory) continue;
    const auto lower = Lower(name);
    if (!EndsWith(lower, ".exe") && !EndsWith(lower, ".dll")) continue;
    if (*seen >= kMfcScanPrograms) {
      out->truncated = true;
      return;
    }
    ++*seen;
    const auto path = relative.empty() ? name : relative + "/" + name;
    const auto facts = ReadPe((root / path).string(), true);
    if (!facts.valid) continue;
    auto& program = facts.pe64 ? out->x64_program : out->x86_program;
    if (!program.empty()) continue;
    for (const auto* list : {&facts.imports, &facts.delay_imports})
      if (std::any_of(list->begin(), list->end(), MfcLibrary)) program = path;
  }
  if (depth >= kMfcScanDepth) return;
  for (const auto& [name, directory] : entries)
    if (directory && !MfcScanDone(*out)) ScanMfc(root, relative.empty() ? name : relative + "/" + name, depth + 1, out, seen);
}

}  // namespace

MfcImports FindMfcImports(const std::string& install_root) {
  MfcImports out;
  std::size_t seen = 0;
  ScanMfc(fs::path(install_root), std::string(), 0, &out, &seen);
  out.programs = seen;
  return out;
}

std::string UnrealGameProgram(const std::string& install_root, const std::string& executable) {
  const fs::path root(install_root);
  const auto slash = executable.rfind('/');
  const std::string folder = slash == std::string::npos ? std::string() : executable.substr(0, slash);
  const auto file = slash == std::string::npos ? executable : executable.substr(slash + 1);
  const auto lower_file = Lower(file);
  const auto here = Entries(root / folder);
  // As ProbeGame decides: a Unity program beside an Engine folder is Unity's.
  if (Find(here, "unityplayer.dll", false) || Find(here, LowerStem(lower_file) + "_data", true)) return {};
  if (!UnrealLauncher(here, lower_file)) return {};
  return FindShipping(root, folder, LowerStem(lower_file));
}

GameProbe ProbeGame(const std::string& install_root, const std::string& executable) {
  GameProbe probe;
  const fs::path root(install_root);
  const auto slash = executable.rfind('/');
  const std::string folder = slash == std::string::npos ? std::string() : executable.substr(0, slash);
  const std::string file = slash == std::string::npos ? executable : executable.substr(slash + 1);
  const auto lower_file = Lower(file);
  const std::string stem = EndsWith(lower_file, ".exe") ? file.substr(0, file.size() - 4) : file;
  const auto here = Entries(root / folder);
  const auto top = Entries(root);

  const auto program = ReadPe((root / executable).string());
  probe.dotnet = program.clr || std::count(program.imports.begin(), program.imports.end(), "mscoree.dll") > 0;
  probe.steam_stub = Lower(program.entry_section) == ".bind";
  if (probe.steam_stub) {
    const auto stub = InspectSteamStub((root / executable).string());
    probe.steam_stub_supported = stub.kind == SteamStubKind::kSupported;
    probe.steam_stub_detail = stub.detail;
  }
  probe.shared_section = program.unaligned_shared;
  AddImports(program, &probe.apis);
  for (const auto& [name, directory] : here)
    if (!directory && EndsWith(Lower(name), ".dll")) AddImports(ReadPe((root / folder / name).string()), &probe.apis);
  std::string marked_engine;
  ScanNames((root / executable).string(), &probe.apis, &marked_engine, &probe.unreal5);

  if (const auto unity = Find(here, "unityplayer.dll", false); unity || Find(here, Lower(stem) + "_data", true)) {
    probe.engine = "unity";
    probe.il2cpp = Find(here, "gameassembly.dll", false).has_value();
    probe.legacy_unity = !unity;
    if (probe.legacy_unity)
      if (const auto data = Find(here, Lower(stem) + "_data", true)) probe.unity_major = UnityMajorVersion(root / folder / *data);
    if (unity) ScanNames((root / folder / *unity).string(), &probe.apis);
  } else if (EndsWith(lower_file, "-shipping.exe") || UnrealLauncher(here, lower_file)) {
    probe.engine = "unreal";
    if (!EndsWith(lower_file, "-shipping.exe")) {
      probe.unreal_program = FindShipping(root, folder, LowerStem(lower_file));
      // The root program is only a launcher: the engine version is in the Shipping program.
      std::set<std::string> shipping_apis;
      if (!probe.unreal_program.empty())
        ScanNames((root / probe.unreal_program).string(), &shipping_apis, nullptr, &probe.unreal5);
    }
  } else if (const auto bin = Find(here, "bin", true); bin && Find(Entries(root / folder / *bin), "engine.dll", false)) {
    probe.engine = "source";
  } else if (Find(here, "data.win", false)) {
    probe.engine = "gamemaker";
  } else if (Find(here, Lower(stem) + ".pck", false)) {
    probe.engine = "godot";
  } else if (Find(here, "libhl.dll", false) || Find(here, "hlboot.dat", false)) {
    // HashLink (Heaps: Dead Cells) loads its graphics library at run time, from a .hdll next to the program.
    probe.engine = "hashlink";
    if (Find(here, "directx.hdll", false)) probe.apis.insert("d3d11");
    if (Find(here, "sdl.hdll", false)) probe.apis.insert("opengl");
  } else if (lower_file == "java.exe" || lower_file == "javaw.exe" ||
             (Find(here, "jre", true) && std::none_of(probe.apis.begin(), probe.apis.end(), [](const std::string& api) {
                return api.rfind("d3d", 0) == 0 || api == "dxgi";
              }))) {
    // Java (a bundled JRE, or a packr launcher beside one): LWJGL and libGDX load OpenGL at run time (Slay the Spire).
    // A program beside a JRE that names Direct3D itself is a native game with a Java side tool.
    probe.engine = "java";
  } else if (Find(here, "love.dll", false)) {
    probe.engine = "love";  // LÖVE draws with OpenGL through SDL, which it loads at run time
  } else if (Find(here, "nw.dll", false)) {
    probe.engine = "nwjs";  // NW.js (RPG Maker MV and MZ): Chromium with Node
  } else if (std::any_of(here.begin(), here.end(), [&](const auto& entry) {
               if (!entry.second) return false;
               const auto inside = Entries(root / folder / entry.first);
               return std::any_of(inside.begin(), inside.end(),
                                  [](const auto& file) { return !file.second && EndsWith(Lower(file.first), ".pk3"); });
             })) {
    probe.engine = "idtech3";  // Quake III Arena's engine and its heirs: data in .pk3 files one folder down
  } else {
    probe.engine = marked_engine;
  }
  if (probe.engine != "unreal") probe.unreal5 = false;  // the mark only means Unreal 5 in an Unreal game
  for (const auto* entries : {&here, &top})
    if (Find(*entries, "easyanticheat", true) || Find(*entries, "battleye", true)) probe.anticheat = true;
  return probe;
}

namespace {
// A Unity before 2017.2 that starts on Direct3D 9: Unity 4 and earlier, or a version that can't be read.
bool LegacyDirect3D9(const GameProbe& probe) { return probe.legacy_unity && probe.unity_major < 5; }
}  // namespace

GameDefaults DefaultsFor(const GameProbe& probe, const RuntimeAbilities& runtime) {
  GameDefaults out;
  const auto has = [&](const char* api) { return probe.apis.count(api) != 0; };
  // Direct3D 12 also loads dxgi, so dxgi alone doesn't mean an older Direct3D the game can fall back to.
  const bool older_direct3d = has("d3d8") || has("d3d9") || has("d3d10") || has("d3d11");
  const bool direct3d = older_direct3d || has("dxgi");
  // Soft refusals: the detection is broad (a .NET launcher in front of a native game), so the
  // engine defaults below are still worked out for a player who tries anyway.
  std::string soft;
  if (probe.shared_section && !runtime.shared_sections)
    soft = "This game's program needs a shared memory section the PS5 can't map yet.";
  else if (probe.dotnet && !runtime.wine_mono)
    soft = "This game needs .NET, which Playden's runtime doesn't include yet.";
  if (probe.steam_stub && !probe.steam_stub_supported) {
    out.refusal = "This game's program has Steam DRM in a form Playden can't unpack yet.";
  } else if (probe.engine == "nwjs") {
    // Chromium starts helper processes, and in one process it reserves 64 GB of address space.
    out.refusal = "This game runs on NW.js (a built-in web browser), which Playden can't start on the PS5 yet.";
  } else if (probe.engine == "unity") {
    // Unity also names d3d12 and Vulkan; D3D11 is the path DXVK covers. Unity 4 and earlier (Hexcells,
    // Thomas Was Alone) draw with Direct3D 9, where D3D11 was a player option. Unity 5 builds may leave
    // Direct3D 9 out ("Forced GfxDevice 'Direct3D 9' was not built", I Can't Believe It's Not Gambling, 5.6) and
    // draw on D3D11 (140, 5.3); a Unity before 2017.2 whose version can't be read keeps Direct3D 9.
    out.graphics = "dxvk";
    out.arguments = LegacyDirect3D9(probe) ? "-force-d3d9 {resolution}" : "-force-d3d11 {resolution}";
    out.resolution_arguments = "-screen-width {w} -screen-height {h}";
  } else if (probe.engine == "unreal") {
    // Unreal 4 draws on DXVK. Unreal 5 projects are often Direct3D 12 only ("Unable to launch with RHI 'DirectX
    // 11'"), so they get vkd3d-proton, which turns -dx11 into -dx12 (game_settings.cpp).
    out.graphics = "dxvk";
    out.arguments = "-dx11";
    if (probe.unreal5 && runtime.vkd3d_proton) out.d3d12 = "vkd3d-proton";
  } else if (probe.engine == "source") {
    out.graphics = "dxvk";
    out.arguments = "{resolution}";
    out.resolution_arguments = "-w {w} -h {h}";
  } else if (probe.engine == "hashlink") {
    // Dead Cells: Automatic graphics stopped with "Failed to initialize DirectX driver"; DXVK draws it.
    out.graphics = has("d3d11") ? "dxvk" : "opengl";
  } else if (probe.engine == "xna") {
    // Automatic graphics leave FNA without a GL pixel format ("No matching GL pixel format available").
    out.graphics = "opengl";
  } else if (probe.engine == "love" || probe.engine == "sil" || probe.engine == "java") {
    // SIL (FTL) draws with OpenGL though its program also names Direct3D 11; under DXVK it
    // quits right after setting up video.
    out.graphics = "opengl";
  } else if (probe.engine == "idtech3") {
    // id Tech 3 copies the OpenGL extension list into a fixed buffer; Mesa's is far longer than any
    // of 2001 and overflows it (Return to Castle Wolfenstein faults reading "rev " as an address).
    out.graphics = "opengl";
    out.environment = "MESA_EXTENSION_MAX_YEAR=2001";
  } else if (probe.engine == "godot") {
    out.graphics = "opengl";
    out.arguments = "--rendering-driver opengl3";
  } else if (has("d3d12") && runtime.vkd3d_proton) {
    // vkd3d-proton presents through DXVK's dxgi, so the game gets DXVK as well. A game that also imports an
    // older Direct3D still asks for Direct3D 12 first (Mina the Hollower with d3d9, Ghosts 'n Goblins with d3d11)
    // and falls back to software rendering or stops on Wine's own; one that picks the older one still gets DXVK.
    out.graphics = "dxvk";
    out.d3d12 = "vkd3d-proton";
  } else if (direct3d || probe.engine == "gamemaker") {
    out.graphics = "dxvk";
  } else if (has("opengl")) {
    out.graphics = "opengl";
  } else if (has("d3d12")) {
    out.refusal = "This game needs Direct3D 12, which this runtime kit can't draw yet.";
  } else if (probe.engine.empty() && probe.apis.empty()) {
    // Nothing names a graphics library: it is loaded at run time (Alan Wake's d3d9). Automatic means Wine's own
    // Direct3D, which can't get an OpenGL context on the console, so DXVK covers the likeliest case.
    out.graphics = "dxvk";
  }
  if (out.refusal.empty() && !soft.empty()) {
    out.refusal = std::move(soft);
    out.refusal_soft = true;
  }
  std::string facts = probe.engine;
  if (probe.unreal5) facts += "5";
  if (probe.il2cpp) facts += facts.empty() ? "il2cpp" : "-il2cpp";
  if (LegacyDirect3D9(probe)) facts += "-legacy";
  else if (probe.legacy_unity) facts += "-legacy5";
  for (const auto& api : probe.apis) facts += (facts.empty() ? "" : ",") + api;
  // An Unreal 5 game names both renderers; this says which one it starts on.
  if (probe.unreal5) facts += out.d3d12.empty() ? ",dx11" : ",dx12";
  if (probe.anticheat) facts += (facts.empty() ? "" : ",") + std::string("anticheat");
  if (probe.shared_section) facts += (facts.empty() ? "" : ",") + std::string("shared-section");
  out.summary = (facts.empty() ? "unknown" : facts) + " graphics=" + out.graphics;
  return out;
}

std::string DetectedLabel(const std::string& summary) {
  const auto space = summary.find(" graphics=");
  if (space == std::string::npos) return {};
  const auto graphics = summary.substr(space + 10);
  std::set<std::string> facts;
  for (std::size_t start = 0; start < space;) {
    const auto end = std::min(summary.find(',', start), space);
    facts.insert(summary.substr(start, end - start));
    start = end + 1;
  }
  // Nothing detected: the graphics value is a default, not something the game's files said.
  if (facts.count("unknown")) return {};
  std::string engine;
  for (const auto& [id, name] : std::initializer_list<std::pair<const char*, const char*>>{
           {"unity", "Unity"}, {"unity-il2cpp", "Unity"}, {"unity-legacy", "Unity"}, {"unity-legacy5", "Unity"},
           {"unreal", "Unreal Engine"}, {"unreal5", "Unreal Engine 5"}, {"source", "Source"},
           {"gamemaker", "GameMaker"}, {"godot", "Godot"}, {"love", "L\xC3\x96VE"}, {"nwjs", "NW.js"}, {"sil", "SIL"},
           {"idtech3", "id Tech 3"}, {"hashlink", "HashLink"}, {"xna", "XNA"}, {"java", "Java"}})
    if (facts.count(id)) engine = name;
  std::string api;
  if (graphics == "opengl") api = "OpenGL";
  else if (graphics == "dxvk" && engine == "Unreal Engine 5")
    api = facts.count("dx12") ? "Direct3D 12" : "Direct3D 11";
  else if (graphics == "dxvk" && facts.count("d3d12") && engine != "Unity" && engine != "Unreal Engine")
    // Direct3D 12 also loads dxgi; a game that names both 11 and 12 can draw with either.
    api = facts.count("d3d11") ? "Direct3D 11 and 12" : "Direct3D 12";
  else if (graphics == "dxvk")
    api = facts.count("unity-legacy") ? "Direct3D 9"
              : facts.count("d3d11") || facts.count("dxgi") || engine == "Unity" || engine == "Unreal Engine" ||
                  engine == "GameMaker"
              ? "Direct3D 11"
              : facts.count("d3d10") ? "Direct3D 10" : facts.count("d3d9") ? "Direct3D 9"
              : facts.count("d3d8") ? "Direct3D 8" : "Direct3D";
  // DXVK without an engine or a Direct3D library named (only anticheat or shared-section facts) is the default too.
  if (engine.empty()) return api == "Direct3D" ? std::string() : api;
  return api.empty() ? engine : engine + " · " + api;
}

}  // namespace playden::runtime
