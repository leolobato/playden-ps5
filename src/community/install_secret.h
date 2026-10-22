#pragma once
// install_secret.h - The console's install secret for sharing recipes and voting (Phase 5, item 8):
// 128 random bits as 32 lowercase hex characters, made once per console and kept in
// <community store root>/sharing-id (0600, private folder, durable write through
// platform::Storage). It is sent only as "Authorization: Bearer <secret>" to the Playden API and
// never logged (diagnostics::RedactSecrets takes it out of any line that holds it).
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace playden::community {

inline constexpr const char* kInstallSecretFileName = "sharing-id";
inline constexpr std::size_t kInstallSecretBytes = 32;  // hex characters

// Creates store_root private (0700) when missing and checks it is: an absolute normalized folder
// whose parent exists. The sharing ID and the vote memory call it before every file operation.
bool PrepareCommunityRoot(const std::string& store_root, std::string* error);

// Exactly 32 lowercase hex characters.
bool IsInstallSecret(std::string_view text);

// bytes random bytes from /dev/urandom as lowercase hex (not std::random_device: on the console it
// goes through libc arc4random, which may abort); nothing when it can't be read or bytes is 0 or
// more than 64.
std::optional<std::string> RandomHex(std::size_t bytes);

// A fresh Idempotency-Key for one share attempt: 32 lowercase hex (matches the API's
// /^[A-Za-z0-9_-]{8,128}$/). The caller keeps it for every retry of that attempt.
std::optional<std::string> NewIdempotencyKey();

// The secret in store_root/sharing-id, made (and written) first when the file is missing or does
// not hold exactly one secret (a trailing newline is accepted). store_root: an absolute normalized
// folder whose parent exists; it is created private (0700) when missing. *note gets a line safe to
// log when a malformed file was replaced (never the content); *error the reason on failure.
std::optional<std::string> LoadOrCreateInstallSecret(const std::string& store_root, std::string* error,
                                                     std::string* note = nullptr);

// Replaces the secret with a new one ("Reset sharing ID"): earlier votes stay on the service but can
// no longer be changed, and earlier submissions are no longer this console's.
std::optional<std::string> ResetInstallSecret(const std::string& store_root, std::string* error);

}  // namespace playden::community
