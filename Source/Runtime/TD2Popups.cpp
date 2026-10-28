#include "TD2Popups.h"
#include "../Common/TD2Math.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <string>

namespace TD2::Popups {
namespace {
using Ui::Rect;
// 窓の縮尺：窓の幅に対する素材の倍率（題の帯の高さが 86·s）。字の大きさと行の間も窓の幅から決める。
constexpr float kScalePerWidth = 1.f / 1100.f, kTextPerWidth = .052f, kLineGap = 1.38f;
// 嵐の増え方の曲がり具合（大きいほど終わりに詰まって増える）と、描く数の上限（古い物は下に隠れるので描かない）。
constexpr double kStormCurve = 3.4;
constexpr int kStormDrawLimit = 110;
constexpr std::array<std::string_view, 6> kStormTitles{"メッセージ", "？？？", "！！", "おしらせ", "メッセージ", "エラー"};
constexpr std::array<std::string_view, 3> kStormIcons{kIconWarning, kIconQuestion, kIconMail};

int Lines(std::string_view text) { return 1 + static_cast<int>(std::count(text.begin(), text.end(), '\n')); }
using Math::Hash01;
}

float Appear(double u) {
    if (u <= 0) return 0;
    if (u >= .16) return 1;
    // 0.09 秒で 1.08 まで伸び、残りで 1 へ戻る。
    if (u < .09) { const double k = u / .09; return static_cast<float>(1.08 * (1 - (1 - k) * (1 - k))); }
    return static_cast<float>(1.08 - .08 * (u - .09) / .07);
}

float Height(float width, std::string_view text, bool buttons, float textScale) {
    const float s = width * kScalePerWidth, f = width * kTextPerWidth * textScale, pad = f * .7f;
    const float body = (std::max)(f * 2.2f, Lines(text) * f * kLineGap);
    return Ui::kWindowTop * s + pad + body + pad + (buttons ? f * 2.1f + pad : 0) + Ui::kWindowBottom * s;
}

Rect Draw(Ui::Painter& paint, float cx, float cy, float width, const Look& look, bool buttons) {
    const float s = width * kScalePerWidth, f = width * kTextPerWidth * look.textScale, pad = f * .7f;
    const float full = Height(width, look.text, buttons, look.textScale);
    // 中身（題・絵・字・ボタンの段）は開ききった窓の位置に置く。開く枠に合わせて動かすと、行き過ぎて戻る間に字が上下に
    // 跳ねて見える。
    const Rect settled{cx - width * .5f, cy - full * .5f, width, full};
    const Rect inner = Ui::WindowInner(settled, s);
    const float bh = f * 2.1f;
    const Rect buttonRow{inner.x + pad, inner.Bottom() - pad - bh, inner.w - 2 * pad, bh};
    if (look.appear <= 0 || look.alpha <= 0) return buttonRow;
    // 出る時はブラウン管の窓のように縦に伸びて開く（幅はそのまま）。動くのは枠だけで、中身は枠がほぼ開いてから出す。
    const float h = (std::max)(full * look.appear, Ui::kWindowTop * s + Ui::kWindowBottom * s);
    const Rect window{cx - width * .5f, cy - h * .5f, width, h};
    paint.SetAlpha(look.alpha);
    const Rect frame = paint.Window(window, s, {});
    if (look.appear < .97f) return buttonRow;
    paint.WindowTitle(settled, s, look.title);
    if (look.dark) paint.Fill(frame, {.08f, .09f, .1f, 1});
    const Ui::Color ink = look.dark ? Ui::Color{.92f, .93f, .9f, 1} : Ui::kInk;
    float x = inner.x + pad;
    const float iconSize = f * 2.2f;
    if (!look.icon.empty()) {
        paint.Image(look.icon, {x, inner.y + pad, iconSize, iconSize});
        x += iconSize + pad;
    }
    // 行ごとに描く（一行の字は折り返さない）。typed が負でなければ、その数だけ打ち出した姿。
    std::string_view rest = look.text;
    size_t budget = look.typed < 0 ? std::string_view::npos : static_cast<size_t>(look.typed);
    const float lines = static_cast<float>(Lines(look.text));
    const float body = (std::max)(iconSize, lines * f * kLineGap);
    float y = inner.y + pad + (body - lines * f * kLineGap) * .5f;
    // 一番長い行が窓に収まるよう字を小さくする（字の幅はおおよそ字の高さ。空白は半分）。
    float longest = 0;
    for (std::string_view scan = look.text;;) {
        const auto cut = scan.find('\n');
        const std::string_view line = scan.substr(0, cut);
        const auto spaces = static_cast<float>(std::count(line.begin(), line.end(), ' '));
        longest = (std::max)(longest, static_cast<float>(Ui::CharCount(line)) - spaces * .5f);
        if (cut == std::string_view::npos) break;
        scan = scan.substr(cut + 1);
    }
    const float textSize = longest > 0 ? (std::min)(f, (inner.Right() - pad - x) / (longest * 1.02f)) : f;
    while (true) {
        const auto cut = rest.find('\n');
        const std::string_view line = rest.substr(0, cut);
        const size_t n = Ui::CharCount(line);
        const std::string_view shown = budget == std::string_view::npos ? line : Ui::CharHead(line, (std::min)(budget, n));
        paint.Text(shown, {x, y, inner.Right() - pad - x, f * kLineGap}, textSize, ink);
        if (budget != std::string_view::npos) budget = budget > n ? budget - n : 0;
        y += f * kLineGap;
        if (cut == std::string_view::npos) break;
        rest = rest.substr(cut + 1);
    }
    return buttonRow;
}

void Buttons(Ui::Painter& paint, Rect area, std::span<const std::string_view> labels, int choice, bool pressed, int hover,
             std::span<Rect> rects) {
    const float bh = area.h, bw = bh * Ui::kButtonAspect, gap = bw * .2f;
    const int count = static_cast<int>(labels.size());
    float x = area.Right() - count * bw - (count - 1) * gap;
    for (int i = 0; i < count; ++i) {
        const auto state = i == choice ? (pressed ? Ui::State::Pressed : Ui::State::Selected) : i == hover ? Ui::State::Selected : Ui::State::Normal;
        const Rect r{x, area.y, bw, bh};
        paint.Button(r, labels[static_cast<size_t>(i)], state, bh * .46f);
        if (static_cast<size_t>(i) < rects.size()) rects[static_cast<size_t>(i)] = r;
        x += bw + gap;
    }
}

int Count(const Storm& storm, double u) {
    if (u <= 0 || storm.peak <= 0) return 0;
    const double x = (std::min)(1.0, u / (std::max)(.01, storm.seconds));
    return (std::min)(storm.peak, static_cast<int>(std::floor(storm.peak * (std::exp(kStormCurve * x) - 1) / (std::exp(kStormCurve) - 1))) + 1);
}

void DrawStorm(Ui::Painter& paint, Rect area, const Storm& storm, double u, float tear) {
    const int count = Count(storm, u);
    if (count <= 0 || storm.texts.empty()) return;
    // i 番目が出た時刻（増え方の逆）。
    const auto bornAt = [&](int i) {
        const double k = (std::exp(kStormCurve) - 1) * double(i) / storm.peak;
        return std::log(1 + k) / kStormCurve * storm.seconds;
    };
    // 横に裂ける揺れ：24 分の 1 秒ごとに時々、全部が横へずれる。
    const auto tick = static_cast<uint32_t>(std::floor(u * 24));
    const float shift = tear > 0 && Hash01(storm.seed ^ 0xA5u, tick) < .18 * tear ? static_cast<float>((Hash01(storm.seed, tick) - .5) * area.w * .07 * tear) : 0.f;
    for (int i = (std::max)(0, count - kStormDrawLimit); i < count; ++i) {
        const auto key = static_cast<uint32_t>(i) * 7u + storm.seed * 131u;
        const float w = area.w * static_cast<float>(.17 + .19 * Hash01(key, 1)) * storm.size;
        // 画の端から少しはみ出してもよい（全面が埋まって見える）。
        const float cx = area.x + area.w * static_cast<float>(-.04 + 1.08 * Hash01(key, 2)) + shift;
        const float cy = area.y + area.h * static_cast<float>(-.04 + 1.08 * Hash01(key, 3));
        Look look;
        look.title = kStormTitles[static_cast<size_t>(Hash01(key, 4) * kStormTitles.size()) % kStormTitles.size()];
        look.text = storm.texts[static_cast<size_t>(Hash01(key, 5) * storm.texts.size()) % storm.texts.size()];
        look.icon = Hash01(key, 6) < .7 ? kStormIcons[static_cast<size_t>(Hash01(key, 7) * kStormIcons.size()) % kStormIcons.size()] : std::string_view{};
        look.dark = Hash01(key, 8) < .1;
        look.appear = Appear(u - bornAt(i));
        // 出たばかりの物は時々その場で小刻みに震える。
        const float shake = u - bornAt(i) < .4 && Hash01(key, tick) < .3 ? static_cast<float>((Hash01(key, tick + 1) - .5) * w * .04) : 0.f;
        (void)Draw(paint, cx + shake, cy, w, look);
    }
    paint.SetAlpha(1);
}
}
