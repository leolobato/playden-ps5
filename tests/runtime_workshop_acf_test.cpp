// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime/workshop_acf.h"

#include "steam/backend/vdf.h"

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
namespace fs = std::filesystem;

namespace {

// As Valve's client wrote it for Brotato in Online, with nothing installed (the account ID replaced).
const std::string kValveFile =
    "\"AppWorkshop\"\n"
    "{\n"
    "\t\"appid\"\t\t\"1942280\"\n"
    "\t\"SizeOnDisk\"\t\t\"0\"\n"
    "\t\"NeedsUpdate\"\t\t\"0\"\n"
    "\t\"NeedsDownload\"\t\t\"1\"\n"
    "\t\"TimeLastUpdated\"\t\t\"1795079261\"\n"
    "\t\"TimeLastFullCheck\"\t\t\"1795083822\"\n"
    "\t\"TimeLastAppRan\"\t\t\"0\"\n"
    "\t\"LastBuildID\"\t\t\"0\"\n"
    "\t\"WorkshopItemsInstalled\"\n"
    "\t{\n"
    "\t}\n"
    "\t\"WorkshopItemDetails\"\n"
    "\t{\n"
    "\t\t\"3479890464\"\n"
    "\t\t{\n"
    "\t\t\t\"manifest\"\t\t\"4263948807607885841\"\n"
    "\t\t\t\"timeupdated\"\t\t\"1746979102\"\n"
    "\t\t\t\"timetouched\"\t\t\"0\"\n"
    "\t\t\t\"subscribedby\"\t\t\"12345\"\n"
    "\t\t\t\"latest_timeupdated\"\t\t\"1746979102\"\n"
    "\t\t\t\"latest_manifest\"\t\t\"4263948807607885841\"\n"
    "\t\t}\n"
    "\t\t\"3000000002\"\n"
    "\t\t{\n"
    "\t\t\t\"manifest\"\t\t\"77\"\n"
    "\t\t\t\"timeupdated\"\t\t\"1700000000\"\n"
    "\t\t\t\"timetouched\"\t\t\"0\"\n"
    "\t\t\t\"subscribedby\"\t\t\"12345\"\n"
    "\t\t\t\"latest_timeupdated\"\t\t\"1700000000\"\n"
    "\t\t\t\"latest_manifest\"\t\t\"77\"\n"
    "\t\t}\n"
    "\t}\n"
    "}\n";

std::string Read(const fs::path& path) {
  std::ifstream file(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void Write(const fs::path& path, const std::string& bytes) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << bytes;
}

std::string Replace(std::string text, const std::string& from, const std::string& to) {
  const auto at = text.find(from);
  CHECK(at != std::string::npos);
  return text.replace(at, from.size(), to);
}

void SerializerRoundTrips() {
  // Valve's own file reads and writes back byte for byte.
  const auto parsed = playden::steam::ParseVdf(kValveFile);
  CHECK(parsed && SerializeVdf(*parsed) == kValveFile);
  // Quotes, backslashes, line ends and tabs come back as they were; a backslash before any other letter too.
  playden::steam::VdfNode root;
  root.is_dict = true;
  playden::steam::VdfNode leaf;
  leaf.value = "C:\\Games\\\"quoted\"\\name\n\tend\\";
  root.children.emplace_back("key \"with\" \\quotes", leaf);
  playden::steam::VdfNode empty;
  empty.is_dict = true;
  root.children.emplace_back("empty", empty);
  const auto text = SerializeVdf(root);
  CHECK(text == "\"key \\\"with\\\" \\\\quotes\"\t\t\"C:\\\\Games\\\\\\\"quoted\\\"\\\\name\\n\\tend\\\\\"\n\"empty\"\n{\n}\n");
  const auto again = playden::steam::ParseVdf(text);
  CHECK(again && again->children.size() == 2 && again->children[0].first == root.children[0].first &&
        again->children[0].second.value == leaf.value && again->children[1].second.is_dict &&
        again->children[1].second.children.empty());
}

void FreshFile() {
  // No file yet: one in the client's format with the items, sizes summed.
  const auto text = MergeAppWorkshopAcf(std::nullopt, 1942280, {{3479890464ull, 2048, 1746979102, 4263948807607885841ull},
                                                                {3000000002ull, 10, 1700000000, 77}});
  CHECK(text && *text ==
                    "\"AppWorkshop\"\n"
                    "{\n"
                    "\t\"appid\"\t\t\"1942280\"\n"
                    "\t\"SizeOnDisk\"\t\t\"2058\"\n"
                    "\t\"NeedsUpdate\"\t\t\"0\"\n"
                    "\t\"NeedsDownload\"\t\t\"0\"\n"
                    "\t\"TimeLastUpdated\"\t\t\"0\"\n"
                    "\t\"TimeLastAppRan\"\t\t\"0\"\n"
                    "\t\"LastBuildID\"\t\t\"0\"\n"
                    "\t\"WorkshopItemsInstalled\"\n"
                    "\t{\n"
                    "\t\t\"3479890464\"\n"
                    "\t\t{\n"
                    "\t\t\t\"size\"\t\t\"2048\"\n"
                    "\t\t\t\"timeupdated\"\t\t\"1746979102\"\n"
                    "\t\t\t\"manifest\"\t\t\"4263948807607885841\"\n"
                    "\t\t}\n"
                    "\t\t\"3000000002\"\n"
                    "\t\t{\n"
                    "\t\t\t\"size\"\t\t\"10\"\n"
                    "\t\t\t\"timeupdated\"\t\t\"1700000000\"\n"
                    "\t\t\t\"manifest\"\t\t\"77\"\n"
                    "\t\t}\n"
                    "\t}\n"
                    "\t\"WorkshopItemDetails\"\n"
                    "\t{\n"
                    "\t}\n"
                    "}\n");
  // No items, no file; nor for items without an ID.
  CHECK(!MergeAppWorkshopAcf(std::nullopt, 1942280, {}));
  CHECK(!MergeAppWorkshopAcf(std::nullopt, 1942280, {{0, 5, 1, 1}}));
  // A repeated item is listed once.
  const auto once = MergeAppWorkshopAcf(std::nullopt, 7, {{5, 1, 1, 1}, {5, 2, 2, 2}});
  CHECK(once && once->find("\"SizeOnDisk\"\t\t\"1\"") != std::string::npos &&
        once->find("\"5\"") == once->rfind("\"5\""));
}

void MergesIntoValvesFile() {
  // Only the installed list, NeedsDownload and SizeOnDisk change; the details, the times and the order stay.
  const auto merged = MergeAppWorkshopAcf(std::string_view(kValveFile), 1942280,
                                          {{3479890464ull, 2048, 1746979102, 4263948807607885841ull}});
  auto expected = Replace(kValveFile, "\t\"WorkshopItemsInstalled\"\n\t{\n\t}\n",
                          "\t\"WorkshopItemsInstalled\"\n\t{\n"
                          "\t\t\"3479890464\"\n\t\t{\n"
                          "\t\t\t\"size\"\t\t\"2048\"\n"
                          "\t\t\t\"timeupdated\"\t\t\"1746979102\"\n"
                          "\t\t\t\"manifest\"\t\t\"4263948807607885841\"\n"
                          "\t\t}\n\t}\n");
  expected = Replace(expected, "\"NeedsDownload\"\t\t\"1\"", "\"NeedsDownload\"\t\t\"0\"");
  expected = Replace(expected, "\"SizeOnDisk\"\t\t\"0\"", "\"SizeOnDisk\"\t\t\"2048\"");
  CHECK(merged && *merged == expected);

  // The client installed the other item since, of another manifest than Playden's: its entry stays as it is, and the
  // size counts every installed item.
  auto with_valve = Replace(expected, "\t\t}\n\t}\n\t\"WorkshopItemDetails\"",
                            "\t\t}\n\t\t\"3000000002\"\n\t\t{\n"
                            "\t\t\t\"size\"\t\t\"500\"\n\t\t\t\"timeupdated\"\t\t\"1700000001\"\n"
                            "\t\t\t\"manifest\"\t\t\"99\"\n\t\t}\n\t}\n\t\"WorkshopItemDetails\"");
  with_valve = Replace(with_valve, "\"NeedsDownload\"\t\t\"0\"", "\"NeedsDownload\"\t\t\"1\"");
  // Every item listed: nothing changes, not even NeedsDownload.
  const auto same = MergeAppWorkshopAcf(std::string_view(with_valve), 1942280,
                                        {{3479890464ull, 2048, 1746979102, 4263948807607885841ull}, {3000000002ull, 10, 1700000000, 77}});
  CHECK(same && *same == with_valve);
  // No items: unchanged.
  CHECK(MergeAppWorkshopAcf(std::string_view(with_valve), 1942280, {}) == with_valve);
  // A new one is added after them; the client's entry for 3000000002 keeps its manifest and size.
  const auto added = MergeAppWorkshopAcf(std::string_view(with_valve), 1942280,
                                         {{3000000002ull, 10, 1700000000, 77}, {42, 8, 3, 4}});
  CHECK(added && added->find("\t\t\t\"manifest\"\t\t\"99\"\n") != std::string::npos &&
        added->find("\t\t\"42\"\n\t\t{\n\t\t\t\"size\"\t\t\"8\"\n") != std::string::npos);
  CHECK(added->find("\"SizeOnDisk\"\t\t\"2556\"") != std::string::npos);
  CHECK(added->find("\"NeedsDownload\"\t\t\"0\"") != std::string::npos);
  CHECK(added->find("\"TimeLastFullCheck\"\t\t\"1795083822\"") != std::string::npos);

  // Keys the file lacks are added; one Playden doesn't know stays where it was.
  const std::string sparse = "\"AppWorkshop\"\n{\n\t\"appid\"\t\t\"7\"\n\t\"Future\"\t\t\"x\"\n}\n";
  const auto filled = MergeAppWorkshopAcf(std::string_view(sparse), 7, {{1, 3, 4, 5}});
  CHECK(filled && *filled == "\"AppWorkshop\"\n{\n\t\"appid\"\t\t\"7\"\n\t\"Future\"\t\t\"x\"\n"
                             "\t\"WorkshopItemsInstalled\"\n\t{\n\t\t\"1\"\n\t\t{\n\t\t\t\"size\"\t\t\"3\"\n"
                             "\t\t\t\"timeupdated\"\t\t\"4\"\n\t\t\t\"manifest\"\t\t\"5\"\n\t\t}\n\t}\n"
                             "\t\"NeedsDownload\"\t\t\"0\"\n\t\"SizeOnDisk\"\t\t\"3\"\n}\n");
  // Keys in another case are found as the client finds them.
  const std::string lower = "\"appworkshop\"\n{\n\t\"APPID\"\t\t\"7\"\n\t\"needsdownload\"\t\t\"1\"\n}\n";
  const auto cased = MergeAppWorkshopAcf(std::string_view(lower), 7, {{1, 3, 4, 5}});
  CHECK(cased && cased->find("\"needsdownload\"\t\t\"0\"") != std::string::npos &&
        cased->find("NeedsDownload") == std::string::npos);
}

void LeavesOtherFilesAlone() {
  const std::vector<WorkshopAcfItem> items{{1, 3, 4, 5}};
  CHECK(!MergeAppWorkshopAcf(std::string_view(kValveFile), 1942281, items));  // another game's
  CHECK(!MergeAppWorkshopAcf(std::string_view("\"AppWorkshop\"\n{\n"), 7, items));  // cut short
  CHECK(!MergeAppWorkshopAcf(std::string_view("\"AppWorkshop\"\t\t\"7\"\n"), 7, items));  // not a dict
  CHECK(!MergeAppWorkshopAcf(std::string_view("\"Other\"\n{\n\t\"appid\"\t\t\"7\"\n}\n"), 7, items));
  CHECK(!MergeAppWorkshopAcf(std::string_view("\"AppWorkshop\"\n{\n\t\"appid\"\t\t\"7\"\n}\n\"More\"\t\t\"1\"\n"), 7, items));
  CHECK(!MergeAppWorkshopAcf(std::string_view("\"AppWorkshop\"\n{\n}\n"), 7, items));  // no app ID
  CHECK(!MergeAppWorkshopAcf(std::string_view("\"AppWorkshop\"\n{\n\t\"appid\"\t\t\"7\"\n\t\"WorkshopItemsInstalled\"\t\t\"\"\n}\n"),
                             7, items));
  CHECK(!MergeAppWorkshopAcf(std::string_view("\"AppWorkshop\"\n{\n\t\"appid\"\t\t\"7\"\n\t\"NeedsDownload\"\n\t{\n\t}\n}\n"),
                             7, items));
  CHECK(!MergeAppWorkshopAcf(std::string_view(""), 7, items));
  // A quoted brace is a value, and stays one.
  const auto brace = MergeAppWorkshopAcf(std::string_view("\"AppWorkshop\"\n{\n\t\"appid\"\t\t\"7\"\n\t\"odd\"\t\t\"}\"\n}\n"), 7, items);
  CHECK(brace && brace->find("\t\"odd\"\t\t\"}\"\n") != std::string::npos);
  const auto open = MergeAppWorkshopAcf(std::string_view("\"AppWorkshop\"\n{\n\t\"appid\"\t\t\"7\"\n\t\"Future\"\t\t\"{\"\n}\n"), 7, items);
  CHECK(open && open->find("\t\"Future\"\t\t\"{\"\n") != std::string::npos);
  // A bare brace where a value belongs is not Valve's file.
  CHECK(!MergeAppWorkshopAcf(std::string_view("\"AppWorkshop\" { \"appid\" \"7\" \"Future\" } }"), 7, items));
  // Caps: the file's size, and the number of items.
  std::string big = "\"AppWorkshop\"\n{\n\t\"appid\"\t\t\"7\"\n\t\"pad\"\t\t\"" + std::string(kWorkshopAcfMaxBytes, 'x') + "\"\n}\n";
  CHECK(!MergeAppWorkshopAcf(std::string_view(big), 7, items));
  // A file just under the cap that the merge would take past it: left alone, so the next read still takes it.
  std::string near = "\"AppWorkshop\"\n{\n\t\"appid\"\t\t\"7\"\n\t\"pad\"\t\t\"\"\n}\n";
  near.insert(near.find("\"\"\n}") + 1, std::string(kWorkshopAcfMaxBytes - near.size() - 40, 'x'));
  CHECK(near.size() == kWorkshopAcfMaxBytes - 40);
  CHECK(playden::steam::ParseVdf(near));
  CHECK(!MergeAppWorkshopAcf(std::string_view(near), 7, items));
  // With room for one item: the first merge fits and reads again; a second that would pass the cap is refused.
  near.erase(near.find("xxx"), 400);
  const auto first = MergeAppWorkshopAcf(std::string_view(near), 7, items);
  CHECK(first && first->size() <= kWorkshopAcfMaxBytes && playden::steam::ParseVdf(*first));
  CHECK(MergeAppWorkshopAcf(std::string_view(*first), 7, items) == *first);
  std::vector<WorkshopAcfItem> more;
  for (std::uint64_t id = 2; id < 12; ++id) more.push_back({id, 1, 1, 1});
  CHECK(!MergeAppWorkshopAcf(std::string_view(*first), 7, more));
  more.resize(2);
  CHECK(MergeAppWorkshopAcf(std::string_view(*first), 7, more));
  std::vector<WorkshopAcfItem> many;
  for (std::uint64_t id = 1; id <= kWorkshopAcfMaxItems + 1; ++id) many.push_back({id, 1, 1, 1});
  CHECK(!MergeAppWorkshopAcf(std::nullopt, 7, many));
  many.pop_back();
  CHECK(MergeAppWorkshopAcf(std::nullopt, 7, many));
}

void DropsAndReadsInstalled() {
  const std::vector<WorkshopAcfItem> both{{3479890464ull, 2048, 1746979102, 4263948807607885841ull}, {42, 8, 3, 4}};
  const auto merged = *MergeAppWorkshopAcf(std::string_view(kValveFile), 1942280, both);
  // What the client lists installed, with its manifest: an entry without one is not counted.
  const auto listed = ParseAppWorkshopInstalled(
      Replace(merged, "\t\t\"42\"\n\t\t{\n\t\t\t\"size\"\t\t\"8\"\n\t\t\t\"timeupdated\"\t\t\"3\"\n\t\t\t\"manifest\"\t\t\"4\"\n",
              "\t\t\"42\"\n\t\t{\n\t\t\t\"size\"\t\t\"8\"\n\t\t\t\"timeupdated\"\t\t\"3\"\n"),
      1942280);
  CHECK(listed.size() == 1 && listed[0].id == 3479890464ull && listed[0].bytes == 2048 &&
        listed[0].time_updated == 1746979102 && listed[0].manifest == 4263948807607885841ull);
  CHECK(ParseAppWorkshopInstalled(merged, 1942280).size() == 2);
  CHECK(ParseAppWorkshopInstalled(merged, 1942281).empty() && ParseAppWorkshopInstalled("not vdf {", 1942280).empty());
  // Dropping one: its entry goes, the size follows, the details stay.
  const auto dropped = DropFromAppWorkshopAcf(merged, 1942280, {42});
  CHECK(dropped && *dropped == *MergeAppWorkshopAcf(std::string_view(kValveFile), 1942280, {both[0]}));
  CHECK(dropped->find("\"3000000002\"") != std::string::npos);  // in WorkshopItemDetails
  // Dropping the last: an empty list and no size; NeedsDownload stays as it was.
  const auto none = DropFromAppWorkshopAcf(*dropped, 1942280, {3479890464ull, 99});
  CHECK(none && none->find("\t\"WorkshopItemsInstalled\"\n\t{\n\t}\n") != std::string::npos &&
        none->find("\"SizeOnDisk\"\t\t\"0\"") != std::string::npos &&
        none->find("\"NeedsDownload\"\t\t\"0\"") != std::string::npos &&
        none->find("\"latest_manifest\"\t\t\"4263948807607885841\"") != std::string::npos);
  // Nothing listed: unchanged; another game's or a damaged file: none.
  CHECK(DropFromAppWorkshopAcf(kValveFile, 1942280, {42}) == kValveFile);
  CHECK(!DropFromAppWorkshopAcf(kValveFile, 1942281, {42}));
  CHECK(!DropFromAppWorkshopAcf("not vdf {", 1942280, {42}));
}

void AppliesOnDisk() {
  char pattern[] = "/tmp/playden-workshop-acf-XXXXXX";
  const char* made = ::mkdtemp(pattern);
  CHECK(made != nullptr);
  const fs::path root = fs::canonical(made);
  const auto workshop = root / "bottle/drive_c/Program Files (x86)/Steam/steamapps/workshop";
  const auto acf = workshop / "appworkshop_1942280.acf";
  const std::vector<WorkshopAcfItem> items{{3479890464ull, 2048, 1746979102, 4263948807607885841ull}};
  // No Workshop folder: nothing is made.
  CHECK(ApplyAppWorkshopAcf(workshop.string(), 1942280, items) == WorkshopAcfOutcome::kFailed);
  CHECK(!fs::exists(workshop));
  fs::create_directories(workshop);
  CHECK(ApplyAppWorkshopAcf(workshop.string(), 1942280, {}) == WorkshopAcfOutcome::kNoItems && !fs::exists(acf));
  // No file: a new one, 0644.
  CHECK(ApplyAppWorkshopAcf(workshop.string(), 1942280, items) == WorkshopAcfOutcome::kWritten);
  CHECK(Read(acf) == *MergeAppWorkshopAcf(std::nullopt, 1942280, items));
  struct stat info {};
  CHECK(::lstat(acf.c_str(), &info) == 0 && (info.st_mode & 07777) == 0644);
  CHECK(!fs::exists(workshop / "appworkshop_1942280.acf.playden-tmp"));
  // Again: nothing to write.
  const auto written_at = fs::last_write_time(acf);
  CHECK(ApplyAppWorkshopAcf(workshop.string(), 1942280, items) == WorkshopAcfOutcome::kUnchanged);
  CHECK(fs::last_write_time(acf) == written_at);
  // The client's file gets the item.
  Write(acf, kValveFile);
  CHECK(ApplyAppWorkshopAcf(workshop.string(), 1942280, items) == WorkshopAcfOutcome::kWritten);
  CHECK(Read(acf) == *MergeAppWorkshopAcf(std::string_view(kValveFile), 1942280, items));
  CHECK(Read(acf).find("TimeLastFullCheck") != std::string::npos);
  // A file Playden doesn't understand, one too big, a link, a folder: left as they are.
  Write(acf, "not vdf {");
  CHECK(ApplyAppWorkshopAcf(workshop.string(), 1942280, items) == WorkshopAcfOutcome::kLeftAlone);
  CHECK(Read(acf) == "not vdf {");
  Write(acf, std::string(kWorkshopAcfMaxBytes + 1, ' '));
  CHECK(ApplyAppWorkshopAcf(workshop.string(), 1942280, items) == WorkshopAcfOutcome::kLeftAlone);
  CHECK(fs::file_size(acf) == kWorkshopAcfMaxBytes + 1);
  fs::remove(acf);
  Write(root / "elsewhere.acf", kValveFile);
  fs::create_symlink(root / "elsewhere.acf", acf);
  CHECK(ApplyAppWorkshopAcf(workshop.string(), 1942280, items) == WorkshopAcfOutcome::kLeftAlone);
  CHECK(fs::is_symlink(acf) && Read(root / "elsewhere.acf") == kValveFile);
  fs::remove(acf);
  fs::create_directory(acf);
  CHECK(ApplyAppWorkshopAcf(workshop.string(), 1942280, items) == WorkshopAcfOutcome::kLeftAlone);
  CHECK(fs::is_directory(acf));
  fs::remove(acf);
  // Removing items: no file is nothing to do; the listed ones go, written as the merge writes.
  CHECK(RemoveFromAppWorkshopAcf(workshop.string(), 1942280, {3479890464ull}) == WorkshopAcfOutcome::kNoItems);
  CHECK(!fs::exists(acf));
  Write(acf, *MergeAppWorkshopAcf(std::string_view(kValveFile), 1942280, items));
  CHECK(ReadAppWorkshopInstalled(workshop.string(), 1942280).size() == 1);
  CHECK(RemoveFromAppWorkshopAcf(workshop.string(), 1942280, {}) == WorkshopAcfOutcome::kNoItems);
  CHECK(RemoveFromAppWorkshopAcf(workshop.string(), 1942280, {42}) == WorkshopAcfOutcome::kUnchanged);
  CHECK(RemoveFromAppWorkshopAcf(workshop.string(), 1942280, {3479890464ull}) == WorkshopAcfOutcome::kWritten);
  CHECK(ReadAppWorkshopInstalled(workshop.string(), 1942280).empty());
  CHECK(::lstat(acf.c_str(), &info) == 0 && (info.st_mode & 07777) == 0644);
  CHECK(Read(acf).find("TimeLastFullCheck") != std::string::npos);
  Write(acf, "not vdf {");
  CHECK(RemoveFromAppWorkshopAcf(workshop.string(), 1942280, {3479890464ull}) == WorkshopAcfOutcome::kLeftAlone);
  CHECK(Read(acf) == "not vdf {" && ReadAppWorkshopInstalled(workshop.string(), 1942280).empty());
  fs::remove(acf);
  // A link on the way to W: nothing is written through it.
  const auto real = root / "real-workshop";
  fs::create_directories(real);
  const auto linked = root / "linked";
  fs::create_directory_symlink(real, linked);
  CHECK(ApplyAppWorkshopAcf(linked.string(), 1942280, items) == WorkshopAcfOutcome::kFailed);
  CHECK(!fs::exists(real / "appworkshop_1942280.acf"));
  fs::remove_all(root);
}

}  // namespace

int main() {
  SerializerRoundTrips();
  FreshFile();
  MergesIntoValvesFile();
  LeavesOtherFilesAlone();
  DropsAndReadsInstalled();
  ::umask(077);  // a strict umask still gives the file Valve's client reads 0644
  AppliesOnDisk();
  return 0;
}
