#pragma once

#include "../Common/TD2WorldLayout.h"
#include "HIKARI/Project/GameComponent.h"

namespace TD2 {
// 隣り合う区域の切り替え演出（Sequence 資産）を結ぶ。接続は Region の配置から決める。
// 演出の中身はエンジンの演出ワークスペースで作る。
class RegionSequenceComponent final : public HIKARI::IGameComponent {
public:
    void DescribeProperties(HIKARI::PropertyFields& f) override {
        f.Text("regionA", "始点のテレビ", binding_.regionA);
        f.Text("regionB", "終点のテレビ", binding_.regionB);
        f.AssetId("sequenceAssetGuid", "演出", HIKARI::GameProjectAssetKind::Sequence, binding_.sequenceAssetGuid);
        f.Text("sequenceSlotName", "カメラスロット", binding_.sequenceSlotName);
        f.SceneObject("cameraObject", "スロットのカメラ", binding_.cameraObject);
        f.Float("speed", "再生速度", binding_.speed, .1f, 4.f);
        f.Bool("autoPlay", "自動で流す", binding_.autoPlay);
        // 以下はエディタの自動生成用（実行時は使わない）。世界地図で辺を選ぶと編集できる。
        f.Text("style", "生成の型", style_);
        f.Float("seconds", "秒数", seconds_, .4f, 10.f);
        f.Bool("keepAuthored", "手直しを残す", keepAuthored_);
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
