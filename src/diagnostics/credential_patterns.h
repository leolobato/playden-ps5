// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Credentials found by their shape in a log line, for every redaction path: diagnostics::RedactSecrets (lines Playden
// writes), web::LogsSource (served logs and the report) and tools/collect_logs.py (the same rules as Python regexes).
// Header-only, with no dependencies, so each of those libraries can use it without linking another.

#include <cstddef>
#include <string>
#include <string_view>

namespace playden::diagnostics {

namespace credential_patterns {

inline char Lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

// A regex word character for \b: ASCII letters, digits and '_'.
inline bool WordChar(char c) {
  const auto u = static_cast<unsigned char>(c);
  return (c >= '0' && c <= '9') || ((u | 0x20) >= 'a' && (u | 0x20) <= 'z') || c == '_';
}

// key at text[at], ignoring ASCII case.
inline bool KeyAt(std::string_view text, std::size_t at, std::string_view key) {
  if (text.size() - at < key.size()) return false;
  for (std::size_t i = 0; i < key.size(); ++i)
    if (Lower(text[at + i]) != key[i]) return false;
  return true;
}

// The characters of a signed value: an OAuth code, a CDN token, a URL-encoded secret.
inline bool SignedValueChar(char c) {
  return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '.' || c == '_' ||
         c == '~' || c == '%' || c == '+' || c == '/' || c == '=' || c == '-';
}

// The end of a key=value token value: blanks and the characters that end a query or a field.
inline bool TokenValueEnd(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v' || c == '&' || c == '"' ||
         c == '\'' || c == ',' || c == ';' || c == '#' || c == '<' || c == '>';
}

inline constexpr std::string_view kRedacted = "<redacted>";

}  // namespace credential_patterns

// Each value becomes <redacted>; the key stays:
// - code=, token= and wsSecret= (any case, after a non-word character): a value of at least 16 characters of
//   [A-Za-z0-9._~%+/=-] that is not only lowercase letters, '_' and '-'. That covers a GOG sign-in code in a URL,
//   fastly's signed path (token=nva=...~dirs=...~token=...) and gcore's wsSecret, and keeps "exit code=1", "code=0"
//   and Playden's own error codes such as code=publication-control-collision and code=authorization_pending.
// - access_token= and refresh_token= (any case, any length), and the JSON "access_token":"..." and
//   "refresh_token":"..." (with blanks around the colon).
// Running it twice gives what running it once does.
inline std::string RedactCredentialPatterns(std::string_view text) {
  namespace cp = credential_patterns;
  static constexpr std::string_view kSignedKeys[] = {"code=", "token=", "wssecret="};
  static constexpr std::string_view kTokenKeys[] = {"access_token", "refresh_token"};
  std::string out;
  out.reserve(text.size());
  std::size_t i = 0;
  while (i < text.size()) {
    const bool boundary = i == 0 || !cp::WordChar(text[i - 1]);
    bool done = false;
    // "access_token" : "value"
    if (text[i] == '"') {
      for (const auto key : kTokenKeys) {
        if (!cp::KeyAt(text, i + 1, key) || i + 1 + key.size() >= text.size() || text[i + 1 + key.size()] != '"')
          continue;
        auto at = i + key.size() + 2;
        while (at < text.size() && (text[at] == ' ' || text[at] == '\t' || text[at] == '\r' || text[at] == '\n')) ++at;
        if (at >= text.size() || text[at] != ':') break;
        ++at;
        while (at < text.size() && (text[at] == ' ' || text[at] == '\t' || text[at] == '\r' || text[at] == '\n')) ++at;
        if (at >= text.size() || text[at] != '"') break;
        const auto start = at + 1;
        const auto end = text.find('"', start);
        if (end == std::string_view::npos || end == start) break;
        out.append(text.substr(i, start - i));
        out += cp::kRedacted;
        i = end;
        done = true;
        break;
      }
    } else if (boundary) {
      // access_token=value
      for (const auto key : kTokenKeys) {
        const auto value = i + key.size() + 1;
        if (!cp::KeyAt(text, i, key) || value > text.size() || text[value - 1] != '=') continue;
        auto end = value;
        while (end < text.size() && !cp::TokenValueEnd(text[end])) ++end;
        if (end == value) break;
        out.append(text.substr(i, value - i));
        out += cp::kRedacted;
        i = end;
        done = true;
        break;
      }
      // code=, token=, wsSecret=: a long signed value.
      for (std::size_t k = 0; !done && k < sizeof(kSignedKeys) / sizeof(kSignedKeys[0]); ++k) {
        const auto key = kSignedKeys[k];
        if (!cp::KeyAt(text, i, key)) continue;
        const auto value = i + key.size();
        auto end = value;
        bool word_only = true;  // lowercase letters, '_' and '-': one of Playden's own error codes
        while (end < text.size() && cp::SignedValueChar(text[end])) {
          if (!((text[end] >= 'a' && text[end] <= 'z') || text[end] == '_' || text[end] == '-')) word_only = false;
          ++end;
        }
        if (end - value < 16 || word_only) break;
        out.append(text.substr(i, value - i));
        out += cp::kRedacted;
        i = end;
        done = true;
      }
    }
    if (!done) out.push_back(text[i++]);
  }
  return out;
}

}  // namespace playden::diagnostics
