#pragma once

#include "TD2Rules.h"
#include "../../Common/TD2WorldLayout.h"
#include <map>

namespace TD2::Rules {
struct RegionSource { WorldDefinition definition; RegionPlacement placement; bool entry = false; };
// 各画面は作者定義と読み取り専用投影だけを持つ。ここに第二の物理状態を作らない。
struct RegionRuntime {
    WorldDefinition definition;
    RegionPlacement placement;
    WorldSnapshot snapshot;
    size_t deviceBegin = 0;
};
struct RegionEvents { std::string regionId; StepOutput output; };
struct RegionTransition { std::string kind, fromRegion, toRegion; Vec2 worldPosition; };
struct ExplorationOutput {
    std::vector<RegionEvents> regions;
    std::vector<RegionTransition> transitions;
    bool inputSuppressed = false;
};

// 一つの連続世界だけを進め、TV ごとのローカル座標へ表示結果を投影する。
class Exploration {
public:
    std::string Initialize(std::vector<RegionSource> sources);
    ExplorationOutput Step(const InputState& input, double seconds);
    const RegionRuntime* FindRegion(std::string_view id) const;
    const std::map<std::string, RegionRuntime>& Regions() const noexcept { return regions_; }
    const std::vector<RegionPlacement>& Placements() const noexcept { return placements_; }
    const WorldDefinition& Definition() const noexcept { return world_; }
    const WorldState& State() const noexcept { return state_; }
    const std::string& ActiveRegion() const noexcept { return activeRegion_; }
    bool Ready() const noexcept { return !activeRegion_.empty(); }
    // SDK adapter が実際の Sequence::IsPlaying だけを渡す。保持中の paused はロックしない。
    void SetInputLocked(bool locked) noexcept { inputLocked_ = locked; }
    bool InputLocked() const noexcept { return inputLocked_; }
    bool ActionNeedsRelease() const noexcept { return actionNeedsRelease_; }
private:
    void BuildBoundaries();
    void RefreshSnapshots();
    void Record(StepOutput output, ExplorationOutput& out);
    std::string RegionAt(const Rect& body) const;

    std::map<std::string, RegionRuntime> regions_;
    std::map<std::string, std::string> deviceOwners_;
    std::vector<RegionPlacement> placements_;
    WorldDefinition world_;
    WorldState state_;
    std::string activeRegion_;
    bool inputLocked_ = false, actionNeedsRelease_ = false;
};
} // namespace TD2::Rules
