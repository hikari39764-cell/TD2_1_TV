#pragma once
// 区域カメラと切り替え演出の生成（TD2CameraLayout.cpp と TD2CameraTravels.cpp）で共有する、幾何と JSON の小さな関数。
// ほかのファイルからは読まない（公開の型と関数は TD2CameraLayout.h）。
#include "TD2CameraLayout.h"
#include "../Common/TD2MachineText.h"
#include "../Common/TD2Math.h"
#include "../Common/TD2RegionData.h"
#include "../Common/TD2TelevisionModels.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <sstream>

namespace TD2::Editor::LayoutDetail {
using Math::Add;
using Math::Cross;
using Math::Dot;
using Math::kPi;
using Math::Lerp;
using Math::Normalize;
using Math::Sub;
inline constexpr double kFov = 26.0;              // 区域カメラと演出の基準の画角（度）
inline constexpr double kNarrowAspect = 4.0 / 3;  // 画面の検査は一番狭い縦横比（4:3 前後）で行う
inline constexpr double kMinShare = .6;           // 切り替え中はどの瞬間も、少なくとも一台の画面の半分以上を画に入れる
inline constexpr std::string_view kAreaComponent = "TD2.CameraArea";

using M3 = std::array<std::array<double, 3>, 3>;
inline Vec3 Flat(const Vec3& a) { return Normalize({a[0], 0, a[2]}); }
inline constexpr Vec3 kUp{0, 1, 0};

// エンジンの回転順（Rz·Ry·Rx）で、度の回転を v に掛ける。カメラもテレビもローカル +Z を向く。
inline Vec3 Rotate(const Vec3& euler, const Vec3& v) { return Math::QRotate(Math::QFromEulerDeg(euler), v); }
inline Vec3 RotateY(const Vec3& v, double deg) { return Rotate({0, deg, 0}, v); }
// カメラの回転行列（列 = ローカル X（画の左）・Y（上）・Z（前））。roll は前の軸まわりの傾き（度）。
inline M3 LookMatrix(const Vec3& eye, const Vec3& look, double roll = 0) {
    const auto f = Normalize(Sub(look, eye));
    auto left = Normalize(Cross(kUp, f));
    auto up = Cross(f, left);
    if (roll != 0) {
        const double c = std::cos(roll * kPi / 180), s = std::sin(roll * kPi / 180);
        const auto l = left, u = up;
        left = Add({l[0] * c, l[1] * c, l[2] * c}, u, s);
        up = Add({u[0] * c, u[1] * c, u[2] * c}, l, -s);
    }
    return M3{{{left[0], up[0], f[0]}, {left[1], up[1], f[1]}, {left[2], up[2], f[2]}}};
}
// M = Rz(c)·Ry(b)·Rx(a) を (a, b, c) 度へ。真横（b = ±90°）は c = 0 として a だけで表す。
inline Vec3 EulerFrom(const M3& m) {
    const double sb = std::clamp(-m[2][0], -1.0, 1.0);
    double a, b, c;
    if (std::abs(sb) < .99999) { b = std::asin(sb); a = std::atan2(m[2][1], m[2][2]); c = std::atan2(m[1][0], m[0][0]); }
    else { b = std::copysign(kPi / 2, sb); c = 0; a = sb < 0 ? std::atan2(-m[0][1], m[1][1]) : std::atan2(m[0][1], m[1][1]); }
    a *= 180 / kPi; b *= 180 / kPi; c *= 180 / kPi;
    if (std::abs(c) > 90) { a += 180; b = 180 - b; c += 180; }   // 同じ回転の読みやすい方（傾き 0 付近）
    const auto wrap = [](double v) { return std::fmod(std::fmod(v + 180, 360) + 360, 360) - 180; };
    return {wrap(a), wrap(b), wrap(c)};
}
inline std::array<double, 4> QuatFrom(const M3& m) {
    const double t = m[0][0] + m[1][1] + m[2][2];
    double x, y, z, w;
    if (t > 0) { const double s = std::sqrt(t + 1) * 2; w = s / 4; x = (m[2][1] - m[1][2]) / s; y = (m[0][2] - m[2][0]) / s; z = (m[1][0] - m[0][1]) / s; }
    else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) { const double s = std::sqrt(1 + m[0][0] - m[1][1] - m[2][2]) * 2; x = s / 4; w = (m[2][1] - m[1][2]) / s; y = (m[0][1] + m[1][0]) / s; z = (m[0][2] + m[2][0]) / s; }
    else if (m[1][1] > m[2][2]) { const double s = std::sqrt(1 + m[1][1] - m[0][0] - m[2][2]) * 2; y = s / 4; w = (m[0][2] - m[2][0]) / s; x = (m[0][1] + m[1][0]) / s; z = (m[1][2] + m[2][1]) / s; }
    else { const double s = std::sqrt(1 + m[2][2] - m[0][0] - m[1][1]) * 2; z = s / 4; w = (m[1][0] - m[0][1]) / s; x = (m[0][2] + m[2][0]) / s; y = (m[1][2] + m[2][1]) / s; }
    return {x, y, z, w};
}
inline double Round(double v, double step = 1e-4) { return std::round(v / step) * step; }

// ---- テレビの形 ----
inline Vec3 TvLocal(const LayoutRoom& r, const Vec3& local) {
    return Add(r.tv.position, Rotate(r.tv.rotation, {local[0] * r.tv.scale[0], local[1] * r.tv.scale[1], local[2] * r.tv.scale[2]}));
}
// 画面の点（u: 左 0 → 右 1、v: 上 0 → 下 1）。
inline Vec3 ScreenPoint(const LayoutRoom& r, double u = .5, double v = .5) {
    return TvLocal(r, {r.screenCenter[0] + (u - .5) * r.screenWidth, r.screenCenter[1] + (.5 - v) * r.screenHeight, r.screenCenter[2]});
}
inline Vec3 TvAxis(const LayoutRoom& r, int axis) { return Normalize(Rotate(r.tv.rotation, {axis == 0 ? 1.0 : 0, axis == 1 ? 1.0 : 0, axis == 2 ? 1.0 : 0})); }
inline LayoutBox TvBox(const LayoutRoom& r) {
    LayoutBox box{{1e9, 1e9, 1e9}, {-1e9, -1e9, -1e9}, r.objectId};
    for (const double x : {-r.body[0] / 2, r.body[0] / 2}) for (const double y : {0.0, r.body[1]}) for (const double z : {-r.body[2] / 2, r.body[2] / 2}) {
        const auto p = TvLocal(r, {x, y, z});
        for (int i = 0; i < 3; ++i) { box.lo[i] = (std::min)(box.lo[i], p[i]); box.hi[i] = (std::max)(box.hi[i], p[i]); }
    }
    return box;
}
inline bool Inside(const Vec3& p, const LayoutBox& box, double margin) {
    for (int i = 0; i < 3; ++i) { if (p[i] <= box.lo[i] - margin || p[i] >= box.hi[i] + margin) { return false; } }
    return true;
}

// ---- シーンの読み取り ----
inline Vec3 ReadVec3(const Json& value, Vec3 fallback) {
    if (!value.is_array() || value.size() != 3) { return fallback; }
    for (int i = 0; i < 3; ++i) { if (value[i].is_number()) { fallback[i] = value[i].get<double>(); } }
    return fallback;
}
inline LayoutTransform ReadTransform(const Json& t) {
    LayoutTransform result;
    result.position = ReadVec3(t.value("position", Json{}), {0, 0, 0});
    result.rotation = ReadVec3(t.value("rotation", t.value("rotationEulerDeg", Json{})), {0, 0, 0});
    result.scale = ReadVec3(t.value("scale", Json{}), {1, 1, 1});
    return result;
}
inline uint64_t ReferenceId(const Json* value) {
    if (!value) { return 0; }
    if (value->is_object()) { return ObjectId(value->value("id", Json{})); }
    return ObjectId(*value);
}
inline Json Describe(EditorWorkspaceHostContext& ctx, uint64_t objectId, uint64_t componentId, const std::vector<std::string>& fields, std::string& error) {
    const auto result = Execute(ctx, "component.describe", {{"objectId", objectId}, {"componentId", componentId}, {"propertyIds", fields}});
    if (!result.ok) { error = result.error; return Json{}; }
    return result.data.value("fields", Json::array());
}
inline double NumberField(const Json& fields, std::string_view id, double fallback) {
    const auto* v = FieldValue(fields, id); return v && v->is_number() ? v->get<double>() : fallback;
}
inline std::string TextField(const Json& fields, std::string_view id) {
    const auto* v = FieldValue(fields, id); return v && v->is_string() ? v->get<std::string>() : std::string{};
}
inline bool BoolField(const Json& fields, std::string_view id) {
    const auto* v = FieldValue(fields, id); return v && v->is_boolean() && v->get<bool>();
}
inline std::vector<std::string> SplitRegions(const std::string& text) {
    std::vector<std::string> result; std::stringstream stream(text); std::string item;
    while (std::getline(stream, item, ',')) {
        const auto b = item.find_first_not_of(" \t"), e = item.find_last_not_of(" \t");
        if (b != std::string::npos) { result.push_back(item.substr(b, e - b + 1)); }
    }
    return result;
}

// ---- 経路 ----
struct Pivot { Vec3 corner{}, out{}; bool valid = false; };
// 二台のテレビの背面の線の交点を角とし、二つの法線の和を角から外への向きとする（柱の角を回る演出の中心）。
// 背面を使うのは、柱に付いたテレビでは背面の交点が柱の角に一致するため。
inline Pivot CornerOf(const LayoutRoom& a, const LayoutRoom& b, double y) {
    Pivot p;
    const auto back = [](const LayoutRoom& r) { return TvLocal(r, {0, r.screenCenter[1], -r.body[2] / 2}); };
    const auto pa = back(a), pb = back(b), da = Flat(TvAxis(a, 0)), db = Flat(TvAxis(b, 0));
    const double det = da[0] * (-db[2]) - da[2] * (-db[0]);
    if (std::abs(det) < .3) { return p; }
    const double rx = pb[0] - pa[0], rz = pb[2] - pa[2];
    const double s = (rx * (-db[2]) - rz * (-db[0])) / det;
    p.corner = {pa[0] + da[0] * s, y, pa[2] + da[2] * s};
    p.out = Flat(Add(TvAxis(a, 2), TvAxis(b, 2)));
    p.valid = true;
    return p;
}

inline double ScreenShare(const LayoutRoom& r, const TravelKey& key) {
    const auto m = LookMatrix(key.eye, key.look, key.roll);
    const Vec3 left{m[0][0], m[1][0], m[2][0]}, up{m[0][1], m[1][1], m[2][1]}, forward{m[0][2], m[1][2], m[2][2]};
    if (Dot(TvAxis(r, 2), Normalize(Sub(key.eye, ScreenPoint(r)))) < .1) { return 0; }
    const double ty = std::tan(key.fov * kPi / 360), tx = ty * kNarrowAspect;
    int seen = 0;
    for (int i = 0; i < 5; ++i) for (int j = 0; j < 5; ++j) {
        const auto d = Sub(ScreenPoint(r, .1 + .2 * i, .1 + .2 * j), key.eye);
        const double z = Dot(d, forward);
        seen += z > .1 && std::abs(Dot(d, left)) < z * tx && std::abs(Dot(d, up)) < z * ty;
    }
    return seen / 25.0;
}
inline bool InScope(const LayoutSnapshot& snap, std::string_view regionId, std::string_view areaName) {
    if (areaName.empty()) { return true; }
    const auto* area = snap.AreaOf(regionId);
    return area && area->name == areaName;
}
}
