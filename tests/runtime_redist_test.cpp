#include "runtime/artifact_hash.h"
#include "runtime/burn_bundle.h"
#include "runtime/install_script.h"
#include "runtime/msi_package.h"
#include "runtime/redist_install.h"
#include "runtime/redist_names.h"

#include "msi_fixture.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <new>
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

// Bytes this program holds through operator new, and the most it held since the last reset: what an install
// costs the console's heap, which is a 128 MiB pool shared with the rest of Playden.
namespace heap {
std::atomic<std::size_t> live{0}, peak{0};
constexpr std::size_t kHeader = alignof(std::max_align_t);
void* Allocate(std::size_t size) {
  auto* block = static_cast<unsigned char*>(std::malloc(size + kHeader));
  if (!block) throw std::bad_alloc();
  *reinterpret_cast<std::size_t*>(block) = size;
  const auto now = live += size;
  for (auto seen = peak.load(); now > seen && !peak.compare_exchange_weak(seen, now);) {}
  return block + kHeader;
}
void Release(void* address) noexcept {
  if (!address) return;
  auto* block = static_cast<unsigned char*>(address) - kHeader;
  live -= *reinterpret_cast<std::size_t*>(block);
  std::free(block);
}
// The most held above what was held when it was called, since then.
struct Watch {
  std::size_t base = (peak = live.load(), live.load());
  std::size_t Above() const { return peak.load() - base; }
};
}  // namespace heap

void* operator new(std::size_t size) { return heap::Allocate(size); }
void* operator new[](std::size_t size) { return heap::Allocate(size); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
  try { return heap::Allocate(size); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
  try { return heap::Allocate(size); } catch (...) { return nullptr; }
}
void operator delete(void* address) noexcept { heap::Release(address); }
void operator delete[](void* address) noexcept { heap::Release(address); }
void operator delete(void* address, std::size_t) noexcept { heap::Release(address); }
void operator delete[](void* address, std::size_t) noexcept { heap::Release(address); }
void operator delete(void* address, const std::nothrow_t&) noexcept { heap::Release(address); }
void operator delete[](void* address, const std::nothrow_t&) noexcept { heap::Release(address); }

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
  // Only the members asked for are collected; a skipped one still feeds the next block's history.
  members.clear();
  CHECK(ExtractCabinet(TwoBlockCabinet(), [&](const std::string& name, std::string_view data) {
    members[name] = std::string(data);
    return true;
  }, &reason, [](const std::string& name) { return name == "second"; }));
  CHECK(members.size() == 1 && members["second"] == "0123456789");
  // A member listed past the end of its folder is refused before its size is set aside.
  auto inflated = StoredCabinet({{"one", "x"}});
  inflated.replace(44, 4, std::string("\0\0\0\x10", 4));  // 256 MiB, with one byte in the folder
  {
    const heap::Watch watch;
    CHECK(!ExtractCabinet(inflated, [](const std::string&, std::string_view) { return true; }, &reason));
    CHECK(reason == "damaged cabinet: files run past their folder" && watch.Above() < (1u << 20));
  }
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
  members.clear();
  CHECK(ExtractCabinet(LzxCabinet({{"big", big}, {"small", "hello"}}), [&](const std::string& name, std::string_view data) {
    members[name] = std::string(data);
    return true;
  }, &reason, [](const std::string& name) { return name == "small"; }));
  CHECK(members.size() == 1 && members["small"] == "hello");
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
  // The MsiPackages of a bundle, in its chain's order; none for one that only runs programs.
  std::vector<std::string> ids;
  CHECK(ReadBurnMsiPackageIds(whole, &ids, &reason));
  CHECK((ids == std::vector<std::string>{"vcRuntimeMinimum_x64", "vcRuntimeAdditional_x64"}));
  CHECK(ReadBurnMsiPackageIds(RuntimeBundle({{"second", "x"}, {"first", "y"}}, false, "prereq"), &ids, &reason));
  CHECK((ids == std::vector<std::string>{"second", "first"}));
  CHECK(ReadBurnMsiPackageIds(RuntimeBundle({}, false, "prereq"), &ids, &reason) && ids.empty());
  // Without a .wixburn section to say where they are, its containers are searched for.
  const auto unsectioned = MfcBundle(true, true, false);
  CHECK(ReadBurnMsiPackageIds(unsectioned, &ids, &reason) && ids.size() == 2);
  // Cut short, the manifest is still there but its attached container isn't.
  const auto compressed = RuntimeBundle({{"Only", MfcPackage(true)}}, true);
  CHECK(ReadBurnMsiPackageIds(compressed, &ids, &reason) && ids.size() == 1);
  CHECK(!ReadBurnMsiPackageIds(compressed.substr(0, compressed.size() - 100), &ids, &reason) && ids.empty());
  CHECK(reason == "the bundle's container WixAttachedContainer is missing");
  // A bundle is told from its first bytes, before it is read whole.
  CHECK(HasBurnSection(std::string_view(whole).substr(0, 4096)) && HasBurnSection(compressed));
  CHECK(!HasBurnSection(unsectioned) && !HasBurnSection("MZ not a bundle") && !HasBurnSection(""));
  CHECK(!ReadBurnMsiPackageIds("MZ not a bundle", &ids, &reason));
}

void TestPath() {
  CHECK(ExtendPath("A;B", {"C:\\X"}, {}) == "C:\\X;A;B");
  CHECK(ExtendPath("A;c:\\x\\", {"C:\\X"}, {"D"}) == "A;c:\\x\\;D");
  CHECK(ExtendPath("", {}, {"D"}) == "D");
}

void TestScripts() {
  // Steam's install script names: installscript*.vdf, or <app id>_install.vdf (Singularity's).
  CHECK(InstallScriptName("installscript.vdf") && InstallScriptName("InstallScript_2.VDF"));
  CHECK(InstallScriptName("42670_install.vdf") && InstallScriptName("42670_Install.vdf"));
  CHECK(!InstallScriptName("_install.vdf") && !InstallScriptName("steam_install.vdf") && !InstallScriptName("42670_install.txt"));
  CHECK(!InstallScriptName("4267a_install.vdf") && !InstallScriptName("installscript") && !InstallScriptName("appmanifest_42670.acf"));
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
                           "Z:\\games\\1", "", &values, &reason));
  CHECK(values.machine.size() == 2);
  // Values under a language subkey apply for that language, then for English; (Default) is the unnamed value.
  const std::string localized =
      "\"InstallScript\" { \"Registry\" { \"HKEY_CURRENT_USER\\\\Software\\\\Studio\" { "
      "\"string\" { \"(Default)\" \"main\" \"Plain\" \"p\" \"english\" { \"Name\" \"en\" } \"Russian\" { \"Name\" \"ru\" } } "
      "\"dword\" { \"english\" { \"Language\" \"1\" } \"french\" { \"Language\" \"2\" } } } } }";
  const auto parsed_for = [&](const std::string& language) {
    InstallScriptValues parsed;
    CHECK(ParseInstallScript(localized, "Z:\\games\\1", language, &parsed, &reason));
    return parsed.user;
  };
  auto english = parsed_for("");
  CHECK(english.size() == 4);
  CHECK(FindValue(english, "Software\\Studio", "Name")->data == "\"en\"");
  CHECK(FindValue(english, "Software\\Studio", "Language")->data == "dword:00000001");
  CHECK(FindValue(english, "Software\\Studio", "Plain")->data == "\"p\"");
  const auto unnamed = std::find_if(english.begin(), english.end(), [](const RegistryValue& value) { return value.default_value; });
  CHECK(unnamed != english.end() && unnamed->name.empty() && unnamed->data == "\"main\"");
  auto russian = parsed_for("russian");
  CHECK(FindValue(russian, "Software\\Studio", "Name")->data == "\"ru\"");
  CHECK(FindValue(russian, "Software\\Studio", "Language")->data == "dword:00000001");  // no Russian value: English
  CHECK(FindValue(parsed_for("french"), "Software\\Studio", "Language")->data == "dword:00000002");
  // Far Cry 3's script (#607, trimmed to two languages) lists its HKLM key and the key's Wow6432Node twin. The plain
  // key is also written to the 32-bit view, so each value is set twice there; the registry takes it once.
  const std::string far_cry_3 = R"("InstallScript"
{
	"Registry"
	{
		"HKEY_LOCAL_MACHINE\\SOFTWARE\\Ubisoft\\Far Cry 3"
		{
			"string"
			{
				"ExecutableName"		"bin\\FC3UpdaterSteam.exe"
				"installdir"		"%INSTALLDIR%"
				"SKU"		"WW_STEAM"
				"version"		"1.03"
				"english"
				{
					"language"		"English"
				}
				"french"
				{
					"language"		"French"
				}
			}
		}
		"HKEY_LOCAL_MACHINE\\SOFTWARE\\Wow6432Node\\Ubisoft\\Far Cry 3"
		{
			"string"
			{
				"ExecutableName"		"bin\\FC3UpdaterSteam.exe"
				"installdir"		"%INSTALLDIR%"
				"SKU"		"WW_STEAM"
				"version"		"1.03"
				"english"
				{
					"language"		"English"
				}
				"french"
				{
					"language"		"French"
				}
			}
		}
	}
}
"kvsignatures"
{
	"InstallScript"		"0bd5c18c"
}
)";
  InstallScriptValues far_cry;
  CHECK(ParseInstallScript(far_cry_3, "Z:\\data\\playden-games\\220240", "french", &far_cry, &reason));
  CHECK(far_cry.user.empty() && far_cry.machine.size() == 10);
  CHECK(FindValue(far_cry.machine, "SOFTWARE\\Ubisoft\\Far Cry 3", "language")->data == "\"French\"");
  CHECK(FindValue(far_cry.machine, "SOFTWARE\\Wow6432Node\\Ubisoft\\Far Cry 3", "installdir")->data ==
        "\"Z:\\\\data\\\\playden-games\\\\220240\"");
  std::string far_cry_registry = "WINE REGISTRY Version 2\n";
  CHECK(ApplyRegistryValues(&far_cry_registry, far_cry.machine, 1760000000, &reason));
  CHECK(far_cry_registry.find("\"SKU\"=\"WW_STEAM\"") != std::string::npos);
  // Road Rash 95's script (#1109, trimmed) names the 32-bit view of HKLM: its values go to Software\Wow6432Node
  // alone, where the 32-bit game reads them. A _WOW64_64 key is the 64-bit view: the key alone. Keys outside
  // Software, and ones already under Wow6432Node, stay as written.
  const std::string road_rash = R"("InstallScript"
{
	"utf8_registry_strings"		"1"
	"Registry"
	{
		"HKEY_LOCAL_MACHINE_WOW64_32\\SOFTWARE\\Electronic Arts\\RoadRash 95"
		{
			"string"
			{
				"english"
				{
					"path"		"%INSTALLDIR%"
					"language"		"English"
				}
				"french"
				{
					"path"		"%INSTALLDIR%"
					"language"		"French"
				}
			}
		}
		"HKEY_LOCAL_MACHINE_WOW64_64\\SOFTWARE\\Studio\\Game64" { "dword" { "Installed" "1" } }
		"HKEY_LOCAL_MACHINE_WOW64_32\\SOFTWARE\\Wow6432Node\\Studio\\Twin" { "string" { "a" "b" } }
		"HKEY_LOCAL_MACHINE_WOW64_32\\SYSTEM\\Studio" { "string" { "c" "d" } }
	}
	"Firewall"
	{
		"RoadRash95.exe"		"%INSTALLDIR%\\RoadRash95.exe"
	}
}
"kvsignatures"
{
	"InstallScript"		"81d32a2a"
}
)";
  InstallScriptValues road_rash_values;
  CHECK(ParseInstallScript(road_rash, "Z:\\data\\playden-games\\2448570", "french", &road_rash_values, &reason));
  CHECK(road_rash_values.user.empty() && road_rash_values.machine.size() == 5);
  CHECK(FindValue(road_rash_values.machine, "Software\\Wow6432Node\\Electronic Arts\\RoadRash 95", "language")->data ==
        "\"French\"");
  CHECK(FindValue(road_rash_values.machine, "Software\\Wow6432Node\\Electronic Arts\\RoadRash 95", "path")->data ==
        "\"Z:\\\\data\\\\playden-games\\\\2448570\"");
  CHECK(!FindValue(road_rash_values.machine, "SOFTWARE\\Electronic Arts\\RoadRash 95", "language"));
  CHECK(FindValue(road_rash_values.machine, "SOFTWARE\\Studio\\Game64", "Installed")->data == "dword:00000001");
  CHECK(!FindValue(road_rash_values.machine, "Software\\Wow6432Node\\Studio\\Game64", "Installed"));
  CHECK(FindValue(road_rash_values.machine, "SOFTWARE\\Wow6432Node\\Studio\\Twin", "a"));
  CHECK(FindValue(road_rash_values.machine, "SYSTEM\\Studio", "c"));
  // The same key and name in both views keep their own data, whatever the case, and a plain HKLM key's copy for a view
  // doesn't replace a value set in that view, before or after it. Values on Software and Software\Wow6432Node
  // themselves land on the 32-bit node; Software\Classes is shared, so a 32-bit value goes to both keys.
  for (const bool plain_first : {true, false}) {
    const std::string plain = "\"HKEY_LOCAL_MACHINE\\\\Software\\\\Vendor\" { \"string\" { \"Path\" \"plain\" } } ";
    const std::string views =
        std::string("\"HKEY_LOCAL_MACHINE_WOW64_64\\\\SOFTWARE\\\\Vendor\" { \"string\" { \"path\" \"x64\" } } ") +
        "\"hkey_local_machine_wow64_32\\\\software\\\\vendor\" { \"string\" { \"PATH\" \"x86\" } } " +
        "\"HKEY_LOCAL_MACHINE_WOW64_32\\\\Software\" { \"string\" { \"Top\" \"1\" } } " +
        "\"HKEY_LOCAL_MACHINE_WOW64_32\\\\Software\\\\Wow6432Node\" { \"string\" { \"Node\" \"2\" } } " +
        "\"HKEY_LOCAL_MACHINE_WOW64_32\\\\Software\\\\Classes\\\\Vendor.Save\" { \"string\" { \"(Default)\" \"Save\" } } ";
    const std::string script =
        "\"InstallScript\" { \"Registry\" { " + (plain_first ? plain + views : views + plain) + "} }";
    InstallScriptValues viewed;
    CHECK(ParseInstallScript(script, "Z:\\games\\1", "", &viewed, &reason));
    CHECK(viewed.machine.size() == 6);
    const auto data = [&](const std::string& key, const std::string& name) {
      for (const auto& value : viewed.machine)
        if (RedistLower(value.key) == RedistLower(key) && RedistLower(value.name) == RedistLower(name)) return value.data;
      return std::string("missing");
    };
    CHECK(data("Software\\Vendor", "Path") == "\"x64\"");
    CHECK(data("Software\\Wow6432Node\\Vendor", "Path") == "\"x86\"");
    CHECK(data("Software\\Wow6432Node", "Top") == "\"1\"");
    CHECK(data("Software\\Wow6432Node", "Node") == "\"2\"");
    CHECK(data("Software\\Wow6432Node\\Wow6432Node", "Node") == "missing");
    CHECK(data("Software", "Top") == "missing");
    CHECK(data("Software\\Classes\\Vendor.Save", "") == "\"Save\"");
    CHECK(data("Software\\Classes\\Wow6432Node\\Vendor.Save", "") == "\"Save\"");
    // Applied to a hive, each view keeps its value, and applying it again changes nothing.
    std::string hive = "WINE REGISTRY Version 2\n;; All keys relative to REGISTRY\\\\Machine\n\n#arch=win64\n";
    CHECK(ApplyRegistryValues(&hive, viewed.machine, 1760000000, &reason));
    const auto section_has = [&hive](const std::string& key, const std::string& line) {
      const auto lower = RedistLower(hive);
      const auto start = lower.find("[" + RedistLower(key) + "]");
      if (start == std::string::npos) return false;
      const auto end = lower.find("\n[", start + 1);
      return lower.substr(start, end == std::string::npos ? std::string::npos : end - start).find(RedistLower(line)) !=
             std::string::npos;
    };
    CHECK(section_has("Software\\\\Vendor", "\"Path\"=\"x64\""));
    CHECK(section_has("Software\\\\Wow6432Node\\\\Vendor", "\"Path\"=\"x86\""));
    CHECK(!section_has("Software\\\\Vendor", "\"Path\"=\"plain\"") &&
          !section_has("Software\\\\Wow6432Node\\\\Vendor", "\"Path\"=\"plain\""));
    const auto once = hive;
    CHECK(ApplyRegistryValues(&hive, viewed.machine, 1760000001, &reason));
    CHECK(hive == once);
  }
  // Every token Steam documents (and common Windows variables) becomes the folder Steam, a 32-bit program, would see
  // in the bottle, in any case; a lone % stays as it is. HKEY_CLASSES_ROOT is HKLM\Software\Classes in both views, and
  // HKCU's view names are HKCU. Names and strings past ASCII are written as Wine writes them: \x and the UTF-16 code,
  // four digits when a hex digit follows. What can't be written is left out and named.
  {
    const std::string generic = R"vdf("InstallScript"
{
	"Registry"
	{
		"HKEY_LOCAL_MACHINE\\Software\\Studio\\%InstallDir%"
		{
			"string"
			{
				"Saves"		"%USER_MYDOCS%\\Studio"
				"Roaming"		"%appdata%\\Studio"
				"Local"		"%LOCALAPPDATA%"
				"Shared"		"%COMMON_MYDOCS%"
				"Windows"		"%WinDir%\\system32"
				"Steam"		"%STEAMPATH%"
				"Root"		"%ROOTDRIVE%:\\"
				"Open"		"\"%InstallDir%\\Game.exe\" \"%1\" \"%2\""
				"Programs"		"%ProgramFiles%|%ProgramW6432%"
				"Share"		"100%"
				"Café"		"Ça va"
				"Hex"		"éa"
				"Wide"		"日本"
				"Smile"		"😀"
				"Quote\"d"		"x"
				"Odd"		"%NOT_A_TOKEN%"
			}
			"dword"
			{
				"Hex"		"0x10"
				"Huge"		"4294967296"
				"Spaced"		" 5"
			}
			"qword"
			{
				"Big"		"1"
			}
		}
		"HKEY_CLASSES_ROOT\\.rrsave" { "string" { "(Default)" "RoadRash.Save" } }
		"HKEY_CLASSES_ROOT\\CLSID\\{1}" { "string" { "(Default)" "Studio" } }
		"HKEY_CURRENT_USER\\Software\\Microsoft\\Windows NT\\CurrentVersion\\AppCompatFlags\\Layers" { "string" { "%INSTALLDIR%\\Game.exe" "WINXPSP3" "%NOPE%\\x.exe" "1" } }
		"HKEY_LOCAL_MACHINE\\Software\\Bad]Key" { "string" { "a" "b" } }
		"HKEY_CURRENT_USER_WOW64_32\\Software\\Studio" { "string" { "User" "%USERPROFILE%" } }
		"HKEY_USERS\\.DEFAULT\\Studio" { "string" { "a" "b" "c" "d" } }
		"HKEY_LOCAL_MACHINE\\Software\\%MISSING%" { "string" { "a" "b" } }
		"HKEY_LOCAL_MACHINE\\Software\\Ünicode" { "string" { "a" "b" } }
	}
}
)vdf";
    InstallScriptValues parsed;
    CHECK(ParseInstallScript(generic, "Z:\\games\\7", "", &parsed, &reason));
    const auto key = std::string("Software\\Studio\\Z:\\games\\7");
    const auto data = [&](const std::string& in, const std::string& name) {
      const auto* found = FindValue(parsed.machine, in, name);
      return found ? found->data : std::string("missing");
    };
    CHECK(data(key, "Saves") == "\"C:\\\\users\\\\prospero\\\\Documents\\\\Studio\"");
    CHECK(data(key, "Roaming") == "\"C:\\\\users\\\\prospero\\\\AppData\\\\Roaming\\\\Studio\"");
    CHECK(data(key, "Local") == "\"C:\\\\users\\\\prospero\\\\AppData\\\\Local\"");
    CHECK(data(key, "Shared") == "\"C:\\\\users\\\\Public\\\\Documents\"");
    CHECK(data(key, "Windows") == "\"C:\\\\windows\\\\system32\"");
    CHECK(data(key, "Steam") == "missing");  // the Steam client folder depends on the game's Steam mode
    CHECK(data(key, "Root") == "\"Z:\\\\\"");  // the game folder's drive
    // Only %NAME% is a token: a shell command's "%1" and "%2" stay as they are.
    CHECK(data(key, "Open") == "\"\\\"Z:\\\\games\\\\7\\\\Game.exe\\\" \\\"%1\\\" \\\"%2\\\"\"");
    CHECK(data(key, "Programs") == "\"C:\\\\Program Files (x86)|C:\\\\Program Files\"");
    CHECK(data(key, "Share") == "\"100%\"");
    CHECK(data(key, "Caf\\xe9") == "\"\\x00c7a va\"");  // four digits: a hex digit follows
    CHECK(data(key, "Hex") == "dword:00000010");  // the dword of the same name comes later and replaces the string
    CHECK(data(key, "Wide") == "\"\\x65e5\\x672c\"");
    CHECK(data(key, "Smile") == "\"\\xd83d\\xde00\"");
    CHECK(data("Software\\Wow6432Node\\Studio\\Z:\\games\\7", "Saves") == data(key, "Saves"));
    const auto* save = FindValue(parsed.machine, "Software\\Classes\\.rrsave", "");
    CHECK(save && save->default_value && save->data == "\"RoadRash.Save\"");
    CHECK(FindValue(parsed.machine, "Software\\Classes\\Wow6432Node\\.rrsave", ""));
    // HKCR's CLSID goes to the 32-bit node Wine keeps under Classes too.
    CHECK(FindValue(parsed.machine, "Software\\Classes\\CLSID\\{1}", "") &&
          FindValue(parsed.machine, "Software\\Classes\\Wow6432Node\\CLSID\\{1}", ""));
    CHECK(parsed.user.size() == 2 && FindValue(parsed.user, "Software\\Studio", "User") &&
          FindValue(parsed.user, "Software\\Studio", "User")->data == "\"C:\\\\users\\\\prospero\"");
    // A value's name takes tokens too: a program's compatibility layer is named by its full path.
    CHECK(FindValue(parsed.user, "Software\\Microsoft\\Windows NT\\CurrentVersion\\AppCompatFlags\\Layers",
                    "Z:\\\\games\\\\7\\\\Game.exe"));
    const std::vector<std::string> dropped = {
        "HKEY_LOCAL_MACHINE\\Software\\Studio\\%InstallDir% Steam: unknown %STEAMPATH%",
        "HKEY_LOCAL_MACHINE\\Software\\Studio\\%InstallDir% Quote\"d: name isn't text Playden can write",
        "HKEY_LOCAL_MACHINE\\Software\\Studio\\%InstallDir% Odd: unknown %NOT_A_TOKEN%",
        "HKEY_LOCAL_MACHINE\\Software\\Studio\\%InstallDir% Huge: dword isn't a 32-bit number",
        "HKEY_LOCAL_MACHINE\\Software\\Studio\\%InstallDir% Spaced: dword isn't a 32-bit number",
        "HKEY_LOCAL_MACHINE\\Software\\Studio\\%InstallDir%: type qword not supported",
        "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows NT\\CurrentVersion\\AppCompatFlags\\Layers %NOPE%\\x.ex...: unknown %NOPE%",  // cut at 96
        "HKEY_LOCAL_MACHINE\\Software\\Bad]Key: key isn't plain ASCII without [ or ]",
        "HKEY_USERS\\.DEFAULT\\Studio: root not supported (2 values)",
        "HKEY_LOCAL_MACHINE\\Software\\%MISSING%: unknown %MISSING%",
        "HKEY_LOCAL_MACHINE\\Software\\??nicode: key isn't plain ASCII without [ or ]"};
    CHECK(parsed.dropped_count == 12);
    CHECK(parsed.dropped == dropped);
    // A string whose bytes aren't UTF-8 is left out too.
    InstallScriptValues latin;
    CHECK(ParseInstallScript("\"InstallScript\" { \"Registry\" { \"HKEY_CURRENT_USER\\\\Software\\\\S\" { \"string\" { "
                             "\"Name\" \"Caf\xe9\" } } } }",
                             "Z:\\g", "", &latin, &reason));
    CHECK(latin.user.empty() && latin.dropped == std::vector<std::string>{"HKEY_CURRENT_USER\\Software\\S Name: string isn't UTF-8 text"});
    // A UTF-16 script keeps a character past the Basic Multilingual Plane; a lone surrogate leaves its value out.
    // A control character in a name leaves it out too, and its note in the log has no line break.
    {
      std::string utf16 = "\xff\xfe";
      const auto put = [&utf16](unsigned unit) { utf16 += std::string{static_cast<char>(unit & 0xff), static_cast<char>(unit >> 8)}; };
      const auto put_text = [&put](std::string_view text) { for (const char c : text) put(static_cast<unsigned char>(c)); };
      put_text("\"InstallScript\" { \"Registry\" { \"HKEY_CURRENT_USER\\\\Software\\\\S\" { \"string\" { \"Save");
      put(0xd83d);
      put(0xde00);
      put_text("\" \"1\" \"Lone\" \"");
      put(0xd800);
      put_text("\" \"a\\nb\" \"2\" } } } }");
      InstallScriptValues wide;
      CHECK(ParseInstallScript(utf16, "Z:\\g", "", &wide, &reason));
      CHECK(wide.user.size() == 1 && wide.user[0].name == "Save\\xd83d\\xde00" && wide.user[0].data == "\"1\"");
      CHECK(wide.dropped == std::vector<std::string>({"HKEY_CURRENT_USER\\Software\\S Lone: string isn't UTF-8 text",
                                                       "HKEY_CURRENT_USER\\Software\\S a?b: name isn't text Playden can write"}));
    }
    // Only the first 16 values left out are named; all are counted.
    {
      std::string many = "\"InstallScript\" { \"Registry\" { \"HKEY_CURRENT_USER\\\\Software\\\\S\" { \"dword\" { ";
      for (int index = 0; index < 20; ++index) many += "\"V" + std::to_string(index) + "\" \"x\" ";
      many += "} } } }";
      InstallScriptValues capped;
      CHECK(ParseInstallScript(many, "Z:\\g", "", &capped, &reason));
      CHECK(capped.user.empty() && capped.dropped.size() == 16 && capped.dropped_count == 20);
    }
    // The escaped forms go into a hive as they are, and applying them again changes nothing. A 32-bit CLSID key Wine
    // already saved (in Software\Classes\Wow6432Node) gets the new value in its own section, not a second one.
    std::string hive = "WINE REGISTRY Version 2\n;; All keys relative to REGISTRY\\\\Machine\n\n#arch=win64\n\n"
                       "[Software\\\\Classes\\\\Wow6432Node\\\\CLSID\\\\{1}] 1750000000\n@=\"Old\"\n";
    CHECK(ApplyRegistryValues(&hive, parsed.machine, 1760000000, &reason));
    const auto clsid = std::string("[Software\\\\Classes\\\\Wow6432Node\\\\CLSID\\\\{1}]");
    CHECK(hive.find(clsid) != std::string::npos && hive.find(clsid, hive.find(clsid) + 1) == std::string::npos);
    CHECK(hive.find("@=\"Studio\"") != std::string::npos && hive.find("@=\"Old\"") == std::string::npos);
    CHECK(hive.find("\"Caf\\xe9\"=\"\\x00c7a va\"\n") != std::string::npos);
    const auto once = hive;
    CHECK(ApplyRegistryValues(&hive, parsed.machine, 1760000001, &reason));
    CHECK(hive == once);
  }
  // A language's value replaces a plain one of the same name, any case: a registry key holds one of each.
  const std::string overlapping =
      "\"InstallScript\" { \"Registry\" { \"HKEY_CURRENT_USER\\\\Software\\\\Studio\" { \"string\" { "
      "\"Language\" \"English\" \"russian\" { \"LANGUAGE\" \"Russian\" } } } } }";
  InstallScriptValues overlaps;
  CHECK(ParseInstallScript(overlapping, "Z:\\games\\1", "russian", &overlaps, &reason));
  CHECK(overlaps.user.size() == 1 && overlaps.user[0].data == "\"Russian\"");
  std::string applied_registry = "WINE REGISTRY Version 2\n";
  CHECK(ApplyRegistryValues(&applied_registry, overlaps.user, 1760000000, &reason));
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

  // A script at the top named after its app (Singularity's 42670_install.vdf) is read as well.
  const auto named = base / "named";
  WriteText(named / "42670_install.vdf",
            "\"InstallScript\" { \"Run Process\" { \"PhysX Version\" { \"process 1\" \"msiexec\" "
            "\"command 1\" \"/i \\\"%INSTALLDIR%\\\\redist\\\\Chosen.msi\\\" /quiet\" } } }");
  WriteText(named / "redist/Chosen.msi", SamplePackage());
  WriteText(named / "_Redist/PhysX-Other.msi", "broken");
  prefix = fresh_prefix("named-prefix");
  result = InstallRedistributables(named.string(), "Z:\\games\\42670", prefix.string(), 1700000100, true);
  CHECK(result.outcomes.size() == 1 && result.outcomes[0].script == "42670_install.vdf" &&
        result.outcomes[0].detail == "redist/Chosen.msi" && result.outcomes[0].result == "installed" && !result.physx_searched);

  // A game whose depots flag their install scripts: those are the scripts, at any depth and whatever their names
  // (Cthulhu Saves the World's CSTW2.vdf); a script by name that isn't flagged is not run.
  const auto flagged = base / "flagged";
  WriteText(flagged / "Data/CSTW2.vdf",
            "\"InstallScript\" { \"Run Process\" { \"PhysX\" { \"process 1\" \"msiexec\" "
            "\"command 1\" \"/i \\\"%INSTALLDIR%\\\\redist\\\\Chosen.msi\\\" /quiet\" } } }");
  WriteText(flagged / "redist/Chosen.msi", SamplePackage());
  WriteText(flagged / "installscript.vdf",
            "\"InstallScript\" { \"Run Process\" { \"Other\" { \"process 1\" \"msiexec\" "
            "\"command 1\" \"/i \\\"%INSTALLDIR%\\\\redist\\\\Broken.msi\\\" /quiet\" } } }");
  WriteText(flagged / "redist/Broken.msi", "broken");
  WriteText(flagged / "real/s.vdf", "\"InstallScript\" { }");
  std::filesystem::create_directory_symlink(flagged / "real", flagged / "linked");
  WriteText(flagged / kInstallScriptList, "v1\nmissing.vdf\n../flagged/installscript.vdf\n/etc/x.vdf\nlinked/s.vdf\n"
                                          "Data/CSTW2.vdf\nData//CSTW2.vdf\nData\n");
  auto scripts = FindInstallScripts(flagged.string());
  CHECK(scripts.flagged && scripts.paths == std::vector<std::string>{"Data/CSTW2.vdf"});
  prefix = fresh_prefix("flagged-prefix");
  result = InstallRedistributables(flagged.string(), "Z:\\games\\107310", prefix.string(), 1700000100, true);
  CHECK(result.outcomes.size() == 1 && result.outcomes[0].script == "Data/CSTW2.vdf" &&
        result.outcomes[0].detail == "redist/Chosen.msi" && result.outcomes[0].result == "installed");
  // An empty list: the game has no install scripts.
  WriteText(flagged / kInstallScriptList, "v1\n");
  scripts = FindInstallScripts(flagged.string());
  CHECK(scripts.flagged && scripts.paths.empty());
  // A list Playden can't read is no list: the scripts are found by name.
  WriteText(flagged / kInstallScriptList, "v2\nData/CSTW2.vdf\n");
  scripts = FindInstallScripts(flagged.string());
  CHECK(!scripts.flagged && scripts.paths == std::vector<std::string>{"installscript.vdf"});

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
  // The installer added afterwards (from the web page, say): the next Play finds it.
  WriteText(bare / "_Redist/PhysX_9.09.msi", "not really a package");
  result = InstallRedistributables(bare.string(), "Z:\\games\\4", prefix.string(), 1700000200, true);
  CHECK(!result.physx_missing && result.physx_searched && result.outcomes.size() == 1 && result.outcomes[0].result == "failed");
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

// Entries a caller found itself (a GOG game's redist list): the same packages, setup programs and covered installers as
// a Steam script's, and with burn_bundles a Burn bundle's MsiPackages. A Steam script's bundle is not looked into.
void TestRedistEntries() {
  char pattern[] = "/tmp/playden-redist-XXXXXX";
  CHECK(::mkdtemp(pattern));
  const auto base = std::filesystem::canonical(pattern);
  const auto game = base / "game";
  const auto prefix = MakeBottle(base / "prefix");
  const std::string install_dir = "Z:\\games\\gog-7";
  const auto entry = [&](const std::string& name, const std::string& program, const std::string& command = {}) {
    auto windows = program;
    std::replace(windows.begin(), windows.end(), '/', '\\');
    return RedistEntry{name, install_dir + "\\" + windows, command, ""};
  };
  const auto run = [&](const std::vector<RedistEntry>& entries, bool burn_bundles = true) {
    return InstallRedistEntries(game.string(), install_dir, prefix.string(), 1700000100, entries, "gog", burn_bundles);
  };
  const auto recorded = [&] { return ReadText(prefix / ".playden-redist"); };

  // An explicit package installs its files and registry values, once.
  WriteText(game / ".playden-gog/redist/XNA/xnafx31_redist.msi",
            RuntimePackage("XNA Runtime", {{"xna.dll", "XNA-DLL"}, {"xna.txt", "XNA"}}));
  auto result = run({entry("XNA", ".playden-gog/redist/XNA/xnafx31_redist.msi", "/quiet")});
  CHECK(result.outcomes.size() == 1 && result.outcomes[0].script == "gog" && result.outcomes[0].name == "XNA");
  CHECK(result.outcomes[0].result == "installed" && result.outcomes[0].files == 2);
  CHECK(result.outcomes[0].detail == ".playden-gog/redist/XNA/xnafx31_redist.msi");
  CHECK(ReadText(prefix / "drive_c/Program Files (x86)/XNA Runtime/xna.dll") == "XNA-DLL");
  CHECK(ReadText(prefix / "system.reg").find("\"Installed\"=dword:00000001") != std::string::npos);
  CHECK(recorded().find(" .playden-gog/redist/XNA/xnafx31_redist.msi\n") != std::string::npos);
  result = run({entry("XNA", ".playden-gog/redist/XNA/xnafx31_redist.msi", "/quiet")});
  CHECK(result.outcomes.size() == 1 && result.outcomes[0].result == "already");

  // A setup program carrying an uncompressed package installs it.
  WriteText(game / ".playden-gog/redist/Embedded/setup.exe",
            "MZ" + std::string(300, '\x90') + RuntimePackage("Embedded Runtime", {{"e.dll", "EMBEDDED"}}) + std::string(100, '\x7f'));
  result = run({entry("Embedded", ".playden-gog/redist/Embedded/setup.exe")});
  CHECK(result.outcomes.size() == 1 && result.outcomes[0].result == "installed" && result.outcomes[0].files == 1);
  CHECK(result.outcomes[0].detail == ".playden-gog/redist/Embedded/setup.exe (its package)");
  CHECK(ReadText(prefix / "drive_c/Program Files (x86)/Embedded Runtime/e.dll") == "EMBEDDED");

  // A Burn bundle with one package, compressed as real ones are: its package isn't in the program's bytes.
  const auto bundle = RuntimeBundle({{"DesktopRuntime", RuntimePackage("dotnet", {{"hostfxr.dll", "FXR"}, {"shared.dll", "SHARED"}})}},
                                    true);
  CHECK(!FindEmbeddedMsi(bundle));
  WriteText(game / ".playden-gog/redist/DOTNET/windowsdesktop-runtime-8.0-win-x64.exe", bundle);
  const auto dotnet = entry("DOTNET", ".playden-gog/redist/DOTNET/windowsdesktop-runtime-8.0-win-x64.exe", "/install /quiet");
  // Without burn_bundles (Steam's way) it is not run.
  result = run({dotnet}, false);
  CHECK(result.outcomes.size() == 1 && result.outcomes[0].result == "not-run" &&
        result.outcomes[0].detail == "windowsdesktop-runtime-8.0-win-x64.exe");
  CHECK(!std::filesystem::exists(prefix / "drive_c/Program Files (x86)/dotnet"));
  result = run({dotnet});
  CHECK(result.outcomes.size() == 1 && result.outcomes[0].result == "installed" && result.outcomes[0].files == 2);
  CHECK(result.outcomes[0].detail == ".playden-gog/redist/DOTNET/windowsdesktop-runtime-8.0-win-x64.exe (DesktopRuntime)");
  CHECK(ReadText(prefix / "drive_c/Program Files (x86)/dotnet/hostfxr.dll") == "FXR");
  CHECK(ReadText(prefix / "drive_c/Program Files (x86)/dotnet/shared.dll") == "SHARED");
  CHECK(recorded().find(" .playden-gog/redist/DOTNET/windowsdesktop-runtime-8.0-win-x64.exe\n") != std::string::npos);
  result = run({dotnet});
  CHECK(result.outcomes.size() == 1 && result.outcomes[0].result == "already");

  // The same bundle named by a Steam install script is still not run.
  std::filesystem::remove_all(prefix / "drive_c/Program Files (x86)/dotnet");
  std::filesystem::remove(prefix / ".playden-redist");
  WriteText(game / "installscript.vdf",
            "\"InstallScript\" { \"Run Process\" { \"DOTNET\" { "
            "\"process 1\" \"%INSTALLDIR%\\\\.playden-gog\\\\redist\\\\DOTNET\\\\windowsdesktop-runtime-8.0-win-x64.exe\" } } }");
  result = InstallRedistributables(game.string(), install_dir, prefix.string(), 1700000100);
  CHECK(result.outcomes.size() == 1 && result.outcomes[0].result == "not-run");
  CHECK(!std::filesystem::exists(prefix / "drive_c/Program Files (x86)/dotnet"));
  std::filesystem::remove(game / "installscript.vdf");

  // Two packages, the first broken: the other installs, the broken one is reported, and the bundle isn't recorded.
  WriteText(game / "redist/two.exe",
            RuntimeBundle({{"Broken", "not a package"}, {"Good", RuntimePackage("Good Runtime", {{"g.dll", "GOOD"}})}}, false));
  result = run({entry("Two", "redist/two.exe")});
  CHECK(result.outcomes.size() == 2);
  CHECK(result.outcomes[0].result == "failed" && result.outcomes[0].detail.rfind("redist/two.exe (Broken): ", 0) == 0);
  CHECK(result.outcomes[1].result == "installed" && result.outcomes[1].detail == "redist/two.exe (Good)");
  CHECK(ReadText(prefix / "drive_c/Program Files (x86)/Good Runtime/g.dll") == "GOOD");
  CHECK(recorded().find("redist/two.exe") == std::string::npos);
  // A second entry naming it in the same run fails without another try.
  result = run({entry("Two", "redist/two.exe"), entry("Two again", "redist/two.exe")});
  CHECK(result.outcomes.size() == 3 && result.outcomes[2].result == "failed" &&
        result.outcomes[2].detail.rfind("redist/two.exe: Broken: ", 0) == 0);

  // A bundle of programs only (UE4's prerequisites): not run.
  WriteText(game / "redist/UE4PrereqSetup_x64.exe", RuntimeBundle({}, false, "prereq"));
  result = run({entry("UE4", "redist/UE4PrereqSetup_x64.exe")});
  CHECK(result.outcomes.size() == 1 && result.outcomes[0].result == "not-run" &&
        result.outcomes[0].detail == "redist/UE4PrereqSetup_x64.exe: no Windows Installer package");

  // Wine's own: a Visual C++ bundle is covered and never opened (it isn't even there).
  result = run({entry("MSVC2019", "redist/MSVC2019/VC_redist.x64.exe", "/install /quiet")});
  CHECK(result.outcomes.size() == 1 && result.outcomes[0].result == "covered" && result.outcomes[0].detail == "vc_redist.x64.exe");
  // A package that isn't there fails; a program outside the game folder is not run.
  result = run({entry("Missing", "redist/missing.msi")});
  CHECK(result.outcomes.size() == 1 && result.outcomes[0].result == "failed" &&
        result.outcomes[0].detail == "redist/missing.msi: the package could not be read");
  result = run({RedistEntry{"Outside", "C:\\Windows\\setup.exe", "", ""}});
  CHECK(result.outcomes.size() == 1 && result.outcomes[0].result == "not-run" && result.outcomes[0].detail == "setup.exe");
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

// Microsoft's x64 bundle is 25 MB and carries an 11 MB package Playden doesn't use: neither may land on the heap
// (a full pool closes Playden on the console). Without the fix this held the whole bundle, and the unused member
// on top of it, twice over while it grew.
void TestMfcInstallMemory() {
  constexpr std::size_t kFiller = 8u << 20, kLimit = 1u << 20;
  const auto bundle = MfcBundle(true, true, true, kFiller);
  std::string reason;
  {
    BurnMsiPackage package;
    const heap::Watch watch;
    CHECK(ReadBurnMsiPackage(bundle, "vcRuntimeAdditional_x64", &package, &reason));
    std::fprintf(stderr, "burn package read: %zu bytes above the bundle\n", watch.Above());
    CHECK(watch.Above() < kLimit);
    CHECK(package.msi == MfcPackage(true) && package.beside.at("cab1.cab") == MfcCabinet(true));
  }
  char pattern[] = "/tmp/playden-mfc-memory-XXXXXX";
  CHECK(::mkdtemp(pattern));
  const auto base = std::filesystem::canonical(pattern);
  const auto redist = SyntheticRedist(true, bundle);
  WriteText(base / "cache/vc-14.44.35211/x64/VC_redist.x64.exe", bundle);
  const auto prefix = MakeBottle(base / "prefix");
  const heap::Watch watch;
  const auto result = InstallMfcRuntime(redist, (base / "cache").string(), prefix.string(), nullptr, 1700000000);
  std::fprintf(stderr, "mfc install from the kept copy: %zu bytes held at most\n", watch.Above());
  CHECK(result.result == "installed" && result.source == "cache" && result.files == 2);
  CHECK(watch.Above() < kLimit);
  CHECK(ReadText(prefix / "drive_c/windows/system32/mfc140u.dll") == "MFC64U");
  std::filesystem::remove_all(base);
}

// Visual C++ 2005's MFC: its IExpress setup's cabinet holds vcredist.msi and vcredis1.cab. Only the MFC and MFCLOC
// assembly folders go into the bottle's winsxs, with their manifests again in winsxs\manifests under the folder's name
// (how Wine finds an assembly); the policy and the C runtime stay out. The bottle's own Windows folder is lower case.
void TestMfc80Install() {
  CHECK(Mfc80AssemblyFile(std::string("Windows/winsxs/") + playden::test::msi::kVc2005MfcFolder + "/mfc80.dll"));
  CHECK(Mfc80AssemblyFile(std::string("windows/WinSxS/") + playden::test::msi::kVc2005LocFolder + "/x.manifest"));
  CHECK(!Mfc80AssemblyFile("Windows/winsxs/x86_microsoft.vc80.crt_1fc8b3b9a1e18e3b_8.0.50727.6195_none_d0/msvcr80.dll"));
  CHECK(!Mfc80AssemblyFile("Windows/winsxs/x86_policy.8.0.microsoft.vc80.mfc_1fc8b3b9a1e18e3b_8.0_none_49/8.0.policy"));
  CHECK(!Mfc80AssemblyFile("Windows/system32/mfc80.dll") && !Mfc80AssemblyFile("Windows/winsxs/x86_microsoft.vc80.mfc_a/b/c.dll"));
  CHECK(!Mfc80AssemblyFile("Windows/winsxs/x86_microsoft.vc80.mfc_a/") && !Mfc80AssemblyFile("Windows/winsxs/manifests"));

  char pattern[] = "/tmp/playden-mfc80-XXXXXX";
  CHECK(::mkdtemp(pattern));
  const auto base = std::filesystem::canonical(pattern);
  const auto setup = playden::test::msi::Vc2005Setup();
  const VcRedist redist{"x86", "https://download.example/vcredist_x86.EXE", *HashArtifactBytes(setup), setup.size(),
                        "vcredist.msi", "8.0.50727.6195", "80"};
  int fetches = 0;
  const RangeFetch fetch = [&](const std::string&, std::uint64_t offset, std::uint64_t length, std::string*) {
    ++fetches;
    return std::optional<std::string>(setup.substr(offset, length));
  };
  const auto prefix = MakeBottle(base / "prefix");
  auto result = InstallMfcRuntime(redist, (base / "cache").string(), prefix.string(), fetch, 1700000000);
  if (result.result != "installed") std::fprintf(stderr, "mfc80: %s\n", result.detail.c_str());
  CHECK(result.result == "installed" && result.source == "download" && fetches == 1);
  const auto sxs = prefix / "drive_c/windows/winsxs";
  const std::string mfc = playden::test::msi::kVc2005MfcFolder, loc = playden::test::msi::kVc2005LocFolder;
  CHECK(ReadText(sxs / mfc / "mfc80.dll") == "MFC80" && ReadText(sxs / mfc / "mfc80u.dll") == "MFC80U");
  CHECK(ReadText(sxs / loc / "mfc80ENU.dll") == "ENU");
  CHECK(ReadText(sxs / "manifests" / (mfc + ".manifest")) == "<assembly>MFC</assembly>");
  CHECK(ReadText(sxs / "manifests" / (loc + ".manifest")) == "<assembly>MFCLOC</assembly>");
  // MFC: 2 DLLs, its manifest and catalog; MFCLOC: a DLL and its manifest; the 2 manifest copies.
  CHECK(result.files == 8);
  std::size_t folders = 0;
  for (const auto& entry : std::filesystem::directory_iterator(sxs)) {
    ++folders;
    const auto name = entry.path().filename().string();
    CHECK(name == mfc || name == loc || name == "manifests");
  }
  CHECK(folders == 3);
  // The bottle's own folder takes them, as listed, whatever the host's file system makes of case.
  for (const auto& entry : std::filesystem::directory_iterator(prefix / "drive_c"))
    CHECK(entry.path().filename() == "windows");
  for (const auto& entry : std::filesystem::directory_iterator(prefix / "drive_c/windows"))
    CHECK(entry.path().filename() != "WinSxS" && entry.path().filename() != "Winsxs");
  CHECK(MfcInstalled(redist, prefix.string()));
  CHECK(ReadText(base / "cache/vc-8.0.50727.6195/x86/vcredist_x86.EXE") == setup);
  // Once is enough; another bottle takes the kept copy.
  CHECK(InstallMfcRuntime(redist, (base / "cache").string(), prefix.string(), fetch, 1700000100).result == "already");
  const auto second = MakeBottle(base / "second");
  result = InstallMfcRuntime(redist, (base / "cache").string(), second.string(), fetch, 1700000200);
  CHECK(result.result == "installed" && result.source == "cache" && fetches == 1);
  // A setup program without the package fails, and records nothing.
  const auto other = std::string("MZ") + std::string(0x200, '\0');
  const VcRedist broken{"x86", "https://download.example/other/vcredist_x86.EXE", *HashArtifactBytes(other), other.size(),
                        "vcredist.msi", "broken", "80"};
  const RangeFetch other_fetch = [&](const std::string&, std::uint64_t offset, std::uint64_t length, std::string*) {
    return std::optional<std::string>(other.substr(offset, length));
  };
  const auto third = MakeBottle(base / "third");
  result = InstallMfcRuntime(broken, (base / "cache").string(), third.string(), other_fetch, 1700000300);
  CHECK(result.result == "failed" && !std::filesystem::exists(third / ".playden-redist"));
  std::filesystem::remove_all(base);
}

// Microsoft's Visual C++ 2005 setup program, when PLAYDEN_TEST_VC2005_REDIST_X86 names a copy: installed from a cache
// seeded with it; the MFC and MFCLOC assemblies land where Wine looks them up.
void TestMicrosoftVc2005Redist() {
  const char* path = std::getenv("PLAYDEN_TEST_VC2005_REDIST_X86");
  if (!path || !*path) return;
  for (const auto& redist : MicrosoftVcRedists()) {
    if (redist.mfc != "80") continue;
    const auto setup = ReadText(path);
    CHECK(setup.size() == redist.bytes && HashArtifactBytes(setup) == redist.sha256);
    char pattern[] = "/tmp/playden-vc2005-XXXXXX";
    CHECK(::mkdtemp(pattern));
    const auto base = std::filesystem::canonical(pattern);
    WriteText(base / "cache/vc-8.0.50727.6195/x86/vcredist_x86.EXE", setup);
    const auto prefix = MakeBottle(base / "prefix");
    const auto result = InstallMfcRuntime(redist, (base / "cache").string(), prefix.string(), nullptr, 1700000000);
    std::fprintf(stderr, "vc2005: %s %s files=%zu\n", result.result.c_str(), result.detail.c_str(), result.files);
    CHECK(result.result == "installed" && result.source == "cache");
    const auto manifests = prefix / "drive_c/windows/winsxs/manifests";
    std::size_t found = 0;
    for (const auto& entry : std::filesystem::directory_iterator(manifests)) {
      const auto name = entry.path().filename().string();
      std::fprintf(stderr, "vc2005 manifest %s\n", name.c_str());
      const auto folder = prefix / "drive_c/windows/winsxs" / name.substr(0, name.size() - 9);
      CHECK(std::filesystem::is_directory(folder));
      if (name.rfind("x86_microsoft.vc80.mfc_", 0) == 0) {
        CHECK(std::filesystem::exists(folder / "mfc80.dll") && std::filesystem::exists(folder / "mfc80u.dll"));
        CHECK(ReadText(entry.path()).find("name=\"Microsoft.VC80.MFC\" version=\"8.0.50727.6195\"") != std::string::npos);
      }
      ++found;
    }
    CHECK(found == 2);
    std::filesystem::remove_all(base);
  }
}

// A merge module's folder that a custom action sets to a standard one goes there, not under the module's own
// Win\System (Visual C++ 2010's and 2012's MFC); a custom action on something else changes nothing.
void TestCustomActionFolders() {
  for (const bool x64 : {false, true}) {
    MsiInstallPlan plan;
    std::string reason;
    CHECK(ReadMsiPackage(playden::test::msi::Vc2010Package(x64), &plan, &reason));
    CHECK(plan.files.size() == 4);
    for (const auto& file : plan.files) CHECK(file.target.rfind(x64 ? "windows/system32/" : "windows/syswow64/", 0) == 0);
    // An action a quiet install doesn't run (not in InstallExecuteSequence, or under a condition that doesn't hold)
    // places nothing: the package's own folders stand.
    for (const int sequence : {1, 2}) {
      CHECK(ReadMsiPackage(playden::test::msi::Vc2010Package(x64, sequence), &plan, &reason));
      CHECK(plan.files.size() == 4);
      for (const auto& file : plan.files) CHECK(file.target.rfind(x64 ? "Win/System64/" : "Win/System/", 0) == 0);
    }
  }
}

// MFC from a setup program Steam's depot holds: Visual C++ 2010's, whose cabinet names its members ".\.\vc_red.msi".
// Only MFC 10's files go in, where the package's custom action places them; recorded by version and architecture, so
// either source counts as installed.
void TestMfcFromProgram() {
  CHECK(MfcFileNameOf("mfc100u.dll", "100") && MfcFileNameOf("MFCM100.DLL", "100") && MfcFileNameOf("mfc100enu.dll", "100"));
  CHECK(!MfcFileNameOf("mfc140u.dll", "100") && !MfcFileNameOf("msvcr100.dll", "100") && !MfcFileName("mfc100u.dll"));
  CHECK(MfcAssemblyFile("Windows/winsxs/amd64_microsoft.vc90.mfc_1fc8b3b9a1e18e3b_9.0.30729.6161_none_04/mfc90.dll", "90", "x64"));
  CHECK(MfcAssemblyFile("Windows/winsxs/x86_microsoft.vc90.mfcloc_1fc8b3b9a1e18e3b_9.0.30729.6161_none_49/mfc90enu.dll", "90", "x86"));
  CHECK(!MfcAssemblyFile("Windows/winsxs/x86_microsoft.vc90.mfc_1fc8b3b9a1e18e3b_9.0.30729.6161_none_4b/mfc90.dll", "90", "x64"));
  CHECK(!MfcAssemblyFile("Windows/winsxs/x86_microsoft.vc80.mfc_1fc8b3b9a1e18e3b_8.0_none_cb/mfc80.dll", "90", "x86"));
  CHECK(SteamVcRedistFor("100") && SteamVcRedistFor("100")->depot == 228983 && SteamVcRedistFor("140")->depot == 228989);
  CHECK(!SteamVcRedistFor("42"));

  char pattern[] = "/tmp/playden-mfc-steam-XXXXXX";
  CHECK(::mkdtemp(pattern));
  const auto base = std::filesystem::canonical(pattern);
  const auto depot = base / "228983";
  WriteText(depot / "_CommonRedist/vcredist/2010/vcredist_x86.exe", playden::test::msi::Vc2010Setup(false));
  WriteText(depot / "_CommonRedist/vcredist/2010/vcredist_x64.exe", playden::test::msi::Vc2010Setup(true));
  CHECK(FindRedistProgram(depot.string(), "VCREDIST_X86.EXE") == (depot / "_CommonRedist/vcredist/2010/vcredist_x86.exe").string());
  CHECK(FindRedistProgram(depot.string(), "vc_redist.x86.exe").empty());
  const auto prefix = MakeBottle(base / "prefix");
  const std::vector<VcRedist> pinned = MicrosoftVcRedists();
  CHECK(!MfcInstalledFrom(prefix.string(), "100", "x86", pinned));
  for (const bool x64 : {false, true}) {
    const auto program = FindRedistProgram(depot.string(), x64 ? "vcredist_x64.exe" : "vcredist_x86.exe");
    auto result = InstallMfcFromProgram(program, "100", x64 ? "x64" : "x86", prefix.string(), 1700000000);
    if (result.result != "installed") std::fprintf(stderr, "mfc100: %s\n", result.detail.c_str());
    CHECK(result.result == "installed" && result.source == "steam" && result.files == 3);
    CHECK(InstallMfcFromProgram(program, "100", x64 ? "x64" : "x86", prefix.string(), 1700000100).result == "already");
  }
  const auto windows = prefix / "drive_c/windows";
  CHECK(ReadText(windows / "syswow64/mfc100u.dll") == "MFC100U-32" && ReadText(windows / "syswow64/mfcm100.dll") == "MFCM100-32");
  CHECK(ReadText(windows / "system32/mfc100u.dll") == "MFC100U-64");
  CHECK(!std::filesystem::exists(windows / "syswow64/msvcr100.dll") && !std::filesystem::exists(windows / "system32/msvcr100.dll"));
  CHECK(!std::filesystem::exists(prefix / "drive_c/Win"));
  CHECK(MfcInstalledFrom(prefix.string(), "100", "x86", pinned) && MfcInstalledFrom(prefix.string(), "100", "x64", pinned));
  CHECK(!MfcInstalledFrom(prefix.string(), "140", "x86", pinned));
  // A pinned download's record counts as well.
  const auto bundle = MfcBundle(false);
  const auto redist = SyntheticRedist(false, bundle);
  const RangeFetch fetch = [&](const std::string&, std::uint64_t offset, std::uint64_t length, std::string*) {
    return std::optional<std::string>(bundle.substr(offset, length));
  };
  CHECK(InstallMfcRuntime(redist, (base / "cache").string(), prefix.string(), fetch, 1700000200).result == "installed");
  CHECK(MfcInstalledFrom(prefix.string(), "140", "x86", {redist}) && !MfcInstalledFrom(prefix.string(), "140", "x86", pinned));
  // The wrong program for the version, or none, fails and records nothing.
  const auto other = MakeBottle(base / "other");
  auto result = InstallMfcFromProgram((depot / "_CommonRedist/vcredist/2010/vcredist_x86.exe").string(), "120", "x86",
                                      other.string(), 1700000000);
  CHECK(result.result == "failed" && !std::filesystem::exists(other / ".playden-redist"));
  CHECK(InstallMfcFromProgram((base / "missing.exe").string(), "100", "x86", other.string(), 1).result == "failed");
  std::filesystem::remove_all(base);
}

// Steam's own Visual C++ setup programs, when PLAYDEN_TEST_STEAM_REDIST names a folder holding depots of app 228980
// (<folder>/<depot>/..., as playden-steam-dev shared-redist keeps them): each version's MFC installs for both
// architectures.
void TestSteamVcRedists() {
  const char* folder = std::getenv("PLAYDEN_TEST_STEAM_REDIST");
  if (!folder || !*folder) return;
  char pattern[] = "/tmp/playden-steam-vc-XXXXXX";
  CHECK(::mkdtemp(pattern));
  const auto base = std::filesystem::canonical(pattern);
  for (const auto& redist : SteamVcRedists()) {
    const auto depot = std::filesystem::path(folder) / std::to_string(redist.depot);
    if (!std::filesystem::is_directory(depot)) continue;
    for (const char* architecture : {"x86", "x64"}) {
      const auto program = FindRedistProgram(depot.string(), std::string(architecture) == "x64" ? redist.x64_program : redist.x86_program);
      CHECK(!program.empty());
      const auto prefix = MakeBottle(base / (redist.mfc + architecture));
      const auto result = InstallMfcFromProgram(program, redist.mfc, architecture, prefix.string(), 1700000000);
      std::fprintf(stderr, "steam vc mfc=%s arch=%s: %s %s files=%zu\n", redist.mfc.c_str(), architecture,
                   result.result.c_str(), result.detail.c_str(), result.files);
      CHECK(result.result == "installed" && result.files >= 2);
      const auto windows = prefix / "drive_c/windows";
      if (redist.mfc == "80" || redist.mfc == "90") {
        std::size_t manifests = 0;
        for (const auto& entry : std::filesystem::directory_iterator(windows / "winsxs/manifests")) {
          CHECK(entry.path().filename().string().rfind(std::string(std::string(architecture) == "x64" ? "amd64" : "x86") +
                                                        "_microsoft.vc" + redist.mfc + ".mfc", 0) == 0);
          ++manifests;
        }
        CHECK(manifests == 2);
      } else {
        const auto system = windows / (std::string(architecture) == "x64" ? "system32" : "syswow64");
        CHECK(std::filesystem::exists(system / ("mfc" + redist.mfc + "u.dll")));
        // The C runtime stays Wine's (MakeBottle's stand-in for its msvcp140.dll included).
        CHECK(!std::filesystem::exists(system / ("msvcr" + redist.mfc + ".dll")));
        const auto msvcp = system / ("msvcp" + redist.mfc + ".dll");
        CHECK(!std::filesystem::exists(msvcp) || ReadText(msvcp) == "WINE-BUILTIN");
      }
    }
  }
  std::filesystem::remove_all(base);
}

// PhysX from Steam's shared depots: the package for the engine a game needs, installed from the depot's file (an
// .msi, or a setup program carrying one), once per bottle.
void TestSteamPhysx() {
  CHECK(SteamPhysxFor("2.8.1") && SteamPhysxFor("2.8.1")->depot == 229033 && SteamPhysxFor("2.7.1")->depot == 229033);
  CHECK(SteamPhysxFor("2.8.3")->package == "PhysX-9.14.0702-SystemSoftware.msi");
  // A program that doesn't say: the package all the same. An engine no Steam package has: none.
  CHECK(SteamPhysxFor("") && SteamPhysxFor("")->depot == 229033);
  CHECK(!SteamPhysxFor("2.6.4") && !SteamPhysxFor("2.7.2") && !SteamPhysxFor("2.8.4"));
  char pattern[] = "/tmp/playden-steam-physx-XXXXXX";
  CHECK(::mkdtemp(pattern));
  const auto base = std::filesystem::canonical(pattern);
  const auto depot = base / "229033/_CommonRedist/PhysX/9.14.0702";
  WriteText(depot / "PhysX-9.14.0702-SystemSoftware.msi", SamplePackage());
  const auto prefix = MakeBottle(base / "prefix");
  std::string reported;
  auto outcome = InstallPhysxPackage((depot / "PhysX-9.14.0702-SystemSoftware.msi").string(), "PhysX", prefix.string(),
                                     1700000000, [&](const std::string& name, std::size_t, std::size_t) { reported = name; });
  if (outcome.result != "installed") std::fprintf(stderr, "physx: %s\n", outcome.detail.c_str());
  CHECK(outcome.result == "installed" && outcome.files == 3 && outcome.name == "PhysX" && reported == "PhysX");
  CHECK(outcome.detail == "PhysX-9.14.0702-SystemSoftware.msi");
  CHECK(ReadText(prefix / "drive_c/Program Files (x86)/Vendor Runtime/Common/Loader.dll") == "0123456789");
  CHECK(ReadText(prefix / ".playden-redist").find(" PhysX-9.14.0702-SystemSoftware.msi\n") != std::string::npos);
  CHECK(InstallPhysxPackage((depot / "PhysX-9.14.0702-SystemSoftware.msi").string(), "PhysX", prefix.string(), 1700000100)
            .result == "already");
  // A setup program carrying the package (8.09.04's): its package.
  const auto legacy = base / "229030/PhysX_8.09.04_SystemSoftware.exe";
  WriteText(legacy, "MZ" + std::string(4096, '\x90') + SamplePackage());
  const auto second = MakeBottle(base / "second");
  outcome = InstallPhysxPackage(legacy.string(), "PhysX", second.string(), 1700000000);
  CHECK(outcome.result == "installed" && outcome.files == 3);
  // Nothing to install: failed, nothing recorded.
  const auto broken = base / "broken.exe";
  WriteText(broken, "MZ" + std::string(4096, '\x90'));
  const auto third = MakeBottle(base / "third");
  outcome = InstallPhysxPackage(broken.string(), "PhysX", third.string(), 1700000000);
  CHECK(outcome.result == "failed" && !std::filesystem::exists(third / ".playden-redist"));
  CHECK(InstallPhysxPackage((base / "missing.msi").string(), "PhysX", third.string(), 1).result == "failed");
  std::filesystem::remove_all(base);
}

// Steam's own PhysX packages, when PLAYDEN_TEST_STEAM_REDIST names a folder holding depots of app 228980: each installs
// the engines SteamPhysxPackages lists, where PhysX's loader looks for them.
void TestSteamPhysxPackages() {
  const char* folder = std::getenv("PLAYDEN_TEST_STEAM_REDIST");
  if (!folder || !*folder) return;
  char pattern[] = "/tmp/playden-steam-physx-real-XXXXXX";
  CHECK(::mkdtemp(pattern));
  const auto base = std::filesystem::canonical(pattern);
  for (const auto& package : SteamPhysxPackages()) {
    const auto path = FindRedistProgram((std::filesystem::path(folder) / std::to_string(package.depot)).string(), package.package);
    CHECK(!path.empty());
    if (path.empty()) continue;
    const auto prefix = MakeBottle(base / std::to_string(package.depot));
    // Its 56 MB cabinet's sectors don't follow one another: copied to disk and mapped, not into the heap.
    const heap::Watch watch;
    const auto outcome = InstallPhysxPackage(path, "PhysX", prefix.string(), 1700000000);
    std::fprintf(stderr, "steam physx depot=%u: %s %s files=%zu heap=%zu\n", package.depot, outcome.result.c_str(),
                 outcome.detail.c_str(), outcome.files, watch.Above());
    CHECK(outcome.result == "installed");
    CHECK(watch.Above() < (20u << 20));
    CHECK(!std::filesystem::exists(prefix / (".playden-cabinet-" + std::to_string(::getpid()))));
    for (const auto& engine : package.engines) {
      if (!BottleHasPhysxEngine(prefix.string(), engine)) std::fprintf(stderr, "  engine %s missing\n", engine.c_str());
      CHECK(BottleHasPhysxEngine(prefix.string(), engine));
    }
  }
  std::filesystem::remove_all(base);
}

void TestMfcScanRecord() {
  char pattern[] = "/tmp/playden-mfc-scan-XXXXXX";
  CHECK(::mkdtemp(pattern));
  const auto base = std::filesystem::canonical(pattern);
  const auto prefix = MakeBottle(base / "prefix");
  CHECK(!ReadMfcScan(prefix.string()));
  CHECK(WriteMfcScan(prefix.string(), {"files-1", {{"x64-140", "Game_Data/Plugins/Chroma.dll"}}}));
  auto read = ReadMfcScan(prefix.string());
  CHECK(read && read->fingerprint == "files-1" &&
        read->programs == (std::map<std::string, std::string>{{"x64-140", "Game_Data/Plugins/Chroma.dll"}}));
  CHECK(WriteMfcScan(prefix.string(), {"", {}}));  // scanned, no MFC, no fingerprint
  read = ReadMfcScan(prefix.string());
  CHECK(read && read->fingerprint.empty() && read->programs.empty());
  CHECK(WriteMfcScan(prefix.string(), {"files-2", {{"x86-80", "FahrenheitSub.exe"}, {"x86-100", "Kenshi x64/A b.exe"}}}));
  read = ReadMfcScan(prefix.string());
  CHECK(read && read->programs.size() == 2 && read->programs.at("x86-80") == "FahrenheitSub.exe" &&
        read->programs.at("x86-100") == "Kenshi x64/A b.exe");
  // A scan from before every version was looked for isn't used: the folder is walked again.
  WriteText(prefix / ".playden-mfc", "mfc-scan 1\nfiles files-1\nx64 Game_Data/Plugins/Chroma.dll\n");
  CHECK(!ReadMfcScan(prefix.string()));
  WriteText(prefix / ".playden-mfc", "mfc-scan 2\nfiles files-1\nx86-mfc80 FahrenheitSub.exe\n");
  CHECK(!ReadMfcScan(prefix.string()));
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
    if (redist.mfc != "140") continue;  // Visual C++ 2005's: TestMicrosoftVc2005Redist
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

// Installers a staged publisher stands in for are covered; the same installer is not-run otherwise.
void TestPublisherInstallers() {
  char pattern[] = "/tmp/playden-redist-XXXXXX";
  CHECK(::mkdtemp(pattern));
  const auto base = std::filesystem::canonical(pattern);
  const auto game = base / "game", prefix = base / "prefix";
  WriteText(game / "installscript.vdf",
            "\"InstallScript\" { \"Run Process\" { "
            "\"Runtime\" { \"process 1\" \"%INSTALLDIR%\\\\Redist\\\\Example-Setup.exe\" } "
            "\"Launcher\" { \"process 1\" \"%INSTALLDIR%\\\\Setup\\\\Example-Launcher.exe\" } "
            "\"Other\" { \"process 1\" \"%INSTALLDIR%\\\\Setup\\\\Other-Setup.exe\" } } }");
  std::filesystem::create_directories(prefix);
  const auto plain = InstallRedistributables(game.string(), "Z:\\games\\42", prefix.string(), 1700000100);
  CHECK(plain.outcomes.size() == 3);
  for (const auto& outcome : plain.outcomes) CHECK(outcome.result == "not-run");
  const auto staged = InstallRedistributables(game.string(), "Z:\\games\\42", prefix.string(), 1700000100, false, {},
                                              {"redist/example-setup.exe", "Setup/Example-Launcher.exe"});
  CHECK(staged.outcomes.size() == 3);
  CHECK(staged.outcomes[0].name == "Runtime" && staged.outcomes[0].result == "covered" &&
        staged.outcomes[0].detail == "publisher: Redist/Example-Setup.exe");
  CHECK(staged.outcomes[1].result == "covered" && staged.outcomes[1].detail == "publisher: Setup/Example-Launcher.exe");
  CHECK(staged.outcomes[2].name == "Other" && staged.outcomes[2].result == "not-run");
  std::filesystem::remove_all(base);
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
  TestPublisherInstallers();
  TestEmbeddedPackage();
  TestRedistEntries();
  TestFoundPhysx();
  TestPhysxPackage();
  TestPhysxSetupProgram();
  TestMfcFilter();
  TestMfcInstall();
  TestMfcInstallMemory();
  TestMfc80Install();
  TestMicrosoftVc2005Redist();
  TestCustomActionFolders();
  TestMfcFromProgram();
  TestSteamVcRedists();
  TestSteamPhysx();
  TestSteamPhysxPackages();
  TestMfcScanRecord();
  TestMicrosoftVcRedist();
  return 0;
}
