#include "TD2Screen3D.h"
#include "TD2Screen3DShared.h"
#include "TD2StageProjection.h"
#include "TD2ScreenKitAssets.h"
#include "HIKARI/Project/ActionViews.h"
#include "HIKARI/Project/PresentationViews.h"
#include "HIKARI/Project/VfxView.h"
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <set>

namespace TD2 {
namespace {
using namespace Stage3D;
// オープニングの登場：エフェクトが始まってから体が現れ始めるまでと、足元から全身になるまで（秒）。
constexpr double kHeroFormDelay = .35, kHeroFormSeconds = .45;
// 演出の長さ（秒）。揺れと閃光は二乗で減衰する。
constexpr double kShakeSeconds = 0.28, kFlashSeconds = 0.22;

bool SameStyle(const ScreenStyleSettings& a, const ScreenStyleSettings& b) {
    // 明るさは保存の丸めで僅かに違うことがあるので、誤差の範囲は同じ組にする（組が増えるとキャプチャが足りなくなる）。
    return a.mode == b.mode && a.outline == b.outline && a.shadow == b.shadow && a.sketchDepth == b.sketchDepth &&
           a.sketchFill == b.sketchFill && std::abs(a.brightness - b.brightness) < 1e-3f && a.atmosphere == b.atmosphere &&
           a.occlusion == b.occlusion && a.castLight == b.castLight && a.inkLine == b.inkLine;
}
// 区域の画面の明るさ（既定 2.2）を基準 1 として描き方の明るさに掛ける。
ScreenStyleSettings Effective(const StageRoom& room) {
    auto style = room.style;
    style.brightness = std::clamp(style.brightness * room.configuration.brightness / 2.2f, .0f, 4.0f);
    return style;
}
HIKARI::GameProjectFloat3 Srgb(uint32_t hex) {
    return {((hex >> 16) & 255) / 255.f, ((hex >> 8) & 255) / 255.f, (hex & 255) / 255.f};
}
// 画面専用の描き方。色は ScreenKit07 の配色と同じ値。長さはゲーム画素で、
// 1 ゲーム画素の texel 数（pixelSize）は窓の精細度から決まる。
// cell は舞台の 1 マスの長さ、depth は立体（深度の効果を使う）、focus はカメラから正面の面までの距離、
// solid は実体の地形（塊の角を深度の線で描く）。
HIKARI::GameProjectSceneCaptureStyleDesc StyleDesc(const ScreenStyleSettings& style, float brightness, int density,
                                                   double cell, bool depth, double focus, bool solid) {
    HIKARI::GameProjectSceneCaptureStyleDesc fx{};
    const auto mode = static_cast<ScreenStyleMode>(style.mode);
    fx.style = mode == ScreenStyleMode::Original ? HIKARI::GameProjectSceneCaptureStyle::Clean
             : mode == ScreenStyleMode::Sketch   ? HIKARI::GameProjectSceneCaptureStyle::Sketch
                                                 : HIKARI::GameProjectSceneCaptureStyle::Pixel;
    fx.pixelSize = (std::max)(1.0f, static_cast<float>(density) / kGamePixelsPerCell);
    // 素描の線の太さはゲーム画素で 1（鮮明は 2 texel、周りは 1 texel）。窓の密度が違っても線の太さをそろえる。
    fx.lineWidth = mode == ScreenStyleMode::Sketch ? 2.0f : std::clamp(style.outline, 0.0f, 4.0f);
    // 素描では shadowStrength が奥行きの強さ。0 で平面的。
    fx.shadowStrength = std::clamp(mode == ScreenStyleMode::Sketch ? style.sketchDepth : style.shadow, 0.0f, 1.0f);
    // 光は左上・手前から。足場から右へ半マス、下へ 1 マスの影が奥の壁に落ちる。
    fx.shadowOffsetX = kGamePixelsPerCell * 0.5f;
    fx.shadowOffsetY = kGamePixelsPerCell * 1.0f;
    fx.backgroundLuminance = 0.035f;
    fx.brightness = std::clamp(brightness, 0.0f, 4.0f);
    // 素描で物体の焼いた色を紙に乗せる濃さ（ほかの描き方では使われない）。
    fx.fillStrength = std::clamp(style.sketchFill, 0.0f, 1.0f);
    // 紙は青瓷の壁より一段明るい暗色、鉛筆はわずかに緑の白。
    fx.lineColor = mode == ScreenStyleMode::Sketch ? Srgb(0xDDEAE4) : Srgb(0x061112);
    fx.paperColor = Srgb(0x0E2425);
    if (depth) {
        // 立体の空間感：舞台カメラからの奥行きで前景（地形・装置・人物、奥行き 0.7 マスまで）と奥（後ろの壁 0.85 と
        // 窓の向こうの大広間）を分ける。奥は輪郭を付けず霧で沈め、物の周りの暗がりと、物の高さに応じた影を足す。
        const auto c = static_cast<float>(cell);
        fx.depthFocus = static_cast<float>(focus);
        fx.backgroundDepth = .78f * c;
        fx.fogStrength = std::clamp(style.atmosphere, 0.0f, 1.0f);
        fx.fogDepth = 5.0f * c;
        // 霧は壁より明るい靄：奥の壁ほど明るく霞み、手前の柱と梁が暗い影絵として前に出る。
        fx.fogColor = mode == ScreenStyleMode::Sketch ? Srgb(0x2A4442) : Srgb(0x2C403F);
        fx.occlusion = std::clamp(style.occlusion, 0.0f, 1.0f);
        fx.occlusionRadius = 3.0f;   // 物のすぐ周りだけ（大きいと壁の染みに見える）
        fx.occlusionDepth = .8f * c;
        // 深度の線は原版の外形の墨線。実体の地形では目地の無い塊の角がこれで読めるので、どの描き方でも引く。
        fx.depthLine = mode == ScreenStyleMode::Original || solid ? std::clamp(style.inkLine, 0.0f, 1.0f) : 0.0f;
        fx.depthLineGap = .15f * c;
        fx.lightShadow = std::clamp(style.castLight, 0.0f, 1.0f);
        // 影の光線：shadowOffset（右へ半マス・下へ 1 マス）進む間に、後ろの壁までの奥行き（0.85 マス）だけ手前へ上がる。
        fx.lightSlope = .85f * c / std::hypot(fx.shadowOffsetX, fx.shadowOffsetY);
        fx.lightReach = 24.0f;
    }
    // 残影の菫色。この色相の鮮やかな物体を残影として横縞で描く（プレイヤーの琥珀・危険の赤とは色相で分かれる）。
    fx.accentColor = Srgb(0xA879F0);
    return fx;
}
// イベントの種類 → エフェクトのスロット。
ScreenFxSlot SlotOf(ScreenEffectKind kind) {
    switch (kind) {
    case ScreenEffectKind::BlinkTrail: return ScreenFxSlot::Dash;
    case ScreenEffectKind::Land: return ScreenFxSlot::Land;
    case ScreenEffectKind::Bounce: return ScreenFxSlot::Bounce;
    case ScreenEffectKind::EchoCreate: return ScreenFxSlot::EchoForm;
    case ScreenEffectKind::EchoRecall: return ScreenFxSlot::EchoRecall;
    case ScreenEffectKind::EchoBlocked: return ScreenFxSlot::EchoBlocked;
    case ScreenEffectKind::Death: return ScreenFxSlot::Death;
    case ScreenEffectKind::Respawn: return ScreenFxSlot::Respawn;
    case ScreenEffectKind::DeviceSwitch: return ScreenFxSlot::Device;
    case ScreenEffectKind::Checkpoint: return ScreenFxSlot::Checkpoint;
    case ScreenEffectKind::EchoAbsorb: return ScreenFxSlot::EchoAbsorb;
    case ScreenEffectKind::Arrive: return ScreenFxSlot::Arrive;
    default: return ScreenFxSlot::Impact;
    }
}
// 初回再生の粒子配置は従来の seed を保つ。座標は再生済みの判定には使わない。
uint32_t FxRandomSeed(const ScreenEffect& e) {
    const auto& r = e.kind == ScreenEffectKind::BlinkTrail ? e.source : e.rect;
    const auto seed = std::to_string(static_cast<int>(e.kind)) + "/" + std::to_string(e.tick) + "/" +
           std::to_string(static_cast<long long>(std::lround(r.x * 64))) + "/" + std::to_string(static_cast<long long>(std::lround(r.y * 64)));
    return static_cast<uint32_t>(std::hash<std::string>{}(seed));
}
// エフェクトの置き方（マス単位の資産に舞台の 1 マスの大きさを掛ける）。着地・復活・チェックポイントは足元、ほかは体や装置の中心。
// 瞬間移動は出発点の体の中心から終点へ向け、X だけ実際の距離まで伸ばす。e は世界のマス。
HIKARI::GameProjectTransform FxTransform(const ScreenEffect& e, const StageProjection& projection, double scale) {
    const auto& r = e.rect;
    const bool feet = e.kind == ScreenEffectKind::Land || e.kind == ScreenEffectKind::Respawn || e.kind == ScreenEffectKind::Checkpoint ||
                      e.kind == ScreenEffectKind::Arrive;
    HIKARI::GameProjectTransform transform{};
    transform.position = projection.Point(r.x + r.w * .5, feet ? r.y + r.h : r.y + r.h * .5, projection.FxDepth());
    const float size = static_cast<float>(projection.scale * scale);
    transform.scale = {size, size, size};
    if (e.kind == ScreenEffectKind::BlinkTrail) {
        const double sx = e.source.x + e.source.w * .5, sy = e.source.y + e.source.h * .5;
        const double dx = r.x + r.w * .5 - sx, dy = r.y + r.h * .5 - sy;
        transform.position = projection.Point(sx, sy, projection.FxDepth());
        transform.rotation = RotateZ(std::atan2(-dy, dx));
        transform.scale.x = static_cast<float>(projection.scale * (std::max)(std::sqrt(dx * dx + dy * dy), .05));
    }
    return transform;
}
}

void Screen3DStage::RestoreCamera(HIKARI::WorldView& world, StageCamera& camera) {
    if (camera.handle.IsValid()) {
        if (const auto object = world.FindObject(camera.handle); object.IsValid()) { (void)object.SetWorldTransform(camera.original); }
    }
    camera = {};
}

bool Screen3DStage::Release(HIKARI::WorldView& world) {
    worldEffects_.Reset();
    const auto vfx = world.GetVfx();
    for (const auto& active : activeFx_) { if (active.handle.IsValid()) { (void)vfx.Stop(active.handle); } }
    activeFx_.clear();
    const auto models = world.GetPresentationModels();
    for (const auto& [owner, id] : submitted_) { models.Remove(owner, id); }
    submitted_.clear(); roomParts_.clear(); bound_.clear(); deviceMemory_.clear();
    const auto captures = world.GetSceneCapture();
    for (auto it = views_.begin(); it != views_.end();) {
        if (!captures.Destroy(it->handle)) { status_ = "3D捕获释放失败，保留句柄重试"; return false; }
        it = views_.erase(it);
    }
    for (auto& camera : cameras_) { RestoreCamera(world, camera); }
    for (auto& camera : lensCameras_) { RestoreCamera(world, camera); }
    ReleaseStageLights(world);
    windows_ = {};
    return true;
}

// 編集中の二枚の窓は別々の正投影カメラで撮る（同じ物体だと姿勢を奪い合う）。鮮明は必須（舞台の縮尺を決める）、二台目は無ければ一枚に収める。
bool Screen3DStage::AcquireCameras(HIKARI::WorldView& world, const ScreenStyleSettings& stage) {
    const uint64_t ids[kOrthoKinds]{stage.captureCameraObject, stage.unplacedCameraObject};
    for (size_t kind = 0; kind < kOrthoKinds; ++kind) {
        auto& camera = cameras_[kind];
        const auto object = ids[kind] ? world.FindSceneObject(ids[kind]) : HIKARI::ObjectView{};
        HIKARI::GameProjectCameraLens lens{};
        bool usable = object.IsValid() && world.GetCamera().GetAuthoredLens(object.GetHandle().value, lens) &&
                      lens.projection == HIKARI::GameProjectCameraProjectionKind::Orthographic && lens.orthographicHeight > 0 &&
                      lens.nearClip < kStageCameraDistance - 1 && lens.farClip > kStageCameraDistance + 1;
        for (size_t other = 0; other < kind; ++other) { usable = usable && cameras_[other].sceneId != ids[kind]; }
        if (!usable) {
            RestoreCamera(world, camera);
            if (kind == static_cast<size_t>(ViewKind::Sharp)) { Release(world); status_ = "请指定独立正交舞台镜头（farClip>31）"; return false; }
            continue;
        }
        if (camera.sceneId != ids[kind] || camera.handle != object.GetHandle()) {
            RestoreCamera(world, camera);
            if (!object.GetWorldTransform(camera.original)) { status_ = "无法读取舞台镜头"; return false; }
            camera.sceneId = ids[kind]; camera.handle = object.GetHandle();
        }
        camera.orthographicHeight = lens.orthographicHeight;
    }
    return true;
}

bool Screen3DStage::AcquireLensCameras(HIKARI::WorldView& world, const ScreenStyleSettings& stage) {
    const uint64_t ids[2]{stage.perspectiveCameraObject, stage.perspectiveCamera2Object};
    bool any = false;
    for (size_t k = 0; k < 2; ++k) {
        auto& camera = lensCameras_[k];
        const auto object = ids[k] ? world.FindSceneObject(ids[k]) : HIKARI::ObjectView{};
        HIKARI::GameProjectCameraLens lens{};
        const bool usable = object.IsValid() && (k == 0 || ids[k] != ids[0]) && world.GetCamera().GetAuthoredLens(object.GetHandle().value, lens) &&
                            lens.projection == HIKARI::GameProjectCameraProjectionKind::Perspective &&
                            lens.verticalFovRadians > .05f && lens.verticalFovRadians < 2.5f;
        if (!usable) { RestoreCamera(world, camera); continue; }
        if (camera.sceneId != ids[k] || camera.handle != object.GetHandle()) {
            RestoreCamera(world, camera);
            if (!object.GetWorldTransform(camera.original)) { continue; }
            camera.sceneId = ids[k]; camera.handle = object.GetHandle();
        }
        camera.verticalFov = lens.verticalFovRadians;
        any = true;
    }
    return any;
}

bool Screen3DStage::AcquireFarCameras(HIKARI::WorldView& world, const ScreenStyleSettings& stage) {
    const uint64_t ids[kFarKinds.size()]{stage.farCameraObject, stage.farCamera2Object};
    for (size_t k = 0; k < kFarKinds.size(); ++k) {
        auto& camera = lensCameras_[static_cast<size_t>(kFarKinds[k])];
        const uint64_t id = ids[k];
        const auto object = id ? world.FindSceneObject(id) : HIKARI::ObjectView{};
        HIKARI::GameProjectCameraLens lens{};
        // 鮮明・大区域の透視カメラやもう一組と同じ物体は使えない（姿勢を奪い合う）。二組目は一組目がある時だけ。
        const bool usable = (k == 0 || lensCameras_[static_cast<size_t>(kFarKinds[0])].handle.IsValid()) && object.IsValid() &&
                            id != stage.perspectiveCameraObject && id != stage.perspectiveCamera2Object && (k == 0 || id != ids[0]) &&
                            world.GetCamera().GetAuthoredLens(object.GetHandle().value, lens) &&
                            lens.projection == HIKARI::GameProjectCameraProjectionKind::Perspective &&
                            lens.verticalFovRadians > .02f && lens.verticalFovRadians < 1.2f;
        if (!usable) { RestoreCamera(world, camera); continue; }
        if (camera.sceneId != id || camera.handle != object.GetHandle()) {
            RestoreCamera(world, camera);
            if (!object.GetWorldTransform(camera.original)) { continue; }
            camera.sceneId = id; camera.handle = object.GetHandle();
        }
        camera.verticalFov = lens.verticalFovRadians;
    }
    return lensCameras_[static_cast<size_t>(ViewKind::Far)].handle.IsValid();
}

void Screen3DStage::ApplyIntro(HIKARI::WorldView& world, std::map<uint64_t, Binding>& next) const {
    if (!introStatic_) { return; }
    const auto view = std::find_if(views_.begin(), views_.end(), [](const View& v) { return v.kind == ViewKind::Far; });
    if (view == views_.end() || !view->handle.IsValid()) { return; }
    const double t = intro_->time;
    const auto tick = static_cast<uint32_t>(std::floor(t * kNoiseFlips));
    // 段：0 黒、1 暗い砂嵐、2 砂嵐、3 明るい砂嵐。窓を横に 4 等分した範囲が一段（板は段ごとに別の明るさ）。
    // テレビごとに段の中の位置を変え、隣り合うテレビが同じ絵にならないようにする。
    // 4 は消える瞬間のブラウン管の線：黒い段の真ん中の細い線を、squash（0〜1）が進むほど広い範囲で写して細くする。
    // 5 は真っ白（黒い段の下の方の白い板）、6 はタイトル（2・3 段目に置いた仮タイトルの板）、
    // 7 は線のあとの光の点（黒い段の上の小さな白い点を、squash が進むほど広く写して小さくする）。
    struct Look { int band = -1; double squash = 0; };
    const auto bind = [&](HIKARI::GameProjectObjectHandle object, uint64_t key, Look look, const std::string& region) {
        double x = (look.band >= 4 ? 0 : look.band) * .25 + .015 + IntroHash(key, 1) * .02;
        double y = IntroHash(key, 2) * .4, w = .2, h = .55;
        if (look.band == 0) { y = IntroHash(key, 2) * .05; h = .15; }   // 黒：点と線にかからない一番上
        if (look.band == 4) { h = kNoiseLine + (.95 - kNoiseLine) * std::pow(std::clamp(look.squash, 0.0, 1.0), .6); y = .5 - h * .5; }
        if (look.band == 5) { y = .64 + IntroHash(key, 2) * .03; h = .3; }
        if (look.band == 6) { x = .25; y = 0; w = .5; h = 1; }
        if (look.band == 7) {
            const double k = std::clamp(look.squash, 0.0, 1.0);
            w = .04 + .16 * k; h = .04 + .16 * k;
            x = .125 - w * .5; y = kNoiseDot - h * .5;
        }
        next[object.value] = {object, view->handle, "TD2_Screen",
                              {static_cast<float>(x), static_cast<float>(y), static_cast<float>(w), static_cast<float>(h)}, false, region};
    };
    // 砂嵐の始まり：パソコンに近いテレビほど早く、最初の稲妻の瞬間に机の周りから工場の奥へ広がる。
    if (introOnset_.empty()) {
        HIKARI::GameProjectTransform origin{};
        (void)world.FindSceneObject(intro_->computerScreen).GetWorldTransform(origin);
        const auto add = [&](uint64_t key, HIKARI::ObjectView object) {
            HIKARI::GameProjectTransform tv{};
            double d = 12;
            if (object.IsValid() && object.GetWorldTransform(tv)) {
                d = std::hypot(tv.position.x - origin.position.x, tv.position.y - origin.position.y, tv.position.z - origin.position.z);
            }
            const double spread = (std::max)(0.0f, intro_->staticFull - intro_->staticStart);
            const double reach = std::pow(std::clamp((d - .4) / 24.0, 0.0, 1.0), .7);
            introOnset_[key] = {intro_->staticStart + (std::max)(0.0, spread * reach + (IntroHash(key, 5) - .5) * .6), reach};
        };
        for (const auto* room : introRooms_) { add(room->configuration.screenObject, world.FindSceneObject(room->configuration.screenObject)); }
        for (const auto& handle : intro_->tvScreens) { add(handle.value, world.FindObject(handle)); }
    }
    // テレビごとの砂嵐の始まりとパソコンからの遠さ（0〜1）。
    const auto infoOf = [&](uint64_t key) {
        const auto found = introOnset_.find(key);
        return found != introOnset_.end() ? found->second : std::array<double, 2>{intro_->staticFull, 1.0};
    };
    // 稲妻の瞬間は砂嵐が一斉に明るくなる（電気が走った感じ）。前兆の電気の乱れでは手元のテレビとパソコンが一瞬砂嵐になる。
    const bool flash = std::any_of(intro_->lightning.begin(), intro_->lightning.end(), [&](float at) { return t >= at && t < at + .12; });
    const double first = intro_->lightning.empty() ? intro_->staticStart : intro_->lightning.front();
    const bool omen = IntroOmenBurst(t, intro_->flickerStart, first);
    // パソコン：タイトルの間は普段の画面、スペースの瞬間に乱れて消え、最初の稲妻まで黒い。吸い込まれる時は明るい砂嵐で光る。
    // ほかのテレビ：テレビごとの時刻から砂嵐になり、段を毎秒 kNoiseFlips 回入れ替えてちらつく。-1 は普段の画面のまま。
    const auto noise = [&](uint64_t key) {
        const double r = IntroHash(key, tick * 13u + 1u);
        return Look{r < .12 ? 0 : r < .42 ? 1 : r < .85 ? 2 : 3};
    };
    // 山場：全部が一度に消え、一斉に真っ白に点き、遠いテレビから一台ずつ線になって消える。始めはまばらで、
    // 手元へ近づくほど詰まる。最後はパソコンだけが白く残り、やがて砂嵐へ戻って吸い込みへ続く。
    const double alone = intro_->aloneEnd;
    const auto bandFor = [&](uint64_t key, bool computer, uint64_t handle) {
        if (t >= intro_->restoreTime) {
            // 暗転の間（カメラは画面の中）、パソコンは普段の画面に戻しておく（砂嵐の窓を写していると暗転の黒が茶色く浮く）。
            if (t < intro_->revealTime) { return computer ? Look{} : Look{0}; }
            // 電気が戻る：手元から遠くへ順に、点 → 横線 → 砂嵐と点く。信号は遠くから合っていき、パソコンが最後。
            const double reach = computer ? 0.0 : infoOf(key)[1];
            const double on = intro_->powerOn + .15 + std::pow(reach, .8) * intro_->powerSeconds * .9 + IntroHash(key, 21) * .2;
            const double locked = intro_->lockStart + (1 - reach) * intro_->lockSeconds + IntroHash(key, 22) * .15;
            if (t < on) { return Look{0}; }
            if (t < on + .22) { return Look{7, 1 - (t - on) / .22}; }
            if (t < on + .44) { return Look{4, 1 - (t - on - .22) / .22}; }
            if (t < locked) { return noise(key); }
            return Look{};
        }
        if (computer) {
            if (t < intro_->titleOff) { return Look{6}; }
            if (t < intro_->titleOff + .3) { const double r = IntroHash(key, tick * 7u + 3u); return Look{r < .35 ? 6 : r < .7 ? 3 : 0}; }
            if (t < intro_->staticStart) { return Look{omen ? 2 + int(IntroHash(key, tick) < .5) : 0}; }
            if (t >= intro_->pullStart) { return Look{3}; }
            if (t >= alone) { return Look{IntroHash(key, tick) < .85 ? 3 : 2}; }
            // 見つめる間の終わりに白い画面がちらつき、砂嵐へ戻る。
            if (t >= alone - .3) { return Look{IntroHash(key, tick * 3u + 1u) < .5 ? 5 : 3}; }
            if (t >= intro_->whiteStart) { return Look{5}; }
            if (t >= intro_->blackoutStart) { return Look{0}; }
            return flash ? Look{3} : noise(key);
        }
        const auto info = infoOf(key);
        if (t < info[0]) { return omen && info[1] < .35 ? Look{1 + int(IntroHash(key, tick) < .5)} : Look{0}; }
        if (t >= intro_->whiteStart) {
            // 消える拍で、白い画面が横線へ潰れ、光の点になって消える（ブラウン管の電源を切った時）。
            const auto found = intro_->offTime.find(handle);
            const double off = found != intro_->offTime.end() ? found->second : intro_->suckStart + intro_->collapseSeconds;
            if (t >= off + .26) { return Look{0}; }
            if (t >= off + .12) { return Look{7, (t - (off + .12)) / .14}; }
            if (t >= off) { return Look{4, (t - off) / .12}; }
            return Look{5};
        }
        if (t >= intro_->blackoutStart) { return Look{0}; }
        return flash ? Look{3} : noise(key);
    };
    std::set<uint64_t> placed;
    for (const auto* room : introRooms_) {
        const auto id = room->configuration.screenObject;
        const auto screen = world.FindSceneObject(id);
        if (!room->placed || !screen.IsValid()) { continue; }
        placed.insert(screen.GetHandle().value);
        if (const auto look = bandFor(id, id == intro_->computerScreen, screen.GetHandle().value); look.band >= 0) { bind(screen.GetHandle(), id, look, room->regionId); }
    }
    // 装飾テレビと置いていない区域のテレビ：普段の画面が無いので、砂嵐になるまでは黒い段に結んでおく
    // （オープニングの始めに一度だけ結び、途中でシーンを作り直さない）。
    for (const auto& handle : intro_->tvScreens) {
        if (placed.contains(handle.value) || next.contains(handle.value)) { continue; }
        // 元に戻した後は結ばない（普段の材質へ戻る）。
        if (const auto look = bandFor(handle.value, false, handle.value); look.band >= 0) { bind(handle, handle.value, look, {}); }
    }
}

void Screen3DStage::Render(HIKARI::WorldView& world, const std::vector<StageRoom>& rooms, const StageFrame& frame,
                          const ScreenStyleSettings& stage, const ScreenFxSettings& fx) {
    const double now = frame.snapshot ? frame.snapshot->realTime : 0;
    const auto fade = [now](double start, double seconds) {
        const double age = now - start;
        return start < 0 || age < 0 || age >= seconds ? 0.0 : (1 - age / seconds) * (1 - age / seconds);
    };
    // 1. 描ける部屋と、窓の大きさの目安（世界に置いた部屋の最大のマス数）。
    std::vector<const StageRoom*> valid;
    int maxCols = 0, maxRows = 0;
    for (const auto& room : rooms) {
        if (!room.style.enabled || !ValidateRegion(room.Region()).valid) { continue; }
        valid.push_back(&room);
        if (room.placed) { maxCols = (std::max)(maxCols, room.Region().cols); maxRows = (std::max)(maxRows, room.Region().rows); }
    }
    if (valid.empty() || !frame.snapshot) { Release(world); status_ = "3D显示已关闭或没有有效区域"; return; }
    flippedRooms_.clear();
    for (const auto* room : valid) if (room->flipped) {
        flippedRooms_.push_back({room->regionId, {double(room->x), double(room->y), double(room->x + room->Region().cols), double(room->y + room->Region().rows)}});
    }
    if (!maxRows) for (const auto* room : valid) { maxCols = (std::max)(maxCols, room->Region().cols); maxRows = (std::max)(maxRows, room->Region().rows); }

    // 2. 舞台カメラと写し方。1 マスの舞台上の長さは、鮮明カメラの正投影の高さを 1 部屋分の行で割った値。
    if (!AcquireCameras(world, stage)) { return; }
    projection_.scale = cameras_[static_cast<size_t>(ViewKind::Sharp)].orthographicHeight / maxRows;
    // 奥行きの格子・透視の窓・受光は Play だけ（編集中は作者が見る元の形のまま）。
    // 透視の窓は正面の透視カメラで撮る。立体の組（斜投影を焼いた部品）なら右上から見た透視になり、
    // 受光と一緒なら斜めに切っていない実体の地形を真ん中から見る（左右の壁・床・天井の内側が見える）。
    depthRows_ = stage.depthGrid && !frame.authoring ? std::clamp(stage.depthRows, 1, 8) : 0;
    perspective_ = stage.perspective && !frame.authoring && AcquireLensCameras(world, stage);
    if (!perspective_) { for (const auto kind : {ViewKind::Sharp, ViewKind::Area}) { RestoreCamera(world, lensCameras_[static_cast<size_t>(kind)]); } }
    // 遠くのテレビの窓は Play だけ（編集中は全部の部屋を一枚に収める）。
    // 編集中でもオープニングのプレビューの間は砂嵐の窓に使う。
    if ((frame.authoring && !frame.intro) || !AcquireFarCameras(world, stage)) {
        for (const auto kind : kFarKinds) { RestoreCamera(world, lensCameras_[static_cast<size_t>(kind)]); }
    }
    // 受光は透視の窓と一緒の時だけ（受光の部品は実体の地形の組しか無い）。舞台の灯は別の切り替え（既定は切る）。
    solid_ = stage.lit && perspective_;
    lit_ = solid_ && stage.stageLights;
    actorScale_ = std::clamp(static_cast<double>(stage.actorScale), .8, 2.0);
    playDepth_ = solid_ ? std::clamp(static_cast<double>(stage.playDepth), 0.0, 4.0) : 0.0;
    // 立体：斜投影を焼いた部品の組を正対のカメラで撮る（天板と右の側面が見え、正面はマスにぴったり）。
    // 実体の地形は斜めに切らない受光の組を透視で撮る。
    projection_.oblique = solid_ ? 0.0 : kScreenKitOblique;
    projection_.pitch = 0.0; projection_.yaw = 0.0;
    const int kit = solid_ ? kScreenKitPlay : kScreenKitEdit;
    travel_ = {frame.travelFrom, frame.travelTo};
    ++frame_;
    // 編集中は Play の大区域の窓の目と部屋を忘れる（次の Play は操作中の部屋から始める）。
    if (frame.authoring) { seen_.clear(); areaEyeValid_ = false; areaMembers_.clear(); PlaceEditorWindow(valid); }
    else { Inspect(world, valid, frame); }
    // 視差：画面を見る角度の正接を 0.25 秒でなめらかに追う（見えていない画面は最後の角度のまま）。手持ちの揺れは残す。
    hall_ = stage.hall;
    parallaxGain_ = std::clamp(static_cast<double>(stage.parallax), 0.0, 2.0);
    {
        const double dt = lastParallaxTime_ < 0 ? 0.0 : std::clamp(now - lastParallaxTime_, 0.0, .1);
        lastParallaxTime_ = now;
        const double follow = dt > 0 ? 1 - std::exp(-dt / .25) : 1.0;
        parallaxFollow_ = follow;
        // プレイヤーのいる部屋は、目がプレイヤーの後を少し追う（部屋の中央からの位置に比例して正接を足す）。外のカメラが
        // 止まっていても、歩くと奥の大広間が窓に対してずれ、奥行きが見える。回された部屋は画面の上の位置で数える。
        std::string playerRoom;
        double followX = 0, followY = 0;
        if (frame.simulated && frame.snapshot->playerPresent) {
            const auto& body = frame.snapshot->presentationPlayer;
            const double px = body.x + body.w * .5, py = body.y + body.h * .5;
            for (const auto* room : valid) {
                const auto& r = room->Region();
                if (px < room->x || py < room->y || px >= room->x + r.cols || py >= room->y + r.rows) { continue; }
                constexpr double kFollowX = .15, kFollowY = .08;   // 部屋の端での正接
                const double ux = (px - (room->x + r.cols * .5)) / (r.cols * .5), uy = ((room->y + r.rows * .5) - py) / (r.rows * .5);
                playerRoom = room->regionId;
                followX = (room->flipped ? -ux : ux) * kFollowX; followY = (room->flipped ? -uy : uy) * kFollowY;
                break;
            }
        }
        for (const auto& [id, seen] : seen_) {
            if (seen.coverage <= 0) { continue; }
            constexpr double kMaxTan = .3;    // これ以上斜めでも奥の層は寄せない（窓の縁の外へ出ない範囲）
            const bool here = id == playerRoom;
            const double tx = std::clamp(seen.eyeX + (here ? followX : 0.0), -kMaxTan, kMaxTan);
            const double ty = std::clamp(seen.eyeY + (here ? followY : 0.0), -kMaxTan, kMaxTan);
            auto [it, fresh] = parallax_.try_emplace(id, std::array<double, 2>{tx, ty});
            if (!fresh) { it->second[0] += (tx - it->second[0]) * follow; it->second[1] += (ty - it->second[1]) * follow; }
        }
        // 診断用（環境変数 TD2_PARALLAX_TRACE がある時だけ）：鮮明に撮る部屋の目の向きを 30 フレームごとに記録する。
        static const bool trace = std::getenv("TD2_PARALLAX_TRACE") != nullptr;
        if (trace && frame_ % 30 == 0) {
            if (const auto it = parallax_.find(frame.activeRegion); it != parallax_.end()) {
                std::fprintf(stderr, "[TD2 parallax-trace] n=%zu region=%s eye=%.3f,%.3f\n", frame_, frame.activeRegion.c_str(), it->second[0], it->second[1]);
            }
        }
    }

    // 透視の窓は目の向きで置くので、視差を求めた後に窓を決める。
    intro_ = frame.intro;
    // 砂嵐の窓はパソコンの信号が合うまで使う。
    introStatic_ = intro_ && intro_->time < (std::max)(intro_->restoreTime, intro_->lockStart + intro_->lockSeconds + .1f);
    // 主人公は登場の光が集まってから足元から現れる。それまでは画面のどこにも出さない。
    // 体の現れ方はエフェクトが実際に出た時刻から数える（舞台の窓に入るまで出ないことがある）。出なくても 1 秒後には現す。
    heroShown_ = true; heroGrow_ = 1;
    if (!intro_) { introArrived_ = false; arrivePlayedAt_ = -1; }
    else {
        const double start = arrivePlayedAt_ >= 0 ? arrivePlayedAt_ : double(intro_->arriveTime) + 1.0 - kHeroFormDelay;
        const double since = intro_->time - start;
        heroShown_ = since >= kHeroFormDelay;
        heroGrow_ = std::clamp((since - kHeroFormDelay) / kHeroFormSeconds, .02, 1.0);
    }
    introRooms_ = valid;
    if (!intro_) { introOnset_.clear(); }
    if (!frame.authoring) { PlaceWindows(valid, frame, maxCols); PlaceFar(valid); }
    if (introStatic_) { PlaceNoise(); }

    // 3. テレビごとの窓（鮮明を優先）。一部屋だけを撮る窓（Play の鮮明二枚）にはその部屋だけを、周りの窓には収まる部屋を、
    // 遠くの窓には選んだ部屋（写っているもの）だけを写す。どの窓にも入らない部屋は描かない。
    // 切り出しは窓を動かしたフレームに同じ窓で行う（キャプチャは同じフレームで主描画より先に描かれるため、
    // 遅らせると別の部屋が一瞬映る）。
    std::vector<Assigned> shown;
    for (const auto kind : {ViewKind::Sharp, ViewKind::Area, ViewKind::Far, ViewKind::Far2}) {
        const auto& window = WindowOf(kind);
        const auto& members = members_[static_cast<size_t>(kind)];
        for (const auto* room : valid) {
            const auto& r = room->Region();
            if (std::any_of(shown.begin(), shown.end(), [room](const Assigned& a) { return a.room == room; })) { continue; }
            // Play の組の窓は組に入れた部屋だけ、編集中は窓に収まる部屋を写す。
            const bool chosen = frame.authoring || std::find(members.begin(), members.end(), room->regionId) != members.end();
            const bool fits = !window.regionId.empty() ? window.valid && room->regionId == window.regionId
                : chosen && window.Contains(room->x, room->y, room->x + r.cols, room->y + r.rows);
            if (fits) { shown.push_back({room, kind}); }
        }
    }

    SyncStageLights(world, shown, now);

    // 4. 舞台カメラ。画面の揺れは舞台カメラを右・上へ少しずらし、画面の中の絵だけを揺らす。
    double shakeX = 0, shakeY = 0;
    if (const double amplitude = shakeAmplitude_ * fade(shakeStart_, kShakeSeconds) * projection_.scale; amplitude > 0) {
        shakeX = (std::sin(now * 91.0) * .6 + std::sin(now * 53.0) * .4) * amplitude;
        shakeY = (std::cos(now * 77.0) * .6 + std::sin(now * 41.0) * .4) * amplitude;
    }
    const auto right = projection_.Right(), up = projection_.Up();
    for (size_t kind = 0; kind < kViewKinds; ++kind) {
        const auto& window = windows_[kind];
        if (!window.valid) { continue; }
        auto pose = projection_.CameraPose(window.x + window.cols * .5, window.y + window.rows * .5);
        if (window.lens >= 0) {
            // 透視の窓：向きはそのまま、目を窓の中心の正面 distance の所へ置く。
            const auto target = projection_.Point(window.x + window.cols * .5, window.y + window.rows * .5);
            const auto forward = projection_.Forward();
            pose.position = {target.x - static_cast<float>(window.distance * forward[0]), target.y - static_cast<float>(window.distance * forward[1]),
                             target.z - static_cast<float>(window.distance * forward[2])};
        }
        pose.position.x += static_cast<float>(right[0] * shakeX + up[0] * shakeY);
        pose.position.y += static_cast<float>(right[1] * shakeX + up[1] * shakeY);
        pose.position.z += static_cast<float>(right[2] * shakeX + up[2] * shakeY);
        const auto& owner = window.lens >= 0 ? lensCameras_[static_cast<size_t>(window.lens)] : cameras_[kind];
        if (const auto camera = world.FindObject(owner.handle); !camera.IsValid() || !camera.SetWorldTransform(pose).Accepted()) {
            status_ = "独立舞台镜头位置提交失败"; return;
        }
    }

    // 5〜7. キャプチャと画面の割り当て、イベント、部品、エフェクト。
    if (!SyncCaptures(world, shown, flashAmount_ * fade(flashStart_, kFlashSeconds))) { return; }
    if (!frame.authoring) { TraceFrame(); }
    // 編集中は出来事が無いので効果を消す。ただしオープニングのプレビューでは主人公の登場を出す（登場の前へ巻き戻すと消す）。
    if (frame.simulated) { worldEffects_.Observe(*frame.snapshot); }
    else if (!intro_ || intro_->time < intro_->arriveTime) { worldEffects_.Reset(); }
    if (intro_ && intro_->time < intro_->arriveTime) { introArrived_ = false; arrivePlayedAt_ = -1; }
    if (intro_ && !introArrived_ && frame.snapshot && intro_->time >= intro_->arriveTime) {
        worldEffects_.Add(ScreenEffectKind::Arrive, frame.snapshot->presentationPlayer, frame.snapshot->realTime, 1.5, frame.snapshot->tick);
        introArrived_ = true;
    }
    SubmitParts(world, valid, shown, *frame.snapshot, kit, fx.enabled && fx.smear, now, frame.simulated);
    PlayEffects(world, fx, frame.simulated || intro_ != nullptr, now);

    size_t ready = 0, failedCaptures = 0;
    const auto captures = world.GetSceneCapture();
    for (const auto& view : views_) {
        HIKARI::GameProjectSceneCaptureSnapshot snapshot{};
        if (captures.GetSnapshot(view.handle, snapshot)) {
            if (snapshot.status == HIKARI::GameProjectSceneCaptureStatus::Ready) { ++ready; }
            if (snapshot.status == HIKARI::GameProjectSceneCaptureStatus::Failed) { ++failedCaptures; }
        }
    }
    const auto& sharp = WindowOf(ViewKind::Sharp); const auto& group = WindowOf(ViewKind::Area);
    status_ = "Screen3D capture=" + std::to_string(views_.size()) + " sharp=" + std::to_string(sharp.width) + "x" +
        std::to_string(sharp.height) + " area=" + std::to_string(group.width) + "x" + std::to_string(group.height) +
        " ready=" + std::to_string(ready) + " gpuFailed=" + std::to_string(failedCaptures) + " screens=" + std::to_string(bound_.size()) +
        " rooms=" + std::to_string(shown.size()) + " models=" + std::to_string(accepted_) + " rejected=" + std::to_string(failed_) +
        " fxRejected=" + std::to_string(fxRejected_);
}

void Screen3DStage::TraceFrame() {
    static const bool enabled = std::getenv("TD2_SCREEN_TRACE") != nullptr;
    traceFrame_ = frame_;
    if (!enabled) { return; }
    std::string state = "sharp=" + WindowOf(ViewKind::Sharp).regionId + " protect=";
    std::vector<std::string> kinds, protect;
    int moved = 0;
    for (const auto& [id, binding] : bound_) {
        const auto view = std::find_if(views_.begin(), views_.end(), [&](const View& v) { return v.handle.value == binding.capture.value; });
        // S 鮮明、A 大区域、F・G 遠くの二組。
        const char kind = view == views_.end() ? '?' : "SAFG"[static_cast<size_t>(view->kind)];
        const auto short_id = binding.regionId.substr(binding.regionId.find('.') + 1);
        kinds.push_back(short_id + ":" + kind);
        if (binding.protect) {
            const auto seen = seen_.find(binding.regionId);
            protect.push_back(short_id);
        }
        const auto old = traceRects_.find(id);
        if (old != traceRects_.end() && std::memcmp(&old->second, &binding.rect, sizeof(binding.rect)) != 0) { ++moved; }
        traceRects_[id] = binding.rect;
    }
    // 保護が変わった画面ごとに理由を記録する。
    std::map<std::string, bool, std::less<>> nowProtect;
    for (const auto& [id, binding] : bound_) { nowProtect[binding.regionId] = binding.protect; }
    for (const auto& [region, on] : nowProtect) {
        const auto old = traceProtect_.find(region);
        if (old != traceProtect_.end() && old->second == on) { continue; }
        std::string reason = "on";
        if (!on) {
            const auto seen = seen_.find(region);
            reason = seen == seen_.end() ? "unseen" : seen->second.facing <= .05 ? "backface"
                : seen->second.coverage < .002 ? "small" : "limit-or-delay";
        }
        std::fprintf(stderr, "[TD2 protect-trace] n=%zu region=%s %s\n", traceFrame_, region.c_str(), reason.c_str());
    }
    for (const auto& [region, on] : traceProtect_) {
        if (on && !nowProtect.contains(region)) { std::fprintf(stderr, "[TD2 protect-trace] n=%zu region=%s unbound\n", traceFrame_, region.c_str()); }
    }
    traceProtect_ = std::move(nowProtect);
    for (const auto& p : protect) { state += p + " "; }
    // 組の窓ごとの部屋の数と、1 マスの texel。
    for (const auto kind : {ViewKind::Area, ViewKind::Far, ViewKind::Far2}) {
        const auto& window = WindowOf(kind);
        if (!window.valid) { continue; }
        state += std::string(1, "SAFG"[static_cast<size_t>(kind)]) + "=" + std::to_string(members_[static_cast<size_t>(kind)].size()) +
                 "rooms@" + std::to_string(window.density) + " ";
    }
    // 2 秒ごとに、写っている画面の 1 マスの texel（窓）と画面上の画素（1080p 換算）を並べる。前者が後者を大きく下回る
    // 画面は拡大されてぼける。
    if (traceFrame_ % 120 == 0) {
        std::string sharpness;
        for (const auto& [id, binding] : bound_) {
            const auto seen = seen_.find(binding.regionId);
            const auto view = std::find_if(views_.begin(), views_.end(), [&](const View& v) { return v.handle.value == binding.capture.value; });
            const auto room = std::find_if(introRooms_.begin(), introRooms_.end(), [&](const StageRoom* r) { return r->regionId == binding.regionId; });
            if (seen == seen_.end() || seen->second.coverage <= 0 || view == views_.end() || room == introRooms_.end()) { continue; }
            const double pixels = CellPixels(**room);
            sharpness += binding.regionId.substr(binding.regionId.find('.') + 1) + ":" + "SAFG"[static_cast<size_t>(view->kind)] +
                         std::to_string(WindowOf(view->kind).density) + "/" + std::to_string(static_cast<int>(pixels)) + " ";
        }
        std::fprintf(stderr, "[TD2 sharpness-trace] n=%zu texels/pixels %s\n", traceFrame_, sharpness.c_str());
    }
    // 画に写っているのに割り当てのない画面（周りの窓から外れている）。
    state += "visibleUnbound=";
    for (const auto& [region, seen] : seen_) {
        const bool bound = std::any_of(bound_.begin(), bound_.end(), [&](const auto& item) { return item.second.regionId == region; });
        if (!bound && seen.coverage > .0005) { state += region.substr(region.find('.') + 1) + "(" + std::to_string(static_cast<int>(seen.coverage * 1000)) + ") "; }
    }
    state += "bind=";
    for (const auto& k : kinds) { state += k + " "; }
    if (state != traceLast_ || moved) {
        std::fprintf(stderr, "[TD2 screen-trace] n=%zu moved=%d %s\n", traceFrame_, moved, state.c_str());
        traceLast_ = state;
    }
}

bool Screen3DStage::SyncCaptures(HIKARI::WorldView& world, const std::vector<Assigned>& screens, double flash) {
    const auto captures = world.GetSceneCapture();
    if (!captures.IsValid()) { status_ = "SceneCapture不可用"; return false; }
    // 窓と描き方（画面の明るさ込み）の組ごとにキャプチャを一つ作る（鮮明から順に、エンジンの上限まで）。
    std::vector<View> wanted;
    std::vector<int> viewOf(screens.size(), -1);
    // オープニングの砂嵐の窓は先に取る（キャプチャの上限に当たっても砂嵐が消えない）。描き方は後処理の無い原版。
    if (introStatic_ && WindowOf(ViewKind::Far).valid) {
        ScreenStyleSettings noise;
        noise.mode = static_cast<int>(ScreenStyleMode::Original); noise.outline = 0; noise.shadow = 0; noise.sketchDepth = 0;
        wanted.push_back({ViewKind::Far, noise});
    }
    for (size_t i = 0; i < screens.size(); ++i) {
        const auto style = Effective(*screens[i].room);
        auto found = std::find_if(wanted.begin(), wanted.end(), [&](const View& v) { return v.kind == screens[i].kind && SameStyle(v.style, style); });
        if (found == wanted.end()) {
            if (wanted.size() == HIKARI::kGameProjectSceneCaptureMaximumActiveCount) { continue; }
            wanted.push_back({screens[i].kind, style}); found = wanted.end() - 1;
        }
        viewOf[i] = static_cast<int>(found - wanted.begin());
    }
    // 今あるキャプチャは同じ種類の窓のものを前から順に使い回し、描き方だけを差し替える。要らなくなったものは消す。
    std::vector<bool> reused(views_.size(), false);
    for (auto& want : wanted) {
        for (size_t j = 0; j < views_.size(); ++j) {
            if (reused[j] || views_[j].kind != want.kind) { continue; }
            reused[j] = true; want.handle = views_[j].handle; want.camera = views_[j].camera;
            want.width = views_[j].width; want.height = views_[j].height;
            break;
        }
    }
    for (size_t j = 0; j < views_.size(); ++j) {
        if (reused[j]) { continue; }
        if (!captures.Destroy(views_[j].handle)) { status_ = "退役3D捕获释放失败"; return false; }
        std::erase_if(bound_, [&](const auto& item) { return item.second.capture.value == views_[j].handle.value; });
    }
    views_ = std::move(wanted);
    for (auto& view : views_) {
        const auto& window = WindowOf(view.kind);
        HIKARI::GameProjectSceneCaptureDesc desc{};
        // 透視の窓は透視カメラで撮る。
        desc.cameraObject = window.lens >= 0 ? lensCameras_[static_cast<size_t>(window.lens)].handle : cameras_[static_cast<size_t>(view.kind)].handle;
        desc.width = window.width; desc.height = window.height;
        // エフェクト（VFX）も舞台の一部として写す。
        desc.flags = HIKARI::GameProjectSceneCaptureFlags(HIKARI::GameProjectSceneCaptureFlag::Enabled |
                                                          HIKARI::GameProjectSceneCaptureFlag::RenderVfx);
        if (!view.handle.IsValid()) {
            view.handle = captures.Create(desc);
            if (!view.handle.IsValid()) { status_ = "3D捕获创建失败（最多4个）"; return false; }
        } else if (view.width != desc.width || view.height != desc.height || view.camera != desc.cameraObject) {
            // 大きさかカメラが変わった時だけ更新する（毎フレーム更新すると GPU 側の再検証が走る）。
            if (!captures.Update(view.handle, desc)) { status_ = "3D捕获更新失败"; return false; }
        }
        view.width = desc.width; view.height = desc.height; view.camera = desc.cameraObject;
        const bool noise = introStatic_ && view.kind == ViewKind::Far;
        auto look = StyleDesc(view.style, view.style.brightness, window.density, projection_.scale, !noise && projection_.oblique > 0,
                              window.lens >= 0 ? window.distance : kStageCameraDistance, !noise && solid_);
        // 画面の閃光：明るさを一時的に上乗せする。描き方の組分け（SameStyle）には含めない。
        look.brightness = static_cast<float>(std::clamp(look.brightness * (1 + flash), 0.0, 4.0));
        if (!captures.SetStyle(view.handle, look)) { status_ = "屏幕风格提交失败，需要含SetStyle v2的新引擎"; return false; }
    }

    // 画面の割り当て：変わった画面だけやり直す（同じ物体・材質スロットなら、別のキャプチャへ移る時もエンジンが差し替える）。
    // 窓から外れた画面と、材質スロットが変わった画面は先に外す。
    std::map<uint64_t, Binding> next;
    for (size_t i = 0; i < screens.size(); ++i) {
        if (viewOf[i] < 0) { continue; }
        const auto& room = *screens[i].room; const auto& config = room.configuration; const auto& r = room.Region();
        const auto screen = world.FindSceneObject(config.screenObject);
        if (!screen.IsValid() || config.screenSlot.empty() || config.screenSlot.size() >= HIKARI::kGameProjectSceneCaptureMaterialSlotNameCapacity) { continue; }
        const auto& view = views_[static_cast<size_t>(viewOf[i])];
        const auto& window = WindowOf(view.kind);
        const HIKARI::GameProjectSceneCaptureSourceRect rect{
            static_cast<float>((room.x - window.x) / window.cols), static_cast<float>((room.y - window.y) / window.rows),
            static_cast<float>(r.cols / window.cols), static_cast<float>(r.rows / window.rows)};
        next[screen.GetHandle().value] = {screen.GetHandle(), view.handle, config.screenSlot, rect, false, room.regionId};
    }
    // 保護は前のフレームのカメラで測った見え方で選ぶ。オープニングの砂嵐はその後で結び直す。
    ChooseProtected(next);
    ApplyIntro(world, next);
    for (const auto& [id, old] : bound_) {
        const auto found = next.find(id);
        if ((found == next.end() || found->second.slot != old.slot) && world.FindObject(old.object).IsValid()) {
            (void)captures.ClearMaterialBinding(old.capture, old.object);
        }
    }
    for (const auto& [id, binding] : next) {
        const auto old = bound_.find(id);
        if (old != bound_.end() && old->second.capture.value == binding.capture.value && old->second.slot == binding.slot &&
            std::memcmp(&old->second.rect, &binding.rect, sizeof(binding.rect)) == 0 && old->second.protect == binding.protect) { continue; }
        HIKARI::GameProjectSceneCaptureMaterialRegionBindingDesc desc{};
        desc.capture = binding.capture; desc.modelObject = binding.object; desc.sourceRect = binding.rect;
        std::memcpy(desc.materialSlotName, binding.slot.c_str(), binding.slot.size() + 1);
        desc.flags = binding.protect ? HIKARI::GameProjectSceneCaptureMaterialBindingFlags(HIKARI::GameProjectSceneCaptureMaterialBindingFlag::ProtectReadability)
                                     : HIKARI::GameProjectSceneCaptureMaterialBindingFlags(HIKARI::GameProjectSceneCaptureMaterialBindingFlag::None);
        if (!captures.BindMaterialRegion(desc)) { status_ = "屏幕3D绑定失败：" + binding.slot; bound_ = std::move(next); return false; }
    }
    bound_ = std::move(next);
    return true;
}

void Screen3DStage::PlayEffects(HIKARI::WorldView& world, const ScreenFxSettings& fx, bool available, double now) {
    const auto vfx = world.GetVfx();
    if (!available) {
        for (const auto& active : activeFx_) { if (active.handle.IsValid()) { (void)vfx.Stop(active.handle); } }
        activeFx_.clear(); return;
    }
    const auto fade = [now](double start, double seconds) {
        const double age = now - start;
        return start < 0 || age < 0 || age >= seconds ? 0.0 : (1 - age / seconds) * (1 - age / seconds);
    };
    // 描いている窓の中のイベントだけを出す（境のエフェクトは両側のテレビに写る）。
    const auto drawn = [this](double x, double y) {
        return std::any_of(windows_.begin(), windows_.end(), [&](const Window& w) { return w.Contains(x, y, x, y); });
    };
    // 新しいイベントごとに一度出す。瞬間移動の伸びと吸い込みは既存のインスタンスの位置だけを更新する。
    std::vector<ActiveFx> next;
    for (const auto& item : worldEffects_.Items()) {
        if (item.kind == ScreenEffectKind::RecallTrail) { continue; }   // 光跡は部品で描く
        const auto& entry = fx.entries[static_cast<size_t>(SlotOf(item.kind))];
        const auto found = std::find_if(activeFx_.begin(), activeFx_.end(), [&item](const auto& v) { return v.eventId == item.id; });
        const auto& anchor = item.kind == ScreenEffectKind::BlinkTrail ? item.source : item.rect;
        if (!fx.enabled || entry.asset.IsEmpty() || !drawn(anchor.x + anchor.w * .5, anchor.y + anchor.h * .5)) {
            if (found != activeFx_.end()) {
                if (found->handle.IsValid()) { (void)vfx.Stop(found->handle); }
                // 画面へ戻った時も同じイベントを再生し直さない。未再生のものだけは入った時に出せる。
                next.push_back({item.id, {}});
            }
            continue;
        }
        // 装置が切れる側は同じエフェクトを小さく出す。
        const double scale = fx.scale * entry.scale * (item.kind == ScreenEffectKind::DeviceSwitch && item.reversed ? .7 : 1.0);
        auto transform = FxTransform(item, projection_, scale);
        // 回された部屋の中のエフェクトは、部屋の中心について点対称に置いて 180° 回す（画面では正立して見える）。
        for (const auto& [id, room] : flippedRooms_) {
            if (anchor.x + anchor.w * .5 < room[0] || anchor.y + anchor.h * .5 < room[1] || anchor.x + anchor.w * .5 >= room[2] ||
                anchor.y + anchor.h * .5 >= room[3]) { continue; }
            const auto center = projection_.Point((room[0] + room[2]) * .5, (room[1] + room[3]) * .5, projection_.FxDepth());
            transform.position = {2 * center.x - transform.position.x, 2 * center.y - transform.position.y, transform.position.z};
            const auto q = transform.rotation;
            transform.rotation = {-q.y, q.x, q.w, -q.z};   // Z 軸 180° を左から掛ける
            break;
        }
        if (found != activeFx_.end()) {
            if (found->handle.IsValid() && (item.kind == ScreenEffectKind::BlinkTrail || item.kind == ScreenEffectKind::EchoAbsorb)) {
                HIKARI::GameProjectVfxUpdateRequest update{}; update.updateTransform = 1; update.transform = transform;
                (void)vfx.Update(found->handle, update);
            }
            next.push_back(*found);
            continue;
        }
        HIKARI::GameProjectVfxPlayRequest request{};
        request.asset = entry.asset; request.ownerObjectHandle = cameras_[static_cast<size_t>(ViewKind::Sharp)].handle.value;
        request.transform = transform;
        request.randomSeed = FxRandomSeed(item); request.loopMode = HIKARI::GameProjectVfxLoopMode::OneShot;
        const auto handle = vfx.Play(request);
        if (handle.IsValid()) { next.push_back({item.id, handle}); } else { ++fxRejected_; }
        if (handle.IsValid() && item.kind == ScreenEffectKind::Arrive && intro_) { arrivePlayedAt_ = intro_->time; }
        // 演出：残っている揺れ・閃光より強い時だけ上書きする。
        const double shake = entry.shake * fx.shake, flash = entry.flash * fx.flash;
        if (shake > 0 && shake >= shakeAmplitude_ * fade(shakeStart_, kShakeSeconds)) { shakeAmplitude_ = shake; shakeStart_ = now; }
        if (flash > 0 && flash >= flashAmount_ * fade(flashStart_, kFlashSeconds)) { flashAmount_ = flash; flashStart_ = now; }
    }
    activeFx_ = std::move(next);
}
}
