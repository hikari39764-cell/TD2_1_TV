// オープニングと通関の毎フレームの駆動：灯の明滅、画角とぼけ、黒帯と黒とまぶた、暗転の中の弾窓、小物。
#include "TD2Intro.h"
#include "TD2Popups.h"
#include "TD2RegionComponent.h"
#include "TD2StageLight.h"
#include "TD2Transform.h"
#include "HIKARI/Project/ActionViews.h"
#include "HIKARI/Project/ComponentViews.h"
#include "HIKARI/Project/EnvironmentView.h"
#include "HIKARI/Project/GameFlowViews.h"
#include "HIKARI/Project/LightTypes.h"
#include "HIKARI/Project/SequenceView.h"
#include "HIKARI/Project/CameraViewSnapshot.h"
#include "HIKARI/Project/UiTypes.h"
#include "HIKARI/Project/UiView.h"
#include "HIKARI/Project/WorldQuery.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>

namespace TD2 {
namespace {
using Math::Hash01;
// UI の論理の大きさ（取れなければ 1920×1080）。黒帯・タイトル・まぶたの帯をこの大きさで置く。
std::pair<float, float> UiSize(HIKARI::WorldView& world) {
    HIKARI::GameProjectCameraViewSnapshot view{};
    if (world.GetCamera().TryGetViewSnapshot(view) && view.uiLogicalWidth > 0 && view.uiLogicalHeight > 0) {
        return {static_cast<float>(view.uiLogicalWidth), static_cast<float>(view.uiLogicalHeight)};
    }
    return {1920.f, 1080.f};
}
// 刻みの間をなめらかにつないだ乱数（frequency 回/秒）。
double Noise(uint32_t seed, double t, double frequency) {
    const double x = (std::max)(0.0, t) * frequency;
    const auto i = static_cast<uint32_t>(std::floor(x));
    const double s = Math::SmoothStep(x - std::floor(x));
    return Hash01(seed, i) * (1 - s) + Hash01(seed, i + 1) * s;
}
// 暗転の中の弾窓（一つ目の演出が終わってからの秒）：飛び込んだ後の黒を kIntroBlackHold 秒見せ、タイトルの知らせ、
// 楽しんでね、そして「ゴールで まってるね」の嵐が画を埋め、ぷつりと切れて黒を少し置いてから二つ目の演出へ。
constexpr double kNoticeIn = kIntroBlackHold, kNoticeOut = kIntroBlackHold + 2.3;
constexpr double kEnjoyIn = kIntroBlackHold + 2.5, kEnjoyOut = kIntroBlackHold + 4.0;
constexpr double kStormIn = kIntroBlackHold + 4.1, kStormSeconds = 2.3;
constexpr int kStormPeak = 150;
constexpr double kNoticeCharsPerSecond = 20, kPopupClose = .1;
constexpr std::string_view kNoticeText = "ちなみにタイトルは：\nビジョン　です", kEnjoyText = "たのしんでね！";
constexpr std::array<std::string_view, 5> kWaitTexts{"ゴールで まってるね", "ゴールで まってるね！", "まってるね まってるね",
                                                     "ずっと まってるね", "ゴールで ゴールで ゴールで"};
// 暗転の中の弾窓の重なりの順（黒帯 100000 とまぶた 100001 の上）。
constexpr int kPopupOrder = 140000;
// タイトル待ちの寄り：画角を何度狭めるか（50 度の広い画ではパソコンの画面が画の半分ほどで、内部の描き分け（DLSS の
// 内部解像度）ではメールの細い字がにじむ）。
constexpr double kTitleZoomDegrees = 12;
// 閉じかけたまぶた（0 閉じ、1 開いた）を上下の黒で描く。
void DrawLids(HIKARI::WorldView& world, double open) {
    if (open >= .999) { return; }
    const auto [width, height] = UiSize(world);
    // UI は仕上げの後に描くので、上下の黒は本当の黒になる（色調整では漫画の仕上げの紙の色が残る）。
    const double lid = height * .5 * (1 - (std::max)(0.0, open));
    HIKARI::GameProjectUiSpriteRequest request{};
    request.sourceId = 0x7D214Bu; request.order = 100001;
    request.referenceWidth = width; request.referenceHeight = height;
    request.width = width; request.height = static_cast<float>(lid + 2);
    const auto ui = world.GetUi();
    request.y = -1;
    (void)ui.SubmitSprite(kLetterboxTexture, request);
    request.y = static_cast<float>(height - lid - 1);
    (void)ui.SubmitSprite(kLetterboxTexture, request);
}
}

void IntroDirector::DrawBlack(HIKARI::WorldView& world) const {
    Ui::Painter paint(world, kPopupOrder);
    paint.Fill({0, 0, paint.Width(), paint.Height()}, {0, 0, 0, 1});
}

void IntroDirector::DrawPopups(HIKARI::WorldView& world, double u, double seconds) {
    Ui::Painter paint(world, kPopupOrder);
    const float W = paint.Width(), H = paint.Height();
    const float width = (std::min)(W * .36f, H * .62f);
    // 弾窓の間は本当の黒（一つ目の演出の暗転の効果はその終わりで切れるので、画の上を黒で覆う）。
    paint.Fill({0, 0, W, H}, {0, 0, 0, 1});
    const double before = u - seconds;
    // 一つずつの弾窓：縦に伸びて現れ、閉じる時は縦に潰れて消える。
    const auto single = [&](std::string_view text, double in, double out, float cx, float cy, std::string_view icon, bool typed) {
        if (u < in || u >= out + kPopupClose) { return; }
        if (before < in) { ++popupSounds_; }
        Popups::Look look;
        look.title = "？？？"; look.text = text; look.icon = icon;
        look.appear = u < out ? Popups::Appear(u - in) : static_cast<float>(1 - (u - out) / kPopupClose);
        // 打ち出す前は 0 字（負の数は「全部」なので、出た瞬間に全文が一瞬見えてしまう）。
        if (typed) { look.typed = (std::max)(0.0, (u - in - .12) * kNoticeCharsPerSecond); }
        (void)Popups::Draw(paint, cx, cy, width, look);
    };
    single(kNoticeText, kNoticeIn, kNoticeOut, W * .5f, H * .5f, Popups::kIconMail, true);
    single(kEnjoyText, kEnjoyIn, kEnjoyOut, W * .53f, H * .47f, {}, false);
    // 嵐：だんだん速く増えて画を埋め、終わりでぷつりと切れる（揺れも終わりへ向けて強くなる）。
    if (u >= kStormIn && u < kStormIn + kStormSeconds) {
        const Popups::Storm storm{kWaitTexts, 0x51u, kStormSeconds, kStormPeak, 1.f};
        const double v = u - kStormIn;
        popupSounds_ += Popups::Count(storm, v) - Popups::Count(storm, (std::max)(0.0, before - kStormIn));
        Popups::DrawStorm(paint, {0, 0, W, H}, storm, v, static_cast<float>(v / kStormSeconds));
    }
}

void IntroDirector::DriveDepthOfField(HIKARI::WorldView& world, double scale) {
    const auto view = world.GetCamera();
    const auto camera = world.FindSceneObject(settings_.cameraObject);
    if (!depthOfFieldRead_) {
        HIKARI::GameProjectCameraLens lens{};
        depthOfFieldRead_ = true;
        if (!camera.IsValid() || !view.GetAuthoredLens(camera.GetHandle().value, lens) || !lens.depthOfField.enabled) { return; }
        depthOfField_ = lens.depthOfField;
    }
    if (!depthOfField_.enabled) { return; }
    // 一人称では手元の机も奥の工場も写るので、ぼけの半径だけを小さくして奥行きの手掛かりは残す。
    HIKARI::GameProjectCameraDepthOfFieldRequest request{};
    request.cameraObjectHandle = camera.GetHandle().value;
    request.sourceId = 0x7D2147u; request.priority = 300;
    request.settings = depthOfField_;
    const float k = std::clamp(settings_.depthOfField, 0.0f, 1.0f) * static_cast<float>(scale);
    request.settings.nearBlurRadius1080pPixels = (std::min)(32.0f, request.settings.nearBlurRadius1080pPixels * k);
    request.settings.farBlurRadius1080pPixels = (std::min)(32.0f, request.settings.farBlurRadius1080pPixels * k);
    request.settings.enabled = k > 0 ? 1u : 0u;
    (void)view.SubmitDepthOfField(request);
}

void IntroDirector::DriveLetterbox(HIKARI::WorldView& world, double t) const {
    if (!(settings_.letterbox > 0)) { return; }
    // 黒帯の範囲の始めから 1 秒で入り、終わりまでの 0.8 秒で退く（なめらかに）。
    const double amount = Math::SmoothStep((t - screens_.letterboxIn) / 1.0) * (1 - Math::SmoothStep((t - (screens_.letterboxOut - .8)) / .8));
    if (amount <= 0) { return; }
    const auto [width, height] = UiSize(world);
    // 帯の太さは 16:9 の画面で letterbox の比になる割合に揃える（編集器の縦長なゲームビューでも帯が太くなりすぎない）。
    const double bar = height * (std::max)(0.0, 1.0 - 16.0 / 9.0 / settings_.letterbox) * .5 * amount;
    if (bar < .5) { return; }
    HIKARI::GameProjectUiSpriteRequest request{};
    request.sourceId = 0x7D2148u; request.order = 100000;
    request.referenceWidth = width; request.referenceHeight = height;
    request.width = width; request.height = static_cast<float>(bar + 1);
    const auto ui = world.GetUi();
    request.y = -1;
    (void)ui.SubmitSprite(kLetterboxTexture, request);
    request.y = static_cast<float>(height - bar);
    (void)ui.SubmitSprite(kLetterboxTexture, request);
}

void IntroDirector::WarmUi(HIKARI::WorldView& world) const {
    HIKARI::GameProjectUiSpriteRequest request{};
    request.sourceId = 0x7D214Cu; request.order = -100000;
    request.referenceWidth = 1920; request.referenceHeight = 1080;
    request.width = 1; request.height = 1;
    request.tint = {1, 1, 1, .004f};
    const auto ui = world.GetUi();
    for (const auto texture : {kLetterboxTexture, kWhiteTexture}) { (void)ui.SubmitSprite(texture, request); ++request.sourceId; }
}

double IntroDirector::EndingLamp(const Lamp& lamp, double t) const {
    // 通関の灯は暗転で全部消える（目覚めはタイトル待ちの世界で、灯は元に戻っている）。
    return t >= screens_.blackTime ? 0.0 : lamp.base;
}

void IntroDirector::DriveEndingBlack(HIKARI::WorldView& world, double t) const {
    // 暗転の始まりから 0.5 秒で黒くなり、そのまま（目覚めはタイトル待ちで描く）。
    if (t < screens_.blackTime) { return; }
    DrawLids(world, 1 - Math::SmoothStep((t - screens_.blackTime) / .5));
}

void IntroDirector::DriveWake(HIKARI::WorldView& world, double u) {
    // まぶたの開き（0 閉じ、1 開いた）：0.8 秒後から薄く開き、一度閉じかけてから、ゆっくり開ききる。
    double open = 0;
    if (u >= .8) { open = .35 * Math::SmoothStep((u - .8) / .7); }
    if (u >= 1.7) { open = .35 - .3 * Math::SmoothStep((u - 1.7) / .25); }
    if (u >= 1.95) { open = .05 + .95 * Math::SmoothStep((u - 1.95) / 1.4); }
    DrawLids(world, open);
    // 机に伏せた顔を上げる：目と注視点を下げた所から、タイトルの構図へなめらかに戻す。
    const auto w = static_cast<float>(1 - Math::SmoothStep((u - 1.0) / 2.6));
    if (w > 0) {
        HIKARI::GameProjectCameraModifier modifier{};
        modifier.sourceId = 0x7D214Au; modifier.priority = 60; modifier.applyDuringOverride = 1;
        modifier.worldPositionOffset = {0, -.11f * w, .07f * w};
        modifier.worldTargetOffset = {0, -.62f * w, -.08f * w};
        (void)world.GetCamera().SubmitModifier(modifier);
    }
    // ぼけ：強くぼかした所からオープニングの弱さへ（ピントが合っていく）。
    const double focus = std::clamp((u - .8) / 3.2, 0.0, 1.0), k = (std::max)(.05f, settings_.depthOfField);
    DriveDepthOfField(world, 1 + (1 / k - 1) * (1 - Math::SmoothStep(focus)));
}

void IntroDirector::DriveTitleZoom(HIKARI::WorldView& world, double weight) const {
    if (weight <= 0) { return; }
    // 画角だけを狭める（目の位置は作者のタイトルの構図のまま。パソコンの画面はほぼ画の真ん中にある）。
    HIKARI::GameProjectCameraModifier modifier{};
    modifier.sourceId = 0x7D214Eu; modifier.priority = 60; modifier.applyDuringOverride = 1;
    modifier.verticalFovOffsetRadians = static_cast<float>(-kTitleZoomDegrees * weight * 3.14159265358979 / 180);
    (void)world.GetCamera().SubmitModifier(modifier);
}

void IntroDirector::DriveTitleFocus(HIKARI::WorldView& world, double weight) const {
    if (weight <= 0 || (screenCenter_.x == 0 && screenCenter_.y == 0 && screenCenter_.z == 0)) { return; }
    // ぼけの量はオープニングの設定のまま、焦点の距離だけを画面の中心の奥行きにする（ぼけの差し替え 300 より強く）。
    HIKARI::GameProjectCameraFocusRequest focus{};
    focus.cameraObjectHandle = world.FindSceneObject(settings_.cameraObject).GetHandle().value;
    focus.sourceId = 0x7D2146u; focus.priority = 400;
    focus.mode = HIKARI::GameProjectCameraFocusMode::WorldPoint;
    focus.weight = static_cast<float>((std::min)(1.0, weight));
    focus.worldPoint = screenCenter_;
    (void)world.GetCamera().SubmitFocus(focus);
}

void IntroDirector::DriveBlack(HIKARI::WorldView& world, double t) {
    // 1 は作者の色調整のまま、0 は真っ黒。演出の暗転と同じく飛び込む直前の 1 フレームで切り、遠景に替わってから 0.4 秒で戻す。
    const double in = std::clamp((t - (screens_.blackTime - .015)) / .015, 0.0, 1.0);
    const double out = std::clamp((t - screens_.revealTime) / .4, 0.0, 1.0);
    const auto k = static_cast<float>(1 - in * (1 - out));
    if (std::abs(k - blackSent_) < .01f) { return; }
    const auto environment = world.GetEnvironment();
    if (!gradingSaved_) {
        grading_ = {};
        if (!environment.GetColorGrading(grading_).Accepted()) { return; }
        gradingSaved_ = true;
    }
    if (k >= .999f) {
        if (gradingOverridden_) { (void)environment.ClearColorGradingOverride(); gradingOverridden_ = false; }
        blackSent_ = 1;
        return;
    }
    auto grading = grading_;
    for (auto* tint : {&grading.shadowsTint, &grading.midtonesTint, &grading.highlightsTint}) {
        tint->x *= k; tint->y *= k; tint->z *= k;
    }
    const auto result = environment.SetColorGradingOverride(grading);
    if (result.Accepted()) { gradingOverridden_ = true; blackSent_ = k; }
}

void IntroDirector::DriveLights(HIKARI::WorldView& world, double t) {
    const auto& s = screens_;
    for (auto& lamp : lamps_) {
        const auto object = world.FindObject(lamp.handle);
        if (!object.IsValid()) { continue; }
        double intensity = lamp.base;
        bool enabled = true;
        if (lamp.lightning) {
            // 稲妻：一回ごとに強い閃きと、少し遅れた弱い閃き。灯ごとに強さを変え、二つの窓が別々に光る。
            double env = 0;
            for (size_t i = 0; i < s.lightning.size(); ++i) {
                const double dt = t - s.lightning[i];
                if (dt < 0 || dt > .6) { continue; }
                const double strength = .55 + .45 * Hash01(lamp.seed, static_cast<uint32_t>(i));
                env += strength * std::exp(-dt / .07);
                if (dt > .11) { env += .45 * strength * std::exp(-(dt - .11) / .05); }
            }
            intensity = lamp.base * (std::min)(env, 1.0);
            enabled = intensity > lamp.base * .002;
        } else if (ending_) {
            intensity = EndingLamp(lamp, t);
        } else if (lamp.glow && t >= s.blackTime) {
            const double on = s.powerOn + .1, locked = s.lockStart + s.lockSeconds;
            intensity = lamp.base * (t < on ? 0 : t < locked ? .5 + .3 * Hash01(lamp.seed, static_cast<uint32_t>(t * 12)) : 1);
        } else if (lamp.glow && t >= s.titleOff && t < s.restoreTime) {
            // パソコンの画面の光：乱れて消え、最初の稲妻で砂嵐と一緒に点き、吸い込まれる時は画面と一緒に強くなる。
            const auto tick = static_cast<uint32_t>(std::floor(t * 12));
            double v = 0;
            if (t < s.titleOff + .3) { v = Hash01(lamp.seed, tick) < .5 ? 1.6 : 0; }
            else if (t < s.staticStart) { v = IntroOmenBurst(t, s.flickerStart, s.staticStart) ? .9 : 0; }
            else if (t >= s.whiteStart) { v = 2.5 + 2 * std::clamp((t - s.suckStart) / (s.blackTime - s.suckStart), 0.0, 1.0); }
            else if (t >= s.blackoutStart) { v = 0; }
            else if (t >= s.staticStart) { v = .5 + .4 * Hash01(lamp.seed, tick + 7u); }
            for (const float flash : s.lightning) { if (t >= flash && t < flash + .12 && t >= s.staticStart) { v = 1.5; } }
            intensity = lamp.base * v;
        } else if (!lamp.glow && t >= s.flickerStart) {
            // 工場の灯の明滅。最初の稲妻までは前兆（時々ちらつき、一瞬落ちる）、稲妻の後は非常灯のように暗いまま
            // ちらつき続け、一部は切れて戻らない。
            const double ramp = std::clamp((t - s.flickerStart) / 1.2, 0.0, 1.0) * std::clamp(double(settings_.flicker), 0.0, 1.0);
            // 工場の灯は同じ電源につながっているので、全部が一緒に揺れる。灯ごとの揺れは少しだけ足す。
            const double power = Noise(0xF11Cu, t, 9), surge = Noise(0xF11Du, t, 2.2), own = Noise(lamp.seed, t, 21);
            const double first = s.lightning.empty() ? s.staticStart : s.lightning.front();
            // 露出が夜向き（明るめ）なので、消える時は 0 まで落とさないと消えて見えない。
            double v = 1;
            if (t < first) {
                // 前兆：初めはまれに揺れる程度で、稲妻が近づくほど落ちる回数も長さも増える。電気が乱れる瞬間は全部落ちる。
                const double p = std::clamp((t - s.flickerStart) / (std::max)(.1, first - s.flickerStart), 0.0, 1.0);
                if (power < .1 + .32 * p) { v = .1; }
                if (surge < .04 + .2 * p * p) { v = 0; }
                if (IntroOmenBurst(t, s.flickerStart, first)) { v = (std::min)(v, .03); }
            } else {
                v = power < .35 ? .02 : .12;
                if (surge < .25) { v = 0; }
                if (Hash01(lamp.seed, 99) < .35 && t > first + .4 + Hash01(lamp.seed, 98) * 6) { v = 0; }
            }
            if (own < .15) { v *= .4; }
            // 稲妻の直前と最中は全部落とし、窓から差す光だけにする。
            for (const float flash : s.lightning) { if (t > flash - .25 && t < flash + .3) { v = 0; } }
            // 全消灯：全部の灯が一度に切れる。白い画面の間は、画面に照らされたように弱く灯り、近くのテレビと同じ拍で消える。
            if (t >= s.blackoutStart) { v = t >= s.whiteStart && t < lamp.off ? .22 : 0; }
            // 電気が戻る：手元の灯から遠くへ順に、何度か頼りなく瞬いてから点く。
            if (t >= s.blackTime) {
                const double on = s.powerOn + std::pow(lamp.reach, .8) * s.powerSeconds + Hash01(lamp.seed, 21) * .25;
                v = t < on ? 0 : t < on + .6 ? (Hash01(lamp.seed, static_cast<uint32_t>((t - on) * 14)) < .45 ? .8 : 0) : 1;
            }
            intensity = lamp.base * (1 + (v - 1) * ramp);
        }
        HIKARI::GameProjectLightPatch patch{};
        if (lamp.lightning && enabled != lamp.enabled) {
            patch.fieldMask |= HIKARI::GameProjectLightPatchFields(HIKARI::GameProjectLightPatchField::Enabled);
            patch.enabled = enabled ? 1u : 0u;
        }
        if (std::abs(intensity - lamp.sent) > lamp.base * .01) {
            patch.fieldMask |= HIKARI::GameProjectLightPatchFields(HIKARI::GameProjectLightPatchField::Intensity);
            patch.intensity = static_cast<float>(intensity);
        }
        if (patch.fieldMask == 0) { continue; }
        if (object.GetLight().ApplyPatch(patch).Accepted()) {
            if (patch.fieldMask & HIKARI::GameProjectLightPatchFields(HIKARI::GameProjectLightPatchField::Intensity)) { lamp.sent = static_cast<float>(intensity); }
            if (lamp.lightning) { lamp.enabled = enabled; }
        }
    }
}

void IntroDirector::DriveProps(HIKARI::WorldView& world, double t) {
    if (props_.empty()) { return; }
    if (t >= screens_.restoreTime) { RestoreProps(world); return; }
    const double shake = screens_.pullStart - .9;
    // 震える前は作者の位置（プレビューで巻き戻した時も元の位置に戻る）。
    if (t < shake) { RestoreProps(world); return; }
    for (size_t i = 0; i < props_.size(); ++i) {
        auto& prop = props_[i];
        const auto object = world.FindObject(prop.handle);
        if (!object.IsValid()) { continue; }
        // 飛び立つ時刻は小物ごとにずらし、順に吸われていく。
        const double launch = screens_.pullStart + .08 + i * .13, flight = .5 + .15 * Hash01(prop.seed, 3);
        auto pose = prop.rest;
        if (t < launch) {
            // 震え：引かれ始めが近づくほど大きく、少し浮く。
            const double k = std::clamp((t - shake) / (launch - shake), 0.0, 1.0);
            const auto tick = static_cast<uint32_t>(t * 30);
            const float jitter = static_cast<float>(.002 + .006 * k);
            pose.position = {pose.position.x + jitter * float(Hash01(prop.seed, tick) - .5) * 2,
                             pose.position.y + static_cast<float>(.012 * k * k) + jitter * float(Hash01(prop.seed, tick + 1)),
                             pose.position.z + jitter * float(Hash01(prop.seed, tick + 2) - .5) * 2};
            pose.rotation = QMul(QFromEulerDeg(float((Hash01(prop.seed, tick + 3) - .5) * 8 * k), 0, float((Hash01(prop.seed, tick + 4) - .5) * 8 * k)), pose.rotation);
        } else {
            // 吸い込み：加速しながら弧を描いて画面の中心へ。回りながら縮み、画面に着くと消える。
            const double u = std::clamp((t - launch) / flight, 0.0, 1.0), e = u * u * u;
            const GameProjectFloat3 target{screenCenter_.x + float((Hash01(prop.seed, 5) - .5) * .12), screenCenter_.y + float((Hash01(prop.seed, 6) - .5) * .08), screenCenter_.z};
            pose.position = Lerp(prop.rest.position, target, static_cast<float>(e));
            pose.position.y += static_cast<float>(.07 * std::sin(u * Math::kPi));
            const float spin = static_cast<float>(e * (300 + 400 * Hash01(prop.seed, 7)));
            pose.rotation = QMul(QFromEulerDeg(spin * .6f, spin, spin * .3f), prop.rest.rotation);
            const float size = u >= 1 ? .001f : static_cast<float>(1 - .8 * e * e);
            pose.scale = {prop.rest.scale.x * size, prop.rest.scale.y * size, prop.rest.scale.z * size};
        }
        (void)object.SetWorldTransform(pose);
        propsMoved_ = true;
    }
}

void IntroDirector::RestoreProps(HIKARI::WorldView& world) {
    if (!propsMoved_) { return; }
    propsMoved_ = false;
    for (const auto& prop : props_) {
        if (const auto object = world.FindObject(prop.handle); object.IsValid()) { (void)object.SetWorldTransform(prop.rest); }
    }
}
}
