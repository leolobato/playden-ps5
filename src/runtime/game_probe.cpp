#include "runtime/game_probe.h"

#include "runtime/prospero_profile.h"
#include "runtime/steam_stub.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdio>
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
  std::uint32_t raw_size = 0;  // bytes the file holds for it (SizeOfRawData)
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
  bool watcom = false;               // linked by Watcom (its BEGTEXT or DGROUP section, or its banner at the entry)
  std::uint64_t resource_offset = 0;  // file offset of the resource directory, 0 when there is none
  std::uint32_t resource_size = 0;
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
    section.raw_size = U32(row + 16);
    section.flags = U32(row + 36);
    const auto mapped = (U32(row + 8) + 0xfffu) & ~0xfffu;
    if ((section.flags & kSharedWritable) == kSharedWritable &&
        (section.address % kConsolePage != 0 || mapped % kConsolePage != 0))
      facts.unaligned_shared = true;
    if (section.name == "BEGTEXT" || section.name == "DGROUP") facts.watcom = true;
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
  // Watcom's startup jumps over its banner, so a program linked with other section names (NFS3's Modern Patch has
  // .code and .data) still carries "WATCOM C/C++32 Run-Time system" a few bytes after its entry point.
  // Only the entry section's own file bytes count: past them is another section, or memory the file doesn't hold.
  for (const auto& section : list) {
    if (facts.watcom || !entry || entry < section.address || entry - section.address >= section.raw_size) continue;
    static constexpr std::string_view kBanner = "WATCOM C/C++32 Run-Time system";
    unsigned char near_entry[64] = {};
    const auto left = section.raw_size - (entry - section.address);
    const auto count = program.ReadSome(static_cast<std::uint64_t>(section.raw) + (entry - section.address), near_entry,
                                        std::min<std::size_t>(sizeof(near_entry), left));
    const std::string_view window(reinterpret_cast<const char*>(near_entry), count);
    facts.watcom = window.find(kBanner) != std::string_view::npos;
  }
  facts.valid = true;
  if (const auto resource_rva = directory(2))
    if (const auto at = file_offset(resource_rva)) {
      facts.resource_offset = *at;
      facts.resource_size = directories + 2 * 8 + 8 <= optional.size() ? U32(optional.data() + directories + 2 * 8 + 4) : 0;
    }

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

// A library's version from its version resource: the "FileVersion" string, else the fixed file version.
// Empty when it carries neither (or only 0.0.0.0). Only letters, digits, '.' and '-' are kept.
std::string ResourceVersion(const std::string& path) {
  const auto pe = ReadPe(path);
  if (!pe.valid || pe.resource_size == 0) return {};
  std::vector<unsigned char> data(std::min(pe.resource_size, kVersionScanBytes));
  data.resize(Program(path).ReadSome(pe.resource_offset, data.data(), data.size()));
  const auto clean = [](std::string text) {
    std::string out;
    for (char value : text) {
      if (!std::isalnum(static_cast<unsigned char>(value)) && value != '.' && value != '-') break;
      if (out.size() == 32) break;
      out += value;
    }
    return out;
  };
  // A String entry: wLength, wValueLength (characters, with the terminating null), wType, the key, padding to
  // 4 bytes from the entry's start, then the value.
  static const std::string key = [] {
    std::string wide;
    for (char value : std::string_view("FileVersion")) wide += value, wide += '\0';
    return wide + std::string(2, '\0');
  }();
  for (auto at = std::search(data.begin(), data.end(), key.begin(), key.end()); at != data.end();
       at = std::search(at + 2, data.end(), key.begin(), key.end())) {
    const auto start = static_cast<std::size_t>(at - data.begin());
    if (start < 6) continue;
    const std::size_t characters = U16(data.data() + start - 4);
    std::size_t value = start + key.size();
    value += (4 - (value - (start - 6)) % 4) % 4;
    if (characters < 2 || value + characters * 2 > data.size()) continue;
    std::string text;
    for (std::size_t i = 0; i + 1 < characters; ++i)
      text += data[value + i * 2 + 1] == 0 ? static_cast<char>(data[value + i * 2]) : '?';
    if (auto version = clean(text); !version.empty()) return version;
  }
  // VS_FIXEDFILEINFO: the signature, the structure version, then the file version as two 32-bit halves.
  static const unsigned char signature[] = {0xbd, 0x04, 0xef, 0xfe};
  const auto fixed = std::search(data.begin(), data.end(), std::begin(signature), std::end(signature));
  if (fixed == data.end() || data.end() - fixed < 16) return {};
  const auto major = U32(&*fixed + 8), minor = U32(&*fixed + 12);
  if (major == 0 && minor == 0) return {};
  auto version = std::to_string(major >> 16) + '.' + std::to_string(major & 0xffff) + '.' + std::to_string(minor >> 16);
  if (minor & 0xffff) version += '.' + std::to_string(minor & 0xffff);
  return version;
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
// An XNA game names XNA's game or graphics parts: the Microsoft.Xna.Framework.Game assembly, the
// Microsoft.Xna.Framework.Graphics namespace or GraphicsDeviceManager (FEZ, built on FNA, names the last two). Wine
// Mono runs it on FNA, which draws through SDL and OpenGL (Celeste). The framework's name alone is not a game:
// Borderlands 2's 2K launcher reads the pad through Microsoft.Xna.Framework.Input and draws nothing with XNA.
constexpr std::pair<const char*, const char*> kEngineMarks[] = {{"sil_windows_use_", "sil"},
                                                                {"microsoft.xna.framework.game", "xna"},
                                                                {"microsoft.xna.framework.graphics", "xna"},
                                                                {"graphicsdevicemanager", "xna"}};

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

// The Unity player beside a program that isn't one: a player needs its <stem>_Data folder, so a program beside
// UnityPlayer.dll without one is a bootstrap or launcher (SUPERHOT.exe in front of SH.exe). It starts the player as a
// child process, which the console cannot do. The one program here with a <stem>_Data folder, relative to root as
// spelled on disk; empty when the program is a player, when no UnityPlayer.dll is here, or when there isn't exactly one.
// A candidate counts only when it loads UnityPlayer.dll itself, so an unrelated tool with a <name>_Data folder doesn't.
std::string UnityPlayerBeside(const fs::path& directory_path, const std::vector<std::pair<std::string, bool>>& here,
                              const std::string& folder, const std::string& lower_file) {
  if (!EndsWith(lower_file, ".exe") || Find(here, LowerStem(lower_file) + "_data", true) ||
      !Find(here, "unityplayer.dll", false))
    return {};
  std::vector<std::string> players;
  for (const auto& [name, directory] : here) {
    const auto lower = Lower(name);
    if (directory || !EndsWith(lower, ".exe") || !Find(here, LowerStem(lower) + "_data", true)) continue;
    const auto facts = ReadPe((directory_path / name).string(), true);
    const auto loads = [](const std::vector<std::string>& list) {
      return std::find(list.begin(), list.end(), "unityplayer.dll") != list.end();
    };
    if (facts.valid && (loads(facts.imports) || loads(facts.delay_imports))) players.push_back(name);
  }
  if (players.size() != 1) return {};
  return folder.empty() ? players.front() : folder + "/" + players.front();
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

std::string UnityGameProgram(const std::string& install_root, const std::string& executable) {
  const auto slash = executable.rfind('/');
  const std::string folder = slash == std::string::npos ? std::string() : executable.substr(0, slash);
  const auto file = slash == std::string::npos ? executable : executable.substr(slash + 1);
  const auto directory = fs::path(install_root) / folder;
  return UnityPlayerBeside(directory, Entries(directory), folder, Lower(file));
}

std::string GameProgramInstead(const std::string& install_root, const std::string& executable) {
  auto program = UnrealGameProgram(install_root, executable);
  return program.empty() ? UnityGameProgram(install_root, executable) : program;
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

bool ProgramUsesPhysx(const std::string& install_root, const std::string& executable) {
  const auto facts = ReadPe((fs::path(install_root) / executable).string(), true);
  if (!facts.valid) return false;
  for (const auto* list : {&facts.imports, &facts.delay_imports})
    if (std::find(list->begin(), list->end(), "physxloader.dll") != list->end()) return true;
  return false;
}

bool ProgramDrawsWithOpenGLOnly(const std::string& install_root, const std::string& executable) {
  const auto facts = ReadPe((fs::path(install_root) / executable).string());
  if (!facts.valid) return false;
  std::set<std::string> apis;
  AddImports(facts, &apis);
  return apis == std::set<std::string>{"opengl"};
}

namespace {

// Whether a program itself looks like a game: what it imports or names, not the libraries beside it (a crash reporter
// sits next to the game's DLLs). A game may load its graphics at run time, so the names inside it count too.
bool ProgramLooksLikeGame(const fs::path& path, const PeFacts& facts) {
  std::set<std::string> apis;
  AddImports(facts, &apis);
  for (const auto& name : facts.delay_imports) {
    for (const auto& [library, api] : Libraries())
      if (name == library) apis.insert(api);
    if (name == "ddraw.dll") apis.insert("ddraw");
  }
  std::string engine;
  ScanNames(path.string(), &apis, &engine);
  if (!apis.empty() || !engine.empty()) return true;
  const auto has = [](const std::vector<std::string>& list, const char* name) {
    return std::find(list.begin(), list.end(), name) != list.end();
  };
  if (has(facts.imports, "unityplayer.dll") || has(facts.delay_imports, "unityplayer.dll")) return true;
  const auto stem = LowerStem(Lower(path.filename().string()));
  return stem == "java" || stem == "javaw" || EndsWith(stem, "-shipping");
}

}  // namespace

// The folder names the title linked in the bottle's steamapps\common (prospero-win src/pw_steam_view.h): what Wine
// named the game by. Empty without a readable table.
static std::vector<std::string> SteamLibraryLinks(const std::string& prefix_dir) {
  std::vector<std::string> names;
  const auto table = fs::path(prefix_dir) / "drive_c/Program Files (x86)/Steam/steamapps/common/.pw-symlinks";
  std::error_code error;
  if (!fs::is_regular_file(fs::symlink_status(table, error))) return names;
  std::FILE* file = std::fopen(table.c_str(), "rb");
  if (!file) return names;
  char buffer[4096];
  const std::string text(buffer, std::fread(buffer, 1, sizeof(buffer), file));
  std::fclose(file);
  std::size_t at = 0;
  while (at < text.size()) {
    const auto end = std::min(text.find('\n', at), text.size());
    const auto line = text.substr(at, end - at);
    const auto tab = line.find('\t');
    if (tab != std::string::npos && IsProsperoSteamInstallDir(line.substr(0, tab))) names.push_back(line.substr(0, tab));
    at = end + 1;
  }
  return names;
}

std::string TakeRefusedGameProgram(const std::string& prefix_dir, const std::string& console_root,
                                   const std::string& host_root) {
  const auto note = fs::path(prefix_dir) / ".pw-exec-refused";
  std::error_code error;
  if (!fs::is_regular_file(fs::symlink_status(note, error))) return {};
  std::FILE* file = std::fopen(note.c_str(), "rb");
  if (!file) return {};  // stays, for a start that can read it
  char buffer[2048];
  std::string text(buffer, std::fread(buffer, 1, sizeof(buffer), file));
  const bool read_failed = std::ferror(file) != 0;
  std::fclose(file);
  if (read_failed) return {};
  // The program named, in the game's folder; empty for anything else.
  const auto named = [&]() -> std::string {
    static constexpr std::string_view kHeader = "pw-exec-refused 1\nimage=";
    if (text.rfind(kHeader, 0) != 0 || text.back() != '\n') return {};
    auto image = text.substr(kHeader.size(), text.size() - kHeader.size() - 1);
    if (image.find('\n') != std::string::npos) return {};
    // A title with [steam] shows the game to Wine in steamapps\common: the program is named there.
    auto root = console_root;
    while (!root.empty() && root.back() == '/') root.pop_back();
    for (const auto& name : SteamLibraryLinks(prefix_dir)) {
      const auto view = Lower(std::string(kProsperoSteamCommon) + "\\" + name + "\\");
      if (image.size() > view.size() && Lower(image.substr(0, view.size())) == view) {
        image = "Z:" + root + "/" + image.substr(view.size());
        break;
      }
    }
    std::replace(image.begin(), image.end(), '\\', '/');
    if (image.size() < 3 || Lower(image.substr(0, 3)) != "z:/") return {};
    image.erase(0, 2);  // Z:
    if (root.empty() || image.size() <= root.size() + 1 || image[root.size()] != '/' ||
        Lower(image.substr(0, root.size())) != Lower(root))
      return {};
    auto relative = image.substr(root.size() + 1);
    if (relative.empty() || relative.front() == '/' || relative.find("..") != std::string::npos) return {};
    if (!fs::is_regular_file(fs::symlink_status(fs::path(host_root) / relative, error))) return {};
    return relative;
  }();
  std::string program;
  if (!named.empty()) {
    const auto path = fs::path(host_root) / named;
    const auto facts = ReadPe(path.string(), true);
    if (!facts.valid) return {};  // stays: the drive may answer at a later start
    if (ProgramLooksLikeGame(path, facts)) program = named;
  }
  // One that can't be removed would say the same at every start, without another failed run.
  if (!fs::remove(note, error) || error) return {};
  return program;
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
  probe.x64 = program.pe64;
  probe.watcom = program.watcom && !program.pe64;
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
    probe.unity_program = UnityPlayerBeside(root / folder, here, folder, lower_file);
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
  } else if (const auto nw = Find(here, "nw.dll", false)) {
    probe.engine = "nwjs";  // NW.js (RPG Maker MV and MZ): Chromium with Node
    probe.engine_version = ResourceVersion((root / folder / *nw).string());
  } else if (Find(here, "nw.pak", false)) {
    probe.engine = "nwjs";  // node-webkit before NW.js 0.13 (CrossCode's demo): Chromium built into the program
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

bool WatcomProgram(const std::string& path) {
  const auto pe = ReadPe(path);
  return pe.valid && pe.watcom && !pe.pe64;
}

int DefaultsRevision(std::string_view engine) {
  engine = engine.substr(0, engine.find_first_of(", "));
  if (engine == "nwjs") return 2;  // 1: low-end device mode and a larger V8 heap. 2: low-end device mode gone
  if (engine == "xna") return 1;   // 1: XNA's name alone no longer makes a .NET launcher the game (Borderlands 2)
  return 0;
}

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
    soft = "This game needs .NET, which Playden's runtime kit doesn't include yet.";
  if (probe.steam_stub && !probe.steam_stub_supported) {
    out.refusal = "This game's program has Steam DRM in a form Playden can't unpack yet.";
  } else if (probe.engine == "nwjs") {
    // Chromium starts helper processes, which a PS5 title can't, so everything runs in the game's
    // process. There Chromium reserves 64 GB of address space, which the kit's high area may now hold.
    // ANGLE draws WebGL with Direct3D 11. 64-bit NW.js plays (Look Outside, NW.js 0.94), and so does a recent 32-bit
    // one (Alabaster Dawn's demo, NW.js 0.86). Older 32-bit builds don't play yet (OMORI, from 2017, opens a black
    // window; CrossCode's demo, node-webkit 0.8, crashes at start), but they start like the others. V8 runs out of heap
    // at about 124 MiB without the larger limit in a 32-bit game. A game loading hundreds of files at once needs the
    // kit's direct-memory sections (Wine 0976); --enable-low-end-device-mode in Extra arguments also avoids those failed
    // loads, if a game has to turn them off.
    out.graphics = "dxvk";
    out.arguments = "--single-process --in-process-gpu --no-sandbox --disable-gpu-sandbox --disable-breakpad "
                    "--js-flags=--max-old-space-size=1024";
    // On an older runtime the game hangs before its first frame in the default Xbox controller mode.
    if (!runtime.driver_threads)
      out.refusal = "This game runs on NW.js (a built-in web browser), which this runtime kit can't start yet.";
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
  // Watcom's runtime saves and reloads FS (push fs / pop fs around its allocator). On the native WoW64 CPU the
  // reloaded selector's base is not the thread's TEB, and the next FS access faults (Fallout, at its start); the
  // translator keeps FS on the TEB.
  if (probe.watcom) out.cpu = "translator";
  if (out.refusal.empty() && !soft.empty()) {
    out.refusal = std::move(soft);
    out.refusal_soft = true;
  }
  std::string facts = probe.engine;
  if (probe.unreal5) facts += "5";
  if (probe.il2cpp) facts += facts.empty() ? "il2cpp" : "-il2cpp";
  if (LegacyDirect3D9(probe)) facts += "-legacy";
  else if (probe.legacy_unity) facts += "-legacy5";
  if (!probe.engine_version.empty()) facts += "," + probe.engine + "-" + probe.engine_version;
  for (const auto& api : probe.apis) facts += (facts.empty() ? "" : ",") + api;
  // An Unreal 5 game names both renderers; this says which one it starts on.
  if (probe.unreal5) facts += out.d3d12.empty() ? ",dx11" : ",dx12";
  if (probe.anticheat) facts += (facts.empty() ? "" : ",") + std::string("anticheat");
  if (probe.shared_section) facts += (facts.empty() ? "" : ",") + std::string("shared-section");
  if (probe.watcom) facts += (facts.empty() ? "" : ",") + std::string("watcom");
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
  for (const auto& fact : facts)
    if (engine == "NW.js" && fact.rfind("nwjs-", 0) == 0) engine += " " + fact.substr(5);
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
