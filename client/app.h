// Client application state shared by the main loop (main.cpp) and the UI (ui.cpp).
#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "net.h"
#include "net_client.h"
#include "renderer.h"
#include "server.h"
#include "upnp.h"
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

  // settings (saved to settings.txt)
  char playerName[21] = "Survivor";
  float mouseSensitivity = 1.0f;

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

// ui.cpp
void uiSetup(App& app, float contentScale);
void uiMainMenu(App& app);
void uiServerBrowser(App& app);
void uiConnecting(App& app);
void uiInGame(App& app);

}  // namespace sg
