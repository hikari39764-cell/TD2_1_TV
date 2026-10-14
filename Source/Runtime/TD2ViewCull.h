#pragma once
// 游戏カメラの視野（視錐台）に球が掛かるかの判定。灯光の予算と、周りの電視を描くかの判断で共有する。
#include "HIKARI/Project/ActionTypes.h"
#include <algorithm>
#include <cmath>

namespace TD2 {
// marginDegrees は視野の四辺を外へ広げる角度。カメラが動く間も、画に入る前に用意できるように。
inline bool SphereInView(const HIKARI::GameProjectCameraPose& pose, const HIKARI::GameProjectFloat3& center, double radius,
                         double marginDegrees) {
    struct Vec { double x, y, z; };
    const auto sub = [](const HIKARI::GameProjectFloat3& a, const HIKARI::GameProjectFloat3& b) { return Vec{a.x - b.x, a.y - b.y, a.z - b.z}; };
    const auto dot = [](const Vec& a, const Vec& b) { return a.x * b.x + a.y * b.y + a.z * b.z; };
    const auto normalize = [&dot](const Vec& v) { const double n = std::sqrt(dot(v, v)); return n > 1e-9 ? Vec{v.x / n, v.y / n, v.z / n} : Vec{0, 0, 1}; };
    const Vec forward = normalize(sub(pose.target, pose.eye)), worldUp{pose.up.x, pose.up.y, pose.up.z};
    const Vec right = normalize({forward.y * worldUp.z - forward.z * worldUp.y, forward.z * worldUp.x - forward.x * worldUp.z,
                                 forward.x * worldUp.y - forward.y * worldUp.x});
    const Vec up{right.y * forward.z - right.z * forward.y, right.z * forward.x - right.x * forward.z, right.x * forward.y - right.y * forward.x};
    const Vec d = sub(center, pose.eye);
    const double z = dot(d, forward), x = dot(d, right), y = dot(d, up);
    if (z < -radius) { return false; }
    const double margin = marginDegrees * 3.14159265358979323846 / 180.0;
    const double halfV = (std::min)(1.5, pose.verticalFovRadians * .5 + margin);
    const double halfH = (std::min)(1.5, std::atan(std::tan(pose.verticalFovRadians * .5) * (std::max)(1.0, static_cast<double>(pose.aspectRatio))) + margin);
    // 側面の法線方向の距離（x·cos − z·sin）が半径より大きければ外。
    return std::abs(x) * std::cos(halfH) - z * std::sin(halfH) <= radius && std::abs(y) * std::cos(halfV) - z * std::sin(halfV) <= radius;
}
} // namespace TD2
