#include "TD2Exploration.h"
#include <algorithm>
#include <cmath>
#include <utility>

namespace TD2::Rules {
namespace {
std::string DeviceId(std::string_view region, std::string_view local) {
    // region ID は '/' を含まないので、別のテレビの同名の装置と混ざらない。
    return std::string(region) + "/" + std::string(local);
}
}

std::string Exploration::Initialize(std::vector<RegionSource> sources, std::vector<MechanismSource> mechanisms,
                                    std::vector<SlideSource> slides, std::vector<StackSource> stacks, std::vector<CarouselSource> carousels,
                                    std::string_view entryOverride, std::optional<Vec2> spawnOverride) {
    if (entryOverride.empty() && spawnOverride) { return "test spawn requires a region"; }
    Exploration next;
    for (const auto& source : sources) next.placements_.push_back(source.placement);
    if (const auto validation = ValidateWorldLayout(next.placements_); !validation.valid)
        return validation.message + ": " + validation.regionA + " / " + validation.regionB;
    size_t entries = 0;
    for (auto& source : sources) {
        const auto& p = source.placement;
        if (!p.worldPlaced) continue;
        const auto& authored = source.definition;
        if (authored.region.regionId != p.regionId || authored.region.cols != p.cols || authored.region.rows != p.rows)
            return "room definition and placement differ: " + p.regionId;
        if (const auto error = ValidateWorld(authored); !error.empty()) return p.regionId + ": " + error;
        if (entryOverride.empty() ? source.entry : p.regionId == entryOverride) {
            ++entries;
            const auto spawn = spawnOverride.value_or(authored.spawn);
            // 空中の位置も受け付ける。区域の外へは丸めない。
            if (!std::isfinite(spawn.x) || !std::isfinite(spawn.y) || spawn.x < 0 || spawn.y < 0 ||
                spawn.x + .625 > p.cols || spawn.y + .875 > p.rows) { return "test spawn outside region: " + p.regionId; }
            next.world_.spawn = {spawn.x + p.worldX, spawn.y + p.worldY};
        }
        RegionRuntime runtime;
        runtime.definition = std::move(source.definition); runtime.placement = p;
        next.world_.chunks.push_back({runtime.definition.region, p.worldX, p.worldY});
        for (auto device : runtime.definition.devices) {
            device.id = DeviceId(p.regionId, device.id);
            device.rect.x += p.worldX; device.rect.y += p.worldY;
            // 「区域ID/装置ID」は別のテレビの装置を指す（テレビをまたぐ圧力板）。
            for (auto& target : device.targets)
                if (target.find('/') == std::string::npos) target = DeviceId(p.regionId, target);
            next.deviceOwners_[device.id] = p.regionId;
            next.world_.devices.push_back(std::move(device));
        }
        next.regions_.emplace(p.regionId, std::move(runtime));
    }
    if (entries != 1) return entryOverride.empty()
        ? "world requires exactly one placed entryRegion; found " + std::to_string(entries)
        : "test start region is missing or not placed: " + std::string(entryOverride);
    next.world_.region.regionId = "td2.world";
    // 機構は「mech/機構ID」という当たりの無い受け口を世界に置き、圧力板の targets から指せるようにする。
    const auto receiver = [&next](std::string id) {
        DeviceDefinition device;
        device.id = std::move(id); device.kind = DeviceKind::Mechanism; device.rect = {-100000, -100000, .01, .01};
        next.world_.devices.push_back(std::move(device));
        return next.world_.devices.size() - 1;
    };
    // 受け口を指す圧力板を集める（板ごとに踏み込みを見る受け口用）。
    const auto plates = [&next, &receiver](std::string id) {
        PlateEdges edges;
        for (size_t i = 0; i < next.world_.devices.size(); ++i) {
            const auto& d = next.world_.devices[i];
            if (d.kind == DeviceKind::Plate && std::find(d.targets.begin(), d.targets.end(), id) != d.targets.end()) edges.plates.push_back(i);
        }
        edges.was.assign(edges.plates.size(), 0);
        (void)receiver(std::move(id));
        return edges;
    };
    // 仕掛けの不備はその仕掛けだけを止める（受け口は残し、圧力板の目標が宙に浮かないようにする）。
    const auto disable = [&next](std::string problem) { next.problems_.push_back(std::move(problem)); };
    for (auto& source : mechanisms) {
        MechanismRuntime runtime;
        std::string problem;
        for (const auto* list : {&source.moves, &source.movesC}) for (const auto& move : *list) {
            const auto found = next.regions_.find(move.regionId);
            if (found == next.regions_.end()) { problem = "mechanism " + source.id + " moves an unplaced region: " + move.regionId; break; }
            if (std::none_of(runtime.origins.begin(), runtime.origins.end(), [&](const auto& o) { return o.regionId == move.regionId; }))
                runtime.origins.push_back({move.regionId, found->second.placement.worldX, found->second.placement.worldY, false});
        }
        runtime.device = receiver("mech/" + source.id);
        if (source.balance) runtime.deviceR = receiver("mech/" + source.id + "/r");
        if (!problem.empty()) { disable(problem); continue; }
        runtime.source = std::move(source);
        next.mechanisms_.push_back(std::move(runtime));
    }
    // スロット棚：メンバーごとに「slide/区域ID」の受け口を置く。今の配置と同じ位置のスロットを、そのメンバーのスロットとする。
    for (auto& source : slides) {
        SlideRuntime runtime;
        std::string problem;
        for (const auto& member : source.members) {
            runtime.devices.push_back(receiver("slide/" + member));
            runtime.powered.push_back(0);
            const auto found = next.regions_.find(member);
            if (found == next.regions_.end()) { problem = "slide " + source.id + " has an unplaced region: " + member; continue; }
            const auto& p = found->second.placement;
            const auto slot = std::find_if(source.slots.begin(), source.slots.end(), [&](const SlideSlot& s) { return s.worldX == p.worldX && s.worldY == p.worldY; });
            if (slot == source.slots.end()) { problem = "slide " + source.id + ": region is not on a slot: " + member; continue; }
            runtime.slotOf.push_back(static_cast<int>(slot - source.slots.begin()));
        }
        if (problem.empty() && runtime.slotOf.size() >= source.slots.size()) problem = "slide " + source.id + " needs an empty slot";
        if (!problem.empty()) { disable(problem); continue; }
        runtime.source = std::move(source);
        next.slides_.push_back(std::move(runtime));
    }
    // 重ねたテレビ：メンバーは下のスロットから順に置いておく。「stack/ID/pull」で一番下を抜き、「stack/ID/reset」で元へ戻す。
    for (auto& source : stacks) {
        StackRuntime runtime;
        runtime.pull = plates("stack/" + source.id + "/pull");
        runtime.reset = plates("stack/" + source.id + "/reset");
        std::string problem;
        if (source.members.size() > source.slots.size()) problem = "stack " + source.id + " has more rooms than slots";
        if (source.docks.empty()) problem = "stack " + source.id + " has no dock";
        for (size_t m = 0; m < source.members.size() && problem.empty(); ++m) {
            const auto found = next.regions_.find(source.members[m]);
            if (found == next.regions_.end()) { problem = "stack " + source.id + " has an unplaced region: " + source.members[m]; break; }
            const auto& p = found->second.placement;
            if (p.worldX != source.slots[m].first || p.worldY != source.slots[m].second) {
                problem = "stack " + source.id + ": region is not on its slot: " + source.members[m]; break;
            }
            runtime.at.push_back(static_cast<int>(m));
        }
        if (!problem.empty()) { disable(problem); continue; }
        runtime.to = runtime.at;
        runtime.source = std::move(source);
        next.stacks_.push_back(std::move(runtime));
    }
    // 回転柱：面は立ち位置 0 から順に置いておく。「carousel/ID/cw」「carousel/ID/ccw」で一つ回す。
    for (auto& source : carousels) {
        CarouselRuntime runtime;
        runtime.cw = plates("carousel/" + source.id + "/cw");
        runtime.ccw = plates("carousel/" + source.id + "/ccw");
        std::string problem;
        if (source.faces.size() != source.stations.size() || source.faces.size() < 2) problem = "carousel " + source.id + " needs one face per station";
        for (size_t i = 0; i < source.faces.size() && problem.empty(); ++i) {
            const auto found = next.regions_.find(source.faces[i]);
            if (found == next.regions_.end()) { problem = "carousel " + source.id + " has an unplaced region: " + source.faces[i]; break; }
            const auto& p = found->second.placement;
            if (p.worldX != source.stations[i].first || p.worldY != source.stations[i].second) {
                problem = "carousel " + source.id + ": face is not on its station: " + source.faces[i]; break;
            }
        }
        if (!problem.empty()) { disable(problem); continue; }
        runtime.source = std::move(source);
        next.carousels_.push_back(std::move(runtime));
    }
    // 圧力板の目標が無い時：仕掛けの受け口なら空の受け口を置き、別のテレビの装置なら目標から外す（どちらも知らせる）。
    for (size_t i = 0; i < next.world_.devices.size(); ++i) {
        if (next.world_.devices[i].kind != DeviceKind::Plate) continue;
        auto targets = next.world_.devices[i].targets;
        std::vector<std::string> kept;
        for (const auto& target : targets) {
            const bool exists = std::any_of(next.world_.devices.begin(), next.world_.devices.end(), [&](const DeviceDefinition& d) { return d.id == target; });
            if (exists) { kept.push_back(target); continue; }
            const bool machine = target.starts_with("mech/") || target.starts_with("slide/") || target.starts_with("stack/") || target.starts_with("carousel/");
            disable("plate " + next.world_.devices[i].id + " targets missing " + (machine ? "machine " : "device ") + target);
            if (machine) { (void)receiver(target); kept.push_back(target); }
        }
        next.world_.devices[i].targets = std::move(kept);
    }
    next.BuildBoundaries();
    if (const auto error = ValidateWorld(next.world_); !error.empty()) return error;
    next.state_ = CreateWorldState(next.world_);
    next.activeRegion_ = next.RegionAt(next.state_.player.rect);
    *this = std::move(next);
    return {};
}

void Exploration::BuildBoundaries() {
    world_.boundaries.clear();
    for (const auto& p : placements_) {
        if (!p.worldPlaced) continue;
        const auto bounds = WorldBounds(p);
        for (const auto side : {WorldSide::Left, WorldSide::Right, WorldSide::Top, WorldSide::Bottom}) {
            const bool vertical = side == WorldSide::Left || side == WorldSide::Right;
            std::vector<std::pair<int64_t, int64_t>> exposed{{vertical ? bounds.top : bounds.left, vertical ? bounds.bottom : bounds.right}};
            // 動いている部屋との共有辺は、境の線の上の薄い壁で閉じる（1 マスの壁だと隣の部屋の端の列を潰してしまう）。
            std::vector<std::pair<int64_t, int64_t>> thin;
            for (const auto& other : placements_) {
                const auto shared = FindSharedWorldEdge(p, other);
                if (!shared || shared->sideA != side) continue;
                if (isolated_.contains(p.regionId) || isolated_.contains(other.regionId)) thin.emplace_back(shared->overlapBegin, shared->overlapEnd);
                std::vector<std::pair<int64_t, int64_t>> remainder;
                for (const auto& [begin, end] : exposed) {
                    if (shared->overlapBegin >= end || shared->overlapEnd <= begin) remainder.emplace_back(begin, end);
                    else {
                        if (begin < shared->overlapBegin) remainder.emplace_back(begin, shared->overlapBegin);
                        if (shared->overlapEnd < end) remainder.emplace_back(shared->overlapEnd, end);
                    }
                }
                exposed = std::move(remainder);
            }
            // 大部屋の一辺に複数の小部屋が接しても、実際に重なる区間だけを開ける。
            for (const auto& [begin, end] : exposed) {
                if (vertical) world_.boundaries.push_back({double(side == WorldSide::Left ? bounds.left - 1 : bounds.right), double(begin), 1, double(end - begin)});
                else world_.boundaries.push_back({double(begin), double(side == WorldSide::Top ? bounds.top - 1 : bounds.bottom), double(end - begin), 1});
            }
            constexpr double kThin = .02;
            for (const auto& [begin, end] : thin) {
                const double edge = side == WorldSide::Left ? bounds.left : side == WorldSide::Right ? bounds.right
                                  : side == WorldSide::Top ? bounds.top : bounds.bottom;
                if (vertical) world_.boundaries.push_back({edge - kThin * .5, double(begin), kThin, double(end - begin)});
                else world_.boundaries.push_back({double(begin), edge - kThin * .5, double(end - begin), kThin});
            }
        }
    }
}

const RegionRuntime* Exploration::FindRegion(std::string_view id) const {
    const auto found = regions_.find(std::string(id));
    return found == regions_.end() ? nullptr : &found->second;
}
std::string Exploration::RegionAt(const Rect& body) const {
    const auto* region = FindRegionAtWorldPoint(placements_, {body.x + body.w / 2, body.y + body.h / 2});
    return region ? region->regionId : std::string{};
}

void Exploration::Record(StepOutput output, ExplorationOutput& out) {
    std::map<std::string, StepOutput> grouped;
    for (auto& event : output.events) {
        if (event.subjectId.starts_with("mech/") || event.subjectId.starts_with("slide/")) continue;   // 機構の受け口は画面に出ない
        const auto owner = deviceOwners_.find(event.subjectId);
        std::string region = owner != deviceOwners_.end() ? owner->second : RegionAt({event.position.x, event.position.y, .625, .875});
        if (region.empty()) region = activeRegion_;
        const auto& p = regions_.at(region).placement;
        event.position.x -= p.worldX; event.position.y -= p.worldY;
        if (owner != deviceOwners_.end()) event.subjectId.erase(0, region.size() + 1);
        auto& result = grouped[region];
        result.tick = output.tick; result.simulationTime = output.simulationTime; result.realTime = output.realTime;
        result.events.push_back(std::move(event));
    }
    for (auto& [region, events] : grouped) out.regions.push_back({region, std::move(events)});
}

void Exploration::ClearEcho() {
    if (!state_.echo && !state_.pendingEcho) return;
    (void)RemoveWorldEcho(world_, state_);
    snapshotDirty_ = true;
}

bool Exploration::StepBack(std::string_view region, int direction) {
    const auto* from = FindRegion(region);
    if (!from || direction == 0) return false;
    auto& body = state_.player;
    const auto& p = from->placement;
    const double edge = direction > 0 ? p.worldX + p.cols : p.worldX;
    Rect back = body.rect;
    back.x = edge - direction * .55 - back.w * .5;
    if (RegionAt(back) != region) return false;
    body.rect = back; body.vx = body.walkVx = 0; body.facing = -direction;
    activeRegion_ = std::string(region); snapshotDirty_ = true;
    return true;
}

const WorldSnapshot& Exploration::Snapshot() {
    if (snapshotDirty_) { snapshot_ = BuildSnapshot(world_, state_); snapshotDirty_ = false; }
    return snapshot_;
}

ExplorationOutput Exploration::Step(const InputState& input, double seconds) {
    ExplorationOutput out;
    if (!Ready()) return out;
    // 部屋の切り替え（カメラの演出）中も操作は効く。
    auto result = StepRules(world_, state_, input, seconds);
    UpdateMechanisms(seconds);
    UpdateSlides(seconds);
    UpdateStacks(seconds);
    UpdateCarousels(seconds);
    const bool swapped = std::any_of(result.events.begin(), result.events.end(), [](const RuleEvent& e) { return e.kind == EventKind::EchoSwapped; });
    const bool respawn = std::any_of(result.events.begin(), result.events.end(), [](const RuleEvent& e) { return e.kind == EventKind::PlayerRespawned; });
    const auto current = RegionAt(state_.player.rect);
    if (!current.empty() && current != activeRegion_) {
        out.transitions.push_back({respawn ? "respawn" : swapped ? "swap" : "crossed", activeRegion_, current, {state_.player.rect.x, state_.player.rect.y}});
        activeRegion_ = current;
    }
    Record(std::move(result), out); snapshotDirty_ = true;
    return out;
}

void Exploration::UpdateMechanisms(double seconds) {
    for (auto& m : mechanisms_) {
        const auto active = [this](size_t index) { return index < state_.devices.size() && state_.devices[index].active; };
        const bool powered = active(m.device);
        if (m.source.dwell > 0) {
            // 自動往復：端で dwell 秒止まったら反対の端へ向かう（圧力板は見ない）。
            if (!m.moving && std::abs(m.progress - m.target) < 1e-9 && (m.rest += seconds) >= m.source.dwell) {
                m.target = m.target ? 0 : 1; m.rest = 0;
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
        for (const auto& origin : m.origins) isolated_.erase(origin.regionId);
        BuildBoundaries();
        snapshotDirty_ = true;
    }
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
    const std::string playerIn = state_.playerPresent ? ownerOf(state_.player.rect) : std::string{};
    const std::string echoIn = state_.echo ? ownerOf(state_.echo->rect) : std::string{};
    const std::string pendingIn = state_.pendingEcho ? ownerOf(state_.pendingEcho->origin) : std::string{};
    const std::string dashIn = state_.dash ? ownerOf(state_.dash->contact) : std::string{};
    const std::string bounceIn = state_.bounce ? ownerOf(state_.bounce->contact) : std::string{};
    const std::string checkpointIn = ownerOf({state_.checkpointPosition.x, state_.checkpointPosition.y, .625, .875});
    const std::string spawnIn = ownerOf({world_.spawn.x, world_.spawn.y, .625, .875});
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
        if (state_.playerPresent && inside(playerIn)) carryBody(state_.player);
        if (state_.echo && inside(echoIn)) carryBody(*state_.echo);
        if (state_.pendingEcho && inside(pendingIn)) map(state_.pendingEcho->origin);
        if (state_.dash && inside(dashIn)) {
            if (flip) state_.dash.reset(); else { map(state_.dash->source); map(state_.dash->contact); }
        }
        if (state_.bounce && inside(bounceIn)) { if (flip) state_.bounce.reset(); else map(state_.bounce->contact); }
        if (inside(checkpointIn)) {
            Rect point{state_.checkpointPosition.x, state_.checkpointPosition.y, .625, .875};
            map(point); state_.checkpointPosition = {point.x, point.y};
        }
        if (inside(spawnIn)) { Rect point{world_.spawn.x, world_.spawn.y, .625, .875}; map(point); world_.spawn = {point.x, point.y}; }
        for (auto& device : world_.devices) {
            const auto owner = deviceOwners_.find(device.id);
            if (owner == deviceOwners_.end() || owner->second != move.regionId) continue;
            map(device.rect);
            if (flip) {
                device.direction = device.direction == Direction::Up ? Direction::Down : device.direction == Direction::Down ? Direction::Up
                                 : device.direction == Direction::Left ? Direction::Right : Direction::Left;
            }
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
