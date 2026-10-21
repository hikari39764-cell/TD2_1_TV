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

// 画面内のイベントごとのエフェクトスロット。順番は kScreenFxDefaults と同じ。
enum class ScreenFxSlot : int { Dash, Impact, Land, Bounce, EchoForm, EchoRecall, EchoBlocked, Death, Respawn, Device, Checkpoint, EchoAbsorb, Arrive };
inline constexpr size_t kScreenFxSlotCount = kScreenFxDefaults.size();
static_assert(static_cast<size_t>(ScreenFxSlot::Arrive) + 1 == kScreenFxSlotCount);

struct ScreenFxEntry {
    HIKARI::GameProjectVfxAssetReference asset{};
    float scale = 1.0f;   // このエフェクトの大きさ
    float shake = 0.0f;   // 画面の揺れ（振幅、マス）
    float flash = 0.0f;   // 画面の一時的な明るさの上乗せ（1 で倍）
};

// 表示だけの設定。ルールや工場の後処理は変えない。資産が空のスロットは出さない。
struct ScreenFxSettings {
    bool enabled = true;
    bool smear = true;        // 瞬間移動の時にプレイヤーの残像を置く
    float scale = 1.0f;       // 全体の大きさ（1 マス基準）
    // 揺れ全体の倍率。既定は 0（舞台カメラがずれて画面の縁がちらつくため）。
    float shake = 0.0f;
    float flash = 0.0f;       // 閃光全体の倍率
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

// 舞台カメラ（画面スタイルと同じ物体）に一つ付ける。無ければ既定値を使う。
class ScreenFxComponent final : public HIKARI::IGameComponent {
public:
    const ScreenFxSettings& Settings() const noexcept { return settings_; }
    void DescribeProperties(HIKARI::PropertyFields& fields) override {
        fields.Bool("enabled", "テレビのエフェクト", settings_.enabled);
        fields.Float("scale", "全体の大きさ", settings_.scale, .25f, 3);
        fields.Float("shake", "画面の振動", settings_.shake, 0, 3);
        fields.Float("flash", "画面の閃光", settings_.flash, 0, 3);
        fields.Bool("smear", "ダッシュ残像", settings_.smear);

        // スロットごとに折り畳む（Load/Save では常に中身を読み書きする）。
        for (size_t i = 0; i < kScreenFxSlotCount; ++i) {
            const auto& item = kScreenFxDefaults[i];
            const std::string name(item.name), label(item.label);
            auto& entry = settings_.entries[i];
            if (fields.Section(label)) {
                fields.Vfx("vfx" + name, "特效资产", entry.asset);
                fields.Float("scale" + name, "大きさ", entry.scale, .25f, 3);
                fields.Float("shake" + name, "画面の振動", entry.shake, 0, .5f);
                fields.Float("flash" + name, "画面の閃光", entry.flash, 0, 1.5f);
            }
        }
        if (fields.IsInspecting() && fields.Button("デフォルト")) { settings_ = ScreenFxSettings{}; }
    }

private:
    ScreenFxSettings settings_;
};
}
