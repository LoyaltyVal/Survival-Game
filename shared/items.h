// Item definitions, recipes, guns, attachments and inventory operations.
// Shared, so the client's UI and the server's rules always agree on stack
// sizes, costs and weapon stats.
#pragma once
#include <array>
#include <cstdint>
#include <vector>

#include "bytes.h"
#include "vecmath.h"

namespace sg {

// Item ids are stored in save files: only ever append new items before Count.
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
  // --- added in 0.4 ---
  Coal, HQMetalOre, HQMetal, Scrap, Rope, GunParts,
  Bow, Revolver, SMG, Shotgun, AssaultRifle, Sniper,
  Arrow, Shells,
  HoloSight, Scope4x, Scope8x, Silencer, MuzzleBrake, VerticalGrip, LaserSight,
  LargeBox, SmallStash, CodeLock, LandClaim, SheetMetalDoor, ArmoredDoor, Explosives, PipeBomb, ExplosiveCharge,
  Count
};
constexpr size_t kItemCount = static_cast<size_t>(Item::Count);

enum class ItemKind : uint8_t { Resource, Food, Medical, Tool, Gun, Ammo, Deployable, Builder, Attachment, Explosive, Lock };

// Sound effects. Also sent over the network, so append only.
enum class Sound : uint8_t {
  None, Bow, Revolver, Pistol, SMG, Shotgun, AssaultRifle, HuntingRifle, Sniper, Silenced,
  Reload, DryFire, Swing, HitWood, HitStone, HitFlesh, Hitmarker, Headshot, Kill,
  Build, Door, Pickup, Footstep, Explosion, Beep, Unlock, Denied, Count
};
const char* soundName(Sound s);  // file name without extension, e.g. "gun_pistol"

// Attachment slots on a gun.
enum class ModSlot : uint8_t { Sight = 0, Muzzle = 1, Grip = 2 };
enum ModSlotBits : uint8_t { kModSight = 1, kModMuzzle = 2, kModGrip = 4, kModScopes = 8 };

struct ItemDef {
  const char* name = "Hands";
  const char* shortName = "";
  const char* description = "";
  ItemKind kind = ItemKind::Tool;
  uint16_t maxStack = 1;
  Vec3 color{0.5f, 0.5f, 0.5f};  // icon / model colour (sRGB-ish for UI)
  float useTime = 0.6f;           // seconds between swings, shots or uses
  float damage = 4.0f;            // damage to players (per pellet for shotguns)
  float range = 2.0f;             // metres
  float gatherWood = 1, gatherStone = 0, gatherOre = 0;  // yield per hit
  Item ammo = Item::None;
  float food = 0, saturation = 0, heal = 0;
  // guns
  bool automatic = false;    // hold to keep firing
  float spread = 0;          // hip-fire cone, radians
  float adsSpread = 0;       // aiming down sights
  uint8_t pellets = 1;
  uint16_t magSize = 0;
  float reloadTime = 0;
  float zoom = 1.25f;        // iron-sight zoom
  float recoil = 0;          // degrees of kick per shot (pattern scales this)
  float sightHeight = 0.05f; // eye line above the grip when aiming
  float muzzleZ = -0.2f;     // muzzle position in item space (flash, silencer)
  uint8_t modSlots = 0;      // ModSlotBits this gun accepts
  Sound sound = Sound::None;
  // attachments
  ModSlot modSlot = ModSlot::Sight;
  float modZoom = 0;            // replaces the gun's zoom (sights)
  float modRecoil = 1;          // multiplies recoil
  float modSpread = 1;          // multiplies hip spread
  float modAdsSpread = 1;       // multiplies aimed spread
  float modDamage = 1;
  bool silences = false;
  // explosives
  float blastDamage = 0;   // to the building piece it is stuck to (falls off with distance)
  float blastRadius = 0;
  float fuse = 0;          // seconds
};

// Item::None returns the "Hands" definition (punching).
const ItemDef& itemDef(Item item);
inline bool validItem(uint16_t raw) { return raw < kItemCount; }

struct ItemStack {
  Item item = Item::None;
  uint16_t count = 0;
  uint16_t data = 0;  // guns: rounds in the magazine
  uint16_t mods = 0;  // guns: attachments, 3 bits per slot (see modAt)
  bool empty() const { return item == Item::None || count == 0; }
};
using Slots = std::vector<ItemStack>;

// Attachments stored on a gun.
Item modAt(const ItemStack& gun, ModSlot slot);
void setMod(ItemStack& gun, ModSlot slot, Item attachment);  // Item::None removes
// Can this attachment go on this gun?
bool modFits(Item gun, Item attachment);

// A gun's stats with its attachments applied.
struct GunStats {
  float damage, spread, adsSpread, recoil, zoom, range;
  bool silenced, scoped;  // scoped: magnified optic (scope overlay)
  bool laser, holo;
};
GunStats gunStats(const ItemStack& gun);

// The learnable recoil pattern: how much the aim kicks on the Nth shot of a
// burst (0-based), before attachments. Same every time apart from a little noise.
void recoilKick(Item gun, int shotIndex, float& yawDeg, float& pitchDeg);

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

// extended = also magazine and attachments (network and saves from version 4 on).
void writeSlots(ByteWriter& w, const Slots& s);
bool readSlots(ByteReader& r, Slots& s, bool extended = true);  // max 64 slots

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
