#include "TD2Screen3D.h"
#include "TD2StageProjection.h"
#include "HIKARI/Project/ActionViews.h"
#include "HIKARI/Project/PresentationViews.h"
#include "HIKARI/Project/VfxView.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>

namespace TD2 {
namespace {
// 1 格の texel 数。清晰は 32（像素の描き方で 2×2 texel が一つのゲーム画素）、周りと未配置は 16。
constexpr int kSharpTexelsPerCell = 32, kWideTexelsPerCell = 16, kGamePixelsPerCell = 16;
// 演出の長さ（秒）。揺れと閃光は二乗で減衰し、瞬移の残像はこの時間で足元へ潰れて消える。
constexpr double kShakeSeconds = 0.28, kFlashSeconds = 0.22, kSmearSeconds = 0.12;
// 蹴り返しの残影を足元から刷り出す秒数（Rules の予告と同じ 0.16 秒）。回収の光跡が細って消える秒数。
constexpr double kEchoPrintSeconds = 0.16, kRecallTrailSeconds = 0.2;
// 特効は地形の正面より手前に置き、隠れないようにする。
constexpr double kFxDepth = 0.35;

bool SameStyle(const ScreenStyleSettings& a, const ScreenStyleSettings& b) {
    return a.mode == b.mode && a.outline == b.outline && a.shadow == b.shadow && a.sketchDepth == b.sketchDepth &&
           a.brightness == b.brightness;
}
// 区域の屏幕亮度（旧 2D の既定 2.2）を基準 1 として描き方の明るさに掛ける。
ScreenStyleSettings Effective(const StageRoom& room) {
    auto style = room.style;
    style.brightness = std::clamp(style.brightness * room.configuration.brightness / 2.2f, .0f, 4.0f);
    return style;
}
HIKARI::GameProjectFloat3 Srgb(uint32_t hex) {
    return {((hex >> 16) & 255) / 255.f, ((hex >> 8) & 255) / 255.f, (hex & 255) / 255.f};
}
// 画面専用の描き方。色は ScreenKit07 の配色と同じ値（輪郭・紙・鉛筆・残影の菫）。長さはゲーム画素で、
// 1 ゲーム画素の texel 数（pixelSize）は窓の精細度から決まる。
HIKARI::GameProjectSceneCaptureStyleDesc StyleDesc(const ScreenStyleSettings& style, int palette, float brightness, int density) {
    HIKARI::GameProjectSceneCaptureStyleDesc fx{};
    const auto mode = static_cast<ScreenStyleMode>(style.mode);
    fx.style = mode == ScreenStyleMode::Original ? HIKARI::GameProjectSceneCaptureStyle::Clean
             : mode == ScreenStyleMode::Sketch   ? HIKARI::GameProjectSceneCaptureStyle::Sketch
                                                 : HIKARI::GameProjectSceneCaptureStyle::Pixel;
    fx.pixelSize = (std::max)(1.0f, static_cast<float>(density) / kGamePixelsPerCell);
    fx.lineWidth = mode == ScreenStyleMode::Sketch ? 1.0f : std::clamp(style.outline, 0.0f, 4.0f);
    // 素描では shadowStrength が纵深の強さ（落ち影・暗い面・接地・空気・外形の太さ）。0 で平らな草図。
    fx.shadowStrength = std::clamp(mode == ScreenStyleMode::Sketch ? style.sketchDepth : style.shadow, 0.0f, 1.0f);
    // 光は左上・手前から（試作の焼き込みと同じ向き）。足場から右へ半格、下へ 1 格の帯が奥の壁に落ちる。
    fx.shadowOffsetX = kGamePixelsPerCell * 0.5f;
    fx.shadowOffsetY = kGamePixelsPerCell * 1.0f;
    fx.backgroundLuminance = 0.035f;
    fx.brightness = std::clamp(brightness, 0.0f, 4.0f);
    const bool teal = palette == 1;
    fx.lineColor = mode == ScreenStyleMode::Sketch ? Srgb(teal ? 0xDDD8C4 : 0xDDD5C1) : Srgb(teal ? 0x061112 : 0x0B1218);
    fx.paperColor = Srgb(teal ? 0x0D2A2B : 0x1A2733);
    // 残影の菫色。描き方はこの色相の鮮やかな物体を残影として横縞で抜く（主角の琥珀・危険の赤とは色相で分かれる）。
    fx.accentColor = Srgb(0xA879F0);
    return fx;
}
// 事件の種類 → 特効の槽。
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
    default: return ScreenFxSlot::Impact;
    }
}
// 初回再生の粒子配置は従来の seed を保つ。座標は再生済み判定には使わない。
uint32_t FxRandomSeed(const ScreenEffect& e) {
    const auto& r = e.kind == ScreenEffectKind::BlinkTrail ? e.source : e.rect;
    const auto seed = std::to_string(static_cast<int>(e.kind)) + "/" + std::to_string(e.tick) + "/" +
           std::to_string(static_cast<long long>(std::lround(r.x * 64))) + "/" + std::to_string(static_cast<long long>(std::lround(r.y * 64)));
    return static_cast<uint32_t>(std::hash<std::string>{}(seed));
}
// 特効の置き方（格単位で作った資産へ舞台の 1 格の大きさを掛ける）。落地・復活・存档点は足元、ほかは体や装置の中心。
// 瞬移は出発点の体の中心から +X を終点へ向け、X だけ実際の距離まで伸ばす。e は世界の格。
HIKARI::GameProjectTransform FxTransform(const ScreenEffect& e, const StageProjection& projection, double scale) {
    const auto& r = e.rect;
    const bool feet = e.kind == ScreenEffectKind::Land || e.kind == ScreenEffectKind::Respawn || e.kind == ScreenEffectKind::Checkpoint;
    HIKARI::GameProjectTransform transform{};
    transform.position = projection.Point(r.x + r.w * .5, feet ? r.y + r.h : r.y + r.h * .5, kFxDepth);
    const float size = static_cast<float>(projection.scale * scale);
    transform.scale = {size, size, size};
    if (e.kind == ScreenEffectKind::BlinkTrail) {
        const double sx = e.source.x + e.source.w * .5, sy = e.source.y + e.source.h * .5;
        const double dx = r.x + r.w * .5 - sx, dy = r.y + r.h * .5 - sy;
        transform.position = projection.Point(sx, sy, kFxDepth);
        transform.rotation = RotateZ(std::atan2(-dy, dx));
        transform.scale.x = static_cast<float>(projection.scale * (std::max)(std::sqrt(dx * dx + dy * dy), .05));
    }
    return transform;
}
// 窓の捕捉の大きさを決める。行数はカメラの正交の高さ（格）で固定し、列数は捕捉の縦横比から求め直す
// （切り出しの計算と実際に写る範囲を一致させる）。2048 texel を超える時は精細度を下げる。
void FitWindow(double rows, double wantedCols, int density, double& outCols, int& outDensity, uint32_t& width, uint32_t& height) {
    const double limit = HIKARI::kGameProjectSceneCaptureMaximumExtent;
    outDensity = (std::max)(1, (std::min)(density, static_cast<int>(std::floor(limit / (std::max)(rows, wantedCols)))));
    height = static_cast<uint32_t>(std::clamp(std::lround(rows * outDensity), 1l, static_cast<long>(limit)));
    width = static_cast<uint32_t>(std::clamp(std::lround(wantedCols * outDensity), 1l, static_cast<long>(limit)));
    outCols = rows * width / height;
}
} // namespace

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
    windows_ = {};
    return true;
}

// 窓ごとに別の正交カメラを取る（同じ物体だと姿勢を奪い合う）。清晰は必須、周り・未配置は無ければその窓を描かない。
bool Screen3DStage::AcquireCameras(HIKARI::WorldView& world, const ScreenStyleSettings& stage) {
    const uint64_t ids[kViewKinds]{stage.captureCameraObject, stage.wideCameraObject, stage.unplacedCameraObject};
    for (size_t kind = 0; kind < kViewKinds; ++kind) {
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

void Screen3DStage::PlaceWindows(const std::vector<const StageRoom*>& rooms, const StageFrame& frame, int maxCols) {
    windows_ = {};
    const StageRoom* active = nullptr;
    for (const auto* room : rooms) {
        if (room->placed && (!active || room->regionId == frame.activeRegion)) { active = room; }
        if (active && active->regionId == frame.activeRegion) { break; }
    }
    if (active) {
        const auto& r = active->Region();
        const double cx = active->x + r.cols * .5, cy = active->y + r.rows * .5;
        // 清晰：操作中の部屋一つ（左上をそろえる）。隣の電視は周りの窓が写しているので、部屋を移る時は
        // 同じ舞台を撮る別の捕捉へ結び直すだけで、画面の中身は途切れない。
        auto& sharp = windows_[static_cast<size_t>(ViewKind::Sharp)];
        sharp.rows = cameras_[static_cast<size_t>(ViewKind::Sharp)].orthographicHeight / projection_.scale;
        FitWindow(sharp.rows, maxCols, kSharpTexelsPerCell, sharp.cols, sharp.density, sharp.width, sharp.height);
        sharp.x = active->x; sharp.y = active->y;
        sharp.valid = true;
        // 周り：操作中の部屋を中心に 3 部屋分の列（行数はカメラの正交の高さ。清晰の 3 倍で 3 部屋分）。
        if (const auto& camera = cameras_[static_cast<size_t>(ViewKind::Wide)]; camera.handle.IsValid()) {
            auto& wide = windows_[static_cast<size_t>(ViewKind::Wide)];
            wide.rows = camera.orthographicHeight / projection_.scale;
            FitWindow(wide.rows, 3.0 * maxCols, kWideTexelsPerCell, wide.cols, wide.density, wide.width, wide.height);
            wide.x = cx - wide.cols * .5; wide.y = cy - wide.rows * .5;
            wide.valid = true;
        }
    }
    // 未配置：世界の外に並べた作者地図の外接矩形を、上下の中央で撮る。
    if (const auto& camera = cameras_[static_cast<size_t>(ViewKind::Unplaced)]; camera.handle.IsValid()) {
        double left = 0, top = 0, right = 0, bottom = 0; bool any = false;
        for (const auto* room : rooms) if (!room->placed) {
            const auto& r = room->Region();
            if (!any) { left = room->x; top = room->y; right = room->x + r.cols; bottom = room->y + r.rows; any = true; }
            left = (std::min)(left, double(room->x)); top = (std::min)(top, double(room->y));
            right = (std::max)(right, double(room->x + r.cols)); bottom = (std::max)(bottom, double(room->y + r.rows));
        }
        if (any) {
            auto& unplaced = windows_[static_cast<size_t>(ViewKind::Unplaced)];
            unplaced.rows = camera.orthographicHeight / projection_.scale;
            FitWindow(unplaced.rows, right - left, kWideTexelsPerCell, unplaced.cols, unplaced.density, unplaced.width, unplaced.height);
            unplaced.x = left; unplaced.y = (top + bottom) * .5 - unplaced.rows * .5;
            unplaced.valid = true;
        }
    }
}

void Screen3DStage::PlaceEditorWindow(const std::vector<const StageRoom*>& rooms) {
    windows_ = {};
    if (rooms.empty()) { return; }
    double left = rooms.front()->x, top = rooms.front()->y, right = left, bottom = top;
    for (const auto* room : rooms) {
        left = (std::min)(left, double(room->x)); top = (std::min)(top, double(room->y));
        right = (std::max)(right, double(room->x) + room->Region().cols);
        bottom = (std::max)(bottom, double(room->y) + room->Region().rows);
    }
    // 作者の並びと境界は保ち、舞台側の縮尺だけを調整する。実際の電視・カメラのレンズ・地図は変更しない。
    // 全画面で共有する最大 2048^2 の捕捉なので、電視ごとに描画パスを増やさない。
    auto& window = windows_[static_cast<size_t>(ViewKind::Sharp)];
    const double cols = (std::max)(1.0, right - left), rows = (std::max)(1.0, bottom - top);
    const double density = (std::min)(double(kSharpTexelsPerCell),
        double(HIKARI::kGameProjectSceneCaptureMaximumExtent) / (std::max)(cols, rows));
    window.width = static_cast<uint32_t>(std::clamp(std::ceil(cols * density),
        double(HIKARI::kGameProjectSceneCaptureMinimumExtent), double(HIKARI::kGameProjectSceneCaptureMaximumExtent)));
    window.height = static_cast<uint32_t>(std::clamp(std::ceil(rows * density),
        double(HIKARI::kGameProjectSceneCaptureMinimumExtent), double(HIKARI::kGameProjectSceneCaptureMaximumExtent)));
    // 丸めで区域の端が一列だけ窓の外へ落ちないよう、両軸を必ず覆う縦横比へ拡張する。
    window.rows = (std::max)(rows, cols * window.height / window.width);
    window.cols = window.rows * window.width / window.height;
    window.x = left; window.y = top; window.valid = true;
    window.density = (std::max)(1, static_cast<int>(std::lround(window.height / window.rows)));
    projection_.scale = cameras_[static_cast<size_t>(ViewKind::Sharp)].orthographicHeight / window.rows;
}

void Screen3DStage::Render(HIKARI::WorldView& world, const std::vector<StageRoom>& rooms, const StageFrame& frame,
                          const ScreenStyleSettings& stage, const ScreenFxSettings& fx) {
    const double now = frame.snapshot ? frame.snapshot->realTime : 0;
    const auto fade = [now](double start, double seconds) {
        const double age = now - start;
        return start < 0 || age < 0 || age >= seconds ? 0.0 : (1 - age / seconds) * (1 - age / seconds);
    };
    // 1. 描ける部屋と、窓の大きさの目安（世界に置いた部屋の最大の格数）。
    std::vector<const StageRoom*> valid;
    int maxCols = 0, maxRows = 0;
    for (const auto& room : rooms) {
        if (!room.style.enabled || !ValidateRegion(room.Region()).valid) { continue; }
        valid.push_back(&room);
        if (room.placed) { maxCols = (std::max)(maxCols, room.Region().cols); maxRows = (std::max)(maxRows, room.Region().rows); }
    }
    if (valid.empty() || !frame.snapshot) { Release(world); status_ = "3D显示已关闭或没有有效区域"; return; }
    if (!maxRows) for (const auto* room : valid) { maxCols = (std::max)(maxCols, room->Region().cols); maxRows = (std::max)(maxRows, room->Region().rows); }

    // 2. 舞台カメラと写し方。1 格の舞台上の長さは、清晰カメラの正交の高さを 1 部屋分の行で割った値。
    if (!AcquireCameras(world, stage)) { return; }
    projection_.scale = cameras_[static_cast<size_t>(ViewKind::Sharp)].orthographicHeight / maxRows;
    projection_.pitch = std::clamp(static_cast<double>(stage.pitch), 0.0, kMaxStagePitchDegrees) * kPi / 180.0;
    projection_.yaw = std::clamp(static_cast<double>(stage.yaw), -kMaxStageYawDegrees, kMaxStageYawDegrees) * kPi / 180.0;
    if (frame.authoring) { PlaceEditorWindow(valid); }
    else { PlaceWindows(valid, frame, maxCols); }

    // 3. 電視ごとの窓（清晰を優先）。どの窓にも入らない部屋は描かず、舞台にも出さない。
    std::vector<Assigned> shown;
    for (const auto kind : {ViewKind::Sharp, ViewKind::Wide, ViewKind::Unplaced}) {
        const auto& window = WindowOf(kind);
        for (const auto* room : valid) {
            const auto& r = room->Region();
            if ((!frame.authoring && room->placed != (kind != ViewKind::Unplaced)) ||
                std::any_of(shown.begin(), shown.end(), [room](const Assigned& a) { return a.room == room; }) ||
                !window.Contains(room->x, room->y, room->x + r.cols, room->y + r.rows)) { continue; }
            shown.push_back({room, kind});
        }
    }

    // 4. 舞台カメラ。画面の揺れは舞台カメラを画面の右・上へ小さくずらし、電視の中の絵だけを揺らす。
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
        pose.position.x += static_cast<float>(right[0] * shakeX + up[0] * shakeY);
        pose.position.y += static_cast<float>(right[1] * shakeX + up[1] * shakeY);
        pose.position.z += static_cast<float>(right[2] * shakeX + up[2] * shakeY);
        if (const auto camera = world.FindObject(cameras_[kind].handle); !camera.IsValid() || !camera.SetWorldTransform(pose).Accepted()) {
            status_ = "独立舞台镜头位置提交失败"; return;
        }
    }

    // 5〜7. 捕捉と画面の結び、事件、部品、特効。
    if (!SyncCaptures(world, shown, stage.palette, flashAmount_ * fade(flashStart_, kFlashSeconds))) { return; }
    if (frame.simulated) { worldEffects_.Observe(*frame.snapshot); } else { worldEffects_.Reset(); }
    SubmitParts(world, valid, shown, *frame.snapshot, stage.palette, fx.enabled && fx.smear, now, frame.simulated);
    PlayEffects(world, fx, frame.simulated, now);

    size_t ready = 0, failedCaptures = 0;
    const auto captures = world.GetSceneCapture();
    for (const auto& view : views_) {
        HIKARI::GameProjectSceneCaptureSnapshot snapshot{};
        if (captures.GetSnapshot(view.handle, snapshot)) {
            if (snapshot.status == HIKARI::GameProjectSceneCaptureStatus::Ready) { ++ready; }
            if (snapshot.status == HIKARI::GameProjectSceneCaptureStatus::Failed) { ++failedCaptures; }
        }
    }
    const auto& sharp = WindowOf(ViewKind::Sharp); const auto& wide = WindowOf(ViewKind::Wide);
    status_ = "Screen3D capture=" + std::to_string(views_.size()) + " sharp=" + std::to_string(sharp.width) + "x" +
        std::to_string(sharp.height) + " wide=" + std::to_string(wide.width) + "x" + std::to_string(wide.height) +
        " ready=" + std::to_string(ready) + " gpuFailed=" + std::to_string(failedCaptures) + " screens=" + std::to_string(bound_.size()) +
        " rooms=" + std::to_string(shown.size()) + " models=" + std::to_string(accepted_) + " rejected=" + std::to_string(failed_) +
        " fxRejected=" + std::to_string(fxRejected_);
}

bool Screen3DStage::SyncCaptures(HIKARI::WorldView& world, const std::vector<Assigned>& screens, int palette, double flash) {
    const auto captures = world.GetSceneCapture();
    if (!captures.IsValid()) { status_ = "SceneCapture不可用"; return false; }
    // 窓と描き方（屏幕亮度込み）の組ごとに捕捉一つ（清晰から順に、引擎の上限まで）。
    std::vector<View> wanted;
    std::vector<int> viewOf(screens.size(), -1);
    for (size_t i = 0; i < screens.size(); ++i) {
        const auto style = Effective(*screens[i].room);
        auto found = std::find_if(wanted.begin(), wanted.end(), [&](const View& v) { return v.kind == screens[i].kind && SameStyle(v.style, style); });
        if (found == wanted.end()) {
            if (wanted.size() == HIKARI::kGameProjectSceneCaptureMaximumActiveCount) { continue; }
            wanted.push_back({screens[i].kind, style}); found = wanted.end() - 1;
        }
        viewOf[i] = static_cast<int>(found - wanted.begin());
    }
    // 今ある捕捉は同じ窓の種類のものを前から順に使い回し、描き方だけを差し替える。要らなくなった捕捉は消す
    // （その結びは引擎が退役させる）。
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
        desc.cameraObject = cameras_[static_cast<size_t>(view.kind)].handle; desc.width = window.width; desc.height = window.height;
        // 特効（VFX）も舞台の一部として写す。
        desc.flags = HIKARI::GameProjectSceneCaptureFlags(HIKARI::GameProjectSceneCaptureFlag::Enabled |
                                                          HIKARI::GameProjectSceneCaptureFlag::RenderVfx);
        if (!view.handle.IsValid()) {
            view.handle = captures.Create(desc);
            if (!view.handle.IsValid()) { status_ = "3D捕获创建失败（最多4个）"; return false; }
        } else if (view.width != desc.width || view.height != desc.height || view.camera != desc.cameraObject) {
            // 大きさかカメラが変わった時だけ更新する（毎フレーム Update すると GPU 側の再検証が走る）。
            if (!captures.Update(view.handle, desc)) { status_ = "3D捕获更新失败"; return false; }
        }
        view.width = desc.width; view.height = desc.height; view.camera = desc.cameraObject;
        auto look = StyleDesc(view.style, palette, view.style.brightness, window.density);
        // 画面の閃光：明るさを一時的に上乗せする。描き方の組分け（SameStyle）には含めない。
        look.brightness = static_cast<float>(std::clamp(look.brightness * (1 + flash), 0.0, 4.0));
        if (!captures.SetStyle(view.handle, look)) { status_ = "屏幕风格提交失败，需要含SetStyle v2的新引擎"; return false; }
    }

    // 画面の結び：変わった画面だけ結び直す（同じ物体・材質槽の結びは、別の捕捉へ移る時も引擎が差し替える）。
    // 窓から外れた画面と、材質槽が変わった画面は先に解く。
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
        next[screen.GetHandle().value] = {screen.GetHandle(), view.handle, config.screenSlot, rect};
    }
    for (const auto& [id, old] : bound_) {
        const auto found = next.find(id);
        if ((found == next.end() || found->second.slot != old.slot) && world.FindObject(old.object).IsValid()) {
            (void)captures.ClearMaterialBinding(old.capture, old.object);
        }
    }
    for (const auto& [id, binding] : next) {
        const auto old = bound_.find(id);
        if (old != bound_.end() && old->second.capture.value == binding.capture.value && old->second.slot == binding.slot &&
            std::memcmp(&old->second.rect, &binding.rect, sizeof(binding.rect)) == 0) { continue; }
        HIKARI::GameProjectSceneCaptureMaterialRegionBindingDesc desc{};
        desc.capture = binding.capture; desc.modelObject = binding.object; desc.sourceRect = binding.rect;
        std::memcpy(desc.materialSlotName, binding.slot.c_str(), binding.slot.size() + 1);
        desc.flags = HIKARI::GameProjectSceneCaptureMaterialBindingFlags(HIKARI::GameProjectSceneCaptureMaterialBindingFlag::ProtectReadability);
        if (!captures.BindMaterialRegion(desc)) { status_ = "屏幕3D绑定失败：" + binding.slot; bound_ = std::move(next); return false; }
    }
    bound_ = std::move(next);
    return true;
}

void Screen3DStage::SubmitParts(HIKARI::WorldView& world, const std::vector<const StageRoom*>& rooms, const std::vector<Assigned>& shown,
                                const Rules::WorldSnapshot& snapshot, int palette, bool smear, double now, bool animate) {
    const auto models = world.GetPresentationModels();
    submitted_.clear(); frameParts_.clear(); accepted_ = failed_ = 0;
    const auto submit = [&](const PartRequest& request) {
        if (models.Submit(request)) { ++accepted_; submitted_.emplace_back(request.ownerObjectHandle, request.instanceId); }
        else { ++failed_; }
    };
    // 世界の占有（格）：舞台に並べた全部屋の地図と、実体のある Solid 装置。部屋の無い所は埋まり扱い。
    const auto solidAt = [&](int wx, int wy) {
        for (const auto* room : rooms) {
            const auto& r = room->Region();
            if (wx >= room->x && wy >= room->y && wx < room->x + r.cols && wy < room->y + r.rows) {
                return r.cells[CellIndex(r, wx - room->x, wy - room->y)] == '1';
            }
        }
        return true;
    };
    std::map<std::string_view, const StageRoom*> byRegion;
    for (const auto& item : shown) { byRegion.emplace(item.room->regionId, item.room); }

    // 背景と地形：部屋と周り 1 格の占有が変わった時だけ作り直し、毎フレームは作った要求をそのまま出す。
    std::vector<char> occupancy;
    for (const auto& item : shown) {
        const auto& room = *item.room; const auto& r = room.Region();
        const int width = r.cols + 2, height = r.rows + 2;
        occupancy.assign(static_cast<size_t>(width * height), 0);
        for (int j = 0; j < height; ++j) for (int i = 0; i < width; ++i) {
            const bool inside = i > 0 && j > 0 && i <= r.cols && j <= r.rows;
            occupancy[static_cast<size_t>(j * width + i)] =
                inside ? r.cells[CellIndex(r, i - 1, j - 1)] == '1' : solidAt(room.x + i - 1, room.y + j - 1);
        }
        for (const auto& device : snapshot.devices) if (device.kind == Rules::DeviceKind::Solid && device.solid) {
            const int left = static_cast<int>(std::floor(device.rect.x)) - room.x + 1, top = static_cast<int>(std::floor(device.rect.y)) - room.y + 1;
            const int right = static_cast<int>(std::ceil(device.rect.x + device.rect.w)) - room.x + 1;
            const int bottom = static_cast<int>(std::ceil(device.rect.y + device.rect.h)) - room.y + 1;
            for (int j = (std::max)(top, 0); j < (std::min)(bottom, height); ++j)
                for (int i = (std::max)(left, 0); i < (std::min)(right, width); ++i) { occupancy[static_cast<size_t>(j * width + i)] = 1; }
        }
        auto& cache = roomParts_[room.regionId];
        if (cache.occupancy != occupancy || cache.x != room.x || cache.y != room.y || cache.palette != palette ||
            cache.scale != projection_.scale || cache.pitch != projection_.pitch || cache.yaw != projection_.yaw) {
            cache = {occupancy, room.x, room.y, palette, projection_.scale, projection_.pitch, projection_.yaw, {}};
            StagePartBuilder builder{projection_, palette, room.owner.value, cache.parts};
            builder.Backdrop(room.x, room.y, r.cols, r.rows);
            builder.Terrain(room.x, room.y, r.cols, r.rows, occupancy);
            failed_ += builder.Failed();
        }
        for (const auto& request : cache.parts) { submit(request); }
    }
    std::erase_if(roomParts_, [&](const auto& item) { return !byRegion.contains(item.first); });

    // 装置と光線：描く部屋の装置だけ。状態で変わる部品は全部出し、表示だけを切り替える。
    // 通電・実体が変わった時刻は描かない部屋の装置も覚える（後で画面に入った時に古い変化を動かし直さない）。
    // 編集中（模擬していない）は時刻が進まないので、動きを付けず最終の姿にする。
    for (const auto& device : snapshot.devices) {
        auto [memory, fresh] = deviceMemory_.try_emplace(device.id, DeviceMemory{device.powered, device.solid});
        if (!fresh && (memory->second.powered != device.powered || memory->second.solid != device.solid)) {
            memory->second = {device.powered, device.solid, animate ? now : -1e9};
        }
        const auto slash = device.id.find('/');
        const auto room = byRegion.find(std::string_view(device.id).substr(0, slash));
        if (slash == std::string::npos || room == byRegion.end()) { continue; }
        StagePartBuilder builder{projection_, palette, room->second->owner.value, frameParts_};
        builder.Device(device, device.id == snapshot.checkpointId, now - memory->second.changed);
        if (device.kind == Rules::DeviceKind::Laser) {
            const auto beam = std::find_if(snapshot.beams.begin(), snapshot.beams.end(), [&](const Rules::BeamState& b) { return b.sourceId == device.id; });
            builder.Beam(device, beam == snapshot.beams.end() ? nullptr : &*beam);
        }
        failed_ += builder.Failed();
    }

    // 人物・残影・残影の予告・瞬移の残像：世界に一組ずつ（舞台カメラが持ち主）。部屋の境に立つ身体も一つで、
    // 両側の電視の切り出しにそれぞれ写る。
    StagePartBuilder global{projection_, palette, cameras_[static_cast<size_t>(ViewKind::Sharp)].handle.value, frameParts_};
    Rules::BodyState idle; idle.grounded = true;
    global.Actor(snapshot.player, snapshot.presentationPlayer, false, snapshot.realTime, snapshot.playerPresent);
    // 残影：いる時はその姿。蹴り返しの予告中（まだいない）は、出る所で姿を足元から刷り出す（予告の 0.16 秒で全高、
    // 生成の特効も同時に鳴る）。出る所が塞がって予告が長引く間は明滅させ、まだ実体が無いことを見せる。
    Rules::BodyState echoBody = snapshot.echo ? *snapshot.echo : idle;
    Rules::Rect echoRect = snapshot.echo ? snapshot.echo->rect : Rules::Rect{};
    bool echoShown = snapshot.echo.has_value();
    double echoGrow = 1;
    if (!snapshot.echo && snapshot.pendingEcho) {
        echoRect = *snapshot.pendingEcho; echoBody.facing = snapshot.pendingEchoFacing;
        echoGrow = std::clamp(snapshot.pendingEchoAge / kEchoPrintSeconds, .08, 1.0);
        echoShown = snapshot.pendingEchoAge < kEchoPrintSeconds || std::fmod(snapshot.pendingEchoAge, .1) < .06;
    }
    global.Actor(echoBody, echoRect, true, snapshot.realTime, echoShown, echoGrow);
    // 残影の印：残影（予告を含む）がいる間、主角の頭の上に菫の菱形を浮かべてゆっくり上下させる（空格で回収できる合図）。
    // 残影が別の電視にいても主角の上で分かる。
    global.Begin("echoMarker");
    const auto& body = snapshot.presentationPlayer;
    const bool marked = snapshot.playerPresent && !snapshot.player.dead && (snapshot.echo || snapshot.pendingEcho);
    if (marked) { global.Add("Echo_Marker", body.x + body.w * .5, body.y - .1 - .05 * (1 + std::sin(now * 4.0)), 1, 1, kActorDepth); }
    else { global.Add("Echo_Marker", kParkX, kParkY, 1, 1, kActorDepth, {}, -1, false); }
    // 回収の光跡：残影がいた所から主角へ伸びる菫の棒が、尾を主角の方へ縮めながら細って消える（吸い込まれる向き）。
    const ScreenEffect* recall = nullptr;
    for (const auto& item : worldEffects_.Items()) {
        const double age = (now - item.born) / kRecallTrailSeconds;
        if (item.kind == ScreenEffectKind::RecallTrail && age >= 0 && age < 1) { recall = &item; }
    }
    global.Begin("recallTrail");
    if (recall) {
        const double t = (now - recall->born) / kRecallTrailSeconds;
        const double sx = recall->source.x + recall->source.w * .5, sy = recall->source.y + recall->source.h * .5;
        const double ex = recall->rect.x + recall->rect.w * .5, ey = recall->rect.y + recall->rect.h * .5;
        const double tx = sx + (ex - sx) * t, ty = sy + (ey - sy) * t, length = std::hypot(ex - tx, ey - ty);
        global.Add("Echo_Streak", (tx + ex) * .5, (ty + ey) * .5, (std::max)(length, .01), (std::max)(1 - t, .05), kActorDepth + .05,
                   RotateZ(std::atan2(-(ey - ty), ex - tx)), 0, length > .05);
    } else {
        global.Add("Echo_Streak", kParkX, kParkY, 1, 1, kActorDepth + .05, {}, 0, false);
    }
    // 瞬移の残像：出発点と終点の間に主角の姿を二体、横に伸ばして置き、足元へ潰して消す。
    // 左右の向きごとに二体ずつ常に出し、使う向きの二体だけを見せる。
    const ScreenEffect* trail = nullptr;
    if (smear) for (const auto& item : worldEffects_.Items()) {
        const double age = (now - item.born) / kSmearSeconds;
        if (item.kind == ScreenEffectKind::BlinkTrail && age >= 0 && age < 1) { trail = &item; break; }
    }
    global.Begin("smear");
    for (const bool rightward : {true, false}) for (int i = 1; i <= 2; ++i) {
        const std::string_view pose = rightward ? "Ninja_Air0_R" : "Ninja_Air0_L";
        if (!trail) { global.Add(pose, kParkX, kParkY, 1, 1, kActorDepth - .08, {}, -1, false); continue; }
        const double t = (now - trail->born) / kSmearSeconds, k = i / 3.0;
        const double sx = trail->source.x + trail->source.w * .5, sb = trail->source.y + trail->source.h;
        const double dx = trail->rect.x + trail->rect.w * .5 - sx, db = trail->rect.y + trail->rect.h - sb, h = trail->rect.h;
        const bool used = (dx >= 0) == rightward;
        if (used) { global.Add(pose, sx + dx * k, sb + db * k, h * 1.3, (std::max)(h * (1 - t), .01), kActorDepth - .08); }
        else { global.Add(pose, kParkX, kParkY, 1, 1, kActorDepth - .08, {}, -1, false); }
    }
    failed_ += global.Failed();
    for (const auto& request : frameParts_) { submit(request); }
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
    // 描いている窓の中の事件だけを鳴らす（舞台は一続きなので、境の特効は両側の電視に写る）。
    const auto drawn = [this](double x, double y) {
        return std::any_of(windows_.begin(), windows_.end(), [&](const Window& w) { return w.Contains(x, y, x, y); });
    };
    // 新しい事件ごとに一度鳴らす。瞬移の伸びと主角への吸収は既存インスタンスの位置だけを更新する。
    std::vector<ActiveFx> next;
    for (const auto& item : worldEffects_.Items()) {
        if (item.kind == ScreenEffectKind::RecallTrail) { continue; }   // 光跡は部品で描く
        const auto& entry = fx.entries[static_cast<size_t>(SlotOf(item.kind))];
        const auto found = std::find_if(activeFx_.begin(), activeFx_.end(), [&item](const auto& v) { return v.eventId == item.id; });
        const auto& anchor = item.kind == ScreenEffectKind::BlinkTrail ? item.source : item.rect;
        if (!fx.enabled || entry.asset.IsEmpty() || !drawn(anchor.x + anchor.w * .5, anchor.y + anchor.h * .5)) {
            if (found != activeFx_.end()) {
                if (found->handle.IsValid()) { (void)vfx.Stop(found->handle); }
                // 画面へ戻った時も同じ事件を再起動しない。未再生の事件だけは入場時に鳴らせる。
                next.push_back({item.id, {}});
            }
            continue;
        }
        // 装置が切れる側は同じ特効を小さく鳴らす。
        const double scale = fx.scale * entry.scale * (item.kind == ScreenEffectKind::DeviceSwitch && item.reversed ? .7 : 1.0);
        const auto transform = FxTransform(item, projection_, scale);
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
        // 演出：残っている揺れ・閃光より強い時だけ上書きする（弱い事件が強い演出を打ち消さない）。
        const double shake = entry.shake * fx.shake, flash = entry.flash * fx.flash;
        if (shake > 0 && shake >= shakeAmplitude_ * fade(shakeStart_, kShakeSeconds)) { shakeAmplitude_ = shake; shakeStart_ = now; }
        if (flash > 0 && flash >= flashAmount_ * fade(flashStart_, kFlashSeconds)) { flashAmount_ = flash; flashStart_ = now; }
    }
    activeFx_ = std::move(next);
}
} // namespace TD2
