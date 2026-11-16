#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace playden::runtime {

// A one-time web page on the local network for signing in to a publisher from a phone: Playden
// shows its address as a QR code, the phone opens it, and the page hands one credential back to
// Playden. Every request needs the random path (/s/<nonce>, 128 bits); anything else gets 404.
//
//   GET  /s/<nonce>             the publisher pack's sign-in page as is (or the paste page below)
//   GET  /s/<nonce>/paste       Playden's own page: paste a credential and send it
//   POST /s/<nonce>/credential  application/json, at most 16 KiB: handed to the submit callback,
//                               answered {"ok":true,"display":"…"} or 4xx {"ok":false,"message":"…"}
//
// The relay carries a publisher credential, in plain HTTP on the local network. That is accepted
// because the publisher's sign-in only works in a real browser, so there is no other way from the
// phone to the console; it is kept as small as it can be: the nonce is spent once a credential is
// accepted (the page then closes), the body is never logged or kept (the relay wipes its copies),
// answers are never cached (Cache-Control: no-store, Referrer-Policy: no-referrer), and the page
// closes on Stop or after ten minutes. What the credential holds is the publisher pack's business:
// the relay never looks inside it.

// How a submitted credential ended: ok with the account's name, or why not (for the phone and the TV).
struct PhoneRelayAnswer {
  bool ok = false;
  std::string display;
  std::string message;
};

// Called on the relay's thread with the request body; it may block (the relay serves one phone at
// a time). The callback owns the credential: it wipes it when done.
using PhoneRelaySubmit = std::function<PhoneRelayAnswer(std::string credential)>;

struct PhoneRelayPage {
  std::string title;  // the paste page's title: "Sign in to Rockstar Games"
  std::string html;   // the publisher pack's sign-in page; empty: the paste page answers /s/<nonce> too
};

// What the TV shows while the page is open.
enum class PhoneRelayPhase {
  kWaiting,    // nobody opened the page yet
  kConnected,  // a phone opened it
  kChecking,   // a credential arrived and is being checked
  kAccepted,   // a credential was accepted: the page is closed
  kRefused,    // the last credential was refused (message says why); the page stays open
  kClosed,     // the page closed unused: Stop, or its ten minutes ran out
};

struct PhoneRelayStatus {
  PhoneRelayPhase phase = PhoneRelayPhase::kClosed;
  std::string display;  // kAccepted
  std::string message;  // kRefused
};

class PhoneRelay {
 public:
  PhoneRelay();
  ~PhoneRelay();
  PhoneRelay(const PhoneRelay&) = delete;
  PhoneRelay& operator=(const PhoneRelay&) = delete;

  // Listens on host (an address of this console; "127.0.0.1" listens on loopback only, any other
  // on all interfaces) at first_port or one of the next nine.
  bool Start(const std::string& host, std::uint16_t first_port, PhoneRelayPage page, PhoneRelaySubmit submit,
             std::string* reason, std::int64_t lifetime_ms = 10 * 60 * 1000);
  std::string Address() const;  // http://<host>:<port>/s/<nonce>
  bool Running() const;
  PhoneRelayStatus Status() const;
  // Asks the page to close and returns at once; its thread ends on its own (a credential being
  // checked finishes first). Stop waits for that.
  void Close();
  // Closes the page and returns once its thread ended.
  void Stop();

 private:
  struct State;
  std::shared_ptr<State> state_;
};

// The console's IPv4 address on the local network (the source address of the default route),
// or nothing when there is no network.
std::optional<std::string> LanAddress();

}  // namespace playden::runtime
