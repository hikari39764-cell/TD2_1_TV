#pragma once
// build_screen_fx.py で生成。画面内エフェクトの既定の資産と、揺れ・閃光の既定値。
// TD2.ScreenFx が無い時もこれを使う。shake は振幅（マス）、flash は明るさの上乗せ。
#include <array>
#include <string_view>

namespace TD2 {
struct ScreenFxDefault { std::string_view name, label, guid, path; float shake, flash; };
inline constexpr std::array<ScreenFxDefault, 13> kScreenFxDefaults{{
    {"Dash", "瞬間移動", "105dfe17eae26654eb473849b8f06cba", "Assets/TD2/Screen3D/Fx/TD2_Fx_Dash.hvfx", 0.00f, 0.00f},
    {"Impact", "衝撃", "b12fb32ce19abcb2fdf2ea6c322bd6da", "Assets/TD2/Screen3D/Fx/TD2_Fx_Impact.hvfx", 0.04f, 0.00f},
    {"Land", "着地", "6266c810bfc4cd30a3fef407561a82a2", "Assets/TD2/Screen3D/Fx/TD2_Fx_Land.hvfx", 0.03f, 0.00f},
    {"Bounce", "跳ね返り", "428f4676da445f316151df1bd3238c47", "Assets/TD2/Screen3D/Fx/TD2_Fx_Bounce.hvfx", 0.06f, 0.00f},
    {"EchoForm", "残影の生成", "c583b85b271889016e5efe1b52625e64", "Assets/TD2/Screen3D/Fx/TD2_Fx_EchoForm.hvfx", 0.00f, 0.00f},
    {"EchoRecall", "残影の回収", "03ba15fa8522fe78b11bac48f693967a", "Assets/TD2/Screen3D/Fx/TD2_Fx_EchoRecall.hvfx", 0.00f, 0.00f},
    {"EchoBlocked", "残影を出せない", "9b3335bf7710e1e4c63396d588a26009", "Assets/TD2/Screen3D/Fx/TD2_Fx_EchoBlocked.hvfx", 0.06f, 0.00f},
    {"Death", "死亡", "ac0a58b7025321b79947b30998df75a6", "Assets/TD2/Screen3D/Fx/TD2_Fx_Death.hvfx", 0.18f, 0.60f},
    {"Respawn", "復活", "d8e8272910a844c0f929ecbc736590f1", "Assets/TD2/Screen3D/Fx/TD2_Fx_Respawn.hvfx", 0.00f, 0.35f},
    {"Device", "仕掛けの切り替え", "57454bad91fc903a0e49ccc9428596e5", "Assets/TD2/Screen3D/Fx/TD2_Fx_Device.hvfx", 0.00f, 0.00f},
    {"Checkpoint", "チェックポイント", "8517caa11e8e3aa7997ebd46314a4dc9", "Assets/TD2/Screen3D/Fx/TD2_Fx_Checkpoint.hvfx", 0.00f, 0.45f},
    {"EchoAbsorb", "残影の吸収", "a6921ba95e3c069f4f5bcbd0c53c9329", "Assets/TD2/Screen3D/Fx/TD2_Fx_EchoAbsorb.hvfx", 0.00f, 0.00f},
    {"Arrive", "主人公の登場", "5b7e2c9d1f3a4e68b0c4d2a9e7f1c385", "Assets/TD2/Screen3D/Fx/TD2_Fx_Arrive.hvfx", 0.05f, 0.50f},
}};
}
