#pragma once

#include "../Common/TD2WorldLayout.h"
#include "HIKARI/Project/GameComponent.h"

namespace TD2 {
// 隣り合う区域の切り替え演出（引擎の Sequence 資産）を結ぶ。空間の接続は Region の配置から求め、ここに通行許可を置かない。
// 演出の中身（镜头关键帧）は引擎の演出工作区で作る。
class RegionSequenceComponent final : public HIKARI::IGameComponent {
public:
    void DescribeProperties(HIKARI::PropertyFields& f) override {
        f.Text("regionA", "正向起点电视", binding_.regionA);
        f.Text("regionB", "正向终点电视", binding_.regionB);
        f.AssetId("sequenceAssetGuid", "引擎演出", HIKARI::GameProjectAssetKind::Sequence, binding_.sequenceAssetGuid);
        f.Text("sequenceSlotName", "摄像机槽（可选）", binding_.sequenceSlotName);
        f.SceneObject("cameraObject", "槽对应摄像机（可选）", binding_.cameraObject);
        f.Float("speed", "播放倍速（1 秒演出 ÷ 倍速 = 实际时长）", binding_.speed, .1f, 4.f);
        // 以下は編集器の自動生成だけが読む（実行時は使わない）。世界地図で辺を選ぶと、これらを画面で編集できる。
        f.Text("style", "生成样式（auto/reveal/arc/low/pull/hug）", style_);
        f.Float("seconds", "全速时长（秒）", seconds_, .4f, 10.f);
        f.Bool("keepAuthored", "保留手调（重新生成时不覆盖）", keepAuthored_);
        // 自動で書いた演出の鍵の記録（编辑器が書く）。今の鍵と違えば作者が演出工作区で直したとみなし、重新生成で残す。
        f.Text("autoHash", "自动生成记录（自动）", autoHash_);
    }
    RegionSequenceBinding Definition() const { return binding_; }
private:
    RegionSequenceBinding binding_;
    std::string style_ = "auto", autoHash_;
    float seconds_ = 3.f;
    bool keepAuthored_ = false;
};
} // namespace TD2
