#include "TD2MachineViews.h"
#include "TD2SignalCable.h"
#include "TD2Transform.h"
#include "HIKARI/Project/ActionViews.h"
#include "HIKARI/Project/GameFlowViews.h"
#include "HIKARI/Project/WorldQuery.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace TD2 {
namespace {
using F3 = HIKARI::GameProjectFloat3;
// 大きく回る転台で、カメラがテレビと一緒に回り込む割合（切り替えの前後で同じ向きに動き続ける）。
constexpr float kTurnOrbit = .24f;
// 信号線の材質の色：素の色と、通電した時の色・発光（前の光る版のモデルと同じ値）。点く・消える時は kCableFadeSeconds で移る。
constexpr F3 kCableBase{.2f, .045f, .12f}, kCableLitBase{1, .3f, .62f}, kCableGlow{1, .26f, .6f};
constexpr float kCableGlowStrength = 4, kCableFadeSeconds = .12f;
constexpr float kPiF = static_cast<float>(Math::kPi);
// 跳ぶ乗り物の段取り：進みの最初と最後の kHopRise で上り下りし、横へ運ぶのは kHopHold から 1 - kHopHold の間。
// 向きは横へ kHopTurn 運んでから回し始める。
constexpr float kHopRise = .3f, kHopHold = .18f, kHopTurn = .2f;
// 乗り物が浮いている高さのうち、カメラが一緒に上がる割合（残りは見上げる）。
constexpr float kRideCameraRise = .45f;
// 乗り物のカメラが横へ膨らみ始める進み（棚を越えるまでは膨らまない）。
constexpr float kRideBulgeAfter = .3f;

// 見る向き（ローカル +Z）と上から作る回転。
HIKARI::GameProjectQuaternion LookRotation(const F3& forward, const F3& up) {
    const auto z = Math::Normalize(ToVec3(forward));
    const auto x = Math::Normalize(Math::Cross(ToVec3(up), z));
    const auto y = Math::Cross(z, x);
    const double m00 = x[0], m01 = y[0], m02 = z[0], m10 = x[1], m11 = y[1], m12 = z[1], m20 = x[2], m21 = y[2], m22 = z[2];
    Math::Quat q;
    if (const double trace = m00 + m11 + m22; trace > 0) {
        const double k = .5 / std::sqrt(trace + 1);
        q = {(m21 - m12) * k, (m02 - m20) * k, (m10 - m01) * k, .25 / k};
    } else if (m00 > m11 && m00 > m22) {
        const double k = 2 * std::sqrt(1 + m00 - m11 - m22);
        q = {.25 * k, (m01 + m10) / k, (m02 + m20) / k, (m21 - m12) / k};
    } else if (m11 > m22) {
        const double k = 2 * std::sqrt(1 + m11 - m00 - m22);
        q = {(m01 + m10) / k, .25 * k, (m12 + m21) / k, (m02 - m20) / k};
    } else {
        const double k = 2 * std::sqrt(1 + m22 - m00 - m11);
        q = {(m02 + m20) / k, (m12 + m21) / k, .25 * k, (m10 - m01) / k};
    }
    return ToQuaternion(q);
}

// テレビが base から pose へ動いた分だけ、付いている物の位置をずらす。
F3 Follow(const F3& attached, const F3& pose, const F3& base) {
    return {attached.x + pose.x - base.x, attached.y + pose.y - base.y, attached.z + pose.z - base.z};
}
// 動けなかった時の揺れ。残りは 1→0 で 0.35 秒で消える。
double ShakeLeft(double elapsed) { return (std::max)(0.0, 1 - elapsed / .35); }
float ShakeOffset(double elapsed, double amplitude) { return static_cast<float>(amplitude * ShakeLeft(elapsed) * std::sin(elapsed * 70)); }

template <class Component, class Visit>
void ForEach(HIKARI::WorldView& world, std::string_view type, Visit&& visit) {
    const HIKARI::ComponentTypeId types[]{HIKARI::MakeComponentTypeId(type)};
    (void)world.ForEachComponents<Component>(types, HIKARI::GameProjectWorldQueryAnchor::FirstRequiredTerm, std::forward<Visit>(visit));
}
void Push(std::vector<HIKARI::GameProjectWorldTransformWrite>& writes, HIKARI::GameProjectObjectHandle object, const HIKARI::GameProjectTransform& pose) {
    writes.push_back({}); writes.back().object = object; writes.back().localTransform = pose;
}
void Commit(HIKARI::WorldView& world, const std::vector<HIKARI::GameProjectWorldTransformWrite>& writes, const char* tag) {
    if (writes.empty()) return;
    std::vector<HIKARI::GameProjectWorldResult> results(writes.size());
    if (!world.SetLocalTransforms(writes, results)) std::fprintf(stderr, "[TD2 %s] transform write failed count=%zu\n", tag, writes.size());
}
}

MachineSources MachineViews::Sync(HIKARI::WorldView& world, const RegionObjects& objects, std::vector<Rules::RegionSource>& regions) {
    MachineSources out;
    mechanisms_.clear(); slides_.clear(); stacks_.clear(); carousels_.clear(); cables_.clear(); movedPose_.clear();
    parked_ = 0;
    // 世界に置いていない回転柱の面は先に預け場所へ置き、その後の配置を他の仕掛けの基準にする。
    SyncCarousels(world, objects, regions, out);
    Placements placements;
    for (const auto& region : regions) placements[region.placement.regionId] = region.placement;
    SyncMechanisms(world, objects, placements, out);
    SyncSlides(world, objects, placements, out);
    SyncStacks(world, objects, placements, out);
    SyncCables(world);
    return out;
}

bool MachineViews::MemberPlaces(HIKARI::WorldView& world, const RegionObjects& objects, const Placements& placements,
                                const std::vector<std::string>& members, std::vector<MemberPlace>& out, std::string& missing) const {
    out.clear();
    for (const auto& member : members) {
        const auto placement = placements.find(member);
        const auto tv = objects.televisions.find(member);
        HIKARI::GameProjectTransform pose{};
        if (placement == placements.end() || !placement->second.worldPlaced || tv == objects.televisions.end() ||
            !world.FindObject(tv->second).GetLocalTransform(pose)) { missing = member; return false; }
        out.push_back({placement->second.worldX, placement->second.worldY, pose.position});
    }
    return true;
}

MachineViews::Member MachineViews::ReadMember(HIKARI::WorldView& world, const RegionObjects& objects, const std::string& region, bool withCamera) const {
    Member member;
    if (const auto tv = objects.televisions.find(region); tv != objects.televisions.end()) {
        member.tv = tv->second;
        if (const auto object = world.FindObject(member.tv); object.IsValid()) (void)object.GetLocalTransform(member.tvPose);
    }
    if (withCamera)
        if (const auto camera = objects.cameras.find(region); camera != objects.cameras.end() && camera->second)
            if (const auto object = world.FindSceneObject(camera->second); object.IsValid() && object.GetLocalTransform(member.cameraPose))
                member.camera = object.GetHandle();
    return member;
}

// ---------------------------------------------------------------- 機構
void MachineViews::SyncMechanisms(HIKARI::WorldView& world, const RegionObjects& objects, const Placements& placements, MachineSources& out) {
    ForEach<MechanismComponent>(world, kMechanismComponentType, [&](HIKARI::ObjectView, MechanismComponent& component) {
        MechanismAuthoring authored; std::string error;
        if (!component.BuildSource(authored, error)) {
            std::fprintf(stderr, "[TD2 configuration] disabled mechanism error=%s\n", error.c_str()); return;
        }
        auto& source = authored.source;
        // 区域のずれを、作者が置いた位置（または錨の区域の位置）から絶対の位置へ直す。
        for (const auto& [offsets, moves] : {std::pair{&authored.offsets, &source.moves}, std::pair{&authored.offsetsC, &source.movesC}}) {
            for (const auto& o : *offsets) {
                if (o.park) { const auto [x, y] = Park(); moves->push_back({o.regionId, x, y, o.flip}); continue; }
                const auto base = placements.find(o.anchor.empty() ? o.regionId : o.anchor);
                if (base == placements.end() || !base->second.worldPlaced) { error = "base region is not placed: " + (o.anchor.empty() ? o.regionId : o.anchor); break; }
                moves->push_back({o.regionId, base->second.worldX + o.dx, base->second.worldY + o.dy, o.flip});
            }
        }
        if (!error.empty()) { std::fprintf(stderr, "[TD2 configuration] disabled mechanism %s %s\n", source.id.c_str(), error.c_str()); return; }
        MechanismView view; view.cameraLift = component.CameraLift(); view.cameraPull = component.CameraPull();
        // 状態 1 と -1 の表に出てくる物体をまとめる（片方にしか無ければ、もう片方は元の姿勢のまま）。
        std::vector<uint64_t> ids;
        for (const auto* list : {&authored.deltas, &authored.deltasC}) for (const auto& delta : *list)
            if (std::find(ids.begin(), ids.end(), delta.object) == ids.end()) ids.push_back(delta.object);
        // 相対の姿勢：基準（状態 A か錨の物体）に、基準の向きで測ったずれと回転を載せる。中心があればそこを軸に回す。
        const auto deltaPose = [&world](const MachineObjectDelta& d, const HIKARI::GameProjectTransform& a, MechanismObject& item) {
            HIKARI::GameProjectTransform base = a;
            if (d.anchor) {
                const auto anchor = world.FindSceneObject(d.anchor);
                if (!anchor.IsValid() || !anchor.GetLocalTransform(base)) return std::optional<HIKARI::GameProjectTransform>{};
            }
            HIKARI::GameProjectTransform relative = a;
            relative.position = ToF3(d.position);
            relative.rotation = QFromEulerDeg(static_cast<float>(d.rotation[0]), static_cast<float>(d.rotation[1]), static_cast<float>(d.rotation[2]));
            auto b = Attach(base, relative);
            if (d.hasPivot) {
                const auto c = QRotate(base.rotation, ToF3(d.pivot));
                item.pivot = true; item.center = {base.position.x + c.x, base.position.y + c.y, base.position.z + c.z};
                const auto turned = QRotate(QMul(b.rotation, QConj(a.rotation)),
                    {a.position.x - item.center.x, a.position.y - item.center.y, a.position.z - item.center.z});
                b.position = {item.center.x + turned.x, item.center.y + turned.y, item.center.z + turned.z};
            }
            return std::optional<HIKARI::GameProjectTransform>{b};
        };
        for (const auto id : ids) {
            const auto object = world.FindSceneObject(id);
            MechanismObject item;
            if (!object.IsValid() || !object.GetLocalTransform(item.a)) {
                std::fprintf(stderr, "[TD2 mechanism] %s missing object=%llu\n", source.id.c_str(), static_cast<unsigned long long>(id));
                continue;
            }
            item.sceneId = id; item.handle = object.GetHandle(); item.b = item.c = item.a;
            for (const auto* list : {&authored.deltas, &authored.deltasC}) for (const auto& delta : *list) {
                if (delta.object != id) continue;
                const auto pose = deltaPose(delta, item.a, item);
                if (!pose) { std::fprintf(stderr, "[TD2 configuration] mechanism %s missing anchor=%llu\n", source.id.c_str(), static_cast<unsigned long long>(delta.anchor)); continue; }
                (list == &authored.deltas ? item.b : item.c) = *pose;
                item.from = static_cast<float>(delta.from); item.to = static_cast<float>(delta.to); item.lift = static_cast<float>(delta.lift);
            }
            // その場で画面の向きの軸に回すだけ（倒立）なら、区域カメラは動かさない（カメラまで逆さにしない）。
            const auto& qa = item.a.rotation; const auto& qb = item.b.rotation;
            const float dx = item.b.position.x - item.a.position.x, dy = item.b.position.y - item.a.position.y, dz = item.b.position.z - item.a.position.z;
            const auto delta = QMul(qb, QConj(qa));
            const float axis = std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
            const bool roll = (item.pivot || dx * dx + dy * dy + dz * dz < 1e-6f) && axis > .05f && std::abs(delta.y) / axis < .7f;
            // 動かすのが区域のテレビなら、その区域カメラもテレビに付いて一緒に動く（乗っている間も画面が正面に残る）。
            for (const auto& [region, handle] : objects.televisions) {
                if (roll || handle.value != item.handle.value) continue;
                const auto camera = objects.cameras.find(region);
                if (camera == objects.cameras.end() || !camera->second) continue;
                const auto cameraObject = world.FindSceneObject(camera->second);
                HIKARI::GameProjectTransform pose{};
                if (!cameraObject.IsValid() || !cameraObject.GetLocalTransform(pose)) continue;
                MechanismAttachment attachment{cameraObject.GetHandle(), view.objects.size(), Relative(item.a, pose)};
                // 状態 A と B の向きの差が 120°以上なら、カメラは途中で向こう側へ切り替える。
                // 乗り物は人を乗せて運ぶので切り替えず、テレビと一緒に回り込んで画面を正面に保つ。
                const float dot = std::abs(qa.x * qb.x + qa.y * qb.y + qa.z * qb.z + qa.w * qb.w);
                attachment.turn = !source.ride && 2.f * std::acos(std::clamp(dot, 0.f, 1.f)) >= 120.f * kPiF / 180.f;
                const auto& r = attachment.relative.position;
                attachment.reach = std::sqrt(r.x * r.x + r.y * r.y + r.z * r.z);
                if (source.ride) {
                    // 乗り物：カメラの視線の先（テレビまでの距離）を狙う点としてテレビに付けておく。
                    // 膨らむ向きは、動きの真ん中でテレビの画面が向く水平の向き。
                    attachment.ride = true;
                    attachment.aim = Mad(r, QRotate(attachment.relative.rotation, {0, 0, 1}), attachment.reach);
                    const auto mid = QRotate(QSlerp(item.a.rotation, item.b.rotation, .5f), r);
                    const float flat = std::sqrt(mid.x * mid.x + mid.z * mid.z);
                    if (flat > 1e-4f) attachment.side = {mid.x / flat, 0, mid.z / flat};
                }
                view.attached.push_back(attachment);
            }
            view.objects.push_back(item);
        }
        std::fprintf(stderr, "[TD2 mechanism] %s toggle=%d seconds=%.2f dwell=%.2f regions=%zu objects=%zu cameras=%zu\n", source.id.c_str(),
            source.toggle, source.seconds, source.dwell, source.moves.size(), view.objects.size(), view.attached.size());
        out.mechanisms.push_back(std::move(source)); mechanisms_.push_back(std::move(view));
    });
}

void MachineViews::UpdateMechanisms(const Frame& frame, Writes& writes) {
    const auto& runtimes = frame.exploration.Mechanisms();
    for (size_t i = 0; i < mechanisms_.size() && i < runtimes.size(); ++i) {
        auto& view = mechanisms_[i];
        const double progress = runtimes[i].progress;
        if (progress == view.written) continue;
        // 診断（環境変数 TD2_SPIKE_EVENTS）：機構が動き出した・止まった時を出す。
        if (static const bool spikeEvents = std::getenv("TD2_SPIKE_EVENTS") != nullptr; spikeEvents) {
            const bool wasRest = view.written == std::round(view.written), atRest = progress == std::round(progress);
            if (wasRest != atRest) {
                std::fprintf(stderr, "[TD2 mech] %s %s progress=%.2f\n", runtimes[i].source.id.c_str(), atRest ? "stop" : "start", progress);
            }
        }
        view.written = progress;
        // 両端でゆっくり止まる。転台は四元数で回し、吊り台は直線で運ぶ。天秤の状態 -1 側は c へ向かう。
        const bool sideC = progress < 0;
        // 物体ごとの進み：自分の区間の中だけで 0→1 に進む（戻りは同じ区間を逆にたどるので、順番も逆になる）。
        const auto eased = [&progress](const MechanismObject& object) {
            const float t = (static_cast<float>(std::abs(progress)) - object.from) / std::max(object.to - object.from, 1e-3f);
            return Math::SmoothStep(std::clamp(t, 0.f, 1.f));
        };
        std::vector<HIKARI::GameProjectTransform> poses;
        std::vector<float> eases, alongs;   // 物体ごとの進みと、跳ぶ物の横へ運ぶ進み
        for (size_t k = 0; k < view.objects.size(); ++k) {
            const auto& object = view.objects[k];
            const float e = eased(object);
            eases.push_back(e); alongs.push_back(e);
            const auto& end = sideC ? object.c : object.b;
            auto pose = object.a;
            if (object.lift != 0) {
                // 跳ぶ乗り物：先に真上へ浮き上がり、高いところで運んで回し、最後に降りる（棚や塔を上から越える）。
                // 負の高さは「先に落ちてから滑る」：最初に真下へ落ち、横へ運ぶにつれて残りの落差へなじむ（終わりで跳ね上がらない）。
                const float along = Math::SmoothStep(std::clamp((e - kHopHold) / (1.f - (object.lift > 0 ? 2.f : 1.f) * kHopHold), 0.f, 1.f));
                const float first = Math::SmoothStep(std::clamp(e / kHopRise, 0.f, 1.f));
                const float up = object.lift > 0 ? first * Math::SmoothStep(std::clamp((1.f - e) / kHopRise, 0.f, 1.f)) : first * (1.f - along);
                alongs.back() = along;
                pose.position = Lerp(object.a.position, end.position, along);
                pose.position.y += object.lift * up;
                // 向きは渡り始めてから変える（浮いてすぐ回ると、付いたカメラが棚の上へ振れる）。
                pose.rotation = QSlerp(object.a.rotation, end.rotation, Math::SmoothStep(std::clamp((along - kHopTurn) / (1.f - kHopTurn), 0.f, 1.f)));
            } else {
                pose.position = Lerp(object.a.position, end.position, e);
                pose.rotation = QSlerp(object.a.rotation, end.rotation, e);
            }
            if (object.pivot) {
                // 中心を動かさずに回す（途中も画面の中心が同じ所に留まる）。
                const auto& c = object.center;
                const auto turned = QRotate(QMul(pose.rotation, QConj(object.a.rotation)),
                    {object.a.position.x - c.x, object.a.position.y - c.y, object.a.position.z - c.z});
                pose.position = {c.x + turned.x, c.y + turned.y, c.z + turned.z};
            }
            poses.push_back(pose);
            // 区域カメラが付いたテレビは、下でカメラ自身の動きで運ぶ。
            const bool hasCamera = std::any_of(view.attached.begin(), view.attached.end(), [k](const auto& a) { return a.parent == k; });
            if (hasCamera) movedPose_[object.handle.value] = pose;
            else Moved(frame, object.handle.value, object.handle, object.a, pose);
            Push(writes, object.handle, pose);
        }
        // 付いて行くカメラ。
        // ・大きく回る転台（120°以上）：前半はテレビの回る向きへ回転の kTurnOrbit 割だけ一緒に回り込み、画面が真横を向いて
        //   表も裏も見えない折り返しの瞬間に転台の向こう側へ切り替え、後半も同じ向きへ回り続けて落ち着く（動きが途切れない）。
        //   前後とも画面へわずかに寄り、切り替えを動きの中に隠す。
        // ・それ以外（吊り台など）：一緒に運び、途中だけ上へ持ち上げて視線の後ろへ引く（両端では元の構図に戻る）。
        for (const auto& attached : view.attached) {
            const auto& object = view.objects[attached.parent];
            const float e = eases[attached.parent], arc = std::sin(e * kPiF);
            HIKARI::GameProjectTransform pose;
            if (attached.ride) {
                // 乗り物：自分のテレビが群れの一台として浮いて見えるよう、カメラは貼り付かずに群れの外から見る。
                // 始めの構図から終わりの構図へ進みながら、浮いた高さの一部だけ一緒に上がって少し見上げ、
                // 渡る途中は持ち上がって画面が向く側へ膨らむ（真ん中で群れの横から画面を正面に見る）。いつも自分の画面を狙う。
                const auto& tv = poses[attached.parent];
                const auto aimAt = Mad(tv.position, QRotate(tv.rotation, attached.aim), 1.f);
                const float along = alongs[attached.parent], w = Math::SmoothStep(along);
                const auto start = Attach(object.a, attached.relative).position;
                const auto end = Attach(object.b, attached.relative).position;
                // 浮いている高さ（両端を結ぶ線からの持ち上がり）の一部だけ一緒に上がり、残りは見上げる。
                const float hop = tv.position.y - Lerp(object.a.position, object.b.position, along).y;
                const float lift = view.cameraLift * std::sin(w * kPiF);
                const float out = view.cameraPull * std::sin(std::clamp((w - kRideBulgeAfter) / (1.f - kRideBulgeAfter), 0.f, 1.f) * kPiF);
                pose = Attach(tv, attached.relative);
                pose.position = Lerp(start, end, w);
                pose.position = {pose.position.x + attached.side.x * out, pose.position.y + lift + kRideCameraRise * hop, pose.position.z + attached.side.z * out};
                pose.rotation = LookRotation(Sub(aimAt, pose.position), {0, 1, 0});
            } else if (attached.turn) {
                // カメラを載せる仮のテレビの姿勢：前半は回転の一部、後半は終わりから同じ割だけ手前。
                auto carrier = poses[attached.parent];
                carrier.rotation = QSlerp(object.a.rotation, object.b.rotation, e < .5f ? kTurnOrbit * e : 1.f - kTurnOrbit * (1.f - e));
                pose = Attach(carrier, attached.relative);
                const auto forward = QRotate(pose.rotation, {0, 0, 1});
                const float push = attached.reach * .05f * (1.f - std::abs(2.f * e - 1.f));
                pose.position = {pose.position.x + forward.x * push, pose.position.y + forward.y * push, pose.position.z + forward.z * push};
            } else {
                pose = Attach(poses[attached.parent], attached.relative);
                const auto forward = QRotate(pose.rotation, {0, 0, 1});
                pose.position = {pose.position.x - forward.x * view.cameraPull * arc, pose.position.y + view.cameraLift * arc,
                                 pose.position.z - forward.z * view.cameraPull * arc};
            }
            Moved(frame, attached.handle.value, object.handle, Attach(object.a, attached.relative), pose);
            Push(writes, attached.handle, pose);
        }
    }
}

// ---------------------------------------------------------------- スロット棚
// 世界地図 1 マスあたりの 3D のずれ（u は X、v は Y）を、同じ行・同じ列のメンバーから先に、残りは斜めの差から解き、
// 空きスロットの 3D の位置を求める。列と行の番号は、スロットの世界地図の X・Y を小さい順に並べた順位。
bool MachineViews::DeriveSlideSlots(Rules::SlideSource& source, const std::vector<MemberPlace>& members, const std::vector<std::pair<int, int>>& empty,
                                    std::vector<F3>& poses, std::string& error) const {
    const auto& o = members.front();
    bool hasU = false, hasV = false;
    F3 u{}, v{};
    const auto diff = [&o](const MemberPlace& m) { return F3{m.position.x - o.position.x, m.position.y - o.position.y, m.position.z - o.position.z}; };
    for (const auto& m : members) {
        const int dx = m.x - o.x, dy = m.y - o.y; const auto d = diff(m);
        if (dx && !dy && !hasU) { u = {d.x / dx, d.y / dx, d.z / dx}; hasU = true; }
        if (dy && !dx && !hasV) { v = {d.x / dy, d.y / dy, d.z / dy}; hasV = true; }
    }
    for (const auto& m : members) {
        const int dx = m.x - o.x, dy = m.y - o.y; const auto d = diff(m);
        if (!dx || !dy) continue;
        if (hasU && !hasV) { v = {(d.x - u.x * dx) / dy, (d.y - u.y * dx) / dy, (d.z - u.z * dx) / dy}; hasV = true; }
        else if (hasV && !hasU) { u = {(d.x - v.x * dy) / dx, (d.y - v.y * dy) / dx, (d.z - v.z * dy) / dx}; hasU = true; }
    }
    std::vector<std::pair<std::pair<int, int>, F3>> slots;
    for (const auto& m : members) slots.push_back({{m.x, m.y}, m.position});
    for (const auto& [ex, ey] : empty) {
        if ((ex && !hasU) || (ey && !hasV)) { error = "cannot infer the 3D position of an empty slot from the members"; return false; }
        slots.push_back({{o.x + ex, o.y + ey}, {o.position.x + u.x * ex + v.x * ey, o.position.y + u.y * ex + v.y * ey, o.position.z + u.z * ex + v.z * ey}});
    }
    std::vector<int> xs, ys;
    for (const auto& [place, p] : slots) { xs.push_back(place.first); ys.push_back(place.second); }
    std::sort(xs.begin(), xs.end()); xs.erase(std::unique(xs.begin(), xs.end()), xs.end());
    std::sort(ys.begin(), ys.end()); ys.erase(std::unique(ys.begin(), ys.end()), ys.end());
    source.slots.clear(); poses.clear();
    for (const auto& [place, p] : slots) {
        const int col = static_cast<int>(std::lower_bound(xs.begin(), xs.end(), place.first) - xs.begin());
        const int row = static_cast<int>(std::lower_bound(ys.begin(), ys.end(), place.second) - ys.begin());
        source.slots.push_back({place.first, place.second, col, row});
        poses.push_back(p);
    }
    return true;
}

void MachineViews::SyncSlides(HIKARI::WorldView& world, const RegionObjects& objects, const Placements& placements, MachineSources& out) {
    ForEach<SlideRackComponent>(world, kSlideRackComponentType, [&](HIKARI::ObjectView, SlideRackComponent& component) {
        Rules::SlideSource source; SlideView view; std::vector<std::pair<int, int>> empty; std::vector<MemberPlace> members; std::string error;
        if (!component.BuildSource(source, empty, error)) { std::fprintf(stderr, "[TD2 configuration] disabled slide error=%s\n", error.c_str()); return; }
        if (std::string missing; !MemberPlaces(world, objects, placements, source.members, members, missing)) error = "member not placed: " + missing;
        else (void)DeriveSlideSlots(source, members, empty, view.slots, error);
        if (!error.empty()) { std::fprintf(stderr, "[TD2 configuration] disabled slide %s %s\n", source.id.c_str(), error.c_str()); return; }
        // 区域カメラは動かさない（棚全体を写す同じ構図のまま、空いたスロットと並びが見え続ける）。
        for (const auto& region : source.members) view.members.push_back(ReadMember(world, objects, region, false));
        std::fprintf(stderr, "[TD2 slide] %s slots=%zu members=%zu\n", source.id.c_str(), source.slots.size(), source.members.size());
        out.slides.push_back(std::move(source)); slides_.push_back(std::move(view));
    });
}

void MachineViews::UpdateSlides(const Frame& frame, Writes& writes, double seconds) {
    const auto& runtimes = frame.exploration.Slides();
    for (size_t i = 0; i < slides_.size() && i < runtimes.size(); ++i) {
        auto& view = slides_[i]; const auto& runtime = runtimes[i];
        // 動けなかった：そのテレビを 0.35 秒だけ左右に細かく揺らす。
        if (runtime.blockedCount != view.blockedCount) { view.blockedCount = runtime.blockedCount; view.shaking = runtime.blocked; view.shake = 0; }
        const bool moving = runtime.moving >= 0;
        if (!moving && view.lastMoving < 0 && view.shaking < 0) continue;
        for (size_t m = 0; m < view.members.size() && m < runtime.slotOf.size(); ++m) {
            auto& member = view.members[m];
            if (!member.tv.IsValid()) continue;
            const bool thisMoving = moving && runtime.moving == static_cast<int>(m);
            if (!thisMoving && view.lastMoving != static_cast<int>(m) && view.shaking != static_cast<int>(m)) continue;
            // スロットの位置の間を、両端でゆっくり止まるように運ぶ。
            const auto& to = view.slots[static_cast<size_t>(thisMoving ? runtime.to : runtime.slotOf[m])];
            const auto& from = view.slots[static_cast<size_t>(thisMoving ? runtime.from : runtime.slotOf[m])];
            const float e = Math::SmoothStep(thisMoving ? static_cast<float>(runtime.progress) : 1.f);
            auto pose = member.tvPose;
            pose.position = Lerp(from, to, e);
            if (view.shaking == static_cast<int>(m)) {
                view.shake += seconds;
                pose.position.x += ShakeOffset(view.shake, .025);
                if (ShakeLeft(view.shake) <= 0) view.shaking = -1;
            }
            Push(writes, member.tv, pose);
        }
        view.lastMoving = runtime.moving;
    }
}

// ---------------------------------------------------------------- テレビの塔
void MachineViews::SyncStacks(HIKARI::WorldView& world, const RegionObjects& objects, const Placements& placements, MachineSources& out) {
    ForEach<StackColumnComponent>(world, kStackColumnComponentType, [&](HIKARI::ObjectView, StackColumnComponent& component) {
        Rules::StackSource source; StackView view; std::vector<double> distances; std::vector<MemberPlace> members; std::string error;
        if (!component.BuildSource(source, distances, error)) { std::fprintf(stderr, "[TD2 configuration] disabled stack error=%s\n", error.c_str()); return; }
        // スロットはメンバーの今の配置とテレビの位置。置き場は一番下のテレビの背の側へ距離ぶん、世界地図では預け場所。
        if (std::string missing; !MemberPlaces(world, objects, placements, source.members, members, missing)) {
            std::fprintf(stderr, "[TD2 configuration] disabled stack %s member not placed: %s\n", source.id.c_str(), missing.c_str()); return;
        }
        for (const auto& m : members) { source.slots.push_back({m.x, m.y}); view.slots.push_back(m.position); }
        HIKARI::GameProjectTransform bottom{};
        const auto tv = objects.televisions.find(source.members.front());
        if (tv == objects.televisions.end() || !world.FindObject(tv->second).GetLocalTransform(bottom)) {
            std::fprintf(stderr, "[TD2 configuration] disabled stack %s missing bottom television\n", source.id.c_str()); return;
        }
        const auto back = QRotate(bottom.rotation, {0, 0, -1});
        for (const double length : distances) {
            const float distance = static_cast<float>(length);
            source.docks.push_back(Park());
            view.docks.push_back({bottom.position.x + back.x * distance, bottom.position.y + back.y * distance, bottom.position.z + back.z * distance});
        }
        for (const auto& region : source.members) view.members.push_back(ReadMember(world, objects, region, true));
        std::fprintf(stderr, "[TD2 stack] %s slots=%zu docks=%zu members=%zu\n", source.id.c_str(), source.slots.size(), source.docks.size(), source.members.size());
        out.stacks.push_back(std::move(source)); stacks_.push_back(std::move(view));
    });
}

void MachineViews::UpdateStacks(const Frame& frame, Writes& writes, double seconds) {
    const auto& runtimes = frame.exploration.Stacks();
    for (size_t i = 0; i < stacks_.size() && i < runtimes.size(); ++i) {
        auto& view = stacks_[i]; const auto& s = runtimes[i];
        if (s.blockedCount != view.blockedCount) { view.blockedCount = s.blockedCount; view.shake = 0; }
        const bool moving = s.phase != 0;
        if (!moving && view.lastPhase == 0 && view.shake < 0) continue;
        const auto placeOf = [&](int place) -> const F3* {
            if (place >= 0 && static_cast<size_t>(place) < view.slots.size()) return &view.slots[static_cast<size_t>(place)];
            const auto dock = static_cast<size_t>(-1 - place);
            return dock < view.docks.size() ? &view.docks[dock] : nullptr;
        };
        // 抜くのは両端でゆっくり、落ちるのはだんだん速く（重さが見えるように）。
        // 戻す時は二段：前半に塔の中のテレビが上のスロットへ上がり、置き場のテレビはその場で戻るスロットの高さまで吊り上がる。
        // 後半に置き場のテレビが水平にスロットへ滑り込む（途中で他のテレビと重ならない）。
        const double t = moving ? s.progress : 1.0;
        for (size_t m = 0; m < view.members.size() && m < s.at.size(); ++m) {
            auto& member = view.members[m];
            if (!member.tv.IsValid()) continue;
            const F3* a = placeOf(s.at[m]); const F3* b = placeOf(moving ? s.to[m] : s.at[m]);
            if (!a || !b) continue;
            double ey = s.phase == 2 ? t * t : Math::SmoothStep(t), ex = ey;
            if (s.phase == 3) { ey = Math::SmoothStep(t / .4); ex = s.at[m] < 0 ? Math::SmoothStep((t - .4) / .6) : ey; }
            auto pose = member.tvPose;
            pose.position = {static_cast<float>(a->x + (b->x - a->x) * ex), static_cast<float>(a->y + (b->y - a->y) * ey),
                             static_cast<float>(a->z + (b->z - a->z) * ex)};
            if (view.shake >= 0 && !moving && s.at[m] == 0) pose.position.z += ShakeOffset(view.shake, .03);
            Push(writes, member.tv, pose);
            if (member.camera.IsValid()) {
                auto camera = member.cameraPose;
                camera.position = Follow(camera.position, pose.position, member.tvPose.position);
                Push(writes, member.camera, camera);
            }
        }
        if (view.shake >= 0) { view.shake += seconds; if (view.shake > .35) view.shake = -1; }
        view.lastPhase = s.phase;
    }
}

// ---------------------------------------------------------------- 回転柱
void MachineViews::SyncCarousels(HIKARI::WorldView& world, const RegionObjects& objects, std::vector<Rules::RegionSource>& regions, MachineSources& out) {
    ForEach<CarouselComponent>(world, kCarouselComponentType, [&](HIKARI::ObjectView owner, CarouselComponent& component) {
        Rules::CarouselSource source; CarouselView view; std::vector<uint64_t> extra; std::string error;
        if (!component.BuildSource(source, extra, error)) { std::fprintf(stderr, "[TD2 configuration] disabled carousel error=%s\n", error.c_str()); return; }
        // この部品を付けた柱の位置を軸にし、柱自体も一緒に回す。
        HIKARI::GameProjectTransform column{};
        if (!owner.GetLocalTransform(column)) { std::fprintf(stderr, "[TD2 configuration] disabled carousel %s no column pose\n", source.id.c_str()); return; }
        view.pivot = column.position;
        if (const auto id = owner.GetSceneObjectId(); id && std::find(extra.begin(), extra.end(), id) == extra.end()) extra.push_back(id);
        // 立ち位置は面の今の配置。世界地図に置いていない面は預け場所（どこともつながらない行き止まり）へ置く。
        for (const auto& face : source.faces) {
            const auto region = std::find_if(regions.begin(), regions.end(), [&](const Rules::RegionSource& r) { return r.placement.regionId == face; });
            if (region == regions.end()) { std::fprintf(stderr, "[TD2 configuration] disabled carousel %s missing face %s\n", source.id.c_str(), face.c_str()); return; }
            if (!region->placement.worldPlaced) {
                const auto [x, y] = Park();
                region->placement.worldPlaced = true; region->placement.worldX = x; region->placement.worldY = y;
            }
            source.stations.push_back({region->placement.worldX, region->placement.worldY});
        }
        view.count = source.faces.size();
        const auto add = [&](HIKARI::GameProjectObjectHandle handle, HIKARI::GameProjectObjectHandle tv, bool camera) {
            CarouselPart part; part.handle = handle; part.tv = tv; part.camera = camera;
            if (const auto object = world.FindObject(handle); object.IsValid() && object.GetLocalTransform(part.pose)) view.parts.push_back(part);
        };
        // 面のテレビと、その区域カメラ（面と一緒に回り、乗っている面を見続ける）。
        for (const auto& face : source.faces) {
            const auto tv = objects.televisions.find(face);
            if (tv == objects.televisions.end()) continue;
            add(tv->second, tv->second, false);
            if (const auto camera = objects.cameras.find(face); camera != objects.cameras.end() && camera->second)
                if (const auto object = world.FindSceneObject(camera->second); object.IsValid()) add(object.GetHandle(), tv->second, true);
        }
        for (const auto id : extra)
            if (const auto object = world.FindSceneObject(id); object.IsValid()) add(object.GetHandle(), {}, false);
        std::fprintf(stderr, "[TD2 carousel] %s faces=%zu parts=%zu\n", source.id.c_str(), source.faces.size(), view.parts.size());
        out.carousels.push_back(std::move(source)); carousels_.push_back(std::move(view));
    });
}

void MachineViews::UpdateCarousels(const Frame& frame, Writes& writes) {
    const auto& runtimes = frame.exploration.Carousels();
    for (size_t i = 0; i < carousels_.size() && i < runtimes.size(); ++i) {
        auto& view = carousels_[i]; const auto& c = runtimes[i];
        const double turns = c.step + c.direction * Math::SmoothStep(c.progress);
        if (turns == view.written) continue;
        view.written = turns;
        // 一面ぶん = 360°/面の数。正の向きは +Z を +X へ回す向き（正面の面が見る人の右へ回る）。
        const auto q = QFromEulerDeg(0, static_cast<float>(turns * 360.0 / static_cast<double>(view.count)), 0);
        for (const auto& part : view.parts) {
            auto pose = part.pose;
            const auto r = QRotate(q, Sub(part.pose.position, view.pivot));
            pose.position = {view.pivot.x + r.x, view.pivot.y + r.y, view.pivot.z + r.z};
            pose.rotation = QMul(q, part.pose.rotation);
            // 操作中の面のカメラなら、追従のばねも同じだけ運ぶ（回る面を見失わない）。
            if (part.camera) Moved(frame, part.handle.value, part.tv, part.pose, pose);
            Push(writes, part.handle, pose);
        }
    }
}

// ---------------------------------------------------------------- 信号線
void MachineViews::SyncCables(HIKARI::WorldView& world) {
    ForEach<SignalCableComponent>(world, kSignalCableComponentType, [&](HIKARI::ObjectView object, SignalCableComponent& component) {
        if (component.Plate().empty()) return;
        cables_.push_back({object.GetHandle(), component.Plate()});
    });
    std::fprintf(stderr, "[TD2 cable] count=%zu\n", cables_.size());
}

void MachineViews::UpdateCables(HIKARI::WorldView& world, Rules::Exploration& exploration, double seconds) {
    if (cables_.empty()) return;
    const auto& devices = exploration.Snapshot().devices;
    const float step = seconds > 0 ? static_cast<float>(seconds) / kCableFadeSeconds : 1;
    for (auto& cable : cables_) {
        const auto plate = std::find_if(devices.begin(), devices.end(), [&](const auto& d) { return d.id == cable.plate; });
        const float target = plate != devices.end() && plate->powered ? 1.f : 0.f;
        if (cable.glow == target) continue;
        cable.glow = target > cable.glow ? std::min(target, cable.glow + step) : std::max(target, cable.glow - step);
        // 材質の色と発光だけを書き換える（モデル・部品・骨は触らないので GPU のシーンは作り直されない）。
        const float g = cable.glow;
        const auto mix = [g](F3 a, F3 b) { return F3{a.x + (b.x - a.x) * g, a.y + (b.y - a.y) * g, a.z + (b.z - a.z) * g}; };
        const auto object = world.FindObject(cable.handle);
        if (!object.IsValid() || !object.GetModel().SetMaterialColorOverride(0, mix(kCableBase, kCableLitBase), 1, kCableGlow, kCableGlowStrength * g).Accepted()) {
            // 材質セットの付いていない線は材質の上書きを持たないので光らせない（毎フレーム試さない）。
            std::fprintf(stderr, "[TD2 cable] %s: no runtime material override, stays unlit\n", cable.plate.c_str());
            cable.glow = target;
            continue;
        }
        if (static const bool spikeEvents = std::getenv("TD2_SPIKE_EVENTS") != nullptr; spikeEvents && g == target) {
            std::fprintf(stderr, "[TD2 cable] %s -> %s\n", cable.plate.c_str(), target > 0 ? "lit" : "normal");
        }
    }
}

// ---------------------------------------------------------------- 更新
void MachineViews::Moved(const Frame& frame, uint64_t key, Handle tv, const Transform& first, const Transform& pose) {
    const auto last = movedPose_.find(key);
    if (tv.value == frame.activeTv.value) frame.camera.Carry(last != movedPose_.end() ? last->second : first, pose);
    movedPose_[key] = pose;
}

void MachineViews::Update(HIKARI::WorldView& world, Rules::Exploration& exploration, const RegionObjects& objects, CameraDirector& camera, double seconds) {
    const Frame frame{exploration, objects.Television(exploration.ActiveRegion()), camera};
    Writes writes;
    UpdateMechanisms(frame, writes); Commit(world, writes, "mechanism");
    writes.clear(); UpdateSlides(frame, writes, seconds); Commit(world, writes, "slide");
    writes.clear(); UpdateStacks(frame, writes, seconds); Commit(world, writes, "stack");
    writes.clear(); UpdateCarousels(frame, writes); Commit(world, writes, "carousel");
    UpdateCables(world, exploration, seconds);
}
}
