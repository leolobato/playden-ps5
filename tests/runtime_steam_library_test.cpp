// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime/steam_library.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(cond)                                                                     \
  do {                                                                                  \
    if (!(cond)) {                                                                      \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);     \
      std::exit(1);                                                                     \
    }                                                                                   \
  } while (0)

using namespace playden::runtime;
namespace fs = std::filesystem;

namespace {

std::string Read(const fs::path& path) {
  std::ifstream file(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

bool Has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

fs::path Fresh() {
  char base[] = "/private/tmp/playden-steam-library-XXXXXX";
  char* made = mkdtemp(base);
  CHECK(made != nullptr);
  return fs::canonical(made);
}

void LibraryFolders() {
  const auto text = SteamLibraryFoldersText("Z:\\data\\playden\\steam-client\\client");
  CHECK(text.rfind("\"libraryfolders\"\n{\n", 0) == 0);
  // Backslashes escaped as the client writes them; the client's own folder is library 0, the bottle's C: library 1.
  CHECK(Has(text, "\t\"0\"\n\t{\n\t\t\"path\"\t\t\"Z:\\\\data\\\\playden\\\\steam-client\\\\client\"\n"));
  CHECK(Has(text, "\t\"1\"\n\t{\n\t\t\"path\"\t\t\"C:\\\\Program Files (x86)\\\\Steam\"\n"));
  CHECK(text.back() == '\n' && text.substr(text.size() - 2) == "}\n");

  const auto root = Fresh();
  const auto client = root / "client";
  fs::create_directories(client);
  CHECK(WriteSteamLibraryFolders(client, "Z:\\data\\playden\\steam-client\\client"));
  for (const char* sub : {"steamapps", "config"}) {
    const auto file = client / sub / "libraryfolders.vdf";
    CHECK(Read(file) == text);
    struct stat info {};
    CHECK(stat(file.c_str(), &info) == 0 && (info.st_mode & 0777) == 0666);
  }
  // A listing the client rewrote with the library in it stays; one without it is written again.
  const std::string rewritten = text + "// the client's own additions\n";
  { std::ofstream(client / "steamapps/libraryfolders.vdf", std::ios::binary) << rewritten; }
  { std::ofstream(client / "config/libraryfolders.vdf", std::ios::binary) << "\"libraryfolders\"\n{\n}\n"; }
  CHECK(WriteSteamLibraryFolders(client, "Z:\\data\\playden\\steam-client\\client"));
  CHECK(Read(client / "steamapps/libraryfolders.vdf") == rewritten);
  CHECK(Read(client / "config/libraryfolders.vdf") == text);
  CHECK(!fs::exists(client / "config/libraryfolders.vdf.playden-tmp"));
  // A link in the way of a folder is refused.
  const auto other = root / "other";
  fs::create_directories(other);
  fs::create_symlink(root, other / "steamapps");
  CHECK(!WriteSteamLibraryFolders(other, "Z:\\x"));
  // A link earlier on the way is refused too, and nothing is written through it.
  const auto real = root / "real";
  fs::create_directories(real / "client");
  fs::create_symlink(real, root / "via");
  CHECK(!WriteSteamLibraryFolders(root / "via/client", "Z:\\x"));
  CHECK(!fs::exists(real / "client/steamapps"));
  // Only a library whose path is the bottle's counts: the path in a comment or another key doesn't.
  const auto mentioned = root / "mentioned";
  fs::create_directories(mentioned / "steamapps");
  fs::create_directories(mentioned / "config");
  { std::ofstream(mentioned / "steamapps/libraryfolders.vdf", std::ios::binary)
        << "\"libraryfolders\"\n{\n// \"C:\\\\Program Files (x86)\\\\Steam\"\n}\n"; }
  { std::ofstream(mentioned / "config/libraryfolders.vdf", std::ios::binary)
        << "\"libraryfolders\"\n{\n\t\"1\"\n\t{\n\t\t\"label\"\t\t\"C:\\\\Program Files (x86)\\\\Steam\"\n\t}\n}\n"; }
  CHECK(WriteSteamLibraryFolders(mentioned, "Z:\\data\\playden\\steam-client\\client"));
  CHECK(Read(mentioned / "steamapps/libraryfolders.vdf") == text);
  CHECK(Read(mentioned / "config/libraryfolders.vdf") == text);
  // The client's own spacing of the line counts.
  { std::ofstream(mentioned / "config/libraryfolders.vdf", std::ios::binary)
        << "\"libraryfolders\"\n{\n  \"1\"\n  {\n    \"path\"  \"C:\\\\Program Files (x86)\\\\Steam\"\r\n  }\n}\n"; }
  const auto spaced = Read(mentioned / "config/libraryfolders.vdf");
  CHECK(WriteSteamLibraryFolders(mentioned, "Z:\\data\\playden\\steam-client\\client"));
  CHECK(Read(mentioned / "config/libraryfolders.vdf") == spaced);
  fs::remove_all(root);
}

void AppManifest() {
  SteamAppManifest manifest;
  manifest.app_id = 257510;
  manifest.name = "The Talos \"Principle\"";
  manifest.install_dir = "The Talos Principle";
  manifest.last_updated = 1794468985;
  manifest.record.app_id = 257510;
  manifest.record.install_bytes = 6227020800ULL;
  manifest.record.language = "german";
  manifest.record.depots = {{257511, 5021113961245098441ULL, 0, 0}, {257515, 77ULL, 257510, 257510},
                            {270050, 88ULL, 270050, 257510}, {257519, 0, 0, 0}};
  const auto text = SteamAppManifestText(manifest);
  CHECK(text.rfind("\"AppState\"\n{\n", 0) == 0);
  CHECK(Has(text, "\t\"appid\"\t\t\"257510\"\n"));
  CHECK(Has(text, "\t\"name\"\t\t\"The Talos \\\"Principle\\\"\"\n"));
  CHECK(Has(text, "\t\"StateFlags\"\t\t\"4\"\n"));
  CHECK(Has(text, "\t\"installdir\"\t\t\"The Talos Principle\"\n"));
  CHECK(Has(text, "\t\"LastUpdated\"\t\t\"1794468985\"\n"));
  CHECK(Has(text, "\t\"SizeOnDisk\"\t\t\"6227020800\"\n"));
  CHECK(Has(text, "\t\t\"257511\"\n\t\t{\n\t\t\t\"manifest\"\t\t\"5021113961245098441\"\n"));
  // A DLC's depot names its DLC; a depot with no manifest is left out.
  CHECK(Has(text, "\t\t\"270050\"\n\t\t{\n\t\t\t\"manifest\"\t\t\"88\"\n\t\t\t\"size\"\t\t\"0\"\n\t\t\t\"dlcappid\"\t\t\"270050\"\n"));
  CHECK(!Has(text, "\"257515\"\n\t\t{\n\t\t\t\"manifest\"\t\t\"77\"\n\t\t\t\"size\"\t\t\"0\"\n\t\t\t\"dlcappid\""));
  CHECK(!Has(text, "\"257519\""));
  CHECK(Has(text, "\t\"UserConfig\"\n\t{\n\t\t\"language\"\t\t\"german\"\n\t}\n"));
  CHECK(!Has(text, "betakey"));
  manifest.record.branch = "beta";
  CHECK(Has(SteamAppManifestText(manifest), "\t\t\"betakey\"\t\t\"beta\"\n"));
  CHECK(SteamAppManifestName(257510) == "appmanifest_257510.acf");

  // Written in the bottle's steamapps, the folders made; the same text again changes nothing.
  const auto root = Fresh();
  const auto prefix = root / "prefix";
  fs::create_directories(prefix / "drive_c");
  CHECK(WriteSteamAppManifest(prefix, 257510, text));
  const auto file = prefix / "drive_c/Program Files (x86)/Steam/steamapps/appmanifest_257510.acf";
  CHECK(Read(file) == text);
  struct stat before {};
  CHECK(stat(file.c_str(), &before) == 0);
  CHECK(WriteSteamAppManifest(prefix, 257510, text));
  struct stat after {};
  CHECK(stat(file.c_str(), &after) == 0 && after.st_ino == before.st_ino);
  CHECK(WriteSteamAppManifest(prefix, 257510, text + "\n"));
  CHECK(Read(file) == text + "\n");
  CHECK(!fs::exists(file.string() + ".playden-tmp"));
  // A link in the bottle's path is refused.
  const auto linked = root / "linked";
  fs::create_directories(linked / "drive_c");
  fs::create_symlink(root, linked / "drive_c/Program Files (x86)");
  CHECK(!WriteSteamAppManifest(linked, 257510, text));
  fs::remove_all(root);
}

}  // namespace

int main() {
  LibraryFolders();
  AppManifest();
  std::puts("steam library: library list, app manifest, writes");
  return 0;
}
