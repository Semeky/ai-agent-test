// Small, dependency-free 3D math used by the software renderer.
#pragma once

#include <algorithm>
#include <cmath>

namespace g3d {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDegToRad = kPi / 180.0;

inline double clampd(double value, double low, double high) {
    return std::max(low, std::min(high, value));
}

inline double lerp(double from, double to, double amount) {
    return from + (to - from) * amount;
}

inline double smoothstep(double edge0, double edge1, double value) {
    if (edge0 == edge1) {
        return value < edge0 ? 0.0 : 1.0;
    }
    const double t = clampd((value - edge0) / (edge1 - edge0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

inline double normalizeDegrees(double degrees) {
    double result = std::fmod(degrees, 360.0);
    if (result < 0.0) {
        result += 360.0;
    }
    return result;
}

struct Vec2 {
    double x = 0.0;
    double y = 0.0;
};

inline Vec2 operator-(Vec2 v) { return Vec2{-v.x, -v.y}; }
inline Vec2 operator+(Vec2 a, Vec2 b) { return Vec2{a.x + b.x, a.y + b.y}; }
inline Vec2 operator-(Vec2 a, Vec2 b) { return Vec2{a.x - b.x, a.y - b.y}; }
inline Vec2 operator*(Vec2 v, double s) { return Vec2{v.x * s, v.y * s}; }
inline Vec2 operator*(double s, Vec2 v) { return Vec2{v.x * s, v.y * s}; }
inline Vec2 operator/(Vec2 v, double s) { return Vec2{v.x / s, v.y / s}; }
inline Vec2& operator+=(Vec2& a, Vec2 b) {
    a.x += b.x;
    a.y += b.y;
    return a;
}
inline Vec2& operator-=(Vec2& a, Vec2 b) {
    a.x -= b.x;
    a.y -= b.y;
    return a;
}
inline double dot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }
inline double cross(Vec2 a, Vec2 b) { return a.x * b.y - a.y * b.x; }
inline double length(Vec2 v) { return std::hypot(v.x, v.y); }
inline double lengthSquared(Vec2 v) { return v.x * v.x + v.y * v.y; }
inline Vec2 normalized(Vec2 v) {
    const double len = length(v);
    return len > 1e-12 ? Vec2{v.x / len, v.y / len} : Vec2{0.0, 0.0};
}
inline Vec2 perpendicular(Vec2 v) { return Vec2{-v.y, v.x}; }
inline Vec2 directionFromDegrees(double degrees) {
    const double radians = degrees * kDegToRad;
    return Vec2{std::cos(radians), std::sin(radians)};
}

struct Vec3 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

inline Vec3 operator-(Vec3 v) { return Vec3{-v.x, -v.y, -v.z}; }
inline Vec3 operator+(Vec3 a, Vec3 b) { return Vec3{a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return Vec3{a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(Vec3 v, double s) { return Vec3{v.x * s, v.y * s, v.z * s}; }
inline Vec3 operator*(double s, Vec3 v) { return Vec3{v.x * s, v.y * s, v.z * s}; }
inline Vec3 operator/(Vec3 v, double s) { return Vec3{v.x / s, v.y / s, v.z / s}; }
inline Vec3& operator+=(Vec3& a, Vec3 b) {
    a.x += b.x;
    a.y += b.y;
    a.z += b.z;
    return a;
}
inline Vec3& operator-=(Vec3& a, Vec3 b) {
    a.x -= b.x;
    a.y -= b.y;
    a.z -= b.z;
    return a;
}
inline double dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(Vec3 a, Vec3 b) {
    return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline double length(Vec3 v) { return std::sqrt(dot(v, v)); }
inline double lengthSquared(Vec3 v) { return dot(v, v); }
inline Vec3 normalized(Vec3 v) {
    const double len = length(v);
    return len > 1e-12 ? v / len : Vec3{0.0, 0.0, 0.0};
}

struct Vec4 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    double w = 0.0;
};

inline Vec4 operator+(Vec4 a, Vec4 b) {
    return Vec4{a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w};
}
inline Vec4 operator*(Vec4 v, double s) { return Vec4{v.x * s, v.y * s, v.z * s, v.w * s}; }

// Row-major 4x4 matrix: m[row][column].  Vectors are column vectors, so
// transforming a point is matrix * vector.
struct Mat4 {
    double m[4][4]{};
};

inline Mat4 identityMatrix() {
    Mat4 result;
    for (int i = 0; i < 4; ++i) {
        result.m[i][i] = 1.0;
    }
    return result;
}

inline Mat4 operator*(const Mat4& a, const Mat4& b) {
    Mat4 result;
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            double sum = 0.0;
            for (int k = 0; k < 4; ++k) {
                sum += a.m[row][k] * b.m[k][column];
            }
            result.m[row][column] = sum;
        }
    }
    return result;
}

inline Vec4 operator*(const Mat4& matrix, Vec4 vector) {
    return Vec4{
        matrix.m[0][0] * vector.x + matrix.m[0][1] * vector.y +
            matrix.m[0][2] * vector.z + matrix.m[0][3] * vector.w,
        matrix.m[1][0] * vector.x + matrix.m[1][1] * vector.y +
            matrix.m[1][2] * vector.z + matrix.m[1][3] * vector.w,
        matrix.m[2][0] * vector.x + matrix.m[2][1] * vector.y +
            matrix.m[2][2] * vector.z + matrix.m[2][3] * vector.w,
        matrix.m[3][0] * vector.x + matrix.m[3][1] * vector.y +
            matrix.m[3][2] * vector.z + matrix.m[3][3] * vector.w};
}

inline Vec3 transformPoint(const Mat4& matrix, Vec3 point) {
    const Vec4 result = matrix * Vec4{point.x, point.y, point.z, 1.0};
    if (std::fabs(result.w) < 1e-12) {
        return Vec3{0.0, 0.0, 0.0};
    }
    return Vec3{result.x / result.w, result.y / result.w, result.z / result.w};
}

inline Vec3 transformDirection(const Mat4& matrix, Vec3 direction) {
    const Vec4 result = matrix * Vec4{direction.x, direction.y, direction.z, 0.0};
    return Vec3{result.x, result.y, result.z};
}

inline Mat4 translationMatrix(Vec3 offset) {
    Mat4 result = identityMatrix();
    result.m[0][3] = offset.x;
    result.m[1][3] = offset.y;
    result.m[2][3] = offset.z;
    return result;
}

inline Mat4 scaleMatrix(Vec3 scale) {
    Mat4 result = identityMatrix();
    result.m[0][0] = scale.x;
    result.m[1][1] = scale.y;
    result.m[2][2] = scale.z;
    return result;
}

inline Mat4 rotationXMatrix(double radians) {
    const double c = std::cos(radians);
    const double s = std::sin(radians);
    Mat4 result = identityMatrix();
    result.m[1][1] = c;
    result.m[1][2] = -s;
    result.m[2][1] = s;
    result.m[2][2] = c;
    return result;
}

inline Mat4 rotationYMatrix(double radians) {
    const double c = std::cos(radians);
    const double s = std::sin(radians);
    Mat4 result = identityMatrix();
    result.m[0][0] = c;
    result.m[0][2] = s;
    result.m[2][0] = -s;
    result.m[2][2] = c;
    return result;
}

inline Mat4 rotationZMatrix(double radians) {
    const double c = std::cos(radians);
    const double s = std::sin(radians);
    Mat4 result = identityMatrix();
    result.m[0][0] = c;
    result.m[0][1] = -s;
    result.m[1][0] = s;
    result.m[1][1] = c;
    return result;
}

// Right-handed OpenGL-style perspective projection.  View space looks down -Z,
// so clip-space w equals the positive view depth.
inline Mat4 perspectiveMatrix(double fovYDegrees, double aspect, double nearPlane,
                             double farPlane) {
    const double fovY = fovYDegrees * kDegToRad;
    const double f = 1.0 / std::tan(fovY * 0.5);
    Mat4 result;
    result.m[0][0] = f / aspect;
    result.m[1][1] = f;
    result.m[2][2] = (farPlane + nearPlane) / (nearPlane - farPlane);
    result.m[2][3] = (2.0 * farPlane * nearPlane) / (nearPlane - farPlane);
    result.m[3][2] = -1.0;
    return result;
}

inline Mat4 lookAtMatrix(Vec3 eye, Vec3 target, Vec3 up) {
    const Vec3 forward = normalized(target - eye);
    Vec3 right = cross(forward, up);
    if (lengthSquared(right) < 1e-12) {
        right = cross(forward, Vec3{0.0, 1.0, 0.0});
    }
    right = normalized(right);
    const Vec3 trueUp = cross(right, forward);

    Mat4 result = identityMatrix();
    result.m[0][0] = right.x;
    result.m[0][1] = right.y;
    result.m[0][2] = right.z;
    result.m[0][3] = -dot(right, eye);
    result.m[1][0] = trueUp.x;
    result.m[1][1] = trueUp.y;
    result.m[1][2] = trueUp.z;
    result.m[1][3] = -dot(trueUp, eye);
    result.m[2][0] = -forward.x;
    result.m[2][1] = -forward.y;
    result.m[2][2] = -forward.z;
    result.m[2][3] = dot(forward, eye);
    return result;
}

// Convert a horizontal field of view to the vertical field of view the
// projection matrix needs.
inline double verticalFovFromHorizontal(double horizontalDegrees, double aspect) {
    const double halfHorizontal = horizontalDegrees * 0.5 * kDegToRad;
    const double halfVertical = std::atan(std::tan(halfHorizontal) / std::max(aspect, 1e-6));
    return 2.0 * halfVertical / kDegToRad;
}

}  // namespace g3d
