// SPDX-License-Identifier: GPL-3.0-or-later
#include "web/http.h"

#include <cstdio>

namespace playden::web {
namespace {

int HexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool TokenChar(unsigned char c) {
  if (c >= '0' && c <= '9') return true;
  if ((c | 0x20) >= 'a' && (c | 0x20) <= 'z') return true;
  for (const char allowed : std::string_view("!#$%&'*+-.^_`|~")) if (c == static_cast<unsigned char>(allowed)) return true;
  return false;
}

std::string_view TrimSpaces(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
  return text;
}

}  // namespace

std::optional<std::string> PercentDecode(std::string_view text, bool plus_is_space) {
  std::string out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    char c = text[i];
    if (c == '%') {
      if (i + 2 >= text.size()) return std::nullopt;
      const int high = HexDigit(text[i + 1]), low = HexDigit(text[i + 2]);
      if (high < 0 || low < 0) return std::nullopt;
      c = static_cast<char>(high * 16 + low);
      i += 2;
    } else if (c == '+' && plus_is_space) {
      c = ' ';
    }
    if (c == '\0') return std::nullopt;
    out.push_back(c);
  }
  return out;
}

std::optional<std::string> QueryValue(std::string_view query, std::string_view key) {
  while (!query.empty()) {
    const auto amp = query.find('&');
    const auto pair = query.substr(0, amp);
    query = amp == std::string_view::npos ? std::string_view() : query.substr(amp + 1);
    const auto equals = pair.find('=');
    const auto name = PercentDecode(pair.substr(0, equals), true);
    if (!name || *name != key) continue;
    if (equals == std::string_view::npos) return std::string();
    return PercentDecode(pair.substr(equals + 1), true);
  }
  return std::nullopt;
}

ParseResult ParseRequestHead(std::string_view bytes, Request* out) {
  std::size_t end = bytes.find("\r\n\r\n");
  std::size_t terminator = 4;
  if (const auto bare = bytes.find("\n\n"); bare != std::string_view::npos && (end == std::string_view::npos || bare < end)) {
    end = bare;
    terminator = 2;
  }
  if (end == std::string_view::npos) return bytes.size() >= kMaxRequestHead ? ParseResult::kTooLarge : ParseResult::kIncomplete;
  if (end + terminator > kMaxRequestHead) return ParseResult::kTooLarge;
  std::string_view head = bytes.substr(0, end);

  Request request;
  const auto line_end = head.find('\n');
  std::string_view line = head.substr(0, line_end);
  if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
  head = line_end == std::string_view::npos ? std::string_view() : head.substr(line_end + 1);

  const auto first_space = line.find(' ');
  const auto second_space = first_space == std::string_view::npos ? first_space : line.find(' ', first_space + 1);
  if (first_space == 0 || second_space == std::string_view::npos || line.find(' ', second_space + 1) != std::string_view::npos)
    return ParseResult::kBadRequest;
  const auto method = line.substr(0, first_space);
  const auto target = line.substr(first_space + 1, second_space - first_space - 1);
  const auto version = line.substr(second_space + 1);
  for (const unsigned char c : method) if (!TokenChar(c)) return ParseResult::kBadRequest;
  if (version != "HTTP/1.1" && version != "HTTP/1.0") return ParseResult::kBadRequest;
  // Origin form only: "/path?query". No absolute URLs, no "*".
  if (target.empty() || target.front() != '/') return ParseResult::kBadRequest;
  for (const unsigned char c : target) if (c <= 0x20 || c >= 0x7f) return ParseResult::kBadRequest;
  request.method_text = std::string(method);
  request.method = method == "GET" ? Method::kGet : method == "HEAD" ? Method::kHead : method == "POST" ? Method::kPost : Method::kOther;
  const auto question = target.find('?');
  auto path = PercentDecode(target.substr(0, question));
  if (!path) return ParseResult::kBadRequest;
  request.path = std::move(*path);
  if (question != std::string_view::npos) request.query = std::string(target.substr(question + 1));

  while (!head.empty()) {
    const auto next = head.find('\n');
    std::string_view header = head.substr(0, next);
    head = next == std::string_view::npos ? std::string_view() : head.substr(next + 1);
    if (!header.empty() && header.back() == '\r') header.remove_suffix(1);
    if (header.empty()) continue;
    const auto colon = header.find(':');
    if (colon == 0 || colon == std::string_view::npos) return ParseResult::kBadRequest;
    std::string name(header.substr(0, colon));
    for (auto& c : name) {
      if (!TokenChar(static_cast<unsigned char>(c))) return ParseResult::kBadRequest;
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    const auto value = TrimSpaces(header.substr(colon + 1));
    for (const unsigned char c : value) if ((c < 0x20 && c != '\t') || c == 0x7f) return ParseResult::kBadRequest;
    request.headers.emplace_back(std::move(name), std::string(value));
  }
  *out = std::move(request);
  return ParseResult::kOk;
}

const char* StatusText(int status) {
  switch (status) {
    case 200: return "OK";
    case 202: return "Accepted";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    default: return "Unknown";
  }
}

Response::Response(Write write, bool head_only) : write_(std::move(write)), head_only_(head_only) {}

bool Response::Start(int status, std::string_view content_type, std::optional<std::uint64_t> length,
                     const std::vector<std::pair<std::string, std::string>>& extra) {
  if (started_) return !failed_;
  started_ = true;
  status_ = status;
  char line[64];
  std::snprintf(line, sizeof line, "HTTP/1.1 %d %s\r\n", status, StatusText(status));
  std::string head = line;
  head += "Content-Type: ";
  head += content_type;
  head += "\r\n";
  if (length) head += "Content-Length: " + std::to_string(*length) + "\r\n";
  for (const auto& [name, value] : extra) head += name + ": " + value + "\r\n";
  head += "X-Content-Type-Options: nosniff\r\nConnection: close\r\n\r\n";
  if (!write_(head.data(), head.size())) failed_ = true;
  return !failed_;
}

bool Response::Body(const char* data, std::size_t size) {
  if (!started_ || failed_) return false;
  if (head_only_ || size == 0) return true;
  if (!write_(data, size)) {
    failed_ = true;
    return false;
  }
  body_bytes_ += size;
  return true;
}

bool Response::Send(int status, std::string_view content_type, std::string_view body,
                    const std::vector<std::pair<std::string, std::string>>& extra) {
  return Start(status, content_type, body.size(), extra) && Body(body);
}

}  // namespace playden::web
