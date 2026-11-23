// SPDX-License-Identifier: GPL-3.0-or-later
// Steam Workshop subscriptions (#856): PublishedFile.GetUserFiles' request and answer, the filter, paging and caps,
// and hostile titles. Answers have the shape of real ones, with synthetic IDs and titles.
#include "steam/backend/proto_wire.h"
#include "steam/backend/workshop_client.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace playden::steam;

namespace {

int failures = 0;
#define CHECK(condition)                                                              \
  do {                                                                                \
    if (!(condition)) {                                                               \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
      ++failures;                                                                     \
    }                                                                                 \
  } while (false)

constexpr std::uint32_t kApp = 1942280;

struct Detail {
  std::uint64_t id = 0;
  std::string title = "Item";
  std::uint64_t manifest = 0x1122334455667788ull;
  std::uint32_t consumer = kApp;
  std::uint32_t file_type = 0;
  std::uint32_t result = 1;
  bool has_result = true;
  bool banned = false;
  std::string url;
  bool manifest_as_varint = false;
  bool id_as_fixed64 = false;
};

proto::Writer DetailMessage(const Detail& detail) {
  proto::Writer item(1u << 20);
  if (detail.has_result) item.AddVarint(1, detail.result);
  if (detail.id_as_fixed64) item.AddFixed64(2, detail.id);
  else item.AddVarint(2, detail.id);
  item.AddFixed64(3, 76561190000000001ull);  // creator: ignored
  item.AddVarint(4, kApp);
  item.AddVarint(5, detail.consumer);
  item.AddString(7, "mod.zip");
  item.AddVarint(8, 4096 + detail.id);
  if (!detail.url.empty()) item.AddString(10, detail.url);
  if (detail.manifest_as_varint) item.AddVarint(14, detail.manifest);
  else if (detail.manifest) item.AddFixed64(14, detail.manifest);
  item.AddString(16, detail.title);
  item.AddString(17, "A long description that Playden does not keep.");
  item.AddVarint(19, 1700000000);
  item.AddVarint(20, 1700000100 + static_cast<std::uint32_t>(detail.id));
  if (detail.banned) item.AddVarint(28, 1);
  item.AddVarint(34, detail.file_type);
  proto::Writer tag;
  tag.AddString(1, "Mods");
  item.AddMessage(52, tag);
  return item;
}

std::vector<std::uint8_t> Page(std::uint32_t total, const std::vector<Detail>& details) {
  proto::Writer page(8u << 20);
  page.AddVarint(1, total);
  page.AddVarint(2, 0);
  for (const auto& detail : details) page.AddMessage(3, DetailMessage(detail));
  proto::Writer app;
  app.AddVarint(1, kApp);
  app.AddString(2, "Game");
  page.AddMessage(4, app);
  return page.bytes();
}

void TestRequest() {
  const auto bytes = WorkshopSubscriptionsRequest(0x0110000100000001ull, kApp, 3);
  const std::vector<std::uint8_t> expected = {
      0x09, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x10, 0x01,  // 1: steamid, fixed64 little-endian
      0x10, 0x88, 0xC6, 0x76,                                 // 2: appid 1942280
      0x20, 0x03,                                             // 4: page
      0x28, 0x64,                                             // 5: numperpage 100
      0x32, 0x0F, 'm', 'y', 's', 'u', 'b', 's', 'c', 'r', 'i', 'p', 't', 'i', 'o', 'n', 's',  // 6: type
      0x98, 0x01, 0x00,                                       // 19: return_vote_data false
      0xA8, 0x01, 0x00,                                       // 21: return_kv_tags false
      0xC0, 0x01, 0x00,                                       // 24: return_short_description false
  };
  CHECK(bytes == expected);
}

void TestParse() {
  Detail plain;
  plain.id = 3000000001ull;
  plain.title = "Brotato Extra Characters";
  WorkshopPageStats stats;
  const auto items = ParseWorkshopSubscriptions(Page(1, {plain}), kApp, &stats);
  CHECK(items && items->size() == 1);
  if (!items || items->empty()) return;
  const auto& item = items->front();
  CHECK(item.id == 3000000001ull);  // varint, beyond 32 bits
  CHECK(item.manifest == 0x1122334455667788ull);  // fixed64
  CHECK(item.title == "Brotato Extra Characters");
  CHECK(item.filename == "mod.zip");
  CHECK(item.file_size == 4096 + plain.id);
  CHECK(item.consumer_appid == kApp);
  CHECK(item.time_created == 1700000000);
  CHECK(item.time_updated == 1700000100 + static_cast<std::uint32_t>(plain.id));
  CHECK(item.downloadable() && !item.legacy());
  CHECK(stats.total == 1 && stats.received == 1 && stats.kept == 1);

  // Wrong wire types are not read as the field: a varint manifest leaves the item without one, a fixed64 ID
  // leaves it without an ID (dropped).
  Detail varint_manifest = plain;
  varint_manifest.manifest_as_varint = true;
  Detail fixed_id = plain;
  fixed_id.id = 3000000002ull;
  fixed_id.id_as_fixed64 = true;
  const auto wrong = ParseWorkshopSubscriptions(Page(2, {varint_manifest, fixed_id}), kApp, &stats);
  CHECK(wrong && wrong->size() == 1 && wrong->front().manifest == 0 && wrong->front().legacy());
  CHECK(stats.invalid == 1);

  // Damaged envelope: none.
  auto damaged = Page(1, {plain});
  damaged.resize(damaged.size() - 3);
  CHECK(!ParseWorkshopSubscriptions(damaged, kApp));
}

void TestFilter() {
  std::vector<Detail> details;
  Detail keep;
  keep.id = 11;
  details.push_back(keep);
  Detail banned = keep;
  banned.id = 12;
  banned.banned = true;
  details.push_back(banned);
  Detail collection = keep;
  collection.id = 13;
  collection.file_type = 2;
  details.push_back(collection);
  Detail other_app = keep;
  other_app.id = 14;
  other_app.consumer = 440;
  details.push_back(other_app);
  Detail failed = keep;
  failed.id = 15;
  failed.result = 2;
  details.push_back(failed);
  Detail gone = keep;
  gone.id = 16;
  gone.result = 9;
  details.push_back(gone);
  Detail legacy = keep;
  legacy.id = 16;
  legacy.manifest = 0;
  legacy.url = "https://ugc.example.invalid/16";
  details.push_back(legacy);
  Detail no_id = keep;
  no_id.id = 0;
  details.push_back(no_id);
  Detail no_consumer = keep;
  no_consumer.id = 17;
  no_consumer.consumer = 0;
  no_consumer.has_result = false;
  details.push_back(no_consumer);
  WorkshopPageStats stats;
  const auto items = ParseWorkshopSubscriptions(Page(8, details), kApp, &stats);
  // A banned item stays listed, flagged: a copy Playden holds must not look unsubscribed.
  CHECK(items && items->size() == 4);
  if (!items || items->size() != 4) return;
  CHECK((*items)[0].id == 11 && !(*items)[0].banned);
  CHECK((*items)[1].id == 12 && (*items)[1].banned);
  CHECK((*items)[2].id == 16 && (*items)[2].legacy() && (*items)[2].file_url == "https://ugc.example.invalid/16");
  CHECK((*items)[3].id == 17);
  CHECK(stats.banned == 1 && stats.not_item == 1 && stats.other_app == 1 && stats.failed == 1 && stats.invalid == 1 &&
        stats.gone == 1);
  CHECK(stats.kept == 3);
}

void TestHostileText() {
  // Invalid UTF-8, line breaks, commas and controls.
  const std::string raw = std::string("Bad\xC3(\xFF\xF0\x9F\x98, mod\r\nitem=1,2\t\x7F\xC2\x85") + "\xE2\x80\xA8" + "end";
  // Each byte of an invalid sequence becomes one U+FFFD.
  const std::string r = "\xEF\xBF\xBD";
  CHECK(SanitiseWorkshopText(raw, 128) == "Bad" + r + "(" + r + r + r + r + ", moditem=1,2end");
  // Overlong encodings and surrogates are invalid.
  CHECK(SanitiseWorkshopText("\xC0\xAF\xED\xA0\x80", 64) ==
        "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD");
  // The cap falls on a code point boundary.
  std::string long_title;
  for (int index = 0; index < 100; ++index) long_title += "\xC3\xA9";  // é, two bytes each
  long_title = "a" + long_title;
  const auto capped = SanitiseWorkshopText(long_title, kWorkshopTitleBytes);
  CHECK(capped.size() == 127);
  CHECK(SanitiseWorkshopText(capped, kWorkshopTitleBytes) == capped);

  Detail hostile;
  hostile.id = 21;
  hostile.title = raw + std::string(300, 'x');
  const auto items = ParseWorkshopSubscriptions(Page(1, {hostile}), kApp);
  CHECK(items && items->size() == 1);
  if (!items || items->empty()) return;
  CHECK(items->front().title.size() <= kWorkshopTitleBytes);
  CHECK(items->front().title.find_first_of("\r\n\t") == std::string::npos);
  CHECK(SanitiseWorkshopText(items->front().title, kWorkshopTitleBytes) == items->front().title);
}

// A session that answers each page from `pages`, checking the requests.
struct FakeSession {
  std::vector<std::vector<std::uint8_t>> pages;
  std::vector<std::uint32_t> asked;
  CmError fail_at_call = CmError::kNone;
  std::size_t fail_call = 0;
  CmError open = CmError::kNone;

  WorkshopSessionRunner Runner() {
    return [this](const std::function<CmError(const CmClient::ServiceCall&, std::uint64_t)>& work) {
      if (open != CmError::kNone) return open;
      const CmClient::ServiceCall call = [this](const std::string& method, const std::vector<std::uint8_t>& body,
                                                std::vector<std::uint8_t>* reply, int* eresult) {
        CHECK(method == "PublishedFile.GetUserFiles#1");
        proto::Reader reader(body);
        proto::Field field;
        std::uint32_t page = 0;
        std::uint64_t steam_id = 0;
        while (reader.Next(&field)) {
          if (field.number == 4) page = static_cast<std::uint32_t>(field.value);
          if (field.number == 1) steam_id = field.value;
        }
        CHECK(steam_id == 76561190000000042ull);
        asked.push_back(page);
        if (fail_at_call != CmError::kNone && asked.size() == fail_call) {
          if (eresult) *eresult = 15;
          return fail_at_call;
        }
        *reply = page - 1 < pages.size() ? pages[page - 1] : Page(0, {});
        return CmError::kNone;
      };
      return work(call, 76561190000000042ull);
    };
  }
};

std::vector<Detail> Run(std::uint64_t first, std::size_t count) {
  std::vector<Detail> details;
  for (std::size_t index = 0; index < count; ++index) {
    Detail detail;
    detail.id = first + index;
    details.push_back(detail);
  }
  return details;
}

void TestPaging() {
  std::atomic<bool> cancel{false};
  {
    // 250 subscriptions over three pages; the second repeats an item of the first.
    FakeSession session;
    auto second = Run(101, 100);
    second[0].id = 1;
    session.pages = {Page(250, Run(1, 100)), Page(250, second), Page(250, Run(201, 50))};
    std::vector<std::string> lines;
    const auto got = FetchWorkshopSubscriptions(session.Runner(), kApp, cancel,
                                                [&lines](const std::string& line) { lines.push_back(line); });
    CHECK(got.ok());
    CHECK(session.asked == (std::vector<std::uint32_t>{1, 2, 3}));
    CHECK(got.items.size() == 249 && got.total == 250 && got.pages == 3 && !got.truncated);
    CHECK(lines.size() == 1 && lines[0].find("workshop subscriptions read") == 0);
  }
  {
    // An empty page ends the list even when Steam's total says more: the list is cut short.
    FakeSession session;
    session.pages = {Page(500, Run(1, 100))};
    const auto got = FetchWorkshopSubscriptions(session.Runner(), kApp, cancel);
    CHECK(got.ok() && got.items.size() == 100 && session.asked.size() == 2);
    CHECK(got.truncated);
    // The first page empty while Steam counts items: cut short too. An empty list that says so is complete.
    FakeSession first;
    first.pages = {Page(3, {})};
    const auto none = FetchWorkshopSubscriptions(first.Runner(), kApp, cancel);
    CHECK(none.ok() && none.items.empty() && none.truncated);
    FakeSession empty;
    empty.pages = {Page(0, {})};
    const auto nothing = FetchWorkshopSubscriptions(empty.Runner(), kApp, cancel);
    CHECK(nothing.ok() && nothing.items.empty() && !nothing.truncated);
  }
  {
    // Filtered entries still count toward the total: one page of 100 of which half are banned and some are of
    // another app is the whole list, not cut short. Banned ones are listed, flagged.
    FakeSession session;
    auto details = Run(1, 100);
    for (std::size_t index = 0; index < details.size(); index += 2) details[index].banned = true;
    details[1].consumer = 440;
    details[3].file_type = 2;
    session.pages = {Page(100, details)};
    const auto got = FetchWorkshopSubscriptions(session.Runner(), kApp, cancel);
    CHECK(got.ok() && got.items.size() == 98 && session.asked.size() == 1 && !got.truncated);
    CHECK(std::count_if(got.items.begin(), got.items.end(), [](const WorkshopItem& item) { return item.banned; }) == 50);
  }
  {
    // An item Steam couldn't describe (a result other than 1, or a damaged detail) may be one Playden holds: the
    // list is read on, but marked cut short.
    for (const int kind : {0, 1, 2}) {
      FakeSession session;
      auto details = Run(1, 100);
      if (kind == 0) details[7].result = 2;
      else if (kind == 1) details[7].id = 0;
      else details[7].result = 9;  // deleted from the Workshop: gone, the list stays whole
      session.pages = {Page(150, details), Page(150, Run(101, 50))};
      const auto got = FetchWorkshopSubscriptions(session.Runner(), kApp, cancel);
      CHECK(got.ok() && got.items.size() == 149 && session.asked.size() == 2 && got.truncated == (kind != 2));
    }
  }
  {
    // The whole list, read to Steam's total: complete.
    FakeSession session;
    session.pages = {Page(150, Run(1, 100)), Page(150, Run(101, 50))};
    const auto got = FetchWorkshopSubscriptions(session.Runner(), kApp, cancel);
    CHECK(got.ok() && got.items.size() == 150 && session.asked.size() == 2 && !got.truncated);
  }
  {
    // A list longer than the caps stops at ten pages / 1000 items.
    FakeSession session;
    for (std::uint32_t page = 0; page < 12; ++page) session.pages.push_back(Page(5000, Run(1 + page * 100, 100)));
    const auto got = FetchWorkshopSubscriptions(session.Runner(), kApp, cancel);
    CHECK(got.ok() && got.truncated && got.items.size() == kWorkshopMaxItems);
    CHECK(session.asked.size() == kWorkshopMaxPages);
  }
  {
    // Steam refusing a page fails the read; nothing is returned.
    FakeSession session;
    session.pages = {Page(250, Run(1, 100))};
    session.fail_at_call = CmError::kRejected;
    session.fail_call = 2;
    const auto got = FetchWorkshopSubscriptions(session.Runner(), kApp, cancel);
    CHECK(got.error == CmError::kRejected && got.eresult == 15 && got.items.empty());
  }
  {
    // A damaged page fails the read as kProtocol.
    FakeSession session;
    auto bad = Page(1, Run(1, 1));
    bad.resize(bad.size() - 2);
    session.pages = {bad};
    std::string line;
    const auto got = FetchWorkshopSubscriptions(session.Runner(), kApp, cancel, [&line](const std::string& text) { line = text; });
    CHECK(got.error == CmError::kProtocol && got.items.empty() && line.find("damaged") != std::string::npos);
  }
  {
    // The session not opening is passed on; canceled before the first page is kCanceled.
    FakeSession session;
    session.open = CmError::kNetwork;
    CHECK(FetchWorkshopSubscriptions(session.Runner(), kApp, cancel).error == CmError::kNetwork);
    FakeSession idle;
    std::atomic<bool> stop{true};
    CHECK(FetchWorkshopSubscriptions(idle.Runner(), kApp, stop).error == CmError::kCanceled);
    CHECK(idle.asked.empty());
  }
}

void TestWorkshopDepot() {
  CHECK(WorkshopDepotId(kApp, "\"appinfo\" { \"depots\" { \"workshopdepot\" \"1942281\" \"1942281\" { } } }") == 1942281);
  CHECK(WorkshopDepotId(kApp, "\"appinfo\" { \"depots\" { \"1942281\" { } } }") == kApp);
  CHECK(WorkshopDepotId(kApp, "\"appinfo\" { \"depots\" { \"workshopdepot\" \"0\" } }") == kApp);
  CHECK(WorkshopDepotId(kApp, "\"appinfo\" { \"depots\" { \"workshopdepot\" \"x\" } }") == kApp);
  CHECK(WorkshopDepotId(kApp, std::string("\"appinfo\" { \"depots\" { \"WorkshopDepot\" \"77\" } }") + '\0') == 77);
  CHECK(WorkshopDepotId(kApp, "not vdf {") == kApp);
}

}  // namespace

// Left 4 Dead 2 (#1319): Steam lists an item from the old UGC system with a URL and, where a manifest would be, the
// file's UGC handle. It is legacy, not downloadable: a depot item has no URL. Also with a URL too long to keep.
void TestLegacyUgc() {
  Detail depot;
  depot.id = 21;
  Detail ugc;
  ugc.id = 22;
  ugc.manifest = 13584402027613737184ull;
  ugc.url = "https://ugc.example.invalid/13584402027613737184/333EB399/";
  Detail long_url = ugc;
  long_url.id = 23;
  long_url.url = "https://ugc.example.invalid/" + std::string(4096, 'a');
  WorkshopPageStats stats;
  const auto items = ParseWorkshopSubscriptions(Page(3, {depot, ugc, long_url}), kApp, &stats);
  CHECK(items && items->size() == 3);
  if (!items || items->size() != 3) return;
  CHECK((*items)[0].downloadable() && !(*items)[0].legacy() && !(*items)[0].has_file_url);
  CHECK((*items)[1].legacy() && !(*items)[1].downloadable() && (*items)[1].manifest == ugc.manifest);
  CHECK((*items)[2].legacy() && (*items)[2].file_url.empty() && (*items)[2].has_file_url);
  CHECK(stats.kept == 3 && stats.legacy == 2);
}

// The log line's legacy count is of the list handed to the sync: an item repeated on a later page counts once.
void TestLegacyCountedOnce() {
  Detail ugc;
  ugc.id = 31;
  ugc.url = "https://ugc.example.invalid/31/";
  std::vector<std::vector<std::uint8_t>> pages = {Page(2, {ugc}), Page(2, {ugc})};
  std::size_t page = 0;
  const WorkshopSessionRunner run = [&](const auto& work) {
    return work([&](const std::string&, const std::vector<std::uint8_t>&, std::vector<std::uint8_t>* reply, int* eresult) {
      *eresult = 1;
      *reply = pages[std::min(page++, pages.size() - 1)];
      return CmError::kNone;
    }, 76561190000000001ull);
  };
  std::atomic<bool> cancel{false};
  std::string line;
  const auto got = FetchWorkshopSubscriptions(run, kApp, cancel, [&line](const std::string& text) { line = text; });
  CHECK(got.ok() && got.items.size() == 1);
  CHECK(line.find(" legacy=1 ") != std::string::npos);
}

int main() {
  TestRequest();
  TestParse();
  TestFilter();
  TestHostileText();
  TestPaging();
  TestWorkshopDepot();
  TestLegacyUgc();
  TestLegacyCountedOnce();
  if (failures) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("workshop_client: ok\n");
  return 0;
}
