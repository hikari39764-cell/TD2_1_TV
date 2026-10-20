#include "TD2LightBudget.h"
#include "TD2ViewCull.h"
#include "HIKARI/Project/ActionViews.h"
#include "HIKARI/Project/ComponentViews.h"
#include "HIKARI/Project/GameFlowViews.h"
#include "HIKARI/Project/WorldQuery.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace TD2 {
namespace {
// 視野判定の余裕（度）。画に入る前に点けておく。
constexpr double kViewMarginDegrees = 12.0;
// 視野の外に出てから消すまでのフレーム数（縁を行き来する時のちらつき止め）。
constexpr uint32_t kOffDelayFrames = 20;

}

void LightBudgetSystem::Update(HIKARI::WorldView& world, const HIKARI::FrameContext&) {
    // 描いた姿勢は一フレーム前のもの。切り替え（硬い切り）の直後に新しい視野の光が一フレーム消えて見えないよう、
    // 操作中の姿勢の視野も合わせて見る。
    HIKARI::GameProjectCameraPose pose{}, control{};
    const bool playing = world.GetRuntimeSession().IsPlayActive() && world.GetCamera().GetRenderedPose(pose);
    const bool hasControl = playing && world.GetCamera().GetControlPose(control);
    if (!playing) {
        // 編集中は何もしない。
        lights_.clear(); status_ = "等待 Play"; return;
    }
    size_t total = 0, off = 0;
    const HIKARI::ComponentTypeId types[]{HIKARI::MakeComponentTypeId(kLightCullComponentType)};
    (void)world.ForEachComponents<LightCullComponent>(types, HIKARI::GameProjectWorldQueryAnchor::FirstRequiredTerm,
        [&](HIKARI::ObjectView object, LightCullComponent& cull) {
            const auto light = object.GetLight();
            HIKARI::GameProjectLightState state{};
            HIKARI::GameProjectTransform transform{};
            if (!light.TryGetState(state) || !object.GetWorldTransform(transform)) { return; }
            ++total;
            auto& tracked = lights_[object.GetHandle().value];
            // 作者が消した光は対象外。ここで消した光だけ点け直す。
            if (!state.enabled && !tracked.off) { return; }
            const double radius = state.range + cull.Margin();
            const bool visible = !cull.Cull() || SphereInView(pose, transform.position, radius, kViewMarginDegrees) ||
                                 (hasControl && SphereInView(control, transform.position, radius, kViewMarginDegrees));
            tracked.outsideFrames = visible ? 0 : tracked.outsideFrames + 1;
            const bool wantOff = !visible && tracked.outsideFrames >= kOffDelayFrames;
            if (wantOff != tracked.off) {
                HIKARI::GameProjectLightPatch patch{};
                patch.fieldMask = HIKARI::GameProjectLightPatchFields(HIKARI::GameProjectLightPatchField::Enabled);
                patch.enabled = wantOff ? 0u : 1u;
                if (light.ApplyPatch(patch).Accepted()) { tracked.off = wantOff; }
            }
            off += tracked.off ? 1 : 0;
        });
    auto next = "灯光预算：" + std::to_string(total - off) + "/" + std::to_string(total) + " 盏开启";
    if (next != status_) { std::fprintf(stderr, "[TD2 lights] on=%zu total=%zu\n", total - off, total); status_ = std::move(next); }
}
}
