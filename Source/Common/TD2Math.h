#pragma once
// ランタイムとエディタで共有する小さな幾何計算。倍精度で、エンジンの型には依存しない。
#include <algorithm>
#include <array>
#include <cmath>

namespace TD2::Math {
inline constexpr double kPi = 3.14159265358979323846;
// 0〜1 へ丸めた三次のなめらかな立ち上がり（始めと終わりの傾きが 0）。
inline double Smoothstep(double x) { x = std::clamp(x, 0.0, 1.0); return x * x * (3 - 2 * x); }

using Vec3 = std::array<double, 3>;
using Quat = std::array<double, 4>;   // x, y, z, w

inline Vec3 Add(const Vec3& a, const Vec3& b, double k = 1) { return {a[0] + b[0] * k, a[1] + b[1] * k, a[2] + b[2] * k}; }
inline Vec3 Sub(const Vec3& a, const Vec3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
inline double Dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
inline Vec3 Cross(const Vec3& a, const Vec3& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
// 長さがほぼ 0 なら +Z を返す。
inline Vec3 Normalize(const Vec3& a) {
    const double n = std::sqrt(Dot(a, a));
    return n > 1e-9 ? Vec3{a[0] / n, a[1] / n, a[2] / n} : Vec3{0, 0, 1};
}
inline Vec3 Lerp(const Vec3& a, const Vec3& b, double t) {
    return {a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t};
}

// 回転の順はエンジンと同じ M = Rz·Ry·Rx（角度は度）。
inline Quat QMul(const Quat& a, const Quat& b) {
    return {a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1], a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
            a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3], a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
}
inline Quat QConj(const Quat& q) { return {-q[0], -q[1], -q[2], q[3]}; }
inline Vec3 QRotate(const Quat& q, const Vec3& v) {
    const auto r = QMul(QMul(q, {v[0], v[1], v[2], 0}), QConj(q));
    return {r[0], r[1], r[2]};
}
inline Quat QFromEulerDeg(const Vec3& degrees) {
    const auto axis = [](double angle, size_t i) {
        const double half = angle * kPi / 360;
        Quat q{0, 0, 0, std::cos(half)};
        q[i] = std::sin(half);
        return q;
    };
    return QMul(QMul(axis(degrees[2], 2), axis(degrees[1], 1)), axis(degrees[0], 0));
}

// 両端でゆっくり止まる補間の重み。
template <class T>
T SmoothStep(T t) {
    t = std::clamp(t, T(0), T(1));
    return t * t * (T(3) - T(2) * t);
}

// 臨界減衰のばね。行き過ぎず、目標が跳んでも速さが連続する。rate は変化の速さの状態。
inline double CriticalDamp(double value, double target, double& rate, double seconds, double dt) {
    const double omega = 2.0 / (std::max)(seconds, 1e-3), x = omega * dt;
    const double decay = 1.0 / (1.0 + x + .48 * x * x + .235 * x * x * x);
    const double change = value - target, temp = (rate + omega * change) * dt;
    rate = (rate - omega * temp) * decay;
    return target + (change + temp) * decay;
}
}
