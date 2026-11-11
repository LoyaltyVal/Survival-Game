#include "items.h"

#include <algorithm>

namespace sg {

static std::array<ItemDef, kItemCount> buildDefs() {
  std::array<ItemDef, kItemCount> d{};
  auto def = [&](Item i) -> ItemDef& { return d[static_cast<size_t>(i)]; };

  auto resource = [&](Item i, const char* name, const char* shortName, Vec3 color, const char* desc) {
    ItemDef& x = def(i);
    x.name = name; x.shortName = shortName; x.description = desc;
    x.kind = ItemKind::Resource; x.maxStack = 1000; x.color = color;
  };
  resource(Item::Wood, "Wood", "WOOD", {0.55f, 0.36f, 0.18f}, "Chop trees. Used for almost everything.");
  resource(Item::Stone, "Stone", "STONE", {0.55f, 0.55f, 0.55f}, "Mine grey rocks.");
  resource(Item::MetalOre, "Metal Ore", "M.ORE", {0.55f, 0.40f, 0.32f}, "Mine rusty rocks on hills. Smelt into fragments.");
  resource(Item::SulfurOre, "Sulfur Ore", "SULF", {0.85f, 0.78f, 0.25f}, "Mine yellow rocks on hills. Makes gunpowder.");
  resource(Item::Cloth, "Cloth", "CLOTH", {0.85f, 0.82f, 0.68f}, "Pick hemp plants (E).");
  resource(Item::MetalFrags, "Metal Fragments", "FRAGS", {0.68f, 0.70f, 0.75f}, "Smelted metal for guns, ammo and upgrades.");
  resource(Item::Gunpowder, "Gunpowder", "POWDER", {0.25f, 0.25f, 0.28f}, "Made from sulfur and wood.");

  auto food = [&](Item i, const char* name, const char* shortName, Vec3 color, float f, float sat, float heal,
                  float time, const char* desc) {
    ItemDef& x = def(i);
    x.name = name; x.shortName = shortName; x.description = desc;
    x.kind = ItemKind::Food; x.maxStack = 20; x.color = color;
    x.food = f; x.saturation = sat; x.heal = heal; x.useTime = time; x.damage = 0; x.range = 0;
    x.gatherWood = 0;
  };
  food(Item::Berries, "Berries", "BERRY", {0.75f, 0.12f, 0.22f}, 8, 3, 1, 0.6f, "Pick from bushes (E). Hold and click to eat.");
  food(Item::Mushroom, "Mushroom", "MUSH", {0.80f, 0.65f, 0.48f}, 15, 8, 3, 0.8f, "Found in grass. Very filling.");
  {
    ItemDef& x = def(Item::Bandage);
    x.name = "Bandage"; x.shortName = "BAND"; x.description = "Hold and click to heal 20 HP.";
    x.kind = ItemKind::Medical; x.maxStack = 5; x.color = {0.92f, 0.92f, 0.88f};
    x.heal = 20; x.useTime = 2.0f; x.damage = 0; x.range = 0; x.gatherWood = 0;
  }

  auto tool = [&](Item i, const char* name, const char* shortName, Vec3 color, float time, float dmg, float range,
                  float wood, float stone, float ore, const char* desc) {
    ItemDef& x = def(i);
    x.name = name; x.shortName = shortName; x.description = desc;
    x.kind = ItemKind::Tool; x.maxStack = 1; x.color = color;
    x.useTime = time; x.damage = dmg; x.range = range;
    x.gatherWood = wood; x.gatherStone = stone; x.gatherOre = ore;
  };
  tool(Item::Rock, "Rock", "ROCK", {0.50f, 0.50f, 0.50f}, 0.9f, 8, 2.2f, 5, 5, 3, "Your first tool. Hit trees and rocks.");
  tool(Item::StoneHatchet, "Stone Hatchet", "HATCH", {0.60f, 0.45f, 0.30f}, 0.75f, 14, 2.4f, 15, 3, 1, "Chops wood fast.");
  tool(Item::StonePickaxe, "Stone Pickaxe", "PICK", {0.55f, 0.50f, 0.45f}, 0.85f, 14, 2.4f, 3, 12, 8, "Mines stone and ore fast.");
  tool(Item::Hammer, "Hammer", "HAMMR", {0.62f, 0.50f, 0.36f}, 0.6f, 5, 3.0f, 0, 0, 0, "Hit your building pieces to upgrade them.");
  {
    ItemDef& x = def(Item::BuildingPlan);
    x.name = "Building Plan"; x.shortName = "PLAN"; x.description = "Click to build. R changes the piece.";
    x.kind = ItemKind::Builder; x.maxStack = 1; x.color = {0.35f, 0.55f, 0.85f};
    x.useTime = 0.3f; x.damage = 0; x.range = 6; x.gatherWood = 0;
  }

  auto gun = [&](Item i, const char* name, const char* shortName, float time, float dmg, float range, Item ammo,
                 const char* desc) {
    ItemDef& x = def(i);
    x.name = name; x.shortName = shortName; x.description = desc;
    x.kind = ItemKind::Gun; x.maxStack = 1; x.color = {0.22f, 0.22f, 0.25f};
    x.useTime = time; x.damage = dmg; x.range = range; x.ammo = ammo; x.gatherWood = 0;
  };
  gun(Item::Pistol, "Pistol", "PISTL", 0.22f, 22, 200, Item::PistolAmmo, "Fast semi-auto. Headshots x2.");
  gun(Item::Rifle, "Hunting Rifle", "RIFLE", 0.9f, 55, 400, Item::RifleAmmo, "Slow and deadly. Headshots x2.");
  for (Item a : {Item::PistolAmmo, Item::RifleAmmo}) {
    ItemDef& x = def(a);
    x.kind = ItemKind::Ammo; x.maxStack = 64; x.color = {0.80f, 0.65f, 0.25f}; x.gatherWood = 0;
  }
  def(Item::PistolAmmo).name = "Pistol Ammo";
  def(Item::PistolAmmo).shortName = "P.AMMO";
  def(Item::PistolAmmo).description = "For the pistol.";
  def(Item::RifleAmmo).name = "Rifle Ammo";
  def(Item::RifleAmmo).shortName = "R.AMMO";
  def(Item::RifleAmmo).description = "For the hunting rifle.";

  auto deploy = [&](Item i, const char* name, const char* shortName, Vec3 color, uint16_t stack, const char* desc) {
    ItemDef& x = def(i);
    x.name = name; x.shortName = shortName; x.description = desc;
    x.kind = ItemKind::Deployable; x.maxStack = stack; x.color = color;
    x.useTime = 0.3f; x.damage = 0; x.range = 5; x.gatherWood = 0;
  };
  deploy(Item::WoodenDoor, "Wooden Door", "DOOR", {0.45f, 0.28f, 0.14f}, 2, "Place in a doorway. Only you can open it.");
  deploy(Item::StorageBox, "Storage Box", "BOX", {0.60f, 0.42f, 0.22f}, 3, "Stores 18 stacks. Keeps your loot when you die.");
  deploy(Item::Workbench, "Workbench", "BENCH", {0.50f, 0.35f, 0.20f}, 1, "Stand near it to craft guns.");
  deploy(Item::SleepingBag, "Sleeping Bag", "BAG", {0.70f, 0.20f, 0.15f}, 3, "Respawn here after you die.");
  return d;
}

const ItemDef& itemDef(Item item) {
  static const std::array<ItemDef, kItemCount> defs = buildDefs();
  size_t i = static_cast<size_t>(item);
  return defs[i < kItemCount ? i : 0];
}

int countItem(const Slots& s, Item item) {
  int n = 0;
  for (const auto& st : s)
    if (st.item == item) n += st.count;
  return n;
}

int addItem(Slots& s, Item item, int count, int lo, int hi) {
  if (item == Item::None || count <= 0) return 0;
  if (hi < 0 || hi > static_cast<int>(s.size())) hi = static_cast<int>(s.size());
  const int maxStack = itemDef(item).maxStack;
  for (int i = lo; i < hi && count > 0; ++i) {  // top up existing stacks first
    if (s[i].item != item || s[i].count >= maxStack) continue;
    int m = std::min(count, maxStack - static_cast<int>(s[i].count));
    s[i].count = static_cast<uint16_t>(s[i].count + m);
    count -= m;
  }
  for (int i = lo; i < hi && count > 0; ++i) {
    if (!s[i].empty()) continue;
    int m = std::min(count, maxStack);
    s[i] = {item, static_cast<uint16_t>(m)};
    count -= m;
  }
  return count;
}

bool removeItem(Slots& s, Item item, int count) {
  if (count <= 0) return true;
  if (countItem(s, item) < count) return false;
  // Take from the back so the hotbar keeps its items as long as possible.
  for (int i = static_cast<int>(s.size()) - 1; i >= 0 && count > 0; --i) {
    if (s[i].item != item) continue;
    int m = std::min(count, static_cast<int>(s[i].count));
    s[i].count = static_cast<uint16_t>(s[i].count - m);
    count -= m;
    if (s[i].count == 0) s[i] = {};
  }
  return true;
}

bool moveItem(Slots& from, int fromSlot, Slots& to, int toSlot, int count) {
  if (fromSlot < 0 || fromSlot >= static_cast<int>(from.size())) return false;
  ItemStack& src = from[fromSlot];
  if (src.empty()) return false;
  int n = (count <= 0 || count > src.count) ? src.count : count;

  if (toSlot == kAutoSlot) {
    int lo = 0, hi = static_cast<int>(to.size());
    if (&from == &to) {  // quick move inside one inventory: hotbar <-> main grid
      if (fromSlot < kHotbarSlots) lo = kHotbarSlots;
      else hi = kHotbarSlots;
    }
    int left = addItem(to, src.item, n, lo, hi);
    int moved = n - left;
    src.count = static_cast<uint16_t>(src.count - moved);
    if (src.count == 0) src = {};
    return moved > 0;
  }

  if (toSlot < 0 || toSlot >= static_cast<int>(to.size())) return false;
  if (&from == &to && fromSlot == toSlot) return false;
  ItemStack& dst = to[toSlot];
  if (dst.empty()) {
    dst = {src.item, static_cast<uint16_t>(n)};
    src.count = static_cast<uint16_t>(src.count - n);
    if (src.count == 0) src = {};
    return true;
  }
  if (dst.item == src.item) {
    int space = itemDef(dst.item).maxStack - dst.count;
    int m = std::min(space, n);
    if (m <= 0) return false;
    dst.count = static_cast<uint16_t>(dst.count + m);
    src.count = static_cast<uint16_t>(src.count - m);
    if (src.count == 0) src = {};
    return true;
  }
  if (n != src.count) return false;  // can't put half a stack on a different item
  std::swap(src, dst);
  return true;
}

bool slotsEmpty(const Slots& s) {
  for (const auto& st : s)
    if (!st.empty()) return false;
  return true;
}

void writeSlots(ByteWriter& w, const Slots& s) {
  size_t n = s.size() < 64 ? s.size() : 64;
  w.u8(static_cast<uint8_t>(n));
  for (size_t i = 0; i < n; ++i) {
    w.u16(s[i].empty() ? 0 : static_cast<uint16_t>(s[i].item));
    w.u16(s[i].empty() ? 0 : s[i].count);
  }
}

bool readSlots(ByteReader& r, Slots& s) {
  uint8_t n;
  if (!r.u8(n) || n > 64) return false;
  s.assign(n, {});
  for (uint8_t i = 0; i < n; ++i) {
    uint16_t item, count;
    if (!r.u16(item) || !r.u16(count) || !validItem(item)) return false;
    if (item != 0 && count > itemDef(static_cast<Item>(item)).maxStack) return false;
    s[i] = item == 0 || count == 0 ? ItemStack{} : ItemStack{static_cast<Item>(item), count};
  }
  return true;
}

const std::vector<Recipe>& recipes() {
  static const std::vector<Recipe> list = {
      {Item::BuildingPlan, 1, {{{Item::Wood, 20}}}, 1.0f, false},
      {Item::Hammer, 1, {{{Item::Wood, 100}}}, 2.0f, false},
      {Item::StoneHatchet, 1, {{{Item::Wood, 200}, {Item::Stone, 100}}}, 4.0f, false},
      {Item::StonePickaxe, 1, {{{Item::Wood, 200}, {Item::Stone, 100}}}, 4.0f, false},
      {Item::MetalFrags, 5, {{{Item::MetalOre, 5}, {Item::Wood, 5}}}, 1.5f, false},
      {Item::Gunpowder, 5, {{{Item::SulfurOre, 5}, {Item::Wood, 5}}}, 1.5f, false},
      {Item::Bandage, 1, {{{Item::Cloth, 4}}}, 1.0f, false},
      {Item::SleepingBag, 1, {{{Item::Cloth, 30}}}, 3.0f, false},
      {Item::StorageBox, 1, {{{Item::Wood, 100}}}, 3.0f, false},
      {Item::WoodenDoor, 1, {{{Item::Wood, 300}}}, 4.0f, false},
      {Item::Workbench, 1, {{{Item::Wood, 500}, {Item::MetalFrags, 50}}}, 8.0f, false},
      {Item::PistolAmmo, 8, {{{Item::MetalFrags, 10}, {Item::Gunpowder, 10}}}, 3.0f, false},
      {Item::Pistol, 1, {{{Item::MetalFrags, 150}, {Item::Wood, 50}}}, 10.0f, true},
      {Item::RifleAmmo, 5, {{{Item::MetalFrags, 15}, {Item::Gunpowder, 20}}}, 4.0f, true},
      {Item::Rifle, 1, {{{Item::MetalFrags, 300}, {Item::Wood, 100}, {Item::Cloth, 20}}}, 15.0f, true},
  };
  return list;
}

bool canAfford(const Slots& s, const Recipe& r, int times) {
  for (const auto& in : r.in)
    if (in.item != Item::None && countItem(s, in.item) < in.count * times) return false;
  return true;
}

void giveStartingKit(Slots& s) {
  addItem(s, Item::Rock, 1);
  addItem(s, Item::Berries, 3);
}

}  // namespace sg
