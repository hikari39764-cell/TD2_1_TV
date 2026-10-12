#pragma once

#include "Rules/TD2Exploration.h"
#include "HIKARI/Project/GameProjectSession.h"
#include "HIKARI/Project/FrameContext.h"
#include "HIKARI/Project/SequenceTypes.h"
#include <limits>
#include <map>

namespace TD2 {
using RegionRuntime = Rules::RegionRuntime;

// Play 全体で一つの物理世界を持つ。シーン交換や画面跨ぎで身体を作り直さない。
class GameSession final : public HIKARI::IGameProjectSession {
public:
    void OnGameStarted(HIKARI::WorldView&) override;
    void OnSceneEntered(HIKARI::WorldView& world) override;
    void OnSceneExiting(HIKARI::WorldView& world) override;
    void Update(HIKARI::WorldView& world, const HIKARI::FrameContext& frame);
    const RegionRuntime* FindRegion(std::string_view id) const { return exploration_.FindRegion(id); }
    bool IsControlled(std::string_view id) const noexcept { return id == exploration_.ActiveRegion(); }
private:
    void SyncRegions(HIKARI::WorldView& world);
    void TraceEvents(std::string_view region, const Rules::StepOutput& output);
    void StartSequence(HIKARI::WorldView& world, const Rules::RegionTransition& transition);
    HIKARI::GameProjectSequencePlaybackHandle PlayBoundSequence(HIKARI::WorldView& world,
                                                               const RegionSequenceBinding& binding, float rate);
    void RememberRegionCameras(const Rules::RegionTransition& transition, const RegionSequenceBinding& binding);
    void RestoreRegionCamera(HIKARI::WorldView& world, std::string_view region);
    void TraceSequence(HIKARI::WorldView& world, uint64_t frame);

    // 到着した領域の実 Sequence 端点。物理位置や新しいカメラ経路は保存しない。
    struct RegionCameraAnchor { RegionSequenceBinding binding; bool atEnd = false; };
    std::map<std::string, RegionCameraAnchor> regionCameras_;
    Rules::Exploration exploration_;
    std::vector<RegionSequenceBinding> bindings_;
    RegionSequenceBinding playbackBinding_;
    HIKARI::GameProjectSequencePlaybackHandle playback_{};
    bool sceneActive_ = false, leftHeld_ = false, rightHeld_ = false, lastInputLocked_ = false;
    double traceSeconds_ = 0;
    uint64_t lastFrame_ = (std::numeric_limits<uint64_t>::max)();
};
} // namespace TD2
