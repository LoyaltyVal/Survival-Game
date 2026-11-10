#include "server.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>

namespace sg {

// Burst allowance: how many inputs a client may have "banked" (100 ms worth).
constexpr float kMaxInputTokens = 6.0f;
constexpr size_t kMaxPendingInputs = 16;

Server::Server(const ServerConfig& cfg) : cfg_(cfg), terrain_(cfg.terrainSeed), rng_(cfg.terrainSeed) {}

bool Server::start() {
  if (!socket_.open(cfg_.port)) {
    std::fprintf(stderr, "[server] could not bind UDP port %u\n", cfg_.port);
    return false;
  }
  std::printf("[server] listening on UDP %u, %d Hz tick, max %d players, seed %u\n", cfg_.port, cfg_.tickRate,
              cfg_.maxPlayers, cfg_.terrainSeed);
  return true;
}

void Server::stop() {
  auto bye = encodeDisconnect();
  for (auto& [addr, c] : clients_) send(addr, bye);
  clients_.clear();
  socket_.close();
}

void Server::run(const std::atomic<bool>& keepRunning) {
  using clock = std::chrono::steady_clock;
  const auto step = std::chrono::duration<double>(1.0 / cfg_.tickRate);
  auto next = clock::now();
  while (keepRunning) {
    tick(step.count());
    next += std::chrono::duration_cast<clock::duration>(step);
    auto now = clock::now();
    if (next > now) {
      std::this_thread::sleep_until(next);
    } else if (now - next > std::chrono::seconds(1)) {
      std::fprintf(stderr, "[server] running behind by over 1 s, skipping ahead\n");
      next = now;
    }
  }
}

void Server::tick(double dt) {
  time_ += dt;
  ++tickCount_;
  receivePackets();
  simulate(dt);
  snapshotAccum_ += dt;
  const double snapshotInterval = 1.0 / cfg_.snapshotRate;
  if (snapshotAccum_ >= snapshotInterval) {
    snapshotAccum_ = std::fmod(snapshotAccum_, snapshotInterval);
    sendSnapshots();
  }
  dropTimedOut();
}

void Server::send(const Address& to, const std::vector<uint8_t>& bytes) {
  socket_.send(to, bytes.data(), bytes.size());
}

void Server::receivePackets() {
  uint8_t buf[kMaxPacketSize];
  Address from;
  int n;
  while ((n = socket_.receive(from, buf, sizeof(buf))) > 0) {
    auto type = peekType(buf, static_cast<size_t>(n));
    if (!type) continue;  // not our protocol: ignore silently

    auto it = clients_.find(from);
    if (*type == PacketType::Hello) {
      handleHello(from, buf, static_cast<size_t>(n));
      continue;
    }
    if (it == clients_.end()) continue;  // unknown sender
    it->second.lastHeard = time_;

    if (*type == PacketType::Input) {
      handleInput(it->second, buf, static_cast<size_t>(n));
    } else if (*type == PacketType::Disconnect) {
      std::printf("[server] %s (id %u) left\n", it->second.name.c_str(), it->second.id);
      clients_.erase(it);
    }
  }
}

void Server::handleHello(const Address& from, const uint8_t* data, size_t size) {
  HelloPacket hello;
  if (!decode(data, size, hello)) return;

  auto existing = clients_.find(from);
  if (existing != clients_.end()) {
    // Our Welcome was probably lost; send it again.
    WelcomePacket w{existing->second.id, cfg_.terrainSeed, static_cast<uint16_t>(cfg_.tickRate),
                    existing->second.state};
    send(from, encode(w));
    return;
  }
  if (hello.version != kProtocolVersion) {
    send(from, encode(RejectPacket{RejectReason::BadVersion}));
    return;
  }
  if (static_cast<int>(clients_.size()) >= cfg_.maxPlayers) {
    send(from, encode(RejectPacket{RejectReason::ServerFull}));
    return;
  }
  // TODO(phase 5): verify a Steam auth session ticket here instead of trusting the name.

  Client c;
  c.id = nextId_++;
  c.name = hello.name.empty() ? "Survivor" : hello.name;
  c.addr = from;
  c.state = pickSpawn();
  c.lastHeard = time_;
  c.inputTokens = kMaxInputTokens;
  WelcomePacket w{c.id, cfg_.terrainSeed, static_cast<uint16_t>(cfg_.tickRate), c.state};
  std::printf("[server] %s joined from %s as id %u at (%.0f, %.0f)\n", c.name.c_str(), from.toString().c_str(),
              c.id, c.state.pos.x, c.state.pos.z);
  clients_.emplace(from, std::move(c));
  send(from, encode(w));
}

void Server::handleInput(Client& c, const uint8_t* data, size_t size) {
  InputPacket p;
  if (!decode(data, size, p)) return;
  for (const auto& in : p.inputs) {
    if (in.seq <= c.lastProcessedInput) continue;  // already simulated (redundant copy)
    // Insert in seq order, skipping duplicates.
    auto pos = std::lower_bound(c.pending.begin(), c.pending.end(), in.seq,
                                [](const PlayerInput& a, uint32_t s) { return a.seq < s; });
    if (pos != c.pending.end() && pos->seq == in.seq) continue;
    c.pending.insert(pos, in);
  }
  // A client flooding inputs only fills its own queue; the oldest are dropped.
  while (c.pending.size() > kMaxPendingInputs) c.pending.pop_front();
}

void Server::simulate(double dt) {
  for (auto& [addr, c] : clients_) {
    // Refill the budget at the real input rate. A speed hack that sends inputs
    // faster than 60 Hz runs out of tokens and its extra inputs wait or drop.
    c.inputTokens = std::min(kMaxInputTokens, c.inputTokens + static_cast<float>(dt * kInputHz));
    while (!c.pending.empty() && c.inputTokens >= 1.0f) {
      PlayerInput in = c.pending.front();
      c.pending.pop_front();
      applyInput(c.state, in, terrain_);  // sanitizes internally
      c.lastProcessedInput = in.seq;
      c.inputTokens -= 1.0f;
    }
  }
}

void Server::sendSnapshots() {
  const float r2 = cfg_.interestRadius * cfg_.interestRadius;
  std::vector<std::pair<float, const Client*>> nearby;
  for (auto& [addr, c] : clients_) {
    nearby.clear();
    for (auto& [otherAddr, o] : clients_) {
      if (o.id == c.id) continue;
      Vec3 d = o.state.pos - c.state.pos;
      float dist2 = d.x * d.x + d.z * d.z;
      if (dist2 <= r2) nearby.push_back({dist2, &o});
    }
    // Closest first, so the packet cap cuts the farthest players.
    std::sort(nearby.begin(), nearby.end(), [](auto& a, auto& b) { return a.first < b.first; });

    SnapshotPacket snap;
    snap.serverTick = tickCount_;
    snap.lastProcessedInput = c.lastProcessedInput;
    snap.self = c.state;
    for (size_t i = 0; i < nearby.size() && i < static_cast<size_t>(kMaxSnapshotEntities); ++i) {
      const Client* o = nearby[i].second;
      snap.others.push_back({o->id, o->state.pos, o->state.yaw, o->state.pitch});
    }
    send(addr, encode(snap));
  }
}

void Server::dropTimedOut() {
  for (auto it = clients_.begin(); it != clients_.end();) {
    if (time_ - it->second.lastHeard > cfg_.timeoutSeconds) {
      std::printf("[server] %s (id %u) timed out\n", it->second.name.c_str(), it->second.id);
      it = clients_.erase(it);
    } else {
      ++it;
    }
  }
}

PlayerState Server::pickSpawn() {
  // Random beach: start at the map edge on a random bearing and walk inward
  // until we reach dry land just above the sea.
  std::uniform_real_distribution<float> angle(0.0f, 2.0f * kPi);
  const float c = kWorldSize * 0.5f;
  for (int attempt = 0; attempt < 32; ++attempt) {
    float a = angle(rng_);
    Vec3 dir{std::cos(a), 0, std::sin(a)};
    for (float r = c * 0.98f; r > 0; r -= 4.0f) {
      float x = c + dir.x * r, z = c + dir.z * r;
      float h = terrain_.heightAt(x, z);
      if (h > 0.5f) {
        PlayerState s;
        s.pos = {x, h, z};
        s.yaw = kPi * 0.5f - a;  // face inland, toward the centre
        s.onGround = true;
        return s;
      }
    }
  }
  PlayerState s;  // fallback: map centre
  s.pos = {c, terrain_.heightAt(c, c), c};
  s.onGround = true;
  return s;
}

}  // namespace sg
