#include "steam/backend/vdf.h"

#include <cstdio>
#include <cstdlib>
#include <string>

#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      std::exit(1);                                                    \
    }                                                                  \
  } while (0)

namespace {

using playden::steam::kMaxVdfDepth;
using playden::steam::ParseVdf;
using playden::steam::VdfNode;

std::string Nested(std::size_t depth) {
  std::string text;
  for (std::size_t i = 0; i < depth; ++i) text += "\"k\" {";
  text += "\"leaf\" \"v\"";
  for (std::size_t i = 0; i < depth; ++i) text += "}";
  return text;
}

void TestNested() {
  const auto root = ParseVdf(
      "\"appinfo\"\n{\n  \"appid\" \"10\"\n  \"common\" { \"name\" \"Game\" \"id\" 7 }\n}\n");
  CHECK(root && root->is_dict);
  const VdfNode* app = root->Find("appinfo");
  CHECK(app && app->is_dict);
  CHECK(app->String("appid") == std::string_view("10"));
  CHECK(app->Uint64("appid") == 10U);
  const VdfNode* common = app->Find("common");
  CHECK(common);
  CHECK(common->String("name") == std::string_view("Game"));
  CHECK(common->Uint64("id") == 7U);
  CHECK(!common->Uint64("name"));
  CHECK(!common->String("missing"));
  CHECK(!app->String("common"));  // dict, not a string
  CHECK(app->children.size() == 2);
}

void TestCommentsAndTokens() {
  const auto root = ParseVdf("// header\nkey1 value1 // trailing\n  key2\t{ inner x }\r\nkey3 \"q\"");
  CHECK(root);
  CHECK(root->String("key1") == std::string_view("value1"));
  CHECK(root->Find("key2")->String("inner") == std::string_view("x"));
  CHECK(root->String("key3") == std::string_view("q"));
  CHECK(root->children.size() == 3);
  const auto adjacent = ParseVdf("a{b\"c\"}");
  CHECK(adjacent && adjacent->Find("a")->String("b") == std::string_view("c"));
  const auto empty = ParseVdf("  // only a comment\n");
  CHECK(empty && empty->is_dict && empty->children.empty());
}

void TestEscapes() {
  const auto root = ParseVdf(R"("a" "x\ny\tz\\w\"q\d")");
  CHECK(root);
  CHECK(root->String("a") == std::string_view("x\ny\tz\\w\"q\\d"));
}

void TestFind() {
  const auto root = ParseVdf("\"Name\" \"first\" \"NAME\" \"second\" \"n\u00c4\" \"x\"");
  CHECK(root);
  CHECK(root->String("name") == std::string_view("first"));
  CHECK(root->String("nAmE") == std::string_view("first"));
  CHECK(!root->Find("nam"));
  CHECK(!root->Find("names"));
  VdfNode leaf;
  leaf.value = "v";
  CHECK(leaf.Find("v") == nullptr);
}

void TestUint64() {
  const auto root = ParseVdf("a 18446744073709551615 b 18446744073709551616 c 12x d -1 e \"\"");
  CHECK(root);
  CHECK(root->Uint64("a") == 18446744073709551615ULL);
  CHECK(!root->Uint64("b"));
  CHECK(!root->Uint64("c"));
  CHECK(!root->Uint64("d"));
  CHECK(!root->Uint64("e"));
}

void TestCaps() {
  CHECK(ParseVdf(Nested(kMaxVdfDepth)));
  CHECK(!ParseVdf(Nested(kMaxVdfDepth + 1)));
  std::string big(playden::steam::kMaxVdfBytes + 1, ' ');
  CHECK(!ParseVdf(big));
  std::string many;
  for (std::size_t i = 0; i < playden::steam::kMaxVdfNodes + 1; ++i) many += "a b ";
  CHECK(many.size() <= playden::steam::kMaxVdfBytes);
  CHECK(!ParseVdf(many));
}

void TestMalformed() {
  CHECK(!ParseVdf("\"abc"));
  CHECK(!ParseVdf("\"k\" \"unterminated"));
  CHECK(!ParseVdf("key"));
  CHECK(!ParseVdf("a { b }"));
  CHECK(!ParseVdf("a { b c"));
  CHECK(!ParseVdf("a { b { c d } "));
  CHECK(!ParseVdf("a b }"));
  // A key without its value before a closing brace, and a dict without a key.
  CHECK(!ParseVdf("\"AppWorkshop\" { \"appid\" \"7\" \"Future\" } }"));
  CHECK(!ParseVdf("{ a b }"));
  CHECK(!ParseVdf("a { { b c } }"));
  CHECK(!ParseVdf(std::string("a b\0", 4)));
  CHECK(!ParseVdf(std::string("a \"b\0c\"", 7)));
}

}  // namespace

// Quoted braces are strings, as keys and as values; only bare ones open or close a dict.
void TestQuotedBraces() {
  const auto root = ParseVdf("\"AppWorkshop\" { \"appid\" \"7\" \"Future\" \"{\" \"}\" \"x\" \"open\" \"}\" }");
  CHECK(root && root->children.size() == 1);
  const auto* workshop = root->Find("AppWorkshop");
  CHECK(workshop && workshop->is_dict && workshop->children.size() == 4);
  CHECK(workshop->String("Future") == std::optional<std::string_view>("{"));
  CHECK(workshop->String("}") == std::optional<std::string_view>("x"));
  CHECK(workshop->String("open") == std::optional<std::string_view>("}"));
}

int main() {
  TestNested();
  TestCommentsAndTokens();
  TestEscapes();
  TestFind();
  TestUint64();
  TestCaps();
  TestMalformed();
  TestQuotedBraces();
  std::puts("steam_vdf_test: ok");
  return 0;
}
