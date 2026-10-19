#pragma once

#include "Rules/TD2Exploration.h"
#include "TD2CameraDirector.h"
#include "TD2MachineViews.h"
#include "HIKARI/Project/GameProjectSession.h"
#include "HIKARI/Project/FrameContext.h"
#include <limits>
#include <optional>

namespace TD2 {
using RegionRuntime = Rules::RegionRuntime;

// Play 全体で一つの物理世界を持つ。シーン交換や画面跨ぎで身体を作り直さない。
// 規則を進め、カメラ（CameraDirector）と仕掛けの 3D 表示（MachineViews）へ結果を渡す。
class GameSession final : public HIKARI::IGameProjectSession {
public:
    void OnGameStarted(HIKARI::WorldView&) override;
    void OnSceneEntered(HIKARI::WorldView& world) override;
    void OnSceneExiting(HIKARI::WorldView& world) override;
    void Update(HIKARI::WorldView& world, const HIKARI::FrameContext& frame);
    const RegionRuntime* FindRegion(std::string_view id) const { return exploration_.FindRegion(id); }
    const std::string& ActiveRegion() const noexcept { return exploration_.ActiveRegion(); }
    // 大区域の切り替え中なら、出る部屋と入る部屋（画面の鮮明な窓と保護を優先する）。
    bool TravelRooms(std::string& from, std::string& to) const { return camera_.TravelRooms(from, to); }
    // 世界座標の表示状態（画面の舞台が読む）。
    const Rules::WorldSnapshot& Snapshot() { return exploration_.Snapshot(); }
    // 同じ大区域の部屋。画面の舞台の周りの窓を大区域ごとに固定するのに使う。
    std::vector<std::string> AreaRooms(std::string_view region) const { return camera_.AreaRooms(region); }

private:
    void SyncRegions(HIKARI::WorldView& world);
    void Trace(HIKARI::WorldView& world, uint64_t frame);

    Rules::Exploration exploration_;
    RegionObjects regions_;
    CameraDirector camera_;
    MachineViews machines_;
    // エディタからの一回限りの入口。保存データには戻さない。
    uint64_t previewEntryObject_ = 0;
    std::optional<Rules::Vec2> previewSpawn_;
    bool sceneActive_ = false, leftHeld_ = false, rightHeld_ = false;
    double traceSeconds_ = 0;
    uint64_t lastFrame_ = (std::numeric_limits<uint64_t>::max)();
};
}
