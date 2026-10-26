#include "TD2Screen3D.h"
#include "TD2Screen3DShared.h"
#include "TD2StageProjection.h"
#include "TD2ScreenKitAssets.h"
#include "HIKARI/Project/ActionViews.h"
#include "HIKARI/Project/PresentationViews.h"
#include "HIKARI/Project/VfxView.h"
#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <functional>
#include <set>

namespace TD2 {
namespace {
using namespace Stage3D;
// オープニングの登場：エフェクトが始まってから体が現れ始めるまでと、足元から全身になるまで（秒）。
constexpr double kHeroFormDelay = .35, kHeroFormSeconds = .45;
// 演出の長さ（秒）。揺れと閃光は二乗で減衰する。
constexpr double kShakeSeconds = 0.28, kFlashSeconds = 0.22;
// 画に写っている覗き窓を描き直す間隔（フレーム）：段ごと。一番粗い段（ごく小さな画面）だけ一つおきにし、残りは毎フレーム。
// 間引いた画面はカメラの揺れの間も前の角度のまま止まり、時間方向の AA の履歴と混ざって揺れて見えるので、大きな画面は間引かない。
// 画の外へ出たばかりの画面（kPortalHold の間）は描き直さない。
constexpr std::array<uint64_t, kPortalTiers> kPortalRedrawEvery{1, 1, 1, 2};
// オープニングで飾りのテレビの信号が合う波の数（波ごとに一度、エンジンが GPU のシーンの経路を組み直す）。
constexpr double kIntroLockWaves = 3;

bool SameStyle(const ScreenStyleSettings& a, const ScreenStyleSettings& b) {
    // 明るさは保存の丸めで僅かに違うことがあるので、誤差の範囲は同じ組にする（組が増えるとキャプチャが足りなくなる）。
    return a.mode == b.mode && a.outline == b.outline && a.shadow == b.shadow && a.sketchDepth == b.sketchDepth &&
           a.sketchFill == b.sketchFill && std::abs(a.brightness - b.brightness) < 1e-3f && a.atmosphere == b.atmosphere &&
           a.occlusion == b.occlusion && a.castLight == b.castLight && a.inkLine == b.inkLine && SameTone(a.tone, b.tone);
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
    // 色調：人物・仕掛けの外（地形・壁・背景）だけ。
    const auto& tone = style.tone;
    fx.toneTint = {std::clamp(tone.tint.x, 0.f, 1.f), std::clamp(tone.tint.y, 0.f, 1.f), std::clamp(tone.tint.z, 0.f, 1.f)};
    fx.toneTintStrength = std::clamp(tone.tintStrength, 0.f, 1.f);
    fx.toneHue = std::clamp(tone.hue, -.5f, .5f);
    fx.toneSaturation = std::clamp(tone.saturation, 0.f, 2.f);
    fx.toneBrightness = std::clamp(tone.brightness, 0.f, 2.f);
    fx.toneContrast = std::clamp(tone.contrast, 0.f, 2.f);
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
    // 残影の土煙は着地の土煙、光の火花は衝撃の火花を小さくして使う。
    case ScreenEffectKind::EchoDust: return ScreenFxSlot::Land;
    case ScreenEffectKind::BeamSpark: return ScreenFxSlot::Impact;
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
                      e.kind == ScreenEffectKind::Arrive || e.kind == ScreenEffectKind::EchoDust;
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

// ---------------------------------------------------------------- 診断の計時
namespace {
bool StageTraceOn() { static const bool on = std::getenv("TD2_STAGE_TRACE") != nullptr; return on; }
long long StageNow() { return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
std::map<std::string, double>& StageTotals() { static std::map<std::string, double> totals; return totals; }
}
StageTimer::StageTimer(const char* name) { if (StageTraceOn()) { name_ = name; start_ = StageNow(); } }
StageTimer::~StageTimer() { if (name_) { StageTotals()[name_] += (StageNow() - start_) * 1e-6; } }
void StageTimer::EndFrame() {
    if (!StageTraceOn()) return;
    static int frames = 0;
    if (++frames < 240) return;
    std::string line;
    for (const auto& [name, ms] : StageTotals()) { char text[64]; std::snprintf(text, sizeof(text), " %s=%.3f", name.c_str(), ms / frames); line += text; }
    std::fprintf(stderr, "[TD2 stage-trace]%s\n", line.c_str());
    StageTotals().clear(); frames = 0;
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
    for (const auto& [region, cache] : roomParts_) { for (const auto& [owner, id] : cache.retained) { models.Remove(owner, id); } }
    submitted_.clear(); roomParts_.clear(); terrainKey_ = 0; bound_.clear(); deviceMemory_.clear();
    const auto captures = world.GetSceneCapture();
    for (auto it = views_.begin(); it != views_.end();) {
        if (!captures.Destroy(it->handle)) { status_ = "3D キャプチャを解放できない（次のフレームで再試行）"; return false; }
        it = views_.erase(it);
    }
    for (auto& camera : cameras_) { RestoreCamera(world, camera); }
    RestoreCamera(world, noiseCamera_);
    ReleaseStageLights(world);
    windows_ = {};
    portals_.clear(); portalTier_.clear(); portalSeen_.clear(); portalTan_.clear(); portalDrawn_.clear(); portalPlace_.clear(); portalLimit_.clear(); follow_.clear(); introPlanes_.clear();
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
            if (kind == static_cast<size_t>(ViewKind::Sharp)) { Release(world); status_ = "専用の正投影の舞台カメラが要る（farClip>31）"; return false; }
            continue;
        }
        if (camera.sceneId != ids[kind] || camera.handle != object.GetHandle()) {
            RestoreCamera(world, camera);
            if (!object.GetWorldTransform(camera.original)) { status_ = "舞台カメラを読めない"; return false; }
            camera.sceneId = ids[kind]; camera.handle = object.GetHandle();
        }
        camera.orthographicHeight = lens.orthographicHeight;
    }
    return true;
}

bool Screen3DStage::AcquireNoiseCamera(HIKARI::WorldView& world, const ScreenStyleSettings& stage) {
    // 「遠くのテレビのカメラ 1」を砂嵐の窓に使う。正投影の舞台カメラと同じ物体は使えない（姿勢を奪い合う）。
    auto& camera = noiseCamera_;
    const uint64_t id = stage.farCameraObject;
    const auto object = id ? world.FindSceneObject(id) : HIKARI::ObjectView{};
    HIKARI::GameProjectCameraLens lens{};
    const bool usable = object.IsValid() && std::none_of(cameras_.begin(), cameras_.end(), [&](const StageCamera& c) { return c.sceneId == id; }) &&
                        world.GetCamera().GetAuthoredLens(object.GetHandle().value, lens) &&
                        lens.projection == HIKARI::GameProjectCameraProjectionKind::Perspective &&
                        lens.verticalFovRadians > .02f && lens.verticalFovRadians < 1.2f;
    if (!usable) { RestoreCamera(world, camera); return false; }
    if (camera.sceneId != id || camera.handle != object.GetHandle()) {
        RestoreCamera(world, camera);
        if (!object.GetWorldTransform(camera.original)) { return false; }
        camera.sceneId = id; camera.handle = object.GetHandle();
    }
    camera.verticalFov = lens.verticalFovRadians;
    return true;
}

void Screen3DStage::ApplyIntro(HIKARI::WorldView& world, std::map<uint64_t, Binding>& next) const {
    if (!introStatic_) { return; }
    const auto view = std::find_if(views_.begin(), views_.end(), [](const View& v) { return v.kind == ViewKind::Noise; });
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
        next[object.value] = {object, view->handle, std::string(kScreenSlot),
                              {static_cast<float>(x), static_cast<float>(y), static_cast<float>(w), static_cast<float>(h)}, false, region};
    };
    // 通関：普段の画面のまま前兆で乱れ、最初の稲妻からはオープニングと同じく砂嵐になり（遠くのテレビは普段の画面を撮っていないので
    // 黒いまま残さない）、ゴールから遠い順にブラウン管のように消える。ゴールのテレビは扉を映したまま、最後にゆっくり消える。
    // 暗転の後は全部黒く、目覚めからはパソコンだけがタイトルを映す（オープニングのタイトル待ちと同じ姿）。
    if (intro_->ending) {
        const double first = intro_->lightning.empty() ? intro_->suckStart : intro_->lightning.front();
        const bool flash = std::any_of(intro_->lightning.begin(), intro_->lightning.end(), [&](float at) { return t >= at && t < at + .12; });
        const bool omen = IntroOmenBurst(t, intro_->flickerStart, first);
        const auto staticLook = [&](uint64_t key) {
            const double r = IntroHash(key, tick * 13u + 1u);
            return Look{r < .12 ? 0 : r < .42 ? 1 : r < .85 ? 2 : 3};
        };
        const auto endingLook = [&](uint64_t handle, bool computer) {
            if (t >= intro_->blackTime) { return computer && t >= intro_->revealTime ? Look{6} : Look{0}; }
            const bool goal = handle == intro_->goalScreen;
            const auto found = intro_->offTime.find(handle);
            const double off = found != intro_->offTime.end() ? found->second : intro_->suckStart + intro_->collapseSeconds;
            const double line = goal ? .45 : .16, dot = goal ? .5 : .2;
            if (t >= off + line + dot) { return Look{0}; }
            if (t >= off + line) { return Look{7, (t - off - line) / dot}; }
            if (t >= off) { return Look{4, (t - off) / line}; }
            if (flash) { return Look{3}; }
            const double r = IntroHash(handle, tick * 5u + 9u);
            if (t >= first) { return !goal ? staticLook(handle) : r < .06 ? Look{1 + int(r < .03)} : Look{}; }
            return omen && IntroHash(handle, 31) < .35 ? Look{1 + int(IntroHash(handle, tick) < .5)} : Look{};
        };
        std::set<uint64_t> placed;
        for (const auto* room : introRooms_) {
            const auto id = room->configuration.screenObject;
            const auto screen = world.FindSceneObject(id);
            if (!room->placed || !screen.IsValid()) { continue; }
            placed.insert(screen.GetHandle().value);
            if (const auto look = endingLook(screen.GetHandle().value, id == intro_->computerScreen); look.band >= 0) { bind(screen.GetHandle(), id, look, room->regionId); }
        }
        for (const auto& handle : intro_->tvScreens) {
            if (placed.contains(handle.value) || next.contains(handle.value)) { continue; }
            if (const auto look = endingLook(handle.value, false); look.band >= 0) { bind(handle, handle.value, look, {}); }
        }
        return;
    }
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
    // decorative は区域の無い飾りのテレビ（普段の画面が自分の材質で、結び付けを外して戻す）。
    const auto bandFor = [&](uint64_t key, bool computer, uint64_t handle, bool decorative) {
        if (t >= intro_->restoreTime) {
            // 暗転の間（カメラは画面の中）、パソコンは普段の画面に戻しておく（砂嵐の窓を写していると暗転の黒が茶色く浮く）。
            if (t < intro_->revealTime) { return computer ? Look{} : Look{0}; }
            // 電気が戻る：手元から遠くへ順に、点 → 横線 → 砂嵐と点く。信号は遠くから合っていき、パソコンが最後。
            const double reach = computer ? 0.0 : infoOf(key)[1];
            const double on = intro_->powerOn + .15 + std::pow(reach, .8) * intro_->powerSeconds * .9 + IntroHash(key, 21) * .2;
            double locked = intro_->lockStart + (1 - reach) * intro_->lockSeconds + IntroHash(key, 22) * .15;
            // 飾りのテレビは結び付けを外して普段の材質へ戻すので、外す時刻を遠い・中ほど・手前の三つの波にまとめる（外すたびに
            // エンジンが GPU のシーンの経路を組み直して数十 ms 止まる。細かく分けると信号が合っていく間ずっと引っかかる）。
            if (decorative) {
                const double wave = (std::max)(.25, (intro_->lockSeconds + .15) / kIntroLockWaves);
                locked = intro_->lockStart + std::ceil((locked - intro_->lockStart) / wave) * wave;
            }
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
        if (const auto look = bandFor(id, id == intro_->computerScreen, screen.GetHandle().value, false); look.band >= 0) { bind(screen.GetHandle(), id, look, room->regionId); }
    }
    // 装飾テレビと置いていない区域のテレビ：普段の画面が無いので、砂嵐になるまでは黒い段に結んでおく
    // （オープニングの始めに一度だけ結び、途中でシーンを作り直さない）。
    for (const auto& handle : intro_->tvScreens) {
        if (placed.contains(handle.value) || next.contains(handle.value)) { continue; }
        // 元に戻した後は結ばない（普段の材質へ戻る）。
        if (const auto look = bandFor(handle.value, false, handle.value, true); look.band >= 0) { bind(handle, handle.value, look, {}); }
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
    if (valid.empty() || !frame.snapshot) { Release(world); status_ = "3D 表示がオフか、有効な区域が無い"; return; }
    flippedRooms_.clear();
    for (const auto* room : valid) if (room->flipped) {
        flippedRooms_.push_back({room->regionId, {double(room->x), double(room->y), double(room->x + room->Region().cols), double(room->y + room->Region().rows)}});
    }
    if (!maxRows) for (const auto* room : valid) { maxCols = (std::max)(maxCols, room->Region().cols); maxRows = (std::max)(maxRows, room->Region().rows); }

    // 2. 舞台カメラと写し方。1 マスの舞台上の長さは、鮮明カメラの正投影の高さを 1 部屋分の行で割った値。
    if (!AcquireCameras(world, stage)) { return; }
    projection_.scale = cameras_[static_cast<size_t>(ViewKind::Sharp)].orthographicHeight / maxRows;
    // 奥行きの格子・透視の覗き窓・受光は Play だけ（編集中は作者が見る元の形のまま）。
    // 透視の覗き窓は外のカメラが画面を見る角度で撮る。立体の組（斜投影を焼いた部品）なら右上から見た透視になり、
    // 受光と一緒なら斜めに切っていない実体の地形を真ん中から見る（左右の壁・床・天井の内側が見える）。
    depthRows_ = stage.depthGrid && !frame.authoring ? std::clamp(stage.depthRows, 1, 8) : 0;
    perspective_ = stage.perspective && !frame.authoring;
    // 砂嵐の窓のカメラ。編集中はオープニングのプレビューの間だけ使う。
    if ((frame.authoring && !frame.intro) || !AcquireNoiseCamera(world, stage)) { RestoreCamera(world, noiseCamera_); }
    // 受光は透視の窓と一緒の時だけ（受光の部品は実体の地形の組しか無い）。舞台の灯は別の切り替え（既定は切る）。
    solid_ = stage.lit && perspective_;
    lit_ = solid_ && stage.stageLights;
    thinScreens_ = stage.thinScreens;
    actorScale_ = std::clamp(static_cast<double>(stage.actorScale), .8, 2.0);
    playDepth_ = solid_ ? std::clamp(static_cast<double>(stage.playDepth), 0.0, 4.0) : 0.0;
    // 立体：斜投影を焼いた部品の組を正対のカメラで撮る（天板と右の側面が見え、正面はマスにぴったり）。
    // 実体の地形は斜めに切らない受光の組を透視で撮る。
    projection_.oblique = solid_ ? 0.0 : kScreenKitOblique;
    projection_.pitch = 0.0; projection_.yaw = 0.0;
    const int kit = solid_ ? kScreenKitPlay : kScreenKitEdit;
    travel_ = {frame.travelFrom, frame.travelTo};
    ++frame_;
    play_ = !frame.authoring;
    if (frame.authoring) { seen_.clear(); portals_.clear(); PlaceEditorWindow(valid); }
    else { StageTimer timer("inspect"); Inspect(world, valid, frame); }
    // 覗き窓の描き方は操作中の部屋の描き方（部屋ごとの違いは明るさの丸め程度で、段ごとに一枚のキャプチャを共有する）。
    portalStyle_ = Effective(*valid.front());
    for (const auto* room : valid) { if (room->regionId == frame.activeRegion) { portalStyle_ = Effective(*room); } }
    // 視差：画面を見る角度の正接を 0.25 秒でなめらかに追う（見えていない画面は最後の角度のまま）。手持ちの揺れは残す。
    hall_ = stage.hall;
    parallaxGain_ = std::clamp(static_cast<double>(stage.parallax), 0.0, 2.0);
    {
        const double dt = lastParallaxTime_ < 0 ? 0.0 : std::clamp(now - lastParallaxTime_, 0.0, .1);
        lastParallaxTime_ = now;
        const double follow = dt > 0 ? 1 - std::exp(-dt / .25) : 1.0;
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
        // 目の追従：プレイヤーのいる部屋だけへ足し、部屋を出ると 0 へ戻す（どちらも 0.25 秒でなめらかに）。
        for (auto& [id, value] : follow_) {
            const bool here = id == playerRoom;
            value[0] += ((here ? followX : 0.0) - value[0]) * follow; value[1] += ((here ? followY : 0.0) - value[1]) * follow;
        }
        if (!playerRoom.empty()) { follow_.try_emplace(playerRoom, std::array<double, 2>{0.0, 0.0}); }
        // 手で付ける視差（透視で撮らない時の窓の向こうの大広間）：見る角度をなめらかに追う。
        for (const auto& [id, seen] : seen_) {
            if (seen.coverage <= 0) { continue; }
            constexpr double kMaxTan = .3;    // これ以上斜めでも奥の層は寄せない（窓の縁の外へ出ない範囲）
            const auto f = follow_.find(id);
            const double tx = std::clamp(seen.eyeX + (f != follow_.end() ? f->second[0] : 0.0), -kMaxTan, kMaxTan);
            const double ty = std::clamp(seen.eyeY + (f != follow_.end() ? f->second[1] : 0.0), -kMaxTan, kMaxTan);
            auto [it, fresh] = parallax_.try_emplace(id, std::array<double, 2>{tx, ty});
            if (!fresh) { it->second[0] += (tx - it->second[0]) * follow; it->second[1] += (ty - it->second[1]) * follow; }
        }
    }

    // 覗き窓は目の向きで撮るので、目の追従を求めた後に決める。
    intro_ = frame.intro;
    uiScreen_ = frame.uiScreen;
    // 砂嵐の窓はパソコンの信号が合うまで使う。
    introStatic_ = intro_ && (intro_->ending || intro_->time < (std::max)(intro_->restoreTime, intro_->lockStart + intro_->lockSeconds + .1f));
    // 主人公は登場の光が集まってから足元から現れる。それまでは画面のどこにも出さない。
    // 体の現れ方はエフェクトが実際に出た時刻から数える（舞台の窓に入るまで出ないことがある）。出なくても 1 秒後には現す。
    heroShown_ = true; heroGrow_ = 1;
    if (!intro_) { introArrived_ = false; arrivePlayedAt_ = -1; }
    else if (intro_->ending) {
        // 通関：扉が閉じ始めると、登場の光の中で足元へ縮んで消える。
        const double since = intro_->time - intro_->closeStart;
        heroShown_ = since < .2;
        heroGrow_ = std::clamp(1 - since / .2, .02, 1.0);
    } else {
        const double start = arrivePlayedAt_ >= 0 ? arrivePlayedAt_ : double(intro_->arriveTime) + 1.0 - kHeroFormDelay;
        const double since = intro_->time - start;
        heroShown_ = since >= kHeroFormDelay;
        heroGrow_ = std::clamp((since - kHeroFormDelay) / kHeroFormSeconds, .02, 1.0);
    }
    introRooms_ = valid;
    if (!intro_ || intro_->ending) { introOnset_.clear(); }
    // Play の砂嵐：機構で動いている部屋と、見せ場の間に新しく画に入った部屋（オープニングの間は砂嵐の窓が別にある）。
    eventFraming_ = frame.eventFraming && !frame.authoring && !introStatic_;
    if (!eventFraming_) { eventSnow_.clear(); movedSnow_.clear(); }
    else { movedSnow_.insert(frame.snowRooms.begin(), frame.snowRooms.end()); }
    snow_.clear();
    if (!frame.authoring && !introStatic_) {
        snow_.insert(frame.snowRooms.begin(), frame.snowRooms.end());
        snow_.insert(eventSnow_.begin(), eventSnow_.end());
        snow_.erase(frame.activeRegion);
    }
    // 診断（環境変数 TD2_SNOW_TRACE）：砂嵐の部屋が変わった時に一覧を出す。
    if (static const bool snowTrace = std::getenv("TD2_SNOW_TRACE") != nullptr; snowTrace) {
        static std::string last;
        std::string list = std::to_string(frame.snowRooms.size()) + (frame.eventFraming ? " event" : "") + ":";
        for (const auto& room : snow_) { list += " " + room; }
        if (list != last) { std::fprintf(stderr, "[TD2 snow] t=%.2f %s\n", now, list.c_str()); last = list; }
    }
    if (!frame.authoring) { StageTimer timer("portals"); windows_ = {}; PlacePortals(valid, frame, now); }
    (void)maxCols;
    if (introStatic_) { PlaceNoise(); }

    // 3. 写す部屋：Play は覗き窓の部屋（目立つ順）、編集中は窓に収まる部屋（鮮明の窓が先）。
    // 視点と切り出しは同じフレームに決める（キャプチャは同じフレームで主描画より先に描かれるため、遅らせると別の部屋が
    // 一瞬映る）。
    std::vector<Assigned> shown;
    if (!frame.authoring) {
        for (const auto& portal : portals_) { shown.push_back({portal.room, ViewKind::Sharp}); }
    } else {
        for (const auto kind : {ViewKind::Sharp, ViewKind::Area}) {
            const auto& window = WindowOf(kind);
            for (const auto* room : valid) {
                const auto& r = room->Region();
                if (std::any_of(shown.begin(), shown.end(), [room](const Assigned& a) { return a.room == room; })) { continue; }
                if (window.Contains(room->x, room->y, room->x + r.cols, room->y + r.rows)) { shown.push_back({room, kind}); }
            }
        }
    }

    { StageTimer timer("lights"); SyncStageLights(world, shown, now); }

    // 4. 窓の舞台カメラ（編集中の二枚と砂嵐）。画面の揺れは舞台カメラ（覗き窓では視点の目）を右・上へ少しずらし、画面の中の
    // 絵だけを揺らす。
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
        if (window.distance > 0) {
            // 透視の窓：向きはそのまま、目を窓の中心の正面 distance の所へ置く。
            const auto target = projection_.Point(window.x + window.cols * .5, window.y + window.rows * .5);
            const auto forward = projection_.Forward();
            pose.position = {target.x - static_cast<float>(window.distance * forward[0]), target.y - static_cast<float>(window.distance * forward[1]),
                             target.z - static_cast<float>(window.distance * forward[2])};
        }
        pose.position.x += static_cast<float>(right[0] * shakeX + up[0] * shakeY);
        pose.position.y += static_cast<float>(right[1] * shakeX + up[1] * shakeY);
        pose.position.z += static_cast<float>(right[2] * shakeX + up[2] * shakeY);
        const auto& owner = static_cast<ViewKind>(kind) == ViewKind::Noise ? noiseCamera_ : cameras_[kind];
        if (const auto camera = world.FindObject(owner.handle); !camera.IsValid() || !camera.SetWorldTransform(pose).Accepted()) {
            status_ = "舞台カメラの位置を送れない"; return;
        }
    }

    // 5〜7. キャプチャと画面の割り当て、イベント、部品、エフェクト。
    { StageTimer timer("captures"); if (!SyncCaptures(world, shown, flashAmount_ * fade(flashStart_, kFlashSeconds), shakeX, shakeY)) { return; } }
    // 編集中は出来事が無いので効果を消す。ただしオープニングのプレビューでは主人公の登場を出す（登場の前へ巻き戻すと消す）。
    if (frame.simulated) { worldEffects_.Observe(*frame.snapshot); }
    else if (!intro_ || intro_->time < intro_->arriveTime) { worldEffects_.Reset(); }
    if (intro_ && intro_->time < intro_->arriveTime) { introArrived_ = false; arrivePlayedAt_ = -1; }
    if (intro_ && !introArrived_ && frame.snapshot && intro_->time >= intro_->arriveTime) {
        worldEffects_.Add(ScreenEffectKind::Arrive, frame.snapshot->presentationPlayer, frame.snapshot->realTime, 1.5, frame.snapshot->tick);
        introArrived_ = true;
    }
    { StageTimer timer("parts"); SubmitParts(world, valid, shown, *frame.snapshot, kit, fx.enabled && fx.smear, now, frame.simulated); }
    { StageTimer timer("effects"); PlayEffects(world, fx, frame.simulated || intro_ != nullptr, now); }
    StageTimer::EndFrame();

    size_t ready = 0, failedCaptures = 0;
    const auto captures = world.GetSceneCapture();
    for (const auto& view : views_) {
        HIKARI::GameProjectSceneCaptureSnapshot snapshot{};
        if (captures.GetSnapshot(view.handle, snapshot)) {
            if (snapshot.status == HIKARI::GameProjectSceneCaptureStatus::Ready) { ++ready; }
            if (snapshot.status == HIKARI::GameProjectSceneCaptureStatus::Failed) { ++failedCaptures; }
        }
    }
    std::array<size_t, kPortalTiers> perTier{};
    for (const auto& portal : portals_) { ++perTier[static_cast<size_t>(portal.tier)]; }
    std::string tiers;
    for (size_t t = 0; t < kPortalTiers; ++t) { tiers += (t ? "/" : "") + std::to_string(perTier[t]); }
    status_ = "Screen3D capture=" + std::to_string(views_.size()) + " portals=" + tiers +
        " ready=" + std::to_string(ready) + " gpuFailed=" + std::to_string(failedCaptures) + " screens=" + std::to_string(bound_.size()) +
        " rooms=" + std::to_string(shown.size()) + " models=" + std::to_string(accepted_) + " rejected=" + std::to_string(failed_) +
        " fxRejected=" + std::to_string(fxRejected_);
}

bool Screen3DStage::SyncCaptures(HIKARI::WorldView& world, const std::vector<Assigned>& screens, double flash, double shakeX, double shakeY) {
    const auto captures = world.GetSceneCapture();
    if (!captures.IsValid()) { status_ = "SceneCapture が使えない"; return false; }
    const bool play = play_;
    // 要るキャプチャ：オープニングの砂嵐の窓（キャプチャの上限に当たっても砂嵐が消えないよう先に取る。描き方は後処理の無い
    // 原版）。Play は覗き窓の段ごとに一枚（空の段は止めておき、テレビが入った瞬間に作り直さない）。編集中は窓と描き方
    // （画面の明るさ込み）の組ごとに一枚。
    std::vector<View> wanted;
    std::vector<int> viewOf(screens.size(), -1);
    if (introStatic_ && WindowOf(ViewKind::Noise).valid) {
        ScreenStyleSettings noise;
        noise.mode = static_cast<int>(ScreenStyleMode::Original); noise.outline = 0; noise.shadow = 0; noise.sketchDepth = 0;
        wanted.push_back({ViewKind::Noise, -1, noise});
    }
    const int parkTier = static_cast<int>(introStatic_ ? kPortalTiers - 1 : kPortalTiers) - 1;
    if (play) {
        const size_t tiers = introStatic_ ? kPortalTiers - 1 : kPortalTiers;
        for (size_t tier = 0; tier < tiers && wanted.size() < HIKARI::kGameProjectSceneCaptureMaximumActiveCount; ++tier) {
            wanted.push_back({ViewKind::Sharp, static_cast<int>(tier), portalStyle_});
        }
    } else {
        for (size_t i = 0; i < screens.size(); ++i) {
            const auto style = Effective(*screens[i].room);
            auto found = std::find_if(wanted.begin(), wanted.end(), [&](const View& v) { return v.tier < 0 && v.kind == screens[i].kind && SameStyle(v.style, style); });
            if (found == wanted.end()) {
                if (wanted.size() == HIKARI::kGameProjectSceneCaptureMaximumActiveCount) { continue; }
                wanted.push_back({screens[i].kind, -1, style}); found = wanted.end() - 1;
            }
            viewOf[i] = static_cast<int>(found - wanted.begin());
        }
    }
    // 今あるキャプチャは同じ種類（窓の種類と段）のものを前から順に使い回し、描き方だけを差し替える。要らなくなったものは消す。
    std::vector<bool> reused(views_.size(), false);
    for (auto& want : wanted) {
        for (size_t j = 0; j < views_.size(); ++j) {
            if (reused[j] || views_[j].kind != want.kind || views_[j].tier != want.tier) { continue; }
            reused[j] = true; want.handle = views_[j].handle; want.camera = views_[j].camera;
            want.width = views_[j].width; want.height = views_[j].height; want.flags = views_[j].flags;
            want.brightness = views_[j].brightness; want.parkLast = views_[j].parkLast;
            break;
        }
    }
    for (size_t j = 0; j < views_.size(); ++j) {
        if (reused[j]) { continue; }
        if (!captures.Destroy(views_[j].handle)) { status_ = "使わない 3D キャプチャを解放できない"; return false; }
        std::erase_if(bound_, [&](const auto& item) { return item.second.capture.value == views_[j].handle.value; });
    }
    views_ = std::move(wanted);
    ++captureFrame_;
    // 診断（環境変数 TD2_PORTAL_TRACE）：240 フレームごとに、描き直した覗き窓と前の絵を残した覗き窓の数。
    if (static const bool trace = std::getenv("TD2_PORTAL_TRACE") != nullptr; trace && captureFrame_ % 240 == 0) {
        std::fprintf(stderr, "[TD2 portals] redrawn=%llu kept=%llu (240 frames)\n", static_cast<unsigned long long>(portalRedrawn_),
                     static_cast<unsigned long long>(portalKept_));
        portalRedrawn_ = portalKept_ = 0;
    }
    std::vector<HIKARI::GameProjectSceneCaptureViewDesc> descs;
    redrawnRooms_.clear();
    for (auto& view : views_) {
        HIKARI::GameProjectSceneCaptureDesc desc{};
        // 段のキャプチャは視点を並べて描く（カメラ物体は作成と更新の判定にだけ使う）。窓は窓の舞台カメラで撮る。
        const auto& window = WindowOf(view.kind);
        desc.cameraObject = view.tier >= 0 ? cameras_[static_cast<size_t>(ViewKind::Sharp)].handle
                          : view.kind == ViewKind::Noise ? noiseCamera_.handle : cameras_[static_cast<size_t>(view.kind)].handle;
        desc.width = view.tier >= 0 ? kTierCapture[static_cast<size_t>(view.tier)][0] : window.width;
        desc.height = view.tier >= 0 ? kTierCapture[static_cast<size_t>(view.tier)][1] : window.height;
        // エフェクト（VFX）も舞台の一部として写す。覗き窓の無い段は止める（描かない）。
        descs.clear();
        const bool noise = introStatic_ && view.kind == ViewKind::Noise;
        const int density = view.tier >= 0 ? kTierTexels[static_cast<size_t>(view.tier)] : window.density;
        const double focus = view.tier >= 0 ? kPortalEyeCells * projection_.scale : window.distance > 0 ? window.distance : kStageCameraDistance;
        auto look = StyleDesc(view.style, view.style.brightness, density, projection_.scale, !noise && projection_.oblique > 0, focus, !noise && solid_);
        // 画面の閃光：明るさを一時的に上乗せする。描き方の組分け（SameStyle）には含めない。
        look.brightness = static_cast<float>(std::clamp(look.brightness * (1 + flash), 0.0, 4.0));
        if (view.tier >= 0) {
            // 描き直す覗き窓：操作中・切り替え中の部屋は毎フレーム、画に写っている画面は段の間隔ごと（部屋ごとに位相をずらして
            // 負荷をならす）、画の外へ出たばかりの画面は描かない。前のフレームに無かった・矩形が変わった画面と、明るさが
            // 変わったフレームは必ず描く。残す画面は前のフレームの絵のまま（エンジンの KeepPrevious）。
            static const bool keepOff = std::getenv("TD2_PORTAL_KEEP_OFF") != nullptr;   // 比べる時だけ（環境変数）：毎フレーム全部描く
            const bool sameLook = view.brightness == look.brightness && !keepOff && thinScreens_;
            const uint64_t every = kPortalRedrawEvery[static_cast<size_t>(view.tier)];
            for (const auto& portal : portals_) {
                if (portal.tier != view.tier) { continue; }
                auto portalView = PortalView(portal, shakeX, shakeY);
                auto& drawn = portalDrawn_[portal.room->regionId];
                const bool same = drawn.frame + 1 == captureFrame_ && drawn.tier == portal.tier && drawn.x == portal.x &&
                                  drawn.y == portal.y && drawn.width == portal.width && drawn.height == portal.height;
                const uint64_t phase = std::hash<std::string>{}(portal.room->regionId) % every;
                const bool due = portal.always || (portal.shown && (captureFrame_ + phase) % every == 0);
                if (same && sameLook && !due) {
                    portalView.flags |= HIKARI::GameProjectSceneCaptureViewFlags(HIKARI::GameProjectSceneCaptureViewFlag::KeepPrevious);
                    ++portalKept_;
                } else {
                    ++portalRedrawn_; redrawnRooms_.insert(portal.room->regionId);
                }
                drawn = {portal.tier, portal.x, portal.y, portal.width, portal.height, captureFrame_};
                descs.push_back(portalView);
            }
        }
        // 砂嵐の部屋がある間は、一番粗い段の右下に砂嵐の板を撮る視点を足す（板が毎フレーム動くので毎回描く）。
        if (view.tier == static_cast<int>(kSnowTier) && !snow_.empty() && !introStatic_) { descs.push_back(SnowView()); }
        view.brightness = look.brightness;
        // 一番粗い段は、下端の隅に何も無い所を撮る小さな視点を常に足す（覗き窓の無いテレビを結んでおく隅。キャプチャを
        // 止めずに描き続け、結んだ画面の材質を外さない）。ほかの段も覗き窓がある間は同じ隅の視点を足す。
        // 隅の視点は前のフレームにもあれば前の絵のまま残す。残す視点が一つでもあると、エンジンは描き方（素描と奥行きの
        // 効果）を描き直した視点の矩形だけに掛ける（無ければ段のキャプチャの全面に掛かり、空いた所の分まで重い）。
        // 診断（環境変数 TD2_RECT_STYLE_OFF）：隅の視点を残さず、描き方を全面に掛ける（比べるため）。
        static const bool rectStyle = std::getenv("TD2_RECT_STYLE_OFF") == nullptr;
        bool parkNow = false;
        if (view.tier >= 0 && (view.tier == parkTier || (rectStyle && !descs.empty()))) {
            Portal park{};
            park.room = portals_.empty() ? introRooms_.front() : portals_.front().room;
            park.tier = view.tier; park.x = 0; park.y = desc.height - kPortalParkRows; park.width = park.height = kPortalParkRows;
            // 視錐台の形は覗き窓と同じ（深度の範囲をそろえる）で、目だけを舞台から遠く離す。
            auto empty = PortalView(park, 0, 0);
            empty.eye = projection_.Point(kNoiseX * 2, kNoiseY * 2, kPortalEyeCells);
            if (rectStyle && view.parkLast) {
                empty.flags |= HIKARI::GameProjectSceneCaptureViewFlags(HIKARI::GameProjectSceneCaptureViewFlag::KeepPrevious);
            }
            descs.push_back(empty);
            parkNow = true;
        }
        const bool enabled = view.tier < 0 || !descs.empty();
        view.parkLast = parkNow && enabled;
        desc.flags = HIKARI::GameProjectSceneCaptureFlags(HIKARI::GameProjectSceneCaptureFlag::RenderVfx) |
                     (enabled ? HIKARI::GameProjectSceneCaptureFlags(HIKARI::GameProjectSceneCaptureFlag::Enabled) : 0u);
        if (!view.handle.IsValid()) {
            view.handle = captures.Create(desc);
            if (!view.handle.IsValid()) { status_ = "3D キャプチャを作れない（最大 4）"; return false; }
        } else if (view.width != desc.width || view.height != desc.height || view.camera != desc.cameraObject || view.flags != desc.flags) {
            // 大きさ・カメラ・止めるかが変わった時だけ更新する（毎フレーム更新すると GPU 側の再検証が走る）。
            if (!captures.Update(view.handle, desc)) { status_ = "3D キャプチャを更新できない"; return false; }
        }
        view.width = desc.width; view.height = desc.height; view.camera = desc.cameraObject; view.flags = desc.flags;
        if (view.tier >= 0 && !descs.empty() && !captures.SetViews(view.handle, descs.data(), static_cast<uint32_t>(descs.size()))) {
            status_ = "覗き窓の視点を送れない（SetViews の SDK 4.6 のエンジンが要る）"; return false;
        }
        if (!captures.SetStyle(view.handle, look)) { status_ = "画面の描き方を送れない（SetStyle v2 のエンジンが要る）"; return false; }
    }

    // 画面の割り当て：変わった画面だけやり直す（同じ物体・材質スロットなら、別のキャプチャへ移る時もエンジンが差し替える）。
    // 外れた画面と、材質スロットが変わった画面は先に外す。
    std::map<uint64_t, Binding> next;
    const auto bind = [&](const StageRoom& room, const View& view, const HIKARI::GameProjectSceneCaptureSourceRect& rect) {
        const auto screen = world.FindSceneObject(room.configuration.screenObject);
        if (screen.IsValid()) { next[screen.GetHandle().value] = {screen.GetHandle(), view.handle, std::string(kScreenSlot), rect, false, room.regionId}; }
    };
    if (play) {
        // 覗き窓：段のキャプチャの中の自分の矩形（余白の内側の部屋の範囲）。
        for (const auto& portal : portals_) {
            const auto view = std::find_if(views_.begin(), views_.end(), [&](const View& v) { return v.tier == portal.tier; });
            if (view == views_.end()) { continue; }
            const auto& r = portal.room->Region();
            const double density = kTierTexels[static_cast<size_t>(portal.tier)], w = view->width, h = view->height;
            bind(*portal.room, *view, {static_cast<float>((portal.x + portal.left) / w), static_cast<float>((portal.y + portal.top) / h),
                                       static_cast<float>(r.cols * density / w), static_cast<float>(r.rows * density / h)});
        }
        // 覗き窓の無いテレビ（画の外・数の上限）も外さず、一番粗い段の下端の隅を結んでおく。
        const auto park = std::find_if(views_.begin(), views_.end(), [&](const View& v) { return v.tier == parkTier; });
        if (park != views_.end()) {
            const double w = park->width, h = park->height;
            const HIKARI::GameProjectSceneCaptureSourceRect corner{
                0.0f, static_cast<float>((h - kPortalParkRows) / h), static_cast<float>(kPortalParkRows / w), static_cast<float>(kPortalParkRows / h)};
            for (const auto* room : introRooms_) {
                const auto screen = room->placed ? world.FindSceneObject(room->configuration.screenObject) : HIKARI::ObjectView{};
                if (screen.IsValid() && !next.contains(screen.GetHandle().value)) { bind(*room, *park, corner); }
            }
        }
        // 砂嵐の部屋：一番粗い段の右下の砂嵐の視点を結ぶ。テレビごとに矩形の中の少しずつ違う所を切り出し、並んだテレビが
        // 同じ砂嵐にならないようにする。
        const auto snowView = std::find_if(views_.begin(), views_.end(), [](const View& v) { return v.tier == static_cast<int>(kSnowTier); });
        if (!snow_.empty() && !introStatic_ && snowView != views_.end()) {
            const double w = snowView->width, h = snowView->height, sw = kSnowWidth * .7, sh = kSnowHeight * .7;
            for (const auto* room : introRooms_) {
                if (!room->placed || !snow_.contains(room->regionId)) { continue; }
                const uint64_t key = std::hash<std::string>{}(room->regionId);
                const double sx = kSnowLeft + (kSnowWidth - sw) * IntroHash(key, 3), sy = kSnowTop + (kSnowHeight - sh) * IntroHash(key, 4);
                bind(*room, *snowView, {static_cast<float>(sx / w), static_cast<float>(sy / h), static_cast<float>(sw / w), static_cast<float>(sh / h)});
            }
        }
    } else {
        for (size_t i = 0; i < screens.size(); ++i) {
            if (viewOf[i] < 0) { continue; }
            const auto& room = *screens[i].room; const auto& r = room.Region();
            const auto& view = views_[static_cast<size_t>(viewOf[i])];
            const auto& window = WindowOf(view.kind);
            bind(room, view, {static_cast<float>((room.x - window.x) / window.cols), static_cast<float>((room.y - window.y) / window.rows),
                              static_cast<float>(r.cols / window.cols), static_cast<float>(r.rows / window.rows)});
        }
    }
    // 保護は前のフレームのカメラで測った見え方で選ぶ。オープニングの砂嵐はその後で結び直す。
    ChooseProtected(next);
    ApplyIntro(world, next);
    // 窓の画布を貼っている画面は結ばない（外しもしない）。結んだ覚えからも外し、放された時に新しく結び直す。
    if (uiScreen_ != 0) { next.erase(uiScreen_); }
    for (const auto& [id, old] : bound_) {
        if (id == uiScreen_) { continue; }
        const auto found = next.find(id);
        if ((found == next.end() || found->second.slot != old.slot) && world.FindObject(old.object).IsValid()) {
            (void)captures.ClearMaterialBinding(old.capture, old.object);
        }
    }
    // 診断（環境変数 TD2_BIND_TRACE）：結び直した理由ごとの数（240 フレームごと）。新しく結ぶ・別のキャプチャへ移る・切り出しだけ
    // 動いた・保護だけ変わった。結び直すと画面の材質が書き換わり、影の元の並びが変わる。
    static const bool bindTrace = std::getenv("TD2_BIND_TRACE") != nullptr;
    static std::array<uint64_t, 4> bindReasons{};
    for (const auto& [id, binding] : next) {
        const auto old = bound_.find(id);
        if (old != bound_.end() && old->second.capture.value == binding.capture.value && old->second.slot == binding.slot &&
            std::memcmp(&old->second.rect, &binding.rect, sizeof(binding.rect)) == 0 && old->second.protect == binding.protect) { continue; }
        if (bindTrace) {
            ++bindReasons[old == bound_.end() ? 0 : old->second.capture.value != binding.capture.value ? 1
                          : std::memcmp(&old->second.rect, &binding.rect, sizeof(binding.rect)) != 0 ? 2 : 3];
        }
        HIKARI::GameProjectSceneCaptureMaterialRegionBindingDesc desc{};
        desc.capture = binding.capture; desc.modelObject = binding.object; desc.sourceRect = binding.rect;
        std::memcpy(desc.materialSlotName, binding.slot.c_str(), binding.slot.size() + 1);
        desc.flags = binding.protect ? HIKARI::GameProjectSceneCaptureMaterialBindingFlags(HIKARI::GameProjectSceneCaptureMaterialBindingFlag::ProtectReadability)
                                     : HIKARI::GameProjectSceneCaptureMaterialBindingFlags(HIKARI::GameProjectSceneCaptureMaterialBindingFlag::None);
        if (!captures.BindMaterialRegion(desc)) { status_ = "画面の 3D 結び付けに失敗：" + binding.slot; bound_ = std::move(next); return false; }
    }
    if (bindTrace && captureFrame_ % 240 == 0) {
        std::fprintf(stderr, "[TD2 bind] new=%llu capture=%llu rect=%llu protect=%llu (240 frames)\n", static_cast<unsigned long long>(bindReasons[0]),
                     static_cast<unsigned long long>(bindReasons[1]), static_cast<unsigned long long>(bindReasons[2]), static_cast<unsigned long long>(bindReasons[3]));
        bindReasons = {};
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
    // 描いている部屋（覗き窓は余白込み）・窓の中のイベントだけを出す（境のエフェクトは両側のテレビに写る）。
    const auto drawn = [this](double x, double y) {
        const bool portal = std::any_of(portals_.begin(), portals_.end(), [&](const Portal& p) {
            const auto& r = p.room->Region();
            return x >= p.room->x - 1 && y >= p.room->y - 1 && x <= p.room->x + r.cols + 1 && y <= p.room->y + r.rows + 1;
        });
        return portal || std::any_of(windows_.begin(), windows_.end(), [&](const Window& w) { return w.Contains(x, y, x, y); });
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
        // 装置が切れる側は同じエフェクトを小さく出す。残影の土煙・光の火花は続けて出すので小さく。
        const double kind = item.kind == ScreenEffectKind::DeviceSwitch && item.reversed ? .7
                          : item.kind == ScreenEffectKind::EchoDust ? .5 : item.kind == ScreenEffectKind::BeamSpark ? .45 : 1.0;
        const double scale = fx.scale * entry.scale * kind;
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
            // 一回きりのエフェクトはイベントより先に終わることがある。終わった句柄は更新せずに手放す（再生し直しもしない）。
            auto kept = *found;
            if (kept.handle.IsValid() && !vfx.IsActive(kept.handle)) { kept.handle = {}; }
            if (kept.handle.IsValid() && (item.kind == ScreenEffectKind::BlinkTrail || item.kind == ScreenEffectKind::EchoAbsorb)) {
                HIKARI::GameProjectVfxUpdateRequest update{}; update.updateTransform = 1; update.transform = transform;
                (void)vfx.Update(kept.handle, update);
            }
            next.push_back(kept);
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
