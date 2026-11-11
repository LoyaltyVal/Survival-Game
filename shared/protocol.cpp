#include "protocol.h"

#include <algorithm>
#include <cstddef>

#include <cmath>
#include <cstddef>

namespace sg {

const char* rejectReasonText(RejectReason r) {
  switch (r) {
    case RejectReason::ServerFull: return "Server is full";
    case RejectReason::BadVersion: return "Game version doesn't match the server";
    case RejectReason::Banned: return "You are banned from this server";
    case RejectReason::NameInUse: return "Someone with that name is already playing";
    case RejectReason::Kicked: return "You were kicked from the server";
  }
  return "Rejected";
}

const char* rankName(Rank r) {
  switch (r) {
    case Rank::Player: return "Player";
    case Rank::Moderator: return "Moderator";
    case Rank::Admin: return "Admin";
    case Rank::Owner: return "Owner";
  }
  return "Player";
}

ByteWriter packetHeader(PacketType t) {
  ByteWriter w;
  w.u32(kProtocolMagic);
  w.u8(static_cast<uint8_t>(t));
  return w;
}

bool readPacketHeader(ByteReader& r, PacketType expected) {
  uint32_t magic;
  uint8_t type;
  return r.u32(magic) && magic == kProtocolMagic && r.u8(type) && type == static_cast<uint8_t>(expected);
}

static void writeState(ByteWriter& w, const PlayerState& s) {
  w.vec3(s.pos);
  w.vec3(s.vel);
  w.f32(s.yaw);
  w.f32(s.pitch);
  w.f32(s.stamina);
  w.f32(s.staminaDelay);
  w.u8(static_cast<uint8_t>((s.onGround ? 1 : 0) | (s.exhausted ? 2 : 0) | (s.starving ? 4 : 0)));
}
static bool readState(ByteReader& r, PlayerState& s) {
  uint8_t f;
  if (!(r.vec3(s.pos) && r.vec3(s.vel) && r.f32(s.yaw) && r.f32(s.pitch) && r.f32(s.stamina) &&
        r.f32(s.staminaDelay) && r.u8(f)))
    return false;
  s.onGround = f & 1;
  s.exhausted = (f & 2) != 0;
  s.starving = (f & 4) != 0;
  return true;
}

std::optional<PacketType> peekType(const uint8_t* data, size_t size) {
  ByteReader r(data, size);
  uint32_t magic;
  uint8_t type;
  if (!r.u32(magic) || magic != kProtocolMagic || !r.u8(type)) return std::nullopt;
  if (type < static_cast<uint8_t>(PacketType::Hello) || type > kLastPacketType) return std::nullopt;
  return static_cast<PacketType>(type);
}

// --- encode --------------------------------------------------------------------
std::vector<uint8_t> encode(const HelloPacket& p) {
  auto w = packetHeader(PacketType::Hello);
  w.u16(p.version);
  w.str(p.name, kMaxNameLength);
  return w.data();
}

std::vector<uint8_t> encode(const WelcomePacket& p) {
  auto w = packetHeader(PacketType::Welcome);
  w.u32(p.playerId);
  w.u32(p.terrainSeed);
  w.u16(p.tickRate);
  writeState(w, p.spawn);
  w.str(p.serverName, kMaxServerNameLength);
  return w.data();
}

std::vector<uint8_t> encode(const RejectPacket& p) {
  auto w = packetHeader(PacketType::Reject);
  w.u8(static_cast<uint8_t>(p.reason));
  return w.data();
}

std::vector<uint8_t> encode(const InputPacket& p) {
  auto w = packetHeader(PacketType::Input);
  w.u32(p.ackTick);
  size_t n = p.inputs.size() < static_cast<size_t>(kMaxInputsPerPacket) ? p.inputs.size() : kMaxInputsPerPacket;
  size_t first = p.inputs.size() - n;  // keep the newest
  w.u8(static_cast<uint8_t>(n));
  for (size_t i = first; i < p.inputs.size(); ++i) {
    const auto& in = p.inputs[i];
    w.u32(in.seq);
    w.f32(in.moveX);
    w.f32(in.moveZ);
    w.f32(in.yaw);
    w.f32(in.pitch);
    w.u8(in.buttons);
    w.u8(in.slot);
  }
  return w.data();
}

std::vector<uint8_t> encode(const SnapshotPacket& p) {
  auto w = packetHeader(PacketType::Snapshot);
  w.u32(p.serverTick);
  w.u32(p.lastProcessedInput);
  writeState(w, p.self);
  w.f32(p.vitals.health);
  w.f32(p.vitals.food);
  w.f32(p.vitals.saturation);
  w.u8(p.vitals.dead ? 1 : 0);
  w.f32(p.timeOfDay);
  size_t n = p.others.size() < static_cast<size_t>(kMaxSnapshotEntities) ? p.others.size() : kMaxSnapshotEntities;
  w.u16(static_cast<uint16_t>(n));
  for (size_t i = 0; i < n; ++i) {
    const auto& e = p.others[i];
    w.u32(e.id);
    w.vec3(e.pos);
    w.f32(e.yaw);
    w.f32(e.pitch);
    w.u16(static_cast<uint16_t>(e.held));
    w.u8(e.flags);
    for (Item a : e.wear) w.u8(armorCode(a));
  }
  return w.data();
}

std::vector<uint8_t> encode(const InfoRequestPacket& p) {
  auto w = packetHeader(PacketType::InfoRequest);
  w.u32(p.nonce);
  while (w.size() < kInfoRequestSize) w.u8(0);
  return w.data();
}

std::vector<uint8_t> encode(const InfoReplyPacket& p) {
  auto w = packetHeader(PacketType::InfoReply);
  w.u32(p.nonce);
  w.str(p.name, kMaxServerNameLength);
  w.u16(p.players);
  w.u16(p.maxPlayers);
  w.u32(p.seed);
  w.u16(p.version);
  return w.data();
}

std::vector<uint8_t> encodeDisconnect() { return packetHeader(PacketType::Disconnect).data(); }

// --- decode --------------------------------------------------------------------
bool decode(const uint8_t* data, size_t size, HelloPacket& out) {
  ByteReader r(data, size);
  return readPacketHeader(r, PacketType::Hello) && r.u16(out.version) && r.str(out.name, kMaxNameLength);
}

bool decode(const uint8_t* data, size_t size, WelcomePacket& out) {
  ByteReader r(data, size);
  return readPacketHeader(r, PacketType::Welcome) && r.u32(out.playerId) && r.u32(out.terrainSeed) &&
         r.u16(out.tickRate) && readState(r, out.spawn) && r.str(out.serverName, kMaxServerNameLength);
}

bool decode(const uint8_t* data, size_t size, RejectPacket& out) {
  ByteReader r(data, size);
  uint8_t reason;
  if (!(readPacketHeader(r, PacketType::Reject) && r.u8(reason))) return false;
  out.reason = static_cast<RejectReason>(reason);
  return true;
}

bool decode(const uint8_t* data, size_t size, InputPacket& out) {
  ByteReader r(data, size);
  uint8_t n;
  if (!(readPacketHeader(r, PacketType::Input) && r.u32(out.ackTick) && r.u8(n)) || n == 0 ||
      n > kMaxInputsPerPacket)
    return false;
  out.inputs.clear();
  for (uint8_t i = 0; i < n; ++i) {
    PlayerInput in;
    if (!(r.u32(in.seq) && r.f32(in.moveX) && r.f32(in.moveZ) && r.f32(in.yaw) && r.f32(in.pitch) &&
          r.u8(in.buttons) && r.u8(in.slot)))
      return false;
    out.inputs.push_back(in);
  }
  return true;
}

bool decode(const uint8_t* data, size_t size, SnapshotPacket& out) {
  ByteReader r(data, size);
  uint16_t n;
  uint8_t dead;
  if (!(readPacketHeader(r, PacketType::Snapshot) && r.u32(out.serverTick) && r.u32(out.lastProcessedInput) &&
        readState(r, out.self) && r.f32(out.vitals.health) && r.f32(out.vitals.food) &&
        r.f32(out.vitals.saturation) && r.u8(dead) && r.f32(out.timeOfDay) && r.u16(n)) ||
      n > kMaxSnapshotEntities)
    return false;
  out.vitals.dead = dead != 0;
  out.others.clear();
  for (uint16_t i = 0; i < n; ++i) {
    EntityState e;
    uint16_t held;
    if (!(r.u32(e.id) && r.vec3(e.pos) && r.f32(e.yaw) && r.f32(e.pitch) && r.u16(held) && r.u8(e.flags)) ||
        !validItem(held))
      return false;
    e.held = static_cast<Item>(held);
    for (auto& a : e.wear) {
      uint8_t code;
      if (!r.u8(code)) return false;
      a = armorFromCode(code);
    }
    out.others.push_back(e);
  }
  return true;
}

bool decode(const uint8_t* data, size_t size, InfoRequestPacket& out) {
  ByteReader r(data, size);
  return size >= kInfoRequestSize && readPacketHeader(r, PacketType::InfoRequest) && r.u32(out.nonce);
}

bool decode(const uint8_t* data, size_t size, InfoReplyPacket& out) {
  ByteReader r(data, size);
  return readPacketHeader(r, PacketType::InfoReply) && r.u32(out.nonce) && r.str(out.name, kMaxServerNameLength) &&
         r.u16(out.players) && r.u16(out.maxPlayers) && r.u32(out.seed) && r.u16(out.version);
}

std::vector<uint8_t> encode(const VoicePacket& p) {
  auto w = packetHeader(PacketType::Voice);
  w.u32(p.speaker);
  w.u16(p.seq);
  w.u16(static_cast<uint16_t>(p.predictor));
  w.u8(p.index);
  size_t n = std::min(p.data.size(), kVoiceFrameBytes);
  w.u16(static_cast<uint16_t>(n));
  w.data().insert(w.data().end(), p.data.begin(), p.data.begin() + static_cast<std::ptrdiff_t>(n));
  return w.data();
}

bool decode(const uint8_t* data, size_t size, VoicePacket& out) {
  ByteReader r(data, size);
  uint16_t pred, n;
  if (!(readPacketHeader(r, PacketType::Voice) && r.u32(out.speaker) && r.u16(out.seq) && r.u16(pred) && r.u8(out.index) &&
        r.u16(n)))
    return false;
  if (out.index > 88 || n == 0 || n > kVoiceFrameBytes || n != r.remaining()) return false;
  out.predictor = static_cast<int16_t>(pred);
  return r.bytes(out.data, n);
}

}  // namespace sg
