#pragma once

#include <string_view>

namespace TD2 {
// テレビのモデルごとの筐体寸法と、ローカル空間での画面（単位 m、縮尺前）。
// 区域の画面寸法の自動設定、カメラ位置・演出の生成、編集器のテレビの新規作成に使う。
// 表に無いモデルは区域に入力された値を使う。
struct TelevisionModel {
    std::string_view stem;                       // モデルの元ファイル名（拡張子なし）
    float width, height, depth;                  // 筐体（底面中心が原点、画面は +Z 側）
    float screenCenterX, screenCenterY, screenCenterZ, screenWidth, screenHeight;
    std::string_view path;                       // 資産の元パスの一部（新規作成に使う）
    std::string_view label;                      // 編集器での名前
    int cols, rows;                              // 新規作成する時の格子（画面の比に合わせる）
    float scale;                                 // 新規作成する時の縮尺
};

inline constexpr TelevisionModel kTelevisionModels[]{
    {"SM_L_TV_Steel", 1.08f, .816f, .556f, 0.f, .38f, .266f, .84f, .49f, "LevelSet01/SM_L_TV_Steel.fbx", "钢壳电视", 12, 7, 1.f},
    {"SM_L_TV_Wide", 1.1f, .856f, .733f, 0.f, .4f, .273f, .84f, .49f, "LevelSet01/SM_L_TV_Wide.fbx", "宽电视", 12, 7, 1.f},
    {"SM_L_TV_Hub", 1.24f, .9563f, .825f, 0.f, .48f, .304f, .96f, .56f, "LevelSet01/SM_L_TV_Hub.fbx", "大电视", 12, 7, 1.f},
    {"SM_L_TV_Console", 1.16f, 1.f, .55f, 0.f, .64f, .276f, .84f, .49f, "LevelSet01/SM_L_TV_Console.fbx", "主机电视", 12, 7, 1.f},
    {"SM_TD2_TV_FlatStand", 1.4f, 1.009f, .304f, 0.f, .557f, .1369f, 1.2765f, .777f, "ExternalDisplays01/SM_TD2_TV_FlatStand.fbx", "平板", 13, 8, .8f},
    {"SM_TD2_TV_Antenna", 1.402f, 1.182f, .568f, -.1295f, .4494f, .2559f, 1.0198f, .777f, "ExternalDisplays01/SM_TD2_TV_Antenna.fbx", "天线电视", 13, 10, .85f},
    {"SM_TD2_Display_Portrait", 2.004f, 4.f, .356f, 0.f, 2.f, .1784f, 1.6893f, 3.6857f, "ExternalDisplays01/SM_TD2_Display_Portrait.fbx", "竖屏", 11, 24, .45f},
    {"SM_TD2_Handheld_Classic", .85f, 1.449f, .32f, -.0022f, 1.0225f, .0564f, .4823f, .4194f, "ExternalDisplays01/SM_TD2_Handheld_Classic.fbx", "小掌机", 8, 7, .5f},
    {"SM_TD2_Handheld_Wide", 3.74f, 1.424f, .268f, -.005f, .7118f, .0346f, 2.3382f, 1.0792f, "ExternalDisplays01/SM_TD2_Handheld_Wide.fbx", "宽掌机", 13, 6, .55f},
};

// 資産の元パスからモデルを探す。
inline const TelevisionModel* FindTelevisionModel(std::string_view sourcePath) {
    const auto slash = sourcePath.find_last_of("/\\");
    auto name = slash == std::string_view::npos ? sourcePath : sourcePath.substr(slash + 1);
    if (const auto dot = name.find('.'); dot != std::string_view::npos) { name = name.substr(0, dot); }
    for (const auto& model : kTelevisionModels) { if (model.stem == name) { return &model; } }
    return nullptr;
}
}
