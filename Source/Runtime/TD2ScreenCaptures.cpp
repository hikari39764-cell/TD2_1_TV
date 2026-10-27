// 舞台の画面の捕捉：覗き窓と編集の舞台カメラを段ごとのキャプチャへ結び、各テレビの画面の材質へ切り出しを渡す。
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
#include <functional>
#include <set>

namespace TD2 {
namespace {
using namespace Stage3D;
// 画に写っている覗き窓を描き直す間隔（フレーム）：段ごと。一番粗い段（ごく小さな画面）だけ一つおきにし、残りは毎フレーム。
// 間引いた画面はカメラの揺れの間も前の角度のまま止まり、時間方向の AA の履歴と混ざって揺れて見えるので、大きな画面は間引かない。
// 画の外へ出たばかりの画面（kPortalHold の間）は描き直さない。
constexpr std::array<uint64_t, kPortalTiers> kPortalRedrawEvery{1, 1, 1, 2};
bool SameStyle(const ScreenStyleSettings& a, const ScreenStyleSettings& b) {
    // 明るさは保存の丸めで僅かに違うことがあるので、誤差の範囲は同じ組にする（組が増えるとキャプチャが足りなくなる）。
    return a.mode == b.mode && a.outline == b.outline && a.shadow == b.shadow && a.sketchDepth == b.sketchDepth &&
           a.sketchFill == b.sketchFill && std::abs(a.brightness - b.brightness) < 1e-3f && a.atmosphere == b.atmosphere &&
           a.occlusion == b.occlusion && a.castLight == b.castLight && a.inkLine == b.inkLine && SameTone(a.tone, b.tone);
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
            const auto style = screens[i].room->EffectiveStyle();
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
}
