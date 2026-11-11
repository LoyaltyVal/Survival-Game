#include "movement.h"

#include <algorithm>
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
  out.buttons = in.buttons & (kButtonJump | kButtonSprint | kButtonPrimary | kButtonReload | kButtonAim);
  out.slot = in.slot < 6 ? in.slot : 0;
  return out;
}

static AABB bodyAt(const Vec3& p) {
  return {{p.x - kPlayerRadius, p.y + kStepUp, p.z - kPlayerRadius},
          {p.x + kPlayerRadius, p.y + kPlayerHeight, p.z + kPlayerRadius}};
}

static float groundUnder(const PlayerState& s, const Terrain& terrain, const World* world, float feetY) {
  return world ? world->groundHeight(terrain, s.pos.x, s.pos.z, kPlayerRadius, feetY, kStepUp) : terrain.heightAt(s.pos.x, s.pos.z);
}

bool isSwimming(const PlayerState& s, const Terrain& terrain, const World* world) {
  return s.pos.y < kSwimFeetY + 0.25f && groundUnder(s, terrain, world, s.pos.y) < kSwimFeetY - 0.05f;
}

void applyInput(PlayerState& s, const PlayerInput& rawIn, const Terrain& terrain, const World* world) {
  const PlayerInput in = sanitizeInput(rawIn);
  const float dt = kInputDt;

  s.yaw = in.yaw;
  s.pitch = in.pitch;

  // Desired horizontal velocity from the stick, relative to where we look.
  Vec3 forward{-std::sin(s.yaw), 0, -std::cos(s.yaw)};
  Vec3 right{std::cos(s.yaw), 0, -std::sin(s.yaw)};
  const bool swimming = isSwimming(s, terrain, world);
  const bool climbing = world && world->onLadder({{s.pos.x - kPlayerRadius, s.pos.y, s.pos.z - kPlayerRadius},
                                                  {s.pos.x + kPlayerRadius, s.pos.y + 0.6f, s.pos.z + kPlayerRadius}});
  bool wantSprint = (in.buttons & kButtonSprint) && in.moveZ > 0;
  const bool aiming = (in.buttons & kButtonAim) != 0 && !swimming;
  bool sprinting = wantSprint && !aiming && !s.exhausted && s.stamina > 0;
  float speed = swimming ? (sprinting ? kSwimFastSpeed : kSwimSpeed) : sprinting ? kSprintSpeed : kWalkSpeed;
  if (aiming) speed *= kAimSpeedFactor;
  Vec3 wish = (forward * in.moveZ + right * in.moveX) * speed;

  // Accelerate toward the wished velocity (less control while airborne).
  float accel = kGroundAccel * (s.onGround || climbing ? 1.0f : swimming ? 0.12f : kAirControl) * dt;
  Vec3 horiz{s.vel.x, 0, s.vel.z};
  Vec3 diff = wish - horiz;
  float dl = length(diff);
  if (dl > accel) diff = diff * (accel / dl);
  s.vel.x += diff.x;
  s.vel.z += diff.z;

  // Stamina: sprinting while moving drains it, resting refills it.
  bool spent = false;
  float hspeed = std::sqrt(s.vel.x * s.vel.x + s.vel.z * s.vel.z);
  if (sprinting && (s.onGround || swimming) && hspeed > 0.5f) {
    s.stamina -= kSprintCost * dt;
    spent = true;
  }

  // Jump and gravity.
  if (s.onGround && !swimming && !climbing && (in.buttons & kButtonJump) && s.stamina >= kJumpCost) {
    s.vel.y = kJumpSpeed;
    s.onGround = false;
    s.stamina -= kJumpCost;
    spent = true;
  }
  if (spent) {
    s.staminaDelay = kStaminaDelay;
    if (s.stamina <= 0) {
      s.stamina = 0;
      s.exhausted = true;
    }
  } else if (s.staminaDelay > 0) {
    s.staminaDelay = std::max(0.0f, s.staminaDelay - dt);
  } else {
    s.stamina = std::min(kMaxStamina, s.stamina + kStaminaRegen * (s.starving ? 0.5f : 1.0f) * dt);
  }
  if (s.exhausted && s.stamina >= kExhaustRecover) s.exhausted = false;

  if (climbing) {
    // Ladders: forward or jump climbs, back climbs down, otherwise hang on.
    float want = (in.moveZ > 0.1f || (in.buttons & kButtonJump)) ? kClimbSpeed : in.moveZ < -0.1f ? -kClimbSpeed : 0.0f;
    s.vel.y = want;
  } else if (swimming) {
    // Buoyancy pulls the feet to the floating depth; water drag eats a fall.
    float target = clampf((kSwimFeetY - s.pos.y) * 3.0f, -2.5f, 2.5f);
    s.vel.y += (target - s.vel.y) * std::min(1.0f, 6.0f * dt);
  } else {
    s.vel.y -= kGravity * dt;
  }

  // Horizontal movement, one axis at a time so we slide along walls.
  const Vec3 old = s.pos;
  const bool stuck = world && world->blocked(bodyAt(old));  // let players walk out of anything they spawned in
  s.pos.x += s.vel.x * dt;
  if (world && !stuck && world->blocked(bodyAt(s.pos))) {
    s.pos.x = old.x;
    s.vel.x = 0;
  }
  s.pos.z += s.vel.z * dt;
  if (world && !stuck && world->blocked(bodyAt(s.pos))) {
    s.pos.z = old.z;
    s.vel.z = 0;
  }

  // Vertical: bump the head on ceilings.
  const float prevY = s.pos.y;
  s.pos.y += s.vel.y * dt;
  if (world && !stuck && s.vel.y > 0 && world->blocked(bodyAt(s.pos))) {
    s.pos.y = prevY;
    s.vel.y = 0;
  }

  // Stay on the island.
  s.pos.x = clampf(s.pos.x, 1.0f, kWorldSize - 1.0f);
  s.pos.z = clampf(s.pos.z, 1.0f, kWorldSize - 1.0f);

  // Ground: terrain heightfield plus the tops of foundations, floors and stairs.
  float ground = world ? world->groundHeight(terrain, s.pos.x, s.pos.z, kPlayerRadius, std::max(prevY, s.pos.y), kStepUp)
                       : terrain.heightAt(s.pos.x, s.pos.z);
  if (s.pos.y <= ground) {
    s.pos.y = ground;
    if (s.vel.y < 0) s.vel.y = 0;
    s.onGround = true;
  } else if (swimming || climbing) {
    s.onGround = false;
  } else {
    // Snap down small steps when walking downhill so we don't "skip".
    s.onGround = s.onGround && s.vel.y <= 0 && (s.pos.y - ground) < 0.3f;
    if (s.onGround) s.pos.y = ground;
  }
}

}  // namespace sg
