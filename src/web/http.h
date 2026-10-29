// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace playden::web {

// The request line and headers, up to the blank line; a longer head is refused with 431.
inline constexpr std::size_t kMaxRequestHead = 8 * 1024;

enum class Method { kGet, kHead, kPost, kOther };

struct Request {
  Method method = Method::kOther;
  std::string method_text;  // as sent, for web.log
  std::string path;         // percent-decoded, without the query
  std::string query;        // raw, after '?'
  std::vector<std::pair<std::string, std::string>> headers;  // names lower-cased
};

enum class ParseResult { kIncomplete, kOk, kBadRequest, kTooLarge };

// Parses an HTTP/1.x request head from the bytes received so far. kIncomplete: no blank line yet
// and still under kMaxRequestHead. The body, if any, is ignored.
ParseResult ParseRequestHead(std::string_view bytes, Request* out);

// The percent-decoded value of key in a query string ("title=FAKE78012&x=1"), or nothing.
std::optional<std::string> QueryValue(std::string_view query, std::string_view key);
// Decodes %XX and, in queries, '+'; nothing on a bad escape or a decoded NUL.
std::optional<std::string> PercentDecode(std::string_view text, bool plus_is_space = false);

const char* StatusText(int status);

// One response written through write(), which returns false once the client is gone. HEAD sends
// the headers only. Every response closes the connection.
class Response {
 public:
  using Write = std::function<bool(const char* data, std::size_t size)>;
  Response(Write write, bool head_only);

  // Sends the status line and headers. length: Content-Length, or none for a body that ends when
  // the connection closes (a streamed zip).
  bool Start(int status, std::string_view content_type, std::optional<std::uint64_t> length,
             const std::vector<std::pair<std::string, std::string>>& extra = {});
  bool Body(const char* data, std::size_t size);
  bool Body(std::string_view text) { return Body(text.data(), text.size()); }
  // A complete small response: Start with the body's length, then the body.
  bool Send(int status, std::string_view content_type, std::string_view body,
            const std::vector<std::pair<std::string, std::string>>& extra = {});

  bool started() const { return started_; }
  int status() const { return status_; }
  std::uint64_t body_bytes() const { return body_bytes_; }
  bool failed() const { return failed_; }

 private:
  Write write_;
  bool head_only_ = false;
  bool started_ = false;
  bool failed_ = false;
  int status_ = 0;
  std::uint64_t body_bytes_ = 0;
};

}  // namespace playden::web
