#include "runtime/phone_relay.h"

#include <arpa/inet.h>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>

namespace playden::runtime {
namespace {

using Clock = std::chrono::steady_clock;

constexpr std::size_t kMaxBodyBytes = 16 * 1024;
constexpr std::size_t kMaxHeaderBytes = 8 * 1024;
constexpr int kRequestTimeoutMs = 5000;
constexpr int kPollMs = 200;
// The console's worker threads get small default stacks.
constexpr std::size_t kStackBytes = 512 * 1024;

std::string HtmlEscape(const std::string& text) {
  std::string out;
  for (char c : text) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      default: out += c;
    }
  }
  return out;
}

std::string JsonString(const std::string& text) {
  static const char* hex = "0123456789abcdef";
  std::string out = "\"";
  for (unsigned char c : text) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += static_cast<char>(c);
    } else if (c < 0x20 || c == '<' || c == '>' || c == '&') {
      out += "\\u00";
      out += hex[c >> 4];
      out += hex[c & 15];
    } else {
      out += static_cast<char>(c);
    }
  }
  return out + "\"";
}

void Wipe(std::string& text) {
  std::fill(text.begin(), text.end(), '\0');
  text.clear();
}

std::optional<std::string> RandomHex(std::size_t bytes) {
  const int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
  if (fd < 0) return std::nullopt;
  std::string raw(bytes, '\0');
  std::size_t got = 0;
  while (got < bytes) {
    const auto count = ::read(fd, raw.data() + got, bytes - got);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) break;
    got += static_cast<std::size_t>(count);
  }
  ::close(fd);
  if (got != bytes) return std::nullopt;
  static const char* hex = "0123456789abcdef";
  std::string out;
  for (unsigned char c : raw) {
    out += hex[c >> 4];
    out += hex[c & 15];
  }
  return out;
}

// Same length and same bytes, without stopping at the first difference.
bool SameSecret(const std::string& left, const std::string& right) {
  if (left.size() != right.size()) return false;
  unsigned char difference = 0;
  for (std::size_t i = 0; i < left.size(); ++i)
    difference |= static_cast<unsigned char>(left[i] ^ right[i]);
  return difference == 0;
}

bool WaitReadable(int fd, Clock::time_point deadline) {
  for (;;) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    if (left <= 0) return false;
    pollfd entry{fd, POLLIN, 0};
    const int ready = ::poll(&entry, 1, static_cast<int>(left));
    if (ready < 0 && errno == EINTR) continue;
    return ready > 0;
  }
}

// Sends all of bytes on a non-blocking socket. Gives up at the deadline, when stop is set, or when
// the peer is gone (a reset must never raise SIGPIPE, and a peer that stops reading must not hold
// the relay).
void SendAll(int fd, const std::string& bytes, Clock::time_point deadline, const std::atomic<bool>& stop) {
#ifdef MSG_NOSIGNAL
  constexpr int kSendFlags = MSG_NOSIGNAL;
#else
  constexpr int kSendFlags = 0;
#endif
  std::size_t at = 0;
  while (at < bytes.size() && !stop && Clock::now() < deadline) {
    const auto count = ::send(fd, bytes.data() + at, bytes.size() - at, kSendFlags);
    if (count > 0) {
      at += static_cast<std::size_t>(count);
      continue;
    }
    if (count < 0 && errno == EINTR) continue;
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      pollfd entry{fd, POLLOUT, 0};
      ::poll(&entry, 1, kPollMs);
      continue;
    }
    return;
  }
}

void MakeNonBlocking(int fd) {
  const int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags >= 0) ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

std::string Response(int status, const char* text, const std::string& body, const char* type) {
  return "HTTP/1.1 " + std::to_string(status) + " " + text + "\r\nContent-Type: " + type +
         "; charset=utf-8\r\nContent-Length: " + std::to_string(body.size()) +
         "\r\nCache-Control: no-store\r\nReferrer-Policy: no-referrer\r\nX-Content-Type-Options: nosniff\r\n"
         "X-Frame-Options: DENY\r\nConnection: close\r\n\r\n" + body;
}

std::string JsonResponse(int status, const char* text, bool ok, const std::string& field, const std::string& value) {
  return Response(status, text, std::string("{\"ok\":") + (ok ? "true" : "false") + ",\"" + field +
                                    "\":" + JsonString(value) + "}",
                  "application/json");
}

std::string Lower(std::string text) {
  for (auto& c : text) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
  return text;
}

std::string Trim(const std::string& text) {
  const auto first = text.find_first_not_of(" \t");
  if (first == std::string::npos) return {};
  return text.substr(first, text.find_last_not_of(" \t") - first + 1);
}

}  // namespace

struct PhoneRelay::State {
  PhoneRelayPage page;
  PhoneRelaySubmit submit;
  std::string host, nonce;
  std::uint16_t port = 0;
  int listener = -1;
  Clock::time_point deadline;
  std::atomic<bool> stop{false}, running{false};
  pthread_t thread{};
  bool joinable = false;
  mutable std::mutex mutex;  // guards status
  PhoneRelayStatus status{PhoneRelayPhase::kWaiting, {}, {}};

  void SetPhase(PhoneRelayPhase phase, std::string display = {}, std::string message = {}) {
    std::lock_guard<std::mutex> lock(mutex);
    status = {phase, std::move(display), std::move(message)};
  }

  // Playden's own page: a credential copied elsewhere (a computer, or the publisher page's own
  // steps) is pasted and sent. No external assets; the script posts to this page's address.
  std::string PastePage() const {
    const auto title = HtmlEscape(page.title);
    return "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
           "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"><title>" + title +
           "</title><style>body{font:17px/1.45 system-ui,sans-serif;margin:24px 16px;max-width:560px;color:#1d1d1f;"
           "background:#fff}@media(prefers-color-scheme:dark){body{color:#f5f5f7;background:#111}}"
           "textarea{width:100%;box-sizing:border-box;min-height:160px;font:14px/1.4 ui-monospace,monospace;"
           "padding:12px;border-radius:10px;border:1px solid #8e8e93}"
           "button{display:block;width:100%;box-sizing:border-box;padding:14px;border-radius:10px;border:0;"
           "font:inherit;font-weight:600;background:#0a6cff;color:#fff;margin-top:12px}"
           "button:disabled{opacity:.5}p.message{padding:12px;border-radius:10px;background:rgba(10,108,255,.12)}"
           "p.message:empty{display:none}small{display:block;margin-top:16px;opacity:.7}</style></head><body><h1>" +
           title + "</h1><p>Paste the sign-in you copied, then send it to your PS5.</p>"
           "<textarea id=\"credential\" autocapitalize=\"off\" autocorrect=\"off\" spellcheck=\"false\"></textarea>"
           "<button id=\"send\" type=\"button\">Send to PS5</button><p id=\"message\" class=\"message\"></p>"
           "<small>It goes straight to your PS5 over your home network, once.</small><script>"
           "(function(){var field=document.getElementById('credential'),button=document.getElementById('send'),"
           "note=document.getElementById('message');button.addEventListener('click',function(){"
           "var text=field.value.trim();try{if(typeof JSON.parse(text)!=='object')throw 0;}catch(e){"
           "note.textContent='That is not a sign-in Playden can read. Copy it again.';return;}"
           "button.disabled=true;note.textContent='Sending to your PS5…';"
           "fetch('/s/" + nonce + "/credential',{method:'POST',cache:'no-store',"
           "headers:{'Content-Type':'application/json'},body:text}).then(function(answer){return answer.json();})"
           ".then(function(answer){if(answer.ok){field.value='';note.textContent='Signed in'+(answer.display?"
           "' as '+answer.display:'')+'. You can close this page.';}else{button.disabled=false;"
           "note.textContent=answer.message||'Your PS5 did not take it.';}})"
           ".catch(function(){button.disabled=false;note.textContent='Your PS5 did not answer. Is Playden still "
           "showing the code?';});});})();</script></body></html>";
  }

  void Send(int fd, const std::string& bytes) const {
    SendAll(fd, bytes, std::min(deadline, Clock::now() + std::chrono::milliseconds(kRequestTimeoutMs)), stop);
  }

  // Returns true once a credential was accepted (the page then closes).
  bool Serve(int client) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(kRequestTimeoutMs);
    std::string request;
    std::size_t header_end = std::string::npos;
    char buffer[2048];
    while (header_end == std::string::npos) {
      if (request.size() > kMaxHeaderBytes + kMaxBodyBytes) {
        Wipe(request);
        Send(client, JsonResponse(413, "Payload Too Large", false, "message", "That is too large."));
        return false;
      }
      const auto got = WaitReadable(client, deadline) ? ::recv(client, buffer, sizeof(buffer), 0) : -1;
      if (got <= 0) {
        Wipe(request);
        return false;
      }
      request.append(buffer, static_cast<std::size_t>(got));
      header_end = request.find("\r\n\r\n");
    }
    const auto line_end = request.find("\r\n");
    const auto line = request.substr(0, line_end);
    const auto first = line.find(' '), second = line.find(' ', first + 1);
    if (first == std::string::npos || second == std::string::npos) {
      Wipe(request);
      Send(client, Response(400, "Bad Request", "Bad request.", "text/plain"));
      return false;
    }
    const auto method = line.substr(0, first);
    auto path = line.substr(first + 1, second - first - 1);
    if (const auto query = path.find('?'); query != std::string::npos) path.erase(query);
    // /s/<nonce>[/paste|/credential]
    std::string rest;
    bool known = path.rfind("/s/", 0) == 0;
    if (known) {
      const auto slash = path.find('/', 3);
      known = SameSecret(path.substr(3, slash == std::string::npos ? std::string::npos : slash - 3), nonce);
      if (slash != std::string::npos) rest = path.substr(slash);
    }
    if (!known || (rest != "" && rest != "/paste" && rest != "/credential")) {
      Wipe(request);
      Send(client, Response(404, "Not Found", "Not found.", "text/plain"));
      return false;
    }
    if (rest != "/credential") {
      Wipe(request);
      if (method != "GET") {
        Send(client, Response(405, "Method Not Allowed", "Method not allowed.", "text/plain"));
        return false;
      }
      {
        std::lock_guard<std::mutex> lock(mutex);
        if (status.phase == PhoneRelayPhase::kWaiting) status.phase = PhoneRelayPhase::kConnected;
      }
      Send(client, Response(200, "OK", rest.empty() && !page.html.empty() ? page.html : PastePage(), "text/html"));
      return false;
    }
    if (method != "POST") {
      Wipe(request);
      Send(client, JsonResponse(405, "Method Not Allowed", false, "message", "Send the sign-in with POST."));
      return false;
    }

    std::size_t length = 0;
    bool has_length = false, json = false;
    std::size_t at = line_end + 2;
    while (at < header_end) {
      const auto end = request.find("\r\n", at);
      const auto header = request.substr(at, end - at);
      at = end + 2;
      const auto colon = header.find(':');
      if (colon == std::string::npos) continue;
      const auto name = Lower(Trim(header.substr(0, colon)));
      const auto value = Trim(header.substr(colon + 1));
      if (name == "content-type") {
        const auto type = Lower(Trim(value.substr(0, value.find(';'))));
        json = type == "application/json";
      } else if (name == "content-length") {
        has_length = !value.empty() && value.size() <= 9 &&
                     std::all_of(value.begin(), value.end(), [](char c) { return c >= '0' && c <= '9'; });
        length = 0;
        if (has_length)
          for (char c : value) length = length * 10 + static_cast<std::size_t>(c - '0');
      }
    }
    const auto refuse = [&](int code, const char* text, const char* message) {
      // Read what the phone already sent before answering, so the answer isn't lost to a reset.
      const auto drain = Clock::now() + std::chrono::milliseconds(300);
      while (WaitReadable(client, drain) && ::recv(client, buffer, sizeof(buffer), 0) > 0) {}
      Wipe(request);
      Send(client, JsonResponse(code, text, false, "message", message));
      return false;
    };
    if (!has_length) return refuse(411, "Length Required", "The sign-in arrived without its length.");
    if (length > kMaxBodyBytes) return refuse(413, "Payload Too Large", "That is too large to be a sign-in.");
    if (!json) return refuse(415, "Unsupported Media Type", "Send the sign-in as JSON.");
    std::string body = request.substr(header_end + 4);
    Wipe(request);
    while (body.size() < length) {
      const auto got = WaitReadable(client, deadline) ? ::recv(client, buffer, sizeof(buffer), 0) : -1;
      if (got <= 0) {
        Wipe(body);
        return false;
      }
      body.append(buffer, static_cast<std::size_t>(got));
    }
    body.resize(length);
    std::memset(buffer, 0, sizeof(buffer));
    SetPhase(PhoneRelayPhase::kChecking);
    // The callback takes the body and wipes it; what a move leaves here is wiped too.
    auto answer = submit(std::move(body));
    Wipe(body);
    if (answer.ok) {
      SetPhase(PhoneRelayPhase::kAccepted, answer.display);
      Send(client, JsonResponse(200, "OK", true, "display", answer.display));
      return true;
    }
    if (answer.message.empty()) answer.message = "Playden could not use that sign-in.";
    SetPhase(PhoneRelayPhase::kRefused, {}, answer.message);
    Send(client, JsonResponse(400, "Bad Request", false, "message", answer.message));
    return false;
  }

  static void* Run(void* raw) {
    // The thread keeps the state alive, so Close can let it finish on its own.
    std::unique_ptr<std::shared_ptr<State>> owner(static_cast<std::shared_ptr<State>*>(raw));
    State* self = owner->get();
    bool accepted = false;
    while (!self->stop && !accepted && Clock::now() < self->deadline) {
      pollfd entry{self->listener, POLLIN, 0};
      const int ready = ::poll(&entry, 1, kPollMs);
      if (ready <= 0) continue;
      const int client = ::accept(self->listener, nullptr, nullptr);
      if (client < 0) continue;  // EAGAIN (a peer that left before accept) included
      MakeNonBlocking(client);
#ifdef SO_NOSIGPIPE
      const int no_sigpipe = 1;
      ::setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe));
#endif
      accepted = self->Serve(client);
      ::shutdown(client, SHUT_WR);
      ::close(client);
    }
    ::close(self->listener);
    self->listener = -1;
    if (!accepted) self->SetPhase(PhoneRelayPhase::kClosed);
    self->running = false;
    return nullptr;
  }
};

PhoneRelay::PhoneRelay() = default;

PhoneRelay::~PhoneRelay() { Stop(); }

bool PhoneRelay::Start(const std::string& host, std::uint16_t first_port, PhoneRelayPage page,
                       PhoneRelaySubmit submit, std::string* reason, std::int64_t lifetime_ms) {
  Stop();
  auto state = std::make_shared<State>();
  const auto nonce = RandomHex(16);
  if (!nonce) {
    *reason = "no random source";
    return false;
  }
  state->nonce = *nonce;
  state->page = std::move(page);
  state->submit = std::move(submit);
  state->host = host;
  const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listener < 0) {
    *reason = "cannot open a socket";
    return false;
  }
  int reuse = 1;
  ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  bool bound = false;
  for (std::uint32_t port = first_port; port < static_cast<std::uint32_t>(first_port) + 10 && port <= 65535 && !bound;
       ++port) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<std::uint16_t>(port));
    address.sin_addr.s_addr = htonl(host == "127.0.0.1" ? INADDR_LOOPBACK : INADDR_ANY);
    if (::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
      bound = true;
      state->port = static_cast<std::uint16_t>(port);
    }
  }
  if (!bound || ::listen(listener, 4) != 0) {
    ::close(listener);
    *reason = "no free port for the sign-in page";
    return false;
  }
  MakeNonBlocking(listener);
  state->listener = listener;
  state->deadline = Clock::now() + std::chrono::milliseconds(lifetime_ms);
  state->running = true;
  pthread_attr_t attributes;
  pthread_attr_init(&attributes);
  pthread_attr_setstacksize(&attributes, kStackBytes);
  auto* owner = new std::shared_ptr<State>(state);
  const int created = pthread_create(&state->thread, &attributes, &State::Run, owner);
  pthread_attr_destroy(&attributes);
  if (created != 0) {
    delete owner;
    ::close(listener);
    *reason = "cannot start the sign-in page";
    return false;
  }
  state->joinable = true;
  state_ = std::move(state);
  return true;
}

std::string PhoneRelay::Address() const {
  if (!state_) return {};
  return "http://" + state_->host + ":" + std::to_string(state_->port) + "/s/" + state_->nonce;
}

bool PhoneRelay::Running() const { return state_ && state_->running; }

PhoneRelayStatus PhoneRelay::Status() const {
  if (!state_) return {};
  std::lock_guard<std::mutex> lock(state_->mutex);
  return state_->status;
}

void PhoneRelay::Close() {
  if (state_) state_->stop = true;
}

void PhoneRelay::Stop() {
  if (!state_) return;
  state_->stop = true;
  if (state_->joinable) {
    pthread_join(state_->thread, nullptr);
    state_->joinable = false;
  }
}

std::optional<std::string> LanAddress() {
  // Connecting a UDP socket sends nothing; it only picks the route and its source address.
  const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) return std::nullopt;
  sockaddr_in remote{};
  remote.sin_family = AF_INET;
  remote.sin_port = htons(53);
  ::inet_pton(AF_INET, "192.0.2.1", &remote.sin_addr);  // TEST-NET-1: never reached
  std::optional<std::string> result;
  sockaddr_in local{};
  socklen_t size = sizeof(local);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&remote), sizeof(remote)) == 0 &&
      ::getsockname(fd, reinterpret_cast<sockaddr*>(&local), &size) == 0 && local.sin_addr.s_addr != 0) {
    char text[INET_ADDRSTRLEN] = {};
    if (::inet_ntop(AF_INET, &local.sin_addr, text, sizeof(text))) result = std::string(text);
  }
  ::close(fd);
  return result;
}

}  // namespace playden::runtime
