#include "net_client.h"

#include <cstdio>

namespace sg {

constexpr double kHelloInterval = 0.5;
constexpr double kServerTimeout = 10.0;
constexpr double kReliableInterval = 1.0 / 30.0;
constexpr size_t kMaxHistory = 256;  // ~4 s of inputs at 60 Hz
constexpr size_t kMaxNotices = 40;

bool NetClient::connect(const std::string& host, uint16_t port, const std::string& name) {
  if (!Address::parse(host, port, server_)) {
    std::fprintf(stderr, "[client] cannot resolve %s\n", host.c_str());
    return false;
  }
  if (!socket_.open(0)) return false;
  name_ = name;
  status_ = Status::Connecting;
  lastHello_ = -1e9;
  notices_.clear();
  deathCause_.clear();
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
  reliable_.reset();
  world_.clear();
  depleted_.clear();
  ++nodesVersion_;
  inventory_.assign(kInventorySlots, {});
  craftQueue_.clear();
  containerId_ = 0;
  containerSlots_.clear();
  gathered_.clear();
  players_.clear();
  lastServerTick_ = 0;
  nextSeq_ = 1;
}

void NetClient::update(double now) {
  now_ = now;
  if (status_ == Status::Disconnected || status_ == Status::Rejected) return;

  if (status_ == Status::Connecting && now - lastHello_ >= kHelloInterval) {
    auto hello = encode(HelloPacket{kProtocolVersion, name_});
    socket_.send(server_, hello.data(), hello.size());
    lastHello_ = now;
    lastHeard_ = now;
  }

  uint8_t buf[kMaxPacketSize];
  Address from;
  int n;
  while ((n = socket_.receive(from, buf, sizeof(buf))) > 0) {
    if (from != server_) continue;  // ignore anyone but our server
    lastHeard_ = now;
    handlePacket(buf, static_cast<size_t>(n), now);
    if (status_ == Status::Disconnected) return;
  }

  if (status_ == Status::Connected) {
    if (reliable_.needsSend() && now - lastReliableSend_ >= kReliableInterval) {
      if (reliableWaiting_ == 0 && reliable_.unackedCount() > 0) reliableSentAt_ = now;
      reliableWaiting_ = reliable_.unackedCount();
      for (auto& p : reliable_.build(2)) socket_.send(server_, p.data(), p.size());
      lastReliableSend_ = now;
    }
    if (now - lastHeard_ > kServerTimeout) {
      std::fprintf(stderr, "[client] server timed out\n");
      notice("Lost connection to the server", TextKind::Warning, now);
      disconnect();
    }
  } else if (status_ == Status::Connecting && now - lastHeard_ > kServerTimeout) {
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
      serverName_ = w.serverName;
      status_ = Status::Connected;
      std::printf("[client] joined %s as id %u\n", serverName_.c_str(), playerId_);
      break;
    }
    case PacketType::Reject: {  // refused at the door, or kicked/banned while playing
      RejectPacket r;
      if (decode(data, size, r)) {
        if (status_ == Status::Connected) socket_.close();
        status_ = Status::Rejected;
        rejectReason_ = r.reason;
        std::fprintf(stderr, "[client] rejected: %s\n", rejectReasonText(r.reason));
      }
      break;
    }
    case PacketType::Snapshot: {
      SnapshotPacket s;
      if (decode(data, size, s) && status_ == Status::Connected) onSnapshot(s, now);
      break;
    }
    case PacketType::Reliable: {
      if (status_ != Status::Connected) return;
      std::vector<std::vector<uint8_t>> msgs;
      if (!reliable_.receive(data, size, msgs)) return;
      if (reliableWaiting_ > 0 && reliable_.unackedCount() == 0) {
        float sample = static_cast<float>((now - reliableSentAt_) * 1000.0);
        pingMs_ = pingMs_ <= 0 ? sample : pingMs_ * 0.7f + sample * 0.3f;
        reliableWaiting_ = 0;
      }
      for (const auto& m : msgs) onMessage(m, now);
      break;
    }
    case PacketType::Disconnect:
      std::fprintf(stderr, "[client] server closed the connection\n");
      notice("Server closed the connection", TextKind::Warning, now);
      socket_.close();
      status_ = Status::Disconnected;
      break;
    default:
      break;
  }
}

void NetClient::notice(std::string text, TextKind kind, double now) {
  notices_.push_back({std::move(text), kind, now});
  while (notices_.size() > kMaxNotices) notices_.pop_front();
}

void NetClient::onMessage(const std::vector<uint8_t>& msg, double now) {
  ByteReader r(msg.data(), msg.size());
  uint8_t raw;
  if (!r.u8(raw)) return;
  switch (static_cast<Msg>(raw)) {
    case Msg::Inventory: {
      Slots s;
      if (readSlots(r, s) && s.size() == kInventorySlots) inventory_ = s;
      break;
    }
    case Msg::CraftQueue: {
      uint8_t n;
      if (!r.u8(n)) return;
      std::vector<CraftJob> q;
      for (uint8_t i = 0; i < n; ++i) {
        CraftJob j;
        if (!(r.u16(j.recipe) && r.f32(j.remaining) && r.f32(j.total))) return;
        if (j.recipe >= recipes().size()) return;
        q.push_back(j);
      }
      craftQueue_ = std::move(q);
      craftQueueTime_ = now;
      break;
    }
    case Msg::Objects: {
      uint8_t n;
      if (!r.u8(n)) return;
      for (uint8_t i = 0; i < n; ++i) {
        WorldObject o;
        if (!readObject(r, o)) return;
        world_.upsert(o);
      }
      break;
    }
    case Msg::ObjectRemoved: {
      uint32_t id;
      if (r.u32(id)) world_.remove(id);
      break;
    }
    case Msg::NodeState: {
      uint32_t id;
      uint8_t depleted;
      if (!(r.u32(id) && r.u8(depleted))) return;
      if (depleted) depleted_.insert(id);
      else depleted_.erase(id);
      ++nodesVersion_;
      break;
    }
    case Msg::Container: {
      uint32_t id;
      uint8_t type;
      Slots s;
      if (!(r.u32(id) && r.u8(type) && readSlots(r, s)) || type >= static_cast<uint8_t>(ObjType::Count)) return;
      containerId_ = id;
      containerType_ = static_cast<ObjType>(type);
      containerSlots_ = std::move(s);
      break;
    }
    case Msg::ContainerClosed:
      containerId_ = 0;
      containerSlots_.clear();
      break;
    case Msg::Text: {
      uint8_t kind;
      std::string text;
      if (r.u8(kind) && kind <= kLastTextKind && r.str(text, kMaxTextLength)) notice(text, static_cast<TextKind>(kind), now);
      break;
    }
    case Msg::PlayerName: {
      uint32_t id;
      std::string name;
      uint8_t rank = 0;
      if (r.u32(id) && r.str(name, kMaxNameLength)) {
        r.u8(rank);
        players_[id] = {name, rank <= 3 ? static_cast<Rank>(rank) : Rank::Player};
      }
      break;
    }
    case Msg::PlayerLeft: {
      uint32_t id;
      if (r.u32(id)) players_.erase(id);
      break;
    }
    case Msg::HitConfirm: {
      uint8_t f;
      if (r.u8(f)) {
        lastHitTime_ = now;
        lastHitFlags_ = f;
      }
      break;
    }
    case Msg::Gathered: {
      uint16_t item, count;
      if (r.u16(item) && r.u16(count) && validItem(item)) {
        if (!gathered_.empty() && gathered_.back().item == static_cast<Item>(item) && now - gathered_.back().time < 2.0) {
          gathered_.back().count += count;  // merge quick repeats into one "+30 Wood"
          gathered_.back().time = now;
        } else {
          gathered_.push_back({static_cast<Item>(item), count, now});
        }
        while (gathered_.size() > 6) gathered_.pop_front();
      }
      break;
    }
    case Msg::Died: {
      std::string cause;
      if (r.str(cause, kMaxTextLength)) deathCause_ = cause;
      break;
    }
    default:
      break;
  }
}

std::string NetClient::nameOf(uint32_t id) const {
  auto it = players_.find(id);
  return it == players_.end() ? "Survivor" : it->second.name;
}

Rank NetClient::rankOf(uint32_t id) const {
  auto it = players_.find(id);
  return it == players_.end() ? Rank::Player : it->second.rank;
}

void NetClient::sendReliable(ByteWriter& w) {
  if (status_ != Status::Connected) return;
  reliable_.queue(std::move(w.data()));
}

void NetClient::moveItem(uint32_t fromContainer, int fromSlot, uint32_t toContainer, int toSlot, int count) {
  ByteWriter w;
  w.u8(static_cast<uint8_t>(Msg::MoveItem));
  w.u32(fromContainer);
  w.u8(static_cast<uint8_t>(fromSlot));
  w.u32(toContainer);
  w.u8(static_cast<uint8_t>(toSlot));
  w.u16(static_cast<uint16_t>(count));
  sendReliable(w);
}

void NetClient::dropItem(uint32_t container, int slot) {
  ByteWriter w;
  w.u8(static_cast<uint8_t>(Msg::Drop));
  w.u32(container);
  w.u8(static_cast<uint8_t>(slot));
  sendReliable(w);
}

void NetClient::craft(int recipe, int times) {
  ByteWriter w;
  w.u8(static_cast<uint8_t>(Msg::Craft));
  w.u16(static_cast<uint16_t>(recipe));
  w.u8(static_cast<uint8_t>(times));
  sendReliable(w);
}

void NetClient::cancelCraft(int queueIndex) {
  ByteWriter w;
  w.u8(static_cast<uint8_t>(Msg::CancelCraft));
  w.u8(static_cast<uint8_t>(queueIndex));
  sendReliable(w);
}

void NetClient::place(ObjType type, const Vec3& pos, uint8_t rot) {
  ByteWriter w;
  w.u8(static_cast<uint8_t>(Msg::Place));
  w.u8(static_cast<uint8_t>(type));
  w.vec3(pos);
  w.u8(rot);
  sendReliable(w);
}

void NetClient::interactObject(uint32_t objectId) {
  ByteWriter w;
  w.u8(static_cast<uint8_t>(Msg::Interact));
  w.u8(0);
  w.u32(objectId);
  sendReliable(w);
}

void NetClient::interactNode(uint32_t nodeId) {
  ByteWriter w;
  w.u8(static_cast<uint8_t>(Msg::Interact));
  w.u8(1);
  w.u32(nodeId);
  sendReliable(w);
}

void NetClient::closeContainer() {
  if (containerId_ == 0) return;
  containerId_ = 0;
  containerSlots_.clear();
  ByteWriter w;
  w.u8(static_cast<uint8_t>(Msg::CloseContainer));
  sendReliable(w);
}

void NetClient::respawn(bool atSleepingBag) {
  ByteWriter w;
  w.u8(static_cast<uint8_t>(Msg::Respawn));
  w.u8(atSleepingBag ? 1 : 0);
  sendReliable(w);
}

void NetClient::chat(const std::string& text) {
  if (text.empty()) return;
  ByteWriter w;
  w.u8(static_cast<uint8_t>(Msg::Chat));
  w.str(text, kMaxChatLength);
  sendReliable(w);
}

void NetClient::sendInput(PlayerInput in) {
  if (status_ != Status::Connected) return;
  in.seq = nextSeq_++;
  in = sanitizeInput(in);

  // Predict: move right away instead of waiting a round trip.
  if (!vitals_.dead) applyInput(predicted_, in, terrain_, &world_);
  history_.push_back(in);
  while (history_.size() > kMaxHistory) history_.pop_front();

  // Send the newest few inputs; if one packet is lost the next carries it.
  InputPacket p;
  p.ackTick = lastServerTick_;
  size_t start = history_.size() > static_cast<size_t>(kMaxInputsPerPacket) ? history_.size() - kMaxInputsPerPacket : 0;
  p.inputs.assign(history_.begin() + static_cast<std::ptrdiff_t>(start), history_.end());
  auto bytes = encode(p);
  socket_.send(server_, bytes.data(), bytes.size());
}

void NetClient::onSnapshot(const SnapshotPacket& snap, double now) {
  if (snap.serverTick <= lastServerTick_) return;  // out of order or duplicate
  lastServerTick_ = snap.serverTick;
  vitals_ = snap.vitals;
  timeOfDay_ = snap.timeOfDay;

  // Reconcile: start from the server's truth, then replay inputs it hasn't
  // processed yet. If prediction was right, nothing visibly changes.
  Vec3 before = predicted_.pos;
  predicted_ = snap.self;
  while (!history_.empty() && history_.front().seq <= snap.lastProcessedInput) history_.pop_front();
  if (!vitals_.dead)
    for (const auto& in : history_) applyInput(predicted_, in, terrain_, &world_);
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
    const EntityState& newest = buf.back().state;
    out.push_back({id, lerp(a->state.pos, b->state.pos, alpha), lerpAngle(a->state.yaw, b->state.yaw, alpha),
                   lerpf(a->state.pitch, b->state.pitch, alpha), newest.held, newest.flags});
  }
  return out;
}

}  // namespace sg
