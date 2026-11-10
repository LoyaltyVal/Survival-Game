// Headless bots for load testing a running server.
// Usage: sg_bot [host] [port] [bots] [seconds]
// Example: sg_bot 127.0.0.1 27015 50 30
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "net_client.h"

using namespace sg;

int main(int argc, char** argv) {
  std::string host = argc > 1 ? argv[1] : "127.0.0.1";
  uint16_t port = argc > 2 ? static_cast<uint16_t>(std::atoi(argv[2])) : kDefaultPort;
  int count = argc > 3 ? std::atoi(argv[3]) : 10;
  double seconds = argc > 4 ? std::atof(argv[4]) : 20.0;

  if (!netInit()) return 1;
  std::vector<std::unique_ptr<NetClient>> bots;
  std::vector<float> yaw(count), turn(count);
  std::mt19937 rng(7);
  std::uniform_real_distribution<float> unit(-1.0f, 1.0f);
  for (int i = 0; i < count; ++i) {
    auto b = std::make_unique<NetClient>();
    if (!b->connect(host, port, "bot" + std::to_string(i))) return 1;
    bots.push_back(std::move(b));
    yaw[i] = unit(rng) * kPi;
  }

  using clock = std::chrono::steady_clock;
  const auto t0 = clock::now();
  auto next = t0;
  const auto step = std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(kInputDt));
  float worstCorrection = 0;
  long frames = 0;
  while (true) {
    double now = std::chrono::duration<double>(clock::now() - t0).count();
    if (now > seconds) break;
    for (int i = 0; i < count; ++i) {
      auto& b = *bots[i];
      b.update(now);
      if (b.status() != NetClient::Status::Connected) continue;
      if (frames % 60 == 0) turn[i] = unit(rng) * 0.03f;  // wander
      yaw[i] += turn[i];
      uint8_t buttons = (frames + i) % 240 == 0 ? kButtonJump : 0;
      b.sendInput({0, 0, 1, yaw[i], 0, buttons});
      if (now > 2.0) worstCorrection = std::fmax(worstCorrection, b.lastCorrection());
    }
    ++frames;
    next += step;
    std::this_thread::sleep_until(next);
  }

  int connected = 0;
  size_t seen = 0;
  double now = std::chrono::duration<double>(clock::now() - t0).count();
  for (auto& b : bots) {
    if (b->status() == NetClient::Status::Connected) ++connected;
    seen += b->remotePlayers(now).size();
  }
  std::printf("[bot] %d/%d connected, avg %.1f other players visible, worst correction %.3f m\n", connected,
              count, count ? static_cast<double>(seen) / count : 0.0, worstCorrection);
  for (auto& b : bots) b->disconnect();
  netShutdown();
  return connected == count ? 0 : 1;
}
