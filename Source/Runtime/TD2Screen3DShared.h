#pragma once
// 舞台（TD2Screen3D*.cpp）の本体・窓・部品で共有する定数と小さな関数。ほかのファイルからは読まない。
#include <algorithm>
#include <array>
#include <cstddef>
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
// Play の覗き窓の細かさの段（1 マスの texel。段 0 が一番細かい）と、段ごとのキャプチャの大きさ（大きさを変えると
// 数フレーム映らないので固定）。段ごとに一枚のキャプチャへ、その段のテレビの視点を並べて描く。
inline constexpr size_t kPortalTiers = 4;
inline constexpr std::array<int, kPortalTiers> kTierTexels{48, 32, 16, 8};
inline constexpr std::array<std::array<uint32_t, 2>, kPortalTiers> kTierCapture{
    {{2048u, 1280u}, {1536u, 1024u}, {1024u, 1024u}, {1024u, 512u}}};
// 覗き窓の目から部屋の正面の面までの距離（マス）。旧方式の大区域の窓（40° の透視カメラで 30.5 行を覆う距離）と同じにし、
// 近いテレビの奥行きの見え方を変えない。目より手前・奥に撮る範囲（マス）。
inline constexpr double kPortalEyeCells = 42, kPortalNearCells = 8, kPortalFarCells = 40;
// 段のキャプチャの下端に空けておく行（texel）。覗き窓の無いテレビをここへ結んでおき、結び付けを外さない（外す・新しく
// 結ぶと材質が作り直しになり、エンジンが GPU のシーンを作り直す）。
inline constexpr uint32_t kPortalParkRows = 8;
// Play の砂嵐：機構で回る・落ちる間のテレビに出す（覗き窓の角度の跳びと段の入れ替わりを見せない）。舞台のずっと外
// （オープニングの砂嵐の窓とも部屋とも重ならない所）の板を、一番粗い段のキャプチャの右下（隅の行の上）に取った小さな
// 視点で撮る。板は毎フレーム跳ばしてノイズを動かすので、結んだテレビは結び直さずに砂嵐が動く。
inline constexpr double kSnowX = -3000, kSnowY = -2700, kSnowCols = 12, kSnowRows = 6;
inline constexpr size_t kSnowTier = kPortalTiers - 1;
inline constexpr uint32_t kSnowWidth = 128u, kSnowHeight = 64u;
inline constexpr uint32_t kSnowLeft = kTierCapture[kSnowTier][0] - kSnowWidth;
inline constexpr uint32_t kSnowTop = kTierCapture[kSnowTier][1] - kPortalParkRows - kSnowHeight;
// 1 ゲーム画素の texel（描き方の pixelSize と同じ丸め）。
inline uint32_t PortalBlock(int texels) { return static_cast<uint32_t>((std::max)(1, texels / kGamePixelsPerCell)); }
// 覗き窓の部屋の周りの余白（texel）。描き方は周りの texel を読む：落ち影は光の側（左上）へ 8×16 ゲーム画素、色鉛筆の
// 外形と接地は数 texel。余白には隣の舞台がそのまま写るので、画面の縁でもほかのテレビの絵を読まない。
struct PortalMargins { uint32_t left = 0, top = 0, right = 0, bottom = 0; };
inline PortalMargins MarginsFor(int texels) {
    const uint32_t b = PortalBlock(texels);
    return {10 * b, 18 * b, 4 * b, 4 * b};
}
// 0〜1 の決まった乱数（テレビごと・刻みごとに同じ値）。
inline double IntroHash(uint64_t key, uint32_t salt) {
    uint64_t h = key * 0x9E3779B97F4A7C15ull ^ (uint64_t(salt) + 0x7F4A7C15ull) * 0xBF58476D1CE4E5B9ull;
    h ^= h >> 31; h *= 0x94D049BB133111EBull; h ^= h >> 29;
    return double(h >> 40) / double(1ull << 24);
}
}
