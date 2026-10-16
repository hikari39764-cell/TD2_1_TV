#pragma once
// Play 中だけ、ゲームカメラの視野に届かない局所光を一時的に消す。
// 見えない光の照明・影・霧の計算を省く。シーンの値は変えない。
#include "HIKARI/Project/GameComponent.h"
#include "HIKARI/Project/GameSystem.h"
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

namespace TD2 {
inline constexpr std::string_view kLightCullComponentType = "TD2.LightCull";
inline constexpr std::string_view kLightBudgetSystemId = "TD2.LightBudget";

// 光に付ける印。付いた光だけを対象にする。
class LightCullComponent final : public HIKARI::IGameComponent {
public:
    void DescribeProperties(HIKARI::PropertyFields& fields) override {
        fields.Bool("cull", "视野外自动关闭", cull_);
        fields.Float("margin", "提前开启余量（米）", margin_, 0.0f, 20.0f);
        fields.Note("仅在 Play 中生效：灯光照射范围不在游戏镜头视野内时暂时关闭，进入视野前提前打开；不改场景里的数值。");
    }
    bool Cull() const noexcept { return cull_; }
    float Margin() const noexcept { return margin_; }
private:
    bool cull_ = true;
    float margin_ = 2.0f;
};

class LightBudgetSystem final : public HIKARI::IGameSystem {
public:
    void Update(HIKARI::WorldView& world, const HIKARI::FrameContext& frame) override;
    void DescribeSettings(HIKARI::PropertyFields& fields) override { fields.Note(status_); }
private:
    // 光ごとの状態。連続フレーム数で判断し、視野の縁でのちらつきを防ぐ。
    struct Tracked { bool off = false; uint32_t outsideFrames = 0; };
    std::unordered_map<uint64_t, Tracked> lights_;
    std::string status_ = "等待 Play";
};
}
