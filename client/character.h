// Procedural player character: a jointed humanoid (torso, head, two-bone arms
// and legs, hands, shoes) plus clothing/armour pieces that ride on the same
// bones. Every mesh is built in its bone's local space: the joint is the
// origin and limbs hang down -Y, with the front of the body facing -Z.
#pragma once
#include <cstdint>

#include "items.h"
#include "meshes.h"
#include "vecmath.h"

namespace sg {

enum class BodyPart : uint8_t { Pelvis, Torso, Head, UpperArm, Forearm, Hand, Thigh, Shin, Foot, Count };
constexpr int kBodyParts = static_cast<int>(BodyPart::Count);

// The bare body and its everyday clothes. Skin/hair/shirt/pants meshes are white
// and get tinted per player; Face, Belt and Shoe carry their own colours.
enum class BodyMesh : uint8_t {
  HeadSkin, Face, Brows, HairShort, HairBuzz, HairLong, Beard,
  UpperArmSkin, ForearmSkin, HandSkin,
  Torso, Sleeve, Pelvis, Belt, Thigh, Shin, Shoe,
  Count
};
constexpr int kBodyMeshes = static_cast<int>(BodyMesh::Count);
constexpr int kHairStyles = 4;  // short, buzz, long, bald

// Bone lengths (metres) and joint positions, shared by the meshes and the animation.
constexpr float kUpperArmLen = 0.28f, kForearmLen = 0.25f;
constexpr float kThighLen = 0.43f, kShinLen = 0.42f;
constexpr float kWaistHeight = 1.0f;         // pelvis/torso bone origin above the feet
constexpr float kNeckHeight = 0.45f;         // head bone origin above the waist
constexpr float kShoulderX = 0.2f, kShoulderY = 0.39f;  // shoulder joints relative to the waist
constexpr float kHipX = 0.095f, kHipY = -0.07f;         // hip joints relative to the waist

BodyPart bodyMeshPart(BodyMesh m);
void buildBodyMesh(BodyMesh m, const Mat4& at, MeshBuilder& mb);
// The piece of an armour item that sits on this body part (adds nothing if it doesn't cover it).
void buildArmorMesh(Item item, BodyPart part, const Mat4& at, MeshBuilder& mb);
// A whole armour item laid out on an invisible body: inventory icon and held model.
void buildArmorDisplay(Item item, MeshBuilder& mb);

}  // namespace sg
