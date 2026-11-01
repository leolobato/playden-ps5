// runtime_kit_catalog_test.cpp - Which installed kit a pin names, and which parked copies stay.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/kit_catalog.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <unistd.h>
#include <vector>

using playden::runtime::IsConcreteKitPin;
using playden::runtime::ParkedKitVersion;
using playden::runtime::PruneUnpinnedKits;
using playden::runtime::ResolveGameKit;
using playden::runtime::ResolveKitPin;
using playden::runtime::ResolvedKitKind;

#define CHECK(value) do { if (!(value)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #value); std::exit(1); } } while (false)

int main() {
  const std::vector<std::string> parked = {"0.15", "0.16"};
  const auto saved = ResolveKitPin("0.16", true, "0.17", parked);
  CHECK(saved.kind == ResolvedKitKind::kVersion && saved.version == "0.16" && !saved.fell_back);
  const auto missing = ResolveKitPin("0.14", true, "0.17", parked);
  CHECK(missing.kind == ResolvedKitKind::kLatestSlot && missing.version == "0.17" && missing.fell_back);
  const auto follows = ResolveGameKit("", "0.15", true, "0.17", parked);
  CHECK(follows.kind == ResolvedKitKind::kVersion && follows.version == "0.15" && !follows.fell_back);
  const auto latest = ResolveKitPin("latest", true, "0.17", parked);
  CHECK(latest.kind == ResolvedKitKind::kLatestSlot && latest.version == "0.17" && !latest.fell_back);
  const auto empty = ResolveKitPin("", true, "0.17", parked);
  CHECK(empty.kind == ResolvedKitKind::kLatestSlot && !empty.fell_back);
  const auto same = ResolveKitPin("0.17", true, "0.17", parked);
  CHECK(same.kind == ResolvedKitKind::kLatestSlot && same.version == "0.17" && !same.fell_back);
  const auto parked_only = ResolveKitPin("0.16", false, "", parked);
  CHECK(parked_only.kind == ResolvedKitKind::kVersion && parked_only.version == "0.16" && !parked_only.fell_back);
  const auto neither = ResolveKitPin("0.14", false, "", parked);
  CHECK(neither.kind == ResolvedKitKind::kUnavailable && neither.fell_back);
  const auto no_latest = ResolveKitPin("latest", false, "", parked);
  CHECK(no_latest.kind == ResolvedKitKind::kUnavailable && !no_latest.fell_back);
  CHECK(IsConcreteKitPin("0.13-dev.20261023.1") && !IsConcreteKitPin("latest") && !IsConcreteKitPin(""));
  std::string version;
  CHECK(ParkedKitVersion("playden-kit", "playden-kit-0.16", &version) && version == "0.16");
  CHECK(!ParkedKitVersion("playden-kit", "playden-kit", &version));
  CHECK(!ParkedKitVersion("playden-kit", ".replaced-playden-kit-1", &version));

  char pattern[] = "/tmp/playden-kit-catalog-XXXXXX";
  const auto made = mkdtemp(pattern);
  CHECK(made != nullptr);
  const std::filesystem::path root = made;
  std::filesystem::create_directories(root / "playden-kit");
  std::filesystem::create_directories(root / "playden-kit-0.15");
  std::filesystem::create_directories(root / "playden-kit-0.16");
  std::filesystem::create_directories(root / "playden-kit-0.14");
  std::filesystem::create_directories(root / ".replaced-old");
  const auto removed = PruneUnpinnedKits(root.string(), "playden-kit", {"0.15"}, [](const std::string& path) {
    return path.find("0.14") == std::string::npos;
  });
  CHECK(removed.size() == 1 && removed[0] == (root / "playden-kit-0.16").string());
  CHECK(std::filesystem::exists(root / "playden-kit"));
  CHECK(std::filesystem::exists(root / "playden-kit-0.15"));
  CHECK(std::filesystem::exists(root / "playden-kit-0.14"));
  CHECK(std::filesystem::exists(root / ".replaced-old"));
  CHECK(!std::filesystem::exists(root / "playden-kit-0.16"));
  std::filesystem::remove_all(root);
  return 0;
}
