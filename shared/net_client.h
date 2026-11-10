// Client side of the netcode: join handshake, input sending, prediction,
// reconciliation and interpolation of other players. No graphics here, so the
// real client and the headless test bot share it.
#pragma once
#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

#include "net.h"
#include "protocol.h"
#include "terrain.h"

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

  Status status() const { return status_; }
  uint32_t playerId() const { return playerId_; }
  const PlayerState& predicted() const { return predicted_; }
  const Terrain& terrain() const { return terrain_; }

  // Last correction size in metres (for a debug overlay; should stay ~0).
  float lastCorrection() const { return lastCorrection_; }

  struct RemotePlayer {
    uint32_t id;
    Vec3 pos;
    float yaw;
    float pitch;
  };
  // Other players interpolated at (now - kInterpDelay).
  std::vector<RemotePlayer> remotePlayers(double now) const;

 private:
  void handlePacket(const uint8_t* data, size_t size, double now);
  void onSnapshot(const SnapshotPacket& snap, double now);

  struct TimedEntity {
    double time;
    EntityState state;
  };

  UdpSocket socket_;
  Address server_;
  std::string name_;
  Status status_ = Status::Disconnected;
  double lastHello_ = -1e9;
  double lastHeard_ = 0;

  uint32_t playerId_ = 0;
  Terrain terrain_;
  PlayerState predicted_;
  uint32_t nextSeq_ = 1;
  std::deque<PlayerInput> history_;  // sent but not yet confirmed
  uint32_t lastServerTick_ = 0;
  float lastCorrection_ = 0;

  std::unordered_map<uint32_t, std::deque<TimedEntity>> remote_;
};

}  // namespace sg
