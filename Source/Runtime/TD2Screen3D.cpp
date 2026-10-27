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
    // 「砂嵐のカメラ」で砂嵐の窓を撮る。正投影の舞台カメラと同じ物体は使えない（姿勢を奪い合う）。
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
    portalStyle_ = valid.front()->EffectiveStyle();
    for (const auto* room : valid) { if (room->regionId == frame.activeRegion) { portalStyle_ = room->EffectiveStyle(); } }
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
