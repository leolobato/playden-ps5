// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime/workshop_mods.h"

#include "homebrew/json.hpp"
#include "runtime/artifact_hash.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#define CHECK(cond)                                                                 \
  do {                                                                              \
    if (!(cond)) {                                                                  \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      std::exit(1);                                                                 \
    }                                                                               \
  } while (0)

using namespace playden::runtime;
using playden::platform::DirectoryPolicy;
using playden::platform::ManagedDirectory;
namespace fs = std::filesystem;
namespace json = playden::homebrew::json;

namespace {

std::string Read(const fs::path& path) {
  std::ifstream file(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void Write(const fs::path& path, const std::string& bytes) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << bytes;
}

const std::string kReplacement = "\xef\xbf\xbd";

json::Value Parse(const std::string& text) {
  auto value = json::Parser(text).parse();
  CHECK(value && value->object());
  return *value;
}

const json::Value::Object& Entry(const json::Value& root, const std::string& key) {
  const auto found = root.object()->find(key);
  CHECK(found != root.object()->end() && found->second.object());
  return *found->second.object();
}

std::string Text(const json::Value::Object& entry, const std::string& key) {
  const auto found = entry.find(key);
  CHECK(found != entry.end() && found->second.string());
  return *found->second.string();
}

// The key order of the top-level object as written (json::Value keeps a sorted map).
std::vector<std::string> Keys(const std::string& text) {
  std::vector<std::string> keys;
  std::size_t at = 0;
  while ((at = text.find("\n  \"", at)) != std::string::npos) {
    at += 4;
    keys.push_back(text.substr(at, text.find('"', at) - at));
  }
  return keys;
}

WorkshopMod Mod(std::uint64_t id, std::string title, std::uint64_t bytes = 100, std::uint32_t time = 1700000000) {
  WorkshopMod mod;
  mod.id = id;
  mod.title = std::move(title);
  mod.bytes = bytes;
  mod.time_updated = time;
  return mod;
}

void TestJsonString() {
  CHECK(WorkshopJsonString("plain") == "\"plain\"");
  CHECK(WorkshopJsonString("a\"b\\c") == "\"a\\\"b\\\\c\"");
  CHECK(WorkshopJsonString("l1\nl2\r\t") == "\"l1\\nl2\\r\\t\"");
  CHECK(WorkshopJsonString(std::string("\x01\x1f\x7f", 3)) == "\"\\u0001\\u001f\x7f\"");
  CHECK(WorkshopJsonString(std::string("a\0b", 3)) == "\"a\\u0000b\"");
  // Valid UTF-8 passes as it is: two, three and four bytes.
  CHECK(WorkshopJsonString("caf\xc3\xa9 \xe6\x97\xa5 \xf0\x9f\x98\x80") == "\"caf\xc3\xa9 \xe6\x97\xa5 \xf0\x9f\x98\x80\"");
  CHECK(WorkshopJsonString("\xef\xbf\xbf\xf4\x8f\xbf\xbf") == "\"\xef\xbf\xbf\xf4\x8f\xbf\xbf\"");
  // Invalid UTF-8 becomes U+FFFD, one per byte that doesn't start a whole sequence.
  CHECK(WorkshopJsonString("\xc0\x80") == "\"" + kReplacement + kReplacement + "\"");               // overlong
  CHECK(WorkshopJsonString("\xe0\x80\xaf") == "\"" + kReplacement + kReplacement + kReplacement + "\"");  // overlong
  CHECK(WorkshopJsonString("\xed\xa0\x80") == "\"" + kReplacement + kReplacement + kReplacement + "\"");  // surrogate
  CHECK(WorkshopJsonString("\xf4\x90\x80\x80") ==
        "\"" + kReplacement + kReplacement + kReplacement + kReplacement + "\"");  // past U+10FFFF
  CHECK(WorkshopJsonString("\x80x") == "\"" + kReplacement + "x\"");  // lone continuation
  CHECK(WorkshopJsonString("a\xe6\x97") == "\"a" + kReplacement + kReplacement + "\"");  // cut short
  CHECK(WorkshopJsonString("\xf5\xff") == "\"" + kReplacement + kReplacement + "\"");
  // A quote after a cut-short sequence is still escaped.
  CHECK(WorkshopJsonString("\xc3\"") == "\"" + kReplacement + "\\\"\"");
}

void TestBuild() {
  CHECK(BuildWorkshopModsJson(1942280, {}, {}) == "{}\n");
  // Out of order, a hostile title, a size past 32 bits.
  std::vector<WorkshopMod> items{Mod(1000000000000ull, "Big \"one\"\\ \n", 5000000000ull, 42),
                                 Mod(30, std::string("bad \xff utf8", 10)), Mod(4, "")};
  const auto text = BuildWorkshopModsJson(1942280, items, {});
  CHECK((Keys(text) == std::vector<std::string>{"4", "30", "1000000000000"}));  // by number, not as text
  const auto root = Parse(text);
  CHECK(root.object()->size() == 3);
  const auto& big = Entry(root, "1000000000000");
  CHECK(Text(big, "title") == "Big \"one\"\\ \n");
  CHECK(Text(big, "path") ==
        "C:\\Program Files (x86)\\Steam\\steamapps\\workshop\\content\\1942280\\1000000000000");
  CHECK(text.find("\"path\": \"C:\\\\Program Files (x86)\\\\Steam\\\\steamapps\\\\workshop\\\\content\\\\1942280\\\\"
                  "1000000000000\"") != std::string::npos);
  CHECK(big.at("primary_filesize").number() == 2147483647.0);
  CHECK(text.find("\"primary_filesize\": 2147483647,") != std::string::npos);
  CHECK(text.find("\"total_files_sizes\": 5000000000,") != std::string::npos);
  CHECK(big.at("time_updated").number() == 42.0);
  CHECK(Text(big, "workshop_item_url") == "https://steamcommunity.com/sharedfiles/filedetails/?id=1000000000000");
  CHECK(big.find("primary_filename") == big.end());
  CHECK(Text(Entry(root, "30"), "title") == "bad " + kReplacement + " utf8");
  CHECK(Entry(root, "4").find("title") == Entry(root, "4").end());  // gbe_fork shows the ID
  CHECK(Entry(root, "4").at("primary_filesize").number() == 100.0);
  // No raw control character anywhere but the layout's line breaks.
  for (const char c : text) CHECK(static_cast<unsigned char>(c) >= 0x20 || c == '\n');
  // Same input, same bytes.
  CHECK(BuildWorkshopModsJson(1942280, items, {}) == text);

  // A primary file: its own size (clamped), the total stays. A name with a folder in it is left out.
  auto with_file = Mod(5, "pak", 9000000000ull);
  with_file.primary_filename = "mod.pak";
  with_file.primary_bytes = 3000000000ull;
  auto with_path = Mod(6, "path", 10);
  with_path.primary_filename = "..\\evil.pak";
  with_path.primary_bytes = 7;
  const auto files = Parse(BuildWorkshopModsJson(7, {with_file, with_path}, {}));
  CHECK(Text(Entry(files, "5"), "primary_filename") == "mod.pak");
  CHECK(Entry(files, "5").at("primary_filesize").number() == 2147483647.0);
  CHECK(Entry(files, "5").at("total_files_sizes").number() == 9000000000.0);
  CHECK(Entry(files, "6").find("primary_filename") == Entry(files, "6").end());
  CHECK(Entry(files, "6").at("primary_filesize").number() == 10.0);

  // A preview picture in the item's folder: named by its full path inside the bottle, as a file:/// URL too. One with
  // a folder in its name is left out.
  auto with_preview = Mod(8, "vpk", 100);
  with_preview.primary_filename = "special.vpk";
  with_preview.primary_bytes = 100;
  with_preview.preview_filename = "preview.jpg";
  with_preview.preview_bytes = 5082057;
  auto bad_preview = Mod(9, "bad", 10);
  bad_preview.preview_filename = "..\\x.jpg";
  const auto previews = Parse(BuildWorkshopModsJson(550, {with_preview, bad_preview}, {}));
  CHECK(Text(Entry(previews, "8"), "preview_filename") ==
        "C:\\Program Files (x86)\\Steam\\steamapps\\workshop\\content\\550\\8\\preview.jpg");
  CHECK(Entry(previews, "8").at("preview_filesize").number() == 5082057.0);
  CHECK(Text(Entry(previews, "8"), "preview_url") ==
        "file:///C:/Program Files (x86)/Steam/steamapps/workshop/content/550/8/preview.jpg");
  CHECK(Entry(previews, "9").find("preview_filename") == Entry(previews, "9").end());

  // The author, when known: gbe_fork names them (or "Unknown User") instead of the player. Big SteamIDs stay exact.
  auto with_owner = Mod(10, "owned", 10);
  with_owner.owner = 76561199222093932ull;
  const auto owned_text = BuildWorkshopModsJson(550, {with_owner, Mod(11, "no owner", 10)}, {});
  CHECK(owned_text.find("\"steam_id_owner\": 76561199222093932,") != std::string::npos);
  const auto owned = Parse(owned_text);
  CHECK(Entry(owned, "11").find("steam_id_owner") == Entry(owned, "11").end());

  // Hand-placed folders: an entry with no path (gbe_fork reads steam_settings/mods/<key>), kept by their own name. One
  // with an item's ID wins over it; a second folder for the same number is left out.
  const auto merged_text = BuildWorkshopModsJson(7, {Mod(30, "item"), Mod(40, "other")}, {"0007", "7", "30"});
  const auto merged = Parse(merged_text);
  CHECK((Keys(merged_text) == std::vector<std::string>{"0007", "30", "40"}));
  CHECK(Entry(merged, "0007").empty() && Entry(merged, "30").empty());
  CHECK(Text(Entry(merged, "40"), "title") == "other");
}

void TestPresent(const fs::path& root) {
  const auto workshop = root / "workshop";
  const auto app = workshop / "content/7";
  fs::create_directories(app / "1");
  Write(app / "2/mod.pak", "12345");
  fs::create_directories(root / "elsewhere/4");
  fs::create_symlink(root / "elsewhere/4", app / "4");
  Write(app / "5", "a file, not a folder");
  auto two = Mod(2, "two");
  two.primary_filename = "mod.pak";
  auto one = Mod(1, "one");
  one.primary_filename = "missing.pak";
  auto six = Mod(6, "six");
  fs::create_directories(app / "6/sub");
  Write(root / "outside.pak", "x");
  fs::create_symlink(root / "outside.pak", app / "6/link.pak");
  six.primary_filename = "link.pak";
  two.preview_filename = "preview.jpg";  // not there: dropped
  Write(app / "1/preview.jpg", "jpeg");
  one.preview_filename = "preview.jpg";
  const auto present = PresentWorkshopMods(workshop.string(), 7, {one, two, Mod(3, "gone"), Mod(4, "link"), Mod(5, "file"), six});
  CHECK(present.size() == 3);
  CHECK(present[0].id == 1 && present[0].primary_filename.empty());
  CHECK(present[0].preview_filename == "preview.jpg" && present[0].preview_bytes == 4);
  CHECK(present[1].id == 2 && present[1].primary_filename == "mod.pak" && present[1].primary_bytes == 5);
  CHECK(present[1].preview_filename.empty() && present[1].preview_bytes == 0);
  CHECK(present[2].id == 6 && present[2].primary_filename.empty());
  CHECK(PresentWorkshopMods(workshop.string(), 8, {Mod(1, "other app")}).empty());
}

std::vector<std::string> HandPlaced(const fs::path& settings) {
  const auto folder = ManagedDirectory::Open(settings, DirectoryPolicy::kApprovedTree);
  CHECK(folder);
  return HandPlacedMods(*folder);
}

void TestHandPlaced(const fs::path& root) {
  const auto settings = root / "hand/steam_settings";
  fs::create_directories(settings);
  CHECK(HandPlaced(settings).empty());
  fs::create_directories(settings / "mods/123");
  fs::create_directories(settings / "mods/0042");
  fs::create_directories(settings / "mods/abc");
  fs::create_directories(settings / "mods/12abc");
  fs::create_directories(settings / "mods/99999999999999999999");  // past 64 bits
  Write(settings / "mods/55", "not a folder");
  fs::create_directories(root / "hand/target");
  fs::create_symlink(root / "hand/target", settings / "mods/66");
  CHECK((HandPlaced(settings) == std::vector<std::string>{"0042", "123"}));
  // A mods folder that is a link is not looked into.
  const auto linked = root / "hand2/steam_settings";
  fs::create_directories(linked);
  fs::create_symlink(settings / "mods", linked / "mods");
  CHECK(HandPlaced(linked).empty());
}

// From the game's folder, as the runtime host opens a steam_settings/ beside an emulated DLL.
WorkshopModsResult Apply(const fs::path& root, std::uint32_t app, const std::vector<WorkshopMod>& items) {
  return ApplyWorkshopMods((root / "game").string(), DirectoryPolicy::kApprovedTree, "Bin/steam_settings", app, items);
}
WorkshopModsResult Remove(const fs::path& root) {
  return RemoveWorkshopMods((root / "game").string(), DirectoryPolicy::kApprovedTree, "Bin/steam_settings");
}

void TestApply(const fs::path& root) {
  const auto settings = root / "game/Bin/steam_settings";
  const auto file = settings / kWorkshopModsFile;
  const auto marker = settings / kWorkshopModsMarker;
  const auto hash = [](const std::string& text) { return *HashArtifactBytes(text); };
  // No steam_settings folder: nothing written; one that is a link: refused.
  CHECK(Apply(root, 7, {Mod(1, "one")}).outcome == WorkshopModsOutcome::kFailed);
  CHECK(Remove(root).outcome == WorkshopModsOutcome::kNone);
  fs::create_directories(root / "real_settings");
  fs::create_directories(settings.parent_path());
  fs::create_symlink(root / "real_settings", settings);
  CHECK(Apply(root, 7, {Mod(1, "one")}).outcome == WorkshopModsOutcome::kFailed);
  CHECK(Remove(root).outcome == WorkshopModsOutcome::kFailed);
  CHECK(fs::is_empty(root / "real_settings"));
  fs::remove(settings);
  fs::create_directories(settings);

  // Nothing to hand over and nothing there.
  CHECK(Apply(root, 7, {}).outcome == WorkshopModsOutcome::kNone);
  CHECK(!fs::exists(file) && !fs::exists(marker));
  // Written, with its hash in the marker; the same items again keep it.
  auto result = Apply(root, 7, {Mod(1, "one")});
  CHECK(result.outcome == WorkshopModsOutcome::kWritten && result.hand_placed == 0);
  const auto first = Read(file);
  CHECK(first == BuildWorkshopModsJson(7, {Mod(1, "one")}, {}));
  CHECK(Read(marker) == hash(first) + "\n");
  struct stat info {};
  CHECK(::lstat(file.c_str(), &info) == 0 && (info.st_mode & 07777) == 0644);
  CHECK(Apply(root, 7, {Mod(1, "one")}).outcome == WorkshopModsOutcome::kKept);
  // New items replace it.
  CHECK(Apply(root, 7, {Mod(1, "one"), Mod(2, "two")}).outcome == WorkshopModsOutcome::kWritten);
  CHECK(Read(marker) == hash(Read(file)) + "\n" && Read(file) != first);
  // A folder placed by hand since is listed too, so Playden's file doesn't hide it.
  fs::create_directories(settings / "mods/77");
  result = Apply(root, 7, {Mod(1, "one"), Mod(2, "two")});
  CHECK(result.outcome == WorkshopModsOutcome::kWritten && result.hand_placed == 1);
  CHECK(Entry(Parse(Read(file)), "77").empty());
  // A stop between the marker and the file: the marker names both, and the old file is still Playden's.
  const auto old_text = Read(file);
  Write(marker, hash(old_text) + "\n" + hash("something newer") + "\n");
  CHECK(Apply(root, 7, {Mod(2, "two")}).outcome == WorkshopModsOutcome::kWritten);
  CHECK(Read(marker) == hash(Read(file)) + "\n");
  // No items: Playden's file and the marker go; the hand-placed folder stays for gbe_fork to find.
  CHECK(Remove(root).outcome == WorkshopModsOutcome::kRemoved);
  CHECK(!fs::exists(file) && !fs::exists(marker) && fs::is_directory(settings / "mods/77"));
  CHECK(Remove(root).outcome == WorkshopModsOutcome::kNone);
  // A temporary file never stays behind.
  for (const auto& entry : fs::directory_iterator(settings))
    CHECK(entry.path().filename().string().find("tmp") == std::string::npos);

  // A mods.json made by hand is never written over or removed.
  const std::string hand = "{ \"12\": { \"title\": \"mine\" } }";
  Write(file, hand);
  CHECK(Apply(root, 7, {Mod(1, "one")}).outcome == WorkshopModsOutcome::kHandMade);
  CHECK(Remove(root).outcome == WorkshopModsOutcome::kHandMade);
  CHECK(Read(file) == hand && !fs::exists(marker));
  // Nor is Playden's once someone edited it.
  fs::remove(file);
  CHECK(Apply(root, 7, {Mod(1, "one")}).outcome == WorkshopModsOutcome::kWritten);
  const auto edited = Read(file) + " ";
  Write(file, edited);
  CHECK(Apply(root, 7, {Mod(3, "three")}).outcome == WorkshopModsOutcome::kHandMade);
  CHECK(Remove(root).outcome == WorkshopModsOutcome::kHandMade);
  CHECK(Read(file) == edited);
  fs::remove(file);
  fs::remove(marker);

  // A mods.json that is a link is not Playden's: neither it nor what it points to changes.
  const auto outside = root / "outside.json";
  Write(outside, "outside");
  fs::create_symlink(outside, file);
  CHECK(Apply(root, 7, {Mod(1, "one")}).outcome == WorkshopModsOutcome::kHandMade);
  CHECK(Remove(root).outcome == WorkshopModsOutcome::kHandMade);
  CHECK(fs::is_symlink(file) && Read(outside) == "outside");
  fs::remove(file);
  // A marker that is a link is neither read nor written through or replaced: nothing is written.
  const auto outside_marker = root / "outside-marker";
  Write(outside_marker, "keep");
  fs::create_symlink(outside_marker, marker);
  CHECK(Apply(root, 7, {Mod(1, "one")}).outcome == WorkshopModsOutcome::kFailed);
  CHECK(fs::is_symlink(marker) && !fs::exists(file) && Read(outside_marker) == "keep");
  // ... and a mods.json whose marker became a link no longer counts as Playden's.
  fs::remove(marker);
  CHECK(Apply(root, 7, {Mod(1, "one")}).outcome == WorkshopModsOutcome::kWritten);
  fs::remove(marker);
  fs::create_symlink(outside_marker, marker);
  CHECK(Remove(root).outcome == WorkshopModsOutcome::kHandMade && fs::exists(file));
}

void TestLinkedAncestor(const fs::path& root) {
  // A folder on the way to steam_settings/ that is a link (the game's Bin/, or the bottle's playden/) would carry
  // the writes elsewhere: refused, and nothing changes where it points.
  const auto elsewhere = root / "elsewhere/Bin/steam_settings";
  fs::create_directories(elsewhere);
  fs::create_directories(root / "linked");
  fs::create_symlink(root / "elsewhere/Bin", root / "linked/Bin");
  const auto apply = [&](const std::vector<WorkshopMod>& items) {
    return ApplyWorkshopMods((root / "linked").string(), DirectoryPolicy::kUnownedTree, "Bin/steam_settings", 7, items);
  };
  CHECK(apply({Mod(1, "one")}).outcome == WorkshopModsOutcome::kFailed);
  CHECK(fs::is_empty(elsewhere));
  // Playden's own file where the link points stays too.
  CHECK(ApplyWorkshopMods((root / "elsewhere").string(), DirectoryPolicy::kUnownedTree, "Bin/steam_settings", 7,
                          {Mod(1, "one")}).outcome == WorkshopModsOutcome::kWritten);
  CHECK(RemoveWorkshopMods((root / "linked").string(), DirectoryPolicy::kUnownedTree, "Bin/steam_settings").outcome ==
        WorkshopModsOutcome::kFailed);
  CHECK(fs::exists(elsewhere / kWorkshopModsFile) && fs::exists(elsewhere / kWorkshopModsMarker));
  // A folder missing on the way: nothing to remove.
  CHECK(RemoveWorkshopMods((root / "linked").string(), DirectoryPolicy::kUnownedTree, "Gone/steam_settings").outcome ==
        WorkshopModsOutcome::kNone);
  CHECK(apply({}).outcome == WorkshopModsOutcome::kFailed);
}

}  // namespace

int main() {
  char name[] = "/tmp/playden-workshop-mods-XXXXXX";
  CHECK(mkdtemp(name));
  const fs::path root = fs::canonical(name);
  TestJsonString();
  TestBuild();
  TestPresent(root);
  TestHandPlaced(root);
  ::umask(077);  // a strict umask still gives files the game's process can read
  TestApply(root);
  TestLinkedAncestor(root);
  fs::remove_all(root);
  std::puts("runtime_workshop_mods: ok");
  return 0;
}
