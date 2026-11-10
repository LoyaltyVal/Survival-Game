// Small vector/matrix library. Column-major matrices to match OpenGL.
#pragma once
#include <cmath>

namespace sg {

constexpr float kPi = 3.14159265358979323846f;

struct Vec2 { float x = 0, y = 0; };

struct Vec3 {
  float x = 0, y = 0, z = 0;
  constexpr Vec3() = default;
  constexpr Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
  Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
  Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
  Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
  Vec3 operator/(float s) const { return {x / s, y / s, z / s}; }
  Vec3& operator+=(const Vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
  Vec3& operator-=(const Vec3& o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
  Vec3& operator*=(float s) { x *= s; y *= s; z *= s; return *this; }
};

inline float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(const Vec3& a, const Vec3& b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float length(const Vec3& v) { return std::sqrt(dot(v, v)); }
inline Vec3 normalize(const Vec3& v) {
  float l = length(v);
  return l > 1e-6f ? v / l : Vec3{0, 0, 0};
}
inline Vec3 lerp(const Vec3& a, const Vec3& b, float t) { return a + (b - a) * t; }
inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Shortest-path interpolation between two angles in radians.
inline float lerpAngle(float a, float b, float t) {
  float d = std::fmod(b - a + 3.0f * kPi, 2.0f * kPi) - kPi;
  return a + d * t;
}

struct Mat4 {
  float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};  // column-major
  float& at(int row, int col) { return m[col * 4 + row]; }
  float at(int row, int col) const { return m[col * 4 + row]; }

  static Mat4 identity() { return {}; }

  Mat4 operator*(const Mat4& o) const {
    Mat4 r;
    for (int c = 0; c < 4; ++c)
      for (int rr = 0; rr < 4; ++rr) {
        float s = 0;
        for (int k = 0; k < 4; ++k) s += at(rr, k) * o.at(k, c);
        r.at(rr, c) = s;
      }
    return r;
  }

  static Mat4 translate(const Vec3& t) {
    Mat4 r;
    r.at(0, 3) = t.x; r.at(1, 3) = t.y; r.at(2, 3) = t.z;
    return r;
  }
  static Mat4 scale(const Vec3& s) {
    Mat4 r;
    r.at(0, 0) = s.x; r.at(1, 1) = s.y; r.at(2, 2) = s.z;
    return r;
  }
  static Mat4 rotateY(float a) {
    Mat4 r;
    float c = std::cos(a), s = std::sin(a);
    r.at(0, 0) = c;  r.at(0, 2) = s;
    r.at(2, 0) = -s; r.at(2, 2) = c;
    return r;
  }
  static Mat4 perspective(float fovyRad, float aspect, float zNear, float zFar) {
    Mat4 r;
    float f = 1.0f / std::tan(fovyRad * 0.5f);
    r.at(0, 0) = f / aspect;
    r.at(1, 1) = f;
    r.at(2, 2) = (zFar + zNear) / (zNear - zFar);
    r.at(2, 3) = (2.0f * zFar * zNear) / (zNear - zFar);
    r.at(3, 2) = -1.0f;
    r.at(3, 3) = 0.0f;
    return r;
  }
  static Mat4 lookAt(const Vec3& eye, const Vec3& target, const Vec3& up) {
    Vec3 f = normalize(target - eye);
    Vec3 s = normalize(cross(f, up));
    Vec3 u = cross(s, f);
    Mat4 r;
    r.at(0, 0) = s.x;  r.at(0, 1) = s.y;  r.at(0, 2) = s.z;  r.at(0, 3) = -dot(s, eye);
    r.at(1, 0) = u.x;  r.at(1, 1) = u.y;  r.at(1, 2) = u.z;  r.at(1, 3) = -dot(u, eye);
    r.at(2, 0) = -f.x; r.at(2, 1) = -f.y; r.at(2, 2) = -f.z; r.at(2, 3) = dot(f, eye);
    return r;
  }
};

// Forward direction for a yaw/pitch pair. Yaw 0 looks down -Z.
inline Vec3 directionFromYawPitch(float yaw, float pitch) {
  return {-std::sin(yaw) * std::cos(pitch), std::sin(pitch), -std::cos(yaw) * std::cos(pitch)};
}

}  // namespace sg
