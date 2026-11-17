// SPDX-License-Identifier: GPL-3.0-or-later
// playden-steam-dev: Steam reads from Playden's own client code, on the host, for a maintainer probing a real
// account (#536 Phase 0). Not shipped: the host preset builds it, the PS5 build doesn't.
//
//   playden-steam-dev --data <folder> sign-in        account name from $PLAYDEN_STEAM_DEV_ACCOUNT, password from
//                                                    $PLAYDEN_STEAM_DEV_PASSWORD; a Steam Guard code is read from
//                                                    <folder>/guard-code when Steam asks for one (the file is
//                                                    deleted after reading). The device is listed as
//                                                    "Playden dev (Mac)". Saves <folder>/session.v1.
//   playden-steam-dev --data <folder> sign-in-qr <png file>
//                                                    QR sign-in, approved in the Steam app; the QR code is written
//                                                    to <png file> (rewritten when Steam renews it). The device is
//                                                    listed as "Playden dev (Mac)". Saves <folder>/session.v1.
//   playden-steam-dev --data <folder> collections     Playden's own read of the Steam collections, as the Library
//                                                    reads them: its log line and each collection's game count
//   playden-steam-dev --data <folder> config <namespace> [version] <out file>
//                                                    CloudConfigStore.Download#1 over CM for one namespace; writes
//                                                    the decoded entries to <out file> (0600) and prints counts
//
// <folder> is absolute, with no link in any component. Output never holds a token or the account name.
#include "homebrew/qr.hpp"
#include "platform/storage.h"
#include "steam/backend/auth_client.h"
#include "steam/backend/collections_client.h"
#include "steam/backend/cm_client.h"
#include "steam/backend/proto_wire.h"
#include "steam/session_store.h"

#include <sys/stat.h>

#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#ifndef PLAYDEN_STEAM_DEV_CA_BUNDLE
#define PLAYDEN_STEAM_DEV_CA_BUNDLE ""
#endif

namespace {
using namespace playden;
namespace proto = playden::steam::proto;

int Usage() {
  std::fputs("usage: playden-steam-dev --data <folder> sign-in | sign-in-qr <png file> | collections |\n"
             "                         config <namespace> [version] <out file>\n", stderr);
  return 2;
}

std::string Escape(const std::string& text) {
  std::string out;
  for (unsigned char c : text) {
    if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
    else if (c == '\n') out += "\\n";
    else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); out += b; }
    else out += static_cast<char>(c);
  }
  return out;
}

int SignIn(const std::string& folder) {
  const char* account = std::getenv("PLAYDEN_STEAM_DEV_ACCOUNT");
  const char* password = std::getenv("PLAYDEN_STEAM_DEV_PASSWORD");
  if (!account || !password) return Usage();
  Storage storage(folder);
  steam::CmClient cm(PLAYDEN_STEAM_DEV_CA_BUNDLE, storage);
  steam::AuthClient auth(PLAYDEN_STEAM_DEV_CA_BUNDLE);
  auth.SetDeviceName("Playden dev (Mac)");
  auth.SetMachineId([&storage] {
    return steam::MakeDeviceIdentity(steam::LoadOrCreateDeviceSeed(storage)).machine_id;
  });
  auth.SetCmRenewal([&cm](const StoredSession& s, const std::atomic<bool>& c) { return steam::RenewThroughCm(cm, s, c); });
  auth.SetFailureReport([](const char* method, long http, int eresult) {
    std::fprintf(stderr, "auth_failure method=%s http=%ld eresult=%d\n", method, http, eresult);
  });
  steam::Credentials credentials{account, password};
  const std::string code_file = folder + "/guard-code";
  std::atomic<bool> cancel{false};
  const auto result = auth.AuthenticateCredentials(
      credentials, cancel,
      [](const steam::GuardPrompt& prompt) {
        std::fprintf(stderr, "guard prompt kind=%d code_allowed=%d rejected=%d; write the code to <folder>/guard-code\n",
                     static_cast<int>(prompt.kind), prompt.code_allowed ? 1 : 0, prompt.code_rejected ? 1 : 0);
      },
      [&code_file]() -> std::optional<std::string> {
        std::ifstream in(code_file);
        if (!in) return std::nullopt;
        std::string code;
        std::getline(in, code);
        in.close();
        std::remove(code_file.c_str());
        while (!code.empty() && (code.back() == '\r' || code.back() == ' ')) code.pop_back();
        if (code.empty()) return std::nullopt;
        return code;
      });
  if (result.error != steam::AuthError::kNone || !result.session) {
    std::fprintf(stderr, "sign-in failed: %d\n", static_cast<int>(result.error));
    return 1;
  }
  SessionStore store(storage);
  if (!store.Save(*result.session).ok()) {
    std::fputs("session not saved\n", stderr);
    return 1;
  }
  std::puts("signed in; session saved");
  return 0;
}

// A PNG of the QR code: 8 pixels a module, 4 modules of quiet zone, stored (uncompressed) deflate blocks.
std::uint32_t Crc(const std::uint8_t* data, std::size_t size, std::uint32_t crc = 0xffffffffu) {
  for (std::size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
  }
  return crc;
}
void Big(std::vector<std::uint8_t>& out, std::uint32_t value) {
  for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<std::uint8_t>(value >> shift));
}
void Chunk(std::vector<std::uint8_t>& png, const char* type, const std::vector<std::uint8_t>& data) {
  Big(png, static_cast<std::uint32_t>(data.size()));
  std::vector<std::uint8_t> body(type, type + 4);
  body.insert(body.end(), data.begin(), data.end());
  png.insert(png.end(), body.begin(), body.end());
  Big(png, ~Crc(body.data(), body.size()));
}
bool WriteQrPng(const std::string& text, const std::string& path) {
  const auto matrix = homebrew::encode_qr(text);
  if (matrix.empty()) return false;
  constexpr int kScale = 8, kQuiet = 4;
  const int side = (matrix.size() + 2 * kQuiet) * kScale;
  std::vector<std::uint8_t> raw;
  for (int y = 0; y < side; ++y) {
    raw.push_back(0);
    for (int x = 0; x < side; ++x) {
      const int mx = x / kScale - kQuiet, my = y / kScale - kQuiet;
      const bool dark = mx >= 0 && my >= 0 && mx < matrix.size() && my < matrix.size() && matrix.at(mx, my);
      raw.push_back(dark ? 0 : 255);
    }
  }
  std::vector<std::uint8_t> zlib{0x78, 0x01};
  std::uint32_t a = 1, b = 0;
  for (const auto byte : raw) {
    a = (a + byte) % 65521;
    b = (b + a) % 65521;
  }
  for (std::size_t at = 0; at < raw.size();) {
    const std::size_t count = std::min<std::size_t>(65535, raw.size() - at);
    zlib.push_back(at + count == raw.size() ? 1 : 0);
    zlib.push_back(static_cast<std::uint8_t>(count));
    zlib.push_back(static_cast<std::uint8_t>(count >> 8));
    zlib.push_back(static_cast<std::uint8_t>(~count));
    zlib.push_back(static_cast<std::uint8_t>(~count >> 8));
    zlib.insert(zlib.end(), raw.begin() + static_cast<std::ptrdiff_t>(at), raw.begin() + static_cast<std::ptrdiff_t>(at + count));
    at += count;
  }
  Big(zlib, (b << 16) | a);
  std::vector<std::uint8_t> png{0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
  std::vector<std::uint8_t> header;
  Big(header, static_cast<std::uint32_t>(side));
  Big(header, static_cast<std::uint32_t>(side));
  header.insert(header.end(), {8, 0, 0, 0, 0});  // 8-bit greyscale
  Chunk(png, "IHDR", header);
  Chunk(png, "IDAT", zlib);
  Chunk(png, "IEND", {});
  const std::string temporary = path + ".tmp";
  FILE* out = std::fopen(temporary.c_str(), "wb");
  if (!out) return false;
  chmod(temporary.c_str(), 0600);
  const bool written = std::fwrite(png.data(), 1, png.size(), out) == png.size();
  std::fclose(out);
  return written && std::rename(temporary.c_str(), path.c_str()) == 0;
}

int SignInQr(const std::string& folder, const std::string& png) {
  Storage storage(folder);
  steam::CmClient cm(PLAYDEN_STEAM_DEV_CA_BUNDLE, storage);
  steam::AuthClient auth(PLAYDEN_STEAM_DEV_CA_BUNDLE);
  auth.SetDeviceName("Playden dev (Mac)");
  auth.SetMachineId([&storage] {
    return steam::MakeDeviceIdentity(steam::LoadOrCreateDeviceSeed(storage)).machine_id;
  });
  auth.SetCmRenewal([&cm](const StoredSession& s, const std::atomic<bool>& c) { return steam::RenewThroughCm(cm, s, c); });
  auth.SetFailureReport([](const char* method, long http, int eresult) {
    std::fprintf(stderr, "auth_failure method=%s http=%ld eresult=%d\n", method, http, eresult);
  });
  std::atomic<bool> cancel{false};
  const auto result = auth.AuthenticateQr(cancel, [&png](const std::string& challenge) {
    std::fprintf(stderr, WriteQrPng(challenge, png) ? "qr code written\n" : "qr code not written\n");
  });
  std::remove(png.c_str());
  if (result.error != steam::AuthError::kNone || !result.session) {
    std::fprintf(stderr, "sign-in failed: %d\n", static_cast<int>(result.error));
    return 1;
  }
  if (!SessionStore(storage).Save(*result.session).ok()) {
    std::fputs("session not saved\n", stderr);
    return 1;
  }
  std::puts("signed in; session saved");
  return 0;
}

int Config(const std::string& folder, std::uint32_t space, std::uint64_t version, const std::string& out_file) {
  Storage storage(folder);
  const auto loaded = SessionStore(storage).Load();
  if (!loaded.ok() || !loaded.value()) {
    std::fputs("no saved session\n", stderr);
    return 1;
  }
  const StoredSession session = *loaded.value();
  steam::CmClient cm(PLAYDEN_STEAM_DEV_CA_BUNDLE, storage);
  proto::Writer ns;
  ns.AddVarint(1, space);
  ns.AddVarint(2, version);
  proto::Writer request;
  request.AddMessage(1, ns);
  std::vector<std::uint8_t> reply;
  int eresult = 0;
  std::atomic<bool> cancel{false};
  const auto error = cm.WithServiceSession(session, 30'000, cancel, [&](const steam::CmClient::ServiceCall& call, std::uint64_t) {
    return call("CloudConfigStore.Download#1", request.bytes(), &reply, &eresult);
  });
  std::fprintf(stderr, "call error=%d eresult=%d reply_bytes=%zu\n", static_cast<int>(error), eresult, reply.size());
  if (error != steam::CmError::kNone) return 1;

  proto::DecodeLimits limits;
  limits.max_message_bytes = 64u << 20;
  limits.max_length_delimited_bytes = 64u << 20;
  limits.max_fields = 1u << 22;
  std::string json = "[\n";
  proto::Reader top(reply, limits);
  proto::Field field;
  std::size_t namespaces = 0, entries = 0, deleted = 0;
  while (top.Next(&field)) {
    if (field.number != 1 || field.wire_type != proto::WireType::kLengthDelimited) {
      std::fprintf(stderr, "top-level field %u wire %d\n", field.number, static_cast<int>(field.wire_type));
      continue;
    }
    ++namespaces;
    proto::Reader data;
    if (!top.Submessage(field, &data)) break;
    proto::Field f;
    while (data.Next(&f)) {
      if (f.number == 3 && f.wire_type == proto::WireType::kLengthDelimited) {
        proto::Reader entry;
        if (!data.Submessage(f, &entry)) break;
        proto::Field e;
        std::string key, value;
        bool is_deleted = false;
        std::uint64_t timestamp = 0, entry_version = 0;
        std::string unknown;
        while (entry.Next(&e)) {
          switch (e.number) {
            case 1: key = e.AsString(); break;
            case 2: is_deleted = e.value != 0; break;
            case 3: value = e.AsString(); break;
            case 4: timestamp = e.value; break;
            case 5: entry_version = e.value; break;
            default: unknown += std::to_string(e.number) + " ";
          }
        }
        ++entries;
        if (is_deleted) ++deleted;
        json += "  {\"key\":\"" + Escape(key) + "\",\"is_deleted\":" + (is_deleted ? "true" : "false") +
                ",\"timestamp\":" + std::to_string(timestamp) + ",\"version\":" + std::to_string(entry_version) +
                (unknown.empty() ? "" : ",\"unknown_fields\":\"" + unknown + "\"") + ",\"value\":\"" + Escape(value) +
                "\"},\n";
      } else {
        std::fprintf(stderr, "namespace field %u wire %d value %" PRIu64 " size %zu\n", f.number,
                     static_cast<int>(f.wire_type), f.value, f.size);
      }
    }
    if (!data.ok()) std::fprintf(stderr, "namespace decode error %d\n", static_cast<int>(data.error()));
  }
  if (!top.ok()) std::fprintf(stderr, "top decode error %d\n", static_cast<int>(top.error()));
  json += "]\n";
  FILE* out = std::fopen(out_file.c_str(), "w");
  if (!out) return 1;
  chmod(out_file.c_str(), 0600);
  std::fwrite(json.data(), 1, json.size(), out);
  std::fclose(out);
  std::printf("namespaces=%zu entries=%zu deleted=%zu\n", namespaces, entries, deleted);
  return 0;
}

// Playden's own read of the account's Steam collections (FetchLibraryCollections, as the Library does): the log line
// it writes, then the number of games in each kept collection (no names or IDs).
int Collections(const std::string& folder) {
  Storage storage(folder);
  const auto loaded = SessionStore(storage).Load();
  if (!loaded.ok() || !loaded.value()) {
    std::fputs("no saved session\n", stderr);
    return 1;
  }
  steam::CmClient cm(PLAYDEN_STEAM_DEV_CA_BUNDLE, storage);
  std::atomic<bool> cancel{false};
  const auto collections = steam::FetchLibraryCollections(cm, *loaded.value(), cancel, [](const std::string& line) {
    std::printf("%s\n", line.c_str());
  });
  if (!collections) return 1;
  std::printf("games per collection:");
  for (const auto& collection : *collections) std::printf(" %zu", collection.members.size());
  std::printf("\n");
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::string folder;
  int i = 1;
  if (i + 1 < argc && std::strcmp(argv[i], "--data") == 0) {
    folder = argv[i + 1];
    i += 2;
  }
  if (folder.empty() || folder[0] != '/' || i >= argc) return Usage();
  const std::string command = argv[i++];
  if (command == "sign-in") return SignIn(folder);
  if (command == "sign-in-qr" && argc - i == 1 && argv[i][0] == '/') return SignInQr(folder, argv[i]);
  if (command == "collections" && argc - i == 0) return Collections(folder);
  if (command == "config") {
    if (argc - i == 2) return Config(folder, std::strtoul(argv[i], nullptr, 10), 0, argv[i + 1]);
    if (argc - i == 3) return Config(folder, std::strtoul(argv[i], nullptr, 10), std::strtoull(argv[i + 1], nullptr, 10), argv[i + 2]);
  }
  return Usage();
}
