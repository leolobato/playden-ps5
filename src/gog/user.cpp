// SPDX-License-Identifier: GPL-3.0-or-later
#include "gog/user.h"

#include "gog/client_config.h"
#include "platform/secret.h"
#include "steam/backend/json.h"

namespace playden::gog {
namespace {

UserResult Result(UserResult::Kind kind, long status) {
  UserResult result;
  result.kind = kind;
  result.status = status;
  return result;
}

bool UsableName(const std::string& name) {
  if (name.empty() || name.size() > kMaxUsernameBytes) return false;
  for (const char c : name) {
    const auto byte = static_cast<unsigned char>(c);
    if (byte < 0x20 || byte == 0x7F) return false;
  }
  return true;
}

UserResult ParseUserResponse(const steam::HttpResponse& response) {
  using Kind = UserResult::Kind;
  if (!response.ok()) return Result(Kind::kNetwork, 0);
  const long status = response.status_code;
  if (status == 401) return Result(Kind::kRejected, status);
  if (status == 429 || (status >= 400 && status <= 599)) return Result(Kind::kNetwork, status);
  if (status != 200) return Result(Kind::kProtocol, status);
  steam::json::Limits limits;
  limits.max_bytes = kMaxUserResponseBytes;
  limits.max_values = kMaxUserValues;
  limits.max_string_bytes = kMaxUserResponseBytes;
  const std::string_view body(reinterpret_cast<const char*>(response.body.data()), response.body.size());
  const auto parsed = steam::json::Parse(body, limits);
  if (!parsed || !parsed->is_object()) return Result(Kind::kProtocol, status);
  const auto* name = parsed->Find("username");
  if (!name || !name->is_string() || !UsableName(name->text())) return Result(Kind::kProtocol, status);
  UserResult result = Result(Kind::kOk, status);
  result.username = name->text();
  return result;
}

}  // namespace

UserResult FetchUser(Transport& transport, std::string_view access_token, const std::atomic<bool>& cancel) {
  steam::HttpRequest request;
  request.method = steam::HttpMethod::kGet;
  request.url = "https://";
  request.url += kEmbedHost;
  request.url += "/userData.json";
  request.timeout_ms = kUserTimeoutMs;
  request.connect_timeout_ms = kUserConnectTimeoutMs;
  request.max_response_bytes = kMaxUserResponseBytes;
  // Reserved up front: a reallocation would leave a copy of the token behind in freed memory.
  std::string bearer;
  bearer.reserve(7 + access_token.size());
  bearer += "Bearer ";
  bearer += access_token;
  request.headers.reserve(2);
  request.headers.emplace_back("User-Agent", kUserAgent);
  request.headers.emplace_back("Authorization", std::string());
  request.headers.back().second.swap(bearer);
  auto response = transport ? transport(request, cancel) : steam::HttpResponse{steam::HttpError::kInvalidRequest};
  platform::ZeroizeSecret(request.headers.back().second);
  auto result = ParseUserResponse(response);
  platform::ZeroizeSecret(response.body);
  return result;
}

}  // namespace playden::gog
