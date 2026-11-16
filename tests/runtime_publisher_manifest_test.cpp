#include "runtime/publisher_manifest.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

#define CHECK(condition)                                                                  \
  do {                                                                                    \
    if (!(condition)) {                                                                   \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
      std::exit(1);                                                                       \
    }                                                                                     \
  } while (false)

namespace fs = std::filesystem;
using namespace playden::runtime;

namespace {

// A made-up publisher's manifest with every section.
const std::string kManifest = R"({
  "version": 1,
  "account": {
    "name": "Example Publisher",
    "display_fields": ["name", "Name"],
    "stage_fields": ["token", "Token"],
    "stage_pattern": { "charset": "base64", "min": 8, "max": 64 },
    "max_bytes": 4096
  },
  "signin": { "page": "page/index.html" },
  "runtime": {
    "label": "Example Runtime",
    "installers": ["Redist/Example-Setup.exe", "Setup/Example-Setup.exe"],
    "version_file": "x86/runtime.dll",
    "folders": { "x86": "x86", "x64": "lib64/Example Runtime" }
  },
  "titles": {
    "4000": { "arch": "x86", "args": "-one -two=4000" },
    "4001": { "arch": "x64", "args": "" }
  },
  "files": [
    { "source": "proxy32.dll", "target": "proxy.dll", "keep_original": "proxy_real.dll", "arch": "x86" },
    { "source": "helper64.dll", "target": "helper.dll", "arch": "x64" },
    { "text": "", "target": ".examplepub/marker" }
  ],
  "ini": { "target": "helper.ini", "lines": ["args=${title.args}", "root=${runtime}\\lib64", "debug=0"] },
  "credential_file": "credential.txt",
  "status": { "file": ".examplepub/status", "expired": "state=stale" }
})";

std::string Replace(std::string text, const std::string& from, const std::string& to) {
  const auto at = text.find(from);
  CHECK(at != std::string::npos);
  return text.replace(at, from.size(), to);
}

bool Parses(const std::string& text, std::string* reason = nullptr) {
  PublisherStageManifest manifest;
  std::string why;
  const bool ok = ParsePublisherStageManifest(text, &manifest, &why);
  if (reason) *reason = why;
  CHECK(ok || !why.empty());
  return ok;
}

}  // namespace

int main() {
  // The whole manifest.
  PublisherStageManifest manifest;
  std::string reason;
  CHECK(ParsePublisherStageManifest(kManifest, &manifest, &reason));
  CHECK(manifest.account.name == "Example Publisher");
  CHECK((manifest.account.display_fields == std::vector<std::string>{"name", "Name"}));
  CHECK((manifest.account.stage_fields == std::vector<std::string>{"token", "Token"}));
  CHECK(manifest.account.stage_min == 8 && manifest.account.stage_max == 64 && manifest.account.max_bytes == 4096);
  CHECK(manifest.signin_page == "page/index.html");
  CHECK(manifest.runtime && manifest.runtime->label == "Example Runtime");
  CHECK(manifest.runtime->installers.size() == 2 && manifest.runtime->installers[1] == "Setup/Example-Setup.exe");
  CHECK(manifest.runtime->version_file == "x86/runtime.dll");
  CHECK(manifest.runtime->folders.at("x64") == "lib64/Example Runtime");
  CHECK(manifest.titles.size() == 2 && manifest.titles.at("4000").arch == StageArch::kX86);
  CHECK(manifest.titles.at("4000").args == "-one -two=4000" && manifest.titles.at("4001").arch == StageArch::kX64);
  CHECK(manifest.files.size() == 3);
  CHECK(manifest.files[0].source == std::optional<std::string>("proxy32.dll") && !manifest.files[0].text);
  CHECK(manifest.files[0].keep_original == "proxy_real.dll" && manifest.files[0].arch == StageArch::kX86);
  CHECK(manifest.files[1].keep_original.empty() && manifest.files[1].arch == StageArch::kX64);
  CHECK(manifest.files[2].text == std::optional<std::string>("") && !manifest.files[2].source && !manifest.files[2].arch);
  CHECK(manifest.files[2].target == ".examplepub/marker");
  CHECK(manifest.ini && manifest.ini->target == "helper.ini" && manifest.ini->lines.size() == 3);
  CHECK(manifest.credential_file == "credential.txt");
  CHECK(manifest.status && manifest.status->file == ".examplepub/status" && manifest.status->expired == "state=stale");
  CHECK(manifest.status->missing.empty());  // optional

  // The status of a start without any sign-in (optional): a single trimmed line, not the expired one.
  {
    const std::string expired = "\"expired\": \"state=stale\"";
    PublisherStageManifest with_missing;
    CHECK(ParsePublisherStageManifest(Replace(kManifest, expired, expired + ", \"missing\": \"state=none\""),
                                      &with_missing, &reason));
    CHECK(with_missing.status && with_missing.status->missing == "state=none");
    CHECK(with_missing.status->expired == "state=stale");
    for (const char* bad : {"\"\"", "\" state=none\"", "\"state=none \"", "\"state\\nnone\"", "1", "\"state=stale\""})
      CHECK(!Parses(Replace(kManifest, expired, expired + ", \"missing\": " + bad), &reason));
    CHECK(reason.find("status.missing must differ from status.expired") != std::string::npos);
  }

  // Ini lines with their variables.
  std::vector<std::string> lines;
  PublisherTitleSpec title;
  title.args = "-one -two=4000";
  CHECK(ExpandPublisherStageIni(manifest, title, "Z:\\data\\runtime\\1.0", &lines, &reason));
  CHECK((lines == std::vector<std::string>{"args=-one -two=4000", "root=Z:\\data\\runtime\\1.0\\lib64", "debug=0"}));
  title.args = "-one";
  CHECK(!ExpandPublisherStageIni(manifest, title, "", &lines, &reason) && lines.empty());
  CHECK(reason.find("${runtime}") != std::string::npos);
  title.args = "-one\r\nextra=1";
  CHECK(!ExpandPublisherStageIni(manifest, title, "Z:\\r", &lines, &reason));
  title.args = "-one";
  title.vars["mode"] = "a\nb";
  CHECK(!ExpandPublisherStageIni(manifest, title, "Z:\\r", &lines, &reason));

  // A title's own program (install-root relative, a safe path).
  {
    PublisherStageManifest own;
    CHECK(manifest.titles.at("4000").program.empty());
    CHECK(ParsePublisherStageManifest(
        Replace(kManifest, "\"args\": \"\" }", "\"args\": \"\", \"program\": \"Game Folder/game64.exe\" }"), &own, &reason));
    CHECK(own.titles.at("4001").program == "Game Folder/game64.exe" && own.titles.at("4000").program.empty());
    for (const char* bad : {"../game.exe", "/game.exe", "C:game.exe", "bin\\\\game.exe", "", "bin//game.exe"})
      CHECK(!Parses(Replace(kManifest, "\"args\": \"\" }", "\"args\": \"\", \"program\": \"" + std::string(bad) + "\" }")));
    CHECK(!Parses(Replace(kManifest, "\"args\": \"\" }", "\"args\": \"\", \"program\": 1 }")));
  }

  // Per-title values: ${title.<key>} when every title defines the key.
  {
    const auto with_vars = Replace(Replace(Replace(kManifest,
        "\"args\": \"-one -two=4000\" }", "\"args\": \"-one -two=4000\", \"vars\": { \"mode\": \"fast\", \"lib_dir\": \"x86\" } }"),
        "\"args\": \"\" }", "\"args\": \"\", \"vars\": { \"mode\": \"slow\" } }"),
        "\"debug=0\"", "\"debug=0\", \"mode=${title.mode}${title.mode}\"");
    PublisherStageManifest titled;
    CHECK(ParsePublisherStageManifest(with_vars, &titled, &reason));
    CHECK(titled.titles.at("4000").vars.size() == 2 && titled.titles.at("4000").vars.at("lib_dir") == "x86");
    CHECK(titled.titles.at("4001").vars.at("mode") == "slow");
    CHECK(ExpandPublisherStageIni(titled, titled.titles.at("4001"), "Z:\\r", &lines, &reason));
    CHECK(lines.back() == "mode=slowslow" && lines.front() == "args=");
    CHECK(ExpandPublisherStageIni(titled, titled.titles.at("4000"), "Z:\\r", &lines, &reason));
    CHECK(lines.back() == "mode=fastfast");
    // A key one title lacks can't be used, nor can a key no title has.
    CHECK(!Parses(Replace(with_vars, "${title.mode}${title.mode}", "${title.lib_dir}"), &reason));
    CHECK(reason.find("${title.lib_dir}") != std::string::npos && reason.find("every title") != std::string::npos);
    CHECK(!Parses(Replace(with_vars, "${title.mode}${title.mode}", "${title.other}")));
    CHECK(!Parses(Replace(kManifest, "\"debug=0\"", "\"debug=${title.mode}\"")));
    // Keys, values and the vars object are checked.
    CHECK(!Parses(Replace(with_vars, "\"lib_dir\"", "\"Lib\""), &reason));
    CHECK(reason.find("titles.4000.vars keys") != std::string::npos);
    CHECK(!Parses(Replace(with_vars, "\"lib_dir\"", "\"args\"")));
    CHECK(!Parses(Replace(with_vars, "\"lib_dir\"", "\"\"")));
    CHECK(!Parses(Replace(with_vars, "\"lib_dir\"", "\"" + std::string(33, 'a') + "\"")));
    CHECK(Parses(Replace(with_vars, "\"lib_dir\"", "\"" + std::string(32, 'a') + "\"")));
    CHECK(!Parses(Replace(with_vars, "\"x86\" }", "\"" + std::string(257, 'x') + "\" }")));
    CHECK(Parses(Replace(with_vars, "\"x86\" }", "\"" + std::string(256, 'x') + "\" }")));
    CHECK(!Parses(Replace(with_vars, "\"x86\" }", "\"a\\nb\" }")));
    CHECK(!Parses(Replace(with_vars, "\"x86\" }", "1 }")));
    CHECK(!Parses(Replace(with_vars, "\"vars\": { \"mode\": \"slow\" }", "\"vars\": [\"slow\"]")));
    CHECK(!Parses(Replace(with_vars, "\"mode\": \"slow\"", "\"mode\": \"slow\", \"mode\": \"slow\"")));
    std::string many;
    for (int index = 0; index < 17; ++index) many += ", \"k" + std::to_string(index) + "\": \"v\"";
    CHECK(!Parses(Replace(with_vars, "\"mode\": \"slow\"", "\"mode\": \"slow\"" + many)));
  }

  // Only the required sections.
  const std::string minimal = R"({"version":1,
    "account":{"name":"Example","display_fields":[],"stage_fields":["token"],
               "stage_pattern":{"charset":"base64","min":1,"max":10},"max_bytes":100},
    "titles":{},"files":[],"credential_file":"credential.txt"})";
  CHECK(ParsePublisherStageManifest(minimal, &manifest, &reason));
  CHECK(manifest.signin_page.empty() && !manifest.runtime && !manifest.ini && !manifest.status);
  CHECK(ExpandPublisherStageIni(manifest, PublisherTitleSpec{}, "", &lines, &reason) && lines.empty());
  // No titles: no ${title.<key>} is defined everywhere.
  CHECK(!Parses(Replace(Replace(kManifest, "\"4000\": { \"arch\": \"x86\", \"args\": \"-one -two=4000\" },\n    \"4001\": { \"arch\": \"x64\", \"args\": \"\" }", ""),
                        "\"debug=0\"", "\"debug=${title.mode}\"")));

  // Strictness: version, unknown top-level keys, repeated keys, types.
  CHECK(!Parses(Replace(kManifest, "\"version\": 1", "\"version\": 2"), &reason));
  CHECK(reason.find("version") != std::string::npos);
  CHECK(!Parses(Replace(kManifest, "\"version\": 1", "\"version\": \"1\"")));
  CHECK(!Parses(Replace(kManifest, "\"version\": 1,", "\"version\": 1, \"extra\": true,"), &reason));
  CHECK(reason.find("unknown key extra") != std::string::npos);
  CHECK(!Parses(Replace(kManifest, "\"version\": 1,", "\"version\": 1, \"version\": 1,")));
  // Other installers the pack stands in for (optional; safe relative paths).
  {
    PublisherStageManifest covered;
    CHECK(manifest.covered_installers.empty());
    CHECK(ParsePublisherStageManifest(
        Replace(kManifest, "\"version\": 1,", "\"version\": 1, \"covered_installers\": [\"Setup/Example-Launcher.exe\"],"),
        &covered, &reason));
    CHECK((covered.covered_installers == std::vector<std::string>{"Setup/Example-Launcher.exe"}));
    CHECK(!Parses(Replace(kManifest, "\"version\": 1,", "\"version\": 1, \"covered_installers\": [\"../x.exe\"],")));
    CHECK(!Parses(Replace(kManifest, "\"version\": 1,", "\"version\": 1, \"covered_installers\": \"x.exe\",")));
  }
  // A launcher run beside the game (optional): an .exe below the title folder, a ready file below drive_c, 1-120 s.
  {
    CHECK(!manifest.helper);
    PublisherStageManifest with_helper;
    const auto helper = [&](const std::string& body) {
      return Replace(kManifest, "\"version\": 1,", "\"version\": 1, \"helper\": {" + body + "},");
    };
    CHECK(ParsePublisherStageManifest(helper("\"program\": \".x/Launcher.exe\", \"ready\": \"playden/x.ready\", \"wait\": 90"),
                                      &with_helper, &reason));
    CHECK(with_helper.helper && with_helper.helper->program == ".x/Launcher.exe" &&
          with_helper.helper->ready == "playden/x.ready" && with_helper.helper->wait == 90);
    CHECK(ParsePublisherStageManifest(helper("\"program\": \"L.exe\", \"ready\": \"r\""), &with_helper, &reason));
    CHECK(with_helper.helper->wait == 60 && with_helper.helper->identity.empty());
    CHECK(ParsePublisherStageManifest(helper("\"program\": \"L.exe\", \"ready\": \"r\", \"identity\": \"launcher\""), &with_helper, &reason));
    CHECK(with_helper.helper->identity == "launcher");
    for (const char* bad : {"\"ready\": \"r\"", "\"program\": \"L.exe\"", "\"program\": \"L.dll\", \"ready\": \"r\"",
                            "\"program\": \"../L.exe\", \"ready\": \"r\"", "\"program\": \"L.exe\", \"ready\": \"a b\"",
                            "\"program\": \"L.exe\", \"ready\": \"r\", \"wait\": 0",
                            "\"program\": \"L.exe\", \"ready\": \"r\", \"wait\": 121",
                            "\"program\": \"L.exe\", \"ready\": \"r\", \"other\": 1",
                            "\"program\": \"L.exe\", \"ready\": \"r\", \"identity\": \"steam\""})
      CHECK(!Parses(helper(bad)));
  }
  CHECK(!Parses("[]"));
  CHECK(!Parses("{"));
  CHECK(!Parses(Replace(kManifest, "\"credential_file\": \"credential.txt\",", "")));
  CHECK(!Parses(Replace(kManifest, "\"max_bytes\": 4096", "\"max_bytes\": \"4096\"")));
  CHECK(!Parses(Replace(kManifest, "\"max_bytes\": 4096", "\"max_bytes\": 32")));  // below stage max
  CHECK(!Parses(Replace(kManifest, "\"max_bytes\": 4096", "\"max_bytes\": 70000")));
  CHECK(!Parses(Replace(kManifest, "\"min\": 8", "\"min\": 0")));
  CHECK(!Parses(Replace(kManifest, "\"min\": 8", "\"min\": 80")));
  CHECK(!Parses(Replace(kManifest, "\"min\": 8", "\"min\": 8.5")));
  CHECK(!Parses(Replace(kManifest, "\"charset\": \"base64\"", "\"charset\": \"hex\"")));
  CHECK(!Parses(Replace(kManifest, "\"stage_fields\": [\"token\", \"Token\"]", "\"stage_fields\": []")));
  CHECK(!Parses(Replace(kManifest, "\"stage_fields\": [\"token\", \"Token\"]", "\"stage_fields\": [\"token\", 3]")));
  CHECK(!Parses(Replace(kManifest, "\"display_fields\": [\"name\", \"Name\"]", "\"display_fields\": \"name\"")));
  CHECK(!Parses(Replace(kManifest, "\"signin\": { \"page\": \"page/index.html\" }", "\"signin\": \"page\"")));
  CHECK(!Parses(Replace(kManifest, "\"folders\": { \"x86\": \"x86\", \"x64\": \"lib64/Example Runtime\" }",
                        "\"folders\": {}")));
  CHECK(!Parses(Replace(kManifest, "\"x86\": \"x86\",", "\"x86/sub\": \"x86\",")));

  // Titles: Steam app ids, x86 or x64, single-line arguments.
  CHECK(!Parses(Replace(kManifest, "\"4001\":", "\"0401\":")));
  CHECK(!Parses(Replace(kManifest, "\"4001\":", "\"game\":")));
  CHECK(!Parses(Replace(kManifest, "\"arch\": \"x64\", \"args\": \"\"", "\"arch\": \"arm64\", \"args\": \"\"")));
  CHECK(!Parses(Replace(kManifest, "\"arch\": \"x64\", \"args\": \"\"", "\"arch\": \"x64\"")));
  CHECK(!Parses(Replace(kManifest, "\"args\": \"\"", "\"args\": \"-a\\n-b\"")));

  // Files: exactly one of source and text; safe, distinct paths.
  CHECK(!Parses(Replace(kManifest, "{ \"text\": \"\", \"target\"", "{ \"text\": \"\", \"source\": \"x.dll\", \"target\"")));
  CHECK(!Parses(Replace(kManifest, "{ \"text\": \"\", \"target\"", "{ \"target\"")));
  CHECK(!Parses(Replace(kManifest, "\"target\": \"proxy.dll\"", "\"target\": \"../proxy.dll\"")));
  CHECK(!Parses(Replace(kManifest, "\"target\": \"proxy.dll\"", "\"target\": \"/proxy.dll\"")));
  CHECK(!Parses(Replace(kManifest, "\"target\": \"proxy.dll\"", "\"target\": \"C:proxy.dll\"")));
  CHECK(!Parses(Replace(kManifest, "\"target\": \"proxy.dll\"", "\"target\": \"sub\\\\proxy.dll\"")));
  CHECK(!Parses(Replace(kManifest, "\"target\": \"proxy.dll\"", "\"target\": \"sub//proxy.dll\"")));
  CHECK(!Parses(Replace(kManifest, "\"target\": \"proxy.dll\"", "\"target\": \"./proxy.dll\"")));
  CHECK(!Parses(Replace(kManifest, "\"target\": \"proxy.dll\"", "\"target\": \"\"")));
  CHECK(!Parses(Replace(kManifest, "\"source\": \"proxy32.dll\"", "\"source\": \"../proxy32.dll\"")));
  CHECK(!Parses(Replace(kManifest, "\"keep_original\": \"proxy_real.dll\"", "\"keep_original\": \"\"")));
  // A path is used once per architecture: the x86 and the x64 entry may share a target or a kept
  // original (at most one applies to a title); entries of the same or of every architecture may not.
  {
    PublisherStageManifest shared;
    CHECK(ParsePublisherStageManifest(Replace(kManifest, "\"target\": \"helper.dll\"", "\"target\": \"PROXY.dll\""),
                                      &shared, &reason));
    CHECK(shared.files[0].target == "proxy.dll" && shared.files[1].target == "PROXY.dll");
    const auto both_keep = Replace(Replace(kManifest, "\"target\": \"helper.dll\"",
                                           "\"target\": \"proxy.dll\", \"keep_original\": \"proxy_real.dll\""),
                                   "\"source\": \"helper64.dll\"", "\"source\": \"proxy64.dll\"");
    CHECK(ParsePublisherStageManifest(both_keep, &shared, &reason));
    CHECK(shared.files[1].target == "proxy.dll" && shared.files[1].keep_original == "proxy_real.dll");
    CHECK(!Parses(Replace(both_keep, "\"arch\": \"x64\" }", "\"arch\": \"x86\" }"), &reason));
    CHECK(reason.find("proxy.dll is used twice") != std::string::npos);
    CHECK(!Parses(Replace(both_keep, ", \"arch\": \"x64\" }", " }"), &reason));
    CHECK(reason.find("proxy.dll is used twice") != std::string::npos);
    // A kept original of one architecture may be the other's target, never its own entry's.
    CHECK(Parses(Replace(kManifest, "\"target\": \"helper.dll\"", "\"target\": \"proxy_real.dll\"")));
    CHECK(!Parses(Replace(kManifest, "\"text\": \"\", \"target\": \".examplepub/marker\"",
                          "\"text\": \"\", \"target\": \"proxy_real.dll\""), &reason));
    CHECK(reason.find("proxy_real.dll is used twice") != std::string::npos);
    // The ini, credential and status paths are every title's.
    CHECK(!Parses(Replace(kManifest, "\"target\": \"helper.dll\"", "\"target\": \"helper.ini\"")));
  }
  CHECK(!Parses(Replace(kManifest, "\"keep_original\": \"proxy_real.dll\"", "\"keep_original\": \"proxy.dll\"")));
  CHECK(!Parses(Replace(kManifest, "\"credential_file\": \"credential.txt\"", "\"credential_file\": \"helper.ini\"")));
  CHECK(!Parses(Replace(kManifest, "\"arch\": \"x64\" }", "\"arch\": \"any\" }")));
  CHECK(!Parses(Replace(kManifest, "\"page\": \"page/index.html\"", "\"page\": \"../index.html\"")));
  CHECK(!Parses(Replace(kManifest, "\"Redist/Example-Setup.exe\"", "\"Redist/../Example-Setup.exe\"")));

  // Ini: known variables only, and ${runtime} needs a runtime.
  CHECK(!Parses(Replace(kManifest, "\"debug=0\"", "\"debug=${cache}\""), &reason));
  CHECK(reason.find("${cache}") != std::string::npos);
  CHECK(!Parses(Replace(kManifest, "\"debug=0\"", "\"debug=${title.args\"")));
  const auto without_runtime = Replace(kManifest, R"("runtime": {
    "label": "Example Runtime",
    "installers": ["Redist/Example-Setup.exe", "Setup/Example-Setup.exe"],
    "version_file": "x86/runtime.dll",
    "folders": { "x86": "x86", "x64": "lib64/Example Runtime" }
  },)", "");
  CHECK(!Parses(without_runtime, &reason));
  CHECK(reason.find("without a runtime") != std::string::npos);
  CHECK(Parses(Replace(without_runtime, "\"root=${runtime}\\\\lib64\", ", "")));

  // Status: a trimmed single line.
  CHECK(!Parses(Replace(kManifest, "\"expired\": \"state=stale\"", "\"expired\": \"\"")));
  CHECK(!Parses(Replace(kManifest, "\"expired\": \"state=stale\"", "\"expired\": \" state=stale\"")));
  CHECK(!Parses(Replace(kManifest, "\"expired\": \"state=stale\"", "\"expired\": 1")));

  // Size limit.
  CHECK(!Parses(Replace(kManifest, "\"debug=0\"", "\"debug=" + std::string(64 * 1024, 'a') + "\""), &reason));
  CHECK(reason.find("64 KiB") != std::string::npos);

  // From a folder: a regular file, links not followed.
  const auto root = fs::temp_directory_path() / ("playden-manifest-" + std::to_string(::getpid()));
  fs::remove_all(root);
  fs::create_directories(root / "pack");
  CHECK(!LoadPublisherStageManifest((root / "pack").string(), &manifest, &reason));
  std::ofstream(root / "real.json", std::ios::binary) << kManifest;
  fs::create_symlink(root / "real.json", root / "pack/stage.json");
  CHECK(!LoadPublisherStageManifest((root / "pack").string(), &manifest, &reason));
  fs::remove(root / "pack/stage.json");
  fs::copy_file(root / "real.json", root / "pack/stage.json");
  manifest = {};
  CHECK(LoadPublisherStageManifest((root / "pack").string(), &manifest, &reason));
  CHECK(manifest.titles.count("4000") == 1);
  fs::remove_all(root);
  return 0;
}
