// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime/title_parameters.h"

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

}  // namespace playden::runtime
