#include "steam/backend/depot_manifest.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace playden::steam;

#define CHECK(cond)                                                                  \
  do {                                                                               \
    if (!(cond)) {                                                                   \
      std::fprintf(stderr, "CHECK failed %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
      std::exit(1);                                                                  \
    }                                                                                \
  } while (0)

namespace {

using Bytes = std::vector<std::uint8_t>;
std::string g_root;
std::map<Bytes, std::string> g_names;  // ciphertext -> plaintext (from encrypted_names.tsv)

Bytes Fixture(const std::string& name) {
  std::ifstream in(g_root + "/" + name, std::ios::binary);
  CHECK(in.good());
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// Stored-only inflate stub: deflated entries are rejected.
bool InflateStub(const std::uint8_t*, std::size_t, std::size_t, Bytes*) { return false; }

int B64(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  return c == '+' ? 62 : c == '/' ? 63 : -1;
}

Bytes DecodeB64(const std::string& s) {
  Bytes out;
  std::uint32_t acc = 0;
  int bits = 0;
  for (char c : s) {
    if (c == '=') break;
    const int v = B64(c);
    CHECK(v >= 0);
    acc = acc << 6 | static_cast<std::uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<std::uint8_t>(acc >> bits));
    }
  }
  return out;
}

void LoadNames() {
  const Bytes raw = Fixture("encrypted_names.tsv");
  std::istringstream lines(std::string(raw.begin(), raw.end()));
  std::string line;
  while (std::getline(lines, line)) {
    const auto tab = line.find('\t');
    CHECK(tab != std::string::npos);
    g_names[DecodeB64(line.substr(0, tab))] = line.substr(tab + 1);
  }
  CHECK(g_names.size() == 3);
}

// Test double for Steam's symmetric decrypt: ciphertext lookup, plus a trailing NUL like real
// padded names can carry. Also checks the key is passed through.
bool LookupDecrypt(const std::uint8_t* data, std::size_t size, const std::array<std::uint8_t, 32>& key,
                   Bytes* out) {
  if (key[0] != 0x10 || key[31] != 0x2F) return false;
  const auto it = g_names.find(Bytes(data, data + size));
  if (it == g_names.end()) return false;
  out->assign(it->second.begin(), it->second.end());
  out->push_back(0);
  return true;
}

std::array<std::uint8_t, 32> TestKey() {
  std::array<std::uint8_t, 32> key{};
  for (std::size_t i = 0; i < key.size(); ++i) key[i] = static_cast<std::uint8_t>(0x10 + i);
  return key;
}

void CheckPlain(const DepotManifest& m) {
  CHECK(m.depot_id == 4242001);
  CHECK(m.gid == 7234567890123456789ull);
  CHECK(m.original_size == 3 * 1024 * 1024 + 123 + 1000);
  CHECK(m.files.size() == 3);
  CHECK(m.files[0].path == "bin/game.exe");
  CHECK(m.files[0].size == 3 * 1024 * 1024 + 123);
  CHECK(m.files[0].flags == 32 && !m.files[0].is_directory() && !m.files[0].is_symlink());
  CHECK(m.files[0].chunks.size() == 4);
  CHECK(m.files[0].chunks[1].offset == 1048576 && m.files[0].chunks[3].offset == 3 * 1048576);
  CHECK(m.files[0].chunks[3].original_size == 123);
  CHECK(m.files[0].chunks[0].compressed_size == 1048576 / 2 + 16);
  CHECK(m.files[0].chunks[0].crc != 0);
  CHECK(m.files[0].sha_content[0] != 0 || m.files[0].sha_content[1] != 0);
  CHECK(m.files[1].path == "data/empty.dat" && m.files[1].size == 0 && m.files[1].chunks.empty());
  CHECK(m.files[2].path == "readme.txt" && m.files[2].chunks.size() == 1);
}

void ParsePlain() {
  DepotManifest m;
  CHECK(ParseDepotManifest(Fixture("manifest_plain.bin"), &m));
  CHECK(!m.filenames_encrypted);
  CheckPlain(m);
  std::string reason;
  CHECK(ValidateDepotManifest(m, &reason));
  CHECK(DecryptManifestFilenames(&m, TestKey(), LookupDecrypt));  // no-op when plain
  CheckPlain(m);
}

void ParseEncrypted() {
  DepotManifest m;
  CHECK(ParseDepotManifest(Fixture("manifest_encrypted.bin"), &m));
  CHECK(m.filenames_encrypted);
  CHECK(m.files[0].path.find("bin") == std::string::npos);
  std::string reason;
  CHECK(!ValidateDepotManifest(m, &reason));  // still encrypted
  CHECK(!DecryptManifestFilenames(&m, TestKey(), nullptr));
  auto wrong = TestKey();
  wrong[0] = 0;
  DepotManifest copy = m;
  CHECK(!DecryptManifestFilenames(&copy, wrong, LookupDecrypt));
  CHECK(DecryptManifestFilenames(&m, TestKey(), LookupDecrypt));
  CHECK(!m.filenames_encrypted);
  CheckPlain(m);
  CHECK(ValidateDepotManifest(m, &reason));
}

void Unzip() {
  const Bytes raw = Fixture("manifest_plain.bin");
  Bytes out;
  CHECK(UnzipFirstEntry(Fixture("manifest_plain_stored.zip"), InflateStub, &out));
  CHECK(out == raw);
  CHECK(UnzipFirstEntry(Fixture("manifest_plain_stored.zip"), nullptr, &out));
  CHECK(!UnzipFirstEntry(Fixture("manifest_plain_deflated.zip"), InflateStub, &out));
  CHECK(!UnzipFirstEntry(Fixture("manifest_plain_deflated.zip"), nullptr, &out));
  // A deflate callback that reproduces the expected output is accepted; a short result is not.
  static const Bytes* expected = nullptr;
  const Bytes expected_bytes = raw;
  expected = &expected_bytes;
  CHECK(UnzipFirstEntry(Fixture("manifest_plain_deflated.zip"),
                        [](const std::uint8_t*, std::size_t, std::size_t, Bytes* o) {
                          *o = *expected;
                          return true;
                        },
                        &out));
  CHECK(out == raw);
  CHECK(!UnzipFirstEntry(Fixture("manifest_plain_deflated.zip"),
                         [](const std::uint8_t*, std::size_t, std::size_t, Bytes* o) {
                           o->assign(3, 0);
                           return true;
                         },
                         &out));
  DepotManifest m;
  CHECK(UnzipFirstEntry(Fixture("manifest_encrypted_stored.zip"), InflateStub, &out));
  CHECK(ParseDepotManifest(out, &m) && m.filenames_encrypted);
  // Truncation, garbage and corrupted headers.
  Bytes zip = Fixture("manifest_plain_stored.zip");
  for (std::size_t cut : {std::size_t{0}, std::size_t{10}, zip.size() / 2, zip.size() - 1}) {
    Bytes part(zip.begin(), zip.begin() + static_cast<std::ptrdiff_t>(cut));
    CHECK(!UnzipFirstEntry(part, InflateStub, &out));
  }
  CHECK(!UnzipFirstEntry(Bytes(100, 0x41), InflateStub, &out));
  Bytes bad = zip;
  bad[0] = 'X';  // local header signature
  CHECK(!UnzipFirstEntry(bad, InflateStub, &out));
  // A large game's manifest inflates past 64 MiB (#451): accepted up to kMaxManifestBytes when the caller
  // asks for it; other zips (Cloud saves) keep the 64 MiB bound.
  {
    const auto stored = [](std::size_t size) {
      Bytes z(30 + size + 46 + 22, 0);
      const auto put16 = [&](std::size_t at, std::uint32_t v) { z[at] = v & 0xff; z[at + 1] = (v >> 8) & 0xff; };
      const auto put32 = [&](std::size_t at, std::uint32_t v) { put16(at, v & 0xffff); put16(at + 2, v >> 16); };
      put32(0, 0x04034B50u);
      put32(18, static_cast<std::uint32_t>(size));
      put32(22, static_cast<std::uint32_t>(size));
      const std::size_t cd = 30 + size;
      put32(cd, 0x02014B50u);
      put32(cd + 20, static_cast<std::uint32_t>(size));
      put32(cd + 24, static_cast<std::uint32_t>(size));
      const std::size_t eocd = cd + 46;
      put32(eocd, 0x06054B50u);
      put16(eocd + 8, 1);
      put16(eocd + 10, 1);
      put32(eocd + 12, 46);
      put32(eocd + 16, static_cast<std::uint32_t>(cd));
      return z;
    };
    Bytes big = stored(kMaxUnzipBytes + 1);
    CHECK(!UnzipFirstEntry(big, InflateStub, &out));
    CHECK(UnzipFirstEntry(big, InflateStub, &out, kMaxManifestBytes) && out.size() == kMaxUnzipBytes + 1);
    out.clear();
    out.shrink_to_fit();
    big = stored(kMaxManifestBytes + 1);
    CHECK(!UnzipFirstEntry(big, InflateStub, &out, kMaxManifestBytes));
  }
}

void RejectBadInput() {
  DepotManifest m;
  const Bytes good = Fixture("manifest_plain.bin");
  CHECK(!ParseDepotManifest({}, &m));
  CHECK(!ParseDepotManifest(Bytes(64, 0xFF), &m));
  CHECK(!ParseDepotManifest(Bytes(5, 0), &m));
  Bytes bad_magic = good;
  bad_magic[0] ^= 0xFF;
  CHECK(!ParseDepotManifest(bad_magic, &m));
  for (std::size_t cut = 0; cut < good.size(); cut += 7) {
    CHECK(!ParseDepotManifest(Bytes(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(cut)), &m));
  }
  // Missing end magic.
  CHECK(!ParseDepotManifest(Bytes(good.begin(), good.end() - 4), &m));
  // Section length beyond the buffer.
  Bytes long_len = good;
  long_len[4] = 0xFF;
  long_len[5] = 0xFF;
  long_len[6] = 0xFF;
  long_len[7] = 0x7F;
  CHECK(!ParseDepotManifest(long_len, &m));
  // Payload only: metadata missing.
  Bytes no_meta(good.begin(), good.begin() + 8 + (good[4] | good[5] << 8 | good[6] << 16));
  no_meta.insert(no_meta.end(), {0xAB, 0x15, 0xC4, 0x32});
  CHECK(!ParseDepotManifest(no_meta, &m));
  CHECK(!ParseDepotManifest(good, nullptr));
}

ManifestFile File(const std::string& path, std::uint64_t size = 10, std::uint32_t flags = 0) {
  ManifestFile f;
  f.path = path;
  f.size = size;
  f.flags = flags;
  if (size != 0 && !(flags & kDepotFileFlagDirectory)) {
    ManifestChunk c;
    c.sha[0] = 1;
    c.original_size = static_cast<std::uint32_t>(size);
    f.chunks.push_back(c);
  }
  return f;
}

bool Valid(std::vector<ManifestFile> files) {
  DepotManifest m;
  m.files = std::move(files);
  std::string reason;
  const bool ok = ValidateDepotManifest(m, &reason);
  CHECK(ok == reason.empty());
  return ok;
}

void Validation() {
  CHECK(Valid({File("a.txt"), File("dir", 0, 64), File("dir/b.txt")}));
  CHECK(Valid({File("A\\B.txt")}));  // backslashes are separators
  CHECK(!Valid({File("a.txt"), File("A.TXT")}));         // case-insensitive duplicate
  CHECK(!Valid({File("d\\x"), File("D/X")}));             // duplicate after slash normalization
  CHECK(!Valid({File("../x")}));
  CHECK(!Valid({File("a/../x")}));
  CHECK(!Valid({File("/abs")}));
  CHECK(!Valid({File("a//b")}));
  CHECK(!Valid({File("")}));
  CHECK(!Valid({File("c:/x")}));
  CHECK(!Valid({File("bad\nname")}));
  CHECK(!Valid({File(".GN-Download/x")}));
  CHECK(!Valid({File("file"), File("file/child")}));      // file under a file
  CHECK(!Valid({File("FILE"), File("file/child/deep")}));
  CHECK(Valid({File("dir", 0, 64), File("DIR/child")}));  // directory parent may differ in case
  CHECK(!Valid({File("dir", 0, 64), File("DIR", 0, 64)}));
  // Chunk bounds.
  ManifestFile past = File("p", 10);
  past.chunks[0].original_size = 11;
  CHECK(!Valid({past}));
  ManifestFile gap = File("g", 10);
  gap.chunks[0].offset = 1;
  CHECK(!Valid({gap}));
  ManifestFile shortfile = File("s", 10);
  shortfile.chunks[0].original_size = 9;
  CHECK(!Valid({shortfile}));
  ManifestFile nochunk = File("n", 10);
  nochunk.chunks.clear();
  CHECK(!Valid({nochunk}));
  ManifestFile zero = File("z", 10);
  zero.chunks[0].original_size = 0;
  CHECK(!Valid({zero}));
  CHECK(!Valid({File("huge", 0x8000000000000000ull)}));
  // Symlinks.
  ManifestFile link = File("d/l", 0);
  link.link_target = "..\\other";
  CHECK(Valid({link}));
  link.link_target = "../../up";
  CHECK(!Valid({link}));
  link.link_target = "/etc/passwd";
  CHECK(!Valid({link}));
  link.link_target = "..";
  CHECK(!Valid({link}));  // resolves to the root itself
  ManifestFile as_dir = File("l", 0);
  as_dir.link_target = "t";
  CHECK(!Valid({as_dir, File("l/x")}));  // writes through a link
}


// Names whose ciphertext is one byte: 1 -> "./data", 2 -> ".\\data\\.\\x.txt", 3 -> "." (root
// directory), 4 -> "./../up".
bool DotDecrypt(const std::uint8_t* data, std::size_t size, const std::array<std::uint8_t, 32>&, Bytes* out) {
  static const char* const kNames[] = {"", "./data", ".\\data\\.\\x.txt", ".", "./../up"};
  if (size != 1 || data[0] < 1 || data[0] > 4) return false;
  const std::string name = kNames[data[0]];
  out->assign(name.begin(), name.end());
  return true;
}

void DotSegments() {
  DepotManifest m;
  m.filenames_encrypted = true;
  m.files = {File("Aw==", 0, kDepotFileFlagDirectory), File("AQ==", 0, kDepotFileFlagDirectory), File("Ag==")};
  CHECK(DecryptManifestFilenames(&m, TestKey(), DotDecrypt));
  CHECK(m.files.size() == 2);  // the "." root entry is dropped
  CHECK(m.files[0].path == "data" && m.files[0].is_directory());
  CHECK(m.files[1].path == "data/x.txt");
  std::string reason;
  CHECK(ValidateDepotManifest(m, &reason));
  DepotManifest up;
  up.filenames_encrypted = true;
  up.files = {File("BA==")};
  CHECK(DecryptManifestFilenames(&up, TestKey(), DotDecrypt));
  CHECK(up.files[0].path == "../up");
  CHECK(!ValidateDepotManifest(up, &reason));  // ".." still escapes
}
}  // namespace

int main(int argc, char** argv) {
  CHECK(argc >= 2);
  g_root = argv[1];
  LoadNames();
  ParsePlain();
  ParseEncrypted();
  Unzip();
  RejectBadInput();
  Validation();
  DotSegments();
  std::puts("steam_depot_manifest_test OK");
  return 0;
}
