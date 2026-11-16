#include "platform/ps5/system_notification.h"
#include <cstdio>
#include <cstdlib>
using namespace playden::ps5;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%d: %s\n", __LINE__, #x); std::exit(1); } } while (0)
int main() {
  CHECK(TruncateUtf8("short", 10) == "short");
  CHECK(TruncateUtf8("exact", 5) == "exact");
  CHECK(TruncateUtf8("abcdef", 3) == "abc");
  // "a" + U+203A (3 bytes) + "b": a cut inside the sequence drops all of it.
  const std::string mixed = "a\xE2\x80\xBA" "b";
  CHECK(TruncateUtf8(mixed, 1) == "a");
  CHECK(TruncateUtf8(mixed, 2) == "a");
  CHECK(TruncateUtf8(mixed, 3) == "a");
  CHECK(TruncateUtf8(mixed, 4) == "a\xE2\x80\xBA");
  CHECK(TruncateUtf8(std::string(5000, 'x'), kSystemNotificationMaxBytes).size() == kSystemNotificationMaxBytes);
  CHECK(TruncateUtf8("x", 0).empty());
  // The host has no system notifications.
  CHECK(!PostSystemNotification("Playden"));
  std::puts("system notification tests passed");
  return 0;
}
