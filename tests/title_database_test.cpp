#include "platform/ps5/title_database.h"
#include "sqlite3.h"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <unistd.h>
using namespace playden::ps5;
using playden::runtime::TitleAvailability;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%d: %s\n", __LINE__, #x); std::exit(1); } } while (0)
int main() {
  char directory[] = "/tmp/playden-title-db-XXXXXX"; CHECK(mkdtemp(directory));
  const auto path = (std::filesystem::canonical(directory) / "app.db").string();
  CHECK(ReadTitleDatabase(path, "FAKE78001").inventory == TitleAvailability::kUnknown);
  CHECK(!std::filesystem::exists(path)); // Read-only mode must never create a DB.
  sqlite3* db = nullptr; CHECK(sqlite3_open(path.c_str(), &db) == SQLITE_OK);
  auto sql = [&](const char* bytes) { CHECK(sqlite3_exec(db, bytes, nullptr, nullptr, nullptr) == SQLITE_OK); };
  CHECK(ReadTitleDatabase(path, "FAKE78001").inventory == TitleAvailability::kUnknown);
  sql("CREATE TABLE tbl_contentinfo(titleId TEXT,contentId TEXT,metaDataPath TEXT)");
  CHECK(ReadTitleDatabase(path, "FAKE78001").inventory == TitleAvailability::kFree);
  sql("INSERT INTO tbl_contentinfo VALUES('FAKE78001','UP9000-FAKE78001_00-PROSPEROWIN00001','/user/app/FAKE78001/sce_sys')");
  auto present = ReadTitleDatabase(path, "FAKE78001"); CHECK(present.inventory == TitleAvailability::kOccupied && present.identity);
  CHECK(present.identity->content_id == "UP9000-FAKE78001_00-PROSPEROWIN00001");
  CHECK(present.identity->metadata_path == "/user/app/FAKE78001/sce_sys");
  CHECK(ReadTitleDatabase(path, "FAKE78002").inventory == TitleAvailability::kFree);
  sql("INSERT INTO tbl_contentinfo VALUES('fake78001','foreign','foreign')");
  present = ReadTitleDatabase(path, "FAKE78001"); CHECK(present.inventory == TitleAvailability::kOccupied && !present.identity);
  CHECK(present.reason == "duplicate-rows");
  sql("DELETE FROM tbl_contentinfo; INSERT INTO tbl_contentinfo VALUES('fake78001','UP9000-FAKE78001_00-PROSPEROWIN00001','/user/app/FAKE78001/sce_sys')");
  CHECK(ReadTitleDatabase(path, "FAKE78001").reason == "title-id-case");
  sql("DELETE FROM tbl_contentinfo; INSERT INTO tbl_contentinfo VALUES('FAKE78001',NULL,NULL)");
  present = ReadTitleDatabase(path, "FAKE78001"); CHECK(present.inventory == TitleAvailability::kOccupied && !present.identity);
  CHECK(present.reason == "content-id-unreadable");
  sql("BEGIN EXCLUSIVE"); CHECK(ReadTitleDatabase(path, "FAKE78002").inventory == TitleAvailability::kUnknown); sql("ROLLBACK");
  CHECK(ReadTitleDatabase(path, "unsafe' OR 1=1").inventory == TitleAvailability::kUnknown);
  CHECK(ReadNativeTitleDatabase("FAKE78002").inventory == TitleAvailability::kUnknown);
  CHECK(sqlite3_close(db) == SQLITE_OK); std::filesystem::remove_all(directory);
  std::puts("title database tests passed");
}
