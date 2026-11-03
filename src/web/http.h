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
  // The body's declared length; nothing without a Content-Length header.
  std::optional<std::uint64_t> content_length;
  // Reads up to size body bytes: the count, 0 at the end of the declared length, -1 when the client went away or
  // went quiet. Empty when there is no connection (tests that route a request directly).
  std::function<std::ptrdiff_t(char* data, std::size_t size)> read_body;
};

// A header's value (names are lower-case), or nothing.
std::optional<std::string> HeaderValue(const Request& request, std::string_view name);

enum class ParseResult { kIncomplete, kOk, kBadRequest, kTooLarge };

// Parses an HTTP/1.x request head from the bytes received so far. kIncomplete: no blank line yet
// and still under kMaxRequestHead. *head_size is set to the head's size with its blank line, so what follows
// is the start of the body. A chunked or repeated body length, or one that isn't a number, is kBadRequest.
ParseResult ParseRequestHead(std::string_view bytes, Request* out, std::size_t* head_size = nullptr);

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
  // Sends the status line and headers of a chunked body. The client sees the download as complete only after
  // FinishChunks: a body cut before it (the connection closed on a failure) shows as a failed download.
  bool StartChunked(int status, std::string_view content_type,
                    const std::vector<std::pair<std::string, std::string>>& extra = {});
  bool FinishChunks();
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
  bool chunked_ = false;
  int status_ = 0;
  std::uint64_t body_bytes_ = 0;
};

}  // namespace playden::web
