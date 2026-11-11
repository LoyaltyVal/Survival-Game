#include "renderer.h"

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <string>

namespace sg {

// Terrain window drawn around the player. Fog hides the edge.
constexpr int kGridCells = 448;           // per side
constexpr float kCellSize = 2.0f;         // metres
constexpr float kRebuildDistance = 64;    // move this far before re-meshing
constexpr float kGrassRadius = 95.0f;
constexpr float kDetailRadius = 160.0f;   // full-detail trees within this distance
constexpr int kIconSize = 128;

// --- shaders ----------------------------------------------------------------------------
// Shared by every shader: lighting uniforms, noise, sky colour, tone mapping.
static const char* kCommon = R"(
uniform vec3 uLightDir;    // sun by day, moon by night
uniform vec3 uLightColor;
uniform vec3 uSunPos;
uniform vec3 uMoonPos;
uniform vec3 uSkyAmb;
uniform vec3 uGroundAmb;
uniform vec3 uZenith;
uniform vec3 uHorizon;
uniform float uNight;
uniform float uExposure;
uniform float uTime;
uniform vec2 uResolution;
uniform int uMode;  // 0 world, 1 inventory icon, 2 first-person view model

float hash13(vec3 p) {
  p = fract(p * 0.1031);
  p += dot(p, p.zyx + 31.32);
  return fract((p.x + p.y) * p.z);
}
float vnoise(vec3 p) {
  vec3 i = floor(p), f = fract(p);
  f = f * f * (3.0 - 2.0 * f);
  return mix(mix(mix(hash13(i), hash13(i + vec3(1, 0, 0)), f.x), mix(hash13(i + vec3(0, 1, 0)), hash13(i + vec3(1, 1, 0)), f.x), f.y),
             mix(mix(hash13(i + vec3(0, 0, 1)), hash13(i + vec3(1, 0, 1)), f.x), mix(hash13(i + vec3(0, 1, 1)), hash13(i + vec3(1, 1, 1)), f.x), f.y), f.z);
}
float fbm(vec3 p) {
  float s = 0.0, a = 0.5;
  for (int i = 0; i < 4; i++) { s += a * vnoise(p); p *= 2.03; a *= 0.5; }
  return s;
}
vec3 skyColor(vec3 d) {
  vec3 col = d.y > 0.0 ? mix(uHorizon, uZenith, pow(clamp(d.y, 0.0, 1.0), 0.5))
                       : mix(uHorizon, uHorizon * 0.6, clamp(-d.y * 3.0, 0.0, 1.0));
  float s = max(dot(d, uSunPos), 0.0);
  col += vec3(1.0, 0.62, 0.32) * (pow(s, 6.0) * 0.35 + pow(s, 64.0) * 0.6) * (1.0 - uNight);
  return col;
}
vec3 finish(vec3 col) {
  col *= uExposure;
  col = clamp((col * (2.51 * col + 0.03)) / (col * (2.43 * col + 0.59) + 0.14), 0.0, 1.0);  // ACES filmic
  col = pow(col, vec3(1.0 / 2.2));
  if (uMode != 1) {
    vec2 uv = gl_FragCoord.xy / uResolution;
    float vig = 1.0 - smoothstep(0.35, 1.25, length(uv - 0.5) * 1.6);
    col *= mix(0.72, 1.0, vig);
  }
  return col;
}
// Cut-out masks for foliage cards (used by the colour and shadow passes so shadows match).
float leafMask(vec2 uv, float seed) {
  vec2 g = uv * 3.0;
  vec2 id = floor(g), f = fract(g) - 0.5;
  float m = 0.0;
  for (int y = -1; y <= 1; y++)
    for (int x = -1; x <= 1; x++) {
      vec2 cid = id + vec2(x, y);
      float h = hash13(vec3(cid, seed));
      vec2 off = vec2(hash13(vec3(cid, seed + 1.0)), hash13(vec3(cid, seed + 2.0))) - 0.5;
      vec2 p = f - vec2(x, y) - off * 0.6;
      float a = h * 6.283;
      p = mat2(cos(a), -sin(a), sin(a), cos(a)) * p;
      m = max(m, 1.0 - smoothstep(0.85, 1.0, length(p / vec2(0.2, 0.4))));
    }
  return m * (1.0 - smoothstep(0.38, 0.5, length(uv - 0.5)));
}
float needleMask(vec2 uv) {
  float across = abs(uv.y - 0.5) * 2.0;
  float width = mix(1.0, 0.2, uv.x);
  if (across > width) return 0.0;
  float stem = 1.0 - smoothstep(0.04, 0.08, across);
  float needles = step(0.45, fract(uv.x * 24.0 + across * 3.5));
  return max(stem, needles);
}
)";

static const char* kVertexShader = R"(#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec3 aColor;
layout(location = 3) in vec2 aExtra;  // material, sway
layout(location = 4) in vec2 aUV;
uniform mat4 uViewProj;
uniform mat4 uModel;
uniform float uTime;
out vec3 vWorld;
out vec3 vNormal;
out vec3 vColor;
out float vMat;
out float vSway;
out vec2 vUV;
vec3 wind(vec3 w, float s) {
  if (s <= 0.0) return w;
  float t = uTime * 1.6 + w.x * 0.15 + w.z * 0.12;
  return w + vec3(sin(t), 0.0, cos(t * 0.83)) * 0.09 * s + vec3(sin(t * 3.1 + w.y), 0.0, 0.0) * 0.02 * s;
}
void main() {
  vec4 world = uModel * vec4(aPos, 1.0);
  world.xyz = wind(world.xyz, aExtra.y);
  vWorld = world.xyz;
  vNormal = mat3(uModel) * aNormal;
  vColor = aColor;
  vMat = aExtra.x;
  vSway = aExtra.y;
  vUV = aUV;
  gl_Position = uViewProj * world;
}
)";

static const char* kFragmentShader = R"(
in vec3 vWorld;
in vec3 vNormal;
in vec3 vColor;
in float vMat;
in float vSway;
in vec2 vUV;
uniform vec3 uCamPos;
uniform vec3 uTint;
uniform float uAlpha;
uniform mat4 uLightVP;
uniform sampler2DShadow uShadowMap;
uniform float uShadowOn;
uniform sampler2D uHeightMap;
uniform vec3 uHeightInfo;  // origin x, origin z, extent
out vec4 fragColor;

float shadowAt(vec3 P, vec3 n) {
  if (uShadowOn < 0.5) return 1.0;
  vec4 lp = uLightVP * vec4(P + n * 0.08, 1.0);
  vec3 c = lp.xyz / lp.w * 0.5 + 0.5;
  if (c.x <= 0.0 || c.x >= 1.0 || c.y <= 0.0 || c.y >= 1.0 || c.z >= 1.0) return 1.0;
  float s = 0.0;
  vec2 texel = vec2(1.0 / 2048.0);
  for (int x = -1; x <= 1; x++)
    for (int y = -1; y <= 1; y++) s += texture(uShadowMap, vec3(c.xy + vec2(x, y) * texel * 1.3, c.z - 0.0012));
  s /= 9.0;
  vec2 e = abs(c.xy * 2.0 - 1.0);
  return mix(s, 1.0, smoothstep(0.8, 1.0, max(e.x, e.y)));
}

vec2 faceUV(vec3 p, vec3 n) {
  vec3 a = abs(n);
  if (a.y > a.x && a.y > a.z) return p.xz;
  if (a.x > a.z) return vec2(p.z, p.y);
  return vec2(p.x, p.y);
}

// Small-scale surface relief from noise: makes flat polygons read as rough rock, soil, bark.
vec3 bump(vec3 n, vec3 P, float freq, float strength) {
  float e = 0.08 / freq;
  float c = vnoise(P * freq);
  vec3 g = vec3(vnoise((P + vec3(e, 0, 0)) * freq) - c, vnoise((P + vec3(0, e, 0)) * freq) - c, vnoise((P + vec3(0, 0, e)) * freq) - c) / e;
  g -= n * dot(g, n);
  return normalize(n - g * strength / freq);
}

vec3 applyFog(vec3 col, vec3 P) {
  if (uMode != 0) return col;
  float dist = length(uCamPos - P);
  float fog = 1.0 - exp(-pow(dist / 250.0, 2.0));
  fog += (1.0 - fog) * clamp((8.0 - P.y) / 30.0, 0.0, 1.0) * (1.0 - exp(-dist / 400.0)) * 0.6;  // low-lying haze
  vec3 fd = normalize(P - uCamPos);
  fd.y = max(fd.y, 0.0) * 0.5;
  return mix(col, skyColor(normalize(fd + vec3(0.0, 0.001, 0.0))), clamp(fog, 0.0, 1.0));
}

void main() {
  vec3 n = normalize(vNormal);
  vec3 P = vWorld;
  vec3 V = normalize(uCamPos - P);
  int m = int(vMat + 0.5);
  vec3 albedo = vColor * uTint;
  float specAmt = 0.04, shin = 16.0;
  vec2 uv = faceUV(P, n);

  if (m == 2) {  // water: waves, fresnel sky reflection, glints, colour by depth, foam on the shore
    vec2 w = P.xz;
    float t = uTime;
    vec3 wn = normalize(vec3(
        (vnoise(vec3(w * 0.25, t * 0.35)) - 0.5) * 0.35 + sin(w.x * 0.3 + t * 1.1) * 0.05 + (vnoise(vec3(w * 0.9 + 7.0, t * 0.6)) - 0.5) * 0.18,
        1.0,
        (vnoise(vec3(w * 0.25 + 19.0, t * 0.35)) - 0.5) * 0.35 + cos(w.y * 0.27 + t * 0.9) * 0.05 + (vnoise(vec3(w * 0.9 + 31.0, t * 0.6)) - 0.5) * 0.18));
    vec2 huv = (w - uHeightInfo.xy) / uHeightInfo.z;
    float ground = (huv.x > 0.0 && huv.x < 1.0 && huv.y > 0.0 && huv.y < 1.0) ? texture(uHeightMap, huv).r : -20.0;
    float depth = max(P.y - ground, 0.0);
    float fres = 0.02 + 0.98 * pow(1.0 - max(dot(wn, V), 0.0), 5.0);
    vec3 refl = skyColor(reflect(-V, wn));
    vec3 lit = uSkyAmb * 1.5 + uLightColor * 0.25;
    vec3 deep = vec3(0.010, 0.050, 0.070) * lit;
    vec3 shallow = vec3(0.06, 0.20, 0.20) * lit;
    vec3 col = mix(mix(shallow, deep, 1.0 - exp(-depth * 0.3)), refl, fres);
    float sh = shadowAt(P, vec3(0.0, 1.0, 0.0));
    col += uLightColor * pow(max(dot(reflect(-uLightDir, wn), V), 0.0), 300.0) * 3.0 * sh;
    float alpha = clamp(0.35 + depth * 0.22, 0.35, 0.95);
    alpha = mix(alpha, 1.0, fres * 0.6);
    // Foam where waves meet the beach.
    float wave = sin(depth * 6.0 - t * 1.8 + vnoise(vec3(w * 0.4, t * 0.2)) * 4.0) * 0.5 + 0.5;
    float foam = (1.0 - smoothstep(0.0, 0.8, depth)) * smoothstep(0.35, 0.75, vnoise(vec3(w * 2.2, t * 0.7)) * 0.6 + wave * 0.6);
    col = mix(col, vec3(0.85) * (uSkyAmb + uLightColor * max(uLightDir.y, 0.0)), foam * 0.85);
    alpha = max(alpha, foam);
    fragColor = vec4(finish(applyFog(col, P)), alpha);
    return;
  }

  if (m == 14) {  // leaf card
    if (leafMask(vUV, floor(vColor.r * 4000.0)) < 0.5) discard;
    albedo *= 0.75 + 0.5 * hash13(floor(vec3(vUV * 3.0, vColor.r * 4000.0)));
    specAmt = 0.05;
  } else if (m == 15) {  // pine needles
    if (needleMask(vUV) < 0.5) discard;
    albedo *= 0.8 + 0.4 * vnoise(vec3(vUV * 30.0, 1.0));
  } else if (m == 1) {  // terrain: material chosen per pixel from height, slope and noise
    float h = P.y, slope = 1.0 - n.y;
    float n1 = vnoise(P * 0.05), n2 = vnoise(P * 0.31), n3 = vnoise(P * 2.3);
    vec3 grassA = vec3(0.060, 0.140, 0.028), grassB = vec3(0.150, 0.165, 0.055), dirt = vec3(0.15, 0.10, 0.06);
    vec3 sand = vec3(0.50, 0.42, 0.28), wet = vec3(0.24, 0.20, 0.13), rock = vec3(0.23, 0.22, 0.20), snow = vec3(0.82, 0.84, 0.88);
    vec3 g = mix(grassA, grassB, smoothstep(0.35, 0.75, n1 + (n2 - 0.5) * 0.3));
    g = mix(g, dirt, smoothstep(0.62, 0.8, n2 * 0.7 + n1 * 0.3) * 0.7);
    float sandAmt = 1.0 - smoothstep(1.4, 2.6, h + (n2 - 0.5) * 0.8);
    vec3 c = mix(g, sand, sandAmt);
    c = mix(c, wet, 1.0 - smoothstep(-0.3, 0.45, h));
    float rockAmt = smoothstep(0.13, 0.25, slope + (n3 - 0.5) * 0.06);
    vec3 rk = rock * (0.7 + 0.55 * vnoise(vec3(P.x * 0.4, P.y * 2.5, P.z * 0.4)));  // layered strata
    c = mix(c, rk, rockAmt);
    c = mix(c, snow, smoothstep(60.0, 66.0, h + n1 * 4.0) * (1.0 - rockAmt * 0.6));
    albedo = c * (0.8 + 0.35 * n3);
    n = bump(n, P, 1.7, 0.25 + rockAmt * 0.6);
    specAmt = mix(0.02, 0.25, 1.0 - smoothstep(-0.3, 0.3, h));  // wet sand shines
    shin = 40.0;
  } else if (m == 3) {  // wood planks
    float rows = uv.y * 3.3;
    float row = floor(rows), f = fract(rows);
    float seam = smoothstep(0.0, 0.07, f) * (1.0 - smoothstep(0.93, 1.0, f));
    float j = fract(uv.x / 2.4 + hash13(vec3(row, 3.0, 1.0)));
    seam *= smoothstep(0.0, 0.012, j) * (1.0 - smoothstep(0.988, 1.0, j));
    float grain = vnoise(vec3(uv.x * 0.8 + row * 13.7, f * 6.0, row)) * 0.6 + vnoise(vec3(uv.x * 2.0, uv.y * 60.0, row)) * 0.4;
    albedo *= (0.78 + 0.35 * hash13(vec3(row, 1.7, 3.1))) * (0.75 + 0.4 * grain) * mix(0.35, 1.0, seam);
    n = bump(n, P, 6.0, 0.15);
  } else if (m == 4) {  // stone bricks
    vec2 b = vec2(uv.x / 0.8, uv.y / 0.4);
    float r = floor(b.y);
    b.x += mod(r, 2.0) * 0.5;
    vec2 c = fract(b), id = floor(b);
    float mortar = smoothstep(0.0, 0.04, c.x) * (1.0 - smoothstep(0.96, 1.0, c.x)) * smoothstep(0.0, 0.08, c.y) * (1.0 - smoothstep(0.92, 1.0, c.y));
    albedo *= (0.78 + 0.4 * hash13(vec3(id, 7.0))) * (0.85 + 0.3 * vnoise(P * 4.0)) * mix(0.45, 1.0, mortar);
    n = bump(n, P, 3.0, 0.4);
  } else if (m == 5) {  // metal panels with rust
    vec2 c = fract(uv / vec2(1.5, 1.0));
    float seam = smoothstep(0.0, 0.015, c.x) * (1.0 - smoothstep(0.985, 1.0, c.x)) * smoothstep(0.0, 0.02, c.y) * (1.0 - smoothstep(0.98, 1.0, c.y));
    albedo *= (0.8 + 0.3 * vnoise(vec3(uv.x * 3.0, uv.y * 0.4, 1.0))) * mix(0.5, 1.0, seam);
    albedo = mix(albedo, albedo * vec3(1.4, 0.75, 0.4), smoothstep(0.6, 0.85, vnoise(P * 1.7)) * 0.5);
    specAmt = 0.35;
    shin = 48.0;
  } else if (m == 6) {  // twig: see-through lattice of sticks
    float s1 = abs(fract((uv.x + uv.y) * 1.6) - 0.5), s2 = abs(fract((uv.x - uv.y) * 1.6 + 0.25) - 0.5);
    vec2 cb = fract(uv / 3.0);
    float frame = min(min(cb.x, 1.0 - cb.x), min(cb.y, 1.0 - cb.y)) * 3.0;
    if (min(s1, s2) > 0.09 && frame > 0.12) discard;
    albedo *= 0.75 + 0.45 * vnoise(P * 9.0);
  } else if (m == 7) {  // foliage volumes
    albedo *= 0.7 + 0.6 * vnoise(P * 1.3);
    specAmt = 0.02;
  } else if (m == 8) {  // grass: darker at the root
    albedo *= mix(0.5, 1.2, vSway);
  } else if (m == 9) {  // bark: vertical furrows
    albedo *= 0.6 + 0.55 * vnoise(vec3(P.x * 14.0, P.y * 1.2, P.z * 14.0));
    n = bump(n, vec3(P.x * 3.0, P.y * 0.4, P.z * 3.0), 4.0, 0.5);
  } else if (m == 10) {  // rock: rough, cracked, mossy on top
    albedo *= 0.65 + 0.4 * vnoise(P * 1.4) + 0.2 * vnoise(P * 5.0);
    float crack = smoothstep(0.02, 0.0, abs(vnoise(P * 2.2) - 0.5) - 0.01);
    albedo *= 1.0 - crack * 0.5;
    n = bump(n, P, 2.5, 0.9);
    albedo = mix(albedo, vec3(0.05, 0.10, 0.025), smoothstep(0.6, 0.9, n.y) * smoothstep(0.45, 0.7, vnoise(P * 0.8)) * 0.75);
  } else if (m == 12) {  // cloth weave
    albedo *= 0.88 + 0.12 * sin((uv.x + uv.y) * 140.0);
  } else if (m == 13) {
    specAmt = 0.08;
  }

  float sh = shadowAt(P, n);
  float ndl = dot(n, uLightDir);
  float diff = max(ndl, 0.0);
  bool foliage = (m == 7 || m == 8 || m == 14 || m == 15);
  if (foliage) diff = max(ndl * 0.6 + 0.4, 0.0);  // soft wrap-around light through leaves
  vec3 amb = mix(uGroundAmb, uSkyAmb, n.y * 0.5 + 0.5);
  if (m == 8) amb *= mix(0.5, 1.0, vSway);
  vec3 col = albedo * (uLightColor * diff * sh + amb);
  if (foliage) col += albedo * uLightColor * pow(max(dot(-V, uLightDir), 0.0), 4.0) * 0.4 * sh;  // backlit glow
  vec3 H = normalize(uLightDir + V);
  col += uLightColor * pow(max(dot(n, H), 0.0), shin) * specAmt * sh * step(0.0, ndl);
  if (m == 11) col = albedo;  // emissive (muzzle flash)
  fragColor = vec4(finish(applyFog(col, P)), uAlpha);
}
)";

static const char* kShadowVertex = R"(#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 2) in vec3 aColor;
layout(location = 3) in vec2 aExtra;
layout(location = 4) in vec2 aUV;
uniform mat4 uModel;
uniform mat4 uLightVP;
uniform float uTime;
out vec2 vUV;
out float vMat;
out float vSeed;
void main() {
  vec4 world = uModel * vec4(aPos, 1.0);
  if (aExtra.y > 0.0) {
    float t = uTime * 1.6 + world.x * 0.15 + world.z * 0.12;
    world.xyz += vec3(sin(t), 0.0, cos(t * 0.83)) * 0.09 * aExtra.y;
  }
  vUV = aUV;
  vMat = aExtra.x;
  vSeed = floor(aColor.r * 4000.0);
  gl_Position = uLightVP * world;
}
)";
static const char* kShadowFragment = R"(
in vec2 vUV;
in float vMat;
in float vSeed;
void main() {
  int m = int(vMat + 0.5);
  if (m == 14 && leafMask(vUV, vSeed) < 0.5) discard;  // leafy shadows, not square ones
  if (m == 15 && needleMask(vUV) < 0.5) discard;
}
)";

static const char* kSkyVertex = R"(#version 330 core
out vec2 vNdc;
void main() {
  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2)) * 2.0 - 1.0;
  vNdc = p;
  gl_Position = vec4(p, 0.0, 1.0);
}
)";
static const char* kSkyFragment = R"(
in vec2 vNdc;
uniform vec3 uForward;
uniform vec3 uRight;
uniform vec3 uUp;
uniform vec2 uTan;
out vec4 fragColor;
void main() {
  vec3 d = normalize(uForward + vNdc.x * uTan.x * uRight + vNdc.y * uTan.y * uUp);
  vec3 col = skyColor(d);
  col += vec3(1.0, 0.92, 0.75) * smoothstep(0.99935, 0.99975, dot(d, uSunPos)) * 25.0 * (1.0 - uNight * 0.9);  // sun
  float moon = smoothstep(0.99955, 0.99975, dot(d, uMoonPos));
  col += vec3(0.75, 0.8, 0.9) * moon * 2.5 * uNight;
  col += vec3(0.3, 0.35, 0.5) * pow(max(dot(d, uMoonPos), 0.0), 60.0) * 0.15 * uNight;  // moon halo
  if (d.y > 0.0) {
    // Stars: sparse twinkling points on a direction grid.
    vec3 sd = d * 220.0;
    vec3 cell = floor(sd);
    float star = step(0.9975, hash13(cell)) * smoothstep(0.35, 0.0, length(fract(sd) - 0.5));
    star *= 0.6 + 0.4 * sin(uTime * 3.0 + hash13(cell + 7.0) * 30.0);
    col += vec3(0.9, 0.95, 1.0) * star * uNight * 1.6 * smoothstep(0.0, 0.15, d.y);
    // Drifting clouds, lit by the sun or (dimly) the moon.
    vec2 cuv = d.xz / (d.y + 0.12) * 0.9 + vec2(uTime * 0.004, uTime * 0.002);
    float c = fbm(vec3(cuv * 1.3, uTime * 0.01));
    c = smoothstep(0.48, 0.78, c) * smoothstep(0.0, 0.18, d.y);
    float lit = fbm(vec3(cuv * 1.3 + uLightDir.xz * 0.08, uTime * 0.01 + 0.3));
    vec3 cc = mix(uHorizon * 0.8, uHorizon * 0.4 + uLightColor * 0.45, smoothstep(0.3, 1.0, lit));
    col = mix(col, cc, c * 0.85);
  }
  fragColor = vec4(finish(col), 1.0);
}
)";

static GLuint compile(GLenum type, const std::string& src) {
  GLuint s = glCreateShader(type);
  const char* p = src.c_str();
  glShaderSource(s, 1, &p, nullptr);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[4096];
    glGetShaderInfoLog(s, sizeof(log), nullptr, log);
    std::fprintf(stderr, "[gl] shader compile error:\n%s\n", log);
    glDeleteShader(s);
    return 0;
  }
  return s;
}

static GLuint link(const std::string& vsSrc, const std::string& fsSrc) {
  GLuint vs = compile(GL_VERTEX_SHADER, vsSrc), fs = compile(GL_FRAGMENT_SHADER, fsSrc);
  if (!vs || !fs) return 0;
  GLuint p = glCreateProgram();
  glAttachShader(p, vs);
  glAttachShader(p, fs);
  glLinkProgram(p);
  glDeleteShader(vs);
  glDeleteShader(fs);
  GLint ok = 0;
  glGetProgramiv(p, GL_LINK_STATUS, &ok);
  if (!ok) {
    char log[4096];
    glGetProgramInfoLog(p, sizeof(log), nullptr, log);
    std::fprintf(stderr, "[gl] link error:\n%s\n", log);
    return 0;
  }
  return p;
}

// --- day and night -----------------------------------------------------------------------
static float smooth01(float a, float b, float x) {
  float t = clampf((x - a) / (b - a), 0.0f, 1.0f);
  return t * t * (3 - 2 * t);
}

Renderer::Lighting Renderer::lightingFor(float hours) {
  Lighting L;
  // Sun rises in the east (+X) at 06:00, highest at noon, sets in the west at 18:00.
  float a = (hours - 6.0f) / 12.0f * kPi;
  L.sunPos = normalize(Vec3{std::cos(a) * 0.9f, std::sin(a), 0.38f});
  L.moonPos = normalize(Vec3{-L.sunPos.x, -L.sunPos.y, 0.3f});
  const float elev = L.sunPos.y;
  const float day = smooth01(-0.12f, 0.1f, elev);
  const float golden = (1.0f - smooth01(0.0f, 0.35f, elev)) * day;  // warm light near the horizon
  L.night = 1.0f - day;
  Vec3 sunColor = lerp(Vec3{2.7f, 2.45f, 2.1f}, Vec3{2.6f, 1.25f, 0.5f}, golden);
  Vec3 moonColor{0.20f, 0.26f, 0.42f};
  if (elev > -0.06f) {
    L.lightDir = L.sunPos.y < 0.06f ? normalize(Vec3{L.sunPos.x, 0.06f, L.sunPos.z}) : L.sunPos;
    L.lightColor = sunColor * smooth01(-0.06f, 0.08f, elev);
  } else {
    L.lightDir = L.moonPos.y < 0.15f ? normalize(Vec3{L.moonPos.x, 0.15f, L.moonPos.z}) : L.moonPos;
    L.lightColor = moonColor * smooth01(-0.06f, -0.2f, elev);
  }
  L.skyAmb = lerp(Vec3{0.030f, 0.040f, 0.075f}, Vec3{0.30f, 0.38f, 0.52f}, day);
  L.groundAmb = lerp(Vec3{0.015f, 0.016f, 0.022f}, Vec3{0.13f, 0.11f, 0.09f}, day);
  L.zenith = lerp(Vec3{0.004f, 0.008f, 0.025f}, Vec3{0.09f, 0.22f, 0.55f}, day);
  Vec3 horizonDay = lerp(Vec3{0.55f, 0.66f, 0.80f}, Vec3{0.95f, 0.52f, 0.28f}, golden * 0.8f);
  L.horizon = lerp(Vec3{0.02f, 0.03f, 0.06f}, horizonDay, day);
  L.exposure = 1.0f + L.night * 1.6f;  // eyes adjust: nights are dark but playable
  return L;
}

void Renderer::setLightingUniforms(GLuint p) {
  auto v3 = [&](const char* n, const Vec3& v) { glUniform3f(glGetUniformLocation(p, n), v.x, v.y, v.z); };
  v3("uLightDir", light_.lightDir);
  v3("uLightColor", light_.lightColor);
  v3("uSunPos", light_.sunPos);
  v3("uMoonPos", light_.moonPos);
  v3("uSkyAmb", light_.skyAmb);
  v3("uGroundAmb", light_.groundAmb);
  v3("uZenith", light_.zenith);
  v3("uHorizon", light_.horizon);
  glUniform1f(glGetUniformLocation(p, "uNight"), light_.night);
  glUniform1f(glGetUniformLocation(p, "uExposure"), light_.exposure);
}

// --- setup ----------------------------------------------------------------------------------
bool Renderer::init() {
  const std::string header = "#version 330 core\n";
  program_ = link(kVertexShader, header + kCommon + kFragmentShader);
  shadowProgram_ = link(kShadowVertex, header + kCommon + kShadowFragment);
  skyProgram_ = link(kSkyVertex, header + kCommon + kSkyFragment);
  if (!program_ || !shadowProgram_ || !skyProgram_) return false;
  uModel_ = glGetUniformLocation(program_, "uModel");
  uTint_ = glGetUniformLocation(program_, "uTint");
  uAlpha_ = glGetUniformLocation(program_, "uAlpha");
  uShadowModel_ = glGetUniformLocation(shadowProgram_, "uModel");
  uShadowLightVP_ = glGetUniformLocation(shadowProgram_, "uLightVP");
  uShadowTime_ = glGetUniformLocation(shadowProgram_, "uTime");
  glGenVertexArrays(1, &skyVao_);
  light_ = lightingFor(15.0f);

  // Sun shadow map.
  glGenTextures(1, &shadowTex_);
  glBindTexture(GL_TEXTURE_2D, shadowTex_);
  glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(GL_DEPTH_COMPONENT24), kShadowSize, kShadowSize, 0, GL_DEPTH_COMPONENT,
               GL_FLOAT, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(GL_LINEAR));
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, static_cast<GLint>(GL_LINEAR));
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, static_cast<GLint>(GL_CLAMP_TO_BORDER));
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, static_cast<GLint>(GL_CLAMP_TO_BORDER));
  const float border[4] = {1, 1, 1, 1};
  glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, border);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, static_cast<GLint>(GL_COMPARE_REF_TO_TEXTURE));
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, static_cast<GLint>(GL_LEQUAL));
  glGenFramebuffers(1, &shadowFbo_);
  glBindFramebuffer(GL_FRAMEBUFFER, shadowFbo_);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, shadowTex_, 0);
  glDrawBuffer(GL_NONE);
  glReadBuffer(GL_NONE);
  shadowsOn_ = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
  if (!shadowsOn_) std::fprintf(stderr, "[gl] shadow framebuffer not supported; shadows off\n");
  glBindFramebuffer(GL_FRAMEBUFFER, 0);

  // Terrain heights for the water shader (filled when the terrain is built).
  glGenTextures(1, &heightTex_);
  glBindTexture(GL_TEXTURE_2D, heightTex_);
  const float flat = -20.0f;
  glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(GL_R32F), 1, 1, 0, GL_RED, GL_FLOAT, &flat);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(GL_LINEAR));
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, static_cast<GLint>(GL_LINEAR));
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, static_cast<GLint>(GL_CLAMP_TO_EDGE));
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, static_cast<GLint>(GL_CLAMP_TO_EDGE));

  {
    MeshBuilder mb;
    mb.box(Mat4::identity(), {1, 1, 1}, kMatPlain);
    upload(box_, mb);
  }
  {
    MeshBuilder mb;
    mb.blob(Mat4::identity(), 1, 0.3f, {1, 1, 1}, kMatEmissive, 1);
    upload(blob_, mb);
  }
  {
    MeshBuilder mb;  // smooth unit sphere for heads, limbs and hands
    mb.blob(Mat4::identity(), 1, 0.0f, {1, 1, 1}, kMatSkin, 2, 0, 0, 90);
    upload(sphere_, mb);
  }
  {
    // Sea: one big quad at sea level reaching past the island.
    MeshBuilder mb;
    const float lo = -3000.0f, hi = kWorldSize + 3000.0f, y = kSeaLevel - 0.15f;
    mb.quad({lo, y, lo}, {lo, y, hi}, {hi, y, hi}, {hi, y, lo}, {1, 1, 1}, kMatWater);
    upload(water_, mb);
  }
  renderItemIcons();

  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  return true;
}

void Renderer::shutdown() {
  for (Mesh* m : {&terrain_, &props_, &grass_, &water_, &box_, &objects_, &blob_, &sphere_}) destroy(*m);
  for (auto& m : items_) destroy(m);
  for (auto& t : icons_)
    if (t) glDeleteTextures(1, &t);
  for (GLuint* t : {&shadowTex_, &heightTex_})
    if (*t) glDeleteTextures(1, t);
  if (shadowFbo_) glDeleteFramebuffers(1, &shadowFbo_);
  if (skyVao_) glDeleteVertexArrays(1, &skyVao_);
  for (GLuint p : {program_, shadowProgram_, skyProgram_})
    if (p) glDeleteProgram(p);
  program_ = shadowProgram_ = skyProgram_ = 0;
}

void Renderer::resize(int width, int height) {
  width_ = width > 0 ? width : 1;
  height_ = height > 0 ? height : 1;
}

void Renderer::upload(Mesh& m, const MeshBuilder& mb) {
  if (!m.vao) {
    glGenVertexArrays(1, &m.vao);
    glGenBuffers(1, &m.vbo);
    glGenBuffers(1, &m.ebo);
  }
  glBindVertexArray(m.vao);
  glBindBuffer(GL_ARRAY_BUFFER, m.vbo);
  glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(mb.v.size() * sizeof(Vertex)), mb.v.data(), GL_STATIC_DRAW);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m.ebo);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(mb.idx.size() * sizeof(uint32_t)), mb.idx.data(),
               GL_STATIC_DRAW);
  const GLsizei stride = sizeof(Vertex);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(offsetof(Vertex, px)));
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(offsetof(Vertex, nx)));
  glEnableVertexAttribArray(2);
  glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(offsetof(Vertex, r)));
  glEnableVertexAttribArray(3);
  glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(offsetof(Vertex, mat)));
  glEnableVertexAttribArray(4);
  glVertexAttribPointer(4, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(offsetof(Vertex, u)));
  glBindVertexArray(0);
  m.count = static_cast<GLsizei>(mb.idx.size());
}

void Renderer::destroy(Mesh& m) {
  if (m.vao) glDeleteVertexArrays(1, &m.vao);
  if (m.vbo) glDeleteBuffers(1, &m.vbo);
  if (m.ebo) glDeleteBuffers(1, &m.ebo);
  m = {};
}

void Renderer::setCommonUniforms(GLuint program, int mode, const Mat4& viewProj, const Vec3& camPos) {
  glUseProgram(program);
  glUniformMatrix4fv(glGetUniformLocation(program, "uViewProj"), 1, GL_FALSE, viewProj.m);
  glUniform3f(glGetUniformLocation(program, "uCamPos"), camPos.x, camPos.y, camPos.z);
  glUniform1f(glGetUniformLocation(program, "uTime"), time_);
  glUniform2f(glGetUniformLocation(program, "uResolution"), static_cast<float>(width_), static_cast<float>(height_));
  glUniform1i(glGetUniformLocation(program, "uMode"), mode);
  setLightingUniforms(program);
}

// 3D item icons for the inventory, rendered once at startup in daylight.
void Renderer::renderItemIcons() {
  GLuint fbo = 0, depth = 0;
  glGenFramebuffers(1, &fbo);
  glGenTextures(1, &depth);
  glBindTexture(GL_TEXTURE_2D, depth);
  glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(GL_DEPTH_COMPONENT24), kIconSize, kIconSize, 0, GL_DEPTH_COMPONENT,
               GL_FLOAT, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(GL_NEAREST));
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, static_cast<GLint>(GL_NEAREST));
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depth, 0);

  const int savedW = width_, savedH = height_;
  const Lighting savedLight = light_;
  light_ = lightingFor(13.0f);
  light_.lightDir = normalize(Vec3{0.45f, 0.65f, -0.35f});  // a studio key light from the camera side
  width_ = height_ = kIconSize;
  glEnable(GL_DEPTH_TEST);
  glDepthMask(GL_TRUE);
  glEnable(GL_CULL_FACE);
  glCullFace(GL_BACK);
  glDisable(GL_BLEND);
  for (size_t i = 1; i < kItemCount; ++i) {
    MeshBuilder mb;
    buildItemModel(static_cast<Item>(i), mb);
    upload(items_[i], mb);

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(GL_RGBA8), kIconSize, kIconSize, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(GL_LINEAR_MIPMAP_LINEAR));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, static_cast<GLint>(GL_LINEAR));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, static_cast<GLint>(GL_CLAMP_TO_EDGE));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, static_cast<GLint>(GL_CLAMP_TO_EDGE));
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    glDrawBuffer(GL_COLOR_ATTACHMENT0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
      glDeleteTextures(1, &tex);
      continue;
    }
    glViewport(0, 0, kIconSize, kIconSize);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // Frame the model from its right side, a little above and in front.
    Vec3 center = (mb.bmin + mb.bmax) * 0.5f;
    float radius = std::fmax(length(mb.bmax - mb.bmin) * 0.5f, 0.03f);
    const float fov = 28.0f * kPi / 180.0f;
    float dist = radius / std::sin(fov * 0.5f) * 1.02f;
    Vec3 eye = center + normalize(Vec3{1.0f, 0.55f, -0.4f}) * dist;
    Mat4 vp = Mat4::perspective(fov, 1.0f, dist * 0.1f, dist * 3.0f) * Mat4::lookAt(eye, center, {0, 1, 0});
    setCommonUniforms(program_, 1, vp, eye);
    glUniform1f(glGetUniformLocation(program_, "uShadowOn"), 0.0f);
    glUniform1f(glGetUniformLocation(program_, "uExposure"), 1.15f);
    drawCmd({&items_[i], Mat4::identity(), {1, 1, 1}, 1.0f, false});
    glBindTexture(GL_TEXTURE_2D, tex);
    glGenerateMipmap(GL_TEXTURE_2D);
    icons_[i] = tex;
  }
  width_ = savedW;
  height_ = savedH;
  light_ = savedLight;
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glDeleteFramebuffers(1, &fbo);
  glDeleteTextures(1, &depth);
  glBindTexture(GL_TEXTURE_2D, 0);
}

// --- world geometry ---------------------------------------------------------------------------
void Renderer::rebuildTerrain(const Terrain& terrain, float cx, float cz) {
  const int n = kGridCells + 1;
  const float half = kGridCells * kCellSize * 0.5f;
  // Snap to the grid so vertices land on the same spots after a rebuild (no swimming).
  float ox = std::floor((cx - half) / kCellSize) * kCellSize;
  float oz = std::floor((cz - half) / kCellSize) * kCellSize;

  MeshBuilder mb;  // smooth shared vertices: filled directly; colour is chosen per pixel in the shader
  mb.v.reserve(static_cast<size_t>(n) * n);
  std::vector<float> heights(static_cast<size_t>(n) * n);
  for (int j = 0; j < n; ++j)
    for (int i = 0; i < n; ++i) {
      float x = ox + i * kCellSize, z = oz + j * kCellSize;
      float h = terrain.heightAt(x, z);
      heights[static_cast<size_t>(j) * n + i] = h;
      Vec3 nrm = terrain.normalAt(x, z);
      mb.v.push_back({x, h, z, nrm.x, nrm.y, nrm.z, 1, 1, 1, static_cast<float>(kMatTerrain), 0});
    }
  mb.idx.reserve(static_cast<size_t>(kGridCells) * kGridCells * 6);
  for (int j = 0; j < kGridCells; ++j)
    for (int i = 0; i < kGridCells; ++i) {
      auto a = static_cast<uint32_t>(j * n + i), b = a + 1, c = a + n, d = c + 1;
      mb.idx.insert(mb.idx.end(), {a, c, b, b, c, d});
    }
  upload(terrain_, mb);

  glBindTexture(GL_TEXTURE_2D, heightTex_);
  glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(GL_R32F), n, n, 0, GL_RED, GL_FLOAT, heights.data());
  glBindTexture(GL_TEXTURE_2D, 0);
  heightOriginX_ = ox;
  heightOriginZ_ = oz;
  heightExtent_ = kGridCells * kCellSize;

  MeshBuilder grass;
  buildGrass(terrain, cx, cz, kGrassRadius, grass);
  upload(grass_, grass);

  terrainCx_ = cx;
  terrainCz_ = cz;
  terrainSeed_ = terrain.seed();
  haveTerrain_ = true;
}

void Renderer::rebuildProps(const Terrain& terrain, const std::unordered_set<uint32_t>& depleted) {
  MeshBuilder mb;
  const float half = kGridCells * kCellSize * 0.5f;
  std::vector<ResourceNode> nodes;
  nodesInRect(terrain, terrainCx_ - half, terrainCz_ - half, terrainCx_ + half, terrainCz_ + half, nodes);
  for (const auto& nd : nodes) {
    if (depleted.count(nd.id)) continue;
    float dx = nd.pos.x - terrainCx_, dz = nd.pos.z - terrainCz_;
    buildNode(nd, mb, dx * dx + dz * dz < kDetailRadius * kDetailRadius);
  }
  upload(props_, mb);
}

// --- frame -------------------------------------------------------------------------------------
void Renderer::beginFrame(const Camera& cam, float time, float timeOfDay) {
  cam_ = cam;
  time_ = time;
  light_ = lightingFor(timeOfDay);
  opaque_.clear();
  transparent_.clear();
  viewModel_.clear();
  drawWater_ = false;
  Vec3 dir = directionFromYawPitch(cam.yaw, cam.pitch);
  Mat4 view = Mat4::lookAt(cam.eye, cam.eye + dir, {0, 1, 0});
  Mat4 proj = Mat4::perspective(cam.fovY, static_cast<float>(width_) / static_cast<float>(height_), 0.05f, 1500.0f);
  viewProj_ = proj * view;
}

void Renderer::box(std::vector<DrawCmd>& list, const Mat4& m, const Vec3& color, float alpha, bool shadow) {
  list.push_back({&box_, m, color, alpha, shadow});
}

void Renderer::drawWorld(const Terrain& terrain, const Vec3& around, const std::unordered_set<uint32_t>& depleted,
                         uint32_t nodesVersion) {
  float dx = around.x - terrainCx_, dz = around.z - terrainCz_;
  if (!haveTerrain_ || terrain.seed() != terrainSeed_ || dx * dx + dz * dz > kRebuildDistance * kRebuildDistance) {
    rebuildTerrain(terrain, around.x, around.z);
    propsVersion_ = 0;
  }
  if (propsVersion_ != nodesVersion) {
    rebuildProps(terrain, depleted);
    propsVersion_ = nodesVersion;
  }
  opaque_.push_back({&terrain_, Mat4::identity(), {1, 1, 1}, 1.0f, false});
  opaque_.push_back({&grass_, Mat4::identity(), {1, 1, 1}, 1.0f, false});
  opaque_.push_back({&props_, Mat4::identity(), {1, 1, 1}, 1.0f, true});
  drawWater_ = true;
}

void Renderer::drawObjects(const World& world) {
  if (objectsVersion_ != world.version()) {
    MeshBuilder mb;
    for (const auto& [id, o] : world.objects()) buildObject(o, mb);
    upload(objects_, mb);
    objectsVersion_ = world.version();
  }
  opaque_.push_back({&objects_, Mat4::identity(), {1, 1, 1}, 1.0f, true});
}

void Renderer::drawGhost(const WorldObject& o, bool valid) {
  std::vector<ObjBox> boxes;
  objectBoxes(o, boxes, true);
  Vec3 color = valid ? Vec3{0.10f, 0.75f, 0.30f} : Vec3{0.90f, 0.12f, 0.10f};
  for (const auto& b : boxes) {
    Vec3 c = (b.box.min + b.box.max) * 0.5f, s = b.box.max - b.box.min;
    box(transparent_, Mat4::translate(c) * Mat4::scale(s), color, 0.38f, false);
  }
}

namespace {
const Vec3 kSkin{0.32f, 0.17f, 0.10f};
const Vec3 kPants{0.06f, 0.06f, 0.075f};
const Vec3 kBoots{0.04f, 0.03f, 0.02f};
const Vec3 kHair{0.05f, 0.03f, 0.015f};
const Vec3 kSleeve{0.16f, 0.12f, 0.07f};
const Vec3 kFlash{7.0f, 4.5f, 1.6f};
constexpr float kHalfPi = kPi * 0.5f;
}  // namespace

void Renderer::drawPlayer(const Vec3& feet, float yaw, float pitch, const PlayerLook& look) {
  using M = Mat4;
  // Rounded body parts: unit spheres stretched into limbs (radius = half the size given).
  auto part = [&](const M& m, const Vec3& size, const Vec3& color) {
    opaque_.push_back({&sphere_, m * M::scale(size * 0.5f), color, 1.0f, true});
  };
  M base = M::translate(feet) * M::rotateY(yaw);
  float sw = std::sin(look.walkPhase) * 0.55f * look.walkAmount;
  if (look.dead) {
    base = M::translate(feet + Vec3{0, 0.14f, 0}) * M::rotateY(yaw) * M::rotateX(-kHalfPi) * M::translate({0, -0.9f, 0});
    sw = 0;
    pitch = 0;
  }
  for (float side : {-1.0f, 1.0f}) {
    M hip = base * M::translate({side * 0.11f, 0.92f, 0}) * M::rotateX(side * sw);
    part(hip * M::translate({0, -0.24f, 0}), {0.19f, 0.5f, 0.21f}, kPants);  // thigh
    M knee = hip * M::translate({0, -0.46f, 0}) * M::rotateX(std::fabs(sw) * 0.6f);
    part(knee * M::translate({0, -0.2f, 0}), {0.15f, 0.46f, 0.17f}, kPants);  // shin
    box(opaque_, knee * M::translate({0, -0.42f, -0.04f}) * M::scale({0.15f, 0.1f, 0.26f}), kBoots);
  }
  part(base * M::translate({0, 0.98f, 0}), {0.40f, 0.26f, 0.24f}, kPants);  // hips
  part(base * M::translate({0, 1.24f, 0}), {0.46f, 0.62f, 0.27f}, look.shirt);  // torso
  part(base * M::translate({0, 1.46f, 0}), {0.52f, 0.18f, 0.26f}, look.shirt);  // shoulders
  M neck = base * M::translate({0, 1.56f, 0}) * M::rotateX(pitch * 0.6f);
  part(neck * M::translate({0, 0.03f, 0}), {0.11f, 0.12f, 0.11f}, kSkin);
  part(neck * M::translate({0, 0.17f, 0}), {0.23f, 0.27f, 0.25f}, kSkin);  // head
  part(neck * M::translate({0, 0.23f, 0.02f}), {0.245f, 0.2f, 0.26f}, kHair);
  part(neck * M::translate({0, 0.15f, -0.12f}), {0.04f, 0.06f, 0.05f}, kSkin * 0.9f);  // nose
  for (float side : {-1.0f, 1.0f}) part(neck * M::translate({side * 0.055f, 0.19f, -0.105f}), {0.035f, 0.025f, 0.02f}, {0.02f, 0.02f, 0.02f});

  const bool holding = look.held != Item::None && !look.dead;
  const ItemKind kind = itemDef(look.held).kind;
  const bool gun = holding && kind == ItemKind::Gun;
  // Right arm: forward and aimed when holding something, swinging otherwise.
  float rightRot = holding ? kHalfPi + pitch + (look.attacking && !gun ? 0.7f : 0.0f) : -sw;
  M rs = base * M::translate({0.3f, 1.44f, 0}) * M::rotateX(rightRot);
  part(rs * M::translate({0, -0.16f, 0}), {0.14f, 0.36f, 0.15f}, look.shirt);
  part(rs * M::translate({0, -0.42f, 0}), {0.11f, 0.32f, 0.12f}, look.shirt);
  part(rs * M::translate({0, -0.6f, 0}), {0.1f, 0.11f, 0.1f}, kSkin);
  float leftRot = gun ? kHalfPi + pitch : sw;
  M ls = base * M::translate({-0.3f, 1.44f, 0}) * M::rotateX(leftRot) * M::rotateZ(gun ? -0.45f : 0.0f);
  part(ls * M::translate({0, -0.16f, 0}), {0.14f, 0.36f, 0.15f}, look.shirt);
  part(ls * M::translate({0, -0.42f, 0}), {0.11f, 0.32f, 0.12f}, look.shirt);
  part(ls * M::translate({0, -0.6f, 0}), {0.1f, 0.11f, 0.1f}, kSkin);
  if (holding) {
    M item = rs * M::translate({0, -0.6f, 0}) * M::rotateX(-kHalfPi);
    opaque_.push_back({&items_[static_cast<size_t>(look.held)], item, {1, 1, 1}, 1.0f, true});
    if (gun && look.attacking && look.held != Item::Bow) {
      Vec3 m = gunMounts(look.held).muzzle;
      opaque_.push_back({&blob_, item * M::translate(m) * M::scale({0.1f, 0.1f, 0.14f}), kFlash, 1.0f, false});
    }
  }
}

void Renderer::gunWithMods(std::vector<DrawCmd>& list, const Mat4& m, const ItemStack& gun, bool shadow) {
  list.push_back({&items_[static_cast<size_t>(gun.item)], m, {1, 1, 1}, 1.0f, shadow});
  if (itemDef(gun.item).kind != ItemKind::Gun) return;
  const GunMounts mt = gunMounts(gun.item);
  const Vec3 at[3] = {mt.sight, mt.muzzle, mt.grip};
  for (int slot = 0; slot < 3; ++slot) {
    Item mod = modAt(gun, static_cast<ModSlot>(slot));
    if (mod != Item::None) list.push_back({&items_[static_cast<size_t>(mod)], m * Mat4::translate(at[slot]), {1, 1, 1}, 1.0f, shadow});
  }
}

void Renderer::drawViewModel(const ItemStack& heldStack, float swing, bool muzzleFlash, float bobPhase, float bobAmount,
                             float aim, bool hideGun) {
  using M = Mat4;
  const Item held = heldStack.empty() ? Item::None : heldStack.item;
  const M cam = M::translate(cam_.eye) * M::rotateY(cam_.yaw) * M::rotateX(cam_.pitch);
  const ItemDef& def = itemDef(held);
  const ItemKind kind = def.kind;
  const float s = std::sin(clampf(swing, 0.0f, 1.0f) * kPi);
  const Vec3 bob = Vec3{std::sin(bobPhase) * 0.012f * bobAmount, -std::fabs(std::cos(bobPhase)) * 0.014f * bobAmount, 0} *
                   (1.0f - 0.85f * aim);
  auto part = [&](const M& m, const Vec3& size, const Vec3& color) {
    viewModel_.push_back({&sphere_, m * M::scale(size * 0.5f), color, 1.0f, false});
  };
  M hand;
  if (held == Item::None) {  // bare fists, punching
    hand = cam * M::translate(Vec3{0.24f, -0.26f, -0.4f - 0.18f * s} + bob) * M::rotateX(0.2f);
  } else if (kind == ItemKind::Gun) {
    // Hip position blends into aiming down the sights: the sight line moves to the screen centre.
    const GunMounts mt = gunMounts(held);
    Item sight = modAt(heldStack, ModSlot::Sight);
    float eyeY = sight != Item::None ? mt.sight.y + sightEyeHeight(sight) : def.sightHeight;
    Vec3 hip{0.15f, -0.15f, -0.32f}, ads{0.0f, -eyeY, held == Item::Bow ? -0.42f : -0.26f};
    Vec3 pos = lerp(hip, ads, aim) + bob + Vec3{0, 0, 0.045f * s * (1.0f - 0.4f * aim)};
    hand = cam * M::translate(pos) * M::rotateX(0.16f * s * (1.0f - 0.6f * aim)) * M::rotateY(0.03f * (1.0f - aim));
  } else if (kind == ItemKind::Food || kind == ItemKind::Medical) {
    hand = cam * M::translate(Vec3{0.24f - 0.12f * s, -0.27f + 0.12f * s, -0.42f} + bob) * M::rotateX(0.4f * s);
  } else {
    hand = cam * M::translate(Vec3{0.27f, -0.28f, -0.42f - 0.08f * s} + bob) * M::rotateY(-0.25f) * M::rotateX(-1.2f * s) *
           M::rotateZ(0.08f);
  }
  if (hideGun) return;  // looking through a scope: the overlay covers the screen
  if (held != Item::None) gunWithMods(viewModel_, hand, heldStack, false);
  // Right hand (palm + knuckles) and forearm in a sleeve.
  part(hand * M::translate({0, -0.02f, 0.02f}), {0.08f, 0.1f, 0.11f}, kSkin);
  part(hand * M::translate({-0.01f, 0.01f, -0.025f}), {0.075f, 0.05f, 0.05f}, kSkin * 0.95f);
  part(hand * M::translate({0.01f, -0.07f, 0.26f}) * M::rotateX(0.3f), {0.095f, 0.095f, 0.46f}, kSleeve);
  if (kind == ItemKind::Gun) {  // left hand supports the gun under the barrel
    const GunMounts mt = gunMounts(held);
    float z = (held == Item::Pistol || held == Item::Revolver) ? -0.02f : held == Item::Bow ? 0.0f : mt.grip.z + 0.06f;
    M lhand = hand * M::translate(Vec3{-0.03f, -0.06f, z});
    part(lhand, {0.085f, 0.085f, 0.11f}, kSkin);
    Vec3 a = {lhand.at(0, 3), lhand.at(1, 3), lhand.at(2, 3)};
    M elbow = cam * M::translate({-0.28f, -0.42f, 0.0f});
    Vec3 b = {elbow.at(0, 3), elbow.at(1, 3), elbow.at(2, 3)};
    // Forearm: a unit sphere stretched along a->b (beam maps -0.5..0.5, the sphere spans -1..1).
    viewModel_.push_back({&sphere_, M::beam(a, b, 0.095f, 0.095f) * M::scale({0.5f, 0.5f, 0.5f}), kSleeve, 1.0f, false});
    if (muzzleFlash && held != Item::Bow) {
      Vec3 m = mt.muzzle + Vec3{0, 0, -0.03f};
      viewModel_.push_back({&blob_, hand * M::translate(m) * M::scale({0.06f, 0.06f, 0.09f}), kFlash, 1.0f, false});
    }
  }
}

void Renderer::drawExplosion(const Vec3& pos, float age) {
  using M = Mat4;
  if (age < 0.45f) {  // fireball
    float r = 0.6f + 3.2f * clampf(age / 0.12f, 0.0f, 1.0f);
    float a = 1.0f - age / 0.45f;
    transparent_.push_back({&blob_, M::translate(pos) * M::scale({r, r * 0.85f, r}), Vec3{9.0f, 4.5f, 1.2f} * a, a, false});
  }
  for (int k = 0; k < 6; ++k) {  // smoke puffs drifting up
    float t = age - 0.05f * k;
    if (t <= 0 || t > 4.0f) continue;
    float ang = k * 1.05f;
    Vec3 p = pos + Vec3{std::cos(ang) * (0.6f + t * 0.4f), 0.6f + t * (1.2f + 0.2f * k), std::sin(ang) * (0.6f + t * 0.4f)};
    float r = 1.0f + t * 0.9f;
    float a = 0.55f * (1.0f - t / 4.0f);
    transparent_.push_back({&sphere_, M::translate(p) * M::scale({r, r, r}), Vec3{0.08f, 0.075f, 0.07f}, a, false});
  }
}

void Renderer::drawCmd(const DrawCmd& c) {
  if (!c.mesh || !c.mesh->count) return;
  glUniformMatrix4fv(uModel_, 1, GL_FALSE, c.model.m);
  glUniform3f(uTint_, c.tint.x, c.tint.y, c.tint.z);
  glUniform1f(uAlpha_, c.alpha);
  glBindVertexArray(c.mesh->vao);
  glDrawElements(GL_TRIANGLES, c.mesh->count, GL_UNSIGNED_INT, nullptr);
}

void Renderer::endFrame() {
  // Light-space shadow map around the camera, a bit ahead of where we look.
  Vec3 fwd = directionFromYawPitch(cam_.yaw, 0);
  Vec3 center = cam_.eye + fwd * (kShadowRadius * 0.35f);
  Mat4 lightView = Mat4::lookAt(center + light_.lightDir * 220.0f, center, {0, 1, 0});
  {
    // Snap to whole shadow texels so shadows don't shimmer as we move.
    const float texel = 2.0f * kShadowRadius / kShadowSize;
    float lx = lightView.at(0, 0) * center.x + lightView.at(0, 1) * center.y + lightView.at(0, 2) * center.z + lightView.at(0, 3);
    float ly = lightView.at(1, 0) * center.x + lightView.at(1, 1) * center.y + lightView.at(1, 2) * center.z + lightView.at(1, 3);
    lightView.at(0, 3) -= lx - std::floor(lx / texel) * texel;
    lightView.at(1, 3) -= ly - std::floor(ly / texel) * texel;
  }
  lightVP_ = Mat4::ortho(-kShadowRadius, kShadowRadius, -kShadowRadius, kShadowRadius, 1.0f, 500.0f) * lightView;

  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_LESS);
  glDepthMask(GL_TRUE);
  glEnable(GL_MULTISAMPLE);
  if (shadowsOn_) {
    glBindFramebuffer(GL_FRAMEBUFFER, shadowFbo_);
    glViewport(0, 0, kShadowSize, kShadowSize);
    glClear(GL_DEPTH_BUFFER_BIT);
    glUseProgram(shadowProgram_);
    glUniformMatrix4fv(uShadowLightVP_, 1, GL_FALSE, lightVP_.m);
    glUniform1f(uShadowTime_, time_);
    glDisable(GL_CULL_FACE);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(2.0f, 4.0f);
    for (const auto& c : opaque_) {
      if (!c.castShadow || !c.mesh || !c.mesh->count) continue;
      glUniformMatrix4fv(uShadowModel_, 1, GL_FALSE, c.model.m);
      glBindVertexArray(c.mesh->vao);
      glDrawElements(GL_TRIANGLES, c.mesh->count, GL_UNSIGNED_INT, nullptr);
    }
    glDisable(GL_POLYGON_OFFSET_FILL);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
  }
  glViewport(0, 0, width_, height_);
  glEnable(GL_CULL_FACE);
  glCullFace(GL_BACK);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

  // Sky: one full-screen triangle behind everything.
  {
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    setCommonUniforms(skyProgram_, 0, viewProj_, cam_.eye);
    Vec3 f = directionFromYawPitch(cam_.yaw, cam_.pitch);
    Vec3 r = normalize(cross(f, {0, 1, 0}));
    Vec3 u = cross(r, f);
    float ty = std::tan(cam_.fovY * 0.5f), tx = ty * static_cast<float>(width_) / static_cast<float>(height_);
    glUniform3f(glGetUniformLocation(skyProgram_, "uForward"), f.x, f.y, f.z);
    glUniform3f(glGetUniformLocation(skyProgram_, "uRight"), r.x, r.y, r.z);
    glUniform3f(glGetUniformLocation(skyProgram_, "uUp"), u.x, u.y, u.z);
    glUniform2f(glGetUniformLocation(skyProgram_, "uTan"), tx, ty);
    glBindVertexArray(skyVao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
  }

  setCommonUniforms(program_, 0, viewProj_, cam_.eye);
  glUniformMatrix4fv(glGetUniformLocation(program_, "uLightVP"), 1, GL_FALSE, lightVP_.m);
  glUniform1f(glGetUniformLocation(program_, "uShadowOn"), shadowsOn_ ? 1.0f : 0.0f);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, shadowTex_);
  glUniform1i(glGetUniformLocation(program_, "uShadowMap"), 0);
  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, heightTex_);
  glUniform1i(glGetUniformLocation(program_, "uHeightMap"), 1);
  glUniform3f(glGetUniformLocation(program_, "uHeightInfo"), heightOriginX_, heightOriginZ_, heightExtent_);
  glActiveTexture(GL_TEXTURE0);
  for (const auto& c : opaque_) drawCmd(c);

  glDepthMask(GL_FALSE);
  if (drawWater_) drawCmd({&water_, Mat4::identity(), {1, 1, 1}, 1.0f, false});
  for (const auto& c : transparent_) drawCmd(c);
  glDepthMask(GL_TRUE);

  if (!viewModel_.empty()) {
    glClear(GL_DEPTH_BUFFER_BIT);  // the held item never clips into walls
    glUniform1i(glGetUniformLocation(program_, "uMode"), 2);
    glUniform1f(glGetUniformLocation(program_, "uShadowOn"), 0.0f);
    for (const auto& c : viewModel_) drawCmd(c);
  }
  glBindVertexArray(0);
}

bool Renderer::project(const Vec3& p, float& sx, float& sy) const {
  const Mat4& m = viewProj_;
  float x = m.at(0, 0) * p.x + m.at(0, 1) * p.y + m.at(0, 2) * p.z + m.at(0, 3);
  float y = m.at(1, 0) * p.x + m.at(1, 1) * p.y + m.at(1, 2) * p.z + m.at(1, 3);
  float w = m.at(3, 0) * p.x + m.at(3, 1) * p.y + m.at(3, 2) * p.z + m.at(3, 3);
  if (w <= 0.01f) return false;
  sx = (x / w) * 0.5f + 0.5f;
  sy = 1.0f - ((y / w) * 0.5f + 0.5f);
  return sx > -0.2f && sx < 1.2f && sy > -0.2f && sy < 1.2f;
}

}  // namespace sg
