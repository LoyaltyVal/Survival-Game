// Unit + integration tests. No framework: each CHECK prints on failure.
#include <cmath>
#include <cstdio>
#include <limits>
#include <random>
#include <string>

#include "net_client.h"
#include "protocol.h"
#include "reliable.h"
#include "server.h"

using namespace sg;

static int g_failures = 0;
static int g_checks = 0;
#define CHECK(cond)                                                          \
  do {                                                                       \
    ++g_checks;                                                              \
    if (!(cond)) {                                                           \
      ++g_failures;                                                          \
      std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);   \
    }                                                                        \
  } while (0)

static bool approxEq(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

static void testProtocolRoundTrip() {
  SnapshotPacket s;
  s.serverTick = 42;
  s.lastProcessedInput = 7;
  s.self.pos = {1, 2, 3};
  s.self.vel = {4, 5, 6};
  s.self.yaw = 0.5f;
  s.self.onGround = true;
  s.self.stamina = 42.5f;
  s.self.exhausted = true;
  s.vitals.health = 55;
  s.vitals.food = 33;
  s.others.push_back({9, {10, 11, 12}, 1.0f, -0.2f, Item::Pistol, kEntityAttacking});
  auto bytes = encode(s);
  SnapshotPacket d;
  CHECK(bytes.size() <= kMaxPacketSize);
  CHECK(decode(bytes.data(), bytes.size(), d));
  CHECK(d.serverTick == 42 && d.lastProcessedInput == 7);
  CHECK(approxEq(d.self.pos.z, 3) && approxEq(d.self.vel.y, 5) && d.self.onGround);
  CHECK(approxEq(d.self.stamina, 42.5f) && d.self.exhausted && !d.self.starving);
  CHECK(approxEq(d.vitals.health, 55) && approxEq(d.vitals.food, 33));
  CHECK(d.others.size() == 1 && d.others[0].id == 9 && approxEq(d.others[0].pos.y, 11));
  CHECK(d.others[0].held == Item::Pistol && d.others[0].flags == kEntityAttacking);

  // A full snapshot must still fit in one packet.
  SnapshotPacket big;
  for (int i = 0; i < kMaxSnapshotEntities; ++i) big.others.push_back({static_cast<uint32_t>(i), {}, 0, 0, Item::Rifle, 3});
  CHECK(encode(big).size() <= kMaxPacketSize);

  HelloPacket h{kProtocolVersion, "Ramiz"};
  auto hb = encode(h);
  HelloPacket hd;
  CHECK(decode(hb.data(), hb.size(), hd) && hd.name == "Ramiz");

  InputPacket ip;
  for (uint32_t i = 1; i <= 6; ++i) ip.inputs.push_back({i, 1, 0, 0, 0, 0, 3});
  auto ib = encode(ip);
  InputPacket id;
  CHECK(decode(ib.data(), ib.size(), id));
  CHECK(id.inputs.size() == static_cast<size_t>(kMaxInputsPerPacket));
  CHECK(id.inputs.back().seq == 6 && id.inputs.back().slot == 3);  // newest kept

  InfoRequestPacket rq{1234};
  auto rb = encode(rq);
  InfoRequestPacket rd;
  CHECK(rb.size() >= kInfoRequestSize && decode(rb.data(), rb.size(), rd) && rd.nonce == 1234);
  CHECK(!decode(rb.data(), 20, rd));  // short requests are ignored (no amplification)
}

static void testProtocolRejectsGarbage() {
  auto bytes = encode(SnapshotPacket{});
  SnapshotPacket d;
  CHECK(!decode(bytes.data(), bytes.size() - 1, d));  // truncated
  bytes[0] ^= 0xFF;
  CHECK(!decode(bytes.data(), bytes.size(), d));  // bad magic
  CHECK(!peekType(bytes.data(), bytes.size()));

  InputPacket ip;
  ip.inputs.push_back({1, std::numeric_limits<float>::quiet_NaN(), 0, 0, 0, 0, 0});
  auto ib = encode(ip);
  InputPacket id;
  CHECK(!decode(ib.data(), ib.size(), id));  // NaN refused at the wire

  // Slots with an item id that doesn't exist, or too many in a stack.
  ByteWriter w;
  w.u8(1);
  w.u16(9999);
  w.u16(1);
  Slots s;
  ByteReader r(w.data().data(), w.size());
  CHECK(!readSlots(r, s));
  ByteWriter w2;
  w2.u8(1);
  w2.u16(static_cast<uint16_t>(Item::Rifle));
  w2.u16(50);  // rifles stack to 1
  ByteReader r2(w2.data().data(), w2.size());
  CHECK(!readSlots(r2, s));
}

static void testSanitize() {
  PlayerInput in{1, 1, 1, 0, 9.0f, 0xFF, 77};
  auto s = sanitizeInput(in);
  CHECK(approxEq(std::sqrt(s.moveX * s.moveX + s.moveZ * s.moveZ), 1.0f));  // no diagonal boost
  CHECK(approxEq(s.pitch, kMaxPitch));
  CHECK(s.buttons == (kButtonJump | kButtonSprint | kButtonPrimary | kButtonReload | kButtonAim));
  CHECK(s.slot < kHotbarSlots);
}

static void testMovement() {
  Terrain t(1337);
  PlayerState a;
  a.pos = {2048, t.heightAt(2048, 2048), 2048};
  a.onGround = true;
  PlayerState b = a;
  PlayerInput fwd{0, 0, 1, 0, 0, 0, 0};
  for (int i = 0; i < 60; ++i) { applyInput(a, fwd, t); applyInput(b, fwd, t); }
  float moved = std::hypot(a.pos.x - 2048, a.pos.z - 2048);
  CHECK(moved > 4.0f && moved < kWalkSpeed + 0.01f);  // ~5 m in 1 s after accelerating
  CHECK(a.pos.x == b.pos.x && a.pos.y == b.pos.y && a.pos.z == b.pos.z);  // deterministic
  CHECK(a.pos.y >= t.heightAt(a.pos.x, a.pos.z) - 1e-3f);                // not under ground

  PlayerInput jump{0, 0, 0, 0, 0, kButtonJump, 0};
  applyInput(a, jump, t);
  CHECK(!a.onGround && a.vel.y > 0);
}

static void testStamina() {
  Terrain t(1337);
  PlayerState s;
  s.pos = {2048, t.heightAt(2048, 2048), 2048};
  s.onGround = true;
  PlayerInput sprint{0, 0, 1, 0, 0, kButtonSprint, 0};
  for (int i = 0; i < 60 * 9; ++i) applyInput(s, sprint, t);  // 9 s of sprinting
  CHECK(s.exhausted);
  CHECK(s.stamina < kExhaustRecover);
  // Exhausted: can't sprint, so speed falls back to walking.
  float hs = std::hypot(s.vel.x, s.vel.z);
  CHECK(hs <= kWalkSpeed + 0.01f);
  PlayerInput rest{0, 0, 0, 0, 0, 0, 0};
  for (int i = 0; i < 60 * 6; ++i) applyInput(s, rest, t);
  CHECK(!s.exhausted && s.stamina > 90);
  // Starving halves regen.
  PlayerState a = s, b = s;
  a.stamina = b.stamina = 10;
  a.staminaDelay = b.staminaDelay = 0;
  b.starving = true;
  for (int i = 0; i < 60; ++i) { applyInput(a, rest, t); applyInput(b, rest, t); }
  CHECK(a.stamina > b.stamina + 5);
}

static void testCameraMath() {
  // A point straight ahead of the camera must land in the middle of the screen.
  Vec3 eye{100, 20, 100};
  float yaw = 0.7f, pitch = -0.3f;
  Vec3 dir = directionFromYawPitch(yaw, pitch);
  Mat4 vp = Mat4::perspective(1.2f, 16.0f / 9.0f, 0.1f, 1000.0f) * Mat4::lookAt(eye, eye + dir, {0, 1, 0});
  Vec3 p = eye + dir * 50.0f;
  float x = vp.at(0, 0) * p.x + vp.at(0, 1) * p.y + vp.at(0, 2) * p.z + vp.at(0, 3);
  float y = vp.at(1, 0) * p.x + vp.at(1, 1) * p.y + vp.at(1, 2) * p.z + vp.at(1, 3);
  float z = vp.at(2, 0) * p.x + vp.at(2, 1) * p.y + vp.at(2, 2) * p.z + vp.at(2, 3);
  float w = vp.at(3, 0) * p.x + vp.at(3, 1) * p.y + vp.at(3, 2) * p.z + vp.at(3, 3);
  CHECK(w > 0);
  CHECK(approxEq(x / w, 0, 1e-3f) && approxEq(y / w, 0, 1e-3f));
  CHECK(z / w > -1 && z / w < 1);
  Vec3 camFwd = directionFromYawPitch(yaw, 0);
  CHECK(approxEq(camFwd.x, -std::sin(yaw)) && approxEq(camFwd.z, -std::cos(yaw)));
  // The view model transform (rotateY * rotateX) points -Z along the look direction.
  Mat4 m = Mat4::rotateY(yaw) * Mat4::rotateX(pitch);
  Vec3 f{-m.at(0, 2), -m.at(1, 2), -m.at(2, 2)};
  CHECK(approxEq(f.x, dir.x, 1e-4f) && approxEq(f.y, dir.y, 1e-4f) && approxEq(f.z, dir.z, 1e-4f));
}

static void testTerrainAndNodes() {
  Terrain t(1337), u(1337), v(99);
  CHECK(t.heightAt(1000, 1234) == u.heightAt(1000, 1234));
  CHECK(t.heightAt(1000, 1234) != v.heightAt(1000, 1234));
  CHECK(t.heightAt(5, 5) < kSeaLevel);  // map corner is sea

  // Nodes are deterministic and every type exists somewhere on the island.
  std::vector<ResourceNode> a, b;
  nodesInRect(t, 1000, 1000, 3000, 3000, a);
  nodesInRect(u, 1000, 1000, 3000, 3000, b);
  CHECK(!a.empty() && a.size() == b.size());
  int counts[16] = {};
  for (const auto& n : a) counts[static_cast<int>(n.type)]++;
  for (int k = 1; k <= static_cast<int>(NodeType::Scrap); ++k) {
    if (counts[k] == 0) std::printf("  no nodes of type %d in the test area\n", k);
    CHECK(counts[k] > 0);
  }
  auto byId = nodeById(t, a[a.size() / 2].id);
  CHECK(byId && byId->type == a[a.size() / 2].type && approxEq(byId->pos.x, a[a.size() / 2].pos.x));
}

static void testInventory() {
  Slots s(kInventorySlots);
  CHECK(addItem(s, Item::Wood, 2500) == 0);  // 1000 + 1000 + 500
  CHECK(countItem(s, Item::Wood) == 2500);
  CHECK(s[0].count == 1000 && s[2].count == 500);
  CHECK(!removeItem(s, Item::Stone, 1));
  CHECK(removeItem(s, Item::Wood, 1200) && countItem(s, Item::Wood) == 1300);
  // Fill everything: overflow is reported.
  Slots small(2);
  CHECK(addItem(small, Item::Rifle, 3) == 1);
  // Move: into empty, merge, swap.
  Slots inv(kInventorySlots);
  addItem(inv, Item::Stone, 10);
  CHECK(moveItem(inv, 0, inv, 10, 4) && inv[0].count == 6 && inv[10].count == 4);
  CHECK(moveItem(inv, 10, inv, 0, 0) && inv[0].count == 10 && inv[10].empty());
  inv[5] = {Item::Rock, 1};
  CHECK(moveItem(inv, 0, inv, 5, 0) && inv[5].item == Item::Stone && inv[0].item == Item::Rock);
  CHECK(!moveItem(inv, 5, inv, 0, 3));  // half a stack can't swap with another item
  // Quick move hotbar -> main grid.
  CHECK(moveItem(inv, 0, inv, kAutoSlot, 0) && inv[0].empty() && countItem(inv, Item::Rock) == 1);
  // Between containers.
  Slots box(18);
  CHECK(moveItem(inv, 5, box, kAutoSlot, 0) && countItem(box, Item::Stone) == 10 && countItem(inv, Item::Stone) == 0);
}

static void testRecipes() {
  Slots s(kInventorySlots);
  const Recipe* hatchet = nullptr;
  for (const auto& r : recipes())
    if (r.output == Item::StoneHatchet) hatchet = &r;
  CHECK(hatchet != nullptr);
  CHECK(!canAfford(s, *hatchet, 1));
  addItem(s, Item::Wood, 400);
  addItem(s, Item::Stone, 150);
  CHECK(canAfford(s, *hatchet, 1) && !canAfford(s, *hatchet, 2));
  // Every recipe's ingredients and outputs are real items.
  for (const auto& r : recipes()) {
    CHECK(r.output != Item::None && r.outCount > 0 && r.outCount <= itemDef(r.output).maxStack);
    CHECK(r.in[0].item != Item::None);
  }
}

static void testBuildingRules() {
  Terrain t(1337);
  World w;
  // Find a flat-ish dry spot near the island centre.
  Vec3 spot;
  bool found = false;
  for (float x = 2000; x < 2300 && !found; x += kGrid)
    for (float z = 2000; z < 2300 && !found; z += kGrid) {
      float cx = std::floor(x / kGrid) * kGrid + kGrid * 0.5f, cz = std::floor(z / kGrid) * kGrid + kGrid * 0.5f;
      float h = t.heightAt(cx, cz);
      if (h > 3 && t.normalAt(cx, cz).y > 0.97f) {
        spot = {cx, h, cz};
        found = true;
      }
    }
  CHECK(found);
  Vec3 eye = spot + Vec3{0, 1.65f, 5.0f};
  Vec3 dir = normalize(spot - eye);
  Placement f = computePlacement(ObjType::Foundation, t, w, eye, dir, 0);
  CHECK(f.error == nullptr);
  WorldObject fo{1, ObjType::Foundation, Tier::Twig, 7, f.pos, 0, 10, 0};
  w.upsert(fo);
  CHECK(checkPlacement(ObjType::Foundation, f.pos, 0, t, w) != nullptr);  // occupied
  // A wall on its north edge is supported; one floating in the air isn't.
  Vec3 wallPos{f.pos.x, f.pos.y, f.pos.z - kGrid * 0.5f};
  CHECK(checkPlacement(ObjType::Wall, wallPos, 0, t, w) == nullptr);
  CHECK(checkPlacement(ObjType::Wall, wallPos + Vec3{0, 6, 0}, 0, t, w) != nullptr);
  CHECK(checkPlacement(ObjType::Wall, wallPos + Vec3{0.7f, 0, 0}, 0, t, w) != nullptr);  // off grid
  WorldObject wall{2, ObjType::Doorway, Tier::Twig, 7, wallPos, 0, 10, 0};
  w.upsert(wall);
  CHECK(checkPlacement(ObjType::Wall, wallPos, 0, t, w) != nullptr);  // edge taken
  CHECK(checkPlacement(ObjType::Door, wallPos, 0, t, w) == nullptr);  // doorway accepts a door
  // A floor on top of the doorway, a box on the foundation.
  CHECK(checkPlacement(ObjType::Floor, f.pos + Vec3{0, kWallHeight, 0}, 0, t, w) == nullptr);
  CHECK(checkPlacement(ObjType::Floor, f.pos + Vec3{0, 5.0f, 0}, 0, t, w) != nullptr);
  CHECK(checkPlacement(ObjType::StorageBox, f.pos + Vec3{0.5f, 0, 0.5f}, 0, t, w) == nullptr);

  // Collision: walking north into the doorway pillar is blocked, through the gap is not.
  PlayerState p;
  p.pos = {wallPos.x - 1.0f, f.pos.y, wallPos.z + 1.0f};
  p.onGround = true;
  PlayerInput north{0, 0, 1, 0, 0, 0, 0};  // yaw 0 = -Z
  for (int i = 0; i < 90; ++i) applyInput(p, north, t, &w);
  CHECK(p.pos.z > wallPos.z);  // stopped by the pillar
  CHECK(approxEq(p.pos.y, f.pos.y, 0.05f));  // standing on the foundation
  PlayerState q;
  q.pos = {wallPos.x, f.pos.y, wallPos.z + 1.0f};
  q.onGround = true;
  for (int i = 0; i < 90; ++i) applyInput(q, north, t, &w);
  CHECK(q.pos.z < wallPos.z - 0.5f);  // walked through the doorway
  // A closed door blocks the gap.
  WorldObject door{3, ObjType::Door, Tier::Twig, 7, wallPos, 0, 200, 0};
  w.upsert(door);
  PlayerState r;
  r.pos = {wallPos.x, f.pos.y, wallPos.z + 1.0f};
  r.onGround = true;
  for (int i = 0; i < 90; ++i) applyInput(r, north, t, &w);
  CHECK(r.pos.z > wallPos.z);
  // Raycast hits the door.
  auto hit = w.raycast({wallPos.x, f.pos.y + 1.2f, wallPos.z + 2}, {0, 0, -1}, 5, false);
  CHECK(hit && hit->objectId == 3);
}

static void testReliableChannel() {
  // Two channels talking through a lossy, reordering "network".
  ReliableChannel a, b;
  std::mt19937 rng(42);
  std::uniform_real_distribution<float> roll(0, 1);
  std::vector<std::vector<uint8_t>> got;
  const int kCount = 500;
  for (int i = 0; i < kCount; ++i) {
    std::vector<uint8_t> m(1 + i % 40, static_cast<uint8_t>(i));
    m[0] = static_cast<uint8_t>(i & 0xFF);
    a.queue(m);
  }
  for (int round = 0; round < 400 && got.size() < static_cast<size_t>(kCount); ++round) {
    auto packets = a.build(3);
    if (roll(rng) < 0.5f && packets.size() > 1) std::swap(packets[0], packets[1]);
    for (auto& p : packets)
      if (roll(rng) > 0.3f) b.receive(p.data(), p.size(), got);  // 30% loss
    if (b.needsSend())
      for (auto& p : b.build(1))
        if (roll(rng) > 0.3f) {
          std::vector<std::vector<uint8_t>> none;
          a.receive(p.data(), p.size(), none);
        }
  }
  CHECK(got.size() == static_cast<size_t>(kCount));
  bool inOrder = true;
  for (int i = 0; i < static_cast<int>(got.size()); ++i)
    if (got[i][0] != static_cast<uint8_t>(i & 0xFF) || got[i].size() != static_cast<size_t>(1 + i % 40)) inOrder = false;
  CHECK(inOrder);
}

// Connects a NetClient to a server driven by hand.
static bool join(Server& server, NetClient& client, uint16_t port, const char* name, double& now) {
  if (!client.connect("127.0.0.1", port, name)) return false;
  for (int i = 0; i < 20 && client.status() != NetClient::Status::Connected; ++i) {
    client.update(now);
    server.tick(1.0 / 30);
    now += 1.0 / 30;
  }
  client.update(now);
  return client.status() == NetClient::Status::Connected;
}

static void pump(Server& server, NetClient& client, double& now, int ticks, PlayerInput in = {}) {
  for (int t = 0; t < ticks; ++t) {
    for (int k = 0; k < 2; ++k) client.sendInput(in);
    server.tick(1.0 / 30);
    now += 1.0 / 30;
    client.update(now);
  }
}

// Full loop over real UDP on localhost: join, walk, check prediction matches.
static void testServerAndClient() {
  ServerConfig cfg;
  cfg.port = 27901;
  Server server(cfg);
  CHECK(server.start());

  NetClient client;
  double now = 0;
  CHECK(join(server, client, cfg.port, "tester", now));
  CHECK(server.playerCount() == 1);

  pump(server, client, now, 60);
  CHECK(countItem(client.inventory(), Item::Rock) == 1);  // starting kit arrived reliably
  CHECK(client.predicted().stamina > kMaxStamina - 0.01f);  // standing still costs nothing

  Vec3 start = client.predicted().pos;
  float maxCorrection = 0;
  for (int t = 0; t < 90; ++t) {        // 3 s
    for (int k = 0; k < 2; ++k)          // 60 Hz input vs 30 Hz tick
      client.sendInput({0, 0.3f, 1, 0.2f, 0, kButtonSprint, 0});
    server.tick(1.0 / 30);
    now += 1.0 / 30;
    client.update(now);
    if (t > 5) maxCorrection = std::fmax(maxCorrection, client.lastCorrection());
  }
  float moved = std::hypot(client.predicted().pos.x - start.x, client.predicted().pos.z - start.z);
  std::printf("  client moved %.1f m in 3 s, max prediction error %.4f m\n", moved, maxCorrection);
  CHECK(moved > 15.0f);
  CHECK(maxCorrection < 0.01f);  // prediction agrees with the server
  CHECK(client.predicted().stamina < kMaxStamina);  // sprinting cost stamina

  client.disconnect();
  server.tick(1.0 / 30);
  CHECK(server.playerCount() == 0);
  server.stop();
}

// Gather wood from a real tree, craft a building plan, build a foundation, eat.
static void testGameplayLoop() {
  ServerConfig cfg;
  cfg.port = 27903;
  Server server(cfg);
  CHECK(server.start());
  NetClient client;
  double now = 0;
  CHECK(join(server, client, cfg.port, "builder", now));
  pump(server, client, now, 5);
  const uint32_t me = client.playerId();

  // Stand next to a tree, facing it.
  Terrain t(cfg.terrainSeed);
  std::vector<ResourceNode> nodes;
  nodesInRect(t, 1500, 1500, 2600, 2600, nodes);
  const ResourceNode* tree = nullptr;
  for (const auto& n : nodes)
    if (n.type == NodeType::Tree) { tree = &n; break; }
  CHECK(tree != nullptr);
  if (!tree) return;
  PlayerState* st = server.debugState(me);
  st->pos = {tree->pos.x, t.heightAt(tree->pos.x, tree->pos.z + 1.2f), tree->pos.z + 1.2f};
  st->vel = {};
  pump(server, client, now, 3);
  // Aim at the trunk: yaw 0 looks toward -Z.
  Vec3 eye = client.predicted().pos + Vec3{0, kEyeHeight, 0};
  Vec3 target = tree->pos + Vec3{0, 1.0f, 0};
  float pitch = std::atan2(target.y - eye.y, std::hypot(target.x - eye.x, target.z - eye.z));
  float yaw = std::atan2(-(target.x - eye.x), -(target.z - eye.z));
  PlayerInput chop{0, 0, 0, yaw, pitch, kButtonPrimary, 0};  // slot 0 = rock
  pump(server, client, now, 30 * 6, chop);                   // 6 s of chopping
  int wood = countItem(client.inventory(), Item::Wood);
  std::printf("  chopped %d wood in 6 s with a rock\n", wood);
  CHECK(wood >= 25);

  // Craft a building plan (20 wood, 1 s).
  int planRecipe = -1;
  for (int i = 0; i < static_cast<int>(recipes().size()); ++i)
    if (recipes()[i].output == Item::BuildingPlan) planRecipe = i;
  client.craft(planRecipe, 1);
  pump(server, client, now, 60);
  CHECK(countItem(client.inventory(), Item::BuildingPlan) == 1);

  // Give enough wood to build, then place a foundation where the client says.
  addItem(*server.debugInventory(me), Item::Wood, 200);
  pump(server, client, now, 5);
  Vec3 e2 = client.predicted().pos + Vec3{0, kEyeHeight, 0};
  Vec3 aim = normalize(Vec3{0, -1.0f, 1.0f});  // down and behind us, away from the tree
  Placement p = computePlacement(ObjType::Foundation, client.terrain(), client.world(), e2, aim, 0);
  if (p.error) std::printf("  placement error: %s\n", p.error);
  CHECK(p.error == nullptr);
  client.place(p.type, p.pos, p.rot, Item::BuildingPlan);
  pump(server, client, now, 10);
  CHECK(server.world().objects().size() == 1);
  CHECK(client.world().objects().size() == 1);  // the client heard about it

  // Hunger: eat berries from the hotbar.
  Vitals* v = server.debugVitals(me);
  v->food = 40;
  v->saturation = 0;
  int berries = countItem(client.inventory(), Item::Berries);
  int berrySlot = -1;
  for (int i = 0; i < kHotbarSlots; ++i)
    if (client.inventory()[i].item == Item::Berries) berrySlot = i;
  CHECK(berrySlot >= 0);
  pump(server, client, now, 10, PlayerInput{0, 0, 0, 0, 0, kButtonPrimary, static_cast<uint8_t>(berrySlot)});
  CHECK(countItem(client.inventory(), Item::Berries) < berries);
  CHECK(client.vitals().food > 40 && client.vitals().saturation > 0);

  // Starving kills, and the items end up in a loot bag.
  v->food = 0;
  v->health = 1;
  pump(server, client, now, 60);
  CHECK(client.vitals().dead);
  bool bag = false;
  for (const auto& [id, o] : server.world().objects())
    if (o.type == ObjType::LootBag) bag = true;
  CHECK(bag);
  client.respawn(false);
  pump(server, client, now, 10);
  CHECK(!client.vitals().dead && countItem(client.inventory(), Item::Rock) == 1);
  server.stop();
}

// Everything survives a server restart: buildings, boxes, player inventory, crafting, used-up nodes.
static void testSaveAndReload() {
  const std::string path = "sg_test_world.sav";
  std::remove(path.c_str());
  ServerConfig cfg;
  cfg.port = 27904;
  cfg.savePath = path;
  uint32_t me = 0;
  {
    Server server(cfg);
    CHECK(server.start());
    NetClient client;
    double now = 0;
    CHECK(join(server, client, cfg.port, "saver", now));
    me = client.playerId();
    Slots* inv = server.debugInventory(me);
    addItem(*inv, Item::Wood, 777);
    addItem(*inv, Item::Cloth, 40);
    pump(server, client, now, 5);
    int bagRecipe = -1;
    for (int i = 0; i < static_cast<int>(recipes().size()); ++i)
      if (recipes()[i].output == Item::SleepingBag) bagRecipe = i;
    client.craft(bagRecipe, 1);  // 3 s craft, still queued at shutdown
    pump(server, client, now, 10);
    // Place a foundation in front of us.
    Vec3 e = client.predicted().pos + Vec3{0, kEyeHeight, 0};
    addItem(*server.debugInventory(me), Item::BuildingPlan, 1);
    pump(server, client, now, 3);
    Placement p = computePlacement(ObjType::Foundation, client.terrain(), client.world(), e, normalize(Vec3{0, -0.4f, -1}), 0);
    if (p.error) std::printf("  placement error: %s\n", p.error);
    client.place(p.type, p.pos, p.rot, Item::BuildingPlan);
    pump(server, client, now, 10);
    CHECK(server.world().objects().size() == 1);
    client.disconnect();
    server.tick(1.0 / 30);  // player leaves -> saved
    server.stop();
  }
  {
    Server server(cfg);
    CHECK(server.start());
    CHECK(server.world().objects().size() == 1);  // the foundation is back
    NetClient client;
    double now = 0;
    CHECK(join(server, client, cfg.port, "saver", now));
    CHECK(client.playerId() == me);  // same player
    pump(server, client, now, 10);
    CHECK(countItem(client.inventory(), Item::Wood) >= 777 - 50);  // foundation cost 50
    CHECK(client.craftQueue().size() == 1);                          // unfinished craft kept
    CHECK(client.world().objects().size() == 1);
    pump(server, client, now, 30 * 4);
    CHECK(countItem(client.inventory(), Item::SleepingBag) == 1);  // and it finishes after the restart
    server.stop();
  }
  std::remove(path.c_str());
}

static bool hasNotice(const NetClient& c, const std::string& part) {
  for (const auto& n : c.notices())
    if (n.text.find(part) != std::string::npos) return true;
  return false;
}

// Ranks and chat commands: first player owns the world, permissions are enforced, bans stick.
static void testRanksAndCommands() {
  ServerConfig cfg;
  cfg.port = 27905;
  Server server(cfg);
  CHECK(server.start());
  double now = 0;
  NetClient owner, guest;
  CHECK(join(server, owner, cfg.port, "Boss", now));
  CHECK(join(server, guest, cfg.port, "Guest", now));
  auto pumpBoth = [&](int ticks) {
    for (int t = 0; t < ticks; ++t) {
      owner.sendInput({});
      guest.sendInput({});
      server.tick(1.0 / 30);
      now += 1.0 / 30;
      owner.update(now);
      guest.update(now);
    }
  };
  pumpBoth(10);
  CHECK(owner.myRank() == Rank::Owner);
  CHECK(guest.myRank() == Rank::Player);
  CHECK(guest.rankOf(owner.playerId()) == Rank::Owner);  // everyone sees ranks

  guest.chat("/help");
  pumpBoth(10);
  CHECK(hasNotice(guest, "Help"));
  CHECK(!hasNotice(guest, "Admin commands"));  // players don't see admin commands
  guest.chat("/give wood 100");
  pumpBoth(10);
  CHECK(countItem(guest.inventory(), Item::Wood) == 0);
  CHECK(hasNotice(guest, "needs rank Admin"));

  owner.chat("/give wood 500 Guest");
  owner.chat("/give rifle");
  pumpBoth(15);
  CHECK(countItem(guest.inventory(), Item::Wood) == 500);
  CHECK(countItem(owner.inventory(), Item::Rifle) == 1);

  owner.chat("/rank Guest moderator");
  pumpBoth(10);
  CHECK(guest.myRank() == Rank::Moderator);
  guest.chat("/kick Boss");  // can't kick a higher rank
  pumpBoth(10);
  CHECK(owner.status() == NetClient::Status::Connected);
  CHECK(hasNotice(guest, "same or higher rank"));

  owner.chat("/ban Guest griefing");
  pumpBoth(10);
  CHECK(guest.status() == NetClient::Status::Rejected && guest.rejectReason() == RejectReason::Banned);
  NetClient again;
  CHECK(!join(server, again, cfg.port, "Guest", now));
  CHECK(again.rejectReason() == RejectReason::Banned);
  owner.chat("/unban guest");
  pumpBoth(5);
  NetClient back;
  CHECK(join(server, back, cfg.port, "Guest", now));
  CHECK(server.rankOf("Guest") == Rank::Moderator);  // rank remembered

  owner.chat("/settime 22");
  pumpBoth(5);
  CHECK(owner.timeOfDay() >= 22.0f && owner.timeOfDay() < 22.5f);
  server.stop();
}

static int findSlot(const Slots& s, Item item) {
  for (int i = 0; i < static_cast<int>(s.size()); ++i)
    if (s[i].item == item) return i;
  return -1;
}

// Guns, attachments, code locks, land claims and explosives, with two real clients.
static void testWeaponsLocksAndRaiding() {
  ServerConfig cfg;
  cfg.port = 27906;
  Server server(cfg);
  CHECK(server.start());
  double now = 0;
  NetClient owner, guest;
  CHECK(join(server, owner, cfg.port, "Raider", now));
  CHECK(join(server, guest, cfg.port, "Neighbor", now));
  PlayerInput oin{}, gin{};
  auto pumpBoth = [&](int ticks) {
    for (int t = 0; t < ticks; ++t) {
      for (int k = 0; k < 2; ++k) {
        owner.sendInput(oin);
        guest.sendInput(gin);
      }
      server.tick(1.0 / 30);
      now += 1.0 / 30;
      owner.update(now);
      guest.update(now);
    }
  };
  pumpBoth(5);
  const uint32_t me = owner.playerId(), them = guest.playerId();

  // Put both on flat ground near the island centre, 4 m apart, guest in front (-Z) of owner.
  Terrain t(cfg.terrainSeed);
  Vec3 spot{2100, 0, 2100};
  for (float x = 2000; x < 2400; x += 3)
    if (t.heightAt(x, 2100) > 3 && t.normalAt(x, 2100).y > 0.98f && t.normalAt(x, 2096).y > 0.98f) {
      spot = {x, t.heightAt(x, 2100), 2100};
      break;
    }
  *server.debugState(me) = PlayerState{};
  server.debugState(me)->pos = spot;
  server.debugState(them)->pos = {spot.x, t.heightAt(spot.x, spot.z - 4), spot.z - 4};
  server.debugState(them)->vel = {};
  pumpBoth(5);

  // --- automatic rifle: reload, then hold the trigger ---
  Slots* inv = server.debugInventory(me);
  (*inv)[2] = {Item::AssaultRifle, 1};
  addItem(*inv, Item::RifleAmmo, 60);
  addItem(*inv, Item::Silencer, 1);
  pumpBoth(3);
  oin = {0, 0, 0, 0, 0, kButtonReload, 2};
  pumpBoth(2);
  oin.buttons = 0;
  pumpBoth(100);  // 3.3 s: reload done
  CHECK(owner.inventory()[2].data == 30);
  CHECK(countItem(owner.inventory(), Item::RifleAmmo) == 30);
  oin = {0, 0, 0, 0, 0.6f, kButtonPrimary, 2};  // aim up into the sky, hold the trigger
  pumpBoth(15);                                 // 0.5 s
  oin.buttons = 0;
  pumpBoth(3);
  int left = owner.inventory()[2].data;
  std::printf("  assault rifle fired %d rounds in 0.5 s\n", 30 - left);
  CHECK(left <= 27 && left >= 23);  // automatic: ~4-5 shots

  // --- attachments ---
  int sil = findSlot(owner.inventory(), Item::Silencer);
  owner.attach(kSelfContainer, 2, sil);
  pumpBoth(5);
  CHECK(modAt(owner.inventory()[2], ModSlot::Muzzle) == Item::Silencer);
  CHECK(countItem(owner.inventory(), Item::Silencer) == 0);
  CHECK(gunStats(owner.inventory()[2]).silenced);
  owner.detach(kSelfContainer, 2, ModSlot::Muzzle);
  pumpBoth(5);
  CHECK(modAt(owner.inventory()[2], ModSlot::Muzzle) == Item::None && countItem(owner.inventory(), Item::Silencer) == 1);
  CHECK(!modFits(Item::Pistol, Item::Scope8x) && modFits(Item::Sniper, Item::Scope8x));

  // --- semi-auto pistol: holding the trigger fires once ---
  inv = server.debugInventory(me);
  (*inv)[3] = {Item::Pistol, 1, 12, 0};
  pumpBoth(3);
  oin = {0, 0, 0, 0, 0.6f, kButtonPrimary, 3};
  pumpBoth(30);
  oin.buttons = 0;
  pumpBoth(3);
  CHECK(owner.inventory()[3].data == 11);

  // --- shotgun to the chest at 4 m ---
  inv = server.debugInventory(me);
  (*inv)[4] = {Item::Shotgun, 1, 6, 0};
  pumpBoth(3);
  Vec3 eye = server.debugState(me)->pos + Vec3{0, kEyeHeight, 0};
  Vec3 target = server.debugState(them)->pos + Vec3{0, 1.1f, 0};
  float pitch = std::atan2(target.y - eye.y, std::hypot(target.x - eye.x, target.z - eye.z));
  float yaw = std::atan2(-(target.x - eye.x), -(target.z - eye.z));
  oin = {0, 0, 0, yaw, pitch, kButtonPrimary, 4};
  pumpBoth(1);
  oin.buttons = 0;
  pumpBoth(5);
  std::printf("  shotgun at 4 m: target health %.0f\n", guest.vitals().health);
  CHECK(guest.vitals().health < 50 || guest.vitals().dead);
  *server.debugVitals(them) = Vitals{};

  // --- code lock on a storage box ---
  inv = server.debugInventory(me);
  addItem(*inv, Item::StorageBox, 1);
  addItem(*inv, Item::CodeLock, 1);
  addItem(*inv, Item::LandClaim, 1);
  pumpBoth(3);
  Vec3 e2 = owner.predicted().pos + Vec3{0, kEyeHeight, 0};
  Placement boxPl = computePlacement(ObjType::StorageBox, owner.terrain(), owner.world(), e2, normalize(Vec3{0.7f, -0.6f, 0}), 0);
  if (boxPl.error) std::printf("  box placement: %s\n", boxPl.error);
  owner.place(boxPl.type, boxPl.pos, boxPl.rot, Item::StorageBox);
  pumpBoth(5);
  uint32_t boxId = 0;
  for (const auto& [id, o] : server.world().objects())
    if (o.type == ObjType::StorageBox) boxId = id;
  CHECK(boxId != 0);
  owner.useOn(boxId);
  pumpBoth(5);
  uint32_t promptId = 0;
  uint8_t mode = 9;
  CHECK(owner.takeCodePrompt(promptId, mode) && promptId == boxId && mode == 0);  // asked to choose a code
  owner.enterCode(boxId, "4821");
  pumpBoth(5);
  CHECK(owner.world().find(boxId) && (owner.world().find(boxId)->flags & kLocked));
  // The guest walks up and tries it.
  server.debugState(them)->pos = server.debugState(me)->pos + Vec3{0.5f, 0, 0.5f};
  pumpBoth(3);
  guest.interactObject(boxId);
  pumpBoth(5);
  CHECK(guest.takeCodePrompt(promptId, mode) && mode == 1);  // locked: asked for the code
  CHECK(guest.openContainer() == 0);
  guest.enterCode(boxId, "0000");
  pumpBoth(5);
  CHECK(guest.openContainer() == 0);  // wrong code
  pumpBoth(35);                       // wait out the guess delay
  guest.enterCode(boxId, "4821");
  pumpBoth(5);
  CHECK(guest.openContainer() == boxId);  // right code: in

  // --- land claim: the guest can't build next to the owner's base ---
  e2 = owner.predicted().pos + Vec3{0, kEyeHeight, 0};
  Placement claim = computePlacement(ObjType::LandClaim, owner.terrain(), owner.world(), e2, normalize(Vec3{-0.7f, -0.6f, 0}), 0);
  owner.place(claim.type, claim.pos, claim.rot, Item::LandClaim);
  pumpBoth(5);
  size_t before = server.world().objects().size();
  addItem(*server.debugInventory(them), Item::BuildingPlan, 1);
  addItem(*server.debugInventory(them), Item::Wood, 200);
  pumpBoth(3);
  Vec3 ge = guest.predicted().pos + Vec3{0, kEyeHeight, 0};
  Placement f = computePlacement(ObjType::Foundation, guest.terrain(), guest.world(), ge, normalize(Vec3{0, -0.45f, 1}), 0);
  guest.place(f.type, f.pos, f.rot, Item::BuildingPlan);
  pumpBoth(5);
  CHECK(server.world().objects().size() == before);
  CHECK(hasNotice(guest, "Building blocked"));

  // --- raid: an explosive charge destroys the box ---
  server.debugState(them)->pos = server.debugState(me)->pos + Vec3{40, 0, 0};  // out of the blast
  server.debugState(them)->pos.y = t.heightAt(server.debugState(them)->pos.x, server.debugState(them)->pos.z);
  addItem(*server.debugInventory(me), Item::ExplosiveCharge, 1);
  *server.debugVitals(me) = Vitals{};
  pumpBoth(3);
  const WorldObject* box = server.world().find(boxId);
  CHECK(box != nullptr);
  if (box) owner.place(ObjType::Charge, box->pos + Vec3{0, 0.75f, 0}, 0, Item::ExplosiveCharge);
  pumpBoth(5);
  bool planted = false;
  for (const auto& [id, o] : server.world().objects()) planted = planted || o.type == ObjType::Charge;
  CHECK(planted);
  CHECK(server.world().find(boxId) != nullptr);  // still standing during the fuse
  pumpBoth(30 * 11);                             // 11 s
  CHECK(server.world().find(boxId) == nullptr);  // boom
  CHECK(owner.vitals().health < 100);            // standing next to it hurt
  server.stop();
}

// A cheater sending inputs 4x too fast must not move faster than allowed.
static void testSpeedHackIsCapped() {
  ServerConfig cfg;
  cfg.port = 27902;
  Server server(cfg);
  CHECK(server.start());

  UdpSocket sock;
  CHECK(sock.open(0));
  Address srv;
  Address::parse("127.0.0.1", cfg.port, srv);
  auto hello = encode(HelloPacket{kProtocolVersion, "cheater"});
  sock.send(srv, hello.data(), hello.size());
  server.tick(1.0 / 30);

  uint8_t buf[kMaxPacketSize];
  Address from;
  WelcomePacket w;
  bool welcomed = false;
  int n;
  while ((n = sock.receive(from, buf, sizeof(buf))) > 0)
    if (peekType(buf, n) == PacketType::Welcome) welcomed = decode(buf, n, w);
  CHECK(welcomed);

  uint32_t seq = 1;
  SnapshotPacket last;
  for (int t = 0; t < 60; ++t) {  // 2 s of server time
    for (int k = 0; k < 8; ++k) {  // 240 inputs/s instead of 60
      InputPacket p;
      p.inputs.push_back({seq++, 0, 1, 0, 0, kButtonSprint, 0});
      auto b = encode(p);
      sock.send(srv, b.data(), b.size());
    }
    server.tick(1.0 / 30);
    while ((n = sock.receive(from, buf, sizeof(buf))) > 0)
      if (peekType(buf, n) == PacketType::Snapshot) decode(buf, n, last);
  }
  float moved = std::hypot(last.self.pos.x - w.spawn.pos.x, last.self.pos.z - w.spawn.pos.z);
  std::printf("  cheater moved %.1f m in 2 s (sprint limit ~%.0f m)\n", moved, 2 * kSprintSpeed);
  CHECK(moved < 2 * kSprintSpeed + 1.5f);
  server.stop();
}

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  if (!netInit()) return 1;
  std::printf("[%s]\n", "testProtocolRoundTrip");
  testProtocolRoundTrip();
  std::printf("[%s]\n", "testProtocolRejectsGarbage");
  testProtocolRejectsGarbage();
  std::printf("[%s]\n", "testSanitize");
  testSanitize();
  std::printf("[%s]\n", "testMovement");
  testMovement();
  std::printf("[%s]\n", "testStamina");
  testStamina();
  std::printf("[%s]\n", "testTerrainAndNodes");
  testTerrainAndNodes();
  std::printf("[%s]\n", "testCameraMath");
  testCameraMath();
  std::printf("[%s]\n", "testInventory");
  testInventory();
  std::printf("[%s]\n", "testRecipes");
  testRecipes();
  std::printf("[%s]\n", "testBuildingRules");
  testBuildingRules();
  std::printf("[%s]\n", "testReliableChannel");
  testReliableChannel();
  std::printf("[%s]\n", "testServerAndClient");
  testServerAndClient();
  std::printf("[%s]\n", "testGameplayLoop");
  testGameplayLoop();
  std::printf("[%s]\n", "testSaveAndReload");
  testSaveAndReload();
  std::printf("[%s]\n", "testRanksAndCommands");
  testRanksAndCommands();
  std::printf("[%s]\n", "testWeaponsLocksAndRaiding");
  testWeaponsLocksAndRaiding();
  std::printf("[%s]\n", "testSpeedHackIsCapped");
  testSpeedHackIsCapped();
  netShutdown();
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
