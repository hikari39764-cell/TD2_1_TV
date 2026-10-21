#include "TD2Screen3D.h"
#include "TD2Screen3DShared.h"
#include "TD2Transform.h"
#include "HIKARI/Project/ActionViews.h"
#include "HIKARI/Project/GameFlowViews.h"
#include <algorithm>
#include <cmath>

namespace TD2 {
namespace {
using namespace Stage3D;
// 見え方：ゲームカメラから見た各画面と、読みやすさの保護を掛ける画面の選び方。
// 読みやすさの保護を一度に掛けられる画面の数（エンジンの kScreenReadabilityRegionCount と同じ）。
constexpr size_t kMaxProtectedScreens = 12;
}

namespace {
using F3 = HIKARI::GameProjectFloat3;
// ゲームカメラの基底（前・右・上）と、画の半分の幅・高さの正接。点を画の座標（-1〜1）と奥行きへ移す。
struct ViewBasis {
    double eye[3]{}, f[3]{}, r[3]{}, u[3]{}, tx = 1, ty = 1;
    bool valid = false;
    // 奥行き 0.05 より手前の点は 0.05 として写す。
    std::pair<double, double> ToView(const F3& q, double* depth = nullptr) const {
        const double d[3]{q.x - eye[0], q.y - eye[1], q.z - eye[2]};
        const double raw = d[0] * f[0] + d[1] * f[1] + d[2] * f[2];
        if (depth) { *depth = raw; }
        const double z = (std::max)(raw, .05);
        return {(d[0] * r[0] + d[1] * r[1] + d[2] * r[2]) / (z * tx), (d[0] * u[0] + d[1] * u[1] + d[2] * u[2]) / (z * ty)};
    }
};
ViewBasis MakeBasis(const HIKARI::GameProjectCameraPose& pose) {
    ViewBasis b;
    const double f[3]{pose.target.x - pose.eye.x, pose.target.y - pose.eye.y, pose.target.z - pose.eye.z};
    const double fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    b.ty = std::tan(pose.verticalFovRadians * .5); b.tx = b.ty * (std::max)(static_cast<double>(pose.aspectRatio), .1);
    if (fl < 1e-6 || !(b.ty > 0)) { return b; }
    for (int i = 0; i < 3; ++i) { b.f[i] = f[i] / fl; }
    const double r[3]{b.f[1] * pose.up.z - b.f[2] * pose.up.y, b.f[2] * pose.up.x - b.f[0] * pose.up.z, b.f[0] * pose.up.y - b.f[1] * pose.up.x};
    const double rl = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    if (rl < 1e-6) { return b; }
    for (int i = 0; i < 3; ++i) { b.r[i] = r[i] / rl; }
    b.u[0] = b.r[1] * b.f[2] - b.r[2] * b.f[1]; b.u[1] = b.r[2] * b.f[0] - b.r[0] * b.f[2]; b.u[2] = b.r[0] * b.f[1] - b.r[1] * b.f[0];
    b.eye[0] = pose.eye.x; b.eye[1] = pose.eye.y; b.eye[2] = pose.eye.z;
    b.valid = true;
    return b;
}
}

ScreenOnCamera ProjectScreen(const HIKARI::GameProjectCameraPose& pose, const HIKARI::GameProjectTransform& tv, const ScreenSurface& s) {
    ScreenOnCamera out;
    const auto basis = MakeBasis(pose);
    if (!basis.valid) { return out; }
    // 向き：画面の法線（ローカル +Z）とカメラへの向きの余弦。裏向きは写らない扱い。
    const auto center = TransformPoint(tv, {s.centerX, s.centerY, s.centerZ});
    const auto normal = QRotate(tv.rotation, {0, 0, 1});
    const double ex = basis.eye[0] - center.x, ey = basis.eye[1] - center.y, ez = basis.eye[2] - center.z;
    const double el = std::sqrt(ex * ex + ey * ey + ez * ez);
    out.facing = el > 1e-6 ? (normal.x * ex + normal.y * ey + normal.z * ez) / el : 0;
    out.depth = -(ex * basis.f[0] + ey * basis.f[1] + ez * basis.f[2]);
    if (out.facing <= .05 || out.depth <= .05) { return out; }
    // 画面の右・上の向きに対する目の向きの正接（画面の奥の層を見る角度）。
    const auto right = QRotate(tv.rotation, {1, 0, 0}), up = QRotate(tv.rotation, {0, 1, 0});
    const double along = normal.x * ex + normal.y * ey + normal.z * ez;
    out.eyeX = (right.x * ex + right.y * ey + right.z * ez) / along;
    out.eyeY = (up.x * ex + up.y * ey + up.z * ez) / along;
    const auto c = basis.ToView(center);
    out.centerX = c.first; out.centerY = c.second;
    // 四隅を画へ写し、画の枠（-1〜1）で切り取った多角形の面積を画の面積（4）で割る。
    constexpr std::pair<double, double> kCorners[]{{-.5, -.5}, {.5, -.5}, {.5, .5}, {-.5, .5}};
    std::vector<std::pair<double, double>> poly;
    for (const auto& [a, b] : kCorners) { poly.push_back(basis.ToView(TransformPoint(tv, {s.centerX + a * s.width, s.centerY + b * s.height, s.centerZ}))); }
    for (int edge = 0; edge < 4 && !poly.empty(); ++edge) {
        const int axis = edge / 2; const double sign = edge % 2 ? -1.0 : 1.0;   // x≤1, x≥-1, y≤1, y≥-1 の順
        const auto inside = [&](const std::pair<double, double>& q) { return sign * (axis ? q.second : q.first) <= 1; };
        std::vector<std::pair<double, double>> clipped;
        for (size_t i = 0; i < poly.size(); ++i) {
            const auto& a = poly[i]; const auto& b = poly[(i + 1) % poly.size()];
            if (inside(a)) { clipped.push_back(a); }
            if (inside(a) != inside(b)) {
                const double va = axis ? a.second : a.first, vb = axis ? b.second : b.first;
                const double t = (sign - va) / (vb - va);
                clipped.push_back({a.first + (b.first - a.first) * t, a.second + (b.second - a.second) * t});
            }
        }
        poly = std::move(clipped);
    }
    double area = 0;
    for (size_t i = 0; i < poly.size(); ++i) {
        const auto& a = poly[i]; const auto& b = poly[(i + 1) % poly.size()];
        area += a.first * b.second - b.first * a.second;
    }
    out.coverage = std::abs(area) * .5 / 4;
    return out;
}

void Screen3DStage::Inspect(HIKARI::WorldView& world, const std::vector<const StageRoom*>& rooms, const StageFrame& frame) {
    seen_.clear();
    HIKARI::GameProjectCameraPose pose{};
    if (frame.authoring || !world.GetRuntimeSession().IsPlayActive() || !world.GetCamera().GetRenderedPose(pose)) { return; }
    for (const auto* room : rooms) {
        HIKARI::GameProjectTransform tv{};
        const auto object = world.FindSceneObject(room->configuration.screenObject);
        if (room->placed && object.IsValid() && object.GetWorldTransform(tv)) { seen_[room->regionId] = ProjectScreen(pose, tv, room->surface); }
    }
}

void Screen3DStage::ChooseProtected(std::map<uint64_t, Binding>& next) const {
    // ゲームカメラが無い時（編集中）は全画面に掛ける（エンジンが面積の大きい順に上限まで選ぶ）。
    if (seen_.empty()) { for (auto& [id, binding] : next) { binding.protect = true; } return; }
    // 保護の有無で画面の見え方が大きく変わるので、写っている画面は全部守る。
    // 一部が隠れていても守ってよい（エンジンが画素ごとに深度で確かめ、手前の物の上では外す）。
    const bool travelling = !travel_[0].empty();
    struct Candidate { uint64_t id; double coverage; bool was, travel; };
    std::vector<Candidate> eligible;
    for (auto& [id, binding] : next) {
        binding.protect = false;
        const auto seen = seen_.find(binding.regionId);
        if (seen == seen_.end()) { continue; }
        const auto old = bound_.find(id);
        const bool was = old != bound_.end() && old->second.protect;
        const bool travel = travelling && (binding.regionId == travel_[0] || binding.regionId == travel_[1]);
        const auto& v = seen->second;
        if (v.coverage <= 0 || v.facing <= .05) { continue; }
        eligible.push_back({id, v.coverage, was, travel});
    }
    // 上限を超える時だけ選ぶ：演出中は出る部屋・入る部屋を先に、次に守っていた画面、残りは大きい順。
    // 演出の外では、外の画面が選んだ中の最小の 1.6 倍を超えた時だけ入れ替える。
    std::sort(eligible.begin(), eligible.end(), [](const auto& a, const auto& b) {
        if (a.travel != b.travel) { return a.travel; }
        if (a.was != b.was) { return a.was; }
        return a.coverage > b.coverage;
    });
    std::vector<Candidate> chosen(eligible.begin(), eligible.begin() + static_cast<std::ptrdiff_t>((std::min)(eligible.size(), kMaxProtectedScreens)));
    for (size_t i = chosen.size(); i < eligible.size() && !travelling; ++i) {
        auto smallest = std::min_element(chosen.begin(), chosen.end(), [](const auto& a, const auto& b) { return a.coverage < b.coverage; });
        if (smallest != chosen.end() && eligible[i].coverage > smallest->coverage * 1.6) { *smallest = eligible[i]; }
    }
    for (const auto& candidate : chosen) { next[candidate.id].protect = true; }
}
}
