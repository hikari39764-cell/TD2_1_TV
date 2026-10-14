#pragma once
#include "TD2ScreenFxAssets.h"
#include "HIKARI/Project/GameComponent.h"
#include "HIKARI/Project/VfxTypes.h"
#include <array>
#include <cstddef>
#include <string>
#include <string_view>

namespace TD2 {
inline constexpr std::string_view kScreenFxComponentType = "TD2.ScreenFx";

// 電視の中の事件ごとの特効の槽。順番は kScreenFxDefaults（build_screen_fx.py の生成表）と同じ。
enum class ScreenFxSlot : int { Dash, Impact, Land, Bounce, EchoForm, EchoRecall, EchoBlocked, Death, Respawn, Device, Checkpoint };
inline constexpr size_t kScreenFxSlotCount = kScreenFxDefaults.size();
static_assert(static_cast<size_t>(ScreenFxSlot::Checkpoint) + 1 == kScreenFxSlotCount);

struct ScreenFxEntry {
    HIKARI::GameProjectVfxAssetReference asset{};
    float scale = 1.0f;   // この特効だけの大きさ
    float shake = 0.0f;   // 電視の中の画面の揺れ（振幅、格）
    float flash = 0.0f;   // 画面の一時的な明るさの上乗せ（1 で倍）
};

// 表示だけの設定。Rules の判定や工厂の後処理は変えない。資産を空にした槽は鳴らさない。
struct ScreenFxSettings {
    bool enabled = true;
    bool smear = true;        // 瞬移の時に主角の残像（潰れて消える二体）を置く
    float scale = 1.0f;       // 全体の大きさ（1 格基準）
    // 揺れ全体の倍率。既定は 0（揺れなし）：揺れは舞台カメラをずらすので、部屋の外の隣の地形が捕捉の端に入った
    // 瞬間だけその落ち影が画面の縁に現れ、黒い帯がちらつく（2026-10-14 利用者の判断で試しに切る）。
    float shake = 0.0f;
    float flash = 1.0f;       // 閃光全体の倍率
    std::array<ScreenFxEntry, kScreenFxSlotCount> entries{};
    ScreenFxSettings() {
        for (size_t i = 0; i < entries.size(); ++i) {
            Copy(entries[i].asset.assetGuid, kScreenFxDefaults[i].guid);
            Copy(entries[i].asset.assetPath, kScreenFxDefaults[i].path);
            entries[i].shake = kScreenFxDefaults[i].shake;
            entries[i].flash = kScreenFxDefaults[i].flash;
        }
    }

private:
    template <size_t N> static void Copy(char (&target)[N], std::string_view text) {
        const size_t count = text.size() < N - 1 ? text.size() : N - 1;
        for (size_t i = 0; i < count; ++i) { target[i] = text[i]; }
        target[count] = '\0';
    }
};

// 舞台カメラ（屏幕风格と同じ物体）に一つ付ける。無ければ既定の一式を使う。
class ScreenFxComponent final : public HIKARI::IGameComponent {
public:
    const ScreenFxSettings& Settings() const noexcept { return settings_; }
    void DescribeProperties(HIKARI::PropertyFields& fields) override {
        fields.Bool("enabled", "显示电视内特效", settings_.enabled);
        fields.Float("scale", "整体大小", settings_.scale, .25f, 3);
        fields.Float("shake", "画面震动倍率", settings_.shake, 0, 3);
        fields.Float("flash", "画面闪光倍率", settings_.flash, 0, 3);
        fields.Bool("smear", "瞬移残像", settings_.smear);
        if (fields.IsInspecting()) {
            fields.Note("每个游戏事件一个 VFX 资产（可换，也可在 VFX 工作区直接修改），并可单独调大小、震动、闪光。");
        }
        // 槽ごとに折り畳み。Load/Save では Section は常に true なので中身は必ず読み書きされる。
        for (size_t i = 0; i < kScreenFxSlotCount; ++i) {
            const auto& item = kScreenFxDefaults[i];
            const std::string name(item.name), label(item.label);
            auto& entry = settings_.entries[i];
            if (fields.Section(label)) {
                fields.Vfx("vfx" + name, "特效资产", entry.asset);
                fields.Float("scale" + name, "大小", entry.scale, .25f, 3);
                fields.Float("shake" + name, "画面震动（格）", entry.shake, 0, .5f);
                fields.Float("flash" + name, "画面闪光", entry.flash, 0, 1.5f);
            }
        }
        if (fields.IsInspecting() && fields.Button("全部恢复默认特效")) { settings_ = ScreenFxSettings{}; }
    }

private:
    ScreenFxSettings settings_;
};
} // namespace TD2
