#pragma once
// Builders for the Windows Installer and cabinet tests: cabinets (stored or MSZIP), compound files and
// minimal .msi packages, all made in memory. Shared by runtime_redist_test.cpp and the runtime host tests.
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace playden::test::msi {

inline void Put16(std::string* out, unsigned value) {
  out->push_back(static_cast<char>(value & 0xff));
  out->push_back(static_cast<char>((value >> 8) & 0xff));
}

inline void Put32(std::string* out, std::uint32_t value) {
  Put16(out, value & 0xffff);
  Put16(out, value >> 16);
}

// Deflate bits, least significant first; Huffman codes go in most significant bit first.
struct Bits {
  std::string bytes;
  unsigned current = 0, count = 0;
  void Put(unsigned value, unsigned width) {
    for (unsigned bit = 0; bit < width; ++bit) Push((value >> bit) & 1);
  }
  void Code(unsigned value, unsigned width) {
    for (unsigned bit = width; bit-- > 0;) Push((value >> bit) & 1);
  }
  void Push(unsigned bit) {
    current |= bit << count;
    if (++count == 8) Flush();
  }
  void Flush() {
    if (count) bytes.push_back(static_cast<char>(current));
    current = count = 0;
  }
};

// A cabinet with one MSZIP folder of two blocks: a stored block "0123456789", then a fixed
// Huffman block that copies those ten bytes back (length 10, distance 10) — only right when
// the second block sees the first one's output. Members: "first" (block 1), "second" (block 2),
// "empty" (no bytes).
inline std::string TwoBlockCabinet() {
  std::string block1 = "CK";
  {
    Bits bits;
    bits.Put(1, 1);  // final
    bits.Put(0, 2);  // stored
    bits.Flush();
    block1 += bits.bytes;
    Put16(&block1, 10);
    Put16(&block1, 0xffff ^ 10);
    block1 += "0123456789";
  }
  std::string block2 = "CK";
  {
    Bits bits;
    bits.Put(1, 1);           // final
    bits.Put(1, 2);           // fixed Huffman
    bits.Code(264 - 256, 7);  // length 10
    bits.Code(6, 5);          // distance 9-12
    bits.Put(1, 2);           // + 1 = 10
    bits.Code(0, 7);          // end of block
    bits.Flush();
    block2 += bits.bytes;
  }
  struct Member { const char* name; std::uint32_t size, offset; };
  const Member members[] = {{"first", 10, 0}, {"second", 10, 10}, {"empty", 0, 20}};
  std::string files;
  for (const auto& member : members) {
    Put32(&files, member.size);
    Put32(&files, member.offset);
    Put16(&files, 0);  // folder
    Put16(&files, 0);
    Put16(&files, 0);
    Put16(&files, 0x20);
    files += member.name;
    files.push_back('\0');
  }
  const std::uint32_t header = 36, folder = 8;
  const std::uint32_t files_at = header + folder;
  const std::uint32_t data_at = files_at + static_cast<std::uint32_t>(files.size());
  std::string data;
  for (const auto* block : {&block1, &block2}) {
    Put32(&data, 0);
    Put16(&data, static_cast<unsigned>(block->size()));
    Put16(&data, 10);
    data += *block;
  }
  std::string cabinet = "MSCF";
  Put32(&cabinet, 0);
  Put32(&cabinet, data_at + static_cast<std::uint32_t>(data.size()));
  Put32(&cabinet, 0);
  Put32(&cabinet, files_at);
  Put32(&cabinet, 0);
  cabinet.push_back(3);
  cabinet.push_back(1);
  Put16(&cabinet, 1);  // folders
  Put16(&cabinet, 3);  // files
  Put16(&cabinet, 0);  // flags
  Put16(&cabinet, 0);
  Put16(&cabinet, 0);
  Put32(&cabinet, data_at);
  Put16(&cabinet, 2);  // blocks
  Put16(&cabinet, 1);  // MSZIP
  return cabinet + files + data;
}

// ---------------------------------------------------------------------------------------------
// A minimal Windows Installer package: a compound file (512-byte sectors, every stream in
// ordinary sectors) holding the string pool, _Columns and a few tables.

inline std::vector<std::uint16_t> EncodeStreamName(const std::string& name) {
  static const std::string kAlphabet = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz._";
  std::vector<std::uint16_t> out;
  std::size_t at = 0;
  if (!name.empty() && name[0] == '!') {
    out.push_back(0x4840);
    at = 1;
  }
  while (at < name.size()) {
    const auto first = kAlphabet.find(name[at]);
    if (first == std::string::npos) {
      out.push_back(static_cast<unsigned char>(name[at++]));
      continue;
    }
    const auto second = at + 1 < name.size() ? kAlphabet.find(name[at + 1]) : std::string::npos;
    if (second != std::string::npos) {
      out.push_back(static_cast<std::uint16_t>(0x3800 + first + (second << 6)));
      at += 2;
    } else {
      out.push_back(static_cast<std::uint16_t>(0x4800 + first));
      ++at;
    }
  }
  return out;
}

inline std::string CompoundFile(const std::vector<std::pair<std::string, std::string>>& streams) {
  constexpr std::uint32_t kSector = 512, kEnd = 0xfffffffe, kFree = 0xffffffff;
  std::vector<std::uint32_t> stream_sectors;
  std::uint32_t data_sectors = 0;
  for (const auto& stream : streams) {
    const auto count = static_cast<std::uint32_t>((stream.second.size() + kSector - 1) / kSector);
    stream_sectors.push_back(count);
    data_sectors += count;
  }
  const auto directory_sectors = static_cast<std::uint32_t>(((streams.size() + 1) * 128 + kSector - 1) / kSector);
  std::uint32_t fat_sectors = 1;
  while (fat_sectors * (kSector / 4) < fat_sectors + directory_sectors + data_sectors) ++fat_sectors;
  const std::uint32_t total = fat_sectors + directory_sectors + data_sectors;
  std::vector<std::uint32_t> fat(fat_sectors * (kSector / 4), kFree);
  for (std::uint32_t index = 0; index < fat_sectors; ++index) fat[index] = 0xfffffffd;
  const auto chain = [&](std::uint32_t first, std::uint32_t count) {
    for (std::uint32_t index = 0; index < count; ++index) fat[first + index] = index + 1 < count ? first + index + 1 : kEnd;
  };
  chain(fat_sectors, directory_sectors);
  std::string directory;
  const auto entry = [&](const std::vector<std::uint16_t>& name, int type, std::uint32_t start, std::uint32_t size) {
    std::string out;
    for (const auto unit : name) Put16(&out, unit);
    out.resize(64, '\0');
    Put16(&out, static_cast<unsigned>((name.size() + 1) * 2));
    out.push_back(static_cast<char>(type));
    out.push_back(1);
    Put32(&out, 0xffffffff);
    Put32(&out, 0xffffffff);
    Put32(&out, type == 5 ? 1 : 0xffffffff);
    out.resize(0x74, '\0');
    Put32(&out, start);
    Put32(&out, size);
    Put32(&out, 0);
    directory += out;
  };
  entry({'R', 'o', 'o', 't', ' ', 'E', 'n', 't', 'r', 'y'}, 5, kEnd, 0);
  std::uint32_t next = fat_sectors + directory_sectors;
  std::string body;
  for (std::size_t index = 0; index < streams.size(); ++index) {
    const auto& [name, data] = streams[index];
    entry(EncodeStreamName(name), 2, stream_sectors[index] ? next : kEnd, static_cast<std::uint32_t>(data.size()));
    chain(next, stream_sectors[index]);
    next += stream_sectors[index];
    std::string padded = data;
    padded.resize(stream_sectors[index] * kSector, '\0');
    body += padded;
  }
  directory.resize(directory_sectors * kSector, '\0');
  std::string header = "\xd0\xcf\x11\xe0\xa1\xb1\x1a\xe1";
  header.resize(0x18, '\0');
  Put16(&header, 0x3e);
  Put16(&header, 3);
  Put16(&header, 0xfffe);
  Put16(&header, 9);
  Put16(&header, 6);
  header.resize(0x2c, '\0');
  Put32(&header, fat_sectors);
  Put32(&header, fat_sectors);  // first directory sector
  Put32(&header, 0);
  Put32(&header, 0);  // mini stream cutoff: nothing in the mini stream
  Put32(&header, kEnd);
  Put32(&header, 0);
  Put32(&header, kEnd);
  Put32(&header, 0);
  for (std::uint32_t index = 0; index < 109; ++index) Put32(&header, index < fat_sectors ? index : kFree);
  std::string fat_bytes;
  for (const auto value : fat) Put32(&fat_bytes, value);
  (void)total;
  return header + fat_bytes + directory + body;
}

struct Column {
  std::string name;
  char kind;  // 's' string, 'i' 2-byte integer, 'l' 4-byte integer
};

struct TableSpec {
  std::string name;
  std::vector<Column> columns;
  std::vector<std::vector<std::string>> rows;  // "" is null for strings; integers as text, "" null
};

inline std::string Package(const std::vector<TableSpec>& tables, const std::vector<std::pair<std::string, std::string>>& extra) {
  std::vector<std::string> strings{""};
  std::map<std::string, std::uint16_t> ids;
  const auto id = [&](const std::string& text) -> std::uint16_t {
    if (text.empty()) return 0;
    const auto found = ids.find(text);
    if (found != ids.end()) return found->second;
    strings.push_back(text);
    return ids[text] = static_cast<std::uint16_t>(strings.size() - 1);
  };
  std::vector<std::pair<std::string, std::string>> streams;
  std::vector<std::string> column_table, column_name;
  std::vector<unsigned> column_number, column_type;
  for (const auto& table : tables) {
    for (std::size_t index = 0; index < table.columns.size(); ++index) {
      column_table.push_back(table.name);
      column_name.push_back(table.columns[index].name);
      column_number.push_back(static_cast<unsigned>(index + 1));
      const char kind = table.columns[index].kind;
      column_type.push_back(kind == 's' ? 0x1dff : kind == 'i' ? 0x1502 : 0x1104);
    }
    std::string data;
    for (std::size_t column = 0; column < table.columns.size(); ++column) {
      for (const auto& row : table.rows) {
        const auto& cell = row[column];
        const char kind = table.columns[column].kind;
        if (kind == 's') Put16(&data, id(cell));
        else if (kind == 'i') Put16(&data, cell.empty() ? 0 : static_cast<unsigned>(std::atoi(cell.c_str()) + 0x8000));
        else Put32(&data, cell.empty() ? 0 : static_cast<std::uint32_t>(std::atoll(cell.c_str()) + 0x80000000ll));
      }
    }
    streams.emplace_back("!" + table.name, data);
  }
  std::string columns;
  for (const auto& name : column_table) Put16(&columns, id(name));
  for (const auto number : column_number) Put16(&columns, number + 0x8000);
  for (const auto& name : column_name) Put16(&columns, id(name));
  for (const auto type : column_type) Put16(&columns, type | 0x8000);
  streams.emplace_back("!_Columns", columns);
  std::string pool, pool_data;
  Put32(&pool, 1252);
  for (std::size_t index = 1; index < strings.size(); ++index) {
    Put16(&pool, static_cast<unsigned>(strings[index].size()));
    Put16(&pool, 1);
    pool_data += strings[index];
  }
  streams.emplace_back("!_StringPool", pool);
  streams.emplace_back("!_StringData", pool_data);
  for (const auto& stream : extra) streams.push_back(stream);
  return CompoundFile(streams);
}

// A cabinet with one stored (uncompressed) folder holding members in order.
inline std::string StoredCabinet(const std::vector<std::pair<std::string, std::string>>& members) {
  std::string files, content;
  for (const auto& [name, data] : members) {
    Put32(&files, static_cast<std::uint32_t>(data.size()));
    Put32(&files, static_cast<std::uint32_t>(content.size()));
    Put16(&files, 0);  // folder
    Put16(&files, 0);
    Put16(&files, 0);
    Put16(&files, 0x20);
    files += name;
    files.push_back('\0');
    content += data;
  }
  std::vector<std::string> chunks;
  for (std::size_t at = 0; at < content.size(); at += 32768) chunks.push_back(content.substr(at, 32768));
  if (chunks.empty()) chunks.emplace_back();
  std::string data;
  for (const auto& chunk : chunks) {
    Put32(&data, 0);
    Put16(&data, static_cast<unsigned>(chunk.size()));
    Put16(&data, static_cast<unsigned>(chunk.size()));
    data += chunk;
  }
  const auto blocks = static_cast<unsigned>(chunks.size());
  const std::uint32_t files_at = 36 + 8;
  const std::uint32_t data_at = files_at + static_cast<std::uint32_t>(files.size());
  std::string cabinet = "MSCF";
  Put32(&cabinet, 0);
  Put32(&cabinet, data_at + static_cast<std::uint32_t>(data.size()));
  Put32(&cabinet, 0);
  Put32(&cabinet, files_at);
  Put32(&cabinet, 0);
  cabinet.push_back(3);
  cabinet.push_back(1);
  Put16(&cabinet, 1);  // folders
  Put16(&cabinet, static_cast<unsigned>(members.size()));
  Put16(&cabinet, 0);  // flags
  Put16(&cabinet, 0);
  Put16(&cabinet, 0);
  Put32(&cabinet, data_at);
  Put16(&cabinet, blocks);
  Put16(&cabinet, 0);  // stored
  return cabinet + files + data;
}

// The members' CFFILE rows, all in folder 0, and their bytes in order.
inline void LzxMembers(const std::vector<std::pair<std::string, std::string>>& members, std::string* files,
                       std::string* content) {
  for (const auto& [name, data] : members) {
    Put32(files, static_cast<std::uint32_t>(data.size()));
    Put32(files, static_cast<std::uint32_t>(content->size()));
    Put16(files, 0);  // folder
    Put16(files, 0);
    Put16(files, 0);
    Put16(files, 0x20);
    *files += name;
    files->push_back('\0');
    *content += data;
  }
}

// LZX bits, most significant first, packed into 16-bit little-endian words.
struct LzxBits {
  std::vector<bool> bits;
  void Put(std::uint32_t value, unsigned count) {
    for (unsigned bit = count; bit-- > 0;) bits.push_back((value >> bit) & 1);
  }
  void Align() { Put(0, bits.size() % 16 ? 16 - bits.size() % 16 : 0); }
  std::string Words() const {
    std::string stream;
    for (std::size_t at = 0; at < bits.size(); at += 16) {
      unsigned word = 0;
      for (unsigned bit = 0; bit < 16; ++bit) word = word << 1 | (at + bit < bits.size() && bits[at + bit] ? 1 : 0);
      Put16(&stream, word);
    }
    return stream;
  }
};

// A cabinet with one LZX folder (64 KiB window) holding stream, its data blocks given as (packed, unpacked) sizes.
inline std::string LzxFolderCabinet(const std::string& files, std::size_t members, const std::string& stream,
                                    const std::vector<std::pair<std::size_t, std::size_t>>& blocks) {
  std::string data;
  std::size_t taken = 0;
  for (const auto& [packed, frame] : blocks) {
    Put32(&data, 0);
    Put16(&data, static_cast<unsigned>(packed));
    Put16(&data, static_cast<unsigned>(frame));
    data += stream.substr(taken, packed);
    taken += packed;
  }
  const std::uint32_t files_at = 36 + 8;
  const std::uint32_t data_at = files_at + static_cast<std::uint32_t>(files.size());
  std::string cabinet = "MSCF";
  Put32(&cabinet, 0);
  Put32(&cabinet, data_at + static_cast<std::uint32_t>(data.size()));
  Put32(&cabinet, 0);
  Put32(&cabinet, files_at);
  Put32(&cabinet, 0);
  cabinet.push_back(3);
  cabinet.push_back(1);
  Put16(&cabinet, 1);  // folders
  Put16(&cabinet, static_cast<unsigned>(members));
  Put16(&cabinet, 0);  // flags
  Put16(&cabinet, 0);
  Put16(&cabinet, 0);
  Put32(&cabinet, data_at);
  Put16(&cabinet, static_cast<unsigned>(blocks.size()));
  Put16(&cabinet, 3 | 16 << 8);  // LZX, 2^16 window
  return cabinet + files + data;
}

// A cabinet with one LZX folder (64 KiB window) whose stream is a single uncompressed LZX block:
// the members' bytes follow the stream header, the block header, its padding and the repeated
// offsets, split into 32 KiB frames across data blocks. translation: the stream's x86 call
// translation size (0 for none); the bytes are stored as an encoder writes them. frames: other frame
// sizes, with the whole stream in the first data block.
inline std::string LzxCabinet(const std::vector<std::pair<std::string, std::string>>& members, std::uint32_t translation = 0,
                              const std::vector<std::size_t>& frames = {}) {
  std::string files, content;
  LzxMembers(members, &files, &content);
  LzxBits bits;
  bits.Put(translation ? 1 : 0, 1);
  if (translation) {
    bits.Put(translation >> 16, 16);
    bits.Put(translation & 0xffff, 16);
  }
  bits.Put(3, 3);  // uncompressed block
  bits.Put(static_cast<std::uint32_t>(content.size()) >> 8, 16);
  bits.Put(static_cast<std::uint32_t>(content.size()) & 0xff, 8);
  bits.Put(0, bits.bits.size() % 16 ? 16 - bits.bits.size() % 16 : 16);  // 1 to 16 bits up to the next word
  std::string stream = bits.Words();
  for (int repeated = 0; repeated < 3; ++repeated) Put32(&stream, 1);
  const auto header = stream.size();
  stream += content;
  if (content.size() & 1) stream.push_back('\0');
  std::vector<std::pair<std::size_t, std::size_t>> blocks;
  if (!frames.empty()) {
    for (const auto frame : frames) blocks.emplace_back(blocks.empty() ? stream.size() : 0, frame);
  } else {
    for (std::size_t at = 0; at < content.size(); at += 32768) {
      const auto frame = std::min<std::size_t>(32768, content.size() - at);
      blocks.emplace_back((at == 0 ? header : 0) + frame + (at + frame == content.size() ? stream.size() - header - content.size() : 0),
                          frame);
    }
  }
  return LzxFolderCabinet(files, members.size(), stream, blocks);
}

// A cabinet with one LZX folder whose stream is a single verbatim block of literals, in one frame: every byte value
// has an 8-bit code (the code is the byte), with no matches. cut: bytes taken off the stream's end (and its data
// block's packed size). missing_literal: a byte value left out of the main tree, which is then incomplete.
inline std::string LzxVerbatimCabinet(const std::string& name, const std::string& content, std::size_t cut = 0,
                                      int missing_literal = -1) {
  std::string files, all;
  LzxMembers({{name, content}}, &files, &all);
  LzxBits bits;
  bits.Put(0, 1);  // no translation
  bits.Put(1, 3);  // verbatim block
  bits.Put(static_cast<std::uint32_t>(content.size()) >> 8, 16);
  bits.Put(static_cast<std::uint32_t>(content.size()) & 0xff, 8);
  // Each tree's lengths go through a pretree whose codes are 0 (length unchanged from 0) and 1 (pretree symbol 9:
  // (0 - 8) mod 17, so length 8).
  const auto pretree = [&] {
    for (unsigned symbol = 0; symbol < 20; ++symbol) bits.Put(symbol == 0 || symbol == 9 ? 1 : 0, 4);
  };
  pretree();
  for (int literal = 0; literal < 256; ++literal) bits.Put(literal == missing_literal ? 0 : 1, 1);
  pretree();
  for (unsigned symbol = 256; symbol < 256 + 32 * 8; ++symbol) bits.Put(0, 1);  // no matches
  pretree();
  for (unsigned symbol = 0; symbol < 249; ++symbol) bits.Put(0, 1);  // an empty length tree
  for (const auto byte : content) bits.Put(static_cast<unsigned char>(byte), 8);
  bits.Align();
  auto stream = bits.Words();
  stream.resize(stream.size() - cut);
  return LzxFolderCabinet(files, 1, stream, {{stream.size(), content.size()}});
}

// A WiX Burn bundle: a small PE whose .wixburn section lists the UX container (file "0" is the manifest)
// and one attached container. signed_form: the attached container follows an original signature, as in
// Microsoft's bundles; otherwise it follows the UX container. with_section false: no .wixburn section.
inline std::string BurnBundle(const std::string& manifest, const std::string& attached, bool signed_form = true,
                              bool with_section = true) {
  const auto ux = StoredCabinet({{"0", manifest}, {"u0", "ux-dll"}});
  std::string stub(0x400, '\0');
  stub[0] = 'M'; stub[1] = 'Z'; stub[0x3c] = 0x40;
  stub[0x40] = 'P'; stub[0x41] = 'E';
  stub[0x44] = 0x4c; stub[0x45] = 0x01;  // i386
  stub[0x46] = 1;                         // one section
  stub[0x54] = static_cast<char>(0xe0);   // optional header size
  stub[0x58] = 0x0b; stub[0x59] = 0x01;   // PE32
  const std::size_t row = 0x58 + 0xe0;
  stub.replace(row, 8, with_section ? ".wixburn" : ".rsrc\0\0\0", 8);
  std::string field;
  Put32(&field, 0x200); Put32(&field, 0x1000);  // virtual size, address
  Put32(&field, 0x200); Put32(&field, 0x200);   // raw size, raw offset
  stub.replace(row + 8, field.size(), field);
  const std::uint32_t signature = signed_form ? static_cast<std::uint32_t>(0x400 + ux.size() + 4) : 0;
  const std::string fake_signature(24, '\x5a');
  std::string section;
  Put32(&section, 0x00f14300);
  Put32(&section, 2);
  section += std::string(16, '\x11');  // bundle id
  Put32(&section, 0x400);               // stub size
  Put32(&section, 0);
  Put32(&section, signature);
  Put32(&section, signed_form ? static_cast<std::uint32_t>(fake_signature.size()) : 0);
  Put32(&section, 1);  // cabinet containers
  Put32(&section, 2);
  Put32(&section, static_cast<std::uint32_t>(ux.size()));
  Put32(&section, static_cast<std::uint32_t>(attached.size()));
  stub.replace(0x200, section.size(), section);
  std::string bundle = stub + ux;
  if (signed_form) bundle += std::string(4, '\0') + fake_signature;
  return bundle + attached + std::string(40, '\x77');  // the signature Microsoft adds at the end
}

// Like Microsoft's vc_runtimeAdditional package for one architecture: MFC files in the system folder
// (System64Folder for x64, SystemFolder for x86) taken from an external cab1.cab, a C runtime file that
// must not be installed, and a registry value. MfcCabinet is its cab1.cab.
inline std::string MfcCabinet(bool x64) {
  const std::string suffix = x64 ? "_amd64" : "_x86";
  return StoredCabinet({{"mfc140u.dll" + suffix, x64 ? "MFC64U" : "MFC32U"},
                        {"mfcm140.dll" + suffix, x64 ? "MFCM64" : "MFCM32"},
                        {"msvcp140.dll" + suffix, "MICROSOFT-CRT"}});
}
inline std::string MfcPackage(bool x64) {
  const std::string suffix = x64 ? "_amd64" : "_x86";
  const std::string system = x64 ? "System64Folder" : "SystemFolder";
  std::vector<TableSpec> tables;
  tables.push_back({"Property", {{"Property", 's'}, {"Value", 's'}},
                    {{"ProductCode", "{86AB2CC9-08BD-4643-B0F9-F82D006D72FF}"},
                     {"ProductName", "Microsoft Visual C++ 2022 Additional Runtime"}, {"ProductVersion", "14.44.35211"}}});
  tables.push_back({"Directory", {{"Directory", 's'}, {"Directory_Parent", 's'}, {"DefaultDir", 's'}},
                    {{"TARGETDIR", "", "SourceDir"}, {system, "TARGETDIR", "."}}});
  tables.push_back({"Feature", {{"Feature", 's'}, {"Feature_Parent", 's'}, {"Level", 'i'}}, {{"F_Main", "", "1"}}});
  tables.push_back({"Condition", {{"Feature_", 's'}, {"Level", 'i'}, {"Condition", 's'}}, {}});
  tables.push_back({"Component",
                    {{"Component", 's'}, {"ComponentId", 's'}, {"Directory_", 's'}, {"Attributes", 'i'}, {"Condition", 's'}, {"KeyPath", 's'}},
                    {{"C_MFC", "{A}", system, x64 ? "256" : "0", "", ""}, {"C_CRT", "{B}", system, x64 ? "256" : "0", "", ""}}});
  tables.push_back({"FeatureComponents", {{"Feature_", 's'}, {"Component_", 's'}}, {{"F_Main", "C_MFC"}, {"F_Main", "C_CRT"}}});
  tables.push_back({"File",
                    {{"File", 's'}, {"Component_", 's'}, {"FileName", 's'}, {"FileSize", 'l'}, {"Version", 's'},
                     {"Language", 's'}, {"Attributes", 'i'}, {"Sequence", 'i'}},
                    {{"mfc140u.dll" + suffix, "C_MFC", "mfc140u.dll", "6", "", "", "512", "1"},
                     {"mfcm140.dll" + suffix, "C_MFC", "mfcm140.dll", "6", "", "", "512", "2"},
                     {"msvcp140.dll" + suffix, "C_CRT", "msvcp140.dll", "13", "", "", "512", "3"}}});
  tables.push_back({"Media", {{"DiskId", 'i'}, {"LastSequence", 'i'}, {"DiskPrompt", 's'}, {"Cabinet", 's'}},
                    {{"1", "3", "", "cab1.cab"}}});
  tables.push_back({"Registry",
                    {{"Registry", 's'}, {"Root", 'i'}, {"Key", 's'}, {"Name", 's'}, {"Value", 's'}, {"Component_", 's'}},
                    {{"r1", "2", "SOFTWARE\\Microsoft\\DevDiv\\VC\\Servicing\\14.0\\RuntimeAdditional", "Install", "#1", "C_MFC"}}});
  tables.push_back({"Environment", {{"Environment", 's'}, {"Name", 's'}, {"Value", 's'}, {"Component_", 's'}}, {}});
  tables.push_back({"CustomAction", {{"Action", 's'}, {"Type", 'i'}, {"Source", 's'}, {"Target", 's'}}, {}});
  return Package(tables, {});
}

// A bundle like Microsoft's VC_redist.<arch>.exe: a Minimum package and the Additional one (MFC), listed
// out of order in the attached container, with an InstallCondition that holds an escaped quote.
inline std::string MfcBundle(bool x64, bool signed_form = true, bool with_section = true) {
  const std::string arch = x64 ? "x64" : "x86", folder = x64 ? "amd64" : "x86";
  const auto msi = MfcPackage(x64), cab = MfcCabinet(x64);
  const std::string minimum = "minimum-msi", minimum_cab = StoredCabinet({{"vcruntime140.dll", "CRT"}});
  const auto payload = [](const std::string& id, const std::string& path, std::size_t size, const std::string& source) {
    return "<Payload Id=\"" + id + "\" FilePath=\"" + path + "\" FileSize=\"" + std::to_string(size) +
           "\" Hash=\"00\" Packaging=\"embedded\" SourcePath=\"" + source + "\" Container=\"WixAttachedContainer\" />\r\n";
  };
  std::string manifest = "\xEF\xBB\xBF<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
      "<BurnManifest xmlns=\"http://schemas.microsoft.com/wix/2008/Burn\">\r\n"
      "<!-- <MsiPackage Id=\"vcRuntimeAdditional_" + arch + "\"> in a comment is not read -->\r\n"
      "<UX><Payload Id=\"wixstdba\" FilePath=\"wixstdba.dll\" FileSize=\"6\" SourcePath=\"u0\" /></UX>\r\n"
      "<Container Id=\"WixAttachedContainer\" FileSize=\"1\" Hash=\"00\" FilePath=\"VC_redist." + arch +
      ".exe\" AttachedIndex=\"1\" Attached=\"yes\" Primary=\"yes\" />\r\n" +
      payload("vcRuntimeMinimum_" + arch, "packages\\vcRuntimeMinimum_" + folder + "\\vc_runtimeMinimum_" + arch + ".msi",
              minimum.size(), "a0") +
      payload("vcRuntimeAdditional_" + arch, "packages\\vcRuntimeAdditional_" + folder + "\\vc_runtimeAdditional_" + arch + ".msi",
              msi.size(), "a3") +
      payload("cabMinimum", "packages\\vcRuntimeMinimum_" + folder + "\\cab1.cab", minimum_cab.size(), "a2") +
      payload("cabAdditional", "packages\\vcRuntimeAdditional_" + folder + "\\cab1.cab", cab.size(), "a1") +
      "<Chain>\r\n"
      "<MsiPackage Id=\"vcRuntimeMinimum_" + arch + "\" InstallCondition=\"(VersionNT64) AND NOT (Arm64_Check = &quot;ARM64&quot;)\">"
      "<PayloadRef Id=\"vcRuntimeMinimum_" + arch + "\" /><PayloadRef Id=\"cabMinimum\" /></MsiPackage>\r\n"
      "<MsiPackage Id=\"vcRuntimeAdditional_" + arch + "\" InstallCondition=\"a &gt; b\" Version=\"14.44.35211\">"
      "<MsiProperty Id=\"ARPSYSTEMCOMPONENT\" Value=\"1\" />"
      "<PayloadRef Id=\"vcRuntimeAdditional_" + arch + "\" /><PayloadRef Id=\"cabAdditional\" /></MsiPackage>\r\n"
      "</Chain></BurnManifest>\r\n";
  const auto attached = StoredCabinet({{"a0", minimum}, {"a1", cab}, {"a2", minimum_cab}, {"a3", msi}});
  return BurnBundle(manifest, attached, signed_form, with_section);
}

// A small package like XNA's or a runtime's: files in Program Files\<folder> from its own stored cabinet, and one
// registry value ("Installed" under HKLM\SOFTWARE\<folder>).
inline std::string RuntimePackage(const std::string& folder, const std::vector<std::pair<std::string, std::string>>& files) {
  std::vector<TableSpec> tables;
  tables.push_back({"Property", {{"Property", 's'}, {"Value", 's'}},
                    {{"ProductCode", "{22222222-3333-4444-5555-666666666666}"}, {"ProductName", folder},
                     {"ProductVersion", "1.0.0"}}});
  tables.push_back({"Directory", {{"Directory", 's'}, {"Directory_Parent", 's'}, {"DefaultDir", 's'}},
                    {{"TARGETDIR", "", "SourceDir"}, {"ProgramFilesFolder", "TARGETDIR", "."},
                     {"D_APP", "ProgramFilesFolder", folder}}});
  tables.push_back({"Feature", {{"Feature", 's'}, {"Feature_Parent", 's'}, {"Level", 'i'}}, {{"F_Main", "", "1"}}});
  tables.push_back({"Condition", {{"Feature_", 's'}, {"Level", 'i'}, {"Condition", 's'}}, {}});
  tables.push_back({"Component",
                    {{"Component", 's'}, {"ComponentId", 's'}, {"Directory_", 's'}, {"Attributes", 'i'}, {"Condition", 's'}, {"KeyPath", 's'}},
                    {{"C_APP", "{A}", "D_APP", "0", "", ""}}});
  tables.push_back({"FeatureComponents", {{"Feature_", 's'}, {"Component_", 's'}}, {{"F_Main", "C_APP"}}});
  std::vector<std::vector<std::string>> rows;
  std::vector<std::pair<std::string, std::string>> members;
  for (std::size_t index = 0; index < files.size(); ++index) {
    const auto member = "f" + std::to_string(index);
    rows.push_back({member, "C_APP", files[index].first, std::to_string(files[index].second.size()), "", "", "512",
                    std::to_string(index + 1)});
    members.emplace_back(member, files[index].second);
  }
  tables.push_back({"File",
                    {{"File", 's'}, {"Component_", 's'}, {"FileName", 's'}, {"FileSize", 'l'}, {"Version", 's'},
                     {"Language", 's'}, {"Attributes", 'i'}, {"Sequence", 'i'}},
                    rows});
  tables.push_back({"Media", {{"DiskId", 'i'}, {"LastSequence", 'i'}, {"DiskPrompt", 's'}, {"Cabinet", 's'}},
                    {{"1", std::to_string(files.size()), "", "#data.cab"}}});
  tables.push_back({"Registry",
                    {{"Registry", 's'}, {"Root", 'i'}, {"Key", 's'}, {"Name", 's'}, {"Value", 's'}, {"Component_", 's'}},
                    {{"r1", "2", "SOFTWARE\\" + folder, "Installed", "#1", "C_APP"}}});
  tables.push_back({"Environment", {{"Environment", 's'}, {"Name", 's'}, {"Value", 's'}, {"Component_", 's'}}, {}});
  tables.push_back({"CustomAction", {{"Action", 's'}, {"Type", 'i'}, {"Source", 's'}, {"Target", 's'}}, {}});
  return Package(tables, {{"data.cab", StoredCabinet(members)}});
}

// A Burn bundle like GOG's .NET desktop runtime ones: an MsiPackage for each (id, .msi) in packages, and an ExePackage
// first when exe_package names one. compressed: the attached container is an LZX one, as real bundles' are, so the
// packages can't be found in the program's bytes (one package at most); otherwise it is stored.
inline std::string RuntimeBundle(const std::vector<std::pair<std::string, std::string>>& packages, bool compressed,
                                 const std::string& exe_package = {}) {
  const auto payload = [](const std::string& id, const std::string& path, std::size_t size, const std::string& source) {
    return "<Payload Id=\"" + id + "\" FilePath=\"" + path + "\" FileSize=\"" + std::to_string(size) +
           "\" Hash=\"00\" Packaging=\"embedded\" SourcePath=\"" + source + "\" Container=\"WixAttachedContainer\" />\r\n";
  };
  std::string payloads, chain;
  std::vector<std::pair<std::string, std::string>> members;
  if (!exe_package.empty()) {
    const std::string setup = "MZ-prerequisite-setup";
    payloads += payload(exe_package, exe_package + ".exe", setup.size(), "a" + std::to_string(members.size()));
    chain += "<ExePackage Id=\"" + exe_package + "\" InstallCommand=\"/q\"><PayloadRef Id=\"" + exe_package +
             "\" /></ExePackage>\r\n";
    members.emplace_back("a" + std::to_string(members.size()), setup);
  }
  for (const auto& [id, msi] : packages) {
    payloads += payload(id, "packages\\" + id + "\\" + id + ".msi", msi.size(), "a" + std::to_string(members.size()));
    chain += "<MsiPackage Id=\"" + id + "\" Vital=\"yes\"><PayloadRef Id=\"" + id + "\" /></MsiPackage>\r\n";
    members.emplace_back("a" + std::to_string(members.size()), msi);
  }
  const std::string manifest =
      "\xEF\xBB\xBF<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
      "<BurnManifest xmlns=\"http://schemas.microsoft.com/wix/2008/Burn\">\r\n"
      "<UX><Payload Id=\"wixstdba\" FilePath=\"wixstdba.dll\" FileSize=\"6\" SourcePath=\"u0\" /></UX>\r\n"
      "<Container Id=\"WixAttachedContainer\" FileSize=\"1\" Hash=\"00\" FilePath=\"setup.exe\" AttachedIndex=\"1\" "
      "Attached=\"yes\" Primary=\"yes\" />\r\n" +
      payloads + "<Chain>\r\n" + chain + "</Chain></BurnManifest>\r\n";
  const auto attached = compressed && members.size() == 1 ? LzxVerbatimCabinet(members[0].first, members[0].second)
                                                          : StoredCabinet(members);
  return BurnBundle(manifest, attached);
}

}  // namespace playden::test::msi
