// The order of an uninstall: Cloud saves, then the PS5 title, then the download.
#include "homebrew/uninstall_flow.hpp"

#include <cstdio>
#include <cstdlib>
#include "steam_id.h"

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #x); std::exit(1); } } while (0)

using namespace playden::homebrew;
using Kind = UninstallAction::Kind;

int main() {
  // With Cloud: the sync is issued once, and nothing is removed until it settles.
  UninstallFlow flow;
  CHECK(flow.Begin(SteamId(42), true, true));
  CHECK(!flow.Begin(SteamId(42), false, true));
  CHECK(flow.Step(SteamId(42)) == UninstallStep::kSync);
  UninstallFacts titled;
  titled.has_title = true;
  CHECK(flow.Advance(SteamId(42), titled).kind == Kind::kNone);
  CHECK(flow.TakeSyncRequests() == std::vector<GameId>{SteamId(42)});
  CHECK(flow.TakeSyncRequests().empty());
  // A status from before the sync was sent is not its result.
  CHECK(flow.CloudSettled(SteamId(42), CloudStatus::Synced).kind == Kind::kNone && flow.Step(SteamId(42)) == UninstallStep::kSync);
  flow.SyncDispatched(SteamId(42));
  CHECK(flow.CloudSettled(SteamId(42), CloudStatus::Syncing).kind == Kind::kNone && flow.WaitingForCloud(SteamId(42)));
  CHECK(flow.CloudSettled(SteamId(42), CloudStatus::Synced).kind == Kind::kNone);
  CHECK(flow.Step(SteamId(42)) == UninstallStep::kRemoveTitle);

  // The title is removed when no saves move and the game is not running, with the saves choice.
  titled.saves_moving = true;
  CHECK(flow.Advance(SteamId(42), titled).kind == Kind::kNone);
  titled.saves_moving = false;
  titled.session_active = true;
  CHECK(flow.Advance(SteamId(42), titled).kind == Kind::kNone);
  titled.session_active = false;
  titled.removals_handled = 7;
  auto action = flow.Advance(SteamId(42), titled);
  CHECK(action.kind == Kind::kRemoveTitle && action.delete_saves);
  titled.removing = true;
  CHECK(flow.Advance(SteamId(42), titled).kind == Kind::kNone);
  // Only after the runtime finished and the record is gone does the download go.
  UninstallFacts gone;
  gone.removals_handled = 8;
  CHECK(flow.Advance(SteamId(42), gone).kind == Kind::kRemoveDownload);
  // The uninstall lasts until the files are gone, and the download is asked for once.
  CHECK(flow.Step(SteamId(42)) == UninstallStep::kRemoveDownload && flow.Started(SteamId(42)));
  CHECK(flow.Advance(SteamId(42), gone).kind == Kind::kNone);
  gone.download_gone = true;
  CHECK(flow.Advance(SteamId(42), gone).kind == Kind::kFinished);
  CHECK(!flow.Step(SteamId(42)) && flow.Active().empty());

  // A failed removal stops with the runtime's reason and keeps the download.
  CHECK(flow.Begin(SteamId(43), false, false));
  CHECK(flow.TakeSyncRequests().empty());
  titled = {};
  titled.has_title = true;
  CHECK(flow.Advance(SteamId(43), titled).kind == Kind::kRemoveTitle);
  titled.removals_handled = 1;
  titled.removal_failure = "Close the game before uninstalling it.";
  action = flow.Advance(SteamId(43), titled);
  CHECK(action.kind == Kind::kStop && action.reason.find("Close the game") != std::string::npos);
  // A refusal before the removal began has no removal for Retry to continue.
  CHECK(action.reason.find("Retry") == std::string::npos);
  CHECK(!flow.Step(SteamId(43)));

  // A removal that stopped partway is continued by Retry.
  CHECK(flow.Begin(SteamId(47), false, false));
  titled = {};
  titled.has_title = true;
  CHECK(flow.Advance(SteamId(47), titled).kind == Kind::kRemoveTitle);
  titled.removals_handled = 1;
  titled.removal_started = true;
  titled.removal_failure = "The console has not confirmed that the PS5 title is gone.";
  action = flow.Advance(SteamId(47), titled);
  CHECK(action.kind == Kind::kStop && action.reason.find("Retry on its PS5 title continues") != std::string::npos);

  // A failed removal keeps the download even when the runtime lists no title: its install list
  // is empty when its registry could not be written, while the PS5 title is still there.
  CHECK(flow.Begin(SteamId(49), false, false));
  titled = {};
  titled.has_title = true;
  CHECK(flow.Advance(SteamId(49), titled).kind == Kind::kRemoveTitle);
  UninstallFacts unlisted;
  unlisted.removals_handled = 1;
  unlisted.removal_failure = "The removal could not be recorded: runtime install registry could not be written";
  action = flow.Advance(SteamId(49), unlisted);
  CHECK(action.kind == Kind::kStop && action.reason.find("could not be recorded") != std::string::npos);
  CHECK(!flow.Step(SteamId(49)));

  // A game without a PS5 title goes straight to the download.
  CHECK(flow.Begin(SteamId(44), false, false));
  CHECK(flow.Advance(SteamId(44), UninstallFacts{}).kind == Kind::kRemoveDownload);

  // A Cloud conflict stops and asks; uninstalling again syncs again.
  CHECK(flow.Begin(SteamId(45), false, true));
  flow.TakeSyncRequests();
  flow.SyncDispatched(SteamId(45));
  action = flow.CloudSettled(SteamId(45), CloudStatus::Conflict);
  CHECK(action.kind == Kind::kStop && action.reason.find("Choose which Cloud saves") != std::string::npos);
  CHECK(flow.Begin(SteamId(45), false, true) && flow.Step(SteamId(45)) == UninstallStep::kSync);
  flow.TakeSyncRequests();
  flow.SyncDispatched(SteamId(45));
  // A failed sync stops; the next uninstall goes on without syncing.
  action = flow.CloudSettled(SteamId(45), CloudStatus::Failed);
  CHECK(action.kind == Kind::kStop && action.reason.find("without syncing") != std::string::npos);
  CHECK(flow.Begin(SteamId(45), false, true) && flow.Step(SteamId(45)) == UninstallStep::kRemoveTitle);
  CHECK(flow.TakeSyncRequests().empty());

  // Not set up (#1395): the check waits for a bottle, so no choice is asked for. It says why once, and the next
  // uninstall goes on without waiting on Cloud.
  CHECK(flow.Begin(SteamId(1395), true, true));
  flow.TakeSyncRequests();
  flow.SyncDispatched(SteamId(1395));
  action = flow.CloudSettled(SteamId(1395), CloudStatus::Waiting);
  CHECK(action.kind == Kind::kStop && action.reason.find("Choose which Cloud saves") == std::string::npos &&
        action.reason.find("set up") != std::string::npos && action.reason.find("Uninstall again") != std::string::npos);
  CHECK(flow.Begin(SteamId(1395), true, true) && flow.Step(SteamId(1395)) == UninstallStep::kRemoveTitle);
  CHECK(flow.TakeSyncRequests().empty());
  flow.Abandon(SteamId(1395));
  // That pass is used once, also by an uninstall without Cloud: a later one syncs first again.
  CHECK(flow.Begin(SteamId(1395), true, true));
  flow.TakeSyncRequests();
  flow.SyncDispatched(SteamId(1395));
  CHECK(flow.CloudSettled(SteamId(1395), CloudStatus::Waiting).kind == Kind::kStop);
  CHECK(flow.Begin(SteamId(1395), true, false) && flow.Step(SteamId(1395)) == UninstallStep::kRemoveTitle);
  flow.Abandon(SteamId(1395));
  CHECK(flow.Begin(SteamId(1395), true, true) && flow.Step(SteamId(1395)) == UninstallStep::kSync);
  flow.Abandon(SteamId(1395));

  // A running title stops a waiting sync; a game with nothing to sync continues.
  CHECK(flow.Begin(SteamId(46), false, true));
  CHECK(flow.CloudRefused(SteamId(46)).kind == Kind::kNone);  // not issued yet: nothing to refuse
  flow.TakeSyncRequests();
  CHECK(flow.CloudRefused(SteamId(46)).kind == Kind::kStop && !flow.Step(SteamId(46)));
  CHECK(flow.Begin(SteamId(47), false, true));
  flow.TakeSyncRequests();
  flow.CloudSkipped(SteamId(47));
  CHECK(flow.Step(SteamId(47)) == UninstallStep::kRemoveTitle);
  // Unsupported Cloud counts as done.
  CHECK(flow.Begin(SteamId(48), false, true));
  flow.TakeSyncRequests();
  flow.SyncDispatched(SteamId(48));
  CHECK(flow.CloudSettled(SteamId(48), CloudStatus::Unsupported).kind == Kind::kNone && flow.Step(SteamId(48)) == UninstallStep::kRemoveTitle);

  // Two uninstalls: one PS5 title removal at a time, because the runtime counts removals for all games together.
  CHECK(flow.Begin(SteamId(60), false, false) && flow.Begin(SteamId(61), false, false));
  UninstallFacts both;
  both.has_title = true;
  both.removals_handled = 5;
  CHECK(flow.Advance(SteamId(60), both).kind == Kind::kRemoveTitle);
  CHECK(flow.Advance(SteamId(61), both).kind == Kind::kNone && flow.Step(SteamId(61)) == UninstallStep::kRemoveTitle);
  // The first removal finishes while the second game is still queued: it must not read that as its own result.
  UninstallFacts first_done;
  first_done.removals_handled = 6;
  both.removals_handled = 6;
  CHECK(flow.Advance(SteamId(61), both).kind == Kind::kNone);
  CHECK(flow.Advance(SteamId(60), first_done).kind == Kind::kRemoveDownload);
  // Now the second removal goes, and only its own completion moves it on.
  action = flow.Advance(SteamId(61), both);
  CHECK(action.kind == Kind::kRemoveTitle);
  CHECK(flow.Advance(SteamId(61), both).kind == Kind::kNone && flow.Step(SteamId(61)) == UninstallStep::kRemoveTitle);
  both.removing = true;
  CHECK(flow.Advance(SteamId(61), both).kind == Kind::kNone);
  UninstallFacts second_done;
  second_done.removals_handled = 7;
  CHECK(flow.Advance(SteamId(61), second_done).kind == Kind::kRemoveDownload);
  // A download that could not be deleted ends the uninstall outside the flow.
  flow.Abandon(SteamId(60));
  flow.Abandon(SteamId(61));
  CHECK(!flow.Step(SteamId(60)) && !flow.Step(SteamId(61)));

  // A stopped removal frees the slot for the waiting game.
  CHECK(flow.Begin(SteamId(62), false, false) && flow.Begin(SteamId(63), false, false));
  UninstallFacts stuck;
  stuck.has_title = true;
  CHECK(flow.Advance(SteamId(62), stuck).kind == Kind::kRemoveTitle);
  CHECK(flow.Advance(SteamId(63), stuck).kind == Kind::kNone);
  stuck.removals_handled = 1;
  CHECK(flow.Advance(SteamId(62), stuck).kind == Kind::kStop);
  stuck.removals_handled = 0;
  CHECK(flow.Advance(SteamId(63), stuck).kind == Kind::kRemoveTitle);
  UninstallFacts done;
  done.removals_handled = 1;
  CHECK(flow.Advance(SteamId(63), done).kind == Kind::kRemoveDownload);

  // One game's Cloud conflict stops only that game; the other goes on to its removal.
  CHECK(flow.Begin(SteamId(64), false, true) && flow.Begin(SteamId(65), false, true));
  CHECK(flow.TakeSyncRequests() == std::vector<GameId>({SteamId(64)}));
  flow.SyncDispatched(SteamId(64));
  CHECK(flow.CloudSettled(SteamId(64), CloudStatus::Conflict).kind == Kind::kStop);
  CHECK(!flow.Step(SteamId(64)) && flow.Step(SteamId(65)) == UninstallStep::kSync);
  CHECK(flow.TakeSyncRequests() == std::vector<GameId>({SteamId(65)}));
  flow.SyncDispatched(SteamId(65));
  CHECK(flow.CloudSettled(SteamId(65), CloudStatus::Synced).kind == Kind::kNone);
  UninstallFacts after_sync;
  after_sync.has_title = true;
  CHECK(flow.Advance(SteamId(65), after_sync).kind == Kind::kRemoveTitle);
  after_sync = {};
  after_sync.removals_handled = 1;
  CHECK(flow.Advance(SteamId(65), after_sync).kind == Kind::kRemoveDownload && flow.Step(SteamId(65)) == UninstallStep::kRemoveDownload);

  // First in, first out: uninstalls go in the order they were asked for, whatever their app IDs,
  // and only the one at work says it has started.
  UninstallFlow fifo;
  for (const auto app : {30u, 10u, 20u}) CHECK(fifo.Begin(SteamId(app), false, true));
  CHECK(fifo.Active() == std::vector<GameId>({SteamId(30), SteamId(10), SteamId(20)}));
  // One sync at a time, in that order: the next goes out once the one before settles.
  CHECK(fifo.TakeSyncRequests() == std::vector<GameId>({SteamId(30)}));
  CHECK(fifo.TakeSyncRequests().empty());
  CHECK(!fifo.Started(SteamId(30)));  // issued, not yet sent to Cloud
  fifo.SyncDispatched(SteamId(30));
  CHECK(fifo.Started(SteamId(30)) && !fifo.Started(SteamId(10)) && !fifo.Started(SteamId(20)));
  CHECK(fifo.CloudSettled(SteamId(30), CloudStatus::Synced).kind == Kind::kNone);
  CHECK(!fifo.Started(SteamId(30)));  // synced, waiting for its title removal
  CHECK(fifo.TakeSyncRequests() == std::vector<GameId>({SteamId(10)}));
  fifo.SyncDispatched(SteamId(10));
  CHECK(fifo.CloudSettled(SteamId(10), CloudStatus::UpToDate).kind == Kind::kNone);
  CHECK(fifo.TakeSyncRequests() == std::vector<GameId>({SteamId(20)}));
  fifo.CloudSkipped(SteamId(20));
  CHECK(fifo.TakeSyncRequests().empty());
  // Title removals follow the same order: walking Active() hands the one slot to the first.
  UninstallFacts listed;
  listed.has_title = true;
  std::vector<GameId> removed;
  for (const auto id : fifo.Active())
    if (fifo.Advance(id, listed).kind == Kind::kRemoveTitle) removed.push_back(id);
  CHECK(removed == std::vector<GameId>({SteamId(30)}));
  CHECK(fifo.Started(SteamId(30)) && !fifo.Started(SteamId(10)) && !fifo.Started(SteamId(20)));
  UninstallFacts removed_one;
  removed_one.removals_handled = 1;
  CHECK(fifo.Advance(SteamId(30), removed_one).kind == Kind::kRemoveDownload);
  removed.clear();
  listed.removals_handled = 1;
  for (const auto id : fifo.Active())
    if (fifo.Advance(id, listed).kind == Kind::kRemoveTitle) removed.push_back(id);
  CHECK(removed == std::vector<GameId>({SteamId(10)}));
  // A game asked for again after a stop goes to the back of the line.
  CHECK(fifo.CloudRefused(SteamId(20)).kind == Kind::kNone);
  fifo.Abandon(SteamId(20));
  CHECK(fifo.Begin(SteamId(20), false, false) && fifo.Begin(SteamId(5), false, false));
  CHECK(fifo.Active() == std::vector<GameId>({SteamId(30), SteamId(10), SteamId(20), SteamId(5)}));

  // The install service deletes one game at a time, in its order: only the first of these uninstalls
  // it lists is at work; one not listed (an imported game leaving the library) is at work too.
  UninstallFlow deleting;
  for (const auto app : {70u, 71u, 72u}) {
    CHECK(deleting.Begin(SteamId(app), false, false));
    CHECK(deleting.Advance(SteamId(app), UninstallFacts{}).kind == Kind::kRemoveDownload);
  }
  const std::vector<GameId> queued{SteamId(99), SteamId(71), SteamId(70)};
  CHECK(deleting.Started(SteamId(71), queued) && !deleting.Started(SteamId(70), queued));
  CHECK(deleting.Started(SteamId(72), queued));
  CHECK(deleting.Started(SteamId(70)));

  // A drive that is not connected stops the uninstall before its PS5 title is removed...
  UninstallFlow absent;
  UninstallFacts unplugged;
  unplugged.has_title = true;
  unplugged.drive_absent = true;
  CHECK(absent.Begin(SteamId(80), false, false));
  action = absent.Advance(SteamId(80), unplugged);
  CHECK(action.kind == Kind::kStop && action.reason == "Its drive is not connected. Connect it, then uninstall again.");
  CHECK(!absent.Step(SteamId(80)));
  // ...lets a removal already sent finish, then stops before asking for the download.
  CHECK(absent.Begin(SteamId(81), false, false));
  unplugged.drive_absent = false;
  CHECK(absent.Advance(SteamId(81), unplugged).kind == Kind::kRemoveTitle);
  unplugged.drive_absent = true;
  unplugged.removing = true;
  CHECK(absent.Advance(SteamId(81), unplugged).kind == Kind::kNone && absent.Step(SteamId(81)) == UninstallStep::kRemoveTitle);
  UninstallFacts removed_unplugged;
  removed_unplugged.removals_handled = 1;
  removed_unplugged.drive_absent = true;
  CHECK(absent.Advance(SteamId(81), removed_unplugged).kind == Kind::kStop && !absent.Step(SteamId(81)));

  // A sync Cloud did not take stops that uninstall, and the next one's sync goes out.
  UninstallFlow refused;
  CHECK(refused.Begin(SteamId(90), false, true) && refused.Begin(SteamId(91), false, true));
  CHECK(refused.SyncRefused(SteamId(90)).kind == Kind::kNone);  // not issued yet
  CHECK(refused.TakeSyncRequests() == std::vector<GameId>({SteamId(90)}));
  refused.SyncDispatched(SteamId(90));
  action = refused.SyncRefused(SteamId(90));
  CHECK(action.kind == Kind::kStop && action.reason.find("Uninstall again") != std::string::npos && !refused.Step(SteamId(90)));
  CHECK(refused.TakeSyncRequests() == std::vector<GameId>({SteamId(91)}));
  std::puts("uninstall flow tests passed");
  return 0;
}
