#pragma once
// 電視の中の舞台の座標変換。舞台は世界地図の並びそのままの小さな 3D 世界で、本来の世界から遠く離れた場所に置く。
// 世界の格（左上原点、+Y 下）を舞台の世界座標へ写し、舞台を撮る正交カメラの姿勢を決める。計算だけで状態は持たない。
// 各電視の画面は、舞台を撮った像のうち自分の部屋の範囲の切り出し（主角・光線・特効は世界に一つずつ）。
#include "HIKARI/Project/ValueTypes.h"
#include <array>
#include <cmath>

namespace TD2 {
inline constexpr double kPi = 3.14159265358979323846;
inline constexpr float kStageCoordinate = 10000.0f;
// 舞台カメラは注視点から 30 離れた正交カメラ。角度の上限は部品の剛体の傾き（Lean）が目立ちすぎない範囲。
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
    double scale = 1;             // 1 格の舞台上の長さ（どの舞台カメラにも共通）
    double pitch = 0, yaw = 0;    // 舞台カメラの俯角と側角（ラジアン、正の側角は右から見る）
    // カメラは先に側角で世界 Y 軸回り、次に俯角で世界 X 軸回りに回す。こうすると世界の X 軸（Rules の行）が
    // 画面でも水平に写り、床が傾かない。以下は画面の右・上・視線の向き。
    Vector3 Right() const { return {std::cos(yaw), -std::sin(yaw) * std::sin(pitch), -std::sin(yaw) * std::cos(pitch)}; }
    Vector3 Up() const { return {0, std::cos(pitch), -std::sin(pitch)}; }
    Vector3 Forward() const { return {-std::sin(yaw), -std::cos(yaw) * std::sin(pitch), -std::cos(yaw) * std::cos(pitch)}; }
    // 世界の格と奥行き depth（格、手前が正）を舞台の世界座標へ。どの奥行きの部品も基準点が正面の格と同じ
    // 画面位置に落ちるよう、画面の右・上への射影から世界の X・Y を逆算する。
    HIKARI::GameProjectFloat3 Point(double x, double y, double depth = 0) const {
        const double screenX = x * scale, screenY = -y * scale, z = depth * scale;
        const double worldY = (screenY + z * std::sin(pitch)) / std::cos(pitch);
        const double worldX = (screenX + worldY * std::sin(yaw) * std::sin(pitch) + z * std::sin(yaw) * std::cos(pitch)) / std::cos(yaw);
        return {kStageCoordinate + static_cast<float>(worldX), kStageCoordinate + static_cast<float>(worldY),
                kStageCoordinate + static_cast<float>(z)};
    }
    // 俯角と側角を両方付けると、剛体の部品の縦の辺は 1 格下がるごとに画面で右へこの格数だけ傾く（片方だけなら 0）。
    double Lean() const { return std::sin(yaw) * std::tan(pitch); }
    // 世界の格 (x, y) を像の中心に撮る舞台カメラの姿勢。カメラの local +Z が視線、+Y が上、+X は Y×Z（画面の左）。
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
} // namespace TD2
