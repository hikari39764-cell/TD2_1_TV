#pragma once
#include "../Common/TD2Math.h"
#include "HIKARI/Project/ValueTypes.h"
#include <algorithm>
#include <cmath>

namespace TD2 {
// エンジンの型（単精度）での姿勢計算。仕掛けで物体を動かす時とカメラで使う（縮尺は扱わない）。
using HIKARI::GameProjectFloat3;
using HIKARI::GameProjectQuaternion;
using HIKARI::GameProjectTransform;

inline Math::Vec3 ToVec3(const GameProjectFloat3& v) { return {v.x, v.y, v.z}; }
inline GameProjectFloat3 ToF3(const Math::Vec3& v) { return {static_cast<float>(v[0]), static_cast<float>(v[1]), static_cast<float>(v[2])}; }
inline GameProjectFloat3 Sub(const GameProjectFloat3& a, const GameProjectFloat3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
// a + b·k
inline GameProjectFloat3 Mad(const GameProjectFloat3& a, const GameProjectFloat3& b, float k) { return {a.x + b.x * k, a.y + b.y * k, a.z + b.z * k}; }
inline GameProjectFloat3 Lerp(const GameProjectFloat3& a, const GameProjectFloat3& b, float t) {
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
}

// 四元数の計算は TD2Math（倍精度）に一つだけ置き、ここはエンジンの型との受け渡しだけ。
inline Math::Quat ToQuat(const GameProjectQuaternion& q) { return {q.x, q.y, q.z, q.w}; }
inline GameProjectQuaternion ToQuaternion(const Math::Quat& q) {
    return {static_cast<float>(q[0]), static_cast<float>(q[1]), static_cast<float>(q[2]), static_cast<float>(q[3])};
}
inline GameProjectQuaternion QMul(const GameProjectQuaternion& a, const GameProjectQuaternion& b) {
    return ToQuaternion(Math::QMul(ToQuat(a), ToQuat(b)));
}
inline GameProjectQuaternion QConj(const GameProjectQuaternion& q) { return ToQuaternion(Math::QConj(ToQuat(q))); }
inline GameProjectFloat3 QRotate(const GameProjectQuaternion& q, const GameProjectFloat3& v) {
    return ToF3(Math::QRotate(ToQuat(q), ToVec3(v)));
}
// エンジンの回転順（X → Y → Z、M = Rz·Ry·Rx）で度から四元数へ。
inline GameProjectQuaternion QFromEulerDeg(double rx, double ry, double rz) {
    return ToQuaternion(Math::QFromEulerDeg({rx, ry, rz}));
}
// ローカルの点を変換で移す（縮尺 → 回転 → 平行移動）。translate が false なら向きとして移す（平行移動しない）。
inline GameProjectFloat3 TransformPoint(const GameProjectTransform& t, const Math::Vec3& local, bool translate = true) {
    auto r = Math::QRotate(ToQuat(t.rotation), {local[0] * t.scale.x, local[1] * t.scale.y, local[2] * t.scale.z});
    if (translate) { r[0] += t.position.x; r[1] += t.position.y; r[2] += t.position.z; }
    return ToF3(r);
}
inline GameProjectQuaternion QSlerp(GameProjectQuaternion a, GameProjectQuaternion b, float t) {
    float dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (dot < 0) { b = {-b.x, -b.y, -b.z, -b.w}; dot = -dot; }
    float wa = 1 - t, wb = t;
    if (dot < .9995f) {
        const float angle = std::acos(std::clamp(dot, -1.f, 1.f)), s = std::sin(angle);
        wa = std::sin((1 - t) * angle) / s; wb = std::sin(t * angle) / s;
    }
    GameProjectQuaternion q{a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb, a.w * wa + b.w * wb};
    const float length = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (length > 0) { q.x /= length; q.y /= length; q.z /= length; q.w /= length; }
    return q;
}
// parent から見た child の相対姿勢と、その逆（相対を parent に載せ直す）。
inline GameProjectTransform Relative(const GameProjectTransform& parent, const GameProjectTransform& child) {
    GameProjectTransform r = child;
    const auto inverse = QConj(parent.rotation);
    r.position = QRotate(inverse, {child.position.x - parent.position.x, child.position.y - parent.position.y, child.position.z - parent.position.z});
    r.rotation = QMul(inverse, child.rotation);
    return r;
}
inline GameProjectTransform Attach(const GameProjectTransform& parent, const GameProjectTransform& relative) {
    GameProjectTransform r = relative;
    const auto p = QRotate(parent.rotation, relative.position);
    r.position = {parent.position.x + p.x, parent.position.y + p.y, parent.position.z + p.z};
    r.rotation = QMul(parent.rotation, relative.rotation);
    return r;
}
}
