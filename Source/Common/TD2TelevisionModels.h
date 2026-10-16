#pragma once

#include <string_view>

namespace TD2 {
// テレビのモデルごとの筐体寸法と、ローカル空間での画面（単位 m、縮尺前）。
// 区域の画面寸法の自動設定と、カメラ位置・演出の生成に使う。
// 表に無いモデルは区域に入力された値を使う。
struct TelevisionModel {
    std::string_view stem;                       // モデルの元ファイル名（拡張子なし）
    float width, height, depth;                  // 筐体（底面中心が原点）
    float screenCenterX, screenCenterY, screenCenterZ, screenWidth, screenHeight;
};

inline constexpr TelevisionModel kTelevisionModels[]{
    {"SM_L_TV_Hub", 1.24f, .9563f, .825f, 0.f, .48f, .304f, .96f, .56f},
    {"SM_L_TV_Wide", 1.1f, .856f, .733f, 0.f, .4f, .273f, .84f, .49f},
    {"SM_L_TV_Steel", 1.08f, .816f, .556f, 0.f, .38f, .266f, .84f, .49f},
    {"SM_L_TV_Console", 1.16f, 1.f, .55f, 0.f, .64f, .276f, .84f, .49f},
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
