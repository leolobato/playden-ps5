#pragma once
// zip_extract.h - Unpacks one folder of a release zip (miniz), refusing anything unsafe.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: MIT
#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace playden::update {

struct ZipExtractOptions {
  // Only entries below this folder are unpacked, without it ("playden-ps5-0.3.0/FAKE77031/").
  // Must end with "/". Empty unpacks everything.
  std::string inner_prefix;
  std::uint64_t max_entries = 50000;
  std::uint64_t max_total_bytes = 8ull << 30;
  // The mode a file gets; nullopt refuses the file. By default the zip's own mode, or 0644.
  std::function<std::optional<unsigned>(const std::string& relative, unsigned zip_mode)> mode_for;
  // Called after each file with the bytes unpacked so far and in total.
  std::function<void(std::uint64_t done, std::uint64_t total)> progress;
  // Diagnostics (one line each, no newline).
  std::function<void(const std::string& line)> log;
  const std::atomic<bool>* cancel = nullptr;
  // Sets a written file's mode; fchmod by default (tests replace it).
  std::function<int(int fd, unsigned mode)> set_mode;
  // Sets the mode of a file the app user doesn't own, as root (see ExtractZip). Without it such a
  // file is deleted and written again instead.
  std::function<bool(const std::string& path, unsigned mode)> fix_mode;
  // When set, every folder it creates below dest gets this mode, also the ones the zip only implies
  // (mkdir alone is umasked). A folder the app user can't chmod gets it through fix_mode.
  std::optional<unsigned> folder_mode;
  // Sets a created folder's mode; chmod by default (tests replace it).
  std::function<int(const std::string& path, unsigned mode)> set_folder_mode;
};

struct ZipExtractResult {
  bool ok = false;
  bool cancelled = false;
  std::string error;  // short reason, with the entry name when one is at fault
  std::uint64_t files = 0;
  std::uint64_t folders = 0;
  std::uint64_t bytes = 0;
  std::uint64_t rewritten = 0;   // files that failed the read-back check and were written again
  std::uint64_t mode_fixed = 0;  // root-owned files whose mode fix_mode set
};

// Unpacks into dest, which must be an existing empty folder. Folders are created 0777 (less the
// umask, unless folder_mode is set). Every
// file is checked after it is written (size and mode, by fstat). On the PS5 another thread may
// hold root's effective user id for a moment (euid is per process): a file created then belongs to
// root and the app user can't change its mode (results/20261015-self-update-probes). Such a file
// gets its mode through fix_mode; any other failed file is deleted and written again, up to twice.
// On failure dest is left as it is; the caller removes it.
ZipExtractResult ExtractZip(const std::string& zip_path, const std::string& dest, const ZipExtractOptions& options);

// The folder ("top/inner/") of the first entry whose path is "<top>/<inner>/<marker>", for
// example marker "sce_sys/param.json" or "manifest.json". Empty when there is none.
std::string FindInnerPrefix(const std::string& zip_path, const std::string& marker);

// True when name is a safe relative path: no leading "/", no "..", ".", empty parts,
// backslashes, colons or control characters.
bool SafeEntryName(const std::string& name);

}  // namespace playden::update
