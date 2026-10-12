#include "TD2Exploration.h"
#include <algorithm>
#include <utility>

namespace TD2::Rules {
namespace {
Rect RoomRect(const RegionPlacement& p) { return {double(p.worldX), double(p.worldY), double(p.cols), double(p.rows)}; }
bool Overlap(const Rect& a, const Rect& b) {
    return a.x < b.x + b.w && a.x + a.w > b.x && a.y < b.y + b.h && a.y + a.h > b.y;
}
Rect Local(Rect r, const RegionPlacement& p) { r.x -= p.worldX; r.y -= p.worldY; return r; }
std::string DeviceId(std::string_view region, std::string_view local) {
    // region ID は '/' を含まない契約なので、同名の局所装置が別 TV と混ざらない。
    return std::string(region) + "/" + std::string(local);
}
} // namespace

std::string Exploration::Initialize(std::vector<RegionSource> sources) {
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
        if (source.entry) {
            ++entries;
            next.world_.spawn = {authored.spawn.x + p.worldX, authored.spawn.y + p.worldY};
        }
        RegionRuntime runtime;
        runtime.definition = std::move(source.definition); runtime.placement = p;
        runtime.deviceBegin = next.world_.devices.size();
        next.world_.chunks.push_back({runtime.definition.region, p.worldX, p.worldY});
        for (auto device : runtime.definition.devices) {
            device.id = DeviceId(p.regionId, device.id);
            device.rect.x += p.worldX; device.rect.y += p.worldY;
            for (auto& target : device.targets) target = DeviceId(p.regionId, target);
            next.deviceOwners_[device.id] = p.regionId;
            next.world_.devices.push_back(std::move(device));
        }
        next.regions_.emplace(p.regionId, std::move(runtime));
    }
    if (entries != 1) return "world requires exactly one placed entryRegion; found " + std::to_string(entries);
    next.world_.region.regionId = "td2.world";
    next.BuildBoundaries();
    if (const auto error = ValidateWorld(next.world_); !error.empty()) return error;
    next.state_ = CreateWorldState(next.world_);
    next.activeRegion_ = next.RegionAt(next.state_.player.rect);
    next.RefreshSnapshots();
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

void Exploration::RefreshSnapshots() {
    const auto global = BuildSnapshot(world_, state_);
    for (auto& [id, region] : regions_) {
        const auto& p = region.placement; const auto bounds = RoomRect(p);
        auto local = global;
        local.regionId = id;
        local.playerPresent = global.playerPresent && Overlap(global.presentationPlayer, bounds);
        local.player.rect = Local(global.player.rect, p); local.presentationPlayer = Local(global.presentationPlayer, p);
        if (global.echo && Overlap(global.echo->rect, bounds)) local.echo->rect = Local(global.echo->rect, p);
        else local.echo.reset();
        if (global.pendingEcho && Overlap(*global.pendingEcho, bounds)) local.pendingEcho = Local(*global.pendingEcho, p);
        else local.pendingEcho.reset();
        const auto prefix = id + "/";
        local.checkpointId = global.checkpointId.starts_with(prefix) ? global.checkpointId.substr(prefix.size()) : std::string{};
        local.devices.clear();
        for (size_t i = 0; i < region.definition.devices.size(); ++i) {
            auto device = global.devices[region.deviceBegin + i];
            device.id = region.definition.devices[i].id; device.rect = Local(device.rect, p);
            local.devices.push_back(std::move(device));
        }
        local.beams.clear();
        for (auto beam : global.beams) if (Overlap(beam.rect, bounds)) {
            const double right = std::min(beam.rect.x + beam.rect.w, bounds.x + bounds.w);
            const double bottom = std::min(beam.rect.y + beam.rect.h, bounds.y + bounds.h);
            beam.rect.x = std::max(beam.rect.x, bounds.x); beam.rect.y = std::max(beam.rect.y, bounds.y);
            beam.rect.w = right - beam.rect.x; beam.rect.h = bottom - beam.rect.y;
            beam.rect = Local(beam.rect, p); local.beams.push_back(std::move(beam));
        }
        region.snapshot = std::move(local);
    }
}

ExplorationOutput Exploration::Step(const InputState& input, double seconds) {
    ExplorationOutput out;
    if (!Ready()) return out;
    auto buttons = input;
    if (inputLocked_) {
        out.inputSuppressed = true;
        if (input.action.held || input.action.pressed) actionNeedsRelease_ = true;
        if (input.action.released || (!input.action.held && !input.action.pressed)) actionNeedsRelease_ = false;
        buttons = {};
        state_.blinkBuffer = 0; state_.pendingActionPressed = state_.pendingActionReleased = false;
    } else if (actionNeedsRelease_) {
        buttons.action = {};
        if (input.action.released || !input.action.held) actionNeedsRelease_ = false;
    }
    // 操作を捨てても重力・反発・残影・装置の物理はそのまま進める。領域切替は結果の観測だけ。
    auto result = StepRules(world_, state_, buttons, seconds);
    const bool respawn = std::any_of(result.events.begin(), result.events.end(), [](const RuleEvent& e) { return e.kind == EventKind::PlayerRespawned; });
    const auto current = RegionAt(state_.player.rect);
    if (!current.empty() && current != activeRegion_) {
        out.transitions.push_back({respawn ? "respawn" : "crossed", activeRegion_, current, {state_.player.rect.x, state_.player.rect.y}});
        activeRegion_ = current;
    }
    Record(std::move(result), out); RefreshSnapshots();
    return out;
}
} // namespace TD2::Rules
