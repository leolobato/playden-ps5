// SPDX-License-Identifier: GPL-3.0-or-later
#include "web/pairing.h"

#include "platform/random_id.h"
#include "platform/storage.h"
#include "runtime/artifact_hash.h"
#include "runtime/private_directories.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <filesystem>

namespace playden::web {
namespace {

constexpr const char* kStoreFile = "browsers.v1";
constexpr const char* kStoreHeader = "playden-web-browsers 1";
constexpr std::size_t kTokenDigits = 64;
// The file holds a header and at most a few lines of 64 + 1 + 20 characters.
constexpr std::size_t kMaxStoreBytes = 4096;

std::string RandomHexDigits(const std::function<std::optional<std::string>()>& source, std::size_t digits) {
  std::string out;
  while (out.size() < digits) {
    const auto text = source();
    if (!text) return {};
    for (const char c : *text)
      if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) out += c;
    if (text->empty()) return {};
  }
  out.resize(digits);
  return out;
}

bool EqualConstantTime(const std::string& left, const std::string& right) {
  if (left.size() != right.size()) return false;
  unsigned char difference = 0;
  for (std::size_t i = 0; i < left.size(); ++i)
    difference = static_cast<unsigned char>(difference | (left[i] ^ right[i]));
  return difference == 0;
}

bool LowerHex(const std::string& text, std::size_t digits) {
  if (text.size() != digits) return false;
  for (const char c : text)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  return true;
}

// What a stored line says: a token's hash and its last use. Anything else: the whole file is refused.
bool ParseStore(const std::string& text, std::vector<std::pair<std::string, std::int64_t>>* out) {
  std::size_t start = 0;
  bool header = true;
  while (start < text.size()) {
    auto end = text.find('\n', start);
    if (end == std::string::npos) return false;  // every line ends with a newline: a cut file is refused
    const auto line = text.substr(start, end - start);
    start = end + 1;
    if (header) {
      if (line != kStoreHeader) return false;
      header = false;
      continue;
    }
    const auto space = line.find(' ');
    if (space == std::string::npos) return false;
    const auto hash = line.substr(0, space);
    const auto digits = line.substr(space + 1);
    if (!LowerHex(hash, kTokenDigits) || digits.empty() || digits.size() > 19 ||
        digits.find_first_not_of("0123456789") != std::string::npos)
      return false;
    errno = 0;
    const auto value = std::strtoll(digits.c_str(), nullptr, 10);
    if (errno == ERANGE) return false;
    out->emplace_back(hash, value);
  }
  return !header;
}

}  // namespace

Pairing::Pairing(Options options) : options_(std::move(options)) {
  if (!options_.random_hex) options_.random_hex = [] { return playden::RandomUuid(); };
  loaded_ = options_.store_root.empty();
}

std::int64_t Pairing::Now() const {
  if (options_.now) return options_.now();
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::int64_t Pairing::WallNow() const {
  if (options_.wall_now) return options_.wall_now();
  return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

void Pairing::Expire(std::int64_t now) {
  if (!code_.empty() && now >= code_expires_) code_.clear();
  if (locked_until_ != 0 && now >= locked_until_) {
    locked_until_ = 0;
    wrong_tries_ = 0;
  }
}

bool Pairing::Expired(const Entry& entry, std::int64_t wall) const {
  return entry.remembered && wall - entry.last_used >= options_.remember_seconds;
}

void Pairing::MarkDirty() {
  if (options_.store_root.empty()) return;
  dirty_ = true;
  ++changes_;
}

bool Pairing::StoreWanted() const { return !store_disabled_ && (!loaded_ || dirty_); }

void Pairing::SetStoreChanged(std::function<void()> changed) {
  std::lock_guard<std::mutex> lock(mutex_);
  store_changed_ = std::move(changed);
}

bool Pairing::Start(bool replace, std::int64_t* wait) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto now = Now();
  Expire(now);
  if (wait) *wait = 0;
  if (locked_until_ != 0) {
    if (wait) *wait = locked_until_ - now;
    return false;
  }
  if (!code_.empty() && !replace) return true;
  // 12 hex digits are 48 bits: the bias of taking them modulo a million is far too small to matter.
  const auto hex = RandomHexDigits(options_.random_hex, 12);
  if (hex.empty()) return false;
  std::uint64_t value = 0;
  for (const char c : hex) value = value * 16 + static_cast<std::uint64_t>(c <= '9' ? c - '0' : c - 'a' + 10);
  std::string code = std::to_string(value % 1000000);
  code.insert(0, 6 - code.size(), '0');
  code_ = std::move(code);
  code_expires_ = now + options_.code_lifetime_ms;
  return true;
}

void Pairing::Cancel() {
  std::lock_guard<std::mutex> lock(mutex_);
  code_.clear();
}

Pairing::ConfirmResult Pairing::Confirm(const std::string& code, bool remember, std::string* token) {
  std::function<void()> changed;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto now = Now();
    Expire(now);
    if (locked_until_ != 0) return ConfirmResult::kLocked;
    if (code_.empty()) return ConfirmResult::kNoCode;
    if (!EqualConstantTime(code, code_)) {
      if (++wrong_tries_ >= options_.max_wrong_tries) {
        locked_until_ = now + options_.lockout_ms;
        code_.clear();
        return ConfirmResult::kLocked;
      }
      return ConfirmResult::kWrong;
    }
    auto minted = RandomHexDigits(options_.random_hex, kTokenDigits);
    const auto hash = minted.empty() ? std::nullopt : runtime::HashArtifactBytes(minted);
    if (!hash) return ConfirmResult::kFailed;
    code_.clear();
    wrong_tries_ = 0;
    remember = remember && !options_.store_root.empty();
    const auto wall = WallNow();
    // The cap holds remembered and session browsers alike: the one used least recently makes room.
    bool forgot_remembered = false;
    while (!entries_.empty() && entries_.size() >= std::max<std::size_t>(options_.max_tokens, 1)) {
      const auto oldest = std::min_element(entries_.begin(), entries_.end(),
                                           [](const Entry& a, const Entry& b) { return a.last_used < b.last_used; });
      forgot_remembered = forgot_remembered || oldest->remembered;
      entries_.erase(oldest);
    }
    entries_.push_back({*hash, remember, wall, wall});
    if (remember || forgot_remembered) {
      MarkDirty();
      changed = store_changed_;
    }
    if (token) *token = std::move(minted);
  }
  if (changed) changed();
  return ConfirmResult::kPaired;
}

Pairing::Browser Pairing::Check(const std::string& token) {
  if (!LowerHex(token, kTokenDigits)) return Browser::kNone;
  const auto hash = runtime::HashArtifactBytes(token);
  if (!hash) return Browser::kNone;
  std::function<void()> changed;
  Browser result = Browser::kNone;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto wall = WallNow();
    Entry* found = nullptr;
    for (auto& entry : entries_)
      if (EqualConstantTime(*hash, entry.hash)) found = &entry;
    if (found && Expired(*found, wall)) {
      entries_.erase(entries_.begin() + (found - entries_.data()));
      MarkDirty();
      changed = store_changed_;
      found = nullptr;
    }
    if (found) {
      found->last_used = wall;
      result = found->remembered ? Browser::kRemembered : Browser::kSession;
      // Written again once a day, or when the clock went back, so each request doesn't write to /data.
      if (found->remembered && (wall - found->stored >= options_.touch_seconds || wall < found->stored)) {
        found->stored = wall;
        MarkDirty();
        changed = store_changed_;
      }
    }
  }
  if (changed) changed();
  return result;
}

std::string Pairing::ActiveCode() {
  std::lock_guard<std::mutex> lock(mutex_);
  Expire(Now());
  return code_;
}

std::size_t Pairing::Count() {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto wall = WallNow();
  return static_cast<std::size_t>(
      std::count_if(entries_.begin(), entries_.end(), [&](const Entry& entry) { return !Expired(entry, wall); }));
}

void Pairing::ForgetAll() {
  std::function<void()> changed;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();
    if (!loaded_) forgot_unloaded_ = true;
    MarkDirty();
    changed = store_changed_;
  }
  if (changed) changed();
}

std::string Pairing::Serialize() const {
  std::string text = std::string(kStoreHeader) + "\n";
  for (const auto& entry : entries_)
    if (entry.remembered) text += entry.hash + " " + std::to_string(entry.stored) + "\n";
  return text;
}

Pairing::StoreWork Pairing::DoStoreWork(const std::function<std::shared_ptr<void>()>& hold, std::string* log) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!StoreWanted()) return StoreWork::kDone;
  }
  const auto held = hold ? hold() : std::make_shared<int>(0);
  if (!held) return StoreWork::kBusy;
  const std::filesystem::path root(options_.store_root);
  if (!folder_ready_) {
    std::string reason;
    if (!root.is_absolute() || root.lexically_normal() != root || !root.has_parent_path() ||
        !runtime::EnsurePrivateDirectories(root.parent_path(), root, root, &reason)) {
      // Nothing can be kept or read this session: pairing goes on in memory, as for a browser not remembered.
      std::lock_guard<std::mutex> lock(mutex_);
      store_disabled_ = true;
      if (log) *log = "browsers store unavailable: " + (reason.empty() ? std::string("bad path") : reason);
      return StoreWork::kDone;
    }
    folder_ready_ = true;
  }
  Storage storage(options_.store_root);
  bool was_loaded = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    was_loaded = loaded_;
  }
  std::string notes;
  if (!was_loaded) {
    const auto read = storage.ReadDurable(kStoreFile, kMaxStoreBytes);
    std::vector<std::pair<std::string, std::int64_t>> stored;
    bool refused = false;
    if (read.ok()) {
      refused = !ParseStore(std::string(read.value().begin(), read.value().end()), &stored);
      if (refused) stored.clear();
    } else if (read.error() != StorageError::kNotFound) {
      refused = true;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    loaded_ = true;
    const auto wall = WallNow();
    std::size_t kept = 0;
    bool corrected = false;
    if (!forgot_unloaded_) {
      for (const auto& [hash, last_used] : stored) {
        // A last use in the future (the clock went back) counts as now, so the entry can't outlive its 90 days.
        Entry entry{hash, true, std::min(last_used, wall), std::min(last_used, wall)};
        // Written back, or each restart would count from now again and keep an unused browser past its 90 days.
        corrected = corrected || last_used > wall;
        if (Expired(entry, wall)) continue;
        if (std::any_of(entries_.begin(), entries_.end(), [&](const Entry& known) { return known.hash == hash; })) continue;
        entries_.push_back(entry);
        ++kept;
      }
      // Browsers paired before the file was read come first; the rest, newest first, fill the cap.
      std::stable_sort(entries_.begin(), entries_.end(),
                       [](const Entry& a, const Entry& b) { return a.last_used > b.last_used; });
      const auto cap = std::max<std::size_t>(options_.max_tokens, 1);
      for (std::size_t i = cap; i < entries_.size(); ++i) corrected = corrected || entries_[i].remembered;
      if (entries_.size() > cap) entries_.resize(cap);
    }
    forgot_unloaded_ = false;
    if (refused || corrected || kept != stored.size()) MarkDirty();
    notes = "browsers loaded remembered=" + std::to_string(kept) + (refused ? " (store refused, starting empty)" : "");
  }
  std::string text;
  std::uint64_t changes = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!dirty_) {
      if (log) *log = notes;
      return StoreWork::kDone;
    }
    text = Serialize();
    changes = changes_;
  }
  const auto written = storage.WriteDurable(kStoreFile, Bytes(text.begin(), text.end()));
  bool still_dirty = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    // A change made while writing needs another write.
    if (written.ok() && changes == changes_) dirty_ = false;
    still_dirty = !written.ok();
  }
  if (!notes.empty()) notes += "; ";
  notes += written.ok() ? "browsers saved" : "browsers not saved: " + written.message();
  if (log) *log = notes;
  // A failed write (a full disk, say) is tried again later, or Forget all could come undone at the next start.
  return still_dirty ? StoreWork::kFailed : StoreWork::kDone;
}

}  // namespace playden::web
