// Forward renderer: terrain around the camera, water, trees and players.
// One shader with sun + sky lighting, distance fog and a filmic tone curve
// gives the "realistic with stylised grading" look.
#pragma once
#include <cstdint>
#include <vector>

#include "gl.h"
#include "vecmath.h"
#include "terrain.h"

namespace sg {

struct Camera {
  Vec3 eye;
  float yaw = 0;
  float pitch = 0;
  float fovY = 70.0f * kPi / 180.0f;
};

class Renderer {
 public:
  bool init();
  void shutdown();
  void resize(int width, int height);

  void beginFrame(const Camera& cam);
  void drawWorld(const Terrain& terrain, const Vec3& around);  // terrain, trees, water
  void drawPlayer(const Vec3& feet, float yaw, const Vec3& color);

 private:
  struct Vertex {
    float px, py, pz;
    float nx, ny, nz;
    float r, g, b;
  };
  struct Mesh {
    GLuint vao = 0, vbo = 0, ebo = 0;
    GLsizei count = 0;
  };

  static void upload(Mesh& m, const std::vector<Vertex>& v, const std::vector<uint32_t>& idx);
  static void destroy(Mesh& m);
  static void addBox(std::vector<Vertex>& v, std::vector<uint32_t>& idx, const Vec3& center, const Vec3& half,
                     const Vec3& color);
  void rebuildTerrain(const Terrain& terrain, float cx, float cz);
  void draw(const Mesh& m, const Mat4& model, float specular, const Vec3& tint = {1, 1, 1});

  GLuint program_ = 0;
  GLint uViewProj_ = -1, uModel_ = -1, uCamPos_ = -1, uSpecular_ = -1, uTint_ = -1;
  Mesh terrain_, props_, water_, box_;
  bool haveTerrain_ = false;
  float terrainCx_ = 0, terrainCz_ = 0;
  uint32_t terrainSeed_ = 0;
  int width_ = 1, height_ = 1;
};

}  // namespace sg
