#pragma once
// 画面の中の舞台を照らす灯の印（受光の組を使う時だけ）。Play 中、写っている部屋ごとに役ごとの灯を一つずつ置き直す。
// 明るさ・色・影は灯の部品の値をそのまま使い、ここでは置き場所と点け消しだけを決める。
#include "HIKARI/Project/GameComponent.h"
#include <string_view>

namespace TD2 {
inline constexpr std::string_view kStageLightComponentType = "TD2.StageLight";

// 役：主光（左上の手前から、影を落とす）、補光（右手前の弱い光）、背光（奥の上から手前へ、縁を光らせる）。
enum class StageLightRole : int { Key = 0, Fill = 1, Back = 2 };
inline constexpr int kStageLightRoleCount = 3;

struct StageLightSettings {
    int role = 0;
    // 部屋の正面の中心からの置き場所。x・y は部屋の半分の幅・高さを 1 とした割合（上が正）、z は手前へのマス数。
    float offsetX = -.5f, offsetY = 1.2f, offsetZ = 10.0f;
    // 照らす所：部屋の正面の中心から奥へのマス数。
    float aimDepth = 1.5f;
};

class StageLightComponent final : public HIKARI::IGameComponent {
public:
    const StageLightSettings& Settings() const noexcept { return settings_; }
    void DescribeProperties(HIKARI::PropertyFields& fields) override {
        // 名前はシーンに保存されるので変えない。並びは StageLightRole の順。
        static constexpr std::string_view kRoles[]{"主光", "补光", "背光"};
        fields.Choice("role", "キャラクター", settings_.role, kRoles);
        fields.Float("offsetX", "横の位置", settings_.offsetX, -3, 3);
        fields.Float("offsetY", "縦の位置", settings_.offsetY, -3, 3);
        fields.Float("offsetZ", "手前への距離", settings_.offsetZ, -12, 30);
        fields.Float("aimDepth", "部屋の照らす深さ", settings_.aimDepth, 0, 12);
        fields.Note("ライトのコンポーネント：舞台の部屋を照らす灯の置き場所。主光・補光・背光の三つを置く。");
    }
private:
    StageLightSettings settings_;
};
}
