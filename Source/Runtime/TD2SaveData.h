#pragma once
// 遊びの進みと設定を、保存の枠（SaveGameView）へ書く・読む。形式は一行一項目の文字列で、先頭の行が形式と版、
// 最後の行が項目の数（途中で切れた書き込みを読まない）。
#include "Rules/TD2Exploration.h"
#include "HIKARI/Project/WorldView.h"
#include <string>
#include <string_view>

namespace TD2 {
// 遊ぶ人の設定。音量は 0〜10 の段（10 が作者の調整そのまま）。
struct UserSettings {
    int bgm = 10, se = 10, ambient = 10;
    int windowMode = 0;    // 0 ウィンドウ、1 ボーダーレス、2 フルスクリーン
    int resolution = 1;    // 0 1280×720、1 1920×1080、2 2560×1440、3 3840×2160
    bool vsync = false;
    bool operator==(const UserSettings&) const = default;
};

// 進みと設定の保存の枠の名前。検証の無頭実行は環境変数 TD2_SAVE_SLOT で別の枠（設定は「…_settings」）を使い、
// 遊ぶ人の保存を書き換えない。
std::string_view ProgressSlot();
std::string_view SettingsSlot();

std::string WriteProgressText(const Rules::Progress& progress);
bool ReadProgressText(std::string_view text, Rules::Progress& out);
std::string WriteSettingsText(const UserSettings& settings);
bool ReadSettingsText(std::string_view text, UserSettings& out);

// 枠の読み書き。読めない・無い時は false。
bool LoadSlot(HIKARI::WorldView& world, std::string_view slot, std::string& out);
bool StoreSlot(HIKARI::WorldView& world, std::string_view slot, const std::string& text);
void DeleteSlot(HIKARI::WorldView& world, std::string_view slot);
}
