// SPDX-License-Identifier: GPL-3.0-or-later
// GOG manifest paths made safe to write: separators, traversal and other refusals, Playden's own names, the merge of
// every depot (case folding, the last depot wins, folder spelling from the first path, file/folder clashes, links)
// and where support files go. Pure: no files.
#include "gog/path_rules.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>

#define CHECK(condition)                                                                  \
  do {                                                                                    \
    if (!(condition)) {                                                                   \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
      std::exit(1);                                                                       \
    }                                                                                     \
  } while (false)

using namespace playden::gog;

namespace {

PathRefusal Refusal(std::string_view raw) {
  PathRefusal why = PathRefusal::kNone;
  CHECK(!NormalizePath(raw, &why));
  CHECK(why != PathRefusal::kNone);
  return why;
}

std::string Normal(std::string_view raw) {
  PathRefusal why = PathRefusal::kClash;
  const auto path = NormalizePath(raw, &why);
  CHECK(path.has_value());
  CHECK(why == PathRefusal::kNone);
  return *path;
}

void TestSeparators() {
  CHECK(Normal("save\\a.lst") == "save/a.lst");
  CHECK(Normal("save/a.lst") == "save/a.lst");
  CHECK(Normal("/save/a.lst") == "save/a.lst");
  CHECK(Normal("\\save\\a.lst") == "save/a.lst");
  CHECK(Normal("SPEECH\\1302_10A.VOC") == "SPEECH/1302_10A.VOC");
  CHECK(Normal("Arena Codex Scientia.pdf") == "Arena Codex Scientia.pdf");
  CHECK(Normal("a/.b/c..d/e.f") == "a/.b/c..d/e.f");  // dots inside names are not traversal
}

void TestRefusals() {
  CHECK(Refusal("") == PathRefusal::kEmpty);
  CHECK(Refusal("/") == PathRefusal::kEmpty);
  CHECK(Refusal("\\") == PathRefusal::kEmpty);
  CHECK(Refusal("..") == PathRefusal::kDotComponent);
  CHECK(Refusal("a/../b") == PathRefusal::kDotComponent);
  CHECK(Refusal("a\\..\\..\\b") == PathRefusal::kDotComponent);
  CHECK(Refusal("./a") == PathRefusal::kDotComponent);
  CHECK(Refusal("a/.") == PathRefusal::kDotComponent);
  CHECK(Refusal("a//b") == PathRefusal::kEmptyComponent);
  CHECK(Refusal("a/") == PathRefusal::kEmptyComponent);
  CHECK(Refusal("//x") == PathRefusal::kAbsolute);
  CHECK(Refusal("\\\\server\\share") == PathRefusal::kAbsolute);
  CHECK(Refusal("C:\\x") == PathRefusal::kDrive);
  CHECK(Refusal("c:x") == PathRefusal::kDrive);
  CHECK(Refusal("a/file:stream") == PathRefusal::kDrive);
  CHECK(Refusal("a/what?.txt") == PathRefusal::kDrive);
  CHECK(Refusal("a/...") == PathRefusal::kDrive);
  CHECK(Refusal("name./b") == PathRefusal::kDrive);
  CHECK(Refusal("a/name ") == PathRefusal::kDrive);
  CHECK(Refusal(".playden-install.json.") == PathRefusal::kDrive);
  CHECK(Refusal(".playden-files /x") == PathRefusal::kDrive);
  CHECK(Normal(" a/b c") == " a/b c");
  CHECK(Refusal(std::string("a\0b", 3)) == PathRefusal::kControl);
  CHECK(Refusal("a\nb") == PathRefusal::kControl);
  CHECK(Refusal("a\tb") == PathRefusal::kControl);
  CHECK(Refusal("a\x7f") == PathRefusal::kControl);
  CHECK(Refusal("a\x1f/b") == PathRefusal::kControl);

  // 1024 bytes in all and 255 in a component are allowed; one more is not.
  std::string path;
  while (path.size() < 1024 - 11) path += std::string(10, 'p') + "/";
  path.resize(1023);
  path += "x";
  CHECK(path.size() == 1024 && path.back() != '/');
  CHECK(Normal(path) == path);
  CHECK(Refusal(path + "y") == PathRefusal::kTooLong);
  CHECK(Normal("/" + path) == path);  // the dropped separator doesn't count
  CHECK(Normal(std::string(255, 'n')).size() == 255);
  CHECK(Refusal(std::string(256, 'n')) == PathRefusal::kComponentTooLong);
  CHECK(Refusal("a/" + std::string(256, 'n') + "/b") == PathRefusal::kComponentTooLong);
}

void TestReserved() {
  for (const char* name : {".playden-install.json", ".playden-files", ".playden-files.tmp", ".playden-download",
                           ".playden-gog", ".playden-copy", ".playden-update", ".playden-tree",
                           ".playden-files-changed"}) {
    CHECK(Refusal(name) == PathRefusal::kReserved);
    CHECK(Refusal(std::string(name) + "/x") == PathRefusal::kReserved);
    CHECK(Refusal(std::string("\\") + name) == PathRefusal::kReserved);
    CHECK(IsReservedName(name));
  }
  CHECK(Refusal(".PLAYDEN-install.json") == PathRefusal::kReserved);
  CHECK(Refusal(".playden-gog/x") == PathRefusal::kReserved);
  CHECK(Refusal(".Playden-Download\\journal") == PathRefusal::kReserved);
  // Deeper down, or a different name, is the game's own business.
  CHECK(Normal("data/.playden-files") == "data/.playden-files");
  CHECK(Normal("data/.playden-install.json") == "data/.playden-install.json");
  // Any ".playden…" name at the top is Playden's, today's names or not, and a non-ASCII letter that exFAT's upcase
  // table may take for an ASCII one doesn't get past it (U+0131 dotless i, U+017F long s).
  for (const char* name : {".playden-files2", ".playden", ".PLAYDENx", ".Playden-NEW.DAT.tmp",
                           ".playden-f\xC4\xB1les", ".playden-\xC4\xB1nstall.json", ".playden-update\xC5\xBF"}) {
    CHECK(Refusal(name) == PathRefusal::kReserved);
    CHECK(IsReservedName(name));
  }
  CHECK(Normal(".playde") == ".playde");
  CHECK(Normal("playden-files") == "playden-files");
  CHECK(Normal("x.playden-files") == "x.playden-files");
}

void TestFoldCase() {
  // exFAT compares names through its upcase table, which also pairs non-ASCII letters: each pair gets one key.
  const std::pair<const char*, const char*> same[] = {
      {"Jazz2.EXE", "jazz2.exe"},
      {"Donn\xC3\xA9" "es", "DONN\xC3\x89" "ES"},              // é / É
      {"\xC3\xBF", "\xC5\xB8"},                                 // ÿ / Ÿ
      {"\xC4\x81", "\xC4\x80"},                                 // ā / Ā
      {"\xC5\x82\xC3\xB3" "d\xC5\xBA", "\xC5\x81\xC3\x93" "D\xC5\xB9"},  // łódź / ŁÓDŹ
      {"f\xC4\xB1le", "FILE"},                                   // dotless i: I
      {"\xC5\xBF" "ave", "SAVE"},                                // long s: S
      {"\xCE\xB1\xCF\x82", "\xCE\x91\xCE\xA3"},              // ας / ΑΣ (final sigma)
      {"\xD0\xB4\xD0\xB0\xD0\xBD\xD1\x96", "\xD0\x94\xD0\x90\xD0\x9D\xD0\x86"},  // дані / ДАНІ
      {"\xC2\xB5", "\xCE\xBC"},                                 // micro sign / mu
  };
  for (const auto& [a, b] : same) CHECK(FoldCase(a) == FoldCase(b));
  CHECK(FoldCase("DONN\xC3\x89" "ES") == "donn\xC3\xA9" "es");
  CHECK(FoldCase("f\xC4\xB1le") == "file");
  // Not pairs: ß, İ (exFAT keeps it apart from I), the multiplication sign, a three-byte letter, stray bytes.
  CHECK(FoldCase("\xC3\x9F") == "\xC3\x9F");
  CHECK(FoldCase("\xC4\xB0") != FoldCase("I"));
  CHECK(FoldCase("\xC3\x97") == "\xC3\x97" && FoldCase("\xC3\xB7") == "\xC3\xB7");
  CHECK(FoldCase("\xE2\x84\xAA") == "\xE2\x84\xAA");
  CHECK(FoldCase("A\xC3") == "a\xC3" && FoldCase("\xC3" "A") == "\xC3" "a" && FoldCase("\xFF\xC0\x80") == "\xFF\xC0\x80");
  CHECK(FoldCase("\xC3\xA9") != FoldCase("e"));
}

void TestPlacement() {
  PathRefusal why = PathRefusal::kNone;
  CHECK(PlaceFile(Normal("app\\dosbox_arena.conf"), 1435828982, true, &why) == std::optional<std::string>("dosbox_arena.conf"));
  CHECK(PlaceFile(Normal("APP/x.conf"), 1435828982, true) == std::optional<std::string>("x.conf"));
  CHECK(PlaceFile(Normal("App/sub/y.conf"), 1435828982, true) == std::optional<std::string>("sub/y.conf"));
  CHECK(PlaceFile(Normal("save\\NAMES.DAT"), 1435828982, true) ==
        std::optional<std::string>(".playden-gog/support/1435828982/save/NAMES.DAT"));
  CHECK(PlaceFile(Normal("app"), 7, true) == std::optional<std::string>(".playden-gog/support/7/app"));
  CHECK(PlaceFile(Normal("application/z"), 7, true) == std::optional<std::string>(".playden-gog/support/7/application/z"));
  // Without the support flag a file stays where it is, `app/` or not.
  CHECK(PlaceFile(Normal("app/x"), 1435828982, false) == std::optional<std::string>("app/x"));
  CHECK(PlaceFile(Normal("save/NAMES.DAT"), 1435828982, false) == std::optional<std::string>("save/NAMES.DAT"));
  // A support file under app/ may not land on Playden's own names once the prefix is gone.
  why = PathRefusal::kNone;
  CHECK(!PlaceFile(Normal("app/.playden-install.json"), 1, true, &why));
  CHECK(why == PathRefusal::kReserved);
  CHECK(!PlaceFile(Normal("APP\\.Playden-Files"), 1, true, &why));
  CHECK(why == PathRefusal::kReserved);
}

void TestPathSet() {
  {
    // Case folding: one file, and the later depot wins; the earlier entry's index comes back to be dropped.
    PathSet set;
    const auto first = set.Add("Jazz2.exe", PathKind::kFile, 1351891846, false, 0);
    CHECK(first.ok() && first.path == "Jazz2.exe" && !first.replaced);
    const auto second = set.Add("JAZZ2.EXE", PathKind::kFile, 1351891846, false, 1);
    CHECK(second.ok() && second.path == "JAZZ2.EXE" && second.replaced == std::optional<std::size_t>(0));
    CHECK(set.entries() == 1);
  }
  {
    // Non-ASCII case too: two depots' "Données" and "DONNÉES" are one folder on exFAT, so one entry, in the first
    // spelling; a file under each lands in it.
    PathSet set;
    const auto lower = set.Add("Donn\xC3\xA9" "es/a.txt", PathKind::kFile, 1, false, 0);
    const auto upper = set.Add("DONN\xC3\x89" "ES/A.TXT", PathKind::kFile, 1, false, 1);
    CHECK(lower.ok() && upper.ok() && upper.replaced == std::optional<std::size_t>(0));
    CHECK(upper.path == "Donn\xC3\xA9" "es/A.TXT");
    CHECK(set.entries() == 1);
  }
  {
    // DLC over base by order: the DLC's depot comes later and replaces the base file.
    PathSet set;
    CHECK(set.Add("data\\level1.dat", PathKind::kFile, 100, false, 10).ok());
    CHECK(set.Add("data/other.dat", PathKind::kFile, 100, false, 11).ok());
    const auto dlc = set.Add("Data/Level1.dat", PathKind::kFile, 200, false, 20);
    CHECK(dlc.ok() && dlc.replaced == std::optional<std::size_t>(10));
    CHECK(dlc.path == "data/Level1.dat");  // the folder keeps its first spelling
    CHECK(set.entries() == 2);
  }
  {
    // Folder spelling kept from the first path, at every level.
    PathSet set;
    CHECK(set.Add("Data/a", PathKind::kFile, 1, false, 0).path == "Data/a");
    CHECK(set.Add("data/b", PathKind::kFile, 1, false, 1).path == "Data/b");
    CHECK(set.Add("DATA\\Sub\\c", PathKind::kFile, 1, false, 2).path == "Data/Sub/c");
    CHECK(set.Add("data/SUB/d", PathKind::kFile, 1, false, 3).path == "Data/Sub/d");
    const auto folder = set.Add("data/sub/Deeper", PathKind::kDirectory, 1, false, 4);
    CHECK(folder.ok() && folder.path == "Data/Sub/Deeper" && !folder.replaced);
    CHECK(set.Add("DATA/SUB/DEEPER/e", PathKind::kFile, 1, false, 5).path == "Data/Sub/Deeper/e");
    // The same folder entry again replaces the earlier one.
    const auto again = set.Add("Data/Sub/deeper", PathKind::kDirectory, 1, false, 6);
    CHECK(again.ok() && again.replaced == std::optional<std::size_t>(4));
  }
  {
    // A file where a folder is, and a path under a file, both ways round. A refused path records nothing.
    PathSet set;
    CHECK(set.Add("a/b", PathKind::kFile, 1, false, 0).ok());
    auto clash = set.Add("A", PathKind::kFile, 1, false, 1);
    CHECK(!clash.ok() && clash.refusal == PathRefusal::kClash);
    clash = set.Add("a/B/c", PathKind::kFile, 1, false, 2);
    CHECK(clash.refusal == PathRefusal::kClash);
    clash = set.Add("A/b", PathKind::kDirectory, 1, false, 3);
    CHECK(clash.refusal == PathRefusal::kClash);
    CHECK(set.Add("x", PathKind::kFile, 1, false, 4).ok());
    clash = set.Add("X/y", PathKind::kFile, 1, false, 5);
    CHECK(clash.refusal == PathRefusal::kClash);
    clash = set.Add("x", PathKind::kDirectory, 1, false, 6);
    CHECK(clash.refusal == PathRefusal::kClash);
    // The refused "X/y" left no folder spelling: a later "q/X..." is unaffected, and "x" is still a file.
    CHECK(set.Add("Folder/z", PathKind::kFile, 1, false, 7).path == "Folder/z");
    CHECK(set.Add("folder", PathKind::kDirectory, 1, false, 8).path == "Folder");
    CHECK(set.Add("FOLDER", PathKind::kFile, 1, false, 9).refusal == PathRefusal::kClash);
    CHECK(set.entries() == 4);
  }
  {
    // A refused clash records no spelling: "Cap/one" is refused under the file "cap", so a later "CAP/..." can't
    // exist either, and the file keeps its entry.
    PathSet set;
    CHECK(set.Add("cap", PathKind::kFile, 1, false, 0).ok());
    CHECK(set.Add("Cap/one", PathKind::kFile, 1, false, 1).refusal == PathRefusal::kClash);
    const auto replaced = set.Add("CAP", PathKind::kFile, 1, false, 2);
    CHECK(replaced.ok() && replaced.replaced == std::optional<std::size_t>(0));
  }
  {
    // Links are refused and never recorded.
    PathSet set;
    const auto link = set.Add("bin/game", PathKind::kLink, 1, false, 0);
    CHECK(link.refusal == PathRefusal::kLink);
    CHECK(set.entries() == 0);
    CHECK(set.Add("bin", PathKind::kFile, 1, false, 1).ok());  // "bin" never became a folder
  }
  {
    // Normalization and placement happen inside Add: refusals come back, support files land where PlaceFile says.
    PathSet set;
    CHECK(set.Add("../evil", PathKind::kFile, 1, false, 0).refusal == PathRefusal::kDotComponent);
    CHECK(set.Add(".playden-install.json", PathKind::kFile, 1, false, 1).refusal == PathRefusal::kReserved);
    CHECK(set.Add("app/.playden-files", PathKind::kFile, 1, true, 2).refusal == PathRefusal::kReserved);
    CHECK(set.Add("app\\dosbox_arena.conf", PathKind::kFile, 1435828982, true, 3).path == "dosbox_arena.conf");
    CHECK(set.Add("save\\NAMES.DAT", PathKind::kFile, 1435828982, true, 4).path ==
          ".playden-gog/support/1435828982/save/NAMES.DAT");
    // The game's own save folder is another path than the support copy.
    CHECK(set.Add("SAVE/names.dat", PathKind::kFile, 1435828982, false, 5).path == "SAVE/names.dat");
    CHECK(set.entries() == 3);
  }
}

}  // namespace

int main() {
  TestSeparators();
  TestRefusals();
  TestReserved();
  TestFoldCase();
  TestPlacement();
  TestPathSet();
  return 0;
}
