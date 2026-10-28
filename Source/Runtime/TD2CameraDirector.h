#pragma once
// ゲームカメラ。同じ大区域（TD2.CameraArea）の中は CameraRig が区域カメラを基準にプレイヤーへ向けて解き、
// 大区域どうしは結んだ演出（Sequence）を流す。地面を歩いて越えた時は世界を止めて A/D で演出を進める。
#include "Rules/TD2Exploration.h"
#include "TD2CameraRig.h"
#include "TD2RegionComponent.h"
#include "HIKARI/Project/SequenceTypes.h"
#include "HIKARI/Project/WorldView.h"
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace TD2 {
// 区域ごとのテレビの物体・区域カメラ・画面の位置。Session がシーンから集め、カメラと仕掛けの表示が読む。
struct RegionObjects {
    std::map<std::string, HIKARI::GameProjectObjectHandle, std::less<>> televisions;
    std::map<std::string, uint64_t, std::less<>> cameras;
    std::map<std::string, ScreenSurface, std::less<>> geometry;
    // 区域のテレビの物体（無ければ無効な値）。
    HIKARI::GameProjectObjectHandle Television(std::string_view region) const {
        const auto found = televisions.find(region);
        return found == televisions.end() ? HIKARI::GameProjectObjectHandle{} : found->second;
    }
};

class CameraDirector {
public:
    void Reset() { *this = {}; }
    // シーンを出る時：演出を止め、次に入ったシーンで初期化し直す。
    void Stop(HIKARI::WorldView& world);
    // 区域どうしの演出の結び付けと大区域を読み直す。不正な結び付けは使わない。
    void SyncBindings(HIKARI::WorldView& world);
    void SyncAreas(HIKARI::WorldView& world);
    bool Initialized() const noexcept { return initialized_; }
    // 演出の Camera スロットの物体（無ければ入口の区域カメラ）をゲームカメラにし、今の区域から追従を始める。
    void Initialize(HIKARI::WorldView& world, const Rules::Exploration& exploration, const RegionObjects& objects);
    // 手動で演出を押している間は世界を止める。
    bool HoldsWorld() const noexcept { return travel_ && travel_->manual; }
    void AdvanceClock(double seconds) { time_ += seconds; }
    void OnCrossing(HIKARI::WorldView& world, Rules::Exploration& exploration, const RegionObjects& objects,
                    const Rules::RegionTransition& transition);
    // 演出か追従を一フレーム進め、被写界深度の焦点を合わせる。input は A/D の横入力。
    void Update(HIKARI::WorldView& world, Rules::Exploration& exploration, const RegionObjects& objects, float input,
                double frameSeconds, double focusSeconds);
    // 瞬間移動した時：演出を止め、その区域カメラから追従し直す（復活と同じ）。
    void Jump(HIKARI::WorldView& world, const Rules::Exploration& exploration, const RegionObjects& objects, std::string_view region);
    // オープニングが終わった時：最後に映した画（演出のカメラ）から追従を始め、ゲームのカメラへ滑らかに寄せる
    // （演出の終わりの画が追従の位置と少しずれていても、引き継ぐ瞬間に跳ばない）。
    void AdoptView(HIKARI::WorldView& world);
    // 仕掛けが操作中のテレビを動かした時、追従の基準を同じ剛体の動きで運ぶ。
    void Carry(const HIKARI::GameProjectTransform& from, const HIKARI::GameProjectTransform& to) { rig_.Carry(from, to); }
    // 画面を揺らす（強さ 1 で数センチ）。今の揺れより強い時だけ上書きし、すぐに収まっていく。
    void Kick(double strength) { shake_ = (std::max)(shake_, (std::min)(strength, 2.0)); }
    // 見せ場の仕掛け（揺れのある機構）が動いている間に画へ入れたい物（世界の中心と半径 m）。毎フレーム渡す。
    // 渡されている間は向きを変えずに注視点を寄せて後ろへ引き、渡されなくなると少し待ってから元の追従へ戻る。
    void FrameEvent(const HIKARI::GameProjectFloat3& center, float radius) { eventPoints_.push_back({center, radius}); }
    // 見せ場で引いている（戻る途中も含む）。新しく画に入るテレビは砂嵐にしてもらう。
    bool EventFraming() const noexcept { return eventWeight_ > .02 || !eventPoints_.empty(); }

    // 大区域の切り替え中なら、出る部屋と入る部屋。
    bool TravelRooms(std::string& from, std::string& to) const;
    // 同じ大区域の部屋（大区域に入っていなければその部屋だけ）。
    std::vector<std::string> AreaRooms(std::string_view region) const;
    // 部屋の大区域の印（同じ大区域なら同じ値。大区域に入っていなければ部屋の ID）。
    std::string AreaKey(std::string_view region) const {
        const auto key = areaKey_.find(region);
        return key == areaKey_.end() ? std::string(region) : key->second;
    }
    // 記録用：切り替えの部屋（"出る>入る"、無ければ "-"）と進み。
    std::string TravelLabel() const { return travel_ ? travel_->from + ">" + travel_->to : "-"; }
    double TravelProgress() const { return travel_ ? travel_->progress : 0.0; }
    void TraceSequence(HIKARI::WorldView& world, uint64_t frame) const;

private:
    // 大区域の切り替え（演出）。
    struct Travel {
        RegionSequenceBinding binding;
        std::string from, to, pending;      // 自動の切り替え中に先へ進んだ部屋（終わったら追従が引き継ぐ）
        bool reverse = false, manual = false;
        bool hurried = false;               // 見せ場の仕掛けが動き出して演出を速めた
        int direction = 0;                   // 手動で越えた向き（+1 右、-1 左）
        double velocity = 0, progress = 0;   // 手動の進む速さ（演出の長さに対する割合／秒）と進み（0→1）
        double accel = 0;                    // 手動の速さのばねの状態（押し始めと離し際を滑らかにする）
        HIKARI::GameProjectFloat3 eyeOffset{}, lookOffset{};  // 開始時のカメラのずれ（序盤で 0 に戻す）
        // 最後に演出へ足したずれ。終了時に差し引き、二重に足さない。
        HIKARI::GameProjectFloat3 appliedEye{}, appliedLook{};
    };

    bool RegionFrame(HIKARI::WorldView& world, const Rules::Exploration& exploration, const RegionObjects& objects,
                     std::string_view region, CameraRegionFrame& out) const;
    const CameraAreaSettings& AreaOf(std::string_view region) const;
    bool SameArea(std::string_view a, std::string_view b) const;
    void Snap(HIKARI::WorldView& world, const Rules::Exploration& exploration, const RegionObjects& objects, std::string_view region);
    bool StartTravel(HIKARI::WorldView& world, const Rules::Exploration& exploration, const RegionObjects& objects,
                     const std::string& from, const std::string& to, bool manual, int direction);
    void UpdateTravel(HIKARI::WorldView& world, Rules::Exploration& exploration, const RegionObjects& objects, float input, double dt);
    void FinishTravel(HIKARI::WorldView& world, const Rules::Exploration& exploration, const RegionObjects& objects,
                      const std::string& arrived);
    void StopTravel(HIKARI::WorldView& world);
    void UpdateFollow(HIKARI::WorldView& world, const Rules::Exploration& exploration, const RegionObjects& objects, double dt);
    void Focus(HIKARI::WorldView& world, const Rules::Exploration& exploration, const RegionObjects& objects, double seconds);
    HIKARI::GameProjectSequencePlaybackHandle PlayBoundSequence(HIKARI::WorldView& world, const RegionSequenceBinding& binding, float rate);

    std::optional<Travel> travel_;
    CameraRig rig_;
    std::vector<RegionSequenceBinding> bindings_;
    HIKARI::GameProjectSequencePlaybackHandle playback_{};
    // 区域 ID → 大区域の設定と名前（同じ名前なら同じ大区域）。未登録の区域は一部屋で一つの大区域。
    std::map<std::string, CameraAreaSettings, std::less<>> areaSettings_;
    std::map<std::string, std::string, std::less<>> areaKey_;
    HIKARI::GameProjectObjectHandle driver_{};   // ゲームカメラ（演出の Camera スロットと同じ物体）
    bool initialized_ = false;
    double time_ = 0, focusDistance_ = 0;
    double swapBoost_ = 0;   // 交換の直後、区域カメラの乗り換えを速めている残り秒
    double shake_ = 0;       // 画面の揺れの今の強さ
    void Shake(HIKARI::WorldView& world, double seconds);
    // 見せ場の画：このフレームに入れたい物、引き具合（0〜1）、渡されなくなってから戻り始めるまでの残り秒、引いた先の目と注視点。
    struct EventPoint { HIKARI::GameProjectFloat3 center{}; float radius = 0; };
    std::vector<EventPoint> eventPoints_;
    double eventWeight_ = 0, eventHold_ = 0;
    HIKARI::GameProjectFloat3 eventEye_{}, eventLook_{};
    double eventFov_ = 0;   // 振った先の画角（縦、ラジアン）
    bool eventValid_ = false;
    void FrameEventPose(CameraRigPose& pose, double dt);
};
}
