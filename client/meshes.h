// Procedural low-poly models: items (held and inventory icons), resource
// nodes, building pieces, deployables and grass. Flat-shaded triangles with a
// per-vertex material id that the shader turns into wood planks, stone bricks,
// metal panels, bark, rock and so on. Real art can replace these later.
#pragma once
#include <cstdint>
#include <vector>

#include "items.h"
#include "terrain.h"
#include "vecmath.h"
#include "world.h"

namespace sg {

enum Material : int {
  kMatPlain = 0,
  kMatTerrain = 1,
  kMatWater = 2,
  kMatWood = 3,
  kMatStone = 4,
  kMatMetal = 5,
  kMatTwig = 6,
  kMatLeaves = 7,
  kMatGrass = 8,
  kMatBark = 9,
  kMatRock = 10,
  kMatEmissive = 11,
  kMatCloth = 12,
  kMatSkin = 13,
  kMatLeafCard = 14,  // alpha-tested quad full of leaves (uv drives the leaf pattern)
  kMatNeedles = 15,   // alpha-tested pine branch (u along the branch)
};

struct Vertex {
  float px, py, pz;
  float nx, ny, nz;
  float r, g, b;
  float mat;   // Material
  float sway;  // 0 = rooted, 1 = moves fully in the wind
  float u = 0, v = 0;
};

class MeshBuilder {
 public:
  std::vector<Vertex> v;
  std::vector<uint32_t> idx;
  Vec3 bmin{1e9f, 1e9f, 1e9f}, bmax{-1e9f, -1e9f, -1e9f};

  // Counter-clockwise seen from outside; flat normal.
  void tri(const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& color, int mat, float sa = 0, float sb = 0,
           float sc = 0);
  void quad(const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& d, const Vec3& color, int mat);
  // Unit cube (-0.5..0.5) transformed by m. Sway grows from the bottom (0) to the top (sway).
  void box(const Mat4& m, const Vec3& color, int mat, float sway = 0);
  void box(const Vec3& center, const Vec3& half, const Vec3& color, int mat, float sway = 0);
  // Cylinder/cone along local +Y from 0 to 1, radius r0 at the bottom and r1 at the top.
  void cylinder(const Mat4& m, int segments, float r0, float r1, const Vec3& color, int mat, float swayTop = 0,
                bool caps = true);
  // Lumpy sphere (subdivided icosahedron with noise), radius 1 before m.
  // smoothAngle > 0 averages normals across edges sharper than that (degrees): round, not faceted.
  void blob(const Mat4& m, uint32_t seed, float roughness, const Vec3& color, int mat, int subdiv = 1,
            float sway = 0, float colorJitter = 0.08f, float smoothAngle = 60.0f, int octaves = 1);
  // Triangle with explicit per-vertex normals, sway and uv.
  void triN(const Vec3 p[3], const Vec3 n[3], const float s[3], const float uv[3][2], const Vec3& color, int mat);
  // Double-sided textured quad (corners counter-clockwise), normals bent away from `lightCenter`
  // so a clump of cards shades like one soft volume.
  void card(const Vec3 c[4], const Vec3& lightCenter, const float sway[4], const Vec3& color, int mat);
  // Smooth shading for vertices added since `first`: average normals of coincident vertices
  // whose faces meet at less than maxAngle degrees.
  void smooth(size_t first, float maxAngleDeg);
};

float noise3(float x, float y, float z);  // smooth value noise, 0..1

void buildItemModel(Item item, MeshBuilder& mb);               // item space: origin at the grip, -Z forward

// Where attachments sit on each gun (item space). Attachment models have their
// origin at the mount: sights sit on top (+Y), muzzle devices extend along -Z, grips hang below.
struct GunMounts {
  Vec3 sight, muzzle, grip;
};
GunMounts gunMounts(Item gun);
float sightEyeHeight(Item attachment);  // eye line above the sight mount when aiming through it
// World space. detailed = close to the player: full leaves; far away: cheaper canopies.
void buildNode(const ResourceNode& n, MeshBuilder& mb, bool detailed = true);
void buildObject(const WorldObject& o, MeshBuilder& mb);       // world space
void buildGrass(const Terrain& t, float cx, float cz, float radius, MeshBuilder& mb);
// Albedo for terrain at height h with normal n (shared by the terrain mesh and grass).
Vec3 groundColor(float h, const Vec3& n, float variation);

}  // namespace sg
