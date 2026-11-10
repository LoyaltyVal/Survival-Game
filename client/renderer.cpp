#include "renderer.h"

#include <cmath>
#include <cstddef>
#include <cstdio>

namespace sg {

// Terrain window drawn around the player. Fog hides the edge.
constexpr int kGridCells = 384;         // per side
constexpr float kCellSize = 2.0f;       // metres
constexpr float kRebuildDistance = 96;  // move this far before re-meshing

static const char* kVertexShader = R"(#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec3 aColor;
uniform mat4 uViewProj;
uniform mat4 uModel;
out vec3 vWorld;
out vec3 vNormal;
out vec3 vColor;
void main() {
  vec4 world = uModel * vec4(aPos, 1.0);
  vWorld = world.xyz;
  vNormal = mat3(uModel) * aNormal;
  vColor = aColor;
  gl_Position = uViewProj * world;
}
)";

static const char* kFragmentShader = R"(#version 330 core
in vec3 vWorld;
in vec3 vNormal;
in vec3 vColor;
uniform vec3 uCamPos;
uniform float uSpecular;
uniform vec3 uTint;
out vec4 fragColor;

const vec3 kSunDir   = normalize(vec3(0.45, 0.65, 0.35));
const vec3 kSunColor = vec3(3.0, 2.75, 2.35);   // warm late-afternoon sun
const vec3 kSky      = vec3(0.32, 0.45, 0.65);  // ambient from above
const vec3 kGround   = vec3(0.16, 0.13, 0.10);  // bounce from below
const vec3 kFog      = vec3(0.55, 0.66, 0.80);

// ACES-style filmic curve: soft highlights, rich mid-tones.
vec3 tonemap(vec3 x) {
  return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

void main() {
  vec3 n = normalize(vNormal);
  vec3 v = normalize(uCamPos - vWorld);
  float diffuse = max(dot(n, kSunDir), 0.0);
  vec3 ambient = mix(kGround, kSky, n.y * 0.5 + 0.5);
  vec3 color = vColor * uTint * (kSunColor * diffuse + ambient);
  float spec = pow(max(dot(reflect(-kSunDir, n), v), 0.0), 80.0) * uSpecular;
  color += kSunColor * spec;

  float dist = length(uCamPos - vWorld);
  float fog = 1.0 - exp(-pow(dist / 420.0, 2.0));
  color = mix(color, kFog, clamp(fog, 0.0, 1.0));

  color = tonemap(color * 0.9);
  fragColor = vec4(pow(color, vec3(1.0 / 2.2)), 1.0);
}
)";

static GLuint compile(GLenum type, const char* src) {
  GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &src, nullptr);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[2048];
    glGetShaderInfoLog(s, sizeof(log), nullptr, log);
    std::fprintf(stderr, "[gl] shader compile error:\n%s\n", log);
    glDeleteShader(s);
    return 0;
  }
  return s;
}

bool Renderer::init() {
  GLuint vs = compile(GL_VERTEX_SHADER, kVertexShader);
  GLuint fs = compile(GL_FRAGMENT_SHADER, kFragmentShader);
  if (!vs || !fs) return false;
  program_ = glCreateProgram();
  glAttachShader(program_, vs);
  glAttachShader(program_, fs);
  glLinkProgram(program_);
  glDeleteShader(vs);
  glDeleteShader(fs);
  GLint ok = 0;
  glGetProgramiv(program_, GL_LINK_STATUS, &ok);
  if (!ok) {
    char log[2048];
    glGetProgramInfoLog(program_, sizeof(log), nullptr, log);
    std::fprintf(stderr, "[gl] link error:\n%s\n", log);
    return false;
  }
  uViewProj_ = glGetUniformLocation(program_, "uViewProj");
  uModel_ = glGetUniformLocation(program_, "uModel");
  uCamPos_ = glGetUniformLocation(program_, "uCamPos");
  uSpecular_ = glGetUniformLocation(program_, "uSpecular");
  uTint_ = glGetUniformLocation(program_, "uTint");

  // Unit box (players), centred at origin, size 1.
  {
    std::vector<Vertex> v;
    std::vector<uint32_t> idx;
    addBox(v, idx, {0, 0, 0}, {0.5f, 0.5f, 0.5f}, {1, 1, 1});
    upload(box_, v, idx);
  }
  // Sea: one big quad at sea level reaching past the island.
  {
    const float lo = -2000.0f, hi = kWorldSize + 2000.0f, y = kSeaLevel - 0.15f;
    const Vec3 c{0.03f, 0.10f, 0.16f};
    std::vector<Vertex> v = {{lo, y, lo, 0, 1, 0, c.x, c.y, c.z}, {hi, y, lo, 0, 1, 0, c.x, c.y, c.z},
                             {hi, y, hi, 0, 1, 0, c.x, c.y, c.z}, {lo, y, hi, 0, 1, 0, c.x, c.y, c.z}};
    upload(water_, v, {0, 3, 2, 0, 2, 1});
  }

  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_LESS);
  glEnable(GL_CULL_FACE);
  glCullFace(GL_BACK);
  glEnable(GL_MULTISAMPLE);
  return true;
}

void Renderer::shutdown() {
  destroy(terrain_);
  destroy(props_);
  destroy(water_);
  destroy(box_);
  if (program_) glDeleteProgram(program_);
  program_ = 0;
}

void Renderer::resize(int width, int height) {
  width_ = width > 0 ? width : 1;
  height_ = height > 0 ? height : 1;
  glViewport(0, 0, width_, height_);
}

void Renderer::upload(Mesh& m, const std::vector<Vertex>& v, const std::vector<uint32_t>& idx) {
  if (!m.vao) {
    glGenVertexArrays(1, &m.vao);
    glGenBuffers(1, &m.vbo);
    glGenBuffers(1, &m.ebo);
  }
  glBindVertexArray(m.vao);
  glBindBuffer(GL_ARRAY_BUFFER, m.vbo);
  glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(v.size() * sizeof(Vertex)), v.data(), GL_STATIC_DRAW);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m.ebo);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(idx.size() * sizeof(uint32_t)), idx.data(),
               GL_STATIC_DRAW);
  const GLsizei stride = sizeof(Vertex);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(offsetof(Vertex, px)));
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(offsetof(Vertex, nx)));
  glEnableVertexAttribArray(2);
  glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(offsetof(Vertex, r)));
  glBindVertexArray(0);
  m.count = static_cast<GLsizei>(idx.size());
}

void Renderer::destroy(Mesh& m) {
  if (m.vao) glDeleteVertexArrays(1, &m.vao);
  if (m.vbo) glDeleteBuffers(1, &m.vbo);
  if (m.ebo) glDeleteBuffers(1, &m.ebo);
  m = {};
}

void Renderer::addBox(std::vector<Vertex>& v, std::vector<uint32_t>& idx, const Vec3& c, const Vec3& h,
                      const Vec3& col) {
  // 6 faces, 4 vertices each so every face gets a flat normal.
  const Vec3 normals[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
  for (const Vec3& n : normals) {
    // Two axes spanning the face, ordered so the winding is counter-clockwise from outside.
    Vec3 u = std::fabs(n.y) > 0.5f ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
    Vec3 w = cross(n, u);
    Vec3 fc{c.x + n.x * h.x, c.y + n.y * h.y, c.z + n.z * h.z};
    Vec3 du{u.x * h.x, u.y * h.y, u.z * h.z}, dw{w.x * h.x, w.y * h.y, w.z * h.z};
    auto base = static_cast<uint32_t>(v.size());
    Vec3 corners[4] = {fc - du - dw, fc + du - dw, fc + du + dw, fc - du + dw};
    for (const Vec3& p : corners) v.push_back({p.x, p.y, p.z, n.x, n.y, n.z, col.x, col.y, col.z});
    // cross(u, w) == n, so this order is counter-clockwise seen from outside.
    idx.insert(idx.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
  }
}

// Linear-space albedo by height and slope: sand, grass, rock, snow.
static Vec3 groundColor(float h, const Vec3& n, float noise) {
  const Vec3 sand{0.62f, 0.52f, 0.36f}, grass{0.16f, 0.26f, 0.07f}, dryGrass{0.30f, 0.30f, 0.12f};
  const Vec3 rock{0.28f, 0.26f, 0.24f}, snow{0.85f, 0.87f, 0.90f};
  Vec3 c = lerp(grass, dryGrass, noise);
  if (h < 1.8f) c = lerp(sand, c, clampf((h - 0.8f) / 1.0f, 0, 1));
  if (h > 62.0f) c = lerp(c, snow, clampf((h - 62.0f) / 6.0f, 0, 1));
  float steep = clampf((0.82f - n.y) / 0.12f, 0, 1);
  return lerp(c, rock, steep);
}

static float hash2(int x, int z) {
  uint32_t h = static_cast<uint32_t>(x) * 73856093u ^ static_cast<uint32_t>(z) * 19349663u;
  h = (h ^ (h >> 13)) * 1274126177u;
  return static_cast<float>((h ^ (h >> 16)) & 0xFFFF) / 65535.0f;
}

void Renderer::rebuildTerrain(const Terrain& terrain, float cx, float cz) {
  const int n = kGridCells + 1;
  const float half = kGridCells * kCellSize * 0.5f;
  // Snap to the grid so vertices land on the same spots after a rebuild (no swimming).
  float ox = std::floor((cx - half) / kCellSize) * kCellSize;
  float oz = std::floor((cz - half) / kCellSize) * kCellSize;

  std::vector<Vertex> v;
  v.reserve(static_cast<size_t>(n) * n);
  for (int j = 0; j < n; ++j)
    for (int i = 0; i < n; ++i) {
      float x = ox + i * kCellSize, z = oz + j * kCellSize;
      float h = terrain.heightAt(x, z);
      Vec3 nrm = terrain.normalAt(x, z);
      float variation = hash2(static_cast<int>(x / 8), static_cast<int>(z / 8)) * 0.5f +
                        hash2(static_cast<int>(x / 2), static_cast<int>(z / 2)) * 0.15f;
      Vec3 c = groundColor(h, nrm, clampf(variation, 0, 1));
      v.push_back({x, h, z, nrm.x, nrm.y, nrm.z, c.x, c.y, c.z});
    }
  std::vector<uint32_t> idx;
  idx.reserve(static_cast<size_t>(kGridCells) * kGridCells * 6);
  for (int j = 0; j < kGridCells; ++j)
    for (int i = 0; i < kGridCells; ++i) {
      auto a = static_cast<uint32_t>(j * n + i), b = a + 1, c = a + n, d = c + 1;
      idx.insert(idx.end(), {a, c, b, b, c, d});
    }
  upload(terrain_, v, idx);

  // Trees: deterministic per 6 m cell, only on flat grass. Stylised blocky
  // shapes for now; real meshes come with the art pass.
  std::vector<Vertex> pv;
  std::vector<uint32_t> pi;
  const float cell = 6.0f;
  int x0 = static_cast<int>(std::floor(ox / cell)), z0 = static_cast<int>(std::floor(oz / cell));
  int cells = static_cast<int>(kGridCells * kCellSize / cell);
  for (int j = 0; j < cells; ++j)
    for (int i = 0; i < cells; ++i) {
      int gx = x0 + i, gz = z0 + j;
      if (hash2(gx, gz) > 0.07f) continue;
      float x = (gx + hash2(gz, gx)) * cell, z = (gz + hash2(gx + 7, gz - 3)) * cell;
      float h = terrain.heightAt(x, z);
      if (h < 2.5f || h > 55.0f || terrain.normalAt(x, z).y < 0.88f) continue;
      float s = 0.8f + hash2(gx * 3, gz * 5) * 0.6f;  // size variation
      addBox(pv, pi, {x, h + 2.0f * s, z}, {0.22f * s, 2.0f * s, 0.22f * s}, {0.20f, 0.12f, 0.06f});
      Vec3 leaf = lerp(Vec3{0.05f, 0.14f, 0.04f}, Vec3{0.10f, 0.18f, 0.05f}, hash2(gx, gz * 2));
      addBox(pv, pi, {x, h + 4.6f * s, z}, {1.5f * s, 1.3f * s, 1.5f * s}, leaf);
      addBox(pv, pi, {x, h + 6.4f * s, z}, {0.9f * s, 0.8f * s, 0.9f * s}, leaf * 1.15f);
    }
  upload(props_, pv, pi);

  terrainCx_ = cx;
  terrainCz_ = cz;
  terrainSeed_ = terrain.seed();
  haveTerrain_ = true;
}

void Renderer::beginFrame(const Camera& cam) {
  glClearColor(0.62f, 0.72f, 0.84f, 1.0f);  // matches the fog after tonemapping, roughly
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  glUseProgram(program_);

  Vec3 dir = directionFromYawPitch(cam.yaw, cam.pitch);
  Mat4 view = Mat4::lookAt(cam.eye, cam.eye + dir, {0, 1, 0});
  Mat4 proj = Mat4::perspective(cam.fovY, static_cast<float>(width_) / height_, 0.1f, 1500.0f);
  Mat4 vp = proj * view;
  glUniformMatrix4fv(uViewProj_, 1, GL_FALSE, vp.m);
  glUniform3f(uCamPos_, cam.eye.x, cam.eye.y, cam.eye.z);
}

void Renderer::draw(const Mesh& m, const Mat4& model, float specular, const Vec3& tint) {
  if (!m.count) return;
  glUniform3f(uTint_, tint.x, tint.y, tint.z);
  glUniformMatrix4fv(uModel_, 1, GL_FALSE, model.m);
  glUniform1f(uSpecular_, specular);
  glBindVertexArray(m.vao);
  glDrawElements(GL_TRIANGLES, m.count, GL_UNSIGNED_INT, nullptr);
}

void Renderer::drawWorld(const Terrain& terrain, const Vec3& around) {
  float dx = around.x - terrainCx_, dz = around.z - terrainCz_;
  if (!haveTerrain_ || terrain.seed() != terrainSeed_ || dx * dx + dz * dz > kRebuildDistance * kRebuildDistance)
    rebuildTerrain(terrain, around.x, around.z);
  draw(terrain_, Mat4::identity(), 0.0f);
  draw(props_, Mat4::identity(), 0.0f);
  draw(water_, Mat4::identity(), 0.6f);
}

void Renderer::drawPlayer(const Vec3& feet, float yaw, const Vec3& color) {
  // Body: 0.6 x 1.8 x 0.4 box; a dark visor shows which way they face.
  // TODO(art pass): skinned character mesh + animation.
  Mat4 base = Mat4::translate(feet) * Mat4::rotateY(yaw);
  Mat4 body = base * Mat4::translate({0, 0.9f, 0}) * Mat4::scale({0.6f, 1.8f, 0.4f});
  Mat4 visor = base * Mat4::translate({0, 1.55f, -0.22f}) * Mat4::scale({0.4f, 0.15f, 0.08f});
  draw(box_, body, 0.1f, color);
  draw(box_, visor, 0.8f, {0.02f, 0.02f, 0.03f});
}

}  // namespace sg
