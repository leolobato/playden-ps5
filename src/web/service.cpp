// SPDX-License-Identifier: GPL-3.0-or-later
#include "web/service.h"

#include "web/address.h"
#include "web/assets.h"

#include <algorithm>

namespace playden::web {
namespace {
const std::vector<std::pair<std::string, std::string>> kNoStore = {{"Cache-Control", "no-store"}};

// The host part of a Host or Origin authority: an IPv4 literal or localhost. A DNS rebinding site reaches this
// server under its own domain name, and is same-site with itself, so a name is refused on every route. A request
// with no Host at all isn't from a browser.
bool LocalAuthority(std::string_view authority) {
  const auto colon = authority.find(':');
  const auto host = authority.substr(0, colon);
  if (host == "localhost") return true;
  int parts = 0, digits = 0, value = 0;
  for (const char c : host) {
    if (c == '.') {
      if (digits == 0) return false;
      ++parts;
      digits = value = 0;
    } else if (c >= '0' && c <= '9') {
      value = value * 10 + (c - '0');
      if (++digits > 3 || value > 255) return false;
    } else {
      return false;
    }
  }
  return parts == 3 && digits > 0;
}

// An Origin, when sent, must be this server's own: the page's address, as Host names it.
bool SameOrigin(const Request& request) {
  const auto origin = HeaderValue(request, "origin");
  if (!origin) return true;
  const auto host = HeaderValue(request, "host");
  return host && (*origin == "http://" + *host);
}
}  // namespace

Service::Service(ServiceConfig config) : config_(std::move(config)), logs_(config_.paths), files_(config_.files) {
  files_.Register(router_);
  router_.Exact("/", [](const Request&, const std::string&, Response& response) {
    const auto* page = FindEmbeddedAsset("/");
    if (!page) return SendError(response, 404, "Not found.");
    response.Send(200, page->content_type, std::string_view(reinterpret_cast<const char*>(page->data), page->size),
                  {{"Cache-Control", "no-cache"}});
  });
  router_.Prefix("/assets/", [](const Request& request, const std::string&, Response& response) {
    const auto* asset = FindEmbeddedAsset(request.path);
    if (!asset) return SendError(response, 404, "Not found.");
    response.Send(200, asset->content_type, std::string_view(reinterpret_cast<const char*>(asset->data), asset->size),
                  {{"Cache-Control", "max-age=86400"}});
  });
  router_.Exact("/api/status", [this](const Request&, const std::string&, Response& response) {
    response.Send(200, "application/json", logs_.StatusJson(Now()), kNoStore);
  });
  router_.Exact("/api/logs", [this](const Request&, const std::string&, Response& response) {
    response.Send(200, "application/json", logs_.IndexJson(logs_.Index()), kNoStore);
  });
  router_.Prefix("/api/logs/", [this](const Request&, const std::string& name, Response& response) {
    Redaction account;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      account = account_;
    }
    // The name must be one the index lists: no path from the request reaches the file system.
    const auto text = logs_.ReadLog(logs_.Index(), name, account);
    if (!text) return SendError(response, 404, "No such log.");
    response.Send(200, "text/plain; charset=utf-8", *text, kNoStore);
  });
  router_.Exact("/api/report.zip", [this](const Request& request, const std::string&, Response& response) {
    Redaction account;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      account = account_;
    }
    const auto now = Now();
    const auto title = QueryValue(request.query, "title").value_or(std::string());
    const auto index = logs_.Index();
    // Streamed, so no length: the end of the connection ends the zip.
    if (!response.Start(200, "application/zip", std::nullopt,
                        {{"Content-Disposition", "attachment; filename=\"" + ReportFileName(now) + "\""},
                         {"Cache-Control", "no-store"}}) ||
        request.method == Method::kHead)
      return;
    logs_.WriteReport(index, title, account, now, [&response](const char* data, std::size_t size) { return response.Body(data, size); });
  });
  // The only action. It must carry X-Playden-Action: a custom header makes a browser ask first (a CORS
  // preflight, never answered), so another web page can't quit Playden from the player's browser.
  router_.Post("/api/quit", [this](const Request& request, const std::string&, Response& response) {
    bool asked = false;
    for (const auto& [name, value] : request.headers) asked = asked || (name == "x-playden-action" && value == "quit");
    if (!asked) return SendError(response, 403, "Send X-Playden-Action: quit to quit Playden.");
    {
      std::lock_guard<std::mutex> lock(mutex_);
      quit_requested_ = true;
    }
    response.Send(202, "application/json", "{\"quitting\":true}", kNoStore);
  });
  // Remembered browsers are read and written on the worker: never on the UI thread, and never on a request's.
  files_.SetStoreChanged([this] {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      ++store_changes_;
    }
    wake_.notify_all();
  });
  worker_ = std::thread([this] { Run(); });
}

bool Service::TakeQuitRequest() {
  std::lock_guard<std::mutex> lock(mutex_);
  const bool asked = quit_requested_;
  quit_requested_ = false;
  return asked;
}

Service::~Service() { Shutdown(); }

void Service::Log(const std::string& line) const {
  if (config_.server.log) config_.server.log(line);
}

std::int64_t Service::Now() const {
  if (config_.now) return config_.now();
  return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string Service::LookUpAddress() const {
  auto address = LocalAddress();
  if (address.empty() && config_.address_fallback) address = config_.address_fallback();
  return address;
}

void Service::SetEnabled(bool enabled) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (enabled_ == enabled) return;
    enabled_ = enabled;
    ++requests_;
  }
  wake_.notify_all();
}

void Service::SetAccount(Redaction account) {
  std::lock_guard<std::mutex> lock(mutex_);
  account_ = std::move(account);
}

ServiceSnapshot Service::Snapshot() const {
  auto code = files_.PairingCode();
  const auto browsers = files_.PairedBrowsers();
  std::lock_guard<std::mutex> lock(mutex_);
  auto snapshot = snapshot_;
  snapshot.pairing_code = std::move(code);
  snapshot.paired_browsers = browsers;
  return snapshot;
}

void Service::SetFileCatalog(FileCatalog catalog) { files_.SetCatalog(std::move(catalog)); }

void Service::CancelPairing() { files_.CancelPairing(); }

void Service::ForgetBrowsers() {
  files_.ForgetBrowsers();
  Log("forgot paired browsers");
}

bool Service::FilesInUse() const { return files_.InUse(); }

void Service::Shutdown() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
  }
  wake_.notify_all();
  if (worker_.joinable()) worker_.join();
}

void Service::Handle(const Request& request, Response& response) {
  if (const auto host = HeaderValue(request, "host"); host && !LocalAuthority(*host))
    return SendError(response, 403, "Open this page by the PS5's IP address.");
  if (request.method == Method::kPost && !SameOrigin(request))
    return SendError(response, 403, "This request came from another site.");
  router_.Handle(request, response);
}

void Service::Run() {
  using Clock = std::chrono::steady_clock;
  std::unique_lock<std::mutex> lock(mutex_);
  std::uint64_t tried = 0;  // the change a failed start answered; another change tries again
  auto next_lookup = Clock::time_point::max();
  std::uint64_t store_seen = 0;
  auto store_retry = Clock::time_point::max();  // a game's setup held the file lock: try again then
  while (!stopping_) {
    if (store_changes_ != store_seen || Clock::now() >= store_retry) {
      store_seen = store_changes_;
      lock.unlock();
      std::string line;
      const auto work = files_.DoStoreWork(&line);
      if (!line.empty()) Log(line);
      lock.lock();
      store_retry = work == Pairing::StoreWork::kBusy     ? Clock::now() + std::chrono::seconds(1)
                    : work == Pairing::StoreWork::kFailed ? Clock::now() + std::chrono::seconds(30)
                                                          : Clock::time_point::max();
      continue;
    }
    if (enabled_ && !server_.running() && (snapshot_.state != ServiceSnapshot::State::kFailed || tried != requests_)) {
      tried = requests_;
      snapshot_ = {};
      snapshot_.state = ServiceSnapshot::State::kStarting;
      lock.unlock();
      std::string error;
      const bool started = server_.Start(config_.server, [this](const Request& request, Response& response) {
        Handle(request, response);
      }, &error);
      const auto address = started ? LookUpAddress() : std::string();
      lock.lock();
      snapshot_ = {};
      if (started) {
        snapshot_.state = ServiceSnapshot::State::kOn;
        snapshot_.port = server_.port();
        snapshot_.address = address;
        snapshot_.url = PageUrl(address, server_.port());
        Log("started port=" + std::to_string(server_.port()) + " address=" + (address.empty() ? "unknown" : address));
        next_lookup = Clock::now() + config_.address_refresh;
      } else {
        snapshot_.state = ServiceSnapshot::State::kFailed;
        snapshot_.error = error;
      }
      continue;
    }
    if (!enabled_ && server_.running()) {
      lock.unlock();
      server_.Stop();
      Log("stopped");
      lock.lock();
      snapshot_ = {};
      next_lookup = Clock::time_point::max();
      continue;
    }
    if (!enabled_) snapshot_ = {};
    if (server_.running() && Clock::now() >= next_lookup) {
      lock.unlock();
      const auto address = LookUpAddress();
      lock.lock();
      if (address != snapshot_.address && server_.running()) {
        Log("address=" + (address.empty() ? std::string("unknown") : address));
        snapshot_.address = address;
        snapshot_.url = PageUrl(address, snapshot_.port);
      }
      next_lookup = Clock::now() + config_.address_refresh;
      continue;
    }
    const auto next = std::min(next_lookup, store_retry);
    if (next == Clock::time_point::max()) wake_.wait(lock);
    else wake_.wait_until(lock, next);
  }
  const bool was_running = server_.running();
  lock.unlock();
  server_.Stop();
  if (was_running) Log("stopped");
  // A change still unwritten (the file lock was busy) gets one last try.
  std::string line;
  files_.DoStoreWork(&line);
  if (!line.empty()) Log(line);
}

}  // namespace playden::web
