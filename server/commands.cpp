// Chat commands and ranks.
//
// Ranks: Owner > Admin > Moderator > Player. Everyone can use the commands of
// their own rank and every rank below it. You can only act on (kick, ban,
// mute, re-rank) players whose rank is lower than yours, and you can only hand
// out ranks lower than your own (Owners can make other Owners).
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

#include "server.h"

namespace sg {

namespace {

struct CommandInfo {
  const char* name;
  Rank minRank;
  const char* usage;
  const char* help;
};

const CommandInfo kCommands[] = {
    {"help", Rank::Player, "/help [topic]", "How the game works. Topics: start gather craft build survive combat ranks"},
    {"players", Rank::Player, "/players", "Who is online, their rank and ping"},
    {"msg", Rank::Player, "/msg <player> <text>", "Private message (also /w)"},
    {"r", Rank::Player, "/r <text>", "Reply to your last private message"},
    {"pos", Rank::Player, "/pos", "Your coordinates"},
    {"time", Rank::Player, "/time", "Current time of day"},
    {"rules", Rank::Player, "/rules", "Server rules"},
    {"kill", Rank::Player, "/kill", "Respawn if you are stuck (you drop your items)"},
    {"kick", Rank::Moderator, "/kick <player> [reason]", "Remove a player from the server"},
    {"mute", Rank::Moderator, "/mute <player>", "Stop a player from chatting"},
    {"unmute", Rank::Moderator, "/unmute <player>", "Let a muted player chat again"},
    {"tp", Rank::Moderator, "/tp <player>", "Teleport to a player"},
    {"announce", Rank::Moderator, "/announce <text>", "Big message on everyone's screen"},
    {"ban", Rank::Admin, "/ban <player> [reason]", "Kick and keep a player out"},
    {"unban", Rank::Admin, "/unban <name>", "Let a banned player back in"},
    {"tphere", Rank::Admin, "/tphere <player>", "Bring a player to you"},
    {"give", Rank::Admin, "/give <item> [amount] [player]", "Spawn items (see /items)"},
    {"items", Rank::Admin, "/items", "List item names for /give"},
    {"heal", Rank::Admin, "/heal [player]", "Full health, food and stamina"},
    {"god", Rank::Admin, "/god", "Toggle god mode: no damage, no hunger"},
    {"settime", Rank::Admin, "/settime <0-24|day|night>", "Change the time of day"},
    {"rank", Rank::Admin, "/rank <player> <player|moderator|admin|owner>", "Change someone's rank"},
    {"save", Rank::Admin, "/save", "Save the world now"},
};

const char* kHelpTopics[][2] = {
    {"start",
     "Getting started: 1) Left-click trees with your rock for wood. 2) Tab -> craft a Building Plan. "
     "3) Build a foundation, walls and a doorway (R changes piece). 4) Craft a door and a sleeping bag. 5) Keep loot in a box."},
    {"gather",
     "Gathering: trees = wood, grey rocks = stone, rusty rocks = metal ore, yellow rocks = sulfur (ore is up on the hills). "
     "Press E on hemp (cloth), berry bushes and mushrooms. A hatchet or pickaxe gathers about 3x faster."},
    {"craft",
     "Crafting: Tab opens it. Smelt metal ore + wood into metal fragments, sulfur + wood into gunpowder. "
     "Guns and rifle ammo need a workbench nearby. Crafts keep going while you play and survive a log-out."},
    {"build",
     "Building: foundation first, then walls/doorways on its edges, floors on top of walls, stairs on a foundation. "
     "Doors only open for whoever placed them. Hit your pieces with a hammer to upgrade: twig > wood > stone > metal."},
    {"survive",
     "Survival: food drains slowly; saturation (the yellow line) is a buffer that drains first. Above 70 food you heal. "
     "At 0 food you starve. Sprinting and jumping use stamina. Nights are dark - build before sunset."},
    {"combat",
     "Combat: pistol and rifle need ammo, headshots do double damage. When you die you drop a loot bag; "
     "your boxes are safe. Place a sleeping bag to respawn at home."},
    {"ranks",
     "Ranks: Owner > Admin > Moderator > Player. Mods: kick, mute, tp, announce. Admins: ban, give, heal, god, "
     "settime, rank. Use /rank <player> <rank>. You can only manage players below your rank."},
};

std::string lower(std::string s) {
  for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return s;
}

// Lowercase, letters and digits only: "Pistol Ammo" -> "pistolammo".
std::string squash(const std::string& s) {
  std::string out;
  for (char ch : s)
    if (std::isalnum(static_cast<unsigned char>(ch))) out += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return out;
}

std::vector<std::string> splitWords(const std::string& s) {
  std::vector<std::string> out;
  std::istringstream in(s);
  std::string w;
  while (in >> w) out.push_back(w);
  return out;
}

std::string joinFrom(const std::vector<std::string>& w, size_t from) {
  std::string out;
  for (size_t i = from; i < w.size(); ++i) {
    if (!out.empty()) out += ' ';
    out += w[i];
  }
  return out;
}

bool parseRank(const std::string& s, Rank& out) {
  std::string l = lower(s);
  if (l == "player" || l == "default" || l == "none") out = Rank::Player;
  else if (l == "moderator" || l == "mod") out = Rank::Moderator;
  else if (l == "admin") out = Rank::Admin;
  else if (l == "owner") out = Rank::Owner;
  else return false;
  return true;
}

// Finds an item by name, short name or the start of its name: "rifle", "pistolammo", "frags".
Item findItem(const std::string& query) {
  const std::string q = squash(query);
  if (q.empty()) return Item::None;
  Item prefix = Item::None;
  int prefixMatches = 0;
  for (size_t i = 1; i < kItemCount; ++i) {
    const ItemDef& d = itemDef(static_cast<Item>(i));
    std::string n = squash(d.name), sn = squash(d.shortName);
    if (n == q || sn == q) return static_cast<Item>(i);
    if (n.find(q) != std::string::npos) {
      prefix = static_cast<Item>(i);
      ++prefixMatches;
    }
  }
  return prefixMatches == 1 ? prefix : Item::None;
}

std::string clock(float hours) {
  int h = static_cast<int>(hours) % 24, m = static_cast<int>((hours - std::floor(hours)) * 60);
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%02d:%02d", h, m);
  return buf;
}

}  // namespace

void Server::setOwnerName(const std::string& name) {
  std::lock_guard<std::mutex> lock(ownerMu_);
  cfg_.ownerName = name;
}

std::string Server::ownerName() const {
  std::lock_guard<std::mutex> lock(ownerMu_);
  return cfg_.ownerName;
}

Rank Server::rankOf(const std::string& name) const {
  auto it = ranks_.find(name);
  return it == ranks_.end() ? Rank::Player : it->second;
}

bool Server::isBanned(const std::string& name) const {
  for (const auto& b : bans_)
    if (lower(b) == lower(name)) return true;
  return false;
}

bool Server::canManage(const Client& actor, const std::string& targetName) const {
  if (actor.name == targetName) return false;
  Rank a = rankOf(actor.name), t = rankOf(targetName);
  if (a > t) return true;
  // The person hosting the world can manage the other owners.
  return a == Rank::Owner && t == Rank::Owner && !ownerName().empty() && actor.name == ownerName();
}

ByteWriter Server::playerInfoMsg(const Client& c) const {
  ByteWriter w;
  w.u8(static_cast<uint8_t>(Msg::PlayerName));
  w.u32(c.id);
  w.str(c.name, kMaxNameLength);
  w.u8(static_cast<uint8_t>(rankOf(c.name)));
  return w;
}

void Server::broadcastPlayerInfo(const Client& c) { broadcast(playerInfoMsg(c)); }

Server::Client* Server::findPlayer(Client& asker, const std::string& partial) {
  const std::string q = lower(partial);
  Client* exact = nullptr;
  std::vector<Client*> starts;
  for (auto& [addr, c] : clients_) {
    std::string n = lower(c.name);
    if (n == q) exact = &c;
    else if (n.rfind(q, 0) == 0) starts.push_back(&c);
  }
  if (exact) return exact;
  if (starts.size() == 1) return starts[0];
  if (starts.empty()) sendText(asker, TextKind::Warning, "No online player called '" + partial + "'");
  else sendText(asker, TextKind::Warning, "'" + partial + "' matches several players, type more of the name");
  return nullptr;
}

void Server::teleport(Client& c, const Vec3& pos) {
  c.state.pos = pos;
  c.state.vel = {};
  c.state.onGround = false;
  c.history.clear();
}

void Server::handleCommand(Client& c, const std::string& line) {
  std::vector<std::string> w = splitWords(line.substr(1));
  if (w.empty()) return;
  std::string cmd = lower(w[0]);
  if (cmd == "w" || cmd == "whisper" || cmd == "tell") cmd = "msg";
  if (cmd == "h" || cmd == "?") cmd = "help";
  if (cmd == "online" || cmd == "list" || cmd == "who") cmd = "players";
  if (cmd == "i" || cmd == "item") cmd = "give";

  const Rank myRank = rankOf(c.name);
  const CommandInfo* info = nullptr;
  for (const auto& ci : kCommands)
    if (cmd == ci.name) info = &ci;
  if (!info) {
    sendText(c, TextKind::Warning, "Unknown command /" + w[0] + ". Type /help for a list.");
    return;
  }
  if (myRank < info->minRank) {
    sendText(c, TextKind::Warning, "/" + cmd + " needs rank " + rankName(info->minRank) + " or higher.");
    return;
  }
  auto usage = [&] { sendText(c, TextKind::Warning, std::string("Usage: ") + info->usage); };
  auto info_ = [&](const std::string& text) { sendText(c, TextKind::Info, text); };

  if (cmd == "help") {
    if (w.size() >= 2) {
      for (const auto& t : kHelpTopics)
        if (lower(w[1]) == t[0]) {
          info_(t[1]);
          return;
        }
      for (const auto& ci : kCommands)  // /help give -> usage of a command
        if (lower(w[1]) == ci.name || lower(w[1]) == std::string("/") + ci.name) {
          info_(std::string(ci.usage) + "  -  " + ci.help);
          return;
        }
      sendText(c, TextKind::Warning, "No help for '" + w[1] + "'. Topics: start gather craft build survive combat ranks");
      return;
    }
    info_("--- Help (your rank: " + std::string(rankName(myRank)) + ") ---");
    info_("New here? Type /help start. Topics: start, gather, craft, build, survive, combat, ranks");
    for (Rank r : {Rank::Player, Rank::Moderator, Rank::Admin}) {
      if (myRank < r) break;
      std::string list = r == Rank::Player ? "Commands:" : std::string(rankName(r)) + " commands:";
      for (const auto& ci : kCommands)
        if (ci.minRank == r) list += std::string(" /") + ci.name;
      info_(list);
    }
    info_("Type /help <command> for details, e.g. /help give");
    return;
  }
  if (cmd == "rules") {
    info_("Rules: be respectful, no cheating or exploits, raiding and PvP are allowed, don't camp fresh spawns.");
    return;
  }
  if (cmd == "players") {
    std::vector<const Client*> list;
    for (const auto& [addr, o] : clients_) list.push_back(&o);
    std::sort(list.begin(), list.end(), [&](const Client* x, const Client* y) {
      return rankOf(x->name) != rankOf(y->name) ? rankOf(x->name) > rankOf(y->name) : x->name < y->name;
    });
    std::string out = "Online (" + std::to_string(list.size()) + "):";
    for (const Client* o : list) {
      std::string entry = " " + (rankOf(o->name) == Rank::Player ? "" : "[" + std::string(rankName(rankOf(o->name))) + "] ") +
                          o->name + " (" + std::to_string(static_cast<int>(o->rtt * 1000)) + "ms)";
      if (out.size() + entry.size() > 190) {
        info_(out);
        out.clear();
      }
      out += entry;
    }
    if (!out.empty()) info_(out);
    return;
  }
  if (cmd == "pos") {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "Position: %.0f, %.0f, %.0f", c.state.pos.x, c.state.pos.y, c.state.pos.z);
    info_(buf);
    return;
  }
  if (cmd == "time") {
    bool day = timeOfDay_ >= 6 && timeOfDay_ < 20;
    info_("It is " + clock(timeOfDay_) + (day ? " (day)" : " (night)"));
    return;
  }
  if (cmd == "kill") {
    if (!c.vitals.dead) killPlayer(c, "You gave up", c.name + " gave up");
    return;
  }
  if (cmd == "msg" || cmd == "r") {
    Client* to = nullptr;
    std::string text;
    if (cmd == "msg") {
      if (w.size() < 3) return usage();
      to = findPlayer(c, w[1]);
      text = joinFrom(w, 2);
    } else {
      if (w.size() < 2) return usage();
      to = findClient(c.replyTo);
      if (!to) return sendText(c, TextKind::Warning, "Nobody to reply to");
      text = joinFrom(w, 1);
    }
    if (!to) return;
    if (isMuted(c.name)) return warn(c, "You are muted");
    sendText(*to, TextKind::Whisper, "[from " + c.name + "] " + text);
    sendText(c, TextKind::Whisper, "[to " + to->name + "] " + text);
    to->replyTo = c.id;
    return;
  }

  // --- moderator ---
  if (cmd == "announce") {
    if (w.size() < 2) return usage();
    broadcastText(TextKind::Announce, joinFrom(w, 1));
    return;
  }
  if (cmd == "kick" || cmd == "ban") {
    if (w.size() < 2) return usage();
    std::string reason = w.size() > 2 ? joinFrom(w, 2) : "";
    Client* t = nullptr;
    for (auto& [addr, o] : clients_)
      if (lower(o.name) == lower(w[1])) t = &o;
    if (!t && cmd == "kick") t = findPlayer(c, w[1]);
    std::string name = t ? t->name : w[1];
    if (cmd == "ban" && !t) {  // banning someone who is offline: must match a known player exactly
      bool known = false;
      for (const auto& [n, rec] : records_)
        if (lower(n) == lower(w[1])) {
          name = n;
          known = true;
        }
      if (!known) return sendText(c, TextKind::Warning, "No player called '" + w[1] + "' has played here");
    }
    if (cmd == "kick" && !t) return;
    if (!canManage(c, name)) return sendText(c, TextKind::Warning, "You can't " + cmd + " " + name + " (same or higher rank)");
    if (cmd == "ban") bans_.insert(name);
    if (t) t->kick = static_cast<uint8_t>(cmd == "ban" ? RejectReason::Banned : RejectReason::Kicked);
    broadcastText(TextKind::Kill, name + " was " + (cmd == "ban" ? "banned" : "kicked") + " by " + c.name +
                                      (reason.empty() ? "" : " (" + reason + ")"));
    if (cmd == "ban") save();
    return;
  }
  if (cmd == "unban") {
    if (w.size() < 2) return usage();
    for (auto it = bans_.begin(); it != bans_.end(); ++it)
      if (lower(*it) == lower(w[1])) {
        info_(*it + " is no longer banned");
        bans_.erase(it);
        save();
        return;
      }
    return sendText(c, TextKind::Warning, w[1] + " is not banned");
  }
  if (cmd == "mute" || cmd == "unmute") {
    if (w.size() < 2) return usage();
    Client* t = findPlayer(c, w[1]);
    if (!t) return;
    if (!canManage(c, t->name)) return sendText(c, TextKind::Warning, "You can't mute " + t->name + " (same or higher rank)");
    if (cmd == "mute") mutes_.insert(t->name);
    else mutes_.erase(t->name);
    info_(t->name + (cmd == "mute" ? " is muted" : " can chat again"));
    sendText(*t, TextKind::Warning, cmd == "mute" ? "You have been muted by " + c.name : "You can chat again");
    return;
  }
  if (cmd == "tp" || cmd == "tphere") {
    if (w.size() < 2) return usage();
    Client* t = findPlayer(c, w[1]);
    if (!t || t == &c) return;
    if (cmd == "tp") teleport(c, t->state.pos + Vec3{0.6f, 0.2f, 0.6f});
    else teleport(*t, c.state.pos + Vec3{0.6f, 0.2f, 0.6f});
    info_(cmd == "tp" ? "Teleported to " + t->name : "Brought " + t->name + " to you");
    if (cmd == "tphere") sendText(*t, TextKind::Info, c.name + " teleported you");
    return;
  }

  // --- admin ---
  if (cmd == "items") {
    std::string out = "Items:";
    for (size_t i = 1; i < kItemCount; ++i) {
      std::string entry = std::string(" ") + squash(itemDef(static_cast<Item>(i)).name);
      if (out.size() + entry.size() > 190) {
        info_(out);
        out = "      ";
      }
      out += entry;
    }
    info_(out);
    return;
  }
  if (cmd == "give") {
    if (w.size() < 2) return usage();
    Item item = findItem(w[1]);
    if (item == Item::None) return sendText(c, TextKind::Warning, "Unknown item '" + w[1] + "'. Type /items for names.");
    int amount = 1;
    size_t next = 2;
    if (w.size() > 2 && std::isdigit(static_cast<unsigned char>(w[2][0]))) {
      amount = std::clamp(std::atoi(w[2].c_str()), 1, 10000);
      next = 3;
    }
    Client* t = &c;
    if (w.size() > next) {
      t = findPlayer(c, w[next]);
      if (!t) return;
    }
    if (t->vitals.dead) return sendText(c, TextKind::Warning, t->name + " is dead");
    give(*t, item, amount);
    info_("Gave " + std::to_string(amount) + " " + itemDef(item).name + (t == &c ? "" : " to " + t->name));
    if (t != &c) sendText(*t, TextKind::Info, c.name + " gave you " + std::to_string(amount) + " " + itemDef(item).name);
    return;
  }
  if (cmd == "heal") {
    Client* t = w.size() > 1 ? findPlayer(c, w[1]) : &c;
    if (!t) return;
    if (t->vitals.dead) return sendText(c, TextKind::Warning, t->name + " is dead; they need to respawn");
    t->vitals.health = kMaxHealth;
    t->vitals.food = kMaxFood;
    t->vitals.saturation = kMaxSaturation;
    t->state.stamina = kMaxStamina;
    t->state.exhausted = false;
    info_("Healed " + t->name);
    return;
  }
  if (cmd == "god") {
    c.god = !c.god;
    info_(c.god ? "God mode ON: no damage, no hunger" : "God mode off");
    return;
  }
  if (cmd == "settime") {
    if (w.size() < 2) return usage();
    std::string a = lower(w[1]);
    float h;
    if (a == "day" || a == "morning") h = 8;
    else if (a == "noon") h = 12;
    else if (a == "evening" || a == "sunset") h = 18.5f;
    else if (a == "night") h = 22;
    else h = static_cast<float>(std::atof(a.c_str()));
    if (h < 0 || h >= 24) return usage();
    timeOfDay_ = h;
    broadcastText(TextKind::Info, c.name + " set the time to " + clock(h));
    return;
  }
  if (cmd == "rank") {
    if (w.size() < 3) return usage();
    Rank newRank;
    if (!parseRank(w[2], newRank)) return usage();
    Client* t = findPlayer(c, w[1]);
    if (!t) return;
    if (!canManage(c, t->name)) return sendText(c, TextKind::Warning, "You can't change " + t->name + "'s rank (same or higher rank)");
    if (newRank >= myRank && !(myRank == Rank::Owner && newRank == Rank::Owner))
      return sendText(c, TextKind::Warning, std::string("You can only give ranks below ") + rankName(myRank));
    if (newRank == Rank::Player) ranks_.erase(t->name);
    else ranks_[t->name] = newRank;
    broadcastPlayerInfo(*t);
    broadcastText(TextKind::Info, t->name + " is now " + rankName(newRank) + " (set by " + c.name + ")");
    save();
    return;
  }
  if (cmd == "save") {
    info_(save() ? "World saved" : "This server has no save file");
    return;
  }
}

}  // namespace sg
