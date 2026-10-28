#include "steam/backend/app_info.h"

#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
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
  CHECK(app->branches.size() == 1 && app->branches[0].first == "public" && app->branches[0].second == 42);

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

void TestSelectDepots() {
  const auto app = ParseAppInfo(kApp, Buffer());
  CHECK(app);
  std::vector<DepotInfo> selected;
  std::string reason;
  CHECK(SelectDepots(*app, {kApp}, std::nullopt, &selected, &reason));
  CHECK((Ids(selected) == U32s{101, 106}));
  CHECK(SelectDepots(*app, {kApp, 999}, std::nullopt, &selected, &reason));
  CHECK((Ids(selected) == U32s{101, 105, 106}));
  CHECK(SelectDepots(*app, {kApp, 999}, std::set<std::uint32_t>{101}, &selected, &reason));
  CHECK((Ids(selected) == U32s{101}));
  CHECK(!SelectDepots(*app, {999}, std::nullopt, &selected, &reason));
  CHECK(reason.find("does not own") != std::string::npos);

  CHECK(!SelectDepots(*app, {kApp}, std::set<std::uint32_t>{102, 103, 104}, &selected, &reason));
  CHECK(reason.find("no downloadable") != std::string::npos);

  AppInfo empty_manifest = *app;
  for (DepotInfo& d : empty_manifest.depots) d.manifest_gid = 0;
  CHECK(!SelectDepots(empty_manifest, {kApp, 999}, std::nullopt, &selected, &reason));
  CHECK(reason.find("public branch") != std::string::npos);

  AppInfo duplicate = *app;
  duplicate.depots.push_back(duplicate.depots[0]);
  CHECK(!SelectDepots(duplicate, {kApp}, std::nullopt, &selected, &reason));

  // depot_from_app grants access even when the account's depot list omits it.
  AppInfo from_app = *app;
  from_app.depots[0].depot_from_app = 5;
  CHECK(SelectDepots(from_app, {kApp}, std::set<std::uint32_t>{106}, &selected, &reason));
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
  CHECK(SelectDepots(architectures, {kApp}, std::nullopt, &selected, &reason));
  CHECK((Ids(selected) == U32s{201, 203}));

  architectures.depots.erase(architectures.depots.begin() + 2);
  CHECK(SelectDepots(architectures, {kApp}, std::nullopt, &selected, &reason));
  CHECK((Ids(selected) == U32s{202, 203}));

  architectures.depots.push_back(win64);
  architectures.depots.back().manifest_gid = 0;
  CHECK(SelectDepots(architectures, {kApp}, std::nullopt, &selected, &reason));
  CHECK((Ids(selected) == U32s{202, 203}));
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
  CHECK(SelectDepots(app, {440}, partial, &selected, &reason));
  CHECK((Ids(selected) == U32s{232251}));
  CHECK((MissingBaseDepots(app, partial, selected) == U32s{441, 450}));

  const std::set<std::uint32_t> full{441, 232251};
  CHECK(SelectDepots(app, {440}, full, &selected, &reason));
  CHECK((Ids(selected) == U32s{441, 232251}));
  // An unowned depot smaller than the game is an extra, not the game.
  CHECK(MissingBaseDepots(app, full, selected).empty());
  // Unknown licenses, or appinfo without sizes, never block an install.
  CHECK(MissingBaseDepots(app, std::nullopt, selected).empty());
  AppInfo sizeless = app;
  for (DepotInfo& depot : sizeless.depots) depot.size = 0;
  CHECK(SelectDepots(sizeless, {440}, partial, &selected, &reason));
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
  CHECK(SelectDepots(rivals, {2767030}, free_license, &selected, &reason));
  CHECK((Ids(selected) == U32s{2767031}));
  CHECK(MissingBaseDepots(rivals, free_license, selected).empty());
}

void TestEligibleLaunches() {
  const auto app = ParseAppInfo(kApp, Buffer());
  CHECK(app);
  std::vector<AppLaunch> launches;
  std::string reason;
  CHECK(EligibleLaunches(*app, {kApp}, &launches, &reason));
  CHECK((LaunchIds(launches) == Strings{"0", "10"}));
  CHECK(EligibleLaunches(*app, {kApp, 999}, &launches, &reason));
  CHECK((LaunchIds(launches) == Strings{"0", "3", "10"}));

  AppInfo none = *app;
  none.launches.resize(2);  // "0" is not first any more; keep only "3" and "10"-less set
  none.launches.clear();
  CHECK(!EligibleLaunches(none, {kApp}, &launches, &reason));
  CHECK(reason.find("launch configuration") != std::string::npos);

  AppInfo reordered = *app;
  reordered.launches[0].type = "";
  reordered.launches[3].type = "Default";  // launch "3" sorts first despite its id
  CHECK(EligibleLaunches(reordered, {kApp, 999}, &launches, &reason));
  CHECK((LaunchIds(launches) == Strings{"3", "0", "10"}));

  // A level editor or dedicated server entry is not the game.
  AppInfo with_editor = *app;
  with_editor.launches[3].type = "editor";
  CHECK(EligibleLaunches(with_editor, {kApp, 999}, &launches, &reason));
  CHECK((LaunchIds(launches) == Strings{"0", "10"}));
  with_editor.launches[0].type = "Server";
  CHECK(EligibleLaunches(with_editor, {kApp, 999}, &launches, &reason));
  CHECK((LaunchIds(launches) == Strings{"10"}));

  // Steam's betakey is a comma list of branches.
  AppInfo listed = *app;
  listed.launches[2].beta_key = "build_debug, Public ,latest\t";
  bool any_branch = true;
  CHECK(EligibleLaunches(listed, {kApp}, &launches, &reason, &any_branch));
  CHECK((LaunchIds(launches) == Strings{"0", "2", "10"}) && !any_branch);
  listed.launches[2].beta_key = "default, valve_review";
  CHECK(EligibleLaunches(listed, {kApp}, &launches, &reason));
  CHECK((LaunchIds(launches) == Strings{"0", "2", "10"}));
  CHECK(!BetaKeyMatchesPublic("publicbeta") && !BetaKeyMatchesPublic("debug") && BetaKeyMatchesPublic(" "));

  // Options only for other branches (GUILTY GEAR -STRIVE-: "debug"; RuneScape: Dragonwilds):
  // every branch's options, still without DLC the account lacks.
  AppInfo branches = *app;
  for (auto& launch : branches.launches) launch.beta_key = "debug";
  CHECK(EligibleLaunches(branches, {kApp}, &launches, &reason, &any_branch));
  CHECK((LaunchIds(launches) == Strings{"0", "2", "10"}) && any_branch);
  CHECK(EligibleLaunches(branches, {kApp, 999}, &launches, &reason, &any_branch));
  CHECK((LaunchIds(launches) == Strings{"0", "2", "3", "10"}) && any_branch);
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

int main() {
  TestParse();
  TestParseDefaultsAndFailures();
  TestSelectDepots();
  TestMissingBaseDepots();
  TestEligibleLaunches();
  TestWindowsArguments();
  TestRelativeGamePath();
  TestOsList();
  std::puts("steam_app_info_test: ok");
  return 0;
}
