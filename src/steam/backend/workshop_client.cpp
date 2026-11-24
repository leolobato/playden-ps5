// SPDX-License-Identifier: GPL-3.0-or-later
#include "steam/backend/workshop_client.h"

#include "steam/backend/proto_wire.h"
#include "steam/backend/vdf.h"

#include <algorithm>
#include <set>

namespace playden::steam {
namespace {

constexpr char kMethod[] = "PublishedFile.GetUserFiles#1";
constexpr std::size_t kMaximumReplyBytes = 8u << 20;
constexpr std::uint32_t kDeadlineMs = 60'000;  // up to kWorkshopMaxPages calls on one session
constexpr std::size_t kMaximumUrlBytes = 2048;

proto::DecodeLimits ReplyLimits() {
  proto::DecodeLimits limits;
  limits.max_message_bytes = kMaximumReplyBytes;
  limits.max_length_delimited_bytes = kMaximumReplyBytes;
  // Per reader: the page's details and apps, or one detail's fields with its repeated tags and previews.
  limits.max_fields = 4096;
  return limits;
}

// One code point from `text` at `*at`; false (and `*at` past one byte) for an invalid sequence.
bool NextCodePoint(std::string_view text, std::size_t* at, std::uint32_t* code) {
  const auto byte = [&](std::size_t index) { return static_cast<unsigned char>(text[index]); };
  const unsigned char lead = byte(*at);
  std::size_t length = 0;
  std::uint32_t value = 0, minimum = 0;
  if (lead < 0x80) { *code = lead; ++*at; return true; }
  if (lead >= 0xC2 && lead <= 0xDF) { length = 2; value = lead & 0x1F; minimum = 0x80; }
  else if (lead >= 0xE0 && lead <= 0xEF) { length = 3; value = lead & 0x0F; minimum = 0x800; }
  else if (lead >= 0xF0 && lead <= 0xF4) { length = 4; value = lead & 0x07; minimum = 0x10000; }
  else { ++*at; return false; }
  if (*at + length > text.size()) { ++*at; return false; }
  for (std::size_t index = 1; index < length; ++index) {
    const unsigned char next = byte(*at + index);
    if ((next & 0xC0) != 0x80) { ++*at; return false; }
    value = (value << 6) | (next & 0x3F);
  }
  if (value < minimum || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF)) { ++*at; return false; }
  *at += length;
  *code = value;
  return true;
}

void AppendUtf8(std::string* out, std::uint32_t code) {
  if (code < 0x80) {
    out->push_back(static_cast<char>(code));
  } else if (code < 0x800) {
    out->push_back(static_cast<char>(0xC0 | (code >> 6)));
    out->push_back(static_cast<char>(0x80 | (code & 0x3F)));
  } else if (code < 0x10000) {
    out->push_back(static_cast<char>(0xE0 | (code >> 12)));
    out->push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (code & 0x3F)));
  } else {
    out->push_back(static_cast<char>(0xF0 | (code >> 18)));
    out->push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (code & 0x3F)));
  }
}

bool Control(std::uint32_t code) {
  return code < 0x20 || code == 0x7F || (code >= 0x80 && code <= 0x9F) || code == 0x2028 || code == 0x2029;
}

std::string Sanitised(const proto::Field& field, std::size_t max_bytes) {
  // Bounds the work on a huge field; dropped control characters still let later text in.
  const std::size_t read = std::min<std::size_t>(field.size, 64u << 10);
  return SanitiseWorkshopText(std::string_view(reinterpret_cast<const char*>(field.data), read), max_bytes);
}

std::string ErrorText(CmError error, int eresult) {
  return std::string("error=") + CmErrorName(error) + " eresult=" + std::to_string(eresult);
}

}  // namespace

std::string SanitiseWorkshopText(std::string_view text, std::size_t max_bytes) {
  std::string out;
  std::size_t at = 0;
  while (at < text.size()) {
    std::uint32_t code = 0;
    if (!NextCodePoint(text, &at, &code)) code = 0xFFFD;
    if (Control(code)) continue;
    std::string piece;
    AppendUtf8(&piece, code);
    if (out.size() + piece.size() > max_bytes) break;
    out += piece;
  }
  return out;
}

std::vector<std::uint8_t> WorkshopSubscriptionsRequest(std::uint64_t steam_id, std::uint32_t app_id, std::uint32_t page) {
  proto::Writer request;
  request.AddFixed64(1, steam_id);
  request.AddVarint(2, app_id);
  request.AddVarint(4, page);
  request.AddVarint(5, kWorkshopPageSize);
  request.AddString(6, "mysubscriptions");
  // Leave out what Playden doesn't read: vote data, key-value tags and short descriptions default on.
  request.AddVarint(19, 0);
  request.AddVarint(21, 0);
  request.AddVarint(24, 0);
  return request.bytes();
}

std::optional<std::vector<WorkshopItem>> ParseWorkshopSubscriptions(const std::vector<std::uint8_t>& reply,
                                                                    std::uint32_t app_id, WorkshopPageStats* stats) {
  WorkshopPageStats local;
  WorkshopPageStats& count = stats ? *stats : local;
  count = {};
  if (reply.size() > kMaximumReplyBytes) return std::nullopt;
  std::vector<WorkshopItem> items;
  proto::Reader top(reply, ReplyLimits());
  proto::Field field;
  while (top.Next(&field)) {
    if (field.number == 1 && field.wire_type == proto::WireType::kVarint) {
      count.total = field.value > UINT32_MAX ? UINT32_MAX : static_cast<std::uint32_t>(field.value);
      continue;
    }
    if (field.number != 3 || field.wire_type != proto::WireType::kLengthDelimited) continue;
    ++count.received;
    proto::Reader details;
    if (!top.Submessage(field, &details)) return std::nullopt;
    WorkshopItem item;
    std::uint64_t result = 1;
    proto::Field part;
    while (details.Next(&part)) {
      const bool varint = part.wire_type == proto::WireType::kVarint;
      const bool bytes = part.wire_type == proto::WireType::kLengthDelimited;
      const auto u32 = [&part] { return part.value > UINT32_MAX ? UINT32_MAX : static_cast<std::uint32_t>(part.value); };
      switch (part.number) {
        case 1: if (varint) result = part.value; break;
        case 2: if (varint) item.id = part.value; break;
        case 3: if (part.wire_type == proto::WireType::kFixed64) item.creator = part.value; break;
        case 5: if (varint) item.consumer_appid = u32(); break;
        case 7: if (bytes) item.filename = Sanitised(part, kWorkshopFilenameBytes); break;
        case 8: if (varint) item.file_size = part.value; break;
        case 10:
          if (bytes && part.size > 0) {
            item.has_file_url = true;
            if (part.size <= kMaximumUrlBytes) item.file_url = Sanitised(part, kMaximumUrlBytes);
          }
          break;
        case 9: if (varint) item.preview_size = part.value; break;
        case 11: if (bytes && part.size <= kMaximumUrlBytes) item.preview_url = Sanitised(part, kMaximumUrlBytes); break;
        case 14: if (part.wire_type == proto::WireType::kFixed64) item.manifest = part.value; break;
        case 16: if (bytes) item.title = Sanitised(part, kWorkshopTitleBytes); break;
        case 19: if (varint) item.time_created = u32(); break;
        case 20: if (varint) item.time_updated = u32(); break;
        case 28: if (varint) item.banned = part.value != 0; break;
        case 34: if (varint) item.file_type = u32(); break;
        default: break;
      }
    }
    if (!details.ok()) {
      ++count.invalid;
      continue;
    }
    if (item.id == 0) { ++count.invalid; continue; }
    // A deleted item stays in the subscription list with File Not Found: it is gone, like an unsubscribed one, and
    // doesn't make the list incomplete (it would otherwise stay incomplete for good).
    if (result == 9) { ++count.gone; continue; }
    if (result != 1) { ++count.failed; continue; }
    if (item.consumer_appid != 0 && item.consumer_appid != app_id) { ++count.other_app; continue; }
    if (item.file_type != 0) { ++count.not_item; continue; }
    // Listed, flagged: a copy Playden holds stays (PlanWorkshopSync), nothing downloads.
    if (item.banned) ++count.banned;
    else ++count.kept;
    if (item.legacy()) ++count.legacy;  // per page; FetchWorkshopSubscriptions counts the list itself
    items.push_back(std::move(item));
  }
  if (!top.ok()) return std::nullopt;
  return items;
}

WorkshopSubscriptions FetchWorkshopSubscriptions(const WorkshopSessionRunner& run, std::uint32_t app_id,
                                                 const std::atomic<bool>& cancel, const WorkshopLog& log) {
  WorkshopSubscriptions out;
  if (app_id == 0 || !run) return out;
  WorkshopPageStats sum;
  bool damaged = false;
  const auto error = run([&](const CmClient::ServiceCall& call, std::uint64_t steam_id) {
    std::set<std::uint64_t> seen;
    std::size_t received = 0;
    std::uint32_t expected = 0;  // Steam's total: the largest any page said
    bool undescribed = false;
    for (std::uint32_t page = 1;; ++page) {
      if (cancel.load()) return CmError::kCanceled;
      if (page > kWorkshopMaxPages) { out.truncated = true; break; }
      std::vector<std::uint8_t> reply;
      const auto called = call(kMethod, WorkshopSubscriptionsRequest(steam_id, app_id, page), &reply, &out.eresult);
      if (called != CmError::kNone) return called;
      WorkshopPageStats stats;
      auto items = ParseWorkshopSubscriptions(reply, app_id, &stats);
      if (!items) { damaged = true; return CmError::kProtocol; }
      ++out.pages;
      out.total = stats.total;
      received += stats.received;
      sum.received += stats.received; sum.kept += stats.kept; sum.failed += stats.failed; sum.banned += stats.banned; sum.gone += stats.gone;
      sum.other_app += stats.other_app; sum.not_item += stats.not_item; sum.invalid += stats.invalid;
      for (auto& item : *items) {
        if (!seen.insert(item.id).second) continue;
        if (out.items.size() >= kWorkshopMaxItems) { out.truncated = true; break; }
        if (item.legacy()) ++sum.legacy;
        out.items.push_back(std::move(item));
      }
      // An item Steam couldn't describe may be one Playden holds: the list doesn't name everything.
      if (stats.failed || stats.invalid) undescribed = true;
      if (out.truncated) break;
      expected = std::max(expected, stats.total);
      // A page that comes back empty before Steam's total (the largest any page said) ends the list there, cut short.
      if (stats.received == 0) {
        if (received < expected) out.truncated = true;
        break;
      }
      if (received >= stats.total) break;
    }
    if (undescribed) out.truncated = true;
    return CmError::kNone;
  });
  out.error = error;
  if (error != CmError::kNone) out.items.clear();
  if (log)
    log(std::string(error == CmError::kNone ? "workshop subscriptions read" :
                    damaged ? "workshop subscriptions damaged" : "workshop subscriptions failed") +
        " " + ErrorText(error, out.eresult) + " pages=" + std::to_string(out.pages) + " total=" + std::to_string(out.total) +
        " received=" + std::to_string(sum.received) + " kept=" + std::to_string(out.items.size()) +
        " failed=" + std::to_string(sum.failed) + " banned=" + std::to_string(sum.banned) + " gone=" + std::to_string(sum.gone) +
        " legacy=" + std::to_string(sum.legacy) +
        " other_app=" + std::to_string(sum.other_app) + " not_item=" + std::to_string(sum.not_item) +
        " invalid=" + std::to_string(sum.invalid) + (out.truncated ? " truncated" : ""));
  return out;
}

WorkshopSubscriptions FetchWorkshopSubscriptions(CmClient& client, const StoredSession& session, std::uint32_t app_id,
                                                 const std::atomic<bool>& cancel, const WorkshopLog& log) {
  const WorkshopSessionRunner run = [&](const auto& work) {
    return client.WithServiceSession(session, kDeadlineMs, cancel, work);
  };
  return FetchWorkshopSubscriptions(run, app_id, cancel, log);
}

std::uint32_t WorkshopDepotId(std::uint32_t app_id, std::string_view appinfo) {
  while (!appinfo.empty() && appinfo.back() == '\0') appinfo.remove_suffix(1);
  const auto doc = ParseVdf(appinfo);
  const VdfNode* root = doc ? doc->Find("appinfo") : nullptr;
  const VdfNode* depots = root ? root->Find("depots") : nullptr;
  const auto depot = depots ? depots->Uint64("workshopdepot") : std::nullopt;
  if (depot && *depot != 0 && *depot <= UINT32_MAX) return static_cast<std::uint32_t>(*depot);
  return app_id;
}

}  // namespace playden::steam
