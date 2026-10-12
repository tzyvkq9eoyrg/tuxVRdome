#pragma once

#include <cmath>

struct Vec3 {
  float x = 0, y = 0, z = 0;
};

struct Quat {
  float x = 0, y = 0, z = 0, w = 1;
};

inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(Vec3 a, Vec3 b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float length(Vec3 a) { return std::sqrt(dot(a, a)); }
inline Vec3 normalize(Vec3 a) {
  const float len = length(a);
  return len > 0 ? a * (1.0f / len) : a;
}

// Column-major 4x4 matrix (OpenGL convention), identity by default.
struct Mat4 {
  float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
};

inline Mat4 operator*(const Mat4& a, const Mat4& b) {
  Mat4 r;
  for (int c = 0; c < 4; ++c) {
    for (int row = 0; row < 4; ++row) {
      float s = 0;
      for (int k = 0; k < 4; ++k) s += a.m[k * 4 + row] * b.m[c * 4 + k];
      r.m[c * 4 + row] = s;
    }
  }
  return r;
}

inline Mat4 translation(Vec3 t) {
  Mat4 r;
  r.m[12] = t.x;
  r.m[13] = t.y;
  r.m[14] = t.z;
  return r;
}

inline Mat4 scaling(float x, float y, float z) {
  Mat4 r;
  r.m[0] = x;
  r.m[5] = y;
  r.m[10] = z;
  return r;
}

inline Mat4 rotationY(float radians) {
  Mat4 r;
  const float c = std::cos(radians), s = std::sin(radians);
  r.m[0] = c;
  r.m[2] = -s;
  r.m[8] = s;
  r.m[10] = c;
  return r;
}

inline Mat4 rotation(Quat q) {
  const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
  const float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
  const float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
  Mat4 r;
  r.m[0] = 1 - 2 * (yy + zz);
  r.m[1] = 2 * (xy + wz);
  r.m[2] = 2 * (xz - wy);
  r.m[4] = 2 * (xy - wz);
  r.m[5] = 1 - 2 * (xx + zz);
  r.m[6] = 2 * (yz + wx);
  r.m[8] = 2 * (xz + wy);
  r.m[9] = 2 * (yz - wx);
  r.m[10] = 1 - 2 * (xx + yy);
  return r;
}

// Inverse of a pure rotation matrix.
inline Mat4 transposedRotation(const Mat4& r) {
  Mat4 t;
  for (int c = 0; c < 3; ++c)
    for (int row = 0; row < 3; ++row) t.m[c * 4 + row] = r.m[row * 4 + c];
  return t;
}

// Inverse of a rigid pose: world -> eye.
inline Mat4 viewFromPose(Quat q, Vec3 p) {
  const Mat4 r = rotation(q);
  Mat4 v;
  for (int c = 0; c < 3; ++c)
    for (int row = 0; row < 3; ++row) v.m[c * 4 + row] = r.m[row * 4 + c];
  v.m[12] = -(v.m[0] * p.x + v.m[4] * p.y + v.m[8] * p.z);
  v.m[13] = -(v.m[1] * p.x + v.m[5] * p.y + v.m[9] * p.z);
  v.m[14] = -(v.m[2] * p.x + v.m[6] * p.y + v.m[10] * p.z);
  return v;
}

// Asymmetric OpenGL projection from OpenXR field-of-view angles (radians).
inline Mat4 projectionFromFov(float left, float right, float up, float down,
                              float nearZ, float farZ) {
  const float tl = std::tan(left), tr = std::tan(right);
  const float tu = std::tan(up), td = std::tan(down);
  const float w = tr - tl, h = tu - td;
  Mat4 p;
  p.m[0] = 2 / w;
  p.m[5] = 2 / h;
  p.m[8] = (tr + tl) / w;
  p.m[9] = (tu + td) / h;
  p.m[10] = -(farZ + nearZ) / (farZ - nearZ);
  p.m[11] = -1;
  p.m[14] = -2 * farZ * nearZ / (farZ - nearZ);
  p.m[15] = 0;
  return p;
}

inline Vec3 transformPoint(const Mat4& m, Vec3 p) {
  return {m.m[0] * p.x + m.m[4] * p.y + m.m[8] * p.z + m.m[12],
          m.m[1] * p.x + m.m[5] * p.y + m.m[9] * p.z + m.m[13],
          m.m[2] * p.x + m.m[6] * p.y + m.m[10] * p.z + m.m[14]};
}

inline Vec3 transformDir(const Mat4& m, Vec3 d) {
  return {m.m[0] * d.x + m.m[4] * d.y + m.m[8] * d.z,
          m.m[1] * d.x + m.m[5] * d.y + m.m[9] * d.z,
          m.m[2] * d.x + m.m[6] * d.y + m.m[10] * d.z};
}

// Heading (rotation around +Y) of an orientation, ignoring pitch and roll.
inline float yawFromQuat(Quat q) {
  const Mat4 r = rotation(q);
  return std::atan2(r.m[8], r.m[10]);
}
