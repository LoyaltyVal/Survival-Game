// Client side of the netcode: join handshake, input sending, prediction,
// reconciliation, interpolation of other players, and the reliable message
// channel that carries inventory, building and crafting. No graphics here, so
// the real client and the headless test bot share it.
#pragma once
#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "net.h"
#include "protocol.h"
#include "reliable.h"
#include "terrain.h"
#include "world.h"

namespace sg {

class NetClient {
 public:
  enum class Status { Disconnected, Connecting, Connected, Rejected };

  // How far in the past other players are drawn, so there are always two
  // snapshots to interpolate between.
  static constexpr double kInterpDelay = 0.1;

  bool connect(const std::string& host, uint16_t port, const std::string& name);
  void disconnect();

  // Call every frame with the current time in seconds.
  void update(double now);

  // Call at exactly kInputHz. Predicts locally and sends to the server.
  void sendInput(PlayerInput in);

  // --- reliable actions --------------------------------------------------------
  void moveItem(uint32_t fromContainer, int fromSlot, uint32_t toContainer, int toSlot, int count = 0);
  void dropItem(uint32_t container, int slot);
  void craft(int recipe, int times);
  void cancelCraft(int queueIndex);
  void place(ObjType type, const Vec3& pos, uint8_t rot, Item item);
  void attach(uint32_t container, int gunSlot, int attachmentSlot);
  void detach(uint32_t container, int gunSlot, ModSlot slot);
  void useOn(uint32_t objectId);                  // use the held item (code lock) on an object
  void enterCode(uint32_t objectId, const std::string& code);
  void interactObject(uint32_t objectId);
  void interactNode(uint32_t nodeId);
  void closeContainer();
  void respawn(bool atSleepingBag);
  void chat(const std::string& text);

  // --- state -------------------------------------------------------------------
  Status status() const { return status_; }
  RejectReason rejectReason() const { return rejectReason_; }
  uint32_t playerId() const { return playerId_; }
  const std::string& serverName() const { return serverName_; }
  const PlayerState& predicted() const { return predicted_; }
  const Terrain& terrain() const { return terrain_; }
  const Vitals& vitals() const { return vitals_; }
  const Slots& inventory() const { return inventory_; }
  const World& world() const { return world_; }
  const std::unordered_set<uint32_t>& depletedNodes() const { return depleted_; }
  uint32_t nodesVersion() const { return nodesVersion_; }
  float pingMs() const { return pingMs_; }

  struct CraftJob {
    uint16_t recipe;
    float remaining;
    float total;
  };
  const std::vector<CraftJob>& craftQueue() const { return craftQueue_; }
  double craftQueueTime() const { return craftQueueTime_; }  // when craftQueue() was received

  // Open storage box / loot bag (id 0 = none).
  uint32_t openContainer() const { return containerId_; }
  ObjType openContainerType() const { return containerType_; }
  const Slots& containerSlots() const { return containerSlots_; }

  struct Notice {
    std::string text;
    TextKind kind;
    double time;
  };
  const std::deque<Notice>& notices() const { return notices_; }
  struct Gathered {
    Item item;
    int count;
    double time;
  };
  const std::deque<Gathered>& gathered() const { return gathered_; }
  double lastHitTime() const { return lastHitTime_; }
  uint8_t lastHitFlags() const { return lastHitFlags_; }
  const std::string& deathCause() const { return deathCause_; }
  std::string nameOf(uint32_t id) const;
  Rank rankOf(uint32_t id) const;
  Rank myRank() const { return rankOf(playerId_); }
  float timeOfDay() const { return timeOfDay_; }  // hours, 0..24

  // Sounds the server asked us to play (other players' shots, hits, doors...).
  struct SoundEvent {
    Sound sound;
    Vec3 pos;
    bool positional;  // false: play flat (our own hit markers, gather thuds)
  };
  std::vector<SoundEvent> takeSounds() {
    std::vector<SoundEvent> out;
    out.swap(sounds_);
    return out;
  }
  uint32_t inventoryVersion() const { return inventoryVersion_; }  // bumps on every inventory update
  // A lock asked for a code: object id and mode (0 = choose, 1 = enter). Cleared when taken.
  bool takeCodePrompt(uint32_t& objectId, uint8_t& mode) {
    if (!codePromptObject_) return false;
    objectId = codePromptObject_;
    mode = codePromptMode_;
    codePromptObject_ = 0;
    return true;
  }
  struct PlayerEntry {
    std::string name;
    Rank rank = Rank::Player;
  };
  const std::unordered_map<uint32_t, PlayerEntry>& players() const { return players_; }

  // Last correction size in metres (for a debug overlay; should stay ~0).
  float lastCorrection() const { return lastCorrection_; }

  struct RemotePlayer {
    uint32_t id;
    Vec3 pos;
    float yaw;
    float pitch;
    Item held;
    uint8_t flags;
    std::array<Item, kArmorSlots> wear;
  };
  // Proximity voice: send 40 ms of microphone audio (kVoiceFrameSamples at kVoiceRate).
  void sendVoice(const int16_t* pcm, size_t samples);
  struct VoiceChunk {
    uint32_t speaker;
    std::vector<int16_t> pcm;
  };
  // Voice received since the last call (from players near you).
  std::vector<VoiceChunk> takeVoice();

  // Other players interpolated at (now - kInterpDelay).
  std::vector<RemotePlayer> remotePlayers(double now) const;

 private:
  void handlePacket(const uint8_t* data, size_t size, double now);
  void onSnapshot(const SnapshotPacket& snap, double now);
  void onMessage(const std::vector<uint8_t>& msg, double now);
  void sendReliable(ByteWriter& w);
  void notice(std::string text, TextKind kind, double now);

  struct TimedEntity {
    double time;
    EntityState state;
  };

  UdpSocket socket_;
  Address server_;
  std::string name_;
  Status status_ = Status::Disconnected;
  RejectReason rejectReason_ = RejectReason::ServerFull;
  double lastHello_ = -1e9;
  double lastHeard_ = 0;
  double lastReliableSend_ = -1e9;
  double now_ = 0;

  uint32_t playerId_ = 0;
  std::string serverName_;
  Terrain terrain_;
  PlayerState predicted_;
  uint32_t nextSeq_ = 1;
  std::deque<PlayerInput> history_;  // sent but not yet confirmed
  uint32_t lastServerTick_ = 0;
  float lastCorrection_ = 0;
  std::unordered_map<uint32_t, std::deque<TimedEntity>> remote_;

  // Ping: when each snapshot tick arrived; the server reports nothing back, so
  // we time Reliable round trips instead.
  float pingMs_ = 0;
  double reliableSentAt_ = 0;
  size_t reliableWaiting_ = 0;

  ReliableChannel reliable_;
  Vitals vitals_;
  Slots inventory_ = Slots(kPlayerSlots);
  std::vector<VoiceChunk> voiceIn_;
  uint16_t voiceSeq_ = 0;
  int16_t voicePred_ = 0;
  uint8_t voiceIndex_ = 0;
  World world_;
  std::unordered_set<uint32_t> depleted_;
  uint32_t nodesVersion_ = 1;
  std::vector<CraftJob> craftQueue_;
  double craftQueueTime_ = 0;
  uint32_t containerId_ = 0;
  ObjType containerType_ = ObjType::None;
  Slots containerSlots_;
  std::deque<Notice> notices_;
  std::deque<Gathered> gathered_;
  double lastHitTime_ = -1e9;
  uint8_t lastHitFlags_ = 0;
  std::string deathCause_;
  std::unordered_map<uint32_t, PlayerEntry> players_;
  float timeOfDay_ = 9.0f;
  std::vector<SoundEvent> sounds_;
  uint32_t inventoryVersion_ = 0;
  uint32_t codePromptObject_ = 0;
  uint8_t codePromptMode_ = 0;
};

}  // namespace sg
