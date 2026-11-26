#include "runtime/steam_stub.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <optional>
#include <set>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <mbedtls/aes.h>

namespace playden::runtime {
namespace {

namespace fs = std::filesystem;

// Variant 3.x keeps its header just before the wrapper's start: 0xF0 bytes in 3.1, 0xD0 or 0xB0 in
// 3.0 (same fields; the shorter one ends after the SteamDRMP keys).
constexpr std::size_t kHeaderSize = 0xF0;
constexpr std::uint32_t kSignature = 0xC0DEC0DF;
constexpr std::uint32_t kSignature30 = 0xC0DEC0DE;
// Flags that only switch the wrapper's own checks off (module verification, ownership,
// debugger, error dialog), and 0x4: the code is not encrypted, so only the entry point comes back.
constexpr std::uint32_t kNoEncryption = 0x04;
constexpr std::uint32_t kKnownFlags = 0x02 | kNoEncryption | 0x10 | 0x20 | 0x40;
constexpr std::size_t kAesBlock = 16;

std::uint32_t U32(const unsigned char* bytes) {
  return bytes[0] | (bytes[1] << 8) | (bytes[2] << 16) | (static_cast<std::uint32_t>(bytes[3]) << 24);
}
std::uint64_t U64(const unsigned char* bytes) { return U32(bytes) | static_cast<std::uint64_t>(U32(bytes + 4)) << 32; }
std::uint16_t U16(const unsigned char* bytes) { return static_cast<std::uint16_t>(bytes[0] | (bytes[1] << 8)); }
void Put32(char* bytes, std::uint32_t value) {
  for (int i = 0; i < 4; ++i) bytes[i] = static_cast<char>((value >> (8 * i)) & 0xff);
}
void Put16(char* bytes, std::uint16_t value) {
  bytes[0] = static_cast<char>(value & 0xff);
  bytes[1] = static_cast<char>(value >> 8);
}

using Reader = std::function<bool(std::uint64_t offset, void* buffer, std::size_t size)>;

struct Row {
  std::string name;
  std::size_t at = 0;  // file offset of the section header
  std::uint32_t virtual_size = 0;
  std::uint32_t address = 0;
  std::uint32_t raw_size = 0;
  std::uint32_t raw = 0;
  std::uint32_t span() const { return std::max(virtual_size, raw_size); }
  bool Holds(std::uint32_t rva) const { return rva >= address && rva - address < span(); }
};

struct Layout {
  std::size_t pe = 0;
  std::size_t optional = 0;
  bool pe64 = false;
  std::uint64_t image_base = 0;
  std::uint32_t entry = 0;
  std::uint32_t section_alignment = 0;
  std::vector<std::pair<std::uint32_t, std::uint32_t>> directories;  // rva, size
  std::vector<Row> rows;
};

std::optional<Layout> ReadLayout(const Reader& read) {
  unsigned char dos[64];
  if (!read(0, dos, sizeof(dos)) || dos[0] != 'M' || dos[1] != 'Z') return std::nullopt;
  Layout layout;
  layout.pe = U32(dos + 0x3c);
  if (layout.pe > 64u * 1024u * 1024u) return std::nullopt;
  unsigned char header[24];
  if (!read(layout.pe, header, sizeof(header)) || std::memcmp(header, "PE\0\0", 4) != 0) return std::nullopt;
  const unsigned machine = U16(header + 4);
  const unsigned sections = U16(header + 6);
  const unsigned optional_size = U16(header + 20);
  if (sections == 0 || sections > 96 || optional_size < 96 || optional_size > 1024) return std::nullopt;
  layout.optional = layout.pe + 24;
  std::vector<unsigned char> optional(optional_size);
  if (!read(layout.optional, optional.data(), optional.size())) return std::nullopt;
  const unsigned magic = U16(optional.data());
  if (magic != 0x10b && magic != 0x20b) return std::nullopt;
  layout.pe64 = magic == 0x20b && machine == 0x8664;
  layout.entry = U32(optional.data() + 16);
  layout.image_base = magic == 0x20b ? U64(optional.data() + 24) : U32(optional.data() + 28);
  layout.section_alignment = U32(optional.data() + 32);
  const std::size_t first = magic == 0x20b ? 112 : 96;
  for (std::size_t at = first; at + 8 <= optional.size() && layout.directories.size() < 16; at += 8)
    layout.directories.emplace_back(U32(optional.data() + at), U32(optional.data() + at + 4));
  std::vector<unsigned char> table(sections * 40u);
  const std::size_t table_at = layout.optional + optional_size;
  if (!read(table_at, table.data(), table.size())) return std::nullopt;
  for (unsigned i = 0; i < sections; ++i) {
    const auto* row = table.data() + i * 40;
    Row section;
    section.name.assign(reinterpret_cast<const char*>(row), strnlen(reinterpret_cast<const char*>(row), 8));
    section.at = table_at + i * 40;
    section.virtual_size = U32(row + 8);
    section.address = U32(row + 12);
    section.raw_size = U32(row + 16);
    section.raw = U32(row + 20);
    layout.rows.push_back(section);
  }
  return layout;
}

// What the wrapper's header says, in either variant, and where the pieces it names are.
struct Wrapper {
  Layout layout;
  std::size_t bind = 0;  // index of the wrapper's section
  std::size_t code = 0;  // index of the encrypted code section
  std::uint32_t original_entry = 0;
  bool encrypted = true;
  // 2.0: the code section is XOR-chained instead (from xor_key, xor_size bytes); 0 = not encoded.
  std::uint32_t xor_key = 0, xor_size = 0;
  std::uint32_t code_size = 0;  // bytes encrypted from the code section's start
  unsigned char key[32] = {};
  unsigned char iv[kAesBlock] = {};     // still encrypted (ECB) with the key
  unsigned char first[kAesBlock] = {};  // the code's first encrypted block, kept by the wrapper
  // Bytes to write besides the entry point (3.0 x64 started from a TLS callback: the callback's slot).
  std::vector<std::pair<std::uint64_t, std::uint64_t>> slots;  // file offset, 64-bit value
  bool keep_bind = false;  // something still points into .bind (a TLS callback)
};

std::optional<std::uint64_t> FileOffset(const Layout& layout, std::uint32_t rva, std::uint64_t size) {
  for (const auto& row : layout.rows)
    if (rva >= row.address && rva - row.address < row.raw_size && row.raw_size - (rva - row.address) >= size)
      return static_cast<std::uint64_t>(row.raw) + (rva - row.address);
  return std::nullopt;
}

// --- Variant 2.1 (32-bit only) ------------------------------------------------------------
//
// The wrapper's start function begins with this prologue, right after a 0xC0DEC0DE marker.
// It copies an XOR-chained header from .bind; that header names an XOR-chained payload, and
// the payload names an XTEA-encrypted SteamDRMP.dll. The DLL's own code is what knows where
// in the payload the entry point, code section and AES key sit, so the unpacker reads those
// offsets from its instructions, as Steamless does.
constexpr std::uint32_t kMarker21 = 0xC0DEC0DE;
constexpr unsigned char kPrologue21[] = {0x53, 0x51, 0x52, 0x56, 0x57, 0x55, 0x8B, 0xEC,
                                         0x81, 0xEC, 0x00, 0x10, 0x00, 0x00, 0xC7};
constexpr std::uint32_t kNoEncryption21 = 0x04;
constexpr std::uint32_t kKnownFlags21 = 0x02 | kNoEncryption21 | 0x10 | 0x20;

// Length of a ModRM operand (with SIB and displacement); 0 past the end.
std::size_t ModRmLength(const unsigned char* at, std::size_t left) {
  if (left < 1) return 0;
  const unsigned mod = at[0] >> 6, rm = at[0] & 7;
  std::size_t length = 1;
  if (mod != 3 && rm == 4) {
    if (left < 2) return 0;
    ++length;
    if (mod == 0 && (at[1] & 7) == 5) length += 4;
  }
  if (mod == 0 && rm == 5) length += 4;
  if (mod == 1) length += 1;
  if (mod == 2) length += 4;
  return length <= left ? length : 0;
}

struct Header21Site {
  std::uint32_t address = 0;  // full address, as the instruction holds it
  std::uint32_t size = 0;     // bytes
  std::uint32_t key = 0;
};

// Steamless reads the start function: the first "mov dword [m], imm" stores the header's
// address, the second its XOR key, and a "mov reg, imm" before them its size in dwords. Only
// the few instructions the stub uses are decoded; anything else ends the search.
std::optional<Header21Site> FindHeader21(const unsigned char* code, std::size_t size) {
  Header21Site site;
  for (std::size_t at = 0; at < size;) {
    const unsigned char* op = code + at;
    const std::size_t left = size - at;
    std::size_t length = 0;
    if ((op[0] >= 0x50 && op[0] <= 0x5F) || op[0] == 0x90 || op[0] == 0xFC || op[0] == 0xA5 || op[0] == 0xAB) {
      length = 1;
    } else if (op[0] == 0xF3 && left >= 2 && (op[1] == 0xA4 || op[1] == 0xA5 || op[1] == 0xAB)) {
      length = 2;
    } else if (op[0] >= 0xB8 && op[0] <= 0xBF && left >= 5) {
      site.size = U32(op + 1) * 4;
      length = 5;
    } else if (op[0] == 0xC7 && left >= 2 && ((op[1] >> 3) & 7) == 0) {
      const auto operand = ModRmLength(op + 1, left - 1);
      if (!operand || left < 1 + operand + 4) return std::nullopt;
      const std::uint32_t value = U32(op + 1 + operand);
      if ((op[1] >> 6) == 3) site.size = value * 4;
      else if (!site.address) site.address = value;
      else site.key = value;
      length = 1 + operand + 4;
    } else if (op[0] == 0x81 || op[0] == 0x83 || op[0] == 0xC1) {
      const auto operand = ModRmLength(op + 1, left - 1);
      if (!operand) return std::nullopt;
      length = 1 + operand + (op[0] == 0x81 ? 4 : 1);
    } else if (std::memchr("\x01\x03\x09\x0B\x21\x23\x29\x2B\x31\x33\x39\x3B\x85\x89\x8B\x8D", op[0], 16)) {
      const auto operand = ModRmLength(op + 1, left - 1);
      if (!operand) return std::nullopt;
      length = 1 + operand;
    } else {
      return std::nullopt;
    }
    if (left < length) return std::nullopt;
    at += length;
    if (site.address && site.size && site.key) return site;
  }
  return std::nullopt;
}

// Each dword XORed with the stored dword before it, starting from key; returns the next key.
std::uint32_t SteamXor(unsigned char* data, std::size_t size, std::uint32_t key) {
  for (std::size_t at = 0; at + 4 <= size; at += 4) {
    const std::uint32_t stored = U32(data + at);
    const std::uint32_t value = stored ^ key;
    for (int i = 0; i < 4; ++i) data[at + i] = static_cast<unsigned char>((value >> (8 * i)) & 0xff);
    key = stored;
  }
  return key;
}

// SteamDRMP.dll: XTEA (32 rounds) chained with an XOR of the previous stored block.
void DecryptDrmp(unsigned char* data, std::size_t size, const std::vector<std::uint32_t>& keys) {
  std::uint32_t chain1 = 0x55555555, chain2 = 0x55555555;
  for (std::size_t at = 0; at + 8 <= size; at += 8) {
    const std::uint32_t d1 = U32(data + at), d2 = U32(data + at + 4);
    std::uint32_t v1 = d1, v2 = d2, sum = 0x9E3779B9u * 32;
    for (int round = 0; round < 32; ++round) {
      v2 -= (((v1 << 4) ^ (v1 >> 5)) + v1) ^ (sum + keys[(sum >> 11) & 3]);
      sum -= 0x9E3779B9u;
      v1 -= (((v2 << 4) ^ (v2 >> 5)) + v2) ^ (sum + keys[sum & 3]);
    }
    v1 ^= chain1;
    v2 ^= chain2;
    for (int i = 0; i < 4; ++i) {
      data[at + i] = static_cast<unsigned char>((v1 >> (8 * i)) & 0xff);
      data[at + 4 + i] = static_cast<unsigned char>((v2 >> (8 * i)) & 0xff);
    }
    chain1 = d1;
    chain2 = d2;
  }
}

// Pattern of hex bytes with "??" wildcards, like Steamless's.
std::optional<std::size_t> FindPattern(const std::vector<unsigned char>& data, const char* pattern) {
  std::vector<int> bytes;
  for (const char* at = pattern; *at;) {
    if (*at == ' ') { ++at; continue; }
    if (at[0] == '?') bytes.push_back(-1);
    else bytes.push_back(static_cast<int>(std::strtoul(std::string(at, 2).c_str(), nullptr, 16)));
    at += 2;
  }
  if (bytes.empty() || data.size() < bytes.size()) return std::nullopt;
  for (std::size_t at = 0; at + bytes.size() <= data.size(); ++at) {
    std::size_t i = 0;
    while (i < bytes.size() && (bytes[i] < 0 || data[at + i] == bytes[i])) ++i;
    if (i == bytes.size()) return at;
  }
  return std::nullopt;
}

// After the fifth field load, SteamDRMP.dll stores it, then takes the key with "lea reg,
// [payload+key]" and the IV with "add eax, iv". Some builds (Final Fantasy XIII) push registers
// and load a global in between, so the two are found by stepping over what sits there rather
// than at fixed positions. False when the tail holds something this does not step over.
bool TailFields21(const unsigned char* tail, std::size_t size, std::int32_t* key_at, std::int32_t* iv_at) {
  bool key = false, iv = false;
  for (std::size_t at = 0; at + 6 <= size && !(key && iv);) {
    const unsigned char op = tail[at];
    if (op >= 0x50 && op <= 0x5F) { at += 1; continue; }                 // push/pop reg
    if (op == 0x31 || op == 0x33 || op == 0x8A) { at += 2; continue; }   // xor reg, reg; mov r8, r/m8 (register form)
    if (op == 0xA3 || op == 0xA1) { at += 5; continue; }                 // mov [abs], eax; mov eax, [abs]
    if (op == 0x05) {                                                    // add eax, imm32
      if (key && !iv) { *iv_at = static_cast<std::int32_t>(U32(tail + at + 1)); iv = true; }
      at += 5;
      continue;
    }
    if (op == 0x89 || op == 0x8B || op == 0x8D) {
      const unsigned char modrm = tail[at + 1];
      const bool disp32 = (modrm & 0xC0) == 0x80, absolute = (modrm & 0xC7) == 0x05;
      if (op == 0x8D && !key && disp32) { *key_at = static_cast<std::int32_t>(U32(tail + at + 2)); key = true; }
      at += disp32 || absolute ? 6 : (modrm & 0xC0) == 0x40 ? 3 : 2;
      continue;
    }
    break;
  }
  return key && iv;
}

SteamStubInfo Inspect21(const Reader& read, const Layout& layout, const Row& bind, Wrapper* out) {
  SteamStubInfo info;
  info.kind = SteamStubKind::kUnsupported;
  info.detail = "2.1 x86";
  const auto refuse = [&](const char* why) {
    info.detail = std::string("2.1 x86 ") + why;
    return info;
  };
  const std::uint32_t into = layout.entry - bind.address;
  std::vector<unsigned char> start(std::min<std::size_t>(4096, bind.raw_size - into));
  if (!read(static_cast<std::uint64_t>(bind.raw) + into, start.data(), start.size())) return refuse("start unreadable");
  const auto site = FindHeader21(start.data(), start.size());
  if (!site || site->address < layout.image_base || site->size < 72 || site->size > 64 * 1024)
    return refuse("header not found");
  const std::uint32_t header_rva = static_cast<std::uint32_t>(site->address - layout.image_base);
  const auto header_at = FileOffset(layout, header_rva, site->size);
  std::vector<unsigned char> header(site->size);
  if (!header_at || !read(*header_at, header.data(), header.size())) return refuse("header unreadable");
  std::uint32_t key = SteamXor(header.data(), header.size(), site->key);
  // A 0xD0-dword header has no LoadLibraryW, so the fields after it sit one dword earlier.
  const std::size_t shift = site->size / 4 == 0xD0 ? 4 : 0;
  const auto field = [&](std::size_t at) { return U32(header.data() + at - shift); };
  info.app_id = field(44);
  const std::uint32_t payload_va = field(36), payload_size = field(40);
  const std::uint32_t drmp_va_at = field(60), drmp_size_at = field(64), xtea_at = field(68);
  if (payload_va < layout.image_base || payload_size == 0 || payload_size > 1024 * 1024) return refuse("payload not found");
  const auto payload_at = FileOffset(layout, static_cast<std::uint32_t>(payload_va - layout.image_base), payload_size);
  std::vector<unsigned char> payload(payload_size);
  if (!payload_at || !read(*payload_at, payload.data(), payload.size())) return refuse("payload unreadable");
  key = SteamXor(payload.data(), payload.size(), key);
  const auto in_payload = [&](std::uint64_t at, std::uint64_t size) { return at + size <= payload.size(); };
  if (!in_payload(drmp_va_at, 4) || !in_payload(drmp_size_at, 4) || xtea_at >= payload.size() ||
      (payload.size() - xtea_at) / 4 < 4)
    return refuse("payload layout unknown");
  const std::uint32_t drmp_va = U32(payload.data() + drmp_va_at), drmp_size = U32(payload.data() + drmp_size_at);
  if (drmp_va < layout.image_base || drmp_size < 1024 || drmp_size > 32u * 1024u * 1024u || drmp_size % 8 != 0)
    return refuse("SteamDRMP.dll not found");
  const auto drmp_at = FileOffset(layout, static_cast<std::uint32_t>(drmp_va - layout.image_base), drmp_size);
  std::vector<unsigned char> drmp(drmp_size);
  if (!drmp_at || !read(*drmp_at, drmp.data(), drmp.size())) return refuse("SteamDRMP.dll unreadable");
  std::vector<std::uint32_t> keys;
  for (std::size_t at = xtea_at; at + 4 <= payload.size(); at += 4) keys.push_back(U32(payload.data() + at));
  DecryptDrmp(drmp.data(), drmp.size(), keys);

  // Where SteamDRMP.dll reads each field from the payload; the fallback pattern stores
  // some of them with shorter instructions, which moves the operands.
  bool fallback = false;
  auto found = FindPattern(drmp, "8B ?? ?? ?? ?? ?? 89 ?? ?? ?? ?? ?? 8B ?? ?? ?? ?? ?? 89 ?? ?? ?? ?? ?? 8B ?? ?? ?? ?? ?? 89 ?? ?? ?? ?? ?? "
                                 "8B ?? ?? ?? ?? ?? 89 ?? ?? ?? ?? ?? 8B ?? ?? ?? ?? ?? 89 ?? ?? ?? ?? ?? 8D ?? ?? ?? ?? ?? 05");
  if (!found)
    found = FindPattern(drmp, "8B ?? ?? ?? ?? ?? 89 ?? ?? ?? ?? ?? 8B ?? ?? ?? ?? ?? 89 ?? ?? ?? ?? ?? 8B ?? ?? ?? ?? ?? 89 ?? ?? ?? ?? ?? "
                              "8B ?? ?? ?? ?? ?? 89 ?? ?? ?? ?? ?? 8B");
  if (!found) {
    found = FindPattern(drmp, "8B ?? ?? ?? ?? ?? 89 ?? ?? ?? ?? ?? 8B ?? ?? ?? ?? ?? A3 ?? ?? ?? ?? 8B ?? ?? ?? ?? ?? A3 ?? ?? ?? ?? "
                              "8B ?? ?? ?? ?? ?? A3 ?? ?? ?? ?? 8B");
    fallback = true;
  }
  if (!found || *found + 1024 > drmp.size()) return refuse("SteamDRMP.dll layout unknown");
  const unsigned char* block = drmp.data() + *found;
  const std::int32_t flags_at = static_cast<std::int32_t>(U32(block + 2));
  const std::int32_t entry_at = static_cast<std::int32_t>(U32(block + (fallback ? 25 : 26)));
  const std::int32_t code_va_at = static_cast<std::int32_t>(U32(block + (fallback ? 36 : 38)));
  const std::int32_t code_size_at = static_cast<std::int32_t>(U32(block + (fallback ? 47 : 50)));
  std::int32_t key_at = static_cast<std::int32_t>(U32(block + (fallback ? 61 : 62)));
  std::int32_t iv_at = static_cast<std::int32_t>(U32(block + (fallback ? 72 : 67)));
  if (!fallback && !TailFields21(block + 54, 64, &key_at, &iv_at)) return refuse("SteamDRMP.dll tail unknown");
  const auto in_range = [&](std::int32_t at, int size) { return at >= 0 && in_payload(static_cast<std::uint64_t>(at), size); };
  if (!in_range(flags_at, 4) || !in_range(entry_at, 4)) return refuse("payload fields out of range");
  const std::uint32_t flags = U32(payload.data() + flags_at);
  if ((flags & ~kKnownFlags21) != 0) {
    char text[48];
    std::snprintf(text, sizeof(text), "with flags 0x%x", flags);
    return refuse(text);
  }
  // Without encryption only the entry point matters; the code fields may not even be there
  // (Call of Juarez Gunslinger's SteamDRMP.dll reads none of them).
  const bool encrypted = (flags & kNoEncryption21) == 0;
  if (encrypted && (!in_range(code_va_at, 4) || !in_range(code_size_at, 4) || !in_range(key_at, 32) || !in_range(iv_at, 32)))
    return refuse("payload fields out of range");
  const std::uint32_t entry_va = U32(payload.data() + entry_at);
  const std::uint32_t code_va = encrypted ? U32(payload.data() + code_va_at) : entry_va;
  const std::uint32_t code_size = encrypted ? U32(payload.data() + code_size_at) : 0;
  const auto holder = [&](std::uint32_t va) -> std::optional<std::size_t> {
    for (std::size_t i = 0; i < layout.rows.size(); ++i)
      if (&layout.rows[i] != &bind && va >= layout.image_base && layout.rows[i].Holds(static_cast<std::uint32_t>(va - layout.image_base)))
        return i;
    return std::nullopt;
  };
  const auto code = holder(code_va);
  if (!code || !holder(entry_va) || layout.rows[*code].raw_size == 0 ||
      (encrypted && (code_size == 0 || code_size % kAesBlock != 0 || code_size > layout.rows[*code].raw_size)))
    return refuse("with a code section it doesn't describe");
  info.kind = SteamStubKind::kSupported;
  info.detail = "2.1 x86";
  if (out) {
    out->layout = layout;
    out->bind = static_cast<std::size_t>(&bind - layout.rows.data());
    out->code = *code;
    out->original_entry = static_cast<std::uint32_t>(entry_va - layout.image_base);
    out->encrypted = encrypted;
    out->code_size = code_size;
    if (encrypted) {
      std::memcpy(out->key, payload.data() + key_at, 32);
      std::memcpy(out->iv, payload.data() + iv_at, kAesBlock);
      std::memcpy(out->first, payload.data() + iv_at + kAesBlock, kAesBlock);
    }
  }
  return info;
}

// --- Variant 2.0 (32-bit only) ------------------------------------------------------------
//
// Like 2.1, the start function names its header with "mov esi, header" and "mov ecx, dwords"; the
// header is XOR-chained from its first dword and holds the entry point and, when flag 0x4 is set,
// the key that XOR-chains the code section. Steamless finds the code section through BaseOfCode.
constexpr unsigned char kPrologue20[] = {0x53, 0x51, 0x52, 0x56, 0x57, 0x55, 0x8B, 0xEC,
                                         0x81, 0xEC, 0x00, 0x10, 0x00, 0x00, 0xBE};
constexpr std::uint32_t kEncodedCode20 = 0x04;

SteamStubInfo Inspect20(const Reader& read, const Layout& layout, const Row& bind, Wrapper* out) {
  SteamStubInfo info;
  info.kind = SteamStubKind::kUnsupported;
  const auto refuse = [&](const char* why) {
    info.detail = std::string("2.0 x86 ") + why;
    return info;
  };
  const std::uint32_t into = layout.entry - bind.address;
  unsigned char start[64];
  const std::size_t length = std::min<std::size_t>(sizeof(start), bind.raw_size - into);
  if (!read(static_cast<std::uint64_t>(bind.raw) + into, start, length)) return refuse("start unreadable");
  // mov esi, header (right after the prologue), then the next mov reg, imm32 is the size in dwords.
  const std::uint32_t header_va = U32(start + sizeof(kPrologue20));
  std::uint32_t dwords = 0;
  for (std::size_t at = sizeof(kPrologue20) + 4; at + 5 <= length && !dwords; ++at)
    if (start[at] >= 0xB8 && start[at] <= 0xBF) dwords = U32(start + at + 1);
  const std::uint32_t size = dwords * 4;
  if (header_va < layout.image_base || (size != 856 && size != 884 && size != 952)) return refuse("header not found");
  const auto header_at = FileOffset(layout, static_cast<std::uint32_t>(header_va - layout.image_base), size);
  std::vector<unsigned char> header(size);
  if (!header_at || !read(*header_at, header.data(), size)) return refuse("header unreadable");
  SteamXor(header.data() + 4, size - 4, U32(header.data()));
  // The 856-byte header has one import fewer before the flags.
  const std::size_t shift = size == 856 ? 0 : 4;
  const std::uint32_t flags = U32(header.data() + 20 + shift);
  const std::uint32_t entry_va = U32(header.data() + 40 + shift);
  const std::uint32_t code_size = U32(header.data() + 48 + shift);
  const std::uint32_t code_key = U32(header.data() + 52 + shift);
  info.app_id = U32(header.data() + 56 + shift);
  if (entry_va < layout.image_base) return refuse("with an entry point it doesn't describe");
  std::optional<std::size_t> code;
  const std::uint32_t base_of_code = [&] {
    unsigned char value[4];
    return read(layout.optional + 20, value, 4) ? U32(value) : 0u;
  }();
  for (std::size_t i = 0; i < layout.rows.size(); ++i)
    if (&layout.rows[i] != &bind && layout.rows[i].Holds(base_of_code)) code = i;
  const bool encoded = (flags & kEncodedCode20) != 0;
  if (encoded && (!code || code_size > layout.rows[*code].raw_size)) return refuse("with a code section it doesn't describe");
  info.kind = SteamStubKind::kSupported;
  info.detail = "2.0 x86";
  if (out) {
    out->layout = layout;
    out->bind = static_cast<std::size_t>(&bind - layout.rows.data());
    out->code = code.value_or(0);
    out->original_entry = static_cast<std::uint32_t>(entry_va - layout.image_base);
    out->encrypted = false;
    if (encoded) {
      out->xor_key = code_key;
      out->xor_size = code_size;
    }
  }
  return info;
}

// --- Variant 1.0 (32-bit only) ------------------------------------------------------------
//
// pushad; sub esp, 0x1000; mov esi, header; mov ecx, dwords; ... popad; mov eax, entry; jmp eax.
// The header (each byte XORed with its index squared) names the stub, which must be the entry
// point; the code is not encrypted. Steamless only knows the 0x6A-dword header; Return to Castle
// Wolfenstein's is 0x6B dwords, with one more import before the stub (as 2.0's 884 vs 856), and
// DOOM 3's is 0x6D dwords, with the stub and app id where 0x6B has them.
SteamStubInfo Inspect10(const Reader& read, const Layout& layout, const Row& bind, Wrapper* out) {
  SteamStubInfo info;
  std::vector<unsigned char> data(std::min<std::size_t>(bind.raw_size, 4u * 1024u * 1024u));
  if (!read(bind.raw, data.data(), data.size())) return info;
  const auto start = FindPattern(data, "60 81 EC 00 10 00 00 BE ?? ?? ?? ?? B9 ?? 00 00 00");
  if (!start) return info;
  const std::uint32_t size = U32(data.data() + *start + 13) * 4;
  if (size != 0x6A * 4 && size != 0x6B * 4 && size != 0x6D * 4) return info;
  info.kind = SteamStubKind::kUnsupported;
  const std::uint32_t header_va = U32(data.data() + *start + 8);
  if (header_va < layout.image_base) {
    info.detail = "1.0 x86 header not found";
    return info;
  }
  const auto header_at = FileOffset(layout, static_cast<std::uint32_t>(header_va - layout.image_base), size);
  std::vector<unsigned char> header(size);
  if (!header_at || !read(*header_at, header.data(), size)) {
    info.detail = "1.0 x86 header unreadable";
    return info;
  }
  for (std::size_t i = 0; i < size; ++i) header[i] ^= static_cast<unsigned char>(i * i);
  const std::size_t shift = size == 0x6A * 4 ? 0 : 4;
  const std::uint32_t stub = U32(header.data() + 8 + shift);
  info.app_id = U32(header.data() + 20 + shift);
  const auto jump = FindPattern(data, "61 B8 ?? ?? ?? ?? FF E0");
  const std::uint32_t entry_va = jump ? U32(data.data() + *jump + 2) : 0;
  if (stub < layout.image_base || stub - layout.image_base != layout.entry || entry_va < layout.image_base) {
    info.detail = "1.0 x86 with an entry point it doesn't describe";
    return info;
  }
  info.kind = SteamStubKind::kSupported;
  info.detail = "1.0 x86";
  if (out) {
    out->layout = layout;
    out->bind = static_cast<std::size_t>(&bind - layout.rows.data());
    out->original_entry = static_cast<std::uint32_t>(entry_va - layout.image_base);
    out->encrypted = false;
  }
  return info;
}

// --- Variant 3.x --------------------------------------------------------------------------

// The decoded 3.x header just before rva (the wrapper's start), if one is there.
struct Header3 {
  bool v31 = false;
  std::size_t size = 0;
  unsigned char bytes[kHeaderSize] = {};
};
std::optional<Header3> DecodeHeader3(const Reader& read, const Row& bind, std::uint32_t rva) {
  if (!bind.Holds(rva)) return std::nullopt;
  const std::uint32_t into = rva - bind.address;
  for (const std::size_t size : {std::size_t{0xF0}, std::size_t{0xD0}, std::size_t{0xB0}}) {
    if (into < size || into > bind.raw_size) continue;
    unsigned char raw[kHeaderSize];
    if (!read(static_cast<std::uint64_t>(bind.raw) + into - size, raw, size)) continue;
    // Each dword is stored XORed with the stored dword before it; the first is the seed.
    Header3 header;
    header.size = size;
    std::memcpy(header.bytes, raw, 4);
    for (std::size_t at = 4; at < size; at += 4) {
      const std::uint32_t value = U32(raw + at) ^ U32(raw + at - 4);
      for (int i = 0; i < 4; ++i) header.bytes[at + i] = static_cast<unsigned char>((value >> (8 * i)) & 0xff);
    }
    const auto signature = U32(header.bytes + 4);
    if (size == 0xF0 && signature == kSignature) header.v31 = true;
    else if (size == 0xF0 || signature != kSignature30) continue;
    return header;
  }
  return std::nullopt;
}

// The first TLS callback's address (relative) and where its slot is in the file.
struct TlsCallback {
  std::uint32_t rva = 0;
  std::uint64_t slot = 0;
};
std::optional<TlsCallback> FirstTlsCallback(const Reader& read, const Layout& layout) {
  if (layout.directories.size() <= 9 || layout.directories[9].first == 0) return std::nullopt;
  const auto directory = FileOffset(layout, layout.directories[9].first, layout.pe64 ? 32 : 16);
  unsigned char tls[32];
  if (!directory || !read(*directory, tls, layout.pe64 ? 32 : 16)) return std::nullopt;
  const std::uint64_t callbacks = layout.pe64 ? U64(tls + 24) : U32(tls + 12);
  if (callbacks < layout.image_base || callbacks - layout.image_base > 0xffffffffull) return std::nullopt;
  const auto slot = FileOffset(layout, static_cast<std::uint32_t>(callbacks - layout.image_base), layout.pe64 ? 8 : 4);
  unsigned char value[8];
  if (!slot || !read(*slot, value, layout.pe64 ? 8 : 4)) return std::nullopt;
  const std::uint64_t first = layout.pe64 ? U64(value) : U32(value);
  if (first < layout.image_base || first - layout.image_base > 0xffffffffull) return std::nullopt;
  return TlsCallback{static_cast<std::uint32_t>(first - layout.image_base), *slot};
}

SteamStubInfo Inspect3(const Reader& read, std::uint64_t file_size, const Layout& layout, Wrapper* out) {
  SteamStubInfo info;
  const Row* bind = nullptr;
  for (const auto& row : layout.rows)
    if (row.name == ".bind") bind = &row;
  if (!bind) return info;
  // The wrapper starts at the entry point, or (when the game has TLS callbacks) from the first one.
  auto header = bind->Holds(layout.entry) ? DecodeHeader3(read, *bind, layout.entry) : std::nullopt;
  std::optional<TlsCallback> tls;
  if (!header) {
    tls = FirstTlsCallback(read, layout);
    if (tls) header = DecodeHeader3(read, *bind, tls->rva);
    if (!header) {
      if (bind->Holds(layout.entry)) {
        info.kind = SteamStubKind::kUnsupported;
        info.detail = "no 3.x header before the entry point";
      }
      return info;
    }
  }
  info.kind = SteamStubKind::kUnsupported;
  const auto* h = header->bytes;
  std::string arch = std::string(header->v31 ? "3.1" : "3.0") + (layout.pe64 ? " x64" : " x86");
  if (tls) arch += " (TLS)";
  // 3.1: 64-bit fields at 0x20 (entry), 0x48/0x50 (code); 3.0: 32-bit ones, code size from its section.
  info.app_id = U32(h + (header->v31 ? 0x38 : 48));
  const std::uint32_t flags = U32(h + (header->v31 ? 0x3C : 52));
  if ((flags & ~kKnownFlags) != 0) {
    char text[48];
    std::snprintf(text, sizeof(text), "%s with flags 0x%x", arch.c_str(), flags);
    info.detail = text;
    return info;
  }
  const bool encrypted = (flags & kNoEncryption) == 0;
  // Both are relative to the image, in 32-bit programs too (Steamless reads them the same way); a large 32-bit
  // program's addresses can be above its image base, so that is no sign of a full address.
  const std::uint64_t original_entry = header->v31 ? U64(h + 0x20) : U32(h + 28);
  const std::uint64_t code_address = header->v31 ? U64(h + 0x48) : U32(h + 64);
  std::optional<std::size_t> code;
  for (std::size_t i = 0; i < layout.rows.size(); ++i)
    if (&layout.rows[i] != bind && (header->v31 ? layout.rows[i].address == code_address
                                                : code_address <= 0xffffffffull && layout.rows[i].Holds(static_cast<std::uint32_t>(code_address))))
      code = i;
  const Row* row = code ? &layout.rows[*code] : nullptr;
  const std::uint64_t code_size = header->v31 ? U64(h + 0x50) : (row ? row->raw_size : 0);
  bool described = original_entry <= 0xffffffffull;
  if (encrypted)
    described = described && row && code_size != 0 && code_size == row->raw_size && code_size % kAesBlock == 0 &&
                static_cast<std::uint64_t>(row->raw) + row->raw_size <= file_size;
  // 3.0 x64 started from a TLS callback that replaced the game's own: the header says so, and the
  // real entry point is hidden in the stub's entry (Steamless: RebuildTlsCallbackInformation).
  const bool tls_replaced = tls && !header->v31 && layout.pe64 && header->size >= 156 && U32(h + 152) == 1;
  std::uint32_t entry = static_cast<std::uint32_t>(original_entry);
  if (described && tls_replaced) {
    described = false;
    const auto at = FileOffset(layout, layout.entry, 0x100);
    unsigned char start[0x100];
    if (at && read(*at, start, sizeof(start))) {
      // lea... ; sub rdx, imm32 ; mov edx, [rdx] ; xor edx, imm32 (the key, XORed with the header seed)
      static const unsigned char pattern[] = {0x48, 0x81, 0xEA, 0, 0, 0, 0, 0x8B, 0x12, 0x81, 0xF2};
      for (std::size_t i = 0; i + sizeof(pattern) + 4 <= sizeof(start); ++i) {
        bool match = true;
        for (std::size_t j = 0; j < sizeof(pattern) && match; ++j)
          if (!(j >= 3 && j < 7) && start[i + j] != pattern[j]) match = false;
        if (!match) continue;
        entry = layout.entry + (U32(h) ^ U32(start + i + 0x0B));
        described = true;
        break;
      }
    }
  }
  if (!described || (encrypted && !row)) {
    info.detail = arch + " with a code section it doesn't describe";
    return info;
  }
  info.kind = SteamStubKind::kSupported;
  info.detail = arch;
  if (out) {
    Wrapper wrapper;
    wrapper.layout = layout;
    wrapper.bind = static_cast<std::size_t>(bind - layout.rows.data());
    wrapper.code = code.value_or(0);
    wrapper.original_entry = entry;
    wrapper.encrypted = encrypted;
    wrapper.code_size = static_cast<std::uint32_t>(code_size);
    const std::size_t key = header->v31 ? 0x58 : 72, iv = header->v31 ? 0x78 : 104, first = header->v31 ? 0x88 : 120;
    std::memcpy(wrapper.key, h + key, 32);
    std::memcpy(wrapper.iv, h + iv, kAesBlock);
    std::memcpy(wrapper.first, h + first, kAesBlock);
    if (tls_replaced) wrapper.slots.emplace_back(tls->slot, layout.image_base + original_entry);
    // Otherwise a wrapper started from a TLS callback leaves that callback pointing into .bind.
    wrapper.keep_bind = tls && !tls_replaced;
    *out = std::move(wrapper);
  }
  return info;
}

// kNone when the entry point is not in ".bind"; otherwise why it can't be unpacked, or a
// filled wrapper.
SteamStubInfo Inspect(const Reader& read, std::uint64_t file_size, Wrapper* out) {
  SteamStubInfo info;
  const auto layout = ReadLayout(read);
  if (!layout) return info;
  const Row* bind = nullptr;
  for (const auto& row : layout->rows)
    if (row.Holds(layout->entry)) bind = &row;
  // An entry point outside .bind can still be wrapped through a TLS callback (3.x only).
  if (!bind || bind->name != ".bind") return Inspect3(read, file_size, *layout, out);
  const std::uint32_t into = layout->entry - bind->address;
  if (!layout->pe64 && into >= 4 && into < bind->raw_size && bind->raw_size - into >= sizeof(kPrologue21)) {
    unsigned char start[4 + sizeof(kPrologue21)];
    if (read(static_cast<std::uint64_t>(bind->raw) + into - 4, start, sizeof(start)) && U32(start) == kMarker21 &&
        std::memcmp(start + 4, kPrologue21, sizeof(kPrologue21)) == 0)
      return Inspect21(read, *layout, *bind, out);
    if (read(static_cast<std::uint64_t>(bind->raw) + into - 4, start, sizeof(start)) && U32(start) == kMarker21 &&
        std::memcmp(start + 4, kPrologue20, sizeof(kPrologue20)) == 0)
      return Inspect20(read, *layout, *bind, out);
  }
  if (!layout->pe64)
    if (auto v10 = Inspect10(read, *layout, *bind, out); v10.kind != SteamStubKind::kNone) return v10;
  return Inspect3(read, file_size, *layout, out);
}

// Whether ".bind" can go: the last section, nothing after its data in the file, and no
// directory or relocation pointing into it.
// Whether .bind can go; nothing when the relocations could not be read.
std::optional<bool> CanDropBind(const Reader& read, std::uint64_t file_size, const Wrapper& wrapper) {
  const auto& rows = wrapper.layout.rows;
  const Row& bind = rows[wrapper.bind];
  if (wrapper.bind + 1 != rows.size() || wrapper.bind == 0) return false;
  if (static_cast<std::uint64_t>(bind.raw) + bind.raw_size != file_size) return false;
  const auto& directories = wrapper.layout.directories;
  for (std::size_t i = 0; i < directories.size(); ++i) {
    const auto [rva, size] = directories[i];
    if (rva == 0 && size == 0) continue;
    if (i == 4) return false;  // a signature: a file offset, after the sections
    if (bind.Holds(rva)) return false;
  }
  if (directories.size() <= 5 || directories[5].second == 0) return true;
  const auto [relocations, size] = directories[5];
  const Row* holder = nullptr;
  for (const auto& row : rows)
    if (row.Holds(relocations)) holder = &row;
  if (!holder) return false;
  const std::uint64_t start = static_cast<std::uint64_t>(holder->raw) + (relocations - holder->address);
  if (start + size > file_size) return false;
  for (std::uint64_t at = start; at + 8 <= start + size;) {
    unsigned char block_header[8];
    if (!read(at, block_header, sizeof(block_header))) return std::nullopt;
    const std::uint32_t page = U32(block_header);
    const std::uint32_t block = U32(block_header + 4);
    if (block < 8) break;
    if (page + 0x1000u > bind.address && page < bind.address + bind.span()) return false;
    at += block;
  }
  return true;
}

// The program being unpacked: in memory, or a file changed in place a piece at a time, so a
// large program never has to fit in the console's memory (Ghosts 'n Goblins Resurrection's is 100 MB).
struct Image {
  Reader read;
  std::function<bool(std::uint64_t offset, const void* buffer, std::size_t size)> write;
  std::function<bool(std::uint64_t size)> truncate;
  std::uint64_t size = 0;
};

// Restores the program in image as it was built, chunk bytes at a time (a multiple of the AES block).
bool Apply(Image& image, const Wrapper& wrapper, std::size_t chunk, std::string* why) {
  const std::uint64_t code_at = wrapper.layout.rows[wrapper.code].raw;
  std::vector<unsigned char> buffer(chunk), out(chunk);
  if (wrapper.xor_size) {  // 2.0: XOR-chained dwords from the section's start
    std::uint32_t key = wrapper.xor_key;
    const std::size_t total = wrapper.xor_size / 4 * 4;
    for (std::size_t at = 0; at < total; at += chunk) {
      const std::size_t count = std::min(chunk, total - at);
      if (!image.read(code_at + at, buffer.data(), count)) return false;
      for (std::size_t i = 0; i < count; i += 4) {
        const std::uint32_t stored = U32(buffer.data() + i);
        const std::uint32_t value = stored ^ key;
        for (int b = 0; b < 4; ++b) buffer[i + b] = static_cast<unsigned char>((value >> (8 * b)) & 0xff);
        key = stored;
      }
      if (!image.write(code_at + at, buffer.data(), count)) return false;
    }
  }
  if (wrapper.encrypted) {
    unsigned char iv[kAesBlock];
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    bool ok = mbedtls_aes_setkey_dec(&aes, wrapper.key, 256) == 0 &&
              mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_DECRYPT, wrapper.iv, iv) == 0;
    if (!ok) {
      mbedtls_aes_free(&aes);
      if (why) *why = "decryption failed";
      return false;  // mbedtls refuses only a bad key, before any block is written
    }
    // CBC over first ‖ section data, written back one block later than it was read: plain
    // block k (the code at offset 16k) comes from the section's block k-1 XOR block k-2 (first
    // before the section). Going forward, the section's blocks are read before their place is
    // written, and the two before each chunk are carried over; block 0 comes last, from first.
    const std::size_t blocks = wrapper.code_size / kAesBlock;
    const std::size_t per_chunk = chunk / kAesBlock;
    unsigned char last1[kAesBlock], last2[kAesBlock];  // cipher blocks a-1 and a-2
    std::memcpy(last2, wrapper.first, kAesBlock);
    ok = blocks == 0 || image.read(code_at, last1, kAesBlock);
    for (std::size_t a = 1; ok && a < blocks; a += per_chunk) {
      const std::size_t count = std::min(per_chunk, blocks - a);
      ok = image.read(code_at + a * kAesBlock, buffer.data(), count * kAesBlock);
      const auto cipher = [&](std::size_t j) -> const unsigned char* {  // the section's block j, j >= a-2
        return j + 1 == a ? last1 : j + 2 == a ? last2 : buffer.data() + (j - a) * kAesBlock;
      };
      for (std::size_t k = a; ok && k < a + count; ++k) {
        unsigned char plain[kAesBlock];
        ok = mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_DECRYPT, cipher(k - 1), plain) == 0;
        const unsigned char* previous = k >= 2 ? cipher(k - 2) : wrapper.first;
        for (std::size_t i = 0; i < kAesBlock; ++i) out[(k - a) * kAesBlock + i] = plain[i] ^ previous[i];
      }
      if (!ok) break;
      unsigned char next1[kAesBlock], next2[kAesBlock];
      std::memcpy(next1, cipher(a + count - 1), kAesBlock);
      std::memcpy(next2, cipher(a + count - 2), kAesBlock);
      std::memcpy(last1, next1, kAesBlock);
      std::memcpy(last2, next2, kAesBlock);
      ok = image.write(code_at + a * kAesBlock, out.data(), count * kAesBlock);
    }
    unsigned char plain[kAesBlock];
    ok = ok && mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_DECRYPT, wrapper.first, plain) == 0;
    mbedtls_aes_free(&aes);
    if (!ok) return false;
    for (std::size_t i = 0; i < kAesBlock; ++i) plain[i] ^= iv[i];
    if (blocks && !image.write(code_at, plain, kAesBlock)) return false;
  }

  char field[8];
  const auto put32 = [&](std::uint64_t at, std::uint32_t value) {
    Put32(field, value);
    return image.write(at, field, 4);
  };
  if (!put32(wrapper.layout.optional + 16, wrapper.original_entry)) return false;
  if (!put32(wrapper.layout.optional + 64, 0)) return false;  // the checksum no longer matches; Steamless clears it too
  for (const auto& [at, value] : wrapper.slots) {
    for (int i = 0; i < 8; ++i) field[i] = static_cast<char>((value >> (8 * i)) & 0xff);
    if (!image.write(at, field, 8)) return false;
  }
  const auto drop_bind = wrapper.keep_bind ? std::optional<bool>(false) : CanDropBind(image.read, image.size, wrapper);
  if (!drop_bind) {
    if (why) *why = "the program could not be read";
    return false;
  }
  if (*drop_bind) {
    const auto& rows = wrapper.layout.rows;
    const Row& bind = rows[wrapper.bind];
    const Row& last = rows[wrapper.bind - 1];
    const std::uint32_t alignment = wrapper.layout.section_alignment ? wrapper.layout.section_alignment : 0x1000;
    const std::uint64_t end = static_cast<std::uint64_t>(last.address) + (last.virtual_size ? last.virtual_size : last.raw_size);
    if (!put32(wrapper.layout.optional + 56, static_cast<std::uint32_t>((end + alignment - 1) / alignment * alignment)))
      return false;
    Put16(field, static_cast<std::uint16_t>(rows.size() - 1));
    if (!image.write(wrapper.layout.pe + 6, field, 2)) return false;
    const char zeros[40] = {};
    if (!image.write(bind.at, zeros, sizeof(zeros)) || !image.truncate(bind.raw)) return false;
  }
  return true;
}

// pread/pwrite of a whole range on an open file.
bool ReadAt(int file, std::uint64_t offset, void* buffer, std::size_t size) {
  auto* bytes = static_cast<unsigned char*>(buffer);
  std::size_t done = 0;
  while (done < size) {
    const auto count = ::pread(file, bytes + done, size - done, static_cast<off_t>(offset + done));
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) return false;
    done += static_cast<std::size_t>(count);
  }
  return true;
}
bool WriteAt(int file, std::uint64_t offset, const void* buffer, std::size_t size) {
  const auto* bytes = static_cast<const unsigned char*>(buffer);
  std::size_t done = 0;
  while (done < size) {
    const auto count = ::pwrite(file, bytes + done, size - done, static_cast<off_t>(offset + done));
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) return false;
    done += static_cast<std::size_t>(count);
  }
  return true;
}

// A regular file, not through a link, with its size and permission bits; -1 when there is none.
int OpenRegular(const fs::path& path, std::uint64_t* size, mode_t* mode = nullptr) {
  const int file = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (file < 0) return -1;
  struct stat info {};
  if (::fstat(file, &info) != 0 || !S_ISREG(info.st_mode)) {
    ::close(file);
    return -1;
  }
  *size = static_cast<std::uint64_t>(info.st_size);
  if (mode) *mode = info.st_mode & 07777;
  return file;
}

std::optional<std::uint64_t> RegularSize(const fs::path& path) {
  struct stat info {};
  if (::lstat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) return std::nullopt;
  return static_cast<std::uint64_t>(info.st_size);
}

// Whether two regular files hold the same bytes, compared a chunk at a time.
bool SameFile(const fs::path& a, const fs::path& b, std::size_t chunk) {
  std::uint64_t size_a = 0, size_b = 0;
  const int file_a = OpenRegular(a, &size_a);
  const int file_b = file_a < 0 ? -1 : OpenRegular(b, &size_b);
  bool same = file_a >= 0 && file_b >= 0 && size_a == size_b;
  std::vector<unsigned char> left(chunk), right(chunk);
  for (std::uint64_t at = 0; same && at < size_a; at += chunk) {
    const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(chunk, size_a - at));
    same = ReadAt(file_a, at, left.data(), count) && ReadAt(file_b, at, right.data(), count) &&
           std::memcmp(left.data(), right.data(), count) == 0;
  }
  if (file_a >= 0) ::close(file_a);
  if (file_b >= 0) ::close(file_b);
  return same;
}

// Copies source into a new temporary file in target's folder, with mode; the open file (read and
// write) or -1. The caller syncs it and renames it over target.
int CopyToTemporary(int source, std::uint64_t size, const fs::path& temporary, mode_t mode, std::size_t chunk) {
  ::unlink(temporary.c_str());
  const int file = ::open(temporary.c_str(), O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, mode);
  if (file < 0) return -1;
  std::vector<unsigned char> buffer(chunk);
  bool ok = true;
  for (std::uint64_t at = 0; ok && at < size; at += chunk) {
    const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(chunk, size - at));
    ok = ReadAt(source, at, buffer.data(), count) && WriteAt(file, at, buffer.data(), count);
  }
  if (!ok || ::fchmod(file, mode) != 0) {
    ::close(file);
    ::unlink(temporary.c_str());
    return -1;
  }
  return file;
}

// Syncs and closes the temporary file, then renames it over path and syncs the folder.
bool Commit(int file, const fs::path& temporary, const fs::path& path) {
  bool ok = ::fsync(file) == 0;
  ok = ::close(file) == 0 && ok;
  if (!ok || ::rename(temporary.c_str(), path.c_str()) != 0) {
    ::unlink(temporary.c_str());
    return false;
  }
  const int folder = ::open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (folder >= 0) {
    ::fsync(folder);
    ::close(folder);
  }
  return true;
}

// Unpacks the program open as source into a new temporary file with mode; the open file (read and write) or -1, with
// the temporary file removed. The caller commits or removes it.
int UnpackToTemporary(int source, std::uint64_t size, const fs::path& temporary, mode_t mode, std::size_t chunk) {
  const int file = CopyToTemporary(source, size, temporary, mode, chunk);
  if (file < 0) return -1;
  Image image;
  image.read = [&](std::uint64_t offset, void* buffer, std::size_t count) { return ReadAt(file, offset, buffer, count); };
  image.write = [&](std::uint64_t offset, const void* buffer, std::size_t count) { return WriteAt(file, offset, buffer, count); };
  image.truncate = [&](std::uint64_t length) { return ::ftruncate(file, static_cast<off_t>(length)) == 0; };
  image.size = size;
  Wrapper wrapper;
  std::string why;
  if (Inspect(image.read, image.size, &wrapper).kind == SteamStubKind::kSupported && Apply(image, wrapper, chunk, &why))
    return file;
  ::close(file);
  ::unlink(temporary.c_str());
  return -1;
}

// Whether program is what unpacking original gives. A wrapper entered from a TLS callback keeps its section and the
// callback once unpacked, so the program still reads as wrapped; a newer wrapped program from a game update is not
// this. The check is left in a temporary file next to program and removed.
bool UnpackedFrom(const fs::path& original, const fs::path& program, std::size_t chunk) {
  std::uint64_t size = 0;
  mode_t mode = 0600;
  const int source = OpenRegular(original, &size, &mode);
  if (source < 0) return false;
  const auto temporary = program.parent_path() / (".playden-" + program.filename().string() + ".check.tmp");
  const int file = UnpackToTemporary(source, size, temporary, mode, chunk);
  ::close(source);
  if (file < 0) return false;
  ::close(file);
  const bool same = SameFile(temporary, program, chunk);
  ::unlink(temporary.c_str());
  return same;
}

}  // namespace

SteamStubInfo InspectSteamStub(const std::string& path) {
  const int file = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (file < 0) return {};
  struct stat info {};
  SteamStubInfo result;
  if (::fstat(file, &info) == 0 && S_ISREG(info.st_mode)) {
    const Reader read = [&](std::uint64_t offset, void* buffer, std::size_t size) {
      auto* bytes = static_cast<unsigned char*>(buffer);
      std::size_t done = 0;
      while (done < size) {
        const auto count = ::pread(file, bytes + done, size - done, static_cast<off_t>(offset + done));
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        done += static_cast<std::size_t>(count);
      }
      return true;
    };
    result = Inspect(read, static_cast<std::uint64_t>(info.st_size), nullptr);
  }
  ::close(file);
  return result;
}

bool UnpackSteamStub(std::string* program, std::string* why) {
  Image image;
  image.read = [&](std::uint64_t offset, void* buffer, std::size_t size) {
    if (offset > program->size() || program->size() - offset < size) return false;
    std::memcpy(buffer, program->data() + offset, size);
    return true;
  };
  image.write = [&](std::uint64_t offset, const void* buffer, std::size_t size) {
    if (offset > program->size() || program->size() - offset < size) return false;
    std::memcpy(program->data() + offset, buffer, size);
    return true;
  };
  image.truncate = [&](std::uint64_t size) {
    program->resize(static_cast<std::size_t>(size));
    return true;
  };
  image.size = program->size();
  Wrapper wrapper;
  const auto info = Inspect(image.read, image.size, &wrapper);
  if (info.kind != SteamStubKind::kSupported) {
    if (why) *why = info.kind == SteamStubKind::kNone ? "not wrapped" : info.detail;
    return false;
  }
  return Apply(image, wrapper, kSteamStubChunkBytes, why);
}

SteamStubResult UnpackSteamStubPrograms(const std::string& install_root, const std::vector<std::string>& programs,
                                        std::size_t chunk_bytes) {
  SteamStubResult result;
  const auto fail = [&](std::string why) {
    result.message = std::move(why);
    return result;
  };
  const std::size_t chunk = std::max<std::size_t>(chunk_bytes / kAesBlock, 2) * kAesBlock;
  const fs::path root(install_root);
  for (const auto& relative : std::set<std::string>(programs.begin(), programs.end())) {
    const auto path = root / relative;
    const auto orig = fs::path(path.string() + ".orig");
    const auto info = InspectSteamStub(path.string());
    if (info.kind == SteamStubKind::kNone) {
      if (RegularSize(orig)) result.found.push_back(relative);  // unpacked earlier
      continue;
    }
    result.found.push_back(relative);
    if (info.kind == SteamStubKind::kUnsupported)
      return fail("This game's program has Steam DRM in a form Playden can't unpack yet.");
    std::uint64_t size = 0;
    mode_t mode = 0755;
    const int source = OpenRegular(path, &size, &mode);
    if (source < 0) return fail("Playden could not read the game's " + relative + ".");
    const auto kept_size = RegularSize(orig);
    if (kept_size && *kept_size == size && !SameFile(orig, path, chunk) && UnpackedFrom(orig, path, chunk)) {
      ::close(source);  // unpacked earlier, though it still reads as wrapped
      continue;
    }
    // A first unpack, or a newer wrapped program from a game update: keep it first.
    if (kept_size != size || !SameFile(orig, path, chunk)) {
      const auto kept = path.parent_path() / (".playden-" + orig.filename().string() + ".tmp");
      const int copy = CopyToTemporary(source, size, kept, mode, chunk);
      if (copy < 0 || !Commit(copy, kept, orig)) {
        ::close(source);
        return fail("Playden could not keep a copy of the game's " + relative + ".");
      }
    }
    // The unpacked program is made in a copy next to it, then takes its place.
    const auto temporary = path.parent_path() / (".playden-" + path.filename().string() + ".tmp");
    const int file = UnpackToTemporary(source, size, temporary, mode, chunk);
    ::close(source);
    if (file < 0) return fail("Playden could not unpack the game's " + relative + ".");
    if (!Commit(file, temporary, path)) return fail("Playden could not write the game's unpacked " + relative + ".");
    result.changed.push_back(relative);
  }
  result.ok = true;
  return result;
}

}  // namespace playden::runtime
