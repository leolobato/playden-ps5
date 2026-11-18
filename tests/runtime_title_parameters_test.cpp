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

  // The any-title shell's download-data request goes, and every other line stays as it was (#1001).
  const std::string shell =
      "{\n  \"attribute3\": 64,\n  \"contentVersion\": \"01.000.000\",\n  \"downloadDataSize\": 256,\n"
      "  \"gameIntent\": {\n    \"permittedIntents\": []\n  },\n  \"titleId\": \"FAKE78010\"\n}\n";
  std::string removed = shell;
  CHECK(TitleRequestsDownloadData(removed));
  CHECK(RemoveTitleDownloadData(&removed));
  CHECK(removed ==
        "{\n  \"attribute3\": 64,\n  \"contentVersion\": \"01.000.000\",\n"
        "  \"gameIntent\": {\n    \"permittedIntents\": []\n  },\n  \"titleId\": \"FAKE78010\"\n}\n");
  CHECK(!TitleRequestsDownloadData(removed));
  // Without the key there is nothing to do; a second pass changes nothing.
  const auto once = removed;
  CHECK(RemoveTitleDownloadData(&removed) && removed == once);
  std::string quoted = "{\"titleName\": \"\\\"downloadDataSize\\\": 1\"}";  // only text in a name
  const auto quoted_before = quoted;
  CHECK(RemoveTitleDownloadData(&quoted) && quoted == quoted_before);
  // The last key, the only key, and a value of 0.
  std::string last = "{\"titleId\": \"FAKE78010\",\n  \"downloadDataSize\": 256\n}";
  CHECK(RemoveTitleDownloadData(&last) && last == "{\"titleId\": \"FAKE78010\"\n}");
  std::string only = "{ \"downloadDataSize\":0 }";
  CHECK(RemoveTitleDownloadData(&only) && only == "{  }");
  std::string compact_first = "{\"downloadDataSize\":256,\"titleId\":\"FAKE78010\"}";
  CHECK(RemoveTitleDownloadData(&compact_first) && compact_first == "{\"titleId\":\"FAKE78010\"}");
  // With VRR set as WriteTitleMetadata does, in either order.
  std::string both = shell;
  CHECK(SetTitleVariableRefresh(&both, true) && RemoveTitleDownloadData(&both) && TitleAttribute3(both) == 0x40040u);
  CHECK(!TitleRequestsDownloadData(both));

  // Any other layout is refused and left as it is.
  for (std::string refused : {std::string("{\"downloadDataSize\": 256, \"downloadDataSize\": 256}"),
                              std::string("{\"downloadDataSize\": \"256\", \"titleId\": \"FAKE78010\"}"),
                              std::string("{\"downloadDataSize\": -1, \"titleId\": \"FAKE78010\"}"),
                              std::string("{\"downloadDataSize\": 2.5, \"titleId\": \"FAKE78010\"}"),
                              std::string("{\"downloadDataSize\": 2e8, \"titleId\": \"FAKE78010\"}"),
                              std::string("{\"pubtools\": {\"downloadDataSize\": 256}, \"titleId\": \"FAKE78010\"}"),
                              std::string("{\"downloadDataSize\": 256"),
                              std::string("[{\"downloadDataSize\": 256}]")}) {
    const auto before = refused;
    CHECK(!RemoveTitleDownloadData(&refused));
    CHECK(refused == before);
  }
  CHECK(!RemoveTitleDownloadData(nullptr));
  // The key spelled with an escape is the same key: refused, and never reported absent.
  std::string escaped = "{\"download\\u0044ataSize\": 256, \"titleId\": \"FAKE78010\"}";
  const auto escaped_before = escaped;
  CHECK(TitleRequestsDownloadData(escaped));
  CHECK(!RemoveTitleDownloadData(&escaped) && escaped == escaped_before);
  CHECK(TitleRequestsDownloadData("not json"));
  std::puts("title parameters ok");
  return 0;
}
