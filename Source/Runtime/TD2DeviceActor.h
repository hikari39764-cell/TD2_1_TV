#pragma once
// 画面の中の舞台で装置を演じる骨つきの物体の印。物体はシーンに置き（モデルと Animator は作者が付ける）、Play の間だけ
// 装置へ割り当てて舞台へ動かす。状態の変化は Animator の片段（姿勢）を切り替えるだけで、モデルや部品は入れ替えない。
// 割り当てられない物体は退避点に置いたまま、動作の計算も止める。
#include "HIKARI/Project/GameComponent.h"
#include <array>
#include <string_view>

namespace TD2 {
inline constexpr std::string_view kDeviceActorComponentType = "TD2.DeviceActor";

// 演じる装置の型。名前はシーンに保存されるので変えない（並びも変えずに後ろへ足す）。
enum class DeviceActorModel : int {
    PlateHold2 = 0, PlateHoldWorld2, PlateHold1, PlateHoldWorld1, Toggle, ToggleWorld, ResetWorld,
    Spring, Saw, Checkpoint, Gem, Beam, Spark, Door1, Door2, Door3, Bridge2, Goal, KeyAD, KeySpace, KeySpaceHold, ExitArrow, Count
};
inline constexpr std::array<std::string_view, static_cast<size_t>(DeviceActorModel::Count)> kDeviceActorModelNames{
    "踏み板 2 マス", "踏み板 2 マス（外）", "踏み板 1 マス", "踏み板 1 マス（外）", "レバー", "レバー（外）", "戻すボタン（外）",
    "バネ", "丸鋸", "チェックポイント", "収集品", "光線", "光線の火花", "扉 1 マス", "扉 2 マス", "扉 3 マス", "橋 2 マス", "ゴール",
    "キー AD", "キー スペース", "キー 長押し", "出口の矢印"};
// モデルを作った幅（マス）。装置の幅がこれと違えば横だけ伸ばす。
inline constexpr std::array<double, static_cast<size_t>(DeviceActorModel::Count)> kDeviceActorModelWidths{
    2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 1, 1, 1, 1};

class DeviceActorComponent final : public HIKARI::IGameComponent {
public:
    DeviceActorModel Model() const noexcept { return static_cast<DeviceActorModel>(model_); }
    void DescribeProperties(HIKARI::PropertyFields& fields) override {
        fields.Choice("model", "演じる装置", model_, kDeviceActorModelNames);
    }
private:
    int model_ = 0;
};
}
