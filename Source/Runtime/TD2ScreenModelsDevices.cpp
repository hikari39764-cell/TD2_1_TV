// 舞台の仕掛けの部品：板・扉・橋・バネ・光線・押し刺・検査点・宝石・ゴール・札。
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
std::string_view DirectionName(Rules::Direction direction) {
    switch (direction) {
    case Rules::Direction::Down: return "Down";
    case Rules::Direction::Left: return "Left";
    case Rules::Direction::Right: return "Right";
    default: return "Up";
    }
}
// 扉の羽根・橋板が出入りする秒数（当たりは即座に変わるので短く）。
constexpr double kSwitchSeconds = 0.14;
// バネの冷却が終わった時に踏み板が一度光る秒数。
constexpr double kReadyPopSeconds = 0.28;
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
    // 通関の扉：柱と楣（上にアンテナ）の枠に、光る画面を嵌めた門。開いている間は画面が白く光り、閉じている（通電していない）
    // 間は左右の戸が画面の前で閉じる。本体が入ると戸が閉じ、合わせ目の光が縦の線から点になって消える（ブラウン管の電源を切った時）。
    // 戸と画面は斜めに切っていない平らな板なので、立体の組では奥行きの分だけ右上へずらす。
    case Rules::DeviceKind::Goal: {
        const double k = (std::min)(r.w / 2, r.h / 3);            // 幅 2・高さ 3 の形のまま矩形に収める
        constexpr double kHalf = .62, kLow = .2, kTall = 2.28;      // 画面の半幅・下端・高さ（枠の中、1 マス = 1）
        constexpr double kScreenDepth = -.55, kLeafDepth = -.46;
        const bool entered = d.pressed || goalClosing_ >= 0;
        const double closing = goalClosing_ >= 0 ? goalClosing_ : 1e3;
        // 戸の開き（1 = 開いた）。入った後は 0.15〜0.75 秒で閉じる。通電の切り替えは 0.35 秒。
        const double open = entered ? 1 - Math::SmoothStep((closing - .15) / .6) : d.active ? Math::SmoothStep(since / .35) : 1 - Math::SmoothStep(since / .35);
        Add("Goal_Frame", x, bottom, k, k);
        const double sx = projection_.ShiftX(kScreenDepth), sy = projection_.ShiftY(kScreenDepth);
        const double cy = bottom - (kLow + kTall * .5) * k;
        const bool lit = d.active && (!entered || closing < .75);
        Add("Goal_Screen", x + sx, cy + sy, 2 * kHalf * k, kTall * k, kScreenDepth, {}, -1, lit);
        Add("Goal_ScreenOff", x + sx, cy + sy, 2 * kHalf * k, kTall * k, kScreenDepth - .01, {}, -1, !lit);
        const double lx = projection_.ShiftX(kLeafDepth), ly = projection_.ShiftY(kLeafDepth), leaf = kHalf * k * (1 - open);
        const double leafBottom = bottom - kLow * k;
        Add("Goal_LeafL", x - kHalf * k + lx, leafBottom + ly, (std::max)(leaf, .001), kTall * k, kLeafDepth, {}, 0, leaf > .002);
        Add("Goal_LeafR", x + kHalf * k + lx, leafBottom + ly, (std::max)(leaf, .001), kTall * k, kLeafDepth, {}, 0, leaf > .002);
        // 閉じた合わせ目の光：0.75〜0.95 秒で縦の線が点に縮み、1.05 秒までに消える。
        const double seam = closing - .75;
        const double line = seam < .2 ? 1 - Math::SmoothStep(seam / .2) * .97 : .03 * (1 - Math::SmoothStep((seam - .2) / .1));
        const double fx = projection_.ShiftX(kLeafDepth + .02), fy = projection_.ShiftY(kLeafDepth + .02);
        Add("Goal_Screen", x + fx, cy + fy, .06 * k, (std::max)(kTall * line, .001) * k, kLeafDepth + .02, {}, -1, entered && seam >= 0 && seam < .3);
        Lamp(x, bottom - 2.62 * k, d.powered && !entered, since);
        break;
    }
    case Rules::DeviceKind::Laser: LaserBody(d); break;
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
        // 弾簧の印：踏み板の上に浮かぶ光る緑の「^」（当たりは無い）。押し板と同じ平たい形に見えないよう、上へ飛ぶ向きを示す。
        // ゆっくり上下し、溜めている間は速く跳ね、冷却中は隠す。幅が 2 マス以上なら二つ並べる。
        {
            const double rate = hot ? 9.0 : 2.6, lift = hot ? .07 : .045;
            const double bob = lift * (.5 + .5 * std::sin(time_ * rate + r.x * 1.7));
            const double padTop = bottom - kBase - kCoil * squeeze - kPad;
            const int arrows = r.w >= 2 ? 2 : 1;
            // 二段の「^^」：下は大きく、上は小さく。上の段は少し遅れて跳ねる。
            const double bob2 = lift * (.5 + .5 * std::sin(time_ * rate + r.x * 1.7 - .9));
            for (int k = 0; k < arrows; ++k) {
                const double ax = r.x + r.w * (k + .5) / arrows;
                Add("Spring_Arrow", ax, padTop - .16 - bob, 1, 1, 0, {}, 0, !cooling);
                Add("Spring_Arrow", ax, padTop - .48 - bob2, .75, .75, 0, {}, 0, !cooling);
            }
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
    // 操作の貼り絵：後ろの壁に描いたキーの札（平らな枠と文字）。ad は A と D を左右に、space / spaceHold はスペースキー。
    // 立体では背景として描かれ、輪郭も影も付かない（跳べる足場に見えない）。
    // 地形の 1 マスより小さく見える kSignSize マスの大きさで描く。
    case Rules::DeviceKind::Decal: {
        // 作者が置く前景：玩法の面より depth マス手前の暗い塊（fore）か、上から垂れる線（foreCable）。
        // 実体の地形（受光と透視の窓）の時だけ描く。平らな画面では手前に浮いた物の意味が無い。
        if (d.variant == "fore" || d.variant == "foreCable") {
            const double depth = std::clamp(d.depth, .25, 6.0);
            if (d.variant == "fore") {
                partDepth_ = .8;
                Add("Fore_Block", x, bottom, r.w, r.h, depth, {}, -1, solid_);
                partDepth_ = 1;
            } else {
                Add("Solid_RodY", x, bottom, 1.6, r.h, depth, {}, 0, solid_);
            }
            break;
        }
        constexpr double kWall = kSignWall;
        // 立体の組では壁の目盛りと同じだけ右上へずらす（壁に貼った絵として読める）。
        const double cx = x + projection_.ShiftX(kWall), cy = r.y + r.h * .5 + .5 * kSignSize + projection_.ShiftY(kWall);   // キーの絵を矩形の縦の中ほどへ
        // キーは押して見せる：A と D は交互に、スペースは短く、長押しは長く押したままにする。
        const double cycle = std::fmod(time_, 1.6);
        if (d.variant == "ad") {
            const bool a = cycle < .35, dd = cycle >= .8 && cycle < 1.15;
            Add("Key_A", cx - .75 * kSignSize, cy, kSignSize, kSignSize, kWall, {}, 0, !a);
            Add("Key_A_Down", cx - .75 * kSignSize, cy, kSignSize, kSignSize, kWall, {}, 0, a);
            Add("Key_D", cx + .75 * kSignSize, cy, kSignSize, kSignSize, kWall, {}, 0, !dd);
            Add("Key_D_Down", cx + .75 * kSignSize, cy, kSignSize, kSignSize, kWall, {}, 0, dd);
        } else if (d.variant == "space" || d.variant == "spaceHold") {
            const bool hold = d.variant == "spaceHold", down = hold ? cycle < 1.0 : std::fmod(time_, 1.2) < .25;
            Add(hold ? "Key_SpaceHold" : "Key_Space", cx, cy, kSignSize, kSignSize, kWall, {}, 0, !down);
            Add(hold ? "Key_SpaceHold_Down" : "Key_Space_Down", cx, cy, kSignSize, kSignSize, kWall, {}, 0, down);
        } else if (d.variant.starts_with("exit")) {
            // 出口の矢印（壁の緑の山形）。exit は右、exitLeft・exitUp・exitDown は面の中で回し、進む向きへ小さく往復させる。
            const double turn = d.variant == "exitLeft" ? kPi : d.variant == "exitUp" ? kPi * .5 : d.variant == "exitDown" ? -kPi * .5 : 0.0;
            const double push = .08 * (1 - std::cos(time_ * 4.5)) * .5;
            Add("Exit_Chevron", cx + std::cos(turn) * push, r.y + r.h * .5 + projection_.ShiftY(kWall) - std::sin(turn) * push, 1, 1, kWall, RotateZ(turn), 0);
        }
        break;
    }
    case Rules::DeviceKind::Pusher: PusherParts(d, true, since); break;
    case Rules::DeviceKind::Mechanism: break;   // 画面の外の機構は画面に描かない
    case Rules::DeviceKind::LockWall:
        cells("LockWall_Tile", d.solid);
        Add("LockWall_Emblem", x, r.y + r.h * .5 + .35, 1, 1, 0, {}, -1, d.solid);
        Add("FX_Ring", x, bottom, .4, .4, .15, {}, -1, !d.solid);
        break;
    }
}

void StagePartBuilder::PusherBody(const Rules::DeviceSnapshot& pusher) {
    Begin("device/" + pusher.id);
    PusherParts(pusher, false, 1e3);
}

// 押し刺：伸縮橋と同じ石墨の箱を今の位置に 1 マスずつ並べ（縦に伸ばすと立体の組で天板の斜めまで伸びる）、
// 元の位置から箱の背中まで灰色の腕を伸ばす。刺の面には赤い丸鋸を並べ、大半を箱に沈めて縁だけ出す（当たりの帯と同じ 4/32）。
// 圧力板につながる物は箱の上に通電灯を載せる（往復する物は載せない）。
void StagePartBuilder::PusherParts(const Rules::DeviceSnapshot& d, bool saws, double since) {
    const auto& r = d.rect;
    for (double cy = 0; cy < r.h - 1e-6; cy += 1) for (double cx = 0; cx < r.w - 1e-6; cx += 1) {
        Add("Bridge_Deck", r.x + cx + .5, r.y + cy + 1, 1, 1);
    }
    const double reach = d.travel * d.push, length = (std::max)(reach, .01);
    const bool out = reach > .02;
    const double mx = r.x + r.w * .5, my = r.y + r.h * .5;
    // 棒の部品は原点から上へ伸びる。横の腕は 90°回す（正の角は画面の上で反時計回り＝上向きが左向きになる）。
    switch (d.direction) {
    case Rules::Direction::Up: Add("Solid_RodY", mx, r.y + r.h + reach, 1.2, length, 0, {}, 0, out); break;
    case Rules::Direction::Down: Add("Solid_RodY", mx, r.y, 1.2, length, 0, {}, 0, out); break;
    case Rules::Direction::Left: Add("Solid_RodY", r.x + r.w, my, 1.2, length, 0, RotateZ(-kPi * .5), 0, out); break;
    default: Add("Solid_RodY", r.x, my, 1.2, length, 0, RotateZ(kPi * .5), 0, out); break;
    }
    if (!d.timed) { Lamp(mx, r.y + .3, d.powered, since); }
    if (!saws) { return; }
    const double ox = projection_.ShiftX(-.3), oy = projection_.ShiftY(-.3);
    const bool horizontal = d.direction == Rules::Direction::Left || d.direction == Rules::Direction::Right;
    const int count = static_cast<int>(std::ceil((horizontal ? r.h : r.w) - 1e-6));
    constexpr double kInset = .38;   // 刃の中心を面から箱の中へ寄せる量（刃の半径 .5 のうち .12 だけ出る）
    for (int k = 0; k < count; ++k) {
        double sx = r.x + k + .5, sy = r.y + k + .5;
        switch (d.direction) {
        case Rules::Direction::Up: sy = r.y + kInset; break;
        case Rules::Direction::Down: sy = r.y + r.h - kInset; break;
        case Rules::Direction::Left: sx = r.x + kInset; break;
        default: sx = r.x + r.w - kInset; break;
        }
        Add("Spikes_Saw", sx + ox, sy + oy, 1, 1, 0, RotateZ(-time_ * 4.5 + k * .31), 0);
    }
}

void StagePartBuilder::LaserBody(const Rules::DeviceSnapshot& laser) {
    Begin("device/" + laser.id);
    const auto& r = laser.rect;
    Add(std::string("Laser_") + std::string(DirectionName(laser.direction)), r.x + r.w * .5, r.y + r.h, r.w, r.h);
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
    const double pulse = (1 + .14 * std::sin(time_ * 37.0) + .06 * std::sin(time_ * 91.0)) * (horizontal ? r.h : r.w) / (4.0 / 32);
    if (horizontal) { Add("LaserBeam", r.x + r.w * .5 + ox, r.y + r.h * .5 + oy, r.w, pulse, 0, {}, -1, shown); }
    else { Add("LaserBeam", r.x + r.w * .5 + ox, r.y + r.h * .5 + oy, r.h, pulse, 0, RotateZ(kPi * .5), -1, shown); }
    // 当たりの火花は光線の先端に置く。
    Add("FX_Shard", (horizontal ? (direction == Rules::Direction::Left ? r.x : r.x + r.w) : r.x + r.w * .5) + ox,
        (horizontal ? r.y + r.h * .5 : (direction == Rules::Direction::Up ? r.y : r.y + r.h)) + .06 + oy, .12, .12, .3, {}, -1, shown);
}
}
