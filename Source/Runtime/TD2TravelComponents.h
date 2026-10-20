#pragma once

#include "../Common/TD2WorldLayout.h"
#include "HIKARI/Project/GameComponent.h"

namespace TD2 {
// 隣り合う区域の切り替え演出（Sequence 資産）を結ぶ。接続は Region の配置から決める。
// 演出の中身はエンジンの演出ワークスペースで作る。
class RegionSequenceComponent final : public HIKARI::IGameComponent {
public:
    void DescribeProperties(HIKARI::PropertyFields& f) override {
        f.Text("regionA", "正向起点电视", binding_.regionA);
        f.Text("regionB", "正向终点电视", binding_.regionB);
        f.AssetId("sequenceAssetGuid", "引擎演出", HIKARI::GameProjectAssetKind::Sequence, binding_.sequenceAssetGuid);
        f.Text("sequenceSlotName", "摄像机槽（可选）", binding_.sequenceSlotName);
        f.SceneObject("cameraObject", "槽对应摄像机（可选）", binding_.cameraObject);
        f.Float("speed", "播放倍速（1 秒演出 ÷ 倍速 = 实际时长）", binding_.speed, .1f, 4.f);
        // 以下はエディタの自動生成用（実行時は使わない）。世界地図で辺を選ぶと編集できる。
        f.Text("style", "生成样式（auto/reveal/arc/low/pull/hug）", style_);
        f.Float("seconds", "全速时长（秒）", seconds_, .4f, 10.f);
        f.Bool("keepAuthored", "保留手调（重新生成时不覆盖）", keepAuthored_);
        // 自動生成したキーの記録。今のキーと違えば手で直したとみなし、再生成で残す。
    }
    RegionSequenceBinding Definition() const { return binding_; }
private:
    RegionSequenceBinding binding_;
    std::string style_ = "auto", autoHash_;
    float seconds_ = 3.f;
    bool keepAuthored_ = false;
};
}
