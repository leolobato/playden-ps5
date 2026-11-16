#include "runtime/publisher_manifest.h"

#include "steam/backend/json.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <fcntl.h>
#include <iterator>
#include <map>
#include <set>
#include <sys/stat.h>
#include <unistd.h>

namespace playden::runtime {
namespace {

namespace json = steam::json;
using json::Value;

constexpr std::size_t kMaxPathBytes = 512;
constexpr std::size_t kMaxFiles = 64;
constexpr std::size_t kMaxFieldNames = 16;
constexpr std::size_t kMaxBlobLimit = 64 * 1024;
constexpr std::size_t kMaxTitleVars = 16;
constexpr std::size_t kMaxTitleVarBytes = 256;

bool Fail(std::string* reason, std::string text) {
  if (reason) *reason = "stage.json: " + std::move(text);
  return false;
}

std::string Lower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c); });
  return text;
}

bool HasControl(std::string_view text) {
  return std::any_of(text.begin(), text.end(), [](unsigned char c) { return c < 0x20 || c == 0x7f; });
}

// The publisher pack's path rules (publisher_pack.cpp SafeArchivePath): relative, '/' separators,
// no drive, backslash, control characters, or empty, "." and ".." parts. Hidden parts are allowed:
// the manifest itself is the list of the ones a publisher needs.
bool StagePath(std::string_view path) {
  if (path.empty() || path.size() > kMaxPathBytes || path.front() == '/' || HasControl(path)) return false;
  if (path.find(':') != std::string_view::npos || path.find('\\') != std::string_view::npos) return false;
  std::size_t begin = 0;
  for (;;) {
    const auto end = path.find('/', begin);
    const auto part = path.substr(begin, end == std::string_view::npos ? std::string_view::npos : end - begin);
    if (part.empty() || part == "." || part == "..") return false;
    if (end == std::string_view::npos) return true;
    begin = end + 1;
  }
}

bool UniqueKeys(const Value& object) {
  std::set<std::string_view> keys;
  for (const auto& entry : object.object())
    if (!keys.insert(entry.first).second) return false;
  return true;
}

// An object member that must be an object without repeated keys (nullptr when absent).
bool Object(const Value& parent, const char* key, bool required, const Value** out, const std::string& where,
            std::string* reason) {
  *out = parent.Find(key);
  if (!*out) return !required || Fail(reason, where + key + " is missing");
  if (!(*out)->is_object() || !UniqueKeys(**out)) return Fail(reason, where + key + " must be an object");
  return true;
}

bool String(const Value& parent, const char* key, bool required, std::string* out, const std::string& where,
            std::string* reason) {
  const auto* value = parent.Find(key);
  if (!value) return !required || Fail(reason, where + key + " is missing");
  if (!value->is_string()) return Fail(reason, where + key + " must be a string");
  *out = value->text();
  return true;
}

bool Path(const Value& parent, const char* key, bool required, std::string* out, const std::string& where,
          std::string* reason) {
  if (!String(parent, key, required, out, where, reason)) return false;
  if (parent.Find(key) && !StagePath(*out)) return Fail(reason, where + key + " is not a safe relative path");
  return true;
}

bool Size(const Value& parent, const char* key, std::size_t* out, const std::string& where, std::string* reason) {
  const auto* value = parent.Find(key);
  if (!value) return Fail(reason, where + key + " is missing");
  const auto number = value->AsUint64();
  if (!number || *number > kMaxBlobLimit) return Fail(reason, where + key + " must be a whole number up to 65536");
  *out = static_cast<std::size_t>(*number);
  return true;
}

bool Names(const Value& parent, const char* key, std::size_t minimum, std::vector<std::string>* out,
           const std::string& where, std::string* reason) {
  const auto* value = parent.Find(key);
  if (!value) return Fail(reason, where + key + " is missing");
  if (!value->is_array() || value->array().size() < minimum || value->array().size() > kMaxFieldNames)
    return Fail(reason, where + key + " must be a list of 1 to 16 names");
  for (const auto& name : value->array()) {
    if (!name.is_string() || name.text().empty() || HasControl(name.text()))
      return Fail(reason, where + key + " must hold non-empty names");
    out->push_back(name.text());
  }
  return true;
}

bool Arch(const Value& parent, const char* key, std::optional<StageArch>* out, bool required,
          const std::string& where, std::string* reason) {
  std::string text;
  if (!String(parent, key, required, &text, where, reason)) return false;
  if (!parent.Find(key)) return true;
  if (text == "x86") *out = StageArch::kX86;
  else if (text == "x64") *out = StageArch::kX64;
  else return Fail(reason, where + key + " must be x86 or x64");
  return true;
}

bool ParseAccount(const Value& root, PublisherAccountSpec* account, std::string* reason) {
  const Value* object = nullptr;
  if (!Object(root, "account", true, &object, "", reason)) return false;
  const std::string at = "account.";
  if (!String(*object, "name", true, &account->name, at, reason)) return false;
  if (account->name.empty() || HasControl(account->name)) return Fail(reason, "account.name must be a name");
  if (!Names(*object, "display_fields", 0, &account->display_fields, at, reason) ||
      !Names(*object, "stage_fields", 1, &account->stage_fields, at, reason) ||
      !Size(*object, "max_bytes", &account->max_bytes, at, reason))
    return false;
  const Value* pattern = nullptr;
  if (!Object(*object, "stage_pattern", true, &pattern, at, reason)) return false;
  std::string charset;
  const std::string pattern_at = "account.stage_pattern.";
  if (!String(*pattern, "charset", true, &charset, pattern_at, reason)) return false;
  if (charset != "base64") return Fail(reason, "account.stage_pattern.charset must be base64");
  if (!Size(*pattern, "min", &account->stage_min, pattern_at, reason) ||
      !Size(*pattern, "max", &account->stage_max, pattern_at, reason))
    return false;
  if (account->stage_min == 0 || account->stage_max < account->stage_min)
    return Fail(reason, "account.stage_pattern needs 1 <= min <= max");
  if (account->max_bytes == 0 || account->stage_max > account->max_bytes)
    return Fail(reason, "account.max_bytes must hold the longest stage value");
  return true;
}

bool ParseRuntime(const Value& root, std::optional<PublisherRuntimeSpec>* runtime, std::string* reason) {
  const Value* object = nullptr;
  if (!Object(root, "runtime", false, &object, "", reason)) return false;
  if (!object) return true;
  PublisherRuntimeSpec spec;
  const std::string at = "runtime.";
  if (!String(*object, "label", true, &spec.label, at, reason)) return false;
  if (spec.label.empty() || HasControl(spec.label)) return Fail(reason, "runtime.label must be a name");
  std::vector<std::string> installers;
  if (!Names(*object, "installers", 1, &installers, at, reason)) return false;
  for (const auto& path : installers)
    if (!StagePath(path)) return Fail(reason, "runtime.installers holds an unsafe path: " + path);
  spec.installers = std::move(installers);
  if (!Path(*object, "version_file", true, &spec.version_file, at, reason)) return false;
  const Value* folders = nullptr;
  if (!Object(*object, "folders", true, &folders, at, reason)) return false;
  if (folders->object().empty()) return Fail(reason, "runtime.folders is empty");
  for (const auto& [from, to] : folders->object()) {
    if (!StagePath(from) || from.find('/') != std::string::npos)
      return Fail(reason, "runtime.folders keys must be single folder names: " + from);
    if (!to.is_string() || !StagePath(to.text()))
      return Fail(reason, "runtime.folders." + from + " is not a safe relative path");
    spec.folders[from] = to.text();
  }
  *runtime = std::move(spec);
  return true;
}

bool ParseTitles(const Value& root, std::map<std::string, PublisherTitleSpec>* titles, std::string* reason) {
  const Value* object = nullptr;
  if (!Object(root, "titles", true, &object, "", reason)) return false;
  for (const auto& [id, value] : object->object()) {
    const bool digits = !id.empty() && id.size() <= 10 && id[0] != '0' &&
                        std::all_of(id.begin(), id.end(), [](char c) { return c >= '0' && c <= '9'; });
    if (!digits) return Fail(reason, "titles keys must be Steam app ids: " + id);
    const std::string at = "titles." + id + ".";
    if (!value.is_object() || !UniqueKeys(value)) return Fail(reason, "titles." + id + " must be an object");
    std::optional<StageArch> arch;
    PublisherTitleSpec title;
    if (!Arch(value, "arch", &arch, true, at, reason) || !String(value, "args", true, &title.args, at, reason))
      return false;
    if (HasControl(title.args)) return Fail(reason, at + "args holds a control character");
    if (!Path(value, "program", false, &title.program, at, reason)) return false;
    const Value* vars = nullptr;
    if (!Object(value, "vars", false, &vars, at, reason)) return false;
    if (vars) {
      if (vars->object().size() > kMaxTitleVars) return Fail(reason, at + "vars holds more than 16 values");
      for (const auto& [key, text] : vars->object()) {
        const bool name = !key.empty() && key.size() <= 32 && key != "args" &&
                          std::all_of(key.begin(), key.end(),
                                      [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; });
        if (!name) return Fail(reason, at + "vars keys must be 1 to 32 of a-z, 0-9 and _ (not args): " + key);
        if (!text.is_string() || text.text().size() > kMaxTitleVarBytes || HasControl(text.text()))
          return Fail(reason, at + "vars." + key + " must be a single-line string of up to 256 bytes");
        title.vars[key] = text.text();
      }
    }
    title.arch = *arch;
    (*titles)[id] = std::move(title);
  }
  return true;
}

bool ParseFiles(const Value& root, std::vector<PublisherStageFile>* files, std::string* reason) {
  const auto* value = root.Find("files");
  if (!value) return Fail(reason, "files is missing");
  if (!value->is_array() || value->array().size() > kMaxFiles) return Fail(reason, "files must be a list of up to 64 entries");
  for (std::size_t index = 0; index < value->array().size(); ++index) {
    const auto& entry = value->array()[index];
    const std::string at = "files[" + std::to_string(index) + "].";
    if (!entry.is_object() || !UniqueKeys(entry)) return Fail(reason, "files[" + std::to_string(index) + "] must be an object");
    PublisherStageFile file;
    std::string text;
    if (entry.Find("source")) {
      file.source.emplace();
      if (!Path(entry, "source", true, &*file.source, at, reason)) return false;
    }
    if (entry.Find("text")) {
      if (!String(entry, "text", true, &text, at, reason)) return false;
      file.text = std::move(text);
    }
    if (file.source.has_value() == file.text.has_value()) return Fail(reason, at + "needs exactly one of source and text");
    if (!Path(entry, "target", true, &file.target, at, reason) ||
        !Path(entry, "keep_original", false, &file.keep_original, at, reason) ||
        !Arch(entry, "arch", &file.arch, false, at, reason))
      return false;
    files->push_back(std::move(file));
  }
  return true;
}

// Replaces ${title.args}, ${title.<key>} and ${runtime} in one line.
bool ExpandLine(const std::string& line, const std::string& args, const std::map<std::string, std::string>& vars,
                const std::string& runtime, bool has_runtime, std::string* out, std::string* reason) {
  out->clear();
  std::size_t at = 0;
  for (;;) {
    const auto open = line.find("${", at);
    out->append(line, at, open == std::string::npos ? std::string::npos : open - at);
    if (open == std::string::npos) return true;
    const auto close = line.find('}', open + 2);
    if (close == std::string::npos) return Fail(reason, "ini line has an unfinished variable: " + line);
    const auto name = line.substr(open + 2, close - open - 2);
    const auto var = name.rfind("title.", 0) == 0 ? vars.find(name.substr(6)) : vars.end();
    if (name == "title.args") {
      out->append(args);
    } else if (var != vars.end()) {
      out->append(var->second);
    } else if (name == "runtime") {
      if (!has_runtime) return Fail(reason, "ini uses ${runtime} without a runtime");
      out->append(runtime);
    } else {
      return Fail(reason, "ini uses an unknown variable: ${" + name + "}");
    }
    at = close + 1;
  }
}

bool ParseIni(const Value& root, const PublisherStageManifest& manifest, std::optional<PublisherStageIni>* ini,
              std::string* reason) {
  const Value* object = nullptr;
  if (!Object(root, "ini", false, &object, "", reason)) return false;
  if (!object) return true;
  PublisherStageIni spec;
  if (!Path(*object, "target", true, &spec.target, "ini.", reason)) return false;
  const auto* lines = object->Find("lines");
  if (!lines || !lines->is_array()) return Fail(reason, "ini.lines must be a list");
  // A ${title.<key>} every title defines, so no title meets an unknown variable.
  std::map<std::string, std::string> shared;
  for (auto title = manifest.titles.begin(); title != manifest.titles.end(); ++title) {
    if (title == manifest.titles.begin()) {
      shared = title->second.vars;
      continue;
    }
    for (auto key = shared.begin(); key != shared.end();)
      key = title->second.vars.count(key->first) ? std::next(key) : shared.erase(key);
  }
  for (const auto& line : lines->array()) {
    if (!line.is_string() || HasControl(line.text())) return Fail(reason, "ini.lines must hold single-line strings");
    std::string expanded;
    if (!ExpandLine(line.text(), "", shared, "", manifest.runtime.has_value(), &expanded, reason)) {
      if (reason && line.text().find("${title.") != std::string::npos) *reason += " (a ${title.<key>} needs the key in every title's vars)";
      return false;
    }
    spec.lines.push_back(line.text());
  }
  *ini = std::move(spec);
  return true;
}

bool ParseStatus(const Value& root, std::optional<PublisherStatusSpec>* status, std::string* reason) {
  const Value* object = nullptr;
  if (!Object(root, "status", false, &object, "", reason)) return false;
  if (!object) return true;
  PublisherStatusSpec spec;
  if (!Path(*object, "file", true, &spec.file, "status.", reason) ||
      !String(*object, "expired", true, &spec.expired, "status.", reason) ||
      !String(*object, "missing", false, &spec.missing, "status.", reason))
    return false;
  const auto trimmed_line = [](const std::string& value) {
    return !value.empty() && !HasControl(value) && value.front() != ' ' && value.back() != ' ';
  };
  if (!trimmed_line(spec.expired)) return Fail(reason, "status.expired must be a single trimmed line");
  if (object->Find("missing") && !trimmed_line(spec.missing))
    return Fail(reason, "status.missing must be a single trimmed line");
  if (spec.missing == spec.expired) return Fail(reason, "status.missing must differ from status.expired");
  *status = std::move(spec);
  return true;
}

bool ParseHelper(const Value& root, std::optional<PublisherHelperSpec>* helper, std::string* reason) {
  const Value* object = nullptr;
  if (!Object(root, "helper", false, &object, "", reason)) return false;
  if (!object) return true;
  for (const auto& entry : object->object())
    if (entry.first != "program" && entry.first != "ready" && entry.first != "wait" && entry.first != "identity")
      return Fail(reason, "unknown key helper." + entry.first);
  PublisherHelperSpec spec;
  if (!Path(*object, "program", true, &spec.program, "helper.", reason) ||
      !Path(*object, "ready", true, &spec.ready, "helper.", reason))
    return false;
  const auto lower = Lower(spec.program);
  if (lower.size() < 5 || lower.compare(lower.size() - 4, 4, ".exe") != 0) return Fail(reason, "helper.program must be an .exe");
  for (const unsigned char c : spec.ready)
    if (!std::isalnum(c) && c != '.' && c != '-' && c != '_' && c != '/') return Fail(reason, "helper.ready holds a character the runtime refuses");
  if (const auto* wait = object->Find("wait")) {
    const auto seconds = wait->AsUint64();
    if (!seconds || *seconds < 1 || *seconds > 120) return Fail(reason, "helper.wait must be 1-120");
    spec.wait = static_cast<std::uint32_t>(*seconds);
  }
  if (const auto* identity = object->Find("identity")) {
    if (!identity->is_string() || identity->text() != "launcher") return Fail(reason, "helper.identity must be \"launcher\"");
    spec.identity = identity->text();
  }
  *helper = std::move(spec);
  return true;
}

}  // namespace

bool ParsePublisherStageManifest(std::string_view text, PublisherStageManifest* manifest, std::string* reason) {
  if (text.size() > kMaxPublisherStageManifestBytes) return Fail(reason, "larger than 64 KiB");
  json::Limits limits;
  limits.max_bytes = kMaxPublisherStageManifestBytes;
  const auto parsed = json::Parse(text, limits);
  if (!parsed) return Fail(reason, "not valid JSON");
  if (!parsed->is_object() || !UniqueKeys(*parsed)) return Fail(reason, "must be a JSON object");
  static const std::set<std::string_view> kKeys = {"version", "account", "signin", "runtime", "titles",
                                                   "files", "ini", "credential_file", "status",
                                                   "covered_installers", "helper"};
  for (const auto& entry : parsed->object())
    if (!kKeys.count(entry.first)) return Fail(reason, "unknown key " + entry.first);
  const auto* version = parsed->Find("version");
  if (!version || version->AsUint64() != std::optional<std::uint64_t>(1)) return Fail(reason, "version must be 1");

  PublisherStageManifest result;
  if (!ParseAccount(*parsed, &result.account, reason)) return false;
  const Value* signin = nullptr;
  if (!Object(*parsed, "signin", false, &signin, "", reason)) return false;
  if (signin && !Path(*signin, "page", true, &result.signin_page, "signin.", reason)) return false;
  if (!ParseRuntime(*parsed, &result.runtime, reason) || !ParseTitles(*parsed, &result.titles, reason) ||
      !ParseFiles(*parsed, &result.files, reason) || !ParseIni(*parsed, result, &result.ini, reason) ||
      !Path(*parsed, "credential_file", true, &result.credential_file, "", reason) ||
      !ParseStatus(*parsed, &result.status, reason) || !ParseHelper(*parsed, &result.helper, reason))
    return false;
  if (const auto* covered = parsed->Find("covered_installers")) {
    if (!covered->is_array() || covered->array().size() > kMaxFieldNames)
      return Fail(reason, "covered_installers must be a list of up to 16 paths");
    for (const auto& path : covered->array()) {
      if (!path.is_string() || !StagePath(path.text()))
        return Fail(reason, "covered_installers holds an unsafe path");
      result.covered_installers.push_back(path.text());
    }
  }

  // Every path Playden writes in the title folder is distinct (names compared without case), per
  // architecture: a files entry for x86 titles and one for x64 titles may share a path, as at most
  // one of them applies to a title.
  std::map<std::string, std::set<StageArch>> written;
  const auto claim = [&](const std::string& path, std::optional<StageArch> arch = std::nullopt) {
    auto& taken = written[Lower(path)];
    const std::set<StageArch> wanted = arch ? std::set<StageArch>{*arch} : std::set<StageArch>{StageArch::kX86, StageArch::kX64};
    for (const auto one : wanted)
      if (!taken.insert(one).second) return false;
    return true;
  };
  for (const auto& file : result.files) {
    if (!claim(file.target, file.arch)) return Fail(reason, "the title folder path " + file.target + " is used twice");
    if (!file.keep_original.empty() && !claim(file.keep_original, file.arch))
      return Fail(reason, "the title folder path " + file.keep_original + " is used twice");
  }
  if (result.ini && !claim(result.ini->target)) return Fail(reason, "the title folder path " + result.ini->target + " is used twice");
  if (!claim(result.credential_file)) return Fail(reason, "the title folder path " + result.credential_file + " is used twice");
  if (result.status && !claim(result.status->file)) return Fail(reason, "the title folder path " + result.status->file + " is used twice");
  *manifest = std::move(result);
  return true;
}

bool LoadPublisherStageManifest(const std::string& directory, PublisherStageManifest* manifest, std::string* reason) {
  const auto path = directory + "/" + kPublisherStageManifestName;
  const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) return Fail(reason, "cannot open " + path);
  struct stat info {};
  std::string bytes;
  bool ok = ::fstat(fd, &info) == 0 && S_ISREG(info.st_mode);
  if (ok && static_cast<std::uint64_t>(info.st_size) > kMaxPublisherStageManifestBytes) {
    ::close(fd);
    return Fail(reason, "larger than 64 KiB");
  }
  if (ok) {
    bytes.resize(static_cast<std::size_t>(info.st_size));
    std::size_t at = 0;
    while (at < bytes.size()) {
      const auto count = ::read(fd, bytes.data() + at, bytes.size() - at);
      if (count < 0 && errno == EINTR) continue;
      if (count <= 0) break;
      at += static_cast<std::size_t>(count);
    }
    ok = at == bytes.size();
  }
  ::close(fd);
  if (!ok) return Fail(reason, "cannot read " + path);
  return ParsePublisherStageManifest(bytes, manifest, reason);
}

bool ExpandPublisherStageIni(const PublisherStageManifest& manifest, const PublisherTitleSpec& title,
                             const std::string& runtime_path, std::vector<std::string>* lines, std::string* reason) {
  lines->clear();
  if (!manifest.ini) return true;
  if (HasControl(title.args) || HasControl(runtime_path) ||
      std::any_of(title.vars.begin(), title.vars.end(), [](const auto& var) { return HasControl(var.second); }))
    return Fail(reason, "an ini value holds a line break");
  const bool has_runtime = manifest.runtime.has_value() && !runtime_path.empty();
  for (const auto& line : manifest.ini->lines) {
    std::string expanded;
    if (!ExpandLine(line, title.args, title.vars, runtime_path, has_runtime, &expanded, reason)) {
      lines->clear();
      return false;
    }
    lines->push_back(std::move(expanded));
  }
  return true;
}

}  // namespace playden::runtime
