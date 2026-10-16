#pragma once
#include "HIKARI/Project/GameComponent.h"
#include <string>
#include <string_view>

namespace TD2 {
inline constexpr std::string_view kSignalCableComponentType = "TD2.SignalCable";

// 画面の外の信号線。つながった圧力板が通電している間だけ、光る版のモデルに替える
// （踏んだ板と、それが動かす転台・扉の関係を 3D の画でも見せる）。
class SignalCableComponent final : public HIKARI::IGameComponent {
public:
    void DescribeProperties(HIKARI::PropertyFields& fields) override {
        fields.Text("plate", "连接的压板（区域ID/装置ID）", plate_);
        fields.Text("litModel", "通电时的模型 GUID", litModel_);
    }
    const std::string& Plate() const noexcept { return plate_; }
    const std::string& LitModel() const noexcept { return litModel_; }

private:
    std::string plate_, litModel_;
};
}
