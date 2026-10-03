#pragma once

#include <openxr/openxr.h>
#include <cmath>

// Just enough pose arithmetic for xrLocateSpace / xrLocateViews.
// Poses are (orientation, position); composing A*B applies B then A.

inline XrQuaternionf quat_identity() {
    return XrQuaternionf{0.0f, 0.0f, 0.0f, 1.0f};
}

inline XrPosef pose_identity() {
    return XrPosef{quat_identity(), XrVector3f{0.0f, 0.0f, 0.0f}};
}

inline XrQuaternionf quat_multiply(const XrQuaternionf& a, const XrQuaternionf& b) {
    return XrQuaternionf{
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
    };
}

inline XrQuaternionf quat_conjugate(const XrQuaternionf& q) {
    return XrQuaternionf{-q.x, -q.y, -q.z, q.w};
}

inline XrVector3f quat_rotate(const XrQuaternionf& q, const XrVector3f& v) {
    const XrQuaternionf p{v.x, v.y, v.z, 0.0f};
    const XrQuaternionf r = quat_multiply(quat_multiply(q, p), quat_conjugate(q));
    return XrVector3f{r.x, r.y, r.z};
}

inline XrVector3f vec_add(const XrVector3f& a, const XrVector3f& b) {
    return XrVector3f{a.x + b.x, a.y + b.y, a.z + b.z};
}

inline XrVector3f vec_sub(const XrVector3f& a, const XrVector3f& b) {
    return XrVector3f{a.x - b.x, a.y - b.y, a.z - b.z};
}

inline XrPosef pose_multiply(const XrPosef& a, const XrPosef& b) {
    return XrPosef{quat_multiply(a.orientation, b.orientation),
                   vec_add(a.position, quat_rotate(a.orientation, b.position))};
}

inline XrPosef pose_inverse(const XrPosef& p) {
    const XrQuaternionf inverse_orientation = quat_conjugate(p.orientation);
    const XrVector3f rotated = quat_rotate(inverse_orientation, p.position);
    return XrPosef{inverse_orientation, XrVector3f{-rotated.x, -rotated.y, -rotated.z}};
}
