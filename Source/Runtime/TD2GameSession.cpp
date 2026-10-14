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
    case E::SpringLaunched: return "SpringLaunched"; case E::CollectibleTaken: return "CollectibleTaken";
    }
    return "Unknown";
}
} // namespace

void GameSession::OnGameStarted(HIKARI::WorldView&) {
    exploration_ = {}; bindings_.clear(); playback_ = {}; playbackBinding_ = {};
    regionCameras_.clear();
    sceneActive_ = leftHeld_ = rightHeld_ = false;
    cameraFrom_.clear(); cameraAt_.clear(); cameraQueue_.clear();
    regionObjects_.clear(); focusDistance_ = 0;
    traceSeconds_ = 0; lastFrame_ = (std::numeric_limits<uint64_t>::max)();
}
void GameSession::OnSceneEntered(HIKARI::WorldView& world) {
    sceneActive_ = true; SyncRegions(world);
}
void GameSession::OnSceneExiting(HIKARI::WorldView& world) {
    if (playback_.IsValid()) (void)world.GetSequence().Stop(playback_);
    playback_ = {}; sceneActive_ = false;
}

void GameSession::SyncRegions(HIKARI::WorldView& world) {
    std::vector<Rules::RegionSource> sources;
    bool definitionsValid = true;
    regionObjects_.clear();
    const HIKARI::ComponentTypeId regionTypes[]{HIKARI::MakeComponentTypeId(kRegionComponentType)};
    const auto regions = world.ForEachComponents<RegionComponent>(regionTypes,
        HIKARI::GameProjectWorldQueryAnchor::FirstRequiredTerm,
        [this, &sources, &definitionsValid](HIKARI::ObjectView object, RegionComponent& component) {
            Rules::RegionSource source; source.placement = component.Placement(); source.entry = component.IsEntryRegion();
            regionObjects_[source.placement.regionId] = object.GetHandle();
            std::string error;
            if (!component.BuildDefinition(source.definition, error) && source.placement.worldPlaced) {
                std::fprintf(stderr, "[TD2 configuration] region=%s error=%s\n", source.placement.regionId.c_str(), error.c_str());
                definitionsValid = false;
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

// 镜头は主角が実際に通った部屋の順に追う（入力は止めないので、演出の途中で部屋を出入りできる）。
// ・来た部屋へ引き返した時は、流れている演出をその場で逆向きにする。
// ・ほかの部屋へ進んだ時は列に積み、今の切り替えが終わってから次を流す。演出の途中から別の演出の始点へ跳ばない。
//   行き来で戻った分は列から消す。
// ・復活は演出を流さず、その部屋の到着の端点へ置く（遠い存档へ戻るので隣接を問わない）。
void GameSession::FollowCamera(HIKARI::WorldView& world, const Rules::RegionTransition& transition) {
    const auto& region = transition.toRegion;
    if (transition.kind == "respawn") {
        cameraQueue_.clear(); RestoreRegionCamera(world, region); cameraFrom_ = cameraAt_ = region;
        return;
    }
    if (cameraQueue_.empty()) {
        if (region == cameraAt_) { return; }
        if (region == cameraFrom_ && world.GetSequence().IsPlaying(playback_) && PlayTransition(world, cameraAt_, region, 1.0f)) {
            std::swap(cameraFrom_, cameraAt_);
            return;
        }
        cameraQueue_.push_back(region);
        return;
    }
    const auto& previous = cameraQueue_.size() >= 2 ? cameraQueue_[cameraQueue_.size() - 2] : cameraAt_;
    if (region == previous) { cameraQueue_.pop_back(); } else { cameraQueue_.push_back(region); }
}

// 切り替えが止まっていれば、列の次の部屋へ流す。列に残りがある間は速く流して追いつく。
// 演出の無い組（未接続）は流さず、その部屋の到着の端点があればそこへ置き、無ければ今の画を保つ。
void GameSession::AdvanceCamera(HIKARI::WorldView& world) {
    if (cameraQueue_.empty() || world.GetSequence().IsPlaying(playback_)) { return; }
    const auto next = cameraQueue_.front();
    cameraQueue_.pop_front();
    const float catchUp = 1.0f + 0.5f * static_cast<float>(cameraQueue_.size());
    if (!PlayTransition(world, cameraAt_, next, catchUp)) { RestoreRegionCamera(world, next); }
    cameraFrom_ = cameraAt_; cameraAt_ = next;
}

// from → to の演出を流す。同じ演出が残っていれば、その場で向きを変えて続ける（ゲーム時間は巻き戻さない）。
bool GameSession::PlayTransition(HIKARI::WorldView& world, const std::string& from, const std::string& to, float rateScale) {
    const auto sequence = world.GetSequence();
    const auto* a = exploration_.FindRegion(from);
    const auto* b = exploration_.FindRegion(to);
    const auto found = std::find_if(bindings_.begin(), bindings_.end(), [&](const auto& binding) {
        return (binding.regionA == from && binding.regionB == to) || (binding.regionB == from && binding.regionA == to);
    });
    // 未接続対に残った古い演出レコードを、通常の隣室移動として再生しない。
    if (!a || !b || !FindSharedWorldEdge(a->placement, b->placement) || found == bindings_.end() || found->sequenceAssetGuid.empty()) {
        return false;
    }
    // 未設定・失敗した到着で古い鏡頭を復活させない。
    regionCameras_.erase(to);
    const auto& binding = *found;
    const bool reverse = from == binding.regionB;
    const float speed = std::clamp(binding.speed * rateScale, .25f, 8.f);
    const float rate = reverse ? -speed : speed;
    if (binding == playbackBinding_ && sequence.IsActive(playback_)) {
        if (sequence.SetPlaybackRate(playback_, rate) && (sequence.IsPlaying(playback_) || sequence.Resume(playback_))) {
            RememberRegionCameras(from, to, binding);
            std::fprintf(stderr, "[TD2 sequence] reused asset=%s reverse=%d rate=%.3f handle=%llu\n", binding.sequenceAssetGuid.c_str(),
                reverse, rate, static_cast<unsigned long long>(playback_.value));
            return true;
        }
    }
    const auto started = PlayBoundSequence(world, binding, rate);
    if (!started.IsValid()) return false;
    playback_ = started; playbackBinding_ = binding;
    HIKARI::GameProjectSequencePlaybackSnapshot snapshot;
    // 逆方向の初回だけ、ロード済みアセットが返す実時間長を使う。作者 duration の複製は持たない。
    if (!sequence.GetSnapshot(playback_, snapshot) || !std::isfinite(snapshot.durationSeconds) || snapshot.durationSeconds <= 0 ||
        (reverse && (!sequence.Seek(playback_, snapshot.durationSeconds) || (!sequence.IsPlaying(playback_) && !sequence.Resume(playback_))))) {
        (void)sequence.Stop(playback_); playback_ = {};
        std::fprintf(stderr, "[TD2 sequence] invalid snapshot or reverse seek failed asset=%s\n", binding.sequenceAssetGuid.c_str());
        return false;
    }
    RememberRegionCameras(from, to, binding);
    std::fprintf(stderr, "[TD2 sequence] started asset=%s reverse=%d rate=%.3f duration=%.4f handle=%llu\n", binding.sequenceAssetGuid.c_str(),
        reverse, rate, snapshot.durationSeconds, static_cast<unsigned long long>(playback_.value));
    return true;
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

void GameSession::RememberRegionCameras(const std::string& from, const std::string& to, const RegionSequenceBinding& binding) {
    const bool reverse = from == binding.regionB;
    // 開始領域はまだ到着履歴がないので出発端を補う。既存の領域は直近の到着鏡頭を優先する。
    regionCameras_.try_emplace(from, RegionCameraAnchor{binding, reverse});
    regionCameras_[to] = {binding, !reverse};
}

// 被写界深度の焦点を今の部屋の電視の画面へ合わせ続ける。機位ごとに画面までの距離が違い（2.1〜3.9 m）、
// カメラに作者が付けた固定の焦点では遠い電視（寄せて撮る二台など）がぼけるため。
// 画面の中心は電視の原点から局所 (0, 0.5, 0.28)×縮尺の近く（ScreenKit の電視はどれも ±0.15 以内、正面は局所 +Z）。
// 焦点は画面の中心の視線方向の深さ。部屋が変わった瞬間に跳ばないよう、時定数 0.12 秒で寄せる。
void GameSession::FocusActiveScreen(HIKARI::WorldView& world, double seconds) {
    constexpr float kScreenHeight = .5f, kScreenFront = .28f;
    constexpr double kFocusSeconds = .12;
    constexpr uint64_t kFocusSource = 0x5444325F464F4355ull; // "TD2_FOCU"
    auto camera = world.GetCamera();
    const auto found = regionObjects_.find(exploration_.ActiveRegion());
    HIKARI::GameProjectCameraPose pose{};
    HIKARI::GameProjectTransform tv{};
    if (found == regionObjects_.end() || !camera.GetRenderedPose(pose)) { return; }
    if (const auto object = world.FindObject(found->second); !object.IsValid() || !object.GetWorldTransform(tv)) { return; }
    // 局所の点を四元数で回す（v' = v + 2w(q×v) + 2q×(q×v)）。
    const auto& q = tv.rotation;
    const double vx = 0, vy = kScreenHeight * tv.scale.y, vz = kScreenFront * tv.scale.z;
    const double tx = 2 * (q.y * vz - q.z * vy), ty = 2 * (q.z * vx - q.x * vz), tz = 2 * (q.x * vy - q.y * vx);
    const double px = tv.position.x + vx + q.w * tx + (q.y * tz - q.z * ty);
    const double py = tv.position.y + vy + q.w * ty + (q.z * tx - q.x * tz);
    const double pz = tv.position.z + vz + q.w * tz + (q.x * ty - q.y * tx);
    const double fx = pose.target.x - pose.eye.x, fy = pose.target.y - pose.eye.y, fz = pose.target.z - pose.eye.z;
    const double length = std::sqrt(fx * fx + fy * fy + fz * fz);
    if (length < 1e-6) { return; }
    const double depth = ((px - pose.eye.x) * fx + (py - pose.eye.y) * fy + (pz - pose.eye.z) * fz) / length;
    if (!(depth > .1)) { return; }
    focusDistance_ = focusDistance_ > 0 ? focusDistance_ + (depth - focusDistance_) * (1 - std::exp(-seconds / kFocusSeconds)) : depth;
    HIKARI::GameProjectCameraFocusRequest focus{};
    focus.sourceId = kFocusSource; focus.priority = 100;
    focus.mode = HIKARI::GameProjectCameraFocusMode::Distance;
    focus.distance = static_cast<float>(focusDistance_);
    (void)camera.SubmitFocus(focus);
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
    double remaining = std::isfinite(frame.gameDt) ? std::clamp(double(frame.gameDt), 0.0, .25) : 0;
    if (cameraAt_.empty()) { cameraFrom_ = cameraAt_ = exploration_.ActiveRegion(); }
    do {
        const double slice = std::min(remaining, 1.0 / 120);
        const auto output = exploration_.Step(buttons, slice);
        for (const auto& region : output.regions) TraceEvents(region.regionId, region.output);
        for (const auto& transition : output.transitions) {
            std::fprintf(stderr, "[TD2 crossing] kind=%s from=%s to=%s world=(%.5f,%.5f) velocity=(%.5f,%.5f) frame=%llu\n",
                transition.kind.c_str(), transition.fromRegion.c_str(), transition.toRegion.c_str(), transition.worldPosition.x, transition.worldPosition.y,
                exploration_.State().player.vx, exploration_.State().player.vy, static_cast<unsigned long long>(frame.frameIndex));
            FollowCamera(world, transition);
        }
        buttons.action.pressed = buttons.action.released = false; remaining -= slice;
    } while (remaining > 1e-12);
    AdvanceCamera(world);
    FocusActiveScreen(world, std::isfinite(frame.gameDt) ? std::max(0.0, double(frame.gameDt)) : 0.0);
    traceSeconds_ += std::max(0.0, double(frame.gameDt));
    if (traceSeconds_ >= 1) {
        traceSeconds_ = 0;
        const auto& state = exploration_.State();
        std::fprintf(stderr, "[TD2 world-state] region=%s tick=%llu sim=%.4f real=%.4f player=(%.5f,%.5f) velocity=(%.5f,%.5f) echo=%d echoPos=(%.5f,%.5f) deaths=%llu checkpoint=%s cameraQueue=%zu frame=%llu\n",
            exploration_.ActiveRegion().c_str(), static_cast<unsigned long long>(state.tick), state.simulationTime, state.realTime,
            state.player.rect.x, state.player.rect.y, state.player.vx, state.player.vy, state.echo.has_value(), state.echo ? state.echo->rect.x : 0,
            state.echo ? state.echo->rect.y : 0, static_cast<unsigned long long>(state.deaths), state.checkpointId.c_str(),
            cameraQueue_.size(), static_cast<unsigned long long>(frame.frameIndex));
        const auto prefix = exploration_.ActiveRegion() + "/";
        for (const auto& device : exploration_.Snapshot().devices) if (device.id.starts_with(prefix))
            std::fprintf(stderr, "[TD2 device] region=%s id=%s active=%d pressed=%d solid=%d\n", exploration_.ActiveRegion().c_str(),
                device.id.c_str() + prefix.size(), device.active, device.pressed, device.solid);
        TraceSequence(world, frame.frameIndex);
        std::fflush(stderr);
    }
}

} // namespace TD2
