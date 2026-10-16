#pragma once

#include "Rules/TD2Exploration.h"
#include "TD2CameraRig.h"
#include "TD2RegionComponent.h"
#include "HIKARI/Project/GameProjectSession.h"
#include "HIKARI/Project/FrameContext.h"
#include "HIKARI/Project/SequenceTypes.h"
#include <limits>
#include <map>
#include <optional>

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
    // 大区域どうしの切り替えの演出の途中なら、出る部屋と入る部屋（電視の舞台が清晰の窓・保護をこの二台に絞るのに使う）。
    bool TravelRooms(std::string& from, std::string& to) const {
        if (!travel_) { return false; }
        from = travel_->from; to = travel_->to; return true;
    }
    // 世界座標の表示状態（電視の舞台が読む）。
    const Rules::WorldSnapshot& Snapshot() { return exploration_.Snapshot(); }
private:
    void SyncRegions(HIKARI::WorldView& world);
    void SyncCameraAreas(HIKARI::WorldView& world);
    void TraceEvents(std::string_view region, const Rules::StepOutput& output);
    void TraceSequence(HIKARI::WorldView& world, uint64_t frame);
    void FocusActiveScreen(HIKARI::WorldView& world, double seconds);

    // ---- 镜头（TD2CameraRig.h の方針：大区域の中は追従、大区域どうしは演出） ----
    bool RegionFrame(HIKARI::WorldView& world, std::string_view region, CameraRegionFrame& out) const;
    const CameraAreaSettings& AreaOf(std::string_view region) const;
    bool SameArea(std::string_view a, std::string_view b) const;
    void InitializeCamera(HIKARI::WorldView& world);
    void SnapCamera(HIKARI::WorldView& world, std::string_view region);
    void OnCrossing(HIKARI::WorldView& world, const Rules::RegionTransition& transition);
    bool StartTravel(HIKARI::WorldView& world, const std::string& from, const std::string& to, bool manual, int direction);
    void UpdateTravel(HIKARI::WorldView& world, float input, double dt);
    void FinishTravel(HIKARI::WorldView& world, const std::string& region);
    void StopTravel(HIKARI::WorldView& world);
    void UpdateFollow(HIKARI::WorldView& world, double dt);
    HIKARI::GameProjectSequencePlaybackHandle PlayBoundSequence(HIKARI::WorldView& world,
                                                               const RegionSequenceBinding& binding, float rate);

    // 大区域どうしの切り替え（演出）。手動は世界を止めて A/D で押し進める。
    struct Travel {
        RegionSequenceBinding binding;
        std::string from, to, pending;      // pending：自動の切り替え中に先へ進んだ部屋（終わった後に追従が引き継ぐ）
        bool reverse = false, manual = false;
        int direction = 0;                   // 手動で越えた向き（+1 右、-1 左）
        double velocity = 0, progress = 0;   // 手動の進む速さ（演出の長さに対する割合／秒）と進み（0→1）
        double accel = 0;                    // 手動の速さの変化の速さ（ばねの状態。押し始め・離し際を滑らかにする）
        HIKARI::GameProjectFloat3 eyeOffset{}, lookOffset{};  // 始まりの機位のずれ（序盤で 0 へ戻す）
        // 最後のフレームに演出へ重ねたずれ（呼吸・手持ちと始まりのずれ）。終わりに追従の基準から差し引き、二重に足さない。
        HIKARI::GameProjectFloat3 appliedEye{}, appliedLook{};
    };
    std::optional<Travel> travel_;
    CameraRig rig_;
    // 区域 ID → 大区域の设定・大区域の名（同じ名なら同じ大区域）。登録の無い区域はそれぞれ一部屋の大区域。
    std::map<std::string, CameraAreaSettings, std::less<>> areaSettings_;
    std::map<std::string, std::string, std::less<>> areaKey_;
    // 区域ごとの区域机位（Scene の Camera 物体）・電視の物体・電視の局所の画面。
    std::map<std::string, uint64_t, std::less<>> regionCameras_;
    std::map<std::string, HIKARI::GameProjectObjectHandle, std::less<>> regionObjects_;
    std::map<std::string, RegionComponent::ScreenGeometry, std::less<>> regionGeometry_;
    HIKARI::GameProjectObjectHandle driverCamera_{};   // 機位を提出するゲーム用カメラ（演出の Camera 槽と同じ物体）
    bool cameraInitialized_ = false;
    double cameraTime_ = 0, focusDistance_ = 0;

    Rules::Exploration exploration_;
    std::vector<RegionSequenceBinding> bindings_;
    RegionSequenceBinding playbackBinding_;
    HIKARI::GameProjectSequencePlaybackHandle playback_{};
    // Editor の一回限りの入口。Scene entry を消費した時だけ設定し、保存データへ戻さない。
    uint64_t previewEntryObject_ = 0;
    std::optional<Rules::Vec2> previewSpawn_;
    bool sceneActive_ = false, leftHeld_ = false, rightHeld_ = false;
    double traceSeconds_ = 0;
    std::string traceFocus_;   // 診断用：最後に焦点を合わせた画面
    uint64_t lastFrame_ = (std::numeric_limits<uint64_t>::max)();
};
} // namespace TD2
