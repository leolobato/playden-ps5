// SPDX-License-Identifier: GPL-3.0-or-later
#include "steam/backend/collections_client.h"

#include "steam/backend/json.h"
#include "steam/backend/proto_wire.h"

#include <set>
#include <string_view>

namespace playden::steam {
namespace {

constexpr std::uint32_t kLibraryNamespace = 1;
constexpr char kMethod[] = "CloudConfigStore.Download#1";
constexpr std::string_view kKeyPrefix = "user-collections.";
constexpr std::size_t kMaximumReplyBytes = 32u << 20;
constexpr std::size_t kMaximumEntries = 100000;
constexpr std::size_t kMaximumNameBytes = 1024;
constexpr std::size_t kMaximumMembers = 200000;  // all collections together

proto::DecodeLimits ReplyLimits() {
  proto::DecodeLimits limits;
  limits.max_message_bytes = kMaximumReplyBytes;
  limits.max_length_delimited_bytes = kMaximumReplyBytes;
  limits.max_fields = kMaximumEntries + 16;
  return limits;
}

struct Entry {
  std::string key;
  std::string value;
  bool deleted = false;
};

// Collections Steam fills from a saved filter are left out (#536: not listed at all). Their value has a
// "filterSpec" object, which static ones lack (and their "added" is empty).
bool Dynamic(const json::Value& value) {
  const json::Value* filter = value.Find("filterSpec");
  return filter && !filter->is_null();
}

}  // namespace

std::vector<std::uint8_t> LibraryCollectionsRequest() {
  proto::Writer version;
  version.AddVarint(1, kLibraryNamespace);
  version.AddVarint(2, 0);
  proto::Writer request;
  request.AddMessage(1, version);
  return request.bytes();
}

std::optional<std::vector<LibraryCollection>> ParseLibraryCollections(const std::vector<std::uint8_t>& reply,
                                                                      CollectionsReadStats* stats) {
  CollectionsReadStats local;
  CollectionsReadStats& count = stats ? *stats : local;
  count = {};
  count.reply_bytes = reply.size();
  if (reply.size() > kMaximumReplyBytes) return std::nullopt;

  std::vector<Entry> entries;
  proto::Reader top(reply, ReplyLimits());
  proto::Field field;
  while (top.Next(&field)) {
    if (field.number != 1 || field.wire_type != proto::WireType::kLengthDelimited) continue;
    proto::Reader data;
    if (!top.Submessage(field, &data)) return std::nullopt;
    proto::Field part;
    std::uint64_t space = 0;
    std::vector<Entry> found;
    while (data.Next(&part)) {
      if (part.number == 1 && part.wire_type == proto::WireType::kVarint) space = part.value;
      if (part.number != 3 || part.wire_type != proto::WireType::kLengthDelimited) continue;
      proto::Reader item;
      if (!data.Submessage(part, &item)) return std::nullopt;
      Entry entry;
      proto::Field value;
      while (item.Next(&value)) {
        if (value.number == 1 && value.wire_type == proto::WireType::kLengthDelimited) entry.key = value.AsString();
        else if (value.number == 2 && value.wire_type == proto::WireType::kVarint) entry.deleted = value.value != 0;
        else if (value.number == 3 && value.wire_type == proto::WireType::kLengthDelimited) entry.value = value.AsString();
      }
      if (!item.ok()) return std::nullopt;
      found.push_back(std::move(entry));
      if (entries.size() + found.size() > kMaximumEntries) return std::nullopt;
    }
    if (!data.ok()) return std::nullopt;
    if (space != kLibraryNamespace) continue;
    for (auto& entry : found) entries.push_back(std::move(entry));
  }
  if (!top.ok()) return std::nullopt;

  json::Limits limits;
  limits.max_bytes = 4u << 20;
  limits.max_values = kMaximumMembers + 64;
  std::vector<LibraryCollection> result;
  std::set<std::string> ids;
  std::size_t members = 0;
  for (const Entry& entry : entries) {
    ++count.entries;
    if (entry.key.compare(0, kKeyPrefix.size(), kKeyPrefix) != 0) continue;
    if (entry.deleted) {
      ++count.deleted;
      continue;
    }
    const std::string id = entry.key.substr(kKeyPrefix.size());
    if (id == "favorite" || id == "hidden") {
      ++count.special;
      continue;
    }
    const auto value = json::Parse(entry.value, limits);
    const json::Value* name = value ? value->Find("name") : nullptr;
    const json::Value* value_id = value ? value->Find("id") : nullptr;
    const json::Value* added = value ? value->Find("added") : nullptr;
    if (!value || !value->is_object() || !ValidLibraryCollectionId(id) || !value_id || !value_id->is_string() ||
        value_id->text() != id || !name || !name->is_string() || name->text().empty() ||
        name->text().size() > kMaximumNameBytes || (added && !added->is_array()) || ids.count(id)) {
      ++count.invalid;
      continue;
    }
    if (Dynamic(*value)) {
      ++count.dynamic;
      continue;
    }
    LibraryCollection collection;
    collection.id = id;
    collection.name = name->text();
    std::set<std::uint32_t> removed;
    if (const json::Value* list = value->Find("removed"); list && list->is_array())
      for (const auto& app : list->array())
        if (const auto number = app.AsUint64(); number && *number && *number <= UINT32_MAX)
          removed.insert(static_cast<std::uint32_t>(*number));
    std::set<std::uint32_t> seen;
    if (added)
      for (const auto& app : added->array()) {
        const auto number = app.AsUint64();
        if (!number || !*number || *number > UINT32_MAX) continue;
        const auto app_id = static_cast<std::uint32_t>(*number);
        if (removed.count(app_id) || !seen.insert(app_id).second) continue;
        if (++members > kMaximumMembers) return std::nullopt;
        collection.members.push_back(GameId::Steam(app_id));
      }
    ids.insert(id);
    result.push_back(std::move(collection));
  }
  count.collections = result.size();
  return result;
}

std::optional<std::vector<LibraryCollection>> FetchLibraryCollections(CmClient& client, const StoredSession& session,
                                                                      const std::atomic<bool>& cancel,
                                                                      const CollectionsLog& log) {
  std::vector<std::uint8_t> reply;
  int eresult = 0;
  const auto request = LibraryCollectionsRequest();
  const auto error = client.WithServiceSession(session, 25'000, cancel, [&](const CmClient::ServiceCall& call, std::uint64_t) {
    return call(kMethod, request, &reply, &eresult);
  });
  if (error != CmError::kNone) {
    if (log) log("collections read failed error=" + std::to_string(static_cast<int>(error)) +
                 " eresult=" + std::to_string(eresult));
    return std::nullopt;
  }
  CollectionsReadStats stats;
  auto collections = ParseLibraryCollections(reply, &stats);
  if (log)
    log(std::string(collections ? "collections read" : "collections read damaged") +
        " bytes=" + std::to_string(stats.reply_bytes) + " entries=" + std::to_string(stats.entries) +
        " kept=" + std::to_string(stats.collections) + " deleted=" + std::to_string(stats.deleted) +
        " special=" + std::to_string(stats.special) + " dynamic=" + std::to_string(stats.dynamic) +
        " invalid=" + std::to_string(stats.invalid));
  return collections;
}

}  // namespace playden::steam
