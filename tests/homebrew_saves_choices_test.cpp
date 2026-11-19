// SPDX-License-Identifier: GPL-3.0-or-later
// #1103: the Saves tab, a game's Saves details page and the game page's Cloud saves dialog say why Cloud saves need a
// choice and what each choice does. Uses the saves-choice scenarios (one game in each state).
#include "homebrew/simulation.hpp"
#include "steam_id.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <unistd.h>

using playden::homebrew::CloudStatus;
using playden::homebrew::Simulation;

namespace {

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::fprintf(stderr, "CHECK failed: %s\n", message.c_str());
        std::exit(1);
    }
}

const playden::homebrew::ViewRow* saves_row(const Simulation& model, const std::string& key) {
    for (const auto& row : model.view().rows) if (row.key == key) return &row;
    return nullptr;
}

// #1103: every state that needs a Cloud saves choice says why, and each button says what it does.
void saves_choices_say_what_they_do(const std::string& fixtures, const std::filesystem::path& profile) {
    using playden::homebrew::SavesChoice;
    using playden::homebrew::SavesChoiceText;
    check(SavesChoiceText({}) .empty(), "no outcome: no words");
    check(SavesChoiceText({1, 0, 0, 0}) == "Downloads 1 save" && SavesChoiceText({0, 3, 0, 0}) == "Uploads 3 saves",
          "one part names the saves");
    check(SavesChoiceText({0, 0, 1, 0}) == "Deletes 1 save here" && SavesChoiceText({0, 0, 0, 2}) == "Deletes 2 saves from Cloud",
          "a delete says where");
    check(SavesChoiceText({0, 2, 0, 1}) == "Uploads 2, deletes 1 from Cloud" &&
              SavesChoiceText({3, 1, 1, 0}) == "Downloads 3, uploads 1, deletes 1 here",
          "several parts stay short");

    // Saves list: status and line; Saves details: the note and the buttons' words.
    struct Expect { const char* label; const char* reason; const char* note; const char* use_cloud; const char* use_local;
                    bool red_cloud, red_local; };
    const Expect expected[] = {
        {"Conflict", "Changed on both sides", "1 save changed here and in Steam Cloud since the last sync.", "Downloads 1 save",
         "Uploads 1 save", false, false},
        {"Deleted here", "Deleted here · Steam Cloud still has it",
         "1 save was deleted on this console since the last sync. Steam Cloud still has it.", "Downloads 1 save",
         "Deletes 1 save from Cloud", false, true},
        {"Deleted in Cloud", "Deleted in Steam Cloud · this console still has it",
         "1 save was deleted from Steam Cloud since the last sync. This console still has it.", "Deletes 1 save here",
         "Uploads 1 save", true, false},
        {"Conflict", "Changed on both sides · deleted here",
         "Since the last sync, 2 saves changed on both sides and 1 was deleted on this console.", "Downloads 3 saves",
         "Uploads 2, deletes 1 from Cloud", false, true},
        {"Choose saves", "First sync · both sides already have saves", "Never synced on this PS5, and both sides already have saves.",
         "Downloads 2 saves", "Uploads 2 saves", false, false},
        {"Choose saves", "First upload would replace Cloud saves", "Saves here would replace the ones in Steam Cloud",
         "Downloads 2 saves", "Uploads 2 saves", false, false},
    };
    Simulation list(fixtures, profile);
    check(list.scenario("saves-choices"), "the choices scenario loads");
    for (int index = 0; index < 6; ++index) {
        const auto& item = expected[index];
        const auto id = list.state().games[static_cast<std::size_t>(index)].id;
        const auto* row = saves_row(list, "save-" + playden::ToKey(id));
        check(row && row->value == item.label && row->subtitle == item.reason, std::string("Saves list words: ") + item.label);
        check(playden::homebrew::SavesConflictNote(list.state().cloud.at(id)).rfind(item.note, 0) == 0,
              std::string("What's going on: ") + item.note);
        Simulation details(fixtures, profile);
        check(details.scenario("saves-choice-" + std::to_string(index + 1)), "a choice's Saves details page opens");
        const auto* cloud_button = saves_row(details, "saves-use-cloud");
        const auto* local_button = saves_row(details, "saves-use-local");
        check(details.view().subtitle == item.label && cloud_button && local_button && cloud_button->value == item.use_cloud &&
                  local_button->value == item.use_local && (cloud_button->tone == "problem") == item.red_cloud &&
                  (local_button->tone == "problem") == item.red_local,
              std::string("Saves details buttons: ") + item.reason);
    }
    // A state an older Playden saved: today's words, and the side's newest change instead of an outcome.
    playden::homebrew::LiveCloudGame old{SteamId(1), CloudStatus::Conflict, "", 1, 1, 10, 10, 5, 6};
    check(playden::homebrew::SavesConflictNote(old).rfind("Both changed since the last sync.", 0) == 0,
          "an old state keeps today's note");
}


// The dialog over the game page: the table's game, the buttons' words and the one line under them.
void dialog(const std::string& fixtures, const std::filesystem::path& profile) {
    Simulation model(fixtures, profile);
    check(model.scenario("saves-choice-dialog-2"), "the dialog scenario loads");
    const auto& modal = model.state().navigation.modals.back();
    check(modal.saves_game == model.state().games[1].id && modal.title.find(": which saves?") != std::string::npos &&
              modal.body == playden::homebrew::kSavesBackupNote,
          "the dialog draws the game's table and one line of text");
    const auto* cloud_button = saves_row(model, "cloud-use-cloud");
    const auto* local_button = saves_row(model, "cloud-use-local");
    check(cloud_button && cloud_button->value == "Downloads 1 save" && local_button &&
              local_button->value == "Deletes 1 save from Cloud" && local_button->tone == "problem",
          "the dialog's buttons say what they do");
    check(model.scenario("saves-choice-dialog-6") &&
              model.state().navigation.modals.back().body.find("If the game didn't show your progress") == 0,
          "a first upload keeps its warning");
}

}  // namespace

int main(int argc, char** argv) {
    check(argc == 2, "usage: homebrew_saves_choices_test <fixtures/ui-comparison>");
    const auto profile = std::filesystem::temp_directory_path() / ("playden-saves-choices-test-" + std::to_string(getpid()) + ".json");
    saves_choices_say_what_they_do(argv[1], profile);
    dialog(argv[1], profile);
    std::filesystem::remove(profile);
    std::puts("saves choices test passed");
}
