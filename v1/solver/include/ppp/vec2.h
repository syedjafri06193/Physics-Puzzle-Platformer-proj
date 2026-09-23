// Minimal 2D vector maths.
//
// Deliberately plain: no transcendentals anywhere in the solver's hot path
// (design doc 6.3 — libm differs between platforms and even between versions,
// and the one thing this solver must never do is drift). Length() uses
// sqrtf, which IEEE-754 requires to be correctly rounded, so it is safe.

#pragma once

#include <cmath>

namespace ppp {

struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;

    constexpr Vec2() = default;
    constexpr Vec2(float x_, float y_) : x(x_), y(y_) {}

    static constexpr Vec2 Zero() { return {0.0f, 0.0f}; }

    constexpr Vec2 operator+(const Vec2& o) const { return {x + o.x, y + o.y}; }
    constexpr Vec2 operator-(const Vec2& o) const { return {x - o.x, y - o.y}; }
    constexpr Vec2 operator*(float s) const { return {x * s, y * s}; }
    constexpr Vec2 operator/(float s) const { return {x / s, y / s}; }
    constexpr Vec2 operator-() const { return {-x, -y}; }

    Vec2& operator+=(const Vec2& o) { x += o.x; y += o.y; return *this; }
    Vec2& operator-=(const Vec2& o) { x -= o.x; y -= o.y; return *this; }
    Vec2& operator*=(float s) { x *= s; y *= s; return *this; }

    constexpr bool operator==(const Vec2& o) const { return x == o.x && y == o.y; }

    constexpr float LengthSquared() const { return x * x + y * y; }
    float Length() const { return std::sqrt(LengthSquared()); }

    static constexpr float Dot(const Vec2& a, const Vec2& b) { return a.x * b.x + a.y * b.y; }
    static constexpr float Cross(const Vec2& a, const Vec2& b) { return a.x * b.y - a.y * b.x; }

    /// Right-hand perpendicular: rotates (x, y) to (y, -x).
    constexpr Vec2 PerpCW() const { return {y, -x}; }

    Vec2 Normalized() const {
        const float len = Length();
        // Guarded, like every division in this file. One NaN propagates
        // through the constraints and destroys a body inside a frame
        // (doc 4.4).
        return len > 1e-9f ? Vec2{x / len, y / len} : Vec2{0.0f, 0.0f};
    }

    bool IsFinite() const { return std::isfinite(x) && std::isfinite(y); }
};

inline constexpr Vec2 operator*(float s, const Vec2& v) { return v * s; }

inline constexpr float Min(float a, float b) { return a < b ? a : b; }
inline constexpr float Max(float a, float b) { return a > b ? a : b; }
inline constexpr float Clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

/// An axis-aligned box. Static geometry, trigger regions and the player's
/// body are all boxes, which keeps collision exact and cheap and makes the
/// minimum-wall-thickness rule (doc 4.5) something a validator can check.
struct AABB {
    Vec2 min;
    Vec2 max;

    constexpr AABB() = default;
    constexpr AABB(const Vec2& mn, const Vec2& mx) : min(mn), max(mx) {}

    static constexpr AABB FromRect(float x, float y, float w, float h) {
        return {{x, y}, {x + w, y + h}};
    }

    constexpr float Width() const { return max.x - min.x; }
    constexpr float Height() const { return max.y - min.y; }
    constexpr Vec2 Center() const { return {(min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f}; }
    constexpr float Thickness() const { return Min(Width(), Height()); }

    constexpr bool Contains(const Vec2& p) const {
        return p.x >= min.x && p.x <= max.x && p.y >= min.y && p.y <= max.y;
    }

    constexpr bool Overlaps(const AABB& o) const {
        return min.x < o.max.x && max.x > o.min.x && min.y < o.max.y && max.y > o.min.y;
    }

    constexpr AABB Expanded(float r) const {
        return {{min.x - r, min.y - r}, {max.x + r, max.y + r}};
    }
};

}  // namespace ppp
