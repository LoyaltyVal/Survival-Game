// Client application state shared by the main loop (main.cpp) and the UI (ui.cpp).
#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "net.h"
#include "net_client.h"
#include "renderer.h"
#include "server.h"
#include "sfx.h"
#include "upnp.h"
#include "voice.h"
#include "world.h"

struct GLFWwindow;
struct ImFont;

namespace sg {

struct ServerEntry {
  std::string name;
  std::string host;
  uint16_t port = kDefaultPort;
  uint32_t seed = 1337;  // map used when we host it ourselves
  // Live status from server-browser pings.
  bool online = false;
  int players = 0;
  int maxPlayers = 0;
  float pingMs = -1;
  double lastReply = -1e9;
  double sentAt = 0;
  uint32_t nonce = 0;
  bool isLocal() const { return host == "127.0.0.1" || host == "localhost"; }
};

enum class Screen { MainMenu, ServerBrowser, Connecting, InGame };

struct App {
  GLFWwindow* window = nullptr;
  Renderer renderer;
  NetClient net;
  Screen screen = Screen::MainMenu;
  double now = 0;
  float fps = 0;
  bool quit = false;

  // settings (saved to settings.txt, changed in the Settings window)
  char playerName[21] = "Survivor";
  float mouseSensitivity = 1.0f;
  bool invertY = false;
  float fov = 75.0f;           // degrees, vertical
  bool fullscreen = false;
  bool vsync = true;
  bool showFps = true;
  float ambienceVolume = 1.0f; // wind, waves, birds, crickets
  float voiceVolume = 1.0f;    // other players talking
  float micGain = 1.0f;
  int voiceMode = 1;           // 0 off, 1 push to talk (V), 2 open mic
  float voiceThreshold = 0.04f;  // open mic: how loud before it transmits
  bool settingsOpen = false;
  int windowedX = 100, windowedY = 100, windowedW = 1600, windowedH = 900;  // restored when leaving fullscreen

  // proximity voice chat
  Microphone mic;
  bool micFailed = false;
  bool micTest = false;        // the Settings voice tab is showing its level meter
  bool transmitting = false;
  double voiceHold = 0;        // open mic keeps sending a moment after you stop talking
  std::unordered_map<uint32_t, double> speaking;  // player id -> last time we heard them

  // server browser
  std::vector<ServerEntry> servers;
  int selectedServer = 0;
  char directConnect[64] = "";
  std::string connectError;
  std::string connectingTo;
  double connectStart = 0;
  UdpSocket browserSocket;
  double lastPing = -1e9;

  // Worlds this game hosts itself: every listed 127.0.0.1 server, started at
  // launch and kept running (and saving) until the game quits.
  struct HostedServer {
    std::unique_ptr<Server> server;
    std::thread thread;
    std::atomic<bool> running{false};
  };
  std::vector<std::unique_ptr<HostedServer>> hosted;
  bool noUpnp = false;                  // --upnp off: don't touch the router
  bool noHost = false;                  // --host off: don't host the listed local worlds
  PortForwarder forwarder;              // opens the hosted ports on the router for internet friends
  std::vector<std::string> lanAddresses;  // this PC's address on the home network

  // "Add server" dialog
  char addName[48] = "";
  char addAddress[64] = "";
  bool showPlayers = false;

  // in-game UI state
  bool inventoryOpen = false;
  bool pauseOpen = false;
  bool chatOpen = false;
  bool chatJustOpened = false;
  char chatBuf[128] = "";
  int hotbar = 0;
  int buildPiece = 0;
  bool thirdPerson = false;
  float yaw = 0, pitch = 0;
  bool havePlacement = false;
  Placement placement;
  std::string prompt;            // "E  Open door" etc.
  uint32_t lookObject = 0;       // object under the crosshair (for E)
  uint32_t lookNode = 0;         // pickup plant under the crosshair (for E)
  double swingStart = -10;
  double nextLocalUse = 0;
  double lastMuzzle = -10;

  // guns
  float aimT = 0;               // 0 hip .. 1 aiming down sights (smoothed)
  float zoom = 1;               // current gun's sight magnification
  bool scoped = false;          // looking through a 4x/8x scope (overlay, no view model)
  bool holo = false;            // red dot sight
  float spreadNow = 0;          // current cone, for the crosshair
  float recoilYaw = 0, recoilPitch = 0;  // kick still to be applied to the view (radians)
  int burstShots = 0;           // shot index in the current burst (drives the recoil pattern)
  double lastShot = -10;
  double reloadEnd = 0;         // local reload timer (the server does the real one)
  int reloadInputs = 0;         // send the reload button for a few inputs
  bool wantReload = false;
  int predMag = 0, predSlot = -1;
  uint32_t seenInvVersion = 0;
  float shake = 0;              // camera shake from nearby explosions
  struct Blast {
    Vec3 pos;
    double time;
  };
  std::vector<Blast> blasts;
  int selectedGun = -1;         // inventory slot shown in the weapon/attachments panel

  // code lock keypad
  bool codeOpen = false;
  uint32_t codeObject = 0;
  uint8_t codeMode = 0;
  char codeBuf[8] = "";
  uint32_t lookAny = 0;         // any object under the crosshair (for code locks)

  // audio
  Audio audio;
  float volume = 0.8f;
  double stepPhase = 0;

  ImFont* fontBody = nullptr;
  ImFont* fontBig = nullptr;
  ImFont* fontHuge = nullptr;

  Item heldItem() const {
    const auto& s = net.inventory()[static_cast<size_t>(hotbar)];
    return s.empty() ? Item::None : s.item;
  }
};

// main.cpp
void joinServer(App& app, const ServerEntry& entry);
void addServer(App& app, const std::string& name, const std::string& address);
void removeServer(App& app, int index);
void leaveServer(App& app, const std::string& error = "");
void applyVideo(App& app);   // fullscreen / vsync from the settings
void saveSettings(const App& app);

// ui.cpp
void uiSetup(App& app, float contentScale);
void uiMainMenu(App& app);
void uiServerBrowser(App& app);
void uiConnecting(App& app);
void uiInGame(App& app);
void uiSettings(App& app);  // drawn over the menu or the game while settingsOpen

}  // namespace sg
