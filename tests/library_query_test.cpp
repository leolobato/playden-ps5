#include "library/query.h"
#include <cstdio>
#include <cstdlib>
#include "steam_id.h"
using namespace playden;
static void Check(bool condition,const char* description){
 if(!condition){std::fprintf(stderr,"FAIL: %s\n",description);std::exit(1);}
}
int main(){
 const std::vector<GameSummary> games={
  {SteamId(20),"Portal 2","","","Valve"}, {SteamId(10),"portal","","","Valve"},
  {SteamId(30),"ÉTÉ: L’Aventure!","","","Studio Étoile"},
  {SteamId(40),"Straße","","","Example"}, {SteamId(50),"星の旅","","","星工房"},
  {SteamId(60),"ŁÓDŹ","","","Example"}, {SteamId(70),"portal","","","Other"}};
 Check(QueryLibrary(games,{},LibrarySort::AppId)==std::vector<GameId>({SteamId(10),SteamId(20),SteamId(30),SteamId(40),SteamId(50),SteamId(60),SteamId(70)}),"empty query returns every supplied game");
 Check(QueryLibrary(games,{"PORTAL"})==std::vector<GameId>({SteamId(10),SteamId(70),SteamId(20)}),"case-insensitive Latin search and stable ID tie break");
 Check(QueryLibrary(games,{"été:"})==std::vector<GameId>({SteamId(30)}),"accented Latin casefold retains punctuation");
 Check(QueryLibrary(games,{"strasse"})==std::vector<GameId>({SteamId(40)}),"Latin multi-character casefold");
 Check(QueryLibrary(games,{"łódź"})==std::vector<GameId>({SteamId(60)}),"extended Latin casefold");
 Check(QueryLibrary(games,{"の旅"})==std::vector<GameId>({SteamId(50)}),"non-Latin substring search");
 Check(QueryLibrary(games,{"portal","VALVE"},LibrarySort::TitleDescending)==std::vector<GameId>({SteamId(20),SteamId(10)}),"developer filter uses real supplied metadata");
 Check(QueryLibrary(games,{"missing"}).empty(),"no results remains empty");
 Check(QueryLibrary(games,{""}).size()==7,"clearing query recovers full library");
 const std::vector<GameSummary> duplicates={{SteamId(2),"A"},{SteamId(2),"B"},{SteamId(0),"Invalid"},{SteamId(1),"A"}};
 Check(QueryLibrary(duplicates)==std::vector<GameId>({SteamId(1),SteamId(2)}),"deduplicate valid stable identities");
 auto malformed=games;malformed.push_back({SteamId(99),std::string("Bad\xff",4)});
 Check(QueryLibrary(malformed,{"portal"}).size()==3,"malformed unrelated UTF-8 cannot break query");
 Check(TrimLibraryQuery("  portal \t\r\n")=="portal","ASCII spaces around a search are trimmed");
 Check(TrimLibraryQuery("\xC2\xA0portal 2\xE3\x80\x80")=="portal 2","Unicode spaces around a search are trimmed, inner spaces kept");
 Check(TrimLibraryQuery(" \xC2\xA0\xE3\x80\x80 ").empty(),"a search of only spaces is empty");
 Check(TrimLibraryQuery(std::string(" bad\xff ",6))==std::string("bad\xff",4),"a malformed byte is kept as text");
 Check(TrimLibraryQuery(std::string(" \xff",2))==std::string("\xff",1),"a lone malformed byte is not trimmed away");
 std::puts("library_query_test passed");
}
