#pragma once

#include "TD2RegionData.h"
#include <string>
#include <string_view>

namespace TD2::SpritePalette {

// Manifest のフレームキーを、SDK が受け取る独立 PNG のプロジェクト相対パスへ変換する。
inline std::string FramePath(std::string_view key) {
    std::string path = "Assets/TD2/Sprites04/frames/";
    path.reserve(path.size() + key.size() + 4u);
    for (const char c : key) { path.push_back(c == '.' ? '_' : c); }
    path += ".png";
    return path;
}

// 提供素材は全て色を焼き込み済みなので、描画時の RGB は白に固定する。
inline constexpr RegionColor kSpriteTint{1.0f, 1.0f, 1.0f, 1.0f};
// 出現待ちと残像に限り、描画時の不透明度だけを下げる。
inline constexpr RegionColor kPendingSpriteTint{1.0f, 1.0f, 1.0f, 0.42f};

} // namespace TD2::SpritePalette
