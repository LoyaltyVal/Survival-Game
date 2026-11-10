// Wire format shared by client and server. Every packet starts with
// kProtocolMagic and a PacketType byte. All integers are little-endian.
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "movement.h"

namespace sg {

constexpr uint32_t kProtocolMagic = 0x31304753;  // "SG01"
constexpr uint16_t kProtocolVersion = 1;
constexpr uint16_t kDefaultPort = 27015;
constexpr size_t kMaxPacketSize = 1200;  // stays under typical MTU
constexpr int kMaxInputsPerPacket = 4;   // redundancy against packet loss
constexpr int kMaxSnapshotEntities = 40;
constexpr size_t kMaxNameLength = 32;

enum class PacketType : uint8_t {
  Hello = 1,       // client -> server: join request
  Welcome = 2,     // server -> client: accepted
  Reject = 3,      // server -> client: refused
  Input = 4,       // client -> server: latest inputs
  Snapshot = 5,    // server -> client: world state
  Disconnect = 6,  // either way
};

enum class RejectReason : uint8_t { ServerFull = 1, BadVersion = 2, Banned = 3 };

// --- byte stream helpers -----------------------------------------------------
class ByteWriter {
 public:
  void u8(uint8_t v) { buf_.push_back(v); }
  void u16(uint16_t v);
  void u32(uint32_t v);
  void f32(float v);
  void str(const std::string& s, size_t maxLen);
  const std::vector<uint8_t>& data() const { return buf_; }

 private:
  std::vector<uint8_t> buf_;
};

class ByteReader {
 public:
  ByteReader(const uint8_t* data, size_t size) : data_(data), size_(size) {}
  bool u8(uint8_t& v);
  bool u16(uint16_t& v);
  bool u32(uint32_t& v);
  bool f32(float& v);  // rejects NaN/inf
  bool str(std::string& s, size_t maxLen);
  size_t remaining() const { return size_ - pos_; }

 private:
  const uint8_t* data_;
  size_t size_;
  size_t pos_ = 0;
};

// --- packets -----------------------------------------------------------------
struct HelloPacket {
  uint16_t version = kProtocolVersion;
  std::string name;
};

struct WelcomePacket {
  uint32_t playerId = 0;
  uint32_t terrainSeed = 0;
  uint16_t tickRate = 0;
  PlayerState spawn;
};

struct RejectPacket {
  RejectReason reason = RejectReason::ServerFull;
};

struct InputPacket {
  std::vector<PlayerInput> inputs;  // oldest first, newest last
};

struct EntityState {
  uint32_t id = 0;
  Vec3 pos;
  float yaw = 0;
  float pitch = 0;
};

struct SnapshotPacket {
  uint32_t serverTick = 0;
  uint32_t lastProcessedInput = 0;  // for client reconciliation
  PlayerState self;                 // authoritative state of the receiving player
  std::vector<EntityState> others;  // nearby players only
};

std::vector<uint8_t> encode(const HelloPacket& p);
std::vector<uint8_t> encode(const WelcomePacket& p);
std::vector<uint8_t> encode(const RejectPacket& p);
std::vector<uint8_t> encode(const InputPacket& p);
std::vector<uint8_t> encode(const SnapshotPacket& p);
std::vector<uint8_t> encodeDisconnect();

// Reads the header. Returns the packet type, or nullopt if not ours.
std::optional<PacketType> peekType(const uint8_t* data, size_t size);

// Each decoder returns false on malformed input. Never trust packet contents.
bool decode(const uint8_t* data, size_t size, HelloPacket& out);
bool decode(const uint8_t* data, size_t size, WelcomePacket& out);
bool decode(const uint8_t* data, size_t size, RejectPacket& out);
bool decode(const uint8_t* data, size_t size, InputPacket& out);
bool decode(const uint8_t* data, size_t size, SnapshotPacket& out);

}  // namespace sg
