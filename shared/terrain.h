// Deterministic terrain height. Client and server call the same function with
// the same seed, so the server never has to send terrain over the network.
#pragma once
#include <cstdint>
#include "vecmath.h"

namespace sg {

constexpr float kWorldSize = 4096.0f;  // metres, square island from 0 to kWorldSize
constexpr float kSeaLevel = 0.0f;

class Terrain {
 public:
  explicit Terrain(uint32_t seed = 1337) : seed_(seed) {}

  // Ground height in metres at world position (x, z).
  float heightAt(float x, float z) const;

  // Surface normal from central differences.
  Vec3 normalAt(float x, float z) const;

  uint32_t seed() const { return seed_; }

 private:
  float valueNoise(float x, float z) const;
  float hash(int32_t x, int32_t z) const;
  uint32_t seed_;
};

}  // namespace sg
