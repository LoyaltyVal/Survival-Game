#include "net_client.h"

#include <cstdio>

namespace sg {

constexpr double kHelloInterval = 0.5;
constexpr double kServerTimeout = 10.0;
constexpr size_t kMaxHistory = 256;  // ~4 s of inputs at 60 Hz

bool NetClient::connect(const std::string& host, uint16_t port, const std::string& name) {
  if (!Address::parse(host, port, server_)) {
    std::fprintf(stderr, "[client] cannot resolve %s\n", host.c_str());
    return false;
  }
  if (!socket_.open(0)) return false;
  name_ = name;
  status_ = Status::Connecting;
  lastHello_ = -1e9;
  return true;
}

void NetClient::disconnect() {
  if (status_ == Status::Connected) {
    auto bye = encodeDisconnect();
    socket_.send(server_, bye.data(), bye.size());
  }
  socket_.close();
  status_ = Status::Disconnected;
  remote_.clear();
  history_.clear();
}

void NetClient::update(double now) {
  if (status_ == Status::Disconnected || status_ == Status::Rejected) return;

  if (status_ == Status::Connecting && now - lastHello_ >= kHelloInterval) {
    auto hello = encode(HelloPacket{kProtocolVersion, name_});
    socket_.send(server_, hello.data(), hello.size());
    lastHello_ = now;
  }

  uint8_t buf[kMaxPacketSize];
  Address from;
  int n;
  while ((n = socket_.receive(from, buf, sizeof(buf))) > 0) {
    if (from != server_) continue;  // ignore anyone but our server
    lastHeard_ = now;
    handlePacket(buf, static_cast<size_t>(n), now);
  }

  if (status_ == Status::Connected && now - lastHeard_ > kServerTimeout) {
    std::fprintf(stderr, "[client] server timed out\n");
    disconnect();
  }
}

void NetClient::handlePacket(const uint8_t* data, size_t size, double now) {
  auto type = peekType(data, size);
  if (!type) return;
  switch (*type) {
    case PacketType::Welcome: {
      WelcomePacket w;
      if (!decode(data, size, w) || status_ != Status::Connecting) return;
      playerId_ = w.playerId;
      terrain_ = Terrain(w.terrainSeed);
      predicted_ = w.spawn;
      status_ = Status::Connected;
      std::printf("[client] joined as id %u\n", playerId_);
      break;
    }
    case PacketType::Reject: {
      RejectPacket r;
      if (decode(data, size, r)) {
        status_ = Status::Rejected;
        std::fprintf(stderr, "[client] rejected, reason %d\n", static_cast<int>(r.reason));
      }
      break;
    }
    case PacketType::Snapshot: {
      SnapshotPacket s;
      if (decode(data, size, s) && status_ == Status::Connected) onSnapshot(s, now);
      break;
    }
    case PacketType::Disconnect:
      std::fprintf(stderr, "[client] server closed the connection\n");
      socket_.close();
      status_ = Status::Disconnected;
      break;
    default:
      break;
  }
}

void NetClient::sendInput(PlayerInput in) {
  if (status_ != Status::Connected) return;
  in.seq = nextSeq_++;
  in = sanitizeInput(in);

  // Predict: move right away instead of waiting a round trip.
  applyInput(predicted_, in, terrain_);
  history_.push_back(in);
  while (history_.size() > kMaxHistory) history_.pop_front();

  // Send the newest few inputs; if one packet is lost the next carries it.
  InputPacket p;
  size_t start = history_.size() > static_cast<size_t>(kMaxInputsPerPacket) ? history_.size() - kMaxInputsPerPacket : 0;
  p.inputs.assign(history_.begin() + static_cast<std::ptrdiff_t>(start), history_.end());
  auto bytes = encode(p);
  socket_.send(server_, bytes.data(), bytes.size());
}

void NetClient::onSnapshot(const SnapshotPacket& snap, double now) {
  if (snap.serverTick <= lastServerTick_) return;  // out of order or duplicate
  lastServerTick_ = snap.serverTick;

  // Reconcile: start from the server's truth, then replay inputs it hasn't
  // processed yet. If prediction was right, nothing visibly changes.
  Vec3 before = predicted_.pos;
  predicted_ = snap.self;
  while (!history_.empty() && history_.front().seq <= snap.lastProcessedInput) history_.pop_front();
  for (const auto& in : history_) applyInput(predicted_, in, terrain_);
  lastCorrection_ = length(predicted_.pos - before);

  // Buffer other players for interpolation.
  for (const auto& e : snap.others) {
    auto& buf = remote_[e.id];
    buf.push_back({now, e});
    while (buf.size() > 32) buf.pop_front();
  }
  // Forget players no longer in range.
  for (auto it = remote_.begin(); it != remote_.end();) {
    if (now - it->second.back().time > 1.0) it = remote_.erase(it);
    else ++it;
  }
}

std::vector<NetClient::RemotePlayer> NetClient::remotePlayers(double now) const {
  std::vector<RemotePlayer> out;
  const double t = now - kInterpDelay;
  for (const auto& [id, buf] : remote_) {
    if (buf.empty()) continue;
    const TimedEntity* a = &buf.front();
    const TimedEntity* b = &buf.front();
    for (size_t i = 1; i < buf.size(); ++i) {
      if (buf[i].time >= t) { a = &buf[i - 1]; b = &buf[i]; break; }
      a = b = &buf[i];  // t is past every sample: hold the newest
    }
    float alpha = 1.0f;
    if (b->time > a->time) alpha = clampf(static_cast<float>((t - a->time) / (b->time - a->time)), 0.0f, 1.0f);
    out.push_back({id, lerp(a->state.pos, b->state.pos, alpha), lerpAngle(a->state.yaw, b->state.yaw, alpha),
                   lerpf(a->state.pitch, b->state.pitch, alpha)});
  }
  return out;
}

}  // namespace sg
