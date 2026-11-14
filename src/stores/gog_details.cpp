// SPDX-License-Identifier: GPL-3.0-or-later
#include "stores/gog_details.h"

#include "gog/api.h"
#include "gog/hash.h"
#include "gog/inflate.h"
#include "gog/manifest_v1.h"

#include <algorithm>
#include <cstdio>
#include <string_view>
#include <utility>

namespace playden::stores {
namespace {

// A build meta or the dependency store's manifest, as GOG serves it (zlib): a few KiB in practice.
constexpr std::size_t kMaxCompressedMetaBytes = 4 * 1024 * 1024;
constexpr std::size_t kMaxCompressedDependencyBytes = 1024 * 1024;

enum class Got { kOk, kNetwork, kCorrupt, kCanceled };

// The zlib file GOG names by its MD5, from the first address that serves it whole, inflated (at most `cap` bytes).
Got FetchZlib(gog::Transport& transport, const std::vector<std::string>& urls, const std::string& md5,
              std::uint64_t cap, std::size_t max_bytes, std::string* inflated, const std::atomic<bool>& cancel) {
  Got last = Got::kNetwork;
  for (const auto& url : urls) {
    if (cancel.load()) return Got::kCanceled;
    steam::HttpRequest request;
    request.url = url;
    request.timeout_ms = gog::kApiTimeoutMs;
    request.connect_timeout_ms = gog::kApiConnectTimeoutMs;
    request.max_response_bytes = max_bytes;
    request.headers.emplace_back("User-Agent", gog::kUserAgent);
    const auto response = transport(request, cancel);
    if (cancel.load() || response.error == steam::HttpError::kCanceled) return Got::kCanceled;
    if (!response.ok() || response.status_code != 200) {
      last = Got::kNetwork;
      continue;
    }
    gog::Md5 hash;
    hash.Update(response.body.data(), response.body.size());
    if (hash.FinalHex() != md5) {
      last = Got::kCorrupt;
      continue;
    }
    std::string text;
    gog::ZlibStream stream(cap);
    auto status = stream.Feed(response.body.data(), response.body.size(), [&text](const std::uint8_t* data, std::size_t size) {
      text.append(reinterpret_cast<const char*>(data), size);
      return true;
    });
    if (status == gog::ZlibStream::Status::kOk) status = stream.Finish();
    if (status != gog::ZlibStream::Status::kOk) {
      last = Got::kCorrupt;
      continue;
    }
    *inflated = std::move(text);
    return Got::kOk;
  }
  return last;
}

// A plain file with no hash to check (a gen 1 repository.json), from the first address that answers 200.
Got FetchPlain(gog::Transport& transport, const std::vector<std::string>& urls, std::size_t max_bytes,
               std::string* body, const std::atomic<bool>& cancel) {
  for (const auto& url : urls) {
    if (cancel.load()) return Got::kCanceled;
    steam::HttpRequest request;
    request.url = url;
    request.timeout_ms = gog::kApiTimeoutMs;
    request.connect_timeout_ms = gog::kApiConnectTimeoutMs;
    request.max_response_bytes = max_bytes;
    request.headers.emplace_back("User-Agent", gog::kUserAgent);
    auto response = transport(request, cancel);
    if (cancel.load() || response.error == steam::HttpError::kCanceled) return Got::kCanceled;
    if (!response.ok() || response.status_code != 200) continue;
    body->assign(reinterpret_cast<const char*>(response.body.data()), response.body.size());
    return Got::kOk;
  }
  return Got::kNetwork;
}

// The CDN base of a URL: everything before `marker` (/content-system/v2/meta/ for a build meta,
// /content-system/v1/manifests/ for a gen 1 repository).
std::optional<std::string> CdnBase(const std::string& url, std::string_view marker = "/content-system/v2/meta/") {
  const auto at = url.find(marker);
  if (at == std::string::npos || at < 9) return std::nullopt;
  return url.substr(0, at);
}

std::string LastSegment(const std::string& url) {
  const auto end = url.find_first_of("?#");
  const auto path = url.substr(0, end);
  const auto slash = path.rfind('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

}  // namespace

GogDetailsSource::GogDetailsSource(gog::Session& session, std::function<gog::Transport()> make_transport,
                                   std::function<std::string()> content_language, LogSink log)
    : session_(session),
      make_transport_(std::move(make_transport)),
      content_language_(std::move(content_language)),
      log_(std::move(log)) {
  pthread_attr_t attributes;
  if (pthread_attr_init(&attributes) != 0) {
    Log("details result=no_worker");
    return;
  }
  bool ok = pthread_attr_setstacksize(&attributes, kWorkerStackBytes) == 0;
  ok = ok && pthread_create(&worker_, &attributes,
                            [](void* self) -> void* {
                              static_cast<GogDetailsSource*>(self)->Run();
                              return nullptr;
                            },
                            this) == 0;
  pthread_attr_destroy(&attributes);
  started_ = ok;
  if (!ok) Log("details result=no_worker");
}

GogDetailsSource::~GogDetailsSource() { Shutdown(); }

void GogDetailsSource::Log(const std::string& line) const {
  if (log_) log_(line);
}

void GogDetailsSource::Request(const GameId& id, std::uint64_t generation) { Queue(id, generation, false, false); }

void GogDetailsSource::RequestFirst(const GameId& id, std::uint64_t generation) { Queue(id, generation, true, false); }

void GogDetailsSource::Refresh(const GameId& id, std::uint64_t generation, bool first) {
  Queue(id, generation, first, true);
}

void GogDetailsSource::Queue(const GameId& id, std::uint64_t generation, bool first, bool again) {
  if (id.store != Store::kGog || !id.gog_product()) return;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_ || !started_) return;
    if (generation != generation_) {
      if (generation < generation_) return;  // a request of an earlier sign-in
      ResetLocked(generation);
    }
    if (again) requested_.erase(id);
    const auto queued = std::find(pending_.begin(), pending_.end(), id);
    if (queued != pending_.end()) {
      if (first && queued != pending_.begin()) {
        pending_.erase(queued);
        pending_.push_front(id);
      }
      return;
    }
    if (requested_.count(id)) return;
    if (pending_.size() >= kMaximumPending) {
      if (!first) return;
      requested_.erase(pending_.back());
      pending_.pop_back();
    }
    requested_.insert(id);
    if (first) pending_.push_front(id);
    else pending_.push_back(id);
  }
  wake_.notify_all();
}

void GogDetailsSource::ResetLocked(std::uint64_t generation) {
  generation_ = generation;
  pending_.clear();
  requested_.clear();
  events_.clear();
  if (active_cancel_) active_cancel_->store(true);
}

void GogDetailsSource::Invalidate(std::uint64_t generation) {
  std::lock_guard<std::mutex> lock(mutex_);
  ResetLocked(generation);
}

std::vector<GameDetails> GogDetailsSource::Pump(std::size_t maximum) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<GameDetails> result;
  while (!events_.empty() && result.size() < maximum) {
    if (events_.front().account_generation == generation_) result.push_back(std::move(events_.front()));
    events_.pop_front();
  }
  return result;
}

void GogDetailsSource::Shutdown() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
    pending_.clear();
    if (active_cancel_) active_cancel_->store(true);
    if (!started_ || joined_) return;
    joined_ = true;
  }
  wake_.notify_all();
  pthread_join(worker_, nullptr);
}

void GogDetailsSource::Run() {
  for (;;) {
    GameId id;
    std::uint64_t generation = 0;
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    {
      std::unique_lock<std::mutex> lock(mutex_);
      wake_.wait(lock, [this] { return stopping_ || !pending_.empty(); });
      if (stopping_) return;
      id = pending_.front();
      pending_.pop_front();
      generation = generation_;
      active_cancel_ = cancel;
    }
    auto details = Fetch(id, generation, *cancel);
    std::lock_guard<std::mutex> lock(mutex_);
    if (active_cancel_ == cancel) active_cancel_.reset();
    if (stopping_) return;
    // Invalidate, or a newer sign-in, came meanwhile: the answer is dropped.
    if (cancel->load() || generation != generation_) continue;
    // Asking again may work: the next visit asks.
    if (details.failed) requested_.erase(id);
    if (events_.size() >= kMaximumEvents) {
      requested_.erase(events_.front().id);
      events_.pop_front();
    }
    events_.push_back(std::move(details));
  }
}

GameDetails GogDetailsSource::Fetch(const GameId& id, std::uint64_t generation, const std::atomic<bool>& cancel) {
  GameDetails details;
  details.id = id;
  details.account_generation = generation;
  const auto product = *id.gog_product();
  if (!transport_made_) {
    transport_made_ = true;
    if (make_transport_) transport_ = make_transport_();
  }
  const auto failed = [&](const char* kind) {
    details.failed = true;
    Log(std::string("details result=") + kind);
    return details;
  };
  // Known, but not installable from what GOG sent: the page says so (and asks again at the next sign-in), rather than
  // staying on "Checking GOG…".
  const auto unusable = [&](const char* kind) {
    details.available = true;
    details.windows_installable = false;
    details.reason = "GOG returned unexpected data for this game.";
    Log(std::string("details result=") + kind);
    return details;
  };
  const auto not_installable = [&](const char* reason, const char* kind) {
    details.available = true;
    details.windows_installable = false;
    details.reason = reason;
    Log(std::string("details result=") + kind);
    return details;
  };
  if (!transport_) return failed("network");

  const auto builds = gog::FetchBuilds(transport_, product, cancel);
  if (cancel.load() || builds.canceled) return details;
  if (!builds.answered || builds.status == 429 || builds.status >= 500) return failed("network");
  if (builds.status == 404) return not_installable(kNoWindowsReason, "no_build");
  if (builds.status != 200) return unusable("protocol");
  const auto choice = gog::ChooseBuild(builds.body);
  switch (choice.kind) {
    case gog::BuildChoice::Kind::kGen2:
    case gog::BuildChoice::Kind::kGen1: break;
    case gog::BuildChoice::Kind::kNone: return not_installable(kNoWindowsReason, "no_build");
    case gog::BuildChoice::Kind::kProtocol: return unusable("protocol");
  }
  const bool gen1 = choice.kind == gog::BuildChoice::Kind::kGen1;

  std::vector<std::string> urls;
  std::vector<std::string> bases;
  for (const auto& endpoint : gen1 ? choice.repository : choice.meta)
    if (auto url = gog::FillUrl(endpoint, "")) {
      if (auto base = gen1 ? CdnBase(*url, "/content-system/v1/manifests/") : CdnBase(*url))
        bases.push_back(std::move(*base));
      urls.push_back(std::move(*url));
    }
  std::optional<gog::Meta> meta;
  std::optional<gog::RepositoryV1> repository;
  if (gen1) {
    std::string body;
    const auto got = FetchPlain(transport_, urls, gog::kMaxRepositoryV1Bytes, &body, cancel);
    if (got == Got::kCanceled) return details;
    if (got != Got::kOk) return failed("network");
    repository = gog::ParseRepositoryV1(body);
    if (!repository || repository->root_product != product || repository->timestamp != choice.timestamp)
      return unusable("protocol");
  } else {
    std::string inflated;
    const auto got =
        FetchZlib(transport_, urls, choice.meta_id, gog::kMaxMetaBytes, kMaxCompressedMetaBytes, &inflated, cancel);
    if (got == Got::kCanceled) return details;
    if (got != Got::kOk) return failed(got == Got::kCorrupt ? "corrupt" : "network");
    meta = gog::ParseMeta(inflated);
    if (!meta || meta->base_product != product) return unusable("protocol");
  }

  // Every owned DLC is installed with the game; without the owned list, only the game is counted.
  std::set<std::uint64_t> owned{product};
  const auto list = Owned(cancel);
  if (cancel.load()) return details;
  if (list) owned.insert(list->begin(), list->end());
  const auto language = content_language_ ? content_language_() : std::string();
  std::uint64_t download = 0, install = 0;
  std::size_t depots = 0, products = 0;
  std::vector<std::string> wanted;
  if (gen1) {
    const auto selection = gog::SelectDepotsV1(*repository, owned, language);
    download = selection.compressed_size;
    install = selection.size;
    depots = selection.depots.size();
    products = selection.products.size();
    wanted = selection.dependencies;
  } else {
    const auto selection = gog::SelectDepots(*meta, owned, language);
    download = selection.compressed_size;
    install = selection.size;
    depots = selection.depots.size();
    products = selection.products.size();
    wanted = meta->dependencies;
  }
  std::size_t folder_dependencies = 0;
  if (!wanted.empty()) {
    const auto read = Dependencies(bases, cancel);
    if (read == Fetched::kCanceled) return details;
    if (read == Fetched::kNetwork) return failed("network");
    if (read == Fetched::kProtocol) return unusable("protocol");
    folder_dependencies = AddDependencies(wanted, &install, &download);
  }
  details.available = true;
  details.windows_installable = true;
  details.download_bytes = download;
  details.install_bytes = install;
  char line[160];
  std::snprintf(line, sizeof(line), "details result=%s depots=%zu products=%zu dependencies=%zu owned=%s",
                gen1 ? "gen1" : "gen2", depots, products, folder_dependencies, list ? "list" : "base");
  Log(line);
  return details;
}

std::size_t GogDetailsSource::AddDependencies(const std::vector<std::string>& wanted, std::uint64_t* install,
                                              std::uint64_t* download) const {
  std::size_t count = 0;
  for (std::size_t index = 0; index < wanted.size(); ++index) {
    const auto& id = wanted[index];
    // Named twice, installed once (the installer drops repeats too).
    if (std::find(wanted.begin(), wanted.begin() + static_cast<std::ptrdiff_t>(index), id) !=
        wanted.begin() + static_cast<std::ptrdiff_t>(index))
      continue;
    const auto found = std::find_if(dependencies_->begin(), dependencies_->end(),
                                    [&id](const gog::Dependency& dependency) { return dependency.id == id; });
    // Game-folder files and __redist installers alike: the installer downloads both.
    if (found == dependencies_->end()) continue;
    *download += found->compressed_size;
    *install += found->size;
    ++count;
  }
  return count;
}

std::optional<std::set<std::uint64_t>> GogDetailsSource::Owned(const std::atomic<bool>& cancel) {
  // Read once per sign-in: a purchase since shows after the next sign-in or restart.
  if (owned_generation_ != 0 && session_.signed_in() && session_.generation() == owned_generation_) return owned_;
  for (int round = 0; round < 2; ++round) {
    auto token = session_.Token(cancel);
    if (token.status != gog::AccessToken::Status::kOk) return std::nullopt;
    const auto result = gog::FetchOwned(transport_, token.token, cancel);
    switch (result.kind) {
      case gog::OwnedResult::Kind::kOk:
        owned_ = std::set<std::uint64_t>(result.ids.begin(), result.ids.end());
        owned_generation_ = token.generation;
        return owned_;
      case gog::OwnedResult::Kind::kRejected:
        // Once: the session drops the token and the next one is fresh.
        if (round == 0) session_.InvalidateAccessToken(token.generation, token.token);
        continue;
      case gog::OwnedResult::Kind::kNetwork:
      case gog::OwnedResult::Kind::kProtocol: return std::nullopt;
    }
  }
  return std::nullopt;
}

GogDetailsSource::Fetched GogDetailsSource::Dependencies(const std::vector<std::string>& bases,
                                                         const std::atomic<bool>& cancel) {
  if (dependencies_) return Fetched::kOk;
  const auto answer = gog::FetchDependencyRepository(transport_, cancel);
  if (cancel.load() || answer.canceled) return Fetched::kCanceled;
  if (!answer.answered || answer.status == 429 || answer.status >= 500) return Fetched::kNetwork;
  if (answer.status != 200) return Fetched::kProtocol;
  const auto repository = gog::ParseDependencyRepository(answer.body);
  if (!repository) return Fetched::kProtocol;
  const auto id = LastSegment(repository->manifest_url);
  const auto galaxy = gog::GalaxyPath(id);
  if (!gog::IsMd5Hex(id) || !galaxy) return Fetched::kProtocol;
  std::vector<std::string> urls{repository->manifest_url};
  for (const auto& base : bases) {
    auto url = base + "/content-system/v2/dependencies/meta/" + *galaxy;
    if (std::find(urls.begin(), urls.end(), url) == urls.end()) urls.push_back(std::move(url));
  }
  std::string inflated;
  const auto got = FetchZlib(transport_, urls, id, gog::kMaxDependencyManifestBytes, kMaxCompressedDependencyBytes,
                             &inflated, cancel);
  if (got == Got::kCanceled) return Fetched::kCanceled;
  if (got != Got::kOk) return Fetched::kNetwork;
  auto parsed = gog::ParseDependencyManifest(inflated);
  if (!parsed) return Fetched::kProtocol;
  dependencies_ = std::move(*parsed);
  return Fetched::kOk;
}

}  // namespace playden::stores
