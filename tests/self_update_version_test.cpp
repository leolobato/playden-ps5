#include "update/version.h"
#include <cstdio>
#include <cstdlib>
using namespace playden::update;
#define CHECK(value) do { if (!(value)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #value); std::exit(1); } } while(false)
int main() {
  CHECK(ParseVersion("0.2.0") == (Version{0, 2, 0}));
  CHECK(ParseVersion("v0.3.10") == (Version{0, 3, 10}));
  CHECK(ParseVersion("00.002.000") == (Version{0, 2, 0}));  // param.json contentVersion
  CHECK(ParseVersion("01.000.000") == (Version{1, 0, 0}));
  for (const char* bad : {"", "v", "0.2", "0.2.0.1", "0.2.0-beta", "0..1", "a.b.c", " 0.2.0", "0.2.0 ", "1234567.0.0", "vv0.2.0"})
    CHECK(!ParseVersion(bad));
  CHECK((Version{0, 2, 0}) < (Version{0, 2, 1}));
  CHECK((Version{0, 2, 9}) < (Version{0, 10, 0}));
  CHECK((Version{0, 99, 99}) < (Version{1, 0, 0}));
  CHECK(!((Version{1, 0, 0}) < (Version{1, 0, 0})));
  CHECK((Version{0, 3, 10}).ToString() == "0.3.10");
  // The build's own version comes from CMake.
  CHECK(ParseVersion(PLAYDEN_VERSION) && CurrentVersion() == *ParseVersion(PLAYDEN_VERSION));
  const auto beta = ParseVersion("v0.7.1-beta.1");
  CHECK(beta);
  CHECK(beta->ToString() == "0.7.1-beta.1");
  CHECK(ParseVersion(beta->ToString()) == beta);
  CHECK(*ParseVersion("0.7.0") < *beta);
  CHECK(*beta < *ParseVersion("0.7.1-beta.2"));
  CHECK(*ParseVersion("0.7.1-beta.999999") < *ParseVersion("0.7.1"));
  CHECK(*beta != *ParseVersion("0.7.1"));
  for (const char* bad : {"0.7.1-rc.1", "0.7.1-beta.0", "0.7.1-beta.01", "0.7.1-beta.1000000",
                          "0.7.1-beta.-1", "0.7.1-beta.1+meta", "0.7.1+meta", "0.7.1-beta.1.2"})
    CHECK(!ParseVersion(bad));
  return 0;
}
