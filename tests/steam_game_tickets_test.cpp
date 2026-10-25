#include "steam/backend/game_tickets.h"
#include "steam/backend/proto_wire.h"

#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

using namespace playden::steam;

namespace {

std::vector<std::uint8_t> Bytes(const std::string& text) { return {text.begin(), text.end()}; }

std::uint32_t LE32(const std::vector<std::uint8_t>& data, std::size_t at) {
  return data[at] | data[at + 1] << 8 | data[at + 2] << 16 | static_cast<std::uint32_t>(data[at + 3]) << 24;
}
std::uint64_t LE64(const std::vector<std::uint8_t>& data, std::size_t at) {
  return LE32(data, at) | static_cast<std::uint64_t>(LE32(data, at + 4)) << 32;
}

void CrcMatchesZlib() {
  assert(cm::TicketCrc({}) == 0);
  assert(cm::TicketCrc(Bytes("123456789")) == 0xcbf43926u);
}

void OwnershipTicketRoundTrip() {
  // app_id 1245620 = varint b4 83 4c
  assert((cm::EncodeAppOwnershipTicketRequest(1245620) == std::vector<std::uint8_t>{0x08, 0xb4, 0x83, 0x4c}));
  proto::Writer reply;
  reply.AddVarint(1, 1);
  reply.AddVarint(2, 1245620);
  reply.AddBytes(3, Bytes("OWNERSHIP"));
  cm::AppOwnershipTicketResponse response;
  assert(cm::DecodeAppOwnershipTicketResponse(reply.bytes(), &response));
  assert(response.eresult == cm::kEResultOk && response.app_id == 1245620 && response.ticket == Bytes("OWNERSHIP"));

  cm::AppOwnershipTicketResponse refused;
  assert(cm::DecodeAppOwnershipTicketResponse({}, &refused) && refused.eresult == cm::kEResultFail);

  proto::Writer huge;
  huge.AddBytes(3, std::vector<std::uint8_t>(cm::kMaxTicketBytes + 1, 0));
  assert(!cm::DecodeAppOwnershipTicketResponse(huge.bytes(), &response));
}

void ConnectTokensKeepNewest() {
  proto::Writer message;
  message.AddVarint(1, 2);
  for (std::uint8_t first : {1, 2, 3}) {
    std::vector<std::uint8_t> token(8, 0);
    token[0] = first;
    message.AddBytes(2, token);
  }
  message.AddBytes(2, Bytes("short"));  // not a token: dropped
  cm::GameConnectTokens tokens;
  assert(cm::DecodeGameConnectTokens(message.bytes(), &tokens));
  assert(tokens.max_tokens_to_keep == 2 && tokens.tokens.size() == 3);

  cm::GameConnectTokenPool pool;
  pool.Add(tokens);
  assert(pool.size() == 2);
  assert(pool.Take() == 2u && pool.Take() == 3u && !pool.Take());
}

void SessionTicketLayout() {
  cm::SessionTicketInput input;
  input.gc_token = 0x1122334455667788ull;
  input.steam_id = 76561197960287930ull;
  input.unix_time = 1700000000;
  input.public_ip = 0x0a000001;
  input.ms_connected = 5000;
  input.ownership_ticket = Bytes("OWN");
  const auto ticket = cm::BuildSessionTicket(input);
  assert(ticket.size() == 52 + 3);
  assert(LE32(ticket, 0) == 20 && LE64(ticket, 4) == input.gc_token && LE64(ticket, 12) == input.steam_id);
  assert(LE32(ticket, 20) == 1700000000);
  assert(LE32(ticket, 24) == 24 && LE32(ticket, 28) == 1 && LE32(ticket, 32) == 2);
  assert(LE32(ticket, 36) == 0x0a000001 && LE32(ticket, 40) == 0 && LE32(ticket, 44) == 5000 && LE32(ticket, 48) == 1);
  assert(ticket[52] == 'O' && ticket[54] == 'N');

  input.ownership_ticket.clear();
  assert(cm::BuildSessionTicket(input).empty());
}

void AuthListCarriesTickets() {
  const auto ticket = Bytes("TICKET");
  const auto body = cm::EncodeAuthList({{1245620, ticket, 1}}, {1245620}, 9, 3);
  proto::Reader reader(body);
  proto::Field field;
  bool saw_ticket = false;
  std::uint32_t sequence = 0, app = 0, left = 0;
  while (reader.Next(&field)) {
    if (field.number == 1) left = static_cast<std::uint32_t>(field.value);
    if (field.number == 5) app = static_cast<std::uint32_t>(field.value);
    if (field.number == 6) sequence = static_cast<std::uint32_t>(field.value);
    if (field.number == 4) {
      proto::Reader entry;
      assert(reader.Submessage(field, &entry));
      proto::Field inner;
      while (entry.Next(&inner)) {
        if (inner.number == 4) assert(inner.value == 1245620);
        if (inner.number == 6) assert(inner.value == cm::TicketCrc(ticket));
        if (inner.number == 7) saw_ticket = inner.AsString() == "TICKET";
      }
    }
  }
  assert(reader.ok() && saw_ticket && sequence == 3 && app == 1245620 && left == 9);

  proto::Writer ack;
  ack.AddVarint(1, cm::TicketCrc(ticket));
  ack.AddBytes(2, std::vector<std::uint8_t>{0xb4, 0x83, 0x4c, 0x05});  // packed: 1245620, 5
  ack.AddVarint(3, 3);
  cm::AuthListAck decoded;
  assert(cm::DecodeAuthListAck(ack.bytes(), &decoded));
  assert((decoded.ticket_crcs == std::vector<std::uint32_t>{cm::TicketCrc(ticket)}));
  assert((decoded.app_ids == std::vector<std::uint32_t>{1245620, 5}) && decoded.message_sequence == 3);
  assert(!cm::DecodeAuthListAck(std::vector<std::uint8_t>{0x12, 0x01, 0x80}, &decoded));  // truncated packed varint
}

void EncryptedAppTicketRoundTrip() {
  assert((cm::EncodeEncryptedAppTicketRequest(10, {}) == std::vector<std::uint8_t>{0x08, 0x0a}));
  assert((cm::EncodeEncryptedAppTicketRequest(10, {7}) == std::vector<std::uint8_t>{0x08, 0x0a, 0x12, 0x01, 0x07}));
  assert(cm::EncodeEncryptedAppTicketRequest(10, std::vector<std::uint8_t>(cm::kMaxTicketBytes + 1)).empty());

  proto::Writer inner;
  inner.AddVarint(1, 4);
  inner.AddBytes(5, Bytes("SECRET"));
  proto::Writer reply;
  reply.AddVarint(1, 10);
  reply.AddVarint(2, 1);
  reply.AddMessage(3, inner);
  cm::EncryptedAppTicketResponse response;
  assert(cm::DecodeEncryptedAppTicketResponse(reply.bytes(), &response));
  assert(response.app_id == 10 && response.eresult == cm::kEResultOk && response.ticket == inner.bytes());

  proto::Writer broken;
  broken.AddBytes(3, std::vector<std::uint8_t>{0x0a, 0x05});  // inner field runs past the end
  assert(!cm::DecodeEncryptedAppTicketResponse(broken.bytes(), &response));
}

}  // namespace

int main() {
  CrcMatchesZlib();
  OwnershipTicketRoundTrip();
  ConnectTokensKeepNewest();
  SessionTicketLayout();
  AuthListCarriesTickets();
  EncryptedAppTicketRoundTrip();
  std::puts("steam_game_tickets: ok");
  return 0;
}
