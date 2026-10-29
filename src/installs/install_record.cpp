// SPDX-License-Identifier: GPL-3.0-or-later
#include "installs/install_record.h"

#include "steam/backend/json.h"

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
