#include "protocol.h"

#include <cmath>
#include <cstddef>
#include <cstring>

namespace sg {

// --- ByteWriter ----------------------------------------------------------------
void ByteWriter::u16(uint16_t v) {
  buf_.push_back(static_cast<uint8_t>(v));
  buf_.push_back(static_cast<uint8_t>(v >> 8));
}
void ByteWriter::u32(uint32_t v) {
  for (int i = 0; i < 4; ++i) buf_.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
void ByteWriter::f32(float v) {
  uint32_t bits;
  std::memcpy(&bits, &v, 4);
  u32(bits);
}
void ByteWriter::str(const std::string& s, size_t maxLen) {
  size_t n = s.size() < maxLen ? s.size() : maxLen;
  u8(static_cast<uint8_t>(n));
  buf_.insert(buf_.end(), s.begin(), s.begin() + static_cast<std::ptrdiff_t>(n));
}

// --- ByteReader ----------------------------------------------------------------
bool ByteReader::u8(uint8_t& v) {
  if (remaining() < 1) return false;
  v = data_[pos_++];
  return true;
}
bool ByteReader::u16(uint16_t& v) {
  if (remaining() < 2) return false;
  v = static_cast<uint16_t>(data_[pos_] | (data_[pos_ + 1] << 8));
  pos_ += 2;
  return true;
}
bool ByteReader::u32(uint32_t& v) {
  if (remaining() < 4) return false;
  v = 0;
  for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(data_[pos_ + i]) << (8 * i);
  pos_ += 4;
  return true;
}
bool ByteReader::f32(float& v) {
  uint32_t bits;
  if (!u32(bits)) return false;
  std::memcpy(&v, &bits, 4);
  return std::isfinite(v);
}
bool ByteReader::str(std::string& s, size_t maxLen) {
  uint8_t n;
  if (!u8(n) || n > maxLen || remaining() < n) return false;
  s.assign(reinterpret_cast<const char*>(data_ + pos_), n);
  pos_ += n;
  return true;
}

// --- helpers -----------------------------------------------------------------
static ByteWriter header(PacketType t) {
  ByteWriter w;
  w.u32(kProtocolMagic);
  w.u8(static_cast<uint8_t>(t));
  return w;
}

static bool readHeader(ByteReader& r, PacketType expected) {
  uint32_t magic;
  uint8_t type;
  return r.u32(magic) && magic == kProtocolMagic && r.u8(type) && type == static_cast<uint8_t>(expected);
}

static void writeVec3(ByteWriter& w, const Vec3& v) { w.f32(v.x); w.f32(v.y); w.f32(v.z); }
static bool readVec3(ByteReader& r, Vec3& v) { return r.f32(v.x) && r.f32(v.y) && r.f32(v.z); }

static void writeState(ByteWriter& w, const PlayerState& s) {
  writeVec3(w, s.pos);
  writeVec3(w, s.vel);
  w.f32(s.yaw);
  w.f32(s.pitch);
  w.u8(s.onGround ? 1 : 0);
}
static bool readState(ByteReader& r, PlayerState& s) {
  uint8_t g;
  if (!(readVec3(r, s.pos) && readVec3(r, s.vel) && r.f32(s.yaw) && r.f32(s.pitch) && r.u8(g))) return false;
  s.onGround = g != 0;
  return true;
}

std::optional<PacketType> peekType(const uint8_t* data, size_t size) {
  ByteReader r(data, size);
  uint32_t magic;
  uint8_t type;
  if (!r.u32(magic) || magic != kProtocolMagic || !r.u8(type)) return std::nullopt;
  if (type < static_cast<uint8_t>(PacketType::Hello) || type > static_cast<uint8_t>(PacketType::Disconnect))
    return std::nullopt;
  return static_cast<PacketType>(type);
}

// --- encode --------------------------------------------------------------------
std::vector<uint8_t> encode(const HelloPacket& p) {
  auto w = header(PacketType::Hello);
  w.u16(p.version);
  w.str(p.name, kMaxNameLength);
  return w.data();
}

std::vector<uint8_t> encode(const WelcomePacket& p) {
  auto w = header(PacketType::Welcome);
  w.u32(p.playerId);
  w.u32(p.terrainSeed);
  w.u16(p.tickRate);
  writeState(w, p.spawn);
  return w.data();
}

std::vector<uint8_t> encode(const RejectPacket& p) {
  auto w = header(PacketType::Reject);
  w.u8(static_cast<uint8_t>(p.reason));
  return w.data();
}

std::vector<uint8_t> encode(const InputPacket& p) {
  auto w = header(PacketType::Input);
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
  }
  return w.data();
}

std::vector<uint8_t> encode(const SnapshotPacket& p) {
  auto w = header(PacketType::Snapshot);
  w.u32(p.serverTick);
  w.u32(p.lastProcessedInput);
  writeState(w, p.self);
  size_t n = p.others.size() < static_cast<size_t>(kMaxSnapshotEntities) ? p.others.size() : kMaxSnapshotEntities;
  w.u16(static_cast<uint16_t>(n));
  for (size_t i = 0; i < n; ++i) {
    const auto& e = p.others[i];
    w.u32(e.id);
    writeVec3(w, e.pos);
    w.f32(e.yaw);
    w.f32(e.pitch);
  }
  return w.data();
}

std::vector<uint8_t> encodeDisconnect() { return header(PacketType::Disconnect).data(); }

// --- decode --------------------------------------------------------------------
bool decode(const uint8_t* data, size_t size, HelloPacket& out) {
  ByteReader r(data, size);
  return readHeader(r, PacketType::Hello) && r.u16(out.version) && r.str(out.name, kMaxNameLength);
}

bool decode(const uint8_t* data, size_t size, WelcomePacket& out) {
  ByteReader r(data, size);
  return readHeader(r, PacketType::Welcome) && r.u32(out.playerId) && r.u32(out.terrainSeed) &&
         r.u16(out.tickRate) && readState(r, out.spawn);
}

bool decode(const uint8_t* data, size_t size, RejectPacket& out) {
  ByteReader r(data, size);
  uint8_t reason;
  if (!(readHeader(r, PacketType::Reject) && r.u8(reason))) return false;
  out.reason = static_cast<RejectReason>(reason);
  return true;
}

bool decode(const uint8_t* data, size_t size, InputPacket& out) {
  ByteReader r(data, size);
  uint8_t n;
  if (!(readHeader(r, PacketType::Input) && r.u8(n)) || n == 0 || n > kMaxInputsPerPacket) return false;
  out.inputs.clear();
  for (uint8_t i = 0; i < n; ++i) {
    PlayerInput in;
    if (!(r.u32(in.seq) && r.f32(in.moveX) && r.f32(in.moveZ) && r.f32(in.yaw) && r.f32(in.pitch) &&
          r.u8(in.buttons)))
      return false;
    out.inputs.push_back(in);
  }
  return true;
}

bool decode(const uint8_t* data, size_t size, SnapshotPacket& out) {
  ByteReader r(data, size);
  uint16_t n;
  if (!(readHeader(r, PacketType::Snapshot) && r.u32(out.serverTick) && r.u32(out.lastProcessedInput) &&
        readState(r, out.self) && r.u16(n)) ||
      n > kMaxSnapshotEntities)
    return false;
  out.others.clear();
  for (uint16_t i = 0; i < n; ++i) {
    EntityState e;
    if (!(r.u32(e.id) && readVec3(r, e.pos) && r.f32(e.yaw) && r.f32(e.pitch))) return false;
    out.others.push_back(e);
  }
  return true;
}

}  // namespace sg
