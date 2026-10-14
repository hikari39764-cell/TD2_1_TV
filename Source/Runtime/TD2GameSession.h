#pragma once

#include "Rules/TD2Exploration.h"
#include "HIKARI/Project/GameProjectSession.h"
#include "HIKARI/Project/FrameContext.h"
#include "HIKARI/Project/SequenceTypes.h"
#include <deque>
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
    const std::string& ActiveRegion() const noexcept { return exploration_.ActiveRegion(); }
    // 世界座標の表示状態（電視の舞台が読む）。
    const Rules::WorldSnapshot& Snapshot() { return exploration_.Snapshot(); }
private:
    void SyncRegions(HIKARI::WorldView& world);
    void TraceEvents(std::string_view region, const Rules::StepOutput& output);
    void FollowCamera(HIKARI::WorldView& world, const Rules::RegionTransition& transition);
    void AdvanceCamera(HIKARI::WorldView& world);
    bool PlayTransition(HIKARI::WorldView& world, const std::string& from, const std::string& to, float rateScale);
    HIKARI::GameProjectSequencePlaybackHandle PlayBoundSequence(HIKARI::WorldView& world,
                                                               const RegionSequenceBinding& binding, float rate);
    void RememberRegionCameras(const std::string& from, const std::string& to, const RegionSequenceBinding& binding);
    void RestoreRegionCamera(HIKARI::WorldView& world, std::string_view region);
    void TraceSequence(HIKARI::WorldView& world, uint64_t frame);
    void FocusActiveScreen(HIKARI::WorldView& world, double seconds);

    // 到着した領域の実 Sequence 端点。物理位置や新しいカメラ経路は保存しない。
    struct RegionCameraAnchor { RegionSequenceBinding binding; bool atEnd = false; };
    std::map<std::string, RegionCameraAnchor> regionCameras_;
    Rules::Exploration exploration_;
    std::vector<RegionSequenceBinding> bindings_;
    RegionSequenceBinding playbackBinding_;
    HIKARI::GameProjectSequencePlaybackHandle playback_{};
    // 镜头が流れている切り替え（cameraFrom_ → cameraAt_）と、その後に通った部屋（順に流す）。
    std::string cameraFrom_, cameraAt_;
    std::deque<std::string> cameraQueue_;
    // 区域の電視の物体（焦点を今の部屋の画面へ合わせる）と、なだらかに寄せている焦点距離（0 は未設定）。
    std::map<std::string, HIKARI::GameProjectObjectHandle, std::less<>> regionObjects_;
    double focusDistance_ = 0;
    bool sceneActive_ = false, leftHeld_ = false, rightHeld_ = false;
    double traceSeconds_ = 0;
    uint64_t lastFrame_ = (std::numeric_limits<uint64_t>::max)();
};
} // namespace TD2
