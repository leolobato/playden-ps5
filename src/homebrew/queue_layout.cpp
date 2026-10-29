// queue_layout.cpp - What the Queue screen shows: downloads, and preparations and uninstalls, each section's rows and counts.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: MIT
#include "homebrew/queue_layout.hpp"

#include <algorithm>

namespace playden::homebrew {
namespace {
std::string game_title(const State& state, GameId id) {
    for (const auto& game : state.games)
        if (game.id == id && !game.title.empty()) return game.title;
    return "Steam app " + ToKey(id);
}

std::string need_attention(std::size_t count) {
    return std::to_string(count) + (count == 1 ? " needs attention" : " need attention");
}

void join(std::string& text, const std::string& part) {
    if (part.empty()) return;
    if (!text.empty()) text += " · ";
    text += part;
}

bool running(const Job& job) {
    return job.state == JobState::Downloading || job.state == JobState::Verifying || job.state == JobState::Pausing;
}

QueueSection downloads(const State& state) {
    QueueSection section{kQueueDownloads, "Downloads"};
    std::vector<const Job*> jobs;
    for (const auto& job : state.jobs)
        if (job.state != JobState::Cancelled) jobs.push_back(&job);
    std::stable_sort(jobs.begin(), jobs.end(), [](const Job* left, const Job* right) { return left->queue_order < right->queue_order; });
    // The card shows the running transfer, or else the first unfinished one in queue order (a failed or
    // paused download does not jump ahead of the queue).
    auto card = std::find_if(jobs.begin(), jobs.end(), [](const Job* job) { return running(*job); });
    if (card == jobs.end())
        card = std::find_if(jobs.begin(), jobs.end(), [](const Job* job) { return job->state != JobState::Downloaded; });
    const auto entry = [&state](const Job& job) {
        QueueEntry item;
        item.key = "job-" + job.id;
        item.game_id = job.game_id;
        item.job = &job;
        item.title = !job.title.empty() ? job.title : game_title(state, job.game_id);
        item.note = job.failure;
        item.completed = job.state == JobState::Downloaded && job.kind == "install";
        item.finished_at = job.finished_at;
        return item;
    };
    std::vector<const Job*> finished;
    std::size_t paused = 0, failed = 0;
    for (const auto* job : jobs) {
        if (running(*job)) {
            ++section.running;
            if (job->game_id.store == Store::kLocal) ++section.copying;
        }
        else if (job->state == JobState::Queued) ++section.waiting;
        else if (job->state == JobState::Paused || job->state == JobState::Interrupted) ++paused;
        else if (job->state == JobState::Failed) ++failed;
        else if (job->state == JobState::Downloaded && job->kind != "install") ++section.updates;
        if (card != jobs.end() && job == *card) section.active = entry(*job);
        // A staged Playden update is a Downloaded job that still asks for a restart: it stays with the work.
        else if (job->state == JobState::Downloaded && job->kind == "install") finished.push_back(job);
        else section.rows.push_back(entry(*job));
    }
    std::stable_sort(finished.begin(), finished.end(), [](const Job* left, const Job* right) {
        return left->finished_at > right->finished_at;
    });
    for (const auto* job : finished) section.rows.push_back(entry(*job));
    section.attention = paused + failed;
    section.completed = finished.size();
    return section;
}

std::string downloads_words(const QueueSection& section, std::size_t paused, std::size_t failed) {
    std::string text;
    if (section.running > section.copying) join(text, std::to_string(section.running - section.copying) + " downloading");
    if (section.copying) join(text, std::to_string(section.copying) + " copying");
    if (section.waiting) join(text, std::to_string(section.waiting) + " waiting");
    if (paused) join(text, std::to_string(paused) + " paused");
    if (failed) join(text, std::to_string(failed) + " failed");
    if (section.updates) join(text, "Playden update ready");
    return text;
}

QueueSection preparations(const State& state) {
    QueueSection section{kQueuePreparation, "Preparation"};
    const auto& queue = state.preparation_queue;
    const auto runtime = [&state](GameId id) -> const LiveRuntimeGame* {
        const auto found = state.runtime_games.find(id);
        return found == state.runtime_games.end() ? nullptr : &found->second;
    };
    std::string active_title;
    if (queue.active) {
        QueueEntry item;
        item.key = "prep-active-" + ToKey(*queue.active);
        item.game_id = *queue.active;
        item.title = active_title = game_title(state, *queue.active);
        item.status = "Preparing";
        item.tone = "busy";
        // The actual step, never a made-up percentage.
        if (const auto* game = runtime(*queue.active)) item.note = game->detail;
        section.active = item;
        section.running = 1;
    }
    // Uninstalls run apart from preparations, first in, first out. The first at work takes the card when
    // no preparation does; the others at work come first after it, and those waiting after the waiting
    // preparations. They are not numbered: their order is not the preparation queue's.
    std::vector<QueueEntry> uninstalls_running, uninstalls_waiting;
    for (const auto& uninstall : queue.uninstalls) {
        QueueEntry item;
        item.key = "prep-unin-" + ToKey(uninstall.game_id);
        item.game_id = uninstall.game_id;
        item.title = game_title(state, uninstall.game_id);
        item.uninstall = true;
        if (uninstall.step.empty()) {
            item.status = "Waiting to uninstall";
            item.tone = "quiet";
            uninstalls_waiting.push_back(std::move(item));
            ++section.waiting;
            continue;
        }
        item.status = "Uninstalling";
        item.note = uninstall.step;
        item.tone = "busy";
        ++section.running;
        ++section.uninstalling;
        if (!section.active) section.active = std::move(item);
        else uninstalls_running.push_back(std::move(item));
    }
    section.rows = std::move(uninstalls_running);
    for (const auto& deletion : state.leftover_deletions) {
        QueueEntry item;
        item.key = "prep-leftover-" + deletion.title_id;
        item.title = deletion.name.empty() ? deletion.title_id : deletion.name;
        item.leftover = true;
        item.note = deletion.failure.empty() ? deletion.step : deletion.failure;
        if (!deletion.failure.empty()) {
            item.status = "Deletion stopped";
            item.tone = "attention";
            ++section.attention;
        } else if (deletion.step.empty()) {
            item.status = "Waiting to delete";
            item.note = deletion.title_id;
            item.tone = "quiet";
            ++section.waiting;
        } else {
            item.status = "Deleting title";
            item.tone = "busy";
            ++section.running;
            ++section.deleting;
            if (!section.active) { section.active = std::move(item); continue; }
        }
        section.rows.push_back(std::move(item));
    }
    int position = 0;
    for (const auto id : queue.waiting) {
        if (queue.active && id == *queue.active) continue;
        QueueEntry item;
        item.key = "prep-" + ToKey(id);
        item.game_id = id;
        item.title = game_title(state, id);
        item.position = ++position;
        item.status = position == 1 ? "Up next" : "Queued";
        item.tone = position == 1 ? "busy" : "quiet";
        item.note = position == 1 ? (active_title.empty() ? "Starts next" : "Starts after " + active_title)
                                  : "Waiting for preparation";
        section.rows.push_back(std::move(item));
        ++section.waiting;
    }
    for (auto& item : uninstalls_waiting) section.rows.push_back(std::move(item));
    // Games whose preparation needs the player: it failed, stopped part way or waits for the PS5. A game
    // being uninstalled is listed as such only.
    const auto uninstalling = [&queue](GameId id) {
        return std::any_of(queue.uninstalls.begin(), queue.uninstalls.end(), [id](const LiveUninstall& item) { return item.game_id == id; });
    };
    const auto listed = [&](GameId id) {
        return (queue.active && *queue.active == id) || uninstalling(id) ||
               std::find(queue.waiting.begin(), queue.waiting.end(), id) != queue.waiting.end();
    };
    for (const auto& [id, game] : state.runtime_games) {
        if (listed(id)) continue;
        QueueEntry item;
        item.key = "prep-attn-" + ToKey(id);
        item.game_id = id;
        item.title = game_title(state, id);
        item.note = game.detail;
        item.tone = "attention";
        if (game.status == "Waiting for the PS5") {
            item.status = "Waiting for the PS5";
            item.note = "Quit Playden and the PS5 adds it to Home";
        } else if (game.status == "Registration pending") {
            item.status = "Registration pending";
        } else if (game.status == "Needs recovery") {
            item.status = "Interrupted";
        } else if (game.status == "Failed") {
            item.status = "Couldn't prepare";
            item.tone = "problem";
        } else if (const auto refused = queue.refused.find(id);
                   refused != queue.refused.end() && !game.ready && !game.prepared && !game.awaiting_registration) {
            item.status = "Couldn't prepare";
            item.note = refused->second;
            item.tone = "problem";
        } else {
            continue;
        }
        section.rows.push_back(std::move(item));
        ++section.attention;
    }
    // Uninstalls that stopped: the reason, until the game goes again or the player dismisses it.
    for (const auto& ended : state.ended_uninstalls) {
        if (ended.stopped.empty() || uninstalling(ended.game_id)) continue;
        QueueEntry item;
        item.key = "prep-unin-attn-" + ToKey(ended.game_id);
        item.game_id = ended.game_id;
        item.title = !ended.title.empty() ? ended.title : game_title(state, ended.game_id);
        item.status = "Uninstall stopped";
        item.note = ended.stopped;
        item.tone = "attention";
        item.uninstall = true;
        section.rows.push_back(std::move(item));
        ++section.attention;
    }
    // Finished preparations and uninstalls together, newest first.
    std::vector<QueueEntry> finished;
    for (const auto& done : queue.completed) {
        QueueEntry item;
        item.key = "prep-done-" + ToKey(done.game_id) + "-" + std::to_string(done.finished_at);
        item.game_id = done.game_id;
        item.title = game_title(state, done.game_id);
        item.status = "Ready";
        item.tone = "good";
        item.completed = true;
        item.finished_at = done.finished_at;
        finished.push_back(std::move(item));
    }
    for (const auto& ended : state.ended_uninstalls) {
        // A game shows once: while it is being uninstalled again, not also as done.
        if (!ended.stopped.empty() || uninstalling(ended.game_id)) continue;
        QueueEntry item;
        item.key = "prep-unin-done-" + ToKey(ended.game_id) + "-" + std::to_string(ended.finished_at);
        item.game_id = ended.game_id;
        item.title = !ended.title.empty() ? ended.title : game_title(state, ended.game_id);
        item.status = "Uninstalled";
        item.tone = "good";
        item.uninstall = true;
        item.completed = true;
        item.finished_at = ended.finished_at;
        finished.push_back(std::move(item));
    }
    std::stable_sort(finished.begin(), finished.end(),
                     [](const QueueEntry& left, const QueueEntry& right) { return left.finished_at > right.finished_at; });
    section.completed = finished.size();
    for (auto& item : finished) section.rows.push_back(std::move(item));
    return section;
}

// The heading's words: what runs and waits when expanded, how many more when collapsed; both keep
// what needs the player and the completed count.
void summarize(QueueSection& section, const std::string& expanded_words, const std::string& attention_words,
               const char* idle) {
    std::string text;
    // Games after the card waiting their turn; what needs the player is counted apart.
    const std::size_t more = static_cast<std::size_t>(std::count_if(section.rows.begin(), section.rows.end(), [](const QueueEntry& row) {
        if (row.job) return row.job->state == JobState::Queued || running(*row.job);
        return row.position > 0 || ((row.uninstall || row.leftover) && !row.completed && row.tone != "attention");
    }));
    if (section.expanded) {
        text = expanded_words;
        if (section.running && !section.waiting && !section.attention) join(text, "No games waiting");
    } else if (section.active) {
        text = more == 0 ? "No other games queued"
             : more == 1 ? "1 more game queued" : std::to_string(more) + " more games queued";
        join(text, attention_words);
    } else {
        // Nothing runs: what waits, needs attention or is ready.
        text = expanded_words;
    }
    if (section.completed) join(text, std::to_string(section.completed) + " completed");
    if (text.empty()) text = section.unfinished() ? expanded_words : idle;
    section.summary = text;
}
}  // namespace

std::size_t QueuePending(const State& state) {
    auto pending = static_cast<std::size_t>(std::count_if(state.jobs.begin(), state.jobs.end(), [](const Job& job) {
        return job.state != JobState::Cancelled && job.state != JobState::Downloaded && job.stage != JobStage::Uninstall;
    }));
    const auto& preparation = state.preparation_queue;
    pending += std::count_if(state.leftover_deletions.begin(), state.leftover_deletions.end(),
                             [](const LiveLeftoverDelete& deletion) { return deletion.failure.empty(); });
    return pending + preparation.waiting.size() + (preparation.active ? 1U : 0U) + preparation.uninstalls.size();
}

std::string QueueSectionOf(const std::string& key) {
    if (key == "queue-downloads" || key.rfind("job-", 0) == 0) return kQueueDownloads;
    if (key == "queue-preparation" || key.rfind("prep-", 0) == 0) return kQueuePreparation;
    return {};
}

QueueLayout BuildQueueLayout(const State& state) {
    QueueLayout layout{downloads(state), preparations(state)};
    auto& download = layout.downloads;
    auto& prepare = layout.preparation;
    // A row (not a card: it shows either way) with focus keeps its section open.
    std::string open;
    const auto& focus = state.navigation.modals.empty() ? state.navigation.focus_key
                                                        : state.navigation.modals.front().opener_focus_key;
    for (const auto* section : {&download, &prepare})
        for (const auto& row : section->rows)
            if (row.key == focus) open = section->id;
    const auto section = [&](const std::string& id) -> QueueSection& { return id == kQueueDownloads ? download : prepare; };
    if (open.empty() && !state.queue_expanded.empty() && !section(state.queue_expanded).empty()) open = state.queue_expanded;
    if (open.empty() && state.queue_expanded != kQueueCollapsed) {
        if (prepare.waiting > 0) open = kQueuePreparation;
        else if (download.unfinished()) open = kQueueDownloads;
        else if (prepare.unfinished()) open = kQueuePreparation;
        else if (!download.empty()) open = kQueueDownloads;
        else if (!prepare.empty()) open = kQueuePreparation;
    }
    download.expanded = !download.empty() && open == kQueueDownloads;
    prepare.expanded = !prepare.empty() && open == kQueuePreparation;

    std::size_t paused = 0, failed = 0;
    for (const auto& job : state.jobs) {
        if (job.state == JobState::Paused || job.state == JobState::Interrupted) ++paused;
        if (job.state == JobState::Failed) ++failed;
    }
    std::string download_attention;
    if (paused) join(download_attention, std::to_string(paused) + " paused");
    if (failed) join(download_attention, std::to_string(failed) + " failed");
    summarize(download, downloads_words(download, paused, failed), download_attention, "No downloads");
    std::string prepare_words, prepare_attention;
    if (prepare.running > prepare.uninstalling + prepare.deleting) join(prepare_words, "1 preparing");
    if (prepare.deleting) join(prepare_words, std::to_string(prepare.deleting) + " deleting titles");
    if (prepare.uninstalling) join(prepare_words, std::to_string(prepare.uninstalling) + " uninstalling");
    if (prepare.waiting) join(prepare_words, std::to_string(prepare.waiting) + " waiting");
    if (prepare.attention) join(prepare_attention, need_attention(prepare.attention));
    join(prepare_words, prepare_attention);
    summarize(prepare, prepare_words, prepare_attention, "No preparation queued");

    std::string summary;
    if (download.running > download.copying) join(summary, std::to_string(download.running - download.copying) + " downloading");
    if (download.copying) join(summary, std::to_string(download.copying) + " copying");
    if (prepare.running > prepare.uninstalling + prepare.deleting) join(summary, "1 preparing");
    if (prepare.deleting) join(summary, std::to_string(prepare.deleting) + " deleting titles");
    if (prepare.uninstalling) join(summary, std::to_string(prepare.uninstalling) + " uninstalling");
    if (download.waiting + prepare.waiting) join(summary, std::to_string(download.waiting + prepare.waiting) + " waiting");
    if (download.attention + prepare.attention)
        join(summary, need_attention(download.attention + prepare.attention));
    if (download.updates) join(summary, "Playden update ready");
    if (summary.empty() && !layout.nothing())
        summary = "No active work · " + std::to_string(download.completed + prepare.completed) + " completed";
    layout.summary = summary;
    return layout;
}
}  // namespace playden::homebrew
