#include "TD2GameSession.h"
#include "TD2RegionComponent.h"
#include "TD2TravelComponents.h"
#include "HIKARI/Project/ActionViews.h"
#include "HIKARI/Project/GameFlowViews.h"
#include "HIKARI/Project/SequenceView.h"
#include "HIKARI/Project/WorldQuery.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace TD2 {
namespace {
std::string_view EventName(Rules::EventKind event) {
    using E = Rules::EventKind;
    switch (event) {
    case E::Blink: return "Blink"; case E::Bounce: return "Bounce"; case E::BlinkRejected: return "BlinkRejected";
    case E::EchoCreated: return "EchoCreated"; case E::EchoRecalled: return "EchoRecalled"; case E::EchoRemoved: return "EchoRemoved";
    case E::EchoSpawnBlocked: return "EchoSpawnBlocked"; case E::PlateChanged: return "PlateChanged";
    case E::DeviceChanged: return "DeviceChanged"; case E::CheckpointActivated: return "CheckpointActivated";
    case E::PlayerDied: return "PlayerDied"; case E::PlayerRespawned: return "PlayerRespawned";
    case E::ExplorationRestarted: return "ExplorationRestarted";
    }
    return "Unknown";
}
} // namespace

void GameSession::OnGameStarted(HIKARI::WorldView&) {
    exploration_ = {}; bindings_.clear(); playback_ = {}; playbackBinding_ = {};
    regionCameras_.clear();
    sceneActive_ = leftHeld_ = rightHeld_ = lastInputLocked_ = false;
    traceSeconds_ = 0; lastFrame_ = (std::numeric_limits<uint64_t>::max)();
}
void GameSession::OnSceneEntered(HIKARI::WorldView& world) {
    sceneActive_ = true; SyncRegions(world);
}
void GameSession::OnSceneExiting(HIKARI::WorldView& world) {
    if (playback_.IsValid()) (void)world.GetSequence().Stop(playback_);
    playback_ = {}; exploration_.SetInputLocked(false); sceneActive_ = false;
}

void GameSession::SyncRegions(HIKARI::WorldView& world) {
    std::vector<Rules::RegionSource> sources;
    bool definitionsValid = true;
    const HIKARI::ComponentTypeId regionTypes[]{HIKARI::MakeComponentTypeId(kRegionComponentType)};
    const auto regions = world.ForEachComponents<RegionComponent>(regionTypes,
        HIKARI::GameProjectWorldQueryAnchor::FirstRequiredTerm,
        [&sources, &definitionsValid](HIKARI::ObjectView, RegionComponent& component) {
            Rules::RegionSource source; source.placement = component.Placement(); source.entry = component.IsEntryRegion();
            std::string error;
            if (!component.BuildDefinition(source.definition, error) && source.placement.worldPlaced) {
                std::fprintf(stderr, "[TD2 configuration] region=%s error=%s\n", source.placement.regionId.c_str(), error.c_str());
                component.SetRuntimeStatus(error); definitionsValid = false;
            }
            sources.push_back(std::move(source));
        });
    if (!regions.Succeeded() || !definitionsValid) {
        std::fprintf(stderr, "[TD2 configuration] world rejected query=%u validDefinitions=%d\n", static_cast<unsigned>(regions.code), definitionsValid);
        return;
    }
    if (!exploration_.Ready()) {
        const auto error = exploration_.Initialize(std::move(sources));
        if (!error.empty()) { std::fprintf(stderr, "[TD2 configuration] %s\n", error.c_str()); return; }
        std::fprintf(stderr, "[TD2 world] ready rooms=%zu entry=%s\n", exploration_.Regions().size(), exploration_.ActiveRegion().c_str());
    }
    std::vector<RegionSequenceBinding> authored;
    const HIKARI::ComponentTypeId sequenceTypes[]{HIKARI::MakeComponentTypeId(kRegionSequenceComponentType)};
    const auto result = world.ForEachComponents<RegionSequenceComponent>(sequenceTypes,
        HIKARI::GameProjectWorldQueryAnchor::FirstRequiredTerm,
        [&authored](HIKARI::ObjectView, RegionSequenceComponent& component) { authored.push_back(component.Definition()); });
    bindings_.clear();
    if (!result.Succeeded()) { std::fprintf(stderr, "[TD2 sequence] binding query failed code=%u\n", static_cast<unsigned>(result.code)); return; }
    for (const auto& binding : authored) {
        const auto validation = ValidateRegionSequenceBinding(binding);
        const auto duplicates = std::count_if(authored.begin(), authored.end(), [&](const auto& other) { return SameRegionPair(binding, other); });
        if (!validation.valid || duplicates != 1) {
            std::fprintf(stderr, "[TD2 sequence] disabled pair=%s/%s error=%s duplicateCount=%lld\n", binding.regionA.c_str(), binding.regionB.c_str(),
                validation.message.c_str(), static_cast<long long>(duplicates));
            continue;
        }
        bindings_.push_back(binding);
    }
    std::fflush(stderr);
}

void GameSession::StartSequence(HIKARI::WorldView& world, const Rules::RegionTransition& transition) {
    const auto sequence = world.GetSequence();
    // 遠隔セーブ復帰は現在地との隣接を要求しない。A→B→C の後でも記録済み A の端点を評価する。
    if (transition.kind == "respawn") { RestoreRegionCamera(world, transition.toRegion); return; }
    // 未設定・失敗した到着で古い鏡頭を復活させない。その領域は以後も「現鏡頭保持」に戻る。
    regionCameras_.erase(transition.toRegion);
    const auto* from = exploration_.FindRegion(transition.fromRegion);
    const auto* to = exploration_.FindRegion(transition.toRegion);
    const auto found = std::find_if(bindings_.begin(), bindings_.end(), [&](const auto& binding) {
        return (binding.regionA == transition.fromRegion && binding.regionB == transition.toRegion) ||
               (binding.regionB == transition.fromRegion && binding.regionA == transition.toRegion);
    });
    // 未接続対に残った古い演出レコードを、通常の隣室移動として再生しない。
    if (transition.kind != "crossed" || !from || !to || !FindSharedWorldEdge(from->placement, to->placement) ||
        found == bindings_.end() || found->sequenceAssetGuid.empty()) {
        if (sequence.IsPlaying(playback_)) (void)sequence.Pause(playback_);
        return;
    }
    const auto& binding = *found;
    const bool reverse = transition.fromRegion == binding.regionB;
    const float rate = reverse ? -1.f : 1.f;
    if (binding == playbackBinding_ && sequence.IsActive(playback_)) {
        // 同じ実アセットの保持ハンドルを反転する。ゲーム時間や入力履歴は巻き戻さない。
        if (sequence.SetPlaybackRate(playback_, rate) && (sequence.IsPlaying(playback_) || sequence.Resume(playback_))) {
            RememberRegionCameras(transition, binding);
            std::fprintf(stderr, "[TD2 sequence] reused asset=%s reverse=%d handle=%llu\n", binding.sequenceAssetGuid.c_str(), reverse,
                static_cast<unsigned long long>(playback_.value));
            return;
        }
    }
    const auto started = PlayBoundSequence(world, binding, rate);
    if (!started.IsValid()) return;
    playback_ = started; playbackBinding_ = binding;
    HIKARI::GameProjectSequencePlaybackSnapshot snapshot;
    // 逆方向の初回だけ、ロード済みアセットが返す実時間長を使う。作者 duration の複製は持たない。
    if (!sequence.GetSnapshot(playback_, snapshot) || !std::isfinite(snapshot.durationSeconds) || snapshot.durationSeconds <= 0 ||
        (reverse && (!sequence.Seek(playback_, snapshot.durationSeconds) || (!sequence.IsPlaying(playback_) && !sequence.Resume(playback_))))) {
        (void)sequence.Stop(playback_); playback_ = {};
        std::fprintf(stderr, "[TD2 sequence] invalid snapshot or reverse seek failed asset=%s\n", binding.sequenceAssetGuid.c_str());
        return;
    }
    RememberRegionCameras(transition, binding);
    std::fprintf(stderr, "[TD2 sequence] started asset=%s reverse=%d duration=%.4f handle=%llu\n", binding.sequenceAssetGuid.c_str(), reverse,
        snapshot.durationSeconds, static_cast<unsigned long long>(playback_.value));
}

HIKARI::GameProjectSequencePlaybackHandle GameSession::PlayBoundSequence(HIKARI::WorldView& world,
                                                                        const RegionSequenceBinding& binding, float rate) {
    const auto sequence = world.GetSequence();
    HIKARI::GameProjectSequencePlayOptions options;
    options.holdAtEnd = 1; options.loop = 0; options.playbackRate = rate; options.priority = 1000;
    options.channelPolicy = HIKARI::GameProjectSequenceChannelPolicy::Replace;
    constexpr char channel[] = "TD2.RegionTransition";
    std::memcpy(options.channel, channel, sizeof(channel));
    HIKARI::GameProjectSequenceBinding slot;
    if (!binding.sequenceSlotName.empty()) {
        const auto object = world.FindSceneObject(binding.cameraObject);
        if (!object.IsValid()) {
            std::fprintf(stderr, "[TD2 sequence] missing binding object=%llu asset=%s\n", static_cast<unsigned long long>(binding.cameraObject), binding.sequenceAssetGuid.c_str());
            if (sequence.IsPlaying(playback_)) (void)sequence.Pause(playback_);
            return {};
        }
        std::memcpy(slot.slotName, binding.sequenceSlotName.c_str(), binding.sequenceSlotName.size() + 1);
        slot.object = object.GetHandle(); options.bindings = &slot; options.bindingCount = 1;
    }
    const auto started = sequence.Play(binding.sequenceAssetGuid, options);
    if (!started.IsValid()) {
        std::fprintf(stderr, "[TD2 sequence] Play rejected asset=%s reverse=%d\n", binding.sequenceAssetGuid.c_str(), rate < 0);
        if (sequence.IsPlaying(playback_)) (void)sequence.Pause(playback_);
        return {};
    }
    return started;
}

void GameSession::RememberRegionCameras(const Rules::RegionTransition& transition, const RegionSequenceBinding& binding) {
    const bool reverse = transition.fromRegion == binding.regionB;
    // 開始領域はまだ到着履歴がないので出発端を補う。既存の領域は直近の到着鏡頭を優先する。
    regionCameras_.try_emplace(transition.fromRegion, RegionCameraAnchor{binding, reverse});
    regionCameras_[transition.toRegion] = {binding, !reverse};
}

void GameSession::RestoreRegionCamera(HIKARI::WorldView& world, std::string_view region) {
    const auto sequence = world.GetSequence();
    const auto found = regionCameras_.find(std::string(region));
    if (found == regionCameras_.end()) {
        if (sequence.IsPlaying(playback_)) (void)sequence.Pause(playback_);
        std::fprintf(stderr, "[TD2 sequence-restore] region=%.*s mode=hold reason=no-bound-arrival\n", static_cast<int>(region.size()), region.data());
        return;
    }
    const auto& anchor = found->second;
    const bool reused = anchor.binding == playbackBinding_ && sequence.IsActive(playback_);
    if (!reused) {
        // 別の区間の演出へ移っていても、保存した同じ既存アセットを同期ロードして端点だけを評価する。
        const auto started = PlayBoundSequence(world, anchor.binding, 1.f);
        if (!started.IsValid()) {
            std::fprintf(stderr, "[TD2 sequence-restore] region=%.*s restored=0 reason=play-rejected\n", static_cast<int>(region.size()), region.data());
            return;
        }
        playback_ = started; playbackBinding_ = anchor.binding;
    }
    HIKARI::GameProjectSequencePlaybackSnapshot snapshot;
    const bool snapshotValid = sequence.GetSnapshot(playback_, snapshot) && std::isfinite(snapshot.durationSeconds) && snapshot.durationSeconds > 0;
    const float time = anchor.atEnd ? snapshot.durationSeconds : 0.f;
    const bool sought = snapshotValid && sequence.Seek(playback_, time);
    // Seek と Pause は同じ更新内。復活で経路を再生せず、保持中のハンドルを入力ロックにしない。
    const bool paused = sequence.IsPaused(playback_) || sequence.Pause(playback_);
    std::fprintf(stderr, "[TD2 sequence-restore] region=%.*s asset=%s endpoint=%s time=%.4f reused=%d restored=%d playing=%d\n",
        static_cast<int>(region.size()), region.data(), anchor.binding.sequenceAssetGuid.c_str(), anchor.atEnd ? "end" : "start", time,
        reused, sought && paused, sequence.IsPlaying(playback_));
}

void GameSession::TraceEvents(std::string_view region, const Rules::StepOutput& output) {
    for (const auto& event : output.events) {
        const auto name = EventName(event.kind);
        std::fprintf(stderr, "[TD2 event] region=%.*s event=%.*s subject=%s x=%.4f y=%.4f sim=%.4f detail=%s\n",
            static_cast<int>(region.size()), region.data(), static_cast<int>(name.size()), name.data(), event.subjectId.c_str(),
            event.position.x, event.position.y, event.simulationTime, event.detail.c_str());
    }
}
void GameSession::TraceSequence(HIKARI::WorldView& world, uint64_t frame) {
    HIKARI::GameProjectSequencePlaybackSnapshot snapshot;
    const auto sequence = world.GetSequence(); const bool available = sequence.GetSnapshot(playback_, snapshot);
    std::fprintf(stderr, "[TD2 sequence-state] available=%d playing=%d state=%u time=%.4f duration=%.4f asset=%s frame=%llu\n",
        available, sequence.IsPlaying(playback_), static_cast<unsigned>(snapshot.state), snapshot.timeSeconds, snapshot.durationSeconds,
        snapshot.assetGuid, static_cast<unsigned long long>(frame));
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
    bool suppressed = false;
    double remaining = std::isfinite(frame.gameDt) ? std::clamp(double(frame.gameDt), 0.0, .25) : 0;
    // 境界を跨いだ残りの同一フレームにも実際の Sequence ロックを適用する。
    do {
        exploration_.SetInputLocked(world.GetSequence().IsPlaying(playback_));
        const double slice = std::min(remaining, 1.0 / 120);
        const auto output = exploration_.Step(buttons, slice); suppressed = suppressed || output.inputSuppressed;
        for (const auto& region : output.regions) TraceEvents(region.regionId, region.output);
        for (const auto& transition : output.transitions) {
            std::fprintf(stderr, "[TD2 crossing] kind=%s from=%s to=%s world=(%.5f,%.5f) velocity=(%.5f,%.5f) frame=%llu\n",
                transition.kind.c_str(), transition.fromRegion.c_str(), transition.toRegion.c_str(), transition.worldPosition.x, transition.worldPosition.y,
                exploration_.State().player.vx, exploration_.State().player.vy, static_cast<unsigned long long>(frame.frameIndex));
            StartSequence(world, transition);
        }
        buttons.action.pressed = buttons.action.released = false; remaining -= slice;
    } while (remaining > 1e-12);
    const bool locked = world.GetSequence().IsPlaying(playback_);
    exploration_.SetInputLocked(locked);
    if (locked != lastInputLocked_ || (suppressed && (action.pressed || action.released))) {
        const auto& player = exploration_.State().player;
        std::fprintf(stderr, "[TD2 input-lock] inputLocked=%d inputSuppressed=%d actionNeedsRelease=%d rawMove=%.2f rawActionDown=%u rawActionPressed=%u rawActionReleased=%u region=%s world=(%.5f,%.5f) frame=%llu\n",
            locked, suppressed, exploration_.ActionNeedsRelease(), move.x, static_cast<unsigned>(action.down), static_cast<unsigned>(action.pressed),
            static_cast<unsigned>(action.released), exploration_.ActiveRegion().c_str(), player.rect.x, player.rect.y, static_cast<unsigned long long>(frame.frameIndex));
        TraceSequence(world, frame.frameIndex);
    }
    lastInputLocked_ = locked;
    traceSeconds_ += std::max(0.0, double(frame.gameDt));
    if (traceSeconds_ >= 1) {
        traceSeconds_ = 0;
        const auto& state = exploration_.State();
        std::fprintf(stderr, "[TD2 world-state] region=%s tick=%llu sim=%.4f real=%.4f player=(%.5f,%.5f) velocity=(%.5f,%.5f) echo=%d echoPos=(%.5f,%.5f) deaths=%llu checkpoint=%s inputLocked=%d actionNeedsRelease=%d frame=%llu\n",
            exploration_.ActiveRegion().c_str(), static_cast<unsigned long long>(state.tick), state.simulationTime, state.realTime,
            state.player.rect.x, state.player.rect.y, state.player.vx, state.player.vy, state.echo.has_value(), state.echo ? state.echo->rect.x : 0,
            state.echo ? state.echo->rect.y : 0, static_cast<unsigned long long>(state.deaths), state.checkpointId.c_str(), locked,
            exploration_.ActionNeedsRelease(), static_cast<unsigned long long>(frame.frameIndex));
        for (const auto& [id, region] : exploration_.Regions()) for (const auto& device : region.snapshot.devices)
            std::fprintf(stderr, "[TD2 device] region=%s id=%s active=%d pressed=%d solid=%d\n", id.c_str(), device.id.c_str(), device.active, device.pressed, device.solid);
        TraceSequence(world, frame.frameIndex);
    }
    std::fflush(stderr);
}
} // namespace TD2
