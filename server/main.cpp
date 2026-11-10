// Dedicated server entry point.
// Usage: sg_server [--port 27015] [--seed 1337] [--max-players 100]
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "server.h"

static std::atomic<bool> g_running{true};

static void onSignal(int) { g_running = false; }

int main(int argc, char** argv) {
  sg::ServerConfig cfg;
  for (int i = 1; i + 1 < argc; i += 2) {
    if (!std::strcmp(argv[i], "--port")) cfg.port = static_cast<uint16_t>(std::atoi(argv[i + 1]));
    else if (!std::strcmp(argv[i], "--seed")) cfg.terrainSeed = static_cast<uint32_t>(std::strtoul(argv[i + 1], nullptr, 10));
    else if (!std::strcmp(argv[i], "--max-players")) cfg.maxPlayers = std::atoi(argv[i + 1]);
    else { std::fprintf(stderr, "unknown option %s\n", argv[i]); return 1; }
  }

  std::signal(SIGINT, onSignal);
  std::signal(SIGTERM, onSignal);

  if (!sg::netInit()) return 1;
  sg::Server server(cfg);
  if (!server.start()) return 1;
  server.run(g_running);
  std::printf("[server] shutting down\n");
  server.stop();
  sg::netShutdown();
  return 0;
}
