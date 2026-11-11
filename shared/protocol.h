// Wire format shared by client and server. Every packet starts with
// kProtocolMagic and a PacketType byte. All integers are little-endian.
//
// Two kinds of traffic:
//  - unreliable, sent constantly: Input (client) and Snapshot (server). Losing
//    one doesn't matter because the next one carries fresh state.
//  - reliable messages (inventory, building, crafting, chat, ...) carried in
//    Reliable packets and resent until acknowledged. See reliable.h.
#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "bytes.h"
#include "items.h"
#include "movement.h"
#include "world.h"

namespace sg {

constexpr uint32_t kProtocolMagic = 0x31304753;  // "SG01"
constexpr uint16_t kProtocolVersion = 5;
constexpr uint16_t kDefaultPort = 27015;
constexpr size_t kMaxPacketSize = 1200;  // stays under typical MTU
constexpr int kMaxInputsPerPacket = 4;   // redundancy against packet loss
constexpr int kMaxSnapshotEntities = 32;
constexpr size_t kMaxNameLength = 20;
constexpr size_t kMaxServerNameLength = 48;
constexpr size_t kMaxChatLength = 120;
constexpr size_t kMaxTextLength = 200;
constexpr size_t kInfoRequestSize = 64;  // padded so replies are never bigger than requests (no amplification)

enum class PacketType : uint8_t {
  Hello = 1,        // client -> server: join request
  Welcome = 2,      // server -> client: accepted
  Reject = 3,       // server -> client: refused
  Input = 4,        // client -> server: latest inputs
  Snapshot = 5,     // server -> client: world state
  Disconnect = 6,   // either way
  Reliable = 7,     // either way: reliable messages + ack
  InfoRequest = 8,  // anyone -> server: server browser ping
  InfoReply = 9,    // server -> anyone
  Voice = 10,       // either way: 40 ms of proximity voice chat
};
constexpr uint8_t kLastPacketType = 10;

// Proximity voice: 16 kHz mono, IMA ADPCM (see adpcm.h), one 40 ms frame per packet.
constexpr int kVoiceRate = 16000;
constexpr size_t kVoiceFrameSamples = 640;
constexpr size_t kVoiceFrameBytes = kVoiceFrameSamples / 2;
constexpr float kVoiceRange = 45.0f;  // metres; you hear people within this

enum class RejectReason : uint8_t { ServerFull = 1, BadVersion = 2, Banned = 3, NameInUse = 4, Kicked = 5 };

// Server ranks. Each rank can do everything the ranks below it can.
enum class Rank : uint8_t { Player = 0, Moderator = 1, Admin = 2, Owner = 3 };
const char* rankName(Rank r);
const char* rejectReasonText(RejectReason r);

ByteWriter packetHeader(PacketType t);
bool readPacketHeader(ByteReader& r, PacketType expected);

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
  std::string serverName;
};

struct RejectPacket {
  RejectReason reason = RejectReason::ServerFull;
};

struct InputPacket {
  uint32_t ackTick = 0;             // newest snapshot tick seen (server measures ping from it)
  std::vector<PlayerInput> inputs;  // oldest first, newest last
};

enum EntityFlags : uint8_t { kEntityDead = 1, kEntityAttacking = 2 };

struct EntityState {
  uint32_t id = 0;
  Vec3 pos;
  float yaw = 0;
  float pitch = 0;
  Item held = Item::None;
  uint8_t flags = 0;
  std::array<Item, kArmorSlots> wear{};  // clothing/armour, by ArmorSlot (so everyone sees it)
};

struct Vitals {
  float health = 100;
  float food = 100;
  float saturation = 10;
  bool dead = false;
};
constexpr float kMaxHealth = 100, kMaxFood = 100, kMaxSaturation = 20;

struct SnapshotPacket {
  uint32_t serverTick = 0;
  uint32_t lastProcessedInput = 0;  // for client reconciliation
  PlayerState self;                 // authoritative state of the receiving player
  Vitals vitals;
  float timeOfDay = 9.0f;           // hours, 0..24
  std::vector<EntityState> others;  // nearby players only
};

struct VoicePacket {
  uint32_t speaker = 0;   // filled in by the server (clients send 0)
  uint16_t seq = 0;
  int16_t predictor = 0;  // ADPCM state at the start of this frame
  uint8_t index = 0;
  std::vector<uint8_t> data;  // <= kVoiceFrameBytes
};

struct InfoRequestPacket {
  uint32_t nonce = 0;
};

struct InfoReplyPacket {
  uint32_t nonce = 0;
  std::string name;
  uint16_t players = 0;
  uint16_t maxPlayers = 0;
  uint32_t seed = 0;
  uint16_t version = kProtocolVersion;
};

std::vector<uint8_t> encode(const HelloPacket& p);
std::vector<uint8_t> encode(const WelcomePacket& p);
std::vector<uint8_t> encode(const RejectPacket& p);
std::vector<uint8_t> encode(const InputPacket& p);
std::vector<uint8_t> encode(const SnapshotPacket& p);
std::vector<uint8_t> encode(const InfoRequestPacket& p);
std::vector<uint8_t> encode(const InfoReplyPacket& p);
std::vector<uint8_t> encode(const VoicePacket& p);
std::vector<uint8_t> encodeDisconnect();

// Reads the header. Returns the packet type, or nullopt if not ours.
std::optional<PacketType> peekType(const uint8_t* data, size_t size);

// Each decoder returns false on malformed input. Never trust packet contents.
bool decode(const uint8_t* data, size_t size, HelloPacket& out);
bool decode(const uint8_t* data, size_t size, WelcomePacket& out);
bool decode(const uint8_t* data, size_t size, RejectPacket& out);
bool decode(const uint8_t* data, size_t size, InputPacket& out);
bool decode(const uint8_t* data, size_t size, SnapshotPacket& out);
bool decode(const uint8_t* data, size_t size, InfoRequestPacket& out);
bool decode(const uint8_t* data, size_t size, InfoReplyPacket& out);
bool decode(const uint8_t* data, size_t size, VoicePacket& out);

// --- reliable messages ----------------------------------------------------------
// First byte of every reliable message. Contents are written/read with
// ByteWriter/ByteReader where they are sent/handled.
enum class Msg : uint8_t {
  // server -> client
  Inventory = 1,      // slots
  CraftQueue,         // u8 n, n x (u16 recipe, f32 remaining, f32 total)
  Objects,            // u8 n, n x object  (add or update)
  ObjectRemoved,      // u32 id
  NodeState,          // u32 node id, u8 depleted
  Container,          // u32 object id, u8 objType, slots
  ContainerClosed,    //
  Text,               // u8 TextKind, str
  PlayerName,         // u32 id, str name, u8 rank (join or rank change)
  HitConfirm,         // u8 flags (1 = headshot, 2 = kill, 4 = hit armour)
  Gathered,           // u16 item, u16 count
  Died,               // str cause
  PlayerLeft,         // u32 id
  SoundAt,            // u8 Sound, vec3 position, u32 source player (0 = world)
  CodePrompt,         // u32 object id, u8 mode (0 = choose a new code, 1 = enter the code)
  // client -> server
  MoveItem = 64,      // u32 fromContainer, u8 fromSlot, u32 toContainer, u8 toSlot, u16 count (0 = all)
  Craft,              // u16 recipe, u8 times
  CancelCraft,        // u8 queue index
  Place,              // u8 objType, vec3 pos, u8 rot
  Interact,           // u8 kind (0 object, 1 node), u32 id
  CloseContainer,     //
  Respawn,            // u8 atSleepingBag
  Chat,               // str
  Drop,               // u32 container, u8 slot
  Attach,             // u32 container, u8 gun slot, u8 attachment slot (in the same container)
  Detach,             // u32 container, u8 gun slot, u8 ModSlot
  UseOn,              // u32 object id: use the held item on it (code lock)
  Code,               // u32 object id, str 4 digits (set a new code or try one)
};

enum class TextKind : uint8_t { Info = 0, Chat = 1, Kill = 2, Warning = 3, Whisper = 4, Announce = 5 };
constexpr uint8_t kLastTextKind = 5;

// Container id 0 always means "my own inventory".
constexpr uint32_t kSelfContainer = 0;

}  // namespace sg
