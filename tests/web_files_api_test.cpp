// SPDX-License-Identifier: GPL-3.0-or-later
#include "web/service.h"

#include <arpa/inet.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace playden::web;
namespace fs = std::filesystem;

#define CHECK(condition)                                                                  \
  do {                                                                                    \
    if (!(condition)) {                                                                   \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
      std::exit(1);                                                                       \
    }                                                                                     \
  } while (false)

namespace {
void Put(const fs::path& path, const std::string& text) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << text;
}
std::string Text(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), {});
}
int Open(std::uint16_t port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  CHECK(fd >= 0 && ::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof address) == 0);
  return fd;
}
std::string Exchange(std::uint16_t port, const std::string& text) {
  const int fd = Open(port);
  CHECK(::send(fd, text.data(), text.size(), 0) == static_cast<ssize_t>(text.size()));
  std::string answer;
  char buffer[8192];
  ssize_t count;
  while ((count = ::recv(fd, buffer, sizeof buffer, 0)) > 0) answer.append(buffer, static_cast<std::size_t>(count));
  ::close(fd);
  return answer;
}
int Status(const std::string& answer) { return answer.size() > 12 ? std::atoi(answer.c_str() + 9) : 0; }
std::string Body(const std::string& answer) {
  const auto end = answer.find("\r\n\r\n");
  return end == std::string::npos ? std::string() : answer.substr(end + 4);
}
std::string Encode(const std::string& text) {
  std::string out;
  for (const char raw : text) {
    const auto c = static_cast<unsigned char>(raw);
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_') out += raw;
    else { char code[4]; std::snprintf(code, sizeof code, "%%%02X", c); out += code; }
  }
  return out;
}
}  // namespace

int main() {
  const auto base = fs::canonical(fs::temp_directory_path()) / ("playden-web-api-" + std::to_string(::getpid()));
  fs::remove_all(base);
  const auto bottle = base / "games/steam-12210/library/prefixes/game";
  const auto profile = bottle / "drive_c/users/prospero";
  Put(profile / "Documents/Rockstar Games/GTA IV/Profiles/ABC/SGTA400", "save");
  fs::create_directories(bottle / "drive_c/ProgramData");
  const auto game_folder = base / "usb/playden/import/Game";
  Put(game_folder / "game.exe", "exe");
  Put(game_folder / "page.html", "<script>alert(1)</script>");
  Put(game_folder / "steam_settings/configs.user.ini", "account");
  Put(game_folder / ".playden-install.json", "{}");
  Put(game_folder / "_Redist/PhysX.msi", "p");

  ServiceConfig config;
  config.paths = {(base / "logs").string(), (base / "homebrew").string(), (base / "kits").string(), (base / "app").string()};
  config.server.first_port = config.server.last_port = 0;
  config.server.loopback_only = true;
  config.server.poll_interval_ms = 20;
  config.server.body_timeout_ms = 500;
  config.files.private_profile_paths = {"AppData/Local/Launcher"};
  fs::create_directories(base / "logs");
  Service service(std::move(config));
  FileCatalog catalog;
  catalog.known = true;
  {
    FileGame gta;
    gta.id = "steam-12210";
    gta.name = "Grand \"Theft\" Auto IV";
    gta.kind = "steam";
    gta.app = 12210;
    gta.bottle = bottle.string();
    gta.game_folder = game_folder.string();
    gta.game_folder_shown = "/mnt/usb0/playden/import/Game";
    gta.drive = "usb";
    gta.running = true;
    gta.windows_user_bytes = 123;
    gta.physx_needed = "2.8.1";
    catalog.games.push_back(gta);
    FileGame kept;
    kept.id = "steam-274520";
    kept.name = "Darkwood";
    kept.kind = "steam";
    kept.bottle = bottle.string();
    kept.installed = false;
    catalog.games.push_back(kept);
    FileGame gone;
    gone.id = "local-gone";
    gone.name = "Unplugged";
    gone.kind = "imported";
    gone.bottle = bottle.string();
    gone.game_folder = (base / "absent").string();
    gone.drive = "usb";
    gone.drive_absent = true;
    catalog.games.push_back(gone);
  }
  service.SetFileCatalog(catalog);
  service.SetEnabled(true);
  ServiceSnapshot snapshot;
  for (int i = 0; i < 200 && snapshot.state != ServiceSnapshot::State::kOn; ++i) {
    snapshot = service.Snapshot();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  CHECK(snapshot.state == ServiceSnapshot::State::kOn);
  const auto port = snapshot.port;
  const std::string host = "Host: 127.0.0.1:" + std::to_string(port) + "\r\n";

  const auto get = [&](const std::string& target, const std::string& cookie = {}) {
    return Exchange(port, "GET " + target + " HTTP/1.1\r\n" + host + (cookie.empty() ? "" : "Cookie: " + cookie + "\r\n") + "\r\n");
  };
  const auto post = [&](const std::string& target, const std::string& cookie, const std::string& body = {}, const std::string& more = "X-Playden-Action: files\r\n") {
    return Exchange(port, "POST " + target + " HTTP/1.1\r\n" + host + more + (cookie.empty() ? "" : "Cookie: " + cookie + "\r\n") +
                              "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body);
  };
  const std::string game = "game=steam-12210&root=";

  // Unpaired: nothing about a game, nothing changes.
  CHECK(Body(get("/api/pair/status")).find("\"paired\":false") != std::string::npos);
  for (const std::string& target : std::vector<std::string>{"/api/files/games", "/api/files/game?game=steam-12210", "/api/files/list?" + game + "user", "/api/files/download?" + game + "game&path=game.exe",
                                   "/api/files/zip?" + game + "game", "/api/files/text?" + game + "game&path=game.exe"})
    CHECK(Status(get(target)) == 401);
  for (const std::string& target : std::vector<std::string>{"/api/files/upload?" + game + "game&path=x", "/api/files/mkdir?" + game + "game&path=x", "/api/files/delete?" + game + "game&path=game.exe", "/api/files/restore?" + game + "game&path=x"}) {
    CHECK(Status(post(target, "", "data")) == 401);
    CHECK(Status(post(target, "playden_files=0123456789", "data")) == 401);
  }
  CHECK(fs::exists(game_folder / "game.exe") && !fs::exists(game_folder / "x"));

  // Pairing needs the header, a code the TV shows, and answers with a cookie the page's scripts can't read.
  CHECK(Status(post("/api/pair/start", "", "", "")) == 403);
  CHECK(service.Snapshot().pairing_code.empty());
  CHECK(Status(post("/api/pair/start", "")) == 200);
  const auto code = service.Snapshot().pairing_code;
  CHECK(code.size() == 6);
  CHECK(Status(post("/api/pair/confirm?code=" + code, "", "", "")) == 403);
  const std::string wrong = code == "000000" ? "000001" : "000000";
  CHECK(Status(post("/api/pair/confirm?code=" + wrong, "")) == 403);
  const auto paired = post("/api/pair/confirm?code=" + code, "");
  CHECK(Status(paired) == 200 && service.Snapshot().pairing_code.empty());
  const auto cookie_start = paired.find("Set-Cookie: ");
  CHECK(cookie_start != std::string::npos);
  const auto cookie_line = paired.substr(cookie_start, paired.find("\r\n", cookie_start) - cookie_start);
  CHECK(cookie_line.find("HttpOnly") != std::string::npos && cookie_line.find("SameSite=Strict") != std::string::npos);
  const auto cookie = cookie_line.substr(12, cookie_line.find(';') - 12);
  CHECK(Body(get("/api/pair/status", cookie)).find("\"paired\":true") != std::string::npos);
  // Among other cookies, too.
  CHECK(Status(get("/api/files/games", "a=b; " + cookie + "; c=d")) == 200);

  // Games.
  auto answer = get("/api/files/games", cookie);
  CHECK(Status(answer) == 200);
  auto body = Body(answer);
  CHECK(body.find("\"name\":\"Grand \\\"Theft\\\" Auto IV\"") != std::string::npos && body.find("\"running\":true") != std::string::npos);
  CHECK(body.find(game_folder.string()) == std::string::npos);
  // The game's page: roots, redistributable folders, what it needs.
  CHECK(Body(get("/api/files/game?game=steam-12210", cookie)).find("_Redist") == std::string::npos);
  body = Body(get("/api/files/game?game=steam-12210&redist=1", cookie));
  for (const char* part : {"\"id\":\"user\"", "\"id\":\"steam\"", "\"id\":\"programdata\"", "\"id\":\"public\"", "\"id\":\"game\"", "\"physx_needed\":\"2.8.1\"", "\"path\":\"_Redist\"",
                           "\"physx_installer\":\"_Redist/PhysX.msi\"", "/mnt/usb0/playden/import/Game"})
    CHECK(body.find(part) != std::string::npos);
  CHECK(Status(get("/api/files/game?game=nope", cookie)) == 404);
  // A kept bottle has its saves and no game folder; an unplugged drive the same, with why.
  CHECK(Status(get("/api/files/list?game=steam-274520&root=user", cookie)) == 200);
  CHECK(Status(get("/api/files/list?game=steam-274520&root=game", cookie)) == 404);
  CHECK(Status(get("/api/files/list?game=local-gone&root=user", cookie)) == 200);
  answer = get("/api/files/list?game=local-gone&root=game", cookie);
  CHECK(Status(answer) == 404 && Body(answer).find("Connect this game's drive") != std::string::npos);
  CHECK(Body(get("/api/files/game?game=local-gone", cookie)).find("\"id\":\"game\"") == std::string::npos);

  // Lists and paths.
  body = Body(get("/api/files/list?" + game + "user&path=" + Encode("Documents/Rockstar Games/GTA IV/Profiles/ABC"), cookie));
  CHECK(body.find("\"name\":\"SGTA400\"") != std::string::npos);
  body = Body(get("/api/files/list?" + game + "game", cookie));
  CHECK(body.find(".playden") == std::string::npos && body.find("\"name\":\"steam_settings\"") != std::string::npos);
  CHECK(Status(get("/api/files/list?" + game + "nope", cookie)) == 404);
  CHECK(Status(get("/api/files/list?" + game + "user&path=" + Encode("../../.."), cookie)) == 404);
  CHECK(Status(get("/api/files/list?game=steam-12210&root=" + Encode(base.string()), cookie)) == 404);

  // A download: an attachment, sandboxed; a game's page can't run on this origin.
  answer = get("/api/files/download?" + game + "game&path=page.html", cookie);
  CHECK(Status(answer) == 200 && Body(answer) == "<script>alert(1)</script>");
  CHECK(answer.find("Content-Type: application/octet-stream") != std::string::npos);
  CHECK(answer.find("Content-Disposition: attachment; filename*=UTF-8''page.html") != std::string::npos);
  CHECK(answer.find("Content-Security-Policy: sandbox") != std::string::npos && answer.find("X-Content-Type-Options: nosniff") != std::string::npos);
  CHECK(Status(get("/api/files/download?" + game + "game&path=.playden-install.json", cookie)) == 404);
  answer = Exchange(port, "HEAD /api/files/download?" + game + "game&path=game.exe HTTP/1.1\r\n" + host + "Cookie: " + cookie + "\r\n\r\n");
  CHECK(Status(answer) == 200 && Body(answer).empty() && answer.find("Content-Length: 3") != std::string::npos);

  // Changes need the header, whatever the cookie.
  CHECK(Status(post("/api/files/delete?" + game + "game&path=game.exe", cookie, "", "")) == 403);
  CHECK(fs::exists(game_folder / "game.exe"));
  // Upload, with the header split across packets as a browser may send it.
  CHECK(Status(post("/api/files/upload?" + game + "user&path=Documents/new.sav", cookie, "hello")) == 200);
  CHECK(Text(profile / "Documents/new.sav") == "hello");
  CHECK(Status(post("/api/files/upload?" + game + "user&path=Documents/new.sav", cookie, "other")) == 409);
  answer = post("/api/files/upload?" + game + "user&path=Documents/new.sav&overwrite=1", cookie, "other");
  CHECK(Status(answer) == 200 && Body(answer).find("\"replaced\":true") != std::string::npos);
  CHECK(Status(post("/api/files/upload?" + game + "game&path=steam_settings/x", cookie, "x")) == 403);
  CHECK(Status(post("/api/files/upload?" + game + "game&path=.playden-x", cookie, "x")) == 400);
  CHECK(Status(Exchange(port, "POST /api/files/upload?" + game + "user&path=nolength HTTP/1.1\r\n" + host + "X-Playden-Action: files\r\nCookie: " + cookie + "\r\n\r\n")) == 411);
  // A body that stops: the file doesn't appear.
  {
    const int fd = Open(port);
    const std::string head = "POST /api/files/upload?" + game + "user&path=short.sav HTTP/1.1\r\n" + host + "X-Playden-Action: files\r\nCookie: " + cookie + "\r\nContent-Length: 100\r\n\r\nabc";
    CHECK(::send(fd, head.data(), head.size(), 0) == static_cast<ssize_t>(head.size()));
    ::shutdown(fd, SHUT_WR);
    char buffer[512];
    while (::recv(fd, buffer, sizeof buffer, 0) > 0) {}
    ::close(fd);
    CHECK(!fs::exists(profile / "short.sav"));
    for (const auto& entry : fs::directory_iterator(profile)) CHECK(entry.path().filename().string().rfind(".playden-upload", 0) != 0);
  }
  // Previous version, restore, new folder, delete.
  answer = get("/api/files/download?" + game + "user&path=Documents/new.sav&previous=1", cookie);
  CHECK(Body(answer) == "hello");
  CHECK(Body(get("/api/files/list?" + game + "user&path=Documents", cookie)).find("\"previous\":{\"size\":5") != std::string::npos);
  CHECK(Status(post("/api/files/restore?" + game + "user&path=Documents/new.sav", cookie)) == 200 && Text(profile / "Documents/new.sav") == "hello");
  CHECK(Status(post("/api/files/mkdir?" + game + "game&path=_Redist2", cookie)) == 200 && fs::is_directory(game_folder / "_Redist2"));
  CHECK(Status(post("/api/files/delete?" + game + "game&path=_Redist2", cookie)) == 200 && !fs::exists(game_folder / "_Redist2"));
  CHECK(Status(post("/api/files/delete?" + game + "user&path=Documents/new.sav", cookie)) == 200 && !fs::exists(profile / "Documents/new.sav"));
  // A POST from another origin never reaches these.
  CHECK(Status(post("/api/files/mkdir?" + game + "game&path=evil", cookie, "", "X-Playden-Action: files\r\nOrigin: http://evil.example\r\n")) == 403);
  CHECK(!fs::exists(game_folder / "evil"));
  // A name in Host never reaches these either, cookie or not.
  CHECK(Status(Exchange(port, "GET /api/files/games HTTP/1.1\r\nHost: evil.example\r\nCookie: " + cookie + "\r\n\r\n")) == 403);

  // The editor's text, as JSON.
  Put(game_folder / "autoexec.cfg", "a \"b\"\n");
  body = Body(get("/api/files/text?" + game + "game&path=autoexec.cfg", cookie));
  CHECK(body.find("\"text\":\"a \\\"b\\\"\\u000a\"") != std::string::npos);
  const auto hash_at = body.find("\"sha256\":\"") + 10;
  const auto hash = body.substr(hash_at, body.find('"', hash_at) - hash_at);
  CHECK(Status(post("/api/files/upload?" + game + "game&path=autoexec.cfg&if_sha256=" + hash, cookie, "new")) == 200 && Text(game_folder / "autoexec.cfg") == "new");
  CHECK(Status(post("/api/files/upload?" + game + "game&path=autoexec.cfg&if_sha256=" + hash, cookie, "stale")) == 409 && Text(game_folder / "autoexec.cfg") == "new");

  // A zip, chunked: decoded, it opens.
  answer = get("/api/files/zip?" + game + "game&path=_Redist", cookie);
  CHECK(Status(answer) == 200 && answer.find("Transfer-Encoding: chunked") != std::string::npos);
  CHECK(answer.find("filename*=UTF-8''_Redist.zip") != std::string::npos);
  std::string chunks = Body(answer), zip;
  while (true) {
    const auto line_end = chunks.find("\r\n");
    CHECK(line_end != std::string::npos);
    const auto size = std::strtoul(chunks.substr(0, line_end).c_str(), nullptr, 16);
    if (size == 0) break;
    zip += chunks.substr(line_end + 2, size);
    chunks.erase(0, line_end + 2 + size + 2);
  }
  CHECK(zip.find("_Redist/PhysX.msi") != std::string::npos);

  // Only a couple of transfers at once; the next answers 503 and a list still works.
  {
    Put(game_folder / "big.bin", std::string(16 * 1024 * 1024, 'b'));
    const std::string request = "GET /api/files/download?" + game + "game&path=big.bin HTTP/1.1\r\n" + host + "Cookie: " + cookie + "\r\n\r\n";
    const int first = Open(port), second = Open(port);
    CHECK(::send(first, request.data(), request.size(), 0) > 0 && ::send(second, request.data(), request.size(), 0) > 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));  // both blocked sending to clients that don't read
    answer = get("/api/files/download?" + game + "game&path=game.exe", cookie);
    CHECK(Status(answer) == 503 && answer.find("Retry-After: 1") != std::string::npos);
    const auto started = std::chrono::steady_clock::now();
    CHECK(Status(get("/api/files/list?" + game + "game", cookie)) == 200);
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(1));
    ::close(first);
    ::close(second);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(Status(get("/api/files/download?" + game + "game&path=game.exe", cookie)) == 200);
  }

  // A game's setup: file actions say so at once, and the game's page still opens without touching the disk.
  {
    auto during = catalog;
    during.setting_up = true;
    service.SetFileCatalog(during);
    const auto started = std::chrono::steady_clock::now();
    answer = get("/api/files/list?" + game + "user", cookie);
    CHECK(Status(answer) == 503 && answer.find("X-Playden-Busy: setup") != std::string::npos && answer.find("setting up a game") != std::string::npos);
    answer = get("/api/files/game?game=steam-12210&redist=1", cookie);
    CHECK(Status(answer) == 200 && answer.find("\"all_setting_up\":true") != std::string::npos && answer.find("\"redist_busy\":true") != std::string::npos);
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(1));
    service.SetFileCatalog(catalog);
    CHECK(Status(get("/api/files/list?" + game + "user", cookie)) == 200);
  }

  service.Shutdown();
  fs::remove_all(base);
  std::puts("web_files_api: ok");
}
