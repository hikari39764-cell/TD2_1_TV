#pragma once
// 灯光の予算：Play 中だけ、游戏カメラの視野に影響範囲（光の届く球）が掛からない局所光を一時的に消す。
// 主な照明は場景に残したまま（作者の値は変えない）で、見えない光の陰影計算・影の描画・霧の散乱を省く。
// 変更は実行時だけのパッチで、Play を止めると場景の値へ戻る。編集中は何もしない。
#include "HIKARI/Project/GameComponent.h"
#include "HIKARI/Project/GameSystem.h"
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

namespace TD2 {
inline constexpr std::string_view kLightCullComponentType = "TD2.LightCull";
inline constexpr std::string_view kLightBudgetSystemId = "TD2.LightBudget";

// 光の物体に付ける印。付いた光だけを予算の対象にする（付いていない光は常に場景のまま）。
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
    // 光ごとの状態。消してからの連続フレーム数で点け直しを判断し、視野の縁でのちらつきを防ぐ。
    struct Tracked { bool off = false; uint32_t outsideFrames = 0; };
    std::unordered_map<uint64_t, Tracked> lights_;
    std::string status_ = "等待 Play";
};
} // namespace TD2
