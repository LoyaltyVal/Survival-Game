// Forward renderer with sun shadows, a procedural sky, animated water and
// per-material surface detail (wood planks, stone bricks, metal panels, bark,
// rock, foliage). Draw calls are queued during the frame and executed in
// endFrame(): shadow map -> sky -> opaque -> water -> previews -> view model.
#pragma once
#include <array>
#include <cstdint>
#include <unordered_set>
#include <vector>

#include "gl.h"
#include "items.h"
#include "meshes.h"
#include "terrain.h"
#include "vecmath.h"
#include "world.h"

namespace sg {

struct Camera {
  Vec3 eye;
  float yaw = 0;
  float pitch = 0;
  float fovY = 70.0f * kPi / 180.0f;
};

struct PlayerLook {
  Vec3 shirt{0.3f, 0.1f, 0.05f};
  Item held = Item::None;
  bool dead = false;
  bool attacking = false;
  float walkPhase = 0;   // radians, advances with distance walked
  float walkAmount = 0;  // 0 standing .. 1 running
};

class Renderer {
 public:
  bool init();
  void shutdown();
  void resize(int width, int height);

  // time: seconds (animation); timeOfDay: hours 0..24 (sun, moon, sky colours).
  void beginFrame(const Camera& cam, float time, float timeOfDay);
  // Terrain, grass, water and resource nodes (skipping depleted ones).
  void drawWorld(const Terrain& terrain, const Vec3& around, const std::unordered_set<uint32_t>& depleted,
                 uint32_t nodesVersion);
  void drawObjects(const World& world);
  void drawGhost(const WorldObject& o, bool valid);  // building preview
  void drawPlayer(const Vec3& feet, float yaw, float pitch, const PlayerLook& look);
  // First-person arms and held item. swing is 0..1 through an attack.
  void drawViewModel(Item held, float swing, bool muzzleFlash, float bobPhase, float bobAmount);
  void endFrame();

  // World position -> normalized screen position (0..1, y down). False if behind the camera.
  bool project(const Vec3& p, float& sx, float& sy) const;
  // 3D-rendered inventory icon for an item (OpenGL texture id, 0 if none).
  unsigned iconTexture(Item item) const { return icons_[static_cast<size_t>(item)]; }

 private:
  struct Mesh {
    GLuint vao = 0, vbo = 0, ebo = 0;
    GLsizei count = 0;
  };
  struct DrawCmd {
    const Mesh* mesh;
    Mat4 model;
    Vec3 tint;
    float alpha;
    bool castShadow;
  };

  static void upload(Mesh& m, const MeshBuilder& mb);
  static void destroy(Mesh& m);
  void rebuildTerrain(const Terrain& terrain, float cx, float cz);
  void rebuildProps(const Terrain& terrain, const std::unordered_set<uint32_t>& depleted);
  void renderItemIcons();
  void setCommonUniforms(GLuint program, int mode, const Mat4& viewProj, const Vec3& camPos);
  void drawCmd(const DrawCmd& c);
  void box(std::vector<DrawCmd>& list, const Mat4& m, const Vec3& color, float alpha = 1.0f, bool shadow = true);

  GLuint program_ = 0, shadowProgram_ = 0, skyProgram_ = 0;
  GLint uModel_ = -1, uTint_ = -1, uAlpha_ = -1;
  GLint uShadowModel_ = -1, uShadowLightVP_ = -1, uShadowTime_ = -1;
  GLuint skyVao_ = 0;
  GLuint shadowFbo_ = 0, shadowTex_ = 0;
  static constexpr int kShadowSize = 2048;
  static constexpr float kShadowRadius = 70.0f;  // metres around the camera that get sun shadows
  Mat4 lightVP_;
  bool shadowsOn_ = false;

  struct Lighting {
    Vec3 lightDir{0, 1, 0};   // sun by day, moon by night (also casts the shadows)
    Vec3 lightColor;
    Vec3 sunPos, moonPos;     // where to draw them in the sky
    Vec3 skyAmb, groundAmb, zenith, horizon;
    float night = 0;          // 0 day .. 1 night
    float exposure = 1;
  };
  static Lighting lightingFor(float hours);
  void setLightingUniforms(GLuint program);

  Mesh terrain_, props_, grass_, water_, box_, objects_, blob_, sphere_;
  GLuint heightTex_ = 0;         // terrain heights around the camera, for shore foam
  float heightOriginX_ = 0, heightOriginZ_ = 0, heightExtent_ = 1;
  Lighting light_;
  std::array<Mesh, kItemCount> items_{};
  std::array<GLuint, kItemCount> icons_{};
  bool haveTerrain_ = false;
  float terrainCx_ = 0, terrainCz_ = 0;
  uint32_t terrainSeed_ = 0;
  uint32_t propsVersion_ = 0;
  uint32_t objectsVersion_ = 0;

  std::vector<DrawCmd> opaque_, transparent_, viewModel_;
  bool drawWater_ = false;
  Camera cam_;
  float time_ = 0;
  Mat4 viewProj_;
  int width_ = 1, height_ = 1;
};

}  // namespace sg
