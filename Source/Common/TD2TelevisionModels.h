#pragma once

#include <string_view>

namespace TD2 {
// テレビのモデルごとの筐体寸法と、ローカル空間での画面（単位 m、縮尺前）。
// 区域の画面の位置と大きさ、新しいテレビの格子、カメラ位置・演出の生成はすべてここから決める。
struct TelevisionModel {
    std::string_view stem;                       // モデルの元ファイル名（拡張子なし）
    std::string_view guid;                       // モデル資産の GUID（ModelComponent の assetId）
    float width, height, depth;                  // 筐体（底面中心が原点、画面は +Z 側）
    float screenCenterX, screenCenterY, screenCenterZ, screenWidth, screenHeight;
    std::string_view label;                      // 編集器での名前
    int cols, rows;                              // 新しいテレビの格子（画面の比に合わせる）
};

inline constexpr TelevisionModel kTelevisionModels[]{
    {"SM_L_TV_Steel", "e5a1324c54ef74f37ae319784887244a", 1.08f, .816f, .556f, 0.f, .38f, .266f, .84f, .49f, "鉄のテレビ", 12, 7},
    {"SM_L_TV_Wide", "28aa6fabccdc0527b4833a67e3818076", 1.1f, .856f, .733f, 0.f, .4f, .273f, .84f, .49f, "横長のテレビ", 12, 7},
    {"SM_L_TV_Hub", "e432c800fa5edf88bfa8fdf8a810ab2b", 1.24f, .9563f, .825f, 0.f, .48f, .304f, .96f, .56f, "大きいテレビ", 12, 7},
    {"SM_L_TV_Console", "4cf21b764201efc11657342470d1a69f", 1.16f, 1.f, .55f, 0.f, .64f, .276f, .84f, .49f, "ゲーム機のテレビ", 12, 7},
    {"SM_TD2_TV_FlatStand", "b24723c95921133bed99946ede180b39", 1.4f, 1.009f, .304f, 0.f, .557f, .1369f, 1.2765f, .777f, "薄型", 13, 8},
    {"SM_TD2_TV_Antenna", "aba2d0055edcc94d6cdb99d92cbacbfc", 1.402f, 1.182f, .568f, -.1295f, .4494f, .2559f, 1.0198f, .777f, "アンテナのテレビ", 13, 10},
    {"SM_TD2_Display_Portrait", "db48ed57876894d8627d10e5aaed7e52", 2.004f, 4.f, .356f, 0.f, 2.f, .1784f, 1.6893f, 3.6857f, "縦長の画面", 11, 24},
    {"SM_TD2_Handheld_Classic", "dbfc8576f205313e2789f2db1cbe3d97", .85f, 1.449f, .32f, -.0022f, 1.0225f, .0564f, .4823f, .4194f, "小さい携帯機", 8, 7},
    {"SM_TD2_Handheld_Wide", "f9775fbe1685f63af6069d83fe2ff2d1", 3.74f, 1.424f, .268f, -.005f, .7118f, .0346f, 2.3382f, 1.0792f, "横長の携帯機", 13, 6},
};

// モデル資産の GUID からテレビの型を探す。表に無いモデルは null（区域は既定の画面で動き、検査で知らせる）。
inline const TelevisionModel* FindTelevisionModel(std::string_view guid) {
    for (const auto& model : kTelevisionModels) { if (model.guid == guid) { return &model; } }
    return nullptr;
}
}
