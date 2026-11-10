// Game client.
//   sg_client                       solo: starts a local server and joins it
//   sg_client --connect 1.2.3.4     join a dedicated server (port 27015)
//   sg_client --connect host:port --name Ramiz
//
// Controls: WASD move, Shift sprint, Space jump, mouse look, V first/third
// person, Esc frees the mouse (click to capture again).
#include <GLFW/glfw3.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

#include "../server/server.h"
#include "gl.h"
#include "net_client.h"
#include "renderer.h"

using namespace sg;

namespace {

struct Options {
  std::string host;  // empty = solo with a local server
  uint16_t port = kDefaultPort;
  std::string name = "Survivor";
};

Options parseArgs(int argc, char** argv) {
  Options o;
  for (int i = 1; i + 1 < argc; i += 2) {
    if (!std::strcmp(argv[i], "--connect")) {
      std::string hp = argv[i + 1];
      auto colon = hp.find(':');
      o.host = hp.substr(0, colon);
      if (colon != std::string::npos) o.port = static_cast<uint16_t>(std::stoi(hp.substr(colon + 1)));
    } else if (!std::strcmp(argv[i], "--name")) {
      o.name = argv[i + 1];
    }
  }
  return o;
}

Vec3 colorForId(uint32_t id) {
  // Muted, readable team-ish colours in linear space.
  const Vec3 palette[] = {{0.55f, 0.12f, 0.08f}, {0.10f, 0.25f, 0.55f}, {0.45f, 0.38f, 0.10f},
                          {0.20f, 0.40f, 0.20f}, {0.40f, 0.15f, 0.40f}, {0.10f, 0.40f, 0.42f}};
  return palette[id % 6];
}

struct InputState {
  float yaw = 0, pitch = 0;
  double lastX = 0, lastY = 0;
  bool haveCursor = false;
  bool captured = true;
  bool thirdPerson = false;
  bool vWasDown = false;
};

constexpr float kMouseSensitivity = 0.0022f;

}  // namespace

int main(int argc, char** argv) {
  Options opt = parseArgs(argc, argv);
  if (!netInit()) return 1;

  // Solo mode: run a real server on a background thread. Same code path as
  // multiplayer, so what works solo works online.
  std::unique_ptr<Server> localServer;
  std::atomic<bool> serverRunning{true};
  std::thread serverThread;
  if (opt.host.empty()) {
    ServerConfig cfg;
    cfg.port = opt.port;
    cfg.maxPlayers = 4;
    localServer = std::make_unique<Server>(cfg);
    if (!localServer->start()) return 1;
    serverThread = std::thread([&] { localServer->run(serverRunning); });
    opt.host = "127.0.0.1";
  }

  if (!glfwInit()) {
    std::fprintf(stderr, "GLFW init failed\n");
    return 1;
  }
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
  glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);  // required on macOS
  glfwWindowHint(GLFW_SAMPLES, 4);
  GLFWwindow* window = glfwCreateWindow(1600, 900, "Survival Game", nullptr, nullptr);
  if (!window) {
    std::fprintf(stderr, "Could not open an OpenGL 3.3 window\n");
    glfwTerminate();
    return 1;
  }
  glfwMakeContextCurrent(window);
  glfwSwapInterval(1);
  if (!loadGL(reinterpret_cast<GLLoadFn>(glfwGetProcAddress))) return 1;

  glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
  if (glfwRawMouseMotionSupported()) glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);

  Renderer renderer;
  if (!renderer.init()) return 1;
  int fbw, fbh;
  glfwGetFramebufferSize(window, &fbw, &fbh);
  renderer.resize(fbw, fbh);

  NetClient net;
  if (!net.connect(opt.host, opt.port, opt.name)) return 1;

  InputState input;
  bool spawnedLook = false;
  double accumulator = 0;
  double last = glfwGetTime();
  double titleTimer = 0;
  int frames = 0;
  PlayerState prevState, currState;

  while (!glfwWindowShouldClose(window)) {
    glfwPollEvents();
    double now = glfwGetTime();
    double frameDt = std::fmin(now - last, 0.25);
    last = now;

    // --- mouse & keys --------------------------------------------------------
    if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS && input.captured) {
      input.captured = false;
      glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
    }
    if (!input.captured && glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS) {
      input.captured = true;
      input.haveCursor = false;
      glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    }
    bool vDown = glfwGetKey(window, GLFW_KEY_V) == GLFW_PRESS;
    if (vDown && !input.vWasDown) input.thirdPerson = !input.thirdPerson;
    input.vWasDown = vDown;

    double mx, my;
    glfwGetCursorPos(window, &mx, &my);
    if (input.captured && input.haveCursor) {
      input.yaw -= static_cast<float>(mx - input.lastX) * kMouseSensitivity;
      input.pitch -= static_cast<float>(my - input.lastY) * kMouseSensitivity;
      input.pitch = clampf(input.pitch, -kMaxPitch, kMaxPitch);
    }
    input.lastX = mx;
    input.lastY = my;
    input.haveCursor = true;

    // --- network + fixed-rate input -------------------------------------------
    net.update(now);
    if (net.status() == NetClient::Status::Connected) {
      if (!spawnedLook) {  // face the way the server spawned us
        input.yaw = net.predicted().yaw;
        prevState = currState = net.predicted();
        spawnedLook = true;
      }
      accumulator += frameDt;
      while (accumulator >= kInputDt) {
        PlayerInput in;
        if (input.captured) {
          in.moveZ = static_cast<float>((glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) -
                                        (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS));
          in.moveX = static_cast<float>((glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) -
                                        (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS));
          if (glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS) in.buttons |= kButtonJump;
          if (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS) in.buttons |= kButtonSprint;
        }
        in.yaw = input.yaw;
        in.pitch = input.pitch;
        prevState = net.predicted();
        net.sendInput(in);
        currState = net.predicted();
        accumulator -= kInputDt;
      }
    }

    // --- render --------------------------------------------------------------
    glfwGetFramebufferSize(window, &fbw, &fbh);
    renderer.resize(fbw, fbh);

    float alpha = static_cast<float>(accumulator / kInputDt);
    Vec3 feet = lerp(prevState.pos, currState.pos, alpha);
    Camera cam;
    cam.yaw = input.yaw;    // mouse look is applied instantly, not at 60 Hz
    cam.pitch = input.pitch;
    cam.eye = feet + Vec3{0, kEyeHeight, 0};
    if (input.thirdPerson) cam.eye = cam.eye - directionFromYawPitch(cam.yaw, cam.pitch) * 4.0f + Vec3{0, 0.5f, 0};

    renderer.beginFrame(cam);
    if (net.status() == NetClient::Status::Connected) {
      renderer.drawWorld(net.terrain(), feet);
      for (const auto& p : net.remotePlayers(now)) renderer.drawPlayer(p.pos, p.yaw, colorForId(p.id));
      if (input.thirdPerson) renderer.drawPlayer(feet, input.yaw, colorForId(net.playerId()));
    }
    glfwSwapBuffers(window);

    // --- window title as a debug readout --------------------------------------
    ++frames;
    titleTimer += frameDt;
    if (titleTimer >= 0.5) {
      char title[256];
      const char* status = net.status() == NetClient::Status::Connected    ? "online"
                           : net.status() == NetClient::Status::Connecting ? "connecting..."
                           : net.status() == NetClient::Status::Rejected   ? "rejected"
                                                                            : "disconnected";
      std::snprintf(title, sizeof(title), "Survival Game | %s | %.0f fps | pos %.0f %.0f %.0f | nearby %zu | corr %.3f m",
                    status, frames / titleTimer, feet.x, feet.y, feet.z, net.remotePlayers(now).size(),
                    net.lastCorrection());
      glfwSetWindowTitle(window, title);
      titleTimer = 0;
      frames = 0;
    }
  }

  net.disconnect();
  renderer.shutdown();
  glfwDestroyWindow(window);
  glfwTerminate();
  if (localServer) {
    serverRunning = false;
    serverThread.join();
    localServer->stop();
  }
  netShutdown();
  return 0;
}
