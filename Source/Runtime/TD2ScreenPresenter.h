#pragma once
#include "TD2Screen3D.h"
#include "HIKARI/Project/GameSystem.h"
#include <string>
#include <vector>

namespace TD2 {
// 性能記録用：ルール更新の CPU 時間を同じフレームの集計に載せる。
inline double& GameUpdateMilliseconds() { static double value = 0; return value; }

// テレビの画面を毎フレーム描く。編集中と Play で同じ舞台を使い、シミュレーションは進めない。
// Play 中は Session の状態を、編集中は作者の地図から作った状態を描く。
class ScreenPresentationSystem final : public HIKARI::IGameSystem {
public:
    void Update(HIKARI::WorldView& world, const HIKARI::FrameContext& frame) override;
    void OnWorldDetached(HIKARI::WorldView& world) override { intro_.Stop(world); stage_.Release(world); }
    void DescribeSettings(HIKARI::PropertyFields& fields) override { fields.Note(status_); }
private:
    void ReportStatus();
    void ReportTiming(double frameSeconds, double renderSeconds);
    Screen3DStage stage_;
    // 編集中：演出ワークスペースがオープニングの演出をプレビューしている間、その時刻の灯・画面・小物を出す。
    IntroDirector intro_;
    std::vector<StageRoom> rooms_;
    // 編集中の舞台の状態（装置と、スポーン位置に立つプレイヤー）。
    Rules::WorldSnapshot authored_;
    std::string status_ = "区域の 3D 表示を待っている";
    std::string loggedStatus_;
    // 性能記録：直近 240 フレームの間隔と描画の CPU 時間（4 秒ごとにログへ出す）。
    std::vector<float> frameMs_, renderMs_, gameMs_;
};
}
