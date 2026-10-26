#pragma once

#include "Rules/TD2Exploration.h"
#include "TD2CameraDirector.h"
#include "TD2MachineViews.h"
#include "TD2Intro.h"
#include "TD2Menus.h"
#include "TD2Sound.h"
#include "../Common/TD2PlayRecord.h"
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
    // Play を止めた時に試遊の記録を書き終える。
    void OnGameStopped() override { FinishRecord(); }
    void Update(HIKARI::WorldView& world, const HIKARI::FrameContext& frame);
    const RegionRuntime* FindRegion(std::string_view id) const { return exploration_.FindRegion(id); }
    const std::string& ActiveRegion() const noexcept { return exploration_.ActiveRegion(); }
    // 画面の外の機構（どの部屋を回すかを画面の舞台が読む）。
    const std::vector<Rules::MechanismRuntime>& Mechanisms() const noexcept { return exploration_.Mechanisms(); }
    // 機構で動かす物体の置いたままの姿勢（回す部屋のテレビの元の傾きを画面の舞台が読む）。
    // 大区域の切り替え中なら、出る部屋と入る部屋（画面の鮮明な窓と保護を優先する）。
    bool TravelRooms(std::string& from, std::string& to) const { return camera_.TravelRooms(from, to); }
    // 世界座標の表示状態（画面の舞台が読む）。
    const Rules::WorldSnapshot& Snapshot() { return exploration_.Snapshot(); }
    // 同じ大区域の部屋。画面の舞台の周りの窓を大区域ごとに固定するのに使う。
    std::vector<std::string> AreaRooms(std::string_view region) const { return camera_.AreaRooms(region); }
    // オープニングの演出の画面の状態（タイトルと砂嵐）。オープニングが無い・終わった後は active が false。
    const IntroScreens& Intro() const noexcept { return intro_.Screens(); }
    // 窓（タイトルのメール・ESC の窓）の画布を貼っているテレビ（ハンドルの値。無ければ 0）。舞台はこの画面を結ばない。
    uint64_t UiScreen() const noexcept { return menu_.CanvasScreen(); }
    // 機構で動いている部屋（画面を砂嵐にする）と、見せ場でカメラが引いているか（新しく画に入るテレビも砂嵐にする）。
    std::vector<std::string> SnowRooms() const { return machines_.SnowRooms(); }
    bool EventFraming() const noexcept { return camera_.EventFraming(); }

private:
    void SyncRegions(HIKARI::WorldView& world);
    // ゴールの扉に入った：試遊の記録を締め、通関の演出を始める（演出が無ければすぐタイトル待ちへ戻す）。
    void StartEnding(HIKARI::WorldView& world, const std::string& region);
    // 通関の後：世界を最初の状態へ戻し（仕掛けのテレビも最初の位置へ）、タイトル待ちから始める。
    void RestartFromTitle(HIKARI::WorldView& world);
    // 開発用（O）：ゴールの扉のある部屋の出生点へ移す。
    void TeleportToGoal(HIKARI::WorldView& world);
    // 世界を作者の最初の状態へ戻す（仕掛けのテレビも最初の位置へ。カメラも付け直す）。
    void ResetWorld(HIKARI::WorldView& world);
    // 保存した進みへ戻して遊びを始める（タイトル待ちは飛ばす）。戻せなければその保存を消し、最初の世界のタイトル待ちへ。
    bool ResumeProgress(HIKARI::WorldView& world, const Rules::Progress& progress);
    // 今の進みを保存の枠へ書く（チェックポイントに触れた時と、やめる前に保存を選んだ時）。
    void WriteProgress(HIKARI::WorldView& world);
    // 開発用の巡回（TD2_TOUR）の進み。
    uint64_t tourFrame_ = 0; size_t tourIndex_ = 0;
    void Trace(HIKARI::WorldView& world, uint64_t frame);
    void FinishRecord();

    Rules::Exploration exploration_;
    RegionObjects regions_;
    CameraDirector camera_;
    MachineViews machines_;
    IntroDirector intro_;
    SoundDirector sound_;
    MenuDirector menu_;
    // 起動時に読んだ続き（問いに答えるまで持つ）。
    std::optional<Rules::Progress> pendingProgress_;
    // 進みを保存するか（「この画面から」や開発用の入口、巡回では遊ぶ人の保存を書き換えない）。
    bool saving_ = false, checkpointTouched_ = false;
    bool introChecked_ = false, ending_ = false;
    // 世界を作った時の区域と仕掛け（通関の後に最初の状態へ戻すため、作者の配置のまま取っておく）。
    std::vector<Rules::RegionSource> initialSources_;
    MachineSources initialMachines_;
    // エディタからの一回限りの入口。保存データには戻さない。
    uint64_t previewEntryObject_ = 0;
    std::optional<Rules::Vec2> previewSpawn_;
    std::string previewEntryRegion_;
    std::vector<std::pair<std::string, int>> previewStates_;
    // 試遊の記録（途中は一時ディレクトリへ少しずつ書き足し、止めた時に終わりの行を書く）。
    PlayRecord record_;
    PlayRecordCursor recordCursor_;
    bool recording_ = false, recordClosed_ = false;
    uint32_t recordFrame_ = 0;
    std::string recordRegion_;
    bool sceneActive_ = false, leftHeld_ = false, rightHeld_ = false;
    double traceSeconds_ = 0;
    uint64_t lastFrame_ = (std::numeric_limits<uint64_t>::max)();
};
}
