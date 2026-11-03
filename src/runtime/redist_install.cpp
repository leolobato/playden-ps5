#include "runtime/redist_install.h"

#include "platform/managed_directory.h"
#include "runtime/artifact_hash.h"
#include "runtime/burn_bundle.h"
#include "runtime/game_settings.h"
#include "runtime/msi_package.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <sys/stat.h>
#include <unistd.h>

namespace playden::runtime {
namespace {

using platform::DirectoryPolicy;
using platform::ManagedDirectory;

constexpr std::size_t kMaxPackageBytes = 256u * 1024u * 1024u;
constexpr std::size_t kMaxScripts = 32;
constexpr int kMaxDepth = 4;
constexpr std::size_t kMaxPhysxPackages = 8;
constexpr const char* kStateFile = ".playden-redist";
constexpr const char* kMfcScanFile = ".playden-mfc";
constexpr const char* kEnvironmentKey = "System\\CurrentControlSet\\Control\\Session Manager\\Environment";

std::string Lower(std::string_view text) {
  std::string out(text);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char value) { return static_cast<char>(value >= 'A' && value <= 'Z' ? value + 32 : value); });
  return out;
}

bool ReadFile(const std::filesystem::path& path, std::size_t maximum, std::string* text, off_t offset = 0) {
  text->clear();
  const int file = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (file < 0) return false;
  if (offset && ::lseek(file, offset, SEEK_SET) != offset) {
    ::close(file);
    return false;
  }
  // The whole file at once: growing the string a piece at a time briefly holds two copies.
  struct stat info {};
  std::size_t expected = 0;
  if (::fstat(file, &info) == 0 && info.st_size > offset) {
    if (static_cast<std::uint64_t>(info.st_size - offset) > maximum) {  // too large: refused before any allocation
      ::close(file);
      return false;
    }
    expected = static_cast<std::size_t>(info.st_size - offset);
    text->reserve(expected + 1);
  }
  bool ok = true;
  // Large packages: read straight into the string, not through a stack buffer (small worker stacks).
  for (;;) {
    const auto size = text->size();
    // Within the reserved size (one byte more, to see the end), so the string never grows past it.
    std::size_t chunk = 1u << 20;
    if (expected && size <= expected) chunk = std::min(chunk, expected + 1 - size);
    text->resize(size + chunk);
    const auto count = ::read(file, &(*text)[size], chunk);
    if (count < 0 && errno == EINTR) {
      text->resize(size);
      continue;
    }
    if (count < 0) { ok = false; text->resize(size); break; }
    text->resize(size + static_cast<std::size_t>(count));
    if (count == 0) break;
    if (text->size() > maximum) { ok = false; break; }
  }
  ::close(file);
  return ok;
}

// Folders where Steam games keep their redistributables, looked into below the game's top level.
bool RedistFolder(const std::string& name) {
  static const std::set<std::string> kNames = {
      "_commonredist", "commonredist", "redist", "redists", "_redist", "redistributables", "redistributable",
      "support", "__installer", "_installer", "installer", "installers", "prerequisites", "prereqs", "physx",
      "directx", "vcredist", "dotnet"};
  return kNames.count(Lower(name)) != 0;
}

bool ScriptName(const std::string& name) {
  const auto lower = Lower(name);
  return lower.rfind("installscript", 0) == 0 && lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".vdf") == 0;
}

void FindScripts(const std::filesystem::path& folder, const std::string& relative, int depth,
                 std::vector<std::string>* scripts) {
  std::error_code error;
  std::vector<std::pair<std::string, bool>> children;
  for (std::filesystem::directory_iterator it(folder, error), end; !error && it != end; it.increment(error)) {
    std::error_code type_error;
    if (it->is_symlink(type_error)) continue;
    children.emplace_back(it->path().filename().string(), it->is_directory(type_error));
  }
  std::sort(children.begin(), children.end());
  for (const auto& [name, directory] : children) {
    if (scripts->size() >= kMaxScripts) return;
    const auto path = relative.empty() ? name : relative + "/" + name;
    if (!directory && ScriptName(name) && depth > 0) scripts->push_back(path);
    else if (directory && depth < kMaxDepth && (depth > 0 || RedistFolder(name)))
      FindScripts(folder / name, path, depth + 1, scripts);
  }
}

bool PhysxPackageName(const std::string& name) {
  const auto lower = Lower(name);
  if (lower.rfind("physx", 0) != 0 || lower.size() <= 4) return false;
  const auto extension = lower.substr(lower.size() - 4);
  return extension == ".msi" || extension == ".exe";
}

// PhysX's installers in the game's redistributable folders, top-level ones or ones a level down (a game whose
// files sit in one folder of what was imported), by name.
void FindPhysxPackages(const std::filesystem::path& folder, const std::string& relative, int depth, bool in_redist,
                       std::vector<std::string>* packages) {
  std::error_code error;
  std::vector<std::pair<std::string, bool>> children;
  for (std::filesystem::directory_iterator it(folder, error), end; !error && it != end; it.increment(error)) {
    std::error_code type_error;
    if (it->is_symlink(type_error)) continue;
    children.emplace_back(it->path().filename().string(), it->is_directory(type_error));
  }
  std::sort(children.begin(), children.end());
  for (const auto& [name, directory] : children) {
    if (packages->size() >= kMaxPhysxPackages) return;
    const auto path = relative.empty() ? name : relative + "/" + name;
    if (!directory && in_redist && PhysxPackageName(name)) packages->push_back(path);
    else if (directory && depth < kMaxDepth && (in_redist || depth == 0 || RedistFolder(name)))
      FindPhysxPackages(folder / name, path, depth + 1, in_redist || RedistFolder(name), packages);
  }
}

// Splits a command line into arguments (double quotes group, as Windows does for msiexec).
std::vector<std::string> Arguments(std::string_view command) {
  std::vector<std::string> out;
  std::string current;
  bool quoted = false, any = false;
  for (const char value : command) {
    if (value == '"') {
      quoted = !quoted;
      any = true;
    } else if ((value == ' ' || value == '\t') && !quoted) {
      if (any) out.push_back(current);
      current.clear();
      any = false;
    } else {
      current += value;
      any = true;
    }
  }
  if (any) out.push_back(current);
  return out;
}

std::string FileName(std::string path) {
  std::replace(path.begin(), path.end(), '/', '\\');
  const auto slash = path.rfind('\\');
  return Lower(slash == std::string::npos ? path : path.substr(slash + 1));
}

// A Windows path inside the game folder, relative to it with '/' separators.
bool InsideGame(const std::string& windows_path, const std::string& install_dir, std::string* relative) {
  auto base = install_dir;
  while (!base.empty() && base.back() == '\\') base.pop_back();
  if (windows_path.size() <= base.size() + 1 || Lower(windows_path.substr(0, base.size())) != Lower(base) ||
      windows_path[base.size()] != '\\')
    return false;
  auto rest = windows_path.substr(base.size() + 1);
  std::replace(rest.begin(), rest.end(), '\\', '/');
  // Strictly below the game folder: no root, empty, "." or ".." part, and no drive.
  if (rest.empty() || rest.front() == '/' || rest.back() == '/' || rest.find(':') != std::string::npos) return false;
  for (std::size_t at = 0; at <= rest.size();) {
    auto end = rest.find('/', at);
    if (end == std::string::npos) end = rest.size();
    const auto part = rest.substr(at, end - at);
    if (part.empty() || part == "." || part == "..") return false;
    at = end + 1;
  }
  *relative = rest;
  return true;
}

// Opens (creating when asked) a folder below root, matching existing names without regard to
// case as Windows does, so a package's "Program Files (x86)" lands in the bottle's own.
std::optional<ManagedDirectory> OpenPath(const std::filesystem::path& root, const std::string& relative, bool create) {
  auto directory = ManagedDirectory::Open(root, DirectoryPolicy::kApprovedTree);
  if (!directory) return std::nullopt;
  for (std::size_t start = 0; start < relative.size();) {
    const auto slash = relative.find('/', start);
    auto name = relative.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
    start = slash == std::string::npos ? relative.size() : slash + 1;
    if (name.empty()) continue;
    std::error_code error;
    for (std::filesystem::directory_iterator it(directory->path(), error), end; !error && it != end; it.increment(error)) {
      const auto existing = it->path().filename().string();
      if (Lower(existing) == Lower(name)) {
        name = existing;
        break;
      }
    }
    auto next = directory->OpenChild(name, DirectoryPolicy::kApprovedTree, create);
    if (!next) return std::nullopt;
    directory = std::move(next);
  }
  return directory;
}

bool WriteFileAt(const ManagedDirectory& directory, const std::string& name, std::string_view data) {
  struct stat existing {};
  std::string target = name;
  if (!directory.InspectChild(name, &existing)) {
    if (errno != ENOENT) return false;
    // Reuse a differently cased file of the same name rather than adding a second one.
    std::error_code error;
    for (std::filesystem::directory_iterator it(directory.path(), error), end; !error && it != end; it.increment(error))
      if (Lower(it->path().filename().string()) == Lower(name)) target = it->path().filename().string();
  } else if (!S_ISREG(existing.st_mode)) {
    return false;
  }
  const auto temporary = target + ".playden-tmp-" + std::to_string(static_cast<long long>(::getpid())) + "-" +
                         std::to_string(static_cast<long long>(std::chrono::steady_clock::now().time_since_epoch().count()));
  const int file = directory.CreateExclusiveFile(temporary, 0644);
  if (file < 0) return false;
  std::size_t written = 0;
  while (written < data.size()) {
    const auto count = ::write(file, data.data() + written, data.size() - written);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) break;
    written += static_cast<std::size_t>(count);
  }
  const bool closed = ::close(file) == 0;
  if (written != data.size() || !closed || !directory.RenameChild(temporary, directory, target, true)) {
    directory.RemoveRegularFile(temporary);
    return false;
  }
  return true;
}

// The machine PATH as system.reg holds it (REG_EXPAND_SZ, Wine's text form), or Wine's default.
std::string MachinePath(const std::filesystem::path& prefix) {
  static const std::string kDefault =
      "%SystemRoot%\\system32;%SystemRoot%;%SystemRoot%\\system32\\wbem;%SystemRoot%\\system32\\WindowsPowershell\\v1.0";
  std::string text;
  if (!ReadFile(prefix / "system.reg", 64u * 1024u * 1024u, &text)) return kDefault;
  const auto header = Lower("[System\\\\CurrentControlSet\\\\Control\\\\Session Manager\\\\Environment]");
  const auto lower = Lower(text);
  auto at = lower.find(header);
  if (at == std::string::npos) return kDefault;
  const auto section_end = lower.find("\n[", at + 1);
  at = lower.find("\n\"path\"=", at);
  if (at == std::string::npos || (section_end != std::string::npos && at > section_end)) return kDefault;
  at = text.find('=', at) + 1;
  if (text.compare(at, 7, "str(2):") == 0) at += 7;
  if (at >= text.size() || text[at] != '"') return kDefault;
  std::string value;
  for (++at; at < text.size() && text[at] != '"' && text[at] != '\n'; ++at) {
    if (text[at] == '\\' && at + 1 < text.size()) ++at;
    value += text[at];
  }
  return value;
}

std::string WineString(std::string_view text) {
  std::string out = "\"";
  for (const char value : text) {
    if (value == '\\' || value == '"') out += '\\';
    out += value;
  }
  return out + "\"";
}

std::map<std::string, std::string> InstalledPackages(const std::filesystem::path& prefix) {
  std::map<std::string, std::string> out;
  std::string text;
  if (!ReadFile(prefix / kStateFile, 1024 * 1024, &text)) return out;
  for (std::size_t at = 0; at < text.size();) {
    auto end = text.find('\n', at);
    if (end == std::string::npos) end = text.size();
    const auto line = text.substr(at, end - at);
    at = end + 1;
    const auto space = line.find(' ');
    if (space == 64) out[line.substr(0, space)] = line.substr(space + 1);
  }
  return out;
}

bool RecordPackage(const std::filesystem::path& prefix, const std::string& sha256, const std::string& package) {
  auto packages = InstalledPackages(prefix);
  packages[sha256] = package;
  std::string text;
  for (const auto& [hash, name] : packages) text += hash + " " + name + "\n";
  auto directory = ManagedDirectory::Open(prefix, DirectoryPolicy::kApprovedTree);
  return directory && WriteFileAt(*directory, kStateFile, text);
}

// Reads a cabinet the package names as a file beside it into *bytes.
using CabinetSource = std::function<bool(const std::string& name, std::string* bytes)>;

// Installs one package: its files (those keep accepts, when given) and its registry and PATH values.
// Counts the files written in *files, or gives a reason in *reason.
bool InstallPlan(std::string_view package, const CabinetSource& beside, const std::filesystem::path& prefix,
                 const RedistEntry& entry, std::int64_t unix_time, const std::function<bool(const MsiFile&)>& keep,
                 std::size_t* files, std::string* reason, const std::function<void(std::size_t, std::size_t)>& progress = {}) {
  MsiInstallPlan plan;
  if (!ReadMsiPackage(package, &plan, reason)) return false;
  if (plan.unsupported_files) {
    *reason = std::to_string(plan.unsupported_files) + " of the package's files are stored in a way Playden can't install";
    return false;
  }
  if (keep) {
    plan.files.erase(std::remove_if(plan.files.begin(), plan.files.end(), [&](const MsiFile& file) { return !keep(file); }),
                     plan.files.end());
    if (plan.files.empty()) {
      *reason = "the package has none of the files Playden takes from it";
      return false;
    }
  }
  std::map<std::string, std::vector<const MsiFile*>> wanted;  // member → files
  std::set<std::size_t> used;  // cabinets holding a wanted file
  for (const auto& file : plan.files) {
    wanted[file.member].push_back(&file);
    used.insert(file.cabinet);
  }
  std::set<const MsiFile*> delivered;
  const auto drive = prefix / "drive_c";
  if (progress) progress(0, plan.files.size());
  for (std::size_t index = 0; index < plan.cabinets.size(); ++index) {
    if (!used.count(index)) continue;
    const auto& cabinet = plan.cabinets[index];
    std::string bytes;
    std::optional<std::string_view> view;  // an embedded cabinet in place, without a copy, when it can be
    if (cabinet.embedded) {
      view = ViewMsiStream(package, cabinet.name);
      if (!view) {
        auto stream = ReadMsiStream(package, cabinet.name);
        if (!stream) {
          *reason = "the package's cabinet " + cabinet.name + " is missing";
          return false;
        }
        bytes = std::move(*stream);
      }
    } else if (cabinet.name.find('/') != std::string::npos || cabinet.name.find('\\') != std::string::npos ||
               !beside(cabinet.name, &bytes)) {
      *reason = "the cabinet " + cabinet.name + " beside the package could not be read";
      return false;
    }
    std::string failure;
    const bool ok = ExtractCabinet(view ? *view : std::string_view(bytes), [&](const std::string& member, std::string_view data) {
      const auto found = wanted.find(member);
      if (found == wanted.end()) return true;
      for (const auto* file : found->second) {
        if (file->cabinet != index) continue;
        if (data.size() != file->size || !delivered.insert(file).second) {
          failure = "the package's copy of C:\\" + file->target + " is not the size it lists";
          return false;
        }
        const auto slash = file->target.rfind('/');
        const auto folder = slash == std::string::npos ? std::string() : file->target.substr(0, slash);
        const auto name = slash == std::string::npos ? file->target : file->target.substr(slash + 1);
        const auto directory = OpenPath(drive, folder, true);
        if (!directory || !WriteFileAt(*directory, name, data)) {
          failure = "could not write C:\\" + file->target;
          return false;
        }
        ++*files;
        if (progress) progress(*files, plan.files.size());
      }
      return true;
    }, reason);
    if (!ok) {
      if (!failure.empty()) *reason = failure;
      return false;
    }
  }
  if (delivered.size() != plan.files.size()) {
    *reason = std::to_string(plan.files.size() - delivered.size()) + " of the package's files are missing from its cabinets";
    return false;
  }
  auto machine = plan.machine;
  if (!plan.path_prepend.empty() || !plan.path_append.empty()) {
    const auto current = MachinePath(prefix);
    const auto extended = ExtendPath(current, plan.path_prepend, plan.path_append);
    if (extended != current) machine.push_back({kEnvironmentKey, "PATH", "str(2):" + WineString(extended)});
  }
  // Steam marks the entry as run under its HasRunKey (a DWORD named after the entry), unless the
  // package writes that value itself (PhysX's "PhysX Version" under AGEIA Technologies).
  const auto run_key = Lower(entry.has_run_key);
  if (run_key.rfind("hkey_local_machine\\software\\", 0) == 0 && run_key.rfind("hkey_local_machine\\software\\wow6432node\\", 0) != 0) {
    RegistryValue marker{"Software\\Wow6432Node\\" + entry.has_run_key.substr(28), entry.name, "dword:00000001"};
    const bool written = std::any_of(machine.begin(), machine.end(), [&](const RegistryValue& value) {
      return !value.default_value && Lower(value.key) == Lower(marker.key) && Lower(value.name) == Lower(marker.name);
    });
    if (!written) machine.push_back(std::move(marker));
  }
  for (const auto& [hive, values] : {std::pair<const char*, const std::vector<RegistryValue>*>{"system.reg", &machine},
                                     std::pair<const char*, const std::vector<RegistryValue>*>{"user.reg", &plan.user}}) {
    if (!values->empty() && !ApplyBottleRegistryValues(*values, prefix.string(), unix_time, hive, reason)) return false;
  }
  return true;
}

// Installs a package read from the game folder (on its own, or inside the setup program at
// package_path), its external cabinets beside that file.
bool InstallPackageBytes(std::string_view package, const std::filesystem::path& package_path,
                         const std::filesystem::path& prefix, const RedistEntry& entry, std::int64_t unix_time,
                         std::size_t* files, std::string* reason, const std::function<void(std::size_t, std::size_t)>& progress) {
  const auto beside = [&](const std::string& name, std::string* bytes) {
    return ReadFile(package_path.parent_path() / name, kMaxPackageBytes, bytes);
  };
  return InstallPlan(package, beside, prefix, entry, unix_time, {}, files, reason, progress);
}

bool InstallPackage(const std::filesystem::path& package_path, const std::filesystem::path& prefix,
                    const RedistEntry& entry, std::int64_t unix_time, std::size_t* files, std::string* reason,
                    const std::function<void(std::size_t, std::size_t)>& progress) {
  std::string package;
  if (!ReadFile(package_path, kMaxPackageBytes, &package)) {
    *reason = "the package could not be read";
    return false;
  }
  return InstallPackageBytes(package, package_path, prefix, entry, unix_time, files, reason, progress);
}

// A package that reads as one and has files.
bool PackageWithFiles(std::string_view package) {
  MsiInstallPlan plan;
  std::string reason;
  return ReadMsiPackage(package, &plan, &reason) && !plan.files.empty();
}

constexpr char kCompoundSignature[] = "\xd0\xcf\x11\xe0\xa1\xb1\x1a\xe1";
constexpr std::size_t kMaxCandidates = 16;
// A package a setup program carries, at most (PhysX 9.09's is 33 MiB): it is read whole, and the console's heap is
// small.
constexpr std::size_t kMaxEmbeddedBytes = 96u * 1024u * 1024u;

// Whether the compound file header at offset looks like one a package has (the sector sizes Compound reads), so
// a signature that only happens to be in the program isn't read to the end.
bool CompoundHeaderAt(int file, off_t offset) {
  unsigned char header[0x22];
  if (::pread(file, header, sizeof(header), offset) != static_cast<ssize_t>(sizeof(header))) return false;
  const unsigned shift = header[0x1e] | header[0x1f] << 8, mini = header[0x20] | header[0x21] << 8;
  return std::memcmp(header, kCompoundSignature, 8) == 0 && (shift == 9 || shift == 12) && mini == 6;
}

// FindEmbeddedMsi on a file, read a piece at a time: only the package, from its offset to the
// file's end, is kept, in *package (the console's heap is small and a setup program can be large).
bool FindEmbeddedMsiFile(const std::filesystem::path& path, std::string* package) {
  const int file = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (file < 0) return false;
  const std::string_view signature(kCompoundSignature, 8);
  std::vector<char> chunk(1u << 20);
  std::string carry;  // the previous piece's last bytes, for a signature across two pieces
  off_t carry_at = 0;  // where carry starts in the file
  std::size_t candidates = 0;
  bool found = false;
  for (off_t read_at = 0; !found && candidates < kMaxCandidates && read_at < static_cast<off_t>(kMaxPackageBytes);) {
    const auto count = ::read(file, chunk.data(), chunk.size());
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) break;
    std::string window = carry;
    window.append(chunk.data(), static_cast<std::size_t>(count));
    const off_t window_at = carry_at;
    for (auto at = window.find(signature); at != std::string::npos && !found && candidates < kMaxCandidates;
         at = window.find(signature, at + 1)) {
      const off_t offset = window_at + static_cast<off_t>(at);
      if (offset == 0) continue;  // the program itself
      ++candidates;
      if (CompoundHeaderAt(file, offset) && ReadFile(path, kMaxEmbeddedBytes, package, offset) &&
          PackageWithFiles(*package))
        found = true;
    }
    read_at += count;
    const auto keep = std::min<std::size_t>(window.size(), signature.size() - 1);
    carry = window.substr(window.size() - keep);
    carry_at = window_at + static_cast<off_t>(window.size() - keep);
  }
  ::close(file);
  if (!found) package->clear();
  return found;
}

}  // namespace

std::optional<std::size_t> FindEmbeddedMsi(std::string_view program) {
  // Compound files the program carries; a setup program can hold other ones (its own resources),
  // so each is read as a package until one has files. A few are enough for a setup program.
  const std::string_view signature(kCompoundSignature, 8);
  std::size_t candidates = 0;
  for (auto at = program.find(signature, 1); at != std::string_view::npos && candidates < kMaxCandidates;
       at = program.find(signature, at + 1)) {
    ++candidates;
    if (PackageWithFiles(program.substr(at))) return at;
  }
  return std::nullopt;
}

RedistAction ClassifyRedist(const RedistEntry& entry, const std::string& install_dir, std::string* package) {
  const auto program = FileName(entry.process);
  if (program == "msiexec" || program == "msiexec.exe") {
    const auto arguments = Arguments(entry.command);
    for (std::size_t index = 0; index + 1 < arguments.size(); ++index) {
      const auto flag = Lower(arguments[index]);
      if ((flag == "/i" || flag == "-i" || flag == "/package" || flag == "-package") &&
          Lower(arguments[index + 1]).size() > 4 &&
          Lower(arguments[index + 1]).compare(arguments[index + 1].size() - 4, 4, ".msi") == 0 &&
          InsideGame(arguments[index + 1], install_dir, package))
        return RedistAction::kMsi;
    }
    return RedistAction::kNotRun;
  }
  if (program.size() > 4 && program.compare(program.size() - 4, 4, ".msi") == 0 &&
      InsideGame(entry.process, install_dir, package))
    return RedistAction::kMsi;
  // Installers whose DLLs and frameworks Wine and the kit already provide.
  static constexpr const char* kCovered[] = {"vcredist", "vc_redist", "dxsetup", "dxwebsetup", "d3d11install",
                                             "dotnetfx", "dotnetfx35", "ndp", "netfx", "xnafx"};
  for (const auto* prefix : kCovered)
    if (program.rfind(prefix, 0) == 0) return RedistAction::kCovered;
  return RedistAction::kNotRun;
}

std::vector<VcRedist> MicrosoftVcRedists() {
  constexpr const char* kBase = "https://download.visualstudio.microsoft.com/download/pr/990c720a-46ce-4dc7-87b9-c2eb615d34b9/";
  return {
      {"x86", std::string(kBase) + "0C09F2611660441084CE0DF425C51C11E147E6447963C3690F97E0B25C55ED64/VC_redist.x86.exe",
       "0c09f2611660441084ce0df425c51c11e147e6447963c3690f97e0b25c55ed64", 13953392, "vcRuntimeAdditional_x86", "14.44.35211"},
      {"x64", std::string(kBase) + "CC0FF0EB1DC3F5188AE6300FAEF32BF5BEEBA4BDD6E8E445A9184072096B713B/VC_redist.x64.exe",
       "cc0ff0eb1dc3f5188ae6300faef32bf5beeba4bdd6e8e445a9184072096b713b", 25635768, "vcRuntimeAdditional_x64", "14.44.35211"},
  };
}

bool MfcFileName(std::string_view name) {
  const auto lower = Lower(name);
  return (lower.rfind("mfc140", 0) == 0 || lower.rfind("mfcm140", 0) == 0) && lower.size() > 4 &&
         lower.compare(lower.size() - 4, 4, ".dll") == 0 && lower.find('/') == std::string::npos;
}

MfcInstallResult InstallMfcRuntime(const VcRedist& redist, const std::string& cache_root, const std::string& prefix_dir,
                                   const RangeFetch& fetch, std::int64_t unix_time) {
  MfcInstallResult out;
  const auto fail = [&](std::string why) {
    out.result = "failed";
    out.detail = std::move(why);
    return out;
  };
  const std::filesystem::path prefix(prefix_dir);
  if (InstalledPackages(prefix).count(redist.sha256)) {
    out.result = "already";
    return out;
  }
  const auto slash = redist.url.rfind('/');
  const auto file_name = slash == std::string::npos ? std::string() : redist.url.substr(slash + 1);
  if (file_name.empty() || file_name == "." || file_name == ".." || redist.bytes == 0 || redist.bytes > kMaxPackageBytes)
    return fail("the redistributable's download is not one Playden can use");
  const auto folder = std::filesystem::path(cache_root) / ("vc-" + redist.version) / redist.architecture;
  const auto cached = folder / file_name;
  // The kept copy, when it is still the pinned one.
  std::string bundle;
  if (ReadFile(cached, redist.bytes + 1, &bundle) && bundle.size() == redist.bytes &&
      HashArtifactBytes(bundle).value_or("") == redist.sha256) {
    out.source = "cache";
  } else {
    bundle.clear();
    std::error_code ignored;
    std::filesystem::remove(cached, ignored);
    if (!fetch) return fail("Playden can't download it here");
    // In ranges: one HTTP response is kept small.
    bundle.reserve(redist.bytes);
    for (std::uint64_t offset = 0; offset < redist.bytes;) {
      const auto length = std::min<std::uint64_t>(kRedistChunkBytes, redist.bytes - offset);
      std::optional<std::string> chunk;
      std::string error;
      for (int attempt = 0; attempt < 3 && !chunk; ++attempt) {
        chunk = fetch(redist.url, offset, length, &error);
        if (chunk && chunk->size() != length) {
          error = "the server sent " + std::to_string(chunk->size()) + " bytes for " + std::to_string(length);
          chunk.reset();
        }
      }
      if (!chunk) {
        out.download_failed = true;
        return fail("the download failed at byte " + std::to_string(offset) + ": " + error);
      }
      bundle += *chunk;
      offset += length;
    }
    if (HashArtifactBytes(bundle).value_or("") != redist.sha256) {
      out.download_failed = true;
      return fail("the download is not the redistributable Playden expects (SHA-256 mismatch)");
    }
    out.source = "download";
    // Kept for other games: written beside, then renamed into place.
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    // Copies an earlier write left behind (Playden stopped part way).
    std::vector<std::filesystem::path> stale;
    for (std::filesystem::directory_iterator it(folder, error), end; !error && it != end; it.increment(error))
      if (it->path().filename().string().rfind(file_name + ".part-", 0) == 0) stale.push_back(it->path());
    for (const auto& path : stale) std::filesystem::remove(path, error);
    error.clear();
    const auto temporary = folder / (file_name + ".part-" + std::to_string(static_cast<long long>(::getpid())));
    const int file = error ? -1 : ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0644);
    std::size_t written = 0;
    if (file >= 0) {
      while (written < bundle.size()) {
        const auto count = ::write(file, bundle.data() + written, bundle.size() - written);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        written += static_cast<std::size_t>(count);
      }
      const bool synced = ::fsync(file) == 0;
      const bool closed = ::close(file) == 0;
      if (written != bundle.size() || !synced || !closed || ::rename(temporary.c_str(), cached.c_str()) != 0) {
        out.cache_problem = "could not write " + cached.string() + " (errno " + std::to_string(errno) + ")";
        ::unlink(temporary.c_str());
      }
    } else {
      out.cache_problem = "could not create " + folder.string() + (error ? ": " + error.message() : " (errno " + std::to_string(errno) + ")");
    }
  }
  BurnMsiPackage package;
  std::string reason;
  if (!ReadBurnMsiPackage(bundle, redist.package_id, &package, &reason)) return fail(reason);
  bundle = std::string();  // its packages are copied out; the console's memory is better spent elsewhere
  const auto beside = [&](const std::string& name, std::string* bytes) {
    for (auto& [candidate, data] : package.beside)
      if (Lower(candidate) == Lower(name)) {
        *bytes = std::move(data);  // each cabinet is read once
        return true;
      }
    return false;
  };
  const auto keep = [](const MsiFile& file) {
    const auto slash = file.target.rfind('/');
    return MfcFileName(slash == std::string::npos ? file.target : file.target.substr(slash + 1));
  };
  if (!InstallPlan(package.msi, beside, prefix, RedistEntry{}, unix_time, keep, &out.files, &reason))
    return fail(package.msi_name + ": " + reason);
  if (!RecordPackage(prefix, redist.sha256, "vc-" + redist.version + "-" + redist.architecture + "-mfc"))
    return fail("installed, but could not be recorded");
  out.result = "installed";
  return out;
}

bool MfcInstalled(const VcRedist& redist, const std::string& prefix_dir) {
  return InstalledPackages(prefix_dir).count(redist.sha256) != 0;
}

std::optional<MfcScanRecord> ReadMfcScan(const std::string& prefix_dir) {
  std::string text;
  if (!ReadFile(std::filesystem::path(prefix_dir) / kMfcScanFile, 64 * 1024, &text)) return std::nullopt;
  MfcScanRecord record;
  bool versioned = false;
  for (std::size_t at = 0; at < text.size();) {
    auto end = text.find('\n', at);
    if (end == std::string::npos) end = text.size();
    const auto line = text.substr(at, end - at);
    at = end + 1;
    const auto space = line.find(' ');
    const auto key = line.substr(0, space), value = space == std::string::npos ? std::string() : line.substr(space + 1);
    if (key == "mfc-scan" && value == "1") versioned = true;
    else if (key == "files") record.fingerprint = value;
    else if (key == "x86") record.x86_program = value;
    else if (key == "x64") record.x64_program = value;
  }
  if (!versioned) return std::nullopt;
  return record;
}

bool WriteMfcScan(const std::string& prefix_dir, const MfcScanRecord& record) {
  const auto line = [](const char* key, const std::string& value) {
    return value.empty() || value.find('\n') != std::string::npos ? std::string() : std::string(key) + " " + value + "\n";
  };
  const auto text = "mfc-scan 1\n" + line("files", record.fingerprint) + line("x86", record.x86_program) +
                    line("x64", record.x64_program);
  auto directory = ManagedDirectory::Open(prefix_dir, DirectoryPolicy::kApprovedTree);
  return directory && WriteFileAt(*directory, kMfcScanFile, text);
}

std::string GameFilesFingerprint(const std::string& game_root) {
  std::string out;
  for (const char* name : {".playden-install.json", ".playden-files"}) {
    struct stat info {};
    if (::lstat((std::filesystem::path(game_root) / name).c_str(), &info) != 0 || !S_ISREG(info.st_mode)) continue;
#if defined(__APPLE__)
    const auto nanoseconds = static_cast<long long>(info.st_mtimespec.tv_sec) * 1000000000ll + info.st_mtimespec.tv_nsec;
#else
    const auto nanoseconds = static_cast<long long>(info.st_mtim.tv_sec) * 1000000000ll + info.st_mtim.tv_nsec;
#endif
    out += std::string(out.empty() ? "" : ",") + name + ":" + std::to_string(static_cast<long long>(info.st_size)) + ":" +
           std::to_string(nanoseconds);
  }
  return out;
}

std::string ExtendPath(const std::string& current, const std::vector<std::string>& prepend,
                       const std::vector<std::string>& append) {
  std::vector<std::string> entries;
  for (std::size_t at = 0; at <= current.size();) {
    auto end = current.find(';', at);
    if (end == std::string::npos) end = current.size();
    if (end > at) entries.push_back(current.substr(at, end - at));
    at = end + 1;
  }
  const auto listed = [&](const std::string& folder) {
    auto wanted = Lower(folder);
    while (!wanted.empty() && wanted.back() == '\\') wanted.pop_back();
    return std::any_of(entries.begin(), entries.end(), [&](const std::string& existing) {
      auto have = Lower(existing);
      while (!have.empty() && have.back() == '\\') have.pop_back();
      return have == wanted;
    });
  };
  std::vector<std::string> front;
  for (const auto& folder : prepend)
    if (!listed(folder)) front.push_back(folder);
  entries.insert(entries.begin(), front.begin(), front.end());
  for (const auto& folder : append)
    if (!listed(folder)) entries.push_back(folder);
  std::string out;
  for (const auto& folder : entries) out += (out.empty() ? "" : ";") + folder;
  return out;
}

namespace {

// The name in folder that matches without regard to case, as Windows does; empty when there is none.
std::string NameIn(const std::filesystem::path& folder, const std::string& lower) {
  std::error_code error;
  for (std::filesystem::directory_iterator it(folder, error), end; !error && it != end; it.increment(error))
    if (Lower(it->path().filename().string()) == lower) return it->path().filename().string();
  return {};
}

// a.b.c of a file's version resource, read from the file's bytes; empty when it has none.
std::string FileVersion(const std::filesystem::path& path) {
  std::string data;
  if (!ReadFile(path, 32u * 1024u * 1024u, &data)) return {};
  static constexpr char kKey[] = "V\0S\0_\0V\0E\0R\0S\0I\0O\0N\0_\0I\0N\0F\0O\0";
  const auto key = data.find(std::string_view(kKey, sizeof(kKey) - 1));
  if (key == std::string::npos) return {};
  static constexpr char kSignature[] = "\xBD\x04\xEF\xFE";
  const auto signature = data.find(std::string_view(kSignature, 4), key);
  if (signature == std::string::npos || signature > key + 256 || data.size() < signature + 16) return {};
  const auto word = [&](std::size_t at) {
    return static_cast<unsigned>(static_cast<unsigned char>(data[at])) | static_cast<unsigned>(static_cast<unsigned char>(data[at + 1])) << 8;
  };
  // VS_FIXEDFILEINFO: signature, struct version, then the file version's two parts, each low word first.
  return std::to_string(word(signature + 10)) + "." + std::to_string(word(signature + 8)) + "." + std::to_string(word(signature + 14));
}

std::filesystem::path ProgramFolder(const std::string& game_root, const std::string& program) {
  return (std::filesystem::path(game_root) / program).parent_path();
}

}  // namespace

std::string PhysxVersionBeside(const std::string& game_root, const std::string& program) {
  const auto folder = ProgramFolder(game_root, program);
  for (const auto* library : {"nxcooking.dll", "physxcooking.dll", "physxextensions.dll"}) {
    const auto name = NameIn(folder, library);
    if (name.empty()) continue;
    const auto version = FileVersion(folder / name);
    if (version.rfind("2.", 0) == 0) return version;
  }
  return {};
}

bool PhysxEngineBeside(const std::string& game_root, const std::string& program) {
  const auto folder = ProgramFolder(game_root, program);
  return !NameIn(folder, "physxcore.dll").empty() || !NameIn(folder, "physxlocal").empty();
}

bool BottleHasPhysxEngine(const std::string& prefix_dir, const std::string& version) {
  const auto drive = std::filesystem::path(prefix_dir) / "drive_c";
  const auto files = NameIn(drive, "program files (x86)");
  if (files.empty()) return false;
  // Walks down names without regard to case; empty when a part is missing.
  const auto below = [](std::filesystem::path at, std::initializer_list<const char*> parts) {
    for (const auto* part : parts) {
      const auto name = NameIn(at, part);
      if (name.empty()) return std::filesystem::path();
      at /= name;
    }
    return at;
  };
  for (const auto& engines : {below(drive / files, {"nvidia corporation", "physx", "engine"}), below(drive / files, {"ageia technologies"})}) {
    if (engines.empty()) continue;
    if (!version.empty()) {
      if (!below(engines, {("v" + version).c_str(), "physxcore.dll"}).empty()) return true;
      continue;
    }
    std::error_code error;
    for (std::filesystem::directory_iterator it(engines, error), end; !error && it != end; it.increment(error))
      if (!NameIn(it->path(), "physxcore.dll").empty()) return true;
  }
  return false;
}

bool RedistIsPhysx(const std::string& name) { return Lower(name).find("physx") != std::string::npos; }

RedistInstallResult InstallRedistributables(const std::string& game_root, const std::string& install_dir,
                                            const std::string& prefix_dir, std::int64_t unix_time, bool find_physx,
                                            const RedistProgress& progress) {
  RedistInstallResult result;
  std::vector<std::string> scripts;
  {
    // The top level first (its scripts' registry values are ApplyInstallScripts' job too).
    std::error_code error;
    std::vector<std::string> top;
    for (std::filesystem::directory_iterator it(game_root, error), end; !error && it != end; it.increment(error)) {
      std::error_code type_error;
      if (ScriptName(it->path().filename().string()) && it->is_regular_file(type_error) && !it->is_symlink(type_error))
        top.push_back(it->path().filename().string());
    }
    std::sort(top.begin(), top.end());
    scripts = top;
    FindScripts(game_root, std::string(), 0, &scripts);
    if (scripts.size() > kMaxScripts) scripts.resize(kMaxScripts);
  }
  const std::filesystem::path prefix(prefix_dir);
  auto installed = InstalledPackages(prefix);
  // A package two scripts run is installed once. One that failed isn't tried again from the same folder (its
  // external cabinets are looked for there), and a setup program is looked into once.
  std::map<std::string, std::string> failed;  // hash and folder -> why
  std::set<std::string> without_package;      // setup programs' hashes
  const auto failure_key = [](const std::string& hash, const std::filesystem::path& path) {
    return hash + '\n' + path.parent_path().string();
  };
  const auto run = [&](const std::string& script, const RedistEntry& entry) {
    const auto files_done = [&](std::size_t done, std::size_t total) {
      if (progress) progress(entry.name, done, total);
    };
    RedistOutcome outcome;
    outcome.script = script;
    outcome.name = entry.name;
    std::string package;
    const auto action = ClassifyRedist(entry, install_dir, &package);
    if (action == RedistAction::kNotRun && InsideGame(entry.process, install_dir, &package) && package.size() > 4 &&
        Lower(package).compare(package.size() - 4, 4, ".exe") == 0) {
      // A setup program the console can't start may carry its package as is (PhysX's). One
      // installed before is known by the program's hash, without reading its package again.
      const auto path = std::filesystem::path(game_root) / package;
      const auto hash = HashArtifactFile(path);
      std::string embedded;
      const bool already = hash && installed.count(*hash);
      const auto earlier = hash ? failed.find(failure_key(*hash, path)) : failed.end();
      bool carries = false;
      if (hash && !already && earlier == failed.end() && !without_package.count(*hash)) {
        carries = FindEmbeddedMsiFile(path, &embedded);
        if (!carries) without_package.insert(*hash);
      }
      if (already || earlier != failed.end() || carries) {
        outcome.detail = package + " (its package)";
        if (already) {
          outcome.result = "already";
        } else if (earlier != failed.end()) {
          outcome.result = "failed";
          outcome.detail = package + ": " + earlier->second;
        } else {
          std::string failure;
            files_done(0, 1);
          if (!InstallPackageBytes(embedded, path, prefix, entry, unix_time, &outcome.files, &failure, files_done)) {
            outcome.result = "failed";
          } else if (!RecordPackage(prefix, *hash, package)) {
            outcome.result = "failed";
            failure = "installed, but could not be recorded";
          } else {
            outcome.result = "installed";
            installed[*hash] = package;
          }
          if (outcome.result == "failed") {
            outcome.detail = package + ": " + failure;
            failed[failure_key(*hash, path)] = failure;
          }
        }
        result.outcomes.push_back(std::move(outcome));
        return;
      }
    }
    if (action != RedistAction::kMsi) {
      outcome.result = action == RedistAction::kCovered ? "covered" : "not-run";
      outcome.detail = FileName(entry.process);
      result.outcomes.push_back(std::move(outcome));
      return;
    }
    outcome.detail = package;
    const auto path = std::filesystem::path(game_root) / package;
    const auto hash = HashArtifactFile(path);
    if (!hash) {
      outcome.result = "failed";
      outcome.detail = package + ": the package could not be read";
      result.outcomes.push_back(std::move(outcome));
      return;
    }
    if (installed.count(*hash)) {
      outcome.result = "already";
      result.outcomes.push_back(std::move(outcome));
      return;
    }
    std::string failure;
    if (const auto earlier = failed.find(failure_key(*hash, path)); earlier != failed.end()) {
      outcome.result = "failed";
      failure = earlier->second;
    } else if (files_done(0, 1); !InstallPackage(path, prefix, entry, unix_time, &outcome.files, &failure, files_done)) {
      outcome.result = "failed";
    } else if (!RecordPackage(prefix, *hash, package)) {
      outcome.result = "failed";
      failure = "installed, but could not be recorded";
    } else {
      outcome.result = "installed";
      installed[*hash] = package;
    }
    if (outcome.result == "failed") {
      outcome.detail = package + ": " + failure;
      failed[failure_key(*hash, path)] = failure;
    }
    result.outcomes.push_back(std::move(outcome));
  };
  for (const auto& script : scripts) {
    std::string text, reason;
    std::vector<RedistEntry> entries;
    if (!ReadInstallScript((std::filesystem::path(game_root) / script).string(), &text) ||
        !ParseRunProcess(text, install_dir, &entries, &reason))
      continue;
    for (const auto& entry : entries) run(script, entry);
  }
  // A game that needs PhysX and has no script to install it (an imported folder): the package it ships. A script
  // that names PhysX has said which one the game wants.
  const auto names_physx = [](const RedistOutcome& outcome) {
    return Lower(outcome.name).find("physx") != std::string::npos || Lower(outcome.detail).find("physx") != std::string::npos;
  };
  if (find_physx && std::none_of(result.outcomes.begin(), result.outcomes.end(), names_physx)) {
    std::vector<std::string> packages;
    FindPhysxPackages(game_root, std::string(), 0, false, &packages);
    auto base = install_dir;
    while (!base.empty() && base.back() == '\\') base.pop_back();
    for (const auto& package : packages) {
      auto windows = package;
      std::replace(windows.begin(), windows.end(), '/', '\\');
      run("(no script)", RedistEntry{"PhysX", base + "\\" + windows, "", ""});
    }
    result.physx_missing = packages.empty();
    result.physx_searched = true;
  }
  return result;
}

}  // namespace playden::runtime
