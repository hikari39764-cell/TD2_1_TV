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
// 背景の壁は遊びの平面より 0.85 格奥。目盛りは壁のすぐ手前。
constexpr double kBackdropDepth = -0.85, kGridDepth = -0.845;
// 傾きの隙間を塞ぐ下敷きは背景板より奥。Backdrop 部品の下から 15% までは光の輪の外（闇一色）。
constexpr double kUnderlayDepth = -1.2, kBackdropVoidBand = 0.15;
// 隠す部品を退避位置からさらに下へ退ける距離（舞台カメラの写す範囲より十分遠い）。
constexpr float kHiddenDrop = 2000.0f;

std::string_view DirectionName(Rules::Direction direction) {
    switch (direction) {
    case Rules::Direction::Down: return "Down";
    case Rules::Direction::Left: return "Left";
    case Rules::Direction::Right: return "Right";
    default: return "Up";
    }
}
// 扉の羽根・橋板が出入りする秒数（当たりは即座に変わるので短く）と、通電灯の光の輪が広がる秒数。
constexpr double kSwitchSeconds = 0.14, kPulseSeconds = 0.32;
// 8・4・2・1 のまとめ部品のうち、残りに収まる一番大きいもの。
int BundleOf(int left) { return left >= 8 ? 8 : left >= 4 ? 4 : left >= 2 ? 2 : 1; }
} // namespace

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
    // 俯角と側角を両方付けた時の傾きは部品の中ほどで釣り合わせ、格の中心からのずれを上下に半分ずつにする。
    // 光線は中心が原点なのでずらさない。
    const double pivot = leanPivot >= 0 ? leanPivot : (part == "LaserBeam" ? 0.0 : height * .5);
    request.worldTransform.position = projection_.Point(x + projection_.Lean() * pivot, bottomY, depth);
    request.worldTransform.rotation = rotation;
    const bool quarterTurn = std::abs(std::abs(rotation.z) - .70710678f) < .0001f && rotation.x == 0 && rotation.y == 0;
    // 正面の 1 格が像でもちょうど 1 格になるよう、俯角で縦、側角で横の縮みを補う。90°回した部品は軸を入れ替える。
    const double pitchCos = std::cos(projection_.pitch), yawCos = std::cos(projection_.yaw);
    request.worldTransform.scale = {static_cast<float>(width * projection_.scale / (quarterTurn ? pitchCos : yawCos)),
        static_cast<float>(height * projection_.scale / (quarterTurn ? yawCos : pitchCos)), static_cast<float>(projection_.scale)};
    // 陰影は部品に焼いてあり、捕捉は灯も影も使わない。visible=0 は引擎の表面一覧から外れて組の変化（場景の作り直し）に
    // なるので、表示のまま舞台カメラの写らない所へ退ける。
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
    for (int row = 0, left = rows + 1; left > 0;) {
        const int n = BundleOf(left);
        // 横線の束は下端から上へ n 本。最初の線が row + n - 1 に来るよう下端を合わせる。
        Add(kGridH[static_cast<size_t>(n)], x + cols * .5, y + row + n - 1, cols, 1, kGridDepth);
        row += n; left -= n;
    }
    for (int col = 0, left = cols + 1; left > 0;) {
        const int n = BundleOf(left);
        Add(kGridV[static_cast<size_t>(n)], x + col, y + rows, 1, rows, kGridDepth);
        col += n; left -= n;
    }
    // 俯角と側角を両方付けると背景板の上下の角が斜めにずれ、部屋の角に隙間ができる。Backdrop 部品の下の
    // 闇一色の帯が部屋を余白ごと覆うよう縦に引き伸ばした板を奥に敷き、隙間を同じ闇で塞ぐ。
    if (const double lean = std::abs(projection_.Lean()); lean > 1e-4) {
        const double margin = lean * rows + 1, tall = (rows + 2 * margin) / kBackdropVoidBand;
        Add("Backdrop", x + cols * .5, y + rows + margin, cols + 2 * margin, tall, kUnderlayDepth, {}, rows * .5 + margin);
    }
}

// 上が空いた格は象牙の天板付き（横の端が空いていれば天板を少し張り出す）、それ以外は墨の躯体。
// 隣の部屋の格も見るので、部屋の境を跨いで続く床は一続きに見える。部屋の無い所は埋まり扱い（天板を張り出さない）。
// 格ごとに出すと数千の描画実体になり引擎の上限を超えるので、天板は横の連なり（8・4・2・1 格）、
// 躯体は大きい塊（8×4 から 1×1）から順に当てはめる。どの電視も同じ部品なので見た目はそろう。
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

void StagePartBuilder::Lamp(double x, double bottomY, bool lit, double since) {
    Add("Signal_Lamp", x, bottomY, 1, 1, 0, {}, 0, lit);
    Add("Signal_LampOff", x, bottomY, 1, 1, 0, {}, 0, !lit);
    // 光の輪は灯の中心（灯は高さ 0.28）から広がる。圧力板と繋がった機関で同時に鳴るので、繋がりが一目で分かる。
    const double t = since / kPulseSeconds, size = .4 + .9 * std::clamp(t, 0.0, 1.0);
    Add("FX_Ring", x, bottomY - .14 + size * .5, size, size, .12, {}, -1, t >= 0 && t < 1);
}

void StagePartBuilder::Outline(double left, double top, double right, double bottom, bool visible) {
    // 1 格の破線部品を辺に沿って並べ、端の半端は縮める。
    const auto along = [&](std::string_view part, double from, double to, bool horizontal, double at) {
        for (double a = from; a < to - 1e-6; a += 1) {
            const double length = (std::min)(1.0, to - a);
            if (horizontal) { Add(part, a + length * .5, at, length, 1, .02, {}, 0, visible); }
            else { Add(part, at, a + length, 1, length, .02, {}, 0, visible); }
        }
    };
    along("Ghost_DashH", left, right, true, top);
    along("Ghost_DashH", left, right, true, bottom);
    along("Ghost_DashV", top, bottom, false, left);
    along("Ghost_DashV", top, bottom, false, right);
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
    // 圧力板：踏まれると沈み、正面の通電灯が点く（繋がった扉・橋の灯も同時に点く）。
    case Rules::DeviceKind::Plate: {
        const bool down = d.pressed || d.active;
        Add("Plate", x, bottom, r.w, 1, 0, {}, -1, !down);
        Add("Plate_Pressed", x, bottom, r.w, 1, 0, {}, -1, down);
        Lamp(x, bottom, d.powered, since);
        break;
    }
    case Rules::DeviceKind::Door: {
        // 扉：柱は矩形の高さ、鴨居は上端（正面に通電灯）、閉じた扉は 1 格ずつの象牙の羽根板。開く時は羽根が下から
        // 鴨居へ巻き上がって畳まれ、閉じる時は上から下りる（kSwitchSeconds）。開いている間は戸口を菫の破線で囲む
        // （「ここに扉が戻る」）。
        const bool open = d.active || !d.solid;
        const double p = std::clamp(since / kSwitchSeconds, 0.0, 1.0);
        const double leaves = r.h * (open ? 1 - p : p);   // 上から下りている羽根の高さ
        Add("Door_Jamb", x, bottom, r.w, r.h);
        Add("Door_Lintel", x, r.y + .36, r.w, 1);
        Add("Door_LeafFolded", x, r.y + .62, r.w, 1, 0, {}, -1, leaves < r.h - 1e-3);
        for (int cy = 0; cy < static_cast<int>(std::ceil(r.h - 1e-6)); ++cy) {
            const double part = std::clamp(leaves - cy, 0.0, 1.0);
            Add("Door_LeafTile", x, r.y + cy + part, r.w, (std::max)(part, .01), 0, {}, -1, part > .01);
        }
        Lamp(x, r.y + .33, d.powered, since);
        Outline(x - .34 * r.w, r.y + .62, x + .34 * r.w, bottom, open);
        break;
    }
    case Rules::DeviceKind::Laser: Add(std::string("Laser_") + std::string(DirectionName(d.direction)), x, bottom, r.w, r.h); break;
    case Rules::DeviceKind::Spikes: cells(std::string("Spikes_") + std::string(DirectionName(d.direction)), true); break;
    case Rules::DeviceKind::Checkpoint:
        Add("Checkpoint_Idle", x, bottom, r.w, 1, 0, {}, -1, !checkpoint);
        Add("Checkpoint_Active", x, bottom, r.w, 1, 0, {}, -1, checkpoint);
        break;
    // 弾簧：格ごとに台・ばね・踏み板と、その上の太い上向きの矢印（打ち上げる向き）。
    // 使える間は踏み板の縁と矢印が光る。乗って溜めている間は踏み板全体が白く光る。
    // 冷却中は踏み板の灯が消え、矢印は空の枠に戻ってから下から満ちていく（満ちたら使える）。
    case Rules::DeviceKind::Spring: {
        const bool cooling = d.cooldown > 0, hot = !cooling && d.charge > 0;
        const double fill = cooling ? std::clamp(1.0 - d.cooldown, .04, 1.0) : 1.0;
        const double arrowBottom = bottom - .43;
        for (double cx = 0; cx < r.w; cx += 1) {
            const double px = r.x + cx + .5;
            Add("Spring", px, bottom, 1, 1, 0, {}, -1, !cooling && !hot);
            Add("Spring_Hot", px, bottom, 1, 1, 0, {}, -1, hot);
            Add("Spring_Off", px, bottom, 1, 1, 0, {}, -1, cooling);
            Add("Spring_ArrowDim", px, arrowBottom, 1, 1, 0, {}, 0);
            Add("Spring_Arrow", px, arrowBottom, 1, fill, 0, {}, 0);
        }
        break;
    }
    // 伸縮橋：当たりは格いっぱいなので、出ている間は格いっぱいの象牙の羽目板（扉の羽根と同じ「動く機関」の材質）。
    // 引っ込む時は板が中央から両端の受け金具へ縮み、出る時は両端から伸びる（kSwitchSeconds）。引っ込んでいる間は
    // 同じ所を菫の破線で囲む。両端の受け金具は残り、通電灯を載せる。
    case Rules::DeviceKind::Bridge: {
        const bool out = d.solid;
        const double p = std::clamp(since / kSwitchSeconds, 0.0, 1.0);
        const bool moving = p < 1;
        const double half = r.w * (out ? p : 1 - p) * .5;   // 片側の受け金具から出ている長さ
        row("Bridge_Deck", out && !moving);
        Add("Bridge_Deck", r.x + half * .5, bottom, (std::max)(half, .01), r.h, 0, {}, -1, moving && half > .01);
        Add("Bridge_Deck", r.x + r.w - half * .5, bottom, (std::max)(half, .01), r.h, 0, {}, -1, moving && half > .01);
        Add("Bridge_Anchor", r.x + .09, bottom, 1, r.h);
        Add("Bridge_Anchor", r.x + r.w - .09, bottom, 1, r.h);
        Lamp(r.x + .09, r.y + .6, d.powered, since);
        Lamp(r.x + r.w - .09, r.y + .6, d.powered, since);
        Outline(r.x, r.y, r.x + r.w, bottom, !out);
        break;
    }
    case Rules::DeviceKind::Collectible: Add("Checkpoint_Idle", x, bottom, r.w, 1, 0, {}, -1, !d.active); break;
    case Rules::DeviceKind::Decal: break;
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
    const Rules::Rect r = shown ? beam->rect : Rules::Rect{laser.rect.x, laser.rect.y, 1, 1};
    if (horizontal) { Add("LaserBeam", r.x + r.w * .5, r.y + r.h * .5, r.w, 1, 0, {}, -1, shown); }
    else { Add("LaserBeam", r.x + r.w * .5, r.y + r.h * .5, r.h, 1, 0, RotateZ(kPi * .5), -1, shown); }
    // 当たりの火花は光線の先端（世界は一続きなので、途中で切った端ではない）。
    Add("FX_Shard", horizontal ? (direction == Rules::Direction::Left ? r.x : r.x + r.w) : r.x + r.w * .5,
        (horizontal ? r.y + r.h * .5 : (direction == Rules::Direction::Up ? r.y : r.y + r.h)) + .06, .12, .12, .3, {}, -1, shown);
}

// 姿勢（待機・走り・空中）と左右の向きごとに焼いた模型。高さ 1 を Rules の身長へ合わせる。
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
    const double depth = kActorDepth + (echo ? -.05 : 0);
    for (size_t i = 0; i < poses.size(); ++i) {
        // 使う一体だけを人物の位置へ。ほかは退避点に留め、人物が動いても触らない。
        if (visible && i == current) { Add(poses[i], r.x + r.w * .5, r.y + r.h, r.h, r.h * std::clamp(grow, .01, 1.0), depth); }
        else { Add(poses[i], kParkX, kParkY, 1, 1, depth, {}, -1, false); }
    }
}
} // namespace TD2
