// SPDX-License-Identifier: GPL-3.0-or-later
#include "web/file_policy.h"

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace playden::web;

#define CHECK(condition)                                                                  \
  do {                                                                                    \
    if (!(condition)) {                                                                   \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
      std::exit(1);                                                                       \
    }                                                                                     \
  } while (false)

int main() {
  // Hidden, in any case.
  for (const char* name : {".playden-install.json", ".PLAYDEN-x", ".Playden", ".playden-bak.save", "playden_online.txt", "PLAYDEN_ONLINE.TXT", "SocialClub_ScAuth.txt"})
    CHECK(HiddenName(name));
  for (const char* name : {"playden", "my.playden", "save.dat", ".hidden", "steam_settings"}) CHECK(!HiddenName(name));

  // Names.
  for (const char* name : {"", ".", "..", "a/b", "a\\b", "a\tb", "a\x7f" "b", "\xff", "\xc0\x80", "\xed\xa0\x80", "\xf5\x80\x80\x80", "\xe2\x82"})
    CHECK(!UsableName(name));
  for (const char* name : {"save.dat", "Rockstar Games", "Café Étoile", "セーブ", "..hidden", "a..b", "😀"}) CHECK(UsableName(name));
  CHECK(UsableName(std::string(255, 'a')) && !UsableName(std::string(256, 'a')));
  CHECK(NewName(std::string(230, 'a')) && !NewName(std::string(231, 'a')));
  CHECK(!NewName(".playden-x") && !NewName("playden_online.txt") && NewName("save.dat"));
  CHECK(UsableName(std::string("a\0b", 3)) == false);

  // Paths.
  CHECK(SplitRelativePath("")->empty());
  CHECK((*SplitRelativePath("Documents/Rockstar Games/GTA IV")).size() == 3);
  for (const char* path : {"/etc", "a//b", "a/", "/", "..", "../x", "a/../b", "a/./b", ".", "a\\..\\b", "a/.playden-install.json", ".PLAYDEN-bak.x",
                           "x/playden_online.txt", "a/\xff"})
    CHECK(!SplitRelativePath(path));

  // Read-only folders, at any depth and in any case.
  CHECK(ReadOnlyPath({"steam_settings"}) && ReadOnlyPath({"Bin", "x64", "Steam_Settings", "configs.main.ini"}));
  CHECK(!ReadOnlyPath({"steam_appid.txt"}) && !ReadOnlyPath({}) && !ReadOnlyPath({"steam_settings2"}));

  // Private paths, at or below, in any case.
  const std::vector<std::string> secret = {"AppData/Local/Launcher"};
  CHECK(PrivatePath({"AppData", "Local", "Launcher"}, secret) && PrivatePath({"appdata", "LOCAL", "launcher", "token"}, secret));
  CHECK(!PrivatePath({"AppData", "Local"}, secret) && !PrivatePath({"AppData", "Local", "Other"}, secret) && !PrivatePath({}, secret));
  std::puts("web_file_policy: ok");
}
