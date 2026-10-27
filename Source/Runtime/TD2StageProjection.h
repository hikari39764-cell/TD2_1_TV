#pragma once
// 画面内の舞台の座標変換。舞台は世界地図どおりに並べた小さな 3D 世界で、本来の世界から離れた所に置く。
// 世界のマス（左上原点、+Y 下）を舞台の座標へ写し、正投影カメラの姿勢を決める。
// 各テレビの画面は、撮った像から自分の部屋の範囲を切り出したもの。
#include "HIKARI/Project/ValueTypes.h"
#include "../Common/TD2Math.h"
#include <array>
#include <cmath>

namespace TD2 {
using Math::kPi;
inline constexpr float kStageCoordinate = 10000.0f;
// 舞台カメラは注視点から 30 離れた正投影カメラ。
inline constexpr double kStageCameraDistance = 30.0;

using Vector3 = Math::Vec3;
using Math::Cross;
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
    // 立体：部品に焼いた斜投影（奥へ 1 マスで右と上へずれる量）。0 なら平面の組。カメラは正対のまま。
    double oblique = 0;
    // 人物の奥行き。立体では部品の手前に立たせ、天板や側面に隠れないようにする。平面では地形の奥行きの中ほど。
    double ActorDepth() const { return oblique > 0 ? .45 : -.15; }
    // エフェクトは人物と地形より手前。
    double FxDepth() const { return oblique > 0 ? .6 : .35; }
    // 奥行き depth（マス、手前が正）の点が斜投影でずれる量（マス、y は下が正）。斜めに切っていない部品を合わせるのに使う。
    double ShiftX(double depth) const { return -depth * oblique; }
    double ShiftY(double depth) const { return depth * oblique; }
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
        // 舞台は横と縦だけ世界から離す。奥行き（z）を大きな値へずらすと float の刻み（10000 で約 0.001）が部品どうしの
        // 小さな前後の差より粗くなり、編集中の小さな縮尺では重なった面が入れ替わって描かれる。
        return {kStageCoordinate + static_cast<float>(worldX), kStageCoordinate + static_cast<float>(worldY), static_cast<float>(z)};
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
