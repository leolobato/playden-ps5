#include "steam/store_details_service.h"

#include "steam/backend/json.h"

#include <utility>

namespace playden::steam {
namespace {

// The store's category for "Valve Anti-Cheat enabled".
constexpr std::uint64_t kVacCategory = 8;

// The store writes HTML: tags go, common entities become their characters, and runs
// of whitespace collapse, as the macOS app shows it.
std::string PlainText(std::string_view html) {
  std::string text;
  text.reserve(html.size());
  bool tag = false;
  for (const char c : html) {
    if (c == '<') { tag = true; text += ' '; continue; }
    if (c == '>') { tag = false; continue; }
    if (!tag) text += c;
  }
  static const std::pair<const char*, const char*> kEntities[] = {
      {"&quot;", "\""}, {"&#39;", "'"}, {"&apos;", "'"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&nbsp;", " "}, {"&amp;", "&"}};
  for (const auto& [entity, value] : kEntities) {
    const std::string_view from(entity);
    for (auto at = text.find(from); at != std::string::npos; at = text.find(from, at + 1)) text.replace(at, from.size(), value);
  }
  std::string collapsed;
  collapsed.reserve(text.size());
  for (const char c : text) {
    const bool space = c == ' ' || c == '\n' || c == '\r' || c == '\t';
    if (space) {
      if (!collapsed.empty() && collapsed.back() != ' ') collapsed += ' ';
    } else {
      collapsed += c;
    }
  }
  while (!collapsed.empty() && collapsed.back() == ' ') collapsed.pop_back();
  return collapsed;
}

}  // namespace

std::string StoreDetailsUrl(std::uint32_t app) {
  return "https://store.steampowered.com/api/appdetails?appids=" + std::to_string(app) + "&l=english";
}

std::optional<StoreDetails> ParseStoreDetails(std::uint32_t id, std::string_view body) {
  const auto root = json::Parse(body, {2 * 1024 * 1024, 32, 1 << 17, 512 * 1024});
  if (!root || !root->is_object()) return std::nullopt;
  const auto* entry = root->Find(std::to_string(id));
  if (!entry || !entry->is_object()) return std::nullopt;
  const auto* success = entry->Find("success");
  const auto* data = entry->Find("data");
  if (!success || !success->is_bool() || !success->boolean() || !data || !data->is_object()) return std::nullopt;
  const auto* app = data->Find("steam_appid");
  if (!app || app->AsUint64() != std::optional<std::uint64_t>(id)) return std::nullopt;
  StoreDetails details;
  details.id = GameId::Steam(id);
  if (const auto* text = data->Find("short_description"); text && text->is_string())
    details.description = PlainText(text->text());
  if (const auto* genres = data->Find("genres"); genres && genres->is_array())
    for (const auto& genre : genres->array())
      if (const auto* name = genre.Find("description"); name && name->is_string() && !name->text().empty())
        details.genres.push_back(PlainText(name->text()));
  if (const auto* support = data->Find("controller_support"); support && support->is_string())
    details.controller_support = support->text();
  if (const auto* categories = data->Find("categories"); categories && categories->is_array())
    for (const auto& category : categories->array())
      if (const auto* id = category.Find("id"); id && id->AsUint64() == std::optional<std::uint64_t>(kVacCategory))
        details.vac = true;
  return details;
}

StoreDetailsService::StoreDetailsService(Fetch fetch) : fetch_(std::move(fetch)) {
  worker_ = std::thread([this] { Run(); });
}

StoreDetailsService::~StoreDetailsService() { Shutdown(); }

void StoreDetailsService::Request(GameId id) {
  if (!id.steam_app()) return;  // Steam games only
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_ || pending_.size() >= kMaxPending || !requested_.insert(id).second) return;
    pending_.push_back(id);
  }
  wake_.notify_one();
}

std::vector<StoreDetails> StoreDetailsService::Pump() {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<StoreDetails> out(std::make_move_iterator(results_.begin()), std::make_move_iterator(results_.end()));
  results_.clear();
  return out;
}

void StoreDetailsService::Shutdown() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
  }
  cancel_.store(true);
  wake_.notify_all();
  if (worker_.joinable()) worker_.join();
}

void StoreDetailsService::Run() {
  for (;;) {
    GameId id;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      wake_.wait(lock, [this] { return stopping_ || !pending_.empty(); });
      if (stopping_) return;
      id = pending_.front();
      pending_.pop_front();
    }
    const auto app = *id.steam_app();
    const auto body = fetch_ ? fetch_(StoreDetailsUrl(app), cancel_) : std::nullopt;
    auto details = body ? ParseStoreDetails(app, *body) : std::nullopt;
    if (!details) continue;
    std::lock_guard<std::mutex> lock(mutex_);
    results_.push_back(std::move(*details));
  }
}

}  // namespace playden::steam
