#include "TD2GameSession.h"
#include "../Common/TD2PreviewEntry.h"
#include "TD2TravelComponents.h"
#include "TD2Transform.h"
#include "TD2SignalCable.h"
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
    case E::EchoSwapped: return "EchoSwapped";
    }
    return "Unknown";
}
// 手動の切り替えの重さ（ばねの時定数、秒）：加速、離した時の減速、逆に押した時の制動。
// 経路自体が両端で緩やかに止まるので、端の手前で別に減速しない。
constexpr double kTravelPressSeconds = .7, kTravelCoastSeconds = 1.8, kTravelBrakeSeconds = .55;
// 歩いて越えた時に持ち込む勢い（全速に対する割合）。
constexpr double kTravelCarry = .55;
// 大きく回る転台で、カメラがテレビと一緒に回り込む割合（切り替えの前後で同じ向きに動き続ける）。
constexpr float kTurnOrbit = .24f;
// 交換の直後だけ区域カメラの乗り換えをこの秒数まで速める（その状態をこの秒数続ける）。
constexpr float kSwapRegionSeconds = .38f;
constexpr double kSwapBoostSeconds = 1.0;
// 演出の序盤のこの割合までに、開始時のカメラのずれを 0 に戻す。
constexpr double kTravelBlendIn = .35;
constexpr uint64_t kFollowSource = 0x5444325F464F4C4Cull;   // 識別値 "TD2_FOLL"
constexpr uint64_t kTravelSource = 0x5444325F5452564Cull;   // 識別値 "TD2_TRVL"
// スカラーの臨界減衰のばね。変化の速さも連続するので、動き出しと止まり際が滑らか。
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
}

void GameSession::OnGameStarted(HIKARI::WorldView&) {
    exploration_ = {}; bindings_.clear(); playback_ = {}; playbackBinding_ = {};
    regionCameras_.clear(); regionObjects_.clear(); regionGeometry_.clear(); areaSettings_.clear(); areaKey_.clear();
    mechanismViews_.clear(); movedPose_.clear();
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
        // 不正な入口は既定の区域へ落とさずエラーにする。
        if (previewEntryObject_ && previewEntry.empty()) {
            std::fprintf(stderr, "[TD2 configuration] test start object missing: %llu\n",
                static_cast<unsigned long long>(previewEntryObject_)); return;
        }
        std::vector<Rules::MechanismSource> mechanisms;
        SyncMechanisms(world, mechanisms);
        std::vector<Rules::SlideSource> slides;
        SyncSlides(world, slides);
        SyncCables(world);
        const auto error = exploration_.Initialize(std::move(sources), std::move(mechanisms), std::move(slides), previewEntry, previewSpawn_);
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
    // Play の開始：演出の Camera スロットの物体をゲームカメラにし、入口の区域カメラから追従を始める。
    if (world.GetRuntimeSession().IsPlayActive() && exploration_.Ready() && !cameraInitialized_) {
        InitializeCamera(world);
    }
    std::fflush(stderr);
}

// ---------------------------------------------------------------- 機構
void GameSession::SyncMechanisms(HIKARI::WorldView& world, std::vector<Rules::MechanismSource>& sources) {
    mechanismViews_.clear(); movedPose_.clear();
    const HIKARI::ComponentTypeId types[]{HIKARI::MakeComponentTypeId(kMechanismComponentType)};
    (void)world.ForEachComponents<MechanismComponent>(types, HIKARI::GameProjectWorldQueryAnchor::FirstRequiredTerm,
        [&](HIKARI::ObjectView, MechanismComponent& component) {
            Rules::MechanismSource source; std::vector<MechanismObjectMove> moves, movesC; std::string error;
            if (!component.BuildSource(source, moves, movesC, error)) {
                std::fprintf(stderr, "[TD2 mechanism] rejected error=%s\n", error.c_str()); return;
            }
            MechanismView view; view.id = source.id; view.cameraLift = component.CameraLift(); view.cameraPull = component.CameraPull();
            // 状態 1 と -1 の表に出てくる物体をまとめる（片方にしか無ければ、もう片方は元の姿勢のまま）。
            std::vector<uint64_t> ids;
            for (const auto* list : {&moves, &movesC}) for (const auto& move : *list)
                if (std::find(ids.begin(), ids.end(), move.object) == ids.end()) ids.push_back(move.object);
            const auto poseOf = [](const MechanismObjectMove& move, HIKARI::GameProjectTransform pose) {
                const auto start = pose;
                pose.position = {move.position[0], move.position[1], move.position[2]};
                pose.rotation = QFromEulerDeg(move.rotation[0], move.rotation[1], move.rotation[2]);
                if (move.hasPivot) {
                    // 中心のまわりに回した位置。
                    const auto turned = QRotate(QMul(pose.rotation, QConj(start.rotation)),
                        {start.position.x - move.pivot[0], start.position.y - move.pivot[1], start.position.z - move.pivot[2]});
                    pose.position = {move.pivot[0] + turned.x, move.pivot[1] + turned.y, move.pivot[2] + turned.z};
                }
                return pose;
            };
            for (const auto id : ids) {
                const auto object = world.FindSceneObject(id);
                MechanismObject item;
                if (!object.IsValid() || !object.GetLocalTransform(item.a)) {
                    std::fprintf(stderr, "[TD2 mechanism] %s missing object=%llu\n", source.id.c_str(), static_cast<unsigned long long>(id));
                    continue;
                }
                item.handle = object.GetHandle(); item.b = item.c = item.a;
                for (const auto* list : {&moves, &movesC}) for (const auto& move : *list) {
                    if (move.object != id) continue;
                    (list == &moves ? item.b : item.c) = poseOf(move, item.a);
                    if (move.hasPivot) { item.pivot = true; item.center = {move.pivot[0], move.pivot[1], move.pivot[2]}; }
                }
                // その場で画面の向きの軸に回すだけ（倒立）なら、区域カメラは動かさない（カメラまで逆さにしない）。
                const auto& qa = item.a.rotation; const auto& qb = item.b.rotation;
                const float dx = item.b.position.x - item.a.position.x, dy = item.b.position.y - item.a.position.y, dz = item.b.position.z - item.a.position.z;
                const auto delta = QMul(qb, QConj(qa));
                const float axis = std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
                const bool roll = (item.pivot || dx * dx + dy * dy + dz * dz < 1e-6f) && axis > .05f && std::abs(delta.y) / axis < .7f;
                // 動かすのが区域のテレビなら、その区域カメラもテレビに付いて一緒に動く（乗っている間も画面が正面に残る）。
                for (const auto& [region, handle] : regionObjects_) {
                    if (roll || handle.value != item.handle.value) continue;
                    const auto camera = regionCameras_.find(region);
                    if (camera == regionCameras_.end() || !camera->second) continue;
                    const auto cameraObject = world.FindSceneObject(camera->second);
                    HIKARI::GameProjectTransform pose{};
                    if (!cameraObject.IsValid() || !cameraObject.GetLocalTransform(pose)) continue;
                    MechanismAttachment attachment{cameraObject.GetHandle(), view.objects.size(), Relative(item.a, pose)};
                    // 状態 A と B の向きの差が 120°以上なら、カメラは途中で向こう側へ切り替える。
                    const auto& qa = item.a.rotation; const auto& qb = item.b.rotation;
                    const float dot = std::abs(qa.x * qb.x + qa.y * qb.y + qa.z * qb.z + qa.w * qb.w);
                    attachment.turn = 2.f * std::acos(std::clamp(dot, 0.f, 1.f)) >= 120.f * 3.14159265f / 180.f;
                    const auto& r = attachment.relative.position;
                    attachment.reach = std::sqrt(r.x * r.x + r.y * r.y + r.z * r.z);
                    view.attached.push_back(attachment);
                }
                view.objects.push_back(item);
            }
            std::fprintf(stderr, "[TD2 mechanism] %s toggle=%d seconds=%.2f dwell=%.2f regions=%zu objects=%zu cameras=%zu\n", source.id.c_str(),
                source.toggle, source.seconds, source.dwell, source.moves.size(), view.objects.size(), view.attached.size());
            sources.push_back(std::move(source)); mechanismViews_.push_back(std::move(view));
        });
}

// ---------------------------------------------------------------- 槽の棚
void GameSession::SyncSlides(HIKARI::WorldView& world, std::vector<Rules::SlideSource>& sources) {
    slideViews_.clear();
    const HIKARI::ComponentTypeId types[]{HIKARI::MakeComponentTypeId(kSlideRackComponentType)};
    (void)world.ForEachComponents<SlideRackComponent>(types, HIKARI::GameProjectWorldQueryAnchor::FirstRequiredTerm,
        [&](HIKARI::ObjectView, SlideRackComponent& component) {
            Rules::SlideSource source; SlideView view; std::string error;
            if (!component.BuildSource(source, view.slots, error)) { std::fprintf(stderr, "[TD2 slide] rejected error=%s\n", error.c_str()); return; }
            for (const auto& region : source.members) {
                SlideMember member;
                const auto tv = regionObjects_.find(region);
                if (tv != regionObjects_.end()) {
                    member.tv = tv->second;
                    if (const auto object = world.FindObject(member.tv); object.IsValid()) (void)object.GetLocalTransform(member.tvPose);
                }
                // 区域カメラは動かさない（棚全体を写す同じ構図のまま、空いた槽と並びが見え続ける）。
                view.members.push_back(member);
            }
            std::fprintf(stderr, "[TD2 slide] %s slots=%zu members=%zu\n", source.id.c_str(), source.slots.size(), source.members.size());
            sources.push_back(std::move(source)); slideViews_.push_back(std::move(view));
        });
}

void GameSession::UpdateSlideObjects(HIKARI::WorldView& world, double seconds) {
    const auto& runtimes = exploration_.Slides();
    std::vector<HIKARI::GameProjectWorldTransformWrite> writes;
    const auto active = regionObjects_.find(exploration_.ActiveRegion());
    for (size_t i = 0; i < slideViews_.size() && i < runtimes.size(); ++i) {
        auto& view = slideViews_[i]; const auto& runtime = runtimes[i];
        // 動けなかった：そのテレビを 0.35 秒だけ左右に細かく揺らす。
        if (runtime.blockedCount != view.blockedCount) { view.blockedCount = runtime.blockedCount; view.shaking = runtime.blocked; view.shake = 0; }
        const bool moving = runtime.moving >= 0;
        if (!moving && view.lastMoving < 0 && view.shaking < 0) continue;
        for (size_t m = 0; m < view.members.size() && m < runtime.slotOf.size(); ++m) {
            auto& member = view.members[m];
            if (!member.tv.IsValid()) continue;
            const bool thisMoving = moving && runtime.moving == static_cast<int>(m);
            if (!thisMoving && view.lastMoving != static_cast<int>(m) && view.shaking != static_cast<int>(m)) continue;
            // 槽の位置の間を、両端でゆっくり止まるように運ぶ。
            const auto& to = view.slots[static_cast<size_t>(thisMoving ? runtime.to : runtime.slotOf[m])].position;
            const auto& from = view.slots[static_cast<size_t>(thisMoving ? runtime.from : runtime.slotOf[m])].position;
            const float t = thisMoving ? static_cast<float>(runtime.progress) : 1.f, e = t * t * (3 - 2 * t);
            auto pose = member.tvPose;
            pose.position = {from[0] + (to[0] - from[0]) * e, from[1] + (to[1] - from[1]) * e, from[2] + (to[2] - from[2]) * e};
            if (view.shaking == static_cast<int>(m)) {
                view.shake += seconds;
                const double left = (std::max)(0.0, 1 - view.shake / .35);
                pose.position.x += static_cast<float>(.025 * left * std::sin(view.shake * 70));
                if (left <= 0) view.shaking = -1;
            }
            writes.push_back({}); writes.back().object = member.tv; writes.back().localTransform = pose;
            if (member.camera.IsValid()) {
                auto camera = member.cameraPose;
                camera.position = {camera.position.x + pose.position.x - member.tvPose.position.x, camera.position.y + pose.position.y - member.tvPose.position.y,
                                   camera.position.z + pose.position.z - member.tvPose.position.z};
                // 操作中の区域なら追従のばねも同じだけ運ぶ。
                const auto last = movedPose_.find(member.camera.value);
                if (active != regionObjects_.end() && active->second.value == member.tv.value && rig_.Ready())
                    rig_.Carry(last != movedPose_.end() ? last->second : member.cameraPose, camera);
                movedPose_[member.camera.value] = camera;
                writes.push_back({}); writes.back().object = member.camera; writes.back().localTransform = camera;
            }
        }
        view.lastMoving = runtime.moving;
    }
    if (writes.empty()) return;
    std::vector<HIKARI::GameProjectWorldResult> results(writes.size());
    if (!world.SetLocalTransforms(writes, results)) std::fprintf(stderr, "[TD2 slide] transform write failed count=%zu\n", writes.size());
}

void GameSession::SyncCables(HIKARI::WorldView& world) {
    cables_.clear();
    const HIKARI::ComponentTypeId types[]{HIKARI::MakeComponentTypeId(kSignalCableComponentType)};
    (void)world.ForEachComponents<SignalCableComponent>(types, HIKARI::GameProjectWorldQueryAnchor::FirstRequiredTerm,
        [&](HIKARI::ObjectView object, SignalCableComponent& component) {
            HIKARI::GameProjectModelAppearance appearance{};
            if (component.Plate().empty() || component.LitModel().empty() || !object.GetModel().TryGetAppearance(appearance)) return;
            cables_.push_back({object.GetHandle(), component.Plate(), appearance.assetId, component.LitModel()});
        });
    std::fprintf(stderr, "[TD2 cable] count=%zu\n", cables_.size());
}

void GameSession::UpdateCables(HIKARI::WorldView& world) {
    if (cables_.empty()) return;
    const auto& devices = exploration_.Snapshot().devices;
    for (auto& cable : cables_) {
        const auto plate = std::find_if(devices.begin(), devices.end(), [&](const auto& d) { return d.id == cable.plate; });
        const bool lit = plate != devices.end() && plate->powered;
        if (lit == cable.shown) continue;
        const auto& asset = lit ? cable.lit : cable.normal;
        HIKARI::GameProjectModelAppearance appearance{};
        const auto object = world.FindObject(cable.handle);
        if (!object.IsValid() || !object.GetModel().TryGetAppearance(appearance) || asset.size() >= sizeof(appearance.assetId)) continue;
        std::memset(appearance.assetId, 0, sizeof(appearance.assetId));
        std::memcpy(appearance.assetId, asset.data(), asset.size());
        if (object.GetModel().ApplyAppearance(appearance).Accepted()) cable.shown = lit;
    }
}

void GameSession::UpdateMechanismObjects(HIKARI::WorldView& world) {
    const auto& runtimes = exploration_.Mechanisms();
    std::vector<HIKARI::GameProjectWorldTransformWrite> writes;
    const auto active = regionObjects_.find(exploration_.ActiveRegion());
    for (size_t i = 0; i < mechanismViews_.size() && i < runtimes.size(); ++i) {
        auto& view = mechanismViews_[i];
        const double progress = runtimes[i].progress;
        if (progress == view.written) continue;
        view.written = progress;
        // 両端でゆっくり止まる。転台は四元数で回し、吊り台は直線で運ぶ。天平の状態 -1 側は c へ向かう。
        const bool sideC = progress < 0;
        const float t = static_cast<float>(std::abs(progress)), e = t * t * (3 - 2 * t);
        // 操作中の区域の物が動いたら、追従カメラのばねも同じだけ運ぶ（ばねの遅れで画面を見失わない）。
        const auto carry = [&](HIKARI::GameProjectObjectHandle tv, uint64_t key, const HIKARI::GameProjectTransform& first,
                               const HIKARI::GameProjectTransform& pose) {
            const auto last = movedPose_.find(key);
            if (active != regionObjects_.end() && active->second.value == tv.value && rig_.Ready())
                rig_.Carry(last != movedPose_.end() ? last->second : first, pose);
            movedPose_[key] = pose;
        };
        std::vector<HIKARI::GameProjectTransform> poses;
        for (size_t k = 0; k < view.objects.size(); ++k) {
            const auto& object = view.objects[k];
            const auto& end = sideC ? object.c : object.b;
            auto pose = object.a;
            pose.position = {object.a.position.x + (end.position.x - object.a.position.x) * e,
                             object.a.position.y + (end.position.y - object.a.position.y) * e,
                             object.a.position.z + (end.position.z - object.a.position.z) * e};
            pose.rotation = QSlerp(object.a.rotation, end.rotation, e);
            if (object.pivot) {
                // 中心を動かさずに回す（途中も画面の中心が同じ所に留まる）。
                const auto& c = object.center;
                const auto turned = QRotate(QMul(pose.rotation, QConj(object.a.rotation)),
                    {object.a.position.x - c.x, object.a.position.y - c.y, object.a.position.z - c.z});
                pose.position = {c.x + turned.x, c.y + turned.y, c.z + turned.z};
            }
            poses.push_back(pose);
            // 区域カメラが付いたテレビは、下でカメラ自身の動きで運ぶ。
            const bool hasCamera = std::any_of(view.attached.begin(), view.attached.end(), [k](const auto& a) { return a.parent == k; });
            if (hasCamera) movedPose_[object.handle.value] = pose;
            else carry(object.handle, object.handle.value, object.a, pose);
            HIKARI::GameProjectWorldTransformWrite write{}; write.object = object.handle; write.localTransform = pose;
            writes.push_back(write);
        }
        // 付いて行くカメラ。
        // ・大きく回る転台（120°以上）：前半はテレビの回る向きへ回転の kTurnOrbit 割だけ一緒に回り込み、画面が真横を向いて
        //   表も裏も見えない折り返しの瞬間に転台の向こう側へ切り替え、後半も同じ向きへ回り続けて落ち着く（動きが途切れない）。
        //   前後とも画面へわずかに寄り、切り替えを動きの中に隠す。
        // ・それ以外（吊り台など）：一緒に運び、途中だけ上へ持ち上げて視線の後ろへ引く（両端では元の構図に戻る）。
        const float arc = std::sin(e * 3.14159265f);
        for (const auto& attached : view.attached) {
            HIKARI::GameProjectWorldTransformWrite write{}; write.object = attached.handle;
            const auto& object = view.objects[attached.parent];
            HIKARI::GameProjectTransform pose;
            if (attached.turn) {
                // カメラを載せる仮のテレビの姿勢：前半は回転の一部、後半は終わりから同じ割だけ手前。
                auto carrier = poses[attached.parent];
                carrier.rotation = QSlerp(object.a.rotation, object.b.rotation, e < .5f ? kTurnOrbit * e : 1.f - kTurnOrbit * (1.f - e));
                pose = Attach(carrier, attached.relative);
                const auto forward = QRotate(pose.rotation, {0, 0, 1});
                const float push = attached.reach * .05f * (1.f - std::abs(2.f * e - 1.f));
                pose.position = {pose.position.x + forward.x * push, pose.position.y + forward.y * push, pose.position.z + forward.z * push};
            } else {
                pose = Attach(poses[attached.parent], attached.relative);
                const auto forward = QRotate(pose.rotation, {0, 0, 1});
                pose.position = {pose.position.x - forward.x * view.cameraPull * arc, pose.position.y + view.cameraLift * arc,
                                 pose.position.z - forward.z * view.cameraPull * arc};
            }
            carry(object.handle, attached.handle.value, Attach(object.a, attached.relative), pose);
            write.localTransform = pose;
            writes.push_back(write);
        }
    }
    if (writes.empty()) return;
    std::vector<HIKARI::GameProjectWorldResult> results(writes.size());
    if (!world.SetLocalTransforms(writes, results)) std::fprintf(stderr, "[TD2 mechanism] transform write failed count=%zu\n", writes.size());
}

// ---------------------------------------------------------------- カメラ
// 二通りで動かす（方針は TD2CameraRig.h）。
// ・同じ大区域（TD2.CameraArea）の中：CameraRig が区域カメラを基準にプレイヤーへ向けて毎フレーム解く。
// 部屋を移ると基準が次の区域カメラへ滑るだけで、演出も操作の停止も無い。
// ・大区域どうし：結んだ演出を流す。地面を歩いて越えた時は世界を止め、A/D で演出を進める（慣性あり）。
// 起点まで戻すとプレイヤーも元の区域へ戻る。空中で越えた時は世界を止めずに自動で流す。
// 終わったら同じフレームで最後の画から追従へ戻る。

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
    // 演出の Camera スロットの物体（無ければ入口の区域カメラ）をゲームカメラにする。
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
    if (transition.kind == "respawn") {   // 復活は演出を流さず、その区域カメラから追従し直す
        StopTravel(world); SnapCamera(world, to);
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
    exploration_.ClearEcho();         // 大区域を出たら残影は置いて行かない
    const auto* a = exploration_.FindRegion(from);
    const auto* b = exploration_.FindRegion(to);
    int direction = 0;
    if (a && b) {
        if (a->placement.worldX + a->placement.cols == b->placement.worldX) direction = 1;
        else if (b->placement.worldX + b->placement.cols == a->placement.worldX) direction = -1;
    }
    // 地面を歩いて横へ越えた時だけ手動（落下中・跳躍中は止めない）。
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
        // 歩いて越えた勢いを持ち込む。止まっていたら 0 から押し始める。
        const double walk = std::clamp(exploration_.State().player.vx * direction / 5.0, 0.0, 1.0);
        travel.velocity = kTravelCarry * walk * speed / snapshot.durationSeconds;
    }
    // 開始時のカメラのずれ（今の追従の画 − 演出の始点）。序盤で 0 に戻す。
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
        // 押した向き（越えた向きが前）。全速は演出の倍速（binding.speed）と同じ。
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
            // 起点まで戻した：プレイヤーを元の区域へ戻し、そこから追従を続ける。
            const auto from = travel.from; const int direction = travel.direction;
            (void)exploration_.StepBack(from, direction);
            FinishTravel(world, from);
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
        FinishTravel(world, travel.to);
        return;
    }
    // 開始時のずれを序盤で 0 に戻し、演出中も呼吸を重ねる。
    const float weight = static_cast<float>(1 - SmoothStep(travel.progress / kTravelBlendIn));
    HIKARI::GameProjectFloat3 breathEye{}, breathLook{};
    double handRoll = 0;
    // 揺れの向きは描いているカメラ（前のフレーム）の向きから、足したずれを引いて測る。
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
    // 演出の終わりの画からそのまま追従を始める。描いた画には演出へ足した揺れが入っており、追従も同じ揺れを足すので、
    // 基準はそのずれを引いた所に置く（引かないと最初のフレームで少し跳ぶ）。
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
    // 交換の直後は区域カメラの乗り換えを速くする（本体が一瞬で移ったのに画が遅れて付いて来ないように）。
    auto area = AreaOf(region);
    if (swapBoost_ > 0) { area.regionSeconds = (std::min)(area.regionSeconds, kSwapRegionSeconds); swapBoost_ -= dt; }
    const auto pose = rig_.Update(frame, area, target, velocity, state.playerPresent && !state.player.dead, dt, cameraTime_);
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

// 被写界深度の焦点を今の部屋の画面へ合わせ続ける（カメラごとに画面までの距離が違うため）。
// 焦点は画面の中心の視線方向の深さ。部屋が変わった時に跳ばないよう、時定数 0.12 秒で寄せる。
void GameSession::FocusActiveScreen(HIKARI::WorldView& world, double seconds) {
    // 焦点は画で一番目立つ画面へゆっくり送る。目立ち具合は写る面積を、画の中心から離れるほど割り引いた値。
    // 候補は今いる大区域の画面だけ（隙間の奥に写る遠い箱庭の画面へ焦点が逃げない）。
    constexpr double kFocusSeconds = .35;
    constexpr uint64_t kFocusSource = 0x5444325F464F4355ull; // 識別値 "TD2_FOCU"
    auto camera = world.GetCamera();
    HIKARI::GameProjectCameraPose pose{};
    if (!camera.GetRenderedPose(pose)) { return; }
    double best = 0, depth = 0, travelWeight = 0, travelDepth = 0;
    std::string focusRegion;
    for (const auto& [region, handle] : regionObjects_) {
        // 切り替え中は出る部屋と入る部屋の画面だけを見る（途中で遠くの画面に焦点が飛ばないように）。
        const bool travelRoom = travel_ && (region == travel_->from || region == travel_->to);
        if (travel_ && !travelRoom) { continue; }
        if (!travel_ && !SameArea(region, exploration_.ActiveRegion())) { continue; }
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
    // 描いたカメラの姿勢も記録する。
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
    UpdateMechanismObjects(world);
    UpdateSlideObjects(world, frameSeconds);
    UpdateCables(world);
    if (travel_) UpdateTravel(world, moveValid ? move.x : 0.f, frameSeconds);
    if (!travel_) UpdateFollow(world, frameSeconds);
    FocusActiveScreen(world, std::isfinite(frame.gameDt) ? std::max(0.0, double(frame.gameDt)) : 0.0);
    traceSeconds_ += std::max(0.0, double(frame.gameDt));
    // 状態の記録は 1 秒ごと（検証で細かく見たい時は TD2_TRACE_SECONDS で間隔を変える）。
    static const double traceInterval = std::getenv("TD2_TRACE_SECONDS") ? std::atof(std::getenv("TD2_TRACE_SECONDS")) : 1.0;
    if (traceSeconds_ >= traceInterval) {
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

}
