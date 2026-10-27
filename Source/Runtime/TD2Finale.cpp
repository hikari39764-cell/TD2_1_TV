#include "TD2Finale.h"
#include "TD2Popups.h"
#include "../Common/TD2Math.h"
#include "HIKARI/Project/ActionViews.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace TD2 {
namespace {
using Ui::Rect;
// 通関の秒：ゴールのテレビの画面に弾窓が溢れ始める・画全体へはみ出す・問いかけが始まる。
constexpr double kScreenStormAt = 2.2, kScreenStormSeconds = 2.4, kBreakoutAt = 4.6, kFullStormSeconds = 1.9, kLinesAt = 6.7;
constexpr int kScreenStormPeak = 70, kFullStormPeak = 130, kLaughPeak = 150;
constexpr double kLaughSeconds = 1.8, kLaughLinesDelay = 1.5;
// 問いかけの字を打ち出す速さ（1 秒の字の数）、ボタンが出るまでの余白、次へ進むのを受け付けるまで。
constexpr double kCharsPerSecond = 26, kButtonsAfter = .12, kSkipAfter = .5;
// 暗転：黒くなるまでの秒と、黒のまま置いてから次へ移るまでの秒。
constexpr double kBlackFade = .5, kBlackHold = 1.2;
// 画の上の重なりの順（オープニングの弾窓 140000 と同じ高さ。メニューの窓 120000〜136000 より上）。
constexpr int kStormOrder = 141000, kLineOrder = 150000, kBlackOrder = 160000, kScreenOrder = 90000;
// 画に残す問いかけの数（古いものから消す）。
constexpr size_t kPileLimit = 6;

constexpr std::array<std::string_view, 5> kClearTexts{"クリア おめでとう！", "おめでとう！", "おめでとう おめでとう", "やったね！", "クリア クリア クリア"};
constexpr std::array<std::string_view, 4> kLaughTexts{"アハハハハハハ", "アハハハ", "ハハハハハハハハハ", "アハハハハハハハハハハハ"};
constexpr std::array<std::string_view, 4> kTrueTexts{"ほんとうに クリア しちゃうなんて", "すごい！", "ほんとうに？", "おめでとう！"};

}

// 普通の通関の問いかけ。
static const std::array<FinaleDirector::Line, 7> kEndingLines{{
    {"クリア おめでとう！", {"はい"}, 1, 0, Popups::kIconQuestion},
    {"でも まだ かえっちゃ だめだよ", {"？"}, 1, 0, Popups::kIconQuestion},
    {"だって きみは", {}, 0, 1.3},
    {"ずっと ここで\nわたしと あそぶんだから", {"はい", "いいえ"}, 2, 0, Popups::kIconQuestion, false, FinaleDirector::Next::Laugh, FinaleDirector::Next::Line},
    {"ほんとうに もう\nわたしと あそんで くれないの？", {"はい", "いいえ"}, 2, 0, Popups::kIconQuestion, false, FinaleDirector::Next::Laugh, FinaleDirector::Next::Line},
    {"もう しょうがないなあ", {}, 0, 1.9, {}, false, FinaleDirector::Next::Line, FinaleDirector::Next::Line, true},
    {"こんどは マウスを ロックしたよ。\nおとなしく「はい」を えらんでね", {"はい", "いいえ"}, 2, 0, Popups::kIconWarning, true, FinaleDirector::Next::Laugh, FinaleDirector::Next::Hidden},
}};
// はい を選んだ後（眠らされてタイトルへ）。
static const std::array<FinaleDirector::Line, 3> kLaughLines{{
    {"ゆっくり やすんでね", {}, 0, 1.9}, {"めが さめたら", {}, 0, 1.5}, {"また いっしょに あそぼうね", {}, 0, 2.2},
}};
// 本当のマウスで いいえ を押した後（隠しの道へ）。
static const std::array<FinaleDirector::Line, 6> kHiddenLines{{
    {"まさか そこまで ほんきだなんて", {}, 0, 2.2, {}, false, FinaleDirector::Next::Line, FinaleDirector::Next::Line, true},
    {"こんどこそ ほんとうに おてあげだよ", {}, 0, 2.2},
    {"つぎの ステージ\nもうすこしだけ あそんで くれる？", {}, 0, 2.5},
    {"これが わたしの さいごの おねがい", {}, 0, 2.3},
    {"……ごめんね。\nこわがらせちゃった かな", {}, 0, 2.5},
    {"でも やっぱり\nたのしんで ほしいな", {}, 0, 2.7},
}};
// 隠しの道を通した通関（眠らされ、目覚めるとパソコンだけが残る）。
static const std::array<FinaleDirector::Line, 7> kTrueLines{{
    {"ほんとうに クリア しちゃうなんて", {}, 0, 2.3}, {"すっごく うれしい", {}, 0, 2.0},
    {"ここまで あそんでくれるなんて\nおもわなかった", {}, 0, 2.6}, {"イライラ させちゃった\nかもしれないけど", {}, 0, 2.5},
    {"ほんとうに ありがとう", {}, 0, 2.2}, {"ゆめを みてたと おもってね", {}, 0, 2.4}, {"ゆっくり やすんでね。\nおつかれさま", {}, 0, 2.8},
}};

void FinaleDirector::Begin(Kind kind, HIKARI::GameProjectObjectHandle goalScreen) {
    *this = {};
    active_ = true; kind_ = kind; goal_ = goalScreen;
    if (kind == Kind::Ending) { lines_ = kEndingLines.data(); lineCount_ = kEndingLines.size(); }
    else { lines_ = kTrueLines.data(); lineCount_ = kTrueLines.size(); pending_ = Outcome::WakeTrue; }
    lineStart_ = kLinesAt;
    std::fprintf(stderr, "[TD2 finale] begin kind=%s\n", kind == Kind::Ending ? "ending" : "true");
}

void FinaleDirector::StartLines(const Line* lines, size_t count, double t) {
    lines_ = lines; lineCount_ = count; current_ = 0; lineStart_ = t; choice_ = 0; cursor_ = 0;
}

void FinaleDirector::Answer(int choice, double t) {
    const Line& line = lines_[current_];
    const Next next = choice == 0 ? line.yes : line.no;
    std::fprintf(stderr, "[TD2 finale] answer line=%zu choice=%d\n", current_, choice);
    if (!shown_.empty()) shown_.back().answered = choice;
    if (next == Next::Laugh) {
        laughStart_ = t; pending_ = Outcome::WakeNormal;
        StartLines(kLaughLines.data(), kLaughLines.size(), t + kLaughLinesDelay);
        return;
    }
    if (next == Next::Hidden) {
        pending_ = Outcome::EnterHidden;
        StartLines(kHiddenLines.data(), kHiddenLines.size(), t + .4);
        return;
    }
    ++current_; lineStart_ = t; choice_ = 0; cursor_ = 0;
}

FinaleDirector::Outcome FinaleDirector::Update(HIKARI::WorldView& world, MenuDirector& menu, double t, double seconds, bool& blackOut) {
    blackOut = false;
    if (!active_) return Outcome::None;
    const double step = (std::min)(seconds, 1.0 / 30);
    // 暗転：黒くし終えて少し置いたら、終わり方を返す。
    if (blackStart_ >= 0 && t - blackStart_ >= kBlackFade + kBlackHold) {
        active_ = false;
        std::fprintf(stderr, "[TD2 finale] outcome=%d\n", static_cast<int>(pending_));
        return pending_;
    }
    // 問いかけ：出る時刻になったら山に加える。
    if (blackStart_ < 0 && current_ < lineCount_ && t >= lineStart_ && (shown_.empty() || shown_.back().line != &lines_[current_])) {
        Shown s;
        s.line = &lines_[current_]; s.born = t;
        // 最初は真ん中、後のものは少しずつずれて重なる。
        const auto key = static_cast<uint32_t>(shown_.size() * 13 + current_ * 7 + (lines_ == kEndingLines.data() ? 0 : 101));
        s.cx = shown_.empty() ? .5f : static_cast<float>(.5 + (Math::Hash01(key, 1) - .5) * .14);
        s.cy = shown_.empty() ? .5f : static_cast<float>(.5 + (Math::Hash01(key, 2) - .5) * .16);
        shown_.push_back(s);
        if (shown_.size() > kPileLimit) shown_.erase(shown_.begin());
        ++sounds_;
    }
    const bool hasLine = blackStart_ < 0 && current_ < lineCount_ && !shown_.empty() && shown_.back().line == &lines_[current_] && t >= lineStart_;
    if (hasLine) {
        const Line& line = lines_[current_];
        Shown& top = shown_.back();
        const double age = t - top.born;
        const bool typing = (age - .1) * kCharsPerSecond < static_cast<double>(Ui::CharCount(line.text)) + kButtonsAfter * kCharsPerSecond;
        if (line.buttonCount == 0) {
            // 語りかけ：hold 秒で次へ（スペースで早める）。
            if (age >= line.hold || (age >= kSkipAfter && menu.ConfirmPressed())) {
                if (menu.ConfirmPressed()) menu.HoldAction();
                ++current_; lineStart_ = t; choice_ = 0; cursor_ = 0;
            }
        } else if (typing) {
            // 打っている途中でスペース：打ち終えた姿にする（その押下では選ばない）。
            if (menu.ConfirmPressed()) { top.born = t - 10; menu.HoldAction(); }
        } else {
            if (line.locked) {
                // 窓の矢印は はい から動かない（押すと震える）。本当のマウスはこの間だけ見える。
                if (menu.LeftPressed() || menu.RightPressed()) lockShake_ = .35f;
                choice_ = 0;
                menu.RequestPointer();
                // 開発用（環境変数 TD2_FINALE_FAKE_MOUSE）：無頭の検証では本当のマウスが無いので、1 秒後に いいえ を押したことにする。
                static const bool fakeMouse = std::getenv("TD2_FINALE_FAKE_MOUSE") != nullptr;
                if (fakeMouse && age > 1.0) { Answer(1, t); return Outcome::None; }
                HIKARI::GameProjectPointerState pointer{};
                const bool down = world.GetInput().GetPointerState(pointer) && pointer.valid && pointer.primaryDown;
                if (down && !pointerDown_) {
                    Ui::Painter probe(world, kLineOrder);
                    const float px = pointer.normalizedX * probe.Width(), py = pointer.normalizedY * probe.Height();
                    for (int i = 0; i < line.buttonCount; ++i) {
                        const Rect& r = buttonRects_[static_cast<size_t>(i)];
                        if (px >= r.x && px <= r.Right() && py >= r.y && py <= r.Bottom()) { pointerDown_ = down; Answer(i, t); return Outcome::None; }
                    }
                }
                pointerDown_ = down;
            } else if (line.buttonCount > 1) {
                if (menu.LeftPressed()) choice_ = 0;
                if (menu.RightPressed()) choice_ = 1;
            }
            if (menu.ConfirmPressed()) { menu.HoldAction(); Answer(choice_, t); }
        }
    }
    // 問いかけを全部終えた：暗転へ（普通の通関の問いかけは、答えのどれかで別の流れへ移っている）。
    if (blackStart_ < 0 && current_ >= lineCount_ && pending_ != Outcome::None) { blackStart_ = t; blackOut = true; }
    lockShake_ = (std::max)(0.f, lockShake_ - static_cast<float>(step));
    Draw(world, menu, t, step);
    return Outcome::None;
}

void FinaleDirector::Draw(HIKARI::WorldView& world, MenuDirector& menu, double t, double seconds) {
    const double before = t - seconds;
    const std::span<const std::string_view> clearTexts = kind_ == Kind::Ending ? std::span<const std::string_view>(kClearTexts) : std::span<const std::string_view>(kTrueTexts);
    // ゴールのテレビの画面：消えた後、黒い画面に弾窓が溢れる（溢れきった後もそのまま）。
    if (t >= kScreenStormAt) {
        const Popups::Storm storm{clearTexts, 0x31u, kScreenStormSeconds, kScreenStormPeak, 1.25f};
        const double u = (std::min)(t - kScreenStormAt, kScreenStormSeconds);
        sounds_ += Popups::Count(storm, u) - Popups::Count(storm, (std::max)(0.0, (std::min)(before - kScreenStormAt, kScreenStormSeconds)));
        const float tear = t > kBreakoutAt - .35 && t < kBreakoutAt ? 1.f : .25f;
        (void)menu.PaintOnScreen(world, world.FindObject(goal_), kScreenOrder,
                                 [&](Ui::Painter& paint, Rect region) { Popups::DrawStorm(paint, region, storm, u, tear); });
    }
    Ui::Painter storms(world, kStormOrder);
    const float W = storms.Width(), H = storms.Height();
    // 画全体へはみ出す：破れる瞬間に白く光り、弾窓が画を埋め尽くす（埋めきった後も時々裂ける）。
    if (t >= kBreakoutAt) {
        const Popups::Storm storm{clearTexts, 0x77u, kFullStormSeconds, kFullStormPeak, 1.f};
        const double u = (std::min)(t - kBreakoutAt, kFullStormSeconds);
        sounds_ += Popups::Count(storm, u) - Popups::Count(storm, (std::max)(0.0, (std::min)(before - kBreakoutAt, kFullStormSeconds)));
        Popups::DrawStorm(storms, {0, 0, W, H}, storm, u, t - kBreakoutAt < kFullStormSeconds ? 1.f : .35f);
        if (t - kBreakoutAt < .07) { storms.SetAlpha(1); storms.Fill({0, 0, W, H}, {1, 1, 1, .85f}); }
    }
    if (laughStart_ >= 0) {
        const Popups::Storm storm{kLaughTexts, 0x99u, kLaughSeconds, kLaughPeak, 1.1f};
        const double u = (std::min)(t - laughStart_, kLaughSeconds);
        sounds_ += Popups::Count(storm, u) - Popups::Count(storm, (std::max)(0.0, (std::min)(before - laughStart_, kLaughSeconds)));
        Popups::DrawStorm(storms, {0, 0, W, H}, storm, u, 1.f);
    }
    // 問いかけの山：古いものから描き、一番上（今の問い）だけ打ち出しとボタンとカーソル。
    Ui::Painter lines(world, kLineOrder);
    const float width = (std::min)(W * .4f, H * .72f);
    // 主と従：問いかけが出ている間は嵐を暗く沈め、今の問いだけをはっきり見せる（前の問いは薄く後ろへ退く）。
    if (!shown_.empty() && blackStart_ < 0) {
        const double k = std::clamp((t - shown_.front().born) / .3, 0.0, 1.0);
        lines.Fill({0, 0, W, H}, {0, 0, 0, static_cast<float>(.55 * k)});
    }
    const bool hasLine = blackStart_ < 0 && current_ < lineCount_ && !shown_.empty() && shown_.back().line == &lines_[current_] && t >= lineStart_;
    // 震える問い（「もう しょうがないなあ」など）と、隠しの道の語りかけの間は山ごと揺れる。
    const auto tick = static_cast<uint32_t>(std::floor(t * 24));
    const bool shaking = hasLine && lines_[current_].shake;
    const float shake = shaking ? static_cast<float>((Math::Hash01(tick, 3) - .5) * W * .012) : 0.f;
    for (size_t i = 0; i < shown_.size(); ++i) {
        const Shown& s = shown_[i];
        const bool top = hasLine && i + 1 == shown_.size();
        const Line& line = *s.line;
        Popups::Look look;
        look.title = "？？？"; look.text = line.text; look.icon = line.icon;
        look.appear = Popups::Appear(t - s.born);
        look.typed = top ? (t - s.born - .1) * kCharsPerSecond : -1;
        const bool buttons = line.buttonCount > 0;
        const float cx = W * s.cx + shake, cy = H * s.cy;
        const float w = top ? width : width * .82f;
        if (!top) { look.alpha = .42f; look.typed = -1; }
        else {
            // 今の問いの下に暗い縁を敷き、周りの窓から切り離す。
            const float h = Popups::Height(w, line.text, buttons) * look.appear, pad = w * .035f;
            lines.SetAlpha(1);
            lines.Fill({cx - w * .5f - pad, cy - h * .5f - pad, w + 2 * pad, h + 2 * pad}, {0, 0, 0, .5f});
        }
        const Rect area = Popups::Draw(lines, cx, cy, w, look, buttons);
        if (!buttons || look.appear < .99f) continue;
        const bool ready = !top || (t - s.born - .1) * kCharsPerSecond >= static_cast<double>(Ui::CharCount(line.text)) + kButtonsAfter * kCharsPerSecond;
        if (!ready) continue;
        std::span<const std::string_view> labels(line.buttons.data(), static_cast<size_t>(line.buttonCount));
        int hover = -1;
        if (top && line.locked) {
            // 本当のマウスが指しているボタンを少し光らせる（押せると気付ける）。
            HIKARI::GameProjectPointerState pointer{};
            if (world.GetInput().GetPointerState(pointer) && pointer.valid) {
                const float px = pointer.normalizedX * W, py = pointer.normalizedY * H;
                for (int b = 0; b < line.buttonCount; ++b) {
                    const Rect& r = buttonRects_[static_cast<size_t>(b)];
                    if (b != 0 && px >= r.x && px <= r.Right() && py >= r.y && py <= r.Bottom()) hover = b;
                }
            }
        }
        std::array<Rect, 2> rects{};
        Popups::Buttons(lines, area, labels, top ? choice_ : s.answered, !top, hover, rects);
        if (!top) continue;
        buttonRects_ = rects;
        // 窓の矢印：選んだボタンへ滑って寄る。ロックされている時は はい の上で震える。
        cursor_ = cursor_ + (static_cast<float>(choice_) - cursor_) * static_cast<float>(1 - std::exp(-seconds * 18));
        const float bw = rects[0].w;
        const float x1 = line.buttonCount > 1 ? rects[1].x : rects[0].x;
        float cxCursor = rects[0].x + (x1 - rects[0].x) * cursor_ + bw * .64f;
        if (lockShake_ > 0) cxCursor += static_cast<float>((Math::Hash01(tick, 9) - .5) * bw * .12 * lockShake_ / .35f);
        lines.SetAlpha(1);
        lines.Cursor(cxCursor, rects[0].y + rects[0].h * .58f, rects[0].h * .78f);
    }
    // 暗転：全部の上から黒くする（ゴールのテレビの画面の弾窓も見えなくなる）。
    if (blackStart_ >= 0) {
        Ui::Painter black(world, kBlackOrder);
        const double k = std::clamp((t - blackStart_) / kBlackFade, 0.0, 1.0);
        black.Fill({0, 0, black.Width(), black.Height()}, {0, 0, 0, static_cast<float>(Math::SmoothStep(k))});
    }
}
}
