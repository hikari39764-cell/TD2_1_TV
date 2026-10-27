// 進みの保存と復元：検査点・仕掛けの状態・宝石・封じた口を Progress へ書き出し、読み戻して世界を合わせる。
#include "TD2Exploration.h"
#include <algorithm>
#include <cmath>
#include <utility>

namespace TD2::Rules {

Progress Exploration::SaveProgress() const {
    Progress p;
    // 復活点は、それがある部屋の中の位置で残す（部屋が仕掛けで動いた世界でも、戻した後の同じ部屋に立たせる）。
    const Rect body{state_.checkpointPosition.x, state_.checkpointPosition.y, state_.player.rect.w, state_.player.rect.h};
    p.region = RegionAt(body);
    if (p.region.empty()) p.region = activeRegion_;
    if (const auto* room = FindRegion(p.region)) p.local = {body.x - room->placement.worldX, body.y + body.h - room->placement.worldY};
    p.checkpoint = state_.checkpointId;
    // 動いている途中の仕掛けは、向かっている先の状態で残す（板で動かした物は戻した後も同じ先へ向かうので、着いた姿で戻す）。
    for (const auto& m : mechanisms_) p.mechanisms.push_back({m.source.id, m.target});
    for (const auto& s : slides_) p.slides.push_back({s.source.id, s.slotOf});
    for (const auto& s : stacks_) p.stacks.push_back({s.source.id, s.at});
    for (const auto& c : carousels_) p.carousels.push_back({c.source.id, c.step});
    for (size_t i = 0; i < world_.devices.size() && i < state_.devices.size(); ++i) {
        const auto& d = world_.devices[i];
        if (d.kind == DeviceKind::Plate && d.latch && state_.devices[i].active) p.latched.push_back(d.id);
        if (d.kind == DeviceKind::Collectible && state_.devices[i].active) p.taken.push_back(d.id);
    }
    for (const auto& edge : sealed_) p.sealed.push_back(edge);
    return p;
}

std::string Exploration::RestoreProgress(const Progress& p) {
    // 1. 部屋の行き先を仕掛けごとに集め、一度にまとめて動かす（一つずつ動かすと途中で重なる並びがある）。
    std::map<std::string, MechanismMove> moves;
    const auto place = [&](const std::string& region, int x, int y, bool flip) { moves[region] = {region, x, y, flip}; };
    const auto flipped = [this](const std::string& region) { return regions_.at(region).flipped; };
    for (const auto& [id, value] : p.mechanisms) {
        const auto m = std::find_if(mechanisms_.begin(), mechanisms_.end(), [&](const MechanismRuntime& r) { return r.source.id == id; });
        if (m == mechanisms_.end()) continue;
        if (value < -1 || value > 1 || (value < 0 && !m->source.balance)) return "bad mechanism state " + id;
        for (const auto& move : TargetsFor(*m, value)) place(move.regionId, move.worldX, move.worldY, move.flip);
    }
    for (const auto& [id, slots] : p.slides) {
        const auto s = std::find_if(slides_.begin(), slides_.end(), [&](const SlideRuntime& r) { return r.source.id == id; });
        if (s == slides_.end()) continue;
        const int count = static_cast<int>(s->source.slots.size());
        if (slots.size() != s->source.members.size()) return "bad slide " + id;
        for (size_t i = 0; i < slots.size(); ++i) {
            if (slots[i] < 0 || slots[i] >= count || std::count(slots.begin(), slots.end(), slots[i]) != 1) return "bad slide " + id;
            const auto& slot = s->source.slots[static_cast<size_t>(slots[i])];
            place(s->source.members[i], slot.worldX, slot.worldY, flipped(s->source.members[i]));
        }
    }
    for (const auto& [id, places] : p.stacks) {
        const auto s = std::find_if(stacks_.begin(), stacks_.end(), [&](const StackRuntime& r) { return r.source.id == id; });
        if (s == stacks_.end()) continue;
        if (places.size() != s->source.members.size()) return "bad stack " + id;
        for (size_t i = 0; i < places.size(); ++i) {
            const int k = places[i];
            const bool slot = k >= 0;
            const size_t index = static_cast<size_t>(slot ? k : -1 - k);
            if (index >= (slot ? s->source.slots.size() : s->source.docks.size()) || std::count(places.begin(), places.end(), k) != 1) return "bad stack " + id;
            const auto& at = slot ? s->source.slots[index] : s->source.docks[index];
            place(s->source.members[i], at.first, at.second, flipped(s->source.members[i]));
        }
    }
    for (const auto& [id, step] : p.carousels) {
        const auto c = std::find_if(carousels_.begin(), carousels_.end(), [&](const CarouselRuntime& r) { return r.source.id == id; });
        if (c == carousels_.end()) continue;
        const int count = static_cast<int>(c->source.faces.size());
        if (step < 0 || step >= count) return "bad carousel " + id;
        for (int i = 0; i < count; ++i) {
            const auto& station = c->source.stations[static_cast<size_t>((i + step) % count)];
            place(c->source.faces[static_cast<size_t>(i)], station.first, station.second, flipped(c->source.faces[static_cast<size_t>(i)]));
        }
    }
    std::vector<MechanismMove> targets;
    for (const auto& [region, move] : moves) targets.push_back(move);
    if (!MoveRegions(targets)) return "rooms overlap";
    // 2. 仕掛けを確定した状態に置く（動いていない・待っていない）。
    for (const auto& [id, value] : p.mechanisms)
        for (auto& m : mechanisms_) if (m.source.id == id) {
            m.state = m.target = m.pending = value; m.progress = value; m.moving = false; m.rest = 0; m.pendingTime = 0; m.rideArmed = false;
        }
    for (const auto& [id, slots] : p.slides)
        for (auto& s : slides_) if (s.source.id == id) { s.slotOf = slots; s.moving = s.from = s.to = s.blocked = -1; s.progress = 0; }
    for (const auto& [id, places] : p.stacks)
        for (auto& s : stacks_) if (s.source.id == id) { s.at = s.to = places; s.phase = 0; s.progress = 0; }
    for (const auto& [id, step] : p.carousels)
        for (auto& c : carousels_) if (c.source.id == id) { c.step = step; c.direction = 0; c.progress = 0; }
    // 3. 切り替え式の板と宝石。受け口の通電を今の板にそろえ、読み込んだ直後に踏んだ扱いで切り替わらないようにする。
    const auto has = [](const std::vector<std::string>& list, const std::string& id) { return std::find(list.begin(), list.end(), id) != list.end(); };
    for (size_t i = 0; i < world_.devices.size() && i < state_.devices.size(); ++i) {
        const auto& d = world_.devices[i];
        if (d.kind == DeviceKind::Plate && d.latch) state_.devices[i].active = has(p.latched, d.id);
        if (d.kind == DeviceKind::Collectible) state_.devices[i].active = has(p.taken, d.id);
    }
    const auto active = [this](size_t index) { return index < state_.devices.size() && state_.devices[index].active; };
    for (auto& m : mechanisms_) m.powered = active(m.device);
    for (auto& s : slides_) for (size_t i = 0; i < s.devices.size() && i < s.powered.size(); ++i) s.powered[i] = active(s.devices[i]);
    isolated_.clear();
    sealed_.clear(); pendingSeals_.clear();
    for (const auto& [a, b] : p.sealed) if (regions_.contains(a) && regions_.contains(b)) sealed_.insert(std::minmax(a, b));
    BuildBoundaries();
    // 4. 復活点へ立たせる。
    const auto* room = FindRegion(p.region);
    if (!room || !room->placement.worldPlaced) return "missing region " + p.region;
    const Vec2 at{p.local.x, p.local.y - state_.player.rect.h};
    if (!Teleport(p.region, &at)) return "cannot place player in " + p.region;
    state_.checkpointId = p.checkpoint;
    // 乗り物の部屋で続きから始めた時は運ばれた後として扱う（決めた口へ降りれば、そこを閉じる）。
    for (auto& m : mechanisms_)
        if (m.source.ride) m.carried = std::any_of(m.origins.begin(), m.origins.end(), [&](const MechanismMove& o) { return o.regionId == p.region; });
    // 乗り物の部屋で続きから始めた時は、その部屋の閉じた口を開け直す（そこで死んで起き上がった時と同じ。閉じ込めない）。
    bool reopened = false;
    for (const auto& m : mechanisms_) {
        if (!m.source.ride || std::none_of(m.origins.begin(), m.origins.end(), [&](const MechanismMove& o) { return o.regionId == p.region; })) continue;
        for (const auto& exit : m.source.sealExits) reopened = sealed_.erase(std::minmax(p.region, exit)) > 0 || reopened;
    }
    if (reopened) BuildBoundaries();
    snapshotDirty_ = true;
    return {};
}
}
