// SPDX-License-Identifier: GPL-3.0-or-later
#include "gog/dependencies.h"

#include "gog/path_rules.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <memory>
#include <vector>

namespace playden::gog {
namespace {

constexpr std::string_view kRedistPrefix = "__redist/";

bool PrintableAscii(std::string_view text) {
  for (const char c : text) {
    const auto byte = static_cast<unsigned char>(c);
    if (byte < 0x20 || byte > 0x7e) return false;
  }
  return true;
}

bool UsableArguments(std::string_view arguments) {
  return arguments.size() <= kMaxDependencyArgumentsBytes && PrintableAscii(arguments);
}

std::optional<RedistSkip> SkipFromName(std::string_view name) {
  for (const auto skip : {RedistSkip::kMissing, RedistSkip::kNoProgram, RedistSkip::kRefused})
    if (name == RedistSkipName(skip)) return skip;
  return std::nullopt;
}

bool ValidEntry(const RedistEntry& entry) {
  if (!ValidDependencyId(entry.id)) return false;
  if (!entry.package) return true;
  return IsRedistPlacement(entry.program) && UsableArguments(entry.arguments);
}

std::string EntryLine(const RedistEntry& entry) {
  if (entry.package) return "P\t" + entry.id + "\t" + entry.program + "\t" + entry.arguments + "\n";
  return "S\t" + entry.id + "\t" + RedistSkipName(entry.skip) + "\n";
}

// The fields of a line split at tabs.
std::vector<std::string_view> Fields(std::string_view line) {
  std::vector<std::string_view> fields;
  std::size_t start = 0;
  for (;;) {
    const auto tab = line.find('\t', start);
    if (tab == std::string_view::npos) {
      fields.push_back(line.substr(start));
      return fields;
    }
    fields.push_back(line.substr(start, tab - start));
    start = tab + 1;
  }
}

std::optional<RedistEntry> ParseLine(std::string_view line) {
  for (const char c : line) {
    const auto byte = static_cast<unsigned char>(c);
    if ((byte < 0x20 && c != '\t') || byte == 0x7f) return std::nullopt;
  }
  const auto fields = Fields(line);
  RedistEntry entry;
  if (fields[0] == "P" && fields.size() == 4) {
    entry.package = true;
    entry.id = std::string(fields[1]);
    entry.program = std::string(fields[2]);
    entry.arguments = std::string(fields[3]);
  } else if (fields[0] == "S" && fields.size() == 3) {
    const auto skip = SkipFromName(fields[2]);
    if (!skip) return std::nullopt;
    entry.id = std::string(fields[1]);
    entry.skip = *skip;
  } else {
    return std::nullopt;
  }
  if (!ValidEntry(entry)) return std::nullopt;
  return entry;
}

bool WriteAll(int fd, const std::string& text) {
  std::size_t done = 0;
  while (done < text.size()) {
    const auto wrote = write(fd, text.data() + done, text.size() - done);
    if (wrote < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    done += static_cast<std::size_t>(wrote);
  }
  return true;
}

}  // namespace

bool RedistDependency(const Dependency& dependency) { return !GameFolderDependency(dependency); }

std::optional<std::string> RedistPlace(std::string_view raw) {
  auto normalized = NormalizePath(raw);
  if (!normalized) return std::nullopt;
  std::string rest = std::move(*normalized);
  if (rest.size() > kRedistPrefix.size() && FoldCase(rest.substr(0, kRedistPrefix.size())) == kRedistPrefix) {
    auto again = NormalizePath(std::string_view(rest).substr(kRedistPrefix.size()));
    if (!again) return std::nullopt;
    rest = std::move(*again);
  }
  std::string placed = kRedistFolder + rest;
  if (!IsRedistPlacement(placed)) return std::nullopt;
  return placed;
}

const char* RedistSkipName(RedistSkip skip) {
  switch (skip) {
    case RedistSkip::kMissing: return "missing";
    case RedistSkip::kNoProgram: return "no_program";
    case RedistSkip::kRefused: return "refused";
  }
  return "missing";
}

int WriteRedistList(const std::string& root, const std::vector<RedistEntry>& entries) {
  if (root.empty() || root.front() != '/') return EINVAL;
  return WriteRedistListFile(root + "/" + kRedistListPath, entries);
}

int WriteRedistListFile(const std::string& list_path, const std::vector<RedistEntry>& entries) {
  if (list_path.empty() || list_path.front() != '/' || entries.size() > kMaxRedistEntries) return EINVAL;
  std::string text = std::string(kRedistListHeader) + "\n";
  for (const auto& entry : entries) {
    if (!ValidEntry(entry)) return EINVAL;
    text += EntryLine(entry);
  }
  if (text.size() > kMaxRedistListBytes) return EINVAL;

  const std::string temporary = list_path + ".tmp";
  const std::string& path = list_path;
  // A stale temporary (or a link someone left there) goes first: O_NOFOLLOW | O_EXCL never writes through it.
  if (unlink(temporary.c_str()) != 0 && errno != ENOENT) return errno;
  const int fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0666);
  if (fd < 0) return errno;
  int error = 0;
  if (!WriteAll(fd, text) || fsync(fd) != 0) error = errno;
  if (close(fd) != 0 && error == 0) error = errno;
  if (error == 0 && rename(temporary.c_str(), path.c_str()) != 0) error = errno;
  if (error != 0) unlink(temporary.c_str());
  return error;
}

RedistList ReadRedistList(const std::string& root) {
  RedistList list;
  const auto refused = [&list] {
    list.status = RedistList::Status::kRefused;
    list.entries.clear();
    return list;
  };
  if (root.empty() || root.front() != '/') return refused();
  const std::string folder = root + "/.playden-gog";
  struct stat info {};
  if (lstat(folder.c_str(), &info) != 0) {
    if (errno == ENOENT) return list;
    return refused();
  }
  if (!S_ISDIR(info.st_mode)) return refused();
  const std::string path = root + "/" + kRedistListPath;
  const int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) {
    if (errno == ENOENT) return list;
    return refused();  // ELOOP for a link, or unreadable
  }
  if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) {
    close(fd);
    return refused();
  }
  // One byte past the limit tells a list that is too large.
  std::unique_ptr<char[]> buffer(new char[kMaxRedistListBytes + 1]);
  std::size_t size = 0;
  bool failed = false;
  while (size <= kMaxRedistListBytes) {
    const auto got = read(fd, buffer.get() + size, kMaxRedistListBytes + 1 - size);
    if (got < 0) {
      if (errno == EINTR) continue;
      failed = true;
      break;
    }
    if (got == 0) break;
    size += static_cast<std::size_t>(got);
  }
  close(fd);
  if (failed || size > kMaxRedistListBytes || size == 0 || buffer[size - 1] != '\n') return refused();

  const std::string_view text(buffer.get(), size - 1);  // without the last newline
  std::size_t start = 0;
  bool header = true;
  for (;;) {
    const auto end = text.find('\n', start);
    const auto line = text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
    if (header) {
      if (line != kRedistListHeader) return refused();
      header = false;
    } else {
      auto entry = ParseLine(line);
      if (!entry || list.entries.size() >= kMaxRedistEntries) return refused();
      list.entries.push_back(std::move(*entry));
    }
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  list.status = RedistList::Status::kOk;
  return list;
}

}  // namespace playden::gog
