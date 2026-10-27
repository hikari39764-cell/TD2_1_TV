// スロット棚・テレビの塔・回転柱の 3D：規則の状態から棚の滑り、塔の抜き差し、柱の回転と付いている物の姿勢を作る。
#include "TD2MachineViewsShared.h"
#include "TD2SignalCable.h"
#include "TD2Transform.h"
#include "TD2ViewCull.h"
#include "HIKARI/Project/ActionViews.h"
#include "HIKARI/Project/GameFlowViews.h"
#include "HIKARI/Project/WorldQuery.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace TD2 {
using namespace MachineDetail;
namespace {
// テレビが base から pose へ動いた分だけ、付いている物の位置をずらす。
F3 Follow(const F3& attached, const F3& pose, const F3& base) {
    return {attached.x + pose.x - base.x, attached.y + pose.y - base.y, attached.z + pose.z - base.z};
}
// 動けなかった時の揺れ。残りは 1→0 で 0.35 秒で消える。
double ShakeLeft(double elapsed) { return (std::max)(0.0, 1 - elapsed / .35); }
float ShakeOffset(double elapsed, double amplitude) { return static_cast<float>(amplitude * ShakeLeft(elapsed) * std::sin(elapsed * 70)); }
}

// ---------------------------------------------------------------- スロット棚
// 世界地図 1 マスあたりの 3D のずれ（u は X、v は Y）を、同じ行・同じ列のメンバーから先に、残りは斜めの差から解き、
// 空きスロットの 3D の位置を求める。列と行の番号は、スロットの世界地図の X・Y を小さい順に並べた順位。
bool MachineViews::DeriveSlideSlots(Rules::SlideSource& source, const std::vector<MemberPlace>& members, const std::vector<std::pair<int, int>>& empty,
                                    std::vector<F3>& poses, std::string& error) const {
    const auto& o = members.front();
    bool hasU = false, hasV = false;
    F3 u{}, v{};
    const auto diff = [&o](const MemberPlace& m) { return F3{m.position.x - o.position.x, m.position.y - o.position.y, m.position.z - o.position.z}; };
    for (const auto& m : members) {
        const int dx = m.x - o.x, dy = m.y - o.y; const auto d = diff(m);
        if (dx && !dy && !hasU) { u = {d.x / dx, d.y / dx, d.z / dx}; hasU = true; }
        if (dy && !dx && !hasV) { v = {d.x / dy, d.y / dy, d.z / dy}; hasV = true; }
    }
    for (const auto& m : members) {
        const int dx = m.x - o.x, dy = m.y - o.y; const auto d = diff(m);
        if (!dx || !dy) continue;
        if (hasU && !hasV) { v = {(d.x - u.x * dx) / dy, (d.y - u.y * dx) / dy, (d.z - u.z * dx) / dy}; hasV = true; }
        else if (hasV && !hasU) { u = {(d.x - v.x * dy) / dx, (d.y - v.y * dy) / dx, (d.z - v.z * dy) / dx}; hasU = true; }
    }
    std::vector<std::pair<std::pair<int, int>, F3>> slots;
    for (const auto& m : members) slots.push_back({{m.x, m.y}, m.position});
    for (const auto& [ex, ey] : empty) {
        if ((ex && !hasU) || (ey && !hasV)) { error = "cannot infer the 3D position of an empty slot from the members"; return false; }
        slots.push_back({{o.x + ex, o.y + ey}, {o.position.x + u.x * ex + v.x * ey, o.position.y + u.y * ex + v.y * ey, o.position.z + u.z * ex + v.z * ey}});
    }
    std::vector<int> xs, ys;
    for (const auto& [place, p] : slots) { xs.push_back(place.first); ys.push_back(place.second); }
    std::sort(xs.begin(), xs.end()); xs.erase(std::unique(xs.begin(), xs.end()), xs.end());
    std::sort(ys.begin(), ys.end()); ys.erase(std::unique(ys.begin(), ys.end()), ys.end());
    source.slots.clear(); poses.clear();
    for (const auto& [place, p] : slots) {
        const int col = static_cast<int>(std::lower_bound(xs.begin(), xs.end(), place.first) - xs.begin());
        const int row = static_cast<int>(std::lower_bound(ys.begin(), ys.end(), place.second) - ys.begin());
        source.slots.push_back({place.first, place.second, col, row});
        poses.push_back(p);
    }
    return true;
}

void MachineViews::SyncSlides(HIKARI::WorldView& world, const RegionObjects& objects, const Placements& placements, MachineSources& out) {
    ForEach<SlideRackComponent>(world, kSlideRackComponentType, [&](HIKARI::ObjectView, SlideRackComponent& component) {
        Rules::SlideSource source; SlideView view; std::vector<std::pair<int, int>> empty; std::vector<MemberPlace> members; std::string error;
        if (!component.BuildSource(source, empty, error)) { std::fprintf(stderr, "[TD2 configuration] disabled slide error=%s\n", error.c_str()); return; }
        if (std::string missing; !MemberPlaces(world, objects, placements, source.members, members, missing)) error = "member not placed: " + missing;
        else (void)DeriveSlideSlots(source, members, empty, view.slots, error);
        if (!error.empty()) { std::fprintf(stderr, "[TD2 configuration] disabled slide %s %s\n", source.id.c_str(), error.c_str()); return; }
        // 区域カメラは動かさない（棚全体を写す同じ構図のまま、空いたスロットと並びが見え続ける）。
        for (const auto& region : source.members) view.members.push_back(ReadMember(world, objects, region, false));
        std::fprintf(stderr, "[TD2 slide] %s slots=%zu members=%zu\n", source.id.c_str(), source.slots.size(), source.members.size());
        out.slides.push_back(std::move(source)); slides_.push_back(std::move(view));
    });
}

void MachineViews::UpdateSlides(const Frame& frame, Writes& writes, double seconds) {
    const auto& runtimes = frame.exploration.Slides();
    for (size_t i = 0; i < slides_.size() && i < runtimes.size(); ++i) {
        auto& view = slides_[i]; const auto& runtime = runtimes[i];
        // 動けなかった：そのテレビを 0.35 秒だけ左右に細かく揺らす。
        if (runtime.blockedCount != view.blockedCount) { view.blockedCount = runtime.blockedCount; view.shaking = runtime.blocked; view.shake = 0; }
        const bool moving = runtime.moving >= 0;
        if (!moving && view.lastMoving < 0 && view.shaking < 0 && !resync_) continue;
        for (size_t m = 0; m < view.members.size() && m < runtime.slotOf.size(); ++m) {
            auto& member = view.members[m];
            if (!member.tv.IsValid()) continue;
            const bool thisMoving = moving && runtime.moving == static_cast<int>(m);
            if (!thisMoving && view.lastMoving != static_cast<int>(m) && view.shaking != static_cast<int>(m) && !resync_) continue;
            // スロットの位置の間を、両端でゆっくり止まるように運ぶ。
            const auto& to = view.slots[static_cast<size_t>(thisMoving ? runtime.to : runtime.slotOf[m])];
            const auto& from = view.slots[static_cast<size_t>(thisMoving ? runtime.from : runtime.slotOf[m])];
            const float e = Math::SmoothStep(thisMoving ? static_cast<float>(runtime.progress) : 1.f);
            auto pose = member.tvPose;
            pose.position = Lerp(from, to, e);
            if (view.shaking == static_cast<int>(m)) {
                view.shake += seconds;
                pose.position.x += ShakeOffset(view.shake, .025);
                if (ShakeLeft(view.shake) <= 0) view.shaking = -1;
            }
            Push(writes, member.tv, pose);
        }
        view.lastMoving = runtime.moving;
    }
}

// ---------------------------------------------------------------- テレビの塔
void MachineViews::SyncStacks(HIKARI::WorldView& world, const RegionObjects& objects, const Placements& placements, MachineSources& out) {
    ForEach<StackColumnComponent>(world, kStackColumnComponentType, [&](HIKARI::ObjectView, StackColumnComponent& component) {
        Rules::StackSource source; StackView view; std::vector<double> distances; std::vector<MemberPlace> members; std::string error;
        if (!component.BuildSource(source, distances, error)) { std::fprintf(stderr, "[TD2 configuration] disabled stack error=%s\n", error.c_str()); return; }
        // スロットはメンバーの今の配置とテレビの位置。置き場は一番下のテレビの背の側へ距離ぶん、世界地図では預け場所。
        if (std::string missing; !MemberPlaces(world, objects, placements, source.members, members, missing)) {
            std::fprintf(stderr, "[TD2 configuration] disabled stack %s member not placed: %s\n", source.id.c_str(), missing.c_str()); return;
        }
        for (const auto& m : members) { source.slots.push_back({m.x, m.y}); view.slots.push_back(m.position); }
        HIKARI::GameProjectTransform bottom{};
        const auto tv = objects.televisions.find(source.members.front());
        if (tv == objects.televisions.end() || !world.FindObject(tv->second).GetLocalTransform(bottom)) {
            std::fprintf(stderr, "[TD2 configuration] disabled stack %s missing bottom television\n", source.id.c_str()); return;
        }
        const auto back = QRotate(bottom.rotation, {0, 0, -1});
        for (const double length : distances) {
            const float distance = static_cast<float>(length);
            source.docks.push_back(Park());
            view.docks.push_back({bottom.position.x + back.x * distance, bottom.position.y + back.y * distance, bottom.position.z + back.z * distance});
        }
        for (const auto& region : source.members) view.members.push_back(ReadMember(world, objects, region, true));
        std::fprintf(stderr, "[TD2 stack] %s slots=%zu docks=%zu members=%zu\n", source.id.c_str(), source.slots.size(), source.docks.size(), source.members.size());
        out.stacks.push_back(std::move(source)); stacks_.push_back(std::move(view));
    });
}

void MachineViews::UpdateStacks(const Frame& frame, Writes& writes, double seconds) {
    const auto& runtimes = frame.exploration.Stacks();
    for (size_t i = 0; i < stacks_.size() && i < runtimes.size(); ++i) {
        auto& view = stacks_[i]; const auto& s = runtimes[i];
        if (s.blockedCount != view.blockedCount) { view.blockedCount = s.blockedCount; view.shake = 0; }
        const bool moving = s.phase != 0;
        if (!moving && view.lastPhase == 0 && view.shake < 0 && !resync_) continue;
        const auto placeOf = [&](int place) -> const F3* {
            if (place >= 0 && static_cast<size_t>(place) < view.slots.size()) return &view.slots[static_cast<size_t>(place)];
            const auto dock = static_cast<size_t>(-1 - place);
            return dock < view.docks.size() ? &view.docks[dock] : nullptr;
        };
        // 抜くのは両端でゆっくり、落ちるのはだんだん速く（重さが見えるように）。
        // 戻す時は二段：前半に塔の中のテレビが上のスロットへ上がり、置き場のテレビはその場で戻るスロットの高さまで吊り上がる。
        // 後半に置き場のテレビが水平にスロットへ滑り込む（途中で他のテレビと重ならない）。
        const double t = moving ? s.progress : 1.0;
        for (size_t m = 0; m < view.members.size() && m < s.at.size(); ++m) {
            auto& member = view.members[m];
            if (!member.tv.IsValid()) continue;
            const F3* a = placeOf(s.at[m]); const F3* b = placeOf(moving ? s.to[m] : s.at[m]);
            if (!a || !b) continue;
            double ey = s.phase == 2 ? t * t : Math::SmoothStep(t), ex = ey;
            if (s.phase == 3) { ey = Math::SmoothStep(t / .4); ex = s.at[m] < 0 ? Math::SmoothStep((t - .4) / .6) : ey; }
            auto pose = member.tvPose;
            pose.position = {static_cast<float>(a->x + (b->x - a->x) * ex), static_cast<float>(a->y + (b->y - a->y) * ey),
                             static_cast<float>(a->z + (b->z - a->z) * ex)};
            if (view.shake >= 0 && !moving && s.at[m] == 0) pose.position.z += ShakeOffset(view.shake, .03);
            Push(writes, member.tv, pose);
            if (member.camera.IsValid()) {
                auto camera = member.cameraPose;
                camera.position = Follow(camera.position, pose.position, member.tvPose.position);
                Push(writes, member.camera, camera);
            }
        }
        if (view.shake >= 0) { view.shake += seconds; if (view.shake > .35) view.shake = -1; }
        view.lastPhase = s.phase;
    }
}

// ---------------------------------------------------------------- 回転柱
void MachineViews::SyncCarousels(HIKARI::WorldView& world, const RegionObjects& objects, std::vector<Rules::RegionSource>& regions, MachineSources& out) {
    ForEach<CarouselComponent>(world, kCarouselComponentType, [&](HIKARI::ObjectView owner, CarouselComponent& component) {
        Rules::CarouselSource source; CarouselView view; std::vector<uint64_t> extra; std::string error;
        if (!component.BuildSource(source, extra, error)) { std::fprintf(stderr, "[TD2 configuration] disabled carousel error=%s\n", error.c_str()); return; }
        // この部品を付けた柱の位置を軸にし、柱自体も一緒に回す。
        HIKARI::GameProjectTransform column{};
        if (!owner.GetLocalTransform(column)) { std::fprintf(stderr, "[TD2 configuration] disabled carousel %s no column pose\n", source.id.c_str()); return; }
        view.pivot = column.position;
        if (const auto id = owner.GetSceneObjectId(); id && std::find(extra.begin(), extra.end(), id) == extra.end()) extra.push_back(id);
        // 立ち位置は面の今の配置。世界地図に置いていない面は預け場所（どこともつながらない行き止まり）へ置く。
        for (const auto& face : source.faces) {
            const auto region = std::find_if(regions.begin(), regions.end(), [&](const Rules::RegionSource& r) { return r.placement.regionId == face; });
            if (region == regions.end()) { std::fprintf(stderr, "[TD2 configuration] disabled carousel %s missing face %s\n", source.id.c_str(), face.c_str()); return; }
            if (!region->placement.worldPlaced) {
                const auto [x, y] = Park();
                region->placement.worldPlaced = true; region->placement.worldX = x; region->placement.worldY = y;
            }
            source.stations.push_back({region->placement.worldX, region->placement.worldY});
        }
        view.count = source.faces.size();
        const auto add = [&](HIKARI::GameProjectObjectHandle handle, HIKARI::GameProjectObjectHandle tv, bool camera) {
            CarouselPart part; part.handle = handle; part.tv = tv; part.camera = camera;
            if (const auto object = world.FindObject(handle); object.IsValid() && object.GetLocalTransform(part.pose)) view.parts.push_back(part);
        };
        // 面のテレビと、その区域カメラ（面と一緒に回り、乗っている面を見続ける）。
        for (const auto& face : source.faces) {
            const auto tv = objects.televisions.find(face);
            if (tv == objects.televisions.end()) continue;
            add(tv->second, tv->second, false);
            if (const auto camera = objects.cameras.find(face); camera != objects.cameras.end() && camera->second)
                if (const auto object = world.FindSceneObject(camera->second); object.IsValid()) add(object.GetHandle(), tv->second, true);
        }
        for (const auto id : extra)
            if (const auto object = world.FindSceneObject(id); object.IsValid()) add(object.GetHandle(), {}, false);
        std::fprintf(stderr, "[TD2 carousel] %s faces=%zu parts=%zu\n", source.id.c_str(), source.faces.size(), view.parts.size());
        out.carousels.push_back(std::move(source)); carousels_.push_back(std::move(view));
    });
}

void MachineViews::UpdateCarousels(const Frame& frame, Writes& writes) {
    const auto& runtimes = frame.exploration.Carousels();
    for (size_t i = 0; i < carousels_.size() && i < runtimes.size(); ++i) {
        auto& view = carousels_[i]; const auto& c = runtimes[i];
        // 回っている間、面のテレビ（操作中の面を除く）は砂嵐にする（回るにつれて覗き窓の角度が大きく振れる）。
        if (c.direction != 0) {
            for (const auto& part : view.parts) {
                if (part.camera || !part.tv.IsValid() || part.tv.value == frame.activeTv.value) continue;
                if (const auto room = tvRooms_.find(part.tv.value); room != tvRooms_.end()) snowUntil_[room->second] = clock_ + kSnowTail;
            }
        }
        const double turns = c.step + c.direction * Math::SmoothStep(c.progress);
        if (turns == view.written) continue;
        view.written = turns;
        // 一面ぶん = 360°/面の数。正の向きは +Z を +X へ回す向き（正面の面が見る人の右へ回る）。
        const auto q = QFromEulerDeg(0, static_cast<float>(turns * 360.0 / static_cast<double>(view.count)), 0);
        for (const auto& part : view.parts) {
            auto pose = part.pose;
            const auto r = QRotate(q, Sub(part.pose.position, view.pivot));
            pose.position = {view.pivot.x + r.x, view.pivot.y + r.y, view.pivot.z + r.z};
            pose.rotation = QMul(q, part.pose.rotation);
            // 操作中の面のカメラなら、追従のばねも同じだけ運ぶ（回る面を見失わない）。
            if (part.camera) Moved(frame, part.handle.value, part.tv, part.pose, pose);
            Push(writes, part.handle, pose);
        }
    }
}
}
