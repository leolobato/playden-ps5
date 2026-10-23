#include "steam/backend/game_tickets.h"

#include "steam/backend/proto_wire.h"

namespace playden::steam::cm {
namespace {

constexpr proto::DecodeLimits kLimits{kMaxFrameBytes, kMaxFrameBytes, kMaxRepeatedEntries + 16, 8};

void AppendLE32(std::vector<std::uint8_t>& out, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) out.push_back(static_cast<std::uint8_t>(value >> shift));
}
void AppendLE64(std::vector<std::uint8_t>& out, std::uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) out.push_back(static_cast<std::uint8_t>(value >> shift));
}

std::int32_t AsInt32(std::uint64_t value) { return static_cast<std::int32_t>(static_cast<std::uint32_t>(value)); }
bool IsVarint(const proto::Field& field) { return field.wire_type == proto::WireType::kVarint; }
bool IsBytes(const proto::Field& field) { return field.wire_type == proto::WireType::kLengthDelimited; }

// A repeated uint32: packed (length-delimited) or one varint per entry, as protobuf allows both.
bool AppendUint32s(const proto::Field& field, std::vector<std::uint32_t>* out) {
  if (IsVarint(field)) {
    out->push_back(static_cast<std::uint32_t>(field.value));
    return true;
  }
  if (!IsBytes(field)) return false;
  std::size_t at = 0;
  while (at < field.size) {
    std::uint64_t value = 0;
    int shift = 0;
    for (;;) {
      if (at >= field.size || shift > 63) return false;
      const std::uint8_t byte = field.data[at++];
      value |= static_cast<std::uint64_t>(byte & 0x7f) << shift;
      if (!(byte & 0x80)) break;
      shift += 7;
    }
    if (out->size() >= kMaxRepeatedEntries) return false;
    out->push_back(static_cast<std::uint32_t>(value));
  }
  return true;
}

}  // namespace

bool DecodeGameConnectTokens(const std::vector<std::uint8_t>& body, GameConnectTokens* tokens) {
  proto::Reader reader(body, kLimits);
  proto::Field field;
  GameConnectTokens decoded;
  while (reader.Next(&field)) {
    if (field.number == 1) {
      if (!IsVarint(field)) return false;
      decoded.max_tokens_to_keep = static_cast<std::uint32_t>(field.value);
    } else if (field.number == 2) {
      if (!IsBytes(field)) return false;
      if (field.size != kGameConnectTokenBytes) continue;
      decoded.tokens.emplace_back(field.data, field.data + field.size);
    }
  }
  if (!reader.ok()) return false;
  *tokens = std::move(decoded);
  return true;
}

void GameConnectTokenPool::Add(const GameConnectTokens& message) {
  for (const auto& bytes : message.tokens) {
    if (bytes.size() != kGameConnectTokenBytes) continue;
    std::uint64_t token = 0;
    for (std::size_t i = 0; i < kGameConnectTokenBytes; ++i) token |= static_cast<std::uint64_t>(bytes[i]) << (8 * i);
    tokens_.push_back(token);
  }
  const std::size_t keep = message.max_tokens_to_keep == 0 ? 1 : message.max_tokens_to_keep;
  while (tokens_.size() > keep) tokens_.pop_front();
}

std::optional<std::uint64_t> GameConnectTokenPool::Take() {
  if (tokens_.empty()) return std::nullopt;
  const std::uint64_t token = tokens_.front();
  tokens_.pop_front();
  return token;
}

std::vector<std::uint8_t> EncodeAppOwnershipTicketRequest(std::uint32_t app_id) {
  proto::Writer body;
  body.AddVarint(1, app_id);
  return body.bytes();
}

bool DecodeAppOwnershipTicketResponse(const std::vector<std::uint8_t>& body, AppOwnershipTicketResponse* response) {
  proto::Reader reader(body, kLimits);
  proto::Field field;
  AppOwnershipTicketResponse decoded;
  while (reader.Next(&field)) {
    if (field.number == 1) {
      if (!IsVarint(field)) return false;
      decoded.eresult = AsInt32(field.value);
    } else if (field.number == 2) {
      if (!IsVarint(field)) return false;
      decoded.app_id = static_cast<std::uint32_t>(field.value);
    } else if (field.number == 3) {
      if (!IsBytes(field) || field.size > kMaxTicketBytes) return false;
      decoded.ticket.assign(field.data, field.data + field.size);
    }
  }
  if (!reader.ok()) return false;
  *response = std::move(decoded);
  return true;
}

std::vector<std::uint8_t> BuildSessionTicket(const SessionTicketInput& input) {
  if (input.ownership_ticket.empty() || input.ownership_ticket.size() > kMaxTicketBytes) return {};
  std::vector<std::uint8_t> ticket;
  ticket.reserve(52 + input.ownership_ticket.size());
  AppendLE32(ticket, 20);
  AppendLE64(ticket, input.gc_token);
  AppendLE64(ticket, input.steam_id);
  AppendLE32(ticket, input.unix_time);
  AppendLE32(ticket, 24);
  AppendLE32(ticket, 1);
  AppendLE32(ticket, 2);
  AppendLE32(ticket, input.public_ip);
  AppendLE32(ticket, input.private_ip);
  AppendLE32(ticket, input.ms_connected);
  AppendLE32(ticket, input.connection_count);
  ticket.insert(ticket.end(), input.ownership_ticket.begin(), input.ownership_ticket.end());
  return ticket;
}

std::uint32_t TicketCrc(const std::vector<std::uint8_t>& ticket) {
  std::uint32_t crc = 0xffffffffu;
  for (const std::uint8_t byte : ticket) {
    crc ^= byte;
    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

std::vector<std::uint8_t> EncodeAuthList(const std::vector<ActiveTicket>& tickets,
                                         const std::vector<std::uint32_t>& app_ids,
                                         std::uint32_t tokens_left, std::uint32_t message_sequence) {
  proto::Writer body;
  body.AddVarint(1, tokens_left);
  body.AddVarint(2, message_sequence);
  body.AddVarint(3, 0);
  for (const auto& active : tickets) {
    proto::Writer entry;
    entry.AddVarint(1, 0);  // estate
    entry.AddVarint(2, 1);  // eresult: OK, as Steam's client sends it
    entry.AddFixed64(4, active.game_id);
    entry.AddVarint(5, active.h_steam_pipe);
    entry.AddVarint(6, TicketCrc(active.ticket));
    entry.AddBytes(7, active.ticket);
    body.AddMessage(4, entry);
  }
  for (const std::uint32_t app_id : app_ids) body.AddVarint(5, app_id);
  body.AddVarint(6, message_sequence);
  return body.bytes();
}

bool DecodeAuthListAck(const std::vector<std::uint8_t>& body, AuthListAck* ack) {
  proto::Reader reader(body, kLimits);
  proto::Field field;
  AuthListAck decoded;
  while (reader.Next(&field)) {
    if (field.number == 1) {
      if (!AppendUint32s(field, &decoded.ticket_crcs)) return false;
    } else if (field.number == 2) {
      if (!AppendUint32s(field, &decoded.app_ids)) return false;
    } else if (field.number == 3) {
      if (!IsVarint(field)) return false;
      decoded.message_sequence = static_cast<std::uint32_t>(field.value);
    }
  }
  if (!reader.ok()) return false;
  *ack = std::move(decoded);
  return true;
}

std::vector<std::uint8_t> EncodeEncryptedAppTicketRequest(std::uint32_t app_id,
                                                          const std::vector<std::uint8_t>& user_data) {
  if (user_data.size() > kMaxTicketBytes) return {};
  proto::Writer body;
  body.AddVarint(1, app_id);
  if (!user_data.empty()) body.AddBytes(2, user_data);
  return body.bytes();
}

bool DecodeEncryptedAppTicketResponse(const std::vector<std::uint8_t>& body, EncryptedAppTicketResponse* response) {
  proto::Reader reader(body, kLimits);
  proto::Field field;
  EncryptedAppTicketResponse decoded;
  while (reader.Next(&field)) {
    if (field.number == 1) {
      if (!IsVarint(field)) return false;
      decoded.app_id = static_cast<std::uint32_t>(field.value);
    } else if (field.number == 2) {
      if (!IsVarint(field)) return false;
      decoded.eresult = AsInt32(field.value);
    } else if (field.number == 3) {
      if (!IsBytes(field) || field.size > kMaxTicketBytes) return false;
      proto::Reader ticket(field.data, field.size, kLimits);
      proto::Field inner;
      while (ticket.Next(&inner)) {}
      if (!ticket.ok()) return false;
      decoded.ticket.assign(field.data, field.data + field.size);
    }
  }
  if (!reader.ok()) return false;
  *response = std::move(decoded);
  return true;
}

}  // namespace playden::steam::cm
