#pragma once
#include <openxr/openxr.h>
#include <cmath>
#include <optional>

inline XrVector3f difference(XrVector3f a, XrVector3f b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

inline XrVector3f rotateVector(XrQuaternionf q, XrVector3f v) {
    const XrVector3f t{2 * (q.y * v.z - q.z * v.y), 2 * (q.z * v.x - q.x * v.z), 2 * (q.x * v.y - q.y * v.x)};
    return {v.x + q.w * t.x + q.y * t.z - q.z * t.y,
            v.y + q.w * t.y + q.z * t.x - q.x * t.z,
            v.z + q.w * t.z + q.x * t.y - q.y * t.x};
}

inline float dot(XrVector3f a, XrVector3f b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

inline std::optional<XrVector2f> horizontalForward(XrQuaternionf q) {
    const float x = -2.0f * (q.x * q.z + q.w * q.y);
    const float z = -(1.0f - 2.0f * (q.x * q.x + q.y * q.y));
    const float length = std::hypot(x, z);
    if (!(length > 0.12f)) return {};
    return XrVector2f{x / length, z / length};
}
