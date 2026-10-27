// 舞台の灯：写っている部屋に灯を付けて離さず、部屋が出入りする時は消えてから次の部屋で点ける。
#include "TD2Screen3D.h"
#include "TD2Screen3DShared.h"
#include "TD2StageProjection.h"
#include "TD2ScreenKitAssets.h"
#include "TD2StageLight.h"
#include "TD2Transform.h"
#include "HIKARI/Project/ActionViews.h"
#include "HIKARI/Project/WorldQuery.h"
#include "HIKARI/Project/PresentationViews.h"
#include <optional>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>

namespace TD2 {
using namespace Stage3D;

void Screen3DStage::SyncStageLights(HIKARI::WorldView& world, const std::vector<Assigned>& shown, double now) {
    // 灯は部屋に付けて離さない：その部屋が写っている間は同じ灯が照らし続ける（並び順で灯を配ると、部屋が出入りする
    // たびに灯が別の部屋へ飛び、写っている部屋の明るさが跳ねた）。空いた灯を新しく写った部屋へ、鮮明の窓の部屋から配る。
    // 灯を移す時は今の部屋で消えてから次の部屋で点く（それぞれ kFade 秒）。
    constexpr double kFade = .2;
    // 時計の無い時（編集中など）は待たずに切り替える。
    const double dt = now > 0 ? std::clamp(now - stageLightTime_, 0.0, .1) : kFade;
    stageLightTime_ = now;
    std::array<std::vector<HIKARI::ObjectView>, kStageLightRoleCount> pools;
    std::map<uint64_t, StageLightSettings> settings;
    const HIKARI::ComponentTypeId types[]{HIKARI::MakeComponentTypeId(kStageLightComponentType)};
    (void)world.ForEachComponents<StageLightComponent>(types, HIKARI::GameProjectWorldQueryAnchor::FirstRequiredTerm,
        [&](HIKARI::ObjectView object, StageLightComponent& light) {
            pools[static_cast<size_t>(std::clamp(light.Settings().role, 0, kStageLightRoleCount - 1))].push_back(object);
            settings[object.GetHandle().value] = light.Settings();
        });
    const auto shownRoom = [&](const std::string& id) {
        return std::find_if(shown.begin(), shown.end(), [&](const Assigned& a) { return a.room->regionId == id; });
    };
    for (auto& pool : pools) {
        std::sort(pool.begin(), pool.end(), [](const auto& a, const auto& b) { return a.GetHandle().value < b.GetHandle().value; });
        // 照らす部屋：写っている順（鮮明の窓が先）に灯の数まで。今照らしている部屋はその中にある限り灯を保つ。
        std::vector<std::string> wanted;
        for (size_t i = 0; lit_ && i < shown.size() && wanted.size() < pool.size(); ++i) { wanted.push_back(shown[i].room->regionId); }
        std::vector<StageLightTrack*> free;
        for (const auto& object : pool) {
            auto& t = stageLights_[object.GetHandle().value];
            if (!t.known) {
                HIKARI::GameProjectLightState state{};
                if (!object.GetLight().TryGetState(state)) { continue; }
                t.handle = object.GetHandle(); t.base = state.intensity; t.shadows = state.castsShadows != 0; t.known = true;
            }
            const auto keep = std::find(wanted.begin(), wanted.end(), t.target);
            if (!t.target.empty() && keep != wanted.end()) { wanted.erase(keep); } else { t.target.clear(); free.push_back(&t); }
        }
        for (size_t i = 0; i < free.size() && i < wanted.size(); ++i) { free[i]->target = wanted[i]; }
    }
    for (const auto& pool : pools) for (const auto& object : pool) {
        auto& t = stageLights_[object.GetHandle().value];
        if (!t.known) { continue; }
        // 今の部屋が画面から消えたら、その場で消す（写っていない所の光は見えない）。
        if (!t.room.empty() && shownRoom(t.room) == shown.end()) { t.room.clear(); t.weight = 0; }
        if (t.room != t.target) {
            t.weight = std::max(0.0, t.weight - dt / kFade);
            if (t.weight <= 0) { t.room = t.target; }
        } else if (!t.room.empty()) { t.weight = std::min(1.0, t.weight + dt / kFade); }
        const auto at = t.room.empty() ? shown.end() : shownRoom(t.room);
        const bool use = at != shown.end();
        if (use) {
            // 部屋の大きさに合わせて置き、部屋の正面の中心から奥 aimDepth の所へ向ける（灯の前は +Z）。
            const auto& s = settings[object.GetHandle().value];
            const auto& room = *at->room; const auto& r = room.Region();
            const double cx = room.x + r.cols * .5, cy = room.y + r.rows * .5;
            const auto from = projection_.Point(cx + s.offsetX * r.cols * .5, cy - s.offsetY * r.rows * .5, s.offsetZ);
            const auto aim = projection_.Point(cx, cy, -s.aimDepth);
            Vector3 forward{double(aim.x - from.x), double(aim.y - from.y), double(aim.z - from.z)};
            const double length = std::sqrt(forward[0] * forward[0] + forward[1] * forward[1] + forward[2] * forward[2]);
            if (length > 1e-6) {
                for (auto& v : forward) { v /= length; }
                const Vector3 worldUp = std::abs(forward[1]) > .99 ? Vector3{0, 0, 1} : Vector3{0, 1, 0};
                auto side = Cross(worldUp, forward);
                const double sl = std::sqrt(side[0] * side[0] + side[1] * side[1] + side[2] * side[2]);
                for (auto& v : side) { v /= sl; }
                HIKARI::GameProjectTransform pose{};
                pose.position = from;
                pose.rotation = QuaternionFromBasis(side, Cross(forward, side), forward);
                pose.scale = {1, 1, 1};
                (void)object.SetWorldTransform(pose);
            }
        }
        // 強さは作者の値に滑らかな重みを掛ける。変わった時だけ送る。
        const double w = Math::SmoothStep(t.weight);
        const float intensity = static_cast<float>(t.base * w);
        const bool enabled = use && w > 0;
        HIKARI::GameProjectLightPatch patch{};
        if (enabled != t.enabled) {
            patch.fieldMask |= HIKARI::GameProjectLightPatchFields(HIKARI::GameProjectLightPatchField::Enabled);
            patch.enabled = enabled ? 1u : 0u;
        }
        // 舞台は場景キャプチャにしか写らず、キャプチャには局所光の影が出ない。毎フレーム動く灯の影を描かないよう切る。
        if (!t.shadowOff) {
            patch.fieldMask |= HIKARI::GameProjectLightPatchFields(HIKARI::GameProjectLightPatchField::CastShadows);
            patch.castsShadows = 0u;
        }
        if (enabled && std::abs(intensity - t.sent) > t.base * .002f) {
            patch.fieldMask |= HIKARI::GameProjectLightPatchFields(HIKARI::GameProjectLightPatchField::Intensity);
            patch.intensity = intensity;
        }
        if (patch.fieldMask != 0 && object.GetLight().ApplyPatch(patch).Accepted()) {
            t.enabled = enabled; t.shadowOff = true;
            if (patch.fieldMask & HIKARI::GameProjectLightPatchFields(HIKARI::GameProjectLightPatchField::Intensity)) { t.sent = intensity; }
        }
    }
}

void Screen3DStage::ReleaseStageLights(HIKARI::WorldView& world) {
    // 消して作者の強さへ戻す（Play を止めた後の場景に半端な値を残さない）。
    for (auto& [id, t] : stageLights_) {
        if (!t.known) { continue; }
        if (const auto object = world.FindObject(t.handle); object.IsValid()) {
            HIKARI::GameProjectLightPatch patch{};
            patch.fieldMask = HIKARI::GameProjectLightPatchFields(HIKARI::GameProjectLightPatchField::Enabled) |
                              HIKARI::GameProjectLightPatchFields(HIKARI::GameProjectLightPatchField::Intensity) |
                              HIKARI::GameProjectLightPatchFields(HIKARI::GameProjectLightPatchField::CastShadows);
            patch.enabled = 0u; patch.intensity = t.base; patch.castsShadows = t.shadows ? 1u : 0u;
            (void)object.GetLight().ApplyPatch(patch);
        }
    }
    stageLights_.clear();
}
}
