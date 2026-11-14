#pragma once

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace playden::runtime {

// A WiX Burn bundle (a setup .exe such as Microsoft's VC_redist.x64.exe) read without running it: its
// .wixburn section locates the UX container, whose file "0" is the bundle's manifest, and the attached
// container that holds the packages. Both containers are cabinets (runtime/msi_package.h ExtractCabinet).

// One Windows Installer package of the bundle, by its MsiPackage Id: the .msi and the bundle's other
// payloads for it (its external cabinets), by the file name of their FilePath ("cab1.cab").
struct BurnMsiPackage {
  std::string msi_name;  // the package's file name ("vc_runtimeAdditional_x64.msi")
  std::string msi;
  std::map<std::string, std::string> beside;  // file name → bytes
};

// false with a reason when the bundle, its manifest or the package can't be read. Payloads are found
// by the manifest's ids (SourcePath), never by position, and each must be the size the manifest lists.
bool ReadBurnMsiPackage(std::string_view bundle, const std::string& package_id, BurnMsiPackage* package,
                        std::string* reason);

// The Ids of the bundle's MsiPackages, in the manifest's order (none for a bundle of other programs only). false
// with a reason when the bundle or its manifest can't be read, or an attached container the manifest lists isn't
// found (a bundle cut short).
bool ReadBurnMsiPackageIds(std::string_view bundle, std::vector<std::string>* ids, std::string* reason);

// Whether a program's first bytes (its PE headers and section table) name a .wixburn section: what every Burn
// bundle has, checked before a program is read whole.
bool HasBurnSection(std::string_view head);

}  // namespace playden::runtime
