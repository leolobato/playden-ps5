#pragma once

#include "runtime/game_settings.h"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace playden::runtime {

// A Windows Installer package (.msi) read the way `msiexec /i <package> /quiet` would install it
// for all users on 64-bit Windows 7, without running anything: the console can't start the
// installer, so Playden copies the package's files and writes its registry and PATH values into
// the bottle itself (runtime/redist_install.h). Custom actions, UI, patches and self-registration
// are not run. Paths are relative to the bottle's drive_c with '/' separators.

// One file the package installs, taken from cabinet `cabinet` by its member name (File.File).
struct MsiFile {
  std::string target;   // e.g. "Program Files (x86)/NVIDIA Corporation/PhysX/Common/PhysXLoader.dll"
  std::string member;   // the cabinet member name
  std::size_t cabinet = 0;  // index into MsiInstallPlan::cabinets
  std::uint32_t size = 0;
};

// A cabinet holding files: a stream in the package ("#name" in the Media table) or a file
// beside it.
struct MsiCabinet {
  std::string name;
  bool embedded = false;
};

struct MsiInstallPlan {
  std::string product_code;
  std::string product_name;
  std::string product_version;
  std::vector<MsiCabinet> cabinets;
  std::vector<MsiFile> files;
  std::vector<RegistryValue> machine;  // system.reg (HKEY_LOCAL_MACHINE)
  std::vector<RegistryValue> user;     // user.reg (HKEY_CURRENT_USER)
  // Folders (Windows paths) the package adds to the machine-wide PATH.
  std::vector<std::string> path_prepend;
  std::vector<std::string> path_append;
  std::size_t skipped_files = 0;      // under a per-user folder, which this installer leaves alone
  std::size_t unsupported_files = 0;  // uncompressed beside the package, or a name the bottle can't hold
  std::size_t skipped_registry = 0;  // binary or multi-string values, HKEY_USERS
  std::size_t custom_actions = 0;    // not run
};

// Reads a package. false with a reason for a file that is not a readable package; files it
// can't install are counted (skipped_files, unsupported_files) for the caller to judge.
bool ReadMsiPackage(std::string_view package, MsiInstallPlan* plan, std::string* reason);

// One stream of the package by its decoded name ("media1.cab"); nullopt when missing.
std::optional<std::string> ReadMsiStream(std::string_view package, const std::string& name);

// The same stream in place, without a copy, when its sectors follow one another in the package (a
// large cabinet's usually do); nullopt otherwise, and the caller reads it with ReadMsiStream.
std::optional<std::string_view> ViewMsiStream(std::string_view package, const std::string& name);
// A large stream's bytes in order, in pieces handed to sink (each in place in package), so a stream whose sectors don't
// follow one another needn't be copied whole into memory. False when it isn't one (a small stream: ReadMsiStream), the
// package doesn't read, or sink returns false.
bool CopyMsiStream(std::string_view package, const std::string& name, const std::function<bool(std::string_view)>& sink);

// Extracts the files of a cabinet (MSZIP, LZX or stored folders; Quantum is refused), calling sink
// with each member's name and contents in cabinet order; the sink may move the contents away. With
// wants, only the members it accepts are collected: the others are decoded past without being kept,
// and a folder holding none of them isn't decoded. Stops at the first sink that returns false.
// Cabinets that continue into another one are refused.
bool ExtractCabinet(std::string_view cabinet, const std::function<bool(const std::string&, std::string&)>& sink,
                    std::string* reason, const std::function<bool(const std::string&)>& wants = {});

// Evaluates a Windows Installer condition ("VersionNT64", "NOT Installed AND (A = 1)") against
// properties; missing properties are empty. nullopt when the text can't be parsed.
std::optional<bool> EvaluateMsiCondition(std::string_view condition, const std::map<std::string, std::string>& properties);

}  // namespace playden::runtime
