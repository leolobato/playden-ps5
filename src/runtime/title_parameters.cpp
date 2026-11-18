// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime/title_parameters.h"

#include "steam/backend/json.h"

#include <limits>
#include <string_view>

namespace playden::runtime {
namespace {

struct Span {
  std::size_t begin = 0;
  std::size_t end = 0;
  std::uint64_t value = 0;
};

// The digits after the one "attribute3" key; nothing for a second key, a sign, a fraction or an exponent.
std::optional<Span> Attribute3Span(const std::string& param) {
  static constexpr std::string_view key = "\"attribute3\"";
  const auto at = param.find(key);
  if (at == std::string::npos || param.find(key, at + key.size()) != std::string::npos) return std::nullopt;
  auto cursor = param.find_first_not_of(" \t\r\n", at + key.size());
  if (cursor == std::string::npos || param[cursor] != ':') return std::nullopt;
  cursor = param.find_first_not_of(" \t\r\n", cursor + 1);
  if (cursor == std::string::npos) return std::nullopt;
  Span span{cursor, cursor, 0};
  while (span.end < param.size() && param[span.end] >= '0' && param[span.end] <= '9') {
    const std::uint64_t digit = static_cast<std::uint64_t>(param[span.end] - '0');
    if (span.value > (std::numeric_limits<std::uint32_t>::max() - digit) / 10) return std::nullopt;
    span.value = span.value * 10 + digit;
    ++span.end;
  }
  if (span.end == span.begin || (span.end - span.begin > 1 && param[span.begin] == '0')) return std::nullopt;
  const auto after = param.find_first_not_of(" \t\r\n", span.end);
  if (after == std::string::npos || (param[after] != ',' && param[after] != '}')) return std::nullopt;
  return span;
}

constexpr std::string_view kDownloadDataKey = "\"downloadDataSize\"";

bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

}  // namespace

std::optional<std::uint64_t> TitleAttribute3(const std::string& param_json) {
  const auto span = Attribute3Span(param_json);
  if (!span) return std::nullopt;
  return span->value;
}

bool SetTitleVariableRefresh(std::string* param_json, bool supported) {
  if (!param_json) return false;
  const auto span = Attribute3Span(*param_json);
  if (!span) return false;
  const auto value = supported ? span->value | kTitleAttribute3VariableRefresh
                               : span->value & ~kTitleAttribute3VariableRefresh;
  param_json->replace(span->begin, span->end - span->begin, std::to_string(value));
  return true;
}

bool TitleRequestsDownloadData(const std::string& param_json) {
  if (param_json.find(kDownloadDataKey) != std::string::npos) return true;
  // The key spelled with escapes ("download\u0044ataSize") is the same key to a JSON reader.
  const auto parsed = playden::steam::json::Parse(param_json, {64 * 1024, 8, 1024, 64 * 1024});
  return !parsed || !parsed->is_object() || parsed->Find("downloadDataSize");
}

bool RemoveTitleDownloadData(std::string* param_json) {
  if (!param_json) return false;
  const std::string& param = *param_json;
  const auto at = param.find(kDownloadDataKey);
  // Only the plain spelling is edited: one with escapes is refused, never taken for absent.
  if (at == std::string::npos) return !TitleRequestsDownloadData(param);
  if (param.find(kDownloadDataKey, at + kDownloadDataKey.size()) != std::string::npos) return false;
  // At the top level: the parsed object holds it as a number. Nested in another object, it would be found there.
  const playden::steam::json::Limits limits{64 * 1024, 8, 1024, 64 * 1024};
  const auto parsed = playden::steam::json::Parse(param, limits);
  const auto* value = parsed && parsed->is_object() ? parsed->Find("downloadDataSize") : nullptr;
  if (!value || !value->is_number()) return false;
  auto cursor = at + kDownloadDataKey.size();
  while (cursor < param.size() && IsSpace(param[cursor])) ++cursor;
  if (cursor == param.size() || param[cursor] != ':') return false;
  ++cursor;
  while (cursor < param.size() && IsSpace(param[cursor])) ++cursor;
  const auto digits = cursor;
  while (cursor < param.size() && param[cursor] >= '0' && param[cursor] <= '9') ++cursor;
  if (cursor == digits) return false;  // a sign, a fraction or an exponent: not the shell's plain value
  const auto value_end = cursor;
  while (cursor < param.size() && IsSpace(param[cursor])) ++cursor;
  if (cursor == param.size()) return false;
  std::size_t begin = at, end = value_end;
  if (param[cursor] == ',') {
    // "downloadDataSize": 256,<space>"next": the entry, its comma and the space before the next key.
    end = cursor + 1;
    while (end < param.size() && IsSpace(param[end])) ++end;
  } else if (param[cursor] == '}') {
    // The last entry: from the comma that ends the one before it, or alone in the object.
    auto before = at;
    while (before > 0 && IsSpace(param[before - 1])) --before;
    if (before > 0 && param[before - 1] == ',') begin = before - 1;
    else if (before == 0 || param[before - 1] != '{') return false;
  } else {
    return false;
  }
  std::string edited = param;
  edited.erase(begin, end - begin);
  // The edit must leave the same object without the key.
  const auto check = playden::steam::json::Parse(edited, limits);
  if (!check || !check->is_object() || check->Find("downloadDataSize") ||
      check->object().size() + 1 != parsed->object().size())
    return false;
  *param_json = std::move(edited);
  return true;
}

}  // namespace playden::runtime
