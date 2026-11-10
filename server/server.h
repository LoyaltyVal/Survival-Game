// Authoritative dedicated server. Clients send inputs; the server simulates
// everyone with the shared movement code and sends back what it decided.
#pragma once
#include <atomic>
#include <cstdint>
#include <deque>
#include <random>
#include <string>
#include <unordered_map>

#include "net.h"
#include "protocol.h"
#include "terrain.h"

namespace sg {

struct ServerConfig {
  uint16_t port = kDefaultPort;
  int maxPlayers = 100;
  int tickRate = 30;             // simulation ticks per second
  int snapshotRate = 20;         // snapshots per second per client
  uint32_t terrainSeed = 1337;
  float interestRadius = 300.0f; // metres; players farther away are not sent
  double timeoutSeconds = 10.0;  // drop silent clients
};

class Server {
 public:
  explicit Server(const ServerConfig& cfg);

  bool start();
  void run(const std::atomic<bool>& keepRunning);  // blocking loop
  void stop();

  // One simulation tick of `dt` seconds. Public so tests can drive it.
  void tick(double dt);

  size_t playerCount() const { return clients_.size(); }

 private:
  struct Client {
    uint32_t id = 0;
    std::string name;
    Address addr;
    PlayerState state;
    uint32_t lastProcessedInput = 0;
    std::deque<PlayerInput> pending;  // sorted by seq, not yet simulated
    double lastHeard = 0;
    float inputTokens = 0;  // anti speed-hack budget, refilled at kInputHz
  };

  void receivePackets();
  void handleHello(const Address& from, const uint8_t* data, size_t size);
  void handleInput(Client& c, const uint8_t* data, size_t size);
  void simulate(double dt);
  void sendSnapshots();
  void dropTimedOut();
  void send(const Address& to, const std::vector<uint8_t>& bytes);
  PlayerState pickSpawn();

  ServerConfig cfg_;
  Terrain terrain_;
  UdpSocket socket_;
  std::unordered_map<Address, Client, AddressHash> clients_;
  uint32_t nextId_ = 1;
  uint32_t tickCount_ = 0;
  double time_ = 0;
  double snapshotAccum_ = 0;
  std::mt19937 rng_;
};

}  // namespace sg
