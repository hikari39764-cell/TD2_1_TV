#include "TD2CameraDirector.h"
#include "TD2Transform.h"
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
// 手動の切り替えの重さ（ばねの時定数、秒）：加速、離した時の減速、逆に押した時の制動。
// 経路自体が両端で緩やかに止まるので、端の手前で別に減速しない。
constexpr double kTravelPressSeconds = .7, kTravelCoastSeconds = 1.8, kTravelBrakeSeconds = .55;
// 歩いて越えた時に持ち込む勢い（全速に対する割合）。
constexpr double kTravelCarry = .55;
// 交換の直後だけ区域カメラの乗り換えをこの秒数まで速める（その状態をこの秒数続ける）。
constexpr float kSwapRegionSeconds = .38f;
constexpr double kSwapBoostSeconds = 1.0;
// 演出の序盤のこの割合までに、開始時のカメラのずれを 0 に戻す。
constexpr double kTravelBlendIn = .35;
constexpr uint64_t kFollowSource = 0x5444325F464F4C4Cull;   // 識別値 "TD2_FOLL"
constexpr uint64_t kTravelSource = 0x5444325F5452564Cull;   // 識別値 "TD2_TRVL"
constexpr uint64_t kFocusSource = 0x5444325F464F4355ull;    // 識別値 "TD2_FOCU"
}

void CameraDirector::Stop(HIKARI::WorldView& world) {
    if (playback_.IsValid()) (void)world.GetSequence().Stop(playback_);
    playback_ = {}; travel_.reset(); initialized_ = false;
}

void CameraDirector::SyncBindings(HIKARI::WorldView& world) {
    std::vector<RegionSequenceBinding> authored;
    const HIKARI::ComponentTypeId types[]{HIKARI::MakeComponentTypeId(kRegionSequenceComponentType)};
    const auto result = world.ForEachComponents<RegionSequenceComponent>(types, HIKARI::GameProjectWorldQueryAnchor::FirstRequiredTerm,
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
}

void CameraDirector::SyncAreas(HIKARI::WorldView& world) {
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

bool CameraDirector::TravelRooms(std::string& from, std::string& to) const {
    if (!travel_) { return false; }
    from = travel_->from; to = travel_->to; return true;
}

std::vector<std::string> CameraDirector::AreaRooms(std::string_view region) const {
    const auto key = areaKey_.find(region);
    if (key == areaKey_.end()) { return {std::string(region)}; }
    std::vector<std::string> rooms;
    for (const auto& [id, value] : areaKey_) { if (value == key->second) { rooms.push_back(id); } }
    return rooms;
}

bool CameraDirector::RegionFrame(HIKARI::WorldView& world, const Rules::Exploration& exploration, const RegionObjects& objects,
                                 std::string_view region, CameraRegionFrame& out) const {
    const auto camera = objects.cameras.find(region);
    const auto tv = objects.televisions.find(region);
    const auto* runtime = exploration.FindRegion(region);
    if (camera == objects.cameras.end() || tv == objects.televisions.end() || !runtime || !camera->second) return false;
    const auto cameraObject = world.FindSceneObject(camera->second);
    const auto tvObject = world.FindObject(tv->second);
    HIKARI::GameProjectTransform pose{};
    if (!cameraObject.IsValid() || !tvObject.IsValid() || !cameraObject.GetWorldTransform(pose) || !tvObject.GetWorldTransform(out.screen)) {
        return false;
    }
    if (const auto geometry = objects.geometry.find(region); geometry != objects.geometry.end()) {
        const auto& g = geometry->second;
        out.screenCenter = {g.centerX, g.centerY, g.centerZ}; out.screenWidth = g.width; out.screenHeight = g.height;
    }
    const auto& placement = runtime->placement;
    out.worldX = placement.worldX; out.worldY = placement.worldY; out.cols = placement.cols; out.rows = placement.rows;
    // 区域カメラはローカル +Z を見る。注視点は画面の中心の深さに置く。
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

const CameraAreaSettings& CameraDirector::AreaOf(std::string_view region) const {
    static const CameraAreaSettings kSingleRoom{};
    const auto found = areaSettings_.find(region);
    return found == areaSettings_.end() ? kSingleRoom : found->second;
}

bool CameraDirector::SameArea(std::string_view a, std::string_view b) const {
    if (a == b) return true;
    const auto ka = areaKey_.find(a), kb = areaKey_.find(b);
    return ka != areaKey_.end() && kb != areaKey_.end() && ka->second == kb->second;
}

void CameraDirector::Initialize(HIKARI::WorldView& world, const Rules::Exploration& exploration, const RegionObjects& objects) {
    uint64_t driver = 0;
    for (const auto& binding : bindings_) if (binding.cameraObject) { driver = binding.cameraObject; break; }
    if (!driver) if (const auto found = objects.cameras.find(exploration.ActiveRegion()); found != objects.cameras.end()) driver = found->second;
    const auto object = world.FindSceneObject(driver);
    if (!object.IsValid() || !world.GetCamera().SetGameplayCamera(object.GetHandle().value)) {
        std::fprintf(stderr, "[TD2 camera] no gameplay camera object=%llu\n", static_cast<unsigned long long>(driver));
        return;
    }
    driver_ = object.GetHandle();
    Snap(world, exploration, objects, exploration.ActiveRegion());
    initialized_ = true;
}

void CameraDirector::Snap(HIKARI::WorldView& world, const Rules::Exploration& exploration, const RegionObjects& objects, std::string_view region) {
    CameraRegionFrame frame;
    if (RegionFrame(world, exploration, objects, region, frame)) rig_.Snap(frame.eye, frame.look, frame.verticalFovRadians);
}

void CameraDirector::OnCrossing(HIKARI::WorldView& world, Rules::Exploration& exploration, const RegionObjects& objects,
                                const Rules::RegionTransition& transition) {
    const auto& from = transition.fromRegion;
    const auto& to = transition.toRegion;
    if (transition.kind == "respawn") {   // 復活は演出を流さず、その区域カメラから追従し直す
        StopTravel(world); Snap(world, exploration, objects, to);
        return;
    }
    if (transition.kind == "swap") {      // 交換は演出を流さず、追従が新しい区域カメラへ速めに滑って行く
        if (travel_) StopTravel(world);
        swapBoost_ = kSwapBoostSeconds;
        return;
    }
    if (travel_) {
        // 自動の切り替え中：来た部屋へ戻れば逆再生にし、先へ進めば終了後に追従が引き継ぐ。
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
    if (SameArea(from, to)) return;   // 大区域の中：追従の基準が次の区域カメラへ移る
    exploration.ClearEcho();          // 大区域を出たら残影は置いて行かない
    const auto* a = exploration.FindRegion(from);
    const auto* b = exploration.FindRegion(to);
    int direction = 0;
    if (a && b) {
        if (a->placement.worldX + a->placement.cols == b->placement.worldX) direction = 1;
        else if (b->placement.worldX + b->placement.cols == a->placement.worldX) direction = -1;
    }
    // 地面を歩いて横へ越えた時だけ手動（落下中・跳躍中は止めない）。
    const bool manual = direction != 0 && exploration.State().player.grounded;
    if (!StartTravel(world, exploration, objects, from, to, manual, direction)) {
        std::fprintf(stderr, "[TD2 camera] no transition for %s -> %s; follow glides\n", from.c_str(), to.c_str());
    }
}

bool CameraDirector::StartTravel(HIKARI::WorldView& world, const Rules::Exploration& exploration, const RegionObjects& objects,
                                 const std::string& from, const std::string& to, bool manual, int direction) {
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
        // 歩いて越えた勢いを持ち込む。止まっていたら 0 から押し始める。
        const double walk = std::clamp(exploration.State().player.vx * direction / 5.0, 0.0, 1.0);
        travel.velocity = kTravelCarry * walk * speed / snapshot.durationSeconds;
    }
    // 開始時のカメラのずれ（今の追従の画 − 演出の始点）。序盤で 0 に戻す。
    CameraRegionFrame frame;
    if (rig_.Ready() && RegionFrame(world, exploration, objects, from, frame)) {
        travel.eyeOffset = Sub(rig_.Last().eye, frame.eye); travel.lookOffset = Sub(rig_.Last().look, frame.look);
    }
    travel_ = travel; playback_ = started;
    std::fprintf(stderr, "[TD2 camera] travel %s -> %s manual=%d reverse=%d duration=%.3f speed=%.3f\n", from.c_str(), to.c_str(),
        manual, reverse, snapshot.durationSeconds, speed);
    return true;
}

void CameraDirector::UpdateTravel(HIKARI::WorldView& world, Rules::Exploration& exploration, const RegionObjects& objects, float input, double dt) {
    auto& travel = *travel_;
    const auto sequence = world.GetSequence();
    HIKARI::GameProjectSequencePlaybackSnapshot snapshot;
    if (!sequence.GetSnapshot(playback_, snapshot) || !(snapshot.durationSeconds > 0)) { FinishTravel(world, exploration, objects, travel.to); return; }
    const double raw = std::clamp(static_cast<double>(snapshot.timeSeconds) / snapshot.durationSeconds, 0.0, 1.0);
    travel.progress = travel.reverse ? 1 - raw : raw;
    if (travel.manual) {
        // 押した向き（越えた向きが前）。全速は演出の倍速（binding.speed）と同じ。
        const double along = std::clamp(static_cast<double>(input) * travel.direction, -1.0, 1.0);
        const double full = std::clamp(travel.binding.speed, .1f, 8.f) / snapshot.durationSeconds;
        const double target = std::abs(along) > .1 ? along * full : 0.0;
        double seconds = kTravelCoastSeconds;
        if (target != 0 && travel.velocity * target < 0) seconds = kTravelBrakeSeconds;
        else if (target != 0) seconds = kTravelPressSeconds;
        travel.velocity = Math::CriticalDamp(travel.velocity, target, travel.accel, seconds, dt);
        if (target == 0 && std::abs(travel.velocity) < full * 2e-3) { travel.velocity = 0; travel.accel = 0; }
        if (travel.progress >= 1 - 1e-3 && travel.velocity >= 0 && target >= 0) { FinishTravel(world, exploration, objects, travel.to); return; }
        if (travel.progress <= 1e-3 && travel.velocity <= 0 && target < 0) {
            // 起点まで戻した：プレイヤーを元の区域へ戻し、そこから追従を続ける。
            const auto from = travel.from; const int direction = travel.direction;
            (void)exploration.StepBack(from, direction);
            FinishTravel(world, exploration, objects, from);
            return;
        }
        // 速さを演出の再生倍率にする。0 は一時停止。
        const double rate = travel.velocity * snapshot.durationSeconds * (travel.reverse ? -1 : 1);
        if (std::abs(travel.velocity) < 1e-4) { if (sequence.IsPlaying(playback_)) (void)sequence.Pause(playback_); }
        else {
            (void)sequence.SetPlaybackRate(playback_, static_cast<float>(rate));
            if (!sequence.IsPlaying(playback_)) (void)sequence.Resume(playback_);
        }
    } else if (travel.progress >= 1 - 1e-3 || snapshot.state != HIKARI::GameProjectSequencePlaybackState::Playing) {
        FinishTravel(world, exploration, objects, travel.to);
        return;
    }
    // 開始時のずれを序盤で 0 に戻し、演出中も呼吸を重ねる。
    const float weight = static_cast<float>(1 - Math::SmoothStep(travel.progress / kTravelBlendIn));
    HIKARI::GameProjectFloat3 breathEye{}, breathLook{};
    double handRoll = 0;
    // 揺れの向きは描いているカメラ（前のフレーム）の向きから、足したずれを引いて測る。
    HIKARI::GameProjectCameraPose rendered{};
    if (world.GetCamera().GetRenderedPose(rendered)) {
        CameraRig::BreathAround(AreaOf(travel.to), time_, Mad(rendered.eye, travel.appliedEye, -1.f),
                                Mad(rendered.target, travel.appliedLook, -1.f), breathEye, breathLook, handRoll);
    } else {
        rig_.Breath(AreaOf(travel.to), time_, breathEye, breathLook, handRoll);
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
}

void CameraDirector::FinishTravel(HIKARI::WorldView& world, const Rules::Exploration& exploration, const RegionObjects& objects,
                                  const std::string& arrived) {
    const std::string region = arrived;   // travel_ の中の文字列を指していることがあるので、消す前に写す
    HIKARI::GameProjectCameraPose rendered{};
    const bool haveRendered = world.GetCamera().GetRenderedPose(rendered);
    const auto pending = travel_ ? travel_->pending : std::string{};
    const auto appliedEye = travel_ ? travel_->appliedEye : HIKARI::GameProjectFloat3{};
    const auto appliedLook = travel_ ? travel_->appliedLook : HIKARI::GameProjectFloat3{};
    StopTravel(world);
    // 演出の終わりの画からそのまま追従を始める。描いた画には演出へ足した揺れが入っており、追従も同じ揺れを足すので、
    // 基準はそのずれを引いた所に置く（引かないと最初のフレームで少し跳ぶ）。
    if (haveRendered) rig_.Snap(Mad(rendered.eye, appliedEye, -1.f), Mad(rendered.target, appliedLook, -1.f), rendered.verticalFovRadians);
    else Snap(world, exploration, objects, region);
    std::fprintf(stderr, "[TD2 camera] travel finished at %s\n", region.c_str());
    if (!pending.empty() && pending != region && !SameArea(region, pending)) (void)StartTravel(world, exploration, objects, region, pending, false, 0);
}

void CameraDirector::StopTravel(HIKARI::WorldView& world) {
    if (playback_.IsValid()) (void)world.GetSequence().Stop(playback_);
    playback_ = {}; travel_.reset();
}

void CameraDirector::UpdateFollow(HIKARI::WorldView& world, const Rules::Exploration& exploration, const RegionObjects& objects, double dt) {
    if (!driver_.IsValid()) return;
    const auto& region = exploration.ActiveRegion();
    CameraRegionFrame frame;
    if (!RegionFrame(world, exploration, objects, region, frame)) return;
    const auto& state = exploration.State();
    const auto& body = state.player.rect;
    const auto target = CameraRig::ScreenPoint(frame, body.x + body.w * .5, body.y + body.h * .5);
    const auto velocity = CameraRig::ScreenVelocity(frame, state.player.vx, state.player.vy);
    // 交換の直後は区域カメラの乗り換えを速くする（本体が一瞬で移ったのに画が遅れて付いて来ないように）。
    auto area = AreaOf(region);
    if (swapBoost_ > 0) { area.regionSeconds = (std::min)(area.regionSeconds, kSwapRegionSeconds); swapBoost_ -= dt; }
    const auto pose = rig_.Update(frame, area, target, velocity, !state.player.dead, dt, time_);
    HIKARI::GameProjectCameraPoseRequest request{};
    request.cameraObjectHandle = driver_.value; request.sourceId = kFollowSource; request.priority = 50;
    request.pose.eye = pose.eye; request.pose.target = pose.look; request.pose.up = pose.up;
    request.pose.verticalFovRadians = pose.verticalFovRadians;
    (void)world.GetCamera().SubmitPose(request);
}

void CameraDirector::Update(HIKARI::WorldView& world, Rules::Exploration& exploration, const RegionObjects& objects, float input,
                            double frameSeconds, double focusSeconds) {
    if (travel_) UpdateTravel(world, exploration, objects, input, frameSeconds);
    if (!travel_) UpdateFollow(world, exploration, objects, frameSeconds);
    Focus(world, exploration, objects, focusSeconds);
}

HIKARI::GameProjectSequencePlaybackHandle CameraDirector::PlayBoundSequence(HIKARI::WorldView& world, const RegionSequenceBinding& binding, float rate) {
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

// 被写界深度の焦点を、画で一番目立つ画面の奥行きへゆっくり送る。目立ち具合は写る面積を、画の中心から離れるほど割り引いた値。
// 候補は今いる大区域の画面だけ（隙間の奥に写る遠い箱庭の画面へ焦点が逃げない）。
void CameraDirector::Focus(HIKARI::WorldView& world, const Rules::Exploration& exploration, const RegionObjects& objects, double seconds) {
    constexpr double kFocusSeconds = .35;
    auto camera = world.GetCamera();
    HIKARI::GameProjectCameraPose pose{};
    if (!camera.GetRenderedPose(pose)) { return; }
    double best = 0, depth = 0, travelWeight = 0, travelDepth = 0;
    std::string focusRegion;
    for (const auto& [region, handle] : objects.televisions) {
        // 切り替え中は出る部屋と入る部屋の画面だけを見る（途中で遠くの画面に焦点が飛ばないように）。
        const bool travelRoom = travel_ && (region == travel_->from || region == travel_->to);
        if (travel_ && !travelRoom) { continue; }
        if (!travel_ && !SameArea(region, exploration.ActiveRegion())) { continue; }
        const auto geometry = objects.geometry.find(region);
        HIKARI::GameProjectTransform tv{};
        const auto object = world.FindObject(handle);
        if (geometry == objects.geometry.end() || !object.IsValid() || !object.GetWorldTransform(tv)) { continue; }
        const auto seen = ProjectScreen(pose, tv, geometry->second);
        const double score = seen.coverage / (1 + 1.5 * (seen.centerX * seen.centerX + seen.centerY * seen.centerY));
        if (seen.depth <= .1) { continue; }
        if (score > best) { best = score; depth = seen.depth; focusRegion = region; }
        if (travelRoom) { travelWeight += score; travelDepth += score * seen.depth; }
    }
    // 二台の奥行きを目立ち具合で混ぜ、連続に移す。
    if (travel_ && travelWeight > 0) { depth = travelDepth / travelWeight; }
    // 診断用（環境変数 TD2_SCREEN_TRACE）：焦点の画面が替わったフレームを記録する。
    static const bool trace = std::getenv("TD2_SCREEN_TRACE") != nullptr;
    if (trace && focusRegion != traceFocus_) {
        std::fprintf(stderr, "[TD2 focus-trace] region=%s depth=%.3f travel=%.3f\n", focusRegion.c_str(), depth, travel_ ? travel_->progress : -1.0);
        traceFocus_ = focusRegion;
    }
    if (!(best > 0)) {
        // 画面が一つも写っていない時は、操作中の区域の画面の奥行きを保つ。
        CameraRegionFrame frame;
        if (!RegionFrame(world, exploration, objects, exploration.ActiveRegion(), frame)) { return; }
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

void CameraDirector::TraceSequence(HIKARI::WorldView& world, uint64_t frame) const {
    HIKARI::GameProjectSequencePlaybackSnapshot snapshot;
    const auto sequence = world.GetSequence(); const bool available = sequence.GetSnapshot(playback_, snapshot);
    // 描いたカメラの姿勢も記録する。
    HIKARI::GameProjectCameraPose pose{};
    (void)world.GetCamera().GetRenderedPose(pose);
    std::fprintf(stderr, "[TD2 sequence-state] available=%d playing=%d state=%u time=%.4f duration=%.4f asset=%s frame=%llu "
        "eye=(%.3f,%.3f,%.3f) target=(%.3f,%.3f,%.3f)\n",
        available, sequence.IsPlaying(playback_), static_cast<unsigned>(snapshot.state), snapshot.timeSeconds, snapshot.durationSeconds,
        snapshot.assetGuid, static_cast<unsigned long long>(frame), pose.eye.x, pose.eye.y, pose.eye.z, pose.target.x, pose.target.y,
        pose.target.z);
}
}
