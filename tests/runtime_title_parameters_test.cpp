// SPDX-License-Identifier: GPL-3.0-or-later
#include "runtime/title_parameters.h"

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace playden::runtime;

#define CHECK(condition) do { if (!(condition)) { \
  std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
  std::exit(1); } } while (false)

int main() {
  // The any-title shell's layout: 120 Hz allowed (0x40) only.
  std::string param = "{\n  \"attribute\": 0,\n  \"attribute3\": 64,\n  \"titleId\": \"FAKE78010\"\n}\n";
  CHECK(TitleAttribute3(param) == 64u);
  CHECK(SetTitleVariableRefresh(&param, true));
  CHECK(TitleAttribute3(param) == 0x40040u);
  CHECK(param == "{\n  \"attribute\": 0,\n  \"attribute3\": 262208,\n  \"titleId\": \"FAKE78010\"\n}\n");
  // Setting it twice keeps one bit; clearing it gives the kit's value back.
  CHECK(SetTitleVariableRefresh(&param, true) && TitleAttribute3(param) == 0x40040u);
  CHECK(SetTitleVariableRefresh(&param, false) && TitleAttribute3(param) == 64u);
  CHECK(param == "{\n  \"attribute\": 0,\n  \"attribute3\": 64,\n  \"titleId\": \"FAKE78010\"\n}\n");
  // Last key, no spaces.
  std::string compact = "{\"titleId\":\"FAKE78010\",\"attribute3\":0}";
  CHECK(SetTitleVariableRefresh(&compact, true) && compact == "{\"titleId\":\"FAKE78010\",\"attribute3\":262144}");

  // Any other layout is refused and left as it is.
  for (std::string refused : {std::string("{\"titleId\": \"FAKE78010\"}"),
                              std::string("{\"attribute3\": 64, \"attribute3\": 64}"),
                              std::string("{\"attribute3\": \"64\"}"),
                              std::string("{\"attribute3\": -64}"),
                              std::string("{\"attribute3\": 64.0}"),
                              std::string("{\"attribute3\": 6e1}"),
                              std::string("{\"attribute3\": 064}"),
                              std::string("{\"attribute3\": 4294967296}"),
                              std::string("{\"attribute3\" 64}"),
                              std::string("{\"attribute3\": 64")}) {
    const auto before = refused;
    CHECK(!TitleAttribute3(refused));
    CHECK(!SetTitleVariableRefresh(&refused, true));
    CHECK(refused == before);
  }
  CHECK(!SetTitleVariableRefresh(nullptr, true));
  std::puts("title parameters ok");
  return 0;
}
