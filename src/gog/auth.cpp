// SPDX-License-Identifier: GPL-3.0-or-later
#include "gog/auth.h"

#include "gog/client_config.h"
#include "platform/secret.h"
#include "steam/backend/json.h"

#include <utility>

namespace playden::gog {
namespace {

namespace json = steam::json;

constexpr std::string_view kLoginSuccess = "https://embed.gog.com/on_login_success";
constexpr std::size_t kMaxPasteBytes = 4096;
constexpr std::size_t kMinBareCode = 16;
constexpr std::size_t kMaxCode = 1024;

bool Blank(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

bool Control(char c) {
  const auto u = static_cast<unsigned char>(c);
  return u < 0x20 || u == 0x7f;
}

// RFC 3986 unreserved: what a code is made of, and what URL encoding leaves as it is.
bool Unreserved(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' ||
         c == '_' || c == '~';
}

bool CodeChars(std::string_view code) {
  for (const char c : code)
    if (!Unreserved(c)) return false;
  return true;
}

int Hex(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// %XX once (either case); '+' stays as it is. Nothing for a bad escape.
std::optional<std::string> PercentDecode(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] != '%') {
      out.push_back(text[i]);
      continue;
    }
    const int high = i + 2 < text.size() ? Hex(text[i + 1]) : -1;
    const int low = i + 2 < text.size() ? Hex(text[i + 2]) : -1;
    if (high < 0 || low < 0) {
      platform::ZeroizeSecret(out);
      return std::nullopt;
    }
    out.push_back(static_cast<char>(high * 16 + low));
    i += 2;
  }
  return out;
}

void AppendEncoded(std::string& out, std::string_view text) {
  static constexpr char kDigits[] = "0123456789ABCDEF";
  for (const char c : text) {
    if (Unreserved(c)) {
      out.push_back(c);
    } else {
      const auto u = static_cast<unsigned char>(c);
      out.push_back('%');
      out.push_back(kDigits[u >> 4]);
      out.push_back(kDigits[u & 15]);
    }
  }
}

std::size_t EncodedSize(std::string_view text) {
  std::size_t size = 0;
  for (const char c : text) size += Unreserved(c) ? 1 : 3;
  return size;
}

// A user ID as GOG's answers give it: 1 to 20 digits, no leading zero.
bool CanonicalUserId(std::string_view id) {
  if (id.empty() || id.size() > 20 || id.front() == '0') return false;
  for (const char c : id)
    if (c < '0' || c > '9') return false;
  return true;
}

const json::Value* StringField(const json::Value& object, std::string_view key) {
  const auto* value = object.Find(key);
  return value && value->is_string() && !value->text().empty() ? value : nullptr;
}

TokenResult Result(TokenResult::Kind kind, long status) {
  TokenResult result;
  result.kind = kind;
  result.status = status;
  return result;
}

TokenResult ParseTokenResponse(const steam::HttpResponse& response) {
  using Kind = TokenResult::Kind;
  // No answer: a timeout, DNS, TLS, a body over the cap, a cancel. The session stays; try again later.
  if (!response.ok()) return Result(Kind::kNetwork, 0);
  const long status = response.status_code;
  json::Limits limits;
  limits.max_bytes = kMaxTokenResponseBytes;
  limits.max_values = 64;
  limits.max_string_bytes = kMaxTokenResponseBytes;
  const std::string_view body(reinterpret_cast<const char*>(response.body.data()), response.body.size());
  if (status == 401) return Result(Kind::kRejected, status);
  if (status == 400) {
    const auto parsed = json::Parse(body, limits);
    const auto* error = parsed && parsed->is_object() ? StringField(*parsed, "error") : nullptr;
    return Result(error && error->text() == "invalid_grant" ? Kind::kRejected : Kind::kNetwork, status);
  }
  if (status == 429 || (status >= 400 && status <= 599)) return Result(Kind::kNetwork, status);
  if (status != 200) return Result(Kind::kProtocol, status);

  const auto parsed = json::Parse(body, limits);
  if (!parsed || !parsed->is_object()) return Result(Kind::kProtocol, status);
  const auto* access = StringField(*parsed, "access_token");
  const auto* refresh = StringField(*parsed, "refresh_token");
  // GOG sends the user ID as a string; a number is read the same way.
  const auto* user = parsed->Find("user_id");
  if (!access || !refresh || !user || !(user->is_string() || user->is_number()) || !CanonicalUserId(user->text()))
    return Result(Kind::kProtocol, status);
  std::uint32_t expires_in = kDefaultExpiresIn;
  if (const auto* expires = parsed->Find("expires_in"); expires && !expires->is_null()) {
    if (!expires->is_number()) return Result(Kind::kProtocol, status);
    const auto& text = expires->text();
    bool digits = !text.empty();
    for (const char c : text) digits = digits && c >= '0' && c <= '9';
    if (!digits) return Result(Kind::kProtocol, status);  // negative or fractional
    const auto seconds = expires->AsUint64();               // nothing: too large for 64 bits
    expires_in = !seconds || *seconds > kMaxExpiresIn ? kMaxExpiresIn
               : *seconds < kMinExpiresIn             ? kMinExpiresIn
                                                      : static_cast<std::uint32_t>(*seconds);
  }
  TokenResult result = Result(Kind::kOk, status);
  result.tokens.access_token = access->text();
  result.tokens.refresh_token = refresh->text();
  result.tokens.user_id = user->text();
  result.tokens.expires_in = expires_in;
  return result;
}

TokenResult Call(Transport& transport, Grant grant, std::string& secret, const std::atomic<bool>& cancel) {
  auto request = TokenRequest(grant, secret);
  platform::ZeroizeSecret(secret);
  auto response = transport(request, cancel);
  platform::ZeroizeSecret(request.url);
  auto result = ParseTokenResponse(response);
  platform::ZeroizeSecret(response.body);
  return result;
}

}  // namespace

std::string AuthorizeUrl() {
  std::string url = "https://";
  url += kAuthHost;
  url += "/auth?client_id=";
  url += kClientId;
  url += "&redirect_uri=";
  AppendEncoded(url, kRedirectUri);
  url += "&response_type=code&layout=galaxy";
  return url;
}

std::optional<std::string> CodeFromPaste(std::string_view paste) {
  while (!paste.empty() && Blank(paste.front())) paste.remove_prefix(1);
  while (!paste.empty() && Blank(paste.back())) paste.remove_suffix(1);
  if (paste.empty() || paste.size() > kMaxPasteBytes) return std::nullopt;
  for (const char c : paste)
    if (Control(c)) return std::nullopt;

  if (paste.substr(0, kLoginSuccess.size()) != kLoginSuccess) {
    // A bare code.
    if (paste.size() < kMinBareCode || paste.size() > kMaxCode || !CodeChars(paste)) return std::nullopt;
    return std::string(paste);
  }
  // The address: exactly that scheme, host and path, then a query. The fragment is never read.
  auto rest = paste.substr(kLoginSuccess.size());
  if (const auto hash = rest.find('#'); hash != std::string_view::npos) rest = rest.substr(0, hash);
  if (rest.empty() || rest.front() != '?') return std::nullopt;
  rest.remove_prefix(1);
  while (true) {
    const auto amp = rest.find('&');
    const auto pair = rest.substr(0, amp);
    if (pair.size() > 5 && pair.substr(0, 5) == "code=") {
      auto code = PercentDecode(pair.substr(5));
      if (!code) return std::nullopt;
      if (code->size() > kMaxCode || !CodeChars(*code)) {
        platform::ZeroizeSecret(*code);
        return std::nullopt;
      }
      return code;
    }
    if (amp == std::string_view::npos) break;
    rest = rest.substr(amp + 1);
  }
  return std::nullopt;
}

steam::HttpRequest TokenRequest(Grant grant, std::string_view secret) {
  const bool code = grant == Grant::kAuthorizationCode;
  steam::HttpRequest request;
  request.method = steam::HttpMethod::kGet;
  request.headers = {{"User-Agent", kUserAgent}};
  request.timeout_ms = kTokenTimeoutMs;
  request.connect_timeout_ms = kTokenConnectTimeoutMs;
  request.max_response_bytes = kMaxTokenResponseBytes;
  // Reserved up front: a reallocation would leave a copy of the secret behind in freed memory.
  auto& url = request.url;
  url.reserve(256 + EncodedSize(kRedirectUri) + EncodedSize(secret));
  url += "https://";
  url += kAuthHost;
  url += "/token?client_id=";
  url += kClientId;
  url += "&client_secret=";
  url += kClientSecret;
  if (code) {
    url += "&grant_type=authorization_code&redirect_uri=";
    AppendEncoded(url, kRedirectUri);
    url += "&code=";
  } else {
    url += "&grant_type=refresh_token&refresh_token=";
  }
  AppendEncoded(url, secret);
  return request;
}

void Tokens::Wipe() {
  platform::ZeroizeSecret(access_token);
  platform::ZeroizeSecret(refresh_token);
  platform::ZeroizeSecret(user_id);
  expires_in = kDefaultExpiresIn;
}

TokenResult Exchange(Transport& transport, std::string code, const std::atomic<bool>& cancel) {
  return Call(transport, Grant::kAuthorizationCode, code, cancel);
}

TokenResult Refresh(Transport& transport, std::string refresh_token, const std::atomic<bool>& cancel) {
  return Call(transport, Grant::kRefreshToken, refresh_token, cancel);
}

}  // namespace playden::gog
