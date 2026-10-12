#pragma once

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace TD2 {

inline constexpr std::string_view kRegionComponentType = "TD2.Region";
inline constexpr std::string_view kScreenSystemId = "TD2.ScreenPresentation";
inline constexpr int kDefaultTilePixels = 64;

// ルールはこの列挙と整数の格座標を使う。tilePixels は表示専用で、距離・衝突に使わない。
enum class TileKind : uint8_t {
    Empty = 0,
    Solid = 1,
};

// 作者データだけを保持する。テレビの大きさや実行中の WorldState はここへ混ぜない。
// cells は左上原点の行優先配列で、0=空、1=テスト用ブロック。初回は 4×3 とする。
struct RegionDefinition {
    std::string regionId = "td2.region.test";
    int cols = 4;
    int rows = 3;
    int tilePixels = kDefaultTilePixels;
    std::string cells = "000000001111";

    bool operator==(const RegionDefinition&) const = default;
};

struct RegionValidation {
    bool valid = false;
    std::string_view message{};
};

inline RegionValidation ValidateRegion(const RegionDefinition& region) noexcept {
    if (region.regionId.empty() || region.regionId.size() > 95) {
        return {false, "区域 ID 必须为 1～95 个字符"};
    }
    for (const char c : region.regionId) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) {
            return {false, "区域 ID 仅允许英文字母、数字、点、下划线和连字符"};
        }
    }
    if (region.cols <= 0 || region.rows <= 0 || region.tilePixels <= 0) {
        return {false, "行、列和格子像素必须为正整数"};
    }
    // 掛け算は先に拡張し、不正な寸法でも符号付き整数をあふれさせない。
    const auto count = static_cast<uint64_t>(region.cols) * static_cast<uint64_t>(region.rows);
    if (count != region.cells.size()) {
        return {false, "格数据数量必须等于 cols × rows；可明确重建测试网格"};
    }
    const auto width = static_cast<uint64_t>(region.cols) * static_cast<uint64_t>(region.tilePixels);
    const auto height = static_cast<uint64_t>(region.rows) * static_cast<uint64_t>(region.tilePixels);
    if (width > (std::numeric_limits<uint32_t>::max)() ||
        height > (std::numeric_limits<uint32_t>::max)()) {
        return {false, "区域逻辑像素尺寸超出可表示范围"};
    }
    for (const char cell : region.cells) {
        if (cell != '0' && cell != '1') {
            return {false, "首批格数据仅支持 0（空）与 1（测试块）"};
        }
    }
    return {true, {}};
}

inline size_t CellIndex(const RegionDefinition& region, int col, int row) noexcept {
    return static_cast<size_t>(row) * static_cast<size_t>(region.cols) + static_cast<size_t>(col);
}

// シーンの簡潔な文字列保存形式をここで解釈し、ルール側に文字コードの知識を持たせない。
// 範囲外を空と決め付けず false を返す。境界の扱いは後続のルール契約に従う。
inline bool TryGetTile(const RegionDefinition& region, int col, int row, TileKind& tile) noexcept {
    if (col < 0 || row < 0 || col >= region.cols || row >= region.rows) { return false; }
    const size_t index = CellIndex(region, col, row);
    if (index >= region.cells.size()) { return false; }
    if (region.cells[index] == '0') { tile = TileKind::Empty; return true; }
    if (region.cells[index] == '1') { tile = TileKind::Solid; return true; }
    return false;
}

// この図形は表示確認専用であり、衝突・ジャンプ・残影のルールを定義しない。
struct RegionColor {
    float r, g, b, a = 1.0f;
};

inline constexpr RegionColor CellColor(char cell) noexcept {
    // 暗い外部セットでも格の輪郭が読めるよう、色相数を増やさず画面内だけの明度を上げる。
    return cell == '1' ? RegionColor{0.60f, 0.84f, 0.77f} : RegionColor{0.22f, 0.30f, 0.33f};
}

} // namespace TD2
