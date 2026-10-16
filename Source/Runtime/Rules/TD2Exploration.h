#pragma once

#include "TD2Rules.h"
#include "../../Common/TD2WorldLayout.h"
#include <map>

namespace TD2::Rules {
struct RegionSource { WorldDefinition definition; RegionPlacement placement; bool entry = false; };
// 各画面は作者の定義と配置だけを持つ。物理状態は世界に一つ。
struct RegionRuntime {
    WorldDefinition definition;
    RegionPlacement placement;
};
struct RegionEvents { std::string regionId; StepOutput output; };
struct RegionTransition { std::string kind, fromRegion, toRegion; Vec2 worldPosition; };
struct ExplorationOutput {
    std::vector<RegionEvents> regions;
    std::vector<RegionTransition> transitions;
};

// 一つの連続した世界を進める。装置 ID は「区域ID/装置ID」。
class Exploration {
public:
    // この試遊だけ開始区域を差し替える。作者の設定は変えない。
    std::string Initialize(std::vector<RegionSource> sources, std::string_view entryOverride = {},
                           std::optional<Vec2> spawnOverride = {});
    ExplorationOutput Step(const InputState& input, double seconds);
    const RegionRuntime* FindRegion(std::string_view id) const;
    const std::map<std::string, RegionRuntime>& Regions() const noexcept { return regions_; }
    const std::vector<RegionPlacement>& Placements() const noexcept { return placements_; }
    const WorldDefinition& Definition() const noexcept { return world_; }
    const WorldState& State() const noexcept { return state_; }
    const std::string& ActiveRegion() const noexcept { return activeRegion_; }
    // 世界座標の表示状態。進めた後、最初に読まれた時に作る。
    const WorldSnapshot& Snapshot();
    bool Ready() const noexcept { return !activeRegion_.empty(); }
    // 手動の切り替えを起点まで戻した時、プレイヤーを元の区域の境の内側へ戻す。
    // direction は越えた向き（+1 右、-1 左）。時間や装置は戻さない。
    bool StepBack(std::string_view region, int direction);
private:
    void BuildBoundaries();
    void Record(StepOutput output, ExplorationOutput& out);
    std::string RegionAt(const Rect& body) const;

    std::map<std::string, RegionRuntime> regions_;
    std::map<std::string, std::string> deviceOwners_;
    std::vector<RegionPlacement> placements_;
    WorldDefinition world_;
    WorldState state_;
    WorldSnapshot snapshot_;
    bool snapshotDirty_ = true;
    std::string activeRegion_;
};
}
