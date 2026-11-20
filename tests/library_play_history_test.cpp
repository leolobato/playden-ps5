// SPDX-License-Identifier: GPL-3.0-or-later
#include "library/play_history.h"

#include <cstdio>
#include <climits>
#include <cstdlib>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

using namespace playden;

namespace {

void Check(bool value, const char* message) {
  if (!value) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
  }
}

void Write(Storage& storage, const std::string& text) {
  Check(Storage(storage).OpenTree().WriteDurable(PlayHistoryStore::kFile, Bytes(text.begin(), text.end())).ok(),
        "write fixture");
}

}  // namespace

int main() {
  // RecordPlay: the later date wins, seconds add up, an earlier play only adds its seconds.
  {
    PlayHistory history;
    Check(RecordPlay(history, GameId::Steam(440), 1000, 60), "a first play is recorded");
    Check(history[GameId::Steam(440)] == PlayRecord{1000, 60}, "first play's date and time");
    Check(RecordPlay(history, GameId::Steam(440), 900, 30), "an earlier play still adds its time");
    Check(history[GameId::Steam(440)] == PlayRecord{1000, 90}, "an earlier play keeps the later date");
    Check(RecordPlay(history, GameId::Steam(440), 2000, 0), "a later Home play moves the date");
    Check(history[GameId::Steam(440)] == PlayRecord{2000, 90}, "a Home play adds no time");
    Check(!RecordPlay(history, GameId::Steam(440), 2000, 0), "the same Home play again changes nothing");
    Check(!RecordPlay(history, GameId{}, 3000, 10), "an invalid game is not recorded");
    Check(!RecordPlay(history, GameId::Gog(7), 0, 10), "a play without a date is not recorded");
    Check(history.size() == 1, "only the valid play is kept");
  }

  char temporary[] = "/tmp/playden-play-history-XXXXXX";
  Check(mkdtemp(temporary), "temporary root");
  // The managed tree refuses a link in the path (macOS /tmp is one).
  char root[PATH_MAX];
  Check(realpath(temporary, root) != nullptr, "temporary root's real path");
  Storage storage(root);
  PlayHistoryStore store(storage);

  // Nothing saved yet: an empty history, not an error.
  auto missing = store.Load();
  Check(missing.ok() && missing.value().empty(), "no file is an empty history");

  // A round trip keeps every store's games.
  PlayHistory saved;
  saved[GameId::Steam(440)] = {1700000000, 3600};
  saved[*ParseKey("gog-1207658924")] = {1700000100, 0};
  saved[*ParseKey("local-0f8fad5b-d9cb-469f-a165-70867728950e")] = {1700000200, 42};
  Check(store.Save(saved).ok(), "save");
  auto loaded = store.Load();
  Check(loaded.ok() && loaded.value() == saved, "round trip");

  // Lines that don't read, a newer Playden's store and a line cut short are skipped; the rest stay.
  Write(storage,
        "version=1\n"
        "440 1700000000 3600\n"
        "epic-0f8f 1700000000 10\n"
        "garbage\n"
        "0440 1 1\n"
        "620 0 5\n"
        "570 -5 5\n"
        "730 1700000300 12");
  loaded = store.Load();
  Check(loaded.ok(), "a damaged line doesn't fail the history");
  Check(loaded.value().size() == 1 && loaded.value().at(GameId::Steam(440)) == PlayRecord{1700000000, 3600},
        "only the lines that read are kept");

  // Another version is a newer Playden's file: the load fails, so it is never overwritten.
  Write(storage, "version=2\n440 1700000000 3600 9\n");
  loaded = store.Load();
  Check(!loaded.ok() && loaded.error() == StorageError::kInvalidData, "another version fails the load");
  // An empty or zero-filled file is a save cut short by a power loss: an empty history the next save replaces.
  Write(storage, "");
  loaded = store.Load();
  Check(loaded.ok() && loaded.value().empty(), "an empty file is an empty history");
  Write(storage, std::string(16, '\0'));
  loaded = store.Load();
  Check(loaded.ok() && loaded.value().empty(), "a zero-filled file is an empty history");

  std::remove((std::string(root) + "/" + PlayHistoryStore::kFile).c_str());
  rmdir(root);
  std::puts("library_play_history: ok");
  return 0;
}
