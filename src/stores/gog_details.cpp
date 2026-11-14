// SPDX-License-Identifier: GPL-3.0-or-later
#include "stores/gog_details.h"

#include "gog/api.h"
#include "gog/hash.h"
#include "gog/inflate.h"
#include "gog/installer.h"
#include "gog/languages.h"
#include "gog/manifest_v1.h"
#include "gog/plan_file.h"
#include "gog/update_diff.h"
#include "stores/gog_update.h"

#include <sys/stat.h>

#include <algorithm>
#include <cerrno>
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

// A DLC's name from a build meta, as a page shows it: control characters dropped, trimmed, at most kMaxDlcNameBytes
// (cut on a character boundary).
constexpr std::size_t kMaxDlcNameBytes = 256;
std::string DlcName(const std::string& raw) {
  std::string name;
  for (const char c : raw) {
    const auto byte = static_cast<unsigned char>(c);
    if (byte < 0x20 || byte == 0x7f) continue;
    name.push_back(c);
  }
  const auto first = name.find_first_not_of(' ');
  if (first == std::string::npos) return {};
  name = name.substr(first, name.find_last_not_of(' ') - first + 1);
  if (name.size() > kMaxDlcNameBytes) {
    std::size_t cut = kMaxDlcNameBytes;
    while (cut > 0 && (static_cast<unsigned char>(name[cut]) & 0xC0) == 0x80) --cut;
    name.resize(cut);
    const auto last = name.find_last_not_of(' ');
    name.resize(last == std::string::npos ? 0 : last + 1);
  }
  return name;
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

void GogDetailsSource::SetInstalled(InstalledLookup installed) {
  std::lock_guard<std::mutex> lock(mutex_);
  installed_ = std::move(installed);
}

void GogDetailsSource::SetInstallRoot(RootLookup root) {
  std::lock_guard<std::mutex> lock(mutex_);
  install_root_ = std::move(root);
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
  check_ = {};
  check_pending_.clear();
  check_waiting_.clear();
  ++check_serial_;
  measure_pending_.clear();
  measure_requested_.clear();
  measured_.clear();
  if (active_cancel_) active_cancel_->store(true);
}

bool GogDetailsSource::CheckRoomLocked() const {
  const auto waiting = std::count_if(events_.begin(), events_.end(), [](const Event& event) { return event.check != 0; });
  return static_cast<std::size_t>(waiting) < kMaximumEvents;
}

void GogDetailsSource::PushLocked(GameDetails details, std::uint64_t check) {
  // kMaximumEvents pages' answers waiting: the oldest goes (its page asks again). A check's answers are never dropped
  // and never push a page's out: the worker takes no more of the check while kMaximumEvents of them wait.
  if (check == 0) {
    const auto pages = std::count_if(events_.begin(), events_.end(), [](const Event& event) { return event.check == 0; });
    if (static_cast<std::size_t>(pages) >= kMaximumEvents) {
      const auto page =
          std::find_if(events_.begin(), events_.end(), [](const Event& event) { return event.check == 0; });
      requested_.erase(page->details.id);
      events_.erase(page);
    }
  }
  events_.push_back({std::move(details), check});
}

bool GogDetailsSource::CheckInstalled(const std::vector<GameId>& ids, std::uint64_t generation) {
  if (generation == 0) return false;
  std::deque<GameId> games;
  std::set<GameId> seen;
  for (const auto& id : ids)
    if (id.store == Store::kGog && id.gog_product() && seen.insert(id).second) games.push_back(id);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_ || !started_ || generation < generation_) return false;
    if (generation != generation_) ResetLocked(generation);
    if (check_.total && !check_.done()) return false;
    if (games.empty()) return false;
    check_ = {};
    check_waiting_.clear();
    ++check_serial_;
    check_.account_generation = generation;
    check_.total = games.size();
    check_pending_ = std::move(games);
  }
  wake_.notify_all();
  return true;
}

steam::InstalledCheck GogDetailsSource::CheckProgress() {
  std::lock_guard<std::mutex> lock(mutex_);
  return check_;
}

bool GogDetailsSource::Measure(const steam::SizeRequest& request, std::uint64_t generation) {
  if (!request.update || request.id.store != Store::kGog || !request.id.gog_product() || generation == 0) return false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_ || !started_ || generation < generation_) return false;
    if (generation != generation_) ResetLocked(generation);
    if (measure_requested_.count(request)) return true;
    if (measure_pending_.size() >= kMaximumPending) return false;
    measure_requested_.insert(request);
    measure_pending_.push_back(request);
  }
  wake_.notify_all();
  return true;
}

std::vector<steam::MeasuredSize> GogDetailsSource::PumpMeasured() {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<steam::MeasuredSize> result;
  for (auto& size : measured_)
    if (size.account_generation == generation_) result.push_back(std::move(size));
  measured_.clear();
  return result;
}

void GogDetailsSource::Invalidate(std::uint64_t generation) {
  std::lock_guard<std::mutex> lock(mutex_);
  ResetLocked(generation);
}

std::vector<GameDetails> GogDetailsSource::Pump(std::size_t maximum) {
  std::vector<GameDetails> result;
  bool taken = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    while (!events_.empty() && result.size() < maximum) {
      auto& event = events_.front();
      if (event.details.account_generation == generation_) {
        // The check counts a game once its answer is handed out, so its progress never runs ahead of the answers. An
        // earlier answer for the same game (a page's) is not the check's.
        if (event.check != 0 && event.check == check_serial_ && check_waiting_.erase(event.details.id)) {
          ++check_.answered;
          if (event.details.failed) ++check_.failed;
        }
        result.push_back(std::move(event.details));
      }
      events_.pop_front();
      taken = true;
    }
  }
  // Room for the check's next answers.
  if (taken) wake_.notify_all();
  return result;
}

void GogDetailsSource::Shutdown() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
    pending_.clear();
    check_pending_.clear();
    measure_pending_.clear();
    if (active_cancel_) active_cancel_->store(true);
    if (!started_ || joined_) return;
    joined_ = true;
  }
  wake_.notify_all();
  pthread_join(worker_, nullptr);
}

void GogDetailsSource::Run() {
  for (;;) {
    enum class Job { kPage, kCheck, kMeasure };
    Job job = Job::kPage;
    GameId id;
    steam::SizeRequest size;
    std::uint64_t generation = 0, check = 0;
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    {
      std::unique_lock<std::mutex> lock(mutex_);
      wake_.wait(lock, [this] {
        return stopping_ || !pending_.empty() || (!check_pending_.empty() && CheckRoomLocked()) ||
               !measure_pending_.empty();
      });
      if (stopping_) return;
      // Pages first, then the installed check, then updates' sizes.
      if (!pending_.empty()) {
        id = pending_.front();
        pending_.pop_front();
      } else if (!check_pending_.empty() && CheckRoomLocked()) {
        job = Job::kCheck;
        id = check_pending_.front();
        check_pending_.pop_front();
        ++check_.asked;
        check_waiting_.insert(id);
        requested_.insert(id);
        check = check_serial_;
      } else {
        job = Job::kMeasure;
        size = measure_pending_.front();
        measure_pending_.pop_front();
      }
      generation = generation_;
      active_cancel_ = cancel;
    }
    if (job == Job::kMeasure) {
      auto measured = MeasureUpdate(size, generation, *cancel);
      std::lock_guard<std::mutex> lock(mutex_);
      if (active_cancel_ == cancel) active_cancel_.reset();
      if (stopping_) return;
      if (cancel->load() || generation != generation_) continue;
      measure_requested_.erase(size);
      measured_.push_back(std::move(measured));
      continue;
    }
    auto details = Fetch(id, generation, *cancel);
    std::lock_guard<std::mutex> lock(mutex_);
    if (active_cancel_ == cancel) active_cancel_.reset();
    if (stopping_) return;
    // Invalidate, or a newer sign-in, came meanwhile: the answer is dropped.
    if (cancel->load() || generation != generation_) continue;
    // Asking again may work: the next visit asks.
    if (details.failed) requested_.erase(id);
    PushLocked(std::move(details), check);
  }
}

GameDetails GogDetailsSource::Fetch(const GameId& id, std::uint64_t generation, const std::atomic<bool>& cancel) {
  GameDetails details;
  details.id = id;
  details.account_generation = generation;
  const auto product = *id.gog_product();
  // An installed game: whether GOG's default build is still the one it holds.
  const auto record = installed_ ? installed_(id) : std::nullopt;
  const auto no_update = [&] {
    if (record) Log("details update result=none");
  };
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
    no_update();
    return details;
  };
  const auto not_installable = [&](const char* reason, const char* kind) {
    details.available = true;
    details.windows_installable = false;
    details.reason = reason;
    Log(std::string("details result=") + kind);
    no_update();
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
  latest_builds_[product] = std::to_string(choice.generation) + ":" + choice.build_id;

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

  // The language the depots are selected in: an installed game's own, as its update keeps it (MeasureUpdate);
  // otherwise Settings › Game language.
  std::string language;
  if (record && record->gog) {
    language = gog::SteamCodeFor(record->gog->language);
    if (language.empty()) language = record->language;
    if (language.empty()) language = "english";
  } else if (content_language_) {
    language = content_language_();
  }
  // Every owned DLC is installed with the game; without the owned list, only the game is counted (and no DLC is
  // listed). An installed game's sizes leave out the DLC its record left out; its DLC list doesn't: a DLC turned off
  // is still one the player can choose.
  std::set<std::uint64_t> owned{product};
  const auto list = Owned(cancel);
  if (cancel.load()) return details;
  if (list) owned.insert(list->begin(), list->end());
  std::set<std::uint64_t> sized = owned;
  if (record && record->gog)
    for (const auto id : record->gog->excluded)
      if (id != product) sized.erase(id);
  // Each owned DLC the selection takes, after the base: its selected depots' sizes. One whose depots are all of
  // another language or none is still listed with 0 install bytes (the model hides it, as for Steam's).
  std::vector<DlcChoice> dlc;
  std::uint64_t download = 0, install = 0;
  std::size_t depots = 0, products = 0;
  std::vector<std::string> wanted;
  std::vector<std::uint64_t> taken;  // the products the sizes count: the update's, for its target
  if (gen1) {
    const auto all = gog::SelectDepotsV1(*repository, owned, language);
    for (const auto id : all.products) {
      if (id == product) continue;
      DlcChoice entry;
      entry.id = GameId::Gog(id);
      for (const auto& depot : all.depots)
        if (depot.selected_product == id) {
          entry.download_bytes += depot.size;
          entry.install_bytes += depot.size;
        }
      dlc.push_back(std::move(entry));
    }
    const auto selection = sized == owned ? all : gog::SelectDepotsV1(*repository, sized, language);
    download = selection.compressed_size;
    install = selection.size;
    depots = selection.depots.size();
    products = selection.products.size();
    taken = selection.products;
    wanted = selection.dependencies;
  } else {
    const auto all = gog::SelectDepots(*meta, owned, language);
    for (const auto id : all.products) {
      if (id == product) continue;
      DlcChoice entry;
      entry.id = GameId::Gog(id);
      for (const auto& [named, name] : meta->products)
        if (named == id) {
          entry.name = DlcName(name);
          break;
        }
      for (const auto& depot : all.depots)
        if (depot.product == id) {
          entry.download_bytes += depot.compressed_size;
          entry.install_bytes += depot.size;
        }
      dlc.push_back(std::move(entry));
    }
    const auto selection = sized == owned ? all : gog::SelectDepots(*meta, sized, language);
    download = selection.compressed_size;
    install = selection.size;
    depots = selection.depots.size();
    products = selection.products.size();
    taken = selection.products;
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
  char line[192];
  std::snprintf(line, sizeof(line), "details result=%s depots=%zu products=%zu dependencies=%zu owned=%s dlc=%zu",
                gen1 ? "gen1" : "gen2", depots, products, folder_dependencies, list ? "list" : "base", dlc.size());
  Log(line);
  if (record) {
    // GOG never changes a published build: another build ID, or the same one in another generation, is an update. An
    // owned DLC with files that the record neither holds nor left out was bought since: the update brings it.
    const char* update = "none";
    if (record->gog) {
      update = "current";
      const auto& gog_record = *record->gog;
      const auto in = [](const std::vector<std::uint64_t>& ids, std::uint64_t id) {
        return std::find(ids.begin(), ids.end(), id) != ids.end();
      };
      for (const auto& entry : dlc) {
        const auto id = *entry.id.gog_product();
        if (entry.install_bytes != 0 && !in(gog_record.products, id) && !in(gog_record.excluded, id))
          details.update.new_dlc = true;
      }
      if (choice.build_id != gog_record.build_id || choice.generation != gog_record.generation) {
        details.update.newer = true;
        update = "newer";
      } else if (details.update.new_dlc) {
        update = "new_dlc";
      }
      // The target names the build and the products the update takes, as the measure's plan does: a DLC bought or
      // turned off since is another update, measured again. Without the owned list the products aren't known: no
      // target (0), and the measure names it.
      if (details.update.available() && list)
        details.update.target = gog::UpdateTarget(choice.build_id, choice.generation, taken);
    }
    Log(std::string("details update result=") + update);
  }
  details.dlc = std::move(dlc);
  return details;
}

steam::MeasuredSize GogDetailsSource::MeasureUpdate(const steam::SizeRequest& request, std::uint64_t generation,
                                                    const std::atomic<bool>& cancel) {
  using Error = gog::GogInstaller::Outcome::Error;
  steam::MeasuredSize result;
  result.request = request;
  result.account_generation = generation;
  std::uint64_t changed = 0, removed = 0;
  const auto answer = [&](const char* kind, const char* why, bool cached = false) {
    char line[160];
    std::snprintf(line, sizeof(line), "details size result=%s changed=%llu removed=%llu%s%s%s", kind,
                  static_cast<unsigned long long>(changed), static_cast<unsigned long long>(removed),
                  why ? " why=" : "", why ? why : "", cached ? " cached=1" : "");
    Log(line);
    return result;
  };
  const auto product = *request.id.gog_product();
  const auto record = installed_ ? installed_(request.id) : std::nullopt;
  if (!record || !record->gog) return answer("skipped", "no_record");
  const auto root = install_root_ ? install_root_(request.id) : std::nullopt;
  if (!root || root->empty() || root->front() != '/') return answer("skipped", "no_root");
  {
    // An update under way: its own staging says what changes, and the installed plan may be on its way out.
    const auto state = *root + "/" + installs::kUpdateDirName + "/" + installs::kUpdateStateName;
    struct stat info {};
    if (::lstat(state.c_str(), &info) == 0 || errno != ENOENT) return answer("skipped", "updating");
  }
  // The language the game has, as the runner's update keeps it: it selects the depots.
  std::string language = gog::SteamCodeFor(record->gog->language);
  if (language.empty()) language = record->language;
  if (language.empty()) language = "english";

  // Measured before: the folder, the installed build and its products and DLC left out, and the one GOG last named
  // for the game decide. A DLC change on the same build is measured again.
  if (measured_generation_ != generation) {
    measured_updates_.clear();
    measured_generation_ = generation;
  }
  const auto joined = [](const std::vector<std::uint64_t>& ids) {
    std::string text;
    for (const auto id : ids) text += (text.empty() ? "" : ",") + std::to_string(id);
    return text;
  };
  const auto installed_build = std::to_string(record->gog->generation) + ":" + record->gog->build_id + "\n" +
                               joined(record->gog->products) + "\n" + joined(record->gog->excluded);
  const auto key = [&](const std::string& latest) {
    return *root + "\n" + installed_build + "\n" + latest + "\n" + language;
  };
  if (const auto latest = latest_builds_.find(product); latest != latest_builds_.end()) {
    if (const auto known = measured_updates_.find(key(latest->second)); known != measured_updates_.end()) {
      result.measured = true;
      result.download_bytes = known->second.download_bytes;
      result.staged_bytes = known->second.staged_bytes;
      result.install_bytes = known->second.install_bytes;
      result.target = known->second.target;
      changed = known->second.changed;
      removed = known->second.removed;
      return answer("measured", nullptr, true);
    }
  }

  // The default build planned beside the install, then compared with it. The measure folder never outlives this.
  struct Cleanup {
    const std::string& root;
    ~Cleanup() { (void)RemoveGogMeasure(root); }
  } cleanup{*root};
  (void)RemoveGogMeasure(*root);
  gog::GogInstaller::Hooks hooks;
  hooks.make_transport = make_transport_;
  hooks.token = [this](const std::atomic<bool>& flag) { return session_.Token(flag); };
  hooks.invalidate = [this](std::uint64_t token_generation, std::string_view token) {
    session_.InvalidateAccessToken(token_generation, token);
  };
  gog::GogInstaller installer(std::move(hooks));
  gog::GogInstaller::Target target;
  target.product = product;
  target.root = *root;
  target.language = language;
  target.seed_list = *root + "/" + gog::kFileListName;
  // As the update would plan it: without the DLC the record left out.
  target.excluded = record->gog->excluded;
  const auto layout = gog::MeasureLayout(*root);
  const auto planned = installer.Plan(target, layout, cancel);
  if (cancel.load() || planned.error == Error::kCanceled) {
    result.failed = true;
    return answer("failed", "canceled");
  }
  if (planned.error == Error::kNetwork || planned.error == Error::kSignedOut) {
    result.failed = true;
    return answer("failed", gog::InstallerErrorName(planned.error));
  }
  if (planned.error != Error::kNone) return answer("skipped", gog::InstallerErrorName(planned.error));
  const auto latest = std::to_string(planned.generation) + ":" + planned.build_id;
  latest_builds_[product] = latest;
  const auto diff = gog::DiffPlans(gog::InstallLayout(*root).plan, layout.plan, *root, cancel, {});
  if (diff.canceled || cancel.load()) {
    result.failed = true;
    return answer("failed", "canceled");
  }
  if (!diff.readable) return answer("skipped", "damaged");
  changed = diff.changed_files;
  removed = diff.removed_files;
  result.measured = true;
  result.download_bytes = diff.download_bytes;
  result.staged_bytes = diff.staged_bytes;
  result.install_bytes = diff.install_bytes;
  result.target = gog::UpdateTarget(planned.build_id, planned.generation, planned.products);
  // The plan read the owned list now: a DLC bought since this sign-in's list was read joins it, so that the game's
  // details, asked again, name the same update.
  if (owned_generation_ != 0 && owned_generation_ == session_.generation())
    owned_.insert(planned.products.begin(), planned.products.end());
  measured_updates_[key(latest)] = {result.download_bytes, result.staged_bytes, result.install_bytes, result.target,
                                    changed, removed};
  return answer("measured", nullptr);
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
