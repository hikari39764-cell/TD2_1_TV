#include "TD2GameSession.h"
#include "../Common/TD2PreviewEntry.h"
#include "HIKARI/Project/ActionViews.h"
#include "HIKARI/Project/GameFlowViews.h"
#include "HIKARI/Project/WorldQuery.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace TD2 {

void GameSession::OnGameStarted(HIKARI::WorldView&) {
    exploration_ = {}; regions_ = {}; camera_.Reset(); machines_ = {};
    sceneActive_ = leftHeld_ = rightHeld_ = false;
    previewEntryObject_ = 0; previewSpawn_.reset();
    traceSeconds_ = 0; lastFrame_ = (std::numeric_limits<uint64_t>::max)();
}

void GameSession::OnSceneEntered(HIKARI::WorldView& world) {
    HIKARI::GameProjectSceneEntry entry{};
    previewEntryObject_ = 0; previewSpawn_.reset();
    if (world.GetSceneFlow().ConsumeSceneEntry(entry) && std::string_view(entry.spawnPointId).starts_with(kPreviewEntryPrefix)) {
        const auto spawn = DecodePreviewSpawn(entry.spawnPointId);
        if (!spawn || !entry.userData) { std::fprintf(stderr, "[TD2 configuration] invalid test spawn\n"); return; }
        previewEntryObject_ = entry.userData; previewSpawn_ = Rules::Vec2{spawn->x, spawn->y};
    }
    sceneActive_ = true; SyncRegions(world);
}

void GameSession::OnSceneExiting(HIKARI::WorldView& world) {
    camera_.Stop(world);
    sceneActive_ = false;
}

void GameSession::SyncRegions(HIKARI::WorldView& world) {
    std::vector<Rules::RegionSource> sources;
    bool definitionsValid = true;
    std::string previewEntry;
    regions_ = {};
    const HIKARI::ComponentTypeId regionTypes[]{HIKARI::MakeComponentTypeId(kRegionComponentType)};
    const auto found = world.ForEachComponents<RegionComponent>(regionTypes, HIKARI::GameProjectWorldQueryAnchor::FirstRequiredTerm,
        [this, &sources, &definitionsValid, &previewEntry](HIKARI::ObjectView object, RegionComponent& component) {
            Rules::RegionSource source; source.placement = component.Placement(); source.entry = component.IsEntryRegion();
            const auto& id = source.placement.regionId;
            if (object.GetSceneObjectId() == previewEntryObject_) { previewEntry = id; }
            regions_.televisions[id] = object.GetHandle();
            regions_.cameras[id] = component.CameraObject();
            regions_.geometry[id] = component.Geometry();
            std::string error;
            if (!component.BuildDefinition(source.definition, error) && source.placement.worldPlaced) {
                std::fprintf(stderr, "[TD2 configuration] region=%s error=%s\n", id.c_str(), error.c_str());
                definitionsValid = false;
            }
            sources.push_back(std::move(source));
        });
    if (!found.Succeeded() || !definitionsValid) {
        std::fprintf(stderr, "[TD2 configuration] world rejected query=%u validDefinitions=%d\n", static_cast<unsigned>(found.code), definitionsValid);
        return;
    }
    if (!exploration_.Ready()) {
        // 不正な入口は既定の区域へ落とさずエラーにする。
        if (previewEntryObject_ && previewEntry.empty()) {
            std::fprintf(stderr, "[TD2 configuration] test start object missing: %llu\n", static_cast<unsigned long long>(previewEntryObject_));
            return;
        }
        auto machines = machines_.Sync(world, regions_, sources);
        const auto error = exploration_.Initialize(std::move(sources), std::move(machines.mechanisms), std::move(machines.slides),
                                                   std::move(machines.stacks), std::move(machines.carousels), previewEntry, previewSpawn_);
        if (!error.empty()) { std::fprintf(stderr, "[TD2 configuration] %s\n", error.c_str()); return; }
        for (const auto& problem : exploration_.Problems()) std::fprintf(stderr, "[TD2 configuration] disabled %s\n", problem.c_str());
        std::fprintf(stderr, "[TD2 world] ready rooms=%zu entry=%s\n", exploration_.Regions().size(), exploration_.ActiveRegion().c_str());
    }
    camera_.SyncBindings(world);
    camera_.SyncAreas(world);
    if (world.GetRuntimeSession().IsPlayActive() && exploration_.Ready() && !camera_.Initialized()) {
        camera_.Initialize(world, exploration_, regions_);
    }
    std::fflush(stderr);
}

void GameSession::Update(HIKARI::WorldView& world, const HIKARI::FrameContext& frame) {
    if (!sceneActive_ || !exploration_.Ready() || !world.GetRuntimeSession().IsPlayActive() || frame.paused || lastFrame_ == frame.frameIndex) return;
    lastFrame_ = frame.frameIndex;
    HIKARI::GameProjectInputActionState move{}, action{};
    const auto input = world.GetInput();
    const bool moveValid = input.GetAction("Gameplay.Move", move), actionValid = input.GetAction("Gameplay.Jump", action);
    const bool left = moveValid && move.x < -.1f, right = moveValid && move.x > .1f;
    Rules::InputState buttons;
    buttons.left = {left, left && !leftHeld_, !left && leftHeld_};
    buttons.right = {right, right && !rightHeld_, !right && rightHeld_};
    buttons.action = {actionValid && action.down != 0, actionValid && action.pressed != 0, actionValid && action.released != 0};
    leftHeld_ = left; rightHeld_ = right;
    const double frameSeconds = std::isfinite(frame.gameDt) ? std::clamp(double(frame.gameDt), 0.0, .25) : 0;
    double remaining = camera_.HoldsWorld() ? 0 : frameSeconds;
    camera_.AdvanceClock(frameSeconds);
    if (!camera_.Initialized()) camera_.Initialize(world, exploration_, regions_);
    while (remaining > 1e-12) {
        const double slice = std::min(remaining, 1.0 / 120);
        const auto output = exploration_.Step(buttons, slice);
        for (const auto& region : output.regions)
            for (const auto& event : region.output.events) {
                const auto name = Rules::EventName(event.kind);
                std::fprintf(stderr, "[TD2 event] region=%s event=%.*s subject=%s x=%.4f y=%.4f sim=%.4f detail=%s\n", region.regionId.c_str(),
                    static_cast<int>(name.size()), name.data(), event.subjectId.c_str(), event.position.x, event.position.y, event.simulationTime,
                    event.detail.c_str());
            }
        for (const auto& transition : output.transitions) {
            std::fprintf(stderr, "[TD2 crossing] kind=%s from=%s to=%s world=(%.5f,%.5f) velocity=(%.5f,%.5f) frame=%llu\n",
                transition.kind.c_str(), transition.fromRegion.c_str(), transition.toRegion.c_str(), transition.worldPosition.x, transition.worldPosition.y,
                exploration_.State().player.vx, exploration_.State().player.vy, static_cast<unsigned long long>(frame.frameIndex));
            camera_.OnCrossing(world, exploration_, regions_, transition);
        }
        buttons.action.pressed = buttons.action.released = false; remaining -= slice;
    }
    machines_.Update(world, exploration_, regions_, camera_, frameSeconds);
    camera_.Update(world, exploration_, regions_, moveValid ? move.x : 0.f, frameSeconds,
                   std::isfinite(frame.gameDt) ? std::max(0.0, double(frame.gameDt)) : 0.0);
    traceSeconds_ += std::max(0.0, double(frame.gameDt));
    Trace(world, frame.frameIndex);
}

// 状態の記録は 1 秒ごと（検証で細かく見たい時は TD2_TRACE_SECONDS で間隔を変える）。
void GameSession::Trace(HIKARI::WorldView& world, uint64_t frame) {
    static const double traceInterval = std::getenv("TD2_TRACE_SECONDS") ? std::atof(std::getenv("TD2_TRACE_SECONDS")) : 1.0;
    if (traceSeconds_ < traceInterval) return;
    traceSeconds_ = 0;
    const auto& state = exploration_.State();
    std::fprintf(stderr, "[TD2 world-state] region=%s tick=%llu sim=%.4f real=%.4f player=(%.5f,%.5f) velocity=(%.5f,%.5f) echo=%d echoPos=(%.5f,%.5f) deaths=%llu checkpoint=%s travel=%s progress=%.3f frame=%llu\n",
        exploration_.ActiveRegion().c_str(), static_cast<unsigned long long>(state.tick), state.simulationTime, state.realTime,
        state.player.rect.x, state.player.rect.y, state.player.vx, state.player.vy, state.echo.has_value(), state.echo ? state.echo->rect.x : 0,
        state.echo ? state.echo->rect.y : 0, static_cast<unsigned long long>(state.deaths), state.checkpointId.c_str(),
        camera_.TravelLabel().c_str(), camera_.TravelProgress(), static_cast<unsigned long long>(frame));
    const auto prefix = exploration_.ActiveRegion() + "/";
    for (const auto& device : exploration_.Snapshot().devices) if (device.id.starts_with(prefix))
        std::fprintf(stderr, "[TD2 device] region=%s id=%s active=%d pressed=%d solid=%d\n", exploration_.ActiveRegion().c_str(),
            device.id.c_str() + prefix.size(), device.active, device.pressed, device.solid);
    camera_.TraceSequence(world, frame);
    std::fflush(stderr);
}

}
