#pragma once
// release.h - GitHub release metadata, Playden's update manifest (playden-update.json) and the nightly builds list.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: MIT
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "update/version.h"

namespace playden::update {

inline constexpr const char* kUpdateManifestAsset = "playden-update.json";

struct ReleaseAsset {
  std::string name;
  std::string url;     // browser_download_url
  std::uint64_t size = 0;
  std::string sha256;  // from GitHub's "digest" ("sha256:<hex>"); empty when GitHub gave none
};

struct Release {
  std::string tag;
  std::string page;  // html_url: the release's GitHub page, when it is one
  Version version;
  bool draft = false;
  bool prerelease = false;
  std::vector<ReleaseAsset> assets;
  const ReleaseAsset* Find(std::string_view name) const;
};

// The JSON of GET /repos/<owner>/<repo>/releases/latest. The tag must be a version ("v0.3.0").
std::optional<Release> ParseRelease(std::string_view json, std::string* error);
// The JSON of GET /repos/<owner>/<repo>/releases: newest first, releases whose tag isn't a version left out.
std::optional<std::vector<Release>> ParseReleases(std::string_view json, std::string* error);
// The same for a repository whose tags are not versions ("r20261022.1", community recipes): the
// tag only has to be a SafeFolderName of at most 64 characters; version stays empty.
std::optional<Release> ParseReleaseAnyTag(std::string_view json, std::string* error);

struct KitPart {
  std::string kit_id;           // stays the same across revisions ("playden-kit")
  std::string revision;         // "0.13"
  std::string manifest_sha256;  // the kit's manifest.json SHA-256
  std::string asset;            // release asset name
  std::string prefix;           // folder inside the zip ("playden-kit-0.13/playden-kit/")
};

// A kit's version: "<generation>.<revision>" ("0.12"), or a development build of one with "-dev.<YYYYMMDD>.<N>"
// ("0.13-dev.20261023.1"), which comes before that release and after the one before it.
struct KitVersion {
  unsigned generation = 0;
  unsigned revision = 0;
  bool development = false;
  unsigned date = 0;
  unsigned build = 0;

  friend bool operator<(const KitVersion& a, const KitVersion& b) {
    if (a.generation != b.generation) return a.generation < b.generation;
    if (a.revision != b.revision) return a.revision < b.revision;
    if (a.development != b.development) return a.development;
    if (a.date != b.date) return a.date < b.date;
    return a.build < b.build;
  }
};

// Anything else, such as the letters of kits before 0.10 ("h-r9"), is refused.
std::optional<KitVersion> ParseKitVersion(std::string_view text);
// The version in a kit's runtime artifact name, "<wine revision>-kit-<version>" ("f729534-kit-0.12").
std::optional<KitVersion> KitVersionFromArtifact(std::string_view artifact);

struct UpdateManifest {
  Version app_version;
  std::string title_id;
  Version min_from;   // oldest installed version that can update to this one
  std::string app_asset;
  std::string app_prefix;
  KitPart kit;
  std::string notes;  // short plain text
};

// playden-update.json, format 1. Names, folders and prefixes are checked for safety.
std::optional<UpdateManifest> ParseUpdateManifest(std::string_view json, std::string* error);

// builds.playden.club's list of nightly builds (updates.json, made by publish-build.py from builds.json), read when
// the nightly-updates feature and beta updates are on. A file's path is relative to the site: Playden puts the
// site's own address in front, and only downloads from there.
inline constexpr const char* kNightlyFeedFile = "updates.json";

struct NightlyFile {
  std::string path;    // "builds/playden-ps5-0.12.0-nightly-20261121.1.zip"
  std::uint64_t size = 0;
  std::string sha256;
  std::string prefix;  // folder inside the zip
  std::string Name() const { return path.substr(path.rfind('/') + 1); }
};

struct NightlyBuild {
  Version version;  // always a nightly
  Version base;     // the release it was built on, never a nightly
  std::string title_id;
  Version min_from;
  std::string notes;  // short plain text
  NightlyFile app;
  KitPart kit;        // asset is kit_file's name
  NightlyFile kit_file;
};

// updates.json, format 1: {"format":1,"nightlies":[...]}. Builds that aren't complete and safe are left out: a
// version that isn't a nightly, a base that is one, an unsafe path or prefix, a missing SHA-256 or no kit.
std::optional<std::vector<NightlyBuild>> ParseNightlyFeed(std::string_view json, std::string* error);

// "folder/file.zip": SafeFolderName parts joined by "/", at most 256 characters.
bool SafeRelativePath(std::string_view path);

bool SafeFolderName(std::string_view name);
bool IsSha256(std::string_view text);

}  // namespace playden::update
