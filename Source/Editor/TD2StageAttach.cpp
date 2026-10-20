#include "TD2StageAttach.h"
#include "../Common/TD2Math.h"

namespace TD2::Editor {
namespace {
using Math::Vec3;
Vec3 Axis(const Pose& pose, const Vec3& local) { return Math::QRotate(Math::QFromEulerDeg(pose.rotation), local); }
}

ScreenBox BoxOf(const LayoutRoom& room) {
    ScreenBox box;
    box.pose.position = room.tv.position; box.pose.rotation = room.tv.rotation; box.pose.scale = room.tv.scale;
    for (int i = 0; i < 3; ++i) { box.body[i] = room.body[i]; box.screenCenter[i] = room.screenCenter[i]; }
    box.screenWidth = room.screenWidth; box.screenHeight = room.screenHeight;
    box.cols = room.cols; box.rows = room.rows;
    return box;
}

RegionPlacement PlacementOf(const LayoutRoom& room) { return {room.regionId, room.cols, room.rows, room.placed, room.worldX, room.worldY}; }

std::optional<Pose> AttachedPose(const ScreenBox& r, const RegionPlacement& rp, const ScreenBox& n, const RegionPlacement& np, double gap) {
    const auto edge = FindSharedWorldEdge(np, rp);
    if (!edge) { return std::nullopt; }
    const double sr = r.Scale(), sn = n.Scale();
    const double center = (static_cast<double>(edge->overlapBegin) + static_cast<double>(edge->overlapEnd)) * .5;
    // neighbor の向きの座標（右・上・前）で、room の原点を置く位置を求める。
    double ox = 0, oy = 0;
    const bool vertical = edge->sideA == WorldSide::Left || edge->sideA == WorldSide::Right;
    if (vertical) {
        // 共有する行の中央の高さを両方の画面で揃える（地図の下向きは 3D の上の逆）。
        const double yn = (n.screenCenter[1] + n.screenHeight * .5) * sn - (center - np.worldY) * n.CellHeight();
        const double yr = (r.screenCenter[1] + r.screenHeight * .5) * sr - (center - rp.worldY) * r.CellHeight();
        oy = yn - yr;
        const double half = n.body[0] * sn * .5 + gap + r.body[0] * sr * .5;
        ox = edge->sideA == WorldSide::Right ? half : -half;
    } else {
        const double xn = (n.screenCenter[0] - n.screenWidth * .5) * sn + (center - np.worldX) * n.CellWidth();
        const double xr = (r.screenCenter[0] - r.screenWidth * .5) * sr + (center - rp.worldX) * r.CellWidth();
        ox = xn - xr;
        // 上は neighbor の筐体の上、下は neighbor の原点（底）より下。
        oy = edge->sideA == WorldSide::Top ? n.body[1] * sn + gap : -(r.body[1] * sr + gap);
    }
    // 画面の面を揃える。
    const double oz = n.screenCenter[2] * sn - r.screenCenter[2] * sr;
    Pose pose;
    const auto right = Axis(n.pose, {1, 0, 0}), up = Axis(n.pose, {0, 1, 0}), forward = Axis(n.pose, {0, 0, 1});
    for (int i = 0; i < 3; ++i) { pose.position[static_cast<size_t>(i)] = n.pose.position[static_cast<size_t>(i)] + right[static_cast<size_t>(i)] * ox + up[static_cast<size_t>(i)] * oy + forward[static_cast<size_t>(i)] * oz; }
    for (auto& v : pose.position) { v = std::round(v * 10000) / 10000; }
    pose.rotation = n.pose.rotation;
    pose.scale = r.pose.scale;
    return pose;
}

MachineObjectDelta MoveDelta(const ScreenBox& r, const RegionPlacement& a, const RegionPlacement& b,
                             const std::vector<std::pair<ScreenBox, RegionPlacement>>& neighbors, bool* aligned) {
    MachineObjectDelta d;
    if (aligned) { *aligned = false; }
    const int dx = b.worldX - a.worldX, dy = b.worldY - a.worldY;
    if (!dx && !dy) { return d; }
    // b で一番長く辺を共有するテレビ。
    const std::pair<ScreenBox, RegionPlacement>* best = nullptr; int64_t length = 0;
    for (const auto& item : neighbors) {
        if (item.second.regionId == b.regionId) { continue; }
        if (const auto edge = FindSharedWorldEdge(item.second, b); edge && edge->overlapEnd - edge->overlapBegin > length) { length = edge->overlapEnd - edge->overlapBegin; best = &item; }
    }
    double right = dx * r.CellWidth(), up = -dy * r.CellHeight();
    if (best) {
        if (const auto pose = AttachedPose(r, b, best->first, best->second)) {
            const auto ar = Axis(r.pose, {1, 0, 0}), au = Axis(r.pose, {0, 1, 0});
            const Vec3 move{pose->position[0] - r.pose.position[0], pose->position[1] - r.pose.position[1], pose->position[2] - r.pose.position[2]};
            // 動く向きの成分だけを使う（吊り台は横の位置と奥行きを変えない）。
            if (dx) { right = Math::Dot(move, ar); }
            if (dy) { up = Math::Dot(move, au); }
            if (aligned) { *aligned = true; }
        }
    }
    d.position = {std::round(right * 10000) / 10000, std::round(up * 10000) / 10000, 0};
    return d;
}

MachineObjectDelta RetargetMove(const MachineObjectDelta& d, const Pose& from, const Pose& to) {
    const auto world = Math::QRotate(Math::QFromEulerDeg(from.rotation), {d.position[0], d.position[1], d.position[2]});
    const auto local = Math::QRotate(Math::QConj(Math::QFromEulerDeg(to.rotation)), world);
    MachineObjectDelta out = d;
    for (size_t i = 0; i < 3; ++i) { out.position[i] = std::round(local[i] * 10000) / 10000; }
    out.rotation = {0, 0, 0}; out.hasPivot = false;
    return out;
}
}
