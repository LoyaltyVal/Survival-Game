// Item definitions, recipes and inventory operations. Shared, so the client's
// UI and the server's rules always agree on stack sizes, costs and stats.
#pragma once
#include <array>
#include <cstdint>
#include <vector>

#include "bytes.h"
#include "vecmath.h"

namespace sg {

enum class Item : uint16_t {
  None = 0,
  // resources
  Wood, Stone, MetalOre, SulfurOre, Cloth, MetalFrags, Gunpowder,
  // food and medical
  Berries, Mushroom, Bandage,
  // tools
  Rock, StoneHatchet, StonePickaxe, Hammer, BuildingPlan,
  // weapons
  Pistol, Rifle, PistolAmmo, RifleAmmo,
  // deployables
  WoodenDoor, StorageBox, Workbench, SleepingBag,
  Count
};
constexpr size_t kItemCount = static_cast<size_t>(Item::Count);

enum class ItemKind : uint8_t { Resource, Food, Medical, Tool, Gun, Ammo, Deployable, Builder };

struct ItemDef {
  const char* name = "Hands";
  const char* shortName = "";
  const char* description = "";
  ItemKind kind = ItemKind::Tool;
  uint16_t maxStack = 1;
  Vec3 color{0.5f, 0.5f, 0.5f};  // icon / model colour (sRGB-ish for UI)
  float useTime = 0.6f;           // seconds between swings, shots or uses
  float damage = 4.0f;            // damage to players
  float range = 2.0f;             // metres
  float gatherWood = 1, gatherStone = 0, gatherOre = 0;  // yield per hit
  Item ammo = Item::None;
  float food = 0, saturation = 0, heal = 0;
};

// Item::None returns the "Hands" definition (punching).
const ItemDef& itemDef(Item item);
inline bool validItem(uint16_t raw) { return raw < kItemCount; }

struct ItemStack {
  Item item = Item::None;
  uint16_t count = 0;
  bool empty() const { return item == Item::None || count == 0; }
};
using Slots = std::vector<ItemStack>;

constexpr int kHotbarSlots = 6;
constexpr int kInventorySlots = 36;  // 0-5 hotbar, 6-35 main grid
constexpr int kAutoSlot = 255;       // "first slot that fits"

int countItem(const Slots& s, Item item);
// Adds into existing stacks first, then empty slots in [lo, hi). Returns how many did NOT fit.
int addItem(Slots& s, Item item, int count, int lo = 0, int hi = -1);
// All or nothing.
bool removeItem(Slots& s, Item item, int count);
// Moves `count` (0 = whole stack) from one slot to another, merging or swapping.
// toSlot == kAutoSlot finds a place (hotbar <-> main grid when from == to).
bool moveItem(Slots& from, int fromSlot, Slots& to, int toSlot, int count);
bool slotsEmpty(const Slots& s);

void writeSlots(ByteWriter& w, const Slots& s);
bool readSlots(ByteReader& r, Slots& s);  // max 64 slots

struct Ingredient {
  Item item = Item::None;
  uint16_t count = 0;
};
struct Recipe {
  Item output = Item::None;
  uint16_t outCount = 1;
  std::array<Ingredient, 3> in{};
  float seconds = 1.0f;
  bool workbench = false;  // must stand near a workbench
};
const std::vector<Recipe>& recipes();
bool canAfford(const Slots& s, const Recipe& r, int times);

// Starting kit for fresh spawns.
void giveStartingKit(Slots& s);

}  // namespace sg
