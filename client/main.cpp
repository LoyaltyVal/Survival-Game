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

#include <algorithm>
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

// What a footstep at `feet` sounds like: whatever is under it, matching how the ground is drawn
// (sand on the beach, rock on steep slopes, snow on the peaks, grass elsewhere) or the floor you stand on.
Sound stepSound(const App& a, const Vec3& feet) {
  const Terrain& t = a.net.terrain();
  const float ground = t.heightAt(feet.x, feet.z);
  if (feet.y < kSeaLevel - 0.15f && ground < kSeaLevel) return Sound::Swim;  // wading or swimming
  if (feet.y > ground + 0.08f) {
    if (auto h = a.net.world().raycast(feet + Vec3{0, 0.3f, 0}, {0, -1, 0}, 0.6f, false))
      if (const WorldObject* o = a.net.world().find(h->objectId))
        return o->tier == Tier::Metal || (o->flags & kMonument) ? Sound::StepMetal
               : o->tier == Tier::Stone                       ? Sound::StepRock
                                                              : Sound::StepWood;
  }
  if (ground < 2.0f) return Sound::StepSand;
  if (1.0f - t.normalAt(feet.x, feet.z).y > 0.19f) return Sound::StepRock;
  if (ground > 63.0f) return Sound::StepSnow;
  return Sound::StepGrass;
}

// Your own character: the look everyone else sees, wearing what is in your clothing slots.
PlayerLook selfLook(const App& a) {
  PlayerLook look = lookFor(a.net.playerId());
  for (int k = 0; k < kArmorSlots; ++k) look.wear[static_cast<size_t>(k)] = wornAt(a.net.inventory(), static_cast<ArmorSlot>(k));
  return look;
}

// --- settings and server list files ------------------------------------------------------
void loadSettings(App& a) {
  std::ifstream f("settings.txt");
  std::string line;
  while (std::getline(f, line)) {
    auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    std::string key = line.substr(0, eq), value = line.substr(eq + 1);
    const float num = std::strtof(value.c_str(), nullptr);
    const bool on = value == "1";
    if (key == "name" && !value.empty()) std::snprintf(a.playerName, sizeof(a.playerName), "%s", value.c_str());
    else if (key == "sensitivity") a.mouseSensitivity = clampf(num, 0.2f, 3.0f);
    else if (key == "volume") a.volume = clampf(num, 0.0f, 1.0f);
    else if (key == "invert_y") a.invertY = on;
    else if (key == "fov") a.fov = clampf(num, 55.0f, 100.0f);
    else if (key == "fullscreen") a.fullscreen = on;
    else if (key == "vsync") a.vsync = on;
    else if (key == "show_fps") a.showFps = on;
    else if (key == "ambience") a.ambienceVolume = clampf(num, 0.0f, 1.0f);
    else if (key == "voice_volume") a.voiceVolume = clampf(num, 0.0f, 2.0f);
    else if (key == "mic_gain") a.micGain = clampf(num, 0.2f, 4.0f);
    else if (key == "voice_mode") a.voiceMode = std::clamp(std::atoi(value.c_str()), 0, 2);
    else if (key == "voice_threshold") a.voiceThreshold = clampf(num, 0.005f, 0.3f);
  }
}

}  // namespace

void sg::saveSettings(const App& a) {
  std::ofstream f("settings.txt");
  f << "name=" << a.playerName << "\nsensitivity=" << a.mouseSensitivity << "\nvolume=" << a.volume
    << "\ninvert_y=" << a.invertY << "\nfov=" << a.fov << "\nfullscreen=" << a.fullscreen << "\nvsync=" << a.vsync
    << "\nshow_fps=" << a.showFps << "\nambience=" << a.ambienceVolume << "\nvoice_volume=" << a.voiceVolume
    << "\nmic_gain=" << a.micGain << "\nvoice_mode=" << a.voiceMode << "\nvoice_threshold=" << a.voiceThreshold << "\n";
}

void sg::applyVideo(App& a) {
  GLFWmonitor* current = glfwGetWindowMonitor(a.window);
  if (a.fullscreen && !current) {
    glfwGetWindowPos(a.window, &a.windowedX, &a.windowedY);
    glfwGetWindowSize(a.window, &a.windowedW, &a.windowedH);
    GLFWmonitor* m = glfwGetPrimaryMonitor();
    if (const GLFWvidmode* mode = m ? glfwGetVideoMode(m) : nullptr)
      glfwSetWindowMonitor(a.window, m, 0, 0, mode->width, mode->height, mode->refreshRate);
  } else if (!a.fullscreen && current) {
    glfwSetWindowMonitor(a.window, nullptr, a.windowedX, a.windowedY, a.windowedW, a.windowedH, 0);
  }
  glfwSwapInterval(a.vsync ? 1 : 0);
}

namespace {

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
  a.audio.clearVoices();
  a.speaking.clear();
  a.transmitting = false;
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
    } else if (!std::strcmp(argv[i], "--host")) {
      a.noHost = !std::strcmp(argv[i + 1], "off");
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
  applyVideo(a);  // fullscreen and vsync from settings.txt
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
  a.audio.init("sounds");  // creates sounds/*.wav on first run; replace any of them with your own
  a.audio.setVolume(a.volume);
  if (!a.noHost) hostLocalServers(a);

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
    const bool pEnter = pressed(GLFW_KEY_ENTER), pF5 = pressed(GLFW_KEY_F5), pR = pressed(GLFW_KEY_R);
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
        if (a.settingsOpen) {
          a.settingsOpen = false;  // back to the pause menu
          saveSettings(a);
        } else if (a.codeOpen) a.codeOpen = false;
        else if (a.chatOpen) a.chatOpen = false;
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
      uint32_t promptObj;
      uint8_t promptMode;
      if (a.net.takeCodePrompt(promptObj, promptMode)) {  // a lock wants a code
        a.codeOpen = true;
        a.codeObject = promptObj;
        a.codeMode = promptMode;
        a.codeBuf[0] = 0;
      }
      const bool uiOpen = a.inventoryOpen || a.pauseOpen || a.chatOpen || a.showPlayers || a.codeOpen || a.settingsOpen || dead;
      captured = !uiOpen && glfwGetWindowAttrib(a.window, GLFW_FOCUSED);

      if (captured) {
        const int before = a.hotbar;
        for (int i = 0; i < kHotbarSlots; ++i)
          if (pNum[i]) a.hotbar = i;
        if (scroll > 0) a.hotbar = (a.hotbar + kHotbarSlots - 1) % kHotbarSlots;
        if (scroll < 0) a.hotbar = (a.hotbar + 1) % kHotbarSlots;
        if (a.hotbar != before) {  // switching weapons cancels a reload and resets the recoil pattern
          a.reloadEnd = 0;
          a.burstShots = 0;
        }
        if (pF5) a.thirdPerson = !a.thirdPerson;
        if (pR) {
          if (itemDef(a.heldItem()).kind == ItemKind::Gun) a.wantReload = true;
          else a.buildPiece = (a.buildPiece + 1) % static_cast<int>(buildPlanPieces().size());
        }
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
      // Slower mouse when zoomed in, so aiming through a scope feels the same as at the hip.
      const float sens = kBaseSensitivity * a.mouseSensitivity / lerpf(1.0f, a.zoom, a.aimT);
      a.yaw -= static_cast<float>(mx - lastX) * sens;
      a.pitch = clampf(a.pitch - static_cast<float>(my - lastY) * sens * (a.invertY ? -1.0f : 1.0f), -kMaxPitch, kMaxPitch);
    }
    haveCursor = captured;
    lastX = mx;
    lastY = my;

    // --- proximity voice chat ---------------------------------------------------------------
    {
      const bool inGame = a.screen == Screen::InGame;
      const bool wantMic = (inGame && a.voiceMode != 0) || a.micTest;
      a.micTest = false;  // the settings window asks again every frame it shows the meter
      if (wantMic && !a.mic.running() && !a.micFailed) a.micFailed = !a.mic.start();
      if (!wantMic && a.mic.running()) a.mic.stop();
      if (!wantMic) a.micFailed = false;  // try again next time
      bool talk = false;
      if (inGame && a.voiceMode == 1)  // push to talk: hold V (not while typing)
        talk = glfwGetKey(a.window, GLFW_KEY_V) == GLFW_PRESS && !io.WantTextInput && !a.chatOpen &&
               glfwGetWindowAttrib(a.window, GLFW_FOCUSED);
      std::vector<int16_t> frame;
      while (a.mic.pop(frame)) {
        float sum = 0;
        for (auto& x : frame) {
          x = static_cast<int16_t>(std::clamp(static_cast<float>(x) * a.micGain, -32768.0f, 32767.0f));
          sum += static_cast<float>(x) * static_cast<float>(x);
        }
        const float rms = std::sqrt(sum / static_cast<float>(frame.size())) / 32768.0f;
        if (inGame && a.voiceMode == 2) {  // open mic: send while you're louder than the threshold, plus a short tail
          if (rms > a.voiceThreshold) a.voiceHold = now + 0.4;
          talk = now < a.voiceHold;
        }
        if (talk && inGame && !a.net.vitals().dead) a.net.sendVoice(frame.data(), frame.size());
      }
      a.transmitting = talk && inGame && !a.net.vitals().dead;
      for (auto& chunk : a.net.takeVoice()) {
        a.audio.voice(chunk.speaker, chunk.pcm.data(), chunk.pcm.size(), a.voiceVolume);
        a.speaking[chunk.speaker] = now;
      }
    }

    // --- gameplay (fixed 60 Hz input) ----------------------------------------------------------
    Vec3 feet;
    Item held = Item::None;
    ItemStack heldStack;
    static std::mt19937 fx(12345);
    std::uniform_real_distribution<float> rnd(0.0f, 1.0f);
    if (a.screen == Screen::InGame) {
      const bool dead = a.net.vitals().dead;
      if (!dead) heldStack = a.net.inventory()[static_cast<size_t>(a.hotbar)];
      held = heldStack.empty() ? Item::None : heldStack.item;
      const ItemDef& hd = itemDef(held);
      const bool isGun = hd.kind == ItemKind::Gun;
      const bool placing = hd.kind == ItemKind::Builder || hd.kind == ItemKind::Deployable || hd.kind == ItemKind::Explosive;
      const GunStats gs = isGun ? gunStats(heldStack) : GunStats{};
      const int reserve = isGun ? countItem(a.net.inventory(), hd.ammo) : 0;

      // Our own idea of the magazine, so firing feels instant; the server's count wins when it arrives.
      if (a.net.inventoryVersion() != a.seenInvVersion || a.hotbar != a.predSlot) {
        if (!(isGun && now < a.reloadEnd)) a.predMag = heldStack.data;
        a.seenInvVersion = a.net.inventoryVersion();
        a.predSlot = a.hotbar;
      }
      bool reloading = now < a.reloadEnd;
      if (a.reloadEnd > 0 && !reloading) {  // our reload finished
        a.reloadEnd = 0;
        a.predMag = std::min<int>(hd.magSize, a.predMag + reserve);
      }
      auto beginReload = [&] {
        if (!isGun || reloading || a.predMag >= hd.magSize || reserve == 0) return;
        a.reloadEnd = now + hd.reloadTime;
        a.reloadInputs = 4;
        reloading = true;
        a.audio.play(Sound::Reload, nullptr, 0.8f, clampf(2.0f / hd.reloadTime, 0.6f, 1.3f));
      };
      if (a.wantReload) {
        beginReload();
        a.wantReload = false;
      }

      // Aiming down sights.
      const bool rmb = glfwGetMouseButton(a.window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
      const bool aiming = captured && rmb && isGun && !reloading && !dead;
      a.aimT = clampf(a.aimT + (aiming ? 1.0f : -1.0f) * static_cast<float>(frameDt) * 7.0f, 0.0f, 1.0f);
      a.zoom = isGun ? gs.zoom : 1.0f;
      a.scoped = isGun && gs.scoped && a.aimT > 0.92f;
      a.holo = isGun && gs.holo && a.aimT > 0.85f;
      a.spreadNow = isGun ? lerpf(gs.spread, gs.adsSpread, a.aimT) : 0.0f;

      // Firing: predicted locally for instant feedback (sound, flash, recoil); the server decides hits.
      if (now - a.lastShot > std::fmax(0.3, hd.useTime * 1.8)) a.burstShots = 0;
      if (isGun && captured && lmbDown && !dead && now >= a.nextLocalUse && !reloading && (hd.automatic || lmbPressed)) {
        if (a.predMag > 0) {
          --a.predMag;
          a.nextLocalUse = now + hd.useTime;
          a.lastShot = now;
          a.swingStart = now;
          if (!gs.silenced && held != Item::Bow) a.lastMuzzle = now;
          a.audio.play(gs.silenced ? Sound::Silenced : hd.sound, nullptr, 1.0f, 0.97f + 0.06f * rnd(fx));
          // The recoil pattern: same kick every time for the Nth shot, plus a little noise.
          float ky, kp;
          recoilKick(held, a.burstShots++, ky, kp);
          float scale = (hd.recoil > 0 ? gs.recoil / hd.recoil : 1.0f) * (aiming ? 0.8f : 1.0f);
          ky += (rnd(fx) - 0.5f) * 0.2f * kp;
          kp *= 0.9f + 0.2f * rnd(fx);
          a.recoilYaw += ky * scale * kPi / 180.0f;
          a.recoilPitch += kp * scale * kPi / 180.0f;
        } else if (lmbPressed) {
          if (reserve > 0) beginReload();
          else a.audio.play(Sound::DryFire, nullptr, 0.8f);
        }
      }
      // Melee / eating animation, timed like the server's cooldown.
      if (!isGun && captured && lmbDown && !placing && hd.kind != ItemKind::Lock && now >= a.nextLocalUse && !dead) {
        a.swingStart = now;
        a.nextLocalUse = now + hd.useTime;
        if (hd.kind == ItemKind::Tool) a.audio.play(Sound::Swing, nullptr, 0.5f, 0.9f + 0.2f * rnd(fx));
      }
      // Recoil moves your real aim (that's what you learn to pull against), spread over ~60 ms.
      {
        float take = clampf(static_cast<float>(frameDt) / 0.06f, 0.0f, 1.0f);
        a.yaw += a.recoilYaw * take;
        a.pitch = clampf(a.pitch + a.recoilPitch * take, -kMaxPitch, kMaxPitch);
        a.recoilYaw -= a.recoilYaw * take;
        a.recoilPitch -= a.recoilPitch * take;
      }

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
          if (lmbDown && !placing && hd.kind != ItemKind::Lock) in.buttons |= kButtonPrimary;
          if (aiming) in.buttons |= kButtonAim;
        }
        if (a.reloadInputs > 0) {
          in.buttons |= kButtonReload;
          --a.reloadInputs;
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

      // What are we looking at? (E prompts, code locks, building preview)
      const Vec3 eye = feet + Vec3{0, kEyeHeight, 0};
      const Vec3 dir = directionFromYawPitch(a.yaw, a.pitch);
      a.prompt.clear();
      a.lookObject = a.lookNode = a.lookAny = 0;
      a.havePlacement = false;
      if (!dead) {
        float best = 3.5f;
        if (auto h = a.net.world().raycast(eye, dir, best, true)) {
          if (const WorldObject* o = a.net.world().find(h->objectId)) {
            best = h->dist;
            a.lookObject = o->id;
            a.lookAny = o->id;
            const bool locked = o->flags & kLocked;
            const char* lockNote = locked ? "  [code lock]" : "";
            switch (o->type) {
              case ObjType::Door: a.prompt = std::string(o->flags & kDoorOpen ? "E  Close door" : "E  Open door") + lockNote; break;
              case ObjType::StorageBox: a.prompt = std::string("E  Open storage box") + lockNote; break;
              case ObjType::LargeBox: a.prompt = std::string("E  Open large box") + lockNote; break;
              case ObjType::Stash: a.prompt = std::string("E  Open stash") + lockNote; break;
              case ObjType::LandClaim: a.prompt = std::string("E  Authorize / deauthorize (25 m building rights)") + lockNote; break;
              case ObjType::LootBag: a.prompt = "E  Open dropped items"; break;
              case ObjType::BodyBag: {
                auto who = a.net.players().find(o->owner);
                a.prompt = o->owner == a.net.playerId() ? "E  Search your body"
                           : who != a.net.players().end() ? "E  Search " + who->second.name + "'s body"
                                                          : std::string("E  Search body bag");
                break;
              }
              case ObjType::Crate: a.prompt = "E  Open military crate"; break;
              case ObjType::Ladder: a.prompt = "W  Climb   S  Climb down"; a.lookObject = 0; break;
              case ObjType::RigLeg: case ObjType::Railing: a.prompt.clear(); a.lookObject = 0; break;
              case ObjType::Charge: a.prompt = "Explosive! Run!"; a.lookObject = 0; break;
              case ObjType::SleepingBag: a.prompt = o->owner == a.net.playerId() ? "Your sleeping bag" : "Sleeping bag"; break;
              default: {
                if (o->flags & kMonument) {  // oil rig decks and walls: nothing to do with them
                  a.lookObject = 0;
                  break;
                }
                char buf[96];
                std::snprintf(buf, sizeof(buf), "%s %s   %.0f / %.0f", tierName(o->tier), objName(o->type), o->hp,
                              maxHp(o->type, o->tier));
                a.prompt = buf;
                a.lookObject = 0;  // nothing to press E on
                break;
              }
            }
            if (hd.kind == ItemKind::Lock && isLockable(o->type)) {
              if (locked) a.prompt = "Already has a lock";
              else if (o->owner != a.net.playerId()) a.prompt = "You can only lock your own things";
              else a.prompt = "Click  Put the code lock on";
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
          a.lookObject = a.lookAny = 0;
          if (info.pickup) {
            a.lookNode = n.id;
            a.prompt = std::string("E  Pick up ") + info.name;
          } else {
            a.prompt = info.name;
          }
        }
        if (hd.kind == ItemKind::Lock && captured && lmbPressed && a.lookAny) a.net.useOn(a.lookAny);
        // Building plan / deployable / explosive preview.
        if (placing) {
          ObjType type = hd.kind == ItemKind::Builder ? buildPlanPieces()[static_cast<size_t>(a.buildPiece)] : deployableFor(held);
          a.placement = computePlacement(type, a.net.terrain(), a.net.world(), eye, dir, a.yaw);
          a.havePlacement = true;
          if (captured && lmbPressed && !a.placement.error) {
            a.net.place(a.placement.type, a.placement.pos, a.placement.rot, held);
            a.swingStart = now;
          }
        }
      }

      // Sounds the server sent (other players' shots, hits, doors, explosions).
      for (const auto& ev : a.net.takeSounds()) {
        if (!ev.positional) {
          a.audio.play(ev.sound, nullptr, 0.7f);
          continue;
        }
        a.audio.play(ev.sound, &ev.pos, 1.0f, 0.95f + 0.1f * rnd(fx));
        if (ev.sound == Sound::Explosion) {
          a.blasts.push_back({ev.pos, now});
          float dist = length(ev.pos - feet);
          a.shake = std::fmax(a.shake, clampf(1.0f - dist / 45.0f, 0.0f, 1.0f));
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
      cam.fovY = a.fov * kPi / 180.0f / lerpf(1.0f, a.zoom, a.aimT);
      if (a.shake > 0) {  // explosions rattle the camera
        cam.yaw += (rnd(fx) - 0.5f) * 0.03f * a.shake;
        cam.pitch += (rnd(fx) - 0.5f) * 0.03f * a.shake;
        a.shake = std::fmax(0.0f, a.shake - static_cast<float>(frameDt) * 1.5f);
      }
      a.audio.setListener(cam.eye, directionFromYawPitch(cam.yaw, cam.pitch));
      {  // Ambience: surf near the coast, wind up high, birds by day and crickets at night.
        static double nextSample = 0;
        static float wet = 0;
        const Terrain& tr = a.net.terrain();
        if (now >= nextSample) {
          nextSample = now + 0.5;
          int hits = 0, total = 0;
          for (float r : {12.0f, 35.0f, 70.0f})
            for (int k = 0; k < 8; ++k, ++total) {
              float ang = static_cast<float>(k) * kPi / 4;
              if (tr.heightAt(cam.eye.x + std::cos(ang) * r, cam.eye.z + std::sin(ang) * r) < kSeaLevel) ++hits;
            }
          wet = static_cast<float>(hits) / static_cast<float>(total);
        }
        const float tod = a.net.timeOfDay();
        const float day = clampf((tod - 5.0f) / 1.5f, 0, 1) * clampf((20.5f - tod) / 1.5f, 0, 1);
        const float alt = cam.eye.y;
        const float amb = a.ambienceVolume;
        a.audio.setAmbience((0.3f + clampf(alt / 90.0f, 0, 0.7f)) * amb, wet * clampf(1.0f - (alt - 15.0f) / 120.0f, 0.25f, 1.0f) * amb,
                            day * (1 - wet) * clampf(1.0f - alt / 120.0f, 0, 1) * amb, (1 - day) * (1 - wet) * amb,
                            static_cast<float>(frameDt));
      }
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
        float before = w.phase;
        w.phase += dist * 2.4f;
        // A footstep each half stride (every ~1.3 m), sounding like what they walk on.
        if (std::floor(before / kPi) != std::floor(w.phase / kPi) && dist > 0) {
          const Sound step = stepSound(a, pos);
          if (id == a.net.playerId()) {
            if (a.net.predicted().onGround || step == Sound::Swim)
              a.audio.play(step, nullptr, step == Sound::Swim ? 0.5f : 0.35f, 0.85f + 0.3f * rnd(fx));
          } else if (length(pos - feet) < 35.0f) {
            Vec3 at = pos;
            a.audio.play(step, &at, 0.6f, 0.85f + 0.3f * rnd(fx));
          }
        }
        float speed = frameDt > 0 ? dist / static_cast<float>(frameDt) : 0;
        w.amount = lerpf(w.amount, clampf(speed / kSprintSpeed, 0, 1), clampf(static_cast<float>(frameDt) * 10, 0, 1));
        return w;
      };
      for (const auto& p : a.net.remotePlayers(now)) {
        WalkState w = walk(p.id, p.pos);
        PlayerLook look = lookFor(p.id);
        look.wear = p.wear;
        look.held = p.held;
        look.dead = p.flags & kEntityDead;
        look.attacking = p.flags & kEntityAttacking;
        look.walkPhase = w.phase;
        look.walkAmount = w.amount;
        a.renderer.drawPlayer(p.pos, p.yaw, p.pitch, look);
        a.audio.setVoicePosition(p.id, p.pos + Vec3{0, kEyeHeight, 0});  // their voice comes from their head
      }
      const bool dead = a.net.vitals().dead;
      WalkState self = walk(a.net.playerId(), feet);
      if (a.thirdPerson && !dead) {
        PlayerLook look = selfLook(a);
        look.held = held;
        look.attacking = now - a.swingStart < 0.15;
        look.walkPhase = self.phase;
        look.walkAmount = self.amount;
        a.renderer.drawPlayer(feet, a.yaw, a.pitch, look);
      }
      for (auto it = a.blasts.begin(); it != a.blasts.end();) {
        float age = static_cast<float>(now - it->time);
        if (age > 4.5f) {
          it = a.blasts.erase(it);
          continue;
        }
        a.renderer.drawExplosion(it->pos, age);
        ++it;
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
        if (itemDef(held).kind == ItemKind::Gun)  // guns: a short recoil kick, not a full swing
          swing = clampf(static_cast<float>((now - a.swingStart) / 0.12), 0.0f, 1.0f) < 1.0f
                      ? static_cast<float>((now - a.swingStart) / 0.12)
                      : 0.0f;
        a.renderer.drawViewModel(heldStack, swing, now - a.lastMuzzle < 0.05, self.phase,
                                 a.net.predicted().onGround ? self.amount : 0.0f, a.aimT, a.scoped, selfLook(a));
      }
    } else {
      // Menu background: slowly look around the island.
      Vec3 eye{2400.0f, 0, 2300.0f};
      eye.y = std::fmax(menuTerrain.heightAt(eye.x, eye.z), 0.0f) + 28.0f;
      cam.eye = eye;
      cam.yaw = static_cast<float>(now * 0.04);
      cam.pitch = -0.12f;
      a.audio.setAmbience(0.4f * a.ambienceVolume, 0.3f * a.ambienceVolume, 0.35f * a.ambienceVolume, 0.0f, static_cast<float>(frameDt));  // quiet island air behind the menu
      a.renderer.beginFrame(cam, static_cast<float>(now), 17.2f);  // golden hour for the menu
      a.renderer.drawWorld(menuTerrain, eye, noDepleted, 1);
    }
    a.renderer.endFrame();
    a.audio.setVolume(a.volume);

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
    if (a.settingsOpen) uiSettings(a);
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    glfwSwapBuffers(a.window);
  }

  leaveServer(a);
  stopHostedServers(a);
  a.audio.shutdown();
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
