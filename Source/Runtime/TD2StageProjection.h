#pragma once
// 画面内の舞台の座標変換。舞台は世界地図どおりに並べた小さな 3D 世界で、本来の世界から離れた所に置く。
// 世界のマス（左上原点、+Y 下）を舞台の座標へ写し、正投影カメラの姿勢を決める。
// 各テレビの画面は、撮った像から自分の部屋の範囲を切り出したもの。
#include "HIKARI/Project/ValueTypes.h"
#include <array>
#include <cmath>

namespace TD2 {
inline constexpr double kPi = 3.14159265358979323846;
inline constexpr float kStageCoordinate = 10000.0f;
// 舞台カメラは注視点から 30 離れた正投影カメラ。角度は部品の傾きが目立たない範囲に抑える。
inline constexpr double kStageCameraDistance = 30.0, kMaxStagePitchDegrees = 30.0, kMaxStageYawDegrees = 30.0;

using Vector3 = std::array<double, 3>;
inline Vector3 Cross(const Vector3& a, const Vector3& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
// 正規直交の三軸（local X/Y/Z が向く世界方向）から回転の四元数を作る。
inline HIKARI::GameProjectQuaternion QuaternionFromBasis(const Vector3& x, const Vector3& y, const Vector3& z) {
    const double trace = x[0] + y[1] + z[2];
    double qx, qy, qz, qw;
    if (trace > 0) {
        const double s = std::sqrt(trace + 1) * 2;
        qw = s / 4; qx = (y[2] - z[1]) / s; qy = (z[0] - x[2]) / s; qz = (x[1] - y[0]) / s;
    } else if (x[0] > y[1] && x[0] > z[2]) {
        const double s = std::sqrt(1 + x[0] - y[1] - z[2]) * 2;
        qw = (y[2] - z[1]) / s; qx = s / 4; qy = (y[0] + x[1]) / s; qz = (z[0] + x[2]) / s;
    } else if (y[1] > z[2]) {
        const double s = std::sqrt(1 + y[1] - x[0] - z[2]) * 2;
        qw = (z[0] - x[2]) / s; qx = (y[0] + x[1]) / s; qy = s / 4; qz = (z[1] + y[2]) / s;
    } else {
        const double s = std::sqrt(1 + z[2] - x[0] - y[1]) * 2;
        qw = (x[1] - y[0]) / s; qx = (z[0] + x[2]) / s; qy = (z[1] + y[2]) / s; qz = s / 4;
    }
    return {static_cast<float>(qx), static_cast<float>(qy), static_cast<float>(qz), static_cast<float>(qw)};
}
inline HIKARI::GameProjectQuaternion RotateZ(double radians) {
    return {0, 0, static_cast<float>(std::sin(radians * .5)), static_cast<float>(std::cos(radians * .5))};
}

struct StageProjection {
    double scale = 1;             // 1 マスの舞台上の長さ（どの舞台カメラにも共通）
    double pitch = 0, yaw = 0;    // 舞台カメラの俯角と側角（ラジアン、正の側角は右から見る）
    // 先に側角で Y 軸回り、次に俯角で X 軸回りに回す（ルールの行が水平に写り、床が傾かない）。
    // 以下は画面の右・上・視線の向き。
    Vector3 Right() const { return {std::cos(yaw), -std::sin(yaw) * std::sin(pitch), -std::sin(yaw) * std::cos(pitch)}; }
    Vector3 Up() const { return {0, std::cos(pitch), -std::sin(pitch)}; }
    Vector3 Forward() const { return {-std::sin(yaw), -std::cos(yaw) * std::sin(pitch), -std::cos(yaw) * std::cos(pitch)}; }
    // マスと奥行き（マス、手前が正）を舞台の座標へ。どの奥行きでも正面のマスと同じ画面位置になるよう逆算する。
    HIKARI::GameProjectFloat3 Point(double x, double y, double depth = 0) const {
        const double screenX = x * scale, screenY = -y * scale, z = depth * scale;
        const double worldY = (screenY + z * std::sin(pitch)) / std::cos(pitch);
        const double worldX = (screenX + worldY * std::sin(yaw) * std::sin(pitch) + z * std::sin(yaw) * std::cos(pitch)) / std::cos(yaw);
        return {kStageCoordinate + static_cast<float>(worldX), kStageCoordinate + static_cast<float>(worldY),
                kStageCoordinate + static_cast<float>(z)};
    }
    // 俯角と側角を両方付けた時、縦の辺が 1 マスごとに右へずれる量（片方だけなら 0）。
    double Lean() const { return std::sin(yaw) * std::tan(pitch); }
    // マス (x, y) を像の中心に撮るカメラの姿勢。ローカル +Z が視線、+Y が上、+X は画面の左。
    HIKARI::GameProjectTransform CameraPose(double x, double y) const {
        const auto target = Point(x, y);
        const auto forward = Forward(), up = Up();
        HIKARI::GameProjectTransform pose{};
        pose.position = {target.x - static_cast<float>(kStageCameraDistance * forward[0]),
                         target.y - static_cast<float>(kStageCameraDistance * forward[1]),
                         target.z - static_cast<float>(kStageCameraDistance * forward[2])};
        pose.rotation = QuaternionFromBasis(Cross(up, forward), up, forward);
        return pose;
    }
};
}
