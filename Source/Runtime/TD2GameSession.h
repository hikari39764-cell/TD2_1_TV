#pragma once

#include "Rules/TD2Exploration.h"
#include "TD2CameraRig.h"
#include "TD2RegionComponent.h"
#include "TD2Mechanism.h"
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
    // 大区域の切り替え中なら、出る部屋と入る部屋（画面の鮮明な窓と保護を優先する）。
    bool TravelRooms(std::string& from, std::string& to) const {
        if (!travel_) { return false; }
        from = travel_->from; to = travel_->to; return true;
    }
    // 世界座標の表示状態（画面の舞台が読む）。
    const Rules::WorldSnapshot& Snapshot() { return exploration_.Snapshot(); }
private:
    void SyncRegions(HIKARI::WorldView& world);
    void SyncCameraAreas(HIKARI::WorldView& world);
    void TraceEvents(std::string_view region, const Rules::StepOutput& output);
    void TraceSequence(HIKARI::WorldView& world, uint64_t frame);
    void FocusActiveScreen(HIKARI::WorldView& world, double seconds);
    void SyncMechanisms(HIKARI::WorldView& world, std::vector<Rules::MechanismSource>& sources);
    void UpdateMechanismObjects(HIKARI::WorldView& world);
    void SyncCables(HIKARI::WorldView& world);
    void SyncSlides(HIKARI::WorldView& world, std::vector<Rules::SlideSource>& sources);
    void UpdateSlideObjects(HIKARI::WorldView& world, double seconds);
    void UpdateCables(HIKARI::WorldView& world);

    // ---- カメラ（大区域の中は追従、大区域どうしは演出） ----
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

    // 大区域の切り替え（演出）。手動の時は世界を止めて A/D で進める。
    struct Travel {
        RegionSequenceBinding binding;
        std::string from, to, pending;      // 自動の切り替え中に先へ進んだ部屋（終わったら追従が引き継ぐ）
        bool reverse = false, manual = false;
        int direction = 0;                   // 手動で越えた向き（+1 右、-1 左）
        double velocity = 0, progress = 0;   // 手動の進む速さ（演出の長さに対する割合／秒）と進み（0→1）
        double accel = 0;                    // 手動の速さのばねの状態（押し始めと離し際を滑らかにする）
        HIKARI::GameProjectFloat3 eyeOffset{}, lookOffset{};  // 開始時のカメラのずれ（序盤で 0 に戻す）
        // 最後に演出へ足したずれ。終了時に差し引き、二重に足さない。
        HIKARI::GameProjectFloat3 appliedEye{}, appliedLook{};
    };
    std::optional<Travel> travel_;
    CameraRig rig_;
    // 区域 ID → 大区域の設定と名前（同じ名前なら同じ大区域）。未登録の区域は一部屋で一つの大区域。
    std::map<std::string, CameraAreaSettings, std::less<>> areaSettings_;
    std::map<std::string, std::string, std::less<>> areaKey_;
    // 区域ごとのカメラ（Scene の Camera）、テレビの物体、ローカルの画面。
    std::map<std::string, uint64_t, std::less<>> regionCameras_;
    std::map<std::string, HIKARI::GameProjectObjectHandle, std::less<>> regionObjects_;
    std::map<std::string, RegionComponent::ScreenGeometry, std::less<>> regionGeometry_;
    HIKARI::GameProjectObjectHandle driverCamera_{};   // ゲームカメラ（演出の Camera スロットと同じ物体）
    bool cameraInitialized_ = false;
    double cameraTime_ = 0, focusDistance_ = 0;
    double swapBoost_ = 0;   // 交換の直後、区域カメラの乗り換えを速めている残り秒

    // 機構で動かす物体（状態 A と B の姿勢）と、動くテレビに付いて行く区域カメラ。
    // 状態 0・1・-1（天平のもう一方）の姿勢。
    // pivot：その点を中心に回す（位置は回転から求める）。
    struct MechanismObject {
        HIKARI::GameProjectObjectHandle handle{}; HIKARI::GameProjectTransform a{}, b{}, c{};
        bool pivot = false; HIKARI::GameProjectFloat3 center{};
    };
    // turn：大きく回る転台に付いたカメラ（途中で向こう側へ切り替える）。reach はテレビまでの距離。
    struct MechanismAttachment {
        HIKARI::GameProjectObjectHandle handle{}; size_t parent = 0; HIKARI::GameProjectTransform relative{};
        bool turn = false; float reach = 0;
    };
    struct MechanismView {
        std::string id;
        std::vector<MechanismObject> objects;
        std::vector<MechanismAttachment> attached;
        double written = -1;   // 最後に書いた進み（変わった時だけ書く）
        float cameraLift = 0, cameraPull = 0;   // 動きの途中だけカメラを持ち上げ・引く量（転台以外）
    };
    std::vector<MechanismView> mechanismViews_;
    // 槽の棚：メンバーのテレビ、槽ごとのテレビの位置。動けなかった時は shake 秒だけ揺らす（区域カメラは動かさない）。
    struct SlideMember { HIKARI::GameProjectObjectHandle tv{}, camera{}; HIKARI::GameProjectTransform tvPose{}, cameraPose{}; };
    struct SlideView {
        std::vector<SlideSlotPose> slots;
        std::vector<SlideMember> members;
        int lastMoving = -1; uint64_t blockedCount = 0; int shaking = -1; double shake = 0;
    };
    std::vector<SlideView> slideViews_;
    // 信号線：つながった圧力板の通電で、光る版と元のモデルを差し替える。
    struct SignalCable { HIKARI::GameProjectObjectHandle handle{}; std::string plate, normal, lit; bool shown = false; };
    std::vector<SignalCable> cables_;
    std::map<uint64_t, HIKARI::GameProjectTransform> movedPose_;   // 動かした物体の今の姿勢
    Rules::Exploration exploration_;
    std::vector<RegionSequenceBinding> bindings_;
    RegionSequenceBinding playbackBinding_;
    HIKARI::GameProjectSequencePlaybackHandle playback_{};
    // エディタからの一回限りの入口。保存データには戻さない。
    uint64_t previewEntryObject_ = 0;
    std::optional<Rules::Vec2> previewSpawn_;
    bool sceneActive_ = false, leftHeld_ = false, rightHeld_ = false;
    double traceSeconds_ = 0;
    std::string traceFocus_;   // 最後に焦点を合わせた画面
    uint64_t lastFrame_ = (std::numeric_limits<uint64_t>::max)();
};
}
