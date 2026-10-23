#include "TD2ScreenPresenter.h"
#include "TD2GameSession.h"
#include "TD2RegionComponent.h"
#include "TD2ScreenTone.h"
#include "TD2Transform.h"
#include "HIKARI/Project/ActionViews.h"
#include "HIKARI/Project/GameFlowViews.h"
#include "HIKARI/Project/WorldQuery.h"
#include <algorithm>
#include <cstdlib>
#include <chrono>
#include <set>
#include <cmath>
#include <cstdio>
#include <map>

namespace TD2 {
namespace {
// 編集中、未配置の部屋の置き場所が決まるまで装置とスポーン位置を持っておく。
struct AuthoredRoom {
    std::vector<Rules::DeviceDefinition> devices;
    Rules::Vec2 spawn;
    bool entry = false;
};
}

void ScreenPresentationSystem::ReportStatus() {
    // ログは準備・失敗・回復の変化の時だけ出す。
    auto category = status_;
    if (const auto models = category.find(" models="); models != std::string::npos) {
        category.resize(models);
        category += status_.find(" rejected=0 ") == std::string::npos ? " model-pending" : " models-ready";
    }
    if (category != loggedStatus_) { std::fprintf(stderr, "[TD2 Screen3D] %s\n", status_.c_str()); loggedStatus_ = std::move(category); }
}

void ScreenPresentationSystem::ReportTiming(double frameSeconds, double renderSeconds) {
    frameMs_.push_back(static_cast<float>(frameSeconds * 1000)); renderMs_.push_back(static_cast<float>(renderSeconds * 1000));
    gameMs_.push_back(static_cast<float>(GameUpdateMilliseconds())); GameUpdateMilliseconds() = 0;
    if (frameMs_.size() < 240) { return; }
    auto sorted = frameMs_; std::sort(sorted.begin(), sorted.end());
    double sum = 0, renderSum = 0, gameSum = 0;
    for (const float v : frameMs_) { sum += v; }
    for (const float v : renderMs_) { renderSum += v; }
    for (const float v : gameMs_) { gameSum += v; }
    std::fprintf(stderr, "[TD2 perf] frame avg=%.2fms p95=%.2fms max=%.2fms screen3d avg=%.3fms max=%.3fms rules avg=%.3fms max=%.3fms\n",
        sum / frameMs_.size(), sorted[sorted.size() * 95 / 100], sorted.back(), renderSum / renderMs_.size(),
        *std::max_element(renderMs_.begin(), renderMs_.end()), gameSum / gameMs_.size(), *std::max_element(gameMs_.begin(), gameMs_.end()));
    frameMs_.clear(); renderMs_.clear(); gameMs_.clear();
}

void ScreenPresentationSystem::Update(HIKARI::WorldView& world, const HIKARI::FrameContext& frame) {
    const auto started = std::chrono::steady_clock::now();
    // 1. 描き方：舞台カメラの共有設定と、区域ごとの設定。
    ScreenStyleSettings globalStyle;
    std::map<uint64_t, ScreenStyleSettings> localStyles;
    const HIKARI::ComponentTypeId styleTypes[]{HIKARI::MakeComponentTypeId(kScreenStyleComponentType)};
    const auto styles = world.ForEachComponents<ScreenStyleComponent>(styleTypes,
        HIKARI::GameProjectWorldQueryAnchor::FirstRequiredTerm,
        [&world, &globalStyle, &localStyles](HIKARI::ObjectView object, ScreenStyleComponent& style) {
            const auto settings = style.Settings(); localStyles[object.GetHandle().value] = settings;
            // 共有設定は self 参照を使える。区域ごとの設定で後から上書きする。
            const auto camera = world.FindSceneObject(settings.captureCameraObject ? settings.captureCameraObject : object.GetSceneObjectId());
            HIKARI::GameProjectCameraLens lens{};
            if (!globalStyle.captureCameraObject && camera.IsValid() && world.GetCamera().GetAuthoredLens(camera.GetHandle().value, lens)) {
                globalStyle = settings; globalStyle.captureCameraObject = camera.GetSceneObjectId();
            }
        });
    if (!styles.Succeeded()) { status_ = "3D の描き方の部品を読めない"; stage_.Release(world); ReportStatus(); return; }
    // 色調：区域の物体に付いたものはその部屋の、ほかの物体に付いたもの（最初の一つ）は全部の画面の既定。
    // どれが区域の物体かは部屋を読んでから分かるので、ここでは全部を覚えておく。
    std::vector<std::pair<uint64_t, ScreenToneSettings>> tones;
    const HIKARI::ComponentTypeId toneTypes[]{HIKARI::MakeComponentTypeId(kScreenToneComponentType)};
    (void)world.ForEachComponents<ScreenToneComponent>(toneTypes, HIKARI::GameProjectWorldQueryAnchor::FirstRequiredTerm,
        [&tones](HIKARI::ObjectView object, ScreenToneComponent& tone) { tones.emplace_back(object.GetHandle().value, tone.Settings()); });
    std::set<uint64_t> regionObjects;
    // エフェクト設定は最初の一つだけ使う。無ければ既定値。
    ScreenFxSettings fx; bool foundFx = false;
    const HIKARI::ComponentTypeId fxTypes[]{HIKARI::MakeComponentTypeId(kScreenFxComponentType)};
    (void)world.ForEachComponents<ScreenFxComponent>(fxTypes, HIKARI::GameProjectWorldQueryAnchor::FirstRequiredTerm,
        [&fx, &foundFx](HIKARI::ObjectView, ScreenFxComponent& component) {
            if (!foundFx) { fx = component.Settings(); foundFx = true; }
        });

    // 2. 部屋。Play 中は Session の配置と地図を使う（世界に置いた部屋だけ）。
    // 編集中は作者の地図を写し、装置とスポーン位置は置き場所が決まってから世界のマスへ移す。
    // 診断用（環境変数 TD2_FORCE_AUTHORING）：Play 中でも編集中と同じ舞台を描く（無窓で編集画面の描画を再現する）。
    static const bool forceAuthoring = std::getenv("TD2_FORCE_AUTHORING") != nullptr;
    const bool playing = world.GetRuntimeSession().IsPlayActive() && !forceAuthoring;
    auto* session = playing ? dynamic_cast<GameSession*>(world.GetGameSession()) : nullptr;
    if (session && session->ActiveRegion().empty()) { session = nullptr; }
    rooms_.clear();
    std::vector<AuthoredRoom> authored;
    const HIKARI::ComponentTypeId types[]{HIKARI::MakeComponentTypeId(kRegionComponentType)};
    const auto regions = world.ForEachComponents<RegionComponent>(types, HIKARI::GameProjectWorldQueryAnchor::FirstRequiredTerm,
        [&](HIKARI::ObjectView object, RegionComponent& region) {
            const auto placement = region.Placement();
            StageRoom room;
            room.owner = object.GetHandle(); room.regionId = placement.regionId; room.configuration = region.Screen(object);
            room.surface = RegionComponent::Surface(object);
            room.style = globalStyle;
            if (const auto own = localStyles.find(object.GetHandle().value); own != localStyles.end()) {
                room.style = own->second; room.style.captureCameraObject = globalStyle.captureCameraObject;
            }
            regionObjects.insert(object.GetHandle().value);
            if (session) {
                const auto* runtime = session->FindRegion(placement.regionId);
                if (!runtime) { return; }
                room.x = runtime->placement.worldX; room.y = runtime->placement.worldY; room.region = &runtime->definition.region;
                room.linkX = room.x; room.linkY = room.y;
                room.flipped = runtime->flipped;
                for (const auto& mechanism : session->Mechanisms()) {
                    for (const auto* moves : {&mechanism.source.moves, &mechanism.source.movesC}) {
                        for (const auto& move : *moves) { if (move.flip && move.regionId == placement.regionId) { room.flippable = true; } }
                    }
                }
                if (const auto* rest = room.flippable ? session->RestPose(room.configuration.screenObject) : nullptr) {
                    const auto right = QRotate(rest->rotation, {1, 0, 0}), up = QRotate(rest->rotation, {0, 1, 0});
                    room.restTurn = std::atan2(right.y, up.y);
                }
                rooms_.push_back(std::move(room));
                return;
            }
            if (playing && !placement.worldPlaced) { return; }
            const auto& definition = region.AuthoredDefinition();
            room.ownedRegion = definition.region; room.placed = placement.worldPlaced;
            room.x = placement.worldX; room.y = placement.worldY;
            rooms_.push_back(std::move(room));
            authored.push_back({definition.devices, definition.spawn, region.IsEntryRegion()});
        });
    if (!regions.Succeeded()) { status_ = "区域の 3D データを読めない"; stage_.Release(world); ReportStatus(); return; }
    // 色調を部屋と共有設定へ写す。
    for (const auto& [handle, tone] : tones) { if (!regionObjects.contains(handle)) { globalStyle.tone = tone; break; } }
    for (auto& room : rooms_) {
        const auto own = std::find_if(tones.begin(), tones.end(), [&](const auto& t) { return t.first == room.owner.value; });
        room.style.tone = own != tones.end() ? own->second : globalStyle.tone;
    }

    StageFrame stage;
    stage.authoring = !playing;
    if (session) {
        stage.snapshot = &session->Snapshot(); stage.activeRegion = session->ActiveRegion(); stage.simulated = true;
        (void)session->TravelRooms(stage.travelFrom, stage.travelTo);
        if (session->Intro().active) { stage.intro = &session->Intro(); }
    } else {
        if (!playing) {
            intro_.Preview(world);
            if (intro_.Screens().active) { stage.intro = &intro_.Screens(); }
        }
        // 未配置の地図は世界の下に 1 マスずつ空けて並べる。
        int left = 0, bottom = 0, maxCols = 1, maxRows = 1; bool any = false;
        for (const auto& room : rooms_) if (room.placed) {
            left = any ? (std::min)(left, room.x) : room.x; bottom = any ? (std::max)(bottom, room.y + room.Region().rows) : room.y + room.Region().rows;
            maxCols = (std::max)(maxCols, room.Region().cols); maxRows = (std::max)(maxRows, room.Region().rows); any = true;
        }
        int next = 0;
        for (auto& room : rooms_) if (!room.placed) { room.x = left + next++ * (maxCols + 1); room.y = bottom + 2 * maxRows; }
        // 世界の位置は隣り合いの判定に残し、描く位置だけを詰めて並べ直す（画面がぼやけないよう 1 マスの texel を確保する）。
        for (auto& room : rooms_) { room.linkX = room.x; room.linkY = room.y; }
        PackRoomsForEditor(rooms_);
        // 開始区域（無ければ最初の部屋）のスポーン位置にプレイヤーを立たせる。
        size_t entry = rooms_.size();
        for (size_t i = 0; i < rooms_.size() && entry == rooms_.size(); ++i) { if (rooms_[i].placed && authored[i].entry) { entry = i; } }
        for (size_t i = 0; i < rooms_.size() && entry == rooms_.size(); ++i) { if (rooms_[i].placed) { entry = i; } }
        if (entry == rooms_.size() && !rooms_.empty()) { entry = 0; }
        authored_ = {};
        authored_.regionId = "td2.authored";
        for (size_t i = 0; i < rooms_.size(); ++i) for (const auto& device : authored[i].devices) {
            Rules::DeviceSnapshot item{rooms_[i].regionId + "/" + device.id, device.kind,
                {device.rect.x + rooms_[i].x, device.rect.y + rooms_[i].y, device.rect.w, device.rect.h}, device.direction,
                device.inverted || device.kind == Rules::DeviceKind::Goal, false,   // 通関の扉は編集中は開けて見せる
                device.kind == Rules::DeviceKind::Solid || device.kind == Rules::DeviceKind::Door || device.kind == Rules::DeviceKind::LockWall};
            item.variant = device.variant; item.latch = device.latch;
            item.world = device.kind == Rules::DeviceKind::Plate && std::any_of(device.targets.begin(), device.targets.end(), [](const std::string& t) {
                return t.starts_with("mech/") || t.starts_with("slide/") || t.starts_with("stack/") || t.starts_with("carousel/");
            });
            authored_.devices.push_back(std::move(item));
        }
        if (entry < rooms_.size()) {
            const Rules::Rect body{rooms_[entry].x + authored[entry].spawn.x, rooms_[entry].y + authored[entry].spawn.y, .625, .875};
            authored_.player.rect = body; authored_.player.grounded = true; authored_.presentationPlayer = body;
            stage.activeRegion = rooms_[entry].regionId;
        }
        stage.snapshot = &authored_;
    }
    stage_.Render(world, rooms_, stage, globalStyle, fx); status_ = stage_.Status(); ReportStatus();
    ReportTiming(frame.rawDt, std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
}
}
