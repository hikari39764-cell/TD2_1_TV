#pragma once

#include "TD2Rules.h"
#include "../../Common/TD2WorldLayout.h"
#include <map>

namespace TD2::Rules {
struct RegionSource { WorldDefinition definition; RegionPlacement placement; bool entry = false; };
// 各画面は作者定義と配置だけを持つ。表示は世界に一つの snapshot を使い、部屋ごとの写しや第二の物理状態を作らない。
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

// 一つの連続世界だけを進める。表示は世界座標の snapshot 一つ（装置 ID は「区域ID/装置ID」）。
class Exploration {
public:
    // 一回の試遊だけ開始区域を置き換える。作者の entryRegion と各区域の出生点は変更しない。
    std::string Initialize(std::vector<RegionSource> sources, std::string_view entryOverride = {},
                           std::optional<Vec2> spawnOverride = {});
    ExplorationOutput Step(const InputState& input, double seconds);
    const RegionRuntime* FindRegion(std::string_view id) const;
    const std::map<std::string, RegionRuntime>& Regions() const noexcept { return regions_; }
    const std::vector<RegionPlacement>& Placements() const noexcept { return placements_; }
    const WorldDefinition& Definition() const noexcept { return world_; }
    const WorldState& State() const noexcept { return state_; }
    const std::string& ActiveRegion() const noexcept { return activeRegion_; }
    // 世界座標の表示状態。進めた後に初めて読まれた時だけ作る（刻みごとには作らない）。
    const WorldSnapshot& Snapshot();
    bool Ready() const noexcept { return !activeRegion_.empty(); }
    // 手動で押していた大区域の切り替えを起点まで戻した時：主角を元の区域の境の内側（半格余り）へ戻し、
    // 元の区域を操作中に戻す。direction は越えた向き（+1 右、-1 左）。世界の時間や装置は巻き戻さない。
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
} // namespace TD2::Rules
