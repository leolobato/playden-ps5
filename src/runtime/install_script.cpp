#include "runtime/install_script.h"

#include "steam/backend/vdf.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace playden::runtime {
namespace {

constexpr std::size_t kMaxScriptBytes = 1024 * 1024;
constexpr std::size_t kMaxScripts = 8;

std::string Lower(std::string_view text) {
  std::string out(text);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char value) { return static_cast<char>(value >= 'A' && value <= 'Z' ? value + 32 : value); });
  return out;
}

// UTF-16 (either byte order, with its mark) to UTF-8; other text is returned as it is.
std::string ScriptText(std::string_view bytes) {
  if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xef && static_cast<unsigned char>(bytes[1]) == 0xbb &&
      static_cast<unsigned char>(bytes[2]) == 0xbf)
    return std::string(bytes.substr(3));
  if (bytes.size() < 2) return std::string(bytes);
  const auto first = static_cast<unsigned char>(bytes[0]), second = static_cast<unsigned char>(bytes[1]);
  const bool little = first == 0xff && second == 0xfe, big = first == 0xfe && second == 0xff;
  if (!little && !big) return std::string(bytes);
  const auto unit_at = [&](std::size_t index) {
    const auto low = static_cast<unsigned char>(bytes[index + (little ? 0 : 1)]);
    const auto high = static_cast<unsigned char>(bytes[index + (little ? 1 : 0)]);
    return static_cast<unsigned>(low | (high << 8));
  };
  std::string out;
  for (std::size_t index = 2; index + 1 < bytes.size(); index += 2) {
    unsigned point = unit_at(index);
    // A surrogate pair is one character past the Basic Multilingual Plane. A lone surrogate is kept as the
    // three bytes UTF-8 forbids, so the value holding it is left out rather than changed.
    if (point >= 0xd800 && point <= 0xdbff && index + 3 < bytes.size()) {
      const auto next = unit_at(index + 2);
      if (next >= 0xdc00 && next <= 0xdfff) {
        point = 0x10000 + ((point - 0xd800) << 10) + (next - 0xdc00);
        index += 2;
      }
    }
    if (point < 0x80) {
      out += static_cast<char>(point);
    } else if (point < 0x800) {
      out += static_cast<char>(0xc0 | (point >> 6));
      out += static_cast<char>(0x80 | (point & 0x3f));
    } else if (point < 0x10000) {
      out += static_cast<char>(0xe0 | (point >> 12));
      out += static_cast<char>(0x80 | ((point >> 6) & 0x3f));
      out += static_cast<char>(0x80 | (point & 0x3f));
    } else {
      out += static_cast<char>(0xf0 | (point >> 18));
      out += static_cast<char>(0x80 | ((point >> 12) & 0x3f));
      out += static_cast<char>(0x80 | ((point >> 6) & 0x3f));
      out += static_cast<char>(0x80 | (point & 0x3f));
    }
  }
  return out;
}

bool Ascii(std::string_view text) {
  return std::all_of(text.begin(), text.end(),
                     [](unsigned char value) { return value >= 0x20 && value < 0x7f; });
}

// The folders Steam's tokens name, as Steam (a 32-bit program) would see them in the bottle, whose Windows user is
// always "prospero" (the runtime kit's template, checked in kit_manifest). Steam documents INSTALLDIR, ROOTDRIVE,
// APPDATA, LOCALAPPDATA, USER_MYDOCS, COMMON_MYDOCS, WinDir and STEAMPATH, and takes Windows' environment variables
// too; the common ones are here. %INSTALLDIR% is the game folder and %ROOTDRIVE% its drive letter (Substitute).
// %STEAMPATH% is left unknown: the Steam client folder depends on the game's Steam mode (none, or
// steamworks' client folder), so no one path is right. Names match in any case.
constexpr std::pair<const char*, const char*> kTokens[] = {
    {"systemdrive", "C:"},
    {"homedrive", "C:"},
    {"windir", "C:\\windows"},
    {"systemroot", "C:\\windows"},
    {"appdata", "C:\\users\\prospero\\AppData\\Roaming"},
    {"localappdata", "C:\\users\\prospero\\AppData\\Local"},
    {"temp", "C:\\users\\prospero\\AppData\\Local\\Temp"},
    {"tmp", "C:\\users\\prospero\\AppData\\Local\\Temp"},
    {"user_mydocs", "C:\\users\\prospero\\Documents"},
    {"common_mydocs", "C:\\users\\Public\\Documents"},
    {"userprofile", "C:\\users\\prospero"},
    {"homepath", "\\users\\prospero"},
    {"username", "prospero"},
    {"public", "C:\\users\\Public"},
    {"allusersprofile", "C:\\ProgramData"},
    {"programdata", "C:\\ProgramData"},
    // A 32-bit program's ProgramFiles is the x86 folder; ProgramW6432 names the 64-bit one.
    {"programfiles", "C:\\Program Files (x86)"},
    {"programfiles(x86)", "C:\\Program Files (x86)"},
    {"programw6432", "C:\\Program Files"},
    {"commonprogramfiles", "C:\\Program Files (x86)\\Common Files"},
    {"commonprogramfiles(x86)", "C:\\Program Files (x86)\\Common Files"},
    {"commonprogramw6432", "C:\\Program Files\\Common Files"},
};

// Steam's %TOKEN%s (any case) replaced. Only %NAME% with a name of letters, digits, _ and () is a token, as in
// Windows' environment variables: "100%" and a shell command's "%1" stay as they are. False for a token this list
// doesn't know, which is named in unknown: its folder can't be guessed, and a wrong path is worse than no value.
bool Substitute(std::string_view text, const std::string& install_dir, std::string* out, std::string* unknown = nullptr) {
  out->clear();
  const auto name_char = [](char value) {
    return std::isalnum(static_cast<unsigned char>(value)) || value == '_' || value == '(' || value == ')';
  };
  for (std::size_t at = 0; at < text.size();) {
    const auto end = text[at] == '%' ? text.find('%', at + 1) : std::string_view::npos;
    const auto inside = end == std::string_view::npos ? std::string_view() : text.substr(at + 1, end - at - 1);
    const bool token = !inside.empty() && std::all_of(inside.begin(), inside.end(), name_char) &&
                       !std::isdigit(static_cast<unsigned char>(inside.front()));
    if (!token) {
      *out += text[at++];
      continue;
    }
    const auto name = Lower(inside);
    std::string folder;
    bool known = true;
    if (name == "installdir") {
      folder = install_dir;
    } else if (name == "rootdrive") {
      // The game folder's drive letter, without its colon (Steam's own scripts write %ROOTDRIVE%:\).
      if (install_dir.size() >= 2 && install_dir[1] == ':') folder = install_dir.substr(0, 1);
      else known = false;
    } else {
      known = false;
      for (const auto& [token_name, path] : kTokens)
        if (name == token_name) {
          folder = path;
          known = true;
        }
    }
    if (!known) {
      if (unknown) *unknown = "%" + std::string(inside) + "%";
      return false;
    }
    *out += folder;
    at = end + 1;
  }
  return true;
}

// UTF-8 text to UTF-16 code units; false for bytes that aren't UTF-8.
bool Utf16Units(std::string_view text, std::vector<unsigned>* units) {
  units->clear();
  for (std::size_t at = 0; at < text.size();) {
    const auto lead = static_cast<unsigned char>(text[at]);
    const std::size_t length = lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2 : (lead >> 4) == 0xe ? 3 : (lead >> 3) == 0x1e ? 4 : 0;
    if (!length || at + length > text.size()) return false;
    unsigned point = length == 1 ? lead : lead & (0x7f >> length);
    for (std::size_t index = 1; index < length; ++index) {
      const auto next = static_cast<unsigned char>(text[at + index]);
      if ((next & 0xc0) != 0x80) return false;
      point = (point << 6) | (next & 0x3f);
    }
    const unsigned least[] = {0, 0, 0x80, 0x800, 0x10000};
    if (point < least[length] || point > 0x10ffff || (point >= 0xd800 && point <= 0xdfff)) return false;
    if (point >= 0x10000) {
      units->push_back(0xd800 + ((point - 0x10000) >> 10));
      units->push_back(0xdc00 + ((point - 0x10000) & 0x3ff));
    } else {
      units->push_back(point);
    }
    at += length;
  }
  return true;
}

// Text as Wine writes it inside a quoted registry string (server/unicode.c dump_strW): \ and \" escaped, and a
// character past ASCII as \x and its UTF-16 code in hex, four digits when a hex digit follows, so the hive reads the
// same after Wine saves it. False for text that isn't UTF-8 or holds control characters.
bool WineText(std::string_view text, std::string* out) {
  std::vector<unsigned> units;
  if (!Utf16Units(text, &units)) return false;
  out->clear();
  for (std::size_t index = 0; index < units.size(); ++index) {
    const auto unit = units[index];
    if (unit < 0x20 || (unit >= 0x7f && unit <= 0x9f)) return false;
    if (unit > 127) {
      const bool hex_next = index + 1 < units.size() && units[index + 1] < 128 && std::isxdigit(static_cast<int>(units[index + 1]));
      char escaped[8];
      std::snprintf(escaped, sizeof(escaped), hex_next ? "\\x%04x" : "\\x%x", unit);
      *out += escaped;
      continue;
    }
    if (unit == '\\' || unit == '"') *out += '\\';
    *out += static_cast<char>(unit);
  }
  return true;
}

// The top level is "InstallScript" { ... } (any case), often followed by Steam's "kvsignatures"
// block; a lone section of another name or a bare section list is read the same way.
const steam::VdfNode* ScriptSection(const steam::VdfNode& root) {
  if (const auto* script = root.Find("InstallScript"); script && script->is_dict) return script;
  if (root.children.size() == 1 && root.children.front().second.is_dict) return &root.children.front().second;
  return &root;
}

bool ReadScript(const std::filesystem::path& path, std::string* text) {
  const int file = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (file < 0) return false;
  char buffer[8192];
  bool ok = true;
  while (true) {
    const auto count = ::read(file, buffer, sizeof(buffer));
    if (count < 0 && errno == EINTR) continue;
    if (count < 0 || text->size() + static_cast<std::size_t>(count) > kMaxScriptBytes) { ok = false; break; }
    if (count == 0) break;
    text->append(buffer, static_cast<std::size_t>(count));
  }
  ::close(file);
  return ok;
}

// The 32-bit view's twin of an HKLM key: Software\Wow6432Node\<rest>, except under Software\Classes, whose 32-bit
// keys Wine keeps in Software\Classes\Wow6432Node (Software\Wow6432Node\Classes is a link to it). Empty for a key
// outside Software, or one already in a 32-bit node.
std::string Wow32Twin(const std::string& subkey) {
  const auto lower = Lower(subkey);
  const auto under = [&lower](const std::string& prefix) {
    return lower == prefix || lower.rfind(prefix + "\\", 0) == 0;
  };
  if (under("software\\wow6432node") || under("software\\classes\\wow6432node")) return {};
  if (under("software\\classes")) return "Software\\Classes\\Wow6432Node" + subkey.substr(16);
  if (under("software")) return "Software\\Wow6432Node" + subkey.substr(8);
  return {};
}

// The keys for a value under one registry view of HKLM (HKEY_LOCAL_MACHINE_WOW64_32 or _WOW64_64). The 64-bit view is
// the key as written; the 32-bit view is its twin (Wow32Twin). Software\Classes is mostly shared by both views (with
// redirected parts like CLSID), so a 32-bit value there gets both keys, as a plain HKLM key does.
std::vector<std::string> ViewKeys(bool view32, const std::string& subkey) {
  const auto twin = view32 ? Wow32Twin(subkey) : std::string();
  if (twin.empty()) return {subkey};
  const auto lower = Lower(subkey);
  if (lower == "software\\classes" || lower.rfind("software\\classes\\", 0) == 0) return {subkey, twin};
  return {twin};
}

// At most this many dropped values are named; the rest are only counted, so a script can't grow the list without end.
constexpr std::size_t kDroppedNamed = 16;

// A key or name as it may go in the log: printable ASCII only (anything else becomes ?), and at most 96 characters,
// so a script's text can't add lines to the log or carry much of itself into a report.
std::string LogSafe(std::string_view text) {
  std::string out;
  for (const char value : text.substr(0, 96)) {
    const auto byte = static_cast<unsigned char>(value);
    out += byte >= 0x20 && byte < 0x7f ? value : '?';
  }
  return text.size() > 96 ? out + "..." : out;
}

// The values a type's section sets for the language, as ParseInstallScript reads them: its plain values and the ones
// in the language's (or English's) subkey.
std::size_t ValueCount(const steam::VdfNode& entries, const std::string& language) {
  std::size_t count = 0;
  const steam::VdfNode* localized = nullptr;
  for (const auto* wanted : {language.c_str(), "english"})
    for (const auto& [name, data] : entries.children)
      if (!localized && data.is_dict && Lower(name) == Lower(wanted)) localized = &data;
  for (const auto& [name, data] : entries.children) count += data.is_dict ? 0 : 1;
  if (localized)
    for (const auto& [name, data] : localized->children) count += data.is_dict ? 0 : 1;
  return count;
}

}  // namespace

std::vector<std::string> InstallScriptKeys(bool machine, const std::string& subkey) {
  std::vector<std::string> keys{subkey};
  // Steam is a 32-bit program, so HKLM\Software lands in the 32-bit view; a 64-bit game
  // looking in its own view finds it too.
  if (const auto twin = machine ? Wow32Twin(subkey) : std::string(); !twin.empty()) keys.push_back(twin);
  return keys;
}

std::string RegistryString(std::string_view text) {
  std::string escaped;
  if (WineText(text, &escaped)) return "\"" + escaped + "\"";
  // Not UTF-8 (or with control characters): each byte as it is, the way it always was.
  std::string out = "\"";
  for (const char value : text) {
    if (value == '\\' || value == '"') out += '\\';
    out += value;
  }
  return out + "\"";
}

void SetRegistryValue(std::vector<RegistryValue>* list, RegistryValue value) {
  list->erase(std::remove_if(list->begin(), list->end(), [&](const RegistryValue& known) {
    return known.default_value == value.default_value && Lower(known.key) == Lower(value.key) &&
           Lower(known.name) == Lower(value.name);
  }), list->end());
  list->push_back(std::move(value));
}

bool ParseInstallScript(std::string_view text, const std::string& install_dir, const std::string& language,
                        InstallScriptValues* values, std::string* reason) {
  const auto fail = [&](const char* why) {
    if (reason) *reason = why;
    return false;
  };
  auto utf8 = ScriptText(text);
  while (!utf8.empty() && utf8.back() == '\0') utf8.pop_back();
  const auto root = steam::ParseVdf(utf8);
  if (!root) return fail("not a valid install script");
  const auto* script = ScriptSection(*root);
  const auto* registry = script->Find("Registry");
  if (!registry) return true;
  // Values a script sets in one registry view. They go in last, so a plain HKLM key's copy for the other view
  // doesn't replace a value the script set there itself.
  std::vector<RegistryValue> viewed;
  // A value left out: counted, and named (while there is room) with its key and why.
  const auto drop = [values](std::string_view where, const std::string& why, std::size_t count = 1) {
    if (!count) return;
    values->dropped_count += count;
    if (values->dropped.size() < kDroppedNamed)
      values->dropped.push_back(LogSafe(where) + ": " + why + (count > 1 ? " (" + std::to_string(count) + " values)" : ""));
  };
  // Every value under a key that is left out as a whole.
  const auto key_values = [&language](const steam::VdfNode& key) {
    std::size_t count = 0;
    for (const auto& [type, entries] : key.children) count += entries.is_dict ? ValueCount(entries, language) : 0;
    return count;
  };
  for (const auto& [path, key] : registry->children) {
    if (!key.is_dict) continue;
    const auto separator = path.find('\\');
    const auto hive = Lower(path.substr(0, separator));
    if (separator == std::string::npos || separator + 1 >= path.size()) {
      drop(path, "no key under the root", key_values(key));
      continue;
    }
    // A script can name one registry view of HKLM: _WOW64_32 is the 32-bit one, where a 32-bit game reads
    // HKLM\Software (Software\Wow6432Node in the bottle), and _WOW64_64 the 64-bit one (the key as written).
    // Road Rash 95 finds its Text\<language>.dll through a _WOW64_32 key and shows a bare "Warning" without it.
    const bool view32 = hive == "hkey_local_machine_wow64_32";
    const bool view64 = hive == "hkey_local_machine_wow64_64";
    // HKEY_CLASSES_ROOT is HKLM\Software\Classes for a program that writes it, as Steam does. HKCU's
    // Software is shared by both views, so its view names are HKCU itself.
    const bool classes = hive == "hkey_classes_root" || hive == "hkcr";
    const bool machine = hive == "hkey_local_machine" || hive == "hklm" || view32 || view64 || classes;
    const bool user = hive == "hkey_current_user" || hive == "hkcu" || hive == "hkey_current_user_wow64_32" ||
                      hive == "hkey_current_user_wow64_64";
    if (!machine && !user) {
      drop(path, "root not supported", key_values(key));
      continue;
    }
    std::string subkey, unknown;
    if (!Substitute(path.substr(separator + 1), install_dir, &subkey, &unknown)) {
      drop(path, "unknown " + unknown, key_values(key));
      continue;
    }
    // Wine's registry files end a key's name at "]"; such a key, and one past ASCII, can't be written.
    if (!Ascii(subkey) || subkey.find_first_of("[]") != std::string::npos) {
      drop(path, "key isn't plain ASCII without [ or ]", key_values(key));
      continue;
    }
    if (classes) subkey = "Software\\Classes\\" + subkey;
    const auto keys = view32 || view64 ? ViewKeys(view32, subkey) : InstallScriptKeys(machine, subkey);
    auto& target = view32 || view64 ? viewed : machine ? values->machine : values->user;
    for (const auto& [type, entries] : key.children) {
      const auto kind = Lower(type);
      if (!entries.is_dict) continue;
      if (kind != "string" && kind != "dword") {
        drop(path, "type " + LogSafe(type) + " not supported", ValueCount(entries, language));
        continue;
      }
      const auto add = [&](const std::string& name, const std::string& data) {
        const auto where = path + " " + name;
        std::string value, expanded_name, written_name;
        if (name.empty()) return drop(path, "value without a name");
        // A name can hold tokens too (AppCompatFlags\Layers values are named by a program's full path).
        if (!Substitute(name, install_dir, &expanded_name, &unknown)) return drop(where, "unknown " + unknown);
        if (expanded_name.find('"') != std::string::npos || !WineText(expanded_name, &written_name))
          return drop(where, "name isn't text Playden can write");
        if (!Substitute(data, install_dir, &value, &unknown)) return drop(where, "unknown " + unknown);
        if (kind == "dword") {
          char* end = nullptr;
          errno = 0;
          // Digits only from the start (strtoull would skip leading spaces), and nothing after them.
          const bool digits = !value.empty() && std::isdigit(static_cast<unsigned char>(value.front())) &&
                              value.find('\0') == std::string::npos;
          const auto number = digits ? std::strtoull(value.c_str(), &end, 0) : 0;
          if (!digits || errno || *end || number > 0xffffffffull) return drop(where, "dword isn't a 32-bit number");
          char text_value[16];
          std::snprintf(text_value, sizeof(text_value), "dword:%08llx", number);
          value = text_value;
        } else {
          std::string escaped;
          if (!WineText(value, &escaped)) return drop(where, "string isn't UTF-8 text");
          value = "\"" + escaped + "\"";
        }
        const bool unnamed = Lower(expanded_name) == "(default)";
        // A language's value replaces the plain one of the same name.
        for (const auto& name_key : keys)
          SetRegistryValue(&target, {name_key, unnamed ? std::string() : written_name, value, unnamed});
      };
      // A subkey holds the values for one language ("dword" { "english" { "Language" "1" } }).
      const steam::VdfNode* localized = nullptr;
      for (const auto* wanted : {language.c_str(), "english"}) {
        for (const auto& [name, data] : entries.children)
          if (!localized && data.is_dict && Lower(name) == Lower(wanted)) localized = &data;
      }
      for (const auto& [name, data] : entries.children)
        if (!data.is_dict) add(name, data.value);
      if (localized)
        for (const auto& [name, data] : localized->children)
          if (!data.is_dict) add(name, data.value);
    }
  }
  for (auto& value : viewed) SetRegistryValue(&values->machine, std::move(value));
  return true;
}

bool ParseRunProcess(std::string_view text, const std::string& install_dir, std::vector<RedistEntry>* entries,
                     std::string* reason) {
  const auto fail = [&](const char* why) {
    if (reason) *reason = why;
    return false;
  };
  auto utf8 = ScriptText(text);
  while (!utf8.empty() && utf8.back() == '\0') utf8.pop_back();
  const auto root = steam::ParseVdf(utf8);
  if (!root) return fail("not a valid install script");
  const auto* script = ScriptSection(*root);
  const auto* run = script->Find("Run Process");
  if (!run) return true;
  for (const auto& [name, node] : run->children) {
    if (!node.is_dict) continue;
    RedistEntry entry;
    entry.name = name;
    bool usable = Ascii(name);
    for (const auto& [field, value] : node.children) {
      if (value.is_dict) continue;
      const auto key = Lower(field);
      std::string* target = key == "process 1" ? &entry.process : key == "command 1" ? &entry.command
                          : key == "hasrunkey" ? &entry.has_run_key : nullptr;
      if (!target) continue;
      if (!Substitute(value.value, install_dir, target) || !Ascii(*target)) usable = false;
    }
    if (usable && !entry.process.empty()) entries->push_back(std::move(entry));
  }
  return true;
}

bool InstallScriptName(std::string_view name) {
  const auto lower = Lower(name);
  if (lower.size() <= 4 || lower.compare(lower.size() - 4, 4, ".vdf") != 0) return false;
  if (lower.rfind("installscript", 0) == 0) return true;
  constexpr std::string_view kAppScript = "_install.vdf";
  if (lower.size() <= kAppScript.size() || lower.compare(lower.size() - kAppScript.size(), kAppScript.size(), kAppScript) != 0)
    return false;
  const auto digits = lower.size() - kAppScript.size();
  return std::all_of(lower.begin(), lower.begin() + static_cast<std::ptrdiff_t>(digits),
                     [](char value) { return value >= '0' && value <= '9'; });
}

bool ReadInstallScript(const std::string& path, std::string* text) {
  text->clear();
  return ReadScript(path, text);
}

namespace {

// A regular file at relative under root, with no link or missing folder on the way.
bool PlainFileUnder(const std::string& root, const std::string& relative) {
  std::string path = root;
  std::size_t start = 0;
  while (start <= relative.size()) {
    const auto slash = relative.find('/', start);
    const auto end = slash == std::string::npos ? relative.size() : slash;
    path += "/" + relative.substr(start, end - start);
    struct stat info {};
    if (::lstat(path.c_str(), &info) != 0) return false;
    if (slash == std::string::npos) return S_ISREG(info.st_mode);
    if (!S_ISDIR(info.st_mode)) return false;
    start = slash + 1;
  }
  return false;
}

bool SafeRelative(const std::string& path) {
  if (path.empty() || path.front() == '/' || path.back() == '/' || path.find('\\') != std::string::npos ||
      path.find('\0') != std::string::npos)
    return false;
  std::size_t start = 0;
  while (true) {
    const auto slash = path.find('/', start);
    const auto part = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
    if (part.empty() || part == "." || part == "..") return false;
    if (slash == std::string::npos) return true;
    start = slash + 1;
  }
}

}  // namespace

InstallScriptFiles FindInstallScripts(const std::string& game_root) {
  InstallScriptFiles out;
  std::string list;
  const auto list_path = std::filesystem::path(game_root) / kInstallScriptList;
  struct stat list_info {};
  if (::lstat(list_path.c_str(), &list_info) == 0 && S_ISREG(list_info.st_mode) && ReadScript(list_path, &list)) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start < list.size()) {
      auto end = list.find('\n', start);
      if (end == std::string::npos) end = list.size();
      auto line = list.substr(start, end - start);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      lines.push_back(std::move(line));
      start = end + 1;
    }
    if (!lines.empty() && lines.front() == "v1") {
      out.flagged = true;
      for (std::size_t index = 1; index < lines.size(); ++index)
        if (SafeRelative(lines[index]) && PlainFileUnder(game_root, lines[index])) out.paths.push_back(lines[index]);
    }
  }
  if (!out.flagged) {
    std::error_code error;
    for (std::filesystem::directory_iterator it(game_root, error), end; !error && it != end; it.increment(error)) {
      std::error_code type_error;
      const auto name = it->path().filename().string();
      if (InstallScriptName(name) && it->is_regular_file(type_error) && !it->is_symlink(type_error)) out.paths.push_back(name);
    }
  }
  // The top level's first, then the rest, each by name (byte order).
  std::sort(out.paths.begin(), out.paths.end(), [](const std::string& left, const std::string& right) {
    const bool left_top = left.find('/') == std::string::npos, right_top = right.find('/') == std::string::npos;
    return left_top != right_top ? left_top : left < right;
  });
  out.paths.erase(std::unique(out.paths.begin(), out.paths.end()), out.paths.end());
  return out;
}

InstallScriptResult ApplyInstallScripts(const std::string& game_root, const std::string& install_dir,
                                        const std::string& language, const std::string& prefix_dir,
                                        std::int64_t unix_time) {
  InstallScriptResult result;
  auto scripts = FindInstallScripts(game_root).paths;
  if (scripts.size() > kMaxScripts) scripts.resize(kMaxScripts);
  InstallScriptValues values;
  for (const auto& name : scripts) {
    const auto path = std::filesystem::path(game_root) / name;
    result.found.push_back(name);
    std::string text, reason;
    InstallScriptValues parsed;
    if (!ReadScript(path, &text) || !ParseInstallScript(text, install_dir, language, &parsed, &reason)) {
      result.skipped.push_back(name);
      continue;
    }
    result.dropped_count += parsed.dropped_count;
    for (const auto& note : parsed.dropped)
      if (result.dropped.size() < kDroppedNamed) result.dropped.push_back(LogSafe(name) + ": " + note);
    // A later script's value replaces an earlier one's, in Playden's own order (file names, sorted byte by byte).
    for (auto& value : parsed.machine) SetRegistryValue(&values.machine, std::move(value));
    for (auto& value : parsed.user) SetRegistryValue(&values.user, std::move(value));
  }
  result.values = values.machine.size() + values.user.size();
  for (const auto& [hive, list] : {std::pair<const char*, const std::vector<RegistryValue>*>{"system.reg", &values.machine},
                                   std::pair<const char*, const std::vector<RegistryValue>*>{"user.reg", &values.user}}) {
    std::string reason;
    if (!list->empty() && !ApplyBottleRegistryValues(*list, prefix_dir, unix_time, hive, &reason)) {
      result.ok = false;
      result.message = reason;
      return result;
    }
  }
  return result;
}

}  // namespace playden::runtime
