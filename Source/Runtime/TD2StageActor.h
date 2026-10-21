#pragma once
// 画面の中の舞台に立つ骨つきの人形の印（主角・残影）。人形の物体はシーンに置き、モデルと Animator は作者が付ける。
// Play だけで使い、置き場所・向き・動作は毎フレーム規則の状態から決める。人形が無い時と編集中は、焼いた静止の形を使う。
#include "HIKARI/Project/GameComponent.h"
#include <string_view>

namespace TD2 {
inline constexpr std::string_view kStageActorComponentType = "TD2.StageActor";

enum class StageActorRole : int { Player = 0, Echo = 1 };

struct StageActorSettings {
    int role = 0;
    // モデルの立ち姿の高さ（モデルの単位）。人物の見た目の高さをこれで割って縮尺にする。
    float height = 1.40f;
    // 横向きから手前（カメラ）へ振る角度（度）。0 なら真横。
    float turn = 10.0f;
};

class StageActorComponent final : public HIKARI::IGameComponent {
public:
    const StageActorSettings& Settings() const noexcept { return settings_; }
    void DescribeProperties(HIKARI::PropertyFields& fields) override {
        // 名前はシーンに保存されるので変えない。並びは StageActorRole の順。
        static constexpr std::string_view kRoles[]{"プレイヤー", "残影"};
        fields.Choice("role", "役", settings_.role, kRoles);
        fields.Float("height", "モデルの背丈", settings_.height, .1f, 10.0f);
        fields.Float("turn", "カメラへ向ける角度", settings_.turn, 0.0f, 60.0f);
    }
private:
    StageActorSettings settings_;
};
}
