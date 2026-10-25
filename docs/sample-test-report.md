# Sample game test report

This is what a good [game test report](../../../issues/new?template=test-report.yml) looks like. The issue form asks
for each part below. The example is a game that did not work on kit G, so it includes logs. (A Short Hike works
on kit H.)

---

**Title:** [Test] A Short Hike

### Game

A Short Hike — https://store.steampowered.com/app/1055540

### Result

❌ Broken: starts, but is not playable

### Versions

Pasted from `python3 tools/collect_logs.py --host 192.168.1.50` (game 2 in its list):

| Field | Value |
| --- | --- |
| collected | 2026-10-09 18:20 UTC |
| Playden package | 792850da44ec |
| firmware | 13.60 |
| kit runtime | f29127a-kit-g-r2 |
| kit capabilities | dxvk, game-root-unowned, offline_bottle, per_game_title, title-any-id, wine-shared-sections, wow64_i386 |
| game titles | FAKE78011 (app 1055540) |
| redacted | Steam account name and SteamIDs |

### Where the game is installed

Console storage

### Controls

- [x] DualSense, default (Xbox controller)

### Game settings you changed

None.

### What happened

1. Downloaded the game (about 400 MB) and pressed Play. Preparation took about 2 minutes.
2. The game started. The screen stayed black with the loading spinner for about 4 seconds.
3. The game closed and the PS5 went back to the Home screen. No error message.
4. Started it again from its tile on the Home screen: same result.

I expected the title screen.

### Logs and screenshots

`playden-logs-20261009-182001.zip` (attached)

From `logs/title-FAKE78011.log`, near the end:

```
mono_win32_wait_for_single_object_ex failed with error 0
SleepConditionVariable failed with error 234
Crash!!!
```

### Checks

- [x] kstuff and ShadowMountPlus were loaded when I tested.
- [x] I own this game on Steam.

---

## Tips

- **Report games that work, too.** They tell us what to keep working.
- **One game per report.** If two games fail the same way, open two reports and mention each other.
- **Say how far the game got:** black screen, spinner, logos, menu, in game. This narrows the cause a lot.
- **Copy error lines** from the log into the report if you see them. The full zip is still needed.
- **Check the logs before you post.** The script hides your Steam account name and SteamIDs, but logs can still
  show the names of your games and folders.
