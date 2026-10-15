#pragma once
// build_screen_fx.py が生成する。電視の中の特効（HVFX）の既定資産と演出（画面の揺れ・閃光）の既定値。
// TD2.ScreenFx が無い時もこれを使う。shake は揺れの振幅（格）、flash は画面の明るさの一時的な上乗せ。
#include <array>
#include <string_view>

namespace TD2 {
struct ScreenFxDefault { std::string_view name, label, guid, path; float shake, flash; };
inline constexpr std::array<ScreenFxDefault, 12> kScreenFxDefaults{{
    {"Dash", "瞬移拖尾", "105dfe17eae26654eb473849b8f06cba", "Assets/TD2/Screen3D/Fx/TD2_Fx_Dash.hvfx", 0.00f, 0.00f},
    {"Impact", "冲击", "b12fb32ce19abcb2fdf2ea6c322bd6da", "Assets/TD2/Screen3D/Fx/TD2_Fx_Impact.hvfx", 0.04f, 0.00f},
    {"Land", "落地", "6266c810bfc4cd30a3fef407561a82a2", "Assets/TD2/Screen3D/Fx/TD2_Fx_Land.hvfx", 0.03f, 0.00f},
    {"Bounce", "反弹", "428f4676da445f316151df1bd3238c47", "Assets/TD2/Screen3D/Fx/TD2_Fx_Bounce.hvfx", 0.06f, 0.00f},
    {"EchoForm", "残影生成", "c583b85b271889016e5efe1b52625e64", "Assets/TD2/Screen3D/Fx/TD2_Fx_EchoForm.hvfx", 0.00f, 0.00f},
    {"EchoRecall", "残影收回", "03ba15fa8522fe78b11bac48f693967a", "Assets/TD2/Screen3D/Fx/TD2_Fx_EchoRecall.hvfx", 0.00f, 0.00f},
    {"EchoBlocked", "残影受阻", "9b3335bf7710e1e4c63396d588a26009", "Assets/TD2/Screen3D/Fx/TD2_Fx_EchoBlocked.hvfx", 0.06f, 0.00f},
    {"Death", "死亡", "ac0a58b7025321b79947b30998df75a6", "Assets/TD2/Screen3D/Fx/TD2_Fx_Death.hvfx", 0.18f, 0.60f},
    {"Respawn", "复活", "d8e8272910a844c0f929ecbc736590f1", "Assets/TD2/Screen3D/Fx/TD2_Fx_Respawn.hvfx", 0.00f, 0.35f},
    {"Device", "机关切换", "57454bad91fc903a0e49ccc9428596e5", "Assets/TD2/Screen3D/Fx/TD2_Fx_Device.hvfx", 0.00f, 0.00f},
    {"Checkpoint", "存档点", "8517caa11e8e3aa7997ebd46314a4dc9", "Assets/TD2/Screen3D/Fx/TD2_Fx_Checkpoint.hvfx", 0.00f, 0.45f},
    {"EchoAbsorb", "残影吸收", "a6921ba95e3c069f4f5bcbd0c53c9329", "Assets/TD2/Screen3D/Fx/TD2_Fx_EchoAbsorb.hvfx", 0.00f, 0.00f},
}};
} // namespace TD2
