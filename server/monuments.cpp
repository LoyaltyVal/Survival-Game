// Monuments: oil rigs out at sea. They are generated from the map seed every
// time the server starts (old copies in the save are thrown away), can't be
// damaged or built on, and their military crates refill with high-tier loot.
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "server.h"

namespace sg {

namespace {
constexpr float kDeckHeight = kSeaLevel + 10.0f;  // lowest deck above the sea
constexpr float kDeckGap = 6.0f;                  // between decks (two flights of stairs)
constexpr double kCrateRefill = 20 * 60.0;        // seconds

struct LootEntry {
  Item item;
  int lo, hi;
  int weight;
};
const LootEntry kCrateLoot[] = {
    {Item::AssaultRifle, 1, 1, 2},   {Item::SMG, 1, 1, 3},           {Item::Shotgun, 1, 1, 3},       {Item::Sniper, 1, 1, 1},
    {Item::Pistol, 1, 1, 4},         {Item::Revolver, 1, 1, 3},      {Item::RifleAmmo, 30, 60, 6},   {Item::PistolAmmo, 40, 80, 6},
    {Item::Shells, 12, 24, 4},       {Item::HQMetal, 20, 60, 5},     {Item::GunParts, 2, 6, 6},      {Item::MetalFrags, 100, 300, 5},
    {Item::HoloSight, 1, 1, 2},      {Item::Scope4x, 1, 1, 2},       {Item::Scope8x, 1, 1, 1},       {Item::Silencer, 1, 1, 2},
    {Item::MuzzleBrake, 1, 1, 2},    {Item::VerticalGrip, 1, 1, 2},  {Item::Explosives, 5, 10, 2},   {Item::ExplosiveCharge, 1, 1, 1},
    {Item::MetalHelmet, 1, 1, 2},    {Item::MetalChestplate, 1, 1, 2}, {Item::PlatedPants, 1, 1, 2}, {Item::ArmoredBoots, 1, 1, 2},
    {Item::Bandage, 3, 5, 5},        {Item::Gunpowder, 50, 150, 3},
};
}  // namespace

void Server::fillCrate(uint32_t id) {
  auto it = containers_.find(id);
  if (it == containers_.end()) return;
  Slots& s = it->second;
  std::fill(s.begin(), s.end(), ItemStack{});
  int total = 0;
  for (const auto& e : kCrateLoot) total += e.weight;
  std::uniform_int_distribution<int> pickCount(3, 5), pickWeight(0, total - 1);
  const int n = pickCount(rng_);
  for (int k = 0; k < n; ++k) {
    int w = pickWeight(rng_);
    for (const auto& e : kCrateLoot) {
      if ((w -= e.weight) >= 0) continue;
      std::uniform_int_distribution<int> amount(e.lo, e.hi);
      addItem(s, e.item, amount(rng_));
      break;
    }
  }
  containerChanged(id);
}

void Server::updateCrates() {
  for (auto& [id, next] : crateRefill_)
    if (time_ >= next) {
      fillCrate(id);
      next = time_ + kCrateRefill;
    }
}

bool Server::nearMonument(const Vec3& p) const {
  for (const auto& m : monuments_) {
    float dx = p.x - m.center.x, dz = p.z - m.center.z;
    if (dx * dx + dz * dz < m.radius * m.radius) return true;
  }
  return false;
}

void Server::buildOilRig(float cx, float cz, int n, int decks, const std::string& name) {
  const float ox = std::floor((cx - n * kGrid * 0.5f) / kGrid) * kGrid, oz = std::floor((cz - n * kGrid * 0.5f) / kGrid) * kGrid;
  auto cell = [&](int i, int j, float y) { return Vec3{ox + (static_cast<float>(i) + 0.5f) * kGrid, y, oz + (static_cast<float>(j) + 0.5f) * kGrid}; };
  auto deckY = [&](int k) { return kDeckHeight + kDeckGap * static_cast<float>(k); };
  auto add = [&](ObjType t, const Vec3& pos, uint8_t rot) {
    WorldObject o;
    o.id = nextObjectId_++;
    o.type = t;
    o.tier = Tier::Metal;
    o.pos = pos;
    o.rot = rot;
    o.hp = isBuildingPiece(t) ? maxHp(t, Tier::Metal) : 1000.0f;
    o.flags = kMonument;
    world_.upsert(o);
    if (int size = containerSize(t)) {
      containers_[o.id] = Slots(static_cast<size_t>(size));
      crateRefill_[o.id] = time_;  // filled on the next world update
    }
    return o.id;
  };
  // Walls/railings sit on cell edges: edge along X at row line j, or along Z at column line i.
  auto edgeX = [&](int i, int j, float y) { return Vec3{ox + (static_cast<float>(i) + 0.5f) * kGrid, y, oz + static_cast<float>(j) * kGrid}; };
  auto edgeZ = [&](int i, int j, float y) { return Vec3{ox + static_cast<float>(i) * kGrid, y, oz + (static_cast<float>(j) + 0.5f) * kGrid}; };

  const int top = decks - 1;
  auto stairCol = [&](int k) { return k % 2 == 0 ? 1 : n - 2; };  // stairs from deck k up to k+1 rise along +Z in rows 1-2
  auto isHole = [&](int k, int i, int j) { return k > 0 && i == stairCol(k - 1) && (j == 1 || j == 2); };
  auto inRoom = [&](int k, int i, int j) { return k == top && i >= n - 2 && j >= n - 2; };
  const int ladderCol = n / 2;

  for (int k = 0; k < decks; ++k) {
    const float y = deckY(k);
    for (int i = 0; i < n; ++i)
      for (int j = 0; j < n; ++j)
        if (!isHole(k, i, j)) add(ObjType::Floor, cell(i, j, y), 0);
    if (k < top) {  // two flights up to the next deck
      add(ObjType::Stairs, cell(stairCol(k), 1, y), 2);
      add(ObjType::Stairs, cell(stairCol(k), 2, y + kWallHeight), 2);
    }
    if (k > 0) {  // rail the stairwell so nobody walks into it
      const int sx = stairCol(k - 1);
      for (int j : {1, 2}) {
        add(ObjType::Railing, edgeZ(sx, j, y), 1);
        add(ObjType::Railing, edgeZ(sx + 1, j, y), 1);
      }
      add(ObjType::Railing, edgeX(sx, 1, y), 0);
    }
    // Railings round the edge (gaps for the ladders; the control room's walls replace them).
    for (int i = 0; i < n; ++i) {
      bool ladderHere = k == 0 && i == ladderCol;
      if (!ladderHere) add(ObjType::Railing, edgeX(i, 0, y), 0);
      if (!(ladderHere && n >= 6) && !inRoom(k, i, n - 1)) add(ObjType::Railing, edgeX(i, n, y), 0);
    }
    for (int j = 0; j < n; ++j) {
      add(ObjType::Railing, edgeZ(0, j, y), 1);
      if (!inRoom(k, n - 1, j)) add(ObjType::Railing, edgeZ(n, j, y), 1);
    }
    // Support columns at the corners (and the middle of big rigs), standing on the deck below.
    std::vector<std::pair<int, int>> legs = {{0, 0}, {n - 1, 0}, {0, n - 1}, {n - 1, n - 1}};
    if (n >= 6 && k == 0) legs.insert(legs.end(), {{n / 2, 0}, {n / 2, n - 1}, {0, n / 2}, {n - 1, n / 2}});
    for (auto [i, j] : legs)
      if (!inRoom(k, i, j)) add(ObjType::RigLeg, cell(i, j, y - 0.2f), 0);
    // Crates in the open on every deck.
    const std::pair<int, int> spots[] = {{n - 1, 1}, {0, n - 2}, {n / 2, n - 1}, {n - 2, n / 2}};
    int placed = 0;
    for (auto [i, j] : spots) {
      if (placed >= 2) break;
      bool stairs = (k < top && i == stairCol(k) && (j == 1 || j == 2)) || (k > 0 && i == stairCol(k - 1) && j == 3);
      bool corner = (i == 0 || i == n - 1) && (j == 0 || j == n - 1);
      if (isHole(k, i, j) || stairs || corner || inRoom(k, i, j) || (k == 0 && i == ladderCol && j == 0)) continue;
      add(ObjType::Crate, cell(i, j, y), static_cast<uint8_t>(placed));
      ++placed;
    }
  }
  // Ladders from the sea up to the lowest deck.
  add(ObjType::Ladder, Vec3{cell(ladderCol, 0, 0).x, deckY(0), oz - 0.4f}, 0);
  if (n >= 6) add(ObjType::Ladder, Vec3{cell(ladderCol, 0, 0).x, deckY(0), oz + n * kGrid + 0.4f}, 2);
  // Control room on the top deck: 2x2 cells, a doorway on two sides, roofed, the best crates inside.
  const float ty = deckY(top);
  for (int i : {n - 2, n - 1}) {
    add(i == n - 2 ? ObjType::Doorway : ObjType::Wall, edgeX(i, n - 2, ty), 0);
    add(ObjType::Wall, edgeX(i, n, ty), 0);
  }
  for (int j : {n - 2, n - 1}) {
    add(j == n - 2 ? ObjType::Doorway : ObjType::Wall, edgeZ(n - 2, j, ty), 1);
    add(ObjType::Wall, edgeZ(n, j, ty), 1);
  }
  for (int i : {n - 2, n - 1})
    for (int j : {n - 2, n - 1}) add(ObjType::Floor, cell(i, j, ty + kWallHeight), 0);
  add(ObjType::Crate, cell(n - 1, n - 1, ty), 1);
  add(ObjType::Crate, cell(n - 2, n - 1, ty), 0);

  monuments_.push_back({name, Vec3{ox + n * kGrid * 0.5f, deckY(0), oz + n * kGrid * 0.5f}, n * kGrid * 0.75f + 15.0f});
  std::printf("[server] %s at %.0f, %.0f\n", name.c_str(), cx, cz);
}

void Server::spawnOilRigs() {
  // Old copies from the save go: the layout is rebuilt fresh (and may have changed between versions).
  std::vector<uint32_t> old;
  for (const auto& [id, o] : world_.objects())
    if (o.flags & kMonument) old.push_back(id);
  for (uint32_t id : old) {
    world_.remove(id);
    containers_.erase(id);
  }
  monuments_.clear();
  crateRefill_.clear();

  // March out from the island centre until the water is deep enough, then a bit further: swimmable from the beach.
  const float c = kWorldSize * 0.5f;
  const float base = kPi * 0.25f + static_cast<float>(cfg_.terrainSeed % 4) * kPi * 0.5f;  // toward a corner: the widest sea
  struct Spec {
    float angle;
    int tiles, decks;
    const char* name;
  };
  const Spec specs[] = {{base, 4, 2, "Small Oil Rig"}, {base + kPi, 6, 3, "Large Oil Rig"}};
  for (const auto& sp : specs) {
    for (float tryAngle = 0; tryAngle < 2 * kPi; tryAngle += 0.2f) {
      const float a = sp.angle + tryAngle;
      const Vec3 dir{std::cos(a), 0, std::sin(a)};
      auto inMap = [](const Vec3& p) { return p.x > 60 && p.z > 60 && p.x < kWorldSize - 60 && p.z < kWorldSize - 60; };
      float r = kWorldSize * 0.43f;  // past the island itself (inland lakes can be deep too)
      bool found = false;
      for (; r < 2900.0f; r += 8.0f) {  // first properly deep water past the beach
        Vec3 p = Vec3{c, 0, c} + dir * r;
        if (!inMap(p)) break;
        if (terrain_.heightAt(p.x, p.z) < kSeaLevel - 6.0f && terrain_.heightAt((p + dir * -60.0f).x, (p + dir * -60.0f).z) > kSeaLevel - 6.5f) {
          found = true;
          break;
        }
      }
      if (!found) continue;
      r += 30.0f + static_cast<float>(sp.tiles) * kGrid * 0.5f;
      Vec3 p = Vec3{c, 0, c} + dir * r;
      if (!inMap(p)) continue;
      bool deep = true;  // the whole footprint stands in water
      for (float dx = -15; dx <= 15 && deep; dx += 5)
        for (float dz = -15; dz <= 15 && deep; dz += 5) deep = terrain_.heightAt(p.x + dx, p.z + dz) < kSeaLevel - 4.0f;
      if (!deep) continue;
      buildOilRig(p.x, p.z, sp.tiles, sp.decks, sp.name);
      break;
    }
  }
}

}  // namespace sg
