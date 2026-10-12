#pragma once

#include "xmath.h"

// Geometry in the room: angles, poses, and where the menu panels float.

constexpr float kPi = 3.14159265f;

// An angle brought into -pi..pi.
float wrapAngle(float a);
// The horizontal direction a heading (yaw) looks along.
Vec3 forwardFromYaw(float yaw);
// The frame things are placed in front of: the head's heading, or in lying
// mode its full orientation, so "front" can point at the ceiling.
Mat4 headFrame(Quat headOrientation, bool lying);
// A tracked pose as a matrix, and its inverse.
Mat4 poseMatrix(Vec3 position, Quat orientation);
Mat4 poseInverse(Vec3 position, Quat orientation);
// Rotation whose +Z axis points along `z` and whose Y axis is as close to
// `up` as possible. False if `z` points (nearly) straight along `up`.
bool facingBasis(Vec3 z, Vec3 up, Mat4& out);

// A flat rectangle floating in the room, facing back along its -Z axis.
struct PanelGeometry {
  Vec3 center;
  Mat4 orientation;
  float width = 1.0f, height = 1.0f;

  Mat4 model() const { return translation(center) * orientation * scaling(width, height, 1); }
  // Intersects a ray with the panel; u/v in 0..1 with v = 0 at the top.
  bool hit(Vec3 origin, Vec3 dir, float& u, float& v, float& distance) const;
};

// The menu panels: the main panel in front of the user when the menu opens,
// and the format panel hinged to its right edge, turned toward the user.
struct MenuPanels {
  bool open = false;
  Vec3 anchor;
  Mat4 orientation;

  // In front of and a little below the eyes, tilted to face them, the same
  // way it faces them while held with the grip (so grabbing does not tilt it).
  void placeInFrontOf(Vec3 headPosition, const Mat4& frame);
  PanelGeometry main(int visibleHeightPx) const;
  PanelGeometry format() const;
};
