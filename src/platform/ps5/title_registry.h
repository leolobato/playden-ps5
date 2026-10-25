#pragma once
#include "platform/ps5/title_runtime.h"
#include <functional>
#include <optional>

namespace playden::ps5 {
// kAbsent means absent from ShadowMount cache, not the native install database.
enum class RegistrationState { kUnavailable, kAbsent, kPending, kReady, kCollision };
struct TitleRegistration {
  RegistrationState state = RegistrationState::kUnavailable;
  std::string path;
  // Why the state is kUnavailable, for logs.
  std::string reason;
};
// Synchronous worker-only boundary to the existing ShadowMountPlus local API.
class TitleRegistry {
 public:
  using Transport = std::function<std::optional<std::string>(const std::string&, const std::string&)>;
  explicit TitleRegistry(Transport transport) : transport_(std::move(transport)) {}
  TitleRegistration Query(const std::string& title_id);
  // For logs: "shadowmount=<version> api=<n>", or why ShadowMountPlus did not answer.
  std::string DescribeVersion();
  // Caller must independently check native install/source paths and establish
  // ownership before registration. ShadowMount inventory alone cannot reserve IDs.
  TitleResult Register(const std::string& title_id);
  // Removes only the manual registration source; caller must retire/delete its owned
  // source folder before Unregister to prevent the scanner registering it again.
  TitleResult RemoveManualSource(const std::string& title_id);
  TitleResult Unregister(const std::string& title_id);
 private:
  TitleResult Post(const std::string& route, const std::string& body);
  Transport transport_;
};
bool IsPlaydenGameTitle(const std::string& title_id);
}  // namespace playden::ps5
