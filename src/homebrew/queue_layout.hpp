#pragma once
// queue_layout.hpp - What the Queue screen shows: downloads, and preparations and uninstalls, each section's rows and counts.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: MIT
#include "homebrew/simulation.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace playden::homebrew {
inline constexpr const char* kQueueDownloads = "downloads";
inline constexpr const char* kQueuePreparation = "preparation";
inline constexpr const char* kQueueCollapsed = "none";  // the player closed the open section

// One game in a section. Downloads point at their job; preparations and uninstalls carry what their row says.
struct QueueEntry {
    // The row's focus key: job-<id>, prep-<id>, prep-attn-<id>, prep-done-<id>-<time>; uninstalls
    // prep-unin-<id>, prep-unin-attn-<id>, prep-unin-done-<id>-<time>.
    std::string key;
    GameId game_id;
    const Job* job = nullptr;
    std::string title;
    std::string status;  // the right-hand tag: "Up next", "Queued", "Waiting for the PS5", "Ready", "Uninstalling"
    std::string note;    // beside the title: why it waits or failed
    std::string tone;    // good, busy, attention, problem, quiet
    int position = 0;    // waiting preparations, from 1
    bool uninstall = false;
    bool leftover = false;
    bool completed = false;
    std::int64_t finished_at = 0;  // completed entries (Unix seconds), 0 unknown
};

struct QueueSection {
    std::string id;    // kQueueDownloads or kQueuePreparation
    std::string name;  // "Downloads", "Preparation"
    // The running download or preparation, shown in the card; a paused or failed download at the front
    // of the queue takes the card too, as Downloads always showed it. With no preparation running, the
    // first uninstall at work takes Preparation's card.
    std::optional<QueueEntry> active;
    // Unfinished entries after the card (running, waiting, then needing attention), then completed ones, newest first.
    std::vector<QueueEntry> rows;
    std::size_t running = 0;    // downloading, preparing or uninstalling now
    std::size_t copying = 0;    // of those running, imported games copied into Playden
    std::size_t deleting = 0;  // leftover PS5 titles
    std::size_t uninstalling = 0;  // of those running, uninstalls
    std::size_t waiting = 0;    // queued to run, card included when it only waits
    std::size_t attention = 0;  // paused, failed or blocked: not ordinary waiting work
    std::size_t completed = 0;
    std::size_t updates = 0;    // a downloaded Playden update waiting for a restart
    bool expanded = false;
    std::string summary;  // the heading's right-hand words

    [[nodiscard]] bool empty() const { return !active && rows.empty(); }
    [[nodiscard]] bool unfinished() const { return active || rows.size() > completed; }
};

struct QueueLayout {
    QueueSection downloads;
    QueueSection preparation;
    std::string summary;  // the page's subtitle: "1 downloading · 1 preparing · 4 waiting"
    [[nodiscard]] bool nothing() const { return downloads.empty() && preparation.empty(); }
};

// The section shown expanded: the one whose row has focus, else the player's choice when it still
// has something to show (none when they closed it), else Preparation when games wait there, Downloads with unfinished work,
// then whichever has anything.
QueueLayout BuildQueueLayout(const State& state);
// The Queue tab's badge: unfinished downloads, and preparations and uninstalls running or waiting.
std::size_t QueuePending(const State& state);
// The section a Queue focus key belongs to (a heading, card or row); empty for other keys.
std::string QueueSectionOf(const std::string& key);
}  // namespace playden::homebrew
