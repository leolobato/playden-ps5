#include "runtime/prospero_profile.h"

#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <vector>

namespace playden::runtime {
namespace {

bool HasControl(std::string_view text) {
  for (const unsigned char value : text)
    if (value < 0x20 || value == 0x7f) return true;
  return false;
}

std::string Backslashes(std::string text) {
  for (auto& value : text)
    if (value == '/') value = '\\';
  return text;
}

// "/data/x/games/220" -> "Z:\data\x\games\220"; prospero-win maps Z: to the console root.
std::optional<std::string> HostToWindows(std::string_view root) {
  while (root.size() > 1 && root.back() == '/') root.remove_suffix(1);
  if (root.size() < 2 || root.front() != '/' || HasControl(root) ||
      root.find_first_of(":\\") != std::string_view::npos)
    return std::nullopt;
  std::size_t start = 1;
  while (start <= root.size()) {
    const auto end = std::min(root.find('/', start), root.size());
    const auto segment = root.substr(start, end - start);
    if (segment.empty() || segment == "." || segment == "..") return std::nullopt;
    start = end + 1;
  }
  return "Z:" + Backslashes(std::string(root));
}

// Cuts at a UTF-8 boundary so a long title stays valid text.
std::string Truncate(std::string text, std::size_t limit) {
  if (text.size() <= limit) return text;
  std::size_t cut = limit;
  while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
  text.resize(cut);
  return text;
}

std::string CleanName(std::string_view raw, std::uint32_t app_id) {
  std::string name;
  for (const unsigned char value : raw) name.push_back(value < 0x20 || value == 0x7f ? ' ' : static_cast<char>(value));
  name = Truncate(std::move(name), kProsperoMaxNameBytes);
  const auto first = name.find_first_not_of(' ');
  if (first == std::string::npos) return "Steam app " + std::to_string(app_id);
  name = name.substr(first, name.find_last_not_of(' ') - first + 1);
  return name;
}

bool WriteAtomically(const std::string& path, const std::string& text) {
  const auto temporary = path + ".tmp";
  FILE* file = std::fopen(temporary.c_str(), "w");
  if (!file) return false;
  const bool written = std::fwrite(text.data(), 1, text.size(), file) == text.size();
  if (std::fclose(file) != 0 || !written) {
    std::remove(temporary.c_str());
    return false;
  }
  return std::rename(temporary.c_str(), path.c_str()) == 0;
}

std::string_view Trim(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r')) text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) text.remove_suffix(1);
  return text;
}

std::string Lower(std::string_view text) {
  std::string out(text);
  for (auto& value : out) value = static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
  return out;
}

bool IsDigits(std::string_view text, std::size_t max_digits) {
  if (text.empty() || text.size() > max_digits) return false;
  return std::all_of(text.begin(), text.end(), [](char value) { return value >= '0' && value <= '9'; });
}

bool OneOf(std::string_view value, std::initializer_list<std::string_view> allowed) {
  const auto lower = Lower(value);
  return std::find(allowed.begin(), allowed.end(), std::string_view(lower)) != allowed.end();
}

bool IsIdentifier(std::string_view text) {
  if (text.empty() || text.size() > kProsperoMaxIdBytes) return false;
  if (!std::isalnum(static_cast<unsigned char>(text.front()))) return false;
  return text.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_-") == std::string_view::npos;
}

bool IsWindowsPath(std::string_view path) {
  if (path.size() < 3 || !std::isalpha(static_cast<unsigned char>(path[0])) || path.substr(1, 2) != ":\\")
    return false;
  std::size_t start = 3;
  while (start <= path.size()) {
    const auto end = std::min(path.find('\\', start), path.size());
    const auto segment = path.substr(start, end - start);
    if (segment.find_first_of("/:") != std::string_view::npos || HasControl(segment) || segment == "..")
      return false;
    if (segment.empty() && end != path.size()) return false;
    start = end + 1;
  }
  return true;
}

constexpr std::string_view kButtons[] = {"cross", "circle", "square", "triangle", "l1",  "r1",
                                         "l2",    "r2",     "l3",     "r3",       "up",  "down",
                                         "left",  "right",  "options", "create",  "touchpad"};

}  // namespace

std::optional<std::string> ProsperoWindowsPath(std::string_view host_path) { return HostToWindows(host_path); }

bool SplitProsperoArguments(std::string_view arguments, std::vector<std::string>* words) {
  std::vector<std::string> out;
  std::size_t index = 0;
  while (index < arguments.size()) {
    while (index < arguments.size() && (arguments[index] == ' ' || arguments[index] == '\t')) ++index;
    if (index >= arguments.size()) break;
    std::string word;
    bool quoted = false;
    while (index < arguments.size() && (quoted || (arguments[index] != ' ' && arguments[index] != '\t'))) {
      if (arguments[index] == '"') quoted = !quoted;
      else word.push_back(arguments[index]);
      ++index;
    }
    if (quoted) return false;
    out.push_back(std::move(word));
    if (out.size() > kProsperoMaxArgumentWords) return false;
  }
  if (words) *words = std::move(out);
  return true;
}

bool IsProsperoBinding(std::string_view raw) {
  static const std::set<std::string, std::less<>> keys = {
      "space", "enter", "escape", "tab", "backspace", "shift", "ctrl", "alt", "pause", "pageup", "pagedown",
      "end", "home", "left", "up", "right", "down", "insert", "delete", "semicolon", "equals", "comma",
      "minus", "period", "slash", "backquote", "lbracket", "backslash", "rbracket", "quote",
      "none", "mouse_left", "mouse_right", "mouse_middle"};
  const auto value = Lower(raw);
  if (keys.count(value)) return true;
  if (value.size() == 1 && std::isalnum(static_cast<unsigned char>(value[0]))) return true;
  if ((value.size() == 2 || value.size() == 3) && value[0] == 'f' && IsDigits(std::string_view(value).substr(1), 2)) {
    const int number = std::atoi(value.c_str() + 1);
    return number >= 1 && number <= 24;
  }
  if (value.size() == 7 && value.compare(0, 5, "vk:0x") == 0 && std::isxdigit(static_cast<unsigned char>(value[5])) &&
      std::isxdigit(static_cast<unsigned char>(value[6])))
    return std::strtol(value.c_str() + 5, nullptr, 16) != 0;
  return false;
}

bool IsProsperoDllOverrides(std::string_view value) {
  if (value.empty() || value.size() > kProsperoMaxDllOverrideBytes) return false;
  std::size_t start = 0;
  while (start <= value.size()) {
    const auto end = std::min(value.find(';', start), value.size());
    const auto entry = value.substr(start, end - start);
    const auto equals = entry.find('=');
    if (equals == std::string_view::npos || equals == 0) return false;
    const auto names = entry.substr(0, equals);
    const auto mode = entry.substr(equals + 1);
    if (names.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789,._*-") !=
            std::string_view::npos ||
        names.front() == ',' || names.back() == ',' || names.find(",,") != std::string_view::npos)
      return false;
    if (!(mode.empty() || mode == "n" || mode == "b" || mode == "d" || mode == "n,b" || mode == "b,n")) return false;
    start = end + 1;
  }
  return true;
}

bool IsProsperoEnvironmentEntry(std::string_view name, std::string_view value) {
  if (name.empty() || name.size() >= 64 || value.empty() || value.size() >= 128) return false;
  for (std::size_t i = 0; i < name.size(); ++i) {
    const char c = name[i];
    if (!((c >= 'A' && c <= 'Z') || c == '_' || (i > 0 && c >= '0' && c <= '9'))) return false;
  }
  const auto starts = [&](std::string_view prefix) { return name.size() > prefix.size() && name.substr(0, prefix.size()) == prefix; };
  if (!(starts("DXVK_") || starts("MESA_") || starts("RADV_") || starts("SDL_") || name == "WINE_CPU_TOPOLOGY")) return false;
  if (name == "DXVK_HUD" || name == "SDL_JOYSTICK_RAWINPUT") return false;
  for (const unsigned char c : value)
    if (c < 0x20 || c > 0x7e || c == '"' || c == '\'') return false;
  return true;
}

bool IsProsperoDebugEnvEntry(std::string_view name, std::string_view value) {
  if (name.empty() || name.size() >= 48 || value.empty() || value.size() >= 96) return false;
  for (const char c : name)
    if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return false;
  const auto starts = [&](std::string_view prefix) { return name.size() > prefix.size() && name.substr(0, prefix.size()) == prefix; };
  if (!(starts("PW_") || starts("DXVK_") || starts("MESA_") || starts("GALLIUM_") || starts("ZINK_") || starts("RADV_") ||
        starts("VK_")))
    return false;
  if (starts("PW_QPC_TSC_") || name == "PW_VK_BATCH" || name == "PW_INPUT_SHARED_FAST" || name == "GALLIUM_DRIVER" ||
      name == "GALLIUM_HUD" || name == "DXVK_HUD")
    return false;
  for (const unsigned char c : value)
    if (c < 0x21 || c > 0x7e || c == '"' || c == '\'' || c == ';') return false;
  return true;
}

bool CheckProsperoProfile(std::string_view text, std::string* reason, ProsperoProfileFeatures features) {
  const auto fail = [&](std::string why) {
    if (reason) *reason = std::move(why);
    return false;
  };
  if (text.empty()) return fail("empty profile");
  if (text.size() > kProsperoMaxProfileBytes) return fail("profile is larger than 8192 bytes");
  std::vector<std::string> sections;
  std::string section;
  std::set<std::string> seen;
  std::set<std::string> application;
  std::string id, prefix, runtime, executable, working;
  std::size_t start = 0, debug_env_count = 0;
  while (start < text.size()) {
    auto end = text.find_first_of("\r\n", start);
    if (end == std::string_view::npos) end = text.size();
    auto line = text.substr(start, end - start);
    start = end + 1;
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) line.remove_suffix(1);
    if (line.empty() || line.front() == ';' || line.front() == '#') continue;
    if (line.front() == '[') {
      if (line.size() < 3 || line.back() != ']') return fail("bad section line");
      const auto name = Lower(line.substr(1, line.size() - 2));
      if ((name != "application" && name != "display" && name != "input" && name != "debug" &&
           !(features.environment_section && name == "environment") &&
           !(features.audio_section && name == "audio") && !(features.runtime_version && name == "runtime")) ||
          std::find(sections.begin(), sections.end(), name) != sections.end())
        return fail("section [" + name + "] unknown or repeated");
      if (sections.empty() != (name == "application")) return fail("[application] must come first");
      sections.push_back(name);
      section = name;
      seen.clear();
      continue;
    }
    const auto equals = line.find('=');
    if (equals == std::string_view::npos) return fail("line without '='");
    auto key_view = line.substr(0, equals);
    auto value = line.substr(equals + 1);
    while (!key_view.empty() && (key_view.back() == ' ' || key_view.back() == '\t')) key_view.remove_suffix(1);
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
    if (key_view.empty()) return fail("line without a key");
    if (section.empty()) return fail("line before any section");
    if (section == "environment") {
      // Names are case-sensitive here, as in the environment itself.
      if (!seen.insert(std::string(key_view)).second) return fail("duplicate " + std::string(key_view));
      if (!IsProsperoEnvironmentEntry(key_view, value)) return fail("environment " + std::string(key_view) + " is not allowed");
      if (seen.size() > kProsperoMaxEnvironmentEntries) return fail("more than 8 environment variables");
      continue;
    }
    const auto key = Lower(key_view);
    // [debug] env repeats, once per variable; each variable once.
    const bool env_line = section == "debug" && key == "env" && features.debug_env;
    if (!env_line && !seen.insert(key).second) return fail("duplicate " + key);
    const auto bad = [&](const char* what) { return fail(key + ": " + what); };
    if (section == "application") {
      application.insert(key);
      const auto text_field = [&](std::size_t limit, bool allow_empty) {
        return (allow_empty || !value.empty()) && value.size() <= limit && !HasControl(value);
      };
      if (key == "id" || key == "prefix" || key == "runtime") {
        if (!text_field(kProsperoMaxIdBytes, false) || !IsIdentifier(value)) return bad("not a lowercase identifier");
        (key == "id" ? id : key == "prefix" ? prefix : runtime) = std::string(value);
      } else if (key == "name") {
        if (!text_field(kProsperoMaxNameBytes, false)) return bad("empty or too long");
      } else if (key == "executable" || key == "working_directory") {
        if (!text_field(kProsperoMaxPathBytes, false) || !IsWindowsPath(value)) return bad("not an absolute Windows path");
        (key == "executable" ? executable : working) = std::string(value);
      } else if (key == "arguments") {
        if (!text_field(kProsperoMaxArgumentBytes, true) || !SplitProsperoArguments(value, nullptr))
          return bad("too long, too many words or an unclosed quote");
      } else if (key == "dll_overrides") {
        if (HasControl(value) || !IsProsperoDllOverrides(value)) return bad("not a DLL override list");
      } else if (key == "startup_command_id") {
        if (!IsDigits(value, 5) || std::atoi(std::string(value).c_str()) > 0xFFFF) return bad("not a number up to 65535");
      } else if (key == "architecture") {
        if (!OneOf(value, {"pe32", "pe64"})) return bad("not pe32 or pe64");
      } else if (key == "graphics") {
        if (!OneOf(value, {"auto", "gdi", "dxvk", "opengl"})) return bad("unknown graphics mode");
      } else {
        return fail("unknown [application] key " + key);
      }
    } else if (section == "display") {
      if (key == "desktop") {
        const auto x = value.find('x');
        if (x == std::string_view::npos || !IsDigits(value.substr(0, x), 9) || !IsDigits(value.substr(x + 1), 9))
          return bad("not WIDTHxHEIGHT");
        const long width = std::atol(std::string(value.substr(0, x)).c_str());
        const long height = std::atol(std::string(value.substr(x + 1)).c_str());
        if (width < 320 || width > 3840 || height < 200 || height > 2160) return bad("outside 320x200-3840x2160");
      } else if (key == "scaling") {
        if (!OneOf(value, {"fit", "integer", "stretch"})) return bad("unknown scaling");
      } else if (key == "view") {
        if (!OneOf(value, {"window", "desktop"})) return bad("unknown view");
      } else if (key == "show_fps" || key == "opengl_thread") {
        if (!OneOf(value, {"true", "false"})) return bad("not true or false");
      } else if (key == "refresh") {
        if (value != "60" && value != "120") return bad("not 60 or 120");
      } else if (features.display_frame_rate && key == "frame_rate") {
        if (!OneOf(value, {"0", "120", "60", "40", "30"})) return bad("not 0, 120, 60, 40 or 30");
      } else if (features.display_framegen && key == "framegen") {
        if (!OneOf(value, {"off", "2", "3", "4"})) return bad("not off, 2, 3 or 4");
      } else {
        return fail("unknown [display] key " + key);
      }
    } else if (section == "input") {
      if (key == "preset") {
        if (!IsIdentifier(value)) return bad("not a preset name");
      } else if (key == "mode") {
        if (!OneOf(value, {"keyboard", "xinput"})) return bad("not keyboard or xinput");
      } else if (key == "mouse") {
        if (!OneOf(value, {"none", "left_stick", "right_stick"})) return bad("unknown pointer stick");
      } else if (key == "mouse_speed") {
        const long speed = IsDigits(value, 9) ? std::atol(std::string(value).c_str()) : 0;
        if (speed < 1 || speed > 20000) return bad("not 1-20000");
      } else if (features.input_wheel && key == "wheel") {
        if (!OneOf(value, {"none", "left_stick", "right_stick"})) return bad("unknown wheel stick");
      } else if ((features.input_tuning && (key == "stick_deadzone" || key == "trigger_deadzone")) ||
                 ((features.input_tuning || features.input_rumble) && key == "rumble")) {
        const long maximum = key == "rumble" ? 100 : 50;
        if (!IsDigits(value, 3) || std::atol(std::string(value).c_str()) > maximum)
          return bad(key == "rumble" ? "not 0-100" : "not 0-50");
      } else if (std::find(std::begin(kButtons), std::end(kButtons), key) != std::end(kButtons)) {
        if (!IsProsperoBinding(value)) return bad("not a key, mouse button or none");
      } else {
        return fail("unknown [input] key " + key);
      }
    } else if (section == "runtime") {
      // Only the profile's version: the defaults the title applies (prospero-win's [runtime] has more keys).
      if (key != "version") return fail("unknown [runtime] key " + key);
      if (value != "1" && value != "2") return bad("not 1 or 2");
    } else if (section == "audio") {
      if (key != "volume") return fail("unknown [audio] key " + key);
      if (!IsDigits(value, 3) || std::atol(std::string(value).c_str()) > 100) return bad("not 0-100");
    } else {
      if (key == "env" && features.debug_env) {
        const auto equals = value.find('=');
        if (equals == std::string_view::npos || !IsProsperoDebugEnvEntry(value.substr(0, equals), value.substr(equals + 1)))
          return bad("not NAME=value with a variable the runtime takes");
        if (!seen.insert("env:" + std::string(value.substr(0, equals))).second) return bad("variable set twice");
        if (++debug_env_count > kProsperoMaxDebugEnvEntries) return bad("more than 5 variables");
        continue;
      }
      if (key != "winedebug") return fail("unknown [debug] key " + key);
      if (value.empty() || value.size() > kProsperoMaxWinedebugBytes ||
          value.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_+-,=.") !=
              std::string_view::npos)
        return bad("not a channel list");
    }
  }
  if (sections.empty()) return fail("no [application] section");
  for (const char* required : {"id", "name", "executable", "working_directory", "prefix", "runtime", "architecture", "graphics"})
    if (!application.count(required)) return fail(std::string("missing ") + required);
  if (Lower(executable).size() < 4 || Lower(executable).compare(Lower(executable).size() - 4, 4, ".exe") != 0)
    return fail("executable does not end in .exe");
  return true;
}

std::optional<PeArchitecture> ParsePeArchitecture(const std::uint8_t* bytes, std::size_t size) {
  if (!bytes || size < 0x40 || bytes[0] != 'M' || bytes[1] != 'Z') return std::nullopt;
  const std::uint32_t offset = bytes[0x3c] | bytes[0x3d] << 8 | bytes[0x3e] << 16 |
                               static_cast<std::uint32_t>(bytes[0x3f]) << 24;
  if (offset > size || size - offset < 6) return std::nullopt;
  const auto* header = bytes + offset;
  if (header[0] != 'P' || header[1] != 'E' || header[2] != 0 || header[3] != 0) return std::nullopt;
  const unsigned machine = header[4] | header[5] << 8;
  if (machine == 0x014c) return PeArchitecture::kPe32;
  if (machine == 0x8664) return PeArchitecture::kPe64;
  return std::nullopt;
}

std::optional<PeArchitecture> ReadPeArchitecture(const std::string& path) {
  FILE* file = std::fopen(path.c_str(), "rb");
  if (!file) return std::nullopt;
  // The PE header follows the DOS stub, normally within the first few hundred bytes.
  std::vector<std::uint8_t> head(4096);
  head.resize(std::fread(head.data(), 1, head.size(), file));
  std::fclose(file);
  return ParsePeArchitecture(head.data(), head.size());
}

std::string ProsperoProfileId(std::uint32_t app_id) { return "playden-steam-" + std::to_string(app_id); }

bool DraftProsperoProfile(const ProsperoGame& game, ProsperoProfile* profile, std::string* reason) {
  const auto fail = [&](const char* why) {
    if (reason) *reason = why;
    return false;
  };
  if (game.app_id == 0) return fail("missing app id");
  const auto root = HostToWindows(game.install_root);
  if (!root) return fail("install folder cannot be mapped for Wine");
  const auto executable = steam::RelativeGamePath(game.launch.executable);
  if (!executable) return fail("launch executable is not a game path");
  std::string working;
  if (game.launch.working_directory.empty()) {
    const auto slash = executable->rfind('/');
    working = slash == std::string::npos ? std::string() : executable->substr(0, slash);
  } else {
    const auto relative = steam::RelativeGamePath(game.launch.working_directory, true);
    if (!relative) return fail("launch working folder is not a game path");
    working = *relative;
  }
  if (HasControl(game.launch.arguments) || !steam::ParseWindowsArguments(game.launch.arguments))
    return fail("launch arguments are malformed");
  if (game.launch.arguments.size() > kProsperoMaxArgumentBytes) return fail("launch arguments are too long");

  const auto executable_path = *root + "\\" + Backslashes(*executable);
  const auto working_path = working.empty() ? *root : *root + "\\" + Backslashes(working);
  if (executable_path.size() > kProsperoMaxPathBytes || working_path.size() > kProsperoMaxPathBytes)
    return fail("install path is too long for prospero-win");
  if (game.prefix.empty() || game.prefix.size() > kProsperoMaxIdBytes ||
      game.prefix.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-_") != std::string::npos ||
      game.prefix.front() == '-' || game.prefix.front() == '_')
    return fail("invalid prefix name");

  ProsperoProfile drafted;
  drafted.id = ProsperoProfileId(game.app_id);
  std::string text = "; Drafted by Playden from Steam launch data.\n[application]\n";
  text += "id = " + drafted.id + "\n";
  text += "name = " + CleanName(game.name, game.app_id) + "\n";
  text += "executable = " + executable_path + "\n";
  text += "working_directory = " + working_path + "\n";
  if (!game.launch.arguments.empty()) text += "arguments = " + game.launch.arguments + "\n";
  text += "prefix = " + game.prefix + "\n";
  text += "runtime = wine-wow64\n";
  text += std::string("architecture = ") + (game.architecture == PeArchitecture::kPe64 ? "pe64" : "pe32") + "\n";
  text += "graphics = auto\n";
  if (text.size() > kProsperoMaxProfileBytes) return fail("profile is too large");
  drafted.text = std::move(text);
  *profile = std::move(drafted);
  return true;
}

bool PublishProsperoProfile(const std::string& prospero_root, const ProsperoProfile& profile, std::string* reason) {
  const auto fail = [&](const char* why) {
    if (reason) *reason = why;
    return false;
  };
  const auto directory = prospero_root + "/profiles";
  mkdir(prospero_root.c_str(), 0777);
  mkdir(directory.c_str(), 0777);
  const auto entry = profile.id + ".profile";
  if (!WriteAtomically(directory + "/" + entry, profile.text)) return fail("cannot write profile");

  const auto catalog_path = directory + "/profiles.lst";
  std::string catalog;
  if (FILE* file = std::fopen(catalog_path.c_str(), "r")) {
    char buffer[1024];
    std::size_t read = 0;
    while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0 && catalog.size() < 64 * 1024)
      catalog.append(buffer, read);
    std::fclose(file);
  }
  std::size_t entries = 0;
  std::size_t start = 0;
  while (start < catalog.size()) {
    const auto end = std::min(catalog.find('\n', start), catalog.size());
    const auto line = Trim(std::string_view(catalog).substr(start, end - start));
    if (!line.empty() && line.front() != '#' && line.front() != ';') {
      if (line == entry) return true;
      ++entries;
    }
    start = end + 1;
  }
  if (entries >= kProsperoMaxCatalogEntries) return fail("prospero-win profile list is full");
  if (!catalog.empty() && catalog.back() != '\n') catalog += '\n';
  catalog += entry + "\n";
  if (!WriteAtomically(catalog_path, catalog)) return fail("cannot update profile list");
  return true;
}

}  // namespace playden::runtime
