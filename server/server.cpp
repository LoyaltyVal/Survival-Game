#include "server.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>
#include <vector>

namespace sg {

// Burst allowance: how many inputs a client may have "banked" (100 ms worth).
constexpr float kMaxInputTokens = 6.0f;
constexpr size_t kMaxPendingInputs = 16;
constexpr float kInteractRange = 4.0f;
constexpr float kPlaceRange = 7.0f;
constexpr double kLootBagLifetime = 900.0;  // 15 minutes
constexpr size_t kMaxCraftQueue = 10;
constexpr size_t kMaxUnacked = 20000;       // client stopped acking: drop it
constexpr int kObjectsPerMessage = 30;
constexpr double kInterpDelay = 0.1;        // matches NetClient::kInterpDelay
constexpr uint32_t kSaveMagic = 0x31574753;  // "SGW1"
constexpr uint32_t kSaveVersion = 3;  // 3: + ranks, bans, mutes, time of day
//  // 2: + crafting queues, used-up nodes, loot bag timers

Server::Server(const ServerConfig& cfg) : cfg_(cfg), terrain_(cfg.terrainSeed), rng_(cfg.terrainSeed) {}

bool Server::start() {
  if (!socket_.open(cfg_.port)) {
    std::fprintf(stderr, "[server] could not bind UDP port %u\n", cfg_.port);
    return false;
  }
  if (!cfg_.savePath.empty()) load();
  std::printf("[server] \"%s\" listening on UDP %u, %d Hz tick, max %d players, seed %u\n", cfg_.name.c_str(),
              cfg_.port, cfg_.tickRate, cfg_.maxPlayers, cfg_.terrainSeed);
  return true;
}

void Server::stop() {
  save();
  auto bye = encodeDisconnect();
  for (auto& [addr, c] : clients_) {
    send(addr, bye);
    storeRecord(c);
  }
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
  timeOfDay_ = std::fmod(timeOfDay_ + static_cast<float>(dt) * 24.0f / (cfg_.dayLengthMinutes * 60.0f), 24.0f);
  receivePackets();
  simulate(dt);
  if (time_ - lastWorldUpdate_ >= 1.0) {
    updateWorld();
    lastWorldUpdate_ = time_;
  }
  snapshotAccum_ += dt;
  const double snapshotInterval = 1.0 / cfg_.snapshotRate;
  if (snapshotAccum_ >= snapshotInterval) {
    snapshotAccum_ = std::fmod(snapshotAccum_, snapshotInterval);
    sendSnapshots();
  }
  dropTimedOut();
  if (!cfg_.savePath.empty() && time_ - lastSave_ >= cfg_.autosaveSeconds) {
    save();
    lastSave_ = time_;
  }
}

void Server::send(const Address& to, const std::vector<uint8_t>& bytes) {
  socket_.send(to, bytes.data(), bytes.size());
}

// --- networking ----------------------------------------------------------------------
void Server::receivePackets() {
  uint8_t buf[kMaxPacketSize];
  Address from;
  int n;
  while ((n = socket_.receive(from, buf, sizeof(buf))) > 0) {
    const size_t size = static_cast<size_t>(n);
    auto type = peekType(buf, size);
    if (!type) continue;  // not our protocol: ignore silently

    if (*type == PacketType::Hello) {
      handleHello(from, buf, size);
      continue;
    }
    if (*type == PacketType::InfoRequest) {
      handleInfoRequest(from, buf, size);
      continue;
    }
    auto it = clients_.find(from);
    if (it == clients_.end()) continue;  // unknown sender
    Client& c = it->second;
    c.lastHeard = time_;

    if (*type == PacketType::Input) {
      handleInput(c, buf, size);
    } else if (*type == PacketType::Reliable) {
      std::vector<std::vector<uint8_t>> msgs;
      if (c.reliable.receive(buf, size, msgs))
        for (const auto& m : msgs) handleMessage(c, m);
    } else if (*type == PacketType::Disconnect) {
      removeClient(it, "left");
    }
  }
}

void Server::handleInfoRequest(const Address& from, const uint8_t* data, size_t size) {
  InfoRequestPacket req;
  if (!decode(data, size, req)) return;
  InfoReplyPacket rep;
  rep.nonce = req.nonce;
  rep.name = cfg_.name;
  rep.players = static_cast<uint16_t>(clients_.size());
  rep.maxPlayers = static_cast<uint16_t>(cfg_.maxPlayers);
  rep.seed = cfg_.terrainSeed;
  send(from, encode(rep));
}

static std::string cleanName(const std::string& in) {
  std::string out;
  for (char ch : in)
    if (ch >= 32 && ch < 127) out += ch;
  while (!out.empty() && out.front() == ' ') out.erase(out.begin());
  while (!out.empty() && out.back() == ' ') out.pop_back();
  if (out.size() > kMaxNameLength) out.resize(kMaxNameLength);
  return out.empty() ? "Survivor" : out;
}

void Server::handleHello(const Address& from, const uint8_t* data, size_t size) {
  HelloPacket hello;
  if (!decode(data, size, hello)) return;

  auto existing = clients_.find(from);
  if (existing != clients_.end()) {
    // Our Welcome was probably lost; send it again.
    WelcomePacket w{existing->second.id, cfg_.terrainSeed, static_cast<uint16_t>(cfg_.tickRate),
                    existing->second.state, cfg_.name};
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
  const std::string name = cleanName(hello.name);
  if (isBanned(name)) {
    send(from, encode(RejectPacket{RejectReason::Banned}));
    return;
  }
  for (const auto& [addr, c] : clients_)
    if (c.name == name) {
      send(from, encode(RejectPacket{RejectReason::NameInUse}));
      return;
    }

  Client c;
  c.name = name;
  c.addr = from;
  c.lastHeard = time_;
  c.inputTokens = kMaxInputTokens;
  bool returning = false;
  if (auto rec = records_.find(name); rec != records_.end()) {
    c.id = rec->second.id;
    c.state = rec->second.state;
    c.vitals = rec->second.vitals;
    c.inv = rec->second.inv;
    c.inv.resize(kInventorySlots);
    c.crafting = rec->second.crafting;
    returning = true;
  } else {
    c.id = nextPlayerId_++;
    c.state = pickSpawn();
    c.vitals.food = 70;
    c.vitals.saturation = 5;
    giveStartingKit(c.inv);
  }
  c.state.vel = {};
  c.state.stamina = kMaxStamina;

  WelcomePacket w{c.id, cfg_.terrainSeed, static_cast<uint16_t>(cfg_.tickRate), c.state, cfg_.name};
  std::printf("[server] %s %s from %s as id %u at (%.0f, %.0f)\n", name.c_str(), returning ? "returned" : "joined",
              from.toString().c_str(), c.id, c.state.pos.x, c.state.pos.z);
  auto [it, inserted] = clients_.emplace(from, std::move(c));
  send(from, encode(w));

  Client& cl = it->second;
  // Ranks: the host (or, on a dedicated server without --owner, the very first player) owns the world.
  bool hasOwner = false;
  for (const auto& [n, r] : ranks_) hasOwner = hasOwner || r == Rank::Owner;
  bool madeOwner = false;
  if ((!ownerName().empty() && name == ownerName()) || (ownerName().empty() && !hasOwner)) {
    madeOwner = rankOf(name) != Rank::Owner;
    ranks_[name] = Rank::Owner;
  }
  broadcastPlayerInfo(cl);
  sendFullState(cl);
  Rank rank = rankOf(name);
  broadcastText(TextKind::Info, (rank == Rank::Player ? "" : "[" + std::string(rankName(rank)) + "] ") + cl.name + " joined the server");
  if (madeOwner) sendText(cl, TextKind::Announce, "You are the Owner of this world. Type /help ranks to manage players.");
}

void Server::removeClient(std::unordered_map<Address, Client, AddressHash>::iterator it, const char* why) {
  Client& c = it->second;
  std::printf("[server] %s (id %u) %s\n", c.name.c_str(), c.id, why);
  storeRecord(c);
  std::string name = c.name;
  uint32_t id = c.id;
  clients_.erase(it);
  ByteWriter left;
  left.u8(static_cast<uint8_t>(Msg::PlayerLeft));
  left.u32(id);
  broadcast(left);
  broadcastText(TextKind::Info, name + " left the server");
  save();  // never lose someone's progress to a crash after they log off
}

void Server::handleInput(Client& c, const uint8_t* data, size_t size) {
  InputPacket p;
  if (!decode(data, size, p)) return;
  if (p.ackTick > 0 && p.ackTick <= tickCount_) {
    float sample = static_cast<float>(tickCount_ - p.ackTick) / static_cast<float>(cfg_.tickRate);
    c.rtt = clampf(c.rtt * 0.9f + sample * 0.1f, 0.0f, 1.0f);
  }
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
    snap.vitals = c.vitals;
    snap.timeOfDay = timeOfDay_;
    for (size_t i = 0; i < nearby.size() && i < static_cast<size_t>(kMaxSnapshotEntities); ++i) {
      const Client* o = nearby[i].second;
      EntityState e;
      e.id = o->id;
      e.pos = o->state.pos;
      e.yaw = o->state.yaw;
      e.pitch = o->state.pitch;
      e.held = o->inv[o->slot].empty() ? Item::None : o->inv[o->slot].item;
      e.flags = static_cast<uint8_t>((o->vitals.dead ? kEntityDead : 0) | (time_ - o->lastAttack < 0.15 ? kEntityAttacking : 0));
      snap.others.push_back(e);
    }
    send(addr, encode(snap));
    flushReliable(c);
  }
}

void Server::flushReliable(Client& c) {
  if (!c.reliable.needsSend()) return;
  for (auto& p : c.reliable.build(4)) send(c.addr, p);
}

void Server::dropTimedOut() {
  for (auto it = clients_.begin(); it != clients_.end();) {
    auto next = std::next(it);
    if (it->second.kick != 0) {
      send(it->first, encode(RejectPacket{static_cast<RejectReason>(it->second.kick)}));
      removeClient(it, it->second.kick == static_cast<uint8_t>(RejectReason::Banned) ? "was banned" : "was kicked");
    } else if (time_ - it->second.lastHeard > cfg_.timeoutSeconds) removeClient(it, "timed out");
    else if (it->second.reliable.unackedCount() > kMaxUnacked) removeClient(it, "stopped responding");
    it = next;
  }
}

// --- reliable messages -------------------------------------------------------------
void Server::queue(Client& c, const ByteWriter& w) { c.reliable.queue(w.data()); }

void Server::broadcast(const ByteWriter& w) {
  for (auto& [addr, c] : clients_) queue(c, w);
}

void Server::sendText(Client& c, TextKind kind, const std::string& text) {
  ByteWriter w;
  w.u8(static_cast<uint8_t>(Msg::Text));
  w.u8(static_cast<uint8_t>(kind));
  w.str(text, kMaxTextLength);
  queue(c, w);
}

void Server::broadcastText(TextKind kind, const std::string& text) {
  for (auto& [addr, c] : clients_) sendText(c, kind, text);
}

void Server::warn(Client& c, const std::string& text) {
  if (time_ - c.lastWarn < 0.4) return;
  c.lastWarn = time_;
  sendText(c, TextKind::Warning, text);
}

void Server::sendInventory(Client& c) {
  ByteWriter w;
  w.u8(static_cast<uint8_t>(Msg::Inventory));
  writeSlots(w, c.inv);
  queue(c, w);
  c.invDirty = false;
}

void Server::sendCraftQueue(Client& c) {
  ByteWriter w;
  w.u8(static_cast<uint8_t>(Msg::CraftQueue));
  w.u8(static_cast<uint8_t>(c.crafting.size()));
  for (const auto& j : c.crafting) {
    w.u16(j.recipe);
    w.f32(j.remaining);
    w.f32(j.total);
  }
  queue(c, w);
  c.craftDirty = false;
}

void Server::sendContainer(Client& c) {
  auto it = containers_.find(c.openContainer);
  const WorldObject* o = world_.find(c.openContainer);
  if (it == containers_.end() || !o) return;
  ByteWriter w;
  w.u8(static_cast<uint8_t>(Msg::Container));
  w.u32(c.openContainer);
  w.u8(static_cast<uint8_t>(o->type));
  writeSlots(w, it->second);
  queue(c, w);
}

void Server::sendFullState(Client& c) {
  for (const auto& [addr, o] : clients_) queue(c, playerInfoMsg(o));
  // Every placed object, in batches.
  std::vector<const WorldObject*> all;
  for (const auto& [id, o] : world_.objects()) all.push_back(&o);
  for (size_t i = 0; i < all.size(); i += kObjectsPerMessage) {
    size_t n = std::min(all.size() - i, static_cast<size_t>(kObjectsPerMessage));
    ByteWriter w;
    w.u8(static_cast<uint8_t>(Msg::Objects));
    w.u8(static_cast<uint8_t>(n));
    for (size_t k = 0; k < n; ++k) writeObject(w, *all[i + k]);
    queue(c, w);
  }
  for (const auto& [id, n] : nodes_) {
    if (n.respawnAt <= 0) continue;
    ByteWriter w;
    w.u8(static_cast<uint8_t>(Msg::NodeState));
    w.u32(id);
    w.u8(1);
    queue(c, w);
  }
  sendInventory(c);
  sendCraftQueue(c);
  sendText(c, TextKind::Info, "Welcome to " + cfg_.name + "!");
  if (c.vitals.dead) {
    ByteWriter w;
    w.u8(static_cast<uint8_t>(Msg::Died));
    w.str("You died while you were away", kMaxTextLength);
    queue(c, w);
  }
}

void Server::objectChanged(const WorldObject& o) {
  ByteWriter w;
  w.u8(static_cast<uint8_t>(Msg::Objects));
  w.u8(1);
  writeObject(w, o);
  broadcast(w);
}

void Server::objectRemoved(uint32_t id) {
  ByteWriter w;
  w.u8(static_cast<uint8_t>(Msg::ObjectRemoved));
  w.u32(id);
  broadcast(w);
  for (auto& [addr, c] : clients_)
    if (c.openContainer == id) {
      c.openContainer = 0;
      ByteWriter cw;
      cw.u8(static_cast<uint8_t>(Msg::ContainerClosed));
      queue(c, cw);
    }
}

void Server::containerChanged(uint32_t objectId) {
  auto it = containers_.find(objectId);
  const WorldObject* o = world_.find(objectId);
  if (it == containers_.end() || !o) return;
  if (o->type == ObjType::LootBag && slotsEmpty(it->second)) {  // looted clean: the bag disappears
    containers_.erase(it);
    bagExpiry_.erase(objectId);
    world_.remove(objectId);
    objectRemoved(objectId);
    return;
  }
  for (auto& [addr, c] : clients_)
    if (c.openContainer == objectId) sendContainer(c);
}

Slots* Server::containerFor(Client& c, uint32_t id) {
  if (id == kSelfContainer) return &c.inv;
  if (id != c.openContainer) return nullptr;
  auto it = containers_.find(id);
  return it == containers_.end() ? nullptr : &it->second;
}

void Server::handleMessage(Client& c, const std::vector<uint8_t>& msg) {
  ByteReader r(msg.data(), msg.size());
  uint8_t raw;
  if (!r.u8(raw)) return;
  switch (static_cast<Msg>(raw)) {
    case Msg::MoveItem: {
      uint32_t from, to;
      uint8_t fromSlot, toSlot;
      uint16_t count;
      if (!(r.u32(from) && r.u8(fromSlot) && r.u32(to) && r.u8(toSlot) && r.u16(count)) || c.vitals.dead) return;
      Slots* a = containerFor(c, from);
      Slots* b = containerFor(c, to);
      if (!a || !b) return;
      if (moveItem(*a, fromSlot, *b, toSlot, count)) {
        if (from == kSelfContainer || to == kSelfContainer) c.invDirty = true;
        if (from != kSelfContainer) containerChanged(from);
        if (to != kSelfContainer && to != from) containerChanged(to);
      }
      break;
    }
    case Msg::Drop: {
      uint32_t from;
      uint8_t slot;
      if (!(r.u32(from) && r.u8(slot)) || c.vitals.dead) return;
      Slots* a = containerFor(c, from);
      if (!a || slot >= a->size() || (*a)[slot].empty()) return;
      Slots bag(1);
      bag[0] = (*a)[slot];
      (*a)[slot] = {};
      Vec3 fwd = directionFromYawPitch(c.state.yaw, 0);
      spawnLootBag(c.state.pos + fwd * 1.0f, bag);
      if (from == kSelfContainer) c.invDirty = true;
      else containerChanged(from);
      break;
    }
    case Msg::Craft: {
      uint16_t recipe;
      uint8_t times;
      if (r.u16(recipe) && r.u8(times)) doCraft(c, recipe, times);
      break;
    }
    case Msg::CancelCraft: {
      uint8_t idx;
      if (!r.u8(idx) || idx >= c.crafting.size()) return;
      const Recipe& rec = recipes()[c.crafting[idx].recipe];
      for (const auto& in : rec.in)
        if (in.item != Item::None) give(c, in.item, in.count);
      c.crafting.erase(c.crafting.begin() + idx);
      c.craftDirty = true;
      break;
    }
    case Msg::Place: {
      uint8_t type, rot;
      Vec3 pos;
      if (!(r.u8(type) && r.vec3(pos) && r.u8(rot)) || type == 0 || type >= static_cast<uint8_t>(ObjType::Count)) return;
      doPlace(c, static_cast<ObjType>(type), pos, rot);
      break;
    }
    case Msg::Interact: {
      uint8_t kind;
      uint32_t id;
      if (r.u8(kind) && r.u32(id)) doInteract(c, kind, id);
      break;
    }
    case Msg::CloseContainer:
      c.openContainer = 0;
      break;
    case Msg::Respawn: {
      uint8_t atBag;
      if (r.u8(atBag)) respawnPlayer(c, atBag != 0);
      break;
    }
    case Msg::Chat: {
      std::string text;
      if (!r.str(text, kMaxChatLength)) return;
      std::string clean;
      for (char ch : text)
        if (ch >= 32 && ch < 127) clean += ch;
      if (clean.empty()) return;
      if (clean[0] == '/') {
        std::printf("[command] %s: %s\n", c.name.c_str(), clean.c_str());
        handleCommand(c, clean);
        return;
      }
      if (time_ - c.lastChat < 0.5) return;  // spam brake
      c.lastChat = time_;
      if (isMuted(c.name)) {
        warn(c, "You are muted");
        return;
      }
      std::printf("[chat] %s: %s\n", c.name.c_str(), clean.c_str());
      Rank rank = rankOf(c.name);
      broadcastText(TextKind::Chat, (rank == Rank::Player ? "" : "[" + std::string(rankName(rank)) + "] ") + c.name + ": " + clean);
      break;
    }
    default:
      break;  // unknown or server-only message: ignore
  }
}

// --- gameplay ------------------------------------------------------------------------------
void Server::simulate(double dt) {
  for (auto& [addr, c] : clients_) {
    // Refill the budget at the real input rate. A speed hack that sends inputs
    // faster than 60 Hz runs out of tokens and its extra inputs wait or drop.
    c.inputTokens = std::min(kMaxInputTokens, c.inputTokens + static_cast<float>(dt * kInputHz));
    while (!c.pending.empty() && c.inputTokens >= 1.0f) {
      PlayerInput in = c.pending.front();
      c.pending.pop_front();
      processInput(c, in);
      c.lastProcessedInput = in.seq;
      c.inputTokens -= 1.0f;
    }
    if (!c.vitals.dead) updateVitals(c, static_cast<float>(dt));
    updateCrafting(c, static_cast<float>(dt));

    c.history.push_back({time_, c.state.pos});
    while (c.history.size() > 2 && c.history.front().time < time_ - 1.0) c.history.pop_front();

    // Walked away from an open box: close it.
    if (c.openContainer) {
      const WorldObject* o = world_.find(c.openContainer);
      if (!o || length(o->pos - c.state.pos) > kInteractRange + 1.5f || c.vitals.dead) {
        c.openContainer = 0;
        ByteWriter w;
        w.u8(static_cast<uint8_t>(Msg::ContainerClosed));
        queue(c, w);
      }
    }
  }
  for (auto& [addr, c] : clients_) {
    if (c.invDirty) sendInventory(c);
    if (c.craftDirty) sendCraftQueue(c);
  }
}

void Server::processInput(Client& c, const PlayerInput& raw) {
  const PlayerInput in = sanitizeInput(raw);
  c.slot = in.slot;
  if (c.vitals.dead) return;
  c.state.starving = c.vitals.food <= 0;
  applyInput(c.state, in, terrain_, &world_);
  c.inputClock += kInputDt;
  c.sprinting = (in.buttons & kButtonSprint) && in.moveZ > 0 && !c.state.exhausted;
  if (in.buttons & kButtonPrimary) primaryAction(c, in);
}

void Server::primaryAction(Client& c, const PlayerInput& in) {
  if (c.inputClock < c.nextUse) return;
  ItemStack& held = c.inv[c.slot];
  const Item item = held.empty() ? Item::None : held.item;
  const ItemDef& d = itemDef(item);
  switch (d.kind) {
    case ItemKind::Food:
    case ItemKind::Medical: {
      Vitals& v = c.vitals;
      if (d.kind == ItemKind::Medical && v.health >= kMaxHealth) {
        warn(c, "You are already at full health");
        c.nextUse = c.inputClock + 0.5;
        return;
      }
      if (d.kind == ItemKind::Food && v.food >= kMaxFood - 0.5f && v.saturation >= kMaxSaturation - 0.5f) {
        warn(c, "You are full");
        c.nextUse = c.inputClock + 0.5;
        return;
      }
      held.count = static_cast<uint16_t>(held.count - 1);
      if (held.count == 0) held = {};
      v.food = std::min(kMaxFood, v.food + d.food);
      v.saturation = std::min(kMaxSaturation, v.saturation + d.saturation);
      v.health = std::min(kMaxHealth, v.health + d.heal);
      c.nextUse = c.inputClock + d.useTime;
      c.invDirty = true;
      break;
    }
    case ItemKind::Gun:
      if (!removeItem(c.inv, d.ammo, 1)) {
        warn(c, std::string("No ammo. Craft ") + itemDef(d.ammo).name);
        c.nextUse = c.inputClock + 0.3;
        return;
      }
      c.invDirty = true;
      shoot(c, in, item);
      c.nextUse = c.inputClock + d.useTime;
      c.lastAttack = time_;
      break;
    case ItemKind::Tool:
      meleeHit(c, in, item);
      c.nextUse = c.inputClock + d.useTime;
      c.lastAttack = time_;
      break;
    case ItemKind::Resource:
    case ItemKind::Ammo:  // holding stuff: punch with your hands
      meleeHit(c, in, Item::None);
      c.nextUse = c.inputClock + itemDef(Item::None).useTime;
      c.lastAttack = time_;
      break;
    default:
      break;  // building plan and deployables are placed with a Place message
  }
}

Vec3 Server::positionAt(const Client& c, double t) const {
  if (c.history.empty() || t >= c.history.back().time) return c.state.pos;
  if (t <= c.history.front().time) return c.history.front().pos;
  for (size_t i = 1; i < c.history.size(); ++i) {
    if (c.history[i].time >= t) {
      const auto& a = c.history[i - 1];
      const auto& b = c.history[i];
      float alpha = b.time > a.time ? static_cast<float>((t - a.time) / (b.time - a.time)) : 1.0f;
      return lerp(a.pos, b.pos, alpha);
    }
  }
  return c.state.pos;
}

// Melee and guns test players at positions rewound to what the attacker saw
// on screen (lag compensation), capped at 250 ms.
void Server::meleeHit(Client& c, const PlayerInput& in, Item item) {
  const ItemDef& d = itemDef(item);
  const Vec3 eye = c.state.pos + Vec3{0, kEyeHeight, 0};
  const Vec3 dir = directionFromYawPitch(in.yaw, in.pitch);
  float best = d.range;
  enum class Kind { None, Player, Object, Node } kind = Kind::None;
  uint32_t hitId = 0;
  bool head = false;

  const double rewindTo = time_ - std::clamp(c.rtt * 0.5 + kInterpDelay, 0.0, 0.25);
  for (auto& [addr, o] : clients_) {
    if (o.id == c.id || o.vitals.dead) continue;
    Vec3 p = positionAt(o, rewindTo);
    AABB body{{p.x - 0.3f, p.y, p.z - 0.3f}, {p.x + 0.3f, p.y + 1.45f, p.z + 0.3f}};
    AABB hd{{p.x - 0.2f, p.y + 1.45f, p.z - 0.2f}, {p.x + 0.2f, p.y + 1.85f, p.z + 0.2f}};
    float t;
    if (rayAABB(eye, dir, hd, best, t)) { best = t; kind = Kind::Player; hitId = o.id; head = true; }
    if (rayAABB(eye, dir, body, best, t)) { best = t; kind = Kind::Player; hitId = o.id; head = false; }
  }
  if (auto h = world_.raycast(eye, dir, best, true)) {
    const WorldObject* o = world_.find(h->objectId);
    if (o && o->type != ObjType::LootBag) { best = h->dist; kind = Kind::Object; hitId = h->objectId; }
  }
  std::vector<ResourceNode> nodes;
  nodesInRect(terrain_, eye.x - best, eye.z - best, eye.x + best, eye.z + best, nodes);
  for (const auto& n : nodes) {
    if (nodeInfo(n.type).pickup) continue;
    auto rt = nodes_.find(n.id);
    if (rt != nodes_.end() && rt->second.respawnAt > 0) continue;  // already used up
    float t;
    if (rayAABB(eye, dir, nodeBounds(n), best, t)) { best = t; kind = Kind::Node; hitId = n.id; }
  }
  if (auto t = rayTerrain(terrain_, eye, dir, best)) {
    if (*t < best) kind = Kind::None;  // swung into the ground
  }

  switch (kind) {
    case Kind::Player:
      if (Client* target = findClient(hitId)) damagePlayer(*target, d.damage * (head ? 1.5f : 1.0f), &c, d.name, head);
      break;
    case Kind::Object: {
      const WorldObject* o = world_.find(hitId);
      if (!o) break;
      if (item == Item::Hammer) {
        if (!isBuildingPiece(o->type)) break;
        if (o->owner != c.id) { warn(c, "You can only upgrade your own building"); break; }
        if (o->tier == Tier::Metal) { warn(c, "Already fully upgraded"); break; }
        Tier next = static_cast<Tier>(static_cast<int>(o->tier) + 1);
        Ingredient cost = buildCost(o->type, next);
        if (!removeItem(c.inv, cost.item, cost.count)) {
          warn(c, "Upgrade to " + std::string(tierName(next)) + " needs " + std::to_string(cost.count) + " " +
                      itemDef(cost.item).name);
          break;
        }
        WorldObject up = *o;
        up.tier = next;
        up.hp = maxHp(up.type, next);
        world_.upsert(up);
        objectChanged(up);
        c.invDirty = true;
      } else {
        bool armored = isBuildingPiece(o->type) && o->tier != Tier::Twig;
        damageObject(hitId, armored ? d.damage * 0.2f : d.damage);
        ByteWriter w;
        w.u8(static_cast<uint8_t>(Msg::HitConfirm));
        w.u8(0);
        queue(c, w);
      }
      break;
    }
    case Kind::Node: {
      auto node = nodeById(terrain_, hitId);
      if (!node) break;
      const NodeInfo& info = nodeInfo(node->type);
      float rate = node->type == NodeType::Tree ? d.gatherWood
                   : node->type == NodeType::Stone ? d.gatherStone
                                                   : d.gatherOre;
      if (rate < 1.0f) {
        warn(c, "You need a better tool for that");
        break;
      }
      NodeRuntime& rt = nodes_[hitId];
      if (rt.remaining <= 0 && rt.respawnAt <= 0) rt.remaining = info.amount;  // first hit
      int amount = static_cast<int>(std::min(rate, rt.remaining));
      rt.remaining -= static_cast<float>(amount);
      int left = addItem(c.inv, info.yield, amount);
      if (amount - left > 0) {
        ByteWriter w;
        w.u8(static_cast<uint8_t>(Msg::Gathered));
        w.u16(static_cast<uint16_t>(info.yield));
        w.u16(static_cast<uint16_t>(amount - left));
        queue(c, w);
        c.invDirty = true;
      }
      if (left > 0) warn(c, "Inventory full");
      if (rt.remaining <= 0) {
        rt.respawnAt = time_ + info.respawn;
        ByteWriter w;
        w.u8(static_cast<uint8_t>(Msg::NodeState));
        w.u32(hitId);
        w.u8(1);
        broadcast(w);
      }
      break;
    }
    default:
      break;
  }
}

void Server::shoot(Client& c, const PlayerInput& in, Item item) {
  const ItemDef& d = itemDef(item);
  const Vec3 eye = c.state.pos + Vec3{0, kEyeHeight, 0};
  const Vec3 dir = directionFromYawPitch(in.yaw, in.pitch);
  float best = d.range;
  uint32_t playerId = 0, objectId = 0;
  bool head = false;

  const double rewindTo = time_ - std::clamp(c.rtt * 0.5 + kInterpDelay, 0.0, 0.25);
  for (auto& [addr, o] : clients_) {
    if (o.id == c.id || o.vitals.dead) continue;
    Vec3 p = positionAt(o, rewindTo);
    AABB body{{p.x - 0.3f, p.y, p.z - 0.3f}, {p.x + 0.3f, p.y + 1.45f, p.z + 0.3f}};
    AABB hd{{p.x - 0.2f, p.y + 1.45f, p.z - 0.2f}, {p.x + 0.2f, p.y + 1.85f, p.z + 0.2f}};
    float t;
    if (rayAABB(eye, dir, hd, best, t)) { best = t; playerId = o.id; head = true; }
    if (rayAABB(eye, dir, body, best, t)) { best = t; playerId = o.id; head = false; }
  }
  if (auto h = world_.raycast(eye, dir, best, false)) {  // walls stop bullets
    best = h->dist;
    playerId = 0;
    objectId = h->objectId;
  }
  if (auto t = rayTerrain(terrain_, eye, dir, best)) {
    if (*t < best) { playerId = 0; objectId = 0; }
  }
  if (playerId) {
    if (Client* target = findClient(playerId)) damagePlayer(*target, d.damage * (head ? 2.0f : 1.0f), &c, d.name, head);
  } else if (objectId) {
    const WorldObject* o = world_.find(objectId);
    if (o) damageObject(objectId, isBuildingPiece(o->type) && o->tier != Tier::Twig ? d.damage * 0.1f : d.damage);
  }
}

void Server::damagePlayer(Client& victim, float amount, Client* attacker, const char* weapon, bool headshot) {
  if (victim.vitals.dead || victim.god) return;
  victim.vitals.health -= amount;
  bool killed = victim.vitals.health <= 0;
  if (attacker) {
    ByteWriter w;
    w.u8(static_cast<uint8_t>(Msg::HitConfirm));
    w.u8(static_cast<uint8_t>((headshot ? 1 : 0) | (killed ? 2 : 0)));
    queue(*attacker, w);
  }
  if (killed) {
    if (attacker)
      killPlayer(victim, "Killed by " + attacker->name + " (" + weapon + ")",
                 attacker->name + " killed " + victim.name + " with " + weapon + (headshot ? " (headshot)" : ""));
    else
      killPlayer(victim, weapon, victim.name + " died");
  }
}

void Server::killPlayer(Client& victim, const std::string& cause, const std::string& feed) {
  if (victim.vitals.dead) return;
  victim.vitals.dead = true;
  victim.vitals.health = 0;
  // Unfinished crafts are refunded into the loot bag.
  for (const auto& job : victim.crafting)
    for (const auto& in : recipes()[job.recipe].in)
      if (in.item != Item::None) addItem(victim.inv, in.item, in.count);
  victim.crafting.clear();
  victim.craftDirty = true;
  if (!slotsEmpty(victim.inv)) spawnLootBag(victim.state.pos, victim.inv);
  victim.inv.assign(kInventorySlots, {});
  victim.invDirty = true;
  victim.state.vel = {};
  victim.pending.clear();
  ByteWriter w;
  w.u8(static_cast<uint8_t>(Msg::Died));
  w.str(cause, kMaxTextLength);
  queue(victim, w);
  broadcastText(TextKind::Kill, feed);
  std::printf("[server] %s\n", feed.c_str());
}

void Server::respawnPlayer(Client& c, bool atBag) {
  if (!c.vitals.dead) return;
  PlayerState s;
  bool found = false;
  if (atBag) {
    for (const auto& [id, o] : world_.objects())
      if (o.type == ObjType::SleepingBag && o.owner == c.id) {
        s.pos = o.pos + Vec3{0, 0.1f, 0};
        s.yaw = static_cast<float>(o.rot) * kPi * 0.5f;
        found = true;
        break;
      }
    if (!found) warn(c, "You have no sleeping bag");
  }
  if (!found) s = pickSpawn();
  s.onGround = true;
  c.state = s;
  c.vitals = {};
  c.vitals.food = 60;
  c.vitals.saturation = 5;
  giveStartingKit(c.inv);
  c.invDirty = true;
  c.history.clear();
  c.nextUse = c.inputClock;
}

void Server::damageObject(uint32_t id, float amount) {
  const WorldObject* p = world_.find(id);
  if (!p || p->type == ObjType::LootBag) return;
  WorldObject o = *p;
  o.hp -= amount;
  if (o.hp > 0) {
    world_.upsert(o);
    objectChanged(o);
    return;
  }
  // Destroyed. Boxes spill their contents; doorways take their door with them.
  if (auto it = containers_.find(id); it != containers_.end()) {
    Slots items = it->second;
    containers_.erase(it);
    world_.remove(id);
    objectRemoved(id);
    if (!slotsEmpty(items)) spawnLootBag(o.pos, items);
  } else {
    world_.remove(id);
    objectRemoved(id);
  }
  if (o.type == ObjType::Doorway) {
    std::vector<uint32_t> doors;
    for (const auto& [did, d] : world_.objects())
      if (d.type == ObjType::Door && length(d.pos - o.pos) < 0.05f) doors.push_back(did);
    for (uint32_t did : doors) {
      world_.remove(did);
      objectRemoved(did);
    }
  }
}

void Server::updateVitals(Client& c, float dt) {
  Vitals& v = c.vitals;
  if (c.god) {
    v.food = std::max(v.food, 50.0f);
    v.health = kMaxHealth;
  }
  // Saturation is a buffer: it drains first, then food starts going down.
  float drain = 0.035f + (c.sprinting ? 0.05f : 0.0f);
  if (v.saturation > 0) v.saturation = std::max(0.0f, v.saturation - drain * dt);
  else v.food = std::max(0.0f, v.food - drain * 2.5f * dt);
  // Well fed: slowly heal, which costs saturation (or food).
  if (v.food >= 70 && v.health < kMaxHealth) {
    v.health = std::min(kMaxHealth, v.health + 0.4f * dt);
    if (v.saturation > 0) v.saturation = std::max(0.0f, v.saturation - 0.1f * dt);
    else v.food = std::max(0.0f, v.food - 0.15f * dt);
  }
  if (v.food <= 0) {
    v.health -= 0.6f * dt;
    if (v.health <= 0) killPlayer(c, "You starved to death", c.name + " starved to death");
  }
}

void Server::updateCrafting(Client& c, float dt) {
  if (c.crafting.empty() || c.vitals.dead) return;
  CraftJob& job = c.crafting.front();
  job.remaining -= dt;
  if (job.remaining > 0) return;
  const Recipe& r = recipes()[job.recipe];
  c.crafting.pop_front();
  c.craftDirty = true;
  give(c, r.output, r.outCount);
  sendText(c, TextKind::Info, std::string("Crafted ") + itemDef(r.output).name);
}

void Server::give(Client& c, Item item, int count) {
  int left = addItem(c.inv, item, count);
  c.invDirty = true;
  if (left <= 0) return;
  Slots overflow;
  const int maxStack = itemDef(item).maxStack;
  while (left > 0) {
    int n = std::min(left, maxStack);
    overflow.push_back({item, static_cast<uint16_t>(n)});
    left -= n;
  }
  spawnLootBag(c.state.pos, overflow);
  warn(c, "Inventory full - dropped on the ground");
}

uint32_t Server::spawnLootBag(const Vec3& pos, const Slots& items) {
  Slots slots(kInventorySlots);
  int k = 0;
  for (const auto& s : items)
    if (!s.empty() && k < kInventorySlots) slots[k++] = s;
  if (k == 0) return 0;
  WorldObject o;
  o.id = nextObjectId_++;
  o.type = ObjType::LootBag;
  o.hp = 1;
  float x = clampf(pos.x, 2.0f, kWorldSize - 2.0f), z = clampf(pos.z, 2.0f, kWorldSize - 2.0f);
  o.pos = {x, world_.groundHeight(terrain_, x, z, 0.1f, pos.y + 0.5f, 0.0f), z};
  world_.upsert(o);
  containers_[o.id] = std::move(slots);
  bagExpiry_[o.id] = time_ + kLootBagLifetime;
  objectChanged(o);
  return o.id;
}

bool Server::nearWorkbench(const Client& c) const {
  std::vector<const WorldObject*> list;
  world_.near(c.state.pos, 4.0f, list);
  for (const WorldObject* o : list)
    if (o->type == ObjType::Workbench) return true;
  return false;
}

void Server::doCraft(Client& c, int recipe, int times) {
  if (c.vitals.dead || recipe < 0 || recipe >= static_cast<int>(recipes().size())) return;
  times = std::clamp(times, 1, 10);
  const Recipe& r = recipes()[recipe];
  if (r.workbench && !nearWorkbench(c)) {
    warn(c, "You need to stand near a workbench");
    return;
  }
  for (int t = 0; t < times; ++t) {
    if (c.crafting.size() >= kMaxCraftQueue) {
      warn(c, "Craft queue is full");
      break;
    }
    if (!canAfford(c.inv, r, 1)) {
      if (t == 0) warn(c, "Not enough resources");
      break;
    }
    for (const auto& in : r.in)
      if (in.item != Item::None) removeItem(c.inv, in.item, in.count);
    c.crafting.push_back({static_cast<uint16_t>(recipe), r.seconds, r.seconds});
    c.invDirty = true;
    c.craftDirty = true;
  }
}

void Server::doPlace(Client& c, ObjType type, const Vec3& pos, uint8_t rot) {
  if (c.vitals.dead) return;
  const Vec3 eye = c.state.pos + Vec3{0, kEyeHeight, 0};
  if (length(pos - eye) > kPlaceRange) {
    warn(c, "Too far away");
    return;
  }
  Ingredient cost{};
  Item needItem = Item::None;
  if (isBuildingPiece(type)) {
    if (countItem(c.inv, Item::BuildingPlan) == 0) return;
    cost = buildCost(type, Tier::Twig);
  } else {
    switch (type) {
      case ObjType::Door: needItem = Item::WoodenDoor; break;
      case ObjType::StorageBox: needItem = Item::StorageBox; break;
      case ObjType::Workbench: needItem = Item::Workbench; break;
      case ObjType::SleepingBag: needItem = Item::SleepingBag; break;
      default: return;
    }
    if (countItem(c.inv, needItem) == 0) return;
  }
  if (const char* err = checkPlacement(type, pos, rot, terrain_, world_)) {
    warn(c, err);
    return;
  }
  WorldObject o;
  o.type = type;
  o.pos = pos;
  o.rot = rot;
  // Never build a solid piece inside a player.
  std::vector<ObjBox> boxes;
  objectBoxes(o, boxes);
  for (const auto& [addr, other] : clients_) {
    if (other.vitals.dead) continue;
    const Vec3& p = other.state.pos;
    AABB pb{{p.x - kPlayerRadius, p.y + 0.05f, p.z - kPlayerRadius}, {p.x + kPlayerRadius, p.y + kPlayerHeight, p.z + kPlayerRadius}};
    for (const auto& b : boxes)
      if (b.solid && overlaps(b.box, pb)) {
        warn(c, "A player is in the way");
        return;
      }
  }
  if (cost.item != Item::None && !removeItem(c.inv, cost.item, cost.count)) {
    warn(c, "Needs " + std::to_string(cost.count) + " " + itemDef(cost.item).name);
    return;
  }
  if (needItem != Item::None) removeItem(c.inv, needItem, 1);
  o.id = nextObjectId_++;
  o.owner = c.id;
  o.tier = Tier::Twig;
  o.hp = maxHp(type, Tier::Twig);
  world_.upsert(o);
  if (int size = containerSize(type)) containers_[o.id] = Slots(static_cast<size_t>(size));
  objectChanged(o);
  c.invDirty = true;
}

void Server::doInteract(Client& c, uint8_t kind, uint32_t id) {
  if (c.vitals.dead) return;
  const Vec3 eye = c.state.pos + Vec3{0, kEyeHeight, 0};
  if (kind == 0) {
    const WorldObject* o = world_.find(id);
    if (!o || length(o->pos + Vec3{0, 0.8f, 0} - eye) > kInteractRange + 1.0f) return;
    switch (o->type) {
      case ObjType::Door: {
        if (o->owner != c.id) {
          warn(c, "The door is locked");
          return;
        }
        WorldObject d = *o;
        d.flags ^= kDoorOpen;
        world_.upsert(d);
        objectChanged(d);
        break;
      }
      case ObjType::StorageBox:
      case ObjType::LootBag:
        c.openContainer = id;
        sendContainer(c);
        break;
      case ObjType::SleepingBag:
        sendText(c, TextKind::Info, o->owner == c.id ? "Your sleeping bag. You can respawn here." : "Someone's sleeping bag.");
        break;
      default:
        break;
    }
    return;
  }
  // Pick up a plant.
  auto node = nodeById(terrain_, id);
  if (!node) return;
  const NodeInfo& info = nodeInfo(node->type);
  if (!info.pickup || length(node->pos + Vec3{0, 0.5f, 0} - eye) > kInteractRange) return;
  NodeRuntime& rt = nodes_[id];
  if (rt.respawnAt > 0) return;
  int left = addItem(c.inv, info.yield, info.pickupCount);
  if (left == info.pickupCount) {
    warn(c, "Inventory full");
    return;
  }
  c.invDirty = true;
  ByteWriter g;
  g.u8(static_cast<uint8_t>(Msg::Gathered));
  g.u16(static_cast<uint16_t>(info.yield));
  g.u16(static_cast<uint16_t>(info.pickupCount - left));
  queue(c, g);
  rt.respawnAt = time_ + info.respawn;
  ByteWriter w;
  w.u8(static_cast<uint8_t>(Msg::NodeState));
  w.u32(id);
  w.u8(1);
  broadcast(w);
}

void Server::updateWorld() {
  for (auto it = nodes_.begin(); it != nodes_.end();) {
    if (it->second.respawnAt > 0 && time_ >= it->second.respawnAt) {
      ByteWriter w;
      w.u8(static_cast<uint8_t>(Msg::NodeState));
      w.u32(it->first);
      w.u8(0);
      broadcast(w);
      it = nodes_.erase(it);
    } else {
      ++it;
    }
  }
  std::vector<uint32_t> expired;
  for (const auto& [id, t] : bagExpiry_)
    if (time_ >= t) expired.push_back(id);
  for (uint32_t id : expired) {
    bagExpiry_.erase(id);
    containers_.erase(id);
    if (world_.remove(id)) objectRemoved(id);
  }
}

Server::Client* Server::findClient(uint32_t id) {
  for (auto& [addr, c] : clients_)
    if (c.id == id) return &c;
  return nullptr;
}

void Server::storeRecord(const Client& c) {
  PlayerRecord& r = records_[c.name];
  r.id = c.id;
  r.name = c.name;
  r.state = c.state;
  r.vitals = c.vitals;
  r.inv = c.inv;
  r.crafting = c.crafting;
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

// --- saving ------------------------------------------------------------------------------
bool Server::save() const {
  if (cfg_.savePath.empty()) return false;
  ByteWriter w;
  w.u32(kSaveMagic);
  w.u32(kSaveVersion);
  w.u32(cfg_.terrainSeed);
  w.u32(nextObjectId_);
  w.u32(nextPlayerId_);
  w.u32(static_cast<uint32_t>(world_.objects().size()));
  for (const auto& [id, o] : world_.objects()) writeObject(w, o);
  w.u32(static_cast<uint32_t>(containers_.size()));
  for (const auto& [id, s] : containers_) {
    w.u32(id);
    writeSlots(w, s);
  }
  std::unordered_map<std::string, PlayerRecord> all = records_;
  for (const auto& [addr, c] : clients_) all[c.name] = {c.id, c.name, c.state, c.vitals, c.inv, c.crafting};
  w.u32(static_cast<uint32_t>(all.size()));
  for (const auto& [name, r] : all) {
    w.u32(r.id);
    w.str(r.name, kMaxNameLength);
    w.vec3(r.state.pos);
    w.f32(r.state.yaw);
    w.f32(r.vitals.health);
    w.f32(r.vitals.food);
    w.f32(r.vitals.saturation);
    w.u8(r.vitals.dead ? 1 : 0);
    writeSlots(w, r.inv);
    w.u8(static_cast<uint8_t>(std::min<size_t>(r.crafting.size(), 255)));
    for (size_t k = 0; k < r.crafting.size() && k < 255; ++k) {
      w.u16(r.crafting[k].recipe);
      w.f32(r.crafting[k].remaining);
      w.f32(r.crafting[k].total);
    }
  }
  // Used-up resource nodes and loot bags keep their timers across restarts.
  w.u32(static_cast<uint32_t>(nodes_.size()));
  for (const auto& [id, n] : nodes_) {
    w.u32(id);
    w.f32(n.remaining);
    w.f32(n.respawnAt > 0 ? static_cast<float>(std::max(1.0, n.respawnAt - time_)) : 0.0f);
  }
  w.u32(static_cast<uint32_t>(bagExpiry_.size()));
  for (const auto& [id, t] : bagExpiry_) {
    w.u32(id);
    w.f32(static_cast<float>(std::max(1.0, t - time_)));
  }
  // v3: ranks, bans, mutes, time of day
  w.u32(static_cast<uint32_t>(ranks_.size()));
  for (const auto& [name, r] : ranks_) {
    w.str(name, kMaxNameLength);
    w.u8(static_cast<uint8_t>(r));
  }
  w.u32(static_cast<uint32_t>(bans_.size()));
  for (const auto& name : bans_) w.str(name, kMaxNameLength);
  w.u32(static_cast<uint32_t>(mutes_.size()));
  for (const auto& name : mutes_) w.str(name, kMaxNameLength);
  w.f32(timeOfDay_);

  const std::string tmp = cfg_.savePath + ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) {
      std::fprintf(stderr, "[server] could not write %s\n", tmp.c_str());
      return false;
    }
    f.write(reinterpret_cast<const char*>(w.data().data()), static_cast<std::streamsize>(w.size()));
    if (!f) return false;
  }
  std::error_code ec;
  std::filesystem::rename(tmp, cfg_.savePath, ec);  // atomic replace: a crash never leaves half a save
  if (ec) {
    std::fprintf(stderr, "[server] could not replace %s: %s\n", cfg_.savePath.c_str(), ec.message().c_str());
    return false;
  }
  std::printf("[server] saved %zu objects, %zu players to %s\n", world_.objects().size(), all.size(),
              cfg_.savePath.c_str());
  return true;
}

bool Server::load() {
  std::ifstream f(cfg_.savePath, std::ios::binary);
  if (!f) {
    std::printf("[server] no save at %s, starting a fresh world\n", cfg_.savePath.c_str());
    return false;
  }
  std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  ByteReader r(data.data(), data.size());
  uint32_t magic = 0, version = 0, seed = 0, objCount = 0, contCount = 0, playerCount = 0;
  if (!(r.u32(magic) && magic == kSaveMagic && r.u32(version) && version >= 1 && version <= kSaveVersion &&
        r.u32(seed))) {
    std::fprintf(stderr, "[server] %s is not a valid save, ignoring it\n", cfg_.savePath.c_str());
    return false;
  }
  if (seed != cfg_.terrainSeed) {
    std::fprintf(stderr, "[server] %s is for map seed %u, not %u; ignoring it\n", cfg_.savePath.c_str(), seed,
                 cfg_.terrainSeed);
    return false;
  }
  World world;
  std::unordered_map<uint32_t, Slots> containers;
  std::unordered_map<std::string, PlayerRecord> records;
  uint32_t nextObj = 1, nextPlayer = 1;
  bool ok = r.u32(nextObj) && r.u32(nextPlayer) && r.u32(objCount) && objCount <= kMaxObjects;
  for (uint32_t i = 0; ok && i < objCount; ++i) {
    WorldObject o;
    ok = readObject(r, o);
    if (ok) world.upsert(o);
  }
  ok = ok && r.u32(contCount);
  for (uint32_t i = 0; ok && i < contCount; ++i) {
    uint32_t id;
    Slots s;
    ok = r.u32(id) && readSlots(r, s);
    if (ok) containers[id] = std::move(s);
  }
  ok = ok && r.u32(playerCount);
  for (uint32_t i = 0; ok && i < playerCount; ++i) {
    PlayerRecord rec;
    uint8_t dead = 0;
    ok = r.u32(rec.id) && r.str(rec.name, kMaxNameLength) && r.vec3(rec.state.pos) && r.f32(rec.state.yaw) &&
         r.f32(rec.vitals.health) && r.f32(rec.vitals.food) && r.f32(rec.vitals.saturation) && r.u8(dead) &&
         readSlots(r, rec.inv);
    if (!ok) break;
    if (version >= 2) {
      uint8_t jobs = 0;
      ok = r.u8(jobs);
      for (uint8_t k = 0; ok && k < jobs; ++k) {
        CraftJob j{};
        ok = r.u16(j.recipe) && r.f32(j.remaining) && r.f32(j.total) && j.recipe < recipes().size();
        if (ok) rec.crafting.push_back(j);
      }
      if (!ok) break;
    }
    rec.vitals.dead = dead != 0;
    rec.state.onGround = true;
    records[rec.name] = std::move(rec);
  }
  std::unordered_map<uint32_t, NodeRuntime> nodes;
  std::unordered_map<uint32_t, double> bagTimes;
  if (ok && version >= 2) {
    uint32_t n = 0;
    ok = r.u32(n);
    for (uint32_t i = 0; ok && i < n; ++i) {
      uint32_t id = 0;
      float remaining = 0, respawnIn = 0;
      ok = r.u32(id) && r.f32(remaining) && r.f32(respawnIn);
      if (ok) nodes[id] = {remaining, respawnIn > 0 ? time_ + respawnIn : 0.0};
    }
    ok = ok && r.u32(n);
    for (uint32_t i = 0; ok && i < n; ++i) {
      uint32_t id;
      float expiresIn = 0;
      ok = r.u32(id) && r.f32(expiresIn);
      if (ok) bagTimes[id] = time_ + expiresIn;
    }
  }
  std::unordered_map<std::string, Rank> ranks;
  std::unordered_set<std::string> bans, mutes;
  float tod = timeOfDay_;
  if (ok && version >= 3) {
    uint32_t n = 0;
    ok = r.u32(n);
    for (uint32_t i = 0; ok && i < n; ++i) {
      std::string name;
      uint8_t rk = 0;
      ok = r.str(name, kMaxNameLength) && r.u8(rk) && rk <= 3;
      if (ok) ranks[name] = static_cast<Rank>(rk);
    }
    for (auto* set : {&bans, &mutes}) {
      ok = ok && r.u32(n);
      for (uint32_t i = 0; ok && i < n; ++i) {
        std::string name;
        ok = r.str(name, kMaxNameLength);
        if (ok) set->insert(name);
      }
    }
    ok = ok && r.f32(tod);
  }
  if (!ok) {
    std::fprintf(stderr, "[server] %s is damaged, starting a fresh world\n", cfg_.savePath.c_str());
    return false;
  }
  world_ = std::move(world);
  containers_ = std::move(containers);
  records_ = std::move(records);
  nextObjectId_ = nextObj;
  nextPlayerId_ = nextPlayer;
  nodes_ = std::move(nodes);
  ranks_ = std::move(ranks);
  bans_ = std::move(bans);
  mutes_ = std::move(mutes);
  timeOfDay_ = std::fmod(std::fabs(tod), 24.0f);
  for (const auto& [id, o] : world_.objects())
    if (o.type == ObjType::LootBag) bagExpiry_[id] = bagTimes.count(id) ? bagTimes[id] : time_ + kLootBagLifetime;
  std::printf("[server] loaded %zu objects and %zu players from %s\n", world_.objects().size(), records_.size(),
              cfg_.savePath.c_str());
  return true;
}

// --- test hooks ------------------------------------------------------------------------------
PlayerState* Server::debugState(uint32_t id) {
  Client* c = findClient(id);
  return c ? &c->state : nullptr;
}
Slots* Server::debugInventory(uint32_t id) {
  Client* c = findClient(id);
  if (c) c->invDirty = true;
  return c ? &c->inv : nullptr;
}
Vitals* Server::debugVitals(uint32_t id) {
  Client* c = findClient(id);
  return c ? &c->vitals : nullptr;
}

}  // namespace sg
