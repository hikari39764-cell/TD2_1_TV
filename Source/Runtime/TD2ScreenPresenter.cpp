#include "TD2ScreenPresenter.h"
#include "TD2GameSession.h"
#include "TD2RegionComponent.h"
#include "HIKARI/Project/ActionViews.h"
#include "HIKARI/Project/GameFlowViews.h"
#include "HIKARI/Project/WorldQuery.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <map>

namespace TD2 {
namespace {
// 編集中に部屋ごとの作者の装置と出生点を、未配置の部屋の置き場所が決まるまで持っておく。
struct AuthoredRoom {
    std::vector<Rules::DeviceDefinition> devices;
    Rules::Vec2 spawn;
    bool entry = false;
};
} // namespace

void ScreenPresentationSystem::ReportStatus() {
    // revision や一時的な effect 数ではログを増やさない。準備・失敗・回復の状態辺沿だけ出す。
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
    // 1. 描き方：舞台カメラに付けた共有設定と、区域の物体に付けた局所設定。
    ScreenStyleSettings globalStyle;
    std::map<uint64_t, ScreenStyleSettings> localStyles;
    const HIKARI::ComponentTypeId styleTypes[]{HIKARI::MakeComponentTypeId(kScreenStyleComponentType)};
    const auto styles = world.ForEachComponents<ScreenStyleComponent>(styleTypes,
        HIKARI::GameProjectWorldQueryAnchor::FirstRequiredTerm,
        [&world, &globalStyle, &localStyles](HIKARI::ObjectView object, ScreenStyleComponent& style) {
            const auto settings = style.Settings(); localStyles[object.GetHandle().value] = settings;
            // Camera に付けた共有設定は self 参照を使える。Region の局所設定は後で上書きする。
            const auto camera = world.FindSceneObject(settings.captureCameraObject ? settings.captureCameraObject : object.GetSceneObjectId());
            HIKARI::GameProjectCameraLens lens{};
            if (!globalStyle.captureCameraObject && camera.IsValid() && world.GetCamera().GetAuthoredLens(camera.GetHandle().value, lens)) {
                globalStyle = settings; globalStyle.captureCameraObject = camera.GetSceneObjectId();
            }
        });
    if (!styles.Succeeded()) { status_ = "3D风格组件查询失败"; stage_.Release(world); ReportStatus(); return; }
    // 特効の設定は最初に見つかった一つだけ（舞台カメラに付ける）。無ければ既定の一式。
    ScreenFxSettings fx; bool foundFx = false;
    const HIKARI::ComponentTypeId fxTypes[]{HIKARI::MakeComponentTypeId(kScreenFxComponentType)};
    (void)world.ForEachComponents<ScreenFxComponent>(fxTypes, HIKARI::GameProjectWorldQueryAnchor::FirstRequiredTerm,
        [&fx, &foundFx](HIKARI::ObjectView, ScreenFxComponent& component) {
            if (!foundFx) { fx = component.Settings(); foundFx = true; }
        });

    // 2. 部屋。Play 中は Session の配置と地図を借りる（開始時の配置で固定し、作者の新配置と混ぜない。世界に置いた部屋だけ）。
    // 編集中は作者の地図を写し、装置と出生点は未配置の部屋の置き場所が決まってから世界の格へ移す。
    const bool playing = world.GetRuntimeSession().IsPlayActive();
    auto* session = playing ? dynamic_cast<GameSession*>(world.GetGameSession()) : nullptr;
    if (session && session->ActiveRegion().empty()) { session = nullptr; }
    rooms_.clear();
    std::vector<AuthoredRoom> authored;
    const HIKARI::ComponentTypeId types[]{HIKARI::MakeComponentTypeId(kRegionComponentType)};
    const auto regions = world.ForEachComponents<RegionComponent>(types, HIKARI::GameProjectWorldQueryAnchor::FirstRequiredTerm,
        [&](HIKARI::ObjectView object, RegionComponent& region) {
            const auto placement = region.Placement();
            StageRoom room;
            room.owner = object.GetHandle(); room.regionId = placement.regionId; room.configuration = region.Screen();
            room.style = globalStyle;
            if (const auto own = localStyles.find(object.GetHandle().value); own != localStyles.end()) {
                room.style = own->second; room.style.captureCameraObject = globalStyle.captureCameraObject;
            }
            if (session) {
                const auto* runtime = session->FindRegion(placement.regionId);
                if (!runtime) { return; }
                room.x = runtime->placement.worldX; room.y = runtime->placement.worldY; room.region = &runtime->definition.region;
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
    if (!regions.Succeeded()) { status_ = "区域3D数据查询失败"; stage_.Release(world); ReportStatus(); return; }

    StageFrame stage;
    if (session) {
        stage.snapshot = &session->Snapshot(); stage.activeRegion = session->ActiveRegion(); stage.simulated = true;
    } else {
        // 世界に置いていない作者地図（机の画面など）は、世界の下に 1 格ずつ空けて横一列に並べる（隣り合わせない）。
        int left = 0, bottom = 0, maxCols = 1, maxRows = 1; bool any = false;
        for (const auto& room : rooms_) if (room.placed) {
            left = any ? (std::min)(left, room.x) : room.x; bottom = any ? (std::max)(bottom, room.y + room.Region().rows) : room.y + room.Region().rows;
            maxCols = (std::max)(maxCols, room.Region().cols); maxRows = (std::max)(maxRows, room.Region().rows); any = true;
        }
        int next = 0;
        for (auto& room : rooms_) if (!room.placed) { room.x = left + next++ * (maxCols + 1); room.y = bottom + 2 * maxRows; }
        // 開始区域（無ければ最初に置いた部屋、それも無ければ最初の部屋）の出生位置に待機姿勢の主角を立たせる。
        size_t entry = rooms_.size();
        for (size_t i = 0; i < rooms_.size() && entry == rooms_.size(); ++i) { if (rooms_[i].placed && authored[i].entry) { entry = i; } }
        for (size_t i = 0; i < rooms_.size() && entry == rooms_.size(); ++i) { if (rooms_[i].placed) { entry = i; } }
        if (entry == rooms_.size() && !rooms_.empty()) { entry = 0; }
        authored_ = {};
        authored_.regionId = "td2.authored";
        for (size_t i = 0; i < rooms_.size(); ++i) for (const auto& device : authored[i].devices) {
            Rules::DeviceSnapshot item{rooms_[i].regionId + "/" + device.id, device.kind,
                {device.rect.x + rooms_[i].x, device.rect.y + rooms_[i].y, device.rect.w, device.rect.h}, device.direction,
                device.inverted, false,
                device.kind == Rules::DeviceKind::Solid || device.kind == Rules::DeviceKind::Door || device.kind == Rules::DeviceKind::LockWall};
            item.variant = device.variant;
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
} // namespace TD2
