#include "TD2Exploration.h"
#include <algorithm>
#include <cmath>
#include <utility>

namespace TD2::Rules {
namespace {
std::string DeviceId(std::string_view region, std::string_view local) {
    // region ID は '/' を含まない契約なので、同名の局所装置が別 TV と混ざらない。
    return std::string(region) + "/" + std::string(local);
}
} // namespace

std::string Exploration::Initialize(std::vector<RegionSource> sources, std::string_view entryOverride,
                                    std::optional<Vec2> spawnOverride) {
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
            // 任意の空中位置も許すが、指定区域の外へ黙って丸めたり別区域へ落としたりしない。
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
            // 「区域ID/装置ID」と書いた接続は別の TV の装置をそのまま指す（地図块を跨ぐ圧板）。
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
    next.BuildBoundaries();
    if (const auto error = ValidateWorld(next.world_); !error.empty()) return error;
    next.state_ = CreateWorldState(next.world_);
    next.activeRegion_ = next.RegionAt(next.state_.player.rect);
    *this = std::move(next);
    return {};
}

void Exploration::BuildBoundaries() {
    for (const auto& p : placements_) {
        if (!p.worldPlaced) continue;
        const auto bounds = WorldBounds(p);
        for (const auto side : {WorldSide::Left, WorldSide::Right, WorldSide::Top, WorldSide::Bottom}) {
            const bool vertical = side == WorldSide::Left || side == WorldSide::Right;
            std::vector<std::pair<int64_t, int64_t>> exposed{{vertical ? bounds.top : bounds.left, vertical ? bounds.bottom : bounds.right}};
            for (const auto& other : placements_) {
                const auto shared = FindSharedWorldEdge(p, other);
                if (!shared || shared->sideA != side) continue;
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

const WorldSnapshot& Exploration::Snapshot() {
    if (snapshotDirty_) { snapshot_ = BuildSnapshot(world_, state_); snapshotDirty_ = false; }
    return snapshot_;
}

ExplorationOutput Exploration::Step(const InputState& input, double seconds) {
    ExplorationOutput out;
    if (!Ready()) return out;
    // 部屋の切り替え（镜头の演出）の間も操作はそのまま効く。領域切替は結果の観測だけ。
    auto result = StepRules(world_, state_, input, seconds);
    const bool respawn = std::any_of(result.events.begin(), result.events.end(), [](const RuleEvent& e) { return e.kind == EventKind::PlayerRespawned; });
    const auto current = RegionAt(state_.player.rect);
    if (!current.empty() && current != activeRegion_) {
        out.transitions.push_back({respawn ? "respawn" : "crossed", activeRegion_, current, {state_.player.rect.x, state_.player.rect.y}});
        activeRegion_ = current;
    }
    Record(std::move(result), out); snapshotDirty_ = true;
    return out;
}
} // namespace TD2::Rules
