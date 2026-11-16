#pragma once

#include "runtime/publisher_manifest.h"

#include <string>
#include <vector>

namespace playden::runtime {

// Places a publisher's stage manifest (runtime/publisher_manifest.h) in a game's title folder,
// and takes it away again. Everything is written as the calling user below the title folder,
// opened one folder at a time without following links: a link anywhere on the way (or as a
// target) is refused, never followed. Missing folders are made 0755; existing ones keep their
// mode. Files are replaced through a temporary file and a rename.
// What Playden placed is recorded in the title folder (target -> SHA-256 of the bytes written,
// and the unpacked release's name), so a file an older pack placed is still known as Playden's.
inline constexpr const char* kPublisherStageRecordName = ".playden-publisher-stage";

struct PublisherStageResult {
  bool ok = false;
  std::string message;               // why not (for logs; paths are the manifest's)
  std::vector<std::string> changed;  // title folder paths written, moved or removed, in order
};

// Stages the files for the title with this Steam app id (decimal): each files entry of its arch,
// in order, then the ini (\r\n line ends), and records what was placed. A target that already
// holds the same bytes is left as it is. A different one is replaced in place when it is Playden's:
// the record names its bytes (an older pack's file), or, with no record of it, a keep_original
// already exists. Otherwise it is the game's own file (or its update) and moves to keep_original
// first, replacing an older copy there. runtime_path is the Windows path of the runtime's version folder ("" when
// the manifest has no runtime). A title the manifest doesn't list is refused. pack_dir: the
// unpacked release the sources come from. The credential file is not written here.
PublisherStageResult ApplyPublisherStage(const PublisherStageManifest& manifest, const std::string& pack_dir,
                                         const std::string& title_folder, const std::string& app_id,
                                         const std::string& runtime_path);

// Undoes ApplyPublisherStage: for each files entry in reverse, its keep_original goes back over
// the target, or a target with the pack's bytes (or the bytes the record says Playden placed) is
// removed; then the ini, the credential file and the record are removed. A target the game changed
// since (other bytes) stays, and so does its kept copy.
// Folders stay. With an app id the manifest lists, only entries of its arch are undone.
PublisherStageResult RestorePublisherStage(const PublisherStageManifest& manifest, const std::string& pack_dir,
                                           const std::string& title_folder, const std::string& app_id);

// Undoes what the record in the title folder lists, without a manifest (the pack can't be loaded):
// each recorded target gets its kept original back, or is removed when it still holds the bytes
// Playden placed; then the recorded credential file and the record go.
PublisherStageResult RestorePublisherRecord(const std::string& title_folder);

// Writes the credential file (value and "\n", mode 0600), last, right before Play. value is wiped
// (steam::ZeroizeSecret) whatever the outcome; so is every copy made here.
bool WritePublisherCredential(const PublisherStageManifest& manifest, const std::string& title_folder,
                              std::string& value, std::string* reason);

// Removes the credential file; a missing one is success.
bool RemovePublisherCredential(const PublisherStageManifest& manifest, const std::string& title_folder,
                               std::string* reason);

}  // namespace playden::runtime
