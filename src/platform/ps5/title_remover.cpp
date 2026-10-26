// title_remover.cpp - Playden's side of the one-shot title remover payload.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: MIT
#include "platform/ps5/title_remover.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <thread>
#include <unistd.h>

namespace playden::ps5 {
namespace {
constexpr std::size_t kMaxPayload = 4u * 1024u * 1024u;

std::optional<std::string> ReadNoFollow(const std::filesystem::path& path, std::size_t limit) {
  const int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) return std::nullopt;
  struct stat info{};
  std::string bytes;
  bool ok = fstat(fd, &info) == 0 && S_ISREG(info.st_mode) && static_cast<std::size_t>(info.st_size) <= limit;
  if (ok) bytes.resize(static_cast<std::size_t>(info.st_size));
  std::size_t done = 0;
  while (ok && done < bytes.size()) {
    const auto count = read(fd, bytes.data() + done, bytes.size() - done);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) { ok = false; break; }
    done += static_cast<std::size_t>(count);
  }
  close(fd);
  return ok ? std::optional<std::string>(bytes) : std::nullopt;
}

bool WriteRequest(const std::filesystem::path& directory, const std::string& text) {
  const auto temporary = directory / "request.tmp";
  unlink(temporary.c_str());
  const int fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
  if (fd < 0) return false;
  bool ok = fchmod(fd, 0644) == 0 && write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size()) &&
            fsync(fd) == 0;
  ok = close(fd) == 0 && ok;
  if (!ok || rename(temporary.c_str(), (directory / "request").c_str()) != 0) {
    unlink(temporary.c_str());
    return false;
  }
  return true;
}
}  // namespace

TitleRemoverOutcome RunTitleRemover(const TitleRemoverConfig& config, const std::string& title, const std::string& nonce) {
  TitleRemoverOutcome outcome;
  const auto& directory = config.request_directory;
  // The payload trusts only a directory owned by Playden's user that others cannot write.
  struct stat info{};
  if (lstat(directory.c_str(), &info) != 0) {
    if (errno != ENOENT || mkdir(directory.c_str(), 0755) != 0 || lstat(directory.c_str(), &info) != 0) {
      outcome.detail = "request directory unavailable";
      return outcome;
    }
  }
  if (!S_ISDIR(info.st_mode) || info.st_uid != geteuid() ||
      ((info.st_mode & 07777) != 0755 && chmod(directory.c_str(), 0755) != 0)) {
    outcome.detail = "request directory is not Playden's";
    return outcome;
  }
  unlink((directory / "result").c_str());
  unlink((directory / "result.tmp").c_str());
  if (!WriteRequest(directory, "title=" + title + "\nnonce=" + nonce + "\n")) {
    outcome.detail = "request could not be written";
    return outcome;
  }
  const auto payload = ReadNoFollow(config.payload, kMaxPayload);
  if (!payload || payload->size() < 4 || payload->compare(0, 4, "\x7f" "ELF") != 0) {
    outcome.detail = "payload missing";
  } else if (!config.send) {
    outcome.detail = "no payload loader";
  } else {
    std::string error;
    outcome.sent = config.send(*payload, &error);
    if (!outcome.sent) outcome.detail = "loader: " + error;
  }
  for (int poll = 0; outcome.sent && poll < config.result_polls; ++poll) {
    if (const auto text = ReadNoFollow(directory / "result", 256)) {
      const auto nonce_line = "nonce=" + nonce + "\n";
      const auto code_at = text->find("code=");
      if (text->rfind(nonce_line, 0) == 0 && code_at != std::string::npos) {
        outcome.code = static_cast<int>(std::strtoul(text->c_str() + code_at + 5, nullptr, 16));
        outcome.detail = "answered";
        break;
      }
    }
    std::this_thread::sleep_for(config.result_interval);
  }
  if (outcome.sent && !outcome.code) outcome.detail = "no answer";
  unlink((directory / "request").c_str());
  return outcome;
}

bool SendToPayloadLoader(const std::string& elf, std::string* error) {
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) { *error = "socket " + std::to_string(errno); return false; }
  struct timeval timeout{10, 0};
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  struct sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(9021);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (connect(fd, reinterpret_cast<struct sockaddr*>(&address), sizeof(address)) != 0) {
    *error = "connect " + std::to_string(errno);
    close(fd);
    return false;
  }
  std::size_t done = 0;
  while (done < elf.size()) {
    const auto count = send(fd, elf.data() + done, elf.size() - done, 0);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) { *error = "send " + std::to_string(errno); close(fd); return false; }
    done += static_cast<std::size_t>(count);
  }
  shutdown(fd, SHUT_WR);
  close(fd);
  return true;
}
}  // namespace playden::ps5
