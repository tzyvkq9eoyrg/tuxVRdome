#include "vr_space.h"

#include "menu.h"

#include <cmath>

namespace {

constexpr float kPanelWidth = 1.0f;     // meters
constexpr float kPanelDistance = 0.9f;  // meters in front of the head
constexpr float kPanelDrop = 0.25f;     // meters below eye level

}  // namespace

float wrapAngle(float a) {
  while (a > kPi) a -= 2 * kPi;
  while (a < -kPi) a += 2 * kPi;
  return a;
}

Vec3 forwardFromYaw(float yaw) { return {-std::sin(yaw), 0.0f, -std::cos(yaw)}; }

Mat4 headFrame(Quat headOrientation, bool lying) {
  return lying ? rotation(headOrientation) : rotationY(yawFromQuat(headOrientation));
}

Mat4 poseMatrix(Vec3 position, Quat orientation) {
  return translation(position) * rotation(orientation);
}

Mat4 poseInverse(Vec3 position, Quat orientation) {
  return transposedRotation(rotation(orientation)) * translation(position * -1.0f);
}

bool facingBasis(Vec3 z, Vec3 up, Mat4& out) {
  const Vec3 x = cross(up, z);
  if (length(x) < 1e-3f) return false;
  const Vec3 xn = normalize(x), y = cross(z, xn);
  out = Mat4();
  out.m[0] = xn.x, out.m[1] = xn.y, out.m[2] = xn.z;
  out.m[4] = y.x, out.m[5] = y.y, out.m[6] = y.z;
  out.m[8] = z.x, out.m[9] = z.y, out.m[10] = z.z;
  return true;
}

bool PanelGeometry::hit(Vec3 origin, Vec3 dir, float& u, float& v, float& distance) const {
  const Mat4 toLocal = transposedRotation(orientation);
  const Vec3 o = transformDir(toLocal, origin - center);
  const Vec3 d = transformDir(toLocal, dir);
  if (d.z > -1e-4f) return false;  // pointing away from or along the panel
  const float t = -o.z / d.z;
  if (t <= 0) return false;
  const float x = o.x + d.x * t, y = o.y + d.y * t;
  if (std::abs(x) > width / 2 || std::abs(y) > height / 2) return false;
  u = x / width + 0.5f;
  v = 0.5f - y / height;
  distance = t;
  return true;
}

void MenuPanels::placeInFrontOf(Vec3 headPosition, const Mat4& frame) {
  anchor = headPosition + transformDir(frame, {0.0f, -kPanelDrop, -kPanelDistance});
  const Vec3 up = transformDir(frame, {0.0f, 1.0f, 0.0f});
  if (!facingBasis(normalize(headPosition - anchor), up, orientation)) orientation = frame;
}

PanelGeometry MenuPanels::main(int visibleHeightPx) const {
  PanelGeometry g;
  g.center = anchor;
  g.orientation = orientation;
  g.width = kPanelWidth;
  g.height = kPanelWidth * visibleHeightPx / Menu::kMainWidth;
  return g;
}

PanelGeometry MenuPanels::format() const {
  PanelGeometry g;
  g.orientation = orientation * rotationY(-0.35f);  // ~20 degrees toward the user
  g.width = kPanelWidth * Menu::kFormatWidth / Menu::kMainWidth;
  g.height = g.width * Menu::kFormatHeight / Menu::kFormatWidth;
  const Vec3 hinge = anchor + transformDir(orientation, {kPanelWidth / 2 + 0.03f, 0.0f, 0.0f});
  g.center = hinge + transformDir(g.orientation, {g.width / 2, 0.0f, 0.0f});
  return g;
}
