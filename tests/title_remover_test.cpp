// Playden's side of the title remover: request, payload hand-off and answer.
#include "platform/ps5/title_remover.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #x); std::exit(1); } } while (0)

namespace fs = std::filesystem;
using namespace playden::ps5;

namespace {
std::string Read(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), {}};
}
void Write(const fs::path& path, const std::string& bytes) { std::ofstream(path, std::ios::binary) << bytes; }
}  // namespace

int main() {
  char name[] = "/tmp/playden-title-remover-XXXXXX";
  CHECK(mkdtemp(name));
  const fs::path root = fs::canonical(name);
  TitleRemoverConfig config;
  config.request_directory = root / "title-remover";
  config.payload = root / "title-remover.elf";
  config.result_polls = 20;
  config.result_interval = std::chrono::milliseconds(1);
  const std::string nonce = "00000000-0000-4000-8000-000000000001";

  // Without the payload nothing is sent, and the request does not stay behind.
  auto outcome = RunTitleRemover(config, "FAKE78002", nonce);
  CHECK(!outcome.sent && outcome.detail == "payload missing");
  struct stat info{};
  // Open, so FTP can delete it (#868); the payload checks owners, not modes.
  CHECK(stat(config.request_directory.c_str(), &info) == 0 && (info.st_mode & 07777) == 0777);
  CHECK(!fs::exists(config.request_directory / "request"));

  // The payload reads the request and answers for this nonce.
  Write(config.payload, std::string("\x7f" "ELF payload"));
  std::string seen_request, sent;
  unsigned request_mode = 0;
  config.send = [&](const std::string& elf, std::string*) {
    sent = elf;
    seen_request = Read(config.request_directory / "request");
    struct stat request{};
    if (stat((config.request_directory / "request").c_str(), &request) == 0) request_mode = request.st_mode & 07777;
    Write(config.request_directory / "result", "nonce=" + nonce + "\ncode=80990001\n");
    return true;
  };
  outcome = RunTitleRemover(config, "FAKE78002", nonce);
  CHECK(sent.rfind("\x7f" "ELF", 0) == 0);
  CHECK(seen_request == "title=FAKE78002\nnonce=" + nonce + "\n");
  CHECK(request_mode == 0666);
  CHECK(stat((config.request_directory / "result").c_str(), &info) == 0);
  CHECK(outcome.sent && outcome.code && static_cast<unsigned>(*outcome.code) == 0x80990001u);

  // A stale answer for another nonce is removed first and never taken as this one's.
  Write(config.request_directory / "result", "nonce=ffffffff-0000-4000-8000-000000000000\ncode=00000000\n");
  config.send = [](const std::string&, std::string*) { return true; };
  outcome = RunTitleRemover(config, "FAKE78002", nonce);
  CHECK(outcome.sent && !outcome.code && outcome.detail == "no answer");

  // A loader that refuses reports why.
  config.send = [](const std::string&, std::string* error) { *error = "connect 61"; return false; };
  outcome = RunTitleRemover(config, "FAKE78002", nonce);
  CHECK(!outcome.sent && outcome.detail == "loader: connect 61");

  // A request directory that is a link is refused.
  fs::remove_all(config.request_directory);
  fs::create_directory(root / "elsewhere");
  fs::create_directory_symlink(root / "elsewhere", config.request_directory);
  outcome = RunTitleRemover(config, "FAKE78002", nonce);
  CHECK(!outcome.sent && outcome.detail == "request directory is not Playden's");
  CHECK(!fs::exists(root / "elsewhere/request"));
  fs::remove_all(root);
  return 0;
}
