#pragma once

#include "steam/backend/cm_messages.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

// The CM messages and ticket layout a game's online features need from a logged-on Steam client:
// game connect tokens, app ownership tickets, the auth list that activates session tickets, and
// encrypted app tickets. Field numbers follow the pinned reference .proto files in
// fixtures/steam/proto (steammessages_clientserver*.proto, encrypted_app_ticket.proto).
//
// This is the protocol layer only. Nothing here talks to the network or to a game: a game sees
// these tickets only once a Steam API backend inside its process uses them (gbe_fork is offline).
namespace playden::steam::cm {

inline constexpr std::uint32_t kEMsgClientGameConnectTokens = 779;
inline constexpr std::uint32_t kEMsgClientGetAppOwnershipTicket = 857;
inline constexpr std::uint32_t kEMsgClientGetAppOwnershipTicketResponse = 858;
inline constexpr std::uint32_t kEMsgClientTicketAuthComplete = 5429;
inline constexpr std::uint32_t kEMsgClientAuthList = 5432;
inline constexpr std::uint32_t kEMsgClientRequestEncryptedAppTicket = 5526;
inline constexpr std::uint32_t kEMsgClientRequestEncryptedAppTicketResponse = 5527;
inline constexpr std::uint32_t kEMsgClientAuthListAck = 5575;

// A game connect token is 8 bytes; Steam keeps sending more as they are used.
inline constexpr std::size_t kGameConnectTokenBytes = 8;
// The most a game's ticket, ownership ticket or user data may hold here. Real ones are a few
// hundred bytes; anything far larger is not a ticket.
inline constexpr std::size_t kMaxTicketBytes = 4096;

struct GameConnectTokens {
  std::uint32_t max_tokens_to_keep = 10;
  std::vector<std::vector<std::uint8_t>> tokens;  // each kGameConnectTokenBytes
};
// Tokens of any other size are dropped, not an error.
bool DecodeGameConnectTokens(const std::vector<std::uint8_t>& body, GameConnectTokens* tokens);

// Keeps the newest tokens Steam sent, at most max_tokens_to_keep, and hands out the oldest first.
class GameConnectTokenPool {
 public:
  void Add(const GameConnectTokens& message);
  std::optional<std::uint64_t> Take();  // little-endian, as the session ticket stores it
  std::size_t size() const { return tokens_.size(); }

 private:
  std::deque<std::uint64_t> tokens_;
};

std::vector<std::uint8_t> EncodeAppOwnershipTicketRequest(std::uint32_t app_id);
struct AppOwnershipTicketResponse {
  std::int32_t eresult = kEResultFail;
  std::uint32_t app_id = 0;
  std::vector<std::uint8_t> ticket;
};
bool DecodeAppOwnershipTicketResponse(const std::vector<std::uint8_t>& body, AppOwnershipTicketResponse* response);

// What a game gets from GetAuthSessionTicket: the GC token section, the session header and the
// app ownership ticket, as Steam's client lays them out (all little-endian):
//   u32 20, u64 gc_token, u64 steam_id, u32 unix time
//   u32 24, u32 1, u32 2, u32 public IPv4, u32 private IPv4, u32 ms connected, u32 connection count
//   ownership ticket (its own length prefix and signature)
struct SessionTicketInput {
  std::uint64_t gc_token = 0;
  std::uint64_t steam_id = 0;
  std::uint32_t unix_time = 0;
  std::uint32_t public_ip = 0;
  std::uint32_t private_ip = 0;
  std::uint32_t ms_connected = 0;
  std::uint32_t connection_count = 1;
  std::vector<std::uint8_t> ownership_ticket;
};
// Empty when the ownership ticket is empty or larger than kMaxTicketBytes.
std::vector<std::uint8_t> BuildSessionTicket(const SessionTicketInput& input);

// CRC-32 (IEEE 802.3, as zlib) over a ticket: the ticket_crc the auth list and the server use.
std::uint32_t TicketCrc(const std::vector<std::uint8_t>& ticket);

struct ActiveTicket {
  std::uint64_t game_id = 0;  // the app ID for a Steam game
  std::vector<std::uint8_t> ticket;
  std::uint32_t h_steam_pipe = 1;
};
// CMsgClientAuthList: every ticket the game has handed out and not cancelled. Sending it again
// without a ticket cancels that ticket. `message_sequence` counts up from 1 per CM session.
std::vector<std::uint8_t> EncodeAuthList(const std::vector<ActiveTicket>& tickets,
                                         const std::vector<std::uint32_t>& app_ids,
                                         std::uint32_t tokens_left, std::uint32_t message_sequence);

struct AuthListAck {
  std::vector<std::uint32_t> ticket_crcs;
  std::vector<std::uint32_t> app_ids;
  std::uint32_t message_sequence = 0;
};
bool DecodeAuthListAck(const std::vector<std::uint8_t>& body, AuthListAck* ack);

// ISteamUser::RequestEncryptedAppTicket. User data longer than kMaxTicketBytes is refused (empty).
std::vector<std::uint8_t> EncodeEncryptedAppTicketRequest(std::uint32_t app_id,
                                                          const std::vector<std::uint8_t>& user_data);
struct EncryptedAppTicketResponse {
  std::uint32_t app_id = 0;
  std::int32_t eresult = kEResultFail;
  // The EncryptedAppTicket message as Steam sent it: GetEncryptedAppTicket hands these bytes to the
  // game unchanged.
  std::vector<std::uint8_t> ticket;
};
bool DecodeEncryptedAppTicketResponse(const std::vector<std::uint8_t>& body, EncryptedAppTicketResponse* response);

}  // namespace playden::steam::cm
