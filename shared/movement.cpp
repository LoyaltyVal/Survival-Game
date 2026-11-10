#include "movement.h"

#include <cmath>

namespace sg {

static float finiteOr(float v, float fallback) { return std::isfinite(v) ? v : fallback; }

PlayerInput sanitizeInput(const PlayerInput& in) {
  PlayerInput out = in;
  out.moveX = clampf(finiteOr(in.moveX, 0), -1, 1);
  out.moveZ = clampf(finiteOr(in.moveZ, 0), -1, 1);
  float len = std::sqrt(out.moveX * out.moveX + out.moveZ * out.moveZ);
  if (len > 1.0f) { out.moveX /= len; out.moveZ /= len; }  // no diagonal speed boost
  out.yaw = std::fmod(finiteOr(in.yaw, 0), 2.0f * kPi);
  out.pitch = clampf(finiteOr(in.pitch, 0), -kMaxPitch, kMaxPitch);
  out.buttons = in.buttons & (kButtonJump | kButtonSprint);
  return out;
}

void applyInput(PlayerState& s, const PlayerInput& rawIn, const Terrain& terrain) {
  const PlayerInput in = sanitizeInput(rawIn);
  const float dt = kInputDt;

  s.yaw = in.yaw;
  s.pitch = in.pitch;

  // Desired horizontal velocity from the stick, relative to where we look.
  Vec3 forward{-std::sin(s.yaw), 0, -std::cos(s.yaw)};
  Vec3 right{std::cos(s.yaw), 0, -std::sin(s.yaw)};
  float speed = (in.buttons & kButtonSprint) && in.moveZ > 0 ? kSprintSpeed : kWalkSpeed;
  Vec3 wish = (forward * in.moveZ + right * in.moveX) * speed;

  // Accelerate toward the wished velocity (less control while airborne).
  float accel = kGroundAccel * (s.onGround ? 1.0f : kAirControl) * dt;
  Vec3 horiz{s.vel.x, 0, s.vel.z};
  Vec3 diff = wish - horiz;
  float dl = length(diff);
  if (dl > accel) diff = diff * (accel / dl);
  s.vel.x += diff.x;
  s.vel.z += diff.z;

  // Jump and gravity.
  if (s.onGround && (in.buttons & kButtonJump)) {
    s.vel.y = kJumpSpeed;
    s.onGround = false;
  }
  s.vel.y -= kGravity * dt;

  s.pos += s.vel * dt;

  // Stay on the island.
  s.pos.x = clampf(s.pos.x, 1.0f, kWorldSize - 1.0f);
  s.pos.z = clampf(s.pos.z, 1.0f, kWorldSize - 1.0f);

  // Ground collision against the heightfield. (Bullet replaces this for
  // buildings and rocks; terrain stays a heightfield.)
  float ground = terrain.heightAt(s.pos.x, s.pos.z);
  if (s.pos.y <= ground) {
    s.pos.y = ground;
    if (s.vel.y < 0) s.vel.y = 0;
    s.onGround = true;
  } else {
    // Snap down small steps when walking downhill so we don't "skip".
    s.onGround = s.onGround && s.vel.y <= 0 && (s.pos.y - ground) < 0.3f;
    if (s.onGround) s.pos.y = ground;
  }
}

}  // namespace sg
