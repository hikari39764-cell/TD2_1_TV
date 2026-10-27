#include "TD2Exploration.h"
#include "../../Common/TD2MachineText.h"
#include <algorithm>
#include <cmath>
#include <utility>

namespace TD2::Rules {
namespace {
// つながない口に置く薄い壁の厚さ（マス）。境の両側へ半分ずつはみ出す。
constexpr double kThinWall = .02;
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
                spawn.x + kPlayerWidth > p.cols || spawn.y + kPlayerHeight > p.rows) { return "test spawn outside region: " + p.regionId; }
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
            const bool machine = IsMachineTarget(target);
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
                const bool sealed = sealed_.contains(std::minmax(p.regionId, other.regionId));
                if (sealed || isolated_.contains(p.regionId) || isolated_.contains(other.regionId)) thin.emplace_back(shared->overlapBegin, shared->overlapEnd);
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
            constexpr double kThin = kThinWall;
            for (const auto& [begin, end] : thin) {
                const double edge = static_cast<double>(side == WorldSide::Left ? bounds.left : side == WorldSide::Right ? bounds.right
                                                        : side == WorldSide::Top ? bounds.top : bounds.bottom);
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
        if (IsMachineTarget(event.subjectId)) continue;   // 仕掛けの受け口は画面に出ない
        const auto owner = deviceOwners_.find(event.subjectId);
        std::string region = owner != deviceOwners_.end() ? owner->second : RegionAt({event.position.x, event.position.y, kPlayerWidth, kPlayerHeight});
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
    calls_.push_back({"clear", {}, 0});
    if (!state_.echo && !state_.pendingEcho) return;
    (void)RemoveWorldEcho(world_, state_);
    snapshotDirty_ = true;
}

Exploration::CollectibleCount Exploration::Collectibles() const {
    CollectibleCount count;
    for (size_t i = 0; i < world_.devices.size() && i < state_.devices.size(); ++i) {
        if (world_.devices[i].kind != DeviceKind::Collectible) continue;
        ++count.total;
        if (state_.devices[i].active) ++count.taken;
    }
    return count;
}

bool Exploration::Teleport(std::string_view region, const Vec2* at) {
    const auto* room = FindRegion(region);
    if (!room || !room->placement.worldPlaced) return false;
    ClearEcho();
    const auto& p = room->placement;
    auto& body = state_.player;
    const Vec2 local = at ? *at : room->definition.spawn;
    body.rect.x = local.x + static_cast<double>(p.worldX); body.rect.y = local.y + static_cast<double>(p.worldY);
    body.vx = body.vy = body.walkVx = body.reboundTime = 0; body.reboundDirection = 0;
    body.grounded = false; body.dead = false; body.supportId.clear();
    state_.dash.reset(); state_.bounce.reset(); state_.deathRemaining = 0;
    state_.checkpointId.clear(); state_.checkpointPosition = {body.rect.x, body.rect.y};
    activeRegion_ = std::string(region); snapshotDirty_ = true;
    return true;
}

bool Exploration::StepBack(std::string_view region, int direction) {
    calls_.push_back({"back", std::string(region), direction});
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
    // ただし自分のいる部屋が回っている間は、歩き・冲刺・交換を受け付けない（回り終わった時、止まった所から点対称に運ばれる）。
    const auto here = RegionAt(state_.player.rect);
    const bool turning = !here.empty() && std::any_of(mechanisms_.begin(), mechanisms_.end(), [&](const MechanismRuntime& m) {
        if (!m.moving) return false;
        const auto targets = TargetsFor(m, m.target);
        return std::any_of(targets.begin(), targets.end(), [&](const MechanismMove& move) {
            return move.regionId == here && move.flip != regions_.at(here).flipped;
        });
    });
    if (turning) { state_.actionArmed = false; state_.blinkBuffer = 0; state_.pendingActionPressed = state_.pendingActionReleased = false; }
    auto result = StepRules(world_, state_, turning ? InputState{} : input, seconds);
    UpdateMechanisms(seconds);
    for (auto& event : mechanismEvents_) result.events.push_back(std::move(event));
    mechanismEvents_.clear();
    UpdateSlides(seconds);
    UpdateStacks(seconds);
    UpdateCarousels(seconds);
    const bool swapped = std::any_of(result.events.begin(), result.events.end(), [](const RuleEvent& e) { return e.kind == EventKind::EchoSwapped; });
    const bool respawn = std::any_of(result.events.begin(), result.events.end(), [](const RuleEvent& e) { return e.kind == EventKind::PlayerRespawned; });
    const auto current = RegionAt(state_.player.rect);
    if (!current.empty() && current != activeRegion_) {
        out.transitions.push_back({respawn ? "respawn" : swapped ? "swap" : "crossed", activeRegion_, current, {state_.player.rect.x, state_.player.rect.y}});
        const std::string previous = activeRegion_;
        activeRegion_ = current;
        // 乗り物：外から動く部屋へ入ったら、着地して収まるのを待って動き出す（復活でそこに出た時は動かさない）。
        if (!respawn) {
            // 行き先で降りる口（sealExits のうち、着いた時に隣り合う部屋）が全部閉じていれば乗せない。
            // 一度降りて閉じた側へもう一度運ぶと、出口の無い部屋に閉じ込めてしまう（s4 に s3c から入り直した時）。
            const auto deadEnd = [this](const MechanismRuntime& m, const std::string& room) {
                const int goal = m.source.rideFrom.empty() ? (m.target ? 0 : 1) : 1;
                if (goal == m.state) return false;
                const auto targets = TargetsFor(m, goal);
                const auto at = [&](const std::string& id, RegionPlacement p) {
                    for (const auto& t : targets) if (t.regionId == id) { p.worldX = t.worldX; p.worldY = t.worldY; }
                    return p;
                };
                const auto* ride = FindRegion(room);
                if (!ride) return false;
                const auto r = at(room, ride->placement);
                bool touching = false;
                for (const auto& exit : m.source.sealExits) {
                    const auto* e = FindRegion(exit);
                    if (!e || !e->placement.worldPlaced) continue;
                    const auto p = at(exit, e->placement);
                    const int ox = std::min(r.worldX + r.cols, p.worldX + p.cols) - std::max(r.worldX, p.worldX);
                    const int oy = std::min(r.worldY + r.rows, p.worldY + p.rows) - std::max(r.worldY, p.worldY);
                    if (!((ox == 0 && oy > 0) || (oy == 0 && ox > 0))) continue;   // 辺を共有する部屋だけ
                    touching = true;
                    if (!sealed_.contains(std::minmax(room, exit))) return false;
                }
                return touching;
            };
            for (auto& m : mechanisms_) {
                if (!m.source.ride) continue;
                const auto inOrigins = [&m](const std::string& region) {
                    return std::any_of(m.origins.begin(), m.origins.end(), [&](const MechanismMove& o) { return o.regionId == region; });
                };
                // 運ばれた後に決めた口へ降りたら、その境を閉じる。乗り物の部屋へ入り直したら、運んだ印は消す。
                if (m.carried && !m.moving && inOrigins(previous) &&
                    std::find(m.source.sealExits.begin(), m.source.sealExits.end(), current) != m.source.sealExits.end()) {
                    pendingSeals_.push_back({previous, current});
                }
                // 降りかけて（体がまだ境をまたいだまま）戻った時は印を残す。そうしないと次に降りても閉じない。
                if (inOrigins(current) && std::find(m.source.sealExits.begin(), m.source.sealExits.end(), previous) == m.source.sealExits.end()) m.carried = false;
                if (m.moving) continue;
                // 乗り込む前に降りた（ほかの部屋へ出た）時は待ちをやめる。決めた部屋から入って来た時だけ待つ。
                const bool came = m.source.rideEnter.empty() ||
                    std::find(m.source.rideEnter.begin(), m.source.rideEnter.end(), previous) != m.source.rideEnter.end();
                m.rideArmed = came && (m.source.rideFrom.empty()
                    ? std::any_of(m.origins.begin(), m.origins.end(), [&](const MechanismMove& o) { return o.regionId == current; })
                    : std::find(m.source.rideFrom.begin(), m.source.rideFrom.end(), current) != m.source.rideFrom.end());
                if (m.rideArmed && deadEnd(m, current)) m.rideArmed = false;
            }
        } else {
            // 復活は乗り込みではない。乗り込んで着地する前に死んだ時の待ちも捨てる（復活先の部屋で動き出さないように）。
            for (auto& m : mechanisms_) m.rideArmed = false;
            // 乗り物の部屋で復活した（降りた先で死んだ）時は、その部屋の閉じた口を開け直す（閉じ込めない）。
            bool reopened = false;
            for (const auto& m : mechanisms_) {
                if (!m.source.ride || std::none_of(m.origins.begin(), m.origins.end(), [&](const MechanismMove& o) { return o.regionId == current; })) continue;
                for (const auto& exit : m.source.sealExits) reopened = sealed_.erase(std::minmax(current, exit)) > 0 || reopened;
            }
            if (reopened) BuildBoundaries();
        }
    }
    // 降りかけの口：体が乗り物の部屋から出きったら閉じる。乗り物の部屋へ戻った・別の部屋へ移った時はやめる。
    if (!pendingSeals_.empty()) {
        bool sealedNow = false;
        std::erase_if(pendingSeals_, [&](const std::pair<std::string, std::string>& seal) {
            if (activeRegion_ != seal.second) return true;
            const auto* ride = FindRegion(seal.first);
            if (!ride) return true;
            const auto& p = ride->placement; const auto& b = state_.player.rect;
            // 閉じる壁は境から出口側へ半分はみ出す。体がその壁に掛かっている間は閉じない（壁の中から逆へ歩けてしまう）。
            const double m = kThinWall * .5 + 1e-6;
            const bool inside = b.x < p.worldX + p.cols + m && b.x + b.w > p.worldX - m && b.y < p.worldY + p.rows + m && b.y + b.h > p.worldY - m;
            if (inside) return false;
            if (sealed_.insert(std::minmax(seal.first, seal.second)).second) {
                sealedNow = true;
                // 閉じた乗り物の部屋に残影を残さない（交換で戻ると、閉じた口から出られずに詰む）。
                const bool echoInside = state_.echo && state_.echo->supportId != "player" && RegionAt(state_.echo->rect) == seal.first;
                const bool pendingInside = state_.pendingEcho && RegionAt(state_.pendingEcho->origin) == seal.first;
                if (echoInside || pendingInside) (void)RemoveWorldEcho(world_, state_);
            }
            return true;
        });
        if (sealedNow) BuildBoundaries();
    }
    Record(std::move(result), out); snapshotDirty_ = true;
    return out;
}

}
