#include "TD2Menus.h"
#include "../Common/TD2Math.h"
#include "HIKARI/Project/ActionViews.h"
#include "HIKARI/Project/AudioView.h"
#include "HIKARI/Project/CameraViewSnapshot.h"
#include "HIKARI/Project/DisplaySettingsView.h"
#include "HIKARI/Project/GameFlowViews.h"
#include "HIKARI/Project/SceneCaptureView.h"
#include "TD2Popups.h"
#include "TD2RegionComponent.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>

namespace TD2 {
namespace {
using Ui::Rect;
// 画の上の重なりの順：続きの問い < 画布が使えない時の窓 < やめる時の暗転。オープニングのタイトル（99000）より上。
// 画布の中では描いた順に重なる（ESC の窓の上に保存の問い）。
constexpr int kTitleOrder = 120000, kContinueOrder = 124000, kPauseOrder = 128000, kFadeOrder = 136000;
constexpr std::string_view kIconQuestion = "UI/TD2/UI_Icon_Question.png";
constexpr std::string_view kIconWarning = "UI/TD2/UI_Icon_Warning.png";
constexpr std::string_view kIconMail = "UI/TD2/UI_Icon_MailNew.png";
constexpr std::string_view kIconMailOpen = "UI/TD2/UI_Icon_MailOpen.png";
constexpr std::string_view kMailFields = "UI/TD2/UI_MailFields.png";
constexpr std::string_view kMailBody = "ねえ、わたしと いっしょに あそばない？";
// 隠しの道を通した後のメール（開発者から）。本文は二行。
constexpr std::string_view kDevSubject = "件名：ありがとう", kDevSender = "差出人：開発者";
constexpr std::string_view kDevBody = "私たちのゲームを 遊んでくれて、\nありがとうございました。";
constexpr std::string_view kDevRestart = "最初から";
constexpr std::string_view kDevConfirm = "セーブデータを消して、最初から始めます。\nよろしいですか？";
// 弾窓の演出で出す字（起動の直後に一度描いて、初めて出す時に字の読み込みで止まらないようにする）。
constexpr std::string_view kPopupGlyphs =
    "ちなみにタイトルは：ビジョン　ですたのしんでねゴールでまってるねクリアおめでとうでもまだかえっちゃだめだよだってきみは"
    "ずっとここでわたしとあそぶんだからほんとうにもうあそんでくれないのしょうがないなあこんどはマウスをロックしたよおとなしく"
    "「はい」をえらんでねアハハゆっくりやすんでめがさめたらまたいっしょにあそぼうまさかそこまでほんきだなんてこんどこそおてあげ"
    "つぎのステージもうすこしだけくれるこれがさいごのおねがい……ごめんこわがらせちゃったかなやっぱりほしいなしちゃうすっごくうれしい"
    "ここまでおもわなかったイライラさせかもしれないけどありがとうゆめをみてたとおもってつかれさまおしらせエラー！？いいえ";
// メールの出方（タイトル待ちの経過秒）：窓が開き始める・本文を打ち始める・一秒に打つ字の数。
constexpr double kMailOpen = .6, kMailType = .9, kMailCharsPerSecond = 16;
// テレビの画面に貼る画布の大きさ（正方形。画面の比に合わせてその左上を使う）と、画面の材質スロット名
// （TD2Screen3D の kScreenSlot と同じ。どの型のテレビも同じ名前）。
constexpr float kCanvasSize = 1024;
// 画布へ描く色に掛ける明るさ（画面の材質が光らせた後に、素材の色に近く見える所）。
constexpr float kCanvasGain = .6f;
// 画布の字の描き縁（字の高さに対する割合）。
constexpr float kCanvasOutline = .09f;
// 窓の動きを一フレームで進める上限（長いフレームで窓やカーソルが飛ばない）。
constexpr double kUiStep = 1.0 / 30;
// 見えない薄さ。二回だけ提出し、その描画が提示された後に本来の画へ渡す。
constexpr float kWarmAlpha = .0002f;
constexpr uint32_t kWarmFrames = 2;
constexpr std::string_view kScreenSlotName = "TD2_Screen";
// パソコンの机の上の色（落ち着いた青緑。画面の外の暗い工場の中で少し目立つ程度）。
constexpr Ui::Color kDesktop{.17f, .29f, .29f, 1};
constexpr std::array<std::string_view, 3> kWindowModes{"ウィンドウ", "ボーダーレス", "フルスクリーン"};
constexpr std::array<std::string_view, 4> kResolutions{"1280×720", "1920×1080", "2560×1440", "3840×2160"};
constexpr std::array<HIKARI::GameProjectDisplayResolution, 4> kResolutionValues{
    HIKARI::GameProjectDisplayResolution::P720, HIKARI::GameProjectDisplayResolution::P1080,
    HIKARI::GameProjectDisplayResolution::P1440, HIKARI::GameProjectDisplayResolution::P2160};
// ESC の窓の行。
enum Row { kResume, kBgm, kSe, kAmbient, kWindowMode, kResolution, kVsync, kQuit, kRowCount };
constexpr std::array<std::string_view, kRowCount> kRowLabels{"つづける", "BGM", "効果音", "環境音", "画面モード", "解像度", "垂直同期", "ゲームをやめる"};

// 目標へ秒あたり rate の割合で寄せる。
float Follow(float value, float goal, double seconds, double rate = 18) { return value + (goal - value) * static_cast<float>(1 - std::exp(-seconds * rate)); }
// 画面の縁へ向かって少しずつ暗くし、ブラウン管の奥に映っているように見せる（帯を重ねる）。
void Shade(Ui::Painter& paint, Rect s) {
    for (int i = 1; i <= 6; ++i) {
        const float d = s.h * .05f * static_cast<float>(i) / 6.f;
        constexpr Ui::Color shade{0, 0, 0, .07f};
        paint.Fill({s.x, s.y, s.w, d}, shade);
        paint.Fill({s.x, s.Bottom() - d, s.w, d}, shade);
        paint.Fill({s.x, s.y + d, d, s.h - 2 * d}, shade);
        paint.Fill({s.Right() - d, s.y + d, d, s.h - 2 * d}, shade);
    }
}
// 音量の段（0〜10）から線形の音量へ。聞こえ方がおおよそ等間隔になるよう二乗する。
float Gain(int step) { const float v = std::clamp(step, 0, 10) / 10.f; return v * v; }
}

bool MenuDirector::Step(Repeat& repeat, bool down, double seconds) {
    if (!down) { repeat = {}; return false; }
    if (!repeat.held) { repeat.held = true; repeat.time = 0; return true; }
    const double before = repeat.time;
    repeat.time += seconds;
    // 0.38 秒を越えた後は 0.09 秒ごと。
    const auto ticks = [](double t) { return t < .38 ? 0 : 1 + static_cast<int>((t - .38) / .09); };
    return ticks(repeat.time) > ticks(before);
}

void MenuDirector::ReadKeys(HIKARI::WorldView& world, double seconds) {
    HIKARI::GameProjectInputActionState move{}, action{}, menu{};
    const auto input = world.GetInput();
    const bool moveValid = input.GetAction("Gameplay.Move", move);
    const bool actionValid = input.GetAction("Gameplay.Jump", action);
    const bool menuValid = input.GetAction("Gameplay.Menu", menu);
    keys_.left = Step(leftRepeat_, moveValid && move.x < -.5f, seconds);
    keys_.right = Step(rightRepeat_, moveValid && move.x > .5f, seconds);
    keys_.up = Step(upRepeat_, moveValid && move.y > .5f, seconds);
    keys_.down = Step(downRepeat_, moveValid && move.y < -.5f, seconds);
    keys_.confirm = actionValid && action.pressed != 0;
    keys_.menu = menuValid && menu.pressed != 0;
    if (const auto device = input.GetLastActiveDevice(); device != HIKARI::GameProjectInputDevice::None) {
        gamepad_ = device == HIKARI::GameProjectInputDevice::Gamepad;
    }
    if (holdAction_ && !(actionValid && action.down != 0)) holdAction_ = false;
}

void MenuDirector::Start(HIKARI::WorldView& world) {
    if (settingsLoaded_) return;
    settingsLoaded_ = true;
    std::string text;
    const bool found = LoadSlot(world, SettingsSlot(), text) && ReadSettingsText(text, settings_);
    if (!found) {
        // 初めての起動：画面の設定は今のものをそのまま写す（作者の設定を書き換えない）。
        HIKARI::GameProjectDisplaySettings display{};
        if (world.GetDisplaySettings().GetSettings(display)) {
            settings_.windowMode = std::clamp(static_cast<int>(display.windowMode), 0, 2);
            for (size_t i = 0; i < kResolutionValues.size(); ++i) if (kResolutionValues[i] == display.windowResolution) settings_.resolution = static_cast<int>(i);
            settings_.vsync = display.vSync != 0;
        }
    }
    saved_ = settings_;
    Apply(world, found);
    std::fprintf(stderr, "[TD2 menu] settings %s bgm=%d se=%d ambient=%d window=%d resolution=%d vsync=%d\n", found ? "loaded" : "default",
                 settings_.bgm, settings_.se, settings_.ambient, settings_.windowMode, settings_.resolution, settings_.vsync);
}

void MenuDirector::Apply(HIKARI::WorldView& world, bool display) {
    // 音：BGM と効果音はそれぞれの系統、環境音は声の系統（このゲームでは環境音だけが使う）。
    const auto audio = world.GetAudio();
    (void)audio.SetBusVolume(HIKARI::GameProjectAudioBus::Bgm, Gain(settings_.bgm));
    (void)audio.SetBusVolume(HIKARI::GameProjectAudioBus::Se, Gain(settings_.se));
    (void)audio.SetBusVolume(HIKARI::GameProjectAudioBus::Voice, Gain(settings_.ambient));
    if (!display) return;
    // 画面：窓の作り・大きさ・垂直同期だけを変え、AA や DLSS などの描き方の設定は今のまま。
    const auto view = world.GetDisplaySettings();
    HIKARI::GameProjectDisplaySettings next{};
    if (!view.GetSettings(next)) return;
    next.windowMode = static_cast<HIKARI::GameProjectWindowMode>(std::clamp(settings_.windowMode, 0, 2));
    next.windowResolution = next.renderResolution = kResolutionValues[static_cast<size_t>(std::clamp(settings_.resolution, 0, 3))];
    next.vSync = settings_.vsync ? 1 : 0;
    const auto result = view.Apply(next);
    std::fprintf(stderr, "[TD2 menu] display apply=%d\n", static_cast<int>(result));
}

bool MenuDirector::BeginCanvas(HIKARI::WorldView& world, HIKARI::ObjectView screen, Rect& region) {
    if (canvas_.failed || canvas_.drawing || !screen.IsValid()) return false;
    const auto canvases = world.GetUiCanvases();
    if (!canvases.IsValid()) return false;
    if (!canvas_.handle.IsValid()) {
        HIKARI::GameProjectUiCanvasDesc desc{};
        desc.width = desc.height = static_cast<uint32_t>(kCanvasSize);
        desc.clearColor = {0, 0, 0, 1};
        canvas_.handle = canvases.Create(desc);
        if (!canvas_.handle.IsValid()) {
            canvas_.failed = true;
            std::fprintf(stderr, "[TD2 menu] screen canvas unavailable; drawing over the view\n");
            return false;
        }
    }
    // 画面の比（縦÷横）に合わせて画布の左上を使う。縦長の画面は高さいっぱい、横長は幅いっぱい。
    const auto surface = RegionComponent::Surface(screen);
    const float aspect = surface.width > 0 && surface.height > 0 ? surface.height / surface.width : .6f;
    region = aspect <= 1 ? Rect{0, 0, kCanvasSize, kCanvasSize * aspect} : Rect{0, 0, kCanvasSize / aspect, kCanvasSize};
    if (!canvases.BeginUpdate(canvas_.handle, ++canvas_.revision)) return false;
    // 画面の材質を画布へ結ぶ（舞台が結んでいた 3D の絵と同じ所を差し替えるので、材質は作り直さない）。毎フレーム結び直しても
    // 同じ指定なら印を見直すだけ。画布もエンジンの中ではキャプチャの一つなので、キャプチャの結び付けで読みやすさの保護を
    // 付ける（主画面の色調と CRT を画面の上で抑え、字が白く飛ばない）。
    HIKARI::GameProjectSceneCaptureMaterialRegionBindingDesc binding{};
    binding.capture = {canvas_.handle.value, canvas_.handle.epoch};
    binding.modelObject = screen.GetHandle();
    std::memcpy(binding.materialSlotName, kScreenSlotName.data(), kScreenSlotName.size());
    binding.sourceRect = {0, 0, region.w / kCanvasSize, region.h / kCanvasSize};
    binding.flags = HIKARI::GameProjectSceneCaptureMaterialBindingFlags(HIKARI::GameProjectSceneCaptureMaterialBindingFlag::ProtectReadability);
    if (!world.GetSceneCapture().BindMaterialRegion(binding)) { (void)canvases.CancelUpdate(canvas_.handle); return false; }
    canvas_.screen = screen.GetHandle();
    canvas_.drawing = true;
    return true;
}

bool MenuDirector::PaintOnScreen(HIKARI::WorldView& world, HIKARI::ObjectView screen, int order, const std::function<void(Ui::Painter&, Ui::Rect)>& draw) {
    Rect region{};
    if (!BeginCanvas(world, screen, region)) return false;
    Ui::Painter paint(world, canvas_.handle, kCanvasSize, kCanvasSize, order);
    paint.SetGain(kCanvasGain); paint.SetTextOutline(kCanvasOutline);
    draw(paint, region);
    return true;
}

void MenuDirector::EndFrame(HIKARI::WorldView& world) {
    if (canvas_.drawing) {
        (void)world.GetUiCanvases().CommitUpdate(canvas_.handle);
        canvas_.drawing = false;
    } else if (canvas_.screen.value != 0) {
        // このフレームは描かなかった：画面を放す（このあと舞台がその画面を 3D の絵へ結び直す）。
        canvas_.screen = {};
    }
    // OS のマウスポインター：単体のゲームではずっと隠し、通関の「マウスをロックした」問いの間だけ見せる（その一瞬だけ本当の
    // マウスが使えると気付ける）。エディタの Play では、窓の矢印を描いた時だけ隠す（エディタの操作を邪魔しない）。
    const bool standalone = world.GetRuntimeSession().GetHostKind() == HIKARI::GameProjectRuntimeHostKind::StandaloneGame;
    const bool show = std::exchange(pointerRequested_, false), drawn = std::exchange(cursorDrawn_, false);
    // 単体のゲームと Play の既定は相対の捕捉で、エンジンがポインターを隠して毎フレーム中央へ戻す。見せる間だけ UI の操作中に
    // して捕捉を外し（本当のマウスで いいえ まで動かして押せる）、終わったら相対の捕捉へ戻す。
    if (show != pointerFree_) { (void)world.GetInput().SetUiInteractionActive(show ? 1 : 0); pointerFree_ = show; }
    if (show) (void)world.GetInput().SetPointerCursorVisible(1);
    else if (standalone || drawn) (void)world.GetInput().SetPointerCursorVisible(0);
}

void MenuDirector::SaveSettings(HIKARI::WorldView& world) {
    if (settings_ == saved_) return;
    if (StoreSlot(world, SettingsSlot(), WriteSettingsText(settings_))) saved_ = settings_;
}

void MenuDirector::Buttons(Ui::Painter& paint, Rect area, int choice, float& cursor, double seconds, bool pressed) {
    // area は二つのボタンを並べる矩形（右寄せ）。はい が左、いいえ が右。
    const float bh = area.h, bw = bh * Ui::kButtonAspect, gap = bw * .2f;
    const float x1 = area.Right() - bw, x0 = x1 - gap - bw;
    const std::array<std::string_view, 2> labels{"はい", "いいえ"};
    for (int i = 0; i < 2; ++i) {
        const auto state = i != choice ? Ui::State::Normal : pressed ? Ui::State::Pressed : Ui::State::Selected;
        paint.Button({i == 0 ? x0 : x1, area.y, bw, bh}, labels[static_cast<size_t>(i)], state, bh * .46f);
    }
    // カーソルは選んだボタンの中ほどへ滑って寄る（矢印の先をボタンの右下寄りに置く）。
    cursor = Follow(cursor, static_cast<float>(choice), seconds);
    // 押した時はクリックしたように少し沈める。
    const float click = pressed ? bh * .04f : 0;
    const float cx = x0 + (x1 - x0) * cursor + bw * .64f + click * .5f, cy = area.y + bh * .58f + click;
    paint.Cursor(cx, cy, bh * .78f);
    cursorDrawn_ = true;
}

void MenuDirector::DrawContinue(Ui::Painter& paint, float alpha, float windowAlpha, double seconds, bool pressed) {
    const float W = paint.Width(), H = paint.Height();
    paint.SetAlpha(alpha);
    // 背景は真っ黒（画の全面）、問いの窓は真ん中。
    paint.Fill({0, 0, W, H}, {0, 0, 0, 1});
    paint.SetAlpha(alpha * windowAlpha);
    const float w = (std::min)(W * .44f, H * .84f), h = w * .5f;
    const Rect window{W * .5f - w * .5f, H * .5f - h * .5f, w, h};
    const Rect inner = paint.Window(window, h * .13f / Ui::kWindowTop, "メッセージ");
    const float pad = inner.h * .1f, icon = inner.h * .32f;
    paint.Image(kIconQuestion, {inner.x + pad, inner.y + pad, icon * 152.f / 159.f, icon});
    paint.Text("まえの つづきから はじめますか？", {inner.x + pad * 1.8f + icon, inner.y + pad, inner.w - pad * 2.6f - icon, icon}, inner.h * .104f, Ui::kInk);
    const float bh = inner.h * .27f;
    Buttons(paint, {inner.x, inner.Bottom() - pad - bh, inner.w - pad, bh}, choice_, cursor_, seconds, pressed);
}

bool MenuDirector::PrepareStartup(HIKARI::WorldView& world, uint64_t frameIndex) {
    if (startupReady_)
        return true;
    Ui::Painter cover(world, kFadeOrder);
    cover.Fill({0, 0, cover.Width(), cover.Height()}, {0, 0, 0, 1});
    if (startupBlackFrame_ == 0) {
        startupBlackFrame_ = frameIndex;
        return false;
    }
    HIKARI::GameProjectCameraViewSnapshot presented{};
    if (!world.GetCamera().TryGetViewSnapshot(presented) || presented.frameIndex < startupBlackFrame_ ||
        presented.source != HIKARI::GameProjectPrimaryViewSource::Game)
        return false;
    // ウィンドウ設定の復元と初回描画は、既に提示した黒の裏で行う。
    Start(world);
    if (warmFrames_ < kWarmFrames) {
        Warm(world);
        startupWarmFrame_ = frameIndex;
        return false;
    }
    if (presented.frameIndex < startupWarmFrame_)
        return false;
    startupReady_ = true;
    return true;
}

void MenuDirector::Warm(HIKARI::WorldView& world) {
    if (warmFrames_ >= kWarmFrames)
        return;
    ++warmFrames_;
    // 一番下に重ねる。メールの本文は一字ずつ増えるので、全部の字を先に一度出しておく（打っている途中で止まらない）。
    Ui::Painter paint(world, -90000);
    paint.SetAlpha(kWarmAlpha);
    const Rect r{0, 0, 64, 32};
    (void)paint.Window({0, 0, 160, 96}, .2f, "NEW MAIL");
    paint.Row(r, false);
    paint.Row(r, true);
    paint.Button(r, "はい", Ui::State::Normal, 12);
    paint.Button(r, "いいえ", Ui::State::Selected, 12);
    paint.Button(r, "はい", Ui::State::Pressed, 12);
    paint.Cursor(8, 8, 16);
    paint.Meter(r, 1, 2);
    for (const auto texture : {kIconQuestion, kIconWarning, kIconMail, kIconMailOpen, kMailFields})
        paint.Image(texture, r);
    static const std::string glyphs = [] {
        std::string glyphs =
            "メッセージまえの つづきから はじめますか？件名：いっしょにあそぼ差出人：？？？_オートセーブしますか？メニューオンオフ＜＞";
        glyphs += kMailBody;
        for (const auto label : kRowLabels)
            glyphs += label;
        for (const auto label : kWindowModes)
            glyphs += label;
        for (const auto label : kResolutions)
            glyphs += label;
        glyphs += "W・S選ぶA・D変えるスペース決定ESC閉じる十字キー選ぶ・変えるA決定START閉じる";
        glyphs += kPopupGlyphs;
        for (const auto text : {kDevSubject, kDevSender, kDevBody, kDevRestart, kDevConfirm})
            glyphs += text;
        return glyphs;
    }();
    paint.Text(glyphs, {0, 0, 4096, 32}, 24, Ui::kInk);
}

void MenuDirector::DrawClosing(HIKARI::WorldView& world, double seconds) {
    seconds = (std::min)(seconds, kUiStep);
    if (closing_ >= 0 && (closing_ += seconds) < .4) {
        // 押した姿を一瞬見せてから、全体を薄くしていく。
        Ui::Painter paint(world, kContinueOrder);
        DrawContinue(paint, static_cast<float>(1 - Math::SmoothStep((closing_ - .08) / .32)), 1, seconds, true);
    } else {
        closing_ = -1;
    }
    // ESC の窓は開いた時の動きを逆にたどって閉じる。
    if (pauseClosing_ >= 0 && (pauseClosing_ += seconds) < .14) {
        DrawPause(world, world.FindObject(pauseScreen_), static_cast<float>(1 - pauseClosing_ / .14), false, seconds);
    } else {
        pauseClosing_ = -1;
    }
}

MenuDirector::Choice MenuDirector::UpdateContinue(HIKARI::WorldView& world, double seconds) {
    if (mode_ != Mode::Continue) return Choice::None;
    // 起動の交接は PrepareStartup
    // が完了している。窓だけを開き、黒い背景は不透明のまま保つ。
    const double step = (std::min)(seconds, kUiStep);
    opened_ += step;
    if (keys_.left)
        choice_ = 0;
    if (keys_.right)
        choice_ = 1;
    // 起動の最初に出るので、黒い背景は初めから覆い（下のタイトルの画を見せない）、窓だけ現れる。
    Ui::Painter paint(world, kContinueOrder);
    DrawContinue(paint, 1, static_cast<float>(Math::SmoothStep(opened_ / .3)), step, false);
    if (!keys_.confirm || opened_ < .2)
        return Choice::None;
    holdAction_ = true;
    mode_ = Mode::None;
    closing_ = 0;
    std::fprintf(stderr, "[TD2 menu] continue=%s\n", choice_ == 0 ? "yes" : "no");
    return choice_ == 0 ? Choice::Yes : Choice::No;
}

MenuDirector::Choice MenuDirector::UpdateTitle(HIKARI::WorldView& world, HIKARI::ObjectView screen, float alpha, bool interactive, double seconds, bool developer) {
    seconds = (std::min)(seconds, kUiStep);
    titleTime_ += seconds;
    const std::string_view mailBody = developer ? kDevBody : kMailBody;
    // 出てくる途中でスペースを押したら、窓・本文・ボタンをすぐに出しきる（その押下では決めない）。
    const double readyTime = kMailType + (static_cast<double>(Ui::CharCount(mailBody)) + 7) / kMailCharsPerSecond;
    const bool skipped = interactive && leaving_ < 0 && keys_.confirm && titleTime_ < readyTime;
    if (skipped) { titleTime_ = readyTime; holdAction_ = true; }
    // 決めた後：押したボタンを一瞬見せ、ちらついて、ブラウン管を切るように横の線から点へ縮んで消える。
    if (leaving_ >= 0) leaving_ += seconds;
    if (leaving_ >= .5) {
        // はい：消え終えたら画面を放す（パソコンの画面はオープニングの砂嵐へ戻る）。
        if (!leavingQuit_) { leaving_ = -1; return Choice::None; }
        // いいえ：画面が消えた後、画全体を暗くしてから終える。
        Ui::Painter fade(world, kFadeOrder);
        fade.Fill({0, 0, fade.Width(), fade.Height()}, {0, 0, 0, static_cast<float>(Math::SmoothStep((leaving_ - .5) / .35))});
        if (leaving_ < .9 || quitReported_) return Choice::None;
        quitReported_ = true;
        return Choice::No;
    }
    if (alpha <= 0 && leaving_ < 0) return Choice::None;
    // パソコンの画面に画布を貼って描く（画面の絵として 3D で写る）。画布が使えない時は、タイトル待ちで動けなくならないよう
    // 画の真ん中に平らに出す。
    Rect region{};
    const bool onScreen = BeginCanvas(world, screen, region);
    if (!onScreen && titleTime_ < 1) return Choice::None;   // 始まりの数フレームはカメラがまだ無いことがある
    Ui::Painter paint = onScreen ? Ui::Painter(world, canvas_.handle, kCanvasSize, kCanvasSize, kTitleOrder) : Ui::Painter(world, kTitleOrder);
    if (onScreen) { paint.SetGain(kCanvasGain); paint.SetTextOutline(kCanvasOutline); }
    const Rect glass = onScreen ? region : Rect{paint.Width() * .25f, paint.Height() * .25f, paint.Width() * .5f, paint.Height() * .5f};
    if (leaving_ >= 0) {
        const double t = leaving_;
        if (t >= .22) {
            const double k = (t - .22) / .28;
            const float line = (std::max)(2.f, glass.h * .006f);
            if (k < .55) {
                const float h = static_cast<float>(line + (glass.h - line) * (1 - Math::SmoothStep(k / .55)));
                paint.Fill({glass.x, glass.y + (glass.h - h) * .5f, glass.w, h}, {.85f, .92f, .9f, 1});
            } else {
                const float w = static_cast<float>(line + (glass.w - line) * (1 - Math::SmoothStep((k - .55) / .45)));
                paint.Fill({glass.x + (glass.w - w) * .5f, glass.y + (glass.h - line) * .5f, w, line}, {1, 1, 1, 1});
            }
            return Choice::None;
        }
        // ちらつき：30 分の 1 秒ごとに消えたり（画面が一瞬暗くなる）、少し横へずれたりする。
        const int tick = static_cast<int>(t * 30);
        if (t > .1 && tick % 3 == 1) return Choice::None;
        alpha = 1;
    }
    paint.SetAlpha(alpha);
    const bool leaving = leaving_ >= 0;
    const float jitter = leaving && leaving_ > .1 ? static_cast<float>(std::sin(leaving_ * 131.0)) * glass.w * .012f : 0;
    const Rect s{glass.x + jitter, glass.y, glass.w, glass.h};
    // 机の上（画面いっぱい）と、真ん中のメールの窓。窓は最初に机の上だけの間を少し置いてから、縦に伸びて開く
    // （通関の目覚めの後のタイトル待ちでは、もう開いたまま）。画布の地は黒なので、alpha は黒からの出方になる。
    paint.Fill(s, kDesktop);
    Shade(paint, s);
    const float open = leaving ? 1.f : static_cast<float>(Math::SmoothStep((titleTime_ - kMailOpen) / .22));
    if (open <= 0) return Choice::None;
    paint.SetAlpha(alpha * open);
    // 枠だけが縦に伸びて開く。中身は開ききった窓の位置に置き、枠がほぼ開いてから出す（伸びる枠に合わせると字が動いて見える）。
    const float w = s.w * .9f, full = s.h * .86f, h = full * (.9f + .1f * open), scale = full * .11f / Ui::kWindowTop;
    const Rect window{s.x + (s.w - w) * .5f, s.y + (s.h - full) * .48f, w, full};
    (void)paint.Window({window.x, s.y + (s.h - h) * .48f, w, h}, scale, {});
    const float content = leaving ? 1.f : static_cast<float>(Math::SmoothStep((open - .5) / .5));
    paint.SetAlpha(alpha * open * content);
    paint.WindowTitle(window, scale, "NEW MAIL");
    const Rect inner = Ui::WindowInner(window, scale);
    const float pad = inner.h * .07f;
    // 件名と差出人の欄（素材の欄の絵。字は点線の右）。右には届いた印を置くので、窓の 7 割の幅まで。
    float fw = inner.w * .7f, fh = fw / Ui::kMailFieldsAspect;
    if (fh > inner.h * .4f) { fh = inner.h * .4f; fw = fh * Ui::kMailFieldsAspect; }
    const Rect fields{inner.x + pad, inner.y + pad, fw, fh};
    paint.Image(kMailFields, fields);
    const float textX = fields.x + fields.w * .23f, textW = fields.w * .74f, size = fields.h * .185f;
    paint.Text(developer ? kDevSubject : "件名：いっしょにあそぼ", {textX, fields.y + fields.h * .1f, textW, fields.h * .38f}, size, Ui::kInk);
    paint.Text(developer ? kDevSender : "差出人：？？？", {textX, fields.y + fields.h * .52f, textW, fields.h * .38f}, size, Ui::kInk);
    // 届いたばかりの印（右上に赤い点の封筒）。はい を押した後は、開いた封筒（読んだ印）に替わる（いいえ はそのまま）。
    const float mail = fields.h * .62f;
    if (leaving && !leavingQuit_) {
        const float m = mail * 1.06f, mw = m * 221.f / 212.f;
        paint.Image(kIconMailOpen, {inner.Right() - pad - mail * 231.f / 194.f * .5f - mw * .5f, fields.y + (fields.h - m) * .5f, mw, m});
    } else {
        // 2.4 秒ごとに小さく二度跳ねて、届いたばかりだと知らせる。
        const double beat = std::fmod(titleTime_, 2.4);
        const float hop = beat < .36 ? static_cast<float>(std::abs(std::sin(beat / .36 * 6.2832))) * mail * .14f : 0.f;
        paint.Image(kIconMail, {inner.Right() - pad - mail * 231.f / 194.f, fields.y + (fields.h - mail) * .5f - hop, mail * 231.f / 194.f, mail});
    }
    // 本文：窓が開いてから一字ずつ打ち出す（開発者のメールは二行で、少し小さな字）。
    const std::string_view body = mailBody;
    const double typed = leaving ? 1e9 : (titleTime_ - kMailType) * kMailCharsPerSecond;
    const float bodyY = fields.Bottom() + pad * .9f;
    if (typed > 0) {
        // 打っている間と打ち終えて少しの間は、末尾に点滅する「_」を付ける。
        std::string text(Ui::CharHead(body, static_cast<size_t>(typed)));
        const double after = typed - static_cast<double>(Ui::CharCount(body));
        if (after < 12 && std::fmod(titleTime_, .5) < .3) text += "_";
        const float lineH = developer ? inner.h * .12f : inner.h * .15f, textSize = developer ? inner.h * .088f : inner.h * .105f;
        float y = bodyY;
        for (size_t start = 0; start <= text.size();) {
            const size_t cut = text.find('\n', start);
            paint.Text(std::string_view(text).substr(start, cut == std::string::npos ? std::string::npos : cut - start),
                       {inner.x + pad * 1.3f, y, inner.w - pad * 2.6f, lineH}, textSize, Ui::kInk);
            if (cut == std::string::npos) break;
            start = cut + 1; y += lineH;
        }
    }
    // はい・いいえ：打ち終えてから出す。
    const float ready = leaving ? 1.f : static_cast<float>(Math::SmoothStep((typed - static_cast<double>(Ui::CharCount(body)) - 4) / 3));
    if (ready <= 0) return Choice::None;
    if (developer) {
        // 開発者のメール：「最初から」だけ。選ぶと窓の真ん中に保存を消す確かめを出し、はい で最初から。
        paint.SetAlpha(alpha * open * ready);
        const float bh = inner.h * .2f;
        const std::array<std::string_view, 1> restart{kDevRestart};
        std::array<Rect, 1> rects{};
        Popups::Buttons(paint, {inner.x, inner.Bottom() - pad - bh, inner.w - pad, bh}, restart, devConfirm_ ? -1 : 0, leaving, -1, rects);
        if (!devConfirm_) { paint.Cursor(rects[0].x + rects[0].w * .64f, rects[0].y + bh * .58f, bh * .78f); cursorDrawn_ = true; }
        if (devConfirm_ || leaving) {
            paint.SetAlpha(alpha);
            const float cw = inner.w * .86f;
            const Rect buttons = Popups::Draw(paint, window.x + window.w * .5f, window.y + window.h * .52f, cw,
                                              {"メッセージ", kDevConfirm, Popups::kIconWarning, 1, 1}, true);
            const std::array<std::string_view, 2> labels{"はい", "いいえ"};
            std::array<Rect, 2> answer{};
            Popups::Buttons(paint, buttons, labels, devChoice_, leaving, -1, answer);
            devCursor_ = Follow(devCursor_, static_cast<float>(devChoice_), seconds);
            const float cx = answer[0].x + (answer[1].x - answer[0].x) * devCursor_ + answer[0].w * .64f;
            paint.Cursor(cx, answer[0].y + answer[0].h * .58f, answer[0].h * .78f);
            cursorDrawn_ = true;
        }
        if (!interactive || leaving || ready < 1 || skipped || !keys_.confirm) {
            if (interactive && devConfirm_ && !leaving) { if (keys_.left) devChoice_ = 0; if (keys_.right) devChoice_ = 1; }
            return Choice::None;
        }
        holdAction_ = true;
        if (!devConfirm_) { devConfirm_ = true; devChoice_ = 1; devCursor_ = 1; return Choice::None; }
        if (devChoice_ != 0) { devConfirm_ = false; return Choice::None; }
        std::fprintf(stderr, "[TD2 menu] developer mail restart\n");
        leaving_ = 0; leavingQuit_ = false;
        return Choice::Yes;
    }
    if (interactive && !leaving && ready >= 1) {
        if (keys_.left) titleChoice_ = 0;
        if (keys_.right) titleChoice_ = 1;
    }
    paint.SetAlpha(alpha * open * ready);
    const float bh = inner.h * .2f;
    Buttons(paint, {inner.x, inner.Bottom() - pad - bh, inner.w - pad, bh}, titleChoice_, titleCursor_, seconds, leaving);
    if (!interactive || leaving || ready < 1 || !keys_.confirm || skipped) return Choice::None;
    holdAction_ = true;
    std::fprintf(stderr, "[TD2 menu] title=%s\n", titleChoice_ == 0 ? "yes" : "no");
    leaving_ = 0; leavingQuit_ = titleChoice_ != 0;
    return leavingQuit_ ? Choice::None : Choice::Yes;
}

MenuDirector::PauseResult MenuDirector::UpdatePause(HIKARI::WorldView& world, HIKARI::ObjectView screen, double seconds, bool canSave) {
    if (!PauseOpen()) return PauseResult::None;
    seconds = (std::min)(seconds, kUiStep);
    if (mode_ == Mode::Quitting) {
        // 答えたボタンを押した姿のまま、画全体を暗くしてから終える。
        quitting_ += seconds;
        DrawPause(world, world.FindObject(pauseScreen_), 1, quitAsked_, seconds, true);
        Ui::Painter fade(world, kFadeOrder);
        fade.Fill({0, 0, fade.Width(), fade.Height()}, {0, 0, 0, static_cast<float>(Math::SmoothStep(quitting_ / .35))});
        if (quitting_ < .4 || quitReported_) return PauseResult::None;
        quitReported_ = true;
        return PauseResult::Quit;
    }
    opened_ += seconds;
    const bool confirming = mode_ == Mode::Confirm;
    HIKARI::GameProjectDisplayCapabilities capabilities{};
    displayAvailable_ = world.GetDisplaySettings().GetCapabilities(capabilities) && capabilities.gameWindowActive != 0;
    // 選ぶ・変える。
    bool changed = false, displayChanged = false;
    if (!confirming) {
        if (keys_.up) row_ = (row_ + kRowCount - 1) % kRowCount;
        if (keys_.down) row_ = (row_ + 1) % kRowCount;
        const int delta = (keys_.right ? 1 : 0) - (keys_.left ? 1 : 0);
        const bool displayRow = row_ == kWindowMode || row_ == kResolution || row_ == kVsync;
        if (delta != 0 && (displayAvailable_ || !displayRow)) {
            auto volume = [&](int& value) { const int next = std::clamp(value + delta, 0, 10); changed = next != value; value = next; };
            auto option = [&](int& value, int count) { value = (value + delta + count) % count; changed = displayChanged = true; };
            switch (row_) {
            case kBgm: volume(settings_.bgm); break;
            case kSe: volume(settings_.se); break;
            case kAmbient: volume(settings_.ambient); break;
            case kWindowMode: option(settings_.windowMode, 3); break;
            case kResolution: option(settings_.resolution, 4); break;
            case kVsync: { int v = settings_.vsync ? 1 : 0; option(v, 2); settings_.vsync = v != 0; break; }
            default: break;
            }
        }
    } else {
        if (keys_.left) confirmChoice_ = 0;
        if (keys_.right) confirmChoice_ = 1;
    }
    // 音量はすぐ、画面は変え終えて 0.6 秒たってから（閉じる時はその場で）合わせる。
    if (changed) { if (displayChanged) displayPending_ = .6; else Apply(world, false); }
    if (displayPending_ > 0 && (displayPending_ -= seconds) <= 0) { displayPending_ = 0; Apply(world, true); }
    const auto flushDisplay = [&] { if (displayPending_ > 0) { displayPending_ = 0; Apply(world, true); } };
    pauseScreen_ = screen.GetHandle();
    DrawPause(world, screen, static_cast<float>(Math::SmoothStep(opened_ / .18)), confirming, seconds);

    // 決める・閉じる。
    if (confirming) {
        if (keys_.menu) { mode_ = Mode::Pause; return PauseResult::None; }
        if (!keys_.confirm) return PauseResult::None;
        holdAction_ = true;
        quitSave_ = confirmChoice_ == 0;
        flushDisplay();
        SaveSettings(world);
        mode_ = Mode::Quitting; quitting_ = 0; quitAsked_ = true;
        std::fprintf(stderr, "[TD2 menu] quit save=%d\n", quitSave_);
        return PauseResult::None;
    }
    const bool close = keys_.menu || (keys_.confirm && row_ == kResume);
    if (close && opened_ > .1) {
        if (keys_.confirm) holdAction_ = true;
        flushDisplay();
        SaveSettings(world);
        mode_ = Mode::None;
        pauseClosing_ = 0;
        return PauseResult::Closed;
    }
    if (keys_.confirm && row_ == kQuit) {
        holdAction_ = true;
        if (canSave) { mode_ = Mode::Confirm; confirmChoice_ = 0; confirmCursor_ = 0; }
        else {
            quitSave_ = false; flushDisplay(); SaveSettings(world); mode_ = Mode::Quitting; quitting_ = 0; quitAsked_ = false;
            std::fprintf(stderr, "[TD2 menu] quit save=0 (no slot)\n");
        }
    }
    return PauseResult::None;
}

void MenuDirector::DrawPause(HIKARI::WorldView& world, HIKARI::ObjectView screen, float appear, bool confirming, double seconds, bool answered) {
    // 窓の置き場：今いる部屋のテレビの画面（画布を貼り、画面の絵として 3D で写す）。画面には机の上を敷き、窓だけが出入りの
    // 動きをする。画布が使えない時は画の真ん中の仮の画面へ平らに出す。
    Rect region{};
    const bool onScreen = BeginCanvas(world, screen, region);
    Ui::Painter paint = onScreen ? Ui::Painter(world, canvas_.handle, kCanvasSize, kCanvasSize, kPauseOrder) : Ui::Painter(world, kPauseOrder);
    if (onScreen) { paint.SetGain(kCanvasGain); paint.SetTextOutline(kCanvasOutline); }
    const float W = paint.Width(), H = paint.Height();
    const Rect glass = onScreen ? region : Rect{W * .2f, H * .2f, W * .6f, H * .6f};
    if (onScreen) { paint.Fill(glass, kDesktop); Shade(paint, glass); }
    paint.SetAlpha(appear);
    if (!onScreen) paint.Fill(glass, {0, 0, 0, .5f});
    // 画面は横に長いので、窓は高さいっぱい・幅はそれに合わせて少し横長に。
    const float h = glass.h * .94f, w = (std::min)(glass.w * .94f, h * 1.45f);
    const float cx = glass.x + glass.w * .5f, cy = glass.y + glass.h * .5f;
    // 出る時はブラウン管の窓のように少し縦に伸びて開く。動くのは枠だけで、行と字は開ききった窓の位置に、枠がほぼ開いてから
    // 出す（伸びる枠に合わせると字が動いて見える）。
    const float grow = .86f + .14f * appear, scale = h * .1f / Ui::kWindowTop;
    const Rect window{cx - w * .5f, cy - h * .5f, w, h};
    (void)paint.Window({cx - w * .5f, cy - h * grow * .5f, w, h * grow}, scale, {});
    paint.SetAlpha(appear * static_cast<float>(Math::SmoothStep((appear - .5) / .5)));
    paint.WindowTitle(window, scale, "メニュー");
    const Rect inner = Ui::WindowInner(window, scale);
    // 小さな画面でも字が読めるよう、行を詰めて高さを稼ぐ（八行と案内の一行で窓の中いっぱい）。
    const float vpad = inner.h * .035f, pad = inner.w * .03f, rowH = inner.h * .1f, gap = rowH * .06f;
    for (int i = 0; i < kRowCount; ++i) {
        const Rect row{inner.x + pad, inner.y + vpad + i * (rowH + gap), inner.w - 2 * pad, rowH};
        const bool selected = i == row_ && !confirming;
        const bool disabled = !displayAvailable_ && (i == kWindowMode || i == kResolution || i == kVsync);
        paint.Row(row, selected);
        const float text = rowH * .58f;
        paint.Text(kRowLabels[static_cast<size_t>(i)], {row.x + rowH * .55f, row.y, row.w * .45f, row.h}, text, disabled ? Ui::kFaint : Ui::kInk);
        const Rect value{row.x + row.w * .46f, row.y, row.w * .5f, row.h};
        const auto volume = [&](int step) { paint.Meter({value.Right() - (rowH * .5f * 38.f / 41.f) * 13, row.y + rowH * .25f, value.w, rowH * .5f}, step, 10); };
        const auto option = [&](std::string_view label) {
            // 変えられない時は矢印を外す。
            if (disabled) { paint.Text(label, value, text, Ui::kFaint, Ui::Align::Right); return; }
            paint.Text(std::string("＜ ") + std::string(label) + " ＞", value, text, selected ? Ui::kInk : Ui::kFaint, Ui::Align::Right);
        };
        switch (i) {
        case kBgm: volume(settings_.bgm); break;
        case kSe: volume(settings_.se); break;
        case kAmbient: volume(settings_.ambient); break;
        case kWindowMode: option(kWindowModes[static_cast<size_t>(settings_.windowMode)]); break;
        case kResolution: option(kResolutions[static_cast<size_t>(settings_.resolution)]); break;
        case kVsync: option(settings_.vsync ? "オン" : "オフ"); break;
        default: break;
        }
    }
    // カーソルは選んだ行の名前と値の間（目盛りや値に重ならない所）へ滑って寄る。
    rowCursor_ = Follow(rowCursor_, static_cast<float>(row_), seconds);
    if (!confirming) {
        const float y = inner.y + vpad + rowCursor_ * (rowH + gap);
        paint.Cursor(inner.x + pad + (inner.w - 2 * pad) * .4f, y + rowH * .55f, rowH * .9f);
        cursorDrawn_ = true;
    }
    paint.Text(gamepad_ ? "十字キー 選ぶ・変える　A 決定　START 閉じる" : "W・S 選ぶ　A・D 変える　スペース 決定　ESC 閉じる", {inner.x + pad, inner.Bottom() - vpad * .5f - rowH * .62f, inner.w - 2 * pad, rowH * .62f},
               rowH * .42f, Ui::kFaint, Ui::Align::Center);

    // やめる前の保存の問い（ESC の窓の真ん中に重ねる。窓からはみ出さない大きさ）。
    if (confirming) {
        // 同じ画布へ続けて描く（後から描いた物が上に重なる）。
        auto& ask = paint;
        ask.SetAlpha(1);
        const float ch = (std::min)(window.w * .8f * .46f, window.h * .56f), cw = ch / .46f;
        const Rect box{cx - cw * .5f, cy - ch * .5f, cw, ch};
        const Rect in = ask.Window(box, ch * .16f / Ui::kWindowTop, "メッセージ");
        const float p = in.h * .1f, icon = in.h * .36f;
        ask.Image(kIconWarning, {in.x + p, in.y + p, icon * 151.f / 144.f, icon});
        ask.Text("オートセーブしますか？", {in.x + p * 2 + icon * 1.05f, in.y + p, in.w - p * 3 - icon, icon}, in.h * .14f, Ui::kInk);
        const float bh = in.h * .28f;
        Buttons(ask, {in.x, in.Bottom() - p - bh, in.w - p, bh}, confirmChoice_, confirmCursor_, seconds, answered);
    }
}
}
