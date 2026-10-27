// 舞台の部屋の部品：後ろの壁と窓、大広間、地形と継ぎ目、縁、通電灯、引っ込んだ仕掛けの輪郭。
#include "TD2ScreenModels.h"
#include "TD2ScreenKitAssets.h"
#include "../Common/TD2Math.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <span>
#include <string>

namespace TD2 {
namespace {
// 後ろの壁の窓：天井の下と左右に 1.5 マス、床の上に 2.5 マスの明るい壁を残す（床の上の壁に物の影と暗がりが落ち、
// 部屋が実験室の一室に見える）。窓の向こうの層は、目の向きの正接が最大（0.3）の時に部屋の縁を越えても、
// 隣の部屋の壁の帯（1.5 マス以上）に隠れる範囲で作る。
// 窓は画面の上で部屋の中央に見えるよう、斜投影で見える左の壁の側の面（0.28 マス）の分だけ左を広く取る。
constexpr double kWindowLeft = 1.8, kWindowRight = 1.5, kWindowTop = 1.5, kWindowBottom = 2.5;
constexpr double kFrameThickness = .12;   // 窓の縦の縁の厚み（側の面は奥へ 0.35 マス）
// 大広間の層の奥行き（マス）：奥の壁、光の細窓、柱。横の形は足場に見えるので作らない。
// 光の細窓は霧に沈みすぎないよう奥の壁より手前（-3）。
constexpr double kHallFarDepth = -4.5, kHallSlitDepth = -3.0, kHallColumnDepth = -2.3;
constexpr double kHallMargin = 1.4;    // 窓の外へはみ出して作る幅（視差でずらしても窓の中が空かない）
struct WindowRect { double left, top, right, bottom; };
// 窓の範囲（世界のマス、画面の上の位置）。窓が 1.5 マスより低くなる、または部屋が小さく窓が取れなければ false。
bool WindowIn(int x, int y, int cols, int rows, const WindowSpan& span, WindowRect& out) {
    const double top = (std::max)(span.top, kWindowTop), bottom = (std::max)(span.bottom, kWindowBottom);
    if (cols < kWindowLeft + kWindowRight + 2 || rows - top - bottom < 1.5) { return false; }
    out = {x + kWindowLeft, y + top, x + cols - kWindowRight, y + rows - bottom};
    return true;
}
// 傾きの隙間を塞ぐ下敷きは背景板より奥。Backdrop の下 15% は光の輪の外（真っ暗）。
constexpr double kUnderlayDepth = -1.2, kBackdropVoidBand = 0.15;
// 通電灯が点いた時の膨らみの秒数。
constexpr double kPulseSeconds = 0.26;
// 8・4・2・1 のまとめ部品のうち、残りに収まる一番大きいもの。
int BundleOf(int left) { return left >= 8 ? 8 : left >= 4 ? 4 : left >= 2 ? 2 : 1; }
}

void StagePartBuilder::Backdrop(int x, int y, int cols, int rows, WindowSpan span) {
    static constexpr std::array<std::string_view, 9> kGridH{"", "Backdrop_GridH1", "Backdrop_GridH2", "", "Backdrop_GridH4", "", "", "", "Backdrop_GridH8"};
    static constexpr std::array<std::string_view, 9> kGridV{"", "Backdrop_GridV1", "Backdrop_GridV2", "", "Backdrop_GridV4", "", "", "", "Backdrop_GridV8"};
    Begin("backdrop");
    const double wall = WallDepth(), grid = wall + .005;
    const double sx = projection_.ShiftX(wall), sy = projection_.ShiftY(wall);
    WindowRect w{};
    const bool bright = hall_ && Spatial();
    const bool window = bright && WindowIn(x, y, cols, rows, span, w);
    if (!bright) {
        Add("Backdrop", x + cols * .5, y + rows, cols, rows, wall);
    } else if (compact_) {
        // 編集中（全部の部屋を一度に写す）は部品の数を抑え、明るい壁一枚と窓の形の闇だけ（大広間は描かない）。
        const double left = x - std::abs(sx), right = x + cols + .01, top = y - .01, bottom = y + rows + std::abs(sy);
        Add("Wall_Panel", (left + right) * .5, bottom, right - left, bottom - top, wall);
        if (window) { Add("Hall_Far", (w.left + w.right) * .5, w.bottom, w.right - w.left, w.bottom - w.top, wall + .002); }
    } else if (!window) {
        // 窓の取れない部屋は明るい壁だけ。
        const double left = x - std::abs(sx), right = x + cols + .01, top = y - .01, bottom = y + rows + std::abs(sy);
        Add("Wall_Panel", (left + right) * .5, bottom, right - left, bottom - top, wall);
    } else {
        // 窓の周りの明るい壁の帯（斜投影の壁の奥行きの分だけ右上へずらし、左と下は隙間が出ないよう伸ばす）。
        const double left = x - std::abs(sx), right = x + cols + .01, top = y - .01, bottom = y + rows + std::abs(sy);
        Add("Wall_Panel", (left + right) * .5, w.top, right - left, w.top - top, wall);
        Add("Wall_Panel", (left + right) * .5, bottom, right - left, bottom - w.bottom, wall);
        Add("Wall_Panel", (left + w.left) * .5, w.bottom, w.left - left, w.bottom - w.top, wall);
        Add("Wall_Panel", (w.right + right) * .5, w.bottom, right - w.right, w.bottom - w.top, wall);
        // 窓の縦の縁：壁の厚みの棒（奥へ伸び、斜投影で左の側の面が窓の中に見える）。横の縁は付けない（敷居が足場に見える）。
        Add("Window_FrameV", w.left - kFrameThickness * .5, w.bottom, 1, w.bottom - w.top, wall);
        Add("Window_FrameV", w.right + kFrameThickness * .5, w.bottom, 1, w.bottom - w.top, wall);
    }
    // 立体の組では、奥の壁の目盛りを壁の奥行きの分だけ右上へずらす（地形の正面のマスと揃わず、壁が奥にあると読める）。
    // ずらした分の隙間が左と下に出ないよう、線を 1 マス長くする。
    const double gx = projection_.ShiftX(grid), gy = projection_.ShiftY(grid);
    const bool shifted = projection_.oblique > 0;
    // 目盛りは窓の無い昔の暗い壁だけ（明るい壁と窓の部屋では、大広間の柱と光の細窓が奥行きの手掛かりになる）。
    for (int row = 0, left = compact_ || bright ? 0 : rows + 1; left > 0;) {
        const int n = BundleOf(left);
        // 横線の束は下端から上へ n 本。最初の線が row + n - 1 に来るよう下端を合わせる。
        if (shifted) { Add(kGridH[static_cast<size_t>(n)], x + cols * .5 - .5 + gx, y + row + n - 1 + gy, cols + 1, 1, grid); }
        else { Add(kGridH[static_cast<size_t>(n)], x + cols * .5, y + row + n - 1, cols, 1, grid); }
        row += n; left -= n;
    }
    for (int col = 0, left = compact_ || bright ? 0 : cols + 1; left > 0;) {
        const int n = BundleOf(left);
        if (shifted) { Add(kGridV[static_cast<size_t>(n)], x + col + gx, y + rows + 1 + gy, 1, rows + 1, grid); }
        else { Add(kGridV[static_cast<size_t>(n)], x + col, y + rows, 1, rows, grid); }
        col += n; left -= n;
    }
    // 俯角と側角を両方付けると背景板の角がずれて隙間ができるので、Backdrop の下の真っ暗な帯を縦に伸ばした板を
    // 奥に敷いて隙間を塞ぐ。
    if (const double lean = std::abs(projection_.Lean()); lean > 1e-4) {
        const double margin = lean * rows + 1, tall = (rows + 2 * margin) / kBackdropVoidBand;
        Add("Backdrop", x + cols * .5, y + rows + margin, cols + 2 * margin, tall, kUnderlayDepth, {}, rows * .5 + margin);
    }
}

// 上が空いたマスは白い天板と石墨の縁付き（横の端が空いていれば縁が少し張り出す）、それ以外は白い面板。
// 隣の部屋のマスも見るので、部屋をまたぐ床は一続きに見える。部屋の無い所は埋まり扱い。
// マスごとに出すとエンジンの上限を超えるので、天板は横の連なり（8・4・2・1 マス）、
// 躯体は大きい塊（8×4 から 1×1）から順に当てはめる。
void StagePartBuilder::Hall(std::string_view regionId, int x, int y, int cols, int rows, WindowSpan span, double eyeX, double eyeY) {
    WindowRect w{};
    if (!hall_ || compact_ || !Spatial() || !WindowIn(x, y, cols, rows, span, w)) { return; }
    // 区域 ID から部屋ごとの並びを決める（同じ部屋は毎回同じ）。
    uint64_t seed = 1469598103934665603ull;
    for (const unsigned char c : regionId) { seed = (seed ^ c) * 1099511628211ull; }
    const auto random = [&seed]() { seed = seed * 6364136223846793005ull + 1442695040888963407ull; return static_cast<double>((seed >> 33) % 10000) / 10000.0; };
    // 層の範囲（窓とはみ出し）。
    const double wl = w.left - kHallMargin, wr = w.right + kHallMargin, wt = w.top - kHallMargin, wb = w.bottom + kHallMargin;
    // 奥行き depth の層のずれ（目の向きの視差だけ。正面から見ると層は窓にそろい、斜めから見るほど奥の層が大きくずれる）。
    // 目が右にあると奥の層は右へ、目が上にあると上へずれて見える（箱の反対側の内側が見える向き）。y は下が正。
    const auto at = [&](double depth) { const double d = -depth; return std::pair{d * eyeX, -d * eyeY}; };
    // 奥行きの格子の分だけ層を奥へ下げる（視差のずれは元の奥行きのまま。窓のはみ出しの範囲に収まる）。
    const double back = WallDepth() - kBackdropDepth;
    Begin("hall");
    {   // 奥の壁（一色の闇。霧で奥ほど明るく霞む）
        const auto [ox, oy] = at(kHallFarDepth);
        Add("Hall_Far", (wl + wr) * .5 + ox, wb + oy, wr - wl, wb - wt, kHallFarDepth + back);
    }
    // 柱：窓の幅 4.5 マスに一本ほど、間隔を乱す（等間隔の縦縞は格子に見える。部品の数は幅だけで決まる）。
    // 光の細窓一本は最初の二本の柱の間の、片方に寄った所（柱の真後ろに隠れない）。
    const int count = (std::max)(2, static_cast<int>((w.right - w.left) / 4.5));
    std::vector<double> columns;
    for (int k = 0; k < count; ++k) { columns.push_back((k + .5 + (random() - .5) * .5) / count); }
    {
        const auto [lx, ly] = at(kHallSlitDepth);
        const double t = columns[0] + (columns[1] - columns[0]) * (random() < .5 ? .35 : .65);
        Add("Hall_Slit", w.left + (w.right - w.left) * t + lx, wb + ly, 1, wb - wt, kHallSlitDepth + back);
    }
    {
        const auto [cx, cy] = at(kHallColumnDepth);
        for (const double t : columns) { Add("Hall_Column", w.left + (w.right - w.left) * t + cx, wb + cy, 1, wb - wt, kHallColumnDepth + back); }
    }
}

WindowSpan StagePartBuilder::SpanAvoidingSigns(const std::vector<Rules::DeviceSnapshot>& devices, std::string_view regionId,
                                               int roomY, int rows) const {
    // 既定の窓の高さにかかる札の絵の上端と下端（Device の Decal と同じ置き方。板は原点から 0.08〜0.92、長押しの棒は 0.25 下まで）。
    const double windowTop = roomY + kWindowTop, windowBottom = roomY + rows - kWindowBottom;
    double top = 1e9, bottom = -1e9;
    for (const auto& d : devices) {
        if (d.kind != Rules::DeviceKind::Decal || !(d.variant == "ad" || d.variant == "space" || d.variant == "spaceHold")) { continue; }
        if (d.id.size() <= regionId.size() || d.id.compare(0, regionId.size(), regionId) != 0 || d.id[regionId.size()] != '/') { continue; }
        const double origin = d.rect.y + d.rect.h * .5 + .5 * kSignSize + projection_.ShiftY(kSignWall);
        const double signTop = origin - .92 * kSignSize, signBottom = origin + (d.variant == "spaceHold" ? .25 : -.08) * kSignSize;
        if (signBottom <= windowTop || signTop >= windowBottom) { continue; }
        top = (std::min)(top, signTop); bottom = (std::max)(bottom, signBottom);
    }
    if (bottom < top) { return {}; }
    constexpr double kGap = .3;
    const double below = windowBottom - (bottom + kGap), above = (top - kGap) - windowTop;
    if ((std::max)(below, above) < 1.5) { return {rows * 1.0, 0}; }   // どちらも狭い：窓を作らない
    return below >= above ? WindowSpan{bottom + kGap - roomY, 0} : WindowSpan{0, roomY + rows - (top - kGap)};
}

void StagePartBuilder::Terrain(int x, int y, int cols, int rows, const std::vector<char>& occupancy) {
    const auto at = [&](int col, int row) {
        return occupancy[static_cast<size_t>((row + 1) * (cols + 2) + col + 1)] != 0;
    };
    if (solid_) {
        // 実体の地形：固い所を大きい長方形から順に箱一つにまとめ、部屋の奥まで伸ばす。立てる所（上が空いた横の連なり）には
        // 明るい天板を重ね、手前に石墨の縁を付ける（縁は奥へ伸ばさない）。正面は玩法の面より SolidFront() だけ手前。
        const double depth = SolidDepth(), front = SolidFront();
        std::vector<char> taken(static_cast<size_t>(cols * rows), 0);
        const auto open = [&](int col, int row) { return col < cols && row < rows && at(col, row) && !taken[static_cast<size_t>(row * cols + col)]; };
        for (int row = 0; row < rows; ++row) for (int col = 0; col < cols; ++col) {
            if (!open(col, row)) { continue; }
            int w = 1, h = 1;
            while (open(col + w, row)) { ++w; }
            for (bool grow = true; grow && row + h < rows;) {
                for (int i = 0; i < w && grow; ++i) { grow = open(col + i, row + h); }
                if (grow) { ++h; }
            }
            for (int j = 0; j < h; ++j) for (int i = 0; i < w; ++i) { taken[static_cast<size_t>((row + j) * cols + col + i)] = 1; }
            Begin("solid/" + std::to_string(col) + "/" + std::to_string(row));
            partDepth_ = depth;
            Add("Solid_Block", x + col + w * .5, y + row + h, w, h, front);
            partDepth_ = 1;
        }
        for (int row = 0; row < rows; ++row) for (int col = 0; col < cols;) {
            if (!at(col, row) || at(col, row - 1)) { ++col; continue; }
            int length = 0;
            while (col + length < cols && at(col + length, row) && !at(col + length, row - 1)) { ++length; }
            Begin("solidTop/" + std::to_string(col) + "/" + std::to_string(row));
            partDepth_ = depth;
            Add("Solid_Cap", x + col + length * .5, y + row + 1, length, 1, front);
            partDepth_ = 1;
            Add("Solid_Nose", x + col + length * .5, y + row + 1, length, 1, front);
            col += length;
        }
        SolidSeams(x, y, cols, rows, occupancy);
        return;
    }
    std::vector<char> used(static_cast<size_t>(cols * rows), 0);
    const auto index = [cols](int col, int row) { return static_cast<size_t>(row * cols + col); };
    const auto top = [&](int col, int row) { return at(col, row) && !at(col, row - 1); };
    std::string name;
    for (int row = 0; row < rows; ++row) for (int col = 0; col < cols;) {
        if (!top(col, row)) { ++col; continue; }
        int length = 0;
        while (col + length < cols && top(col + length, row)) { used[index(col + length, row)] = 1; ++length; }
        const bool openLeft = !at(col - 1, row), openRight = !at(col + length, row);
        for (int start = col, left = length; left > 0;) {
            const int n = BundleOf(left);
            name = "Terrain_TopRun" + std::to_string(n);
            if (start == col && openLeft) { name += 'L'; }
            if (left == n && openRight) { name += 'R'; }
            Begin("top/" + std::to_string(start) + "/" + std::to_string(row));
            Add(name, x + start + n * .5, y + row + 1);
            start += n; left -= n;
        }
        col += length;
    }
    static constexpr std::array<std::pair<int, int>, 12> kBlocks{{{8, 4}, {4, 4}, {8, 2}, {4, 2}, {2, 4}, {8, 1}, {2, 2},
                                                                 {4, 1}, {1, 4}, {2, 1}, {1, 2}, {1, 1}}};
    const auto free = [&](int col, int row) { return col < cols && row < rows && at(col, row) && !used[index(col, row)]; };
    for (int row = 0; row < rows; ++row) for (int col = 0; col < cols; ++col) {
        if (!free(col, row)) { continue; }
        for (const auto& [w, h] : kBlocks) {
            bool fits = true;
            for (int j = 0; j < h && fits; ++j) for (int i = 0; i < w && fits; ++i) { fits = free(col + i, row + j); }
            if (!fits) { continue; }
            for (int j = 0; j < h; ++j) for (int i = 0; i < w; ++i) { used[index(col + i, row + j)] = 1; }
            Begin("block/" + std::to_string(col) + "/" + std::to_string(row));
            Add("Terrain_Block" + std::to_string(w) + "x" + std::to_string(h), x + col + w * .5, y + row + h);
            break;
        }
    }
    // 奥行きの格子：手前の一列の後ろに同じ形の塊を並べる。天板の縁は付けない（奥の列が段や足場に見えないように）。
    // 斜投影の組は部品に焼いた傾きと同じだけずらし、列の間に隙間を作らない。継ぎ目が奥行きの目盛りになる。
    for (int k = 1; k <= depthRows_; ++k) {
        const double depth = -kTerrainDepth * k, sx = projection_.ShiftX(depth), sy = projection_.ShiftY(depth);
        std::vector<char> taken(static_cast<size_t>(cols * rows), 0);
        const auto open = [&](int col, int row) { return col < cols && row < rows && at(col, row) && !taken[index(col, row)]; };
        for (int row = 0; row < rows; ++row) for (int col = 0; col < cols; ++col) {
            if (!open(col, row)) { continue; }
            for (const auto& [w, h] : kBlocks) {
                bool fits = true;
                for (int j = 0; j < h && fits; ++j) for (int i = 0; i < w && fits; ++i) { fits = open(col + i, row + j); }
                if (!fits) { continue; }
                for (int j = 0; j < h; ++j) for (int i = 0; i < w; ++i) { taken[index(col + i, row + j)] = 1; }
                Begin("deep/" + std::to_string(k) + "/" + std::to_string(col) + "/" + std::to_string(row));
                Add("Terrain_Block" + std::to_string(w) + "x" + std::to_string(h), x + col + w * .5 + sx, y + row + h + sy, 1, 1, depth);
                break;
            }
        }
    }
}

void StagePartBuilder::SolidSeams(int x, int y, int cols, int rows, const std::vector<char>& occupancy) {
    // 実体の面の目地：見える面のマスの境に細い棒を置く（奥へ伸びる線が透視の中心へ集まり、奥行きの目盛りになる）。
    // 部屋の境の線は左と上の部屋の側だけが引く（隣の部屋と二重にならない）。
    // 同じ形の棒が 1 マスおきに並ぶ所は 2・4・8 本を一つにしたまとめ部品で出す（部品は一つごとに毎フレームの手間と
    // 視点ごとの選別がかかる。棒の並びは一本ずつ出した時と同じ）。
    static constexpr std::array<std::string_view, 9> kZAlongX{"", "Solid_RodZ", "Solid_RodZ_X2", "", "Solid_RodZ_X4", "", "", "", "Solid_RodZ_X8"};
    static constexpr std::array<std::string_view, 9> kZAlongY{"", "Solid_RodZ", "Solid_RodZ_Y2", "", "Solid_RodZ_Y4", "", "", "", "Solid_RodZ_Y8"};
    static constexpr std::array<std::string_view, 9> kYAlongX{"", "Solid_RodY", "Solid_RodY_X2", "", "Solid_RodY_X4", "", "", "", "Solid_RodY_X8"};
    static constexpr std::array<std::string_view, 9> kXAlongY{"", "Solid_RodX", "Solid_RodX_Y2", "", "Solid_RodX_Y4", "", "", "", "Solid_RodX_Y8"};
    static constexpr std::array<std::string_view, 5> kXDeep{"", "Solid_RodX", "Solid_RodX_D2", "", "Solid_RodX_D4"};
    static constexpr std::array<std::string_view, 5> kYDeep{"", "Solid_RodY", "Solid_RodY_D2", "", "Solid_RodY_D4"};
    const auto solid = [&](int col, int row) { return occupancy[static_cast<size_t>((row + 1) * (cols + 2) + col + 1)] != 0; };
    const int depth = depthRows_ + 1;
    const double front = SolidFront();
    // 診断（環境変数 TD2_ROD_BUNDLE_OFF）：まとめずに一本ずつ出す（比べるため）。
    static const bool single = std::getenv("TD2_ROD_BUNDLE_OFF") != nullptr;
    // 続いた番号（昇順）を連なりごとに 8・4・2・1 本へ分けて渡す。
    const auto bundles = [](const std::vector<int>& at, const auto& emit) {
        for (size_t i = 0; i < at.size();) {
            size_t run = 1;
            if (single) { emit(at[i], 1); ++i; continue; }
            while (i + run < at.size() && at[i + run] == at[i] + static_cast<int>(run)) { ++run; }
            for (size_t done = 0; done < run;) {
                const int n = BundleOf(static_cast<int>(run - done));
                emit(at[i + done], n);
                done += static_cast<size_t>(n);
            }
            i += run;
        }
    };
    // 奥へ伸びる棒：横（alongX）か下へ 1 マスおきに並べる。
    const auto rodsZ = [&](double px, double py, const std::vector<int>& at, bool alongX) {
        partDepth_ = depth;
        bundles(at, [&](int first, int n) {
            Add((alongX ? kZAlongX : kZAlongY)[static_cast<size_t>(n)], alongX ? px + first : px, alongX ? py : py + first, 1, 1, front);
        });
        partDepth_ = 1;
    };
    // 面の奥行き 1 マスごとの線（k = 1〜depth-1）：奥へ 4・2 本ずつまとめる。
    const auto deep = [&](bool horizontal, double px, double py, int length) {
        for (int k = 1; k < depth;) {
            const int n = single ? 1 : (std::min)(BundleOf(depth - k), 4);
            if (horizontal) { Add(kXDeep[static_cast<size_t>(n)], px, py, length, 1, front - k); }
            else { Add(kYDeep[static_cast<size_t>(n)], px, py, 1, length, front - k); }
            k += n;
        }
    };
    Begin("seams");
    std::vector<int> at;
    // 天板（上が空き）と下面（下が空き）：奥へ伸びる線をマスの境に、横の線を奥行き 1 マスごとに。
    for (int side = 0; side < 2; ++side) {
        const int dy = side == 0 ? -1 : 1;
        const auto face = [&](int col, int row) { return solid(col, row) && !solid(col, row + dy); };
        for (int row = 0; row < rows; ++row) {
            const double py = y + row + (side == 0 ? 0 : 1);
            at.clear();
            for (int col = 0; col < cols; ++col) {
                if (face(col, row) && face(col - 1, row)) { at.push_back(col); }
            }
            rodsZ(x, py, at, true);
            for (int col = 0; col < cols;) {
                if (!face(col, row)) { ++col; continue; }
                int length = 0;
                while (col + length < cols && face(col + length, row)) { ++length; }
                deep(true, x + col + length * .5, py, length);
                col += length;
            }
        }
    }
    // 正面：固いマスどうしの縦と横の境。同じ長さの線が隣の列（行）に続く所はまとめる。
    std::map<std::pair<int, int>, std::vector<int>> lines;
    for (int col = 0; col < cols; ++col) {
        for (int row = 0; row < rows;) {
            if (!(solid(col, row) && solid(col - 1, row))) { ++row; continue; }
            int length = 0;
            while (row + length < rows && solid(col, row + length) && solid(col - 1, row + length)) { ++length; }
            lines[{row, length}].push_back(col);
            row += length;
        }
    }
    for (const auto& [run, list] : lines) {
        const int row = run.first, length = run.second;
        bundles(list, [&](int first, int n) { Add(kYAlongX[static_cast<size_t>(n)], x + first, y + row + length, 1, length, front); });
    }
    lines.clear();
    for (int row = 0; row < rows; ++row) {
        for (int col = 0; col < cols;) {
            if (!(solid(col, row) && solid(col, row - 1))) { ++col; continue; }
            int length = 0;
            while (col + length < cols && solid(col + length, row) && solid(col + length, row - 1)) { ++length; }
            lines[{col, length}].push_back(row);
            col += length;
        }
    }
    for (const auto& [run, list] : lines) {
        const int col = run.first, length = run.second;
        bundles(list, [&](int first, int n) { Add(kXAlongY[static_cast<size_t>(n)], x + col + length * .5, y + first, length, 1, front); });
    }
    // 左右の側面（横が空き）：奥へ伸びる線を縦のマスの境に、縦の線を奥行き 1 マスごとに。
    for (int side = 0; side < 2; ++side) {
        const int dx = side == 0 ? -1 : 1;
        const auto face = [&](int col, int row) { return solid(col, row) && !solid(col + dx, row); };
        for (int col = 0; col < cols; ++col) {
            const double px = x + col + (side == 0 ? 0 : 1);
            at.clear();
            for (int row = 0; row < rows; ++row) {
                if (face(col, row) && face(col, row - 1)) { at.push_back(row); }
            }
            rodsZ(px, y, at, false);
            for (int row = 0; row < rows;) {
                if (!face(col, row)) { ++row; continue; }
                int length = 0;
                while (row + length < rows && face(col, row + length)) { ++length; }
                deep(false, px, y + row + length, length);
                row += length;
            }
        }
    }
}

void StagePartBuilder::Lamp(double x, double bottomY, bool lit, double since, bool world) {
    // 点いた瞬間、灯が一度膨らんで戻り、灯の中心（高さ 0.07）から光の粒が三つ斜め上へ散る。
    // つながった仕掛けで同時に起きるので、どれとどれがつながっているかが分かる。消える時は灯が消えるだけ。
    const double t = since / kPulseSeconds;
    const bool burst = lit && t >= 0 && t < 1;
    const double ease = burst ? 1 - (1 - t) * (1 - t) : 1.0;
    const double pop = burst ? 1 + .5 * (1 - ease) : 1.0;
    // 灯の色：画面の中のつながりは青、画面の外のテレビを動かす圧力板はマゼンタ。
    Add(world ? "Signal_Lamp3D" : "Signal_Lamp", x, bottomY + .07 * (pop - 1), pop, pop, 0, {}, 0, lit);
    Add("Signal_LampOff", x, bottomY, 1, 1, 0, {}, 0, !lit);
    static constexpr double kAngles[]{.55, 1.5708, 2.59};
    for (const double a : kAngles) {
        const double reach = .18 + .42 * ease, size = .13 * (1 - ease) + .02;
        const double cx = x + std::cos(a) * reach, cy = bottomY - .07 - std::sin(a) * reach;
        Add("FX_Shard", cx, cy + size * .5, size, size, .1, {}, -1, burst);
    }
}

void StagePartBuilder::Edges(int x, int y, int cols, int rows, const std::vector<char>& occupancy, const std::vector<char>& linked) {
    // 並びは occupancy と同じ（(cols + 2) × (rows + 2)、周り 1 マス込み）。
    const auto index = [cols](int col, int row) { return static_cast<size_t>((row + 1) * (cols + 2) + col + 1); };
    const auto open = [&](int col, int row) { return occupancy[index(col, row)] == 0; };
    // 口：外周の空いたマスで、外側に部屋が無いもの。左右は縦の鎧戸、上下は横の鎧戸。
    // 部品は口のマスにだけ置く（壁のマスにまで置くと、部屋の多い編集画面で描画の上限を超える）。
    // 口がつながっている間は退避して隠す（機構で部屋が動くたびに部品の数が変わらないように）。
    Begin("edges");
    // 明るい壁の部屋では、口のマスの後ろの壁を外へ向かって暗くする（口が通り道に見える）。編集中は部品を増やさない。
    const bool opening = hall_ && !compact_ && Spatial();
    const double wall = WallDepth(), wx = projection_.ShiftX(wall), wy = projection_.ShiftY(wall);
    const auto recess = [&](double cx, double cy, double turn) {
        if (opening) { Add("Wall_Opening", cx + wx, cy + wy, 1, 1, wall + .001, RotateZ(turn), 0); }
    };
    for (int row = 0; row < rows; ++row) {
        if (open(0, row)) recess(x + .5, y + row + .5, 0.0);
        if (open(cols - 1, row)) recess(x + cols - .5, y + row + .5, kPi);
    }
    for (int col = 0; col < cols; ++col) {
        if (open(col, 0)) recess(x + col + .5, y + .5, -kPi * .5);
        if (open(col, rows - 1)) recess(x + col + .5, y + rows - .5, kPi * .5);
    }
    // 実体の地形では鎧戸を正面から部屋の奥まで伸ばす（口を奥行きごと塞ぐ。部品は奥行き -0.15〜-0.5 の板）。
    const double shutterDepth = solid_ ? SolidDepth() / .35 : 1.0, shutterAt = solid_ ? SolidFront() + .15 * shutterDepth : .05;
    const auto shutter = [&](double sx, double sy, HIKARI::GameProjectQuaternion rotation, bool closed) {
        partDepth_ = shutterDepth;
        if (closed || !compact_) Add("Edge_Shutter", sx, sy, 1, 1, shutterAt, rotation, 0, closed);
        partDepth_ = 1;
    };
    for (int row = 0; row < rows; ++row) {
        if (open(0, row)) shutter(x + .15, y + row + .5, {}, !linked[index(-1, row)]);
        if (open(cols - 1, row)) shutter(x + cols - .15, y + row + .5, {}, !linked[index(cols, row)]);
    }
    // 上下の口は横向きの形（回すと立体の組の斜めの向きまで回ってしまう）。
    const auto across = [&](double sx, double sy, bool closed) {
        partDepth_ = shutterDepth;
        if (closed || !compact_) Add("Edge_ShutterH", sx, sy, 1, 1, shutterAt, {}, 0, closed);
        partDepth_ = 1;
    };
    for (int col = 0; col < cols; ++col) {
        if (open(col, 0)) across(x + col + .5, y + .15, !linked[index(col, -1)]);
        if (open(col, rows - 1)) across(x + col + .5, y + rows - .15, !linked[index(col, rows)]);
    }
}

void StagePartBuilder::Outline(double left, double top, double right, double bottom, bool visible, bool link) {
    // 1 マスの破線部品を辺に沿って並べ、端の半端は縮める。
    const auto along = [&](std::string_view part, double from, double to, bool horizontal, double at) {
        for (double a = from; a < to - 1e-6; a += 1) {
            const double length = (std::min)(1.0, to - a);
            if (horizontal) { Add(part, a + length * .5, at, length, 1, .02, {}, 0, visible); }
            else { Add(part, at, a + length, 1, length, .02, {}, 0, visible); }
        }
    };
    // 扉・橋の「戻るとここに出る」輪郭は仕掛けの青、残影まわりの輪郭は菫。
    const std::string_view h = link ? "Link_DashH" : "Ghost_DashH", v = link ? "Link_DashV" : "Ghost_DashV";
    along(h, left, right, true, top);
    along(h, left, right, true, bottom);
    along(v, top, bottom, false, left);
    along(v, top, bottom, false, right);
}
}
