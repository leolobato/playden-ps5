#include "library/cache.h"
#include <algorithm>
#include <limits>
#include <unordered_set>

namespace playden {
namespace {
constexpr std::size_t kMaximumCacheBytes = 16 * 1024 * 1024;
constexpr std::size_t kMaximumGames = 100000;
// Schema 5 names each game by store and value; 4 and older hold Steam app IDs only. 6 marks demos.
constexpr unsigned kLatestSchema = 7;
constexpr std::size_t kMaximumGameValue = 48;
const Bytes kMagic = {'P','D','L','I','B',0,1,0};
bool ValidAccount(const std::string& account) {
    return !account.empty() && account.size() <= 20 && account[0] != '0' &&
        std::all_of(account.begin(),account.end(),[](char c){return c>='0'&&c<='9';});
}
std::string Path(const std::string& account) { return "library-"+account+".cache"; }
std::uint32_t Checksum(const Bytes& bytes,std::size_t size) {
    std::uint32_t crc=0xffffffffu;
    for(std::size_t i=0;i<size;++i){crc^=bytes[i];for(int bit=0;bit<8;++bit)crc=(crc>>1)^(0xedb88320u & (0u-(crc&1u)));}
    return ~crc;
}
void Number(Bytes& out,std::uint64_t value,unsigned count) {
    for(unsigned i=0;i<count;++i){out.push_back(static_cast<std::uint8_t>(value));value>>=8;}
}
void String(Bytes& out,const std::string& value) {
    Number(out,value.size(),4);out.insert(out.end(),value.begin(),value.end());
}
struct Reader {
    const Bytes& bytes;
    std::size_t offset=0,limit=0;
    bool okay=true;
    std::uint64_t Number(unsigned count) {
        if(!okay||offset>limit||count>limit-offset){okay=false;return 0;}
        std::uint64_t value=0;
        for(unsigned i=0;i<count;++i)value|=std::uint64_t(bytes[offset++])<<(i*8);
        return value;
    }
    std::string String(std::size_t maximum) {
        auto count=Number(4);
        if(!okay||count>maximum||offset>limit||count>limit-offset){okay=false;return {};}
        std::string value(bytes.begin()+offset,bytes.begin()+offset+count);offset+=count;return value;
    }
};
bool ValidGame(const GameSummary& game,unsigned schema) {
    return (schema>=5?game.id.valid():game.id.steam_app().has_value()) && game.id.value.size()<=kMaximumGameValue &&
        !game.title.empty() && game.title.size()<=4096 &&
        (!game.last_played_unix_seconds || (*game.last_played_unix_seconds>0 &&
            *game.last_played_unix_seconds<=static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))) &&
        (!game.added_at_unix_seconds || (*game.added_at_unix_seconds>0 &&
            *game.added_at_unix_seconds<=static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))) &&
        game.artwork_source.size()<=8192 && game.description.size()<=65536 && game.developer.size()<=4096;
}
Result<void> Invalid(const char* message) {return Result<void>::Failure(StorageError::kInvalidData,message);}
}
Result<void> LibraryCache::Save(const std::string& account,const LibrarySnapshot& snapshot) const {
    if(!ValidAccount(account))return Invalid("Invalid cache account identifier.");
    if((snapshot.schema_version<1||snapshot.schema_version>kLatestSchema)||snapshot.games.size()>kMaximumGames||
        snapshot.retrieved_at_unix_seconds>static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))return Invalid("Unsupported library cache schema or size.");
    if(snapshot.schema_version<3&&snapshot.metadata_partial)return Invalid("Cache schema cannot preserve metadata state.");
    Bytes encoded=kMagic;
    encoded[6]=static_cast<std::uint8_t>(snapshot.schema_version);
    String(encoded,account);Number(encoded,snapshot.retrieved_at_unix_seconds,8);
    if(snapshot.schema_version>=3)Number(encoded,snapshot.metadata_partial?1u:0u,1);
    Number(encoded,snapshot.games.size(),4);
    std::unordered_set<GameId> seen;
    for(const auto& game:snapshot.games){
        if(!ValidGame(game,snapshot.schema_version)||!seen.insert(game.id).second)return Invalid("Invalid or duplicate cached game.");
        if(snapshot.schema_version==1 && (game.playtime_minutes || game.last_played_unix_seconds))return Invalid("Cache schema cannot preserve game history.");
        if(snapshot.schema_version<4 && game.added_at_unix_seconds)return Invalid("Cache schema cannot preserve acquisition dates.");
        if(snapshot.schema_version<6 && game.demo)return Invalid("Cache schema cannot preserve demos.");
        if(snapshot.schema_version<7 && game.family_shared)return Invalid("Cache schema cannot preserve family sharing.");
        if(snapshot.schema_version>=5){Number(encoded,static_cast<std::uint8_t>(game.id.store),1);String(encoded,game.id.value);}
        else Number(encoded,*game.id.steam_app(),4);
        String(encoded,game.title);String(encoded,game.artwork_source);String(encoded,game.description);String(encoded,game.developer);
        if(snapshot.schema_version>=2){
            Number(encoded,(game.playtime_minutes?1u:0u)|(game.last_played_unix_seconds?2u:0u)|(game.added_at_unix_seconds?4u:0u)|(game.demo?8u:0u)|(game.family_shared?16u:0u),1);
            if(game.playtime_minutes)Number(encoded,*game.playtime_minutes,8);
            if(game.last_played_unix_seconds)Number(encoded,*game.last_played_unix_seconds,8);
            if(game.added_at_unix_seconds)Number(encoded,*game.added_at_unix_seconds,8);
        }
        if(encoded.size()>kMaximumCacheBytes-4)return Invalid("Library cache exceeds its storage limit.");
    }
    Number(encoded,Checksum(encoded,encoded.size()),4);
    return storage_.WriteAtomic(Path(account),encoded);
}
Result<LibrarySnapshot> LibraryCache::Load(const std::string& account) const {
    auto invalid=[](){return Result<LibrarySnapshot>::Failure(StorageError::kInvalidData,"Library cache is corrupt or belongs to another account.");};
    if(!ValidAccount(account))return invalid();
    const auto read=storage_.Read(Path(account),kMaximumCacheBytes);
    if(!read.ok())return Result<LibrarySnapshot>::Failure(read.error(),read.message());
    const auto& bytes=read.value();
    if(bytes.size()<kMagic.size()+4||!std::equal(kMagic.begin(),kMagic.begin()+6,bytes.begin())||
        (bytes[6]<1||bytes[6]>kLatestSchema)||bytes[7]!=0)return invalid();
    Reader tail{bytes,bytes.size()-4,bytes.size()};
    if(tail.Number(4)!=Checksum(bytes,bytes.size()-4))return invalid();
    Reader reader{bytes,kMagic.size(),bytes.size()-4};
    if(reader.String(20)!=account)return invalid();
    LibrarySnapshot snapshot;
    snapshot.schema_version=bytes[6];
    snapshot.retrieved_at_unix_seconds=reader.Number(8);
    if(snapshot.retrieved_at_unix_seconds>static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))return invalid();
    if(snapshot.schema_version>=3){const auto partial=reader.Number(1);if(partial>1)return invalid();snapshot.metadata_partial=partial!=0;}
    const auto count=reader.Number(4);
    if(!reader.okay||count>kMaximumGames)return invalid();
    std::unordered_set<GameId> seen;
    for(std::uint64_t i=0;i<count;++i){
        GameSummary game;
        if(snapshot.schema_version>=5){
            // An unknown store byte makes an invalid GameId, which ValidGame refuses.
            game.id.store=static_cast<Store>(reader.Number(1));game.id.value=reader.String(kMaximumGameValue);
        } else {
            game.id=GameId::Steam(static_cast<std::uint32_t>(reader.Number(4)));
        }
        game.title=reader.String(4096);
        game.artwork_source=reader.String(8192);game.description=reader.String(65536);game.developer=reader.String(4096);
        if(snapshot.schema_version>=2){
            const auto flags=reader.Number(1);
            if(flags>(snapshot.schema_version>=7?31u:snapshot.schema_version>=6?15u:snapshot.schema_version>=4?7u:3u))return invalid();
            if(flags&1)game.playtime_minutes=reader.Number(8);
            if(flags&2)game.last_played_unix_seconds=reader.Number(8);
            if(flags&4)game.added_at_unix_seconds=reader.Number(8);
            game.demo=(flags&8)!=0;
            game.family_shared=(flags&16)!=0;
        }
        if(!reader.okay||!ValidGame(game,snapshot.schema_version)||!seen.insert(game.id).second)return invalid();
        snapshot.games.push_back(std::move(game));
    }
    if(reader.offset!=reader.limit)return invalid();
    return Result<LibrarySnapshot>::Success(std::move(snapshot));
}

Result<LibrarySnapshot> LibraryCache::Load(std::uint64_t account) const {
    if(!account)return Result<LibrarySnapshot>::Failure(StorageError::kInvalidData,"Invalid cache account identifier.");
    return Load(std::to_string(account));
}
Result<void> LibraryCache::Save(std::uint64_t account,const LibrarySnapshot& snapshot) const {
    if(!account)return Invalid("Invalid cache account identifier.");
    return Save(std::to_string(account),snapshot);
}
Result<void> LibraryCache::Clear(const std::string& account) const {
    if(!ValidAccount(account))return Invalid("Invalid cache account identifier.");
    const auto path=Path(account);
    const auto final_result=storage_.Remove(path);
    const auto temporary_result=storage_.Remove(path+".tmp");
    if(!final_result.ok())return final_result;
    if(!temporary_result.ok())return temporary_result;
    return Result<void>::Success();
}
Result<void> LibraryCache::Clear(std::uint64_t account) const {
    if(!account)return Invalid("Invalid cache account identifier.");
    return Clear(std::to_string(account));
}
} // namespace playden
