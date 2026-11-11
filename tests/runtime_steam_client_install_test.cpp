// Valve's Steam client files for the Steam host (runtime/steam_client_install.h): Valve's current client from its manifest,
// downloaded in ranges, checked against the manifest's hash, unpacked beside the final folder and renamed into place,
// with Playden's steam.exe added and Valve's crash reporter left out.
#include "runtime/steam_client_install.h"

#include "runtime/artifact_hash.h"
#include "stored_zip.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace playden::runtime;

namespace {

std::string Zip(const std::map<std::string, std::string>& files) { return playden::test::StoredZip(files); }

void Write(const fs::path& path, const std::string& bytes) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << bytes;
}
std::string Read(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
unsigned Mode(const fs::path& path) {
  struct stat status {};
  assert(::stat(path.c_str(), &status) == 0);
  return status.st_mode & 07777;
}

void TestManifest() {
  // The shape of Valve's steam_client_win64: other sections around bins_win64, which is the one Playden takes.
  const std::string sha(64, 'a');
  const std::string manifest = "\"win64\"\n{\n\t\"version\"\t\t\"1792618285\"\n\t\"ostype\"\t\t\"win10\"\n"
                               "\t\"tenfoot_images_all\"\n\t{\n\t\t\"file\"\t\t\"tenfoot.zip.1\"\n\t\t\"size\"\t\t\"6\"\n\t}\n"
                               "\t\"bins_win64\"\n\t{\n\t\t\"file\"\t\t\"bins_win64.zip.1f91\"\n\t\t\"size\"\t\t\"64309479\"\n"
                               "\t\t\"sha2\"\t\t\"" + sha + "\"\n\t\t\"zipvz\"\t\t\"bins_win64.zip.vz.4a74\"\n\t}\n"
                               "\t\"bins_cef_win64\"\n\t{\n\t\t\"file\"\t\t\"cef.zip\"\n\t}\n"
                               // A section in a section (as steam_win64's "steamrow"), with a bins_win64 of its own
                               // that isn't the one Playden takes.
                               "\t\"steam_win64\"\n\t{\n\t\t\"file\"\t\"steam.zip\"\n\t\t\"steamrow\"\n\t\t{\n"
                               "\t\t\t\"bins_win64\"\n\t\t\t{\n\t\t\t\t\"file\"\t\"other.zip\"\n\t\t\t}\n\t\t}\n\t}\n}\n";
  const auto parsed = ParseSteamClientManifest(manifest);
  assert(parsed && parsed->version == "1792618285" && parsed->package.file == "bins_win64.zip.1f91" &&
         parsed->package.bytes == 64309479 && parsed->package.sha256 == sha);
  const auto changed = [&](const std::string& from, const std::string& to) {
    auto text = manifest;
    text.replace(text.find(from), from.size(), to);
    return ParseSteamClientManifest(text);
  };
  // Refused: no bins_win64, a version that isn't digits, a hash that isn't 64 lower-case hex digits, a file with a
  // path, a size that isn't a number, text that isn't the manifest.
  assert(!changed("\"bins_win64\"", "\"bins_osx\""));
  assert(!changed("\"1792618285\"", "\"17926x\""));
  assert(!changed(sha, std::string(64, 'A')) && !changed(sha, std::string(63, 'a')));
  assert(!changed("bins_win64.zip.1f91", "../bins.zip") && !changed("bins_win64.zip.1f91", "a/b.zip"));
  assert(!changed("\"64309479\"", "\"64MB\""));
  assert(!ParseSteamClientManifest("") && !ParseSteamClientManifest("<html>not found</html>") &&
         !ParseSteamClientManifest(manifest.substr(0, manifest.size() / 2)));
  // Valve's manifest as it was served on 2026-11-10.
  {
    std::ifstream in(fs::path(__FILE__).parent_path() / "fixtures/steam_client_win64-1792618285.vdf");
    const std::string valve((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const auto real = ParseSteamClientManifest(valve);
    assert(!valve.empty() && real && real->version == "1792618285" &&
           real->package.file == "bins_win64.zip.1f91da2609327de9658146b01307e89a13cc1ef8" && real->package.bytes == 64309479 &&
           real->package.sha256 == "f34b0d5924d0cca4c81775ed2fef0aee1ca060e2daa5c24270711267786c0236");
  }
  // Valve's crash reporter is left out; nothing else is.
  for (const char* name : {"crashhandler.dll", "crashhandler64.dll", "steamerrorreporter.exe", "steamerrorreporter64.exe"})
    assert(SteamClientLeftOut(name));
  assert(!SteamClientLeftOut("steamclient64.dll") && !SteamClientLeftOut("bin/crashhandler64.dll"));
}

}  // namespace

int main() {
  TestManifest();
  char pattern[] = "/private/tmp/playden-steam-client-XXXXXX";
  const fs::path root = ::mkdtemp(pattern);
  const auto folder = root / "steam-client" / "client";
  const auto host = root / "steam.exe";
  Write(host, "MZ playden steam host");
  const auto host_sha = *HashArtifactBytes("MZ playden steam host");
  const std::string client = "MZ valve steamclient64";
  // Valve's packages are made on Windows: one folder is named "bin\shaders/". Its crash reporter is left out.
  const auto zip = Zip({{"steamclient64.dll", client}, {"bin/", ""}, {"bin/SteamService.dll", "MZ service"},
                        {"bin\\shaders/", ""}, {"steam.exe", "MZ valve's own"}, {"config/readme.txt", "text"},
                        {"crashhandler64.dll", "MZ reporter"}, {"crashhandler.dll", "MZ reporter"},
                        {"steamerrorreporter64.exe", "MZ reporter"}});
  const SteamClientManifest manifest{"1788652215", {"bins_win64.zip.test", *HashArtifactBytes(zip), zip.size()}};
  const auto client_sha = *HashArtifactBytes(client);

  // Served in ranges; the first run breaks off after one range, the next resumes from what it kept.
  int calls = 0, fail_after = 1;
  std::uint64_t asked = 0;
  std::string served = zip;
  RangeFetch fetch = [&](const std::string& url, std::uint64_t offset, std::uint64_t length,
                         std::string* error) -> std::optional<std::string> {
    assert(url.rfind(kSteamClientCdn, 0) == 0);
    if (fail_after >= 0 && calls++ >= fail_after) {
      *error = "offline";
      return std::nullopt;
    }
    asked += length;
    return served.substr(offset, length);
  };

  // Nothing yet, and no way to download: refused, nothing in place.
  auto result = InstallSteamClient(folder.string(), host.string(), {}, manifest, false, host_sha);
  assert(result.result == "failed" && result.download_failed && !fs::exists(folder));
  // The download breaks off: refused, and what arrived is kept for the next try.
  fail_after = 0;
  result = InstallSteamClient(folder.string(), host.string(), fetch, manifest, false, host_sha);
  assert(result.result == "failed" && result.download_failed && !fs::exists(folder) && !fs::exists(folder.string() + ".staging"));
  // Whole: installed, Playden's steam.exe in place of Valve's, no crash reporter, folders 0777, programs 0755, other
  // files 0666; the result names the version and the DLL's hash for Playden's own record.
  fail_after = -1;
  result = InstallSteamClient(folder.string(), host.string(), fetch, manifest, false, host_sha);
  assert(result.result == "installed" && result.files == 4 && result.version == "1788652215" && result.client_dll_sha256 == client_sha);
  assert(Read(folder / "steamclient64.dll") == client && Read(folder / "steam.exe") == "MZ playden steam host");
  assert(!fs::exists(folder / "crashhandler64.dll") && !fs::exists(folder / "crashhandler.dll") &&
         !fs::exists(folder / "steamerrorreporter64.exe"));
  assert(Read(folder / "bin/SteamService.dll") == "MZ service" && Read(folder / "config/readme.txt") == "text");
  assert(fs::is_directory(folder / "bin/shaders") && Mode(folder / "bin/shaders") == 0777);
  assert(Mode(folder) == 0777 && Mode(folder / "bin") == 0777 && Mode(folder / "steam.exe") == 0755 &&
         Mode(folder / "steamclient64.dll") == 0755 && Mode(folder / "config/readme.txt") == 0666);
  assert(SteamClientInstalled(folder.string(), host_sha) == std::optional<std::string>("1788652215"));
  assert(!SteamClientInstalled(folder.string(), client_sha));
  // The package is removed once unpacked; a second call finds the install; replace puts it in again.
  assert(!fs::exists(root / "steam-client/downloads/bins_win64.zip.test"));
  result = InstallSteamClient(folder.string(), host.string(), {}, manifest, false, host_sha);
  assert(result.result == "already" && result.version == "1788652215");
  result = InstallSteamClient(folder.string(), host.string(), fetch, manifest, true, host_sha);
  assert(result.result == "installed" && result.client_dll_sha256 == client_sha);
  // The folder is open to everyone: a steam.exe swapped in behind the marker is not taken for Playden's, and the client
  // is put in place again.
  Write(folder / "steam.exe", "MZ someone else's");
  assert(!SteamClientInstalled(folder.string(), host_sha));
  // Without a download it is not put back, and the swapped one is gone: the game starts without Steam.
  result = InstallSteamClient(folder.string(), host.string(), {}, manifest, false, host_sha);
  assert(result.result == "failed" && result.download_failed && !fs::exists(folder / "steam.exe"));
  result = InstallSteamClient(folder.string(), host.string(), fetch, manifest, false, host_sha);
  assert(result.result == "installed" && Read(folder / "steam.exe") == "MZ playden steam host");
  // A link in the folder's place goes too; what it points at stays.
  {
    const auto other = root / "other-client";
    fs::create_directory(other);
    Write(other / "steam.exe", "MZ someone else's");
    Write(other / ".playden-steam-client", Read(folder / ".playden-steam-client"));
    const auto saved = root / "saved-client";
    fs::rename(folder, saved);
    fs::create_directory_symlink(other, folder);
    result = InstallSteamClient(folder.string(), host.string(), {}, manifest, false, host_sha);
    assert(result.result == "failed" && !fs::exists(fs::symlink_status(folder)) && Read(other / "steam.exe") == "MZ someone else's");
    fs::rename(saved, folder);
    fs::remove_all(other);
  }

  // The client's saved sign-in goes; the rest stays.
  Write(folder / "config/config.vdf", "ConnectCache");
  Write(folder / "config/loginusers.vdf", "users");
  assert(RemoveSteamClientSignIn(folder.string()) && RemoveSteamClientSignIn(folder.string()));
  assert(!fs::exists(folder / "config/config.vdf") && !fs::exists(folder / "config/loginusers.vdf"));
  assert(fs::exists(folder / "config/readme.txt"));

  // Valve's next client: while its download fails, the installed one stays whole; once it arrives it takes the
  // folder's place, without the old client's state, and an earlier client folder named by its version goes.
  const std::string client2 = "MZ valve steamclient64 newer";
  const auto zip2 = Zip({{"steamclient64.dll", client2}, {"steam.exe", "MZ valve's own"}});
  const SteamClientManifest newer{"1792618285", {"bins_win64.zip.newer", *HashArtifactBytes(zip2), zip2.size()}};
  Write(folder / "appcache/state", "old");
  Write(root / "steam-client/1788652215/steam.exe", "an earlier Playden's folder");
  fail_after = 0;
  calls = 0;
  result = InstallSteamClient(folder.string(), host.string(), fetch, newer, false, host_sha);
  assert(result.result == "failed" && SteamClientInstalled(folder.string(), host_sha) == std::optional<std::string>("1788652215"));
  assert(Read(folder / "steam.exe") == "MZ playden steam host");
  fail_after = -1;
  served = zip2;
  result = InstallSteamClient(folder.string(), host.string(), fetch, newer, false, host_sha);
  assert(result.result == "installed" && result.version == "1792618285" && result.client_dll_sha256 == *HashArtifactBytes(client2));
  assert(SteamClientInstalled(folder.string(), host_sha) == std::optional<std::string>("1792618285"));
  assert(Read(folder / "steamclient64.dll") == client2 && !fs::exists(folder / "appcache"));
  assert(!fs::exists(root / "steam-client/1788652215"));
  served = zip;

  // Another steam.exe (a Playden update): the folder is made again.
  Write(host, "MZ playden steam host 2");
  const auto host2_sha = *HashArtifactBytes("MZ playden steam host 2");
  assert(!SteamClientInstalled(folder.string(), host2_sha));
  result = InstallSteamClient(folder.string(), host.string(), fetch, manifest, false, host2_sha);
  assert(result.result == "installed" && Read(folder / "steam.exe") == "MZ playden steam host 2");

  // A package that is not the one the manifest names is refused and not kept, and nothing is put in place.
  fs::remove_all(folder);
  SteamClientManifest other = manifest;
  other.package.sha256 = std::string(64, '0');
  result = InstallSteamClient(folder.string(), host.string(), fetch, other, false, host2_sha);
  assert(result.result == "failed" && result.download_failed && !fs::exists(folder));
  assert(!fs::exists(root / "steam-client/downloads/bins_win64.zip.test.part"));
  // A package without the client DLL is refused.
  served = Zip({{"steam.exe", "MZ valve's own"}});
  const SteamClientManifest empty{"1788652216", {"bins_win64.zip.empty", *HashArtifactBytes(served), served.size()}};
  result = InstallSteamClient(folder.string(), host.string(), fetch, empty, false, host2_sha);
  assert(result.result == "failed" && !result.download_failed && !fs::exists(folder));
  served = zip;
  // A damaged steam.exe is refused before anything is downloaded, and an unchecked one in the folder still goes.
  const auto before = asked;
  Write(folder / "steam.exe", "MZ someone else's");
  result = InstallSteamClient(folder.string(), host.string(), fetch, manifest, false, host_sha);
  assert(result.result == "failed" && asked == before && !fs::exists(folder / "steam.exe"));
  fs::remove_all(folder);

  // A link in place of the downloads folder is refused, and what it points at keeps its mode.
  fs::remove_all(root / "steam-client");
  const auto elsewhere = root / "elsewhere";
  fs::create_directory(elsewhere);
  ::chmod(elsewhere.c_str(), 0700);
  fs::create_directory(root / "steam-client");
  fs::create_directory_symlink(elsewhere, root / "steam-client/downloads");
  result = InstallSteamClient(folder.string(), host.string(), fetch, manifest, false, host2_sha);
  assert(result.result == "failed" && Mode(elsewhere) == 0700 && !fs::exists(folder));

  fs::remove_all(root);
  std::puts("steam client install passed");
  return 0;
}
