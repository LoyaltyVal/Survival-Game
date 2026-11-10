#include "terrain.h"

#include <cmath>

namespace sg {

float Terrain::hash(int32_t x, int32_t z) const {
  uint32_t h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(z) * 668265263u + seed_ * 2246822519u;
  h = (h ^ (h >> 13)) * 1274126177u;
  h ^= h >> 16;
  return static_cast<float>(h & 0xFFFFFF) / static_cast<float>(0xFFFFFF);  // 0..1
}

float Terrain::valueNoise(float x, float z) const {
  float fx = std::floor(x), fz = std::floor(z);
  int32_t ix = static_cast<int32_t>(fx), iz = static_cast<int32_t>(fz);
  float tx = x - fx, tz = z - fz;
  // smoothstep fade
  tx = tx * tx * (3.0f - 2.0f * tx);
  tz = tz * tz * (3.0f - 2.0f * tz);
  float a = hash(ix, iz), b = hash(ix + 1, iz);
  float c = hash(ix, iz + 1), d = hash(ix + 1, iz + 1);
  return lerpf(lerpf(a, b, tx), lerpf(c, d, tx), tz);
}

float Terrain::heightAt(float x, float z) const {
  // Fractal noise: big hills plus small bumps.
  float h = 0.0f, amp = 1.0f, freq = 1.0f / 512.0f, norm = 0.0f;
  for (int octave = 0; octave < 5; ++octave) {
    h += valueNoise(x * freq, z * freq) * amp;
    norm += amp;
    amp *= 0.5f;
    freq *= 2.0f;
  }
  h /= norm;  // 0..1

  // Island mask: falls to the sea near the map edge, giving beaches to spawn on.
  float cx = x / kWorldSize - 0.5f, cz = z / kWorldSize - 0.5f;
  float edge = std::sqrt(cx * cx + cz * cz) * 2.0f;  // 0 at centre, 1 at mid-edge
  float mask = clampf(1.0f - (edge - 0.6f) / 0.35f, 0.0f, 1.0f);

  return (h * 90.0f - 12.0f) * mask - 8.0f * (1.0f - mask);
}

Vec3 Terrain::normalAt(float x, float z) const {
  const float e = 0.5f;
  float hl = heightAt(x - e, z), hr = heightAt(x + e, z);
  float hd = heightAt(x, z - e), hu = heightAt(x, z + e);
  return normalize(Vec3{hl - hr, 2.0f * e, hd - hu});
}

}  // namespace sg
