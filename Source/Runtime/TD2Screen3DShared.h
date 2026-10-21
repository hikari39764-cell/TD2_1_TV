#pragma once
// 舞台（TD2Screen3D*.cpp）の本体・窓・部品で共有する定数と小さな関数。ほかのファイルからは読まない。
#include <cstdint>

namespace TD2::Stage3D {
// 1 マスのゲーム画素の数。ピクセルの描き方と窓の切り出しはこの格子に合わせる。
inline constexpr int kGamePixelsPerCell = 16;
// オープニングの砂嵐の窓：舞台のずっと外（部屋と重ならない所）のノイズの板を撮る。行数と、段を切り替える回数（毎秒）。
inline constexpr double kNoiseX = -3000, kNoiseY = -3000, kNoiseRows = 16, kNoiseFlips = 12;
// 消える瞬間の線の太さ（砂嵐の窓の高さに対する割合）。
inline constexpr double kNoiseLine = 1.0 / 36;
// 線のあとの光の点の位置（窓の上からの割合）。黒・線・白の範囲にかからない所。
inline constexpr double kNoiseDot = .32;
// 0〜1 の決まった乱数（テレビごと・刻みごとに同じ値）。
inline double IntroHash(uint64_t key, uint32_t salt) {
    uint64_t h = key * 0x9E3779B97F4A7C15ull ^ (uint64_t(salt) + 0x7F4A7C15ull) * 0xBF58476D1CE4E5B9ull;
    h ^= h >> 31; h *= 0x94D049BB133111EBull; h ^= h >> 29;
    return double(h >> 40) / double(1ull << 24);
}
}
