// Unit + integration tests. No framework: each CHECK prints on failure.
#include <cmath>
#include <cstdio>
#include <limits>

#include "../server/server.h"
#include "net_client.h"
#include "protocol.h"

using namespace sg;

static int g_failures = 0;
static int g_checks = 0;
#define CHECK(cond)                                                          \
  do {                                                                       \
    ++g_checks;                                                              \
    if (!(cond)) {                                                           \
      ++g_failures;                                                          \
      std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);   \
    }                                                                        \
  } while (0)

static bool approxEq(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

static void testProtocolRoundTrip() {
  SnapshotPacket s;
  s.serverTick = 42;
  s.lastProcessedInput = 7;
  s.self.pos = {1, 2, 3};
  s.self.vel = {4, 5, 6};
  s.self.yaw = 0.5f;
  s.self.onGround = true;
  s.others.push_back({9, {10, 11, 12}, 1.0f, -0.2f});
  auto bytes = encode(s);
  SnapshotPacket d;
  CHECK(decode(bytes.data(), bytes.size(), d));
  CHECK(d.serverTick == 42 && d.lastProcessedInput == 7);
  CHECK(approxEq(d.self.pos.z, 3) && approxEq(d.self.vel.y, 5) && d.self.onGround);
  CHECK(d.others.size() == 1 && d.others[0].id == 9 && approxEq(d.others[0].pos.y, 11));

  HelloPacket h{kProtocolVersion, "Ramiz"};
  auto hb = encode(h);
  HelloPacket hd;
  CHECK(decode(hb.data(), hb.size(), hd) && hd.name == "Ramiz");

  InputPacket ip;
  for (uint32_t i = 1; i <= 6; ++i) ip.inputs.push_back({i, 1, 0, 0, 0, 0});
  auto ib = encode(ip);
  InputPacket id;
  CHECK(decode(ib.data(), ib.size(), id));
  CHECK(id.inputs.size() == static_cast<size_t>(kMaxInputsPerPacket));
  CHECK(id.inputs.back().seq == 6);  // newest kept
}

static void testProtocolRejectsGarbage() {
  auto bytes = encode(SnapshotPacket{});
  SnapshotPacket d;
  CHECK(!decode(bytes.data(), bytes.size() - 1, d));  // truncated
  bytes[0] ^= 0xFF;
  CHECK(!decode(bytes.data(), bytes.size(), d));  // bad magic
  CHECK(!peekType(bytes.data(), bytes.size()));

  InputPacket ip;
  ip.inputs.push_back({1, std::numeric_limits<float>::quiet_NaN(), 0, 0, 0, 0});
  auto ib = encode(ip);
  InputPacket id;
  CHECK(!decode(ib.data(), ib.size(), id));  // NaN refused at the wire
}

static void testSanitize() {
  PlayerInput in{1, 1, 1, 0, 9.0f, 0xFF};
  auto s = sanitizeInput(in);
  CHECK(approxEq(std::sqrt(s.moveX * s.moveX + s.moveZ * s.moveZ), 1.0f));  // no diagonal boost
  CHECK(approxEq(s.pitch, kMaxPitch));
  CHECK(s.buttons == (kButtonJump | kButtonSprint));
}

static void testMovement() {
  Terrain t(1337);
  PlayerState a;
  a.pos = {2048, t.heightAt(2048, 2048), 2048};
  a.onGround = true;
  PlayerState b = a;
  PlayerInput fwd{0, 0, 1, 0, 0, 0};
  for (int i = 0; i < 60; ++i) { applyInput(a, fwd, t); applyInput(b, fwd, t); }
  float moved = std::hypot(a.pos.x - 2048, a.pos.z - 2048);
  CHECK(moved > 4.0f && moved < kWalkSpeed + 0.01f);  // ~5 m in 1 s after accelerating
  CHECK(a.pos.x == b.pos.x && a.pos.y == b.pos.y && a.pos.z == b.pos.z);  // deterministic
  CHECK(a.pos.y >= t.heightAt(a.pos.x, a.pos.z) - 1e-3f);                // not under ground

  PlayerInput jump{0, 0, 0, 0, 0, kButtonJump};
  applyInput(a, jump, t);
  CHECK(!a.onGround && a.vel.y > 0);
}

static void testCameraMath() {
  // A point straight ahead of the camera must land in the middle of the screen.
  Vec3 eye{100, 20, 100};
  float yaw = 0.7f, pitch = -0.3f;
  Vec3 dir = directionFromYawPitch(yaw, pitch);
  Mat4 vp = Mat4::perspective(1.2f, 16.0f / 9.0f, 0.1f, 1000.0f) * Mat4::lookAt(eye, eye + dir, {0, 1, 0});
  Vec3 p = eye + dir * 50.0f;
  float x = vp.at(0, 0) * p.x + vp.at(0, 1) * p.y + vp.at(0, 2) * p.z + vp.at(0, 3);
  float y = vp.at(1, 0) * p.x + vp.at(1, 1) * p.y + vp.at(1, 2) * p.z + vp.at(1, 3);
  float z = vp.at(2, 0) * p.x + vp.at(2, 1) * p.y + vp.at(2, 2) * p.z + vp.at(2, 3);
  float w = vp.at(3, 0) * p.x + vp.at(3, 1) * p.y + vp.at(3, 2) * p.z + vp.at(3, 3);
  CHECK(w > 0);
  CHECK(approxEq(x / w, 0, 1e-3f) && approxEq(y / w, 0, 1e-3f));
  CHECK(z / w > -1 && z / w < 1);
  // Movement "forward" and camera "forward" agree when looking level.
  Vec3 camFwd = directionFromYawPitch(yaw, 0);
  CHECK(approxEq(camFwd.x, -std::sin(yaw)) && approxEq(camFwd.z, -std::cos(yaw)));
}

static void testTerrain() {
  Terrain t(1337), u(1337), v(99);
  CHECK(t.heightAt(1000, 1234) == u.heightAt(1000, 1234));
  CHECK(t.heightAt(1000, 1234) != v.heightAt(1000, 1234));
  CHECK(t.heightAt(5, 5) < kSeaLevel);  // map corner is sea
}

// Full loop over real UDP on localhost: join, walk, check prediction matches.
static void testServerAndClient() {
  ServerConfig cfg;
  cfg.port = 27901;
  Server server(cfg);
  CHECK(server.start());

  NetClient client;
  CHECK(client.connect("127.0.0.1", cfg.port, "tester"));
  double now = 0;
  const double serverDt = 1.0 / cfg.tickRate;
  for (int i = 0; i < 10 && client.status() != NetClient::Status::Connected; ++i) {
    client.update(now);
    server.tick(serverDt);
    now += serverDt;
  }
  client.update(now);
  CHECK(client.status() == NetClient::Status::Connected);
  CHECK(server.playerCount() == 1);

  Vec3 start = client.predicted().pos;
  float maxCorrection = 0;
  for (int t = 0; t < 90; ++t) {        // 3 s
    for (int k = 0; k < 2; ++k)          // 60 Hz input vs 30 Hz tick
      client.sendInput({0, 0.3f, 1, 0.2f, 0, kButtonSprint});
    server.tick(serverDt);
    now += serverDt;
    client.update(now);
    if (t > 5) maxCorrection = std::fmax(maxCorrection, client.lastCorrection());
  }
  float moved = std::hypot(client.predicted().pos.x - start.x, client.predicted().pos.z - start.z);
  std::printf("  client moved %.1f m in 3 s, max prediction error %.4f m\n", moved, maxCorrection);
  CHECK(moved > 15.0f);
  CHECK(maxCorrection < 0.01f);  // prediction agrees with the server

  client.disconnect();
  server.tick(serverDt);
  CHECK(server.playerCount() == 0);
  server.stop();
}

// A cheater sending inputs 4x too fast must not move faster than allowed.
static void testSpeedHackIsCapped() {
  ServerConfig cfg;
  cfg.port = 27902;
  Server server(cfg);
  CHECK(server.start());

  UdpSocket sock;
  CHECK(sock.open(0));
  Address srv;
  Address::parse("127.0.0.1", cfg.port, srv);
  auto hello = encode(HelloPacket{kProtocolVersion, "cheater"});
  sock.send(srv, hello.data(), hello.size());
  server.tick(1.0 / 30);

  uint8_t buf[kMaxPacketSize];
  Address from;
  WelcomePacket w;
  bool welcomed = false;
  int n;
  while ((n = sock.receive(from, buf, sizeof(buf))) > 0)
    if (peekType(buf, n) == PacketType::Welcome) welcomed = decode(buf, n, w);
  CHECK(welcomed);

  uint32_t seq = 1;
  SnapshotPacket last;
  for (int t = 0; t < 60; ++t) {  // 2 s of server time
    for (int k = 0; k < 8; ++k) {  // 240 inputs/s instead of 60
      InputPacket p;
      p.inputs.push_back({seq++, 0, 1, 0, 0, kButtonSprint});
      auto b = encode(p);
      sock.send(srv, b.data(), b.size());
    }
    server.tick(1.0 / 30);
    while ((n = sock.receive(from, buf, sizeof(buf))) > 0)
      if (peekType(buf, n) == PacketType::Snapshot) decode(buf, n, last);
  }
  float moved = std::hypot(last.self.pos.x - w.spawn.pos.x, last.self.pos.z - w.spawn.pos.z);
  std::printf("  cheater moved %.1f m in 2 s (sprint limit ~%.0f m)\n", moved, 2 * kSprintSpeed);
  CHECK(moved < 2 * kSprintSpeed + 1.5f);
  server.stop();
}

int main() {
  if (!netInit()) return 1;
  testProtocolRoundTrip();
  testProtocolRejectsGarbage();
  testSanitize();
  testMovement();
  testTerrain();
  testCameraMath();
  testServerAndClient();
  testSpeedHackIsCapped();
  netShutdown();
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
