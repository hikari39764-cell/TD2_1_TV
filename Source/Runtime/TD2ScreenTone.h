#pragma once
// 画面の中の色調（表示だけ）。人物・仕掛け・危険（鮮やかな色の物）とその縁は変えず、地形・壁・背景の色合いだけを調える。
// 区域の物体に付けると、その部屋を操作している間の色調になる。ほかの物体に付けたものは全部の画面の既定。
#include "HIKARI/Project/GameComponent.h"
#include <string_view>

namespace TD2 {
inline constexpr std::string_view kScreenToneComponentType = "TD2.ScreenTone";

// 既定は何も変えない。
struct ScreenToneSettings {
    HIKARI::GameProjectFloat3 tint{1.0f, 1.0f, 1.0f};   // 染める色（明るさは保ったまま、この色相へ寄せる）
    float tintStrength = 0.0f;
    float hue = 0.0f;           // 色相を回す量（一周 = 1）
    float saturation = 1.0f;
    float brightness = 1.0f;
    float contrast = 1.0f;
};
inline bool SameTone(const ScreenToneSettings& a, const ScreenToneSettings& b) {
    return a.tint.x == b.tint.x && a.tint.y == b.tint.y && a.tint.z == b.tint.z && a.tintStrength == b.tintStrength && a.hue == b.hue &&
           a.saturation == b.saturation && a.brightness == b.brightness && a.contrast == b.contrast;
}

class ScreenToneComponent final : public HIKARI::IGameComponent {
public:
    const ScreenToneSettings& Settings() const noexcept { return settings_; }
    void DescribeProperties(HIKARI::PropertyFields& fields) override {
        fields.Color3("tint", "染める色", settings_.tint);
        fields.Float("tintStrength", "染める強さ", settings_.tintStrength, 0, 1);
        fields.Float("hue", "色相", settings_.hue, -.5f, .5f);
        fields.Float("saturation", "彩度", settings_.saturation, 0, 2);
        fields.Float("brightness", "明るさ", settings_.brightness, 0, 2);
        fields.Float("contrast", "コントラスト", settings_.contrast, 0, 2);
    }
private:
    ScreenToneSettings settings_;
};
}
