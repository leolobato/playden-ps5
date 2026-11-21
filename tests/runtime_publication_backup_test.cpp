#include "runtime/publication_backup.h"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <csignal>
#include <unistd.h>
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "%d: %s\n", __LINE__, #c); std::exit(1); } } while (0)
int main() {
  using namespace playden::runtime;
  namespace fs = std::filesystem;
  char pattern[] = "/tmp/playden-publication-backup-XXXXXX";
  CHECK(mkdtemp(pattern));
  const auto root = fs::canonical(pattern);
  const std::string transaction = "12345678-1234-1234-1234-123456789abc";
  const std::string bytes("WINE REGISTRY Version 2\n\0tail", 29);
  auto created = CreatePublicationRegistryBackup(root, transaction, bytes, 0644);
  CHECK(created.ok());
  const auto binding = created.value();
  CHECK(binding.source_mode == 0644 && binding.size == bytes.size());
  CHECK(binding.filename == "registry-backup-" + transaction + ".v1");
  struct stat info{};
  CHECK(lstat((root / binding.filename).c_str(), &info) == 0);
  CHECK((info.st_mode & 07777) == 0600 && info.st_uid == geteuid());
  auto recovered = LoadPublicationRegistryBackup(root, transaction, binding);
  CHECK(recovered.state == PublicationRecoveryState::kValid && recovered.bytes == bytes);
  CHECK(!CreatePublicationRegistryBackup(root, transaction, "replacement", 0600).ok());
  CHECK(LoadPublicationRegistryBackup(root, transaction, binding).bytes == bytes);
  CHECK(LoadPublicationRegistryBackup(root, "22345678-1234-1234-1234-123456789abc", binding).state == PublicationRecoveryState::kCorrupt);
  std::ofstream(root / binding.filename, std::ios::binary) << std::string(bytes.size(), 'x');
  CHECK(LoadPublicationRegistryBackup(root, transaction, binding).state == PublicationRecoveryState::kCorrupt);
  std::ofstream(root / binding.filename, std::ios::binary).write(bytes.data(), bytes.size());
  auto altered = binding; altered.sha256[0] = altered.sha256[0] == '0' ? '1' : '0';
  CHECK(LoadPublicationRegistryBackup(root, transaction, altered).state == PublicationRecoveryState::kCorrupt);
  altered = binding; ++altered.size;
  CHECK(LoadPublicationRegistryBackup(root, transaction, altered).state == PublicationRecoveryState::kCorrupt);
  altered = binding; altered.filename = "../foreign";
  CHECK(LoadPublicationRegistryBackup(root, transaction, altered).state == PublicationRecoveryState::kCorrupt);
  altered = binding; altered.source_mode = 0666;
  CHECK(LoadPublicationRegistryBackup(root, transaction, altered).state == PublicationRecoveryState::kCorrupt);
  altered = binding; altered.sha256[0] = 'A';
  CHECK(LoadPublicationRegistryBackup(root, transaction, altered).state == PublicationRecoveryState::kCorrupt);
  CHECK(!CreatePublicationRegistryBackup(root, "00000000-0000-0000-0000-000000000000", bytes, 0600).ok());
  CHECK(!CreatePublicationRegistryBackup(root, "../escape", bytes, 0600).ok());
  CHECK(!CreatePublicationRegistryBackup(root, "12345678-1234-1234-1234-123456789abC", bytes, 0600).ok());
  CHECK(!CreatePublicationRegistryBackup(root, transaction, bytes, 0666).ok());
  CHECK(!CreatePublicationRegistryBackup(root / "missing", transaction, bytes, 0600).ok());
  CHECK(!fs::exists(root / "missing"));
  CHECK(chmod((root / binding.filename).c_str(), 0644) == 0);
  CHECK(LoadPublicationRegistryBackup(root, transaction, binding).state == PublicationRecoveryState::kCorrupt);
  CHECK(chmod((root / binding.filename).c_str(), 0600) == 0);
  fs::create_hard_link(root / binding.filename, root / "alias");
  CHECK(LoadPublicationRegistryBackup(root, transaction, binding).state == PublicationRecoveryState::kCorrupt);
  fs::remove(root / "alias");
  fs::remove(root / binding.filename);
  CHECK(LoadPublicationRegistryBackup(root, transaction, binding).state == PublicationRecoveryState::kAbsent);
  fs::create_symlink("missing-target", root / binding.filename);
  CHECK(LoadPublicationRegistryBackup(root, transaction, binding).state == PublicationRecoveryState::kCorrupt);
  fs::remove(root / binding.filename);
  fs::create_directory(root / binding.filename);
  CHECK(LoadPublicationRegistryBackup(root, transaction, binding).state == PublicationRecoveryState::kCorrupt);
  fs::remove(root / binding.filename);
  // A control folder opened to FTP (an open bottle, #868) is still the app user's: the backup's absence is read.
  CHECK(chmod(root.c_str(), 0777) == 0);
  CHECK(LoadPublicationRegistryBackup(root, transaction, binding).state == PublicationRecoveryState::kAbsent);
  CHECK(chmod(root.c_str(), 0700) == 0);
  CHECK(LoadPublicationRegistryBackup(root / "missing", transaction, binding).state == PublicationRecoveryState::kUnavailable);
  fs::create_directory_symlink(root, root / "root-alias");
  CHECK(!CreatePublicationRegistryBackup(root / "root-alias", transaction, bytes, 0600).ok());
  // An interrupted partial backup is not adopted, and remains for reconciliation.
  std::ofstream(root / binding.filename) << "partial";
  CHECK(chmod((root / binding.filename).c_str(), 0600) == 0);
  CHECK(LoadPublicationRegistryBackup(root, transaction, binding).state == PublicationRecoveryState::kCorrupt);
  CHECK(!CreatePublicationRegistryBackup(root, transaction, bytes, 0644).ok());
  CHECK(fs::file_size(root / binding.filename) == 7);
  fs::remove(root / binding.filename);
  // Exact bound accepts a full-size registry; a byte beyond it cannot publish.
  const std::string large(kMaxPublicationRegistryBackupBytes, 'a');
  auto full = CreatePublicationRegistryBackup(root, transaction, large, 0600);
  CHECK(full.ok());
  CHECK(LoadPublicationRegistryBackup(root, transaction, full.value()).bytes == large);
  CHECK(!CreatePublicationRegistryBackup(root, "22345678-1234-1234-1234-123456789abc",
                                         large + "a", 0600).ok());
  // Force a real write failure in a child, without changing the parent limits.
  // The exclusive partial file must survive; retry cannot overwrite it.
  const std::string failed_transaction = "32345678-1234-1234-1234-123456789abc";
  const auto child = fork(); CHECK(child >= 0);
  if (!child) {
    struct rlimit limit{};
    if (getrlimit(RLIMIT_FSIZE, &limit)) _exit(2);
    limit.rlim_cur = 16;
    if (setrlimit(RLIMIT_FSIZE, &limit)) _exit(3);
    std::signal(SIGXFSZ, SIG_IGN);
    auto failed = CreatePublicationRegistryBackup(root, failed_transaction, std::string(256, 'z'), 0600);
    _exit(failed.ok() ? 4 : 0);
  }
  int status = 0;
  CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
  const auto partial = root / ("registry-backup-" + failed_transaction + ".v1");
  CHECK(fs::exists(partial) && fs::file_size(partial) <= 16);
  CHECK(!CreatePublicationRegistryBackup(root, failed_transaction, "retry", 0600).ok());
  CHECK(fs::file_size(partial) <= 16);
  fs::remove_all(root);
  std::puts("publication registry backup tests passed");
}
