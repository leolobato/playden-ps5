// install_secret.cpp - The console's install secret for sharing recipes and voting.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "community/install_secret.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <filesystem>

#include "platform/storage.h"
#include "runtime/private_directories.h"

namespace playden::community {

bool PrepareCommunityRoot(const std::string& store_root, std::string* error) {
  const std::filesystem::path root(store_root);
  if (!root.is_absolute() || root.lexically_normal() != root || !root.has_parent_path() ||
      root.filename().empty()) {
    if (error) *error = "community folder must be an absolute normalized path";
    return false;
  }
  std::string reason;
  if (!runtime::EnsurePrivateDirectories(root.parent_path(), root, root, &reason)) {
    if (error) *error = "community folder unavailable: " + reason;
    return false;
  }
  return true;
}

namespace {

std::optional<std::string> WriteNew(const std::string& store_root, std::string* error) {
  auto secret = RandomHex(kInstallSecretBytes / 2);
  if (!secret) {
    if (error) *error = "no random source for the sharing ID";
    return std::nullopt;
  }
  const std::string text = *secret + "\n";
  const auto result = Storage(store_root).WriteDurable(kInstallSecretFileName, Bytes(text.begin(), text.end()));
  if (!result.ok()) {
    if (error) *error = std::string("cannot write the sharing ID: ") + result.message();
    return std::nullopt;
  }
  return secret;
}

}  // namespace

bool IsInstallSecret(std::string_view text) {
  if (text.size() != kInstallSecretBytes) return false;
  for (char c : text)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  return true;
}

std::optional<std::string> RandomHex(std::size_t bytes) {
  if (bytes == 0 || bytes > 64) return std::nullopt;
  unsigned char buffer[64];
  const int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
  if (fd < 0) return std::nullopt;
  std::size_t done = 0;
  while (done < bytes) {
    const auto count = ::read(fd, buffer + done, bytes - done);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) {
      ::close(fd);
      return std::nullopt;
    }
    done += static_cast<std::size_t>(count);
  }
  ::close(fd);
  static constexpr char kHex[] = "0123456789abcdef";
  std::string text;
  text.reserve(bytes * 2);
  for (std::size_t i = 0; i < bytes; ++i) {
    text.push_back(kHex[buffer[i] >> 4]);
    text.push_back(kHex[buffer[i] & 0x0f]);
  }
  return text;
}

std::optional<std::string> NewIdempotencyKey() { return RandomHex(16); }

std::optional<std::string> LoadOrCreateInstallSecret(const std::string& store_root, std::string* error,
                                                     std::string* note) {
  if (note) note->clear();
  if (!PrepareCommunityRoot(store_root, error)) return std::nullopt;
  const auto read = Storage(store_root).ReadDurable(kInstallSecretFileName, 256);
  if (read.ok()) {
    std::string text(read.value().begin(), read.value().end());
    if (!text.empty() && text.back() == '\n') text.pop_back();
    if (IsInstallSecret(text)) return text;
    if (note) *note = "community: sharing ID file was malformed; made a new one";
  } else if (read.error() != StorageError::kNotFound) {
    // Unreadable (wrong owner or mode, a link): replacing it would hide the problem.
    if (error) *error = std::string("cannot read the sharing ID: ") + read.message();
    return std::nullopt;
  }
  return WriteNew(store_root, error);
}

std::optional<std::string> ResetInstallSecret(const std::string& store_root, std::string* error) {
  if (!PrepareCommunityRoot(store_root, error)) return std::nullopt;
  return WriteNew(store_root, error);
}

}  // namespace playden::community
