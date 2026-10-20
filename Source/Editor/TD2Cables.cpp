#include "TD2Cables.h"
#include "../Common/TD2Math.h"
#include <cmath>

namespace TD2::Editor {
namespace {
constexpr double kFloor = .02, kMargin = .05;

// p の足元の台（上面が p の高さの近くにあり、p の真下にある物）。
const LayoutBox* SupportUnder(const Vec3& p, const LayoutSnapshot& layout) {
    const LayoutBox* best = nullptr;
    for (const auto& box : layout.obstacles) {
        if (p[0] < box.lo[0] || p[0] > box.hi[0] || p[2] < box.lo[2] || p[2] > box.hi[2]) { continue; }
        if (box.hi[1] < p[1] - .25 || box.hi[1] > p[1] + .08) { continue; }
        if (!best || box.hi[1] > best->hi[1]) { best = &box; }
    }
    return best;
}

// 台の上の点から、台の縁を越えて床へ降りるまでの点（台が無ければそのまま床へ）。toward の方の縁を少し選びやすくする。
std::vector<Vec3> DownToFloor(const Vec3& p, const Vec3& toward, const LayoutSnapshot& layout) {
    std::vector<Vec3> out;
    if (p[1] <= kFloor + .03) { return out; }
    const auto* box = SupportUnder(p, layout);
    if (!box) { out.push_back({p[0], kFloor, p[2]}); return out; }
    const double top = box->hi[1] + .01;
    // 四つの縁のうち、近く（行き先の方は少し近いとみなす）の縁へ出る。
    struct Exit { double cost; Vec3 at; };
    const double dx = toward[0] - p[0], dz = toward[2] - p[2];
    const std::array<Exit, 4> exits{{
        {p[0] - box->lo[0] - (dx < 0 ? .3 : 0), {box->lo[0] - kMargin, top, p[2]}},
        {box->hi[0] - p[0] - (dx > 0 ? .3 : 0), {box->hi[0] + kMargin, top, p[2]}},
        {p[2] - box->lo[2] - (dz < 0 ? .3 : 0), {p[0], top, box->lo[2] - kMargin}},
        {box->hi[2] - p[2] - (dz > 0 ? .3 : 0), {p[0], top, box->hi[2] + kMargin}},
    }};
    const auto best = std::min_element(exits.begin(), exits.end(), [](const Exit& a, const Exit& b) { return a.cost < b.cost; });
    out.push_back({p[0], top, p[2]});
    out.push_back(best->at);
    out.push_back({best->at[0], kFloor, best->at[2]});
    return out;
}
}

Vec3 CableEnd(const LayoutRoom& room) {
    const auto q = Math::QFromEulerDeg(room.tv.rotation);
    const auto back = Math::QRotate(q, {0, 0, -(room.body[2] * std::abs(room.tv.scale[2]) * .5 + kMargin)});
    return {room.tv.position[0] + back[0], room.tv.position[1] + .02, room.tv.position[2] + back[2]};
}

std::optional<Vec3> MachineCableEnd(const Machine& m, const LayoutSnapshot& layout) {
    for (const auto& part : m.parts) {
        if (!part.owned || part.moves) { continue; }
        if (const auto t = layout.transforms.find(part.object); t != layout.transforms.end()) { return t->second.position; }
    }
    if (m.kind == MachineKind::Carousel) {
        if (const auto t = layout.transforms.find(m.objectId); t != layout.transforms.end()) { return Vec3{t->second.position[0], kFloor, t->second.position[2]}; }
    }
    const auto regions = m.Regions();
    if (!regions.empty()) { if (const auto* room = layout.Room(regions.front())) { return CableEnd(*room); } }
    return std::nullopt;
}

std::vector<Vec3> RouteCable(const Vec3& from, const Vec3& to, const LayoutSnapshot& layout) {
    std::vector<Vec3> points{from};
    const auto down = DownToFloor(from, to, layout);
    points.insert(points.end(), down.begin(), down.end());
    auto up = DownToFloor(to, from, layout);
    std::reverse(up.begin(), up.end());
    const Vec3 a = points.back(), b = up.empty() ? Vec3{to[0], kFloor, to[2]} : up.front();
    // 床の上は X と Z の二本の直線で行く（斜めに横切らない）。
    if (a[1] <= kFloor + .03) { points.push_back({b[0], kFloor, a[2]}); points.push_back({b[0], kFloor, b[2]}); }
    points.insert(points.end(), up.begin(), up.end());
    if (up.empty() && to[1] > kFloor + .03) { points.push_back({to[0], kFloor, to[2]}); }
    points.push_back(to);
    // 重なった点と一直線の途中の点を除く。
    std::vector<Vec3> clean;
    for (const auto& p : points) {
        if (!clean.empty() && std::hypot(p[0] - clean.back()[0], p[1] - clean.back()[1], p[2] - clean.back()[2]) < .01) { continue; }
        if (clean.size() >= 2) {
            const auto& p0 = clean[clean.size() - 2]; const auto& p1 = clean.back();
            const Vec3 d0{p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]}, d1{p[0] - p1[0], p[1] - p1[1], p[2] - p1[2]};
            const double cross = std::hypot(d0[1] * d1[2] - d0[2] * d1[1], d0[2] * d1[0] - d0[0] * d1[2], d0[0] * d1[1] - d0[1] * d1[0]);
            if (cross < 1e-6 && d0[0] * d1[0] + d0[1] * d1[1] + d0[2] * d1[2] > 0) { clean.back() = p; continue; }
        }
        clean.push_back(p);
    }
    return clean;
}

std::vector<CablePiece> CablePieces(const std::vector<Vec3>& points) {
    std::vector<CablePiece> out;
    for (size_t i = 0; i < points.size(); ++i) {
        CablePiece joint; joint.joint = true; joint.pose.position = points[i];
        out.push_back(joint);
        if (i + 1 == points.size()) { break; }
        const auto& a = points[i]; const auto& b = points[i + 1];
        const double dx = b[0] - a[0], dy = b[1] - a[1], dz = b[2] - a[2], length = std::hypot(dx, dy, dz);
        if (length < .01) { continue; }
        // 直管はローカル +Z が長さ。エンジンの回転順（Rz·Ry·Rx）で +Z を線の向きへ向ける。
        CablePiece segment; segment.pose.position = a;
        segment.pose.rotation = {std::round(-std::asin(std::clamp(dy / length, -1.0, 1.0)) * 180 / Math::kPi * 1000) / 1000,
                                 std::round(std::atan2(dx, dz) * 180 / Math::kPi * 1000) / 1000, 0};
        segment.pose.scale = {1, 1, std::round(length * 10000) / 10000};
        out.push_back(segment);
    }
    return out;
}
}
