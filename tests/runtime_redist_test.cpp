#include "runtime/artifact_hash.h"
#include "runtime/burn_bundle.h"
#include "runtime/install_script.h"
#include "runtime/msi_package.h"
#include "runtime/redist_install.h"

#include "msi_fixture.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <sys/resource.h>
#include <unistd.h>
#include <vector>

#define CHECK(cond)                                                                     \
  do {                                                                                  \
    if (!(cond)) {                                                                      \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);     \
      std::exit(1);                                                                     \
    }                                                                                   \
  } while (0)

using namespace playden::runtime;
using namespace playden::test::msi;

namespace {

// A package like NVIDIA's legacy PhysX one: a 32-bit loader in Vendor\Common (added to PATH),
// an engine in Vendor\Engine\v1 found through a registry value, a 64-bit feature turned on by
// VersionNT64, a feature left off, and files in the two-block cabinet above (beside the package for
// kExternalCabinet, as data.cab).
enum class Variant { kNormal, kWrongSize, kUncompressed, kExternalCabinet };

std::string SamplePackage(Variant variant = Variant::kNormal) {
  std::vector<TableSpec> tables;
  tables.push_back({"Property", {{"Property", 's'}, {"Value", 's'}},
                    {{"ProductCode", "{11111111-2222-3333-4444-555555555555}"}, {"ProductName", "Sample Runtime"},
                     {"ProductVersion", "1.2.3"}, {"P_VERSION", "7"}, {"D_FIXED", "C:\\Fixed\\Place\\"}}});
  tables.push_back({"Directory", {{"Directory", 's'}, {"Directory_Parent", 's'}, {"DefaultDir", 's'}},
                    {{"TARGETDIR", "", "SourceDir"}, {"ProgramFilesFolder", "TARGETDIR", "."},
                     {"D_VENDOR", "ProgramFilesFolder", "vendor~1|Vendor Runtime"}, {"D_COMMON", "D_VENDOR", "Common"},
                     {"D_ENGINE", "D_VENDOR", "Engine"}, {"D_V1", "D_ENGINE", "v1~1|v1"},
                     {"D_DESKTOP", "DesktopFolder", "Sample"}, {"D_FIXED", "ProgramFilesFolder", "Wrong"}}});
  tables.push_back({"Feature", {{"Feature", 's'}, {"Feature_Parent", 's'}, {"Level", 'i'}},
                    {{"F_Main", "", "1"}, {"F_64", "", "0"}, {"F_Off", "", "0"}, {"F_Child", "F_Off", "1"}}});
  tables.push_back({"Condition", {{"Feature_", 's'}, {"Level", 'i'}, {"Condition", 's'}},
                    {{"F_64", "1", "VersionNT64"}, {"F_Off", "1", "NOT VersionNT64"}}});
  tables.push_back({"Component",
                    {{"Component", 's'}, {"ComponentId", 's'}, {"Directory_", 's'}, {"Attributes", 'i'}, {"Condition", 's'}, {"KeyPath", 's'}},
                    {{"C_LOADER", "{A}", "D_COMMON", "0", "", "F_LOADER"},
                     {"C_ENGINE", "{B}", "D_V1", "0", "VersionNT >= 501", "F_ENGINE"},
                     {"C_REG", "{C}", "D_COMMON", "4", "", ""},
                     {"C_REG64", "{D}", "D_COMMON", "260", "", ""},
                     {"C_ENV", "{E}", "D_COMMON", "0", "", ""},
                     {"C_OFF", "{F}", "D_COMMON", "0", "", "F_OFF"},
                     {"C_SHORTCUT", "{G}", "D_DESKTOP", "0", "", "F_EMPTY"}}});
  tables.push_back({"FeatureComponents", {{"Feature_", 's'}, {"Component_", 's'}},
                    {{"F_Main", "C_LOADER"}, {"F_Main", "C_ENGINE"}, {"F_Main", "C_REG"}, {"F_64", "C_REG64"},
                     {"F_Main", "C_ENV"}, {"F_Child", "C_OFF"}, {"F_Main", "C_SHORTCUT"}}});
  tables.push_back({"File",
                    {{"File", 's'}, {"Component_", 's'}, {"FileName", 's'}, {"FileSize", 'l'}, {"Version", 's'},
                     {"Language", 's'}, {"Attributes", 'i'}, {"Sequence", 'i'}},
                    {{"first", "C_LOADER", "loader~1.dll|Loader.dll", variant == Variant::kWrongSize ? "9" : "10", "", "",
                      variant == Variant::kUncompressed ? "8192" : "512", "1"},
                     {"second", "C_ENGINE", "Core.dll", "10", "", "", "512", "2"},
                     {"empty", "C_ENGINE", "Empty.dat", "0", "", "", "512", "3"},
                     {"off", "C_OFF", "Off.dll", "10", "", "", "512", "4"},
                     {"link", "C_SHORTCUT", "Link.txt", "0", "", "", "512", "5"}}});
  tables.push_back({"Media", {{"DiskId", 'i'}, {"LastSequence", 'i'}, {"DiskPrompt", 's'}, {"Cabinet", 's'}},
                    {{"1", "5", "", variant == Variant::kExternalCabinet ? "data.cab" : "#data.cab"}}});
  tables.push_back({"Registry",
                    {{"Registry", 's'}, {"Root", 'i'}, {"Key", 's'}, {"Name", 's'}, {"Value", 's'}, {"Component_", 's'}},
                    {{"r1", "2", "SOFTWARE\\Vendor", "Core Path", "[D_ENGINE]", "C_REG"},
                     {"r1b", "2", "SOFTWARE\\Vendor", "Core Path Short", "[D_VENDOR]Engine", "C_REG"},
                     {"r2", "2", "SOFTWARE\\Vendor", "Version", "#[P_VERSION]", "C_REG"},
                     {"r3", "2", "SOFTWARE\\Vendor", "Build", "#1", "C_REG64"},
                     {"r4", "2", "SOFTWARE\\Vendor", "+", "", "C_REG"},
                     {"r5", "2", "SOFTWARE\\Vendor", "Blob", "#x0102", "C_REG"},
                     {"r6", "2", "SOFTWARE\\Vendor\\Engines", "1.0", "#5", "C_ENGINE"},
                     {"r7", "1", "Software\\Vendor", "User", "[#first]", "C_REG"},
                     {"r8", "2", "SOFTWARE\\Vendor", "Hidden", "x", "C_OFF"},
                     {"r9", "2", "SOFTWARE\\Vendor", "Fixed", "[D_FIXED]", "C_REG"},
                     {"r10", "2", "SOFTWARE\\Vendor", "Bad\"Name", "x", "C_REG"}}});
  tables.push_back({"Environment", {{"Environment", 's'}, {"Name", 's'}, {"Value", 's'}, {"Component_", 's'}},
                    {{"e1", "=-*PATH", "[D_COMMON];[~]", "C_ENV"}, {"e2", "=-*OTHER", "x", "C_ENV"}}});
  tables.push_back({"CustomAction", {{"Action", 's'}, {"Type", 'i'}, {"Source", 's'}, {"Target", 's'}},
                    {{"CA1", "1", "Binary1", "Run"}}});
  if (variant == Variant::kExternalCabinet) return Package(tables, {});
  return Package(tables, {{"data.cab", TwoBlockCabinet()}});
}

const RegistryValue* FindValue(const std::vector<RegistryValue>& values, const std::string& key, const std::string& name) {
  for (const auto& value : values)
    if (value.key == key && value.name == name) return &value;
  return nullptr;
}

std::string ReadText(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

void WriteText(const std::filesystem::path& path, const std::string& text) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << text;
}

void TestConditions() {
  const std::map<std::string, std::string> properties = {{"VersionNT", "601"}, {"VersionNT64", "601"}, {"Name", "Abc"}};
  CHECK(EvaluateMsiCondition("", properties) == true);
  CHECK(EvaluateMsiCondition("VersionNT64", properties) == true);
  CHECK(EvaluateMsiCondition("NOT VersionNT64", properties) == false);
  CHECK(EvaluateMsiCondition("Installed", properties) == false);
  CHECK(EvaluateMsiCondition("VersionNT >= 501 AND (Missing OR VersionNT64)", properties) == true);
  CHECK(EvaluateMsiCondition("VersionNT < 600 OR Name = \"Abc\"", properties) == true);
  CHECK(EvaluateMsiCondition("Name ~= \"abc\"", properties) == true);
  CHECK(EvaluateMsiCondition("Name = \"abc\"", properties) == false);
  CHECK(EvaluateMsiCondition("Name >< \"b\" AND Name << \"A\" AND Name >> \"c\"", properties) == true);
  CHECK(EvaluateMsiCondition("&Feature = 3", properties) == false);
  CHECK(!EvaluateMsiCondition("VersionNT >", properties).has_value());
  CHECK(!EvaluateMsiCondition("(VersionNT", properties).has_value());
  // A property set to "0" is set; an integer against missing text compares false (but <>).
  CHECK(EvaluateMsiCondition("Zero", {{"Zero", "0"}}) == true);
  CHECK(EvaluateMsiCondition("0", properties) == false);
  CHECK(EvaluateMsiCondition("Missing < 1", properties) == false);
  CHECK(EvaluateMsiCondition("Missing <> 1", properties) == true);
  CHECK(EvaluateMsiCondition("Name = 1", properties) == false);
  // Deep nesting is refused rather than recursing without bound.
  std::string deep;
  for (int index = 0; index < 1000; ++index) deep += "NOT ";
  CHECK(!EvaluateMsiCondition(deep + "VersionNT", properties).has_value());
  CHECK(!EvaluateMsiCondition(std::string(1000, '(') + "VersionNT" + std::string(1000, ')'), properties).has_value());
  CHECK(EvaluateMsiCondition("((NOT (VersionNT = 0)))", properties) == true);
}

void TestCabinet() {
  std::map<std::string, std::string> members;
  std::vector<std::string> order;
  std::string reason;
  CHECK(ExtractCabinet(TwoBlockCabinet(), [&](const std::string& name, std::string_view data) {
    members[name] = std::string(data);
    order.push_back(name);
    return true;
  }, &reason));
  CHECK(members["first"] == "0123456789");
  CHECK(members["second"] == "0123456789");  // copied from the previous block's history
  CHECK(members.count("empty") && members["empty"].empty());
  CHECK((order == std::vector<std::string>{"first", "second", "empty"}));
  // LZX needs a window of 2^15 to 2^21 bytes, Quantum is refused by name; a sink that stops ends extraction.
  auto lzx = TwoBlockCabinet();
  lzx[36 + 6] = 3;
  CHECK(!ExtractCabinet(lzx, [](const std::string&, std::string_view) { return true; }, &reason));
  CHECK(reason == "unsupported LZX window size");
  auto quantum = TwoBlockCabinet();
  quantum[36 + 6] = 2;
  CHECK(!ExtractCabinet(quantum, [](const std::string&, std::string_view) { return true; }, &reason));
  CHECK(reason == "Quantum cabinets are not supported");
  CHECK(!ExtractCabinet(TwoBlockCabinet(), [](const std::string&, std::string_view) { return false; }, &reason));
  CHECK(!ExtractCabinet("MSCF", [](const std::string&, std::string_view) { return true; }, &reason));
  // LZX: an uncompressed block over two frames, an odd length, and the x86 call translation undone.
  std::string big(40001, '\0');
  for (std::size_t index = 0; index < big.size(); ++index) big[index] = static_cast<char>('a' + index % 23);
  members.clear();
  CHECK(ExtractCabinet(LzxCabinet({{"big", big}, {"small", "hello"}}), [&](const std::string& name, std::string_view data) {
    members[name] = std::string(data);
    return true;
  }, &reason));
  CHECK(members["big"] == big && members["small"] == "hello");
  // At output position 2 an encoder turned the call's relative target 98 into the absolute 100.
  std::string call = "ab\xe8";
  Put32(&call, 100);
  call += std::string(20, 'z');
  std::string expected = "ab\xe8";
  Put32(&expected, 98);
  expected += std::string(20, 'z');
  members.clear();
  CHECK(ExtractCabinet(LzxCabinet({{"call", call}}, 1000), [&](const std::string& name, std::string_view data) {
    members[name] = std::string(data);
    return true;
  }, &reason));
  CHECK(members["call"] == expected);
  // A stream cut short is damaged, not padded out.
  auto cut = LzxCabinet({{"big", big}});
  cut.resize(cut.size() - 2000);
  CHECK(!ExtractCabinet(cut, [](const std::string&, std::string_view) { return true; }, &reason));
  // Literals of a verbatim block decode. Ones that need bits past the stream's end don't, even when the data block
  // says the stream is that short, and nor does a main tree that leaves a code unused.
  const std::string text = "verbatim literals, eight bits each";
  members.clear();
  CHECK(ExtractCabinet(LzxVerbatimCabinet("text", text), [&](const std::string& name, std::string_view data) {
    members[name] = std::string(data);
    return true;
  }, &reason));
  CHECK(members["text"] == text);
  CHECK(!ExtractCabinet(LzxVerbatimCabinet("text", text, 2), [](const std::string&, std::string_view) { return true; }, &reason));
  CHECK(reason == "damaged LZX data (input ends early)");
  CHECK(!ExtractCabinet(LzxVerbatimCabinet("text", text, 0, 255), [](const std::string&, std::string_view) { return true; },
                        &reason));
  CHECK(reason == "damaged LZX data (main tree)");
  // Only the last frame may be short: after a short one, a full frame would run past the window's end.
  const std::string frames(32769, 'q');
  CHECK(ExtractCabinet(LzxCabinet({{"frames", frames}}, 0, {32768, 1}), [](const std::string&, std::string_view) { return true; },
                       &reason));
  CHECK(!ExtractCabinet(LzxCabinet({{"frames", frames}}, 0, {1, 32768}), [](const std::string&, std::string_view) { return true; },
                        &reason));
  CHECK(reason == "damaged cabinet (LZX frame size)");
  // Members that overlap are refused: "second" moved to offset 5.
  auto overlapping = TwoBlockCabinet();
  const auto second = overlapping.find("second") - 16 + 4;
  overlapping[second] = 5;
  CHECK(!ExtractCabinet(overlapping, [](const std::string&, std::string_view) { return true; }, &reason));
  CHECK(reason == "damaged cabinet: members overlap");
}

void TestPlan() {
  MsiInstallPlan plan;
  std::string reason;
  CHECK(!ReadMsiPackage("not a package", &plan, &reason));
  CHECK(ReadMsiPackage(SamplePackage(), &plan, &reason));
  CHECK(plan.product_code == "{11111111-2222-3333-4444-555555555555}" && plan.product_name == "Sample Runtime");
  CHECK(plan.custom_actions == 1);
  CHECK(plan.cabinets.size() == 1 && plan.cabinets[0].name == "data.cab" && plan.cabinets[0].embedded);
  // The off feature's file and the desktop file stay out; the rest go under Program Files (x86).
  CHECK(plan.files.size() == 3);
  CHECK(plan.files[0].target == "Program Files (x86)/Vendor Runtime/Common/Loader.dll" && plan.files[0].member == "first");
  CHECK(plan.files[1].target == "Program Files (x86)/Vendor Runtime/Engine/v1/Core.dll");
  CHECK(plan.files[2].target == "Program Files (x86)/Vendor Runtime/Engine/v1/Empty.dat");
  CHECK(plan.skipped_files == 1);  // Link.txt, under the per-user desktop
  const auto* path = FindValue(plan.machine, "Software\\Wow6432Node\\Vendor", "Core Path");
  CHECK(path && path->data == "\"C:\\\\Program Files (x86)\\\\Vendor Runtime\\\\Engine\\\\\"");
  const auto* short_path = FindValue(plan.machine, "Software\\Wow6432Node\\Vendor", "Core Path Short");
  CHECK(short_path && short_path->data == "\"C:\\\\Program Files (x86)\\\\Vendor Runtime\\\\Engine\"");
  const auto* version = FindValue(plan.machine, "Software\\Wow6432Node\\Vendor", "Version");
  CHECK(version && version->data == "dword:00000007");
  const auto* build = FindValue(plan.machine, "SOFTWARE\\Vendor", "Build");  // a 64-bit component
  CHECK(build && build->data == "dword:00000001");
  CHECK(FindValue(plan.machine, "Software\\Wow6432Node\\Vendor\\Engines", "1.0"));
  CHECK(!FindValue(plan.machine, "Software\\Wow6432Node\\Vendor", "Hidden"));
  CHECK(!FindValue(plan.machine, "Software\\Wow6432Node\\Vendor", "Blob"));
  const auto* user = FindValue(plan.user, "Software\\Vendor", "User");
  CHECK(user && user->data == "\"C:\\\\Program Files (x86)\\\\Vendor Runtime\\\\Common\\\\Loader.dll\"");
  // A directory the Property table sets keeps that place.
  const auto* fixed = FindValue(plan.machine, "Software\\Wow6432Node\\Vendor", "Fixed");
  CHECK(fixed && fixed->data == "\"C:\\\\Fixed\\\\Place\\\\\"");
  CHECK(plan.unsupported_files == 0);
  CHECK(plan.skipped_registry == 3);  // the binary value, the quoted name and the OTHER variable
  CHECK((plan.path_prepend == std::vector<std::string>{"C:\\Program Files (x86)\\Vendor Runtime\\Common\\"}));
  CHECK(plan.path_append.empty());
}

void TestBurnBundle() {
  std::string reason;
  for (const bool x64 : {true, false})
    for (const auto& [signed_form, with_section] : {std::pair{true, true}, std::pair{false, true}, std::pair{true, false}}) {
      BurnMsiPackage package;
      const auto id = std::string("vcRuntimeAdditional_") + (x64 ? "x64" : "x86");
      const bool ok = ReadBurnMsiPackage(MfcBundle(x64, signed_form, with_section), id, &package, &reason);
      if (!ok) std::fprintf(stderr, "bundle: %s\n", reason.c_str());
      CHECK(ok);
      CHECK(package.msi_name == std::string("vc_runtimeAdditional_") + (x64 ? "x64" : "x86") + ".msi");
      CHECK(package.msi == MfcPackage(x64));
      CHECK(package.beside.size() == 1 && package.beside.at("cab1.cab") == MfcCabinet(x64));
      // The other package of the chain, by its own payloads.
      CHECK(ReadBurnMsiPackage(MfcBundle(x64, signed_form, with_section), std::string("vcRuntimeMinimum_") + (x64 ? "x64" : "x86"),
                               &package, &reason));
      CHECK(package.msi == "minimum-msi" && package.beside.at("cab1.cab") == StoredCabinet({{"vcruntime140.dll", "CRT"}}));
    }
  BurnMsiPackage package;
  CHECK(!ReadBurnMsiPackage(MfcBundle(true), "vcRuntimeAdditional_arm64", &package, &reason));
  CHECK(reason == "the bundle has no package vcRuntimeAdditional_arm64");
  CHECK(!ReadBurnMsiPackage("MZ not a bundle", "x", &package, &reason));
  CHECK(reason == "not a setup bundle Playden can read");
  // A payload that isn't the size the manifest lists is refused.
  auto bundle = MfcBundle(true);
  const auto listed = "FileSize=\"" + std::to_string(MfcCabinet(true).size()) + "\"";
  // The manifest is stored, so its text is in the bundle as is; same length keeps the cabinet valid.
  auto wrong = std::to_string(MfcCabinet(true).size() + 1);
  wrong.resize(std::to_string(MfcCabinet(true).size()).size(), '9');
  const auto at = bundle.find(listed);
  CHECK(at != std::string::npos);
  bundle.replace(at, listed.size(), "FileSize=\"" + wrong + "\"");
  CHECK(!ReadBurnMsiPackage(bundle, "vcRuntimeAdditional_x64", &package, &reason));
  CHECK(reason == "the bundle's copy of cab1.cab is not the size its manifest lists");
  // A cut bundle has no attached container to read.
  const auto whole = MfcBundle(true);
  CHECK(!ReadBurnMsiPackage(whole.substr(0, whole.size() - 200), "vcRuntimeAdditional_x64", &package, &reason));
}

void TestPath() {
  CHECK(ExtendPath("A;B", {"C:\\X"}, {}) == "C:\\X;A;B");
  CHECK(ExtendPath("A;c:\\x\\", {"C:\\X"}, {"D"}) == "A;c:\\x\\;D");
  CHECK(ExtendPath("", {}, {"D"}) == "D");
}

void TestScripts() {
  const std::string script =
      "\"InstallScript\"\n{\n\t\"Run Process\"\n\t{\n\t\t\"PhysX\"\n\t\t{\n"
      "\t\t\t\"HasRunKey\"\t\t\"HKEY_LOCAL_MACHINE\\\\Software\\\\Valve\\\\Steam\\\\Apps\\\\696180\"\n"
      "\t\t\t\"process 1\"\t\t\"msiexec\"\n"
      "\t\t\t\"command 1\"\t\t\"/i \\\"%INSTALLDIR%\\\\_CommonRedist\\\\PhysX\\\\9.13.0604\\\\PhysX.msi\\\" /quiet /norestart\"\n"
      "\t\t}\n\t\t\"DirectX\"\n\t\t{\n\t\t\t\"process 1\"\t\"%INSTALLDIR%\\\\DirectX\\\\DXSETUP.exe\"\n\t\t}\n"
      "\t\t\"Launcher\"\n\t\t{\n\t\t\t\"process 1\"\t\"%INSTALLDIR%\\\\Setup.exe\"\n\t\t}\n"
      "\t\t\"Lang\"\n\t\t{\n\t\t\t\"process 1\"\t\"%LANGUAGE%.exe\"\n\t\t}\n\t}\n}\n"
      "\"kvsignatures\"\n{\n\t\"InstallScript\"\t\t\"8ed8c256\"\n}\n";
  std::vector<RedistEntry> entries;
  std::string reason;
  CHECK(ParseRunProcess(script, "Z:\\data\\games\\696180", &entries, &reason));
  CHECK(entries.size() == 3);
  CHECK(entries[0].name == "PhysX" && entries[0].process == "msiexec");
  CHECK(entries[0].command == "/i \"Z:\\data\\games\\696180\\_CommonRedist\\PhysX\\9.13.0604\\PhysX.msi\" /quiet /norestart");
  CHECK(entries[0].has_run_key == "HKEY_LOCAL_MACHINE\\Software\\Valve\\Steam\\Apps\\696180");
  std::string package;
  CHECK(ClassifyRedist(entries[0], "Z:\\data\\games\\696180", &package) == RedistAction::kMsi);
  CHECK(package == "_CommonRedist/PhysX/9.13.0604/PhysX.msi");
  CHECK(ClassifyRedist(entries[1], "Z:\\data\\games\\696180", &package) == RedistAction::kCovered);
  CHECK(ClassifyRedist(entries[2], "Z:\\data\\games\\696180", &package) == RedistAction::kNotRun);
  // A package outside the game folder is not taken.
  RedistEntry outside{"X", "msiexec", "/i C:\\other\\x.msi", ""};
  CHECK(ClassifyRedist(outside, "Z:\\data\\games\\696180", &package) == RedistAction::kNotRun);
  RedistEntry climbing{"X", "msiexec", "/i Z:\\data\\games\\696180\\..\\x.msi", ""};
  CHECK(ClassifyRedist(climbing, "Z:\\data\\games\\696180", &package) == RedistAction::kNotRun);
  // Steam's signature block after the script doesn't hide its sections.
  InstallScriptValues values;
  CHECK(ParseInstallScript("\"InstallScript\" { \"Registry\" { \"HKEY_LOCAL_MACHINE\\\\Software\\\\Studio\" { \"string\" { "
                           "\"Path\" \"%INSTALLDIR%\" } } } } \"kvsignatures\" { \"InstallScript\" \"00\" }",
                           "Z:\\games\\1", &values, &reason));
  CHECK(values.machine.size() == 2);
  RedistEntry rooted{"X", "msiexec", "/i Z:\\data\\games\\696180\\\\tmp\\x.msi", ""};
  CHECK(ClassifyRedist(rooted, "Z:\\data\\games\\696180", &package) == RedistAction::kNotRun);
  RedistEntry dotted{"X", "msiexec", "/i Z:\\data\\games\\696180\\.\\x.msi", ""};
  CHECK(ClassifyRedist(dotted, "Z:\\data\\games\\696180", &package) == RedistAction::kNotRun);
  RedistEntry direct{"X", "Z:\\data\\games\\696180\\redist\\x.msi", "", ""};
  CHECK(ClassifyRedist(direct, "Z:\\data\\games\\696180", &package) == RedistAction::kMsi && package == "redist/x.msi");
}

void TestInstall() {
  char pattern[] = "/tmp/playden-redist-XXXXXX";
  CHECK(::mkdtemp(pattern));
  const auto base = std::filesystem::canonical(pattern);  // /tmp is a symlink on macOS
  const auto game = base / "game", prefix = base / "prefix";
  WriteText(game / "_CommonRedist/Sample/1.0/installscript.vdf",
            "\"InstallScript\" { \"Run Process\" { \"Sample\" { "
            "\"HasRunKey\" \"HKEY_LOCAL_MACHINE\\\\Software\\\\Valve\\\\Steam\\\\Apps\\\\42\" "
            "\"process 1\" \"msiexec\" "
            "\"command 1\" \"/i \\\"%INSTALLDIR%\\\\_CommonRedist\\\\Sample\\\\1.0\\\\sample.msi\\\" /quiet\" } "
            "\"VC\" { \"process 1\" \"%INSTALLDIR%\\\\_CommonRedist\\\\vcredist_x86.exe\" } } } "
            "\"kvsignatures\" { \"InstallScript\" \"00\" }");
  WriteText(game / "_CommonRedist/Sample/1.0/sample.msi", SamplePackage());
  WriteText(game / "data/deep/installscript.vdf", "\"InstallScript\" { \"Run Process\" { \"Ignored\" { \"process 1\" \"x.exe\" } } }");
  WriteText(prefix / "system.reg",
            "WINE REGISTRY Version 2\n;; All keys relative to \\\\Machine\n\n#arch=win64\n\n"
            "[System\\\\CurrentControlSet\\\\Control\\\\Session Manager\\\\Environment] 1700000000\n"
            "\"PATH\"=str(2):\"%SystemRoot%\\\\system32;%SystemRoot%\"\n\"TEMP\"=str(2):\"%SystemRoot%\\\\temp\"\n");
  WriteText(prefix / "user.reg", "WINE REGISTRY Version 2\n;; All keys relative to \\\\User\\\\S-1-5-21-0-0-0-1000\n\n#arch=win64\n");
  std::filesystem::create_directories(prefix / "drive_c/program files (x86)");
  // A bottle is private to Playden, as on the console.
  std::filesystem::permissions(prefix, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace);
  for (const auto* hive : {"system.reg", "user.reg"})
    std::filesystem::permissions(prefix / hive, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::replace);
  const auto result = InstallRedistributables(game.string(), "Z:\\games\\42", prefix.string(), 1700000100);
  CHECK(result.outcomes.size() == 2);
  CHECK(result.outcomes[0].script == "_CommonRedist/Sample/1.0/installscript.vdf");
  CHECK(result.outcomes[0].name == "Sample" && result.outcomes[0].result == "installed" && result.outcomes[0].files == 3);
  CHECK(result.outcomes[1].name == "VC" && result.outcomes[1].result == "covered");
  // The existing folder is reused whatever its case.
  CHECK(ReadText(prefix / "drive_c/program files (x86)/Vendor Runtime/Common/Loader.dll") == "0123456789");
  CHECK(ReadText(prefix / "drive_c/program files (x86)/Vendor Runtime/Engine/v1/Core.dll") == "0123456789");
  CHECK(std::filesystem::exists(prefix / "drive_c/program files (x86)/Vendor Runtime/Engine/v1/Empty.dat"));
  std::vector<std::string> drive;
  for (const auto& child : std::filesystem::directory_iterator(prefix / "drive_c")) drive.push_back(child.path().filename().string());
  CHECK((drive == std::vector<std::string>{"program files (x86)"}));
  const auto system = ReadText(prefix / "system.reg");
  CHECK(system.find("[Software\\\\Wow6432Node\\\\Vendor]") != std::string::npos);
  CHECK(system.find("\"Core Path\"=\"C:\\\\Program Files (x86)\\\\Vendor Runtime\\\\Engine\\\\\"") != std::string::npos);
  CHECK(system.find("\"PATH\"=str(2):\"C:\\\\Program Files (x86)\\\\Vendor Runtime\\\\Common\\\\;%SystemRoot%\\\\system32;%SystemRoot%\"") !=
        std::string::npos);
  CHECK(system.find("\"TEMP\"=str(2):\"%SystemRoot%\\\\temp\"") != std::string::npos);
  CHECK(system.find("[Software\\\\Wow6432Node\\\\Valve\\\\Steam\\\\Apps\\\\42]") != std::string::npos);
  CHECK(system.find("\"Sample\"=dword:00000001") != std::string::npos);
  CHECK(ReadText(prefix / "user.reg").find("\"User\"=") != std::string::npos);
  CHECK(ReadText(prefix / ".playden-redist").find(" _CommonRedist/Sample/1.0/sample.msi\n") != std::string::npos);
  // Installed once: the next Play leaves the bottle alone.
  const auto again = InstallRedistributables(game.string(), "Z:\\games\\42", prefix.string(), 1700000200);
  CHECK(again.outcomes.size() == 2 && again.outcomes[0].result == "already");
  CHECK(ReadText(prefix / "system.reg") == system);
  // A package whose cabinet doesn't match its file list, or that keeps files beside it, fails and isn't recorded.
  for (const auto variant : {Variant::kWrongSize, Variant::kUncompressed}) {
    std::filesystem::remove(prefix / ".playden-redist");
    WriteText(game / "_CommonRedist/Sample/1.0/sample.msi", SamplePackage(variant));
    const auto refused = InstallRedistributables(game.string(), "Z:\\games\\42", prefix.string(), 1700000250);
    CHECK(refused.outcomes.size() == 2 && refused.outcomes[0].result == "failed");
    CHECK(!std::filesystem::exists(prefix / ".playden-redist"));
  }
  // A damaged package fails alone.
  std::filesystem::remove(prefix / ".playden-redist");
  WriteText(game / "_CommonRedist/Sample/1.0/sample.msi", "broken");
  const auto broken = InstallRedistributables(game.string(), "Z:\\games\\42", prefix.string(), 1700000300);
  CHECK(broken.outcomes.size() == 2 && broken.outcomes[0].result == "failed" && broken.outcomes[1].result == "covered");
  CHECK(broken.outcomes[0].detail.find("not a Windows Installer package") != std::string::npos);
  std::filesystem::remove_all(base);
}

// A game with no install script: PhysX's package is found by name, only when the game needs it.
void TestFoundPhysx() {
  char pattern[] = "/tmp/playden-redist-XXXXXX";
  CHECK(::mkdtemp(pattern));
  const auto base = std::filesystem::canonical(pattern);  // /tmp is a symlink on macOS
  const auto fresh_prefix = [&](const char* name) {
    const auto prefix = base / name;
    WriteText(prefix / "system.reg",
              "WINE REGISTRY Version 2\n;; All keys relative to \\\\Machine\n\n#arch=win64\n");
    WriteText(prefix / "user.reg", "WINE REGISTRY Version 2\n;; All keys relative to \\\\User\\\\S-1-5-21-0-0-0-1000\n\n#arch=win64\n");
    std::filesystem::create_directories(prefix / "drive_c/program files (x86)");
    std::filesystem::permissions(prefix, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace);
    for (const auto* hive : {"system.reg", "user.reg"})
      std::filesystem::permissions(prefix / hive, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                   std::filesystem::perm_options::replace);
    return prefix;
  };
  const auto game = base / "game";
  WriteText(game / "_Redist/Nvidia PhysX Legacy Driver/PhysX-9.13.0604-SystemSoftware-Legacy.msi", SamplePackage());
  WriteText(game / "_Redist/Other/OtherRuntime.msi", SamplePackage());  // named otherwise: left alone
  WriteText(game / "Binaries/PhysX-Tool.msi", SamplePackage());         // not a redistributable folder

  // Not needed: nothing happens.
  auto prefix = fresh_prefix("unneeded");
  auto result = InstallRedistributables(game.string(), "Z:\\games\\1", prefix.string(), 1700000100);
  CHECK(result.outcomes.empty() && !result.physx_missing && !std::filesystem::exists(prefix / ".playden-redist"));

  // Needed: only PhysX's package, once.
  prefix = fresh_prefix("needed");
  std::vector<std::pair<std::size_t, std::size_t>> reported;
  std::string reported_name;
  result = InstallRedistributables(game.string(), "Z:\\games\\1", prefix.string(), 1700000100, true,
                                   [&](const std::string& name, std::size_t done, std::size_t total) {
                                     reported_name = name;
                                     reported.emplace_back(done, total);
                                   });
  // It says it started before reading the package, then counts its files.
  CHECK(reported_name == "PhysX" && reported.size() == 5);
  CHECK(reported.front() == std::make_pair(std::size_t{0}, std::size_t{1}) &&
        reported[1] == std::make_pair(std::size_t{0}, std::size_t{3}) && reported.back() == std::make_pair(std::size_t{3}, std::size_t{3}));
  CHECK(RedistIsPhysx("PhysX") && RedistIsPhysx("PhysX Version") && !RedistIsPhysx("Sample"));
  CHECK(result.outcomes.size() == 1 && result.outcomes[0].result == "installed" && result.outcomes[0].name == "PhysX" &&
        result.outcomes[0].detail == "_Redist/Nvidia PhysX Legacy Driver/PhysX-9.13.0604-SystemSoftware-Legacy.msi");
  CHECK(!result.physx_missing);
  CHECK(ReadText(prefix / "drive_c/program files (x86)/Vendor Runtime/Common/Loader.dll") == "0123456789");
  reported.clear();
  result = InstallRedistributables(game.string(), "Z:\\games\\1", prefix.string(), 1700000200, true,
                                   [&](const std::string&, std::size_t done, std::size_t total) { reported.emplace_back(done, total); });
  CHECK(result.outcomes.size() == 1 && result.outcomes[0].result == "already" && reported.empty());

  // The game's files in one folder of what was imported: its redistributables are a level down.
  const auto wrapped = base / "wrapped";
  WriteText(wrapped / "My Game/_Redist/PhysX.msi", SamplePackage());
  WriteText(wrapped / "My Game/Data/Deep/_Redist/PhysX.msi", "broken");  // too far down to be the game's
  prefix = fresh_prefix("wrapped-prefix");
  result = InstallRedistributables(wrapped.string(), "Z:\\games\\2", prefix.string(), 1700000100, true);
  CHECK(result.outcomes.size() == 1 && result.outcomes[0].detail == "My Game/_Redist/PhysX.msi" &&
        result.outcomes[0].result == "installed");

  // A script that names PhysX has said which package the game wants.
  const auto scripted = base / "scripted";
  WriteText(scripted / "_CommonRedist/installscript.vdf",
            "\"InstallScript\" { \"Run Process\" { \"PhysX\" { \"process 1\" \"msiexec\" "
            "\"command 1\" \"/i \\\"%INSTALLDIR%\\\\_CommonRedist\\\\Chosen.msi\\\" /quiet\" } } } "
            "\"kvsignatures\" { \"InstallScript\" \"00\" }");
  WriteText(scripted / "_CommonRedist/Chosen.msi", SamplePackage());
  WriteText(scripted / "_CommonRedist/PhysX-Other.msi", "broken");
  prefix = fresh_prefix("scripted-prefix");
  result = InstallRedistributables(scripted.string(), "Z:\\games\\3", prefix.string(), 1700000100, true);
  CHECK(result.outcomes.size() == 1 && result.outcomes[0].detail == "_CommonRedist/Chosen.msi" && !result.physx_searched);

  // Which version a game was built for, and whether a bottle has it.
  {
    const auto versioned = base / "versioned";
    std::string resource("MZ", 2);
    for (const char value : std::string("VS_VERSION_INFO")) { resource += value; resource += '\0'; }
    resource += std::string(6, '\0');
    resource += std::string("\xBD\x04\xEF\xFE\x00\x00\x01\x00", 8);       // signature, struct version
    resource += std::string("\x08\x00\x02\x00\x0F\x00\x01\x00", 8);       // 2.8 and 1.15, low words first
    WriteText(versioned / "Binaries/NxCooking.dll", resource);
    WriteText(versioned / "Binaries/Game.exe", "MZ");
    CHECK(PhysxVersionBeside(versioned.string(), "Binaries/Game.exe") == "2.8.1");
    CHECK(!PhysxEngineBeside(versioned.string(), "Binaries/Game.exe"));
    WriteText(versioned / "Other/NxCooking.dll", "no version");
    CHECK(PhysxVersionBeside(versioned.string(), "Other/Game.exe").empty());
    std::filesystem::create_directories(versioned / "Local/PhysXLocal");
    CHECK(PhysxEngineBeside(versioned.string(), "Local/Game.exe"));
    const auto bottle = base / "engines";
    CHECK(!BottleHasPhysxEngine(bottle.string(), "2.8.1") && !BottleHasPhysxEngine(bottle.string(), ""));
    WriteText(bottle / "drive_c/Program Files (x86)/NVIDIA Corporation/PhysX/Engine/v2.7.2/PhysXCore.dll", "core");
    CHECK(!BottleHasPhysxEngine(bottle.string(), "2.8.1") && BottleHasPhysxEngine(bottle.string(), "2.7.2") &&
          BottleHasPhysxEngine(bottle.string(), ""));
    WriteText(bottle / "drive_c/Program Files (x86)/AGEIA Technologies/v2.8.1/PhysXCore.dll", "core");
    CHECK(BottleHasPhysxEngine(bottle.string(), "2.8.1"));
    // Every program counts: one with its engine doesn't hide another that lacks its own.
    const auto uses = [](const std::string& program) { return program != "Tools/Plain.exe"; };
    std::string older = resource;
    older[older.size() - 8] = '\x07';  // 2.7
    older[older.size() - 2] = '\x05';  // .5
    WriteText(versioned / "Older/NxCooking.dll", older);
    CHECK(PhysxVersionBeside(versioned.string(), "Older/Game.exe") == "2.7.5");
    CHECK((MissingPhysxEngine(versioned.string(), {"Tools/Plain.exe", "Binaries/Game.exe", "Local/Game.exe", "Older/Game.exe"},
                              bottle.string(), uses) == std::optional<std::string>("2.7.5")));
    CHECK(!MissingPhysxEngine(versioned.string(), {"Tools/Plain.exe", "Binaries/Game.exe", "Local/Game.exe"}, bottle.string(), uses));
  }

  // What the check found stays in the bottle for the next Playden start.
  {
    const auto note = fresh_prefix("note-prefix");
    CHECK(!RecordedPhysxNeeded(note.string()));
    CHECK(RecordPhysxNeeded(note.string(), std::string("2.8.1")) && RecordedPhysxNeeded(note.string()) == std::optional<std::string>("2.8.1"));
    CHECK(RecordPhysxNeeded(note.string(), std::string()) && RecordedPhysxNeeded(note.string()) == std::optional<std::string>(""));
    CHECK(RecordPhysxNeeded(note.string(), std::nullopt) && !RecordedPhysxNeeded(note.string()));
    CHECK(RecordPhysxNeeded(note.string(), std::nullopt));  // nothing to remove
    WriteText(note / ".playden-physx-needed", "not a version");
    CHECK(!RecordedPhysxNeeded(note.string()));
    const auto missing = base / "no-such-bottle";
    CHECK(!RecordedPhysxNeeded(missing.string()));
  }

  // Needed, and nothing to install.
  const auto bare = base / "bare";
  std::filesystem::create_directories(bare / "Binaries");
  prefix = fresh_prefix("bare-prefix");
  result = InstallRedistributables(bare.string(), "Z:\\games\\4", prefix.string(), 1700000100, true);
  CHECK(result.outcomes.empty() && result.physx_missing && result.physx_searched);
  std::filesystem::remove_all(base);
}

// A bottle as Wine makes it: hives and the system folders, private to Playden.
std::filesystem::path MakeBottle(const std::filesystem::path& prefix) {
  WriteText(prefix / "system.reg", "WINE REGISTRY Version 2\n;; All keys relative to \\\\Machine\n\n#arch=win64\n");
  WriteText(prefix / "user.reg", "WINE REGISTRY Version 2\n;; All keys relative to \\\\User\\\\S-1-5-21-0-0-0-1000\n\n#arch=win64\n");
  std::filesystem::create_directories(prefix / "drive_c/windows/system32");
  std::filesystem::create_directories(prefix / "drive_c/windows/syswow64");
  WriteText(prefix / "drive_c/windows/system32/msvcp140.dll", "WINE-BUILTIN");
  std::filesystem::permissions(prefix, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace);
  return prefix;
}

// A setup program that carries its package as is, as NVIDIA's PhysX System Software does (a WISE
// installer): the package is found past another compound file and installed from the program.
void TestEmbeddedPackage() {
  const std::string head = "MZ" + std::string(300, '\x90');
  const auto other = CompoundFile({{"resource", "not a package"}});
  const auto program = head + other + SamplePackage() + std::string(1000, '\x7f');
  CHECK(FindEmbeddedMsi(program) == head.size() + other.size());
  CHECK(!FindEmbeddedMsi(head + other + std::string(1000, '\x7f')));
  CHECK(!FindEmbeddedMsi(""));

  char pattern[] = "/tmp/playden-redist-XXXXXX";
  CHECK(::mkdtemp(pattern));
  const auto base = std::filesystem::canonical(pattern);
  const auto game = base / "game";
  const auto prefix = MakeBottle(base / "prefix");
  WriteText(game / "installscript.vdf",
            "\"InstallScript\" { \"Run Process\" { \"Version\" { "
            "\"HasRunKey\" \"HKEY_LOCAL_MACHINE\\\\SOFTWARE\\\\Vendor\" "
            "\"Process 1\" \"%INSTALLDIR%\\\\redist\\\\PhysX_SystemSoftware.exe\" \"Command 1\" \"/quiet\" } "
            "\"Other\" { \"Process 1\" \"%INSTALLDIR%\\\\redist\\\\other_setup.exe\" } } }");
  WriteText(game / "redist/PhysX_SystemSoftware.exe", program);
  WriteText(game / "redist/other_setup.exe", head + other);
  const auto result = InstallRedistributables(game.string(), "Z:\\games\\35140", prefix.string(), 1700000100);
  CHECK(result.outcomes.size() == 2);
  // The entry's HasRunKey value is one the package writes itself ("Version" under Vendor): the package's is kept.
  CHECK(result.outcomes[0].name == "Version" && result.outcomes[0].result == "installed" && result.outcomes[0].files == 3);
  CHECK(result.outcomes[0].detail == "redist/PhysX_SystemSoftware.exe (its package)");
  CHECK(result.outcomes[1].name == "Other" && result.outcomes[1].result == "not-run" &&
        result.outcomes[1].detail == "other_setup.exe");
  CHECK(ReadText(prefix / "drive_c/Program Files (x86)/Vendor Runtime/Common/Loader.dll") == "0123456789");
  CHECK(ReadText(prefix / "system.reg").find("\"Core Path\"=") != std::string::npos);
  CHECK(ReadText(prefix / "system.reg").find("\"Version\"=dword:00000007") != std::string::npos);
  CHECK(ReadText(prefix / ".playden-redist").find(" redist/PhysX_SystemSoftware.exe\n") != std::string::npos);
  // Installed once, under the program's own hash.
  const auto again = InstallRedistributables(game.string(), "Z:\\games\\35140", prefix.string(), 1700000200);
  CHECK(again.outcomes.size() == 2 && again.outcomes[0].result == "already");
  // The program is read a piece of 1 MiB at a time: a package whose signature straddles two pieces is found.
  std::filesystem::remove(prefix / ".playden-redist");
  const auto straddling = "MZ" + std::string((1u << 20) - 5, '\x90') + SamplePackage();
  CHECK(FindEmbeddedMsi(straddling) == (1u << 20) - 3);
  WriteText(game / "redist/PhysX_SystemSoftware.exe", straddling);
  const auto large = InstallRedistributables(game.string(), "Z:\\games\\35140", prefix.string(), 1700000300);
  CHECK(large.outcomes.size() == 2 && large.outcomes[0].result == "installed" && large.outcomes[0].files == 3);
  // The same program in two folders, its cabinet beside only the second: the first fails (again for a second entry,
  // without another try) and the second still installs.
  std::filesystem::remove(prefix / ".playden-redist");
  const auto external = head + SamplePackage(Variant::kExternalCabinet);
  WriteText(game / "installscript.vdf",
            "\"InstallScript\" { \"Run Process\" { "
            "\"A\" { \"Process 1\" \"%INSTALLDIR%\\\\a\\\\setup.exe\" } "
            "\"A again\" { \"Process 1\" \"%INSTALLDIR%\\\\a\\\\setup.exe\" } "
            "\"B\" { \"Process 1\" \"%INSTALLDIR%\\\\b\\\\setup.exe\" } } }");
  WriteText(game / "a/setup.exe", external);
  WriteText(game / "b/setup.exe", external);
  WriteText(game / "b/data.cab", TwoBlockCabinet());
  const auto split = InstallRedistributables(game.string(), "Z:\\games\\35140", prefix.string(), 1700000400);
  CHECK(split.outcomes.size() == 3);
  CHECK(split.outcomes[0].result == "failed" && split.outcomes[1].result == "failed");
  CHECK(split.outcomes[0].detail == split.outcomes[1].detail && split.outcomes[0].detail.rfind("a/setup.exe: ", 0) == 0);
  CHECK(split.outcomes[2].result == "installed" && split.outcomes[2].files == 3);
  std::filesystem::remove_all(base);
}

// NVIDIA's PhysX System Software 9.09.0814 setup program (Batman: Arkham Asylum's), when PLAYDEN_TEST_PHYSX_SETUP names a copy.
void TestPhysxSetupProgram() {
  const char* path = std::getenv("PLAYDEN_TEST_PHYSX_SETUP");
  if (!path || !*path) return;
  char pattern[] = "/tmp/playden-redist-XXXXXX";
  CHECK(::mkdtemp(pattern));
  const auto base = std::filesystem::canonical(pattern);
  const auto prefix = MakeBottle(base / "prefix");
  const auto game = base / "game";
  WriteText(game / "installscript.vdf",
            "\"InstallScript\" { \"Run Process\" { \"PhysX Version\" { "
            "\"HasRunKey\" \"HKEY_LOCAL_MACHINE\\\\SOFTWARE\\\\AGEIA Technologies\" "
            "\"Process 1\" \"%INSTALLDIR%\\\\redist\\\\PhysX_9.08.14_9.09.0814_SystemSoftware.exe\" \"Command 1\" \"/quiet\" "
            "\"MinimumHasRunValue\" \"9090814\" } } }");
  std::filesystem::create_directories(game / "redist");
  std::filesystem::copy_file(path, game / "redist/PhysX_9.08.14_9.09.0814_SystemSoftware.exe");
  // Installed first, before this test reads anything itself, so the peak it adds can be seen.
  struct rusage before {}, after {};
  ::getrusage(RUSAGE_SELF, &before);
  const auto result = InstallRedistributables(game.string(), "Z:\\games\\35140", prefix.string(), 1700000100);
  ::getrusage(RUSAGE_SELF, &after);
  CHECK(result.outcomes.size() == 1);
  std::fprintf(stderr, "PhysX setup program: %s %s, %zu files, peak memory %ld MB before, %ld MB after\n",
               result.outcomes[0].result.c_str(), result.outcomes[0].detail.c_str(), result.outcomes[0].files,
               static_cast<long>(before.ru_maxrss / (1024 * 1024)), static_cast<long>(after.ru_maxrss / (1024 * 1024)));
  CHECK(result.outcomes[0].result == "installed");
  CHECK(std::filesystem::exists(prefix / "drive_c/windows/syswow64/PhysXLoader.dll"));
  CHECK(std::filesystem::exists(prefix / "drive_c/Program Files (x86)/AGEIA Technologies/v2.8.1/PhysXCore.dll"));
  std::filesystem::remove_all(base);

  const auto program = ReadText(path);
  const auto at = FindEmbeddedMsi(program);
  CHECK(at);
  MsiInstallPlan plan;
  std::string reason;
  CHECK(ReadMsiPackage(std::string_view(program).substr(*at), &plan, &reason));
  const auto has = [&](const std::string& target) {
    return std::any_of(plan.files.begin(), plan.files.end(), [&](const MsiFile& file) { return file.target == target; });
  };
  // 9.09.0814 puts its loader in the system folder and the engines under AGEIA Technologies.
  CHECK(plan.product_name == "NVIDIA PhysX" && plan.product_version == "9.09.0814");
  CHECK(has("windows/syswow64/PhysXLoader.dll"));
  CHECK(has("Program Files (x86)/AGEIA Technologies/v2.8.1/PhysXCore.dll"));
  CHECK(!plan.unsupported_files && result.outcomes[0].files == plan.files.size());
  // Its cabinet is LZX. PLAYDEN_TEST_PHYSX_SETUP_FILES may name a folder with the members another tool extracted.
  if (const char* reference = std::getenv("PLAYDEN_TEST_PHYSX_SETUP_FILES"); reference && *reference) {
    const auto cabinet = ReadMsiStream(std::string_view(program).substr(*at), plan.cabinets.at(0).name);
    CHECK(cabinet);
    std::size_t compared = 0;
    CHECK(ExtractCabinet(*cabinet, [&](const std::string& name, std::string_view data) {
      CHECK(ReadText(std::filesystem::path(reference) / name) == data);
      ++compared;
      return true;
    }, &reason));
    std::fprintf(stderr, "PhysX setup program: %zu members match the reference\n", compared);
    CHECK(compared > 0);
  }
}

VcRedist SyntheticRedist(bool x64, const std::string& bundle) {
  const std::string arch = x64 ? "x64" : "x86";
  return {arch, "https://download.example/pr/" + *HashArtifactBytes(bundle) + "/VC_redist." + arch + ".exe",
          *HashArtifactBytes(bundle), bundle.size(), "vcRuntimeAdditional_" + arch, "14.44.35211"};
}

void TestMfcFilter() {
  CHECK(MfcFileName("mfc140u.dll") && MfcFileName("MFC140.DLL") && MfcFileName("mfcm140u.dll") && MfcFileName("mfc140chs.dll"));
  CHECK(!MfcFileName("msvcp140.dll") && !MfcFileName("vcruntime140.dll") && !MfcFileName("concrt140.dll"));
  CHECK(!MfcFileName("mfc42.dll") && !MfcFileName("mfc140u.pdb") && !MfcFileName("mfc140u.dll.bak"));
  // The package's own folder mapping: x64 files go to system32, x86 ones to SysWOW64 (64-bit Windows).
  for (const bool x64 : {true, false}) {
    MsiInstallPlan plan;
    std::string reason;
    CHECK(ReadMsiPackage(MfcPackage(x64), &plan, &reason));
    CHECK(plan.files.size() == 3 && plan.cabinets.size() == 1 && plan.cabinets[0].name == "cab1.cab" && !plan.cabinets[0].embedded);
    CHECK(plan.files[0].target == std::string(x64 ? "windows/system32" : "windows/syswow64") + "/mfc140u.dll");
  }
}

void TestMfcInstall() {
  char pattern[] = "/tmp/playden-mfc-XXXXXX";
  CHECK(::mkdtemp(pattern));
  const auto base = std::filesystem::canonical(pattern);
  const auto cache = base / "data/playden/redist";
  // Padded past one range, so the download takes two.
  const auto x64_bundle = MfcBundle(true) + std::string(kRedistChunkBytes, '\0');
  const auto x64 = SyntheticRedist(true, x64_bundle);
  int fetches = 0, failures_left = 1;
  std::string served = x64_bundle;
  const RangeFetch fetch = [&](const std::string& url, std::uint64_t offset, std::uint64_t length,
                               std::string* error) -> std::optional<std::string> {
    ++fetches;
    if (failures_left > 0) {  // one network error, which the next attempt gets past
      --failures_left;
      *error = "timed out";
      return std::nullopt;
    }
    CHECK(url.rfind("https://download.example/pr/" + *HashArtifactBytes(served) + "/VC_redist.", 0) == 0 ||
          (url == x64.url && served.size() == x64.bytes));
    if (offset >= served.size()) return std::string();
    return served.substr(offset, length);
  };
  // Installed from the download: only MFC, the bottle's Wine msvcp140 stays.
  const auto first = MakeBottle(base / "one");
  auto result = InstallMfcRuntime(x64, cache.string(), first.string(), fetch, 1700000000);
  if (result.result != "installed") std::fprintf(stderr, "mfc: %s\n", result.detail.c_str());
  CHECK(result.result == "installed" && result.source == "download" && result.files == 2 && result.cache_problem.empty());
  CHECK(fetches == 3);  // the failed attempt and two ranges
  CHECK(ReadText(first / "drive_c/windows/system32/mfc140u.dll") == "MFC64U");
  CHECK(ReadText(first / "drive_c/windows/system32/mfcm140.dll") == "MFCM64");
  CHECK(ReadText(first / "drive_c/windows/system32/msvcp140.dll") == "WINE-BUILTIN");
  CHECK(!std::filesystem::exists(first / "drive_c/windows/syswow64/mfc140u.dll"));
  CHECK(ReadText(first / "system.reg").find("RuntimeAdditional]") != std::string::npos);
  CHECK(ReadText(first / ".playden-redist") == x64.sha256 + " vc-14.44.35211-x64-mfc\n");
  const auto kept = cache / "vc-14.44.35211/x64/VC_redist.x64.exe";
  CHECK(ReadText(kept) == x64_bundle);
  // Once per bottle: nothing is read or fetched again.
  result = InstallMfcRuntime(x64, cache.string(), first.string(), fetch, 1700000100);
  CHECK(result.result == "already" && fetches == 3);
  // Another game's bottle takes the kept copy.
  const auto second = MakeBottle(base / "two");
  result = InstallMfcRuntime(x64, cache.string(), second.string(), nullptr, 1700000200);
  CHECK(result.result == "installed" && result.source == "cache" && fetches == 3);
  CHECK(ReadText(second / "drive_c/windows/system32/mfc140u.dll") == "MFC64U");
  // A kept copy that isn't the pinned one counts as missing: removed and fetched again.
  WriteText(kept, "damaged");
  const auto third = MakeBottle(base / "three");
  result = InstallMfcRuntime(x64, cache.string(), third.string(), nullptr, 1700000300);
  CHECK(result.result == "failed" && result.detail == "Playden can't download it here" && !std::filesystem::exists(kept));
  CHECK(!std::filesystem::exists(third / ".playden-redist"));
  result = InstallMfcRuntime(x64, cache.string(), third.string(), fetch, 1700000300);
  CHECK(result.result == "installed" && result.source == "download" && ReadText(kept) == x64_bundle);
  // A download that isn't the pinned bundle is refused before it is read, and not kept.
  served[served.size() - 1] = 'x';
  std::filesystem::remove(kept);
  const auto fourth = MakeBottle(base / "four");
  result = InstallMfcRuntime(x64, cache.string(), fourth.string(), fetch, 1700000400);
  CHECK(result.result == "failed" && result.detail.find("SHA-256 mismatch") != std::string::npos);
  CHECK(!std::filesystem::exists(kept) && !std::filesystem::exists(fourth / ".playden-redist"));
  CHECK(!std::filesystem::exists(fourth / "drive_c/windows/system32/mfc140u.dll"));
  // A server that keeps failing.
  failures_left = 100;
  result = InstallMfcRuntime(x64, cache.string(), fourth.string(), fetch, 1700000400);
  CHECK(result.result == "failed" && result.detail == "the download failed at byte 0: timed out");
  // x86: into SysWOW64, from its own folder of the cache.
  failures_left = 0;
  served = MfcBundle(false);
  const auto x86 = SyntheticRedist(false, served);
  result = InstallMfcRuntime(x86, cache.string(), fourth.string(), fetch, 1700000500);
  CHECK(result.result == "installed" && result.files == 2);
  CHECK(ReadText(fourth / "drive_c/windows/syswow64/mfc140u.dll") == "MFC32U");
  CHECK(!std::filesystem::exists(fourth / "drive_c/windows/syswow64/msvcp140.dll"));
  CHECK(std::filesystem::exists(cache / "vc-14.44.35211/x86/VC_redist.x86.exe"));
  // A bundle whose package has no MFC files installs nothing.
  served = MfcBundle(true);
  auto minimum_only = SyntheticRedist(true, served);
  minimum_only.package_id = "vcRuntimeMinimum_x64";
  minimum_only.version = "other";
  const auto fifth = MakeBottle(base / "five");
  result = InstallMfcRuntime(minimum_only, cache.string(), fifth.string(), fetch, 1700000600);
  CHECK(result.result == "failed" && !std::filesystem::exists(fifth / ".playden-redist"));
  // The cache can't be written: the game still gets MFC.
  served = MfcBundle(true);
  auto uncached = SyntheticRedist(true, served);
  WriteText(base / "blocked", "a file where the cache folder would be");
  const auto sixth = MakeBottle(base / "six");
  result = InstallMfcRuntime(uncached, (base / "blocked").string(), sixth.string(), fetch, 1700000700);
  CHECK(result.result == "installed" && !result.cache_problem.empty());
  std::filesystem::remove_all(base);
}

void TestMfcScanRecord() {
  char pattern[] = "/tmp/playden-mfc-scan-XXXXXX";
  CHECK(::mkdtemp(pattern));
  const auto base = std::filesystem::canonical(pattern);
  const auto prefix = MakeBottle(base / "prefix");
  CHECK(!ReadMfcScan(prefix.string()));
  CHECK(WriteMfcScan(prefix.string(), {"files-1", "", "Game_Data/Plugins/Chroma.dll"}));
  auto read = ReadMfcScan(prefix.string());
  CHECK(read && read->fingerprint == "files-1" && read->x86_program.empty() && read->x64_program == "Game_Data/Plugins/Chroma.dll");
  CHECK(WriteMfcScan(prefix.string(), {"", "", ""}));  // scanned, no MFC, no fingerprint
  read = ReadMfcScan(prefix.string());
  CHECK(read && read->fingerprint.empty() && read->x64_program.empty());
  // The fingerprint follows the install record and file list Playden writes at the game's root.
  const auto game = base / "game";
  std::filesystem::create_directories(game);
  CHECK(GameFilesFingerprint(game.string()).empty());
  WriteText(game / ".playden-install.json", "{}");
  const auto first = GameFilesFingerprint(game.string());
  CHECK(!first.empty() && GameFilesFingerprint(game.string()) == first);
  WriteText(game / ".playden-files", "a.exe\n");
  const auto second = GameFilesFingerprint(game.string());
  CHECK(second != first);
  WriteText(game / ".playden-files", "a.exe\nb.dll\n");
  CHECK(GameFilesFingerprint(game.string()) != second);
  // A stale part of an earlier download in the cache goes when the next copy is kept.
  const auto bundle = MfcBundle(true);
  const auto redist = SyntheticRedist(true, bundle);
  const auto folder = base / "cache/vc-14.44.35211/x64";
  WriteText(folder / "VC_redist.x64.exe.part-1", "partial");
  const RangeFetch fetch = [&](const std::string&, std::uint64_t offset, std::uint64_t length, std::string*) {
    return std::optional<std::string>(bundle.substr(offset, length));
  };
  CHECK(!MfcInstalled(redist, prefix.string()));
  CHECK(InstallMfcRuntime(redist, (base / "cache").string(), prefix.string(), fetch, 1700000000).result == "installed");
  CHECK(MfcInstalled(redist, prefix.string()));
  CHECK(!std::filesystem::exists(folder / "VC_redist.x64.exe.part-1") && ReadText(folder / "VC_redist.x64.exe") == bundle);
  std::filesystem::remove_all(base);
}

// Microsoft's own bundles, when PLAYDEN_TEST_VC_REDIST_X64 / _X86 name copies: read, mapped and installed (from a
// cache seeded with the copy, so nothing is downloaded).
void TestMicrosoftVcRedist() {
  for (const auto& redist : MicrosoftVcRedists()) {
    const char* path = std::getenv(redist.architecture == "x64" ? "PLAYDEN_TEST_VC_REDIST_X64" : "PLAYDEN_TEST_VC_REDIST_X86");
    if (!path || !*path) continue;
    const auto bundle = ReadText(path);
    CHECK(bundle.size() == redist.bytes && HashArtifactBytes(bundle) == redist.sha256);
    BurnMsiPackage package;
    std::string reason;
    CHECK(ReadBurnMsiPackage(bundle, redist.package_id, &package, &reason));
    MsiInstallPlan plan;
    CHECK(ReadMsiPackage(package.msi, &plan, &reason));
    const auto system = redist.architecture == "x64" ? "windows/system32/" : "windows/syswow64/";
    std::size_t mfc = 0;
    for (const auto& file : plan.files) {
      const bool kept = MfcFileName(file.target.substr(file.target.rfind('/') + 1));
      std::fprintf(stderr, "%s %s %s\n", redist.architecture.c_str(), kept ? "mfc  " : "other", file.target.c_str());
      if (!kept) continue;
      CHECK(file.target.rfind(system, 0) == 0);
      ++mfc;
    }
    for (const auto& value : plan.machine)
      std::fprintf(stderr, "%s registry %s \\ %s = %s\n", redist.architecture.c_str(), value.key.c_str(), value.name.c_str(),
                   value.data.c_str());
    std::fprintf(stderr, "%s: %s, %zu files (%zu MFC), %zu cabinets, %zu custom actions, %zu machine values\n",
                 redist.architecture.c_str(), package.msi_name.c_str(), plan.files.size(), mfc, plan.cabinets.size(),
                 plan.custom_actions, plan.machine.size());
    CHECK(mfc >= 4);
    char pattern[] = "/tmp/playden-vc-redist-XXXXXX";
    CHECK(::mkdtemp(pattern));
    const auto base = std::filesystem::canonical(pattern);
    WriteText(base / "cache" / ("vc-" + redist.version) / redist.architecture / ("VC_redist." + redist.architecture + ".exe"), bundle);
    const auto prefix = MakeBottle(base / "prefix");
    const auto result = InstallMfcRuntime(redist, (base / "cache").string(), prefix.string(), nullptr, 1700000000);
    if (result.result != "installed") std::fprintf(stderr, "install: %s\n", result.detail.c_str());
    CHECK(result.result == "installed" && result.source == "cache" && result.files == mfc);
    CHECK(std::filesystem::file_size(prefix / "drive_c" / system / "mfc140u.dll") > 1000000);
    CHECK(ReadText(prefix / "drive_c/windows/system32/msvcp140.dll") == "WINE-BUILTIN");
    std::filesystem::remove_all(base);
  }
}

// NVIDIA's legacy PhysX package (Bullet Witch's), when PLAYDEN_TEST_PHYSX_MSI names a copy.
void TestPhysxPackage() {
  const char* path = std::getenv("PLAYDEN_TEST_PHYSX_MSI");
  if (!path || !*path) return;
  const auto package = ReadText(path);
  MsiInstallPlan plan;
  std::string reason;
  CHECK(ReadMsiPackage(package, &plan, &reason));
  CHECK(plan.product_name == "NVIDIA PhysX (Legacy)");
  const auto has = [&](const std::string& target) {
    return std::any_of(plan.files.begin(), plan.files.end(), [&](const MsiFile& file) { return file.target == target; });
  };
  CHECK(has("Program Files (x86)/NVIDIA Corporation/PhysX/Common/PhysXLoader.dll"));
  CHECK(has("Program Files (x86)/NVIDIA Corporation/PhysX/Common/PhysXLoader64.dll"));
  CHECK(has("Program Files (x86)/NVIDIA Corporation/PhysX/Engine/v2.6.4/PhysXCore.dll"));
  const auto* core = FindValue(plan.machine, "Software\\Wow6432Node\\AGEIA Technologies", "PhysXCore Path");
  CHECK(core && core->data == "\"C:\\\\Program Files (x86)\\\\NVIDIA Corporation\\\\PhysX\\\\Engine\"");
  CHECK((plan.path_prepend == std::vector<std::string>{"C:\\Program Files (x86)\\NVIDIA Corporation\\PhysX\\Common"}));
  const auto cabinet = ReadMsiStream(package, plan.cabinets.at(0).name);
  CHECK(cabinet);
  // PLAYDEN_TEST_PHYSX_FILES may name a folder with the members another tool extracted.
  const char* reference = std::getenv("PLAYDEN_TEST_PHYSX_FILES");
  std::map<std::string, std::uint32_t> listed;
  for (const auto& file : plan.files) listed[file.member] = file.size;
  std::size_t count = 0, loader = 0, compared = 0;
  CHECK(ExtractCabinet(*cabinet, [&](const std::string& name, std::string_view data) {
    ++count;
    CHECK(!listed.count(name) || listed[name] == data.size());  // the installer checks this
    if (name == "FILE_COMMON_PhysXLoader_dll") loader = data.size();
    if (reference && *reference) {
      CHECK(ReadText(std::filesystem::path(reference) / name) == data);
      ++compared;
    }
    return true;
  }, &reason));
  CHECK(!reference || !*reference || compared == 43);
  CHECK(count == 43 && loader == 57224);
  std::fprintf(stderr, "PhysX package: %zu files, %zu machine values\n", plan.files.size(), plan.machine.size());
}

}  // namespace

int main() {
  TestConditions();
  TestCabinet();
  TestPlan();
  TestPath();
  TestBurnBundle();
  TestScripts();
  TestInstall();
  TestEmbeddedPackage();
  TestFoundPhysx();
  TestPhysxPackage();
  TestPhysxSetupProgram();
  TestMfcFilter();
  TestMfcInstall();
  TestMfcScanRecord();
  TestMicrosoftVcRedist();
  return 0;
}
