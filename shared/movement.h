// Player movement. This one function runs on the server (authoritative) and on
// the client (prediction). Same inputs + same code = same result.
#pragma once
#include <cstdint>

#include "terrain.h"
#include "vecmath.h"
#include "world.h"

namespace sg {

constexpr float kInputHz = 60.0f;
constexpr float kInputDt = 1.0f / kInputHz;

constexpr float kWalkSpeed = 5.0f;     // m/s
constexpr float kSprintSpeed = 8.0f;   // m/s
constexpr float kGravity = 20.0f;      // m/s^2 (a bit above real, feels snappier)
constexpr float kJumpSpeed = 6.5f;     // m/s
constexpr float kAirControl = 0.25f;   // fraction of ground acceleration in the air
constexpr float kGroundAccel = 60.0f;  // m/s^2
constexpr float kEyeHeight = 1.65f;    // metres above feet
constexpr float kMaxPitch = 1.55f;     // just under 90 degrees
constexpr float kPlayerRadius = 0.35f;
constexpr float kPlayerHeight = 1.8f;
constexpr float kStepUp = 0.45f;       // ledges this high are walked onto

// Stamina: sprinting and jumping cost it; it comes back after a short rest.
constexpr float kMaxStamina = 100.0f;
constexpr float kSprintCost = 14.0f;      // per second
constexpr float kJumpCost = 10.0f;
constexpr float kStaminaRegen = 20.0f;    // per second (half when starving)
constexpr float kStaminaDelay = 0.8f;     // seconds of rest before regen starts
constexpr float kExhaustRecover = 25.0f;  // after running dry, sprint again at this much

enum InputButtons : uint8_t {
  kButtonJump = 1 << 0,
  kButtonSprint = 1 << 1,
  kButtonPrimary = 1 << 2,  // attack / gather / eat / shoot
};

struct PlayerInput {
  uint32_t seq = 0;    // increases by one per input; used for reconciliation
  float moveX = 0;     // strafe, -1..1
  float moveZ = 0;     // forward, -1..1
  float yaw = 0;       // radians
  float pitch = 0;     // radians
  uint8_t buttons = 0;
  uint8_t slot = 0;    // selected hotbar slot
};

struct PlayerState {
  Vec3 pos;
  Vec3 vel;
  float yaw = 0;
  float pitch = 0;
  bool onGround = false;
  float stamina = kMaxStamina;
  float staminaDelay = 0;
  bool exhausted = false;  // ran out; must recover before sprinting again
  bool starving = false;   // set by the server from hunger; slows stamina regen
};

// Clamps anything a hacked client could abuse (NaN, move vector > 1, pitch).
PlayerInput sanitizeInput(const PlayerInput& in);

// Advances the player by exactly one input step (kInputDt seconds).
// `world` may be null (terrain only).
void applyInput(PlayerState& s, const PlayerInput& in, const Terrain& terrain, const World* world = nullptr);

}  // namespace sg
