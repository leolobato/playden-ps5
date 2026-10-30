#include "steam/backend/app_info.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <utility>
#include <vector>

#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      std::exit(1);                                                    \
    }                                                                  \
  } while (0)

namespace {

using namespace playden::steam;

constexpr std::uint32_t kApp = 4242;

const char kAppInfoText[] = R"VDF("appinfo"
{
	"appid"		"4242"
	"common"
	{
		"name"		"Test Game"
		"type"		"Game"
		"controller_support"		"full"
	}
	"extended"
	{
		"listofdlc"		"999, 998,bad,1000"
	}
	"config"
	{
		"installdir"		"TestGame"
		"launch"
		{
			"3"
			{
				"executable"		"DLC.exe"
				"config"
				{
					"oslist"		"windows"
					"ownsdlc"		"999"
				}
			}
			"0"
			{
				"executable"		"Game.exe"
				"arguments"		"-a \"b c\""
				"workingdir"		"bin"
				"description"		"Play"
				"type"		"default"
				"config"
				{
					"oslist"		"windows"
					"osarch"		"64"
				}
			}
			"10"
			{
				"executable"		"Other.exe"
				"config"
				{
					"oslist"		"windows,linux"
				}
			}
			"1"
			{
				"executable"		"game_linux"
				"config"
				{
					"oslist"		"linux"
				}
			}
			"2"
			{
				"executable"		"Beta.exe"
				"config"
				{
					"oslist"		"windows"
					"betakey"		"beta"
				}
			}
		}
	}
	"depots"
	{
		"baselanguages"		"english,german"
		"overridescddb"		"1"
		"101"
		{
			"name"		"Game Content"
			"config"
			{
				"oslist"		"windows"
			}
			"manifests"
			{
				"public"
				{
					"gid"		"123"
					"size"		"1000"
					"download"		"400"
				}
			}
		}
		"102"
		{
			"config"
			{
				"oslist"		"macos"
			}
			"manifests"
			{
				"public"
				{
					"gid"		"2"
					"size"		"1"
					"download"		"1"
				}
			}
		}
		"103"
		{
			"config"
			{
				"oslist"		"windows"
				"language"		"german"
			}
			"manifests"
			{
				"public"
				{
					"gid"		"3"
					"size"		"1"
					"download"		"1"
				}
			}
		}
		"104"
		{
			"sharedinstall"		"1"
			"manifests"
			{
				"public"
				{
					"gid"		"4"
					"size"		"1"
					"download"		"1"
				}
			}
		}
		"105"
		{
			"dlcappid"		"999"
			"manifests"
			{
				"public"
				{
					"gid"		"5"
					"size"		"10"
					"download"		"4"
				}
			}
		}
		"106"
		{
			"config"
			{
				"oslist"		"windows"
			}
			"manifests"
			{
				"public"		"777"
			}
		}
		"branches"
		{
			"public"
			{
				"buildid"		"42"
			}
		}
	}
}
)VDF";

std::string Buffer() { return std::string(kAppInfoText, sizeof(kAppInfoText) - 1) + std::string(2, '\0'); }

std::vector<std::uint32_t> Ids(const std::vector<DepotInfo>& depots) {
  std::vector<std::uint32_t> ids;
  for (const DepotInfo& d : depots) ids.push_back(d.id);
  return ids;
}

std::vector<std::string> LaunchIds(const std::vector<AppLaunch>& launches) {
  std::vector<std::string> ids;
  for (const AppLaunch& l : launches) ids.push_back(l.id);
  return ids;
}

using U32s = std::vector<std::uint32_t>;
using Strings = std::vector<std::string>;

// SelectDepots for the game alone, with its outputs split out.
bool Select(const AppInfo& app, const std::set<std::uint32_t>& owned_apps,
            const std::optional<std::set<std::uint32_t>>& owned_depots, std::string_view language,
            std::vector<DepotInfo>* selected, std::string* reason, std::string* effective = nullptr,
            std::vector<std::string>* languages = nullptr) {
  DepotSelection selection;
  selection.language = std::string(language);
  SelectionResult result;
  const bool ok = SelectDepots(app, {}, owned_apps, owned_depots, selection, &result);
  *selected = result.depots;
  *reason = result.reason;
  if (effective != nullptr) *effective = result.effective_language;
  if (languages != nullptr) *languages = result.languages;
  return ok;
}

std::string RejectedAs(const SelectionResult& result, std::uint32_t id) {
  for (const auto& rejected : result.rejected)
    if (rejected.id == id) return rejected.reason;
  return "";
}

void TestParse() {
  const auto app = ParseAppInfo(kApp, Buffer());
  CHECK(app);
  CHECK(app->app_id == kApp);
  CHECK(app->name == "Test Game");
  CHECK(app->type == "Game");
  CHECK(app->install_dir == "TestGame");
  CHECK(app->controller_support == "full");
  CHECK((app->dlc_app_ids == U32s{999, 998, 1000}));
  CHECK((Ids(app->depots) == U32s{101, 102, 103, 104, 105, 106}));
  CHECK(app->branches.size() == 1 && app->branches[0].name == "public" && app->branches[0].build_id == 42 &&
        !app->branches[0].pwd_required && app->branches[0].description.empty());

  const DepotInfo& d101 = app->depots[0];
  CHECK(d101.name == "Game Content");
  CHECK(d101.os_list == "windows");
  CHECK(d101.manifest_gid == 123U);
  CHECK(d101.size == 1000 && d101.download_size == 400);
  CHECK(!d101.is_dlc && !d101.dlc_app_id && !d101.shared_install);
  CHECK(app->depots[1].name == "depot 102");
  CHECK(app->depots[2].language == "german");
  CHECK(app->depots[3].shared_install);
  CHECK(app->depots[4].is_dlc && app->depots[4].dlc_app_id == 999U);
  CHECK(app->depots[5].manifest_gid == 777U);  // old format
  CHECK(app->depots[5].size == 0);

  CHECK((LaunchIds(app->launches) == Strings{"0", "1", "2", "3", "10"}));
  const AppLaunch& l0 = app->launches[0];
  CHECK(l0.executable == "Game.exe");
  CHECK(l0.arguments == "-a \"b c\"");
  CHECK(l0.working_directory == "bin");
  CHECK(l0.type == "default");
  CHECK(l0.os_list == "windows" && l0.os_arch == "64");
  CHECK(l0.description == std::string("Play"));
  CHECK(!l0.beta_key && !l0.required_dlc);
  CHECK(app->launches[2].beta_key == std::string("beta"));
  CHECK(app->launches[3].required_dlc == 999U);
}

void TestParseDefaultsAndFailures() {
  const auto bare = ParseAppInfo(7, "\"appinfo\" { \"appid\" \"7\" }");
  CHECK(bare);
  CHECK(bare->name == "app_7" && bare->type == "?");
  CHECK(bare->depots.empty() && bare->launches.empty());
  CHECK(!ParseAppInfo(7, "\"other\" { }"));
  CHECK(!ParseAppInfo(7, "\"appinfo\" {"));
  CHECK(!ParseAppInfo(7, ""));
  const auto nested = ParseAppInfo(
      7, "\"appinfo\" { \"common\" { \"extended\" { \"listofdlc\" \"5\" } } }");
  CHECK(nested && nested->dlc_app_ids == U32s{5});
}

// A DLC names its game (Iceborne, past Monster Hunter: World's 64-entry listofdlc): extended/dlcforappid,
// common/extended's, whatever the type says; else a DLC's common/parent.
void TestDlcForApp() {
  const auto extended = ParseAppInfo(1118010,
      "\"appinfo\" { \"common\" { \"type\" \"DLC\" \"parent\" \"9\" } \"extended\" { \"dlcforappid\" \"582010\" } }");
  CHECK(extended && extended->dlc_for_app_id == 582010);
  const auto nested = ParseAppInfo(
      8, "\"appinfo\" { \"common\" { \"type\" \"dlc\" \"extended\" { \"dlcforappid\" \"7\" } } }");
  CHECK(nested && nested->dlc_for_app_id == 7);
  const auto parent = ParseAppInfo(
      8, "\"appinfo\" { \"common\" { \"type\" \"DLC\" \"parent\" \"7\" } \"extended\" { \"dlcforappid\" \"0\" } }");
  CHECK(parent && parent->dlc_for_app_id == 7);
  const auto game = ParseAppInfo(8, "\"appinfo\" { \"common\" { \"type\" \"Game\" \"parent\" \"7\" } }");
  CHECK(game && game->dlc_for_app_id == 0);
  const auto own = ParseAppInfo(8, "\"appinfo\" { \"common\" { \"type\" \"DLC\" \"parent\" \"8\" } }");
  CHECK(own && own->dlc_for_app_id == 0);
  // A hidden DLC whose app info carries no common section still names its game.
  const auto hidden = ParseAppInfo(8, "\"appinfo\" { \"extended\" { \"dlcforappid\" \"7\" } }");
  CHECK(hidden && hidden->dlc_for_app_id == 7);
}

void TestSelect() {
  const auto app = ParseAppInfo(kApp, Buffer());
  CHECK(app);
  std::vector<DepotInfo> selected;
  std::string reason;
  CHECK(Select(*app, {kApp}, std::nullopt, "english", &selected, &reason));
  CHECK((Ids(selected) == U32s{101, 106}));
  CHECK(Select(*app, {kApp, 999}, std::nullopt, "english", &selected, &reason));
  CHECK((Ids(selected) == U32s{101, 105, 106}));
  CHECK(Select(*app, {kApp, 999}, std::set<std::uint32_t>{101}, "english", &selected, &reason));
  CHECK((Ids(selected) == U32s{101}));
  CHECK(!Select(*app, {999}, std::nullopt, "english", &selected, &reason));
  CHECK(reason.find("does not own") != std::string::npos);

  // Licenses that grant only the German depot install the game in German.
  CHECK(Select(*app, {kApp}, std::set<std::uint32_t>{102, 103, 104}, "english", &selected, &reason));
  CHECK((Ids(selected) == U32s{103}));
  CHECK(!Select(*app, {kApp}, std::set<std::uint32_t>{102, 104}, "english", &selected, &reason));
  CHECK(reason.find("no downloadable") != std::string::npos);

  AppInfo empty_manifest = *app;
  for (DepotInfo& d : empty_manifest.depots) d.manifest_gid = 0;
  CHECK(!Select(empty_manifest, {kApp, 999}, std::nullopt, "english", &selected, &reason));
  CHECK(reason.find("public branch") != std::string::npos);

  // A depot listed twice is installed once; two manifests for one depot are not installed at all.
  AppInfo duplicate = *app;
  duplicate.depots.push_back(duplicate.depots[0]);
  CHECK(Select(duplicate, {kApp}, std::nullopt, "english", &selected, &reason));
  CHECK((Ids(selected) == U32s{101, 106}));
  duplicate.depots.back().manifest_gid = 124;
  CHECK(!Select(duplicate, {kApp}, std::nullopt, "english", &selected, &reason));
  CHECK(reason.find("conflicting") != std::string::npos);

  // depot_from_app grants access even when the account's depot list omits it.
  AppInfo from_app = *app;
  from_app.depots[0].depot_from_app = 5;
  CHECK(Select(from_app, {kApp}, std::set<std::uint32_t>{106}, "english", &selected, &reason));
  CHECK((Ids(selected) == U32s{101, 106}));

  AppInfo architectures;
  architectures.app_id = kApp;
  DepotInfo win64;
  win64.id = 201;
  win64.os_list = "windows";
  win64.os_arch = "64";
  win64.manifest_gid = 201;
  DepotInfo win32 = win64;
  win32.id = 202;
  win32.os_arch = "32";
  DepotInfo shared = win64;
  shared.id = 203;
  shared.os_arch.clear();
  architectures.depots = {win32, shared, win64};
  CHECK(Select(architectures, {kApp}, std::nullopt, "english", &selected, &reason));
  CHECK((Ids(selected) == U32s{203, 201}));  // listing order: the depot mounting order

  architectures.depots.erase(architectures.depots.begin() + 2);
  CHECK(Select(architectures, {kApp}, std::nullopt, "english", &selected, &reason));
  CHECK((Ids(selected) == U32s{202, 203}));

  architectures.depots.push_back(win64);
  architectures.depots.back().manifest_gid = 0;
  CHECK(Select(architectures, {kApp}, std::nullopt, "english", &selected, &reason));
  CHECK((Ids(selected) == U32s{202, 203}));
}

// The content language picks the localised depots to install beside the language-neutral ones.
void TestDepotLanguage() {
  const auto app = ParseAppInfo(kApp, Buffer());
  CHECK(app);
  std::vector<DepotInfo> selected;
  std::string reason;

  // 101 and 106 carry no language and belong to every one; 103 is the German depot.
  CHECK(Select(*app, {kApp}, std::nullopt, "german", &selected, &reason));
  CHECK((Ids(selected) == U32s{101, 103, 106}));
  CHECK(Select(*app, {kApp}, std::nullopt, "GERMAN", &selected, &reason));
  CHECK((Ids(selected) == U32s{101, 103, 106}));

  // The game has no Russian depot, so it is installed in English rather than without its
  // localised content: asking for a language a game does not carry never installs less.
  CHECK(Select(*app, {kApp}, std::nullopt, "russian", &selected, &reason));
  CHECK((Ids(selected) == U32s{101, 106}));
  // An empty language is English, which is what Playden installed before it could be chosen.
  CHECK(Select(*app, {kApp}, std::nullopt, "", &selected, &reason));
  CHECK((Ids(selected) == U32s{101, 106}));

  // A language depot the licenses do not grant is left out, like any other.
  CHECK(Select(*app, {kApp}, std::set<std::uint32_t>{101, 103}, "german", &selected, &reason));
  CHECK((Ids(selected) == U32s{101, 103}));
  CHECK(Select(*app, {kApp}, std::set<std::uint32_t>{101}, "german", &selected, &reason));
  CHECK((Ids(selected) == U32s{101}));

  CHECK(!Select(*app, {kApp}, std::set<std::uint32_t>{102, 104}, "german", &selected, &reason));
  CHECK(reason.find("no downloadable") != std::string::npos);

  // The effective language and the ones the game carries: neutral content is its English.
  std::string effective;
  std::vector<std::string> languages;
  CHECK(Select(*app, {kApp}, std::nullopt, "russian", &selected, &reason, &effective, &languages));
  CHECK(effective == "english" && (languages == Strings{"english", "german"}));
  CHECK(Select(*app, {kApp}, std::nullopt, "German", &selected, &reason, &effective, &languages));
  CHECK(effective == "german");

  // A game whose only Windows content is localised installs that content.
  AppInfo localised = *app;
  for (DepotInfo& depot : localised.depots)
    if (depot.id == 101) depot.language = "german";
  CHECK(Select(localised, {kApp}, std::nullopt, "german", &selected, &reason));
  CHECK((Ids(selected) == U32s{101, 103, 106}));
  CHECK(Select(localised, {kApp}, std::nullopt, "english", &selected, &reason));
  CHECK((Ids(selected) == U32s{106}));

  // A game with no neutral content installs the first language it lists when it has neither the
  // requested one nor English.
  AppInfo only_localised;
  only_localised.app_id = kApp;
  DepotInfo french;
  french.id = 301;
  french.os_list = "windows";
  french.language = "french";
  french.manifest_gid = 1;
  DepotInfo italian = french;
  italian.id = 302;
  italian.language = "italian";
  only_localised.depots = {french, italian};
  CHECK(Select(only_localised, {kApp}, std::nullopt, "german", &selected, &reason, &effective, &languages));
  CHECK((Ids(selected) == U32s{301}) && effective == "french" && (languages == Strings{"french", "italian"}));
  CHECK(Select(only_localised, {kApp}, std::nullopt, "italian", &selected, &reason, &effective));
  CHECK((Ids(selected) == U32s{302}) && effective == "italian");
  DepotInfo english = french;
  english.id = 303;
  english.language = "english";
  only_localised.depots.push_back(english);
  CHECK(Select(only_localised, {kApp}, std::nullopt, "german", &selected, &reason, &effective));
  CHECK((Ids(selected) == U32s{303}) && effective == "english");

  // A neutral-only game is in whatever language was asked for; there is nothing to choose.
  AppInfo neutral;
  neutral.app_id = kApp;
  DepotInfo content = french;
  content.language.clear();
  neutral.depots = {content};
  CHECK(Select(neutral, {kApp}, std::nullopt, "japanese", &selected, &reason, &effective, &languages));
  CHECK((Ids(selected) == U32s{301}) && effective == "japanese" && languages.empty());

  // A language only another platform, the licenses or a DLC carries is not the game's: it stays
  // in English rather than taking the first language found.
  AppInfo elsewhere = neutral;
  DepotInfo mac_french = french;
  mac_french.id = 310;
  mac_french.os_list = "macos";
  DepotInfo unowned_italian = italian;
  unowned_italian.id = 311;
  DepotInfo dlc_german = french;
  dlc_german.id = 312;
  dlc_german.language = "german";
  dlc_german.dlc_app_id = 999;
  dlc_german.is_dlc = true;
  elsewhere.depots = {content, mac_french, unowned_italian, dlc_german};
  CHECK(Select(elsewhere, {kApp, 999}, std::set<std::uint32_t>{301, 310, 312}, "french", &selected, &reason,
                     &effective, &languages));
  // The DLC's only depot is kept whatever its language.
  CHECK((Ids(selected) == U32s{301, 312}) && effective == "french" && languages.empty());

  // A DLC with several language depots resolves from the requested language, not the game's.
  DepotInfo dlc_english = dlc_german;
  dlc_english.id = 313;
  dlc_english.language = "english";
  DepotInfo dlc_neutral = dlc_german;
  dlc_neutral.id = 314;
  dlc_neutral.language.clear();
  elsewhere.depots = {content, dlc_german, dlc_english, dlc_neutral};
  CHECK(Select(elsewhere, {kApp, 999}, std::nullopt, "german", &selected, &reason, &effective));
  CHECK((Ids(selected) == U32s{301, 312, 314}) && effective == "german");
  CHECK(Select(elsewhere, {kApp, 999}, std::nullopt, "polish", &selected, &reason, &effective));
  CHECK((Ids(selected) == U32s{301, 313, 314}) && effective == "polish");
}

// A partial grant must not install as if complete (issue #58): Team Fortress 2's content depot has
// no oslist or language and comes from a license the account's other one lacks.
void TestMissingBaseDepots() {
  AppInfo app;
  app.app_id = 440;
  DepotInfo content;
  content.id = 441;
  content.manifest_gid = 1;
  content.size = 31'000'000'000ull;
  DepotInfo binaries = content;
  binaries.id = 232251;
  binaries.os_list = "windows";
  binaries.size = 584'000'000ull;
  DepotInfo extra = content;
  extra.id = 450;
  extra.size = 100'000'000ull;
  app.depots = {content, binaries, extra};
  std::vector<DepotInfo> selected;
  std::string reason;

  const std::set<std::uint32_t> partial{232251};
  CHECK(Select(app, {440}, partial, "english", &selected, &reason));
  CHECK((Ids(selected) == U32s{232251}));
  CHECK((MissingBaseDepots(app, partial, selected) == U32s{441, 450}));

  const std::set<std::uint32_t> full{441, 232251};
  CHECK(Select(app, {440}, full, "english", &selected, &reason));
  CHECK((Ids(selected) == U32s{441, 232251}));
  // An unowned depot smaller than the game is an extra, not the game.
  CHECK(MissingBaseDepots(app, full, selected).empty());
  // Unknown licenses, or appinfo without sizes, never block an install.
  CHECK(MissingBaseDepots(app, std::nullopt, selected).empty());
  AppInfo sizeless = app;
  for (DepotInfo& depot : sizeless.depots) depot.size = 0;
  CHECK(Select(sizeless, {440}, partial, "english", &selected, &reason));
  CHECK(MissingBaseDepots(sizeless, partial, selected).empty());

  // Marvel Rivals: the free license grants one content depot; a same-size copy and a third content
  // depot belong to other licenses. Together they outweigh the granted one, which is still the game.
  AppInfo rivals;
  rivals.app_id = 2767030;
  DepotInfo granted = content;
  granted.id = 2767031;
  granted.size = 74'083'193'346ull;
  DepotInfo copy = granted;
  copy.id = 2767032;
  DepotInfo other = granted;
  other.id = 2767033;
  other.size = 20'311'069'758ull;
  rivals.depots = {granted, copy, other};
  const std::set<std::uint32_t> free_license{2767031};
  CHECK(Select(rivals, {2767030}, free_license, "english", &selected, &reason));
  CHECK((Ids(selected) == U32s{2767031}));
  CHECK(MissingBaseDepots(rivals, free_license, selected).empty());

  // Counter-Strike: Source: a license granting the Windows binaries and the 51 KB base depot 209, but not the
  // 4.5 GB content depot 241, is missing the game. The tiny depot is not a copy of the content.
  AppInfo source;
  source.app_id = 240;
  DepotInfo tiny = content;
  tiny.id = 209;
  tiny.size = 51'775;
  DepotInfo source_content = content;
  source_content.id = 241;
  source_content.size = 4'566'652'976ull;
  DepotInfo source_base = content;
  source_base.id = 240;
  source_base.size = 825'745;
  DepotInfo source_binaries = binaries;
  source_binaries.id = 232331;
  source_binaries.size = 321'393'906ull;
  source.depots = {tiny, source_binaries, source_base, source_content};
  const std::set<std::uint32_t> binaries_only{209, 232331};
  CHECK(Select(source, {240}, binaries_only, "english", &selected, &reason));
  CHECK((Ids(selected) == U32s{209, 232331}));
  CHECK((MissingBaseDepots(source, binaries_only, selected) == U32s{240, 241}));
  const std::set<std::uint32_t> whole{209, 232331, 240, 241};
  CHECK(Select(source, {240}, whole, "english", &selected, &reason));
  CHECK(MissingBaseDepots(source, whole, selected).empty());

  // DLC in the selection does not stand in for the game's missing content.
  AppInfo with_dlc = app;
  DepotInfo dlc_depot = binaries;
  dlc_depot.id = 460;
  dlc_depot.dlc_app_id = 459;
  dlc_depot.is_dlc = true;
  dlc_depot.size = 40'000'000'000ull;
  with_dlc.depots.push_back(dlc_depot);
  const std::set<std::uint32_t> partial_with_dlc{232251, 460};
  CHECK(Select(with_dlc, {440, 459}, partial_with_dlc, "english", &selected, &reason));
  CHECK((Ids(selected) == U32s{232251, 460}));
  CHECK((MissingBaseDepots(with_dlc, partial_with_dlc, selected) == U32s{441, 450}));
}

std::string g_fixtures;

AppInfo Fixture(std::uint32_t id, const char* name) {
  const std::string path = g_fixtures + "/" + name;
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) {
    std::fprintf(stderr, "cannot open %s\n", path.c_str());
    std::exit(1);
  }
  std::string text;
  char chunk[4096];
  for (std::size_t got; (got = std::fread(chunk, 1, sizeof(chunk), file)) > 0;) text.append(chunk, got);
  std::fclose(file);
  const auto app = ParseAppInfo(id, text);
  if (!app) {
    std::fprintf(stderr, "cannot parse %s\n", path.c_str());
    std::exit(1);
  }
  return *app;
}

// Every left-out depot says why, and the parser records who lists and owns each depot.
void TestRejectionReasons() {
  const auto app = ParseAppInfo(kApp, Buffer());
  CHECK(app);
  CHECK(app->depots[0].source_app == kApp && app->depots[0].owner_app == kApp);
  CHECK(app->depots[4].source_app == kApp && app->depots[4].owner_app == 999);
  SelectionResult result;
  CHECK(SelectDepots(*app, {}, {kApp}, std::set<std::uint32_t>{101, 102, 103, 104, 105}, DepotSelection{}, &result));
  CHECK((Ids(result.depots) == U32s{101}));
  CHECK(RejectedAs(result, 102) == "os" && RejectedAs(result, 103) == "language" && RejectedAs(result, 104) == "shared" &&
        RejectedAs(result, 105) == "dlc" && RejectedAs(result, 106) == "licence");
  CHECK(result.depots[0].source_app == kApp && result.depots[0].owner_app == kApp);

  // A branch the depots list no manifest for has nothing to install.
  DepotSelection beta;
  beta.branch = "beta";
  CHECK(!SelectDepots(*app, {}, {kApp}, std::nullopt, beta, &result));
  CHECK(RejectedAs(result, 101) == "branch" && result.reason.find("beta branch") != std::string::npos);

  const AppInfo factorio = Fixture(427520, "factorio.vdf");
  CHECK(SelectDepots(factorio, {}, {427520}, std::nullopt, DepotSelection{}, &result));
  CHECK((Ids(result.depots) == U32s{427521}));
  CHECK(RejectedAs(result, 427522) == "arch" && RejectedAs(result, 427523) == "os" && RejectedAs(result, 427524) == "os" &&
        RejectedAs(result, 645391) == "dlc" && RejectedAs(result, 645392) == "os");
  CHECK(result.languages.empty() && result.effective_language == "english");
  // Owning Space Age adds its Windows depot, which the game lists itself.
  CHECK(SelectDepots(factorio, {}, {427520, 645390}, std::nullopt, DepotSelection{}, &result));
  CHECK((Ids(result.depots) == U32s{427521, 645391}));
  CHECK(result.depots[1].owner_app == 645390 && result.depots[1].source_app == 427520);
}

// Portal ships its English as neutral content beside a Russian voice depot.
void TestPortalLanguages() {
  const AppInfo portal = Fixture(400, "portal.vdf");
  SelectionResult result;
  DepotSelection selection;
  selection.language = "french";
  CHECK(SelectDepots(portal, {}, {400}, std::nullopt, selection, &result));
  CHECK((Ids(result.depots) == U32s{401, 403}) && result.effective_language == "english" &&
        (result.languages == Strings{"english", "russian"}) && RejectedAs(result, 402) == "language");
  selection.language = "russian";
  CHECK(SelectDepots(portal, {}, {400}, std::nullopt, selection, &result));
  CHECK((Ids(result.depots) == U32s{401, 402, 403}) && result.effective_language == "russian");

  // Each version carries the size of an install in every language it has, for an install sheet that changes it.
  const auto offers = OfferedBranches(portal, {}, {400}, std::nullopt, "english");
  CHECK(!offers.empty() && offers[0].languages.size() == 2);
  CHECK(offers[0].languages[0].language == "english" && offers[0].languages[0].install_bytes == offers[0].install_bytes &&
        offers[0].install_bytes == 3474712839ull + 37282372ull);
  CHECK(offers[0].languages[1].language == "russian" &&
        offers[0].languages[1].install_bytes == 3474712839ull + 191883542ull + 37282372ull &&
        offers[0].languages[1].download_bytes == 2081548640ull + 115906656ull + 16217568ull);
}

// DLC apps that list their own depots: they follow the game's in listofdlc order, each DLC resolves
// its own language and architecture, and a copy of the game's depot is installed once.
void TestDlcAppDepots() {
  const AppInfo base = Fixture(5000, "dlc-base-5000.vdf");
  const std::vector<AppInfo> dlc = {Fixture(5001, "dlc-app-5001.vdf"), Fixture(5002, "dlc-app-5002.vdf")};
  SelectionResult result;
  DepotSelection german;
  german.language = "german";
  CHECK(SelectDepots(base, dlc, {5000, 5001, 5002}, std::nullopt, german, &result));
  CHECK((Ids(result.depots) == U32s{5001, 5003, 5021, 5023, 5011}));
  CHECK(result.effective_language == "german" && result.languages.empty());
  CHECK(RejectedAs(result, 5022) == "language" && RejectedAs(result, 5012) == "arch");
  const auto copy = std::find_if(result.rejected.begin(), result.rejected.end(),
                                 [](const SelectionResult::Rejected& rejected) { return rejected.id == 5001; });
  CHECK(copy != result.rejected.end() && copy->reason == "duplicate" && copy->source_app == 5002);
  const auto owner = [&result](std::uint32_t id) {
    for (const auto& depot : result.depots)
      if (depot.id == id) return std::make_pair(depot.source_app, depot.owner_app);
    return std::make_pair(0u, 0u);
  };
  CHECK(owner(5001) == std::make_pair(5000u, 5000u) && owner(5003) == std::make_pair(5000u, 5001u) &&
        owner(5021) == std::make_pair(5002u, 5002u) && owner(5011) == std::make_pair(5001u, 5001u));

  // A language no DLC depot carries falls back to English for that DLC.
  DepotSelection polish;
  polish.language = "polish";
  CHECK(SelectDepots(base, dlc, {5000, 5001, 5002}, std::nullopt, polish, &result));
  CHECK((Ids(result.depots) == U32s{5001, 5003, 5022, 5023, 5011}) && result.effective_language == "polish");

  // DLC the account does not own stays out, wherever it is listed.
  CHECK(SelectDepots(base, dlc, {5000, 5002}, std::nullopt, german, &result));
  CHECK((Ids(result.depots) == U32s{5001, 5021, 5023}));
  CHECK(RejectedAs(result, 5003) == "dlc" && RejectedAs(result, 5011) == "dlc" && RejectedAs(result, 5012) == "dlc");

  // The licenses decide for DLC depots too.
  CHECK(SelectDepots(base, dlc, {5000, 5001, 5002}, std::set<std::uint32_t>{5001, 5011, 5023}, german, &result));
  CHECK((Ids(result.depots) == U32s{5001, 5023, 5011}));
  CHECK(RejectedAs(result, 5003) == "licence" && RejectedAs(result, 5021) == "licence");

  // Two manifests for one depot fail the selection.
  std::vector<AppInfo> conflicting = dlc;
  conflicting[1].depots[0].manifest_gid = 99;
  CHECK(!SelectDepots(base, conflicting, {5000, 5001, 5002}, std::nullopt, german, &result));
  CHECK(result.reason.find("conflicting") != std::string::npos && result.depots.empty());

  // The game's listing has no manifest on the branch, a DLC app's copy has: that copy is installed, asked
  // for under the DLC app, in the game's place and still the game's content.
  AppInfo unlisted = base;
  unlisted.depots[0].manifest_gid.reset();
  CHECK(SelectDepots(unlisted, dlc, {5000, 5001, 5002}, std::nullopt, german, &result));
  CHECK((Ids(result.depots) == U32s{5001, 5003, 5021, 5023, 5011}));
  CHECK(result.depots[0].source_app == 5002 && result.depots[0].owner_app == 5000 && result.depots[0].manifest_gid == 51u);
  CHECK(RejectedAs(result, 5001) == "duplicate");
  const auto dropped = std::find_if(result.rejected.begin(), result.rejected.end(),
                                    [](const SelectionResult::Rejected& rejected) { return rejected.id == 5001; });
  CHECK(dropped != result.rejected.end() && dropped->source_app == 5000);
  // Still two usable manifests that differ: conflicting.
  conflicting = dlc;
  conflicting[0].depots.push_back(dlc[1].depots[0]);
  conflicting[0].depots.back().manifest_gid = 99;
  CHECK(!SelectDepots(unlisted, conflicting, {5000, 5001, 5002}, std::nullopt, german, &result));
  CHECK(result.reason.find("conflicting") != std::string::npos);
}

// DLC alone is no version of the game (an update to it would delete the game); content the game takes
// from another app (depotfromapp) is the game's own.
void TestSelectionNeedsTheGamesContent() {
  const AppInfo base = Fixture(5000, "dlc-base-5000.vdf");
  SelectionResult result;
  CHECK(!SelectDepots(base, {}, {5000, 5001}, std::set<std::uint32_t>{5003}, DepotSelection{}, &result));
  CHECK(result.reason == "This version has no content of the game itself.");
  CHECK((Ids(result.depots) == U32s{5003}));
  AppInfo borrowed = base;
  borrowed.depots[0].depot_from_app = 4999;
  CHECK(SelectDepots(borrowed, {}, {5000, 5001}, std::set<std::uint32_t>{5003}, DepotSelection{}, &result));
  CHECK((Ids(result.depots) == U32s{5001, 5003}) && result.depots[0].owner_app == 5000);
  // A branch that carries only the DLC's depot is not offered.
  AppInfo dlc_branch = base;
  dlc_branch.branches.push_back({"dlconly", 9, "", 1, false});
  dlc_branch.depots[1].branches["dlconly"] = BranchManifest{77, 5, 5, false};
  DepotSelection only;
  only.branch = "dlconly";
  CHECK(!SelectDepots(dlc_branch, {}, {5000, 5001}, std::nullopt, only, &result));
  CHECK(result.reason == "This version has no content of the game itself.");
  CHECK(OfferedBranches(dlc_branch, {}, {5000, 5001}, std::nullopt, "english").size() == 1);
}

// A DLC app's own depot never stands in for the game's content the licenses leave out (issue #58):
// one with no oslist and as big as the game still leaves the install refused.
void TestDlcAppDepotDoesNotHideMissingContent() {
  const AppInfo base = Fixture(6000, "free-dlc-base-6000.vdf");
  AppInfo expansion = Fixture(6001, "free-dlc-app-6001.vdf");
  expansion.depots[0].os_list.clear();
  expansion.depots[0].size = 1000;
  const std::set<std::uint32_t> licensed{6011};
  SelectionResult result;
  CHECK(!SelectDepots(base, {expansion}, {6000, 6001}, licensed, {}, &result));
  CHECK(result.reason == "This version has no content of the game itself.");
  CHECK((Ids(result.depots) == U32s{6011}) && RejectedAs(result, 6000) == "licence");
  CHECK((MissingBaseDepots(base, licensed, result.depots) == U32s{6000}));
}

void TestEligibleLaunches() {
  const auto app = ParseAppInfo(kApp, Buffer());
  CHECK(app);
  std::vector<AppLaunch> launches;
  std::string reason;
  CHECK(EligibleLaunches(*app, {kApp}, "public", &launches, &reason));
  CHECK((LaunchIds(launches) == Strings{"0", "10"}));
  CHECK(EligibleLaunches(*app, {kApp, 999}, "public", &launches, &reason));
  CHECK((LaunchIds(launches) == Strings{"0", "3", "10"}));

  AppInfo none = *app;
  none.launches.resize(2);  // "0" is not first any more; keep only "3" and "10"-less set
  none.launches.clear();
  CHECK(!EligibleLaunches(none, {kApp}, "", &launches, &reason));
  CHECK(reason.find("launch configuration") != std::string::npos);

  AppInfo reordered = *app;
  reordered.launches[0].type = "";
  reordered.launches[3].type = "Default";  // launch "3" sorts first despite its id
  CHECK(EligibleLaunches(reordered, {kApp, 999}, "", &launches, &reason));
  CHECK((LaunchIds(launches) == Strings{"3", "0", "10"}));

  // A level editor or dedicated server entry is not the game.
  AppInfo with_editor = *app;
  with_editor.launches[3].type = "editor";
  CHECK(EligibleLaunches(with_editor, {kApp, 999}, "", &launches, &reason));
  CHECK((LaunchIds(launches) == Strings{"0", "10"}));
  with_editor.launches[0].type = "Server";
  CHECK(EligibleLaunches(with_editor, {kApp, 999}, "", &launches, &reason));
  CHECK((LaunchIds(launches) == Strings{"10"}));

  // An untyped dedicated server or relay is left out while the game is listed too (#318), never when it's all there is.
  for (const char* name : {"hlds.exe", "HLTV.EXE", "bin\\srcds_win64.exe", "DedicatedServer.exe", "Game_Server.exe",
                           "MyGameServer.exe", "HLServer.exe", "server.exe"})
    CHECK(IsDedicatedServerProgram(name));
  for (const char* name : {"cof.exe", "hl.exe", "Observer.exe", "hl2.exe", "Serverless Quest.exe", "", ".exe"})
    CHECK(!IsDedicatedServerProgram(name));
  AppInfo servers = *app;
  servers.launches[0].executable = "hlds.exe";
  servers.launches[1].executable = "hltv.exe";
  servers.launches[3].description = "Start a Dedicated Server";
  CHECK(EligibleLaunches(servers, {kApp, 999}, "", &launches, &reason));
  CHECK((LaunchIds(launches) == Strings{"10"}));
  servers.launches.resize(1);
  CHECK(EligibleLaunches(servers, {kApp}, "", &launches, &reason));
  CHECK((LaunchIds(launches) == Strings{"0"}) && launches[0].executable == "hlds.exe");

  // Steam's betakey is a comma list of branches.
  AppInfo listed = *app;
  listed.launches[2].beta_key = "build_debug, Public ,latest\t";
  bool any_branch = true;
  CHECK(EligibleLaunches(listed, {kApp}, "", &launches, &reason, &any_branch));
  CHECK((LaunchIds(launches) == Strings{"0", "2", "10"}) && !any_branch);
  listed.launches[2].beta_key = "default, valve_review";
  CHECK(EligibleLaunches(listed, {kApp}, "", &launches, &reason));
  CHECK((LaunchIds(launches) == Strings{"0", "2", "10"}));
  CHECK(!BetaKeyMatchesPublic("publicbeta") && !BetaKeyMatchesPublic("debug") && BetaKeyMatchesPublic(" "));

  // Options only for other branches (GUILTY GEAR -STRIVE-: "debug"; RuneScape: Dragonwilds):
  // every branch's options, still without DLC the account lacks.
  AppInfo branches = *app;
  for (auto& launch : branches.launches) launch.beta_key = "debug";
  CHECK(EligibleLaunches(branches, {kApp}, "", &launches, &reason, &any_branch));
  CHECK((LaunchIds(launches) == Strings{"0", "2", "10"}) && any_branch);
  CHECK(EligibleLaunches(branches, {kApp, 999}, "", &launches, &reason, &any_branch));
  CHECK((LaunchIds(launches) == Strings{"0", "2", "3", "10"}) && any_branch);
}

// Versions: each branch installs its own manifests, with no fallback to the public ones.
void TestVersions() {
  const AppInfo app = Fixture(7000, "versions-7000.vdf");
  CHECK(app.branches.size() == 6);
  const auto branch = [&](const char* name) -> const AppBranch& {
    for (const AppBranch& item : app.branches)
      if (item.name == name) return item;
    std::fprintf(stderr, "no branch %s\n", name);
    std::exit(1);
  };
  CHECK(branch("4.04").description == "Patch 4.04 (pre next-gen)" && branch("4.04").time_updated == 1670000000 &&
        branch("4.04").build_id == 4040 && !branch("4.04").pwd_required);
  CHECK(branch("beta").pwd_required && branch("public").time_updated == 1760000000);

  // Parsed manifests: public in the depot's fields, every other branch by name; old format and encrypted ones too.
  const DepotInfo& content = app.depots[0];
  CHECK(content.manifest_gid == 1001U && content.size == 30000000000ull && content.download_size == 20000000000ull);
  CHECK(content.branches.count("public") == 0);
  CHECK(content.branches.at("4.04").gid == 1404 && content.branches.at("4.04").size == 25000000000ull &&
        content.branches.at("4.04").download == 18000000000ull && !content.branches.at("4.04").encrypted);
  CHECK(content.branches.at("legacy").gid == 1300 && content.branches.at("legacy").size == 0);
  CHECK(content.branches.at("secret").encrypted && content.branches.at("secret").gid == 0);
  CHECK(!app.depots[2].manifest_gid && app.depots[2].branches.at("4.04").gid == 3404);

  SelectionResult result;
  CHECK(SelectDepots(app, {}, {7000}, std::nullopt, DepotSelection{}, &result));
  CHECK((Ids(result.depots) == U32s{7001, 7002}) && RejectedAs(result, 7003) == "branch" && RejectedAs(result, 7004) == "os");

  // 4.04 takes its own manifests and sizes; the DX12 depot has none there and is not taken from public.
  CHECK(SelectDepots(app, {}, {7000}, std::nullopt, DepotSelection{"", "4.04"}, &result));
  CHECK((Ids(result.depots) == U32s{7001, 7003}) && RejectedAs(result, 7002) == "branch");
  CHECK(result.depots[0].manifest_gid == 1404U && result.depots[0].size == 25000000000ull &&
        result.depots[0].download_size == 18000000000ull);
  CHECK(result.depots[1].manifest_gid == 3404U && result.depots[1].size == 300000000ull);
  // Branch names match whatever their case.
  CHECK(SelectDepots(app, {}, {7000}, std::nullopt, DepotSelection{"", "Legacy"}, &result));
  CHECK((Ids(result.depots) == U32s{7001}) && result.depots[0].manifest_gid == 1300U);

  // Encrypted manifests need the password: nothing to install. A Mac-only branch has no Windows content.
  CHECK(!SelectDepots(app, {}, {7000}, std::nullopt, DepotSelection{"", "secret"}, &result));
  CHECK(RejectedAs(result, 7001) == "branch" && RejectedAs(result, 7002) == "branch");
  CHECK(!SelectDepots(app, {}, {7000}, std::nullopt, DepotSelection{"", "macold"}, &result));
  CHECK(!SelectDepots(app, {}, {7000}, std::nullopt, DepotSelection{"", "nothing"}, &result));
  CHECK(result.reason.find("nothing branch") != std::string::npos);

  // Offers: public first, then usable branches without a password, most recently updated first.
  const auto offers = OfferedBranches(app, {}, {7000}, std::nullopt, "english");
  Strings names;
  for (const auto& offer : offers) names.push_back(offer.name);
  CHECK((names == Strings{"public", "4.04", "legacy"}));
  CHECK(offers[0].build_id == 9000 && offers[0].install_bytes == 30400000000ull &&
        offers[0].download_bytes == 20200000000ull);
  CHECK(offers[1].description == "Patch 4.04 (pre next-gen)" && offers[1].time_updated == 1670000000 &&
        offers[1].build_id == 4040 && offers[1].install_bytes == 25300000000ull &&
        offers[1].download_bytes == 18150000000ull);
  // Licenses: a branch whose only usable depot the account doesn't hold is not offered.
  const auto licensed = OfferedBranches(app, {}, {7000}, std::set<std::uint32_t>{7002, 7003}, "english");
  names.clear();
  for (const auto& offer : licensed) names.push_back(offer.name);
  CHECK((names == Strings{"public", "4.04"}));
  // A game with branches but no other usable one offers public alone.
  AppInfo single = app;
  for (DepotInfo& depot : single.depots) depot.branches.clear();
  CHECK(OfferedBranches(single, {}, {7000}, std::nullopt, "english").size() == 1);

  // Launch options follow the branch: "default" is public's, "4.04" the branch's.
  std::vector<AppLaunch> launches;
  std::string reason;
  bool any_branch = true;
  CHECK(EligibleLaunches(app, {7000}, "public", &launches, &reason, &any_branch));
  CHECK(launches.size() == 1 && launches[0].executable == "bin/x64_dx12/game.exe" && !any_branch);
  CHECK(EligibleLaunches(app, {7000}, "4.04", &launches, &reason, &any_branch));
  CHECK(launches.size() == 1 && launches[0].executable == "bin/x64/game.exe" && !any_branch);
  // A branch no option names: every branch's options, as before.
  CHECK(EligibleLaunches(app, {7000}, "legacy", &launches, &reason, &any_branch));
  CHECK(launches.size() == 2 && any_branch);
  CHECK(BetaKeyMatchesBranch("debug, 4.04", "4.04") && !BetaKeyMatchesBranch("default", "4.04") &&
        BetaKeyMatchesBranch("default", "") && BetaKeyMatchesBranch("", "beta") && !BetaKeyMatchesBranch("4.04", "public"));
}

void TestWindowsArguments() {
  using Args = std::vector<std::string>;
  CHECK(ParseWindowsArguments("-a \"b c\"") == Args({"-a", "b c"}));
  CHECK(ParseWindowsArguments("") == Args{});
  CHECK(ParseWindowsArguments("   \t ") == Args{});
  CHECK(ParseWindowsArguments("a  b\tc") == Args({"a", "b", "c"}));
  CHECK(ParseWindowsArguments("a\\\\\\\"b") == Args({"a\\\"b"}));   // a\\\"b -> a\"b
  CHECK(ParseWindowsArguments("a\\\\\"b c\"") == Args({"a\\b c"}));  // 2 slashes + quote
  CHECK(ParseWindowsArguments("a\\\\b") == Args({"a\\\\b"}));        // slashes w/o quote stay
  CHECK(ParseWindowsArguments("\"a \"\"b\"\" c\"") == Args({"a \"b\" c"}));
  CHECK(ParseWindowsArguments("\"\"") == Args({""}));
  CHECK(ParseWindowsArguments("\"abc") == Args({"abc"}));
  CHECK(ParseWindowsArguments("x\\") == Args({"x\\"}));
  CHECK(!ParseWindowsArguments(std::string("a\0b", 3)));
  CHECK(!ParseWindowsArguments(std::string(32768, 'a')));
}

void TestRelativeGamePath() {
  CHECK(!RelativeGamePath("/abs"));
  CHECK(!RelativeGamePath("\\abs"));
  CHECK(!RelativeGamePath("C:\\x"));
  CHECK(!RelativeGamePath("..\\x"));
  CHECK(!RelativeGamePath("a/../b"));
  CHECK(!RelativeGamePath("a\tb"));
  CHECK(!RelativeGamePath(std::string("a\0b", 3)));
  CHECK(!RelativeGamePath("a\x7f"));
  CHECK(!RelativeGamePath("a\xc2\x85"));
  CHECK(!RelativeGamePath(".gn-download/x"));
  CHECK(!RelativeGamePath(""));
  CHECK(!RelativeGamePath("."));
  CHECK(RelativeGamePath("bin\\Game.exe") == std::string("bin/Game.exe"));
  CHECK(RelativeGamePath(".\\bin//./Game.exe") == std::string("bin/Game.exe"));
  CHECK(RelativeGamePath("caf\xc3\xa9/x") == std::string("caf\xc3\xa9/x"));
  CHECK(RelativeGamePath(".", true) == std::string("."));
  CHECK(RelativeGamePath("", true) == std::string("."));
  CHECK(!RelativeGamePath("/", false));
}

void TestOsList() {
  CHECK(OsListMatchesWindows(""));
  CHECK(OsListMatchesWindows("windows"));
  CHECK(OsListMatchesWindows("macos, Windows"));
  CHECK(OsListMatchesWindows("linux,windows"));
  CHECK(!OsListMatchesWindows("macos,linux"));
  CHECK(!OsListMatchesWindows("windows10"));
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: steam_app_info_test <fixtures/steam/appinfo>\n");
    return 2;
  }
  g_fixtures = argv[1];
  TestParse();
  TestParseDefaultsAndFailures();
  TestDlcForApp();
  TestSelect();
  TestDepotLanguage();
  TestMissingBaseDepots();
  TestDlcAppDepotDoesNotHideMissingContent();
  TestRejectionReasons();
  TestPortalLanguages();
  TestDlcAppDepots();
  TestSelectionNeedsTheGamesContent();
  TestEligibleLaunches();
  TestVersions();
  TestWindowsArguments();
  TestRelativeGamePath();
  TestOsList();
  std::puts("steam_app_info_test: ok");
  return 0;
}
