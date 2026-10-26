#include "TD2TitleCard.h"
#include "HIKARI/Project/ActionViews.h"
#include "HIKARI/Project/CameraViewSnapshot.h"
#include "HIKARI/Project/UiTypes.h"
#include "HIKARI/Project/UiView.h"
#include <algorithm>
#include <array>
#include <cmath>

namespace TD2 {
namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr std::string_view kPaper = "TD2/Intro/T_TD2_White.png";
// 貼り絵（黒い字と、負片・ずれ・縁取りに使う白い版）と、参考の構図（1920×1080）での大きさ。
struct Art { std::string_view black, white; float w, h; };
constexpr Art kBi{"UI/Title/T_Title_Bi.png", "UI/Title/T_Title_Bi_White.png", 125, 143};
constexpr Art kJi{"UI/Title/T_Title_Ji.png", "UI/Title/T_Title_Ji_White.png", 137, 143};
constexpr Art kYo{"UI/Title/T_Title_Yo.png", "UI/Title/T_Title_Yo_White.png", 79, 143};
// 「ン」は「_N」で終わる名前だと取り込みが法線の貼り絵と見なすので、Nn と名付ける。
constexpr Art kN{"UI/Title/T_Title_Nn.png", "UI/Title/T_Title_Nn_White.png", 128, 143};
constexpr Art kVision{"UI/Title/T_Title_Vision.png", "UI/Title/T_Title_Vision_White.png", 241, 65};
constexpr Art kVisionLarge{"UI/Title/T_Title_VisionLarge.png", "UI/Title/T_Title_VisionLarge_White.png", 363, 98};
constexpr Art kVisionSmall{"UI/Title/T_Title_VisionSmall.png", "UI/Title/T_Title_VisionSmall_White.png", 304, 82};
constexpr Art kVisionSmallAlt{"UI/Title/T_Title_VisionSmallAlt.png", "UI/Title/T_Title_VisionSmallAlt_White.png", 304, 82};
constexpr Art kVisionTurned{"UI/Title/T_Title_VisionTurned.png", "UI/Title/T_Title_VisionTurned_White.png", 122, 451};
constexpr Art kVisionUpsideDown{"UI/Title/T_Title_VisionUpsideDown.png", "UI/Title/T_Title_VisionUpsideDown_White.png", 452, 123};
constexpr Art kKanaMirrorHigh{"UI/Title/T_Title_KanaMirrorHigh.png", "UI/Title/T_Title_KanaMirrorHigh_White.png", 433, 97};
constexpr Art kKanaMirrorLow{"UI/Title/T_Title_KanaMirrorLow.png", "UI/Title/T_Title_KanaMirrorLow_White.png", 649, 145};
constexpr Art kKanaCorner{"UI/Title/T_Title_KanaCorner.png", "UI/Title/T_Title_KanaCorner_White.png", 190, 79};
constexpr Art kKanaTurned{"UI/Title/T_Title_KanaTurned.png", "UI/Title/T_Title_KanaTurned_White.png", 144, 460};
constexpr Art kStrokeCorner{"UI/Title/T_Title_StrokeCorner.png", "UI/Title/T_Title_StrokeCorner_White.png", 223, 314};
constexpr Art kStrokeGiant{"UI/Title/T_Title_StrokeGiant.png", "UI/Title/T_Title_StrokeGiant_White.png", 754, 597};

// 参考の構図：中心の「ビジョン」（字ごとの中心）と「Vision」、周りの碎片（左上の位置）。
constexpr double kCenterX = 996, kCenterY = 482;
constexpr double kLogoX = 675, kLogoY = 411, kLogoW = 642, kLogoH = 143, kLogoBottom = 660;
struct Glyph { const Art* art; double cx, cy; };
constexpr std::array<Glyph, 4> kGlyphs{{{&kBi, 737.5, 482.5}, {&kJi, 909.5, 482.5}, {&kYo, 1075.5, 482.5}, {&kN, 1252, 482.5}}};
constexpr Glyph kSub{&kVision, 962.5, 627.5};
// vx・vy：漂う速さ（画素/秒）、depth：推し込みで外へ逃げる度合い、ro：炸裂で余分に回る角度、flip：カードの始めから
// 何秒で鏡のように一度裏返るか（負なら無し）、sweep：巨大な笔画が時計の針のようにゆっくり回る。
struct Satellite { const Art* art; double x, y, vx, vy, depth, ro, flip; bool sweep; };
constexpr std::array<Satellite, 11> kSatellites{{
    {&kVisionLarge, 261, 95, 22, 0, 1.10, -150, 1.62, false},
    {&kKanaMirrorHigh, 743, 60, -18, 0, 1.08, 170, -1, false},
    {&kVisionSmallAlt, 1468, 70, 16, 0, 1.14, -120, -1, false},
    {&kKanaCorner, 34, 17, -14, 4, 1.20, 200, -1, false},
    {&kVisionTurned, 59, 264, 0, 20, 1.12, -210, 2.95, false},
    {&kVisionUpsideDown, 161, 784, -24, 0, 1.10, 190, -1, false},
    {&kStrokeCorner, 0, 765, -10, 10, 1.22, -240, -1, false},
    {&kKanaMirrorLow, 656, 904, -20, 0, 1.08, 160, 2.30, false},
    {&kVisionSmall, 1366, 967, 18, 0, 1.12, -130, -1, false},
    {&kKanaTurned, 1772, 620, 0, 16, 1.18, 220, -1, false},
    {&kStrokeGiant, 1166, 0, 0, 0, 1.25, 90, -1, true},
}};
// 噴き出す碎片の種類（巨大な笔画は別に、31 個に 1 個）。
constexpr std::array<const Art*, 14> kPool{&kVision, &kVisionLarge, &kVisionSmall, &kVisionUpsideDown, &kKanaMirrorHigh, &kKanaMirrorLow,
                                           &kKanaCorner, &kVisionTurned, &kKanaTurned, &kStrokeCorner, &kBi, &kJi, &kYo, &kN};
// 噴出の始めの 1 秒あたりの数（ここから一番激しい時の数まで指数で増える）と、念のための上限。
constexpr double kSprayStartRate = 8;
constexpr int kSprayLimit = 400;

double Clamp01(double v) { return std::clamp(v, 0.0, 1.0); }
double Smooth(double v) { v = Clamp01(v); return v * v * (3 - 2 * v); }
double InCubic(double v) { v = Clamp01(v); return v * v * v; }
double OutBack(double v) { v = Clamp01(v); const double c = 1.7; return 1 + (c + 1) * std::pow(v - 1, 3) + c * std::pow(v - 1, 2); }
// 0〜1 の決まった乱数。
double Hash(double n) { const double s = std::sin(n * 127.1) * 43758.5453; return s - std::floor(s); }

// 画に描く一枚：中心の位置・大きさ（画の画素）、回転（ラジアン）、不透明度、白い縁の太さ（0 で無し）。
struct Sprite { const Art* art; double cx, cy, w, h, rot, alpha, halo; };

// 一コマを組み立てて描く。
class Frame {
public:
    Frame(HIKARI::WorldView& world, const TitleCardTimes& times, const TitleCardLook& look, double t)
        : ui_(world.GetUi()), c_(times), look_(look), t_(t) {
        HIKARI::GameProjectCameraViewSnapshot view{};
        if (world.GetCamera().TryGetViewSnapshot(view) && view.uiLogicalWidth > 0 && view.uiLogicalHeight > 0) {
            width_ = view.uiLogicalWidth; height_ = view.uiLogicalHeight;
        }
        // 参考の構図を画いっぱいに覆うように置く（16:9 でない画では端が少し切れる）。
        scale_ = (std::max)(width_ / 1920, height_ / 1080);
        offsetX_ = (width_ - 1920 * scale_) * .5; offsetY_ = (height_ - 1080 * scale_) * .5;
    }

    void Draw() {
        const double on = t_ - c_.in, off = on - (c_.seconds - .36), line = (std::max)(2.0, height_ * .004);
        const double cx = width_ * .5, cy = height_ * .5;
        // ブラウン管の点灯（点 → 線 → 開く）と熄灭（閉じる → 線 → 点）。
        if (on < .12) { const double d = line * (1 + 2 * Smooth(on / .12)); Put(kPaper, cx, cy, d, d, 0, {1, 1, 1, 1}); return; }
        if (on < .28) { Put(kPaper, cx, cy, line * 3 + (width_ - line * 3) * Smooth((on - .12) / .16), line, 0, {1, 1, 1, 1}); return; }
        if (off >= .24) { const double d = line * 3 * (1 - Smooth((off - .24) / .12)); if (d > .5) Put(kPaper, cx, cy, d, d, 0, {1, 1, 1, 1}); return; }
        if (off >= .12) { Put(kPaper, cx, cy, line * 3 + (width_ - line * 3) * (1 - Smooth((off - .12) / .12)), line, 0, {1, 1, 1, 1}); return; }
        const double open = on < .42 ? Smooth((on - .28) / .14) : off >= 0 ? 1 - Smooth(off / .12) : 1;
        ReadGlitch();
        // 紙：少しちらつく白（負片のコマは黒）。
        const float paper = static_cast<float>(.93 + .03 * Hash(std::floor(t_ * 12)));
        const double cardH = line + (height_ - line) * open;
        Put(kPaper, cx, cy, width_, cardH, 0, negative_ ? HIKARI::GameProjectUiColor{0, 0, 0, 1} : HIKARI::GameProjectUiColor{paper, paper, paper * .985f, 1});
        Collect();
        // 開く・閉じる間は、縦に潰して画の中心へ寄せる。
        if (open < 1) for (auto& s : sprites_) { s.cy = cy + (s.cy - cy) * open; s.h *= open; }
        // 赤青のずれ（字の下に、白い版を赤と青に染めて左右へずらす）。
        if (glitch_ >= 1) {
            for (const auto& s : sprites_) Put(s.art->white, s.cx - 8 * scale_, s.cy, s.w, s.h, s.rot, {.88f, .22f, .29f, static_cast<float>(s.alpha * .7)});
            for (const auto& s : sprites_) Put(s.art->white, s.cx + 8 * scale_, s.cy, s.w, s.h, s.rot, {.12f, .72f, .79f, static_cast<float>(s.alpha * .7)});
        }
        for (const auto& s : sprites_) {
            const auto ink = negative_ ? s.art->white : s.art->black;
            if (s.halo > 0) {
                // 縁：8 方向へずらした紙の色の版を先に敷く（どれだけ散らかっても标题が読める）。
                const auto rim = negative_ ? s.art->black : s.art->white;
                const HIKARI::GameProjectUiColor color = negative_ ? HIKARI::GameProjectUiColor{1, 1, 1, static_cast<float>(s.alpha)}
                                                                   : HIKARI::GameProjectUiColor{paper, paper, paper * .985f, static_cast<float>(s.alpha)};
                for (int k = 0; k < 8; ++k) {
                    const double a = k * kPi / 4;
                    Put(rim, s.cx + std::cos(a) * s.halo, s.cy + std::sin(a) * s.halo, s.w, s.h, s.rot, color);
                }
            }
            Put(ink, s.cx, s.cy, s.w, s.h, s.rot, {1, 1, 1, static_cast<float>(s.alpha)});
        }
    }

private:
    // 乱れ：一つの区間の最初のコマが負片、残りが赤青のずれと横の段ずれ。始まった瞬間は画を一度突く。
    void ReadGlitch() {
        for (const auto& [start, length] : c_.glitches) {
            if (t_ >= start && t_ < start + (std::max)(length, 1.0 / 30)) glitch_ = static_cast<int>(std::floor((t_ - start) * 30));
            if (t_ >= start && t_ < start + .12) hit_ = (std::max)(hit_, 1 - (t_ - start) / .12);
        }
        negative_ = glitch_ == 0;
        if (glitch_ >= 1) {
            bandTop_ = 300 + 400 * Hash(glitch_ + t_); bandBottom_ = bandTop_ + 140;
            bandShift_ = (Hash(t_ * 7) > .5 ? 1 : -1) * 38.0;
        }
    }

    // 参考の構図での点を、推し込み・回転・震え・段ずれの後の画の画素へ写す。
    void Place(double x, double y, double depth, double& sx, double& sy) const {
        const double spread = 1 + (zoom_ - 1) * depth * 1.6;
        double px = 960 + (x - 960) * spread, py = 540 + (y - 540) * spread;
        const double dx = px - 960, dy = py - 540, cs = std::cos(turn_), sn = std::sin(turn_);
        px = 960 + dx * cs - dy * sn + shakeX_; py = 540 + dx * sn + dy * cs + shakeY_;
        if (glitch_ >= 1 && py > bandTop_ && py < bandBottom_) px += bandShift_;
        sx = offsetX_ + px * scale_; sy = offsetY_ + py * scale_;
    }
    void Add(const Art* art, double x, double y, double depth, double size, double widthScale, double rotDeg, double alpha, double halo) {
        if (alpha <= .003 || size <= 0) return;
        double sx = 0, sy = 0;
        Place(x, y, depth, sx, sy);
        const double w = art->w * size * zoom_ * scale_ * widthScale, h = art->h * size * zoom_ * scale_;
        const double r = (std::max)(w, h);
        if (sx < -r || sx > width_ + r || sy < -r || sy > height_ + r) return;
        sprites_.push_back({art, sx, sy, w, h, rotDeg * kPi / 180 + turn_, Clamp01(alpha), halo * zoom_ * scale_});
    }

    void Collect() {
        // 全体：炸裂から巻き戻しまでの間に 12% 推し込み 3° 回る。震えは噴出とともに大きくなり、巻き戻しで静まる。
        const double prog = Smooth((t_ - c_.burst) / (std::max)(.1, c_.rewind - c_.burst));
        zoom_ = 1 + .12 * prog + .035 * hit_;
        turn_ = -3 * kPi / 180 * prog;
        const double sprayLength = (std::max)(.1, c_.sprayEnd - c_.spray);
        double shake = (3 + 20 * std::pow(Smooth((t_ - c_.spray - .1) / (std::max)(.1, sprayLength - .1)), 2)) * Smooth((t_ - c_.burst) / .2);
        if (t_ > c_.rewind) shake *= 1 - Smooth((t_ - c_.rewind) / c_.rewindSeconds);
        const double frame = std::floor(t_ * 60);
        shakeX_ = (Hash(frame * 1.7) - .5) * 2 * shake; shakeY_ = (Hash(frame * 2.3) - .5) * 2 * shake;

        // 周りの碎片：速く動く炸裂と巻き戻しの間は、少し前の自分を薄く三つ残す（残影）。
        for (size_t i = 0; i < kSatellites.size(); ++i) {
            const bool moving = (t_ > c_.burst && t_ < c_.burst + .83) || (t_ > c_.rewind && t_ < c_.rewind + c_.rewindSeconds);
            static constexpr std::array<std::array<double, 2>, 4> kTrail{{{.105, .07}, {.07, .13}, {.035, .22}, {0, 1}}};
            for (size_t k = moving ? 0 : 3; k < kTrail.size(); ++k) AddSatellite(kSatellites[i], i, t_ - kTrail[k][0], kTrail[k][1]);
        }
        // 噴き出す碎片（标题の下）。一つずつ二つの残影を付ける。
        const double e0 = c_.spray, e1 = (std::max)(c_.spray + .1, c_.sprayEnd);
        const double peak = (std::max)(kSprayStartRate + 1, static_cast<double>(look_.sprayPeak));
        const double grow = std::log(peak / kSprayStartRate) / (e1 - e0);
        const int count = t_ < e0 ? 0 : (std::min)(kSprayLimit, static_cast<int>(kSprayStartRate / grow * (std::exp(grow * ((std::min)(t_, e1) - e0)) - 1)));
        for (int i = 0; i < count; ++i) {
            const double born = e0 + std::log(1 + i * grow / kSprayStartRate) / grow;
            static constexpr std::array<std::array<double, 2>, 3> kTrail{{{.05, .1}, {.025, .22}, {0, 1}}};
            for (const auto& [dt, a] : kTrail) AddSpray(i, born, e1, t_ - dt, a);
        }
        // 中心：逐字落ちてきて少し跳ね返る。噴出が激しいほど震え、巻き戻しの終わりに一度突かれる。
        const double punch = 1 + .06 * std::sin(kPi * Clamp01((t_ - (c_.rewind + c_.rewindSeconds - .22)) / .2));
        const double quake = t_ > c_.rewind ? 0 : 7 * std::pow(Smooth((t_ - c_.spray - .3) / (std::max)(.1, sprayLength - .3)), 2);
        for (size_t i = 0; i < kGlyphs.size(); ++i) {
            const double st = c_.burst + .13 + i * .07;
            if (t_ < st) continue;
            const double p = OutBack((t_ - st) / .42), f = std::floor(t_ * 60);
            const double x = kCenterX + (kGlyphs[i].cx - kCenterX) * punch + (Hash(f * 3.3 + i) - .5) * 2 * quake;
            const double y = kCenterY + (kGlyphs[i].cy - kCenterY) * punch - 110 * (1 - p) + (Hash(f * 4.1 + i) - .5) * 2 * quake;
            Add(kGlyphs[i].art, x, y, 1, punch, 1, 0, Clamp01((t_ - st) / .12), 9);
        }
        const double sub = Smooth((t_ - c_.burst - .53) / .35);
        if (sub > 0) {
            Add(kSub.art, kCenterX + (kSub.cx - kCenterX) * punch, kCenterY + (kSub.cy - kCenterY) * punch + 18 * (1 - sub), 1, punch, 1, 0, sub, 7);
        }
    }

    void AddSatellite(const Satellite& s, size_t i, double t, double alpha) {
        const double d0 = c_.burst + i * .02;
        if (t < d0) return;
        const double fx = s.x + s.art->w * .5, fy = s.y + s.art->h * .5;
        const double p = OutBack((t - d0) / .34), drift = (std::max)(0.0, t - (c_.burst + .38));
        // 噴出が激しくなるほど外へ押し出され、画の外へはみ出していく。
        const double dx = fx - kCenterX, dy = fy - kCenterY, dl = (std::max)(1.0, std::hypot(dx, dy));
        const double push = 190 * std::pow(Smooth((t - c_.spray) / (std::max)(.1, c_.sprayEnd - c_.spray)), 2);
        double x = fx + s.vx * drift + dx / dl * push, y = fy + s.vy * drift + dy / dl * push;
        x = kCenterX + (x - kCenterX) * p; y = kCenterY + (y - kCenterY) * p;
        double rot = s.ro * 2 * (1 - p), size = .15 + .85 * p, a = Clamp01((t - d0) * 8), squash = 1;
        if (s.sweep) rot += 5 * Smooth((t - c_.burst - .88) / 2.5);
        const double flip = c_.in + s.flip;
        if (s.flip >= 0 && t > flip && t < flip + .36) squash = std::abs(std::cos(kPi * (t - flip) / .36));
        const double d1 = c_.rewind + (kSatellites.size() - i) * .018;
        if (t > d1) {
            const double q = InCubic((t - d1) / (c_.rewindSeconds * .85));
            x += (kCenterX - x) * q; y += (kCenterY - y) * q;
            rot += s.ro * .6 * q; size *= 1 - .8 * q; a *= 1 - q * q;
        }
        Add(s.art, x, y, s.depth, size, squash, rot, a * alpha, 0);
    }

    // i 番目の噴き出した碎片。放たれた向きへ飛び、遠いほど速く大きくなる（镜头へ向かって来るように）。巻き戻しでは年齢を
    // 巻き戻して喷口へ戻る。标题（外へ一回り広げた矩形）の中では透けていて、飛び出してから見える。
    void AddSpray(int i, double born, double end, double t, double alpha) {
        double age = (std::min)(t, end) - born;
        if (age < 0) return;
        if (t > c_.rewind) {
            const double q = InCubic((t - c_.rewind - Hash(i * 3.1) * .06) / (c_.rewindSeconds * .74));
            if (q >= 1) return;
            age *= 1 - q;
        }
        const double r1 = Hash(i * 1.37), r2 = Hash(i * 2.71), r3 = Hash(i * 5.03), r4 = Hash(i * 7.77);
        const bool giant = i % 31 == 13;
        const Art* art = giant ? &kStrokeGiant : kPool[static_cast<size_t>(Hash(i * 9.13) * kPool.size()) % kPool.size()];
        const double ox = kLogoX + kLogoW * (.08 + .84 * r2), oy = kLogoY + kLogoH * (.2 + .6 * r3);
        const double angle = r1 * 2 * kPi, speed = (300 + 1000 * r4 * r4) * look_.spraySpeed;
        const double dist = speed * age * (1 + age * 1.2);
        const double x = ox + std::cos(angle) * dist, y = oy + std::sin(angle) * dist * .82;
        const double size = (std::min)(giant ? 3.0 : 2.4, (giant ? .9 : .16 + .26 * r2) * look_.spraySize * std::pow(1 + age * 1.9, 1.35));
        const double rot = r1 * 360 + (r3 - .5) * 900 * age;
        const double ex = (std::max)({kLogoX - 40 - x, 0.0, x - (kLogoX + kLogoW + 40)});
        const double ey = (std::max)({kLogoY - 30 - y, 0.0, y - (kLogoBottom + 30)});
        Add(art, x, y, 1, size, 1, rot, Clamp01(std::hypot(ex, ey) / 110) * alpha, 0);
    }

    void Put(std::string_view texture, double cx, double cy, double w, double h, double rot, HIKARI::GameProjectUiColor tint) {
        if (w <= 0 || h <= 0 || tint.a <= 0) return;
        HIKARI::GameProjectUiSpriteRequest request{};
        request.sourceId = next_++; request.order = order_++;
        request.referenceWidth = static_cast<float>(width_); request.referenceHeight = static_cast<float>(height_);
        request.x = static_cast<float>(cx - w * .5); request.y = static_cast<float>(cy - h * .5);
        request.width = static_cast<float>(w); request.height = static_cast<float>(h);
        request.pivotX = request.pivotY = .5f; request.rotationRadians = static_cast<float>(rot);
        request.tint = tint;
        (void)ui_.SubmitSprite(texture, request);
    }

    HIKARI::UiView ui_;
    const TitleCardTimes& c_;
    const TitleCardLook& look_;
    double t_ = 0, width_ = 1920, height_ = 1080, scale_ = 1, offsetX_ = 0, offsetY_ = 0;
    double zoom_ = 1, turn_ = 0, shakeX_ = 0, shakeY_ = 0, hit_ = 0, bandTop_ = 0, bandBottom_ = 0, bandShift_ = 0;
    int glitch_ = -1;
    bool negative_ = false;
    std::vector<Sprite> sprites_;
    // 黒帯（100000）とまぶた（100001）の下。一コマに数千枚まで重ねられるよう低めから始める。
    int order_ = 95000;
    uint64_t next_ = 0x7D600000ull;
};
}

double TitleCardBarsAway(const TitleCardTimes& c, double t) {
    if (!(c.seconds > 0)) return 0;
    return Smooth((t - (c.in + .2)) / .18) * (1 - Smooth((t - (c.in + c.seconds - .5)) / .16));
}

void DrawTitleCard(HIKARI::WorldView& world, const TitleCardTimes& times, const TitleCardLook& look, double t) {
    if (!(times.seconds > 0) || t < times.in || t >= times.in + times.seconds) return;
    Frame(world, times, look, t).Draw();
}

const std::vector<std::string_view>& TitleCardTextures() {
    static const std::vector<std::string_view> textures = [] {
        std::vector<std::string_view> out{kPaper};
        for (const Art* art : {&kBi, &kJi, &kYo, &kN, &kVision, &kVisionLarge, &kVisionSmall, &kVisionSmallAlt, &kVisionTurned, &kVisionUpsideDown,
                               &kKanaMirrorHigh, &kKanaMirrorLow, &kKanaCorner, &kKanaTurned, &kStrokeCorner, &kStrokeGiant}) {
            out.push_back(art->black); out.push_back(art->white);
        }
        return out;
    }();
    return textures;
}
}
