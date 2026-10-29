#include "steam/backend/cm_messages.h"

#include "steam/backend/proto_wire.h"

namespace playden::steam::cm {
namespace {

constexpr proto::DecodeLimits kLimits{kMaxFrameBytes, kMaxFrameBytes, kMaxRepeatedEntries + 16, 8};

std::uint32_t ReadLE32(const std::uint8_t* data) {
  return static_cast<std::uint32_t>(data[0]) | static_cast<std::uint32_t>(data[1]) << 8 |
         static_cast<std::uint32_t>(data[2]) << 16 | static_cast<std::uint32_t>(data[3]) << 24;
}

void AppendLE32(std::vector<std::uint8_t>& out, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8)
    out.push_back(static_cast<std::uint8_t>(value >> shift));
}

// Varint fields declared int32 carry negative values sign-extended to 64 bits.
std::int32_t AsInt32(std::uint64_t value) {
  return static_cast<std::int32_t>(static_cast<std::uint32_t>(value));
}

bool IsVarint(const proto::Field& field) { return field.wire_type == proto::WireType::kVarint; }
bool IsBytes(const proto::Field& field) {
  return field.wire_type == proto::WireType::kLengthDelimited;
}
bool IsFixed32(const proto::Field& field) { return field.wire_type == proto::WireType::kFixed32; }
bool IsFixed64(const proto::Field& field) { return field.wire_type == proto::WireType::kFixed64; }

std::vector<std::uint8_t> Bytes(const proto::Field& field) {
  return std::vector<std::uint8_t>(field.data, field.data + field.size);
}

bool DecodeHeader(const std::uint8_t* data, std::size_t size, Header* header) {
  proto::Reader reader(data, size, kLimits);
  proto::Field field;
  while (reader.Next(&field)) {
    switch (field.number) {
      case 1:
        if (!IsFixed64(field)) return false;
        header->steam_id = field.value;
        break;
      case 2:
        if (!IsVarint(field)) return false;
        header->session_id = AsInt32(field.value);
        break;
      case 10:
        if (!IsFixed64(field)) return false;
        header->jobid_source = field.value;
        break;
      case 11:
        if (!IsFixed64(field)) return false;
        header->jobid_target = field.value;
        break;
      case 13:
        if (!IsVarint(field)) return false;
        header->eresult = AsInt32(field.value);
        break;
      default:
        break;
    }
  }
  return reader.ok();
}

}  // namespace

std::vector<std::uint8_t> EncodeFrame(std::uint32_t emsg, const Header& header,
                                      const std::vector<std::uint8_t>& body) {
  proto::Writer encoded(kMaxFrameBytes);
  encoded.AddFixed64(1, header.steam_id);
  encoded.AddVarint(2, static_cast<std::uint64_t>(static_cast<std::int64_t>(header.session_id)));
  if (header.jobid_source) encoded.AddFixed64(10, *header.jobid_source);
  if (header.jobid_target) encoded.AddFixed64(11, *header.jobid_target);
  if (!header.target_job_name.empty()) encoded.AddString(12, header.target_job_name);
  if (header.eresult)
    encoded.AddVarint(13, static_cast<std::uint64_t>(static_cast<std::int64_t>(*header.eresult)));
  if (!encoded.ok() || 8 + encoded.bytes().size() + body.size() > kMaxFrameBytes) return {};
  std::vector<std::uint8_t> frame;
  frame.reserve(8 + encoded.bytes().size() + body.size());
  AppendLE32(frame, emsg | kProtoMask);
  AppendLE32(frame, static_cast<std::uint32_t>(encoded.bytes().size()));
  frame.insert(frame.end(), encoded.bytes().begin(), encoded.bytes().end());
  frame.insert(frame.end(), body.begin(), body.end());
  return frame;
}

bool DecodeFrame(const std::uint8_t* data, std::size_t size, Frame* frame) {
  if (data == nullptr || size < 8 || size > kMaxFrameBytes) return false;
  const std::uint32_t raw = ReadLE32(data);
  if ((raw & kProtoMask) == 0) return false;
  const std::uint32_t header_size = ReadLE32(data + 4);
  if (header_size > size - 8) return false;
  Frame decoded;
  decoded.emsg = raw & ~kProtoMask;
  if (!DecodeHeader(data + 8, header_size, &decoded.header)) return false;
  decoded.body.assign(data + 8 + header_size, data + size);
  *frame = std::move(decoded);
  return true;
}

std::vector<std::uint8_t> EncodeHello() {
  proto::Writer body;
  body.AddVarint(1, kProtocolVersion);
  return body.bytes();
}

std::vector<std::uint8_t> EncodeLogon(const std::string& account_name,
                                      const std::string& refresh_token, std::uint32_t cell_id,
                                      const DeviceIdentity& device) {
  proto::Writer address;
  address.AddFixed32(1, device.login_id);
  proto::Writer body;
  body.AddVarint(1, kProtocolVersion);
  body.AddVarint(3, cell_id);
  body.AddVarint(5, kClientPackageVersion);
  body.AddString(6, "english");
  body.AddVarint(7, 0);  // EOSType WinUnknown, as the reference client
  body.AddVarint(8, 1);  // should_remember_password: without it Steam signs the device out when the session ends
  body.AddMessage(11, address);
  body.AddBytes(30, device.machine_id);
  body.AddString(50, account_name);
  body.AddString(96, device.machine_name);
  body.AddVarint(102, 1);
  body.AddString(108, refresh_token);  // CM logon takes the refresh token in access_token
  if (!body.ok()) return {};
  return body.bytes();
}

std::vector<std::uint8_t> EncodeHeartBeat() { return {}; }

bool DecodeLogonResponse(const std::vector<std::uint8_t>& body, LogonResponse* response) {
  proto::Reader reader(body, kLimits);
  proto::Field field;
  LogonResponse decoded;
  std::optional<std::int32_t> heartbeat;
  std::optional<std::int32_t> legacy_heartbeat;
  while (reader.Next(&field)) {
    if (field.number == 1 && IsVarint(field)) decoded.eresult = AsInt32(field.value);
    else if (field.number == 2 && IsVarint(field)) legacy_heartbeat = AsInt32(field.value);
    else if (field.number == 3 && IsVarint(field)) heartbeat = AsInt32(field.value);
    else if (field.number == 7 && IsVarint(field)) decoded.cell_id = static_cast<std::uint32_t>(field.value);
    else if (field.number <= 7 && field.number != 4 && field.number != 5 && field.number != 6)
      return false;  // a known field with the wrong wire type
  }
  if (!reader.ok()) return false;
  const std::int32_t seconds = heartbeat ? *heartbeat : legacy_heartbeat.value_or(0);
  decoded.heartbeat_seconds = seconds > 0 ? seconds : 9;
  *response = decoded;
  return true;
}

bool DecodeLoggedOff(const std::vector<std::uint8_t>& body, std::int32_t* eresult) {
  proto::Reader reader(body, kLimits);
  proto::Field field;
  std::int32_t value = kEResultFail;
  while (reader.Next(&field)) {
    if (field.number != 1) continue;
    if (!IsVarint(field)) return false;
    value = AsInt32(field.value);
  }
  if (!reader.ok()) return false;
  *eresult = value;
  return true;
}

bool DecodeMulti(const std::vector<std::uint8_t>& body, Multi* multi) {
  proto::Reader reader(body, kLimits);
  proto::Field field;
  Multi decoded;
  while (reader.Next(&field)) {
    if (field.number == 1) {
      if (!IsVarint(field) || field.value > kMaxFrameBytes) return false;
      decoded.size_unzipped = static_cast<std::uint32_t>(field.value);
    } else if (field.number == 2) {
      if (!IsBytes(field)) return false;
      decoded.message_body = Bytes(field);
    }
  }
  if (!reader.ok()) return false;
  *multi = std::move(decoded);
  return true;
}

bool SplitMultiPayload(const std::vector<std::uint8_t>& payload,
                       std::vector<std::vector<std::uint8_t>>* frames) {
  std::vector<std::vector<std::uint8_t>> result;
  std::size_t offset = 0;
  while (offset < payload.size()) {
    if (payload.size() - offset < 4 || result.size() >= kMaxRepeatedEntries) return false;
    const std::uint32_t size = ReadLE32(payload.data() + offset);
    offset += 4;
    if (size > payload.size() - offset) return false;
    result.emplace_back(payload.begin() + static_cast<std::ptrdiff_t>(offset),
                        payload.begin() + static_cast<std::ptrdiff_t>(offset + size));
    offset += size;
  }
  *frames = std::move(result);
  return true;
}

bool DecodeLicenseList(const std::vector<std::uint8_t>& body, LicenseList* list) {
  proto::Reader reader(body, kLimits);
  proto::Field field;
  LicenseList decoded;
  while (reader.Next(&field)) {
    if (field.number == 1) {
      if (!IsVarint(field)) return false;
      decoded.eresult = AsInt32(field.value);
      continue;
    }
    if (field.number != 2) continue;
    if (decoded.licenses.size() >= kMaxRepeatedEntries) return false;
    proto::Reader child;
    if (!reader.Submessage(field, &child)) return false;
    License license;
    proto::Field entry;
    while (child.Next(&entry)) {
      switch (entry.number) {
        case 1: if (!IsVarint(entry)) return false; license.package_id = static_cast<std::uint32_t>(entry.value); break;
        case 2: if (!IsFixed32(entry)) return false; license.time_created = static_cast<std::uint32_t>(entry.value); break;
        case 4: if (!IsVarint(entry)) return false; license.minute_limit = AsInt32(entry.value); break;
        case 5: if (!IsVarint(entry)) return false; license.minutes_used = AsInt32(entry.value); break;
        case 7: if (!IsVarint(entry)) return false; license.flags = static_cast<std::uint32_t>(entry.value); break;
        case 9: if (!IsVarint(entry)) return false; license.license_type = static_cast<std::uint32_t>(entry.value); break;
        case 12: if (!IsVarint(entry)) return false; license.owner_id = static_cast<std::uint32_t>(entry.value); break;
        case 17: if (!IsVarint(entry)) return false; license.access_token = entry.value; break;
        default: break;
      }
    }
    if (!child.ok()) return false;
    decoded.licenses.push_back(license);
  }
  if (!reader.ok()) return false;
  *list = std::move(decoded);
  return true;
}

std::vector<std::uint8_t> EncodePicsPackagesRequest(const std::vector<PackageRequest>& packages) {
  proto::Writer body(kMaxFrameBytes);
  for (const auto& package : packages) {
    proto::Writer entry;
    entry.AddVarint(1, package.package_id);
    entry.AddVarint(2, package.access_token);
    body.AddMessage(1, entry);
  }
  if (!body.ok()) return {};
  return body.bytes();
}

bool DecodePicsResponse(const std::vector<std::uint8_t>& body, PicsResponse* response) {
  proto::Reader reader(body, kLimits);
  proto::Field field;
  PicsResponse decoded;
  while (reader.Next(&field)) {
    if (field.number == 6) {
      if (!IsVarint(field)) return false;
      decoded.response_pending = field.value != 0;
      continue;
    }
    if (field.number == 1) {
      if (decoded.apps.size() >= kMaxRepeatedEntries) return false;
      proto::Reader child;
      if (!reader.Submessage(field, &child)) return false;
      PicsApp app;
      proto::Field entry;
      while (child.Next(&entry)) {
        if (entry.number == 1) {
          if (!IsVarint(entry)) return false;
          app.app_id = static_cast<std::uint32_t>(entry.value);
        } else if (entry.number == 3) {
          if (!IsVarint(entry)) return false;
          app.missing_token = entry.value != 0;
        } else if (entry.number == 5) {
          if (!IsBytes(entry)) return false;
          app.buffer = Bytes(entry);
        }
      }
      if (!child.ok()) return false;
      decoded.apps.push_back(std::move(app));
      continue;
    }
    if (field.number != 3) continue;
    if (decoded.packages.size() >= kMaxRepeatedEntries) return false;
    proto::Reader child;
    if (!reader.Submessage(field, &child)) return false;
    PicsPackage package;
    proto::Field entry;
    while (child.Next(&entry)) {
      if (entry.number == 1) {
        if (!IsVarint(entry)) return false;
        package.package_id = static_cast<std::uint32_t>(entry.value);
      } else if (entry.number == 3) {
        if (!IsVarint(entry)) return false;
        package.missing_token = entry.value != 0;
      } else if (entry.number == 5) {
        if (!IsBytes(entry)) return false;
        package.buffer = Bytes(entry);
      }
    }
    if (!child.ok()) return false;
    decoded.packages.push_back(std::move(package));
  }
  if (!reader.ok()) return false;
  *response = std::move(decoded);
  return true;
}

std::vector<std::uint8_t> EncodePicsAppsRequest(const std::vector<AppRequest>& apps) {
  proto::Writer body(kMaxFrameBytes);
  for (const auto& app : apps) {
    proto::Writer entry;
    entry.AddVarint(1, app.app_id);
    entry.AddVarint(2, app.access_token);
    body.AddMessage(2, entry);
  }
  if (!body.ok()) return {};
  return body.bytes();
}

std::vector<std::uint8_t> EncodePicsAccessTokenRequest(const std::vector<std::uint32_t>& app_ids) {
  proto::Writer body(kMaxFrameBytes);
  for (const auto id : app_ids) body.AddVarint(2, id);
  if (!body.ok()) return {};
  return body.bytes();
}

bool DecodePicsAccessTokenResponse(const std::vector<std::uint8_t>& body,
                                   std::vector<AppRequest>* tokens) {
  proto::Reader reader(body, kLimits);
  proto::Field field;
  std::vector<AppRequest> decoded;
  while (reader.Next(&field)) {
    if (field.number != 3) continue;
    if (decoded.size() >= kMaxRepeatedEntries) return false;
    proto::Reader child;
    if (!reader.Submessage(field, &child)) return false;
    AppRequest token;
    proto::Field entry;
    while (child.Next(&entry)) {
      if (entry.number == 1 && IsVarint(entry)) token.app_id = static_cast<std::uint32_t>(entry.value);
      else if (entry.number == 2 && IsVarint(entry)) token.access_token = entry.value;
      else if (entry.number == 1 || entry.number == 2) return false;
    }
    if (!child.ok()) return false;
    decoded.push_back(token);
  }
  if (!reader.ok()) return false;
  *tokens = std::move(decoded);
  return true;
}

std::vector<std::uint8_t> EncodeDepotKeyRequest(std::uint32_t depot_id, std::uint32_t app_id) {
  proto::Writer body;
  body.AddVarint(1, depot_id);
  body.AddVarint(2, app_id);
  return body.bytes();
}

bool DecodeDepotKeyResponse(const std::vector<std::uint8_t>& body, DepotKeyResponse* response) {
  proto::Reader reader(body, kLimits);
  proto::Field field;
  DepotKeyResponse decoded;
  while (reader.Next(&field)) {
    if (field.number == 1) {
      if (!IsVarint(field)) return false;
      decoded.eresult = AsInt32(field.value);
    } else if (field.number == 2) {
      if (!IsVarint(field)) return false;
      decoded.depot_id = static_cast<std::uint32_t>(field.value);
    } else if (field.number == 3) {
      if (!IsBytes(field) || field.size > 64) return false;
      decoded.key = Bytes(field);
    }
  }
  if (!reader.ok()) return false;
  *response = std::move(decoded);
  return true;
}

std::vector<std::uint8_t> EncodeManifestRequestCodeRequest(std::uint32_t app_id, std::uint32_t depot_id,
                                                           std::uint64_t manifest_id, std::string_view branch) {
  proto::Writer body;
  body.AddVarint(1, app_id);
  body.AddVarint(2, depot_id);
  body.AddVarint(3, manifest_id);
  body.AddString(4, branch.empty() ? std::string("public") : std::string(branch));
  return body.bytes();
}

bool DecodeManifestRequestCodeResponse(const std::vector<std::uint8_t>& body, std::uint64_t* code) {
  proto::Reader reader(body, kLimits);
  proto::Field field;
  std::uint64_t value = 0;
  while (reader.Next(&field)) {
    if (field.number != 1) continue;
    if (!IsVarint(field)) return false;
    value = field.value;
  }
  if (!reader.ok()) return false;
  *code = value;
  return true;
}

std::vector<std::uint8_t> EncodeFreeLicenseRequest(const std::vector<std::uint32_t>& app_ids) {
  proto::Writer body;
  for (const auto id : app_ids) body.AddVarint(2, id);
  return body.bytes();
}

namespace {

// One repeated uint32 field entry: a varint, or a packed run of them.
bool AppendUint32s(const proto::Field& field, std::vector<std::uint32_t>* out) {
  if (IsVarint(field)) {
    if (out->size() >= kMaxRepeatedEntries) return false;
    out->push_back(static_cast<std::uint32_t>(field.value));
    return true;
  }
  if (!IsBytes(field)) return false;
  std::size_t offset = 0;
  while (offset < field.size) {
    std::uint64_t value = 0;
    int shift = 0;
    for (;;) {
      if (offset >= field.size || shift > 63) return false;
      const std::uint8_t byte = field.data[offset++];
      value |= static_cast<std::uint64_t>(byte & 0x7f) << shift;
      if ((byte & 0x80) == 0) break;
      shift += 7;
    }
    if (out->size() >= kMaxRepeatedEntries) return false;
    out->push_back(static_cast<std::uint32_t>(value));
  }
  return true;
}

}  // namespace

bool DecodeFreeLicenseResponse(const std::vector<std::uint8_t>& body, FreeLicenseResponse* response) {
  proto::Reader reader(body, kLimits);
  proto::Field field;
  FreeLicenseResponse decoded;
  while (reader.Next(&field)) {
    if (field.number == 1) {
      if (!IsVarint(field)) return false;
      decoded.eresult = AsInt32(field.value);
    } else if (field.number == 2) {
      if (!AppendUint32s(field, &decoded.granted_packages)) return false;
    } else if (field.number == 3) {
      if (!AppendUint32s(field, &decoded.granted_apps)) return false;
    }
  }
  if (!reader.ok()) return false;
  *response = std::move(decoded);
  return true;
}

}  // namespace playden::steam::cm
