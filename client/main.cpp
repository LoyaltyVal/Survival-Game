// Game client.
//   sg_client                              main menu -> server list -> play
//   sg_client --connect 1.2.3.4[:port]     skip the menu and join a server
//   sg_client --name Ramiz                 player name
//
// The server list comes from servers.txt next to the game (created on first
// run with two servers). A listed server on this PC (127.0.0.1) that is not
// running is hosted by the game itself, so you can always play.
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "app.h"
#include "gl.h"

using namespace sg;

namespace {

constexpr float kBaseSensitivity = 0.0022f;
double g_scroll = 0;

void onScroll(GLFWwindow*, double, double dy) { g_scroll += dy; }

Vec3 colorForId(uint32_t id) {
  // Muted, readable colours in linear space.
  const Vec3 palette[] = {{0.55f, 0.12f, 0.08f}, {0.10f, 0.25f, 0.55f}, {0.45f, 0.38f, 0.10f},
                          {0.20f, 0.40f, 0.20f}, {0.40f, 0.15f, 0.40f}, {0.10f, 0.40f, 0.42f}};
  return palette[id % 6];
}

// --- settings and server list files ------------------------------------------------------
void loadSettings(App& a) {
  std::ifstream f("settings.txt");
  std::string line;
  while (std::getline(f, line)) {
    auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    std::string key = line.substr(0, eq), value = line.substr(eq + 1);
    if (key == "name" && !value.empty()) std::snprintf(a.playerName, sizeof(a.playerName), "%s", value.c_str());
    else if (key == "sensitivity") a.mouseSensitivity = clampf(std::strtof(value.c_str(), nullptr), 0.2f, 3.0f);
  }
}

void saveSettings(const App& a) {
  std::ofstream f("settings.txt");
  f << "name=" << a.playerName << "\nsensitivity=" << a.mouseSensitivity << "\n";
}

void loadServers(App& a) {
  const char* path = "servers.txt";
  std::ifstream in(path);
  if (!in) {
    std::ofstream out(path);
    out << "# Server list shown in the game. One per line:\n"
           "# name | host | port | map seed (used when the game hosts it for you)\n"
           "# Servers on 127.0.0.1 that aren't running are started by the game when you join.\n"
           "Balanced #1 - Main Island | 127.0.0.1 | 27015 | 1337\n"
           "Balanced #2 - Second Island | 127.0.0.1 | 27016 | 4242\n";
    out.close();
    in.open(path);
  }
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::vector<std::string> parts;
    std::stringstream ss(line);
    std::string part;
    while (std::getline(ss, part, '|')) {
      while (!part.empty() && (part.front() == ' ' || part.front() == '\t')) part.erase(part.begin());
      while (!part.empty() && (part.back() == ' ' || part.back() == '\t' || part.back() == '\r')) part.pop_back();
      parts.push_back(part);
    }
    if (parts.size() < 2 || parts[0].empty() || parts[1].empty()) continue;
    ServerEntry e;
    e.name = parts[0];
    e.host = parts[1];
    if (parts.size() > 2) e.port = static_cast<uint16_t>(std::atoi(parts[2].c_str()));
    if (parts.size() > 3) e.seed = static_cast<uint32_t>(std::strtoul(parts[3].c_str(), nullptr, 10));
    if (e.port == 0) e.port = kDefaultPort;
    a.servers.push_back(e);
  }
}

// Server browser: ask every listed server for its name, players and ping.
void pingServers(App& a) {
  if (!a.browserSocket.isOpen()) return;
  static std::mt19937 rng(std::random_device{}());
  if (a.now - a.lastPing >= 2.0) {
    for (auto& s : a.servers) {
      Address addr;
      if (!Address::parse(s.host, s.port, addr)) continue;
      s.nonce = rng();
      s.sentAt = a.now;
      auto bytes = encode(InfoRequestPacket{s.nonce});
      a.browserSocket.send(addr, bytes.data(), bytes.size());
    }
    a.lastPing = a.now;
  }
  uint8_t buf[kMaxPacketSize];
  Address from;
  int n;
  while ((n = a.browserSocket.receive(from, buf, sizeof(buf))) > 0) {
    InfoReplyPacket rep;
    if (!decode(buf, static_cast<size_t>(n), rep)) continue;
    for (auto& s : a.servers) {
      if (s.nonce != rep.nonce) continue;
      s.online = true;
      s.players = rep.players;
      s.maxPlayers = rep.maxPlayers;
      s.pingMs = static_cast<float>((a.now - s.sentAt) * 1000.0);
      s.lastReply = a.now;
    }
  }
  for (auto& s : a.servers)
    if (a.now - s.lastReply > 4.5) s.online = false;
}

// Starts one listed server inside this game. Fails quietly if the port is
// taken (most likely that world is already running as a dedicated sg_server).
bool hostServer(App& a, const ServerEntry& e) {
  for (const auto& h : a.hosted)
    if (h->server->config().port == e.port) return true;
  ServerConfig cfg;
  cfg.port = e.port;
  cfg.terrainSeed = e.seed;
  cfg.name = e.name;
  cfg.maxPlayers = 50;
  cfg.savePath = "world_" + std::to_string(e.port) + ".sav";
  cfg.ownerName = a.playerName;  // whoever hosts the world owns it
  auto h = std::make_unique<App::HostedServer>();
  h->server = std::make_unique<Server>(cfg);
  if (!h->server->start()) return false;
  h->running = true;
  App::HostedServer* raw = h.get();
  h->thread = std::thread([raw] { raw->server->run(raw->running); });
  a.hosted.push_back(std::move(h));
  return true;
}

void hostLocalServers(App& a) {
  for (const auto& e : a.servers)
    if (e.isLocal()) hostServer(a, e);
  // Let friends on the internet in: ask the router to forward the hosted ports.
  std::vector<uint16_t> ports;
  for (const auto& h : a.hosted) ports.push_back(h->server->config().port);
  if (!ports.empty() && !a.noUpnp) a.forwarder.start(ports);
  a.lanAddresses = localAddresses();
}

void stopHostedServers(App& a) {
  a.forwarder.stop();  // close the router ports again
  for (auto& h : a.hosted) h->running = false;
  for (auto& h : a.hosted) {
    if (h->thread.joinable()) h->thread.join();
    h->server->stop();  // final save
  }
  a.hosted.clear();
}

void resetGameUi(App& a) {
  a.inventoryOpen = a.pauseOpen = a.chatOpen = a.showPlayers = false;
  a.hotbar = 0;
  a.buildPiece = 0;
  a.havePlacement = false;
  a.prompt.clear();
  a.lookObject = a.lookNode = 0;
}

// Cursor: hidden + locked while playing, free while any menu is open.
void setCaptured(App& a, bool captured) {
  static int last = -1;
  if (last == static_cast<int>(captured)) return;
  last = captured;
  glfwSetInputMode(a.window, GLFW_CURSOR, captured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
  if (captured && glfwRawMouseMotionSupported()) glfwSetInputMode(a.window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
}

}  // namespace

namespace sg {

void joinServer(App& a, const ServerEntry& e) {
  a.connectError.clear();
  a.net.disconnect();
  if (e.isLocal() && !e.online && !hostServer(a, e)) {
    a.connectError = "Could not host on port " + std::to_string(e.port) + ". Is another program using it?";
    return;
  }
  // You own the worlds you host, even if you changed your name since the game started.
  if (e.isLocal())
    for (auto& h : a.hosted)
      if (h->server->config().port == e.port) h->server->setOwnerName(a.playerName);
  std::string host = e.host == "localhost" ? "127.0.0.1" : e.host;
  if (!a.net.connect(host, e.port, a.playerName)) {
    a.connectError = "Could not find " + e.host;
    return;
  }
  a.connectingTo = e.name;
  a.connectStart = a.now;
  a.screen = Screen::Connecting;
}

static void saveServers(const App& a) {
  std::ofstream out("servers.txt");
  out << "# Server list shown in the game. One per line:\n"
         "# name | host | port | map seed (used when the game hosts it for you)\n"
         "# Servers on 127.0.0.1 are hosted by the game automatically.\n";
  for (const auto& e : a.servers) out << e.name << " | " << e.host << " | " << e.port << " | " << e.seed << "\n";
}

void addServer(App& a, const std::string& name, const std::string& address) {
  ServerEntry e;
  auto colon = address.find(':');
  e.host = address.substr(0, colon);
  if (colon != std::string::npos) e.port = static_cast<uint16_t>(std::atoi(address.substr(colon + 1).c_str()));
  if (e.port == 0) e.port = kDefaultPort;
  e.name = name.empty() ? address : name;
  if (e.host.empty()) return;
  a.servers.push_back(e);
  saveServers(a);
  a.lastPing = -1e9;
}

void removeServer(App& a, int index) {
  if (index < 0 || index >= static_cast<int>(a.servers.size()) || a.servers[static_cast<size_t>(index)].isLocal()) return;
  a.servers.erase(a.servers.begin() + index);
  saveServers(a);
  a.selectedServer = 0;
}

void leaveServer(App& a, const std::string& error) {
  a.net.disconnect();  // hosted worlds keep running; they save when the game quits
  resetGameUi(a);
  a.connectError = error;
  a.screen = Screen::ServerBrowser;
  a.lastPing = -1e9;
}

}  // namespace sg

int main(int argc, char** argv) {
  auto appPtr = std::make_unique<App>();
  App& a = *appPtr;

  std::string directHost;
  uint16_t directPort = kDefaultPort;
  if (!netInit()) return 1;
  loadSettings(a);
  for (int i = 1; i + 1 < argc; i += 2) {
    if (!std::strcmp(argv[i], "--connect")) {
      std::string hp = argv[i + 1];
      auto colon = hp.find(':');
      directHost = hp.substr(0, colon);
      if (colon != std::string::npos) directPort = static_cast<uint16_t>(std::atoi(hp.substr(colon + 1).c_str()));
    } else if (!std::strcmp(argv[i], "--upnp")) {
      a.noUpnp = !std::strcmp(argv[i + 1], "off");
    } else if (!std::strcmp(argv[i], "--name")) {
      std::snprintf(a.playerName, sizeof(a.playerName), "%s", argv[i + 1]);
    }
  }
  loadServers(a);

  if (!glfwInit()) {
    std::fprintf(stderr, "GLFW init failed\n");
    return 1;
  }
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
  glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);  // required on macOS
  glfwWindowHint(GLFW_SAMPLES, 4);
  a.window = glfwCreateWindow(1600, 900, "Survival Game", nullptr, nullptr);
  if (!a.window) {
    std::fprintf(stderr, "Could not open an OpenGL 3.3 window\n");
    glfwTerminate();
    return 1;
  }
  glfwMakeContextCurrent(a.window);
  glfwSwapInterval(1);
  if (!loadGL(reinterpret_cast<GLLoadFn>(glfwGetProcAddress))) return 1;
  glfwSetScrollCallback(a.window, onScroll);  // before ImGui so it chains to ours

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGui_ImplGlfw_InitForOpenGL(a.window, true);
  ImGui_ImplOpenGL3_Init("#version 330 core");
  float xs = 1, ys = 1;
  glfwGetWindowContentScale(a.window, &xs, &ys);
  uiSetup(a, xs);

  if (!a.renderer.init()) return 1;
  a.browserSocket.open(0);
  hostLocalServers(a);

  if (!directHost.empty()) {
    ServerEntry e;
    e.name = directHost;
    e.host = directHost;
    e.port = directPort;
    e.online = true;
    joinServer(a, e);
  }

  Terrain menuTerrain(1337);
  const std::unordered_set<uint32_t> noDepleted;
  std::array<bool, GLFW_KEY_LAST + 1> keyWas{};
  bool lmbWas = false;
  bool haveCursor = false;
  double lastX = 0, lastY = 0;
  double accumulator = 0;
  double last = glfwGetTime();
  double fpsTimer = 0;
  int frames = 0;
  uint32_t lastContainer = 0;
  PlayerState prevState, currState;
  struct WalkState {
    Vec3 last;
    float phase = 0;
    float amount = 0;
  };
  std::unordered_map<uint32_t, WalkState> walkStates;

  while (!glfwWindowShouldClose(a.window) && !a.quit) {
    glfwPollEvents();
    double now = glfwGetTime();
    double frameDt = std::fmin(now - last, 0.25);
    last = now;
    a.now = now;
    ++frames;
    fpsTimer += frameDt;
    if (fpsTimer >= 0.5) {
      a.fps = static_cast<float>(frames / fpsTimer);
      frames = 0;
      fpsTimer = 0;
    }

    if (a.screen == Screen::MainMenu || a.screen == Screen::ServerBrowser) pingServers(a);
    a.net.update(now);

    // --- screen transitions -----------------------------------------------------------
    if (a.screen == Screen::Connecting) {
      auto st = a.net.status();
      if (st == NetClient::Status::Connected) {
        a.screen = Screen::InGame;
        resetGameUi(a);
        a.yaw = a.net.predicted().yaw;
        a.pitch = 0;
        prevState = currState = a.net.predicted();
        accumulator = 0;
        haveCursor = false;
      } else if (st == NetClient::Status::Rejected) {
        leaveServer(a, std::string("Could not join: ") + rejectReasonText(a.net.rejectReason()));
      } else if (st == NetClient::Status::Disconnected) {
        leaveServer(a, "Could not reach the server. Is it running, and is UDP port open?");
      }
    } else if (a.screen == Screen::InGame && a.net.status() != NetClient::Status::Connected) {
      std::string why = "Disconnected from the server";
      if (a.net.status() == NetClient::Status::Rejected) why = rejectReasonText(a.net.rejectReason());
      else if (!a.net.notices().empty() && a.net.notices().back().kind == TextKind::Warning) why = a.net.notices().back().text;
      leaveServer(a, why);
    }

    // --- input -----------------------------------------------------------------------------
    ImGuiIO& io = ImGui::GetIO();
    auto pressed = [&](int key) {
      bool down = glfwGetKey(a.window, key) == GLFW_PRESS;
      bool edge = down && !keyWas[static_cast<size_t>(key)];
      keyWas[static_cast<size_t>(key)] = down;
      return edge;
    };
    const bool pEsc = pressed(GLFW_KEY_ESCAPE), pTab = pressed(GLFW_KEY_TAB), pI = pressed(GLFW_KEY_I);
    const bool pEnter = pressed(GLFW_KEY_ENTER), pV = pressed(GLFW_KEY_V), pR = pressed(GLFW_KEY_R);
    const bool pE = pressed(GLFW_KEY_E), pP = pressed(GLFW_KEY_P);
    bool pNum[kHotbarSlots];
    for (int i = 0; i < kHotbarSlots; ++i) pNum[i] = pressed(GLFW_KEY_1 + i);
    const bool lmbDown = glfwGetMouseButton(a.window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
    const bool lmbPressed = lmbDown && !lmbWas;
    lmbWas = lmbDown;
    double scroll = g_scroll;
    g_scroll = 0;

    bool captured = false;
    if (a.screen == Screen::InGame) {
      const bool dead = a.net.vitals().dead;
      const bool typing = io.WantTextInput;
      if (pEsc) {
        if (a.chatOpen) a.chatOpen = false;
        else if (a.showPlayers) a.showPlayers = false;
        else if (a.inventoryOpen) {
          a.inventoryOpen = false;
          a.net.closeContainer();
        } else {
          a.pauseOpen = !a.pauseOpen;
        }
      }
      // The server opened a box for us (we pressed E on it): show the inventory.
      if (a.net.openContainer() && a.net.openContainer() != lastContainer && !dead) a.inventoryOpen = true;
      lastContainer = a.net.openContainer();
      if (dead) a.inventoryOpen = false;

      if (!typing) {
        if ((pTab || pI) && !a.pauseOpen && !dead) {
          a.inventoryOpen = !a.inventoryOpen;
          if (!a.inventoryOpen) a.net.closeContainer();
        }
        if (pP && !a.chatOpen) a.showPlayers = !a.showPlayers;
        if (pEnter && !a.inventoryOpen && !a.pauseOpen && !a.chatOpen && !dead) {
          a.chatOpen = true;
          a.chatJustOpened = true;
        }
      }
      const bool uiOpen = a.inventoryOpen || a.pauseOpen || a.chatOpen || a.showPlayers || dead;
      captured = !uiOpen && glfwGetWindowAttrib(a.window, GLFW_FOCUSED);

      if (captured) {
        for (int i = 0; i < kHotbarSlots; ++i)
          if (pNum[i]) a.hotbar = i;
        if (scroll > 0) a.hotbar = (a.hotbar + kHotbarSlots - 1) % kHotbarSlots;
        if (scroll < 0) a.hotbar = (a.hotbar + 1) % kHotbarSlots;
        if (pV) a.thirdPerson = !a.thirdPerson;
        if (pR) a.buildPiece = (a.buildPiece + 1) % static_cast<int>(buildPlanPieces().size());
        if (pE) {
          if (a.lookObject) a.net.interactObject(a.lookObject);
          else if (a.lookNode) a.net.interactNode(a.lookNode);
        }
      }
    }
    setCaptured(a, captured);

    double mx, my;
    glfwGetCursorPos(a.window, &mx, &my);
    if (captured && haveCursor) {
      const float sens = kBaseSensitivity * a.mouseSensitivity;
      a.yaw -= static_cast<float>(mx - lastX) * sens;
      a.pitch = clampf(a.pitch - static_cast<float>(my - lastY) * sens, -kMaxPitch, kMaxPitch);
    }
    haveCursor = captured;
    lastX = mx;
    lastY = my;

    // --- gameplay (fixed 60 Hz input) ----------------------------------------------------------
    Vec3 feet;
    Item held = Item::None;
    if (a.screen == Screen::InGame) {
      const bool dead = a.net.vitals().dead;
      held = dead ? Item::None : a.heldItem();
      const ItemDef& hd = itemDef(held);
      const bool placing = hd.kind == ItemKind::Builder || hd.kind == ItemKind::Deployable;

      accumulator += frameDt;
      while (accumulator >= kInputDt) {
        PlayerInput in;
        if (captured) {
          in.moveZ = static_cast<float>((glfwGetKey(a.window, GLFW_KEY_W) == GLFW_PRESS) -
                                        (glfwGetKey(a.window, GLFW_KEY_S) == GLFW_PRESS));
          in.moveX = static_cast<float>((glfwGetKey(a.window, GLFW_KEY_D) == GLFW_PRESS) -
                                        (glfwGetKey(a.window, GLFW_KEY_A) == GLFW_PRESS));
          if (glfwGetKey(a.window, GLFW_KEY_SPACE) == GLFW_PRESS) in.buttons |= kButtonJump;
          if (glfwGetKey(a.window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS) in.buttons |= kButtonSprint;
          if (lmbDown && !placing) in.buttons |= kButtonPrimary;
        }
        in.yaw = a.yaw;
        in.pitch = a.pitch;
        in.slot = static_cast<uint8_t>(a.hotbar);
        prevState = a.net.predicted();
        a.net.sendInput(in);  // keep sending while dead so the server knows we're still here
        currState = a.net.predicted();
        accumulator -= kInputDt;
      }
      float alpha = static_cast<float>(accumulator / kInputDt);
      feet = lerp(prevState.pos, currState.pos, alpha);

      // Local attack animation, timed like the server's cooldown.
      if (captured && lmbDown && !placing && now >= a.nextLocalUse && !dead) {
        bool canFire = hd.kind != ItemKind::Gun || countItem(a.net.inventory(), hd.ammo) > 0;
        if (canFire) {
          a.swingStart = now;
          a.nextLocalUse = now + hd.useTime;
          if (hd.kind == ItemKind::Gun) a.lastMuzzle = now;
        }
      }

      // What are we looking at? (E prompts, building preview)
      const Vec3 eye = feet + Vec3{0, kEyeHeight, 0};
      const Vec3 dir = directionFromYawPitch(a.yaw, a.pitch);
      a.prompt.clear();
      a.lookObject = a.lookNode = 0;
      a.havePlacement = false;
      if (!dead) {
        float best = 3.5f;
        if (auto h = a.net.world().raycast(eye, dir, best, true)) {
          if (const WorldObject* o = a.net.world().find(h->objectId)) {
            best = h->dist;
            a.lookObject = o->id;
            switch (o->type) {
              case ObjType::Door:
                a.prompt = o->owner == a.net.playerId() ? (o->flags & kDoorOpen ? "E  Close door" : "E  Open door") : "Locked door";
                break;
              case ObjType::StorageBox: a.prompt = "E  Open storage box"; break;
              case ObjType::LootBag: a.prompt = "E  Open loot bag"; break;
              case ObjType::SleepingBag: a.prompt = o->owner == a.net.playerId() ? "Your sleeping bag" : "Sleeping bag"; break;
              default: {
                char buf[96];
                std::snprintf(buf, sizeof(buf), "%s %s   %.0f / %.0f", tierName(o->tier), objName(o->type), o->hp,
                              maxHp(o->type, o->tier));
                a.prompt = buf;
                a.lookObject = 0;  // nothing to press E on
                break;
              }
            }
          }
        }
        std::vector<ResourceNode> nodes;
        nodesInRect(a.net.terrain(), eye.x - 4, eye.z - 4, eye.x + 4, eye.z + 4, nodes);
        for (const auto& n : nodes) {
          if (a.net.depletedNodes().count(n.id)) continue;
          float t;
          if (!rayAABB(eye, dir, nodeBounds(n), best, t)) continue;
          best = t;
          const NodeInfo& info = nodeInfo(n.type);
          a.lookObject = 0;
          if (info.pickup) {
            a.lookNode = n.id;
            a.prompt = std::string("E  Pick ") + info.name;
          } else {
            a.prompt = info.name;
          }
        }
        // Building plan / deployable preview.
        if (placing) {
          ObjType type = hd.kind == ItemKind::Builder ? buildPlanPieces()[static_cast<size_t>(a.buildPiece)] : deployableFor(held);
          a.placement = computePlacement(type, a.net.terrain(), a.net.world(), eye, dir, a.yaw);
          a.havePlacement = true;
          if (captured && lmbPressed) {
            if (!a.placement.error) {
              a.net.place(a.placement.type, a.placement.pos, a.placement.rot);
              a.swingStart = now;
            }
          }
        }
      }
    }

    // --- render ------------------------------------------------------------------------------
    int fbw, fbh;
    glfwGetFramebufferSize(a.window, &fbw, &fbh);
    a.renderer.resize(fbw, fbh);
    Camera cam;
    if (a.screen == Screen::InGame) {
      cam.yaw = a.yaw;  // mouse look is applied instantly, not at 60 Hz
      cam.pitch = a.pitch;
      cam.eye = feet + Vec3{0, kEyeHeight, 0};
      if (a.thirdPerson) cam.eye = cam.eye - directionFromYawPitch(cam.yaw, cam.pitch) * 4.0f + Vec3{0, 0.5f, 0};
      a.renderer.beginFrame(cam, static_cast<float>(now), a.net.timeOfDay());
      a.renderer.drawWorld(a.net.terrain(), feet, a.net.depletedNodes(), a.net.nodesVersion());
      a.renderer.drawObjects(a.net.world());
      // Walk cycles: advance each player's phase by the distance they moved.
      auto walk = [&](uint32_t id, const Vec3& pos) {
        WalkState& w = walkStates[id];
        Vec3 d = pos - w.last;
        float dist = std::sqrt(d.x * d.x + d.z * d.z);
        if (dist > 2.0f) dist = 0;  // teleport / respawn
        w.last = pos;
        w.phase += dist * 2.4f;
        float speed = frameDt > 0 ? dist / static_cast<float>(frameDt) : 0;
        w.amount = lerpf(w.amount, clampf(speed / kSprintSpeed, 0, 1), clampf(static_cast<float>(frameDt) * 10, 0, 1));
        return w;
      };
      for (const auto& p : a.net.remotePlayers(now)) {
        WalkState w = walk(p.id, p.pos);
        PlayerLook look;
        look.shirt = colorForId(p.id);
        look.held = p.held;
        look.dead = p.flags & kEntityDead;
        look.attacking = p.flags & kEntityAttacking;
        look.walkPhase = w.phase;
        look.walkAmount = w.amount;
        a.renderer.drawPlayer(p.pos, p.yaw, p.pitch, look);
      }
      const bool dead = a.net.vitals().dead;
      WalkState self = walk(a.net.playerId(), feet);
      if (a.thirdPerson && !dead) {
        PlayerLook look;
        look.shirt = colorForId(a.net.playerId());
        look.held = held;
        look.attacking = now - a.swingStart < 0.15;
        look.walkPhase = self.phase;
        look.walkAmount = self.amount;
        a.renderer.drawPlayer(feet, a.yaw, a.pitch, look);
      }
      if (a.havePlacement) {
        WorldObject ghost;
        ghost.type = a.placement.type;
        ghost.pos = a.placement.pos;
        ghost.rot = a.placement.rot;
        a.renderer.drawGhost(ghost, a.placement.error == nullptr);
      }
      if (!a.thirdPerson && !dead) {
        float useTime = itemDef(held).useTime;
        float swing = static_cast<float>(clampf(static_cast<float>((now - a.swingStart) / std::fmax(0.2, useTime * 0.6)), 0.0f, 1.0f));
        if (swing >= 1.0f) swing = 0.0f;
        a.renderer.drawViewModel(held, swing, now - a.lastMuzzle < 0.05, self.phase, a.net.predicted().onGround ? self.amount : 0.0f);
      }
    } else {
      // Menu background: slowly look around the island.
      Vec3 eye{2400.0f, 0, 2300.0f};
      eye.y = std::fmax(menuTerrain.heightAt(eye.x, eye.z), 0.0f) + 28.0f;
      cam.eye = eye;
      cam.yaw = static_cast<float>(now * 0.04);
      cam.pitch = -0.12f;
      a.renderer.beginFrame(cam, static_cast<float>(now), 17.2f);  // golden hour for the menu
      a.renderer.drawWorld(menuTerrain, eye, noDepleted, 1);
    }
    a.renderer.endFrame();

    // --- UI ----------------------------------------------------------------------------------
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    switch (a.screen) {
      case Screen::MainMenu: uiMainMenu(a); break;
      case Screen::ServerBrowser: uiServerBrowser(a); break;
      case Screen::Connecting: uiConnecting(a); break;
      case Screen::InGame: uiInGame(a); break;
    }
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    glfwSwapBuffers(a.window);
  }

  leaveServer(a);
  stopHostedServers(a);
  saveSettings(a);
  a.renderer.shutdown();
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();
  glfwDestroyWindow(a.window);
  glfwTerminate();
  a.browserSocket.close();
  netShutdown();
  return 0;
}
