#pragma once
// ゲームカメラの視錐台に球が入るかの判定。光の間引きと周りのテレビの描画で使う。
#include "../Common/TD2Math.h"
#include "HIKARI/Project/ActionTypes.h"
#include <algorithm>
#include <cmath>

namespace TD2 {
// marginDegrees は視野を外へ広げる角度。
inline bool SphereInView(const HIKARI::GameProjectCameraPose& pose, const HIKARI::GameProjectFloat3& center, double radius,
                         double marginDegrees) {
    using Math::Vec3;
    const auto sub = [](const HIKARI::GameProjectFloat3& a, const HIKARI::GameProjectFloat3& b) { return Vec3{a.x - b.x, a.y - b.y, a.z - b.z}; };
    const Vec3 forward = Math::Normalize(sub(pose.target, pose.eye));
    const Vec3 right = Math::Normalize(Math::Cross(forward, {pose.up.x, pose.up.y, pose.up.z}));
    const Vec3 up = Math::Cross(right, forward);
    const Vec3 d = sub(center, pose.eye);
    const double z = Math::Dot(d, forward), x = Math::Dot(d, right), y = Math::Dot(d, up);
    if (z < -radius) { return false; }
    const double margin = marginDegrees * Math::kPi / 180.0;
    const double halfV = (std::min)(1.5, pose.verticalFovRadians * .5 + margin);
    const double halfH = (std::min)(1.5, std::atan(std::tan(pose.verticalFovRadians * .5) * (std::max)(1.0, static_cast<double>(pose.aspectRatio))) + margin);
    // 側面の法線方向の距離（x·cos − z·sin）が半径より大きければ外。
    return std::abs(x) * std::cos(halfH) - z * std::sin(halfH) <= radius && std::abs(y) * std::cos(halfV) - z * std::sin(halfV) <= radius;
}
}
