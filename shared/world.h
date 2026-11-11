// The shared world: resource nodes (trees, rocks, ...), placed objects
// (building pieces, doors, boxes), collision and building rules.
//
// Resource nodes are not stored or sent: they come from the terrain seed, so
// client and server generate the same ones. The server only tracks which are
// depleted. Placed objects are sent to clients through reliable messages and
// both sides keep the same World, so movement prediction collides correctly.
#pragma once
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

#include "bytes.h"
#include "items.h"
#include "terrain.h"
#include "vecmath.h"

namespace sg {

struct AABB {
  Vec3 min, max;
};
inline bool overlaps(const AABB& a, const AABB& b) {
  return a.min.x < b.max.x && a.max.x > b.min.x && a.min.y < b.max.y && a.max.y > b.min.y && a.min.z < b.max.z &&
         a.max.z > b.min.z;
}
// Slab test. On hit, t is the distance along the (normalized) direction.
bool rayAABB(const Vec3& o, const Vec3& d, const AABB& b, float maxDist, float& t, Vec3* normal = nullptr);
// Marches along the ray until it goes under the terrain.
std::optional<float> rayTerrain(const Terrain& terrain, const Vec3& o, const Vec3& d, float maxDist);

// --- resource nodes -------------------------------------------------------------
enum class NodeType : uint8_t { None, Tree, Stone, MetalOre, Sulfur, Hemp, BerryBush, Mushroom };

struct ResourceNode {
  uint32_t id = 0;
  NodeType type = NodeType::None;
  Vec3 pos;  // base on the ground
  float scale = 1.0f;
};

struct NodeInfo {
  const char* name;
  Item yield;
  float amount;    // total yield before it is used up (hit nodes)
  float respawn;   // seconds
  bool pickup;     // picked with E instead of hit
  uint16_t pickupCount;
};
const NodeInfo& nodeInfo(NodeType t);

constexpr float kNodeCell = 6.0f;
constexpr int kNodeCells = static_cast<int>(kWorldSize / kNodeCell);

std::optional<ResourceNode> nodeInCell(const Terrain& t, int gx, int gz);
std::optional<ResourceNode> nodeById(const Terrain& t, uint32_t id);
void nodesInRect(const Terrain& t, float minX, float minZ, float maxX, float maxZ, std::vector<ResourceNode>& out);
AABB nodeBounds(const ResourceNode& n);

// --- placed objects ---------------------------------------------------------------
enum class ObjType : uint8_t {
  None, Foundation, Wall, Doorway, Floor, Stairs, Door, Workbench, StorageBox, SleepingBag, LootBag, Count
};
enum class Tier : uint8_t { Twig, Wood, Stone, Metal };
enum ObjFlags : uint8_t { kDoorOpen = 1 };

constexpr float kGrid = 3.0f;        // building grid cell, metres
constexpr float kWallHeight = 3.0f;
constexpr int kMaxObjects = 50000;

struct WorldObject {
  uint32_t id = 0;
  ObjType type = ObjType::None;
  Tier tier = Tier::Twig;
  uint32_t owner = 0;  // player id
  // Foundation/Floor/Stairs: cell centre, y = top surface.
  // Wall/Doorway/Door: centre of the cell edge, y = base. rot 0 runs along X, 1 along Z.
  // Deployables: base centre on the ground. rot = quarter turns.
  Vec3 pos;
  uint8_t rot = 0;
  float hp = 0;
  uint8_t flags = 0;
};

const char* objName(ObjType t);
const char* tierName(Tier t);
bool isBuildingPiece(ObjType t);  // upgradeable grid pieces
float maxHp(ObjType t, Tier tier);
// Twig placement cost (tier Twig) or the cost to upgrade *to* `tier`.
Ingredient buildCost(ObjType t, Tier tier);
ObjType deployableFor(Item item);  // None if the item can't be placed
int containerSize(ObjType t);      // 0 = not a container
// The pieces the building plan cycles through.
const std::vector<ObjType>& buildPlanPieces();

struct ObjBox {
  AABB box;
  bool solid;  // blocks movement and bullets; non-solid boxes are only for aiming/interaction
};
// visual = true gives slightly longer walls so corners look closed.
void objectBoxes(const WorldObject& o, std::vector<ObjBox>& out, bool visual = false);

void writeObject(ByteWriter& w, const WorldObject& o);
bool readObject(ByteReader& r, WorldObject& o);

class World {
 public:
  void upsert(const WorldObject& o);
  bool remove(uint32_t id);
  void clear();
  const WorldObject* find(uint32_t id) const;
  const std::unordered_map<uint32_t, WorldObject>& objects() const { return objects_; }
  uint32_t version() const { return version_; }  // changes whenever an object changes

  // True if `box` overlaps any solid object box.
  bool blocked(const AABB& box) const;
  // Highest walkable surface under a footprint (terrain, object tops, stairs)
  // that is no higher than feetY + stepUp.
  float groundHeight(const Terrain& terrain, float x, float z, float radius, float feetY, float stepUp) const;

  struct Hit {
    float dist = 0;
    uint32_t objectId = 0;
    Vec3 normal;
  };
  std::optional<Hit> raycast(const Vec3& origin, const Vec3& dir, float maxDist, bool includeNonSolid) const;

  // Objects whose position is within `radius` (horizontally) of p.
  void near(const Vec3& p, float radius, std::vector<const WorldObject*>& out) const;

 private:
  struct Entry {
    AABB box;
    uint32_t id;
    bool solid;
  };
  struct Ramp {
    AABB box;
    uint8_t rot;
  };
  void rebuild() const;
  void cellsFor(const AABB& b, std::vector<int64_t>& keys) const;

  std::unordered_map<uint32_t, WorldObject> objects_;
  uint32_t version_ = 1;
  mutable bool dirty_ = true;
  mutable std::vector<Entry> entries_;
  mutable std::vector<Ramp> ramps_;
  mutable std::unordered_map<int64_t, std::vector<int>> grid_;  // entry indices
  mutable std::unordered_map<int64_t, std::vector<int>> rampGrid_;
};

// --- building rules -----------------------------------------------------------------
struct Placement {
  ObjType type = ObjType::None;
  Vec3 pos;
  uint8_t rot = 0;
  const char* error = nullptr;  // nullptr = can place
};

// Client: where would this piece go when aiming from eye along dir?
Placement computePlacement(ObjType type, const Terrain& terrain, const World& world, const Vec3& eye, const Vec3& dir,
                           float yaw);
// Both: is this exact placement allowed by the building rules? nullptr = yes.
const char* checkPlacement(ObjType type, const Vec3& pos, uint8_t rot, const Terrain& terrain, const World& world);

}  // namespace sg
