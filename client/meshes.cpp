#include "meshes.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>

namespace sg {

// --- helpers -------------------------------------------------------------------------
static Vec3 xf(const Mat4& m, const Vec3& p) {
  return {m.at(0, 0) * p.x + m.at(0, 1) * p.y + m.at(0, 2) * p.z + m.at(0, 3),
          m.at(1, 0) * p.x + m.at(1, 1) * p.y + m.at(1, 2) * p.z + m.at(1, 3),
          m.at(2, 0) * p.x + m.at(2, 1) * p.y + m.at(2, 2) * p.z + m.at(2, 3)};
}

static uint32_t hashU(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352d;
  x ^= x >> 15;
  x *= 0x846ca68b;
  x ^= x >> 16;
  return x;
}
static float hashF(uint32_t x) { return static_cast<float>(hashU(x) & 0xFFFFFF) / static_cast<float>(0xFFFFFF); }
static float hash3(int x, int y, int z) {
  return hashF(static_cast<uint32_t>(x) * 73856093u ^ static_cast<uint32_t>(y) * 19349663u ^ static_cast<uint32_t>(z) * 83492791u);
}

float noise3(float x, float y, float z) {
  float fx = std::floor(x), fy = std::floor(y), fz = std::floor(z);
  int ix = static_cast<int>(fx), iy = static_cast<int>(fy), iz = static_cast<int>(fz);
  float tx = x - fx, ty = y - fy, tz = z - fz;
  tx = tx * tx * (3 - 2 * tx);
  ty = ty * ty * (3 - 2 * ty);
  tz = tz * tz * (3 - 2 * tz);
  auto L = [](float a, float b, float t) { return a + (b - a) * t; };
  float c00 = L(hash3(ix, iy, iz), hash3(ix + 1, iy, iz), tx);
  float c10 = L(hash3(ix, iy + 1, iz), hash3(ix + 1, iy + 1, iz), tx);
  float c01 = L(hash3(ix, iy, iz + 1), hash3(ix + 1, iy, iz + 1), tx);
  float c11 = L(hash3(ix, iy + 1, iz + 1), hash3(ix + 1, iy + 1, iz + 1), tx);
  return L(L(c00, c10, ty), L(c01, c11, ty), tz);
}

// Maps the local Y axis (0..1) onto the segment a->b with the given radius (for cylinders).
static Mat4 alongY(const Vec3& a, const Vec3& b, float radius) {
  Vec3 d = b - a;
  float len = length(d);
  Vec3 y = len > 1e-6f ? d / len : Vec3{0, 1, 0};
  Vec3 ref = std::fabs(y.y) > 0.9f ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
  Vec3 x = normalize(cross(ref, y));
  Vec3 z = cross(x, y);
  Mat4 m;
  m.at(0, 0) = x.x * radius; m.at(1, 0) = x.y * radius; m.at(2, 0) = x.z * radius;
  m.at(0, 1) = y.x * len;    m.at(1, 1) = y.y * len;    m.at(2, 1) = y.z * len;
  m.at(0, 2) = z.x * radius; m.at(1, 2) = z.y * radius; m.at(2, 2) = z.z * radius;
  m.at(0, 3) = a.x; m.at(1, 3) = a.y; m.at(2, 3) = a.z;
  return m;
}

// --- MeshBuilder ------------------------------------------------------------------------
void MeshBuilder::tri(const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& color, int mat, float sa, float sb,
                      float sc) {
  Vec3 n = cross(b - a, c - a);
  float len = length(n);
  if (len < 1e-10f) return;  // degenerate (cone tips)
  n = n / len;
  const Vec3 p[3] = {a, b, c}, ns[3] = {n, n, n};
  const float s[3] = {sa, sb, sc};
  const float uv[3][2] = {{0, 0}, {1, 0}, {1, 1}};
  triN(p, ns, s, uv, color, mat);
}

void MeshBuilder::triN(const Vec3 p[3], const Vec3 n[3], const float s[3], const float uv[3][2], const Vec3& color,
                       int mat) {
  auto base = static_cast<uint32_t>(v.size());
  for (int i = 0; i < 3; ++i) {
    Vertex x{p[i].x, p[i].y, p[i].z, n[i].x, n[i].y, n[i].z, color.x, color.y, color.z, static_cast<float>(mat), s[i]};
    x.u = uv[i][0];
    x.v = uv[i][1];
    v.push_back(x);
    bmin = {std::min(bmin.x, p[i].x), std::min(bmin.y, p[i].y), std::min(bmin.z, p[i].z)};
    bmax = {std::max(bmax.x, p[i].x), std::max(bmax.y, p[i].y), std::max(bmax.z, p[i].z)};
  }
  idx.insert(idx.end(), {base, base + 1, base + 2});
}

void MeshBuilder::card(const Vec3 c[4], const Vec3& lightCenter, const float sway[4], const Vec3& color, int mat) {
  Vec3 n[4];
  for (int i = 0; i < 4; ++i) n[i] = normalize(normalize(c[i] - lightCenter) + Vec3{0, 0.35f, 0});
  static const float uvq[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
  auto emit = [&](int i0, int i1, int i2) {
    const Vec3 p[3] = {c[i0], c[i1], c[i2]}, ns[3] = {n[i0], n[i1], n[i2]};
    const float s[3] = {sway[i0], sway[i1], sway[i2]};
    const float uv[3][2] = {{uvq[i0][0], uvq[i0][1]}, {uvq[i1][0], uvq[i1][1]}, {uvq[i2][0], uvq[i2][1]}};
    triN(p, ns, s, uv, color, mat);
  };
  emit(0, 1, 2);  // front
  emit(0, 2, 3);
  emit(0, 2, 1);  // back
  emit(0, 3, 2);
}

void MeshBuilder::smooth(size_t first, float maxAngleDeg) {
  const float cosT = std::cos(maxAngleDeg * kPi / 180.0f);
  std::map<std::tuple<long long, long long, long long>, std::vector<size_t>> groups;
  for (size_t i = first; i < v.size(); ++i)
    groups[{std::llround(v[i].px * 2000.0), std::llround(v[i].py * 2000.0), std::llround(v[i].pz * 2000.0)}].push_back(i);
  std::vector<Vec3> orig(v.size() - first);
  for (size_t i = first; i < v.size(); ++i) orig[i - first] = {v[i].nx, v[i].ny, v[i].nz};
  for (const auto& [key, list] : groups) {
    if (list.size() < 2) continue;
    for (size_t i : list) {
      Vec3 ni = orig[i - first], sum{0, 0, 0};
      for (size_t j : list) {
        Vec3 nj = orig[j - first];
        if (dot(ni, nj) >= cosT) sum += nj;
      }
      Vec3 n = normalize(sum);
      v[i].nx = n.x;
      v[i].ny = n.y;
      v[i].nz = n.z;
    }
  }
}

void MeshBuilder::quad(const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& d, const Vec3& color, int mat) {
  tri(a, b, c, color, mat);
  tri(a, c, d, color, mat);
}

void MeshBuilder::box(const Mat4& m, const Vec3& color, int mat, float sway) {
  auto P = [&](int i, int j, int k) {
    return xf(m, {static_cast<float>(i) - 0.5f, static_cast<float>(j) - 0.5f, static_cast<float>(k) - 0.5f});
  };
  // Each face listed counter-clockwise from outside.
  static const int faces[6][4][3] = {
      {{1, 0, 0}, {1, 1, 0}, {1, 1, 1}, {1, 0, 1}}, {{0, 0, 0}, {0, 0, 1}, {0, 1, 1}, {0, 1, 0}},
      {{0, 1, 0}, {0, 1, 1}, {1, 1, 1}, {1, 1, 0}}, {{0, 0, 0}, {1, 0, 0}, {1, 0, 1}, {0, 0, 1}},
      {{0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}}, {{0, 0, 0}, {0, 1, 0}, {1, 1, 0}, {1, 0, 0}}};
  for (const auto& f : faces) {
    Vec3 q[4];
    float s[4];
    for (int i = 0; i < 4; ++i) {
      q[i] = P(f[i][0], f[i][1], f[i][2]);
      s[i] = sway * static_cast<float>(f[i][1]);
    }
    tri(q[0], q[1], q[2], color, mat, s[0], s[1], s[2]);
    tri(q[0], q[2], q[3], color, mat, s[0], s[2], s[3]);
  }
}

void MeshBuilder::box(const Vec3& center, const Vec3& half, const Vec3& color, int mat, float sway) {
  box(Mat4::translate(center) * Mat4::scale(half * 2.0f), color, mat, sway);
}

void MeshBuilder::cylinder(const Mat4& m, int segs, float r0, float r1, const Vec3& color, int mat, float swayTop,
                           bool caps) {
  const size_t first = v.size();
  const Vec3 cb = xf(m, {0, 0, 0}), ct = xf(m, {0, 1, 0});
  for (int i = 0; i < segs; ++i) {
    float a0 = 2 * kPi * static_cast<float>(i) / static_cast<float>(segs);
    float a1 = 2 * kPi * static_cast<float>(i + 1) / static_cast<float>(segs);
    Vec3 b0 = xf(m, {r0 * std::cos(a0), 0, r0 * std::sin(a0)}), b1 = xf(m, {r0 * std::cos(a1), 0, r0 * std::sin(a1)});
    Vec3 t0 = xf(m, {r1 * std::cos(a0), 1, r1 * std::sin(a0)}), t1 = xf(m, {r1 * std::cos(a1), 1, r1 * std::sin(a1)});
    tri(b0, t0, t1, color, mat, 0, swayTop, swayTop);
    tri(b0, t1, b1, color, mat, 0, swayTop, 0);
    if (caps) {
      tri(cb, b0, b1, color, mat);
      if (r1 > 0) tri(ct, t1, t0, color, mat, swayTop, swayTop, swayTop);
    }
  }
  smooth(first, 50.0f);  // round sides, crisp caps
}

void MeshBuilder::blob(const Mat4& m, uint32_t seed, float rough, const Vec3& color, int mat, int subdiv, float sway,
                       float colorJitter, float smoothAngle, int octaves) {
  const size_t first = v.size();
  const float t = (1.0f + std::sqrt(5.0f)) / 2.0f;
  const Vec3 V[12] = {{-1, t, 0}, {1, t, 0}, {-1, -t, 0}, {1, -t, 0}, {0, -1, t}, {0, 1, t},
                      {0, -1, -t}, {0, 1, -t}, {t, 0, -1}, {t, 0, 1}, {-t, 0, -1}, {-t, 0, 1}};
  static const int F[20][3] = {{0, 11, 5}, {0, 5, 1},  {0, 1, 7},   {0, 7, 10}, {0, 10, 11}, {1, 5, 9}, {5, 11, 4},
                               {11, 10, 2}, {10, 7, 6}, {7, 1, 8},  {3, 9, 4},  {3, 4, 2},   {3, 2, 6}, {3, 6, 8},
                               {3, 8, 9},  {4, 9, 5},  {2, 4, 11}, {6, 2, 10}, {8, 6, 7},   {9, 8, 1}};
  const float so = static_cast<float>(seed % 997) * 1.37f;
  auto displace = [&](Vec3 d) {
    d = normalize(d);
    float n = noise3(d.x * 1.8f + so, d.y * 1.8f + so * 0.7f, d.z * 1.8f - so);
    if (octaves > 1) n = n * 0.7f + noise3(d.x * 4.3f - so, d.y * 4.3f, d.z * 4.3f + so) * 0.3f;
    float r = 1.0f + rough * (n - 0.5f) * 2.0f;
    return d * r;
  };
  const Vec3 center = xf(m, {0, 0, 0});
  uint32_t faceNo = 0;
  auto emit = [&](Vec3 a, Vec3 b, Vec3 c) {
    Vec3 pa = displace(a), pb = displace(b), pc = displace(c);
    float sa = sway * (pa.y + 1) * 0.5f, sb = sway * (pb.y + 1) * 0.5f, sc = sway * (pc.y + 1) * 0.5f;
    Vec3 wa = xf(m, pa), wb = xf(m, pb), wc = xf(m, pc);
    Vec3 n = cross(wb - wa, wc - wa);
    if (dot(n, (wa + wb + wc) * (1.0f / 3.0f) - center) < 0) {  // keep faces pointing outward
      std::swap(wb, wc);
      std::swap(sb, sc);
    }
    float j = 1.0f + colorJitter * (hashF(seed * 131u + faceNo++) * 2.0f - 1.0f);
    tri(wa, wb, wc, color * j, mat, sa, sb, sc);
  };
  struct Rec {
    static void go(const Vec3& a, const Vec3& b, const Vec3& c, int level, decltype(emit)& e) {
      if (level == 0) {
        e(a, b, c);
        return;
      }
      Vec3 ab = normalize(a + b), bc = normalize(b + c), ca = normalize(c + a);
      go(a, ab, ca, level - 1, e);
      go(ab, b, bc, level - 1, e);
      go(ca, bc, c, level - 1, e);
      go(ab, bc, ca, level - 1, e);
    }
  };
  for (const auto& f : F) Rec::go(normalize(V[f[0]]), normalize(V[f[1]]), normalize(V[f[2]]), subdiv, emit);
  if (smoothAngle > 0) smooth(first, smoothAngle);
}

// --- palette (linear light) ---------------------------------------------------------------
namespace {
const Vec3 kHandle{0.26f, 0.15f, 0.07f};
const Vec3 kStoneCol{0.30f, 0.29f, 0.27f};
const Vec3 kGunMetal{0.045f, 0.047f, 0.055f};
const Vec3 kSteel{0.30f, 0.31f, 0.33f};
const Vec3 kPolymer{0.035f, 0.035f, 0.04f};
const Vec3 kLash{0.42f, 0.34f, 0.20f};
const Vec3 kBrass{0.62f, 0.42f, 0.12f};
const Vec3 kCopper{0.55f, 0.24f, 0.10f};
const Vec3 kRifleWood{0.22f, 0.10f, 0.04f};
const Vec3 kBark{0.13f, 0.085f, 0.05f};
constexpr float kHalfPi = kPi * 0.5f;
}  // namespace

static void handle(MeshBuilder& mb, float length = 0.5f) {
  mb.cylinder(Mat4::translate({0, -0.16f, 0}) * Mat4::scale({0.019f, length, 0.019f}), 10, 1, 0.9f, kHandle, kMatWood);
}

void buildItemModel(Item item, MeshBuilder& mb) {
  using M = Mat4;
  switch (item) {
    case Item::Rock:
      mb.blob(M::translate({0, 0.02f, -0.04f}) * M::scale({0.085f, 0.07f, 0.08f}), 3, 0.3f, kStoneCol, kMatRock, 2, 0, 0.05f, 40, 2);
      break;
    case Item::StoneHatchet:
      handle(mb);
      mb.blob(M::translate({0, 0.3f, -0.07f}) * M::scale({0.028f, 0.075f, 0.09f}), 5, 0.2f, kStoneCol, kMatRock, 2, 0, 0.05f, 40);
      mb.box(M::translate({0, 0.3f, -0.155f}) * M::scale({0.014f, 0.13f, 0.035f}), kStoneCol * 1.2f, kMatRock);
      mb.cylinder(M::translate({0, 0.25f, 0}) * M::scale({0.026f, 0.09f, 0.026f}), 10, 1, 1, kLash, kMatCloth);
      break;
    case Item::StonePickaxe:
      handle(mb);
      mb.box(M::translate({0, 0.32f, -0.01f}) * M::scale({0.04f, 0.05f, 0.12f}), kStoneCol, kMatRock);
      mb.box(M::translate({0, 0.30f, -0.14f}) * M::rotateX(0.35f) * M::scale({0.032f, 0.04f, 0.16f}), kStoneCol * 0.9f, kMatRock);
      mb.box(M::translate({0, 0.30f, 0.12f}) * M::rotateX(-0.35f) * M::scale({0.032f, 0.04f, 0.14f}), kStoneCol * 0.9f, kMatRock);
      mb.cylinder(M::translate({0, 0.27f, 0}) * M::scale({0.026f, 0.08f, 0.026f}), 10, 1, 1, kLash, kMatCloth);
      break;
    case Item::Hammer:
      handle(mb, 0.45f);
      mb.box(M::translate({0, 0.30f, -0.02f}) * M::scale({0.065f, 0.075f, 0.16f}), Vec3{0.20f, 0.11f, 0.05f}, kMatWood);
      mb.box(M::translate({0, 0.30f, -0.105f}) * M::scale({0.07f, 0.08f, 0.012f}), kSteel, kMatMetal);
      break;
    case Item::BuildingPlan: {
      M base = M::translate({0, 0, -0.08f}) * M::rotateX(-0.6f);
      mb.box(base * M::scale({0.22f, 0.006f, 0.28f}), Vec3{0.10f, 0.25f, 0.55f}, kMatPlain);
      for (int i = -2; i <= 2; ++i) {
        mb.box(base * M::translate({i * 0.04f, 0.004f, 0}) * M::scale({0.004f, 0.002f, 0.26f}), Vec3{0.7f, 0.8f, 0.9f}, kMatPlain);
        mb.box(base * M::translate({0, 0.004f, i * 0.05f}) * M::scale({0.2f, 0.002f, 0.004f}), Vec3{0.7f, 0.8f, 0.9f}, kMatPlain);
      }
      break;
    }
    case Item::Pistol: {
      mb.box(M::translate({0, -0.055f, 0.01f}) * M::rotateX(-0.28f) * M::scale({0.03f, 0.11f, 0.045f}), kPolymer, kMatPlain);
      mb.box(M::translate({0, 0.0f, -0.06f}) * M::scale({0.028f, 0.028f, 0.16f}), kPolymer, kMatPlain);
      mb.box(M::translate({0, 0.034f, -0.068f}) * M::scale({0.032f, 0.036f, 0.19f}), kGunMetal, kMatMetal);
      mb.box(M::translate({0, 0.034f, -0.04f}) * M::scale({0.034f, 0.026f, 0.004f}), kSteel, kMatMetal);  // slide serrations
      mb.cylinder(M::translate({0, 0.032f, -0.162f}) * M::rotateX(-kHalfPi) * M::scale({0.008f, 0.012f, 0.008f}), 12, 1, 1, kSteel, kMatMetal);
      mb.box(M::translate({0, -0.03f, -0.045f}) * M::scale({0.008f, 0.006f, 0.06f}), kPolymer, kMatPlain);  // trigger guard
      mb.box(M::translate({0, -0.02f, -0.073f}) * M::scale({0.008f, 0.025f, 0.006f}), kPolymer, kMatPlain);
      mb.box(M::translate({0, -0.012f, -0.035f}) * M::scale({0.005f, 0.02f, 0.006f}), kSteel, kMatMetal);  // trigger
      mb.box(M::translate({0, 0.056f, -0.155f}) * M::scale({0.006f, 0.008f, 0.008f}), kSteel, kMatMetal);  // sights
      mb.box(M::translate({0, 0.056f, 0.02f}) * M::scale({0.02f, 0.008f, 0.008f}), kSteel, kMatMetal);
      break;
    }
    case Item::Rifle: {
      mb.box(M::translate({0, -0.05f, 0.02f}) * M::rotateX(-0.3f) * M::scale({0.03f, 0.09f, 0.045f}), kRifleWood, kMatWood);
      mb.box(M::translate({0, -0.02f, 0.22f}) * M::rotateX(0.08f) * M::scale({0.04f, 0.085f, 0.28f}), kRifleWood, kMatWood);
      mb.box(M::translate({0, -0.025f, 0.365f}) * M::scale({0.043f, 0.1f, 0.012f}), kPolymer, kMatPlain);  // butt pad
      mb.box(M::translate({0, 0.02f, -0.08f}) * M::scale({0.046f, 0.055f, 0.24f}), kGunMetal, kMatMetal);
      mb.box(M::translate({0, 0.0f, -0.36f}) * M::scale({0.042f, 0.045f, 0.32f}), kRifleWood, kMatWood);
      mb.cylinder(M::translate({0, 0.03f, -0.2f}) * M::rotateX(-kHalfPi) * M::scale({0.011f, 0.62f, 0.011f}), 12, 1, 0.85f, kGunMetal, kMatMetal);
      mb.box(M::translate({0, 0.05f, -0.78f}) * M::scale({0.006f, 0.022f, 0.008f}), kSteel, kMatMetal);  // front post
      mb.box(M::translate({0, 0.052f, -0.02f}) * M::scale({0.026f, 0.01f, 0.01f}), kSteel, kMatMetal);   // rear notch
      mb.box(M::translate({0.035f, 0.035f, -0.02f}) * M::rotateZ(-0.6f) * M::scale({0.04f, 0.01f, 0.01f}), kSteel, kMatMetal);  // bolt
      mb.blob(M::translate({0.058f, 0.025f, -0.02f}) * M::scale({0.009f, 0.009f, 0.009f}), 1, 0, kSteel, kMatMetal, 1, 0, 0);
      mb.box(M::translate({0, -0.012f, -0.035f}) * M::scale({0.005f, 0.02f, 0.006f}), kSteel, kMatMetal);
      break;
    }
    case Item::PistolAmmo:
    case Item::RifleAmmo: {
      float h = item == Item::RifleAmmo ? 0.075f : 0.04f;
      float r = item == Item::RifleAmmo ? 0.011f : 0.012f;
      for (int k = 0; k < 3; ++k) {
        Vec3 p{(k - 1) * 0.032f, 0, (k % 2) * 0.02f};
        mb.cylinder(M::translate(p) * M::scale({r, h, r}), 12, 1, 1, kBrass, kMatMetal);
        mb.cylinder(M::translate(p + Vec3{0, h, 0}) * M::scale({r * 0.95f, h * 0.45f, r * 0.95f}), 12, 1, 0.25f, kCopper, kMatMetal);
      }
      break;
    }
    case Item::Wood:
      mb.cylinder(M::translate({-0.16f, 0.06f, -0.04f}) * M::rotateZ(-kHalfPi) * M::scale({0.06f, 0.32f, 0.06f}), 10, 1, 1, kBark, kMatBark);
      mb.cylinder(M::translate({-0.15f, 0.06f, 0.08f}) * M::rotateZ(-kHalfPi) * M::scale({0.055f, 0.3f, 0.055f}), 10, 1, 1, kBark * 1.2f, kMatBark);
      mb.cylinder(M::translate({-0.14f, 0.16f, 0.02f}) * M::rotateZ(-kHalfPi) * M::scale({0.055f, 0.28f, 0.055f}), 10, 1, 1, kBark, kMatBark);
      break;
    case Item::Stone:
      mb.blob(M::scale({0.12f, 0.09f, 0.11f}), 11, 0.35f, kStoneCol, kMatRock, 2, 0, 0.05f, 40, 2);
      break;
    case Item::MetalOre:
      mb.blob(M::scale({0.12f, 0.09f, 0.11f}), 12, 0.35f, Vec3{0.22f, 0.20f, 0.19f}, kMatRock, 2, 0, 0.05f, 40, 2);
      for (int k = 0; k < 4; ++k)
        mb.blob(M::translate({std::cos(k * 1.7f) * 0.09f, 0.04f + 0.02f * k, std::sin(k * 1.7f) * 0.08f}) * M::scale({0.035f, 0.03f, 0.035f}),
                40u + static_cast<uint32_t>(k), 0.3f, Vec3{0.45f, 0.17f, 0.05f}, kMatMetal, 1, 0, 0.05f, 35);
      break;
    case Item::SulfurOre:
      mb.blob(M::scale({0.12f, 0.09f, 0.11f}), 13, 0.35f, Vec3{0.28f, 0.26f, 0.20f}, kMatRock, 2, 0, 0.05f, 40, 2);
      for (int k = 0; k < 4; ++k)
        mb.blob(M::translate({std::cos(k * 1.9f) * 0.09f, 0.04f + 0.02f * k, std::sin(k * 1.9f) * 0.08f}) * M::scale({0.04f, 0.03f, 0.04f}),
                50u + static_cast<uint32_t>(k), 0.3f, Vec3{0.75f, 0.60f, 0.05f}, kMatRock, 1, 0, 0.05f, 35);
      break;
    case Item::Cloth:
      for (int k = 0; k < 3; ++k)
        mb.box(M::translate({0, k * 0.032f, 0}) * M::rotateY(k * 0.25f) * M::scale({0.2f, 0.03f, 0.15f}),
               Vec3{0.62f, 0.55f, 0.40f} * (1.0f - k * 0.08f), kMatCloth);
      break;
    case Item::MetalFrags:
      for (int k = 0; k < 6; ++k)
        mb.box(M::translate({std::cos(k * 2.3f) * 0.06f, 0.015f + (k % 3) * 0.02f, std::sin(k * 2.3f) * 0.05f}) *
                   M::rotateY(k * 0.9f) * M::rotateZ(k * 0.4f) * M::scale({0.07f, 0.02f, 0.04f}),
               kSteel * (0.8f + 0.1f * (k % 3)), kMatMetal);
      break;
    case Item::Gunpowder:
      mb.blob(M::translate({0, 0.07f, 0}) * M::scale({0.09f, 0.08f, 0.09f}), 21, 0.15f, Vec3{0.10f, 0.10f, 0.11f}, kMatCloth, 2);
      mb.cylinder(M::translate({0, 0.13f, 0}) * M::scale({0.035f, 0.05f, 0.035f}), 10, 1, 1.3f, Vec3{0.12f, 0.12f, 0.13f}, kMatCloth);
      mb.cylinder(M::translate({0, 0.135f, 0}) * M::scale({0.038f, 0.012f, 0.038f}), 10, 1, 1, kLash, kMatCloth);
      break;
    case Item::Berries:
      for (int k = 0; k < 7; ++k) {
        float a = k * 0.9f;
        Vec3 p{std::cos(a) * 0.035f * (k % 3), 0.03f + (k % 2) * 0.03f, std::sin(a) * 0.035f * (k % 3)};
        mb.blob(M::translate(p) * M::scale({0.028f, 0.028f, 0.028f}), 60u + static_cast<uint32_t>(k), 0.08f,
                k % 2 ? Vec3{0.45f, 0.02f, 0.08f} : Vec3{0.30f, 0.02f, 0.12f}, kMatPlain, 2, 0, 0.02f, 80);
      }
      mb.box(M::translate({0, 0.09f, 0}) * M::scale({0.006f, 0.03f, 0.006f}), Vec3{0.1f, 0.2f, 0.05f}, kMatPlain);
      break;
    case Item::Mushroom:
      mb.cylinder(M::scale({0.025f, 0.1f, 0.025f}), 10, 1, 0.8f, Vec3{0.75f, 0.70f, 0.60f}, kMatPlain);
      mb.cylinder(M::translate({0, 0.09f, 0}) * M::scale({0.08f, 0.06f, 0.08f}), 14, 1, 0.25f, Vec3{0.42f, 0.18f, 0.07f}, kMatPlain);
      break;
    case Item::Bandage:
      mb.cylinder(M::translate({-0.07f, 0.05f, 0}) * M::rotateZ(-kHalfPi) * M::scale({0.05f, 0.14f, 0.05f}), 14, 1, 1,
                  Vec3{0.85f, 0.85f, 0.80f}, kMatCloth);
      mb.box(M::translate({0.02f, 0.003f, 0.05f}) * M::scale({0.12f, 0.004f, 0.05f}), Vec3{0.85f, 0.85f, 0.80f}, kMatCloth);
      mb.box(M::translate({0.0f, 0.103f, 0}) * M::scale({0.02f, 0.004f, 0.02f}), Vec3{0.6f, 0.05f, 0.05f}, kMatPlain);
      break;
    case Item::WoodenDoor:
      mb.box(M::translate({0, 0.5f, 0}) * M::scale({0.5f, 1.0f, 0.05f}), Vec3{0.20f, 0.10f, 0.04f}, kMatWood);
      mb.box(M::beam({-0.22f, 0.15f, 0.03f}, {0.22f, 0.85f, 0.03f}, 0.05f, 0.015f), Vec3{0.15f, 0.08f, 0.03f}, kMatWood);
      mb.box(M::translate({0.18f, 0.5f, 0.04f}) * M::scale({0.03f, 0.06f, 0.03f}), kSteel, kMatMetal);
      break;
    case Item::StorageBox:
      mb.box(M::translate({0, 0.15f, 0}) * M::scale({0.45f, 0.3f, 0.3f}), Vec3{0.28f, 0.16f, 0.07f}, kMatWood);
      mb.box(M::translate({0, 0.29f, 0}) * M::scale({0.47f, 0.04f, 0.32f}), Vec3{0.22f, 0.12f, 0.05f}, kMatWood);
      for (float x : {-0.15f, 0.15f}) mb.box(M::translate({x, 0.15f, 0}) * M::scale({0.03f, 0.31f, 0.31f}), kSteel * 0.6f, kMatMetal);
      break;
    case Item::Workbench:
      mb.box(M::translate({0, 0.45f, 0}) * M::scale({0.8f, 0.06f, 0.4f}), Vec3{0.30f, 0.17f, 0.07f}, kMatWood);
      for (float x : {-0.35f, 0.35f})
        for (float z : {-0.15f, 0.15f}) mb.box(M::translate({x, 0.21f, z}) * M::scale({0.06f, 0.42f, 0.06f}), Vec3{0.24f, 0.13f, 0.05f}, kMatWood);
      mb.box(M::translate({0.25f, 0.52f, 0.1f}) * M::scale({0.12f, 0.08f, 0.08f}), kSteel, kMatMetal);
      break;
    case Item::SleepingBag:
      mb.cylinder(M::translate({-0.2f, 0.09f, 0}) * M::rotateZ(-kHalfPi) * M::scale({0.09f, 0.4f, 0.09f}), 14, 1, 1,
                  Vec3{0.45f, 0.06f, 0.04f}, kMatCloth);
      for (float x : {-0.1f, 0.1f})
        mb.cylinder(M::translate({x - 0.01f, 0.09f, 0}) * M::rotateZ(-kHalfPi) * M::scale({0.095f, 0.02f, 0.095f}), 14, 1, 1, kLash * 0.5f, kMatCloth);
      break;
    case Item::Revolver:
      mb.box(M::translate({0, -0.055f, 0.012f}) * M::rotateX(-0.35f) * M::scale({0.03f, 0.1f, 0.045f}), kRifleWood, kMatWood);
      mb.box(M::translate({0, 0.012f, -0.035f}) * M::scale({0.03f, 0.04f, 0.09f}), kSteel * 0.6f, kMatMetal);
      mb.cylinder(M::translate({0, 0.03f, -0.035f}) * M::rotateX(-kHalfPi) * M::scale({0.023f, 0.05f, 0.023f}), 12, 1, 1, kSteel * 0.7f, kMatMetal);
      mb.cylinder(M::translate({0, 0.035f, -0.075f}) * M::rotateX(-kHalfPi) * M::scale({0.009f, 0.14f, 0.009f}), 10, 1, 1, kSteel * 0.6f, kMatMetal);
      mb.box(M::translate({0, 0.035f, -0.14f}) * M::scale({0.012f, 0.014f, 0.1f}), kSteel * 0.6f, kMatMetal);  // ejector rod housing
      mb.box(M::translate({0, 0.05f, 0.0f}) * M::rotateX(0.5f) * M::scale({0.008f, 0.025f, 0.012f}), kSteel, kMatMetal);  // hammer
      mb.box(M::translate({0, 0.05f, -0.2f}) * M::scale({0.005f, 0.012f, 0.008f}), kSteel, kMatMetal);
      mb.box(M::translate({0, -0.012f, -0.03f}) * M::scale({0.005f, 0.02f, 0.006f}), kSteel, kMatMetal);
      break;
    case Item::SMG:
      mb.box(M::translate({0, -0.055f, 0.03f}) * M::rotateX(-0.25f) * M::scale({0.03f, 0.1f, 0.045f}), kPolymer, kMatPlain);
      mb.box(M::translate({0, 0.03f, -0.1f}) * M::scale({0.046f, 0.07f, 0.32f}), kGunMetal, kMatMetal);
      mb.cylinder(M::translate({0, 0.035f, -0.26f}) * M::rotateX(-kHalfPi) * M::scale({0.02f, 0.1f, 0.02f}), 12, 1, 1, kPolymer, kMatPlain);
      mb.cylinder(M::translate({0, 0.035f, -0.3f}) * M::rotateX(-kHalfPi) * M::scale({0.009f, 0.06f, 0.009f}), 10, 1, 1, kGunMetal, kMatMetal);
      mb.box(M::translate({0, -0.07f, -0.08f}) * M::rotateX(0.1f) * M::scale({0.028f, 0.15f, 0.04f}), kGunMetal, kMatMetal);  // magazine
      for (float x : {-0.02f, 0.02f})
        mb.box(M::translate({x, 0.02f, 0.18f}) * M::scale({0.006f, 0.012f, 0.2f}), kSteel * 0.6f, kMatMetal);  // wire stock
      mb.box(M::translate({0, -0.005f, 0.28f}) * M::scale({0.05f, 0.06f, 0.012f}), kPolymer, kMatPlain);
      mb.box(M::translate({0, 0.07f, -0.24f}) * M::scale({0.006f, 0.014f, 0.008f}), kSteel, kMatMetal);
      break;
    case Item::Shotgun:
      mb.box(M::translate({0, -0.045f, 0.03f}) * M::rotateX(-0.3f) * M::scale({0.03f, 0.08f, 0.045f}), kRifleWood, kMatWood);
      mb.box(M::translate({0, -0.02f, 0.22f}) * M::rotateX(0.08f) * M::scale({0.042f, 0.09f, 0.3f}), kRifleWood, kMatWood);
      mb.box(M::translate({0, 0.025f, -0.06f}) * M::scale({0.05f, 0.07f, 0.24f}), kGunMetal, kMatMetal);
      mb.cylinder(M::translate({0, 0.038f, -0.17f}) * M::rotateX(-kHalfPi) * M::scale({0.013f, 0.61f, 0.013f}), 12, 1, 1, kGunMetal, kMatMetal);
      mb.cylinder(M::translate({0, 0.012f, -0.17f}) * M::rotateX(-kHalfPi) * M::scale({0.011f, 0.5f, 0.011f}), 12, 1, 1, kGunMetal, kMatMetal);
      mb.box(M::translate({0, 0.012f, -0.38f}) * M::scale({0.05f, 0.045f, 0.18f}), kRifleWood, kMatWood);  // pump
      mb.blob(M::translate({0, 0.055f, -0.76f}) * M::scale({0.006f, 0.006f, 0.006f}), 1, 0, kSteel, kMatMetal, 1, 0, 0);  // bead sight
      break;
    case Item::AssaultRifle:
      mb.box(M::translate({0, -0.05f, 0.03f}) * M::rotateX(-0.3f) * M::scale({0.03f, 0.09f, 0.045f}), kPolymer, kMatPlain);
      mb.box(M::translate({0, 0.03f, -0.1f}) * M::scale({0.05f, 0.08f, 0.38f}), kGunMetal, kMatMetal);
      mb.box(M::translate({0, 0.03f, -0.42f}) * M::scale({0.056f, 0.06f, 0.28f}), kRifleWood, kMatWood);  // handguard
      mb.cylinder(M::translate({0, 0.035f, -0.55f}) * M::rotateX(-kHalfPi) * M::scale({0.01f, 0.17f, 0.01f}), 10, 1, 1, kGunMetal, kMatMetal);
      mb.box(M::translate({0, 0.075f, -0.5f}) * M::scale({0.006f, 0.04f, 0.008f}), kGunMetal, kMatMetal);  // front sight post
      for (int k = 0; k < 3; ++k)  // curved magazine
        mb.box(M::translate({0, -0.05f - 0.045f * k, -0.07f + 0.018f * k * k}) * M::rotateX(0.18f * k) * M::scale({0.028f, 0.05f, 0.05f}),
               kGunMetal * 1.3f, kMatMetal);
      mb.box(M::translate({0, 0.01f, 0.22f}) * M::rotateX(0.06f) * M::scale({0.04f, 0.08f, 0.26f}), kRifleWood, kMatWood);
      mb.box(M::translate({0, 0.0f, 0.355f}) * M::scale({0.043f, 0.1f, 0.012f}), kPolymer, kMatPlain);
      mb.box(M::translate({0, 0.074f, -0.08f}) * M::scale({0.022f, 0.008f, 0.2f}), kSteel * 0.5f, kMatMetal);  // top rail
      break;
    case Item::Sniper:
      mb.box(M::translate({0, -0.05f, 0.04f}) * M::rotateX(-0.3f) * M::scale({0.03f, 0.09f, 0.045f}), kPolymer, kMatPlain);
      mb.box(M::translate({0, 0.025f, -0.1f}) * M::scale({0.05f, 0.065f, 0.3f}), kGunMetal, kMatMetal);
      mb.cylinder(M::translate({0, 0.03f, -0.25f}) * M::rotateX(-kHalfPi) * M::scale({0.014f, 0.77f, 0.014f}), 12, 1, 0.8f, kGunMetal, kMatMetal);
      mb.box(M::translate({0, 0.0f, -0.4f}) * M::scale({0.05f, 0.05f, 0.32f}), Vec3{0.12f, 0.13f, 0.10f}, kMatPlain);  // olive stock
      mb.box(M::translate({0, -0.015f, 0.24f}) * M::rotateX(0.05f) * M::scale({0.045f, 0.1f, 0.32f}), Vec3{0.12f, 0.13f, 0.10f}, kMatPlain);
      mb.box(M::translate({0, 0.05f, 0.18f}) * M::scale({0.04f, 0.03f, 0.12f}), Vec3{0.10f, 0.11f, 0.08f}, kMatPlain);  // cheek rest
      mb.box(M::translate({0.04f, 0.035f, 0.0f}) * M::rotateZ(-0.4f) * M::scale({0.05f, 0.012f, 0.012f}), kSteel, kMatMetal);  // bolt
      mb.blob(M::translate({0.065f, 0.025f, 0.0f}) * M::scale({0.011f, 0.011f, 0.011f}), 2, 0, kSteel, kMatMetal, 1, 0, 0);
      for (float x : {-0.015f, 0.015f})  // folded bipod
        mb.box(M::translate({x, -0.02f, -0.52f}) * M::scale({0.006f, 0.006f, 0.16f}), kSteel * 0.5f, kMatMetal);
      break;
    case Item::Bow: {
      Vec3 prev{0, -0.42f, -0.06f};
      for (int k = -6; k <= 6; ++k) {  // curved limbs, thickest at the grip
        float t = static_cast<float>(k + 1) / 7.0f;
        Vec3 next{0, t * 0.42f, -0.06f + 0.11f * t * t};
        if (k > -6) {
          float th = 0.022f - 0.012f * std::fabs(t);
          mb.box(M::beam(prev, next, th, th * 1.6f), Vec3{0.30f, 0.17f, 0.07f}, kMatWood);
        }
        prev = next;
      }
      mb.box(M::translate({0, 0, -0.06f}) * M::scale({0.03f, 0.1f, 0.035f}), kLash * 0.6f, kMatCloth);  // grip wrap
      mb.box(M::beam({0, 0.42f, 0.05f}, {0, -0.42f, 0.05f}, 0.003f, 0.003f), Vec3{0.7f, 0.68f, 0.6f}, kMatPlain);  // string
      break;
    }
    case Item::Arrow:
      for (int k = 0; k < 3; ++k) {
        Vec3 o{(k - 1) * 0.025f, 0, 0};
        mb.cylinder(M::translate(o) * M::scale({0.004f, 0.5f, 0.004f}), 6, 1, 1, Vec3{0.35f, 0.22f, 0.10f}, kMatWood);
        mb.cylinder(M::translate(o + Vec3{0, 0.5f, 0}) * M::scale({0.009f, 0.04f, 0.009f}), 6, 1, 0, kStoneCol, kMatRock);
        mb.box(M::translate(o + Vec3{0, 0.04f, 0}) * M::scale({0.02f, 0.05f, 0.002f}), Vec3{0.6f, 0.6f, 0.55f}, kMatPlain);
      }
      break;
    case Item::Shells:
      for (int k = 0; k < 3; ++k) {
        Vec3 p{(k - 1) * 0.03f, 0, (k % 2) * 0.02f};
        mb.cylinder(M::translate(p) * M::scale({0.012f, 0.016f, 0.012f}), 12, 1, 1, kBrass, kMatMetal);
        mb.cylinder(M::translate(p + Vec3{0, 0.016f, 0}) * M::scale({0.0115f, 0.05f, 0.0115f}), 12, 1, 1, Vec3{0.45f, 0.04f, 0.03f}, kMatPlain);
      }
      break;
    case Item::Coal:
      for (int k = 0; k < 3; ++k)
        mb.blob(M::translate({(k - 1) * 0.06f, 0.04f, (k % 2) * 0.04f}) * M::scale({0.05f, 0.04f, 0.05f}), 70u + k, 0.4f,
                Vec3{0.03f, 0.03f, 0.035f}, kMatRock, 1, 0, 0.1f, 30);
      break;
    case Item::HQMetalOre:
      mb.blob(M::scale({0.12f, 0.09f, 0.11f}), 14, 0.35f, Vec3{0.20f, 0.20f, 0.21f}, kMatRock, 2, 0, 0.05f, 40, 2);
      for (int k = 0; k < 4; ++k)
        mb.blob(M::translate({std::cos(k * 1.6f) * 0.09f, 0.04f + 0.02f * k, std::sin(k * 1.6f) * 0.08f}) * M::scale({0.03f, 0.04f, 0.03f}),
                80u + k, 0.3f, Vec3{0.30f, 0.45f, 0.70f}, kMatMetal, 1, 0, 0.05f, 30);
      break;
    case Item::HQMetal:
      for (int k = 0; k < 2; ++k)
        mb.box(M::translate({0, 0.025f + k * 0.05f, k * 0.02f}) * M::rotateY(k * 0.3f) * M::scale({0.18f, 0.05f, 0.08f}),
               Vec3{0.45f, 0.52f, 0.62f}, kMatMetal);
      break;
    case Item::Scrap:
      mb.box(M::translate({-0.04f, 0.02f, 0}) * M::rotateY(0.4f) * M::rotateZ(0.2f) * M::scale({0.12f, 0.02f, 0.07f}), Vec3{0.35f, 0.15f, 0.06f}, kMatMetal);
      mb.cylinder(M::translate({0.05f, 0.0f, 0.02f}) * M::scale({0.03f, 0.08f, 0.03f}), 10, 1, 1, Vec3{0.30f, 0.18f, 0.10f}, kMatMetal);
      mb.box(M::translate({0.0f, 0.05f, -0.04f}) * M::rotateX(0.5f) * M::scale({0.08f, 0.01f, 0.05f}), kSteel * 0.6f, kMatMetal);
      break;
    case Item::Rope:
      for (int k = 0; k < 10; ++k) {  // a coil
        float a0 = k * 0.628f, a1 = (k + 1) * 0.628f;
        mb.box(M::beam({std::cos(a0) * 0.08f, 0.02f + k * 0.004f, std::sin(a0) * 0.08f}, {std::cos(a1) * 0.08f, 0.02f + k * 0.004f, std::sin(a1) * 0.08f},
                       0.018f, 0.018f),
               kLash, kMatCloth);
      }
      break;
    case Item::GunParts:
      mb.cylinder(M::translate({-0.05f, 0, 0}) * M::scale({0.015f, 0.09f, 0.015f}), 10, 1, 1, kSteel, kMatMetal);
      mb.box(M::translate({0.03f, 0.02f, 0}) * M::rotateY(0.4f) * M::scale({0.08f, 0.03f, 0.03f}), kGunMetal, kMatMetal);
      mb.box(M::translate({0.02f, 0.05f, 0.03f}) * M::scale({0.02f, 0.02f, 0.06f}), kSteel * 0.7f, kMatMetal);
      break;
    case Item::Explosives:
      for (int k = 0; k < 3; ++k)
        mb.cylinder(M::translate({(k - 1) * 0.035f, 0, 0}) * M::scale({0.016f, 0.15f, 0.016f}), 10, 1, 1, Vec3{0.50f, 0.08f, 0.05f}, kMatCloth);
      mb.box(M::translate({0, 0.075f, 0}) * M::scale({0.11f, 0.02f, 0.036f}), Vec3{0.05f, 0.05f, 0.05f}, kMatPlain);  // tape
      break;
    case Item::PipeBomb:
      mb.cylinder(M::rotateZ(-kHalfPi) * M::translate({0, -0.1f, 0}) * M::scale({0.025f, 0.2f, 0.025f}), 12, 1, 1, kSteel * 0.6f, kMatMetal);
      for (float x : {-0.1f, 0.1f}) mb.box(M::translate({x, 0, 0}) * M::scale({0.02f, 0.06f, 0.06f}), kSteel * 0.4f, kMatMetal);
      mb.box(M::beam({0.11f, 0.0f, 0.0f}, {0.15f, 0.04f, 0.0f}, 0.004f, 0.004f), Vec3{0.6f, 0.55f, 0.4f}, kMatPlain);  // fuse
      break;
    case Item::ExplosiveCharge:
      mb.box(M::translate({0, 0.03f, 0}) * M::scale({0.16f, 0.06f, 0.1f}), Vec3{0.45f, 0.40f, 0.30f}, kMatCloth);
      mb.box(M::translate({0, 0.07f, 0}) * M::scale({0.06f, 0.02f, 0.04f}), Vec3{0.05f, 0.05f, 0.05f}, kMatPlain);  // timer
      mb.box(M::translate({0, 0.081f, 0}) * M::scale({0.03f, 0.003f, 0.02f}), Vec3{0.8f, 0.05f, 0.03f}, kMatEmissive);
      mb.box(M::beam({-0.03f, 0.07f, 0.0f}, {-0.08f, 0.06f, 0.05f}, 0.004f, 0.004f), Vec3{0.6f, 0.05f, 0.03f}, kMatPlain);
      break;
    case Item::CodeLock:
      mb.box(M::translate({0, 0.06f, 0}) * M::scale({0.07f, 0.12f, 0.025f}), Vec3{0.55f, 0.48f, 0.12f}, kMatMetal);
      for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 3; ++c)
          mb.box(M::translate({(c - 1) * 0.018f, 0.03f + r * 0.02f, -0.014f}) * M::scale({0.012f, 0.012f, 0.004f}), Vec3{0.06f, 0.06f, 0.06f}, kMatPlain);
      break;
    case Item::LandClaim:
      mb.box(M::translate({0, 0.25f, 0}) * M::scale({0.14f, 0.5f, 0.12f}), Vec3{0.24f, 0.13f, 0.05f}, kMatWood);
      mb.box(M::translate({0, 0.42f, -0.065f}) * M::scale({0.1f, 0.12f, 0.01f}), Vec3{0.6f, 0.55f, 0.35f}, kMatCloth);
      break;
    case Item::LargeBox:
      mb.box(M::translate({0, 0.12f, 0}) * M::scale({0.5f, 0.24f, 0.25f}), Vec3{0.28f, 0.16f, 0.07f}, kMatWood);
      for (float x : {-0.24f, 0.24f}) mb.box(M::translate({x, 0.12f, 0}) * M::scale({0.03f, 0.25f, 0.26f}), kSteel * 0.5f, kMatMetal);
      break;
    case Item::SmallStash:
      mb.blob(M::translate({0, 0.03f, 0}) * M::scale({0.12f, 0.035f, 0.12f}), 9, 0.1f, Vec3{0.30f, 0.24f, 0.14f}, kMatCloth, 2);
      break;
    case Item::SheetMetalDoor:
    case Item::ArmoredDoor: {
      Vec3 c = item == Item::ArmoredDoor ? Vec3{0.12f, 0.13f, 0.15f} : Vec3{0.30f, 0.31f, 0.33f};
      mb.box(M::translate({0, 0.5f, 0}) * M::scale({0.5f, 1.0f, 0.05f}), c, kMatMetal);
      for (float y : {0.2f, 0.8f}) mb.box(M::translate({0, y, 0.03f}) * M::scale({0.5f, 0.06f, 0.02f}), c * 0.7f, kMatMetal);
      mb.box(M::translate({0.18f, 0.5f, 0.04f}) * M::scale({0.03f, 0.06f, 0.03f}), kSteel, kMatMetal);
      break;
    }
    // --- attachments: origin at the gun's mount point ---
    case Item::HoloSight:
      mb.box(M::translate({0, 0.006f, 0}) * M::scale({0.03f, 0.012f, 0.05f}), kGunMetal, kMatMetal);
      for (float x : {-0.017f, 0.017f}) mb.box(M::translate({x, 0.028f, 0.0f}) * M::scale({0.005f, 0.04f, 0.012f}), kGunMetal, kMatMetal);
      mb.box(M::translate({0, 0.05f, 0.0f}) * M::scale({0.039f, 0.006f, 0.012f}), kGunMetal, kMatMetal);
      break;
    case Item::Scope4x:
    case Item::Scope8x: {
      float len = item == Item::Scope8x ? 0.32f : 0.22f;
      float r = item == Item::Scope8x ? 0.018f : 0.016f;
      for (float z : {-0.05f, 0.05f}) mb.box(M::translate({0, 0.01f, z}) * M::scale({0.02f, 0.02f, 0.015f}), kSteel * 0.5f, kMatMetal);
      mb.cylinder(M::translate({0, 0.03f, len * 0.5f}) * M::rotateX(-kHalfPi) * M::scale({r, len, r}), 14, 1, 1, kGunMetal, kMatMetal);
      mb.cylinder(M::translate({0, 0.03f, -len * 0.5f + 0.04f}) * M::rotateX(-kHalfPi) * M::scale({r * 1.45f, 0.06f, r * 1.45f}), 14, 1, 1,
                  kGunMetal, kMatMetal);  // objective bell
      mb.cylinder(M::translate({0, 0.03f, len * 0.5f}) * M::rotateX(-kHalfPi) * M::scale({r * 1.2f, 0.04f, r * 1.2f}), 14, 1, 1, kGunMetal, kMatMetal);
      mb.cylinder(M::translate({0, 0.03f + r, 0}) * M::scale({0.007f, 0.012f, 0.007f}), 8, 1, 1, kGunMetal, kMatMetal);  // turret
      break;
    }
    case Item::Silencer:
      mb.cylinder(M::rotateX(-kHalfPi) * M::scale({0.017f, 0.17f, 0.017f}), 14, 1, 1, Vec3{0.03f, 0.03f, 0.035f}, kMatMetal);
      break;
    case Item::MuzzleBrake:
      mb.cylinder(M::rotateX(-kHalfPi) * M::scale({0.015f, 0.065f, 0.015f}), 12, 1, 1, kGunMetal, kMatMetal);
      for (float z : {-0.02f, -0.045f}) mb.box(M::translate({0, 0, z}) * M::scale({0.034f, 0.008f, 0.01f}), kSteel * 0.5f, kMatMetal);
      break;
    case Item::VerticalGrip:
      mb.box(M::translate({0, -0.008f, 0}) * M::scale({0.03f, 0.016f, 0.05f}), kPolymer, kMatPlain);
      mb.cylinder(M::translate({0, -0.11f, 0}) * M::scale({0.016f, 0.1f, 0.016f}), 10, 1, 1.1f, kPolymer, kMatPlain);
      break;
    case Item::LaserSight:
      mb.box(M::translate({0, -0.016f, 0}) * M::scale({0.026f, 0.03f, 0.07f}), kPolymer, kMatPlain);
      mb.cylinder(M::translate({0, -0.016f, -0.036f}) * M::rotateX(-kHalfPi) * M::scale({0.007f, 0.003f, 0.007f}), 8, 1, 1,
                  Vec3{3.0f, 0.1f, 0.05f}, kMatEmissive);
      break;
    default: {
      const Vec3 c = itemDef(item).color;
      mb.box(M::scale({0.1f, 0.1f, 0.1f}), Vec3{c.x * c.x, c.y * c.y, c.z * c.z}, kMatPlain);
      break;
    }
  }
}

GunMounts gunMounts(Item gun) {
  switch (gun) {
    case Item::Pistol: return {{0, 0.053f, -0.04f}, {0, 0.032f, -0.17f}, {0, -0.02f, -0.11f}};
    case Item::Revolver: return {{0, 0.055f, -0.06f}, {0, 0.035f, -0.215f}, {0, -0.01f, -0.13f}};
    case Item::SMG: return {{0, 0.066f, -0.08f}, {0, 0.035f, -0.36f}, {0, -0.005f, -0.22f}};
    case Item::Shotgun: return {{0, 0.06f, -0.05f}, {0, 0.038f, -0.78f}, {0, -0.012f, -0.38f}};
    case Item::AssaultRifle: return {{0, 0.078f, -0.1f}, {0, 0.035f, -0.72f}, {0, -0.002f, -0.42f}};
    case Item::Rifle: return {{0, 0.048f, -0.08f}, {0, 0.03f, -0.82f}, {0, -0.025f, -0.36f}};
    case Item::Sniper: return {{0, 0.058f, -0.08f}, {0, 0.03f, -1.02f}, {0, -0.025f, -0.42f}};
    default: return {{0, 0.05f, 0}, {0, 0.03f, -0.2f}, {0, -0.02f, -0.1f}};
  }
}

float sightEyeHeight(Item attachment) {
  switch (attachment) {
    case Item::HoloSight: return 0.028f;
    case Item::Scope4x:
    case Item::Scope8x: return 0.03f;
    default: return 0.0f;
  }
}

// --- resource nodes ----------------------------------------------------------------------
// A clump of leaf cards around c, shaded as one soft volume around lightCenter.
static void leafClump(MeshBuilder& mb, const Vec3& c, float radius, int count, uint32_t seed, const Vec3& color,
                      const Vec3& lightCenter, float sway, int mat = kMatLeafCard) {
  for (int k = 0; k < count; ++k) {
    uint32_t h = seed * 977u + static_cast<uint32_t>(k) * 131u;
    Vec3 off = Vec3{hashF(h) - 0.5f, (hashF(h + 1) - 0.5f) * 0.7f, hashF(h + 2) - 0.5f} * (radius * 1.5f);
    Vec3 center = c + off;
    Vec3 facing = normalize(Vec3{hashF(h + 3) - 0.5f, hashF(h + 4) * 0.9f - 0.15f, hashF(h + 5) - 0.5f});
    Vec3 t1 = normalize(cross(facing, std::fabs(facing.y) > 0.9f ? Vec3{1, 0, 0} : Vec3{0, 1, 0}));
    Vec3 t2 = cross(facing, t1);
    float sz = (0.55f + 0.35f * hashF(h + 6)) * radius;
    const Vec3 q[4] = {center - t1 * sz - t2 * sz, center + t1 * sz - t2 * sz, center + t1 * sz + t2 * sz,
                       center - t1 * sz + t2 * sz};
    const float sw[4] = {sway * 0.8f, sway, sway, sway * 0.8f};
    // The red channel's low digits seed the leaf pattern in the shader, so neighbours differ.
    Vec3 col = color * (0.8f + 0.35f * hashF(h + 7));
    mb.card(q, lightCenter, sw, col, mat);
  }
}

void buildNode(const ResourceNode& n, MeshBuilder& mb, bool detailed) {
  using M = Mat4;
  const float s = n.scale;
  const Vec3 p = n.pos;
  const uint32_t id = n.id;
  const float rot = hashF(id * 7u) * 2 * kPi;
  switch (n.type) {
    case NodeType::Tree: {
      const bool pine = hashF(id * 3u) < 0.35f || p.y > 32.0f;
      if (pine) {
        const float height = 7.5f * s;
        mb.cylinder(M::translate(p - Vec3{0, 0.2f, 0}) * M::scale({0.26f * s, height, 0.26f * s}), 9, 1, 0.12f, kBark, kMatBark);
        Vec3 needle = lerp(Vec3{0.020f, 0.065f, 0.035f}, Vec3{0.035f, 0.095f, 0.045f}, hashF(id));
        if (detailed) {
          // Whorls of drooping needle branches, wide at the bottom and narrow at the top.
          for (int lv = 0; lv < 8; ++lv) {
            float y = (1.5f + lv * 0.75f) * s;
            float len = (2.5f - lv * 0.27f) * s;
            int count = 7 - lv / 3;
            for (int b = 0; b < count; ++b) {
              float a = rot + lv * 0.7f + b * 2 * kPi / static_cast<float>(count);
              Vec3 dir = normalize(Vec3{std::cos(a), -0.28f, std::sin(a)});
              Vec3 side = normalize(Vec3{-dir.z, 0, dir.x});
              Vec3 base = p + Vec3{0, y, 0};
              Vec3 tip = base + dir * len;
              float w = 0.42f * len;
              const float sw[4] = {0.1f, 0.6f, 0.6f, 0.1f};
              const Vec3 flat[4] = {base - side * (w * 0.3f), tip - side * w * 0.5f, tip + side * w * 0.5f, base + side * (w * 0.3f)};
              mb.card(flat, p + Vec3{0, y + 0.6f * s, 0}, sw, needle * (0.85f + 0.3f * hashF(id + lv * 9u + b)), kMatNeedles);
              Vec3 up{0, w * 0.45f, 0};
              const Vec3 vert[4] = {base - up * 0.3f, tip - up, tip + up * 0.6f, base + up * 0.3f};
              mb.card(vert, p + Vec3{0, y + 0.6f * s, 0}, sw, needle * 0.8f, kMatNeedles);
            }
          }
          mb.cylinder(M::translate(p + Vec3{0, 6.6f * s, 0}) * M::scale({0.45f * s, 1.6f * s, 0.45f * s}), 8, 1, 0.0f, needle, kMatLeaves, 0.7f);
        } else {
          for (int k = 0; k < 4; ++k) {
            float r = (1.9f - 0.38f * k) * s;
            float y = (1.3f + 1.1f * k) * s;
            mb.cylinder(M::translate(p + Vec3{0, y, 0}) * M::rotateY(rot + k) * M::scale({r, 2.1f * s, r}), 9, 1, 0.0f,
                        needle * (1.0f + 0.06f * k), kMatLeaves, 0.6f + 0.1f * k);
          }
        }
      } else {
        // Broadleaf: flared, tapering trunk, a few branches, leaf clumps at their ends.
        mb.cylinder(M::translate(p - Vec3{0, 0.2f, 0}) * M::rotateY(rot) * M::scale({0.3f * s, 3.8f * s, 0.3f * s}), 10, 1, 0.55f, kBark, kMatBark);
        mb.cylinder(M::translate(p - Vec3{0, 0.25f, 0}) * M::scale({0.48f * s, 0.75f * s, 0.48f * s}), 10, 1, 0.6f, kBark, kMatBark);
        Vec3 leaf = lerp(Vec3{0.030f, 0.10f, 0.018f}, Vec3{0.065f, 0.13f, 0.025f}, hashF(id * 5u));
        const Vec3 crown = p + Vec3{0, 4.7f * s, 0};
        // Dark inner canopy fills the gaps between the leaf cards.
        mb.blob(M::translate(crown) * M::rotateY(rot) * M::scale({1.7f * s, 1.25f * s, 1.7f * s}), id, 0.35f, leaf * 0.55f,
                kMatLeaves, 1, 0.5f, 0.1f, 70);
        const int branches = 5;
        for (int b = 0; b < branches; ++b) {
          float a = rot + b * 2 * kPi / branches + hashF(id + b) * 0.6f;
          float h0 = (2.4f + 0.35f * b) * s;
          Vec3 start = p + Vec3{0, h0, 0};
          Vec3 end = start + Vec3{std::cos(a) * 1.7f * s, (1.3f + 0.3f * hashF(id + 11u * b)) * s, std::sin(a) * 1.7f * s};
          mb.cylinder(alongY(start, end, 0.11f * s), 7, 1, 0.45f, kBark, kMatBark, 0.2f, false);
          if (detailed) leafClump(mb, end, 1.05f * s, 9, id * 13u + b, leaf, crown, 0.9f);
          else leafClump(mb, end, 1.1f * s, 2, id * 13u + b, leaf, crown, 0.9f);
        }
        if (detailed) {
          leafClump(mb, crown + Vec3{0, 1.4f * s, 0}, 1.2f * s, 10, id * 17u, leaf * 1.1f, crown, 1.0f);
          leafClump(mb, crown, 1.5f * s, 8, id * 19u, leaf * 0.9f, crown, 0.8f);
        }
      }
      break;
    }
    case NodeType::Stone:
    case NodeType::MetalOre:
    case NodeType::Sulfur:
    case NodeType::Coal:
    case NodeType::HQMetal: {
      Vec3 base = n.type == NodeType::Stone ? Vec3{0.30f, 0.29f, 0.27f}
                  : n.type == NodeType::Coal ? Vec3{0.07f, 0.07f, 0.075f}
                                             : Vec3{0.22f, 0.21f, 0.20f};
      int sub = detailed ? 2 : 1;
      mb.blob(M::translate(p + Vec3{0, 0.4f * s, 0}) * M::rotateY(rot) * M::scale({1.05f * s, 0.8f * s, 0.9f * s}), id, 0.38f,
              base, kMatRock, sub, 0, 0.05f, 38, 2);
      mb.blob(M::translate(p + Vec3{0.65f * s, 0.2f * s, 0.3f * s}) * M::rotateY(rot * 2) * M::scale(Vec3{0.55f, 0.45f, 0.5f} * s),
              id + 9u, 0.4f, base * 0.9f, kMatRock, 1, 0, 0.05f, 38, 2);
      if (n.type != NodeType::Stone) {
        Vec3 ore = n.type == NodeType::MetalOre ? Vec3{0.45f, 0.16f, 0.05f}
                   : n.type == NodeType::Sulfur   ? Vec3{0.78f, 0.62f, 0.05f}
                   : n.type == NodeType::Coal     ? Vec3{0.02f, 0.02f, 0.02f}
                                                  : Vec3{0.30f, 0.45f, 0.75f};
        int mat = n.type == NodeType::MetalOre || n.type == NodeType::HQMetal ? kMatMetal : kMatRock;
        for (int k = 0; k < 7; ++k) {
          float a = rot + k * 0.9f;
          float r = 0.78f * s;
          Vec3 q = p + Vec3{std::cos(a) * r, (0.3f + 0.4f * hashF(id + k)) * s * 1.2f, std::sin(a) * r * 0.85f};
          mb.blob(M::translate(q) * M::scale(Vec3{0.2f, 0.15f, 0.2f} * s), id + 31u * k, 0.45f, ore, mat, 1, 0, 0.1f, 35);
        }
      }
      break;
    }
    case NodeType::Hemp: {
      const Vec3 green{0.08f, 0.22f, 0.03f};
      for (int k = 0; k < 6; ++k) {
        float a = rot + k * 1.05f;
        Vec3 q = p + Vec3{std::cos(a) * 0.12f, 0, std::sin(a) * 0.12f};
        float h = 1.0f + 0.4f * hashF(id + k);
        mb.cylinder(M::translate(q) * M::scale({0.02f, h, 0.02f}), 5, 1, 0.5f, green * 0.8f, kMatLeaves, 0.6f, false);
        leafClump(mb, q + Vec3{0, h * 0.8f, 0}, 0.28f, 3, id * 7u + k, green * 1.4f, q + Vec3{0, h * 0.6f, 0}, 0.8f);
      }
      break;
    }
    case NodeType::BerryBush: {
      const Vec3 leaf{0.035f, 0.11f, 0.025f};
      mb.blob(M::translate(p + Vec3{0, 0.42f, 0}) * M::rotateY(rot) * M::scale({0.6f, 0.42f, 0.55f}), id, 0.35f, leaf * 0.6f,
              kMatLeaves, 1, 0.3f, 0.15f, 70);
      leafClump(mb, p + Vec3{0, 0.5f, 0}, 0.55f, detailed ? 10 : 4, id, leaf, p + Vec3{0, 0.3f, 0}, 0.3f);
      for (int k = 0; k < 10; ++k) {
        float a = rot + k * 0.7f;
        float h = 0.25f + 0.5f * hashF(id + 7u * k);
        float r = 0.62f * std::sqrt(std::max(0.05f, 1.0f - ((h - 0.45f) / 0.5f) * ((h - 0.45f) / 0.5f)));
        Vec3 q = p + Vec3{std::cos(a) * r, h, std::sin(a) * r};
        mb.blob(M::translate(q) * M::scale({0.055f, 0.055f, 0.055f}), id + k, 0.05f, Vec3{0.50f, 0.02f, 0.06f}, kMatPlain, 1, 0.3f, 0.1f, 80);
      }
      break;
    }
    case NodeType::Scrap: {
      // Junk washed up on the shore: rusty sheets, a tipped barrel, a crate.
      const Vec3 rust{0.30f, 0.12f, 0.04f};
      mb.cylinder(M::translate(p + Vec3{0.2f, 0.25f, 0}) * M::rotateZ(1.3f) * M::rotateY(rot) * M::scale({0.28f, 0.85f, 0.28f}), 12, 1, 1,
                  rust, kMatMetal);
      mb.box(M::translate(p + Vec3{-0.35f, 0.08f, 0.2f}) * M::rotateY(rot) * M::rotateX(0.2f) * M::scale({0.9f, 0.04f, 0.6f}),
             Vec3{0.25f, 0.20f, 0.16f}, kMatMetal);
      mb.box(M::translate(p + Vec3{-0.2f, 0.2f, -0.35f}) * M::rotateY(rot * 2) * M::scale({0.45f, 0.4f, 0.45f}), Vec3{0.20f, 0.12f, 0.06f}, kMatWood);
      mb.box(M::translate(p + Vec3{0.4f, 0.05f, -0.3f}) * M::rotateY(rot + 1) * M::scale({0.5f, 0.06f, 0.08f}), kSteel * 0.4f, kMatMetal);
      break;
    }
    case NodeType::Mushroom:
      for (int k = 0; k < 3; ++k) {
        float a = rot + k * 2.1f;
        float sz = 1.0f - 0.25f * k;
        Vec3 q = p + Vec3{std::cos(a) * 0.15f * k, 0, std::sin(a) * 0.15f * k};
        mb.cylinder(M::translate(q) * M::scale(Vec3{0.04f, 0.18f, 0.04f} * sz), 8, 1, 0.8f, Vec3{0.70f, 0.66f, 0.56f}, kMatPlain);
        mb.blob(M::translate(q + Vec3{0, 0.18f * sz, 0}) * M::scale(Vec3{0.16f, 0.08f, 0.16f} * sz), id + k, 0.1f,
                Vec3{0.40f, 0.10f, 0.04f}, kMatPlain, 2, 0, 0.05f, 80);
      }
      break;
    default:
      break;
  }
}

// --- placed objects -----------------------------------------------------------------------
static int tierMaterial(Tier t) {
  switch (t) {
    case Tier::Twig: return kMatTwig;
    case Tier::Wood: return kMatWood;
    case Tier::Stone: return kMatStone;
    case Tier::Metal: return kMatMetal;
  }
  return kMatPlain;
}
static Vec3 tierColor(Tier t) {
  switch (t) {
    case Tier::Twig: return {0.36f, 0.25f, 0.11f};
    case Tier::Wood: return {0.28f, 0.16f, 0.07f};
    case Tier::Stone: return {0.40f, 0.39f, 0.36f};
    case Tier::Metal: return {0.28f, 0.29f, 0.31f};
  }
  return {0.5f, 0.5f, 0.5f};
}

void buildObject(const WorldObject& o, MeshBuilder& mb) {
  using M = Mat4;
  const float hpFrac = clampf(o.hp / maxHp(o.type, o.tier), 0.0f, 1.0f);
  const float wear = 0.6f + 0.4f * hpFrac;  // damaged pieces get darker
  if (isBuildingPiece(o.type)) {
    std::vector<ObjBox> boxes;
    objectBoxes(o, boxes, true);
    Vec3 c = tierColor(o.tier) * wear;
    if (o.type == ObjType::Foundation) c = c * 0.85f;
    const int mat = tierMaterial(o.tier);
    for (const auto& b : boxes) {
      Vec3 ctr = (b.box.min + b.box.max) * 0.5f, size = b.box.max - b.box.min;
      mb.box(M::translate(ctr) * M::scale(size), c, mat);
    }
    // Trim so pieces read as built structures, not plain blocks.
    const Vec3& p = o.pos;
    const Vec3 trim = c * 0.7f;
    if (o.type == ObjType::Wall || o.type == ObjType::Doorway) {
      Vec3 along = o.rot % 2 == 0 ? Vec3{1, 0, 0} : Vec3{0, 0, 1};
      Vec3 across = o.rot % 2 == 0 ? Vec3{0, 0, 1} : Vec3{1, 0, 0};
      auto sized = [&](float a, float h, float t) { return along * a + Vec3{0, h, 0} + across * t; };
      for (float side : {-1.0f, 1.0f})  // corner posts
        mb.box(M::translate(p + along * (side * 1.45f) + Vec3{0, 1.5f, 0}) * M::scale(sized(0.2f, 3.0f, 0.3f)), trim, mat);
      if (o.tier == Tier::Twig || o.tier == Tier::Wood)
        mb.box(M::translate(p + Vec3{0, 2.92f, 0}) * M::scale(sized(3.0f, 0.16f, 0.28f)), trim, mat);  // top beam
      mb.box(M::translate(p + Vec3{0, 0.12f, 0}) * M::scale(sized(3.0f, 0.24f, 0.3f)), trim * 0.9f, mat);  // base plate
    } else if (o.type == ObjType::Foundation) {
      mb.box(M::translate(p - Vec3{0, 0.22f, 0}) * M::scale({3.12f, 0.3f, 3.12f}), trim, mat);  // rim
    }
    return;
  }
  const M base = M::translate(o.pos) * M::rotateY(static_cast<float>(o.rot) * kPi * 0.5f);
  switch (o.type) {
    case ObjType::Door: {
      std::vector<ObjBox> boxes;
      objectBoxes(o, boxes, true);
      const AABB& b = boxes[0].box;
      Vec3 ctr = (b.min + b.max) * 0.5f, size = b.max - b.min;
      const bool metalDoor = o.tier >= Tier::Stone;
      Vec3 doorCol = o.tier == Tier::Metal ? Vec3{0.12f, 0.13f, 0.15f} : o.tier == Tier::Stone ? Vec3{0.30f, 0.31f, 0.33f}
                                                                                               : Vec3{0.18f, 0.09f, 0.035f};
      mb.box(M::translate(ctr) * M::scale(size), doorCol * wear, metalDoor ? kMatMetal : kMatWood);
      bool open = o.flags & kDoorOpen;
      bool alongX = (o.rot % 2 == 0) != open;  // which way the slab runs now
      Vec3 across = alongX ? Vec3{0, 0, 1} : Vec3{1, 0, 0};
      Vec3 along = alongX ? Vec3{1, 0, 0} : Vec3{0, 0, 1};
      float th = (alongX ? size.z : size.x) * 0.5f;
      for (float side : {-1.0f, 1.0f}) {
        Vec3 f = across * (side * (th + 0.012f));
        mb.blob(M::translate(ctr + f + along * 0.45f) * M::scale(Vec3{0.03f, 0.03f, 0.03f}), 1, 0, kSteel, kMatMetal, 1, 0, 0);
        if (o.flags & kLocked)  // code lock keypad by the handle
          mb.box(M::translate(ctr + f * 1.6f + along * 0.45f + Vec3{0, 0.18f, 0}) * M::scale(along * 0.06f + Vec3{0, 0.11f, 0} + across * 0.02f),
                 Vec3{0.55f, 0.48f, 0.12f}, kMatMetal);
        mb.box(M::beam(ctr + f - along * 0.5f + Vec3{0, -0.8f, 0}, ctr + f + along * 0.5f + Vec3{0, 0.8f, 0}, 0.1f, 0.02f),
               Vec3{0.13f, 0.06f, 0.025f}, kMatWood);
      }
      break;
    }
    case ObjType::Workbench:
      mb.box(base * M::translate({0, 0.86f, 0}) * M::scale({1.6f, 0.08f, 0.8f}), Vec3{0.30f, 0.17f, 0.07f} * wear, kMatWood);
      for (float x : {-0.72f, 0.72f})
        for (float z : {-0.32f, 0.32f})
          mb.box(base * M::translate({x, 0.41f, z}) * M::scale({0.1f, 0.82f, 0.1f}), Vec3{0.22f, 0.12f, 0.05f}, kMatWood);
      mb.box(base * M::translate({0, 0.2f, 0}) * M::scale({1.45f, 0.05f, 0.65f}), Vec3{0.24f, 0.13f, 0.05f}, kMatWood);
      mb.box(base * M::translate({0.55f, 0.97f, 0.22f}) * M::scale({0.22f, 0.14f, 0.16f}), kSteel * 0.7f, kMatMetal);
      mb.box(base * M::translate({-0.3f, 0.92f, -0.1f}) * M::rotateY(0.3f) * M::scale({0.5f, 0.02f, 0.1f}), kSteel, kMatMetal);
      mb.box(base * M::translate({-0.3f, 0.92f, 0.1f}) * M::rotateY(0.3f) * M::scale({0.12f, 0.03f, 0.04f}), kHandle, kMatWood);
      break;
    case ObjType::StorageBox:
      if (o.flags & kLocked) mb.box(base * M::translate({0, 0.38f, -0.33f}) * M::scale({0.07f, 0.12f, 0.03f}), Vec3{0.55f, 0.48f, 0.12f}, kMatMetal);
      mb.box(base * M::translate({0, 0.27f, 0}) * M::scale({0.9f, 0.54f, 0.6f}), Vec3{0.28f, 0.16f, 0.07f} * wear, kMatWood);
      mb.box(base * M::translate({0, 0.56f, 0}) * M::scale({0.94f, 0.08f, 0.64f}), Vec3{0.22f, 0.12f, 0.05f} * wear, kMatWood);
      for (float x : {-0.3f, 0.3f}) mb.box(base * M::translate({x, 0.3f, 0}) * M::scale({0.06f, 0.62f, 0.63f}), kSteel * 0.5f, kMatMetal);
      mb.box(base * M::translate({0, 0.45f, -0.32f}) * M::scale({0.1f, 0.12f, 0.03f}), kSteel, kMatMetal);
      break;
    case ObjType::LargeBox:
      mb.box(base * M::translate({0, 0.37f, 0}) * M::scale({1.6f, 0.74f, 0.7f}), Vec3{0.28f, 0.16f, 0.07f} * wear, kMatWood);
      mb.box(base * M::translate({0, 0.77f, 0}) * M::scale({1.64f, 0.08f, 0.74f}), Vec3{0.22f, 0.12f, 0.05f} * wear, kMatWood);
      for (float x : {-0.78f, 0.78f})
        for (float z : {-0.33f, 0.33f})
          mb.box(base * M::translate({x, 0.4f, z}) * M::scale({0.08f, 0.82f, 0.08f}), kSteel * 0.45f, kMatMetal);  // corner guards
      if (o.flags & kLocked) mb.box(base * M::translate({0, 0.55f, -0.37f}) * M::scale({0.07f, 0.12f, 0.03f}), Vec3{0.55f, 0.48f, 0.12f}, kMatMetal);
      break;
    case ObjType::Stash:
      mb.blob(base * M::translate({0, 0.05f, 0}) * M::scale({0.25f, 0.06f, 0.25f}), o.id, 0.15f, Vec3{0.20f, 0.17f, 0.10f} * wear, kMatCloth, 2);
      break;
    case ObjType::LandClaim:
      mb.box(base * M::translate({0, 0.8f, 0}) * M::scale({0.5f, 1.6f, 0.4f}), Vec3{0.22f, 0.12f, 0.05f} * wear, kMatWood);
      mb.box(base * M::translate({0, 1.05f, -0.21f}) * M::scale({0.36f, 0.42f, 0.02f}), Vec3{0.55f, 0.50f, 0.32f}, kMatCloth);  // claim notice
      mb.cylinder(base * M::translate({0, 1.6f, 0}) * M::scale({0.015f, 0.6f, 0.015f}), 6, 1, 1, kSteel * 0.6f, kMatMetal);  // flag pole
      mb.box(base * M::translate({0.16f, 2.08f, 0}) * M::scale({0.3f, 0.18f, 0.01f}), Vec3{0.55f, 0.08f, 0.05f}, kMatCloth, 1.0f);
      if (o.flags & kLocked) mb.box(base * M::translate({0.15f, 0.75f, -0.21f}) * M::scale({0.07f, 0.12f, 0.03f}), Vec3{0.55f, 0.48f, 0.12f}, kMatMetal);
      break;
    case ObjType::Charge: {
      MeshBuilder bomb;
      buildItemModel(Item::ExplosiveCharge, bomb);
      for (auto vtx : bomb.v) {  // copy the item model into place
        Vec3 q = Vec3{vtx.px, vtx.py, vtx.pz} + o.pos - Vec3{0, 0.05f, 0};
        vtx.px = q.x; vtx.py = q.y; vtx.pz = q.z;
        mb.idx.push_back(static_cast<uint32_t>(mb.v.size()));
        mb.v.push_back(vtx);
      }
      break;
    }
    case ObjType::SleepingBag:
      mb.blob(base * M::translate({0, 0.07f, 0.1f}) * M::scale({0.4f, 0.09f, 0.9f}), o.id, 0.05f, Vec3{0.42f, 0.06f, 0.04f} * wear, kMatCloth, 2);
      mb.blob(base * M::translate({0, 0.1f, -0.82f}) * M::scale({0.3f, 0.08f, 0.16f}), o.id + 1, 0.05f, Vec3{0.55f, 0.50f, 0.40f}, kMatCloth, 2);
      break;
    case ObjType::LootBag:
      mb.blob(M::translate(o.pos + Vec3{0, 0.22f, 0}) * M::scale({0.3f, 0.25f, 0.28f}), o.id, 0.2f, Vec3{0.20f, 0.14f, 0.07f}, kMatCloth, 2);
      mb.cylinder(M::translate(o.pos + Vec3{0, 0.42f, 0}) * M::scale({0.08f, 0.12f, 0.08f}), 10, 1, 1.4f, Vec3{0.18f, 0.12f, 0.06f}, kMatCloth);
      mb.cylinder(M::translate(o.pos + Vec3{0, 0.44f, 0}) * M::scale({0.085f, 0.03f, 0.085f}), 10, 1, 1, kLash, kMatCloth);
      break;
    default:
      break;
  }
}

// --- terrain and grass -----------------------------------------------------------------------
Vec3 groundColor(float h, const Vec3& n, float variation) {
  const Vec3 sand{0.50f, 0.42f, 0.28f}, wetSand{0.30f, 0.25f, 0.17f};
  const Vec3 grass{0.075f, 0.17f, 0.035f}, dryGrass{0.20f, 0.20f, 0.07f};
  const Vec3 rock{0.22f, 0.21f, 0.19f}, snow{0.80f, 0.82f, 0.86f}, dirt{0.16f, 0.11f, 0.06f};
  Vec3 c = lerp(grass, dryGrass, clampf(variation * 1.2f - 0.1f, 0, 1));
  if (h > 38.0f) c = lerp(c, dirt, clampf((h - 38.0f) / 14.0f, 0, 0.6f));
  if (h < 2.2f) c = lerp(sand, c, clampf((h - 1.2f) / 1.0f, 0, 1));
  if (h < 0.4f) c = lerp(wetSand, sand, clampf((h + 0.6f) / 1.0f, 0, 1));
  if (h > 62.0f) c = lerp(c, snow, clampf((h - 62.0f) / 6.0f, 0, 1));
  float steep = clampf((0.84f - n.y) / 0.12f, 0, 1);
  return lerp(c, rock, steep);
}

void buildGrass(const Terrain& t, float cx, float cz, float radius, MeshBuilder& mb) {
  const float cell = 1.1f;
  int i0 = static_cast<int>(std::floor((cx - radius) / cell)), i1 = static_cast<int>(std::floor((cx + radius) / cell));
  int j0 = static_cast<int>(std::floor((cz - radius) / cell)), j1 = static_cast<int>(std::floor((cz + radius) / cell));
  const float r2 = radius * radius;
  for (int j = j0; j <= j1; ++j)
    for (int i = i0; i <= i1; ++i) {
      uint32_t hsh = static_cast<uint32_t>(i) * 73856093u ^ static_cast<uint32_t>(j) * 19349663u;
      if (hashF(hsh) > 0.5f) continue;
      float x = (static_cast<float>(i) + hashF(hsh + 1)) * cell, z = (static_cast<float>(j) + hashF(hsh + 2)) * cell;
      float dx = x - cx, dz = z - cz;
      if (dx * dx + dz * dz > r2) continue;
      float h = t.heightAt(x, z);
      if (h < 2.4f || h > 48.0f) continue;
      Vec3 n = t.normalAt(x, z);
      if (n.y < 0.86f) continue;
      Vec3 base = groundColor(h, n, hashF(hsh + 3) * 0.6f) * (1.0f + 0.4f * hashF(hsh + 4));
      const int blades = 5;
      for (int b = 0; b < blades; ++b) {
        float a = hashF(hsh + 10u + b) * 2 * kPi;
        float height = 0.28f + 0.4f * hashF(hsh + 20u + b);
        float w = 0.045f;
        Vec3 dir{std::cos(a), 0, std::sin(a)};
        Vec3 side{-dir.z * w, 0, dir.x * w};
        Vec3 root{x + dir.x * 0.07f * b, h - 0.03f, z + dir.z * 0.07f * b};
        // Two segments that bend outward: a curved blade instead of a flat spike.
        Vec3 mid = root + Vec3{dir.x * 0.05f, height * 0.55f, dir.z * 0.05f};
        Vec3 tip = root + Vec3{dir.x * 0.2f, height, dir.z * 0.2f};
        Vec3 ms = side * 0.7f;
        mb.tri(root - side, root + side, mid + ms, base, kMatGrass, 0, 0, 0.45f);
        mb.tri(root - side, mid + ms, mid - ms, base, kMatGrass, 0, 0.45f, 0.45f);
        mb.tri(mid - ms, mid + ms, tip, base, kMatGrass, 0.45f, 0.45f, 1);
        mb.tri(root + side, root - side, mid - ms, base, kMatGrass, 0, 0, 0.45f);  // back faces
        mb.tri(root + side, mid - ms, mid + ms, base, kMatGrass, 0, 0.45f, 0.45f);
        mb.tri(mid + ms, mid - ms, tip, base, kMatGrass, 0.45f, 0.45f, 1);
      }
      if (hashF(hsh + 99) < 0.025f) {  // the odd wildflower
        Vec3 col = hashF(hsh + 98) < 0.5f ? Vec3{0.8f, 0.7f, 0.1f} : Vec3{0.8f, 0.8f, 0.85f};
        mb.blob(Mat4::translate({x, h + 0.42f, z}) * Mat4::scale({0.04f, 0.025f, 0.04f}), hsh, 0.2f, col, kMatPlain, 0, 1.0f, 0.1f, 0);
      }
    }
}

}  // namespace sg
