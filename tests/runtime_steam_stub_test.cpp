#include "runtime/steam_stub.h"

#include "steam_stub_fixture.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#define CHECK(cond)                                                                     \
  do {                                                                                  \
    if (!(cond)) {                                                                      \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);     \
      std::exit(1);                                                                     \
    }                                                                                   \
  } while (0)

using namespace playden::runtime;
namespace fs = std::filesystem;
namespace fx = steam_stub_fixture;

namespace {

void Write(const fs::path& path, const std::string& bytes, mode_t mode = 0755) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary | std::ios::trunc) << bytes;
  ::chmod(path.c_str(), mode);
}

std::string Read(const fs::path& path) {
  std::ifstream file(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

std::uint16_t Sections(const std::string& bytes) {
  const std::size_t pe = fx::Get32(bytes, 0x3c);
  return static_cast<std::uint16_t>(static_cast<unsigned char>(bytes[pe + 6]) |
                                    static_cast<unsigned char>(bytes[pe + 7]) << 8);
}

// The real program and Steamless's output for it, when the environment names them (not
// shipped: a game's program). Everything Windows loads must match; Steamless also clears
// the DOS stub and leaves stray bytes in unused section-name padding.
void CompareWithReference() {
  const char* sample = std::getenv("PLAYDEN_STEAMSTUB_SAMPLE");
  const char* reference = std::getenv("PLAYDEN_STEAMSTUB_REFERENCE");
  if (!sample || !reference) {
    std::printf("reference check skipped (PLAYDEN_STEAMSTUB_SAMPLE, PLAYDEN_STEAMSTUB_REFERENCE)\n");
    return;
  }
  CHECK(InspectSteamStub(sample).kind == SteamStubKind::kSupported);
  auto mine = Read(sample);
  const auto theirs = Read(reference);
  std::string why;
  CHECK(UnpackSteamStub(&mine, &why));
  if (const char* output = std::getenv("PLAYDEN_STEAMSTUB_OUTPUT")) Write(output, mine, 0644);
  if (mine.size() != theirs.size()) std::printf("  size %zu, Steamless %zu\n", mine.size(), theirs.size());
  CHECK(mine.size() == theirs.size());
  const std::size_t pe = fx::Get32(mine, 0x3c);
  const std::size_t table = pe + 24 + (static_cast<unsigned char>(mine[pe + 20]) | static_cast<unsigned char>(mine[pe + 21]) << 8);
  std::size_t differences = 0;
  for (std::size_t i = pe; i < mine.size(); ++i) {
    const bool name_padding = i >= table && i < table + Sections(mine) * 40u && (i - table) % 40 < 8 && mine[i] == '\0';
    if (mine[i] == theirs[i] || name_padding) continue;
    if (++differences <= 8) std::printf("  differs at 0x%zx: 0x%02x, Steamless 0x%02x\n", i,
                                        static_cast<unsigned char>(mine[i]), static_cast<unsigned char>(theirs[i]));
  }
  std::printf("reference check: %zu bytes differ after the DOS stub\n", differences);
  CHECK(differences == 0);
}

}  // namespace

int main() {
  const auto base = fs::temp_directory_path() / ("playden-steam-stub-" + std::to_string(::getpid()));
  fs::remove_all(base);

  {  // Unpacked in place: code decrypted, entry point back, .bind dropped.
    auto bytes = fx::Build();
    std::string why;
    CHECK(UnpackSteamStub(&bytes, &why));
    CHECK(bytes.substr(fx::kCodeRaw, fx::kCodeSize) == fx::PlainCode());
    CHECK(fx::Get32(bytes, fx::kOptional + 16) == fx::kOriginalEntry);
    CHECK(Sections(bytes) == 2);
    CHECK(fx::Get32(bytes, fx::kOptional + 56) == 0x3000);
    CHECK(bytes.size() == fx::kBindRaw);
    CHECK(bytes.substr(fx::kTable + 2 * 40, 40) == std::string(40, '\0'));
  }
  {  // A relocation into .bind keeps it; the code and entry point are still restored.
    fx::Spec spec;
    spec.reloc_into_bind = true;
    auto bytes = fx::Build(spec);
    const auto size = bytes.size();
    std::string why;
    CHECK(UnpackSteamStub(&bytes, &why));
    CHECK(bytes.substr(fx::kCodeRaw, fx::kCodeSize) == fx::PlainCode());
    CHECK(fx::Get32(bytes, fx::kOptional + 16) == fx::kOriginalEntry);
    CHECK(Sections(bytes) == 3 && bytes.size() == size && fx::Get32(bytes, fx::kOptional + 56) == 0x4000);
  }
  for (const std::uint64_t base : {0ull, 0x1000ull}) {  // A 32-bit program; Gunvolt's entry point is above its image base.
    fx::Spec spec;
    spec.pe32 = true;
    spec.image_base = base;
    auto bytes = fx::Build(spec);
    std::string why;
    CHECK(UnpackSteamStub(&bytes, &why));
    CHECK(bytes.substr(fx::kCodeRaw, fx::kCodeSize) == fx::PlainCode());
    CHECK(fx::Get32(bytes, fx::kOptional + 16) == fx::kOriginalEntry);
    CHECK(Sections(bytes) == 2 && bytes.size() == fx::kBindRaw);
  }
  {  // Check-only flags are fine; anything else or another variant is refused untouched.
    fx::Spec checks;
    checks.flags = 0x02 | 0x10 | 0x20 | 0x40;
    auto bytes = fx::Build(checks);
    std::string why;
    CHECK(UnpackSteamStub(&bytes, &why));
    for (const auto& [spec, detail] : {std::pair<fx::Spec, const char*>{{0x100}, "flags 0x100"},
                                       {{0, 0x12345678}, "no 3.x header"},
                                       {{0x100, 0xC0DEC0DF, 1, false, true}, "3.1 x86 with flags 0x100"}}) {
      auto refused = fx::Build(spec);
      const auto before = refused;
      CHECK(!UnpackSteamStub(&refused, &why) && refused == before);
      CHECK(why.find(detail) != std::string::npos);
    }
  }
  for (const bool pe32 : {false, true}) {  // Code not encrypted (flag 0x4): only the entry point comes back.
    fx::Spec spec;
    spec.flags = 0x04;
    spec.pe32 = pe32;
    auto bytes = fx::Build(spec);
    std::string why;
    CHECK(UnpackSteamStub(&bytes, &why));
    CHECK(bytes.substr(fx::kCodeRaw, fx::kCodeSize) == fx::PlainCode());
    CHECK(fx::Get32(bytes, fx::kOptional + 16) == fx::kOriginalEntry && Sections(bytes) == 2);
  }
  for (const std::size_t size : {std::size_t{0xD0}, std::size_t{0xB0}})  // Variant 3.0, both header sizes and bitnesses.
    for (const bool pe32 : {false, true})
      for (const std::uint32_t flags : {0u, 0x04u}) {
        fx::Spec spec;
        spec.header30 = size;
        spec.pe32 = pe32;
        spec.flags = flags;
        auto bytes = fx::Build(spec);
        const auto root = base / ("v30-" + std::to_string(size) + (pe32 ? "-32-" : "-64-") + std::to_string(flags));
        Write(root / "Game.exe", bytes);
        CHECK(InspectSteamStub((root / "Game.exe").string()).detail == std::string("3.0 ") + (pe32 ? "x86" : "x64"));
        std::string why;
        CHECK(UnpackSteamStub(&bytes, &why));
        CHECK(bytes.substr(fx::kCodeRaw, fx::kCodeSize) == fx::PlainCode());
        CHECK(fx::Get32(bytes, fx::kOptional + 16) == fx::kOriginalEntry && Sections(bytes) == 2);
      }
  for (const bool v30 : {false, true}) {  // Started from a TLS callback: entry restored, .bind kept (the callback points there).
    fx::Spec spec;
    spec.tls = true;
    spec.header30 = v30 ? 0xD0 : 0;
    auto bytes = fx::Build(spec);
    const auto size = bytes.size();
    std::string why;
    CHECK(UnpackSteamStub(&bytes, &why));
    CHECK(bytes.substr(fx::kCodeRaw, fx::kCodeSize) == fx::PlainCode());
    CHECK(fx::Get32(bytes, fx::kOptional + 16) == fx::kOriginalEntry && Sections(bytes) == 3 && bytes.size() == size);
  }
  {  // 3.0 x64 whose TLS callback replaced the game's: the callback comes back and the entry point is recovered.
    fx::Spec spec;
    spec.tls = true;
    spec.tls_replaced = true;
    spec.header30 = 0xD0;
    auto bytes = fx::Build(spec);
    std::string why;
    CHECK(UnpackSteamStub(&bytes, &why));
    CHECK(bytes.substr(fx::kCodeRaw, fx::kCodeSize) == fx::PlainCode());
    CHECK(fx::Get32(bytes, fx::kOptional + 16) == fx::kOriginalEntry);
    const std::size_t callbacks_file = fx::kRelocRaw + (fx::kTlsCallbacks - 0x2000);
    CHECK(fx::Get32(bytes, callbacks_file) == 0x40000000u + fx::kOriginalCallback && fx::Get32(bytes, callbacks_file + 4) == 1);
    CHECK(Sections(bytes) == 2);
  }
  for (const std::size_t size : {std::size_t{856}, std::size_t{884}, std::size_t{952}})  // Variant 2.0, code XOR-chained or plain.
    for (const std::uint32_t flags : {0x04u, 0u}) {
      auto bytes = fx::BuildOld(false, flags, size);
      const auto root = base / ("v20-" + std::to_string(size) + "-" + std::to_string(flags));
      Write(root / "Game.exe", bytes);
      const auto info = InspectSteamStub((root / "Game.exe").string());
      CHECK(info.kind == SteamStubKind::kSupported && info.detail == "2.0 x86" && info.app_id == 2000);
      std::string why;
      CHECK(UnpackSteamStub(&bytes, &why));
      CHECK(bytes.substr(fx::kCodeRaw, fx::kCodeSize) == fx::PlainCode());
      CHECK(fx::Get32(bytes, fx::kOptional + 16) == fx::kOriginalEntry && Sections(bytes) == 2);
    }
  for (const std::size_t size : {std::size_t{0x6A * 4}, std::size_t{0x6B * 4}, std::size_t{0x6D * 4}}) {  // Variant 1.0: only the entry point comes back.
    auto bytes = fx::BuildOld(true, 0, 0, size);
    const auto root = base / ("v10-" + std::to_string(size));
    Write(root / "Game.exe", bytes);
    const auto info = InspectSteamStub((root / "Game.exe").string());
    CHECK(info.kind == SteamStubKind::kSupported && info.detail == "1.0 x86" && info.app_id == 1500);
    std::string why;
    CHECK(UnpackSteamStub(&bytes, &why));
    CHECK(bytes.substr(fx::kCodeRaw, fx::kCodeSize) == fx::PlainCode());
    CHECK(fx::Get32(bytes, fx::kOptional + 16) == fx::kOriginalEntry && Sections(bytes) == 2);
    // A header of a known size whose stub field doesn't name the entry point (laid out another way) is refused,
    // and the program is left as it was.
    auto other = fx::BuildOld(true, 0, 0, size);
    other[fx::kBindRaw + (size == 0x6A * 4 ? 8 : 12)] ^= 0x04;
    const auto before = other;
    Write(root / "Other.exe", other);
    const auto refused = InspectSteamStub((root / "Other.exe").string());
    CHECK(refused.kind == SteamStubKind::kUnsupported && refused.detail == "1.0 x86 with an entry point it doesn't describe");
    CHECK(!UnpackSteamStub(&other, &why) && other == before);
  }
  for (const bool d0 : {false, true}) {  // Variant 2.1: code decrypted through the SteamDRMP.dll chain, .bind dropped.
    fx::Spec21 spec;
    spec.d0_header = d0;
    auto bytes = fx::Build21(spec);
    const auto root = base / (d0 ? "v21-d0" : "v21");
    Write(root / "bladesoftime.exe", bytes);
    const auto info = InspectSteamStub((root / "bladesoftime.exe").string());
    CHECK(info.kind == SteamStubKind::kSupported && info.app_id == 208670 && info.detail == "2.1 x86");
    std::string why;
    CHECK(UnpackSteamStub(&bytes, &why));
    CHECK(bytes.substr(fx::kCodeRaw, fx::kCodeSize) == fx::PlainCode());
    CHECK(fx::Get32(bytes, fx::kOptional + 16) == fx::kOriginalEntry);
    CHECK(Sections(bytes) == 2 && bytes.size() == fx::kBindRaw);
  }
  {  // 2.1 whose SteamDRMP.dll pushes registers between the field loads and the key (Final Fantasy XIII).
    fx::Spec21 spec;
    spec.pushes_in_tail = true;
    auto bytes = fx::Build21(spec);
    const auto root = base / "v21-ff13";
    Write(root / "ffxiiiimg.exe", bytes);
    const auto info = InspectSteamStub((root / "ffxiiiimg.exe").string());
    CHECK(info.kind == SteamStubKind::kSupported && info.detail == "2.1 x86");
    std::string why;
    CHECK(UnpackSteamStub(&bytes, &why));
    CHECK(bytes.substr(fx::kCodeRaw, fx::kCodeSize) == fx::PlainCode());
    CHECK(fx::Get32(bytes, fx::kOptional + 16) == fx::kOriginalEntry);
  }
  {  // 2.1 without encryption (Call of Juarez Gunslinger): only the entry point comes back.
    fx::Spec21 spec;
    spec.flags = 0x26;
    auto bytes = fx::Build21(spec);
    std::string why;
    CHECK(UnpackSteamStub(&bytes, &why));
    CHECK(bytes.substr(fx::kCodeRaw, fx::kCodeSize) == fx::PlainCode());
    CHECK(fx::Get32(bytes, fx::kOptional + 16) == fx::kOriginalEntry);
  }
  {  // 2.1 with an option Playden doesn't know is refused untouched.
    fx::Spec21 spec;
    spec.flags = 0x100;
    auto bytes = fx::Build21(spec);
    const auto before = bytes;
    std::string why;
    CHECK(!UnpackSteamStub(&bytes, &why) && bytes == before && why == "2.1 x86 with flags 0x100");
  }
  {  // Inspection from the file's headers.
    const auto root = base / "inspect";
    Write(root / "Balls.exe", fx::Build());
    const auto info = InspectSteamStub((root / "Balls.exe").string());
    CHECK(info.kind == SteamStubKind::kSupported && info.app_id == 2062430 && info.detail == "3.1 x64");
    auto plain = fx::Build();
    std::string why;
    CHECK(UnpackSteamStub(&plain, &why));
    Write(root / "Plain.exe", plain);
    CHECK(InspectSteamStub((root / "Plain.exe").string()).kind == SteamStubKind::kNone);
    CHECK(InspectSteamStub((root / "missing.exe").string()).kind == SteamStubKind::kNone);
    fs::create_symlink(root / "Balls.exe", root / "Link.exe");
    CHECK(InspectSteamStub((root / "Link.exe").string()).kind == SteamStubKind::kNone);
  }
  {  // Files: the original is kept, the mode too; a second call changes nothing; an update is kept anew.
    const auto root = base / "files";
    const auto wrapped = fx::Build();
    Write(root / "bin/Balls.exe", wrapped, 0750);
    Write(root / "Other.exe", "MZ not a program");
    auto result = UnpackSteamStubPrograms(root.string(), {"bin/Balls.exe", "Other.exe", "bin/Balls.exe"});
    CHECK(result.ok && result.changed == std::vector<std::string>{"bin/Balls.exe"});
    CHECK(result.found == std::vector<std::string>{"bin/Balls.exe"});
    CHECK(Read(root / "bin/Balls.exe.orig") == wrapped);
    const auto unpacked = Read(root / "bin/Balls.exe");
    CHECK(unpacked.substr(fx::kCodeRaw, fx::kCodeSize) == fx::PlainCode());
    struct stat info {};
    CHECK(::stat((root / "bin/Balls.exe").c_str(), &info) == 0 && (info.st_mode & 07777) == 0750);
    CHECK(!fs::exists(root / "bin/.playden-Balls.exe.tmp"));

    result = UnpackSteamStubPrograms(root.string(), {"bin/Balls.exe"});
    CHECK(result.ok && result.changed.empty() && result.found == std::vector<std::string>{"bin/Balls.exe"});
    CHECK(Read(root / "bin/Balls.exe") == unpacked);

    fx::Spec update;
    update.app_id = 7;
    const auto newer = fx::Build(update);
    Write(root / "bin/Balls.exe", newer);
    result = UnpackSteamStubPrograms(root.string(), {"bin/Balls.exe"});
    CHECK(result.ok && result.changed.size() == 1 && Read(root / "bin/Balls.exe.orig") == newer);
  }
  {  // On disk the program is unpacked in place a piece at a time; with pieces of two blocks (or of
     // three) every variant comes out byte for byte as unpacked in memory.
    const auto root = base / "chunked";
    fx::Spec reloc;
    reloc.reloc_into_bind = true;
    fx::Spec pe32;
    pe32.pe32 = true;
    fx::Spec v30;
    v30.header30 = 0xD0;
    fx::Spec tls;
    tls.tls = true;
    fx::Spec plain_code;
    plain_code.flags = 0x04;
    const std::vector<std::string> wrapped = {fx::Build(), fx::Build(reloc), fx::Build(pe32), fx::Build(v30),
                                              fx::Build(tls), fx::Build(plain_code), fx::Build21(),
                                              fx::BuildOld(false), fx::BuildOld(true)};
    for (const std::size_t chunk : {std::size_t{32}, std::size_t{48}, kSteamStubChunkBytes}) {
      for (std::size_t i = 0; i < wrapped.size(); ++i) {
        auto expected = wrapped[i];
        std::string why;
        CHECK(UnpackSteamStub(&expected, &why));
        const auto name = "Game" + std::to_string(i) + ".exe";
        Write(root / name, wrapped[i]);
        const auto result = UnpackSteamStubPrograms(root.string(), {name}, chunk);
        CHECK(result.ok && result.changed == std::vector<std::string>{name});
        CHECK(Read(root / name) == expected && Read(root / (name + ".orig")) == wrapped[i]);
        CHECK(!fs::exists(root / (".playden-" + name + ".tmp")) && !fs::exists(root / (".playden-" + name + ".orig.tmp")));
        // The next start finds it unpacked and changes nothing, also when it still reads as wrapped (a wrapper
        // entered from a TLS callback keeps its section).
        const auto again = UnpackSteamStubPrograms(root.string(), {name}, chunk);
        CHECK(again.ok && again.changed.empty() && again.found == std::vector<std::string>{name});
        CHECK(Read(root / name) == expected && Read(root / (name + ".orig")) == wrapped[i]);
        CHECK(!fs::exists(root / (".playden-" + name + ".check.tmp")));
        fs::remove(root / name);
        fs::remove(root / (name + ".orig"));
      }
    }
  }
  {  // A game update brings a newer wrapped program of the same size, entered from a TLS callback: it is not the
     // unpacked program, so it is kept as the new .orig and unpacked.
    const auto root = base / "tls-update";
    fx::Spec first, update;
    first.tls = update.tls = true;
    update.app_id = first.app_id + 1;
    const auto old_wrapped = fx::Build(first), new_wrapped = fx::Build(update);
    CHECK(old_wrapped.size() == new_wrapped.size() && old_wrapped != new_wrapped);
    Write(root / "Game.exe", old_wrapped);
    CHECK(UnpackSteamStubPrograms(root.string(), {"Game.exe"}).changed.size() == 1);
    Write(root / "Game.exe", new_wrapped);
    auto expected = new_wrapped;
    std::string why;
    CHECK(UnpackSteamStub(&expected, &why));
    const auto result = UnpackSteamStubPrograms(root.string(), {"Game.exe"});
    CHECK(result.ok && result.changed == std::vector<std::string>{"Game.exe"});
    CHECK(Read(root / "Game.exe") == expected && Read(root / "Game.exe.orig") == new_wrapped);
    CHECK(!fs::exists(root / ".playden-Game.exe.check.tmp"));
  }
  {  // A variant Playden doesn't unpack stops with a sentence and leaves the files alone.
    const auto root = base / "refused";
    fx::Spec spec;
    spec.flags = 0x100;
    const auto wrapped = fx::Build(spec);
    Write(root / "Game.exe", wrapped);
    const auto result = UnpackSteamStubPrograms(root.string(), {"Game.exe"});
    CHECK(!result.ok && result.message.find("Steam DRM") != std::string::npos);
    CHECK(Read(root / "Game.exe") == wrapped && !fs::exists(root / "Game.exe.orig"));
  }

  CompareWithReference();
  fs::remove_all(base);
  std::printf("runtime_steam_stub: ok\n");
  return 0;
}
