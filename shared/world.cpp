#include "world.h"

#include <algorithm>
#include <cmath>

namespace sg {

// --- ray helpers -------------------------------------------------------------------
bool rayAABB(const Vec3& o, const Vec3& d, const AABB& b, float maxDist, float& t, Vec3* normal) {
  const float oa[3] = {o.x, o.y, o.z}, da[3] = {d.x, d.y, d.z};
  const float mn[3] = {b.min.x, b.min.y, b.min.z}, mx[3] = {b.max.x, b.max.y, b.max.z};
  float tmin = 0.0f, tmax = maxDist;
  int axis = -1;
  float sign = 0;
  for (int a = 0; a < 3; ++a) {
    if (std::fabs(da[a]) < 1e-8f) {
      if (oa[a] < mn[a] || oa[a] > mx[a]) return false;
      continue;
    }
    float inv = 1.0f / da[a];
    float t1 = (mn[a] - oa[a]) * inv, t2 = (mx[a] - oa[a]) * inv;
    float s = -1.0f;  // entering through the min face
    if (t1 > t2) {
      std::swap(t1, t2);
      s = 1.0f;
    }
    if (t1 > tmin) {
      tmin = t1;
      axis = a;
      sign = s;
    }
    tmax = std::min(tmax, t2);
    if (tmin > tmax) return false;
  }
  t = tmin;
  if (normal) {
    if (axis < 0) *normal = d * -1.0f;  // started inside
    else *normal = {axis == 0 ? sign : 0.0f, axis == 1 ? sign : 0.0f, axis == 2 ? sign : 0.0f};
  }
  return true;
}

std::optional<float> rayTerrain(const Terrain& terrain, const Vec3& o, const Vec3& d, float maxDist) {
  if (o.y < terrain.heightAt(o.x, o.z)) return 0.0f;
  const float step = maxDist > 20.0f ? 0.5f : 0.1f;
  float prev = 0.0f;
  for (float s = step; s <= maxDist + step * 0.5f; s += step) {
    float sc = std::min(s, maxDist);
    Vec3 p = o + d * sc;
    if (p.y < terrain.heightAt(p.x, p.z)) {
      float lo = prev, hi = sc;
      for (int i = 0; i < 10; ++i) {
        float mid = (lo + hi) * 0.5f;
        Vec3 m = o + d * mid;
        if (m.y < terrain.heightAt(m.x, m.z)) hi = mid;
        else lo = mid;
      }
      return hi;
    }
    prev = sc;
  }
  return std::nullopt;
}

// --- resource nodes -------------------------------------------------------------------
const NodeInfo& nodeInfo(NodeType t) {
  static const NodeInfo infos[] = {
      {"", Item::None, 0, 0, false, 0},
      {"Tree", Item::Wood, 300, 600, false, 0},
      {"Stone Rock", Item::Stone, 400, 600, false, 0},
      {"Metal Ore Rock", Item::MetalOre, 250, 900, false, 0},
      {"Sulfur Rock", Item::SulfurOre, 250, 900, false, 0},
      {"Hemp", Item::Cloth, 0, 300, true, 10},
      {"Berry Bush", Item::Berries, 0, 300, true, 4},
      {"Mushroom", Item::Mushroom, 0, 300, true, 2},
      {"Coal Rock", Item::Coal, 250, 900, false, 0},
      {"High Quality Ore Rock", Item::HQMetalOre, 60, 1500, false, 0},
      {"Scrap Pile", Item::Scrap, 0, 600, true, 12},
  };
  size_t i = static_cast<size_t>(t);
  return infos[i < sizeof(infos) / sizeof(infos[0]) ? i : 0];
}

static float cellHash(int x, int z, uint32_t salt) {
  uint32_t h = static_cast<uint32_t>(x) * 73856093u ^ static_cast<uint32_t>(z) * 19349663u ^ salt * 83492791u;
  h = (h ^ (h >> 13)) * 1274126177u;
  return static_cast<float>((h ^ (h >> 16)) & 0xFFFF) / 65535.0f;
}

std::optional<ResourceNode> nodeInCell(const Terrain& t, int gx, int gz) {
  if (gx < 0 || gz < 0 || gx >= kNodeCells || gz >= kNodeCells) return std::nullopt;
  const uint32_t seed = t.seed();
  float r = cellHash(gx, gz, seed);
  NodeType type;
  if (r < 0.070f) type = NodeType::Tree;
  else if (r < 0.085f) type = NodeType::Stone;
  else if (r < 0.093f) type = NodeType::MetalOre;
  else if (r < 0.099f) type = NodeType::Sulfur;
  else if (r < 0.109f) type = NodeType::Hemp;
  else if (r < 0.117f) type = NodeType::BerryBush;
  else if (r < 0.124f) type = NodeType::Mushroom;
  else if (r < 0.131f) type = NodeType::Coal;
  else if (r < 0.1335f) type = NodeType::HQMetal;
  else if (r < 0.139f) type = NodeType::Scrap;
  else return std::nullopt;

  float x = (static_cast<float>(gx) + 0.15f + 0.7f * cellHash(gz, gx, seed + 1)) * kNodeCell;
  float z = (static_cast<float>(gz) + 0.15f + 0.7f * cellHash(gx + 7, gz - 3, seed + 1)) * kNodeCell;
  float h = t.heightAt(x, z);
  if (h < 1.5f) return std::nullopt;  // no nodes on the beach or in the sea
  switch (type) {
    case NodeType::Tree:
      if (h > 55.0f || t.normalAt(x, z).y < 0.88f) return std::nullopt;
      break;
    case NodeType::MetalOre:
    case NodeType::Sulfur:
    case NodeType::Coal:
      if (h < 6.0f) return std::nullopt;  // ore lives on the hills
      break;
    case NodeType::HQMetal:
      if (h < 28.0f) return std::nullopt;  // rare, high up in the mountains
      break;
    case NodeType::Scrap:
      if (h > 14.0f) return std::nullopt;  // junk washed up near the coast
      break;
    case NodeType::Hemp:
    case NodeType::BerryBush:
    case NodeType::Mushroom:
      if (h > 50.0f) return std::nullopt;
      break;
    default:
      break;
  }
  ResourceNode n;
  n.id = static_cast<uint32_t>(gz * kNodeCells + gx + 1);
  n.type = type;
  n.scale = 0.8f + 0.6f * cellHash(gx * 3, gz * 5, seed + 2);
  bool rock = type == NodeType::Stone || type == NodeType::MetalOre || type == NodeType::Sulfur ||
              type == NodeType::Coal || type == NodeType::HQMetal;
  n.pos = {x, h - (rock ? 0.2f * n.scale : 0.0f), z};
  return n;
}

std::optional<ResourceNode> nodeById(const Terrain& t, uint32_t id) {
  if (id == 0) return std::nullopt;
  int idx = static_cast<int>(id - 1);
  return nodeInCell(t, idx % kNodeCells, idx / kNodeCells);
}

void nodesInRect(const Terrain& t, float minX, float minZ, float maxX, float maxZ, std::vector<ResourceNode>& out) {
  int x0 = static_cast<int>(std::floor(minX / kNodeCell)), x1 = static_cast<int>(std::floor(maxX / kNodeCell));
  int z0 = static_cast<int>(std::floor(minZ / kNodeCell)), z1 = static_cast<int>(std::floor(maxZ / kNodeCell));
  for (int gz = z0; gz <= z1; ++gz)
    for (int gx = x0; gx <= x1; ++gx)
      if (auto n = nodeInCell(t, gx, gz)) out.push_back(*n);
}

AABB nodeBounds(const ResourceNode& n) {
  const float s = n.scale;
  const Vec3 p = n.pos;
  auto box = [&](float hx, float y0, float y1) { return AABB{{p.x - hx, p.y + y0, p.z - hx}, {p.x + hx, p.y + y1, p.z + hx}}; };
  switch (n.type) {
    case NodeType::Tree: return box(0.3f * s, 0, 4.0f * s);
    case NodeType::Stone:
    case NodeType::MetalOre:
    case NodeType::Sulfur:
    case NodeType::Coal:
    case NodeType::HQMetal: return box(0.9f * s, 0, 1.3f * s);
    case NodeType::Scrap: return box(0.7f, 0, 0.7f);
    case NodeType::Hemp: return box(0.25f, 0, 1.4f);
    case NodeType::BerryBush: return box(0.6f, 0, 1.1f);
    case NodeType::Mushroom: return box(0.25f, 0, 0.4f);
    default: return box(0.1f, 0, 0.1f);
  }
}

// --- objects -----------------------------------------------------------------------------
const char* objName(ObjType t) {
  switch (t) {
    case ObjType::Foundation: return "Foundation";
    case ObjType::Wall: return "Wall";
    case ObjType::Doorway: return "Doorway";
    case ObjType::Floor: return "Floor";
    case ObjType::Stairs: return "Stairs";
    case ObjType::Door: return "Door";
    case ObjType::Workbench: return "Workbench";
    case ObjType::StorageBox: return "Storage Box";
    case ObjType::SleepingBag: return "Sleeping Bag";
    case ObjType::LootBag: return "Loot Bag";
    case ObjType::LargeBox: return "Large Box";
    case ObjType::Stash: return "Small Stash";
    case ObjType::LandClaim: return "Land Claim Post";
    case ObjType::Charge: return "Explosive";
    case ObjType::BodyBag: return "Body Bag";
    case ObjType::RigLeg: return "Support";
    case ObjType::Ladder: return "Ladder";
    case ObjType::Crate: return "Military Crate";
    case ObjType::Railing: return "Railing";
    default: return "?";
  }
}

const char* tierName(Tier t) {
  switch (t) {
    case Tier::Twig: return "Twig";
    case Tier::Wood: return "Wood";
    case Tier::Stone: return "Stone";
    case Tier::Metal: return "Metal";
  }
  return "?";
}

bool isBuildingPiece(ObjType t) {
  return t == ObjType::Foundation || t == ObjType::Wall || t == ObjType::Doorway || t == ObjType::Floor ||
         t == ObjType::Stairs;
}

float maxHp(ObjType t, Tier tier) {
  if (isBuildingPiece(t)) {
    switch (tier) {
      case Tier::Twig: return 10;
      case Tier::Wood: return 250;
      case Tier::Stone: return 500;
      case Tier::Metal: return 1000;
    }
  }
  switch (t) {
    case ObjType::Door: return tier == Tier::Metal ? 1000.0f : tier == Tier::Stone ? 500.0f : 200.0f;
    case ObjType::LargeBox: return 300;
    case ObjType::Stash: return 80;
    case ObjType::LandClaim: return 500;
    case ObjType::Charge: return 50;
    case ObjType::Workbench: return 300;
    case ObjType::StorageBox: return 150;
    case ObjType::SleepingBag: return 50;
    default: return 100;
  }
}

Ingredient buildCost(ObjType t, Tier tier) {
  float scale = 1.0f;
  if (t == ObjType::Floor) scale = 0.5f;
  if (t == ObjType::Doorway) scale = 0.7f;
  auto n = [&](float base) { return static_cast<uint16_t>(base * scale); };
  switch (tier) {
    case Tier::Twig: return {Item::Wood, n(50)};
    case Tier::Wood: return {Item::Wood, n(200)};
    case Tier::Stone: return {Item::Stone, n(300)};
    case Tier::Metal: return {Item::MetalFrags, n(200)};
  }
  return {};
}

ObjType deployableFor(Item item) {
  switch (item) {
    case Item::WoodenDoor: return ObjType::Door;
    case Item::StorageBox: return ObjType::StorageBox;
    case Item::Workbench: return ObjType::Workbench;
    case Item::SleepingBag: return ObjType::SleepingBag;
    case Item::SheetMetalDoor:
    case Item::ArmoredDoor: return ObjType::Door;
    case Item::LargeBox: return ObjType::LargeBox;
    case Item::SmallStash: return ObjType::Stash;
    case Item::LandClaim: return ObjType::LandClaim;
    case Item::PipeBomb:
    case Item::ExplosiveCharge: return ObjType::Charge;
    default: return ObjType::None;
  }
}

Tier doorTierFor(Item item) {
  if (item == Item::ArmoredDoor) return Tier::Metal;
  if (item == Item::SheetMetalDoor) return Tier::Stone;
  return Tier::Wood;
}

bool isLockable(ObjType t) {
  return t == ObjType::Door || t == ObjType::StorageBox || t == ObjType::LargeBox || t == ObjType::Stash ||
         t == ObjType::LandClaim;
}

int containerSize(ObjType t) {
  if (t == ObjType::StorageBox) return 18;
  if (t == ObjType::LootBag || t == ObjType::BodyBag) return kLootBagSlots;
  if (t == ObjType::Crate) return 12;
  if (t == ObjType::LargeBox) return 36;
  if (t == ObjType::Stash) return 6;
  return 0;
}

const std::vector<ObjType>& buildPlanPieces() {
  static const std::vector<ObjType> pieces = {ObjType::Foundation, ObjType::Wall, ObjType::Doorway, ObjType::Floor,
                                              ObjType::Stairs};
  return pieces;
}

// Ascending direction for stairs, and facing for deployables. Yaw 0 looks down -Z.
static Vec3 rotDir(uint8_t rot) {
  switch (rot & 3) {
    case 0: return {0, 0, -1};
    case 1: return {-1, 0, 0};
    case 2: return {0, 0, 1};
    default: return {1, 0, 0};
  }
}

static AABB edgeBox(const WorldObject& o, float a0, float a1, float y0, float y1, float halfThick) {
  const Vec3& p = o.pos;
  if (o.rot % 2 == 0) return {{p.x + a0, p.y + y0, p.z - halfThick}, {p.x + a1, p.y + y1, p.z + halfThick}};
  return {{p.x - halfThick, p.y + y0, p.z + a0}, {p.x + halfThick, p.y + y1, p.z + a1}};
}

static AABB centeredBox(const Vec3& base, float hx, float hy, float hz, uint8_t rot) {
  if (rot % 2) std::swap(hx, hz);
  return {{base.x - hx, base.y, base.z - hz}, {base.x + hx, base.y + 2 * hy, base.z + hz}};
}

void objectBoxes(const WorldObject& o, std::vector<ObjBox>& out, bool visual) {
  const Vec3& p = o.pos;
  const float h = kGrid * 0.5f;
  const float ext = visual ? h : h - 0.1f;  // collision walls stop short of the corner posts
  switch (o.type) {
    case ObjType::Foundation:
      out.push_back({{{p.x - h, p.y - 2.0f, p.z - h}, {p.x + h, p.y, p.z + h}}, true});
      break;
    case ObjType::Floor:
      out.push_back({{{p.x - h, p.y - 0.2f, p.z - h}, {p.x + h, p.y, p.z + h}}, true});
      break;
    case ObjType::Wall:
      out.push_back({edgeBox(o, -ext, ext, 0, kWallHeight, 0.1f), true});
      break;
    case ObjType::Doorway:
      out.push_back({edgeBox(o, -ext, -0.6f, 0, kWallHeight, 0.1f), true});
      out.push_back({edgeBox(o, 0.6f, ext, 0, kWallHeight, 0.1f), true});
      out.push_back({edgeBox(o, -0.6f, 0.6f, 2.2f, kWallHeight, 0.1f), true});
      break;
    case ObjType::Door:
      if (o.flags & kDoorOpen) {
        // Swung open around the hinge at one side of the gap.
        AABB b = o.rot % 2 == 0 ? AABB{{p.x - 0.6f, p.y, p.z}, {p.x - 0.5f, p.y + 2.2f, p.z + 1.2f}}
                                : AABB{{p.x, p.y, p.z - 0.6f}, {p.x + 1.2f, p.y + 2.2f, p.z - 0.5f}};
        out.push_back({b, false});
      } else {
        out.push_back({edgeBox(o, -0.6f, 0.6f, 0, 2.2f, 0.05f), true});
      }
      break;
    case ObjType::Stairs:
      if (visual) {
        Vec3 d = rotDir(o.rot);
        Vec3 side{std::fabs(d.z), 0, std::fabs(d.x)};
        Vec3 low = Vec3{p.x, 0, p.z} - d * h;
        for (int k = 0; k < 6; ++k) {
          Vec3 a = low + d * (k * 0.5f), b = low + d * ((k + 1) * 0.5f);
          Vec3 mn{std::min(a.x, b.x), p.y, std::min(a.z, b.z)}, mx{std::max(a.x, b.x), p.y + (k + 1) * 0.5f,
                                                                  std::max(a.z, b.z)};
          mn = mn - side * h;
          mx = mx + side * h;
          mn.y = p.y;
          mx.y = p.y + (k + 1) * 0.5f;
          out.push_back({{mn, mx}, false});
        }
      } else {
        out.push_back({{{p.x - h, p.y, p.z - h}, {p.x + h, p.y + 1.5f, p.z + h}}, false});
      }
      break;
    case ObjType::Workbench:
      out.push_back({centeredBox(p, 0.8f, 0.45f, 0.4f, o.rot), true});
      break;
    case ObjType::StorageBox:
      out.push_back({centeredBox(p, 0.45f, 0.3f, 0.3f, o.rot), true});
      break;
    case ObjType::SleepingBag:
      out.push_back({centeredBox(p, 0.4f, 0.06f, 1.0f, o.rot), false});
      break;
    case ObjType::LootBag:
      out.push_back({centeredBox(p, 0.3f, 0.25f, 0.3f, 0), false});
      break;
    case ObjType::LargeBox:
      out.push_back({centeredBox(p, 0.8f, 0.4f, 0.35f, o.rot), true});
      break;
    case ObjType::BodyBag:
      out.push_back({centeredBox(p, 0.3f, 0.15f, 0.95f, o.rot), false});
      break;
    case ObjType::RigLeg:
      out.push_back({{{p.x - 0.7f, p.y - 70.0f, p.z - 0.7f}, {p.x + 0.7f, p.y, p.z + 0.7f}}, true});
      break;
    case ObjType::Ladder: {
      // Climbing zone up to just above the deck, and a solid plate on the deck side to climb against.
      out.push_back({centeredBox({p.x, p.y - 16.0f, p.z}, 0.45f, 8.15f, 0.35f, o.rot), false});
      Vec3 toDeck = rotDir(o.rot) * -1.0f;
      out.push_back({centeredBox(Vec3{p.x, p.y - 16.0f, p.z} + toDeck * 0.42f, toDeck.x != 0 ? 0.05f : 0.45f, 7.87f,
                                 toDeck.z != 0 ? 0.05f : 0.45f, 0),
                     true});
      break;
    }
    case ObjType::Crate:
      out.push_back({centeredBox(p, 0.6f, 0.35f, 0.4f, o.rot), true});
      break;
    case ObjType::Railing:
      out.push_back({edgeBox(o, -h, h, 0, 1.1f, 0.05f), true});
      break;
    case ObjType::Stash:
      out.push_back({centeredBox(p, 0.25f, 0.06f, 0.25f, o.rot), false});
      break;
    case ObjType::LandClaim:
      out.push_back({centeredBox(p, 0.3f, 0.8f, 0.25f, o.rot), true});
      break;
    case ObjType::Charge:
      out.push_back({{{p.x - 0.12f, p.y - 0.12f, p.z - 0.12f}, {p.x + 0.12f, p.y + 0.12f, p.z + 0.12f}}, false});
      break;
    default:
      break;
  }
}

void writeObject(ByteWriter& w, const WorldObject& o) {
  w.u32(o.id);
  w.u8(static_cast<uint8_t>(o.type));
  w.u8(static_cast<uint8_t>(o.tier));
  w.u32(o.owner);
  w.vec3(o.pos);
  w.u8(o.rot);
  w.f32(o.hp);
  w.u8(o.flags);
}

bool readObject(ByteReader& r, WorldObject& o) {
  uint8_t type, tier;
  if (!(r.u32(o.id) && r.u8(type) && r.u8(tier) && r.u32(o.owner) && r.vec3(o.pos) && r.u8(o.rot) && r.f32(o.hp) &&
        r.u8(o.flags)))
    return false;
  if (type == 0 || type >= static_cast<uint8_t>(ObjType::Count) || tier > 3 || o.rot > 3) return false;
  o.type = static_cast<ObjType>(type);
  o.tier = static_cast<Tier>(tier);
  return true;
}

// --- World ---------------------------------------------------------------------------------
constexpr float kCollisionCell = 8.0f;

static int64_t cellKey(int x, int z) { return (static_cast<int64_t>(x) << 32) ^ static_cast<uint32_t>(z); }

void World::upsert(const WorldObject& o) {
  objects_[o.id] = o;
  dirty_ = true;
  ++version_;
}

bool World::remove(uint32_t id) {
  if (!objects_.erase(id)) return false;
  dirty_ = true;
  ++version_;
  return true;
}

void World::clear() {
  objects_.clear();
  dirty_ = true;
  ++version_;
}

const WorldObject* World::find(uint32_t id) const {
  auto it = objects_.find(id);
  return it == objects_.end() ? nullptr : &it->second;
}

void World::cellsFor(const AABB& b, std::vector<int64_t>& keys) const {
  keys.clear();
  int x0 = static_cast<int>(std::floor(b.min.x / kCollisionCell)), x1 = static_cast<int>(std::floor(b.max.x / kCollisionCell));
  int z0 = static_cast<int>(std::floor(b.min.z / kCollisionCell)), z1 = static_cast<int>(std::floor(b.max.z / kCollisionCell));
  for (int z = z0; z <= z1; ++z)
    for (int x = x0; x <= x1; ++x) keys.push_back(cellKey(x, z));
}

void World::rebuild() const {
  if (!dirty_) return;
  entries_.clear();
  ramps_.clear();
  grid_.clear();
  rampGrid_.clear();
  std::vector<ObjBox> boxes;
  std::vector<int64_t> keys;
  for (const auto& [id, o] : objects_) {
    boxes.clear();
    objectBoxes(o, boxes, false);
    for (const auto& b : boxes) {
      int idx = static_cast<int>(entries_.size());
      entries_.push_back({b.box, id, b.solid});
      cellsFor(b.box, keys);
      for (auto k : keys) grid_[k].push_back(idx);
    }
    if (o.type == ObjType::Stairs) {
      const float h = kGrid * 0.5f;
      AABB box{{o.pos.x - h, o.pos.y, o.pos.z - h}, {o.pos.x + h, o.pos.y + kWallHeight, o.pos.z + h}};
      int idx = static_cast<int>(ramps_.size());
      ramps_.push_back({box, o.rot});
      cellsFor(box, keys);
      for (auto k : keys) rampGrid_[k].push_back(idx);
    }
  }
  dirty_ = false;
}

bool World::blocked(const AABB& box) const {
  rebuild();
  std::vector<int64_t> keys;
  cellsFor(box, keys);
  for (auto k : keys) {
    auto it = grid_.find(k);
    if (it == grid_.end()) continue;
    for (int idx : it->second) {
      const Entry& e = entries_[idx];
      if (e.solid && overlaps(e.box, box)) return true;
    }
  }
  return false;
}

bool World::onLadder(const AABB& box) const {
  rebuild();
  std::vector<int64_t> keys;
  cellsFor(box, keys);
  for (auto k : keys) {
    auto it = grid_.find(k);
    if (it == grid_.end()) continue;
    for (int idx : it->second) {
      const Entry& e = entries_[idx];
      if (e.solid || !overlaps(e.box, box)) continue;
      auto o = objects_.find(e.id);
      if (o != objects_.end() && o->second.type == ObjType::Ladder) return true;
    }
  }
  return false;
}

float World::groundHeight(const Terrain& terrain, float x, float z, float radius, float feetY, float stepUp) const {
  float h = terrain.heightAt(x, z);
  if (objects_.empty()) return h;
  rebuild();
  const float limit = feetY + stepUp;
  AABB fp{{x - radius, -1e9f, z - radius}, {x + radius, 1e9f, z + radius}};
  std::vector<int64_t> keys;
  cellsFor(fp, keys);
  for (auto k : keys) {
    if (auto it = grid_.find(k); it != grid_.end()) {
      for (int idx : it->second) {
        const Entry& e = entries_[idx];
        if (!e.solid) continue;
        if (e.box.max.x <= fp.min.x || e.box.min.x >= fp.max.x || e.box.max.z <= fp.min.z || e.box.min.z >= fp.max.z)
          continue;
        if (e.box.max.y <= limit && e.box.max.y > h) h = e.box.max.y;
      }
    }
    if (auto it = rampGrid_.find(k); it != rampGrid_.end()) {
      for (int idx : it->second) {
        const Ramp& r = ramps_[idx];
        if (x < r.box.min.x || x > r.box.max.x || z < r.box.min.z || z > r.box.max.z) continue;
        Vec3 c{(r.box.min.x + r.box.max.x) * 0.5f, 0, (r.box.min.z + r.box.max.z) * 0.5f};
        Vec3 d = rotDir(r.rot);
        float t = 0.5f + ((x - c.x) * d.x + (z - c.z) * d.z) / kGrid;
        float rh = r.box.min.y + kWallHeight * clampf(t, 0.0f, 1.0f);
        if (rh <= limit && rh > h) h = rh;
      }
    }
  }
  return h;
}

std::optional<World::Hit> World::raycast(const Vec3& origin, const Vec3& dir, float maxDist, bool includeNonSolid) const {
  rebuild();
  std::optional<Hit> best;
  float bestT = maxDist;
  auto test = [&](int idx) {
    const Entry& e = entries_[idx];
    if (!e.solid && !includeNonSolid) return;
    float t;
    Vec3 n;
    if (rayAABB(origin, dir, e.box, bestT, t, &n) && t <= bestT) {
      bestT = t;
      best = Hit{t, e.id, n};
    }
  };
  if (maxDist > 24.0f) {
    for (int i = 0; i < static_cast<int>(entries_.size()); ++i) test(i);
  } else {
    Vec3 end = origin + dir * maxDist;
    AABB seg{{std::min(origin.x, end.x), 0, std::min(origin.z, end.z)}, {std::max(origin.x, end.x), 0, std::max(origin.z, end.z)}};
    std::vector<int64_t> keys;
    cellsFor(seg, keys);
    for (auto k : keys)
      if (auto it = grid_.find(k); it != grid_.end())
        for (int idx : it->second) test(idx);
  }
  return best;
}

void World::near(const Vec3& p, float radius, std::vector<const WorldObject*>& out) const {
  const float r2 = radius * radius;
  for (const auto& [id, o] : objects_) {
    float dx = o.pos.x - p.x, dz = o.pos.z - p.z;
    if (dx * dx + dz * dz <= r2) out.push_back(&o);
  }
}

// --- building rules ---------------------------------------------------------------------
static int cellOf(float v) { return static_cast<int>(std::floor(v / kGrid)); }
static float cellCenter(int i) { return static_cast<float>(i) * kGrid + kGrid * 0.5f; }
static bool aligned(float v, float target) { return std::fabs(v - target) < 0.02f; }

static uint8_t yawToRot(float yaw) {
  int r = static_cast<int>(std::lround(yaw / (kPi * 0.5f)));
  return static_cast<uint8_t>(((r % 4) + 4) % 4);
}

// Finds an object of one of `types` at exactly (x, z) with |y - pos.y| < yTol.
static const WorldObject* findAt(const World& world, std::initializer_list<ObjType> types, const Vec3& pos, float yTol,
                                 int rot = -1) {
  std::vector<const WorldObject*> list;
  world.near(pos, 0.1f, list);
  for (const WorldObject* o : list) {
    if (std::find(types.begin(), types.end(), o->type) == types.end()) continue;
    if (!aligned(o->pos.x, pos.x) || !aligned(o->pos.z, pos.z) || std::fabs(o->pos.y - pos.y) >= yTol) continue;
    if (rot >= 0 && (o->rot % 2) != (rot % 2)) continue;
    return o;
  }
  return nullptr;
}

static void terrainRange(const Terrain& t, int i, int j, float& minH, float& maxH) {
  minH = 1e9f;
  maxH = -1e9f;
  for (int a = 0; a <= 2; ++a)
    for (int b = 0; b <= 2; ++b) {
      float h = t.heightAt(static_cast<float>(i) * kGrid + a * kGrid * 0.5f, static_cast<float>(j) * kGrid + b * kGrid * 0.5f);
      minH = std::min(minH, h);
      maxH = std::max(maxH, h);
    }
}

static float foundationTop(const Terrain& t, const World& world, int i, int j) {
  float minH, maxH;
  terrainRange(t, i, j, minH, maxH);
  // Line up with a neighbouring foundation when the ground allows it.
  const int ni[4] = {1, -1, 0, 0}, nj[4] = {0, 0, 1, -1};
  for (int k = 0; k < 4; ++k) {
    Vec3 c{cellCenter(i + ni[k]), 0, cellCenter(j + nj[k])};
    std::vector<const WorldObject*> list;
    world.near(c, 0.1f, list);
    for (const WorldObject* o : list)
      if (o->type == ObjType::Foundation && o->pos.y >= maxH + 0.05f && o->pos.y - minH <= 2.0f) return o->pos.y;
  }
  return maxH + 0.25f;
}

// Walls on the four edges of cell (i, j) at base height y (any y if yTol is huge).
template <class F>
static void forCellEdges(int i, int j, F f) {
  f(Vec3{cellCenter(i), 0, static_cast<float>(j) * kGrid}, uint8_t{0});
  f(Vec3{cellCenter(i), 0, static_cast<float>(j + 1) * kGrid}, uint8_t{0});
  f(Vec3{static_cast<float>(i) * kGrid, 0, cellCenter(j)}, uint8_t{1});
  f(Vec3{static_cast<float>(i + 1) * kGrid, 0, cellCenter(j)}, uint8_t{1});
}

// The two cells either side of an edge.
static void edgeCells(const Vec3& pos, uint8_t rot, int ci[2], int cj[2]) {
  if (rot % 2 == 0) {
    int i = cellOf(pos.x), j = static_cast<int>(std::lround(pos.z / kGrid));
    ci[0] = ci[1] = i;
    cj[0] = j - 1;
    cj[1] = j;
  } else {
    int i = static_cast<int>(std::lround(pos.x / kGrid)), j = cellOf(pos.z);
    ci[0] = i - 1;
    ci[1] = i;
    cj[0] = cj[1] = j;
  }
}

const char* checkPlacement(ObjType type, const Vec3& pos, uint8_t rot, const Terrain& terrain, const World& world) {
  if (pos.x < kGrid || pos.z < kGrid || pos.x > kWorldSize - kGrid || pos.z > kWorldSize - kGrid) return "Outside the map";
  if (rot > 3) return "Bad rotation";
  if (static_cast<int>(world.objects().size()) >= kMaxObjects) return "World is full";

  switch (type) {
    case ObjType::Foundation: {
      int i = cellOf(pos.x), j = cellOf(pos.z);
      if (!aligned(pos.x, cellCenter(i)) || !aligned(pos.z, cellCenter(j))) return "Not on the grid";
      float minH, maxH;
      terrainRange(terrain, i, j, minH, maxH);
      if (maxH < 0.3f) return "Can't build in water";
      if (pos.y < maxH + 0.04f) return "Ground is too steep here";
      if (pos.y - minH > 2.05f) return "Too high above the ground";
      if (findAt(world, {ObjType::Foundation}, pos, 1e9f)) return "Something is already here";
      if (findAt(world, {ObjType::Floor, ObjType::Stairs}, {pos.x, pos.y - 1.0f, pos.z}, 1.1f)) return "Something is already here";
      return nullptr;
    }
    case ObjType::Floor: {
      int i = cellOf(pos.x), j = cellOf(pos.z);
      if (!aligned(pos.x, cellCenter(i)) || !aligned(pos.z, cellCenter(j))) return "Not on the grid";
      if (findAt(world, {ObjType::Foundation, ObjType::Floor}, pos, 0.1f)) return "Something is already here";
      bool supported = false;
      forCellEdges(i, j, [&](Vec3 e, uint8_t r) {
        e.y = pos.y - kWallHeight;
        if (findAt(world, {ObjType::Wall, ObjType::Doorway}, e, 0.05f, r)) supported = true;
      });
      const int ni[4] = {1, -1, 0, 0}, nj[4] = {0, 0, 1, -1};
      for (int k = 0; k < 4 && !supported; ++k)
        if (findAt(world, {ObjType::Floor}, {cellCenter(i + ni[k]), pos.y, cellCenter(j + nj[k])}, 0.05f)) supported = true;
      return supported ? nullptr : "Needs a wall or floor next to it";
    }
    case ObjType::Wall:
    case ObjType::Doorway: {
      if (rot > 1) return "Bad rotation";
      bool onEdge = rot == 0 ? aligned(pos.x, cellCenter(cellOf(pos.x))) && aligned(pos.z, std::round(pos.z / kGrid) * kGrid)
                             : aligned(pos.z, cellCenter(cellOf(pos.z))) && aligned(pos.x, std::round(pos.x / kGrid) * kGrid);
      if (!onEdge) return "Not on the grid";
      if (findAt(world, {ObjType::Wall, ObjType::Doorway}, pos, 0.1f, rot)) return "Something is already here";
      int ci[2], cj[2];
      edgeCells(pos, rot, ci, cj);
      for (int k = 0; k < 2; ++k)
        if (findAt(world, {ObjType::Foundation, ObjType::Floor}, {cellCenter(ci[k]), pos.y, cellCenter(cj[k])}, 0.05f))
          return nullptr;
      return "Needs a foundation or floor";
    }
    case ObjType::Door: {
      if (!findAt(world, {ObjType::Doorway}, pos, 0.05f, rot)) return "Aim at a doorway";
      if (findAt(world, {ObjType::Door}, pos, 0.05f)) return "Doorway already has a door";
      return nullptr;
    }
    case ObjType::Stairs: {
      int i = cellOf(pos.x), j = cellOf(pos.z);
      if (!aligned(pos.x, cellCenter(i)) || !aligned(pos.z, cellCenter(j))) return "Not on the grid";
      if (!findAt(world, {ObjType::Foundation, ObjType::Floor}, pos, 0.05f)) return "Needs a foundation or floor";
      if (findAt(world, {ObjType::Stairs}, pos, 0.1f)) return "Something is already here";
      return nullptr;
    }
    case ObjType::Charge:
      return nullptr;  // stuck to whatever it was aimed at; the server checks reach
    case ObjType::Workbench:
    case ObjType::StorageBox:
    case ObjType::SleepingBag:
    case ObjType::LargeBox:
    case ObjType::Stash:
    case ObjType::LandClaim: {
      float g = world.groundHeight(terrain, pos.x, pos.z, 0.05f, pos.y + 0.25f, 0.0f);
      if (std::fabs(g - pos.y) > 0.25f) return "Needs flat ground";
      if (pos.y < 0.2f) return "Can't place in water";
      WorldObject o;
      o.type = type;
      o.pos = pos;
      o.rot = rot;
      std::vector<ObjBox> boxes;
      objectBoxes(o, boxes);
      for (auto& b : boxes) {
        AABB s = b.box;
        s.min += Vec3{0.02f, 0.05f, 0.02f};
        s.max -= Vec3{0.02f, 0.02f, 0.02f};
        if (world.blocked(s)) return "Blocked";
      }
      return nullptr;
    }
    default:
      return "Can't place that";
  }
}

Placement computePlacement(ObjType type, const Terrain& terrain, const World& world, const Vec3& eye, const Vec3& dir,
                           float yaw) {
  Placement p;
  p.type = type;
  p.rot = yawToRot(yaw);
  const float reach = 5.0f;

  float dist = reach;
  uint32_t hitId = 0;
  Vec3 hitN{0, 1, 0};
  bool hit = false;
  if (auto h = world.raycast(eye, dir, reach, type == ObjType::Door)) {
    dist = h->dist;
    hitId = h->objectId;
    hitN = h->normal;
    hit = true;
  }
  if (auto t = rayTerrain(terrain, eye, dir, dist)) {
    if (!hit || *t < dist) {
      dist = *t;
      hitId = 0;
      Vec3 q = eye + dir * dist;
      hitN = terrain.normalAt(q.x, q.z);
      hit = true;
    }
  }
  if (!hit) dist = 4.0f;  // aiming at the sky: snap around a point in front
  const Vec3 P = eye + dir * dist;
  const WorldObject* ho = hitId ? world.find(hitId) : nullptr;
  p.pos = P;

  // Nearest of the candidate heights to the aim point.
  auto nearestY = [&](const std::vector<float>& ys, float fallback) {
    float best = fallback, bestD = 1e9f;
    for (float y : ys)
      if (std::fabs(y - P.y) < bestD) {
        bestD = std::fabs(y - P.y);
        best = y;
      }
    return best;
  };

  switch (type) {
    case ObjType::Foundation: {
      int i = cellOf(P.x), j = cellOf(P.z);
      float top;
      if (ho && ho->type == ObjType::Foundation) {  // extend from the one we aim at
        Vec3 d = P - ho->pos;
        int fi = cellOf(ho->pos.x), fj = cellOf(ho->pos.z);
        if (std::fabs(d.x) > std::fabs(d.z)) { i = fi + (d.x > 0 ? 1 : -1); j = fj; }
        else { j = fj + (d.z > 0 ? 1 : -1); i = fi; }
        top = ho->pos.y;
        float minH, maxH;
        terrainRange(terrain, i, j, minH, maxH);
        if (top < maxH + 0.04f || top - minH > 2.05f) top = foundationTop(terrain, world, i, j);
      } else {
        top = foundationTop(terrain, world, i, j);
      }
      p.pos = {cellCenter(i), top, cellCenter(j)};
      p.rot = 0;
      break;
    }
    case ObjType::Floor: {
      Vec3 q = P - hitN * 0.05f;
      int i = cellOf(q.x), j = cellOf(q.z);
      std::vector<float> ys;
      std::vector<const WorldObject*> list;
      world.near({cellCenter(i), 0, cellCenter(j)}, kGrid * 1.6f, list);
      for (const WorldObject* o : list) {
        if (o->type == ObjType::Wall || o->type == ObjType::Doorway) ys.push_back(o->pos.y + kWallHeight);
        if (o->type == ObjType::Floor) ys.push_back(o->pos.y);
      }
      p.pos = {cellCenter(i), nearestY(ys, P.y), cellCenter(j)};
      p.rot = 0;
      break;
    }
    case ObjType::Wall:
    case ObjType::Doorway: {
      Vec3 q = P - hitN * 0.05f;
      int i = cellOf(q.x), j = cellOf(q.z);
      float fx = q.x / kGrid - static_cast<float>(i), fz = q.z / kGrid - static_cast<float>(j);
      float d[4] = {fz, 1 - fz, fx, 1 - fx};
      int e = static_cast<int>(std::min_element(d, d + 4) - d);
      Vec3 pos;
      uint8_t rot;
      if (e == 0) { pos = {cellCenter(i), 0, static_cast<float>(j) * kGrid}; rot = 0; }
      else if (e == 1) { pos = {cellCenter(i), 0, static_cast<float>(j + 1) * kGrid}; rot = 0; }
      else if (e == 2) { pos = {static_cast<float>(i) * kGrid, 0, cellCenter(j)}; rot = 1; }
      else { pos = {static_cast<float>(i + 1) * kGrid, 0, cellCenter(j)}; rot = 1; }
      int ci[2], cj[2];
      edgeCells(pos, rot, ci, cj);
      std::vector<float> ys;
      for (int k = 0; k < 2; ++k) {
        std::vector<const WorldObject*> list;
        world.near({cellCenter(ci[k]), 0, cellCenter(cj[k])}, 0.1f, list);
        for (const WorldObject* o : list)
          if (o->type == ObjType::Foundation || o->type == ObjType::Floor) ys.push_back(o->pos.y);
      }
      pos.y = nearestY(ys, P.y);
      p.pos = pos;
      p.rot = rot;
      break;
    }
    case ObjType::Door:
      if (ho && ho->type == ObjType::Doorway) {
        p.pos = ho->pos;
        p.rot = ho->rot;
      } else {
        p.error = "Aim at a doorway";
        return p;
      }
      break;
    case ObjType::Stairs: {
      int i = cellOf(P.x), j = cellOf(P.z);
      std::vector<float> ys;
      std::vector<const WorldObject*> list;
      world.near({cellCenter(i), 0, cellCenter(j)}, 0.1f, list);
      for (const WorldObject* o : list)
        if (o->type == ObjType::Foundation || o->type == ObjType::Floor) ys.push_back(o->pos.y);
      p.pos = {cellCenter(i), nearestY(ys, P.y), cellCenter(j)};
      break;
    }
    case ObjType::Charge:
      if (!hit || dist > 3.5f) {
        p.error = "Aim at a wall, door or the ground";
        return p;
      }
      p.pos = P + hitN * 0.1f;
      return p;
    case ObjType::Workbench:
    case ObjType::StorageBox:
    case ObjType::SleepingBag:
    case ObjType::LargeBox:
    case ObjType::Stash:
    case ObjType::LandClaim:
      if (!hit || hitN.y < 0.7f) {
        p.error = "Aim at flat ground";
        return p;
      }
      break;
    default:
      p.error = "Can't place that";
      return p;
  }
  if (!hit && !isBuildingPiece(type)) p.error = "Too far";
  if (!p.error) p.error = checkPlacement(type, p.pos, p.rot, terrain, world);
  return p;
}

}  // namespace sg
