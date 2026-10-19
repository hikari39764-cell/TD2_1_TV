#include "TD2Screen3D.h"
#include "TD2StageProjection.h"
#include "TD2ScreenKitAssets.h"
#include "HIKARI/Project/ActionViews.h"
#include "HIKARI/Project/GameFlowViews.h"
#include "HIKARI/Project/PresentationViews.h"
#include "HIKARI/Project/VfxView.h"
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>

namespace TD2 {
namespace {
// 1 マスの texel 数。部屋のマスが少なく画面の中で 1 マスが大きいので、鮮明は 48（ピクセルの描き方では 3×3 texel が
// 1 ゲーム画素）、周りと未配置は 32。
constexpr int kSharpTexelsPerCell = 48, kWideTexelsPerCell = 32, kGamePixelsPerCell = 16;
// 読みやすさの保護を一度に掛けられる画面の数（エンジンの kScreenReadabilityRegionCount と同じ）。
constexpr size_t kMaxProtectedScreens = 12;
// 舞台の部品の数の目安。工場の物体（約 400）と合わせ、エンジンの 1 フレームの上限 2048 に余裕を残す。
constexpr size_t kPartWarning = 1400;
// 周りの窓で世界全体を撮る時の 1 マスの最低 texel。これを下回る大きな世界は、見えている所の周りだけを撮る
// （下回ったまま全体を撮ると、鮮明の窓に入らない画面だけが目立ってぼける）。
constexpr int kMinWorldTexelsPerCell = 24;
// 大区域ごとに固定する周りの窓の 1 マスの最低 texel（これより粗くなる幅の大区域は、収まる部屋だけを覆う）。
constexpr int kMinAreaTexelsPerCell = 16;
// 演出の長さ（秒）。揺れと閃光は二乗で減衰し、瞬間移動の残像はこの時間で足元へ潰れて消える。
constexpr double kShakeSeconds = 0.28, kFlashSeconds = 0.22, kSmearSeconds = 0.12;
// 跳ね返りの残影を足元から現す秒数（ルールの予告と同じ 0.16 秒）と、回収の光跡が消える秒数。
constexpr double kEchoPrintSeconds = 0.16, kRecallTrailSeconds = 0.2;

bool SameStyle(const ScreenStyleSettings& a, const ScreenStyleSettings& b) {
    return a.mode == b.mode && a.outline == b.outline && a.shadow == b.shadow && a.sketchDepth == b.sketchDepth &&
           a.sketchFill == b.sketchFill && a.brightness == b.brightness && a.atmosphere == b.atmosphere &&
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
// cell は舞台の 1 マスの長さ、depth は立体（深度の効果を使う）。
HIKARI::GameProjectSceneCaptureStyleDesc StyleDesc(const ScreenStyleSettings& style, int palette, float brightness, int density,
                                                   double cell, bool depth) {
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
    // 紙は配色の壁より一段明るい暗色、鉛筆は配色の白（白舱は冷たい白、青瓷はわずかに緑の白）。
    const bool celadon = palette == 1;
    fx.lineColor = mode == ScreenStyleMode::Sketch ? Srgb(celadon ? 0xDDEAE4 : 0xE4EAEE) : Srgb(celadon ? 0x061112 : 0x0A0F14);
    fx.paperColor = Srgb(celadon ? 0x0E2425 : 0x18212B);
    if (depth) {
        // 立体の空間感：舞台カメラからの奥行きで前景（地形・装置・人物、奥行き 0.7 マスまで）と奥（後ろの壁 0.85 と
        // 窓の向こうの大広間）を分ける。奥は輪郭を付けず霧で沈め、物の周りの暗がりと、物の高さに応じた影を足す。
        const auto c = static_cast<float>(cell);
        fx.depthFocus = static_cast<float>(kStageCameraDistance);
        fx.backgroundDepth = .78f * c;
        fx.fogStrength = std::clamp(style.atmosphere, 0.0f, 1.0f);
        fx.fogDepth = 5.0f * c;
        // 霧は壁より明るい靄：奥の壁ほど明るく霞み、手前の柱と梁が暗い影絵として前に出る。
        fx.fogColor = mode == ScreenStyleMode::Sketch ? Srgb(celadon ? 0x2A4442 : 0x333F4C) : Srgb(celadon ? 0x2C403F : 0x3A4652);
        fx.occlusion = std::clamp(style.occlusion, 0.0f, 1.0f);
        fx.occlusionRadius = 3.0f;   // 物のすぐ周りだけ（大きいと壁の染みに見える）
        fx.occlusionDepth = .8f * c;
        fx.depthLine = mode == ScreenStyleMode::Original ? std::clamp(style.inkLine, 0.0f, 1.0f) : 0.0f;
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
    const bool feet = e.kind == ScreenEffectKind::Land || e.kind == ScreenEffectKind::Respawn || e.kind == ScreenEffectKind::Checkpoint;
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
// 窓のキャプチャの大きさを決める。行数はカメラの正投影の高さ（マス）で固定し、列数は縦横比から求め直す。
// 2048 texel を超える時は精細度を下げる。
void FitWindow(double rows, double wantedCols, int density, double& outCols, int& outDensity, uint32_t& width, uint32_t& height) {
    const double limit = HIKARI::kGameProjectSceneCaptureMaximumExtent;
    outDensity = (std::max)(1, (std::min)(density, static_cast<int>(std::floor(limit / (std::max)(rows, wantedCols)))));
    height = static_cast<uint32_t>(std::clamp(std::lround(rows * outDensity), 1l, static_cast<long>(limit)));
    // 列数は幅と高さの比で決まる。丸めで列が足りないと両端の部屋が入らず黒い画面になるので、
    // 幅は欲しい列数を必ず覆うよう切り上げる。
    width = static_cast<uint32_t>(std::clamp(static_cast<long>(std::ceil(wantedCols * height / rows - 1e-6)), 1l, static_cast<long>(limit)));
    outCols = rows * width / height;
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
    windows_ = {};
    return true;
}

// 窓ごとに別の正投影カメラを使う（同じ物体だと姿勢を奪い合う）。鮮明は必須、周り・未配置は無ければ描かない。
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
    const auto find = [&](std::string_view id) -> const StageRoom* {
        for (const auto* room : rooms) { if (room->placed && !id.empty() && room->regionId == id) { return room; } }
        return nullptr;
    };
    const StageRoom* first = find(focus_[0]);
    if (!first) { first = find(frame.activeRegion); }
    if (!first) { for (const auto* room : rooms) { if (room->placed) { first = room; break; } } }
    // 鮮明：一部屋を中央に撮る。主窓は一番目立つ部屋、副窓（未配置用のカメラを Play 中だけ借りる）は二番目。
    // 目立つ二台が同じ密度で写るので、並んだ二台の画面の細かさがそろう。
    // Play 中は未配置の地図が世界に入らないので、未配置用のカメラはいつでも副窓に使える。
    const auto placeRoom = [&](ViewKind kind, const StageRoom* room) {
        const auto& camera = cameras_[static_cast<size_t>(kind)];
        if (!room || !camera.handle.IsValid()) { return; }
        auto& window = windows_[static_cast<size_t>(kind)];
        window.rows = camera.orthographicHeight / projection_.scale;
        FitWindow(window.rows, maxCols, kSharpTexelsPerCell, window.cols, window.density, window.width, window.height);
        const auto& r = room->Region();
        window.x = room->x + r.cols * .5 - window.cols * .5; window.y = room->y + r.rows * .5 - window.rows * .5;
        window.regionId = room->regionId;
        window.valid = true;
    };
    placeRoom(ViewKind::Sharp, first);
    if (const auto* second = find(focus_[1]); second && second != first) { placeRoom(ViewKind::Unplaced, second); }
    // 周り：世界に置いた全部屋を一枚で撮る固定の窓。窓を動かさないので、どのテレビも Play 中ずっと同じ割り当てのまま
    // （割り当てが変わるとエンジンが GPU のシーンを作り直して画が止まる）。
    // 世界が大きすぎて 1 マス kMinWorldTexelsPerCell texel を保てない時だけ、見えている部屋の周りの 8 部屋分を撮る。
    const auto& wideCamera = cameras_[static_cast<size_t>(ViewKind::Wide)];
    if (!wideCamera.handle.IsValid()) { return; }
    auto& wide = windows_[static_cast<size_t>(ViewKind::Wide)];
    wide.rows = wideCamera.orthographicHeight / projection_.scale;
    double left = 0, top = 0, right = 0, bottom = 0; bool any = false;
    for (const auto* room : rooms) if (room->placed) {
        const auto& r = room->Region();
        if (!any) { left = room->x; top = room->y; right = room->x + r.cols; bottom = room->y + r.rows; any = true; }
        left = (std::min)(left, double(room->x)); top = (std::min)(top, double(room->y));
        right = (std::max)(right, double(room->x + r.cols)); bottom = (std::max)(bottom, double(room->y + r.rows));
    }
    if (!any) { return; }
    FitWindow(wide.rows, right - left, kWideTexelsPerCell, wide.cols, wide.density, wide.width, wide.height);
    if (bottom - top <= wide.rows && wide.density >= kMinWorldTexelsPerCell) {
        wide.x = (left + right) * .5 - wide.cols * .5; wide.y = (top + bottom) * .5 - wide.rows * .5;
        wide.valid = true;
        return;
    }
    // 大区域ごとに固定する窓：操作中の部屋から近い順に、同じ大区域の部屋を窓に収まるだけ集め、その外接を覆う位置に置く。
    // 同じ大区域の中で部屋を移っても、操作中の部屋が窓に入っている間は窓を動かさない（画面の割り当てを作り直さない）。
    // 預け場所（世界のずっと外）にいる部屋と、窓に収まらない遠い部屋は数えない（鮮明の窓に任せる）。
    if (const StageRoom* active = find(frame.activeRegion); active && !frame.areaRooms.empty()) {
        const auto inside = [&](const StageRoom* room) {
            const auto& r = room->Region();
            return wide.Contains(room->x, room->y, room->x + r.cols, room->y + r.rows);
        };
        const std::string key = frame.areaRooms.front();
        std::vector<const StageRoom*> members;
        for (const auto& id : frame.areaRooms) {
            const auto* room = find(id);
            if (room && room->x > -50000 && room->y > -50000) { members.push_back(room); }
        }
        const auto distance = [&](const StageRoom* room) {
            return std::abs((room->x + room->Region().cols * .5) - (active->x + active->Region().cols * .5)) +
                   std::abs((room->y + room->Region().rows * .5) - (active->y + active->Region().rows * .5));
        };
        std::sort(members.begin(), members.end(), [&](const StageRoom* a, const StageRoom* b) { return distance(a) < distance(b); });
        const double maxCols = static_cast<double>(HIKARI::kGameProjectSceneCaptureMaximumExtent) / kMinAreaTexelsPerCell;
        double l = active->x, t = active->y, rr = active->x + active->Region().cols, b = active->y + active->Region().rows;
        for (const auto* room : members) {
            const double nl = (std::min)(l, double(room->x)), nt = (std::min)(t, double(room->y));
            const double nr = (std::max)(rr, double(room->x + room->Region().cols)), nb = (std::max)(b, double(room->y + room->Region().rows));
            if (nb - nt > wide.rows || nr - nl > maxCols) { continue; }
            l = nl; t = nt; rr = nr; b = nb;
        }
        // 前の窓が同じ大区域のもので、今の部屋がまだ入っていれば、その窓を使い続ける。
        if (key == wideArea_ && stickyWide_.valid) {
            wide = stickyWide_;
            if (inside(active)) { return; }
        }
        FitWindow(wide.rows, (rr - l) + 2, kWideTexelsPerCell, wide.cols, wide.density, wide.width, wide.height);
        wide.x = (l + rr) * .5 - wide.cols * .5; wide.y = (t + b) * .5 - wide.rows * .5;
        wide.valid = true;
        wideArea_ = key; stickyWide_ = wide; wideCenter_ = active->regionId;
        return;
    }
    // 動く窓：中心は操作中の部屋（同じ大区域の部屋は隣り合うので窓に入る）。操作中の部屋が無い時（編集中など）は
    // 見えている部屋の重心に一番近い部屋。重心が別の部屋の方へ部屋の大きさの 1/4 以上近づくまで今の中心を保つ。
    const StageRoom* center = find(frame.activeRegion);
    if (!center) { center = first; }
    if (!find(frame.activeRegion) && !seen_.empty() && first) {
        double sx = 0, sy = 0, sw = 0;
        for (const auto* room : rooms) {
            const auto found = seen_.find(room->regionId);
            if (!room->placed || found == seen_.end()) { continue; }
            const double weight = found->second.view.coverage * (1 - found->second.blocked / 9.0);
            const auto& r = room->Region();
            sx += weight * (room->x + r.cols * .5); sy += weight * (room->y + r.rows * .5); sw += weight;
        }
        if (sw > 0) {
            const double cx = sx / sw, cy = sy / sw;
            const auto distance = [&](const StageRoom* room) {
                const auto& r = room->Region();
                return std::hypot((room->x + r.cols * .5 - cx) / r.cols, (room->y + r.rows * .5 - cy) / r.rows);
            };
            const StageRoom* nearest = first;
            for (const auto* room : rooms) { if (room->placed && distance(room) < distance(nearest)) { nearest = room; } }
            const auto* current = find(wideCenter_);
            center = current && distance(current) <= distance(nearest) + .25 ? current : nearest;
        }
    }
    if (!center) { return; }
    wideCenter_ = center->regionId;
    const auto& r = center->Region();
    // 一つの大区域（三部屋前後）と、上下の段や両隣の部屋が入る幅。
    FitWindow(wide.rows, 8.0 * maxCols, kWideTexelsPerCell, wide.cols, wide.density, wide.width, wide.height);
    wide.x = center->x + r.cols * .5 - wide.cols * .5; wide.y = center->y + r.rows * .5 - wide.rows * .5;
    wide.valid = true;
}

void PackRoomsForEditor(std::vector<StageRoom>& rooms) {
    constexpr int kGap = 2;          // 部屋の間（よその部屋の影や壁が写り込まない）
    constexpr int kGroupGap = 8;     // 組の間
    // 背の高い部屋から棚に並べる（同じ並びが毎フレーム同じになるよう、区域 ID でも並べる）。
    std::vector<size_t> order(rooms.size());
    for (size_t i = 0; i < order.size(); ++i) { order[i] = i; }
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        const auto& ra = rooms[a].Region(); const auto& rb = rooms[b].Region();
        if (ra.rows != rb.rows) { return ra.rows > rb.rows; }
        if (ra.cols != rb.cols) { return ra.cols > rb.cols; }
        return rooms[a].regionId < rooms[b].regionId;
    });
    // 面積の釣り合う二つの組へ（大きい部屋から、面積の小さい組へ）。組ごとに別の窓で撮り、1 マスの texel を倍近くにする。
    std::array<std::vector<size_t>, 2> groups;
    std::array<double, 2> area{};
    for (const size_t i : order) {
        const auto& r = rooms[i].Region();
        const size_t g = area[0] <= area[1] ? 0 : 1;
        groups[g].push_back(i); area[g] += double(r.cols + kGap) * (r.rows + kGap);
    }
    int offset = 0;
    for (size_t g = 0; g < groups.size(); ++g) {
        if (groups[g].empty()) { continue; }
        int widest = 0;
        for (const size_t i : groups[g]) { widest = (std::max)(widest, rooms[i].Region().cols + kGap); }
        // 棚の幅を変えて試し、縦横の長い方が一番短くなる幅で並べる。
        const auto shelve = [&](int width, bool apply) {
            int x = 0, y = 0, shelf = 0, right = 0;
            for (const size_t i : groups[g]) {
                const auto& r = rooms[i].Region();
                if (x > 0 && x + r.cols + kGap > width) { y += shelf; x = 0; shelf = 0; }
                if (apply) { rooms[i].x = offset + x; rooms[i].y = y; rooms[i].editorGroup = static_cast<int>(g); }
                x += r.cols + kGap; shelf = (std::max)(shelf, r.rows + kGap); right = (std::max)(right, x);
            }
            return std::pair{right, y + shelf};
        };
        const int start = (std::max)(widest, static_cast<int>(std::sqrt(area[g])));
        int best = start, bestSide = INT_MAX;
        for (int width = start; width <= 2 * start + widest; ++width) {
            const auto [w, h] = shelve(width, false);
            if ((std::max)(w, h) < bestSide) { bestSide = (std::max)(w, h); best = width; }
        }
        offset += shelve(best, true).first + kGroupGap;
    }
}

void Screen3DStage::PlaceEditorWindow(const std::vector<const StageRoom*>& rooms) {
    windows_ = {};
    if (rooms.empty()) { return; }
    // 組ごとの範囲。二つ目の組は未配置用のカメラで撮る（鮮明のカメラと正投影の高さが同じ時だけ。窓の行数が舞台の縮尺を
    // 共有するため）。使えなければ一枚の窓に全部を収める。
    struct Box { double left = 0, top = 0, right = 0, bottom = 0; bool any = false; };
    std::array<Box, 2> boxes{};
    const auto& sharp = cameras_[static_cast<size_t>(ViewKind::Sharp)];
    const auto& second = cameras_[static_cast<size_t>(ViewKind::Unplaced)];
    const bool split = second.handle.IsValid() && std::abs(second.orthographicHeight - sharp.orthographicHeight) < 1e-3;
    for (const auto* room : rooms) {
        auto& box = boxes[split && room->editorGroup == 1 ? 1 : 0];
        const double l = room->x, t = room->y, r = room->x + room->Region().cols, b = room->y + room->Region().rows;
        if (!box.any) { box = {l, t, r, b, true}; continue; }
        box.left = (std::min)(box.left, l); box.top = (std::min)(box.top, t);
        box.right = (std::max)(box.right, r); box.bottom = (std::max)(box.bottom, b);
    }
    // 窓は全部同じ行数（舞台の縮尺が共通）。一番大きい辺が 2048 texel に収まる精細度（上限は鮮明と同じ）で撮る。
    double rows = 1, side = 1;
    for (const auto& box : boxes) if (box.any) {
        rows = (std::max)(rows, box.bottom - box.top);
        side = (std::max)({side, box.right - box.left, box.bottom - box.top});
    }
    const double maxExtent = HIKARI::kGameProjectSceneCaptureMaximumExtent - 8.0;
    const double density = (std::min)(double(kSharpTexelsPerCell), maxExtent / side);
    const auto height = static_cast<uint32_t>(std::clamp(std::ceil(rows * density),
        double(HIKARI::kGameProjectSceneCaptureMinimumExtent), double(HIKARI::kGameProjectSceneCaptureMaximumExtent)));
    const double exact = height / rows;   // 丸めた後の 1 マスの texel（行数をちょうど rows にする）
    for (size_t k = 0; k < boxes.size(); ++k) {
        if (!boxes[k].any) { continue; }
        auto& window = windows_[static_cast<size_t>(k == 0 ? ViewKind::Sharp : ViewKind::Unplaced)];
        const double cols = (std::max)(1.0, boxes[k].right - boxes[k].left);
        window.width = static_cast<uint32_t>(std::clamp(std::ceil(cols * exact),
            double(HIKARI::kGameProjectSceneCaptureMinimumExtent), double(HIKARI::kGameProjectSceneCaptureMaximumExtent)));
        window.height = height;
        window.rows = rows; window.cols = window.width / exact;
        window.x = boxes[k].left; window.y = boxes[k].top; window.valid = true;
        window.density = (std::max)(1, static_cast<int>(std::lround(exact)));
    }
    projection_.scale = sharp.orthographicHeight / rows;
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
    // 立体：斜投影を焼いた部品の組を正対のカメラで撮る（天板と右の側面が見え、正面はマスにぴったり）。俯角・側角は使わない。
    projection_.oblique = stage.depthView ? kScreenKitOblique : 0.0;
    projection_.pitch = stage.depthView ? 0.0 : std::clamp(static_cast<double>(stage.pitch), 0.0, kMaxStagePitchDegrees) * kPi / 180.0;
    projection_.yaw = stage.depthView ? 0.0 : std::clamp(static_cast<double>(stage.yaw), -kMaxStageYawDegrees, kMaxStageYawDegrees) * kPi / 180.0;
    const int kit = stage.palette + (stage.depthView ? 2 : 0);
    travel_ = {frame.travelFrom, frame.travelTo};
    ++frame_;
    if (frame.authoring) { seen_.clear(); focus_ = {}; PlaceEditorWindow(valid); }
    else { Inspect(world, valid, frame); ChooseFocus(frame); PlaceWindows(valid, frame, maxCols); }
    // 視差：画面を見る角度の正接を 0.25 秒でなめらかに追う（見えていない画面は最後の角度のまま）。手持ちの揺れは残す。
    hall_ = stage.hall && stage.depthView;
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
                const double fx = (px - (room->x + r.cols * .5)) / (r.cols * .5), fy = ((room->y + r.rows * .5) - py) / (r.rows * .5);
                playerRoom = room->regionId;
                followX = (room->flipped ? -fx : fx) * kFollowX; followY = (room->flipped ? -fy : fy) * kFollowY;
                break;
            }
        }
        for (const auto& [id, seen] : seen_) {
            if (seen.view.coverage <= 0) { continue; }
            constexpr double kMaxTan = .3;    // これ以上斜めでも奥の層は寄せない（窓の縁の外へ出ない範囲）
            const bool here = id == playerRoom;
            const double tx = std::clamp(seen.view.eyeX + (here ? followX : 0.0), -kMaxTan, kMaxTan);
            const double ty = std::clamp(seen.view.eyeY + (here ? followY : 0.0), -kMaxTan, kMaxTan);
            auto [it, fresh] = parallax_.try_emplace(id, std::array<double, 2>{tx, ty});
            if (!fresh) { it->second[0] += (tx - it->second[0]) * follow; it->second[1] += (ty - it->second[1]) * follow; }
        }
        // 診断用（環境変数 TD2_PARALLAX_TRACE がある時だけ）：鮮明に撮る部屋の目の向きを 30 フレームごとに記録する。
        static const bool trace = std::getenv("TD2_PARALLAX_TRACE") != nullptr;
        if (trace && frame_ % 30 == 0) {
            if (const auto it = parallax_.find(focus_[0]); it != parallax_.end()) {
                std::fprintf(stderr, "[TD2 parallax-trace] n=%zu region=%s eye=%.3f,%.3f\n", frame_, focus_[0].c_str(), it->second[0], it->second[1]);
            }
        }
    }

    // 3. テレビごとの窓（鮮明を優先）。一部屋だけを撮る窓（Play の鮮明二枚）にはその部屋だけを、残りは窓に収まる部屋を写す。
    // どの窓にも入らない部屋は描かない。
    // 切り出しは窓を動かしたフレームに同じ窓で行う（キャプチャは同じフレームで主描画より先に描かれるため、
    // 遅らせると別の部屋が一瞬映る）。
    std::vector<Assigned> shown;
    for (const auto kind : {ViewKind::Sharp, ViewKind::Unplaced, ViewKind::Wide}) {
        const auto& window = WindowOf(kind);
        for (const auto* room : valid) {
            const auto& r = room->Region();
            if (std::any_of(shown.begin(), shown.end(), [room](const Assigned& a) { return a.room == room; })) { continue; }
            const bool fits = !window.regionId.empty() ? window.valid && room->regionId == window.regionId
                : (frame.authoring || room->placed == (kind != ViewKind::Unplaced)) && window.Contains(room->x, room->y, room->x + r.cols, room->y + r.rows);
            if (fits) { shown.push_back({room, kind}); }
        }
    }

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
        pose.position.x += static_cast<float>(right[0] * shakeX + up[0] * shakeY);
        pose.position.y += static_cast<float>(right[1] * shakeX + up[1] * shakeY);
        pose.position.z += static_cast<float>(right[2] * shakeX + up[2] * shakeY);
        if (const auto camera = world.FindObject(cameras_[kind].handle); !camera.IsValid() || !camera.SetWorldTransform(pose).Accepted()) {
            status_ = "独立舞台镜头位置提交失败"; return;
        }
    }

    // 5〜7. キャプチャと画面の割り当て、イベント、部品、エフェクト。
    if (!SyncCaptures(world, shown, stage.palette, flashAmount_ * fade(flashStart_, kFlashSeconds))) { return; }
    if (!frame.authoring) { TraceFrame(); }
    if (frame.simulated) { worldEffects_.Observe(*frame.snapshot); } else { worldEffects_.Reset(); }
    SubmitParts(world, valid, shown, *frame.snapshot, kit, fx.enabled && fx.smear, now, frame.simulated);
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

namespace {
using F3 = HIKARI::GameProjectFloat3;
// 四元数 q で v を回す。inverse は共役で戻す。
F3 Rotate(const HIKARI::GameProjectQuaternion& q, const F3& v, bool inverse) {
    const double w = q.w, x = inverse ? -q.x : q.x, y = inverse ? -q.y : q.y, z = inverse ? -q.z : q.z;
    const double cx = 2 * (y * v.z - z * v.y), cy = 2 * (z * v.x - x * v.z), cz = 2 * (x * v.y - y * v.x);
    return {static_cast<float>(v.x + w * cx + (y * cz - z * cy)), static_cast<float>(v.y + w * cy + (z * cx - x * cz)),
            static_cast<float>(v.z + w * cz + (x * cy - y * cx))};
}
F3 ToWorld(const HIKARI::GameProjectTransform& t, double x, double y, double z) {
    const auto r = Rotate(t.rotation, {static_cast<float>(x * t.scale.x), static_cast<float>(y * t.scale.y), static_cast<float>(z * t.scale.z)}, false);
    return {r.x + t.position.x, r.y + t.position.y, r.z + t.position.z};
}
F3 ToLocal(const HIKARI::GameProjectTransform& t, const F3& p) {
    const auto r = Rotate(t.rotation, {p.x - t.position.x, p.y - t.position.y, p.z - t.position.z}, true);
    const auto safe = [](float s) { return std::abs(s) > 1e-6f ? s : 1.f; };
    return {r.x / safe(t.scale.x), r.y / safe(t.scale.y), r.z / safe(t.scale.z)};
}
// 線分 a→b が軸平行の箱を通るか（両端の少し手前までで判定し、画面自体の面は数えない）。
bool SegmentHitsBox(const F3& a, const F3& b, const double lo[3], const double hi[3]) {
    const double p[3]{a.x, a.y, a.z}, d[3]{b.x - a.x, b.y - a.y, b.z - a.z};
    double t0 = 0, t1 = .97;
    for (int i = 0; i < 3; ++i) {
        if (std::abs(d[i]) < 1e-9) { if (p[i] < lo[i] || p[i] > hi[i]) { return false; } continue; }
        double ta = (lo[i] - p[i]) / d[i], tb = (hi[i] - p[i]) / d[i];
        if (ta > tb) { std::swap(ta, tb); }
        t0 = (std::max)(t0, ta); t1 = (std::min)(t1, tb);
        if (t0 > t1) { return false; }
    }
    return true;
}
// ゲームカメラの基底（前・右・上）と、画の半分の幅・高さの正接。点を画の座標（-1〜1）と奥行きへ移す。
struct ViewBasis {
    double eye[3]{}, f[3]{}, r[3]{}, u[3]{}, tx = 1, ty = 1;
    bool valid = false;
    // 奥行き 0.05 より手前の点は 0.05 として写す。
    std::pair<double, double> ToView(const F3& q, double* depth = nullptr) const {
        const double d[3]{q.x - eye[0], q.y - eye[1], q.z - eye[2]};
        const double raw = d[0] * f[0] + d[1] * f[1] + d[2] * f[2];
        if (depth) { *depth = raw; }
        const double z = (std::max)(raw, .05);
        return {(d[0] * r[0] + d[1] * r[1] + d[2] * r[2]) / (z * tx), (d[0] * u[0] + d[1] * u[1] + d[2] * u[2]) / (z * ty)};
    }
};
ViewBasis MakeBasis(const HIKARI::GameProjectCameraPose& pose) {
    ViewBasis b;
    const double f[3]{pose.target.x - pose.eye.x, pose.target.y - pose.eye.y, pose.target.z - pose.eye.z};
    const double fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    b.ty = std::tan(pose.verticalFovRadians * .5); b.tx = b.ty * (std::max)(static_cast<double>(pose.aspectRatio), .1);
    if (fl < 1e-6 || !(b.ty > 0)) { return b; }
    for (int i = 0; i < 3; ++i) { b.f[i] = f[i] / fl; }
    const double r[3]{b.f[1] * pose.up.z - b.f[2] * pose.up.y, b.f[2] * pose.up.x - b.f[0] * pose.up.z, b.f[0] * pose.up.y - b.f[1] * pose.up.x};
    const double rl = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    if (rl < 1e-6) { return b; }
    for (int i = 0; i < 3; ++i) { b.r[i] = r[i] / rl; }
    b.u[0] = b.r[1] * b.f[2] - b.r[2] * b.f[1]; b.u[1] = b.r[2] * b.f[0] - b.r[0] * b.f[2]; b.u[2] = b.r[0] * b.f[1] - b.r[1] * b.f[0];
    b.eye[0] = pose.eye.x; b.eye[1] = pose.eye.y; b.eye[2] = pose.eye.z;
    b.valid = true;
    return b;
}
// テレビの筐体の見積もりの箱（ローカル・縮尺前）。画面の寸法から、縁を含めた幅、筐体の下から画面の上の縁の少し上まで、
// 画面の正面から奥まで。
void BodyBox(const ScreenSurface& s, double lo[3], double hi[3]) {
    lo[0] = s.centerX - s.width * .65; lo[1] = 0; lo[2] = s.centerZ - s.width * .8;
    hi[0] = s.centerX + s.width * .65; hi[1] = s.centerY + s.height * .75; hi[2] = s.centerZ;
}
}

ScreenOnCamera ProjectScreen(const HIKARI::GameProjectCameraPose& pose, const HIKARI::GameProjectTransform& tv, const ScreenSurface& s) {
    ScreenOnCamera out;
    const auto basis = MakeBasis(pose);
    if (!basis.valid) { return out; }
    // 向き：画面の法線（ローカル +Z）とカメラへの向きの余弦。裏向きは写らない扱い。
    const auto center = ToWorld(tv, s.centerX, s.centerY, s.centerZ);
    const auto normal = Rotate(tv.rotation, {0, 0, 1}, false);
    const double ex = basis.eye[0] - center.x, ey = basis.eye[1] - center.y, ez = basis.eye[2] - center.z;
    const double el = std::sqrt(ex * ex + ey * ey + ez * ez);
    out.facing = el > 1e-6 ? (normal.x * ex + normal.y * ey + normal.z * ez) / el : 0;
    out.depth = -(ex * basis.f[0] + ey * basis.f[1] + ez * basis.f[2]);
    if (out.facing <= .05 || out.depth <= .05) { return out; }
    // 画面の右・上の向きに対する目の向きの正接（画面の奥の層を見る角度）。
    const auto right = Rotate(tv.rotation, {1, 0, 0}, false), up = Rotate(tv.rotation, {0, 1, 0}, false);
    const double along = normal.x * ex + normal.y * ey + normal.z * ez;
    out.eyeX = (right.x * ex + right.y * ey + right.z * ez) / along;
    out.eyeY = (up.x * ex + up.y * ey + up.z * ez) / along;
    const auto c = basis.ToView(center);
    out.centerX = c.first; out.centerY = c.second;
    // 四隅を画へ写し、画の枠（-1〜1）で切り取った多角形の面積を画の面積（4）で割る。
    constexpr std::pair<double, double> kCorners[]{{-.5, -.5}, {.5, -.5}, {.5, .5}, {-.5, .5}};
    std::vector<std::pair<double, double>> poly;
    for (const auto& [a, b] : kCorners) { poly.push_back(basis.ToView(ToWorld(tv, s.centerX + a * s.width, s.centerY + b * s.height, s.centerZ))); }
    for (int edge = 0; edge < 4 && !poly.empty(); ++edge) {
        const int axis = edge / 2; const double sign = edge % 2 ? -1.0 : 1.0;   // x≤1, x≥-1, y≤1, y≥-1 の順
        const auto inside = [&](const std::pair<double, double>& q) { return sign * (axis ? q.second : q.first) <= 1; };
        std::vector<std::pair<double, double>> clipped;
        for (size_t i = 0; i < poly.size(); ++i) {
            const auto& a = poly[i]; const auto& b = poly[(i + 1) % poly.size()];
            if (inside(a)) { clipped.push_back(a); }
            if (inside(a) != inside(b)) {
                const double va = axis ? a.second : a.first, vb = axis ? b.second : b.first;
                const double t = (sign - va) / (vb - va);
                clipped.push_back({a.first + (b.first - a.first) * t, a.second + (b.second - a.second) * t});
            }
        }
        poly = std::move(clipped);
    }
    double area = 0;
    for (size_t i = 0; i < poly.size(); ++i) {
        const auto& a = poly[i]; const auto& b = poly[(i + 1) % poly.size()];
        area += a.first * b.second - b.first * a.second;
    }
    out.coverage = std::abs(area) * .5 / 4;
    return out;
}

void Screen3DStage::Inspect(HIKARI::WorldView& world, const std::vector<const StageRoom*>& rooms, const StageFrame& frame) {
    seen_.clear();
    HIKARI::GameProjectCameraPose pose{};
    if (frame.authoring || !world.GetRuntimeSession().IsPlayActive() || !world.GetCamera().GetRenderedPose(pose)) { return; }
    struct Item { const StageRoom* room; HIKARI::GameProjectTransform tv; };
    std::vector<Item> items;
    for (const auto* room : rooms) {
        HIKARI::GameProjectTransform tv{};
        const auto object = world.FindSceneObject(room->configuration.screenObject);
        if (room->placed && object.IsValid() && object.GetWorldTransform(tv)) { items.push_back({room, tv}); }
    }
    // 画面の点からカメラへの線が、他のテレビの箱（grow だけ太らせる）を通るか。
    const auto hidden = [&](const Item& item, double u, double v, double grow) {
        const auto& s = item.room->surface;
        const auto point = ToWorld(item.tv, s.centerX + u * s.width, s.centerY + v * s.height, s.centerZ);
        for (const auto& other : items) {
            if (other.room == item.room || other.room->configuration.screenObject == item.room->configuration.screenObject) { continue; }
            double lo[3], hi[3]; BodyBox(other.room->surface, lo, hi);
            for (int i = 0; i < 3; ++i) {
                const double scale = std::abs(i == 0 ? other.tv.scale.x : i == 1 ? other.tv.scale.y : other.tv.scale.z);
                const double g = scale > 1e-6 ? grow / scale : 0;
                lo[i] -= g; hi[i] += g;
            }
            // 画面の点がよその箱の中にあるのは箱の見積もりが大きすぎる時（テレビの背に立て掛けた小さな機械など）。遮りに数えない。
            const auto local = ToLocal(other.tv, point);
            if (local.x >= lo[0] && local.x <= hi[0] && local.y >= lo[1] && local.y <= hi[1] && local.z >= lo[2] && local.z <= hi[2]) { continue; }
            if (SegmentHitsBox(ToLocal(other.tv, pose.eye), local, lo, hi)) { return true; }
        }
        return false;
    };
    for (const auto& item : items) {
        Seen seen;
        seen.view = ProjectScreen(pose, item.tv, item.room->surface);
        if (seen.view.coverage > 0) {
            // 遮りの標本：画面の 3×3 の点のうち隠れている数（目立ち具合の割引に使う）。
            constexpr double kSamples[]{-.4, 0.0, .4};
            for (const double a : kSamples) for (const double b : kSamples) { seen.blocked += hidden(item, a, b, 0) ? 1 : 0; }
        }
        seen_[item.room->regionId] = seen;
    }
}

void Screen3DStage::ChooseFocus(const StageFrame& frame) {
    // 切り替え中は出る部屋と入る部屋を鮮明に保つ（経路はどの瞬間もどちらかが画の半分以上に入るように作ってある）。
    // 目立つ画面が替わるたびに窓を移すと、画面の細かさが行き来する。
    // 今入っている枠はそのまま、空いている枠へもう一部屋を入れる。
    if (!travel_[0].empty()) {
        auto next = focus_;
        for (const auto& id : travel_) {
            if (id.empty() || next[0] == id || next[1] == id) { continue; }
            auto& slot = next[0] != travel_[0] && next[0] != travel_[1] ? next[0] : next[1];
            slot = id;
        }
        focus_ = next;
        return;
    }
    // 目立ち具合：画に占める割合を、遮られた分だけ割り引く。ゲームカメラが無い時は操作中の部屋だけを鮮明にする。
    const auto score = [&](std::string_view id) {
        const auto found = seen_.find(id);
        return found == seen_.end() ? 0.0 : found->second.view.coverage * (1 - found->second.blocked / 9.0);
    };
    if (seen_.empty()) { focus_ = {frame.activeRegion, std::string{}}; return; }
    constexpr double kVisible = .003, kChallenge = 1.3;
    std::vector<std::pair<double, std::string>> ranked;
    for (const auto& [id, seen] : seen_) { if (const double value = score(id); value > kVisible) { ranked.emplace_back(value, id); } }
    std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    // 今の二部屋は保ち、外の部屋が弱い方の 1.3 倍を超えて写った時だけ入れ替える。空いた枠は大きい順に埋め、
    // 一度埋まった枠は空にしない（副窓のキャプチャを作り直すと最初のフレームが空になり点滅する）。
    auto next = focus_;
    for (const auto& [value, id] : ranked) {
        if (id == next[0] || id == next[1]) { continue; }
        if (next[0].empty()) { next[0] = id; continue; }
        if (next[1].empty()) { next[1] = id; continue; }
        auto& weakest = score(next[0]) <= score(next[1]) ? next[0] : next[1];
        if (value > score(weakest) * kChallenge) { weakest = id; }
    }
    if (next[0].empty()) { std::swap(next[0], next[1]); }
    if (next[0].empty()) { next[0] = frame.activeRegion; }
    // 操作中の部屋が写っていれば必ず鮮明にする（周りの窓は遠い部屋を覆わないことがあり、外れると画面が黒くなる）。
    const auto active = seen_.find(frame.activeRegion);
    if (active != seen_.end() && active->second.view.coverage > 0 && next[0] != frame.activeRegion && next[1] != frame.activeRegion) {
        auto& weakest = next[1].empty() ? next[1] : score(next[0]) <= score(next[1]) ? next[0] : next[1];
        weakest = frame.activeRegion;
    }
    focus_ = next;
}

void Screen3DStage::ChooseProtected(std::map<uint64_t, Binding>& next) const {
    // ゲームカメラが無い時（編集中）は全画面に掛ける（エンジンが面積の大きい順に上限まで選ぶ）。
    if (seen_.empty()) { for (auto& [id, binding] : next) { binding.protect = true; } return; }
    // 保護の有無で画面の見え方が大きく変わるので、写っている画面は全部守る。
    // 一部が隠れていても守ってよい（エンジンが画素ごとに深度で確かめ、手前の物の上では外す）。
    const bool travelling = !travel_[0].empty();
    struct Candidate { uint64_t id; double coverage; bool was, travel; };
    std::vector<Candidate> eligible;
    for (auto& [id, binding] : next) {
        binding.protect = false;
        const auto seen = seen_.find(binding.regionId);
        if (seen == seen_.end()) { continue; }
        const auto old = bound_.find(id);
        const bool was = old != bound_.end() && old->second.protect;
        const bool travel = travelling && (binding.regionId == travel_[0] || binding.regionId == travel_[1]);
        const auto& v = seen->second;
        if (v.view.coverage <= 0 || v.view.facing <= .05) { continue; }
        eligible.push_back({id, v.view.coverage, was, travel});
    }
    // 上限を超える時だけ選ぶ：演出中は出る部屋・入る部屋を先に、次に守っていた画面、残りは大きい順。
    // 演出の外では、外の画面が選んだ中の最小の 1.6 倍を超えた時だけ入れ替える。
    std::sort(eligible.begin(), eligible.end(), [](const auto& a, const auto& b) {
        if (a.travel != b.travel) { return a.travel; }
        if (a.was != b.was) { return a.was; }
        return a.coverage > b.coverage;
    });
    std::vector<Candidate> chosen(eligible.begin(), eligible.begin() + static_cast<std::ptrdiff_t>((std::min)(eligible.size(), kMaxProtectedScreens)));
    for (size_t i = chosen.size(); i < eligible.size() && !travelling; ++i) {
        auto smallest = std::min_element(chosen.begin(), chosen.end(), [](const auto& a, const auto& b) { return a.coverage < b.coverage; });
        if (smallest != chosen.end() && eligible[i].coverage > smallest->coverage * 1.6) { *smallest = eligible[i]; }
    }
    for (const auto& candidate : chosen) { next[candidate.id].protect = true; }
}

void Screen3DStage::TraceFrame() {
    static const bool enabled = std::getenv("TD2_SCREEN_TRACE") != nullptr;
    traceFrame_ = frame_;
    if (!enabled) { return; }
    std::string state = "focus=" + focus_[0] + "," + focus_[1] + " wide=" + wideCenter_ + " protect=";
    std::vector<std::string> kinds, protect;
    int moved = 0;
    for (const auto& [id, binding] : bound_) {
        const auto view = std::find_if(views_.begin(), views_.end(), [&](const View& v) { return v.handle.value == binding.capture.value; });
        const char kind = view == views_.end() ? '?' : view->kind == ViewKind::Sharp ? 'S' : view->kind == ViewKind::Unplaced ? 'U' : 'W';
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
            reason = seen == seen_.end() ? "unseen" : seen->second.view.facing <= .05 ? "backface"
                : seen->second.view.coverage < .002 ? "small" : "limit-or-delay";
        }
        std::fprintf(stderr, "[TD2 protect-trace] n=%zu region=%s %s\n", traceFrame_, region.c_str(), reason.c_str());
    }
    for (const auto& [region, on] : traceProtect_) {
        if (on && !nowProtect.contains(region)) { std::fprintf(stderr, "[TD2 protect-trace] n=%zu region=%s unbound\n", traceFrame_, region.c_str()); }
    }
    traceProtect_ = std::move(nowProtect);
    for (const auto& p : protect) { state += p + " "; }
    // 画に写っているのに割り当てのない画面（周りの窓から外れている）。
    state += "visibleUnbound=";
    for (const auto& [region, seen] : seen_) {
        const bool bound = std::any_of(bound_.begin(), bound_.end(), [&](const auto& item) { return item.second.regionId == region; });
        if (!bound && seen.view.coverage > .0005) { state += region.substr(region.find('.') + 1) + "(" + std::to_string(static_cast<int>(seen.view.coverage * 1000)) + ") "; }
    }
    state += "bind=";
    for (const auto& k : kinds) { state += k + " "; }
    if (state != traceLast_ || moved) {
        std::fprintf(stderr, "[TD2 screen-trace] n=%zu moved=%d %s\n", traceFrame_, moved, state.c_str());
        traceLast_ = state;
    }
}

bool Screen3DStage::SyncCaptures(HIKARI::WorldView& world, const std::vector<Assigned>& screens, int palette, double flash) {
    const auto captures = world.GetSceneCapture();
    if (!captures.IsValid()) { status_ = "SceneCapture不可用"; return false; }
    // 窓と描き方（画面の明るさ込み）の組ごとにキャプチャを一つ作る（鮮明から順に、エンジンの上限まで）。
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
        desc.cameraObject = cameras_[static_cast<size_t>(view.kind)].handle; desc.width = window.width; desc.height = window.height;
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
        auto look = StyleDesc(view.style, palette, view.style.brightness, window.density, projection_.scale, projection_.oblique > 0);
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
    // 保護は前のフレームのカメラで測った見え方で選ぶ。
    ChooseProtected(next);
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

void Screen3DStage::SubmitParts(HIKARI::WorldView& world, const std::vector<const StageRoom*>& rooms, const std::vector<Assigned>& shown,
                                const Rules::WorldSnapshot& snapshot, int palette, bool smear, double now, bool animate) {
    const auto models = world.GetPresentationModels();
    submitted_.clear(); frameParts_.clear(); accepted_ = failed_ = 0;
    const auto submit = [&](const PartRequest& request) {
        if (models.Submit(request)) { ++accepted_; submitted_.emplace_back(request.ownerObjectHandle, request.instanceId); }
        else { ++failed_; }
    };
    // 世界の占有（マス）：舞台に並べた全部屋の地図と、実体のある Solid 装置。部屋の無い所は埋まり扱い。
    // 隣り合いの判定は世界の位置（linkX, linkY）で行う（編集中は描く位置を詰めて並べ直している）。
    const auto solidAt = [&](int wx, int wy) {
        for (const auto* room : rooms) {
            const auto& r = room->Region();
            if (wx >= room->linkX && wy >= room->linkY && wx < room->linkX + r.cols && wy < room->linkY + r.rows) {
                const int lx = wx - room->linkX, ly = wy - room->linkY;
                return r.cells[CellIndex(r, room->flipped ? r.cols - 1 - lx : lx, room->flipped ? r.rows - 1 - ly : ly)] == '1';
            }
        }
        return true;
    };
    // 回された部屋：その部屋の中の世界の点を、作者の向きの絵の上の点へ移す中心（無ければ null）。
    const auto flippedRoomAt = [&](double wx, double wy) -> const StageRoom* {
        for (const auto* room : rooms) {
            const auto& r = room->Region();
            if (room->flipped && wx >= room->x && wy >= room->y && wx < room->x + r.cols && wy < room->y + r.rows) return room;
        }
        return nullptr;
    };
    const auto mirrorFor = [&](StagePartBuilder& builder, const StageRoom* room) {
        if (room && room->flipped) builder.Mirror(room->x + room->Region().cols * .5, room->y + room->Region().rows * .5);
        else builder.NoMirror();
    };
    // そのマスに別の部屋があるか（外周の口がどこかへつながっているか）。
    const auto roomAt = [&](int wx, int wy, const StageRoom* self) {
        return std::any_of(rooms.begin(), rooms.end(), [&](const StageRoom* room) {
            const auto& r = room->Region();
            return room != self && wx >= room->linkX && wy >= room->linkY && wx < room->linkX + r.cols && wy < room->linkY + r.rows;
        });
    };
    std::map<std::string_view, const StageRoom*> byRegion;
    for (const auto& item : shown) { byRegion.emplace(item.room->regionId, item.room); }

    // 背景と地形：部屋と周り 1 マスの占有が変わった時だけ作り直し、毎フレームは作った要求をそのまま出す。
    std::vector<char> occupancy, linked;
    for (const auto& item : shown) {
        const auto& room = *item.room; const auto& r = room.Region();
        const int width = r.cols + 2, height = r.rows + 2;
        occupancy.assign(static_cast<size_t>(width * height), 0);
        linked.assign(static_cast<size_t>(width * height), 1);
        // 絵の上のマス (i, j)（周り 1 マス込み）が、規則の世界のどのマスか。回された部屋は点対称に読む。
        const auto worldCell = [&](int i, int j) {
            return room.flipped ? std::pair{room.linkX + r.cols - i, room.linkY + r.rows - j} : std::pair{room.linkX + i - 1, room.linkY + j - 1};
        };
        for (int j = 0; j < height; ++j) for (int i = 0; i < width; ++i) {
            const bool inside = i > 0 && j > 0 && i <= r.cols && j <= r.rows;
            const auto [wx, wy] = worldCell(i, j);
            occupancy[static_cast<size_t>(j * width + i)] = inside ? r.cells[CellIndex(r, i - 1, j - 1)] == '1' : solidAt(wx, wy);
            if (!inside) { linked[static_cast<size_t>(j * width + i)] = roomAt(wx, wy, &room); }
        }
        for (const auto& device : snapshot.devices) if (device.kind == Rules::DeviceKind::Solid && device.solid) {
            auto rect = device.rect;
            if (room.flipped) { rect.x = 2.0 * room.x + r.cols - rect.x - rect.w; rect.y = 2.0 * room.y + r.rows - rect.y - rect.h; }
            const int left = static_cast<int>(std::floor(rect.x)) - room.x + 1, top = static_cast<int>(std::floor(rect.y)) - room.y + 1;
            const int right = static_cast<int>(std::ceil(rect.x + rect.w)) - room.x + 1;
            const int bottom = static_cast<int>(std::ceil(rect.y + rect.h)) - room.y + 1;
            for (int j = (std::max)(top, 0); j < (std::min)(bottom, height); ++j)
                for (int i = (std::max)(left, 0); i < (std::min)(right, width); ++i) { occupancy[static_cast<size_t>(j * width + i)] = 1; }
        }
        // 窓の上下：操作の札にかからない所へ寄せる。
        const WindowSpan span = StagePartBuilder{projection_, palette, room.owner.value, frameParts_}.SpanAvoidingSigns(
            snapshot.devices, room.regionId, room.y, r.rows);
        windowSpans_[room.regionId] = span;
        auto& cache = roomParts_[room.regionId];
        if (cache.occupancy != occupancy || cache.linked != linked || cache.x != room.x || cache.y != room.y || cache.palette != palette ||
            cache.scale != projection_.scale || cache.pitch != projection_.pitch || cache.yaw != projection_.yaw || cache.compact != !animate ||
            cache.hall != hall_ || !(cache.window == span)) {
            cache = {occupancy, linked, room.x, room.y, palette, projection_.scale, projection_.pitch, projection_.yaw, !animate, hall_, span, {}};
            StagePartBuilder builder{projection_, palette, room.owner.value, cache.parts};
            builder.Compact(!animate);
            builder.UseHall(hall_);
            builder.Backdrop(room.x, room.y, r.cols, r.rows, span);
            builder.Terrain(room.x, room.y, r.cols, r.rows, occupancy);
            builder.Edges(room.x, room.y, r.cols, r.rows, occupancy, linked);
            failed_ += builder.Failed();
        }
        for (const auto& request : cache.parts) { submit(request); }
    }
    std::erase_if(roomParts_, [&](const auto& item) { return !byRegion.contains(item.first); });

    // 窓の向こうの大広間：外のカメラが画面を見る角度（なめらかにした正接）だけ奥の層をずらす。編集中は出さない。
    if (animate && hall_) {
        for (const auto& item : shown) {
            const auto& room = *item.room; const auto& r = room.Region();
            const auto eye = parallax_.find(room.regionId);
            double ex = eye != parallax_.end() ? eye->second[0] : 0.0, ey = eye != parallax_.end() ? eye->second[1] : 0.0;
            // 診断用（環境変数 TD2_PARALLAX_FORCE="x,y" がある時だけ）：全部の画面を同じ角度で見たことにする（比較の撮影用）。
            static const char* forced = std::getenv("TD2_PARALLAX_FORCE");
            if (forced) { (void)std::sscanf(forced, "%lf,%lf", &ex, &ey); }
            // 回されたテレビは画面の右・上が絵の左・下になる。
            if (room.flipped) { ex = -ex; ey = -ey; }
            StagePartBuilder builder{projection_, palette, room.owner.value, frameParts_};
            builder.UseHall(true);
            builder.Hall(room.regionId, room.x, room.y, r.cols, r.rows, windowSpans_[room.regionId], ex * parallaxGain_, ey * parallaxGain_);
            failed_ += builder.Failed();
        }
    }

    // 装置と光線：描く部屋の装置だけ。状態で変わる部品は全部出し、表示だけを切り替える。
    // 通電・実体が変わった時刻は描かない部屋の装置も覚える（後で画面に入った時に古い変化を動かし直さない）。
    // 編集中は時刻が進まないので、動きを付けず最終の姿にする。
    for (const auto& device : snapshot.devices) {
        // バネは使えるようになった時刻も覚える（満ちた瞬間に矢印を弾ませる）。
        const bool ready = device.kind == Rules::DeviceKind::Spring && device.cooldown <= 0;
        // チェックポイントは触れて有効になった時刻も覚える（旗を上げる）。
        const bool checkpoint = device.id == snapshot.checkpointId;
        auto [memory, fresh] = deviceMemory_.try_emplace(device.id, DeviceMemory{device.powered, device.solid, ready, checkpoint});
        if (!fresh && (memory->second.powered != device.powered || memory->second.solid != device.solid || memory->second.ready != ready ||
                       memory->second.checkpoint != checkpoint)) {
            memory->second = {device.powered, device.solid, ready, checkpoint, animate ? now : -1e9};
        }
        const auto slash = device.id.find('/');
        const auto room = byRegion.find(std::string_view(device.id).substr(0, slash));
        if (slash == std::string::npos || room == byRegion.end()) { continue; }
        StagePartBuilder builder{projection_, palette, room->second->owner.value, frameParts_};
        builder.SetTime(animate ? now : 0.0);
        mirrorFor(builder, room->second);
        builder.Device(device, device.id == snapshot.checkpointId, now - memory->second.changed);
        if (device.kind == Rules::DeviceKind::Laser) {
            const auto beam = std::find_if(snapshot.beams.begin(), snapshot.beams.end(), [&](const Rules::BeamState& b) { return b.sourceId == device.id; });
            builder.Beam(device, beam == snapshot.beams.end() ? nullptr : &*beam);
        }
        failed_ += builder.Failed();
    }

    // 人物・残影・予告・瞬間移動の残像：世界に一組ずつ（持ち主は舞台カメラ）。部屋の境に立つ体も一つで、
    // 両側のテレビの切り出しにそれぞれ写る。
    StagePartBuilder global{projection_, palette, cameras_[static_cast<size_t>(ViewKind::Sharp)].handle.value, frameParts_};
    Rules::BodyState idle; idle.grounded = true;
    const auto& shownPlayer = snapshot.presentationPlayer;
    mirrorFor(global, flippedRoomAt(shownPlayer.x + shownPlayer.w * .5, shownPlayer.y + shownPlayer.h * .5));
    // 骨と動きつきの人形を使う時は、焼いた静止の形を退けておく。
    global.Actor(snapshot.player, snapshot.presentationPlayer, false, snapshot.realTime, snapshot.playerPresent);
    global.NoMirror();
    // 残影：いる時はその姿。跳ね返りの予告中は、出る所で姿を足元から現す（0.16 秒で全高、
    // 生成のエフェクトも同時に出る）。出る所が塞がって予告が長引く間は明滅させる。
    Rules::BodyState echoBody = snapshot.echo ? *snapshot.echo : idle;
    Rules::Rect echoRect = snapshot.echo ? snapshot.echo->rect : Rules::Rect{};
    bool echoShown = snapshot.echo.has_value();
    double echoGrow = 1;
    if (!snapshot.echo && snapshot.pendingEcho) {
        echoRect = *snapshot.pendingEcho; echoBody.facing = snapshot.pendingEchoFacing;
        echoGrow = std::clamp(snapshot.pendingEchoAge / kEchoPrintSeconds, .08, 1.0);
        echoShown = snapshot.pendingEchoAge < kEchoPrintSeconds || std::fmod(snapshot.pendingEchoAge, .1) < .06;
    }
    mirrorFor(global, flippedRoomAt(echoRect.x + echoRect.w * .5, echoRect.y + echoRect.h * .5));
    global.Actor(echoBody, echoRect, true, snapshot.realTime, echoShown, echoGrow);
    mirrorFor(global, flippedRoomAt(shownPlayer.x + shownPlayer.w * .5, shownPlayer.y + shownPlayer.h * .5));
    // 残影の印：残影がいる間、プレイヤーの頭上に菫の菱形を浮かべる（スペースで回収できる合図）。
    // 残影が別のテレビにいても分かる。
    global.Begin("echoMarker");
    const auto& body = snapshot.presentationPlayer;
    const bool marked = snapshot.playerPresent && !snapshot.player.dead && (snapshot.echo || snapshot.pendingEcho);
    if (marked) { global.Add("Echo_Marker", body.x + body.w * .5, body.y - .1 - .05 * (1 + std::sin(now * 4.0)), 1, 1, projection_.ActorDepth()); }
    else { global.Add("Echo_Marker", kParkX, kParkY, 1, 1, projection_.ActorDepth(), {}, -1, false); }
    // 回収の光跡：残影がいた所からプレイヤーへ伸びる菫の棒が、プレイヤーの方へ縮みながら消える。
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
        global.Add("Echo_Streak", (tx + ex) * .5, (ty + ey) * .5, (std::max)(length, .01), (std::max)(1 - t, .05), projection_.ActorDepth() + .05,
                   RotateZ(std::atan2(-(ey - ty), ex - tx)), 0, length > .05);
    } else {
        global.Add("Echo_Streak", kParkX, kParkY, 1, 1, projection_.ActorDepth() + .05, {}, 0, false);
    }
    // 瞬間移動の残像：出発点と終点の間にプレイヤーの姿を二体、横に伸ばして置き、足元へ潰して消す。
    // 左右の向きごとに二体ずつ常に出し、使う向きだけを見せる。
    const ScreenEffect* trail = nullptr;
    if (smear) for (const auto& item : worldEffects_.Items()) {
        const double age = (now - item.born) / kSmearSeconds;
        if (item.kind == ScreenEffectKind::BlinkTrail && age >= 0 && age < 1) { trail = &item; break; }
    }
    global.Begin("smear");
    for (const bool rightward : {true, false}) for (int i = 1; i <= 2; ++i) {
        const std::string_view pose = rightward ? "Ninja_Air0_R" : "Ninja_Air0_L";
        if (!trail) { global.Add(pose, kParkX, kParkY, 1, 1, projection_.ActorDepth() - .08, {}, -1, false); continue; }
        const double t = (now - trail->born) / kSmearSeconds, k = i / 3.0;
        const double sx = trail->source.x + trail->source.w * .5, sb = trail->source.y + trail->source.h;
        const double dx = trail->rect.x + trail->rect.w * .5 - sx, db = trail->rect.y + trail->rect.h - sb, h = trail->rect.h;
        const bool used = (dx >= 0) == rightward;
        if (used) { global.Add(pose, sx + dx * k, sb + db * k, h * 1.3, (std::max)(h * (1 - t), .01), projection_.ActorDepth() - .08); }
        else { global.Add(pose, kParkX, kParkY, 1, 1, projection_.ActorDepth() - .08, {}, -1, false); }
    }
    global.NoMirror();
    failed_ += global.Failed();
    for (const auto& request : frameParts_) { submit(request); }
    // エンジンの 1 フレームの描画物体は 2048 まで（工場の物体と合わせて超えると画面全体の描画が止まる）。
    // 舞台の部品が多すぎる時は一度だけ知らせる。
    if (static bool warned = false; !warned && submitted_.size() > kPartWarning) {
        std::fprintf(stderr, "[TD2 Screen3D] stage parts=%zu exceed %zu; the engine frame may reject drawing\n", submitted_.size(), kPartWarning);
        warned = true;
    }
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
        // 演出：残っている揺れ・閃光より強い時だけ上書きする。
        const double shake = entry.shake * fx.shake, flash = entry.flash * fx.flash;
        if (shake > 0 && shake >= shakeAmplitude_ * fade(shakeStart_, kShakeSeconds)) { shakeAmplitude_ = shake; shakeStart_ = now; }
        if (flash > 0 && flash >= flashAmount_ * fade(flashStart_, kFlashSeconds)) { flashAmount_ = flash; flashStart_ = now; }
    }
    activeFx_ = std::move(next);
}
}
