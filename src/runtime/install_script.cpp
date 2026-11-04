#include "runtime/install_script.h"

#include "steam/backend/vdf.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fcntl.h>
#include <unistd.h>

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
  std::string out;
  for (std::size_t index = 2; index + 1 < bytes.size(); index += 2) {
    const auto low = static_cast<unsigned char>(bytes[index + (little ? 0 : 1)]);
    const auto high = static_cast<unsigned char>(bytes[index + (little ? 1 : 0)]);
    const unsigned unit = low | (high << 8);
    // Characters outside the Basic Multilingual Plane never matter to a registry path; drop them.
    if (unit >= 0xd800 && unit <= 0xdfff) continue;
    if (unit < 0x80) {
      out += static_cast<char>(unit);
    } else if (unit < 0x800) {
      out += static_cast<char>(0xc0 | (unit >> 6));
      out += static_cast<char>(0x80 | (unit & 0x3f));
    } else {
      out += static_cast<char>(0xe0 | (unit >> 12));
      out += static_cast<char>(0x80 | ((unit >> 6) & 0x3f));
      out += static_cast<char>(0x80 | (unit & 0x3f));
    }
  }
  return out;
}

bool Ascii(std::string_view text) {
  return std::all_of(text.begin(), text.end(),
                     [](unsigned char value) { return value >= 0x20 && value < 0x7f; });
}

// %INSTALLDIR% (any case) replaced; false when another %TOKEN% is left.
bool Substitute(std::string_view text, const std::string& install_dir, std::string* out) {
  out->clear();
  for (std::size_t at = 0; at < text.size();) {
    if (text[at] != '%') {
      *out += text[at++];
      continue;
    }
    const auto end = text.find('%', at + 1);
    if (end == std::string_view::npos || Lower(text.substr(at, end - at + 1)) != "%installdir%") return false;
    *out += install_dir;
    at = end + 1;
  }
  return true;
}

// Wine's text form of a REG_SZ: quoted, with backslashes and quotes escaped.
std::string RegistryString(std::string_view text) {
  std::string out = "\"";
  for (const char value : text) {
    if (value == '\\' || value == '"') out += '\\';
    out += value;
  }
  return out + "\"";
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

}  // namespace

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
  for (const auto& [path, key] : registry->children) {
    if (!key.is_dict) continue;
    const auto separator = path.find('\\');
    const auto hive = Lower(path.substr(0, separator));
    if (separator == std::string::npos || separator + 1 >= path.size()) continue;
    const bool machine = hive == "hkey_local_machine" || hive == "hklm";
    if (!machine && hive != "hkey_current_user" && hive != "hkcu") continue;
    const auto subkey = path.substr(separator + 1);
    if (!Ascii(subkey) || subkey.find('%') != std::string::npos) continue;
    std::vector<std::string> keys{subkey};
    // Steam is a 32-bit program, so HKLM\Software lands in the 32-bit view; a 64-bit game
    // looking in its own view finds it too.
    if (machine && Lower(subkey).rfind("software\\", 0) == 0 && Lower(subkey).rfind("software\\wow6432node\\", 0) != 0)
      keys.push_back("Software\\Wow6432Node\\" + subkey.substr(9));
    auto& target = machine ? values->machine : values->user;
    for (const auto& [type, entries] : key.children) {
      const auto kind = Lower(type);
      if (!entries.is_dict || (kind != "string" && kind != "dword")) continue;
      const auto add = [&](const std::string& name, const std::string& data) {
        if (name.empty() || !Ascii(name)) return;
        std::string value;
        if (!Substitute(data, install_dir, &value) || !Ascii(value)) return;
        if (kind == "dword") {
          char* end = nullptr;
          errno = 0;
          const auto number = std::strtoull(value.c_str(), &end, 0);
          if (value.empty() || errno || *end || number > 0xffffffffull) return;
          char text_value[16];
          std::snprintf(text_value, sizeof(text_value), "dword:%08llx", number);
          value = text_value;
        } else {
          value = RegistryString(value);
        }
        const bool unnamed = Lower(name) == "(default)";
        for (const auto& name_key : keys) target.push_back({name_key, unnamed ? std::string() : name, value, unnamed});
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

bool ReadInstallScript(const std::string& path, std::string* text) {
  text->clear();
  return ReadScript(path, text);
}

InstallScriptResult ApplyInstallScripts(const std::string& game_root, const std::string& install_dir,
                                        const std::string& language, const std::string& prefix_dir,
                                        std::int64_t unix_time) {
  InstallScriptResult result;
  std::vector<std::filesystem::path> scripts;
  std::error_code error;
  for (std::filesystem::directory_iterator it(game_root, error), end; !error && it != end; it.increment(error)) {
    const auto name = Lower(it->path().filename().string());
    std::error_code type_error;
    if (name.rfind("installscript", 0) == 0 && name.size() > 4 && name.substr(name.size() - 4) == ".vdf" &&
        it->is_regular_file(type_error) && !it->is_symlink(type_error))
      scripts.push_back(it->path());
  }
  std::sort(scripts.begin(), scripts.end());
  if (scripts.size() > kMaxScripts) scripts.resize(kMaxScripts);
  InstallScriptValues values;
  for (const auto& path : scripts) {
    const auto name = path.filename().string();
    result.found.push_back(name);
    std::string text, reason;
    InstallScriptValues parsed;
    if (!ReadScript(path, &text) || !ParseInstallScript(text, install_dir, language, &parsed, &reason)) {
      result.skipped.push_back(name);
      continue;
    }
    values.machine.insert(values.machine.end(), parsed.machine.begin(), parsed.machine.end());
    values.user.insert(values.user.end(), parsed.user.begin(), parsed.user.end());
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
