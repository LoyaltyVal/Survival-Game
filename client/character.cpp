#include "character.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

namespace sg {

namespace {

Vec3 xf(const Mat4& m, const Vec3& p) {
  return {m.at(0, 0) * p.x + m.at(0, 1) * p.y + m.at(0, 2) * p.z + m.at(0, 3),
          m.at(1, 0) * p.x + m.at(1, 1) * p.y + m.at(1, 2) * p.z + m.at(1, 3),
          m.at(2, 0) * p.x + m.at(2, 1) * p.y + m.at(2, 2) * p.z + m.at(2, 3)};
}

// Triangle facing away from `inside` (so winding never has to be worked out by hand).
void triOut(MeshBuilder& mb, Vec3 a, Vec3 b, Vec3 c, const Vec3& inside, const Vec3& col, int mat) {
  Vec3 n = cross(b - a, c - a);
  if (dot(n, n) < 1e-14f) return;
  if (dot(n, (a + b + c) * (1.0f / 3.0f) - inside) < 0) std::swap(b, c);
  mb.tri(a, b, c, col, mat);
}

// Cross-section of a limb or the torso at height y: an ellipse (half width x, half depth z).
struct Ring {
  float y, hw, hd, x = 0, z = 0;
};
using Rings = std::vector<Ring>;

// Ring interpolated at height y (rings run top to bottom).
Ring ringAt(const Rings& r, float y) {
  if (y >= r.front().y) return r.front();
  for (size_t i = 1; i < r.size(); ++i)
    if (y >= r[i].y) {
      float t = (r[i - 1].y - y) / (r[i - 1].y - r[i].y);
      auto L = [&](float a, float b) { return a + (b - a) * t; };
      return {y, L(r[i - 1].hw, r[i].hw), L(r[i - 1].hd, r[i].hd), L(r[i - 1].x, r[i].x), L(r[i - 1].z, r[i].z)};
    }
  return r.back();
}

// The part of a ring list between two heights (top > bottom).
Rings slice(const Rings& r, float top, float bottom) {
  Rings out{ringAt(r, top)};
  for (const auto& x : r)
    if (x.y < top && x.y > bottom) out.push_back(x);
  out.push_back(ringAt(r, bottom));
  return out;
}

// Smooth tube through the rings. a0..a1 limits it to an arc (plates); angle -pi/2 is the front (-Z).
// grow pushes the surface outwards (clothes and armour over the body).
void loft(MeshBuilder& mb, const Mat4& m, const Rings& rings, const Vec3& col, int mat, float grow = 0,
          float a0 = -kPi, float a1 = kPi, bool caps = true, int segs = 16, float power = 1.0f) {
  const size_t first = mb.v.size();
  const bool full = a1 - a0 > 6.2f;
  auto pt = [&](const Ring& r, float a) {
    float c = std::cos(a), s = std::sin(a);
    float cx = std::copysign(std::pow(std::fabs(c), power), c), sz = std::copysign(std::pow(std::fabs(s), power), s);
    return xf(m, {r.x + (r.hw + grow) * cx, r.y, r.z + (r.hd + grow) * sz});
  };
  auto axis = [&](const Ring& r) { return xf(m, {r.x, r.y, r.z}); };
  for (size_t i = 0; i + 1 < rings.size(); ++i) {
    const Vec3 inside = (axis(rings[i]) + axis(rings[i + 1])) * 0.5f;
    for (int j = 0; j < segs; ++j) {
      float aj = a0 + (a1 - a0) * static_cast<float>(j) / static_cast<float>(segs);
      float ak = a0 + (a1 - a0) * static_cast<float>(j + 1) / static_cast<float>(segs);
      Vec3 p00 = pt(rings[i], aj), p01 = pt(rings[i], ak), p10 = pt(rings[i + 1], aj), p11 = pt(rings[i + 1], ak);
      triOut(mb, p00, p10, p11, inside, col, mat);
      triOut(mb, p00, p11, p01, inside, col, mat);
    }
  }
  if (caps && full && rings.size() >= 2) {
    for (int end = 0; end < 2; ++end) {
      const Ring& r = end == 0 ? rings.front() : rings.back();
      const Ring& next = end == 0 ? rings[1] : rings[rings.size() - 2];
      Vec3 c = axis(r), inside = axis(next);
      for (int j = 0; j < segs; ++j) {
        float aj = a0 + (a1 - a0) * static_cast<float>(j) / static_cast<float>(segs);
        float ak = a0 + (a1 - a0) * static_cast<float>(j + 1) / static_cast<float>(segs);
        triOut(mb, c, pt(r, aj), pt(r, ak), inside, col, mat);
      }
    }
  }
  mb.smooth(first, 72.0f);
}

using Keep = std::function<bool(const Vec3&)>;  // direction from the centre (unit sphere) -> keep this patch?

// Smooth ellipsoid, or just the patches `keep` accepts (hair, hoods, helmets, plates).
void ellipsoid(MeshBuilder& mb, const Mat4& m, const Vec3& c, const Vec3& r, const Vec3& col, int mat,
               const Keep& keep = nullptr, int segU = 20, int segV = 12) {
  const size_t first = mb.v.size();
  const Vec3 inside = xf(m, c);
  auto dirAt = [](float th, float ph) { return Vec3{std::sin(th) * std::cos(ph), std::cos(th), std::sin(th) * std::sin(ph)}; };
  for (int iv = 0; iv < segV; ++iv) {
    float t0 = kPi * static_cast<float>(iv) / static_cast<float>(segV), t1 = kPi * static_cast<float>(iv + 1) / static_cast<float>(segV);
    for (int iu = 0; iu < segU; ++iu) {
      float p0 = 2 * kPi * static_cast<float>(iu) / static_cast<float>(segU);
      float p1 = 2 * kPi * static_cast<float>(iu + 1) / static_cast<float>(segU);
      if (keep && !keep(dirAt((t0 + t1) * 0.5f, (p0 + p1) * 0.5f))) continue;
      auto P = [&](float th, float ph) {
        Vec3 d = dirAt(th, ph);
        return xf(m, {c.x + r.x * d.x, c.y + r.y * d.y, c.z + r.z * d.z});
      };
      Vec3 a = P(t0, p0), b = P(t0, p1), cc = P(t1, p1), d = P(t1, p0);
      triOut(mb, a, d, cc, inside, col, mat);
      triOut(mb, a, cc, b, inside, col, mat);
    }
  }
  mb.smooth(first, 80.0f);
}

// --- body shapes (bone space) ----------------------------------------------------------
const Rings& torsoRings() {
  static const Rings r = {{0.47f, 0.09f, 0.06f},          {0.44f, 0.16f, 0.085f},         {0.40f, 0.19f, 0.10f},
                          {0.34f, 0.185f, 0.11f, 0, -0.01f}, {0.26f, 0.172f, 0.108f, 0, -0.012f}, {0.16f, 0.152f, 0.097f, 0, -0.005f},
                          {0.06f, 0.14f, 0.09f},          {-0.02f, 0.145f, 0.092f}};
  return r;
}
const Rings& pelvisRings() {
  static const Rings r = {{0.03f, 0.147f, 0.094f}, {-0.05f, 0.156f, 0.099f}, {-0.11f, 0.152f, 0.096f}, {-0.16f, 0.11f, 0.075f}};
  return r;
}
const Rings& thighRings() {
  static const Rings r = {{0.03f, 0.088f, 0.09f}, {-0.1f, 0.085f, 0.087f}, {-0.25f, 0.072f, 0.074f}, {-0.38f, 0.058f, 0.06f},
                          {-0.44f, 0.053f, 0.055f}};
  return r;
}
const Rings& shinRings() {
  static const Rings r = {{0.03f, 0.056f, 0.058f},          {-0.07f, 0.058f, 0.062f, 0, 0.008f}, {-0.16f, 0.055f, 0.058f, 0, 0.01f},
                          {-0.30f, 0.044f, 0.046f},         {-0.40f, 0.038f, 0.04f},            {-0.44f, 0.036f, 0.038f}};
  return r;
}
const Rings& upperArmRings() {
  static const Rings r = {{-0.02f, 0.058f, 0.058f}, {-0.12f, 0.052f, 0.054f, 0, -0.004f}, {-0.22f, 0.045f, 0.046f}, {-0.29f, 0.041f, 0.041f}};
  return r;
}
const Rings& forearmRings() {
  static const Rings r = {{0.015f, 0.043f, 0.043f}, {-0.06f, 0.047f, 0.044f}, {-0.16f, 0.038f, 0.034f}, {-0.25f, 0.03f, 0.025f}};
  return r;
}

const Vec3 kSkullC{0, 0.2f, 0.008f}, kSkullR{0.094f, 0.112f, 0.106f};
const Vec3 kWhite{1, 1, 1};

// Colours of the clothes and armour.
const Vec3 kBurlap{0.33f, 0.26f, 0.16f}, kRope{0.42f, 0.34f, 0.20f}, kWoodArmor{0.30f, 0.19f, 0.09f};
const Vec3 kSteelArmor{0.30f, 0.31f, 0.33f}, kLeather{0.17f, 0.10f, 0.05f}, kDarkCloth{0.07f, 0.07f, 0.075f};
const Vec3 kSole{0.05f, 0.045f, 0.04f};

void shoe(MeshBuilder& mb, const Mat4& m, const Vec3& upper, const Vec3& sole, float grow, int mat) {
  ellipsoid(mb, m, {0, -0.035f, -0.035f}, {0.05f + grow, 0.05f + grow * 0.6f, 0.115f + grow}, upper, mat,
            [](const Vec3& d) { return d.y > -0.5f; });
  mb.box(m * Mat4::translate({0, -0.072f, -0.035f}) * Mat4::scale({0.104f + grow * 2, 0.018f, 0.245f + grow * 2}), sole, kMatPlain);
  loft(mb, m, {{0.01f, 0.045f + grow, 0.047f + grow}, {-0.04f, 0.05f + grow, 0.06f + grow}}, upper, mat);
}

void strap(MeshBuilder& mb, const Mat4& m, const Rings& rings, float y, float grow, const Vec3& col) {
  loft(mb, m, {ringAt(rings, y + 0.015f), ringAt(rings, y - 0.015f)}, col, kMatCloth, grow);
}

}  // namespace

BodyPart bodyMeshPart(BodyMesh m) {
  switch (m) {
    case BodyMesh::HeadSkin: case BodyMesh::Face: case BodyMesh::Brows: case BodyMesh::HairShort:
    case BodyMesh::HairBuzz: case BodyMesh::HairLong: case BodyMesh::Beard:
      return BodyPart::Head;
    case BodyMesh::UpperArmSkin: case BodyMesh::Sleeve: return BodyPart::UpperArm;
    case BodyMesh::ForearmSkin: return BodyPart::Forearm;
    case BodyMesh::HandSkin: return BodyPart::Hand;
    case BodyMesh::Torso: return BodyPart::Torso;
    case BodyMesh::Pelvis: case BodyMesh::Belt: return BodyPart::Pelvis;
    case BodyMesh::Thigh: return BodyPart::Thigh;
    case BodyMesh::Shin: return BodyPart::Shin;
    case BodyMesh::Shoe: return BodyPart::Foot;
    default: return BodyPart::Torso;
  }
}

void buildBodyMesh(BodyMesh which, const Mat4& m, MeshBuilder& mb) {
  using M = Mat4;
  switch (which) {
    case BodyMesh::HeadSkin:
      loft(mb, m, {{0.1f, 0.05f, 0.05f, 0, 0.01f}, {0.06f, 0.05f, 0.048f}, {-0.03f, 0.056f, 0.052f}}, kWhite, kMatSkin);  // neck
      ellipsoid(mb, m, kSkullC, kSkullR, kWhite, kMatSkin);
      ellipsoid(mb, m, {0, 0.125f, -0.025f}, {0.072f, 0.06f, 0.078f}, kWhite, kMatSkin);   // jaw
      ellipsoid(mb, m, {0, 0.095f, -0.072f}, {0.032f, 0.025f, 0.025f}, kWhite, kMatSkin);  // chin
      ellipsoid(mb, m, {0, 0.17f, -0.103f}, {0.016f, 0.03f, 0.02f}, kWhite, kMatSkin, nullptr, 10, 6);  // nose
      ellipsoid(mb, m, {0, 0.155f, -0.115f}, {0.014f, 0.014f, 0.014f}, kWhite, kMatSkin, nullptr, 10, 6);
      for (float s : {-1.0f, 1.0f})
        ellipsoid(mb, m, {s * 0.093f, 0.185f, 0.012f}, {0.014f, 0.028f, 0.02f}, kWhite, kMatSkin, nullptr, 10, 6);  // ears
      break;
    case BodyMesh::Face:
      for (float s : {-1.0f, 1.0f}) {
        ellipsoid(mb, m, {s * 0.034f, 0.198f, -0.093f}, {0.016f, 0.01f, 0.01f}, {0.75f, 0.73f, 0.70f}, kMatPlain, nullptr, 10, 6);
        ellipsoid(mb, m, {s * 0.034f, 0.198f, -0.101f}, {0.0075f, 0.0075f, 0.004f}, {0.05f, 0.035f, 0.02f}, kMatPlain, nullptr, 8, 5);
      }
      ellipsoid(mb, m, {0, 0.128f, -0.101f}, {0.024f, 0.005f, 0.006f}, {0.22f, 0.08f, 0.07f}, kMatPlain, nullptr, 10, 5);
      break;
    case BodyMesh::Brows:
      for (float s : {-1.0f, 1.0f})
        ellipsoid(mb, m * M::translate({s * 0.036f, 0.218f, -0.096f}) * M::rotateZ(-s * 0.12f), {0, 0, 0}, {0.022f, 0.0055f, 0.008f},
                  kWhite, kMatCloth, nullptr, 8, 5);
      break;
    case BodyMesh::HairShort:
    case BodyMesh::HairLong: {
      ellipsoid(mb, m, kSkullC, kSkullR + Vec3{0.012f, 0.012f, 0.012f}, kWhite, kMatCloth, [](const Vec3& d) {
        if (std::fabs(d.x) > 0.8f && d.y < 0.1f) return false;      // leave the ears out
        if (d.z < -0.5f && d.y < 0.62f) return false;              // face and forehead
        return d.y > -0.15f || (d.z > 0.3f && d.y > -0.5f);        // down to the nape
      });
      if (which == BodyMesh::HairLong)
        ellipsoid(mb, m, {0, 0.12f, 0.06f}, {0.098f, 0.13f, 0.066f}, kWhite, kMatCloth, [](const Vec3& d) { return d.z > -0.2f; });
      break;
    }
    case BodyMesh::HairBuzz:
      ellipsoid(mb, m, kSkullC, kSkullR + Vec3{0.004f, 0.004f, 0.004f}, kWhite, kMatCloth,
                [](const Vec3& d) { return d.y > 0.05f && !(d.z < -0.55f && d.y < 0.6f); });
      break;
    case BodyMesh::Beard:
      ellipsoid(mb, m, {0, 0.125f, -0.03f}, {0.08f, 0.068f, 0.085f}, kWhite, kMatCloth, [](const Vec3& d) {
        if (d.z < -0.8f && d.y > -0.1f && d.y < 0.2f && std::fabs(d.x) < 0.35f) return false;  // mouth
        return d.y < 0.25f && d.z < 0.15f;
      });
      ellipsoid(mb, m, {0, 0.14f, -0.104f}, {0.03f, 0.008f, 0.01f}, kWhite, kMatCloth, nullptr, 10, 5);  // moustache
      break;
    case BodyMesh::UpperArmSkin:
      ellipsoid(mb, m, {0, -0.005f, 0}, {0.064f, 0.068f, 0.064f}, kWhite, kMatSkin);  // deltoid
      loft(mb, m, upperArmRings(), kWhite, kMatSkin);
      break;
    case BodyMesh::ForearmSkin:
      ellipsoid(mb, m, {0, 0, 0}, {0.042f, 0.042f, 0.042f}, kWhite, kMatSkin, nullptr, 12, 8);  // elbow
      loft(mb, m, forearmRings(), kWhite, kMatSkin);
      break;
    case BodyMesh::HandSkin:
      loft(mb, m,
           {{0.0f, 0.02f, 0.03f}, {-0.04f, 0.022f, 0.045f}, {-0.085f, 0.02f, 0.046f}, {-0.1f, 0.019f, 0.043f},
            {-0.15f, 0.016f, 0.039f}, {-0.175f, 0.012f, 0.03f}},
           kWhite, kMatSkin, 0, -kPi, kPi, true, 12);
      loft(mb, m * M::translate({0, -0.03f, -0.035f}) * M::rotateX(0.5f), {{0, 0.013f, 0.013f}, {-0.05f, 0.011f, 0.011f}, {-0.065f, 0.008f, 0.008f}},
           kWhite, kMatSkin, 0, -kPi, kPi, true, 8);  // thumb
      break;
    case BodyMesh::Torso:
      loft(mb, m, torsoRings(), kWhite, kMatCloth, 0, -kPi, kPi, true, 18, 0.9f);
      break;
    case BodyMesh::Sleeve:  // t-shirt sleeve over the top of the arm
      ellipsoid(mb, m, {0, -0.005f, 0}, {0.075f, 0.08f, 0.075f}, kWhite, kMatCloth, [](const Vec3& d) { return d.y > -0.1f; });
      loft(mb, m, {{0.0f, 0.075f, 0.075f}, {-0.06f, 0.07f, 0.07f}, {-0.13f, 0.064f, 0.064f}}, kWhite, kMatCloth);
      break;
    case BodyMesh::Pelvis:
      loft(mb, m, pelvisRings(), kWhite, kMatCloth, 0, -kPi, kPi, true, 18, 0.9f);
      break;
    case BodyMesh::Belt:
      loft(mb, m, {{0.04f, 0.15f, 0.097f}, {0.005f, 0.152f, 0.098f}}, {0.08f, 0.05f, 0.025f}, kMatPlain, 0.006f, -kPi, kPi, true, 18, 0.9f);
      mb.box(m * M::translate({0, 0.022f, -0.106f}) * M::scale({0.045f, 0.03f, 0.008f}), {0.45f, 0.35f, 0.15f}, kMatMetal);
      break;
    case BodyMesh::Thigh:
      loft(mb, m, thighRings(), kWhite, kMatCloth);
      break;
    case BodyMesh::Shin:
      loft(mb, m, shinRings(), kWhite, kMatCloth);
      break;
    case BodyMesh::Shoe:
      shoe(mb, m, {0.10f, 0.085f, 0.075f}, {0.55f, 0.52f, 0.48f}, 0, kMatCloth);
      break;
    default:
      break;
  }
}

void buildArmorMesh(Item item, BodyPart part, const Mat4& m, MeshBuilder& mb) {
  using M = Mat4;
  const float front0 = -kPi + 0.45f, front1 = -0.45f, back0 = 0.45f, back1 = kPi - 0.45f;
  switch (item) {
    // --- head ---
    case Item::ClothHood:
      if (part != BodyPart::Head) return;
      ellipsoid(mb, m, kSkullC, kSkullR + Vec3{0.022f, 0.022f, 0.022f}, kBurlap, kMatCloth,
                [](const Vec3& d) { return d.y > -0.6f && !(d.z < -0.3f && d.y < 0.72f); });
      loft(mb, m, {{0.12f, 0.098f, 0.1f, 0, 0.02f}, {0.03f, 0.085f, 0.088f}, {-0.04f, 0.11f, 0.105f}}, kBurlap, kMatCloth, 0, -kPi, kPi, false);
      return;
    case Item::WoodHelmet:
      if (part != BodyPart::Head) return;
      ellipsoid(mb, m, kSkullC, kSkullR + Vec3{0.03f, 0.028f, 0.03f}, kWoodArmor, kMatWood, [](const Vec3& d) { return d.y > 0.2f; });
      loft(mb, m, {{0.232f, 0.128f, 0.14f}, {0.218f, 0.132f, 0.144f}}, kWoodArmor * 0.8f, kMatWood);  // rim
      strap(mb, m * M::translate({0, 0.16f, 0}) * M::rotateX(0.25f), {{0.02f, 0.1f, 0.1f}, {-0.02f, 0.1f, 0.1f}}, 0, 0.012f, kRope);
      return;
    case Item::MetalHelmet:
      if (part != BodyPart::Head) return;
      ellipsoid(mb, m, kSkullC, kSkullR + Vec3{0.026f, 0.026f, 0.026f}, kSteelArmor, kMatMetal,
                [](const Vec3& d) { return d.y > 0.14f || (d.z > -0.25f && d.y > -0.55f); });
      ellipsoid(mb, m, {0, 0.15f, -0.01f}, {0.105f, 0.1f, 0.115f}, kSteelArmor * 0.9f, kMatMetal,
                [](const Vec3& d) { return d.z < -0.3f && d.y < 0.3f && d.y > -0.8f; });  // face plate under the eye slit
      for (float s : {-1.0f, 1.0f})
        for (int k = 0; k < 2; ++k)
          ellipsoid(mb, m, {s * 0.03f, 0.12f - 0.02f * static_cast<float>(k), -0.124f}, {0.007f, 0.007f, 0.004f}, {0.02f, 0.02f, 0.02f},
                    kMatPlain, nullptr, 8, 5);  // breathing holes
      return;
    // --- chest ---
    case Item::ClothShirt:
      if (part == BodyPart::Torso) {
        loft(mb, m, torsoRings(), kBurlap, kMatCloth, 0.012f, -kPi, kPi, true, 18, 0.9f);
      } else if (part == BodyPart::UpperArm) {
        ellipsoid(mb, m, {0, -0.005f, 0}, {0.077f, 0.081f, 0.077f}, kBurlap, kMatCloth, [](const Vec3& d) { return d.y > -0.2f; });
        loft(mb, m, upperArmRings(), kBurlap, kMatCloth, 0.013f);
      } else if (part == BodyPart::Forearm) {
        loft(mb, m, slice(forearmRings(), 0.02f, -0.2f), kBurlap, kMatCloth, 0.011f);
        strap(mb, m, forearmRings(), -0.19f, 0.016f, kBurlap * 0.8f);  // cuff
      }
      return;
    case Item::WoodChestplate:
      if (part == BodyPart::Torso) {
        Rings r = slice(torsoRings(), 0.42f, 0.08f);
        loft(mb, m, r, kWoodArmor, kMatWood, 0.03f, front0, front1, false, 10, 0.9f);
        loft(mb, m, r, kWoodArmor * 0.9f, kMatWood, 0.03f, back0, back1, false, 10, 0.9f);
        strap(mb, m, torsoRings(), 0.17f, 0.038f, kRope);
        strap(mb, m, torsoRings(), 0.34f, 0.038f, kRope);
        for (float s : {-1.0f, 1.0f}) mb.box(m * M::translate({s * 0.12f, 0.445f, 0}) * M::scale({0.04f, 0.02f, 0.22f}), kRope, kMatCloth);
      }
      return;
    case Item::MetalChestplate:
      if (part == BodyPart::Torso) {
        loft(mb, m, torsoRings(), kDarkCloth, kMatCloth, 0.014f, -kPi, kPi, true, 18, 0.9f);
        Rings r = slice(torsoRings(), 0.43f, 0.02f);
        loft(mb, m, r, kSteelArmor, kMatMetal, 0.034f, -kPi + 0.35f, -0.35f, false, 12, 0.9f);
        loft(mb, m, r, kSteelArmor * 0.9f, kMatMetal, 0.034f, 0.35f, kPi - 0.35f, false, 12, 0.9f);
        strap(mb, m, torsoRings(), 0.12f, 0.04f, kLeather);
      } else if (part == BodyPart::UpperArm) {
        ellipsoid(mb, m, {0, 0, 0}, {0.088f, 0.082f, 0.088f}, kSteelArmor, kMatMetal, [](const Vec3& d) { return d.y > -0.25f; });
        ellipsoid(mb, m, {0, -0.06f, 0}, {0.078f, 0.06f, 0.078f}, kSteelArmor * 0.85f, kMatMetal,
                  [](const Vec3& d) { return d.y > -0.1f && d.y < 0.7f; });
      }
      return;
    // --- legs ---
    case Item::ClothPants:
    case Item::PlatedPants: {
      const Vec3 cloth = item == Item::ClothPants ? kBurlap * 0.92f : kDarkCloth;
      if (part == BodyPart::Pelvis) loft(mb, m, pelvisRings(), cloth, kMatCloth, 0.012f, -kPi, kPi, true, 18, 0.9f);
      if (part == BodyPart::Thigh) loft(mb, m, thighRings(), cloth, kMatCloth, 0.012f);
      if (part == BodyPart::Shin) loft(mb, m, slice(shinRings(), 0.03f, -0.38f), cloth, kMatCloth, 0.012f);
      if (item == Item::PlatedPants) {
        if (part == BodyPart::Thigh) {
          loft(mb, m, slice(thighRings(), -0.03f, -0.33f), kSteelArmor, kMatMetal, 0.03f, -kPi + 0.55f, -0.55f, false, 10);
          strap(mb, m, thighRings(), -0.2f, 0.022f, kLeather);
        }
        if (part == BodyPart::Shin) {
          ellipsoid(mb, m, {0, -0.005f, -0.052f}, {0.05f, 0.055f, 0.032f}, kSteelArmor, kMatMetal, [](const Vec3& d) { return d.z < 0.1f; });
          loft(mb, m, slice(shinRings(), -0.07f, -0.3f), kSteelArmor * 0.9f, kMatMetal, 0.03f, -kPi + 0.6f, -0.6f, false, 10);
        }
      }
      return;
    }
    case Item::WoodLegGuards:
      if (part == BodyPart::Thigh) {
        loft(mb, m, slice(thighRings(), -0.04f, -0.36f), kWoodArmor, kMatWood, 0.028f, -kPi + 0.5f, -0.5f, false, 10);
        strap(mb, m, thighRings(), -0.12f, 0.032f, kRope);
        strap(mb, m, thighRings(), -0.3f, 0.032f, kRope);
      } else if (part == BodyPart::Shin) {
        ellipsoid(mb, m, {0, 0, -0.05f}, {0.045f, 0.05f, 0.03f}, kWoodArmor * 0.85f, kMatWood, [](const Vec3& d) { return d.z < 0.1f; });
        loft(mb, m, slice(shinRings(), -0.04f, -0.34f), kWoodArmor, kMatWood, 0.03f, -kPi + 0.5f, -0.5f, false, 10);
        strap(mb, m, shinRings(), -0.1f, 0.034f, kRope);
        strap(mb, m, shinRings(), -0.28f, 0.03f, kRope);
      }
      return;
    // --- feet ---
    case Item::Boots:
    case Item::ArmoredBoots:
      if (part == BodyPart::Foot) {
        shoe(mb, m, kLeather, kSole, 0.008f, kMatCloth);
        if (item == Item::ArmoredBoots)
          ellipsoid(mb, m, {0, -0.03f, -0.1f}, {0.064f, 0.052f, 0.072f}, kSteelArmor, kMatMetal,
                    [](const Vec3& d) { return d.z < 0.1f && d.y > -0.5f; });  // steel toe cap
      } else if (part == BodyPart::Shin) {
        loft(mb, m, slice(shinRings(), -0.24f, -0.44f), kLeather, kMatCloth, 0.016f);
        strap(mb, m, shinRings(), -0.25f, 0.022f, item == Item::Boots ? kRope : kLeather * 0.7f);
        if (item == Item::ArmoredBoots)
          loft(mb, m, slice(shinRings(), -0.12f, -0.4f), kSteelArmor, kMatMetal, 0.032f, -kPi + 0.6f, -0.6f, false, 10);
      }
      return;
    default:
      return;
  }
}

void buildArmorDisplay(Item item, MeshBuilder& mb) {
  using M = Mat4;
  // An invisible body in a relaxed pose, scaled down so it can also be held.
  const M root = M::scale({0.6f, 0.6f, 0.6f}) * M::translate({0, -kWaistHeight, 0});
  const M waist = root * M::translate({0, kWaistHeight, 0});
  const ArmorSlot slot = itemDef(item).armorSlot;
  switch (slot) {
    case ArmorSlot::Head:
      buildArmorMesh(item, BodyPart::Head, waist * M::translate({0, kNeckHeight, 0}), mb);
      break;
    case ArmorSlot::Chest:
      buildArmorMesh(item, BodyPart::Torso, waist, mb);
      for (float s : {-1.0f, 1.0f}) {
        M arm = waist * M::translate({s * kShoulderX, kShoulderY, 0}) * M::rotateZ(s * 0.25f);
        buildArmorMesh(item, BodyPart::UpperArm, arm, mb);
        buildArmorMesh(item, BodyPart::Forearm, arm * M::translate({0, -kUpperArmLen, 0}), mb);
      }
      break;
    case ArmorSlot::Legs:
    case ArmorSlot::Feet:
      if (slot == ArmorSlot::Legs) buildArmorMesh(item, BodyPart::Pelvis, waist, mb);
      for (float s : {-1.0f, 1.0f}) {
        M thigh = waist * M::translate({s * kHipX, kHipY, 0}) * M::rotateZ(s * 0.06f);
        M shin = thigh * M::translate({0, -kThighLen, 0});
        buildArmorMesh(item, BodyPart::Thigh, thigh, mb);
        buildArmorMesh(item, BodyPart::Shin, shin, mb);
        buildArmorMesh(item, BodyPart::Foot, shin * M::translate({0, -kShinLen, 0}), mb);
      }
      break;
  }
}

}  // namespace sg
