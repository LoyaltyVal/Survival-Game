// Authoritative dedicated server. Clients send inputs and requests; the server
// simulates everyone with the shared code, applies the game rules (gathering,
// crafting, building, combat, hunger) and sends back what it decided.
#pragma once
#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <random>
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

struct ServerConfig {
  uint16_t port = kDefaultPort;
  int maxPlayers = 100;
  int tickRate = 30;             // simulation ticks per second
  int snapshotRate = 20;         // snapshots per second per client
  uint32_t terrainSeed = 1337;
  float interestRadius = 300.0f; // metres; players farther away are not sent
  double timeoutSeconds = 10.0;  // drop silent clients
  std::string name = "Survival Server";
  std::string savePath;          // empty = don't save
  double autosaveSeconds = 30.0;
  std::string ownerName;          // this player is always Owner (the person hosting from the game)
  float dayLengthMinutes = 40.0f; // one full day/night cycle
};

class Server {
 public:
  explicit Server(const ServerConfig& cfg);

  bool start();
  void run(const std::atomic<bool>& keepRunning);  // blocking loop
  void stop();                                     // saves, then says goodbye

  // One simulation tick of `dt` seconds. Public so tests can drive it.
  void tick(double dt);

  size_t playerCount() const { return clients_.size(); }
  const ServerConfig& config() const { return cfg_; }
  const World& world() const { return world_; }

  bool save() const;
  bool load();
  Rank rankOf(const std::string& name) const;
  // The player who owns this world. Safe to call from another thread (the game sets it when you join).
  void setOwnerName(const std::string& name);
  std::string ownerName() const;
  float timeOfDay() const { return timeOfDay_; }

  // --- test hooks ---------------------------------------------------------------
  PlayerState* debugState(uint32_t playerId);
  Slots* debugInventory(uint32_t playerId);
  Vitals* debugVitals(uint32_t playerId);

 private:
  struct CraftJob {
    uint16_t recipe;
    float remaining;
    float total;
  };
  struct Client {
    uint32_t id = 0;
    std::string name;
    Address addr;
    PlayerState state;
    uint32_t lastProcessedInput = 0;
    std::deque<PlayerInput> pending;  // sorted by seq, not yet simulated
    double lastHeard = 0;
    float inputTokens = 0;  // anti speed-hack budget, refilled at kInputHz
    float rtt = 0.1f;       // seconds, from snapshot acks

    ReliableChannel reliable;
    Vitals vitals;
    Slots inv = Slots(kInventorySlots);
    uint8_t slot = 0;
    double inputClock = 0;  // seconds of input simulated; cooldowns use this
    double nextUse = 0;
    double lastAttack = -1e9;
    bool sprinting = false;
    std::deque<CraftJob> crafting;
    uint32_t openContainer = 0;
    struct Sample {
      double time;
      Vec3 pos;
    };
    std::deque<Sample> history;  // for lag compensation
    bool invDirty = true, craftDirty = true;
    double lastWarn = -1e9;
    double lastChat = -1e9;
    uint8_t kick = 0;       // RejectReason to send; removed at the end of the tick
    uint32_t replyTo = 0;   // last player who /msg'd us, for /r
    bool god = false;       // /god: takes no damage, never starves
    bool primaryHeld = false;  // for semi-auto guns: one shot per click
    bool aiming = false;
    bool reloading = false;
    double reloadUntil = 0;    // input clock
    double lastCodeTry = -1e9;
  };
  struct Lock {               // code lock on a door, box or claim post
    std::string code;         // 4 digits; empty until the owner sets it
    std::unordered_set<uint32_t> authorized;  // player ids that know the code
  };
  struct Fuse {               // a planted explosive
    double explodeAt;
    Item item;
    uint32_t owner;
    double nextBeep;
  };
  struct ShotHit {
    float dist = 0;
    uint32_t playerId = 0, objectId = 0;
    bool head = false;
  };
  struct PlayerRecord {  // a player's saved state while offline
    uint32_t id = 0;
    std::string name;
    PlayerState state;
    Vitals vitals;
    Slots inv;
    std::deque<CraftJob> crafting;
  };
  struct NodeRuntime {
    float remaining = 0;
    double respawnAt = 0;  // > 0 once depleted
  };

  // networking
  void receivePackets();
  void handleHello(const Address& from, const uint8_t* data, size_t size);
  void handleInput(Client& c, const uint8_t* data, size_t size);
  void handleInfoRequest(const Address& from, const uint8_t* data, size_t size);
  void sendSnapshots();
  void flushReliable(Client& c);
  void dropTimedOut();
  void send(const Address& to, const std::vector<uint8_t>& bytes);
  void removeClient(std::unordered_map<Address, Client, AddressHash>::iterator it, const char* why);

  // reliable messages
  void handleMessage(Client& c, const std::vector<uint8_t>& msg);
  void queue(Client& c, const ByteWriter& w);
  void broadcast(const ByteWriter& w);
  void sendText(Client& c, TextKind kind, const std::string& text);
  void broadcastText(TextKind kind, const std::string& text);
  void warn(Client& c, const std::string& text);  // rate-limited
  void sendFullState(Client& c);
  void sendInventory(Client& c);
  void sendCraftQueue(Client& c);
  void sendContainer(Client& c);
  void containerChanged(uint32_t objectId);
  void objectChanged(const WorldObject& o);
  void objectRemoved(uint32_t id);

  // ranks and chat commands (commands.cpp)
  void handleCommand(Client& c, const std::string& line);
  Client* findPlayer(Client& asker, const std::string& partial);
  bool canManage(const Client& actor, const std::string& targetName) const;
  bool isBanned(const std::string& name) const;
  bool isMuted(const std::string& name) const { return mutes_.count(name) != 0; }
  ByteWriter playerInfoMsg(const Client& c) const;
  void broadcastPlayerInfo(const Client& c);
  void teleport(Client& c, const Vec3& pos);

  // gameplay
  void simulate(double dt);
  void processInput(Client& c, const PlayerInput& in);
  void primaryAction(Client& c, const PlayerInput& in, bool pressed);
  void meleeHit(Client& c, const PlayerInput& in, Item item);
  void shoot(Client& c, const PlayerInput& in, ItemStack& gun);
  ShotHit traceShot(const Client& c, const Vec3& eye, const Vec3& dir, float range, double rewindTo);
  void startReload(Client& c);
  void finishReload(Client& c);
  void soundAt(const Vec3& pos, Sound s, float radius, uint32_t exceptPlayer);
  bool canAccess(const Client& c, const WorldObject& o) const;
  bool buildBlocked(const Client& c, const Vec3& pos) const;
  void promptCode(Client& c, uint32_t objectId, uint8_t mode);
  void handleCode(Client& c, uint32_t objectId, const std::string& code);
  void doUseOn(Client& c, uint32_t objectId);
  void doAttach(Client& c, uint32_t container, int gunSlot, int modSlot);
  void doDetach(Client& c, uint32_t container, int gunSlot, int modSlot);
  void updateCharges();
  void explode(uint32_t chargeId);
  void damagePlayer(Client& victim, float amount, Client* attacker, const char* weapon, bool headshot);
  void killPlayer(Client& victim, const std::string& cause, const std::string& feed);
  void respawnPlayer(Client& c, bool atBag);
  void damageObject(uint32_t id, float amount);
  void updateVitals(Client& c, float dt);
  void updateCrafting(Client& c, float dt);
  void updateWorld();
  void give(Client& c, Item item, int count);  // overflow goes into a loot bag
  uint32_t spawnLootBag(const Vec3& pos, const Slots& items);
  Slots* containerFor(Client& c, uint32_t id);
  bool nearWorkbench(const Client& c) const;
  Vec3 positionAt(const Client& c, double time) const;
  void doCraft(Client& c, int recipe, int times);
  void doPlace(Client& c, ObjType type, const Vec3& pos, uint8_t rot, Item item);
  void doInteract(Client& c, uint8_t kind, uint32_t id);
  void storeRecord(const Client& c);
  Client* findClient(uint32_t id);
  PlayerState pickSpawn();

  ServerConfig cfg_;
  Terrain terrain_;
  UdpSocket socket_;
  std::unordered_map<Address, Client, AddressHash> clients_;
  std::unordered_map<std::string, PlayerRecord> records_;  // by name (Steam id later)
  World world_;
  std::unordered_map<uint32_t, Slots> containers_;      // storage boxes and loot bags
  std::unordered_map<uint32_t, double> bagExpiry_;      // loot bag id -> despawn time
  std::unordered_map<uint32_t, NodeRuntime> nodes_;     // only nodes that have been hit
  mutable std::mutex ownerMu_;
  std::unordered_map<uint32_t, Lock> locks_;                          // by object id
  std::unordered_map<uint32_t, std::unordered_set<uint32_t>> claims_;  // claim post -> authorized players
  std::unordered_map<uint32_t, Fuse> fuses_;                          // planted explosives
  std::unordered_map<std::string, Rank> ranks_;  // by name; missing = Player
  std::unordered_set<std::string> bans_;
  std::unordered_set<std::string> mutes_;
  float timeOfDay_ = 9.0f;                        // hours
  uint32_t nextPlayerId_ = 1;
  uint32_t nextObjectId_ = 1;
  uint32_t tickCount_ = 0;
  double time_ = 0;
  double snapshotAccum_ = 0;
  double lastWorldUpdate_ = 0;
  double lastSave_ = 0;
  std::mt19937 rng_;
};

}  // namespace sg
