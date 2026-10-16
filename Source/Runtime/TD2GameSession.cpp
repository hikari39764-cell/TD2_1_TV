#include "TD2GameSession.h"
#include "../Common/TD2PreviewEntry.h"
#include "TD2TravelComponents.h"
#include "HIKARI/Project/ActionViews.h"
#include "HIKARI/Project/GameFlowViews.h"
#include "HIKARI/Project/SequenceView.h"
#include "HIKARI/Project/WorldQuery.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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
// 手動の切り替えの重さ（臨界減衰のばねの時定数、秒）：押して全速へ乗るまで、離して滑り止まるまで、逆へ押して止まるまで。
// 道筋そのものも両端で緩やかに止まる（演出の spline 補間）ので、端の手前で別に減速はしない。
constexpr double kTravelPressSeconds = .7, kTravelCoastSeconds = 1.8, kTravelBrakeSeconds = .55;
// 歩いたまま越えた時に持ち込む勢い（全速に対する割合）。越えた瞬間に画が止まって見えないようにする。
constexpr double kTravelCarry = .55;
// 演出の序盤のこの割合までに、始まりの機位のずれを 0 へ戻す。
constexpr double kTravelBlendIn = .35;
constexpr uint64_t kFollowSource = 0x5444325F464F4C4Cull;   // "TD2_FOLL"
constexpr uint64_t kTravelSource = 0x5444325F5452564Cull;   // "TD2_TRVL"
// スカラーの臨界減衰のばね（SmoothDamp と同じ近似）。値の変化の速さ（rate）まで連続するので、動き出しと止まり際に角が出ない。
double Damp(double value, double target, double& rate, double seconds, double dt) {
    const double omega = 2.0 / (std::max)(seconds, 1e-3), x = omega * dt;
    const double decay = 1.0 / (1.0 + x + .48 * x * x + .235 * x * x * x);
    const double change = value - target, temp = (rate + omega * change) * dt;
    rate = (rate - omega * temp) * decay;
    return target + (change + temp) * decay;
}
double SmoothStep(double t) { t = std::clamp(t, 0.0, 1.0); return t * t * (3 - 2 * t); }
HIKARI::GameProjectFloat3 Sub(const HIKARI::GameProjectFloat3& a, const HIKARI::GameProjectFloat3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
HIKARI::GameProjectFloat3 Mad(const HIKARI::GameProjectFloat3& a, const HIKARI::GameProjectFloat3& b, float k) {
    return {a.x + b.x * k, a.y + b.y * k, a.z + b.z * k};
}
} // namespace

void GameSession::OnGameStarted(HIKARI::WorldView&) {
    exploration_ = {}; bindings_.clear(); playback_ = {}; playbackBinding_ = {};
    regionCameras_.clear(); regionObjects_.clear(); regionGeometry_.clear(); areaSettings_.clear(); areaKey_.clear();
    travel_.reset(); rig_ = {}; driverCamera_ = {}; cameraInitialized_ = false; cameraTime_ = 0;
    sceneActive_ = leftHeld_ = rightHeld_ = false;
    focusDistance_ = 0; previewEntryObject_ = 0; previewSpawn_.reset();
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
    if (playback_.IsValid()) (void)world.GetSequence().Stop(playback_);
    playback_ = {}; travel_.reset(); sceneActive_ = false; cameraInitialized_ = false;
}

void GameSession::SyncRegions(HIKARI::WorldView& world) {
    std::vector<Rules::RegionSource> sources;
    bool definitionsValid = true;
    std::string previewEntry;
    regionObjects_.clear();
    regionCameras_.clear();
    regionGeometry_.clear();
    const HIKARI::ComponentTypeId regionTypes[]{HIKARI::MakeComponentTypeId(kRegionComponentType)};
    const auto regions = world.ForEachComponents<RegionComponent>(regionTypes,
        HIKARI::GameProjectWorldQueryAnchor::FirstRequiredTerm,
        [this, &sources, &definitionsValid, &previewEntry](HIKARI::ObjectView object, RegionComponent& component) {
            Rules::RegionSource source; source.placement = component.Placement(); source.entry = component.IsEntryRegion();
            if (object.GetSceneObjectId() == previewEntryObject_) { previewEntry = source.placement.regionId; }
            regionObjects_[source.placement.regionId] = object.GetHandle();
            regionCameras_[source.placement.regionId] = component.CameraObject();
            regionGeometry_[source.placement.regionId] = component.Geometry();
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
        // 不正な入口は既定区域へ黙って落とさず診断する。試遊で別の場所を確認したと誤認させない。
        if (previewEntryObject_ && previewEntry.empty()) {
            std::fprintf(stderr, "[TD2 configuration] test start object missing: %llu\n",
                static_cast<unsigned long long>(previewEntryObject_)); return;
        }
        const auto error = exploration_.Initialize(std::move(sources), previewEntry, previewSpawn_);
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
    SyncCameraAreas(world);
    // Play の始まり：演出の Camera 槽の物体をゲーム用カメラにし、入口の区域机位から追従を始める。
    if (world.GetRuntimeSession().IsPlayActive() && exploration_.Ready() && !cameraInitialized_) {
        InitializeCamera(world);
    }
    std::fflush(stderr);
}

// ---------------------------------------------------------------- 镜头
// 二通りで動かす（方針は TD2CameraRig.h）。
// ・同じ大区域（TD2.CameraArea）の中：CameraRig が区域机位を基準に主角へ向けて毎フレーム解き、ゲーム用カメラへ
//   機位を提出する。部屋を移ると基準が次の区域机位へ滑るだけで、演出も操作の停止も無い（落下・跳躍もそのまま追う）。
// ・大区域どうし：結ばれた引擎の演出を流す。地面を歩いて越えた時は世界を止め、A/D で演出を押し進める（押すと速さが
//   立ち上がり、離すと少し滑って止まり、逆へ押すとまず止まってから戻る）。起点まで戻すと主角も元の区域へ戻る。
//   空中で越えた時は世界を止めずに自動で流す。演出の間は機位を提出せず、始まりの機位のずれは序盤で 0 へ戻す。
//   終わったら演出を止め、同じフレームで最後の画から追従へ戻る（区域机位とずれていても跳ばない）。

bool GameSession::RegionFrame(HIKARI::WorldView& world, std::string_view region, CameraRegionFrame& out) const {
    const auto camera = regionCameras_.find(region);
    const auto tv = regionObjects_.find(region);
    const auto* runtime = exploration_.FindRegion(region);
    if (camera == regionCameras_.end() || tv == regionObjects_.end() || !runtime || !camera->second) return false;
    const auto cameraObject = world.FindSceneObject(camera->second);
    const auto tvObject = world.FindObject(tv->second);
    HIKARI::GameProjectTransform pose{};
    if (!cameraObject.IsValid() || !tvObject.IsValid() || !cameraObject.GetWorldTransform(pose) || !tvObject.GetWorldTransform(out.screen)) {
        return false;
    }
    if (const auto geometry = regionGeometry_.find(region); geometry != regionGeometry_.end()) {
        const auto& g = geometry->second;
        out.screenCenter = {g.centerX, g.centerY, g.centerZ}; out.screenWidth = g.width; out.screenHeight = g.height;
    }
    const auto& placement = runtime->placement;
    out.worldX = placement.worldX; out.worldY = placement.worldY; out.cols = placement.cols; out.rows = placement.rows;
    // 区域机位は局所 +Z を見る。注視点は画面の中心の深さに置く（大区域の中の乗り換えで画面の間を通るように）。
    const auto forward = CameraRig::Forward(pose);
    const auto center = CameraRig::ScreenPoint(out, out.worldX + out.cols * .5, out.worldY + out.rows * .5);
    const auto toCenter = Sub(center, pose.position);
    float depth = toCenter.x * forward.x + toCenter.y * forward.y + toCenter.z * forward.z;
    if (!(depth > .3f)) depth = 3.f;
    out.eye = pose.position; out.look = Mad(pose.position, forward, depth);
    HIKARI::GameProjectCameraLens lens{};
    if (world.GetCamera().GetAuthoredLens(cameraObject.GetHandle().value, lens) && lens.verticalFovRadians > 0) {
        out.verticalFovRadians = lens.verticalFovRadians;
    }
    out.valid = true;
    return true;
}

const CameraAreaSettings& GameSession::AreaOf(std::string_view region) const {
    static const CameraAreaSettings kSingleRoom{};
    const auto found = areaSettings_.find(region);
    return found == areaSettings_.end() ? kSingleRoom : found->second;
}

bool GameSession::SameArea(std::string_view a, std::string_view b) const {
    if (a == b) return true;
    const auto ka = areaKey_.find(a), kb = areaKey_.find(b);
    return ka != areaKey_.end() && kb != areaKey_.end() && ka->second == kb->second;
}

void GameSession::SyncCameraAreas(HIKARI::WorldView& world) {
    areaSettings_.clear(); areaKey_.clear();
    const HIKARI::ComponentTypeId types[]{HIKARI::MakeComponentTypeId(kCameraAreaComponentType)};
    size_t areas = 0;
    (void)world.ForEachComponents<CameraAreaComponent>(types, HIKARI::GameProjectWorldQueryAnchor::FirstRequiredTerm,
        [this, &areas](HIKARI::ObjectView object, CameraAreaComponent& component) {
            const auto& settings = component.Settings();
            const std::string key = std::to_string(object.GetSceneObjectId());
            std::string_view list = settings.regions;
            while (!list.empty()) {
                const auto comma = list.find(',');
                auto item = list.substr(0, comma);
                while (!item.empty() && (item.front() == ' ' || item.front() == '\t')) item.remove_prefix(1);
                while (!item.empty() && (item.back() == ' ' || item.back() == '\t')) item.remove_suffix(1);
                if (!item.empty()) { areaSettings_[std::string(item)] = settings; areaKey_[std::string(item)] = key; }
                list = comma == std::string_view::npos ? std::string_view{} : list.substr(comma + 1);
            }
            ++areas;
        });
    std::fprintf(stderr, "[TD2 camera] areas=%zu regionsInAreas=%zu\n", areas, areaKey_.size());
}

void GameSession::InitializeCamera(HIKARI::WorldView& world) {
    // 演出の Camera 槽の物体（無ければ入口の区域机位）をゲーム用カメラにし、追従の機位はそこへ提出する。
    uint64_t driver = 0;
    for (const auto& binding : bindings_) if (binding.cameraObject) { driver = binding.cameraObject; break; }
    if (!driver) if (const auto found = regionCameras_.find(exploration_.ActiveRegion()); found != regionCameras_.end()) driver = found->second;
    const auto object = world.FindSceneObject(driver);
    if (!object.IsValid() || !world.GetCamera().SetGameplayCamera(object.GetHandle().value)) {
        std::fprintf(stderr, "[TD2 camera] no gameplay camera object=%llu\n", static_cast<unsigned long long>(driver));
        return;
    }
    driverCamera_ = object.GetHandle();
    SnapCamera(world, exploration_.ActiveRegion());
    cameraInitialized_ = true;
}

void GameSession::SnapCamera(HIKARI::WorldView& world, std::string_view region) {
    CameraRegionFrame frame;
    if (RegionFrame(world, region, frame)) rig_.Snap(frame.eye, frame.look, frame.verticalFovRadians);
}

void GameSession::OnCrossing(HIKARI::WorldView& world, const Rules::RegionTransition& transition) {
    const auto& from = transition.fromRegion;
    const auto& to = transition.toRegion;
    if (transition.kind == "respawn") {   // 復活は演出を流さず、その区域机位から追従し直す
        StopTravel(world); SnapCamera(world, to);
        return;
    }
    if (travel_) {
        // 自動の切り替えの途中：来た部屋へ戻ればその場で逆向きにし、先へ進めば終わった後に追従が引き継ぐ。
        if (from == travel_->to && to == travel_->from) {
            travel_->reverse = !travel_->reverse; std::swap(travel_->from, travel_->to); travel_->pending.clear();
            const float speed = std::clamp(travel_->binding.speed, .1f, 8.f);
            (void)world.GetSequence().SetPlaybackRate(playback_, travel_->reverse ? -speed : speed);
            if (!world.GetSequence().IsPlaying(playback_)) (void)world.GetSequence().Resume(playback_);
        } else {
            travel_->pending = to;
        }
        return;
    }
    if (SameArea(from, to)) return;   // 大区域の中：追従の基準が次の区域机位へ滑る
    const auto* a = exploration_.FindRegion(from);
    const auto* b = exploration_.FindRegion(to);
    int direction = 0;
    if (a && b) {
        if (a->placement.worldX + a->placement.cols == b->placement.worldX) direction = 1;
        else if (b->placement.worldX + b->placement.cols == a->placement.worldX) direction = -1;
    }
    // 地面を歩いて横へ越えた時だけ手動（落下・跳躍の途中は止めない）。
    const bool manual = direction != 0 && exploration_.State().player.grounded;
    if (!StartTravel(world, from, to, manual, direction)) {
        std::fprintf(stderr, "[TD2 camera] no transition for %s -> %s; follow glides\n", from.c_str(), to.c_str());
    }
}

bool GameSession::StartTravel(HIKARI::WorldView& world, const std::string& from, const std::string& to, bool manual, int direction) {
    const auto sequence = world.GetSequence();
    const auto found = std::find_if(bindings_.begin(), bindings_.end(), [&](const auto& binding) {
        return (binding.regionA == from && binding.regionB == to) || (binding.regionB == from && binding.regionA == to);
    });
    if (found == bindings_.end() || found->sequenceAssetGuid.empty()) return false;
    const auto& binding = *found;
    const bool reverse = from == binding.regionB;
    const float speed = std::clamp(binding.speed, .1f, 8.f);
    const auto started = PlayBoundSequence(world, binding, reverse ? -speed : speed);
    if (!started.IsValid()) return false;
    HIKARI::GameProjectSequencePlaybackSnapshot snapshot;
    if (!sequence.GetSnapshot(started, snapshot) || !std::isfinite(snapshot.durationSeconds) || snapshot.durationSeconds <= 0 ||
        (reverse && (!sequence.Seek(started, snapshot.durationSeconds) || (!sequence.IsPlaying(started) && !sequence.Resume(started))))) {
        (void)sequence.Stop(started);
        std::fprintf(stderr, "[TD2 camera] transition start failed asset=%s\n", binding.sequenceAssetGuid.c_str());
        return false;
    }
    if (manual) (void)sequence.Pause(started);
    Travel travel;
    travel.binding = binding; travel.from = from; travel.to = to; travel.reverse = reverse; travel.manual = manual;
    travel.direction = direction;
    if (manual) {
        // 歩いて越えた勢いを持ち込む（歩く速さ 5 格/秒で kTravelCarry）。止まっていた主角なら 0 から押し始める。
        const double walk = std::clamp(exploration_.State().player.vx * direction / 5.0, 0.0, 1.0);
        travel.velocity = kTravelCarry * walk * speed / snapshot.durationSeconds;
    }
    // 始まりの機位のずれ（追従の今の画 − 演出の始点 = 区域机位）。序盤で 0 へ戻して繋ぐ。
    CameraRegionFrame frame;
    if (rig_.Ready() && RegionFrame(world, from, frame)) {
        travel.eyeOffset = Sub(rig_.Last().eye, frame.eye); travel.lookOffset = Sub(rig_.Last().look, frame.look);
    }
    travel_ = travel; playback_ = started; playbackBinding_ = binding;
    std::fprintf(stderr, "[TD2 camera] travel %s -> %s manual=%d reverse=%d duration=%.3f speed=%.3f\n", from.c_str(), to.c_str(),
        manual, reverse, snapshot.durationSeconds, speed);
    return true;
}

void GameSession::UpdateTravel(HIKARI::WorldView& world, float input, double dt) {
    auto& travel = *travel_;
    const auto sequence = world.GetSequence();
    HIKARI::GameProjectSequencePlaybackSnapshot snapshot;
    if (!sequence.GetSnapshot(playback_, snapshot) || !(snapshot.durationSeconds > 0)) { FinishTravel(world, travel.to); return; }
    const double raw = std::clamp(static_cast<double>(snapshot.timeSeconds) / snapshot.durationSeconds, 0.0, 1.0);
    travel.progress = travel.reverse ? 1 - raw : raw;
    if (travel.manual) {
        // 押した向き（越えた向きが前）。全速は演出の作者の倍速（binding.speed）と同じ進み。
        const double along = std::clamp(static_cast<double>(input) * travel.direction, -1.0, 1.0);
        const double full = std::clamp(travel.binding.speed, .1f, 8.f) / snapshot.durationSeconds;
        const double target = std::abs(along) > .1 ? along * full : 0.0;
        double seconds = kTravelCoastSeconds;
        if (target != 0 && travel.velocity * target < 0) seconds = kTravelBrakeSeconds;
        else if (target != 0) seconds = kTravelPressSeconds;
        travel.velocity = Damp(travel.velocity, target, travel.accel, seconds, dt);
        if (target == 0 && std::abs(travel.velocity) < full * 2e-3) { travel.velocity = 0; travel.accel = 0; }
        if (travel.progress >= 1 - 1e-3 && travel.velocity >= 0 && target >= 0) { FinishTravel(world, travel.to); return; }
        if (travel.progress <= 1e-3 && travel.velocity <= 0 && target < 0) {
            // 起点まで戻した：主角を元の区域へ戻して、そこから追従を続ける。
            const auto from = travel.from; const int direction = travel.direction;
            (void)exploration_.StepBack(from, direction);
            FinishTravel(world, from);
            return;
        }
        // 速さを演出の再生倍率へ写す。速さ 0 は一時停止（画はその場に留まる）。
        const double rate = travel.velocity * snapshot.durationSeconds * (travel.reverse ? -1 : 1);
        if (std::abs(travel.velocity) < 1e-4) { if (sequence.IsPlaying(playback_)) (void)sequence.Pause(playback_); }
        else {
            (void)sequence.SetPlaybackRate(playback_, static_cast<float>(rate));
            if (!sequence.IsPlaying(playback_)) (void)sequence.Resume(playback_);
        }
    } else if (travel.progress >= 1 - 1e-3 || snapshot.state != HIKARI::GameProjectSequencePlaybackState::Playing) {
        FinishTravel(world, travel.to);
        return;
    }
    // 始まりの機位のずれを序盤で 0 へ戻し、追従の呼吸を演出の間も重ねる（止まった画にしない）。
    const float weight = static_cast<float>(1 - SmoothStep(travel.progress / kTravelBlendIn));
    HIKARI::GameProjectFloat3 breathEye{}, breathLook{};
    double handRoll = 0;
    // 揺れの向きは描いている镜头（前のフレーム）の向き。描いた画に重ねたずれを引いた素の演出の机位で測る。
    HIKARI::GameProjectCameraPose rendered{};
    if (world.GetCamera().GetRenderedPose(rendered)) {
        CameraRig::BreathAround(AreaOf(travel.to), cameraTime_, Mad(rendered.eye, travel.appliedEye, -1.f),
                                Mad(rendered.target, travel.appliedLook, -1.f), breathEye, breathLook, handRoll);
    } else {
        rig_.Breath(AreaOf(travel.to), cameraTime_, breathEye, breathLook, handRoll);
    }
    HIKARI::GameProjectCameraModifier modifier{};
    modifier.sourceId = kTravelSource; modifier.priority = 50; modifier.applyDuringOverride = 1;
    modifier.worldPositionOffset = Mad(breathEye, travel.eyeOffset, weight);
    modifier.worldTargetOffset = Mad(breathLook, travel.lookOffset, weight);
    (void)world.GetCamera().SubmitModifier(modifier);
    travel.appliedEye = modifier.worldPositionOffset; travel.appliedLook = modifier.worldTargetOffset;
    HIKARI::GameProjectCameraRollModifier tilt{};
    tilt.sourceId = kTravelSource; tilt.priority = 50; tilt.applyDuringOverride = 1;
    tilt.localRollRadians = static_cast<float>(handRoll);
    (void)world.GetCamera().SubmitRollModifier(tilt);
    (void)dt;
}

void GameSession::FinishTravel(HIKARI::WorldView& world, const std::string& arrived) {
    const std::string region = arrived;   // travel_ の中の文字列を指していることがあるので、消す前に写す
    HIKARI::GameProjectCameraPose rendered{};
    const bool haveRendered = world.GetCamera().GetRenderedPose(rendered);
    const auto pending = travel_ ? travel_->pending : std::string{};
    const auto appliedEye = travel_ ? travel_->appliedEye : HIKARI::GameProjectFloat3{};
    const auto appliedLook = travel_ ? travel_->appliedLook : HIKARI::GameProjectFloat3{};
    if (playback_.IsValid()) (void)world.GetSequence().Stop(playback_);
    playback_ = {}; playbackBinding_ = {}; travel_.reset();
    // 演出の終わりの画からそのまま追従を始める（区域机位との差は追従がなだらかに詰める）。描いた画には演出へ重ねた
    // 呼吸・手持ちのずれが入っていて、追従も自分で同じ揺れを足すので、基準はそのずれを差し引いた所に置く（足し直すと
    // 最初のフレームに数 cm・0.5° ほど跳ぶ）。
    if (haveRendered) rig_.Snap(Mad(rendered.eye, appliedEye, -1.f), Mad(rendered.target, appliedLook, -1.f), rendered.verticalFovRadians);
    else SnapCamera(world, region);
    std::fprintf(stderr, "[TD2 camera] travel finished at %s\n", region.c_str());
    if (!pending.empty() && pending != region && !SameArea(region, pending)) (void)StartTravel(world, region, pending, false, 0);
}

void GameSession::StopTravel(HIKARI::WorldView& world) {
    if (playback_.IsValid()) (void)world.GetSequence().Stop(playback_);
    playback_ = {}; playbackBinding_ = {}; travel_.reset();
}

void GameSession::UpdateFollow(HIKARI::WorldView& world, double dt) {
    if (!driverCamera_.IsValid()) return;
    const auto& region = exploration_.ActiveRegion();
    CameraRegionFrame frame;
    if (!RegionFrame(world, region, frame)) return;
    const auto& state = exploration_.State();
    const auto& body = state.player.rect;
    const auto target = CameraRig::ScreenPoint(frame, body.x + body.w * .5, body.y + body.h * .5);
    const auto velocity = CameraRig::ScreenVelocity(frame, state.player.vx, state.player.vy);
    const auto pose = rig_.Update(frame, AreaOf(region), target, velocity, state.playerPresent && !state.player.dead, dt, cameraTime_);
    HIKARI::GameProjectCameraPoseRequest request{};
    request.cameraObjectHandle = driverCamera_.value; request.sourceId = kFollowSource; request.priority = 50;
    request.pose.eye = pose.eye; request.pose.target = pose.look; request.pose.up = pose.up;
    request.pose.verticalFovRadians = pose.verticalFovRadians;
    (void)world.GetCamera().SubmitPose(request);
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

// 被写界深度の焦点を今の部屋の電視の画面へ合わせ続ける。機位ごとに画面までの距離が違い（2.1〜3.9 m）、
// カメラに作者が付けた固定の焦点では遠い電視がぼけるため。焦点は画面の中心の視線方向の深さ（画面の位置は区域の
// 「屏幕中心」）。部屋が変わった瞬間に跳ばないよう、時定数 0.12 秒で寄せる。
void GameSession::FocusActiveScreen(HIKARI::WorldView& world, double seconds) {
    // 焦点は「画で一番目立っている画面」へ、ゆっくり送る（操作中の区域が越えた瞬間に切り替わると、まだ画の主役の
    // 前の画面が急にぼける）。目立ち具合は写る面積を、画の中心から離れるほど割り引いた値。
    constexpr double kFocusSeconds = .35;
    constexpr uint64_t kFocusSource = 0x5444325F464F4355ull; // "TD2_FOCU"
    auto camera = world.GetCamera();
    HIKARI::GameProjectCameraPose pose{};
    if (!camera.GetRenderedPose(pose)) { return; }
    double best = 0, depth = 0, travelWeight = 0, travelDepth = 0;
    std::string focusRegion;
    for (const auto& [region, handle] : regionObjects_) {
        // 切り替えの演出の間は、出る部屋と入る部屋の画面だけを見る。镜头が大きく回る途中で遠くの画面が一時的に一番
        // 目立っても焦点を飛ばさない（飛ぶと主役の二台がぼけたり戻ったりする）。
        const bool travelRoom = travel_ && (region == travel_->from || region == travel_->to);
        if (travel_ && !travelRoom) { continue; }
        const auto geometry = regionGeometry_.find(region);
        HIKARI::GameProjectTransform tv{};
        const auto object = world.FindObject(handle);
        if (geometry == regionGeometry_.end() || !object.IsValid() || !object.GetWorldTransform(tv)) { continue; }
        const auto seen = ProjectScreen(pose, tv, geometry->second);
        const double score = seen.coverage / (1 + 1.5 * (seen.centerX * seen.centerX + seen.centerY * seen.centerY));
        if (seen.depth <= .1) { continue; }
        if (score > best) { best = score; depth = seen.depth; focusRegion = region; }
        if (travelRoom) { travelWeight += score; travelDepth += score * seen.depth; }
    }
    // 二台の奥行きを目立ち具合で混ぜる：どちらか一台へ切り替えず、写り方に合わせて連続に移す。
    if (travel_ && travelWeight > 0) { depth = travelDepth / travelWeight; }
    // 診断用（環境変数 TD2_SCREEN_TRACE）：焦点を合わせる画面が替わったフレームを記録する。
    static const bool trace = std::getenv("TD2_SCREEN_TRACE") != nullptr;
    if (trace && focusRegion != traceFocus_) {
        std::fprintf(stderr, "[TD2 focus-trace] region=%s depth=%.3f travel=%.3f\n", focusRegion.c_str(), depth, travel_ ? travel_->progress : -1.0);
        traceFocus_ = focusRegion;
    }
    if (!(best > 0)) {
        // 画面が一つも写っていない時（大きく引いた演出の途中など）は、操作中の区域の画面の奥行きを保つ。
        CameraRegionFrame frame;
        if (!RegionFrame(world, exploration_.ActiveRegion(), frame)) { return; }
        const auto center = CameraRig::ScreenPoint(frame, frame.worldX + frame.cols * .5, frame.worldY + frame.rows * .5);
        const double fx = pose.target.x - pose.eye.x, fy = pose.target.y - pose.eye.y, fz = pose.target.z - pose.eye.z;
        const double length = std::sqrt(fx * fx + fy * fy + fz * fz);
        if (length < 1e-6) { return; }
        depth = ((center.x - pose.eye.x) * fx + (center.y - pose.eye.y) * fy + (center.z - pose.eye.z) * fz) / length;
        if (!(depth > .1)) { return; }
    }
    focusDistance_ = focusDistance_ > 0 ? focusDistance_ + (depth - focusDistance_) * (1 - std::exp(-seconds / kFocusSeconds)) : depth;
    HIKARI::GameProjectCameraFocusRequest focus{};
    focus.sourceId = kFocusSource; focus.priority = 100;
    focus.mode = HIKARI::GameProjectCameraFocusMode::Distance;
    focus.distance = static_cast<float>(focusDistance_);
    (void)camera.SubmitFocus(focus);
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
    // 描いた机位も残す（演出の道筋を作者の鍵と突き合わせるため）。
    HIKARI::GameProjectCameraPose pose{};
    (void)world.GetCamera().GetRenderedPose(pose);
    std::fprintf(stderr, "[TD2 sequence-state] available=%d playing=%d state=%u time=%.4f duration=%.4f asset=%s frame=%llu "
        "eye=(%.3f,%.3f,%.3f) target=(%.3f,%.3f,%.3f)\n",
        available, sequence.IsPlaying(playback_), static_cast<unsigned>(snapshot.state), snapshot.timeSeconds, snapshot.durationSeconds,
        snapshot.assetGuid, static_cast<unsigned long long>(frame), pose.eye.x, pose.eye.y, pose.eye.z, pose.target.x, pose.target.y,
        pose.target.z);
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
    double remaining = travel_ && travel_->manual ? 0 : frameSeconds;   // 手動で演出を押している間は世界を止める
    cameraTime_ += frameSeconds;
    if (!cameraInitialized_) InitializeCamera(world);
    while (remaining > 1e-12) {
        const double slice = std::min(remaining, 1.0 / 120);
        const auto output = exploration_.Step(buttons, slice);
        for (const auto& region : output.regions) TraceEvents(region.regionId, region.output);
        for (const auto& transition : output.transitions) {
            std::fprintf(stderr, "[TD2 crossing] kind=%s from=%s to=%s world=(%.5f,%.5f) velocity=(%.5f,%.5f) frame=%llu\n",
                transition.kind.c_str(), transition.fromRegion.c_str(), transition.toRegion.c_str(), transition.worldPosition.x, transition.worldPosition.y,
                exploration_.State().player.vx, exploration_.State().player.vy, static_cast<unsigned long long>(frame.frameIndex));
            OnCrossing(world, transition);
        }
        buttons.action.pressed = buttons.action.released = false; remaining -= slice;
    }
    if (travel_) UpdateTravel(world, moveValid ? move.x : 0.f, frameSeconds);
    if (!travel_) UpdateFollow(world, frameSeconds);
    FocusActiveScreen(world, std::isfinite(frame.gameDt) ? std::max(0.0, double(frame.gameDt)) : 0.0);
    traceSeconds_ += std::max(0.0, double(frame.gameDt));
    if (traceSeconds_ >= 1) {
        traceSeconds_ = 0;
        const auto& state = exploration_.State();
        std::fprintf(stderr, "[TD2 world-state] region=%s tick=%llu sim=%.4f real=%.4f player=(%.5f,%.5f) velocity=(%.5f,%.5f) echo=%d echoPos=(%.5f,%.5f) deaths=%llu checkpoint=%s travel=%s progress=%.3f frame=%llu\n",
            exploration_.ActiveRegion().c_str(), static_cast<unsigned long long>(state.tick), state.simulationTime, state.realTime,
            state.player.rect.x, state.player.rect.y, state.player.vx, state.player.vy, state.echo.has_value(), state.echo ? state.echo->rect.x : 0,
            state.echo ? state.echo->rect.y : 0, static_cast<unsigned long long>(state.deaths), state.checkpointId.c_str(),
            travel_ ? (travel_->from + ">" + travel_->to).c_str() : "-", travel_ ? travel_->progress : 0.0,
            static_cast<unsigned long long>(frame.frameIndex));
        const auto prefix = exploration_.ActiveRegion() + "/";
        for (const auto& device : exploration_.Snapshot().devices) if (device.id.starts_with(prefix))
            std::fprintf(stderr, "[TD2 device] region=%s id=%s active=%d pressed=%d solid=%d\n", exploration_.ActiveRegion().c_str(),
                device.id.c_str() + prefix.size(), device.active, device.pressed, device.solid);
        TraceSequence(world, frame.frameIndex);
        std::fflush(stderr);
    }
}

} // namespace TD2
