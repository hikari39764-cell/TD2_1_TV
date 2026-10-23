#pragma once
#include "HIKARI/Project/GameComponent.h"
#include <string>
#include <string_view>

namespace TD2 {
inline constexpr std::string_view kSignalCableComponentType = "TD2.SignalCable";

// 画面の外の信号線。つながった圧力板が通電している間は材質を光らせる（踏んだ板と、それが動かす転台・扉の関係を
// 3D の画でも見せる）。モデルの材質セットが材質の上書きを作り、実行時はその色と発光だけを変える。モデルは入れ替えない。
class SignalCableComponent final : public HIKARI::IGameComponent {
public:
    void DescribeProperties(HIKARI::PropertyFields& fields) override {
        fields.Text("plate", "つなぐ圧力板", plate_);
        // 編集器が自動で引いた線の行き先（引き直しで消す組を見分ける）。実行時は読まない。
        fields.Text("target", "つながる仕掛け", target_);
    }
    const std::string& Plate() const noexcept { return plate_; }

private:
    std::string plate_, target_;
};
}
