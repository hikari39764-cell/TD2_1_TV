#pragma once

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace TD2 {

inline constexpr std::string_view kRegionComponentType = "TD2.Region";
inline constexpr std::string_view kScreenSystemId = "TD2.ScreenPresentation";
inline constexpr int kDefaultTilePixels = 64;
// 本体の当たりの大きさ（マス）。人形の頭が天井に埋まらないよう、見た目より高く取る（残影は 1 マスのまま）。
// 位置は当たりの左上。開始位置・チェックポイントの立つ所もこの大きさで確かめる。
inline constexpr double kPlayerWidth = 0.625, kPlayerHeight = 1.5;
// 本体を描く高さ（人形の大きさの基準）。当たりと足元で揃え、当たりを変えても見た目は変えない。
inline constexpr double kPlayerDrawHeight = 0.875;

// ルールはこの列挙と整数のマス座標を使う。tilePixels は表示専用で、距離・衝突に使わない。
enum class TileKind : uint8_t {
    Empty = 0,
    Solid = 1,
};

// 作者データだけを保持する。テレビの大きさや実行中の WorldState はここへ混ぜない。
// cells は左上原点の行優先配列（0=空、1=ブロック）。初期値は 4×3。
struct RegionDefinition {
    std::string regionId = "td2.region.test";
    int cols = 4;
    int rows = 3;
    int tilePixels = kDefaultTilePixels;
    std::string cells = "000000001111";

    bool operator==(const RegionDefinition&) const = default;
};

// 区域・装置・仕掛けの ID に使える文字列（1～95 文字の ASCII 英数字と . _ -）。
inline bool IsStableId(std::string_view id) noexcept {
    if (id.empty() || id.size() > 95) { return false; }
    for (const char c : id) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) { return false; }
    }
    return true;
}

struct RegionValidation {
    bool valid = false;
    std::string_view message{};
};

inline RegionValidation ValidateRegion(const RegionDefinition& region) noexcept {
    if (region.regionId.empty() || region.regionId.size() > 95) {
        return {false, "区域 ID は 1～95 文字"};
    }
    if (!IsStableId(region.regionId)) {
        return {false, "区域 ID は英数字・点・下線・ハイフンだけ"};
    }
    if (region.cols <= 0 || region.rows <= 0 || region.tilePixels <= 0) {
        return {false, "行・列・マスの画素は正の整数"};
    }
    // 不正な寸法でもあふれないよう、先に広い型へ変えて掛ける。
    const auto count = static_cast<uint64_t>(region.cols) * static_cast<uint64_t>(region.rows);
    if (count != region.cells.size()) {
        return {false, "マスの数が cols × rows と合いません"};
    }
    const auto width = static_cast<uint64_t>(region.cols) * static_cast<uint64_t>(region.tilePixels);
    const auto height = static_cast<uint64_t>(region.rows) * static_cast<uint64_t>(region.tilePixels);
    if (width > (std::numeric_limits<uint32_t>::max)() ||
        height > (std::numeric_limits<uint32_t>::max)()) {
        return {false, "区域の画素の大きさが大きすぎます"};
    }
    for (const char cell : region.cells) {
        if (cell != '0' && cell != '1') {
            return {false, "マスは 0（空）か 1（ブロック）だけ"};
        }
    }
    return {true, {}};
}

inline size_t CellIndex(const RegionDefinition& region, int col, int row) noexcept {
    return static_cast<size_t>(row) * static_cast<size_t>(region.cols) + static_cast<size_t>(col);
}

// シーンの文字列形式はここで解釈し、ルール側には持ち込まない。
// 範囲外は false。境界の扱いはルール側で決める。
inline bool TryGetTile(const RegionDefinition& region, int col, int row, TileKind& tile) noexcept {
    if (col < 0 || row < 0 || col >= region.cols || row >= region.rows) { return false; }
    const size_t index = CellIndex(region, col, row);
    if (index >= region.cells.size()) { return false; }
    if (region.cells[index] == '0') { tile = TileKind::Empty; return true; }
    if (region.cells[index] == '1') { tile = TileKind::Solid; return true; }
    return false;
}

}
