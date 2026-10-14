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
        f.Float("speed", "播放倍速（1 秒演出 ÷ 倍速 = 实际时长）", binding_.speed, .25f, 4.f);
    }
    RegionSequenceBinding Definition() const { return binding_; }
private:
    RegionSequenceBinding binding_;
};
} // namespace TD2
