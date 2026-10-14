#pragma once
#include "TD2Screen3D.h"
#include "HIKARI/Project/GameSystem.h"
#include <string>
#include <vector>

namespace TD2 {
// 性能の記録用：規則の更新（Game system）にかかった CPU 時間を同じフレームの要約へ載せる。
inline double& GameUpdateMilliseconds() { static double value = 0; return value; }

// 電視の中の画面を毎フレーム描く system。編集態と Play の両方で同じ舞台を使い、入力・シミュレーションは進めない。
// Play 中は Session の地図と世界の snapshot を借り、編集中は作者の地図と装置から同じ形の状態を作る。
class ScreenPresentationSystem final : public HIKARI::IGameSystem {
public:
    void Update(HIKARI::WorldView& world, const HIKARI::FrameContext& frame) override;
    void OnWorldDetached(HIKARI::WorldView& world) override { stage_.Release(world); }
    void DescribeSettings(HIKARI::PropertyFields& fields) override { fields.Note(status_); }
private:
    void ReportStatus();
    void ReportTiming(double frameSeconds, double renderSeconds);
    Screen3DStage stage_;
    std::vector<StageRoom> rooms_;
    // 編集中の舞台の状態（作者の装置と、開始区域の出生位置に立つ待機姿勢の主角）。
    Rules::WorldSnapshot authored_;
    std::string status_ = "等待区域3D呈现";
    std::string loggedStatus_;
    // 性能の記録：直近 240 フレームの間隔と電視描画の CPU 時間（ログへ 4 秒ごとに要約だけ出す）。
    std::vector<float> frameMs_, renderMs_, gameMs_;
};
} // namespace TD2
