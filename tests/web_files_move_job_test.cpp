// SPDX-License-Identifier: GPL-3.0-or-later
// A move across roots runs on its own thread: POST answers 202 when it is still copying, the status route says how far
// it is and then gives the move's own answer, it counts as a transfer while it runs, and closing stops it safely.
#include "web/files_api.h"
#include "web/router.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>

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

struct Answer {
  int status = 0;
  std::string head, body;
  bool Has(const std::string& text) const { return body.find(text) != std::string::npos; }
};

Answer Call(Router& router, Method method, const std::string& target, const std::string& cookie) {
  Request request;
  request.method = method;
  request.method_text = method == Method::kPost ? "POST" : "GET";
  const auto question = target.find('?');
  request.path = target.substr(0, question);
  request.query = question == std::string::npos ? std::string() : target.substr(question + 1);
  if (method == Method::kPost) {
    request.headers.emplace_back("x-playden-action", "files");
    request.content_length = 0;
  }
  if (!cookie.empty()) request.headers.emplace_back("cookie", cookie);
  std::string bytes;
  Response response([&bytes](const char* data, std::size_t size) { bytes.append(data, size); return true; }, false);
  router.Handle(request, response);
  Answer answer;
  answer.status = response.status();
  const auto end = bytes.find("\r\n\r\n");
  answer.head = bytes.substr(0, end);
  answer.body = end == std::string::npos ? std::string() : bytes.substr(end + 4);
  return answer;
}

// A test's way to hold a move where it is until the test lets it go.
class Gate {
 public:
  void Close() {
    std::lock_guard<std::mutex> lock(mutex_);
    open_ = false;
    waiting_ = 0;
  }
  void Open() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      open_ = true;
    }
    changed_.notify_all();
  }
  void Pass() {
    std::unique_lock<std::mutex> lock(mutex_);
    ++waiting_;
    changed_.notify_all();
    changed_.wait(lock, [this] { return open_; });
  }
  // Until n moves wait at it.
  void AwaitWaiting(int n) {
    std::unique_lock<std::mutex> lock(mutex_);
    CHECK(changed_.wait_for(lock, std::chrono::seconds(10), [&] { return waiting_ >= n; }));
  }

 private:
  std::mutex mutex_;
  std::condition_variable changed_;
  bool open_ = true;
  int waiting_ = 0;
};

std::string JobOf(const Answer& answer) {
  const auto start = answer.body.find("\"job\":\"");
  CHECK(start != std::string::npos);
  const auto from = start + 7;
  return answer.body.substr(from, answer.body.find('"', from) - from);
}

}  // namespace

int main() {
  const auto base = fs::canonical(fs::temp_directory_path()) / ("playden-move-job-" + std::to_string(::getpid()));
  fs::remove_all(base);
  const auto bottle = base / "games/steam-1/library/prefixes/game";
  const auto profile = bottle / "drive_c/users/prospero";
  const auto game_folder = base / "games-folder/Game";
  fs::create_directories(game_folder);
  const std::string big(3 * 1024 * 1024, 'x');  // three chunks
  Put(profile / "Documents/Saves/big.bin", big);
  Put(profile / "Documents/Saves/small.sav", "s");
  Put(profile / "Documents/Other/one.sav", "1");
  Put(profile / "Documents/quick.sav", "q");
  Put(profile / "Documents/Last/a.sav", "a");
  Put(profile / "Documents/Last/b.sav", "b");

  Gate sync_gate, space_gate;
  FilesConfig config;
  config.move_wait_ms = 300;
  config.store.fault = [&](const std::string& step) {
    if (step == "copy-sync") sync_gate.Pass();
    return true;
  };
  config.free_space = [&](const std::string&) -> std::optional<std::uint64_t> {
    space_gate.Pass();
    return std::nullopt;
  };
  auto api = std::make_unique<FilesApi>(config);
  Router router;
  api->Register(router);
  FileCatalog catalog;
  catalog.known = true;
  FileGame game;
  game.id = "steam-1";
  game.name = "Game";
  game.kind = "steam";
  game.app = 1;
  game.bottle = bottle.string();
  game.game_folder = game_folder.string();
  game.drive = "usb";
  catalog.games.push_back(game);
  api->SetCatalog(catalog);

  // Pair this browser.
  CHECK(Call(router, Method::kPost, "/api/pair/start", "").status == 200);
  const auto paired = Call(router, Method::kPost, "/api/pair/confirm?code=" + api->PairingCode(), "");
  CHECK(paired.status == 200);
  const auto cookie_at = paired.head.find("Set-Cookie: ");
  CHECK(cookie_at != std::string::npos);
  const auto cookie = paired.head.substr(cookie_at + 12, paired.head.find(';', cookie_at) - cookie_at - 12);
  const std::string move = "/api/files/move?game=steam-1&root=user&to_root=game&to=&path=";
  const auto status = [&](const std::string& job) { return Call(router, Method::kGet, "/api/files/move/status?job=" + job, cookie); };

  // Over at once: the answer a rename gives, no job.
  auto answer = Call(router, Method::kPost, move + "Documents/quick.sav", cookie);
  CHECK(answer.status == 200 && answer.Has("\"files\":1") && !answer.Has("\"job\"") && Text(game_folder / "quick.sav") == "q");
  // A name already there still asks at once.
  Put(profile / "Documents/quick.sav", "q2");
  answer = Call(router, Method::kPost, move + "Documents/quick.sav", cookie);
  CHECK(answer.status == 409 && answer.Has("\"clash\":true"));
  CHECK(Call(router, Method::kGet, "/api/files/move/status?job=1", "").status == 401);
  CHECK(status("nope").status == 404);

  // Held after the big file's bytes are copied: 202, and the status says how far it is.
  sync_gate.Close();
  answer = Call(router, Method::kPost, move + "Documents/Saves", cookie);
  CHECK(answer.status == 202);
  const auto job = JobOf(answer);
  sync_gate.AwaitWaiting(1);
  answer = status(job);
  CHECK(answer.status == 200 && answer.Has("\"running\":true"));
  CHECK(answer.Has("\"total_bytes\":" + std::to_string(big.size() + 1)) && answer.Has("\"total_files\":2"));
  CHECK(answer.Has("\"bytes\":" + std::to_string(big.size())) && answer.Has("\"files\":0") && answer.Has("\"current\":\"big.bin\""));
  CHECK(api->InUse());
  // A second move across roots takes the other transfer; a third finds none.
  answer = Call(router, Method::kPost, move + "Documents/Other", cookie);
  CHECK(answer.status == 202);
  const auto other = JobOf(answer);
  sync_gate.AwaitWaiting(2);
  answer = Call(router, Method::kPost, move + "Documents/Last", cookie);
  CHECK(answer.status == 503 && fs::exists(profile / "Documents/Last/a.sav"));
  // A rename in one root isn't a transfer.
  CHECK(Call(router, Method::kPost, "/api/files/move?game=steam-1&root=user&path=Documents/Last&name=Final", cookie).status == 200);
  sync_gate.Open();
  for (const auto& id : {job, other}) {
    for (int i = 0; i < 500; ++i) {
      answer = status(id);
      if (!answer.Has("\"running\"")) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(answer.status == 200 && !answer.Has("\"running\""));
  }
  CHECK(status(job).Has("\"files\":2") && status(job).Has("\"name\":\"Saves\""));
  CHECK(Text(game_folder / "Saves/big.bin") == big && Text(game_folder / "Saves/small.sav") == "s" && !fs::exists(profile / "Documents/Saves"));
  CHECK(Text(game_folder / "Other/one.sav") == "1");
  // Asked again (a page that retried): the same answer.
  CHECK(status(job).status == 200 && status(job).Has("\"files\":2"));

  // Closing Playden in the middle of a copy (the service's shutdown): the move stops within a chunk, the file stays
  // whole where it was and no temporary is left in the game folder. StopMoves waits for it; no move starts after.
  space_gate.Close();
  answer = Call(router, Method::kPost, move + "Documents/Final", cookie);
  CHECK(answer.status == 202);
  const auto last = JobOf(answer);
  space_gate.AwaitWaiting(1);
  std::thread closing([&] { api->StopMoves(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  space_gate.Open();
  closing.join();
  CHECK(Text(profile / "Documents/Final/a.sav") == "a" && Text(profile / "Documents/Final/b.sav") == "b");
  for (const auto& entry : fs::recursive_directory_iterator(game_folder))
    CHECK(entry.path().filename().string().rfind(".playden-upload-", 0) != 0);
  CHECK(!fs::exists(game_folder / "Final/a.sav") && !fs::exists(game_folder / "Final/b.sav"));
  CHECK(status(last).status == 404);
  CHECK(Call(router, Method::kPost, move + "Documents/Final", cookie).status == 503);
  api->StopMoves();
  api.reset();

  fs::remove_all(base);
  std::puts("web_files_move_job ok");
  return 0;
}
