#include "platform/ps5/title_registry.h"
#include "steam/backend/json.h"
#include <cerrno>
#include <limits>

namespace playden::ps5 {
namespace {
namespace json = steam::json;
std::optional<bool> Flag(const json::Value& v, const char* key) {
  const auto* value = v.Find(key);
  return value && value->is_bool() ? std::optional<bool>(value->boolean()) : std::nullopt;
}
std::string Text(const json::Value& v, const char* key) {
  const auto* value = v.Find(key);
  return value && value->is_string() ? value->text() : std::string();
}
bool Success(const json::Value& v) {
  const auto* status = v.Find("status");
  return status && status->AsUint64() == 0;
}
std::string SourceBody(const std::string& title) { return "{\"path\":\"/data/homebrew/" + title + "\"}"; }
}
bool IsPlaydenGameTitle(const std::string& title) {
  return ValidTitleId(title) && title.compare(0, 6, "FAKE78") == 0;
}
TitleRegistration TitleRegistry::Query(const std::string& title) {
  const auto unavailable = [](std::string reason) {
    return TitleRegistration{RegistrationState::kUnavailable, {}, std::move(reason)};
  };
  if (!ValidTitleId(title)) return unavailable("title-id");
  if (!transport_) return unavailable("no-transport");
  const auto bytes = transport_("/api/v1/games", "{}");
  if (!bytes) return unavailable("no-response");
  if (bytes->size() > 1024*1024) return unavailable("response-too-large");
  const auto v = json::Parse(*bytes, {1024*1024, 16, 65536, 4096});
  if (!v) return unavailable("bad-json");
  if (!Success(*v)) return unavailable("status");
  const auto* games = v->Find("games");
  if (!games || !games->is_array()) return unavailable("no-games");
  const auto source = "/data/homebrew/" + title;
  TitleRegistration result{RegistrationState::kAbsent, {}, {}};
  unsigned matches = 0;
  for (const auto& game : games->array()) {
    const auto id = Text(game, "title_id");
    const auto path = Text(game, "path");
    if (!ValidTitleId(id)) {
      // Another source's odd ID (homebrew, backups) says nothing about this title. One that
      // names this title's folder may be it, and an incomplete inventory cannot imply absence.
      if (path.empty() || path == source) return unavailable("bad-title-id");
      continue;
    }
    if (id != title) continue;
    if (++matches > 1) return {RegistrationState::kCollision, path, {}};
    if (path.empty()) return unavailable("no-path");
    const auto managed = Flag(game, "managed");
    if (!managed) return unavailable("no-managed");
    // ShadowMount retains a removed title's row after its manual source,
    // registration and directory are gone. This explicit tombstone is absent;
    // the coordinator still checks the directory, native database and activity.
    if (!*managed && path == source &&
        Text(game, "runtime_path") == path && Text(game, "source_type") == "folder" &&
        Flag(game, "installed") == false && Flag(game, "mounted") == false &&
        Flag(game, "source_available") == false)
      continue;
    if (path != source || !*managed)
      result = {RegistrationState::kCollision, path, {}};
    else {
      const auto installed = Flag(game, "installed"), available = Flag(game, "source_available");
      const auto type = Text(game, "source_type");
      if (!installed || !available || type.empty()) return unavailable("incomplete-entry");
      result = {*installed && *available && type == "folder" ? RegistrationState::kReady : RegistrationState::kPending, path, {}};
    }
  }
  return result;
}

std::string TitleRegistry::DescribeVersion() {
  if (!transport_) return "shadowmount=no-transport";
  const auto bytes = transport_("/api/v1/version", "{}");
  if (!bytes) return "shadowmount=unreachable";
  const auto v = bytes->size() <= 65536 ? json::Parse(*bytes, {65536, 16, 4096, 4096}) : std::nullopt;
  if (!v || !Success(*v)) return "shadowmount=bad-answer";
  auto version = Text(*v, "shadowmount_version");
  // Keep a log line one line whatever the API sends.
  if (version.size() > 64) version.resize(64);
  for (auto& c : version) if (c < 0x21 || c > 0x7e) c = '?';
  const auto* api = v->Find("api_version");
  const auto api_number = api ? api->AsUint64() : std::nullopt;
  return "shadowmount=" + (version.empty() ? std::string("unknown") : version) +
         " api=" + (api_number ? std::to_string(*api_number) : std::string("unknown"));
}
TitleResult TitleRegistry::Post(const std::string& route, const std::string& body) {
  if (!transport_) return {false, ENOSYS, "shadowmount"};
  const auto bytes = transport_(route, body);
  if (!bytes || bytes->size() > 1024*1024) return {false, EIO, "shadowmount"};
  const auto v = json::Parse(*bytes, {1024*1024, 16, 65536, 4096});
  const auto* status = v ? v->Find("status") : nullptr;
  const auto code = status ? status->AsUint64() : std::nullopt;
  if (!code || *code > std::numeric_limits<std::uint32_t>::max())
    return {false, EPROTO, "shadowmount"};
  return {*code == 0, static_cast<std::uint32_t>(*code), "shadowmount"};
}
TitleResult TitleRegistry::Register(const std::string& title) {
  if (!IsPlaydenGameTitle(title)) return {false, EINVAL, "title-id"};
  const auto existing = Query(title);
  if (existing.state == RegistrationState::kUnavailable) return {false, EIO, "query"};
  if (existing.state == RegistrationState::kCollision) return {false, EEXIST, "collision"};
  if (existing.state == RegistrationState::kReady) return {true, 0, "registered"};
  // Already submitted and waiting for its scan: never submit it again.
  if (existing.state == RegistrationState::kPending) return {true, 0, "pending"};
  auto result = Post("/api/v1/manual/add", SourceBody(title));
  // No answer: the add may have happened. Report it as uncertain so the caller
  // only queries from now on and never submits it again on its own.
  if (!result.ok && result.code == EIO) return {false, ETIMEDOUT, "uncertain"};
  if (!result.ok) return result;
  // ShadowMount rescans its manual sources by itself (every 15 s on 13.60), so
  // a lost scan request only delays registration.
  const auto scanned = Post("/api/v1/scan", "{}");
  return scanned.ok ? scanned : TitleResult{true, 0, "scan-deferred"};
}
TitleResult TitleRegistry::RemoveManualSource(const std::string& title) {
  if (!IsPlaydenGameTitle(title)) return {false, EINVAL, "title-id"};
  return Post("/api/v1/manual/remove", SourceBody(title));
}
TitleResult TitleRegistry::Unregister(const std::string& title) {
  if (!IsPlaydenGameTitle(title)) return {false, EINVAL, "title-id"};
  return Post("/api/v1/games/uninstall", "{\"title_id\":\"" + title + "\"}");
}
}  // namespace playden::ps5
