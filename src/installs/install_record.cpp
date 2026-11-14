// SPDX-License-Identifier: GPL-3.0-or-later
#include "installs/install_record.h"

#include "steam/backend/json.h"

#include <algorithm>
#include <cstdio>
#include <limits>

namespace playden::installs {
namespace {

using Value = steam::json::Value;

std::optional<std::uint32_t> AsUint32(const Value* value) {
  if (!value) return std::nullopt;
  const auto number = value->AsUint64();
  if (!number || *number > std::numeric_limits<std::uint32_t>::max()) return std::nullopt;
  return static_cast<std::uint32_t>(*number);
}

// A manifest GID is written as a decimal string (it doesn't fit a JSON number exactly); a number is
// accepted too.
std::optional<std::uint64_t> AsGid(const Value* value) {
  if (!value) return std::nullopt;
  if (value->is_number()) return value->AsUint64();
  if (!value->is_string() || value->text().empty() || value->text().size() > 20) return std::nullopt;
  std::uint64_t gid = 0;
  for (const char digit : value->text()) {
    if (digit < '0' || digit > '9') return std::nullopt;
    const std::uint64_t next = static_cast<std::uint64_t>(digit - '0');
    if (gid > (std::numeric_limits<std::uint64_t>::max() - next) / 10) return std::nullopt;
    gid = gid * 10 + next;
  }
  return gid;
}

bool HasControl(std::string_view text) {
  for (const char c : text)
    if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) return true;
  return false;
}

// A GOG ID in a record: canonical decimal (a string; a number is accepted too).
std::optional<std::uint64_t> AsGogId(const Value* value) {
  if (!value) return std::nullopt;
  if (value->is_number()) {
    const auto number = value->AsUint64();
    return number && *number != 0 ? number : std::nullopt;
  }
  return value->is_string() ? ParseGogValue(value->text()) : std::nullopt;
}

bool IsHash(std::string_view text) {
  if (text.size() != 32) return false;
  for (const char c : text)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  return true;
}

// The "gog" object; nothing when any of its fields is malformed.
std::optional<GogRecord> ParseGog(const Value& value) {
  if (!value.is_object()) return std::nullopt;
  GogRecord gog;
  const auto* build = value.Find("build_id");
  if (!build || !build->is_string() || build->text().empty() || build->text().size() > 64 || HasControl(build->text()))
    return std::nullopt;
  gog.build_id = build->text();
  if (const auto* generation = value.Find("generation")) {
    const auto number = generation->AsUint64();
    if (!number || (*number != 1 && *number != 2)) return std::nullopt;
    gog.generation = static_cast<int>(*number);
  }
  const auto* products = value.Find("products");
  if (!products || !products->is_array() || products->array().empty() ||
      products->array().size() > kMaxGogRecordProducts)
    return std::nullopt;
  for (const auto& entry : products->array()) {
    const auto id = AsGogId(&entry);
    if (!id) return std::nullopt;
    gog.products.push_back(*id);
  }
  if (const auto* language = value.Find("language")) {
    if (!language->is_string() || language->text().size() > 32 || HasControl(language->text())) return std::nullopt;
    gog.language = language->text();
  }
  if (const auto* meta = value.Find("meta")) {
    if (!meta->is_string() || (!meta->text().empty() && !IsHash(meta->text()))) return std::nullopt;
    gog.meta_id = meta->text();
  }
  // Optional: the DLC the player turned off. Kept sorted and unique, never the base game.
  if (const auto* excluded = value.Find("excluded")) {
    if (!excluded->is_array() || excluded->array().size() > kMaxGogRecordProducts) return std::nullopt;
    for (const auto& entry : excluded->array()) {
      const auto id = AsGogId(&entry);
      if (!id) return std::nullopt;
      if (*id != gog.products.front()) gog.excluded.push_back(*id);
    }
    std::sort(gog.excluded.begin(), gog.excluded.end());
    gog.excluded.erase(std::unique(gog.excluded.begin(), gog.excluded.end()), gog.excluded.end());
  }
  return gog;
}

// One string field of a launch: absent is empty; nothing when it isn't a string or is too long.
std::optional<std::string> LaunchText(const Value& entry, std::string_view key) {
  const auto* value = entry.Find(key);
  if (!value) return std::string();
  if (!value->is_string() || value->text().size() > kMaxRecordedLaunchBytes) return std::nullopt;
  return value->text();
}

std::optional<RecordedLaunch> ParseLaunch(const Value& entry) {
  if (!entry.is_object()) return std::nullopt;
  auto executable = LaunchText(entry, "executable");
  auto arguments = LaunchText(entry, "arguments");
  auto working = LaunchText(entry, "working_directory");
  auto label = LaunchText(entry, "label");
  if (!executable || executable->empty() || !arguments || !working || !label) return std::nullopt;
  return RecordedLaunch{std::move(*executable), std::move(*arguments), std::move(*working), std::move(*label)};
}

}  // namespace

std::optional<InstalledRecord> ParseInstallRecord(std::string_view text) {
  steam::json::Limits limits;
  limits.max_bytes = kMaxInstallRecordBytes;
  limits.max_values = 64 * 1024;
  const auto root = steam::json::Parse(text, limits);
  if (!root || !root->is_object()) return std::nullopt;
  InstalledRecord record;
  if (const auto version = AsUint32(root->Find("version"))) record.version = static_cast<int>(*version);
  if (const auto app = AsUint32(root->Find("app_id"))) record.app_id = *app;
  if (const auto* bytes = root->Find("install_bytes"))
    if (const auto value = bytes->AsUint64()) record.install_bytes = *value;
  if (const auto* branch = root->Find("branch"); branch && branch->is_string() && !branch->text().empty())
    record.branch = branch->text();
  if (const auto* language = root->Find("language"); language && language->is_string() && !language->text().empty())
    record.language = language->text();
  if (const auto* depots = root->Find("depots"); depots && depots->is_array()) {
    for (const auto& entry : depots->array()) {
      if (!entry.is_object()) return std::nullopt;
      const auto id = AsUint32(entry.Find("id"));
      if (!id || *id == 0) return std::nullopt;
      RecordedDepot depot;
      depot.id = *id;
      depot.gid = AsGid(entry.Find("manifest")).value_or(0);
      depot.owner = AsUint32(entry.Find("owner")).value_or(record.app_id);
      depot.source = AsUint32(entry.Find("source")).value_or(record.app_id);
      record.depots.push_back(depot);
    }
  }
  if (const auto* excluded = root->Find("excluded_dlc"); excluded && excluded->is_array())
    for (const auto& entry : excluded->array())
      if (const auto dlc = AsUint32(&entry); dlc && *dlc != 0) record.excluded_dlc.push_back(*dlc);
  if (const auto* gog = root->Find("gog")) record.gog = ParseGog(*gog);
  if (const auto* launches = root->Find("launches"); launches && launches->is_array())
    for (const auto& entry : launches->array()) {
      if (record.launches.size() >= kMaxRecordedLaunches) break;
      if (auto launch = ParseLaunch(entry)) record.launches.push_back(std::move(*launch));
    }
  return record;
}

namespace {
std::optional<std::string> ReadSmall(const std::string& path) {
  FILE* file = std::fopen(path.c_str(), "rb");
  if (!file) return std::nullopt;
  std::string text;
  char buffer[8192];
  bool too_large = false;
  for (std::size_t read = 0; (read = std::fread(buffer, 1, sizeof(buffer), file)) > 0;) {
    if (text.size() + read > kMaxInstallRecordBytes) {
      too_large = true;
      break;
    }
    text.append(buffer, read);
  }
  const bool failed = std::ferror(file) != 0;
  std::fclose(file);
  if (too_large || failed) return std::nullopt;
  return text;
}

std::string JsonText(const std::string& value) {
  std::string text;
  for (const char c : value) {
    if (c == '"' || c == '\\') {
      text += '\\';
      text += c;
    } else if (static_cast<unsigned char>(c) < 0x20) {
      char escaped[8];
      std::snprintf(escaped, sizeof(escaped), "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
      text += escaped;
    } else {
      text += c;
    }
  }
  return text;
}
}  // namespace

std::string FormatGogInstallRecord(const GameId& game, std::uint64_t install_bytes, std::uint64_t files,
                                   const std::string& steam_language, const GogRecord& gog,
                                   const std::vector<RecordedLaunch>& launches) {
  // install_bytes first after the identity, with no space: InstallService reads it from the first 4096 bytes.
  std::string text = "{\"version\":2,\"app_id\":0,\"store\":\"" + std::string(Traits(game.store).key) + "\",\"game\":\"" +
                     JsonText(game.value) + "\",\"install_bytes\":" + std::to_string(install_bytes) +
                     ",\"files\":" + std::to_string(files) + ",\"branch\":\"public\",\"language\":\"" +
                     JsonText(steam_language.empty() ? std::string("english") : steam_language) +
                     "\",\"depots\":[],\"gog\":{\"build_id\":\"" + JsonText(gog.build_id) +
                     "\",\"generation\":" + std::to_string(gog.generation) + ",\"products\":[";
  for (std::size_t index = 0; index < gog.products.size(); ++index)
    text += std::string(index ? "," : "") + "\"" + std::to_string(gog.products[index]) + "\"";
  text += "],\"language\":\"" + JsonText(gog.language) + "\",\"meta\":\"" + JsonText(gog.meta_id) + "\"";
  // Only when the player turned a DLC off: a record without one stays as earlier builds wrote it.
  std::vector<std::uint64_t> excluded;
  for (const auto id : gog.excluded)
    if (id != 0 && (gog.products.empty() || id != gog.products.front())) excluded.push_back(id);
  std::sort(excluded.begin(), excluded.end());
  excluded.erase(std::unique(excluded.begin(), excluded.end()), excluded.end());
  if (excluded.size() > kMaxGogRecordProducts) excluded.resize(kMaxGogRecordProducts);
  if (!excluded.empty()) {
    text += ",\"excluded\":[";
    for (std::size_t index = 0; index < excluded.size(); ++index)
      text += std::string(index ? "," : "") + "\"" + std::to_string(excluded[index]) + "\"";
    text += "]";
  }
  text += "},\"launches\":[";
  std::size_t written = 0;
  for (const auto& launch : launches) {
    if (written >= kMaxRecordedLaunches) break;
    if (launch.executable.empty() || launch.executable.size() > kMaxRecordedLaunchBytes ||
        launch.arguments.size() > kMaxRecordedLaunchBytes || launch.working_directory.size() > kMaxRecordedLaunchBytes ||
        launch.label.size() > kMaxRecordedLaunchBytes)
      continue;
    text += std::string(written ? "," : "") + "{\"executable\":\"" + JsonText(launch.executable) +
            "\",\"arguments\":\"" + JsonText(launch.arguments) + "\",\"working_directory\":\"" +
            JsonText(launch.working_directory) + "\",\"label\":\"" + JsonText(launch.label) + "\"}";
    ++written;
  }
  text += "]}\n";
  return text;
}

const char* UpdatePhaseName(UpdatePhase phase) {
  switch (phase) {
    case UpdatePhase::kStaging: return "staging";
    case UpdatePhase::kReplacing: return "replacing";
    case UpdatePhase::kDeleting: return "deleting";
  }
  return "staging";
}

std::string FormatUpdateState(const UpdateState& state) {
  std::string text = "{\"version\":" + std::to_string(state.version) + ",\"phase\":\"" + UpdatePhaseName(state.phase) +
                     "\",\"depots\":[";
  for (std::size_t index = 0; index < state.depots.size(); ++index)
    text += std::string(index ? "," : "") + "{\"id\":" + std::to_string(state.depots[index].id) + ",\"manifest\":\"" +
            std::to_string(state.depots[index].gid) + "\"}";
  text += "],\"branch\":\"" + JsonText(state.branch) + "\",\"language\":\"" + JsonText(state.language) + "\"}\n";
  return text;
}

std::optional<UpdateState> ParseUpdateState(std::string_view text) {
  steam::json::Limits limits;
  limits.max_bytes = kMaxInstallRecordBytes;
  limits.max_values = 64 * 1024;
  const auto root = steam::json::Parse(text, limits);
  if (!root || !root->is_object()) return std::nullopt;
  UpdateState state;
  if (const auto version = AsUint32(root->Find("version"))) state.version = static_cast<int>(*version);
  const auto* phase = root->Find("phase");
  if (!phase || !phase->is_string()) return std::nullopt;
  if (phase->text() == "staging") state.phase = UpdatePhase::kStaging;
  else if (phase->text() == "replacing") state.phase = UpdatePhase::kReplacing;
  else if (phase->text() == "deleting") state.phase = UpdatePhase::kDeleting;
  else return std::nullopt;
  if (const auto* branch = root->Find("branch"); branch && branch->is_string() && !branch->text().empty())
    state.branch = branch->text();
  if (const auto* language = root->Find("language"); language && language->is_string() && !language->text().empty())
    state.language = language->text();
  if (const auto* depots = root->Find("depots"); depots && depots->is_array()) {
    for (const auto& entry : depots->array()) {
      if (!entry.is_object()) return std::nullopt;
      const auto id = AsUint32(entry.Find("id"));
      if (!id || *id == 0) return std::nullopt;
      RecordedDepot depot;
      depot.id = *id;
      depot.gid = AsGid(entry.Find("manifest")).value_or(0);
      state.depots.push_back(depot);
    }
  }
  return state;
}

std::optional<UpdateState> ReadUpdateState(const std::string& install_root) {
  const auto text = ReadSmall(install_root + "/" + kUpdateDirName + "/" + kUpdateStateName);
  if (!text) return std::nullopt;
  return ParseUpdateState(*text);
}

std::optional<InstalledRecord> ReadInstallRecord(const std::string& path) {
  const auto text = ReadSmall(path);
  if (!text) return std::nullopt;
  return ParseInstallRecord(*text);
}

}  // namespace playden::installs
