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
namespace {
// 診断用（環境変数 TD2_TEST_ENTRY に区域 ID）：オープニングを飛ばし、その区域の出生点から始める（無窓の検証用）。
const char* TestEntryRegion() {
    static const char* region = std::getenv("TD2_TEST_ENTRY");
    return region;
}
}

void GameSession::OnGameStarted(HIKARI::WorldView&) {
    exploration_ = {}; regions_ = {}; camera_.Reset(); machines_ = {}; introChecked_ = ending_ = false;
    initialSources_.clear(); initialMachines_ = {};
    sceneActive_ = leftHeld_ = rightHeld_ = false;
    previewEntryObject_ = 0; previewSpawn_.reset(); previewEntryRegion_.clear(); previewStates_.clear();
    recording_ = recordClosed_ = false; record_ = {};
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
    intro_.Stop(world); introChecked_ = ending_ = false;
    camera_.Stop(world);
    FinishRecord();
    sceneActive_ = false;
}

// 試遊の記録を書き終える（編集器が解法として取り込む）。
void GameSession::FinishRecord() {
    if (!recording_) return;
    recording_ = false; recordClosed_ = true;
    const auto& state = exploration_.State();
    record_.endRegion = exploration_.ActiveRegion();
    record_.endX = state.player.rect.x; record_.endY = state.player.rect.y; record_.endDeaths = state.deaths;
    (void)AppendPlayRecord(LastPlayPath(), record_, recordCursor_, true);
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
            // まだレベル編集が整えていないテレビ（区域を付けたばかりで ID が既定のまま、またはシーンで複製して ID が重なる）は
            // 世界に入れない。一台のために世界全体を止めない。
            if (id == RegionDefinition{}.regionId || regions_.televisions.contains(id)) {
                std::fprintf(stderr, "[TD2 configuration] skipped unprepared tv object=%llu region=%s\n",
                             static_cast<unsigned long long>(object.GetSceneObjectId()), id.c_str());
                return;
            }
            if (object.GetSceneObjectId() == previewEntryObject_ || (TestEntryRegion() && id == TestEntryRegion())) { previewEntry = id; }
            regions_.televisions[id] = object.GetHandle();
            regions_.cameras[id] = component.CameraObject();
            regions_.geometry[id] = RegionComponent::Surface(object);
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
        initialSources_ = sources; initialMachines_ = machines;
        const auto error = exploration_.Initialize(std::move(sources), std::move(machines.mechanisms), std::move(machines.slides),
                                                   std::move(machines.stacks), std::move(machines.carousels), previewEntry, previewSpawn_);
        if (!error.empty()) { std::fprintf(stderr, "[TD2 configuration] %s\n", error.c_str()); return; }
        for (const auto& problem : exploration_.Problems()) std::fprintf(stderr, "[TD2 configuration] disabled %s\n", problem.c_str());
        // 編集器から途中の状態で試遊する時：書いておいた仕掛けの状態へ置く。
        previewEntryRegion_ = previewEntry; previewStates_.clear();
        if (previewEntryObject_) {
            for (const auto& [id, state] : TakePreviewStates()) {
                if (exploration_.SetMechanismState(id, state)) previewStates_.push_back({id, state});
            }
        }
        (void)exploration_.TakeCalls();
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
    // オープニングの演出：最初の Play のフレームで始め（「この画面から」の試遊は除く）、終わるまで規則を進めない。
    // スペースは演出を始めるだけで、ダッシュにはならない。カメラは演出が動かし、焦点だけは画面に合わせて追う。
    if (!introChecked_) { introChecked_ = true; (void)intro_.Begin(world, previewSpawn_.has_value() || previewEntryObject_ != 0 || TestEntryRegion()); }
    // 開発用：エディタの Play の時だけ、K でオープニングを飛ばし、O でゴールの部屋へ移る（出荷したゲームでは効かない）。
    if (world.GetRuntimeSession().GetHostKind() == HIKARI::GameProjectRuntimeHostKind::EditorPreview) {
        HIKARI::GameProjectInputActionState skip{}, goal{};
        if (input.GetAction("Gameplay.DevSkipIntro", skip) && skip.pressed) { intro_.Skip(world); }
        if (!ending_ && input.GetAction("Gameplay.DevGoal", goal) && goal.pressed) { intro_.Skip(world); TeleportToGoal(world); }
    }
    const bool cinematic = intro_.Update(world, buttons.action.pressed);
    if (intro_.TakeEndingFinished()) { RestartFromTitle(world); }
    if (cinematic) {
        camera_.Update(world, exploration_, regions_, 0.f, 0.0, std::isfinite(frame.gameDt) ? std::max(0.0, double(frame.gameDt)) : 0.0);
        return;
    }
    // 試遊の記録：このフレームの秒とボタン（空格は押した・離した瞬間も）、世界が止まっていたか。
    if (!recording_ && !recordClosed_) {
        record_ = {}; recordCursor_ = {}; recording_ = true; recordFrame_ = 0;
        // 入口は実際に始めた区域と区域内の位置で残す（作者が開始テレビを変えても解法は同じ所から再生する）。
        record_.states = previewStates_;
        recordRegion_ = exploration_.ActiveRegion(); record_.entryRegion = recordRegion_;
        if (const auto* start = exploration_.FindRegion(recordRegion_)) {
            const auto& body = exploration_.State().player.rect;
            record_.spawn = std::pair{body.x - start->placement.worldX, body.y - start->placement.worldY};
        }
        record_.regions.push_back({0, recordRegion_});
    }
    if (recording_) record_.frames.push_back({frameSeconds, static_cast<uint8_t>((left ? 1 : 0) | (right ? 2 : 0) | (buttons.action.held ? 4 : 0) | (remaining <= 0 ? 8 : 0) |
                                                                  (buttons.action.pressed ? 16 : 0) | (buttons.action.released ? 32 : 0))});
    uint32_t sliceIndex = 0;
    std::string goalRegion;
    const auto takeCalls = [this, &sliceIndex] {
        for (auto& call : exploration_.TakeCalls()) if (recording_) record_.calls.push_back({recordFrame_, sliceIndex, call.kind, call.region, call.direction});
    };
    while (remaining > 1e-12) {
        const double slice = std::min(remaining, 1.0 / 120);
        const auto output = exploration_.Step(buttons, slice);
        for (const auto& region : output.regions)
            for (const auto& event : region.output.events)
                if (recording_ && event.kind == Rules::EventKind::PlayerDied) record_.deaths.push_back({recordFrame_, region.regionId});
        for (const auto& region : output.regions)
            for (const auto& event : region.output.events)
                if (event.kind == Rules::EventKind::GoalReached && goalRegion.empty()) goalRegion = region.regionId;
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
        takeCalls();
        ++sliceIndex;
        buttons.action.pressed = buttons.action.released = false; remaining -= slice;
        // ゴールの扉に入ったら、その刻みで世界を止めて通関の演出へ移る。
        if (!goalRegion.empty()) { StartEnding(world, goalRegion); return; }
    }
    machines_.Update(world, exploration_, regions_, camera_, frameSeconds);
    camera_.Update(world, exploration_, regions_, moveValid ? move.x : 0.f, frameSeconds,
                   std::isfinite(frame.gameDt) ? std::max(0.0, double(frame.gameDt)) : 0.0);
    takeCalls();
    if (recording_ && exploration_.ActiveRegion() != recordRegion_) { recordRegion_ = exploration_.ActiveRegion(); record_.regions.push_back({recordFrame_, recordRegion_}); }
    ++recordFrame_;
    // 終わりの呼び出しが来ない止め方でも残るよう、10 秒ごとに続きを書き足す。長い試遊は 20 分で記録を締める。
    if (recording_ && recordFrame_ >= 72000) { FinishRecord(); }
    else if (recording_ && recordFrame_ % 600 == 0) { (void)AppendPlayRecord(LastPlayPath(), record_, recordCursor_, false); }
    traceSeconds_ += std::max(0.0, double(frame.gameDt));
    Trace(world, frame.frameIndex);
}

void GameSession::StartEnding(HIKARI::WorldView& world, const std::string& region) {
    FinishRecord();
    HIKARI::GameProjectFloat3 eye{}, look{};
    (void)camera_.StartOffset(world, exploration_, regions_, region, eye, look);
    const auto camera = regions_.cameras.find(region);
    ending_ = intro_.BeginEnding(world, regions_.Television(region), camera != regions_.cameras.end() ? camera->second : 0, eye, look);
    std::fprintf(stderr, "[TD2 ending] goal region=%s cinematic=%d\n", region.c_str(), ending_);
    if (!ending_) { RestartFromTitle(world); }
}

void GameSession::RestartFromTitle(HIKARI::WorldView& world) {
    ending_ = false;
    auto sources = initialSources_;
    auto machines = initialMachines_;
    exploration_ = {};
    const auto error = exploration_.Initialize(std::move(sources), std::move(machines.mechanisms), std::move(machines.slides),
                                               std::move(machines.stacks), std::move(machines.carousels));
    if (!error.empty()) { std::fprintf(stderr, "[TD2 configuration] restart failed: %s\n", error.c_str()); return; }
    (void)exploration_.TakeCalls();
    camera_.Stop(world); camera_.Reset();
    camera_.SyncBindings(world); camera_.SyncAreas(world);
    camera_.Initialize(world, exploration_, regions_);
    // 試遊の記録は最初の一回だけ（戻った後の分は残さない）。
    recording_ = false; recordClosed_ = true;
    introChecked_ = true;
    (void)intro_.Begin(world, false);
    std::fprintf(stderr, "[TD2 world] restarted from title entry=%s\n", exploration_.ActiveRegion().c_str());
}

void GameSession::TeleportToGoal(HIKARI::WorldView& world) {
    std::string target;
    for (const auto& [id, room] : exploration_.Regions()) {
        if (!room.placement.worldPlaced) continue;
        if (std::any_of(room.definition.devices.begin(), room.definition.devices.end(), [](const auto& d) { return d.kind == Rules::DeviceKind::Goal; })) { target = id; break; }
    }
    if (target.empty() || !exploration_.Teleport(target)) { std::fprintf(stderr, "[TD2 dev] no goal room\n"); return; }
    camera_.Jump(world, exploration_, regions_, target);
    // 瞬間移動した試遊は解法にならないので、記録を書き終えずに捨てる（編集器は書き終わった記録だけ読む）。
    recording_ = false; recordClosed_ = true;
    std::fprintf(stderr, "[TD2 dev] teleported to goal region=%s\n", target.c_str());
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
