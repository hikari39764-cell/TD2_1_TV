// 世界の仕掛け：機構・スロット棚・テレビの塔・回転柱の進みと、区域を世界地図の上で動かす処理。
#include "TD2Exploration.h"
#include <algorithm>
#include <cmath>
#include <utility>

namespace TD2::Rules {

void Exploration::UpdateMechanisms(double seconds) {
    for (auto& m : mechanisms_) {
        const auto active = [this](size_t index) { return index < state_.devices.size() && state_.devices[index].active; };
        const bool powered = active(m.device);
        if (m.source.dwell > 0) {
            // 自動往復：端で dwell 秒止まったら反対の端へ向かう（圧力板は見ない）。
            if (!m.moving && std::abs(m.progress - m.target) < 1e-9 && (m.rest += seconds) >= m.source.dwell) {
                m.target = m.target ? 0 : 1; m.rest = 0;
            }
        } else if (m.source.ride) {
            // 乗り込んだ部屋の中に体が全部収まって着地したら、反対の端へ向かう（入口をまたいだまま動かない）。
            // 乗り込んだ部屋（動く部屋か、rideFrom の出発の部屋）に今いる時だけ動かす。
            const bool aboard = std::any_of(m.origins.begin(), m.origins.end(), [&](const MechanismMove& o) { return o.regionId == activeRegion_; }) ||
                                std::find(m.source.rideFrom.begin(), m.source.rideFrom.end(), activeRegion_) != m.source.rideFrom.end();
            const auto* region = m.rideArmed && aboard ? FindRegion(activeRegion_) : nullptr;
            if (region && !m.moving && std::abs(m.progress - m.target) < 1e-9 && state_.player.grounded && !state_.player.dead) {
                const auto& p = region->placement;
                const auto& b = state_.player.rect;
                if (b.x >= p.worldX && b.y >= p.worldY && b.x + b.w <= p.worldX + p.cols && b.y + b.h <= p.worldY + p.rows) {
                    m.target = m.source.rideFrom.empty() ? (m.target ? 0 : 1) : 1; m.rideArmed = false;
                }
            }
        } else if (m.source.toggle) { if (powered && !m.powered) m.target = m.target ? 0 : 1; }
        else {
            const bool right = m.source.balance && active(m.deviceR);
            const int desired = m.source.balance ? (powered && !right ? 1 : right && !powered ? -1 : 0) : powered ? 1 : 0;
            // 踏まれ方が settle 秒続いた時だけ向かう先を変える。
            if (desired == m.target) { m.pending = desired; m.pendingTime = 0; }
            else {
                if (desired != m.pending) { m.pending = desired; m.pendingTime = 0; } else m.pendingTime += seconds;
                if (m.pendingTime + 1e-9 >= m.source.settle) m.target = desired;
            }
        }
        m.powered = powered;
        const double goal = m.target;
        if (std::abs(m.progress - goal) < 1e-9) continue;
        if (!m.moving) {
            // 動き始め：動く部屋をどこともつながない（中にいる体は出られず、外からも入れない）。
            m.moving = true;
            for (const auto& origin : m.origins) isolated_.insert(origin.regionId);
            BuildBoundaries();
        }
        const double step = std::max(0.0, seconds) / std::max(.05, m.source.seconds);
        m.progress = goal > m.progress ? std::min(goal, m.progress + step) : std::max(goal, m.progress - step);
        if (std::abs(m.progress - goal) > 1e-9) continue;
        // 着いた：向かっていた状態が今と違えば部屋の位置を切り替え、つなぎ直す。重なって切り替えられない時は元の状態へ戻す。
        if (m.target != m.state && MoveRegions(TargetsFor(m, m.target))) m.state = m.target;
        else if (m.target != m.state) { m.target = m.state; m.progress = m.state; }
        m.moving = false;
        // 乗り物：着いた時に体が中にいれば、運んだ印を付ける（降りた口を閉じるため）。
        if (m.source.ride) {
            const auto here = RegionAt(state_.player.rect);
            m.carried = !state_.player.dead && std::any_of(m.origins.begin(), m.origins.end(), [&](const MechanismMove& o) { return o.regionId == here; });
            // 運び終えたら、その部屋の一番近い検査点で起き上がるようにする。乗る前の部屋で起きると、
            // 乗り物はもう向こうに着いていて戻れない（s4 へ b8 から落ちた後、左半分の検査点には触れない）。
            if (m.carried) {
                std::optional<size_t> best; double score = 0;
                const auto& b = state_.player.rect;
                for (size_t i = 0; i < world_.devices.size(); ++i) {
                    const auto& dev = world_.devices[i];
                    const auto owner = deviceOwners_.find(dev.id);
                    if (dev.kind != DeviceKind::Checkpoint || owner == deviceOwners_.end() || owner->second != here) continue;
                    const double d = std::hypot(dev.rect.x + dev.rect.w / 2 - b.x - b.w / 2, dev.rect.y + dev.rect.h / 2 - b.y - b.h / 2);
                    if (!best || d < score) { best = i; score = d; }
                }
                if (best) {
                    if (const auto at = CheckpointRespawnPoint(world_, state_, *best)) {
                        const auto& dev = world_.devices[*best];
                        state_.checkpointId = dev.id; state_.checkpointPosition = *at;
                        for (size_t j = 0; j < world_.devices.size() && j < state_.devices.size(); ++j)
                            if (world_.devices[j].kind == DeviceKind::Checkpoint) state_.devices[j].active = j == *best;
                        mechanismEvents_.push_back({EventKind::CheckpointActivated, dev.id, "ride", {dev.rect.x, dev.rect.y}, state_.simulationTime});
                    }
                }
            }
        }
        for (const auto& origin : m.origins) isolated_.erase(origin.regionId);
        BuildBoundaries();
        snapshotDirty_ = true;
    }
}

bool Exploration::SetMechanismState(std::string_view id, int state) {
    for (auto& m : mechanisms_) {
        if (m.source.id != id) continue;
        if (!m.source.toggle && m.source.dwell <= 0) return false;
        if (state < 0 && !m.source.balance) return false;
        if (state != m.state && !MoveRegions(TargetsFor(m, state))) return false;
        m.state = m.target = m.pending = state; m.progress = state; m.moving = false; m.rest = 0;
        BuildBoundaries();
        snapshotDirty_ = true;
        return true;
    }
    return false;
}

std::vector<MechanismMove> Exploration::TargetsFor(const MechanismRuntime& m, int state) const {
    auto targets = m.origins;
    const auto& list = state > 0 ? m.source.moves : m.source.movesC;
    if (state != 0)
        for (auto& target : targets)
            for (const auto& move : list) if (move.regionId == target.regionId) target = move;
    return targets;
}

void Exploration::UpdateSlides(double seconds) {
    for (auto& s : slides_) {
        const auto& slots = s.source.slots;
        for (size_t i = 0; i < s.source.members.size(); ++i) {
            const bool powered = s.devices[i] < state_.devices.size() && state_.devices[s.devices[i]].active;
            if (powered && !s.powered[i] && s.moving < 0) {
                // 空いたスロットが上下左右の隣にあれば滑る。無ければ動かずに揺れの合図だけ出す。
                int gap = -1;
                for (int k = 0; k < static_cast<int>(slots.size()) && gap < 0; ++k)
                    if (std::find(s.slotOf.begin(), s.slotOf.end(), k) == s.slotOf.end()) gap = k;
                const auto& a = slots[static_cast<size_t>(s.slotOf[i])];
                const bool adjacent = gap >= 0 && std::abs(a.col - slots[static_cast<size_t>(gap)].col) + std::abs(a.row - slots[static_cast<size_t>(gap)].row) == 1;
                if (adjacent) {
                    s.moving = static_cast<int>(i); s.from = s.slotOf[i]; s.to = gap; s.progress = 0;
                    isolated_.insert(s.source.members[i]); BuildBoundaries();
                } else { s.blocked = static_cast<int>(i); ++s.blockedCount; }
            }
            s.powered[i] = powered;
        }
        if (s.moving < 0) continue;
        s.progress = std::min(1.0, s.progress + std::max(0.0, seconds) / std::max(.05, s.source.seconds));
        if (s.progress < 1.0) continue;
        const auto& member = s.source.members[static_cast<size_t>(s.moving)];
        const auto& slot = slots[static_cast<size_t>(s.to)];
        if (MoveRegions({{member, slot.worldX, slot.worldY, regions_.at(member).flipped}})) s.slotOf[static_cast<size_t>(s.moving)] = s.to;
        isolated_.erase(member); BuildBoundaries();
        s.moving = s.from = s.to = -1; s.progress = 0;
        snapshotDirty_ = true;
    }
}

// 前のフレームから新しく踏まれた板があるか。
bool Exploration::Rising(PlateEdges& edges) const {
    bool rising = false;
    for (size_t k = 0; k < edges.plates.size(); ++k) {
        const bool on = edges.plates[k] < state_.devices.size() && state_.devices[edges.plates[k]].pressed;
        if (on && !edges.was[k]) rising = true;
        edges.was[k] = on;
    }
    return rising;
}

void Exploration::UpdateStacks(double seconds) {
    for (auto& s : stacks_) {
        const bool pullEdge = Rising(s.pull), resetEdge = Rising(s.reset);
        const auto& members = s.source.members;
        const auto placeOf = [&](int place) -> const std::pair<int, int>& {
            return place >= 0 ? s.source.slots[static_cast<size_t>(place)] : s.source.docks[static_cast<size_t>(-1 - place)];
        };
        // 動く部屋だけを境界で切り離す（落ちる間・抜く間は中から出られず、外からも入れない）。
        const auto isolate = [&](bool on) {
            for (size_t m = 0; m < members.size(); ++m) {
                if (s.at[m] == s.to[m]) continue;
                if (on) isolated_.insert(members[m]); else isolated_.erase(members[m]);
            }
            BuildBoundaries();
        };
        const auto begin = [&](int phase) {
            if (s.to == s.at) { s.phase = 0; return; }
            s.phase = phase; s.progress = 0; isolate(true);
        };
        if (s.phase == 0) {
            if (resetEdge) {
                for (size_t m = 0; m < members.size(); ++m) s.to[m] = static_cast<int>(m);
                begin(3);
            } else if (pullEdge) {
                // 一番下のスロットのテレビを、空いている最初の置き場へ。抜けない時は揺れの合図だけ。
                int base = -1, dock = -1;
                for (size_t m = 0; m < members.size(); ++m) if (s.at[m] == 0) base = static_cast<int>(m);
                for (int k = 0; k < static_cast<int>(s.source.docks.size()) && dock < 0; ++k)
                    if (std::find(s.at.begin(), s.at.end(), -1 - k) == s.at.end()) dock = k;
                if (base < 0 || dock < 0) ++s.blockedCount;
                else { s.to = s.at; s.to[static_cast<size_t>(base)] = -1 - dock; begin(1); }
            }
        }
        if (s.phase == 0) continue;
        const double length = s.phase == 1 ? s.source.pullSeconds : s.phase == 2 ? s.source.dropSeconds : s.source.resetSeconds;
        s.progress = std::min(1.0, s.progress + std::max(0.0, seconds) / std::max(.05, length));
        if (s.progress < 1.0) continue;
        // 着いた：部屋を世界地図の新しい位置へまとめて移す（重なって移せない時は元のまま）。
        std::vector<MechanismMove> moves;
        for (size_t m = 0; m < members.size(); ++m) {
            if (s.at[m] == s.to[m]) continue;
            const auto& place = placeOf(s.to[m]);
            moves.push_back({members[m], place.first, place.second, regions_.at(members[m]).flipped});
        }
        isolate(false);
        if (MoveRegions(moves)) s.at = s.to; else s.to = s.at;
        // 移した後の配置で境界を作り直す（切り離しを解く時は移す前の配置で作るので、落ちた先の口が塞がったままになる）。
        BuildBoundaries();
        snapshotDirty_ = true;
        const int finished = s.phase;
        s.phase = 0; s.progress = 0;
        if (finished == 1) {
            // 抜いた後：上のテレビを、下から詰めて落とす。
            std::vector<std::pair<int, size_t>> stacked;
            for (size_t m = 0; m < members.size(); ++m) if (s.at[m] >= 0) stacked.push_back({s.at[m], m});
            std::sort(stacked.begin(), stacked.end());
            s.to = s.at;
            int slot = 0;
            for (const auto& entry : stacked) s.to[entry.second] = slot++;
            begin(2);
        }
    }
}

void Exploration::UpdateCarousels(double seconds) {
    for (auto& c : carousels_) {
        const bool cwEdge = Rising(c.cw), ccwEdge = Rising(c.ccw);
        const auto& faces = c.source.faces;
        if (c.direction == 0 && (cwEdge || ccwEdge)) {
            c.direction = cwEdge ? 1 : -1; c.progress = 0;
            for (const auto& face : faces) isolated_.insert(face);
            BuildBoundaries();
        }
        if (c.direction == 0) continue;
        c.progress = std::min(1.0, c.progress + std::max(0.0, seconds) / std::max(.05, c.source.seconds));
        if (c.progress < 1.0) continue;
        // 回り終えた：全部の面を次の立ち位置へ一度に移す（人と残影は乗っている面と一緒に）。
        const int count = static_cast<int>(faces.size());
        const int next = ((c.step + c.direction) % count + count) % count;
        std::vector<MechanismMove> moves;
        for (int i = 0; i < count; ++i) {
            const auto& station = c.source.stations[static_cast<size_t>((i + next) % count)];
            moves.push_back({faces[static_cast<size_t>(i)], station.first, station.second, regions_.at(faces[static_cast<size_t>(i)]).flipped});
        }
        for (const auto& face : faces) isolated_.erase(face);
        if (MoveRegions(moves)) c.step = next;
        c.direction = 0; c.progress = 0;
        BuildBoundaries();
        snapshotDirty_ = true;
    }
}

bool Exploration::MoveRegions(const std::vector<MechanismMove>& targets) {
    // 新しい配置が重ならないか先に確かめる。重なるなら動かさない。
    auto next = placements_;
    for (const auto& move : targets)
        for (auto& p : next) if (p.regionId == move.regionId) { p.worldX = move.worldX; p.worldY = move.worldY; }
    if (const auto validation = ValidateWorldLayout(next); !validation.valid) return false;
    // 体や記録がどの部屋にいるかは、何も運ぶ前に一度だけ決める。先に運んだ体が、後で動く部屋の元の場所に
    // 入ってもう一度運ばれないように（入れ替わる二部屋で、乗っていた部屋ごと隣へ跳んでいた）。
    const auto ownerOf = [this](const Rect& r) { return RegionAt(r); };
    const std::string playerIn = ownerOf(state_.player.rect);
    // 頭に載せた残影は本体と同じ部屋として運ぶ（頭だけ隣のテレビに掛かっていても置いて行かない）。
    const std::string echoIn = !state_.echo ? std::string{} : state_.echo->supportId == "player" ? playerIn : ownerOf(state_.echo->rect);
    const std::string pendingIn = state_.pendingEcho ? ownerOf(state_.pendingEcho->origin) : std::string{};
    const std::string dashIn = state_.dash ? ownerOf(state_.dash->contact) : std::string{};
    const std::string bounceIn = state_.bounce ? ownerOf(state_.bounce->contact) : std::string{};
    // 本体のいるテレビが回る時、別のテレビに残した残影は消す（回った舞台の外へ交換で戻ると、閉じた側から入れずに詰む）。
    const bool turningHere = std::any_of(targets.begin(), targets.end(), [&](const MechanismMove& m) {
        return m.regionId == playerIn && m.flip != regions_.at(m.regionId).flipped;
    });
    // 頭の残影は回すと点対称で足の下へ移る。隣の部屋へ掛かっていて部屋からはみ出す時も消す。
    const auto within = [this](const Rect& r, const std::string& region) {
        const auto it = regions_.find(region);
        if (it == regions_.end()) return false;
        const auto& p = it->second.placement;
        return r.x >= p.worldX && r.y >= p.worldY && r.x + r.w <= p.worldX + p.cols && r.y + r.h <= p.worldY + p.rows;
    };
    const bool headOutside = state_.echo && state_.echo->supportId == "player" && !within(state_.echo->rect, playerIn);
    if (turningHere && ((state_.echo && (echoIn != playerIn || headOutside)) || (state_.pendingEcho && pendingIn != playerIn))) {
        (void)RemoveWorldEcho(world_, state_); snapshotDirty_ = true;
    }
    const std::string checkpointIn = ownerOf({state_.checkpointPosition.x, state_.checkpointPosition.y, kPlayerWidth, kPlayerHeight});
    const std::string spawnIn = ownerOf({world_.spawn.x, world_.spawn.y, kPlayerWidth, kPlayerHeight});
    for (const auto& move : targets) {
        auto& runtime = regions_.at(move.regionId);
        const auto old = runtime.placement;
        const bool flip = move.flip != runtime.flipped;
        if (move.worldX == old.worldX && move.worldY == old.worldY && !flip) continue;
        // 部屋の中の体・装置・記録を、部屋と一緒に運ぶ。
        // 回す時は部屋の中心について点対称に移し、勢いは捨てる（新しい床へ落ちる）。
        const auto inside = [&](const std::string& owner) { return owner == move.regionId; };
        const auto map = [&](Rect& r) {
            double x = r.x - old.worldX, y = r.y - old.worldY;
            if (flip) { x = old.cols - x - r.w; y = old.rows - y - r.h; }
            r.x = move.worldX + x; r.y = move.worldY + y;
        };
        const auto carryBody = [&](BodyState& b) {
            map(b.rect);
            if (!flip) return;
            b.vx = b.vy = b.walkVx = b.reboundTime = 0; b.reboundDirection = 0; b.facing = -b.facing;
            b.grounded = false; b.supportId.clear();
        };
        if (inside(playerIn)) carryBody(state_.player);
        if (state_.echo && inside(echoIn)) carryBody(*state_.echo);
        if (state_.pendingEcho && inside(pendingIn)) map(state_.pendingEcho->origin);
        if (state_.dash && inside(dashIn)) {
            if (flip) state_.dash.reset(); else { map(state_.dash->source); map(state_.dash->contact); }
        }
        if (state_.bounce && inside(bounceIn)) { if (flip) state_.bounce.reset(); else map(state_.bounce->contact); }
        if (inside(checkpointIn)) {
            Rect point{state_.checkpointPosition.x, state_.checkpointPosition.y, kPlayerWidth, kPlayerHeight};
            map(point); state_.checkpointPosition = {point.x, point.y};
        }
        if (inside(spawnIn)) { Rect point{world_.spawn.x, world_.spawn.y, kPlayerWidth, kPlayerHeight}; map(point); world_.spawn = {point.x, point.y}; }
        // 装置は部屋と一緒に運ぶ。回す時は舞台ごと回る：点対称に移し、向きを裏返す（重力を受けるのは体だけ）。
        for (auto& device : world_.devices) {
            const auto owner = deviceOwners_.find(device.id);
            if (owner == deviceOwners_.end() || owner->second != move.regionId) continue;
            map(device.rect);
            if (flip) device.direction = Opposite(device.direction);
        }
        for (auto& chunk : world_.chunks) {
            if (chunk.region.regionId != move.regionId) continue;
            chunk.x = move.worldX; chunk.y = move.worldY;
            // 行ごとに並んだマスは、逆順にすると 180° 回したものになる。
            if (flip) std::reverse(chunk.region.cells.begin(), chunk.region.cells.end());
        }
        runtime.placement.worldX = move.worldX; runtime.placement.worldY = move.worldY; runtime.flipped = move.flip;
    }
    placements_ = std::move(next);
    return true;
}
}
