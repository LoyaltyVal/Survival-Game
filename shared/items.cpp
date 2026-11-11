#include "items.h"

#include <algorithm>
#include <cmath>

namespace sg {

const char* soundName(Sound s) {
  static const char* names[] = {"",           "gun_bow",      "gun_revolver", "gun_pistol",   "gun_smg",   "gun_shotgun",
                                "gun_ar",     "gun_rifle",    "gun_sniper",   "gun_silenced", "reload",    "dry_fire",
                                "swing",      "hit_wood",     "hit_stone",    "hit_flesh",    "hitmarker", "headshot",
                                "kill",       "build",        "door",         "pickup",       "footstep",  "explosion",
                                "beep",       "unlock",       "denied"};
  size_t i = static_cast<size_t>(s);
  return i < sizeof(names) / sizeof(names[0]) ? names[i] : "";
}

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
  resource(Item::Gunpowder, "Gunpowder", "POWDER", {0.25f, 0.25f, 0.28f}, "Made from sulfur and coal.");
  resource(Item::Coal, "Coal", "COAL", {0.15f, 0.15f, 0.16f}, "Mine black rocks, or burn wood into charcoal.");
  resource(Item::HQMetalOre, "High Quality Ore", "HQ.ORE", {0.55f, 0.65f, 0.80f}, "Rare blue-veined rocks high in the mountains.");
  resource(Item::HQMetal, "High Quality Metal", "HQM", {0.70f, 0.80f, 0.95f}, "Refined from HQ ore and coal. For top-tier guns and optics.");
  resource(Item::Scrap, "Scrap", "SCRAP", {0.60f, 0.45f, 0.30f}, "Rusty junk piles near the coast (E). Makes gun parts.");
  resource(Item::Rope, "Rope", "ROPE", {0.75f, 0.65f, 0.45f}, "Twisted from cloth.");
  resource(Item::GunParts, "Gun Parts", "PARTS", {0.45f, 0.48f, 0.52f}, "Springs, pins and barrels made from scrap and metal.");

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
    x.sound = Sound::Swing;
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

  struct GunSpec {
    Item item;
    const char* name;
    const char* shortName;
    const char* desc;
    float damage, useTime, range, spread, adsSpread;
    int pellets, mag;
    float reload, zoom, recoil;
    bool automatic;
    Item ammo;
    uint8_t mods;
    Sound sound;
    float sightHeight, muzzleZ;
  };
  const GunSpec guns[] = {
      {Item::Bow, "Bow", "BOW", "Quiet and cheap. Arrows drop off past ~60 m.", 40, 0.85f, 90, 0.012f, 0.004f, 1, 1, 0.9f, 1.2f, 0.3f,
       false, Item::Arrow, 0, Sound::Bow, 0.03f, -0.35f},
      {Item::Revolver, "Revolver", "REVLV", "Six heavy rounds. Hard-kicking but accurate.", 34, 0.38f, 150, 0.014f, 0.006f, 1, 6, 2.6f,
       1.3f, 2.6f, false, Item::PistolAmmo, kModSight | kModGrip, Sound::Revolver, 0.055f, -0.2f},
      {Item::Pistol, "Pistol", "PISTL", "Fast semi-auto sidearm.", 22, 0.17f, 200, 0.012f, 0.005f, 1, 12, 1.8f, 1.3f, 1.4f, false,
       Item::PistolAmmo, kModSight | kModMuzzle | kModGrip, Sound::Pistol, 0.06f, -0.17f},
      {Item::SMG, "SMG", "SMG", "Fully automatic. Shreds up close, sprays at range.", 17, 0.085f, 160, 0.035f, 0.014f, 1, 24, 2.3f,
       1.35f, 0.35f, true, Item::PistolAmmo, kModSight | kModMuzzle | kModGrip, Sound::SMG, 0.07f, -0.36f},
      {Item::Shotgun, "Pump Shotgun", "SHTGN", "Eight pellets. Devastating within 15 m.", 13, 0.95f, 45, 0.085f, 0.06f, 8, 6, 3.6f,
       1.25f, 4.0f, false, Item::Shells, kModSight | kModGrip, Sound::Shotgun, 0.06f, -0.78f},
      {Item::AssaultRifle, "Assault Rifle", "AR", "Top-tier automatic rifle. Learn its recoil pattern.", 30, 0.115f, 300, 0.028f,
       0.008f, 1, 30, 3.0f, 1.4f, 0.55f, true, Item::RifleAmmo, kModSight | kModMuzzle | kModGrip | kModScopes,
       Sound::AssaultRifle, 0.075f, -0.72f},
      {Item::Rifle, "Hunting Rifle", "RIFLE", "Bolt-action. Slow, accurate, hits hard.", 55, 0.9f, 400, 0.01f, 0.0015f, 1, 5, 3.2f,
       1.4f, 3.0f, false, Item::RifleAmmo, kModSight | kModMuzzle | kModScopes, Sound::HuntingRifle, 0.06f, -0.88f},
      {Item::Sniper, "Bolt Sniper", "SNIPR", "Huge damage at any range. Put a scope on it.", 85, 1.5f, 600, 0.02f, 0.0005f, 1, 4, 3.6f,
       1.6f, 4.5f, false, Item::RifleAmmo, kModSight | kModMuzzle | kModGrip | kModScopes, Sound::Sniper, 0.065f, -1.02f},
  };
  for (const auto& g : guns) {
    ItemDef& x = def(g.item);
    x.name = g.name; x.shortName = g.shortName; x.description = g.desc;
    x.kind = ItemKind::Gun; x.maxStack = 1; x.color = {0.22f, 0.22f, 0.25f};
    x.damage = g.damage; x.useTime = g.useTime; x.range = g.range; x.spread = g.spread; x.adsSpread = g.adsSpread;
    x.pellets = static_cast<uint8_t>(g.pellets); x.magSize = static_cast<uint16_t>(g.mag); x.reloadTime = g.reload;
    x.zoom = g.zoom; x.recoil = g.recoil; x.automatic = g.automatic; x.ammo = g.ammo; x.modSlots = g.mods;
    x.sound = g.sound; x.sightHeight = g.sightHeight; x.muzzleZ = g.muzzleZ; x.gatherWood = 0;
  }
  def(Item::Bow).color = {0.55f, 0.36f, 0.18f};

  auto ammo = [&](Item i, const char* name, const char* shortName, const char* desc, Vec3 color) {
    ItemDef& x = def(i);
    x.name = name; x.shortName = shortName; x.description = desc;
    x.kind = ItemKind::Ammo; x.maxStack = 64; x.color = color; x.gatherWood = 0;
  };
  ammo(Item::PistolAmmo, "Pistol Ammo", "P.AMMO", "For the revolver, pistol and SMG.", {0.80f, 0.65f, 0.25f});
  ammo(Item::RifleAmmo, "Rifle Ammo", "R.AMMO", "For the assault rifle, hunting rifle and sniper.", {0.80f, 0.65f, 0.25f});
  ammo(Item::Arrow, "Arrow", "ARROW", "For the bow.", {0.60f, 0.45f, 0.25f});
  ammo(Item::Shells, "Shotgun Shells", "SHELL", "Buckshot for the pump shotgun.", {0.75f, 0.15f, 0.12f});

  auto mod = [&](Item i, const char* name, const char* shortName, const char* desc, ModSlot slot) -> ItemDef& {
    ItemDef& x = def(i);
    x.name = name; x.shortName = shortName; x.description = desc;
    x.kind = ItemKind::Attachment; x.maxStack = 1; x.color = {0.30f, 0.32f, 0.36f}; x.modSlot = slot;
    x.damage = 0; x.gatherWood = 0;
    return x;
  };
  mod(Item::HoloSight, "Holo Sight", "HOLO", "Red-dot sight: 1.6x zoom, steadier aim.", ModSlot::Sight).modZoom = 1.6f;
  def(Item::HoloSight).modAdsSpread = 0.8f;
  mod(Item::Scope4x, "4x Scope", "4X", "Magnified optic for rifles, AR and sniper.", ModSlot::Sight).modZoom = 4.0f;
  def(Item::Scope4x).modAdsSpread = 0.6f;
  mod(Item::Scope8x, "8x Scope", "8X", "Long-range optic for rifles and sniper.", ModSlot::Sight).modZoom = 8.0f;
  def(Item::Scope8x).modAdsSpread = 0.5f;
  {
    ItemDef& x = mod(Item::Silencer, "Silencer", "SILNC", "Quiet shots, no muzzle flash. -10% damage, -10% recoil.", ModSlot::Muzzle);
    x.silences = true; x.modDamage = 0.9f; x.modRecoil = 0.9f;
  }
  {
    ItemDef& x = mod(Item::MuzzleBrake, "Muzzle Brake", "BRAKE", "-40% recoil, but +25% spread.", ModSlot::Muzzle);
    x.modRecoil = 0.6f; x.modSpread = 1.25f; x.modAdsSpread = 1.25f;
  }
  mod(Item::VerticalGrip, "Vertical Grip", "GRIP", "-25% recoil.", ModSlot::Grip).modRecoil = 0.75f;
  mod(Item::LaserSight, "Laser Sight", "LASER", "-45% hip-fire spread.", ModSlot::Grip).modSpread = 0.55f;

  auto deploy = [&](Item i, const char* name, const char* shortName, Vec3 color, uint16_t stack, const char* desc) {
    ItemDef& x = def(i);
    x.name = name; x.shortName = shortName; x.description = desc;
    x.kind = ItemKind::Deployable; x.maxStack = stack; x.color = color;
    x.useTime = 0.3f; x.damage = 0; x.range = 5; x.gatherWood = 0;
  };
  deploy(Item::WoodenDoor, "Wooden Door", "DOOR", {0.45f, 0.28f, 0.14f}, 2, "Place in a doorway. 200 HP. Add a code lock!");
  deploy(Item::SheetMetalDoor, "Sheet Metal Door", "M.DOOR", {0.50f, 0.52f, 0.55f}, 2, "Place in a doorway. 500 HP.");
  deploy(Item::ArmoredDoor, "Armored Door", "A.DOOR", {0.35f, 0.38f, 0.42f}, 2, "Place in a doorway. 1000 HP.");
  deploy(Item::LargeBox, "Large Box", "L.BOX", {0.55f, 0.38f, 0.20f}, 2, "Stores 36 stacks.");
  deploy(Item::SmallStash, "Small Stash", "STASH", {0.50f, 0.42f, 0.30f}, 3, "6 slots, lies flat on the ground: easy to miss.");
  deploy(Item::LandClaim, "Land Claim Post", "CLAIM", {0.40f, 0.30f, 0.20f}, 1,
         "Only players authorized on it can build within 25 m. Press E to authorize. Lock it!");
  {
    ItemDef& x = def(Item::CodeLock);
    x.name = "Code Lock"; x.shortName = "LOCK"; x.kind = ItemKind::Lock; x.maxStack = 3; x.color = {0.75f, 0.70f, 0.20f};
    x.description = "Hold it and click your door, box or claim post. Set a 4-digit code; only people with it get in.";
    x.useTime = 0.3f; x.damage = 0; x.range = 3; x.gatherWood = 0;
  }
  resource(Item::Explosives, "Explosives", "EXPL", {0.55f, 0.15f, 0.10f}, "Refined blasting compound for raiding charges.");
  auto bomb = [&](Item i, const char* name, const char* shortName, float dmg, float radius, float fuse, const char* desc) {
    ItemDef& x = def(i);
    x.name = name; x.shortName = shortName; x.description = desc;
    x.kind = ItemKind::Explosive; x.maxStack = 10; x.color = {0.60f, 0.20f, 0.10f};
    x.blastDamage = dmg; x.blastRadius = radius; x.fuse = fuse;
    x.useTime = 0.5f; x.damage = 0; x.range = 3.5f; x.gatherWood = 0;
  };
  bomb(Item::PipeBomb, "Pipe Bomb", "P.BOMB", 120, 3.0f, 4.0f, "Stick it on a wall or door (click). 4 s fuse. Cheap, weak.");
  bomb(Item::ExplosiveCharge, "Explosive Charge", "CHARGE", 550, 3.5f, 10.0f,
       "Stick it on a wall or door (click). 10 s fuse. One takes down a stone wall.");
  deploy(Item::StorageBox, "Storage Box", "BOX", {0.60f, 0.42f, 0.22f}, 3, "Stores 18 stacks. Keeps your loot when you die.");
  deploy(Item::Workbench, "Workbench", "BENCH", {0.50f, 0.35f, 0.20f}, 1, "Stand near it to craft guns and attachments.");
  deploy(Item::SleepingBag, "Sleeping Bag", "BAG", {0.70f, 0.20f, 0.15f}, 3, "Respawn here after you die.");
  return d;
}

const ItemDef& itemDef(Item item) {
  static const std::array<ItemDef, kItemCount> defs = buildDefs();
  size_t i = static_cast<size_t>(item);
  return defs[i < kItemCount ? i : 0];
}

// --- attachments ------------------------------------------------------------------------
static constexpr int kFirstMod = static_cast<int>(Item::HoloSight);
static constexpr int kModCount = static_cast<int>(Item::LaserSight) - kFirstMod + 1;

Item modAt(const ItemStack& gun, ModSlot slot) {
  int v = (gun.mods >> (3 * static_cast<int>(slot))) & 7;
  return v == 0 || v > kModCount ? Item::None : static_cast<Item>(kFirstMod + v - 1);
}

void setMod(ItemStack& gun, ModSlot slot, Item attachment) {
  int shift = 3 * static_cast<int>(slot);
  int v = attachment == Item::None ? 0 : static_cast<int>(attachment) - kFirstMod + 1;
  gun.mods = static_cast<uint16_t>((gun.mods & ~(7 << shift)) | ((v & 7) << shift));
}

bool modFits(Item gun, Item attachment) {
  const ItemDef& g = itemDef(gun);
  const ItemDef& a = itemDef(attachment);
  if (g.kind != ItemKind::Gun || a.kind != ItemKind::Attachment) return false;
  uint8_t bit = a.modSlot == ModSlot::Sight ? kModSight : a.modSlot == ModSlot::Muzzle ? kModMuzzle : kModGrip;
  if (!(g.modSlots & bit)) return false;
  if ((attachment == Item::Scope4x || attachment == Item::Scope8x) && !(g.modSlots & kModScopes)) return false;
  return true;
}

GunStats gunStats(const ItemStack& gun) {
  const ItemDef& d = itemDef(gun.item);
  GunStats s{d.damage, d.spread, d.adsSpread, d.recoil, d.zoom, d.range, false, false, false, false};
  for (ModSlot slot : {ModSlot::Sight, ModSlot::Muzzle, ModSlot::Grip}) {
    Item m = modAt(gun, slot);
    if (m == Item::None) continue;
    const ItemDef& a = itemDef(m);
    s.damage *= a.modDamage;
    s.spread *= a.modSpread;
    s.adsSpread *= a.modAdsSpread;
    s.recoil *= a.modRecoil;
    if (a.modZoom > 0) s.zoom = a.modZoom;
    s.silenced = s.silenced || a.silences;
    s.scoped = s.scoped || m == Item::Scope4x || m == Item::Scope8x;
    s.laser = s.laser || m == Item::LaserSight;
    s.holo = s.holo || m == Item::HoloSight;
  }
  return s;
}

void recoilKick(Item gun, int i, float& yaw, float& pitch) {
  const float base = itemDef(gun).recoil;
  const float fi = static_cast<float>(i);
  switch (gun) {
    case Item::AssaultRifle: {
      // Climbs hard for the first 8 shots, then drifts right, then snakes back left.
      float climb = i < 8 ? 1.0f + 0.12f * fi : 1.9f - 0.04f * std::min(fi - 8.0f, 12.0f);
      pitch = base * climb;
      yaw = base * (i < 6 ? -0.15f : (i < 16 ? 0.55f * std::sin((fi - 6) * 0.35f) + 0.35f : -0.6f + 0.25f * std::sin(fi * 0.8f)));
      break;
    }
    case Item::SMG:
      pitch = base * (1.0f + 0.03f * std::min(fi, 20.0f));
      yaw = base * 0.45f * std::sin(fi * 1.1f);  // jittery side to side
      break;
    case Item::Pistol:
    case Item::Revolver:
      pitch = base;
      yaw = base * (i % 2 ? 0.15f : -0.12f);
      break;
    case Item::Shotgun:
      pitch = base;
      yaw = base * 0.2f;
      break;
    default:  // rifles, sniper, bow: a straight kick up and slightly right
      pitch = base;
      yaw = -base * 0.1f;
      break;
  }
}

// --- inventory ----------------------------------------------------------------------------
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
    if (itemDef(src.item).maxStack == 1) {  // single items keep their magazine/attachments: move whole
      int lo = 0, hi = static_cast<int>(to.size());
      if (&from == &to) {
        if (fromSlot < kHotbarSlots) lo = kHotbarSlots;
        else hi = kHotbarSlots;
      }
      for (int i = lo; i < hi; ++i)
        if (to[i].empty()) {
          to[i] = src;
          src = {};
          return true;
        }
      return false;
    }
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
    dst = src;
    dst.count = static_cast<uint16_t>(n);
    src.count = static_cast<uint16_t>(src.count - n);
    if (src.count == 0) src = {};
    return true;
  }
  if (dst.item == src.item && itemDef(dst.item).maxStack > 1) {
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
    bool e = s[i].empty();
    w.u16(e ? 0 : static_cast<uint16_t>(s[i].item));
    w.u16(e ? 0 : s[i].count);
    w.u16(e ? 0 : s[i].data);
    w.u16(e ? 0 : s[i].mods);
  }
}

bool readSlots(ByteReader& r, Slots& s, bool extended) {
  uint8_t n;
  if (!r.u8(n) || n > 64) return false;
  s.assign(n, {});
  for (uint8_t i = 0; i < n; ++i) {
    uint16_t item, count, data = 0, mods = 0;
    if (!r.u16(item) || !r.u16(count) || !validItem(item)) return false;
    if (extended && !(r.u16(data) && r.u16(mods))) return false;
    if (item != 0 && count > itemDef(static_cast<Item>(item)).maxStack) return false;
    if (item == 0 || count == 0) {
      s[i] = {};
      continue;
    }
    ItemStack st{static_cast<Item>(item), count, 0, 0};
    const ItemDef& d = itemDef(st.item);
    if (d.kind == ItemKind::Gun) {  // never trust a magazine bigger than the gun holds
      st.data = std::min<uint16_t>(data, d.magSize);
      st.mods = mods;
      for (ModSlot slot : {ModSlot::Sight, ModSlot::Muzzle, ModSlot::Grip}) {
        Item m = modAt(st, slot);
        if (m != Item::None && (!modFits(st.item, m) || itemDef(m).modSlot != slot)) setMod(st, slot, Item::None);
      }
    }
    s[i] = st;
  }
  return true;
}

const std::vector<Recipe>& recipes() {
  // Saved craft queues store the index: only append new recipes at the end.
  static const std::vector<Recipe> list = {
      {Item::BuildingPlan, 1, {{{Item::Wood, 20}}}, 1.0f, false},
      {Item::Hammer, 1, {{{Item::Wood, 100}}}, 2.0f, false},
      {Item::StoneHatchet, 1, {{{Item::Wood, 200}, {Item::Stone, 100}}}, 4.0f, false},
      {Item::StonePickaxe, 1, {{{Item::Wood, 200}, {Item::Stone, 100}}}, 4.0f, false},
      {Item::MetalFrags, 5, {{{Item::MetalOre, 5}, {Item::Wood, 5}}}, 1.5f, false},
      {Item::Gunpowder, 5, {{{Item::SulfurOre, 5}, {Item::Coal, 3}}}, 1.5f, false},
      {Item::Bandage, 1, {{{Item::Cloth, 4}}}, 1.0f, false},
      {Item::SleepingBag, 1, {{{Item::Cloth, 30}}}, 3.0f, false},
      {Item::StorageBox, 1, {{{Item::Wood, 100}}}, 3.0f, false},
      {Item::WoodenDoor, 1, {{{Item::Wood, 300}}}, 4.0f, false},
      {Item::Workbench, 1, {{{Item::Wood, 500}, {Item::MetalFrags, 50}}}, 8.0f, false},
      {Item::PistolAmmo, 8, {{{Item::MetalFrags, 10}, {Item::Gunpowder, 10}}}, 3.0f, false},
      {Item::Pistol, 1, {{{Item::MetalFrags, 175}, {Item::GunParts, 2}}}, 10.0f, true},
      {Item::RifleAmmo, 5, {{{Item::MetalFrags, 15}, {Item::Gunpowder, 20}}}, 4.0f, true},
      {Item::Rifle, 1, {{{Item::MetalFrags, 300}, {Item::GunParts, 2}, {Item::Wood, 100}}}, 15.0f, true},
      // --- 0.4 ---
      {Item::Coal, 3, {{{Item::Wood, 10}}}, 2.0f, false},  // charcoal
      {Item::HQMetal, 1, {{{Item::HQMetalOre, 2}, {Item::Coal, 2}}}, 3.0f, false},
      {Item::Rope, 1, {{{Item::Cloth, 15}}}, 2.0f, false},
      {Item::GunParts, 1, {{{Item::Scrap, 20}, {Item::MetalFrags, 25}}}, 4.0f, false},
      {Item::Bow, 1, {{{Item::Wood, 150}, {Item::Rope, 2}}}, 4.0f, false},
      {Item::Arrow, 6, {{{Item::Wood, 20}, {Item::Stone, 10}}}, 2.0f, false},
      {Item::Shells, 6, {{{Item::MetalFrags, 10}, {Item::Gunpowder, 12}}}, 3.0f, false},
      {Item::Revolver, 1, {{{Item::MetalFrags, 125}, {Item::GunParts, 1}, {Item::Wood, 50}}}, 8.0f, true},
      {Item::SMG, 1, {{{Item::MetalFrags, 225}, {Item::GunParts, 3}, {Item::HQMetal, 5}}}, 15.0f, true},
      {Item::Shotgun, 1, {{{Item::MetalFrags, 200}, {Item::GunParts, 3}, {Item::Wood, 100}}}, 15.0f, true},
      {Item::AssaultRifle, 1, {{{Item::MetalFrags, 300}, {Item::GunParts, 5}, {Item::HQMetal, 25}}}, 20.0f, true},
      {Item::Sniper, 1, {{{Item::MetalFrags, 250}, {Item::GunParts, 5}, {Item::HQMetal, 40}}}, 20.0f, true},
      {Item::HoloSight, 1, {{{Item::MetalFrags, 50}, {Item::GunParts, 2}, {Item::HQMetal, 5}}}, 6.0f, true},
      {Item::Scope4x, 1, {{{Item::MetalFrags, 100}, {Item::GunParts, 3}, {Item::HQMetal, 15}}}, 8.0f, true},
      {Item::Scope8x, 1, {{{Item::MetalFrags, 150}, {Item::GunParts, 4}, {Item::HQMetal, 30}}}, 10.0f, true},
      {Item::Silencer, 1, {{{Item::MetalFrags, 75}, {Item::GunParts, 2}, {Item::HQMetal, 10}}}, 6.0f, true},
      {Item::MuzzleBrake, 1, {{{Item::MetalFrags, 100}, {Item::GunParts, 2}}}, 6.0f, true},
      {Item::VerticalGrip, 1, {{{Item::MetalFrags, 50}, {Item::GunParts, 1}, {Item::Wood, 50}}}, 5.0f, true},
      {Item::LaserSight, 1, {{{Item::MetalFrags, 50}, {Item::GunParts, 2}, {Item::HQMetal, 5}}}, 6.0f, true},
      {Item::LargeBox, 1, {{{Item::Wood, 250}, {Item::MetalFrags, 50}}}, 5.0f, false},
      {Item::SmallStash, 1, {{{Item::Cloth, 20}}}, 2.0f, false},
      {Item::CodeLock, 1, {{{Item::MetalFrags, 100}, {Item::GunParts, 1}}}, 4.0f, false},
      {Item::LandClaim, 1, {{{Item::Wood, 1000}}}, 5.0f, false},
      {Item::SheetMetalDoor, 1, {{{Item::MetalFrags, 150}}}, 6.0f, true},
      {Item::ArmoredDoor, 1, {{{Item::MetalFrags, 300}, {Item::HQMetal, 20}, {Item::GunParts, 2}}}, 10.0f, true},
      {Item::Explosives, 1, {{{Item::Gunpowder, 40}, {Item::MetalFrags, 10}, {Item::SulfurOre, 10}}}, 5.0f, true},
      {Item::PipeBomb, 1, {{{Item::Gunpowder, 20}, {Item::MetalFrags, 15}, {Item::Rope, 1}}}, 4.0f, false},
      {Item::ExplosiveCharge, 1, {{{Item::Explosives, 15}, {Item::GunParts, 2}, {Item::Cloth, 5}}}, 15.0f, true},
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
