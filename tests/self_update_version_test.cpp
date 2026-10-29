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
  // And its commit from git, when the source is a checkout.
  CHECK(BuildCommit() == PLAYDEN_BUILD_COMMIT);
  CHECK(BuildCommit().empty() || BuildCommit().find_first_not_of("0123456789abcdef") == std::string::npos);
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
  // Nightly builds: before every beta and the stable release of their version, after the one before.
  const auto nightly = ParseVersion("0.8.0-nightly.20261027.2");
  CHECK(nightly && nightly->nightly() && nightly->nightly_date == 20261027 && nightly->nightly_build == 2);
  CHECK(nightly->ToString() == "0.8.0-nightly.20261027.2");
  CHECK(ParseVersion(nightly->ToString()) == nightly);
  CHECK(*ParseVersion("0.7.2") < *nightly);
  CHECK(*ParseVersion("0.8.0-nightly.20261027.1") < *nightly);
  CHECK(*nightly < *ParseVersion("0.8.0-nightly.20261028.1"));
  CHECK(*nightly < *ParseVersion("0.8.0-beta.1"));
  CHECK(*nightly < *ParseVersion("0.8.0"));
  CHECK(!(*ParseVersion("0.8.0") < *nightly) && !(*ParseVersion("0.8.0-beta.1") < *nightly));
  CHECK(*nightly < *ParseVersion("0.8.1-nightly.20200101.1"));
  CHECK(*nightly != *ParseVersion("0.8.0-nightly.20261027.3"));
  for (const char* bad : {"0.8.0-nightly", "0.8.0-nightly.20261027", "0.8.0-nightly.20261027.0", "0.8.0-nightly.20261027.01",
                          "0.8.0-nightly.20261027.1000", "0.8.0-nightly.2026102.1", "0.8.0-nightly.20261327.1",
                          "0.8.0-nightly.20261000.1", "0.8.0-nightly.020261027.1", "0.8.0-nightly.20261027.1.1",
                          "0.8.0-beta.1-nightly.20261027.1", "0.8.0-nightly.20261027.1-beta.1"})
    CHECK(!ParseVersion(bad));
  return 0;
}
