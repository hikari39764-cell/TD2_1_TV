#include "TD2ScreenModels.h"
#include "TD2ScreenKitAssets.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <span>
#include <string>

namespace TD2 {
namespace {
// 背景の壁はプレイ面より 0.85 マス奥。目盛りは壁のすぐ手前。
constexpr double kBackdropDepth = -0.85, kGridDepth = -0.845;
// 傾きの隙間を塞ぐ下敷きは背景板より奥。Backdrop の下 15% は光の輪の外（真っ暗）。
constexpr double kUnderlayDepth = -1.2, kBackdropVoidBand = 0.15;
// 隠す部品を退避位置からさらに下げる距離（舞台カメラに写らない所まで）。
constexpr float kHiddenDrop = 2000.0f;

std::string_view DirectionName(Rules::Direction direction) {
    switch (direction) {
    case Rules::Direction::Down: return "Down";
    case Rules::Direction::Left: return "Left";
    case Rules::Direction::Right: return "Right";
    default: return "Up";
    }
}
// 扉の羽根・橋板が出入りする秒数（当たりは即座に変わるので短く）と、通電灯が点いた時の膨らみの秒数。
constexpr double kSwitchSeconds = 0.14, kPulseSeconds = 0.26;
// バネの冷却が終わった時に踏み板が一度光る秒数。
constexpr double kReadyPopSeconds = 0.28;
// 8・4・2・1 のまとめ部品のうち、残りに収まる一番大きいもの。
int BundleOf(int left) { return left >= 8 ? 8 : left >= 4 ? 4 : left >= 2 ? 2 : 1; }
}

void StagePartBuilder::Begin(std::string_view key) {
    semantic_ = 1469598103934665603ull;
    for (const unsigned char c : key) { semantic_ = (semantic_ ^ c) * 1099511628211ull; }
    partIndex_ = 0;
}

bool StagePartBuilder::Add(std::string_view part, double x, double bottomY, double width, double height, double depth,
                           HIKARI::GameProjectQuaternion rotation, double leanPivot, bool visible) {
    const auto guid = ScreenKitGuid(part, palette_);
    PartRequest request{};
    request.ownerObjectHandle = owner_; request.instanceId = semantic_ ^ (++partIndex_ * 0x9e3779b97f4a7c15ull);
    if (!request.instanceId) { request.instanceId = 1; }
    if (guid.empty() || guid.size() >= sizeof(request.model.assetGuid)) { ++failed_; return false; }
    std::memcpy(request.model.assetGuid, guid.data(), guid.size());
    // 俯角と側角を両方付けた時の傾きは部品の中ほどで釣り合わせ、マスの中心からのずれを上下に半分ずつにする。
    // 光線は中心が原点なのでずらさない。
    double pivot = leanPivot >= 0 ? leanPivot : (part == "LaserBeam" ? 0.0 : height * .5);
    if (mirror_) {
        // 点対称に移すと部品は原点から下へ伸びるので、傾きの釣り合いも下向きに取る。
        x = 2 * mirrorX_ - x; bottomY = 2 * mirrorY_ - bottomY; pivot = -pivot;
        const auto half = RotateZ(kPi);
        rotation = {half.w * rotation.x - half.z * rotation.y, half.w * rotation.y + half.z * rotation.x,
                    half.w * rotation.z + half.z * rotation.w, half.w * rotation.w - half.z * rotation.z};
    }
    request.worldTransform.position = projection_.Point(x + projection_.Lean() * pivot, bottomY, depth);
    request.worldTransform.rotation = rotation;
    const bool quarterTurn = std::abs(std::abs(rotation.z) - .70710678f) < .0001f && rotation.x == 0 && rotation.y == 0;
    // 正面の 1 マスが像でもちょうど 1 マスになるよう、俯角で縦、側角で横の縮みを補う。90°回した部品は軸を入れ替える。
    const double pitchCos = std::cos(projection_.pitch), yawCos = std::cos(projection_.yaw);
    request.worldTransform.scale = {static_cast<float>(width * projection_.scale / (quarterTurn ? pitchCos : yawCos)),
        static_cast<float>(height * projection_.scale / (quarterTurn ? yawCos : pitchCos)), static_cast<float>(projection_.scale)};
    // 陰影は部品に焼いてあり、キャプチャは灯も影も使わない。visible=0 にするとシーンの作り直しになるので、
    // 表示したまま舞台カメラに写らない所へ退ける。
    request.castShadow = 0; request.receiveShadow = 0; request.visible = 1;
    if (!visible) { request.worldTransform.position.y -= kHiddenDrop; }
    out_.push_back(request);
    return true;
}

void StagePartBuilder::Backdrop(int x, int y, int cols, int rows) {
    static constexpr std::array<std::string_view, 9> kGridH{"", "Backdrop_GridH1", "Backdrop_GridH2", "", "Backdrop_GridH4", "", "", "", "Backdrop_GridH8"};
    static constexpr std::array<std::string_view, 9> kGridV{"", "Backdrop_GridV1", "Backdrop_GridV2", "", "Backdrop_GridV4", "", "", "", "Backdrop_GridV8"};
    Begin("backdrop");
    Add("Backdrop", x + cols * .5, y + rows, cols, rows, kBackdropDepth);
    // 立体の組では、奥の壁の目盛りを壁の奥行きの分だけ右上へずらす（地形の正面のマスと揃わず、壁が奥にあると読める）。
    // ずらした分の隙間が左と下に出ないよう、線を 1 マス長くする。
    const double sx = projection_.ShiftX(kGridDepth), sy = projection_.ShiftY(kGridDepth);
    const bool shifted = projection_.oblique > 0;
    for (int row = 0, left = compact_ ? 0 : rows + 1; left > 0;) {
        const int n = BundleOf(left);
        // 横線の束は下端から上へ n 本。最初の線が row + n - 1 に来るよう下端を合わせる。
        if (shifted) { Add(kGridH[static_cast<size_t>(n)], x + cols * .5 - .5 + sx, y + row + n - 1 + sy, cols + 1, 1, kGridDepth); }
        else { Add(kGridH[static_cast<size_t>(n)], x + cols * .5, y + row + n - 1, cols, 1, kGridDepth); }
        row += n; left -= n;
    }
    for (int col = 0, left = compact_ ? 0 : cols + 1; left > 0;) {
        const int n = BundleOf(left);
        if (shifted) { Add(kGridV[static_cast<size_t>(n)], x + col + sx, y + rows + 1 + sy, 1, rows + 1, kGridDepth); }
        else { Add(kGridV[static_cast<size_t>(n)], x + col, y + rows, 1, rows, kGridDepth); }
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
void StagePartBuilder::Terrain(int x, int y, int cols, int rows, const std::vector<char>& occupancy) {
    const auto at = [&](int col, int row) {
        return occupancy[static_cast<size_t>((row + 1) * (cols + 2) + col + 1)] != 0;
    };
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
    const auto shutter = [&](double sx, double sy, HIKARI::GameProjectQuaternion rotation, bool closed) {
        if (closed || !compact_) Add("Edge_Shutter", sx, sy, 1, 1, .05, rotation, 0, closed);
    };
    for (int row = 0; row < rows; ++row) {
        if (open(0, row)) shutter(x + .15, y + row + .5, {}, !linked[index(-1, row)]);
        if (open(cols - 1, row)) shutter(x + cols - .15, y + row + .5, {}, !linked[index(cols, row)]);
    }
    // 上下の口は横向きの形（回すと立体の組の斜めの向きまで回ってしまう）。
    const auto across = [&](double sx, double sy, bool closed) {
        if (closed || !compact_) Add("Edge_ShutterH", sx, sy, 1, 1, .05, {}, 0, closed);
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

void StagePartBuilder::Device(const Rules::DeviceSnapshot& d, bool checkpoint, double since) {
    Begin("device/" + d.id);
    const auto& r = d.rect; const double x = r.x + r.w * .5, bottom = r.y + r.h;
    const auto cells = [&](std::string_view part, bool visible) {
        for (double cy = 0; cy < r.h; cy += 1) for (double cx = 0; cx < r.w; cx += 1) { Add(part, r.x + cx + .5, r.y + cy + 1, 1, 1, 0, {}, -1, visible); }
    };
    const auto row = [&](std::string_view part, bool visible) {
        for (double cx = 0; cx < r.w; cx += 1) { Add(part, r.x + cx + .5, r.y + 1, 1, 1, 0, {}, -1, visible); }
    };
    switch (d.kind) {
    case Rules::DeviceKind::Solid: break;   // 地形の占有へ合流させて天板の判定と一緒に描く
    case Rules::DeviceKind::OneWay: row("OneWay", true); break;
    // 圧力板：踏まれると沈み、正面の通電灯が点く（つながった扉・橋の灯も同時に点く）。
    // 押している間だけの板は丸いドーム、切り替え式は角の丸い四角に白い突起で、入っている間は沈んだまま残る。
    // 画面の外のテレビを動かす板は、品紅の玉を載せた柱を両端に立てた品紅の板（画面の中だけの青い板と形と色で分ける）。
    case Rules::DeviceKind::Plate: {
        const bool down = d.pressed || d.active;
        // 元へ戻すボタン（variant=reset）は品紅の茸形の押しボタン（普通の圧力板と見分ける）。
        const bool reset = d.variant == "reset";
        // 台と踏み面は別の部品。踏み面は台の上面に載り、踏まれている間は沈む（台の高さは素材の PLATE_TOP などと同じ）。
        const std::string_view base = reset ? "Button_ResetBase" : d.world ? "Plate3D_Base" : (d.latch ? "Plate_LatchBase" : "Plate_Base");
        const std::string_view pad = reset ? "Button_ResetCap" : d.world ? (d.latch ? "Plate3D_LatchPad" : "Plate3D_Pad") : (d.latch ? "Plate_LatchPad" : "Plate_Pad");
        const double top = reset ? .14 : d.world ? .18 : .12, sink = reset ? .12 : .065;
        // 台座は板の幅へ伸ばす。丸い押しボタンは伸ばすと丸く見えないので 1 マスの大きさのまま中央に置く。
        const bool button = reset || d.latch;
        Add(base, x, bottom, r.w, 1);
        Add(pad, x, bottom - top + (down ? sink : 0.0), button ? 1.0 : r.w, 1);
        Lamp(x, bottom, d.powered, since, d.world);
        break;
    }
    case Rules::DeviceKind::Door: {
        // 扉：柱は矩形の高さ、格納箱は上端（正面に通電灯）、閉じた扉は 1 マスずつの白い戸板（中央に青い縦の光）。
        // 開く時は戸板が下から格納箱へ上がり、閉じる時は上から下りる。開いている間は戸口を仕掛けの青の破線で囲む。
        const bool open = d.active || !d.solid;
        const double p = std::clamp(since / kSwitchSeconds, 0.0, 1.0);
        const double leaves = r.h * (open ? 1 - p : p);   // 上から下りている羽根の高さ
        // 柱は 1 マスずつ重ねる（縦に伸ばすと、立体の組で天板の斜めまで伸びてしまう）。
        for (int cy = 0; cy < static_cast<int>(std::ceil(r.h - 1e-6)); ++cy) {
            Add("Door_Jamb", x, bottom - cy, r.w, (std::min)(1.0, r.h - cy));
        }
        Add("Door_Lintel", x, r.y + .36, r.w, 1);
        Add("Door_LeafFolded", x, r.y + .62, r.w, 1, 0, {}, -1, leaves < r.h - 1e-3);
        for (int cy = 0; cy < static_cast<int>(std::ceil(r.h - 1e-6)); ++cy) {
            const double part = std::clamp(leaves - cy, 0.0, 1.0);
            Add("Door_LeafTile", x, r.y + cy + part, r.w, (std::max)(part, .01), 0, {}, -1, part > .01);
        }
        Lamp(x, r.y + .27, d.powered, since);
        Outline(x - .34 * r.w, r.y + .62, x + .34 * r.w, bottom, open, true);
        break;
    }
    case Rules::DeviceKind::Laser: Add(std::string("Laser_") + std::string(DirectionName(d.direction)), x, bottom, r.w, r.h); break;
    // 危険：回る赤い丸鋸（マスごとに一枚）。刃の中心を付いている面の縁に置き、半分を地形に沈める（向きは中心の置き方だけ）。
    // 刃は斜めに切っていないので、立体の組では刃の奥行き（0.3）の分だけ右上へずらして溝の中に合わせる。
    // 隣り合う刃は回る角度を少しずらす（同じ姿の歯が並ぶと止まって見える）。
    case Rules::DeviceKind::Spikes: {
        const double ox = projection_.ShiftX(-.3), oy = projection_.ShiftY(-.3);
        for (double cy = 0; cy < r.h; cy += 1) for (double cx = 0; cx < r.w; cx += 1) {
            double sx = r.x + cx + .5, sy = r.y + cy + .5;
            switch (d.direction) {
            case Rules::Direction::Down: sy = r.y + cy; break;
            case Rules::Direction::Left: sx = r.x + cx + 1; break;
            case Rules::Direction::Right: sx = r.x + cx; break;
            default: sy = r.y + cy + 1; break;
            }
            const double spin = -time_ * 4.5 + (cx + cy) * .31;
            Add("Spikes_Saw", sx + ox, sy + oy, 1, 1, 0, RotateZ(spin), 0);
        }
        break;
    }
    // チェックポイント：白い柱を巻く輪。触れていない間は暗い輪が根元に置かれ、触れると光る輪が柱の上へ昇り（0.35 秒）、
    // 上でゆっくり浮き沈みしながら二つの大きさを交互に見せて脈打つ。
    case Rules::DeviceKind::Checkpoint: {
        const double rise = checkpoint ? 1 - std::pow(1 - std::clamp(since / .35, 0.0, 1.0), 3.0) : 0.0;
        const double bob = checkpoint ? .03 * std::sin(time_ * 3.0) * rise : 0.0;
        const double ringBottom = bottom - (.08 + .6 * rise + bob);
        const bool pulse = std::fmod(time_ * 3.0, 2.0) < 1.0;
        Add("Checkpoint_Post", x, bottom, r.w, 1);
        Add("Checkpoint_RingIdle", x, bottom - .08, 1, 1, 0, {}, -1, !checkpoint);
        Add("Checkpoint_Ring0", x, ringBottom, 1, 1, 0, {}, -1, checkpoint && pulse);
        Add("Checkpoint_Ring1", x, ringBottom, 1, 1, 0, {}, -1, checkpoint && !pulse);
        break;
    }
    // バネ：マスごとに白い台・石墨の巻きばね・緑の踏み板。状態は踏み板だけで見せる（素描では鮮やかな色だけが残るので緑系で）：
    // 使える：緑。乗って溜めている間は踏み板が光り、ばねが溜まり具合に合わせて縮む。使えるようになった瞬間（0.28 秒）も一度光る。
    // 冷却中：踏み板は石墨で、その上の緑が中心から左右へ広がって満ちる（満ちると使える）。
    case Rules::DeviceKind::Spring: {
        const bool cooling = d.cooldown > 0, hot = !cooling && d.charge > 0;
        const bool flash = !cooling && since >= 0 && since < kReadyPopSeconds;
        const double fill = cooling ? std::clamp(1.0 - d.cooldown, .02, 1.0) : 1.0;
        // 台・ばね・踏み板の高さ（素材の SPRING_BASE・SPRING_COIL・SPRING_PAD と同じ）。踏み板の上面がルールの当たり。
        constexpr double kBase = .05, kPad = .055, kCoil = 10.0 / 32.0 - kBase - kPad;
        // ばねの縮み：溜めている間は溜まり具合に合わせて縮み、打ち上げた直後（0.2 秒）は伸びてから戻る。
        double squeeze = 1.0;
        if (hot) { squeeze = 1 - .45 * std::clamp(d.charge, 0.0, 1.0); }
        else if (cooling && since >= 0 && since < .2) { const double t = since / .2; squeeze = 1 + .6 * (1 - t) * (1 - t) * std::cos(t * 7.0); }
        for (double cx = 0; cx < r.w; cx += 1) {
            const double px = r.x + cx + .5;
            const double padBottom = bottom - kBase - kCoil * squeeze;
            Add("Spring_Base", px, bottom);
            Add("Spring_Coil", px, bottom - kBase, 1, squeeze, 0, {}, 0);
            Add("Spring_Pad", px, padBottom, 1, 1, 0, {}, 0, !cooling && !hot && !flash);
            Add("Spring_PadHot", px, padBottom, 1, 1, 0, {}, 0, hot || flash);
            Add("Spring_PadOff", px, padBottom, 1, 1, 0, {}, 0, cooling);
            Add("Spring_PadFill", px, padBottom - kPad, fill, 1, 0, {}, 0, cooling);
        }
        break;
    }
    // 伸縮橋：出ている間はマスいっぱいの石墨の箱に白い踏み板と青い筋。
    // 引っ込む時は板が中央から両端の受け金具へ縮み、出る時は両端から伸びる。引っ込んでいる間は
    // 仕掛けの青の破線で囲む。両端の受け金具は残し、通電灯を載せる。
    case Rules::DeviceKind::Bridge: {
        const bool out = d.solid;
        const double p = std::clamp(since / kSwitchSeconds, 0.0, 1.0);
        const bool moving = p < 1;
        const double half = r.w * (out ? p : 1 - p) * .5;   // 片側の受け金具から出ている長さ
        row("Bridge_Deck", out && !moving);
        Add("Bridge_Deck", r.x + half * .5, bottom, (std::max)(half, .01), r.h, 0, {}, -1, moving && half > .01);
        Add("Bridge_Deck", r.x + r.w - half * .5, bottom, (std::max)(half, .01), r.h, 0, {}, -1, moving && half > .01);
        // 受け金具も 1 マスずつ重ねる（扉の柱と同じ理由）。
        for (int cy = 0; cy < static_cast<int>(std::ceil(r.h - 1e-6)); ++cy) {
            Add("Bridge_Anchor", r.x + .09, bottom - cy, 1, (std::min)(1.0, r.h - cy));
            Add("Bridge_Anchor", r.x + r.w - .09, bottom - cy, 1, (std::min)(1.0, r.h - cy));
        }
        Lamp(r.x + .09, r.y + .6, d.powered, since);
        Lamp(r.x + r.w - .09, r.y + .6, d.powered, since);
        Outline(r.x, r.y, r.x + r.w, bottom, !out, true);
        break;
    }
    // 収集品：宙に浮く金の六角の硬貨。四つのこまを順に見せて縦軸で回し（毎秒 6 枚）、上下にゆっくり浮かせる。拾うと消える。
    case Rules::DeviceKind::Collectible: {
        const int frame = static_cast<int>(std::floor(time_ * 6.0)) % 4;
        const double bob = .06 * std::sin(time_ * 2.4);
        static constexpr std::array<std::string_view, 4> kGems{"Gem_0", "Gem_1", "Gem_2", "Gem_3"};
        for (int k = 0; k < 4; ++k) { Add(kGems[static_cast<size_t>(k)], x, bottom - bob, 1, 1, 0, {}, -1, !d.active && k == frame); }
        break;
    }
    // 操作の貼り絵：壁（背景板のすぐ手前）に描いたキーの絵。ad は A と D を左右に、space / spaceHold はスペースキー。
    // 素描の線で細部がつぶれないよう、キーは 1.4 マスの大きさで描く。
    case Rules::DeviceKind::Decal: {
        constexpr double kWall = -.83, kKey = 1.4;
        // 立体の組では壁の目盛りと同じだけ右上へずらす（壁に貼った絵として読める）。
        const double cx = x + projection_.ShiftX(kWall), cy = r.y + r.h * .5 + .5 * kKey + projection_.ShiftY(kWall);   // キーの絵を矩形の縦の中ほどへ
        // キーは押して見せる：A と D は交互に、スペースは短く、長押しは長く押したままにする。
        const double cycle = std::fmod(time_, 1.6);
        if (d.variant == "ad") {
            const bool a = cycle < .35, dd = cycle >= .8 && cycle < 1.15;
            Add("Key_A", cx - .75 * kKey, cy, kKey, kKey, kWall, {}, 0, !a);
            Add("Key_A_Down", cx - .75 * kKey, cy, kKey, kKey, kWall, {}, 0, a);
            Add("Key_D", cx + .75 * kKey, cy, kKey, kKey, kWall, {}, 0, !dd);
            Add("Key_D_Down", cx + .75 * kKey, cy, kKey, kKey, kWall, {}, 0, dd);
        } else if (d.variant == "space" || d.variant == "spaceHold") {
            const bool hold = d.variant == "spaceHold", down = hold ? cycle < 1.0 : std::fmod(time_, 1.2) < .25;
            Add(hold ? "Key_SpaceHold" : "Key_Space", cx, cy, kKey, kKey, kWall, {}, 0, !down);
            Add(hold ? "Key_SpaceHold_Down" : "Key_Space_Down", cx, cy, kKey, kKey, kWall, {}, 0, down);
        } else if (d.variant.starts_with("exit")) {
            // 出口の矢印（壁の緑の山形）。exit は右、exitLeft・exitUp・exitDown は面の中で回し、進む向きへ小さく往復させる。
            const double turn = d.variant == "exitLeft" ? kPi : d.variant == "exitUp" ? kPi * .5 : d.variant == "exitDown" ? -kPi * .5 : 0.0;
            const double push = .08 * (1 - std::cos(time_ * 4.5)) * .5;
            Add("Exit_Chevron", cx + std::cos(turn) * push, r.y + r.h * .5 + projection_.ShiftY(kWall) - std::sin(turn) * push, 1, 1, kWall, RotateZ(turn), 0);
        }
        break;
    }
    case Rules::DeviceKind::Mechanism: break;   // 画面の外の機構は画面に描かない
    case Rules::DeviceKind::LockWall:
        cells("LockWall_Tile", d.solid);
        Add("LockWall_Emblem", x, r.y + r.h * .5 + .35, 1, 1, 0, {}, -1, d.solid);
        Add("FX_Ring", x, bottom, .4, .4, .15, {}, -1, !d.solid);
        break;
    }
}

void StagePartBuilder::Beam(const Rules::DeviceSnapshot& laser, const Rules::BeamState* beam) {
    Begin("beam/" + laser.id);
    const auto direction = laser.direction;
    const bool horizontal = direction == Rules::Direction::Left || direction == Rules::Direction::Right;
    const bool shown = beam != nullptr && beam->rect.w > 0 && beam->rect.h > 0;
    // 光線は単位長の棒（中心が原点の高さ）。snapshot の長さへ伸ばし、縦は 90°回す。無い時は発射器の場所で隠す。
    // 棒は伸び縮みするので斜めに切っていない。立体の組では、発射器のレンズの奥行き（0.35）の分だけ右上へずらして合わせる。
    const Rules::Rect r = shown ? beam->rect : Rules::Rect{laser.rect.x, laser.rect.y, 1, 1};
    const double ox = projection_.ShiftX(-.35), oy = projection_.ShiftY(-.35);
    // 光線は細かく脈打つ（太さだけ。長さと位置は当たりのまま）。
    const double pulse = 1 + .14 * std::sin(time_ * 37.0) + .06 * std::sin(time_ * 91.0);
    if (horizontal) { Add("LaserBeam", r.x + r.w * .5 + ox, r.y + r.h * .5 + oy, r.w, pulse, 0, {}, -1, shown); }
    else { Add("LaserBeam", r.x + r.w * .5 + ox, r.y + r.h * .5 + oy, r.h, pulse, 0, RotateZ(kPi * .5), -1, shown); }
    // 当たりの火花は光線の先端に置く。
    Add("FX_Shard", (horizontal ? (direction == Rules::Direction::Left ? r.x : r.x + r.w) : r.x + r.w * .5) + ox,
        (horizontal ? r.y + r.h * .5 : (direction == Rules::Direction::Up ? r.y : r.y + r.h)) + .06 + oy, .12, .12, .3, {}, -1, shown);
}

// 姿勢（待機・走り・空中）と左右の向きごとに焼いたモデル。高さ 1 をルールの身長へ合わせる。
void StagePartBuilder::Actor(const Rules::BodyState& body, const Rules::Rect& r, bool echo, double time, bool shown, double grow) {
    static constexpr std::array<std::string_view, 18> kPlayerPoses{
        "Ninja_Idle0_R", "Ninja_Idle1_R", "Ninja_Jog0_R", "Ninja_Jog1_R", "Ninja_Jog2_R", "Ninja_Jog3_R", "Ninja_Jog4_R", "Ninja_Jog5_R", "Ninja_Air0_R",
        "Ninja_Idle0_L", "Ninja_Idle1_L", "Ninja_Jog0_L", "Ninja_Jog1_L", "Ninja_Jog2_L", "Ninja_Jog3_L", "Ninja_Jog4_L", "Ninja_Jog5_L", "Ninja_Air0_L"};
    static constexpr std::array<std::string_view, 2> kEchoPoses{"Ninja_Echo_R", "Ninja_Echo_L"};
    Begin(echo ? "echo" : "player");
    const bool left = body.facing < 0;
    size_t current = 0;
    if (echo) { current = left ? 1 : 0; }
    else {
        const size_t pose = !body.grounded ? 8
            : std::abs(body.walkVx) > .01 ? 2 + static_cast<size_t>(static_cast<int>(std::floor(time * 7.2)) % 6)
            : static_cast<size_t>(static_cast<int>(std::floor(time * 1.5)) % 2);
        current = pose + (left ? 9 : 0);
    }
    const bool visible = shown && !body.dead;
    const auto& poses = echo ? std::span<const std::string_view>(kEchoPoses) : std::span<const std::string_view>(kPlayerPoses);
    const double depth = projection_.ActorDepth() + (echo ? -.05 : 0);
    for (size_t i = 0; i < poses.size(); ++i) {
        // 使う一体だけを人物の位置へ。ほかは退避点に留め、人物が動いても触らない。
        if (visible && i == current) { Add(poses[i], r.x + r.w * .5, r.y + r.h, r.h, r.h * std::clamp(grow, .01, 1.0), depth); }
        else { Add(poses[i], kParkX, kParkY, 1, 1, depth, {}, -1, false); }
    }
}
}
