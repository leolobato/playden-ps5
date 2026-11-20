// my_templates_test.cpp - The player's own controller layout templates: rules, changes, my-templates.json and its
// bad-file handling.
// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime/my_templates.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "%d: %s\n", __LINE__, #c); std::exit(1); } } while (0)

namespace {

using namespace playden::runtime;
namespace fs = std::filesystem;
using R = MyTemplateRefusal;

std::string Id(int n) {
  char text[32];
  std::snprintf(text, sizeof(text), "my-%016x", n);
  return text;
}

std::string Read(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  CHECK(input.good());
  return {std::istreambuf_iterator<char>(input), {}};
}

void Write(const fs::path& path, const std::string& text) {
  std::ofstream(path, std::ios::binary | std::ios::trunc) << text;
  CHECK(::chmod(path.c_str(), 0600) == 0);
}

mode_t Mode(const fs::path& path) {
  struct stat info {};
  CHECK(::stat(path.c_str(), &info) == 0);
  return info.st_mode & 07777;
}

// The WASD and mouse template with Cross sending Enter: a layout of the player's.
std::string Custom() {
  auto text = FindControllerLayoutTemplate("wasd-mouse")->text;
  const auto at = text.find("cross = key:space");
  CHECK(at != std::string::npos);
  return text.replace(at, 17, "cross = key:enter");
}

void Changes() {
  std::vector<MyTemplate> templates;
  // A layout and its mode; the text is kept canonical.
  CHECK(AddMyTemplate(&templates, Id(1), "  Brotato menus ", "keyboard", Custom(), 10).ok());
  CHECK(templates.size() == 1 && templates[0].name == "Brotato menus" && templates[0].mode == "keyboard" &&
        templates[0].layout == Custom() && templates[0].created == 10 && templates[0].updated == 10);
  CHECK(FindMyTemplateByLayout(templates, Custom(), "keyboard") == &templates[0]);
  CHECK(!FindMyTemplateByLayout(templates, Custom(), "xinput"));
  // Names follow My presets: trimmed, 1-40 printable characters, unique ignoring case and accents.
  CHECK(AddMyTemplate(&templates, Id(2), "   ", "keyboard", Custom(), 11).refusal == R::kNameEmpty);
  CHECK(AddMyTemplate(&templates, Id(2), std::string(41, 'a'), "keyboard", Custom(), 11).refusal == R::kNameTooLong);
  CHECK(AddMyTemplate(&templates, Id(2), "bad \x01", "keyboard", Custom(), 11).refusal == R::kNameNotPrintable);
  CHECK(AddMyTemplate(&templates, Id(2), "BROTATO MENUS", "keyboard", Custom(), 11).refusal == R::kNameTaken);
  // A bad id, an unknown mode, a bad or empty layout.
  CHECK(AddMyTemplate(&templates, "nope", "Other", "keyboard", Custom(), 11).refusal == R::kBadId);
  CHECK(AddMyTemplate(&templates, Id(1), "Other", "keyboard", Custom(), 11).refusal == R::kBadId);
  CHECK(AddMyTemplate(&templates, Id(2), "Other", "joystick", Custom(), 11).refusal == R::kBadLayout);
  CHECK(AddMyTemplate(&templates, Id(2), "Other", "keyboard", "cross = banana", 11).refusal == R::kBadLayout);
  CHECK(AddMyTemplate(&templates, Id(2), "Other", "keyboard", "", 11).refusal == R::kBadLayout);
  // A built-in template unchanged is refused by name; the same text in another mode is the player's own.
  const auto* mouse = FindControllerLayoutTemplate("mouse");
  const auto same = AddMyTemplate(&templates, Id(2), "Other", mouse->mode, mouse->text, 11);
  CHECK(same.refusal == R::kSameAsBuiltIn && same.detail == "Mouse");
  CHECK(FindBuiltInTemplateByLayout(mouse->text, mouse->mode) == mouse);
  CHECK(AddMyTemplate(&templates, Id(2), "Mouse on a pad", "xinput", mouse->text, 11).ok());
  CHECK(templates.size() == 2);

  // Rename: its own name in another case is fine, another template's is not.
  CHECK(RenameMyTemplate(&templates, Id(1), "brotato MENUS", 12).ok() && templates[0].name == "brotato MENUS" &&
        templates[0].updated == 12);
  CHECK(RenameMyTemplate(&templates, Id(1), "Mouse on a pad", 13).refusal == R::kNameTaken);
  CHECK(RenameMyTemplate(&templates, Id(9), "Gone", 13).refusal == R::kUnknownId);
  // Update replaces the layout and mode, with the same rules.
  CHECK(ReplaceMyTemplateLayout(&templates, Id(1), "playstation", Custom(), 14).ok() && templates[0].mode == "playstation" &&
        templates[0].updated == 14);
  CHECK(ReplaceMyTemplateLayout(&templates, Id(1), mouse->mode, mouse->text, 15).refusal == R::kSameAsBuiltIn);
  CHECK(templates[0].mode == "playstation");
  // Delete.
  CHECK(DeleteMyTemplate(&templates, Id(2)).ok() && templates.size() == 1);
  CHECK(DeleteMyTemplate(&templates, Id(2)).refusal == R::kUnknownId);

  // 50 at most.
  std::vector<MyTemplate> full;
  for (int n = 1; n <= static_cast<int>(kMyTemplatesMax); ++n)
    CHECK(AddMyTemplate(&full, Id(n), "Layout " + std::to_string(n), "keyboard", Custom(), n).ok());
  CHECK(AddMyTemplate(&full, Id(99), "One more", "keyboard", Custom(), 99).refusal == R::kTooMany);
}

void Format() {
  std::vector<MyTemplate> templates;
  CHECK(AddMyTemplate(&templates, Id(1), "Menus \"quoted\"", "keyboard", Custom(), 5).ok());
  CHECK(AddMyTemplate(&templates, Id(2), "Pad", "xinput", FindControllerLayoutTemplate("arrows")->text, 6).ok());
  const auto text = SerializeMyTemplates(templates);
  std::vector<MyTemplate> parsed;
  std::vector<std::string> dropped;
  std::string reason;
  CHECK(ParseMyTemplates(text, &parsed, &dropped, &reason) && parsed == templates && dropped.empty());
  // A template a newer Playden wrote (an unknown mode or line) is dropped; the others load.
  const std::string newer = R"({"format":1,"templates":[)"
                            R"({"id":")" + Id(1) + R"(","name":"Ok","mode":"keyboard","layout":"cross = key:enter","created":1,"updated":1},)"
                            R"({"id":")" + Id(2) + R"(","name":"Future","mode":"wheel","layout":"cross = key:enter","created":1,"updated":1},)"
                            R"({"id":")" + Id(3) + R"(","name":"Future 2","mode":"keyboard","layout":"cross = laser:red","created":1,"updated":1}]})";
  CHECK(ParseMyTemplates(newer, &parsed, &dropped, &reason) && parsed.size() == 1 && parsed[0].name == "Ok" &&
        dropped.size() == 2 && dropped[0].rfind(Id(2) + " ", 0) == 0);
  // A template without a mode and layout this build reads, or with a layout longer than the parser's string limit
  // would allow for one, is dropped alone.
  const std::string odd = R"({"format":1,"templates":[)"
                          R"({"id":")" + Id(1) + R"(","name":"Ok","mode":"keyboard","layout":"cross = key:enter","created":1,"updated":1},)"
                          R"({"id":")" + Id(2) + R"(","name":"Null","mode":"keyboard","layout":null,"created":1,"updated":1},)"
                          R"({"id":")" + Id(3) + R"(","name":"Long","mode":"keyboard","layout":")" + std::string(9000, 'x') +
                          R"(","created":1,"updated":1}]})";
  CHECK(ParseMyTemplates(odd, &parsed, &dropped, &reason) && parsed.size() == 1 && dropped.size() == 2);
  // A file that isn't templates is refused.
  CHECK(!ParseMyTemplates("[]", &parsed, nullptr, &reason));
  CHECK(!ParseMyTemplates(R"({"format":2,"templates":[]})", &parsed, nullptr, &reason));
  CHECK(!ParseMyTemplates(R"({"format":1,"templates":[{"id":"x","name":"a","mode":"keyboard","layout":"","created":1,"updated":1}]})",
                          &parsed, nullptr, &reason));
  const std::string twice = R"({"format":1,"templates":[)"
                            R"({"id":")" + Id(1) + R"(","name":"A","mode":"keyboard","layout":"cross = key:enter","created":1,"updated":1},)"
                            R"({"id":")" + Id(1) + R"(","name":"B","mode":"keyboard","layout":"cross = key:enter","created":1,"updated":1}]})";
  CHECK(!ParseMyTemplates(twice, &parsed, nullptr, &reason) && reason.find("twice") != std::string::npos);
}

void Storage() {
  char pattern[] = "/tmp/playden-my-templates-XXXXXX";
  CHECK(::mkdtemp(pattern));
  const auto root = fs::canonical(pattern);
  const auto folder = root / "game-settings-private";
  CHECK(::mkdir(folder.c_str(), 0700) == 0);
  const auto path = MyTemplatesPath(folder / "game-settings.json");
  CHECK(path == folder / "my-templates.json");
  const auto bad = folder / kMyTemplatesBadFileName;

  // No file: no templates.
  auto load = LoadMyTemplatesFile(path.string());
  CHECK(load.status == MyTemplatesLoadStatus::kLoaded && load.templates.empty());

  std::vector<MyTemplate> templates;
  CHECK(AddMyTemplate(&templates, Id(1), "Menus", "keyboard", Custom(), 5).ok());
  std::string reason;
  CHECK(SaveMyTemplatesFile(path.string(), templates, &reason));
  CHECK(Mode(path) == 0666);
  load = LoadMyTemplatesFile(path.string());
  CHECK(load.status == MyTemplatesLoadStatus::kLoaded && load.templates == templates);

  // Bytes that would not load back intact are refused, and the file stays.
  const auto saved = Read(path);
  auto invalid = templates;
  invalid[0].updated = -1;
  CHECK(!SaveMyTemplatesFile(path.string(), invalid, &reason) && Read(path) == saved);
  invalid = templates;
  invalid[0].mode = "wheel";
  CHECK(!SaveMyTemplatesFile(path.string(), invalid, &reason) && Read(path) == saved);
  invalid = templates;
  invalid.push_back(invalid.front());
  CHECK(!SaveMyTemplatesFile(path.string(), invalid, &reason) && Read(path) == saved);

  // A malformed file is set aside as .bad, and Playden starts with none.
  Write(path, "{not json");
  load = LoadMyTemplatesFile(path.string());
  CHECK(load.status == MyTemplatesLoadStatus::kSetAside && load.templates.empty() && fs::exists(bad) && !fs::exists(path));
  CHECK(!MyTemplatesLoadLog(load).empty());
  // A second malformed file can't be set aside over the first: unavailable, and saving refuses.
  Write(path, "{still not json");
  load = LoadMyTemplatesFile(path.string());
  CHECK(load.status == MyTemplatesLoadStatus::kUnavailable && fs::exists(path));
  CHECK(!SaveMyTemplatesFile(path.string(), templates, &reason) && Read(path) == "{still not json");
  // A newer Playden's file stays for it, long strings and all.
  fs::remove(path);
  fs::remove(bad);
  Write(path, R"({"format":2,"templates":[{"layout":")" + std::string(9000, 'x') + R"("}]})");
  load = LoadMyTemplatesFile(path.string());
  CHECK(load.status == MyTemplatesLoadStatus::kUnavailable && fs::exists(path) && !fs::exists(bad));
  // A format 1 file with a template this build skips loads the rest, and is never rewritten without it.
  fs::remove(path);
  const std::string newer = R"({"format":1,"templates":[)"
                            R"({"id":")" + Id(1) + R"(","name":"Menus","mode":"keyboard","layout":"cross = key:enter","created":1,"updated":1},)"
                            R"({"id":")" + Id(2) + R"(","name":"Future","mode":"wheel","layout":"cross = key:enter","created":1,"updated":1}]})";
  Write(path, newer);
  load = LoadMyTemplatesFile(path.string());
  CHECK(load.status == MyTemplatesLoadStatus::kLoaded && load.templates.size() == 1 && load.dropped.size() == 1);
  auto renamed = load.templates;
  CHECK(RenameMyTemplate(&renamed, Id(1), "Renamed", 2).ok());
  CHECK(!SaveMyTemplatesFile(path.string(), renamed, &reason) && Read(path) == newer);
  fs::remove_all(root);
}

}  // namespace

int main() {
  Changes();
  Format();
  Storage();
  std::puts("my templates tests passed");
}
