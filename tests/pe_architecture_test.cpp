// Copyright (c) 2026 Playden contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime/pe_architecture.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <sys/stat.h>
#include <string>
#include <unistd.h>

namespace {

void Check(bool okay, const char* message) {
  if (okay) return;
  std::fprintf(stderr, "%s\n", message);
  std::exit(1);
}

}  // namespace

int main() {
  using playden::runtime::ExecutableArchitecture;
  const std::string path = "pe_architecture_test.exe";
  const std::string link = "pe_architecture_test_link.exe";
  std::array<unsigned char, 128> pe{};
  pe[0] = 'M'; pe[1] = 'Z'; pe[0x3c] = 64;
  pe[64] = 'P'; pe[65] = 'E';
  const auto write = [&](std::size_t size) {
    const int file = ::open(path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
    Check(file >= 0, "could not create the PE fixture");
    Check(::write(file, pe.data(), size) == static_cast<ssize_t>(size), "could not write the PE fixture");
    ::close(file);
  };
  const auto check = [&](unsigned char machine_lo, unsigned char machine_hi, unsigned char magic_lo,
                         unsigned char magic_hi, const char* expected, const char* message) {
    pe[68] = machine_lo; pe[69] = machine_hi;
    pe[88] = magic_lo; pe[89] = magic_hi;
    write(pe.size());
    Check(ExecutableArchitecture(path) == expected, message);
  };
  check(0x4c, 0x01, 0x0b, 0x01, "pe32", "i386 with a PE32 optional header is pe32");
  check(0x64, 0x86, 0x0b, 0x02, "pe64", "x86-64 with a PE32+ optional header is pe64");
  check(0x64, 0x86, 0x0b, 0x01, "", "x86-64 with a PE32 optional header is neither");
  check(0x4c, 0x01, 0x0b, 0x02, "", "i386 with a PE32+ optional header is neither");
  write(80);
  Check(ExecutableArchitecture(path).empty(), "a header cut short is neither");
  pe[0x3c] = 200;
  write(pe.size());
  Check(ExecutableArchitecture(path).empty(), "a PE offset past the end is neither");
  pe[0x3c] = 64;
  check(0x4c, 0x01, 0x0b, 0x01, "pe32", "fixture restored");
  std::remove(link.c_str());
  Check(::symlink(path.c_str(), link.c_str()) == 0, "could not create the link");
  Check(ExecutableArchitecture(link).empty(), "a symbolic link is not followed");
  Check(ExecutableArchitecture("pe_architecture_test_missing.exe").empty(), "a missing file is neither");
  const std::string fifo = "pe_architecture_test_fifo.exe";
  std::remove(fifo.c_str());
  Check(::mkfifo(fifo.c_str(), 0600) == 0, "could not create the FIFO");
  Check(ExecutableArchitecture(fifo).empty(), "a FIFO is refused without waiting for a writer");
  std::remove(fifo.c_str());
  Check(ExecutableArchitecture(".").empty(), "a folder is neither");
  std::remove(link.c_str());
  std::remove(path.c_str());
}
