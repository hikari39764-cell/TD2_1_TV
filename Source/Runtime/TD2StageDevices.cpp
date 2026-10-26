#include "TD2StageDevices.h"
#include "TD2StageProjection.h"
#include "HIKARI/Project/ActionViews.h"
#include "HIKARI/Project/WorldQuery.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>

namespace TD2 {
namespace {
using Model = DeviceActorModel;
using Role = StageDevices::Role;
// 割り当てた直後や片段を替えた直後は、写らない部屋でも数フレームは動作を計算する（姿勢が片段に追いつくまで）。
constexpr int kSettleFrames = 4;
// 回り続ける動作を飛ばしたフレームの追いつき：これより長く止まっていたら、追いつかずにその時刻から続ける。
constexpr uint64_t kCatchUpFrames = 4;
// 退避点：舞台の下の、どの舞台カメラにも写らない所。
constexpr float kParkDrop = 2000.0f;
// 扉・橋の出入りの秒数（焼いた部品の kSwitchSeconds と同じ）。
constexpr float kSwitchSeconds = .14f;
// 操作の札の大きさと掛ける壁の奥行き（TD2ScreenModels.cpp の kSignSize・kSignWall と同じ）。
constexpr double kSignSize = .8, kSignWall = -.83;

// 踏み板・レバー・戻すボタン。
std::optional<Role> PlateRole(const Rules::DeviceSnapshot& d, double x, double bottom) {
    const bool down = d.pressed || d.active;
    // 元へ戻すボタンと切り替えのレバーは 1 マスの大きさのまま板の中央に置く（伸ばすと形が崩れる）。
    if (d.variant == "reset") { return Role{Model::ResetWorld, down ? "Down" : "Up", x, bottom}; }
    if (d.latch) { return Role{d.world ? Model::ToggleWorld : Model::Toggle, d.active || d.powered ? "On" : "Off", x, bottom}; }
    const bool wide = d.rect.w > 1.5;
    const auto model = d.world ? (wide ? Model::PlateHoldWorld2 : Model::PlateHoldWorld1) : (wide ? Model::PlateHold2 : Model::PlateHold1);
    Role role{model, down ? "Down" : "Up", x, bottom, d.rect.w / kDeviceActorModelWidths[static_cast<size_t>(model)]};
    // 踏んだ姿は板が沈んで光り、周りの光の枠が 0.35 秒で広がって消える片段。
    if (down) { role.hold = .35f; role.once = true; }
    return role;
}

// バネ一マスの片段：使える（矢印がゆっくり上下）→ 溜める（溜まり切るまでの秒数でつなぎ、ばねが縮む）→ 打ち上げ（0.2 秒伸び縮み）
// → 冷却（緑が満ちる片段を冷却の秒数へ伸ばす）→ 使えるようになった瞬間（0.28 秒光る）→ 使える。
Role SpringRole(const Rules::DeviceSnapshot& d, double since) {
    Role role{Model::Spring, "Ready"};
    if (d.cooldown > 0) {
        if (since >= 0 && since < .2) { role.clip = "Launch"; role.blend = 0; role.hold = .2f; role.once = true; }
        else { role.clip = "Cool"; role.blend = .05f; role.hold = 1; role.once = true; role.speed = static_cast<float>(1 / (std::max)(.1, d.cooldownSeconds - .2)); }
    } else if (d.charge > 0) {
        role.clip = "Hot"; role.loop = true; role.blend = static_cast<float>((std::max)(.05, (1 - d.charge) * d.chargeSeconds));
    } else if (since >= 0 && since < .28) {
        role.clip = "Pop"; role.blend = .05f; role.hold = .28f; role.once = true;
    } else {
        role.loop = true;
    }
    return role;
}

// 装置の役を out へ並べる（物体で演じない装置は何も足さない）。
void RolesFor(const StagePartBuilder& builder, const Rules::DeviceSnapshot& d, const Rules::BeamState* beam, bool checkpoint,
              double since, std::vector<Role>& out) {
    const auto& r = d.rect;
    const double x = r.x + r.w * .5, bottom = r.y + r.h;
    const auto& projection = builder.Projection();
    switch (d.kind) {
    case Rules::DeviceKind::Plate:
        if (const auto role = PlateRole(d, x, bottom)) { out.push_back(*role); }
        break;
    case Rules::DeviceKind::Spring: {
        const Role role = SpringRole(d, since);
        for (double c = 0; c < r.w - 1e-6; c += 1) {
            Role cell = role; cell.x = r.x + c + .5; cell.bottom = bottom; out.push_back(cell);
        }
        break;
    }
    // 丸鋸：マスごとに一枚。刃の中心を付いている面の縁に置く。隣り合う刃は回る角度をずらした片段にする。
    case Rules::DeviceKind::Spikes: {
        const double ox = projection.ShiftX(-.3), oy = projection.ShiftY(-.3);
        for (double cy = 0; cy < r.h - 1e-6; cy += 1) for (double cx = 0; cx < r.w - 1e-6; cx += 1) {
            double sx = r.x + cx + .5, sy = r.y + cy + .5;
            switch (d.direction) {
            case Rules::Direction::Down: sy = r.y + cy; break;
            case Rules::Direction::Left: sx = r.x + cx + 1; break;
            case Rules::Direction::Right: sx = r.x + cx; break;
            default: sy = r.y + cy + 1; break;
            }
            static constexpr std::string_view kSpins[]{"Spin0", "Spin1", "Spin2"};
            Role role{Model::Saw, kSpins[static_cast<int>(cx + cy) % 3], sx + ox, sy + oy};
            role.pivot = 0; role.loop = true; role.blend = 0;
            out.push_back(role);
        }
        break;
    }
    // 押し刺：刺の面に丸鋸を並べる（箱と腕は焼いた部品）。刃の中心は面から箱の中へ寄せ、縁だけを出す。
    case Rules::DeviceKind::Pusher: {
        const double ox = projection.ShiftX(-.3), oy = projection.ShiftY(-.3);
        const bool horizontal = d.direction == Rules::Direction::Left || d.direction == Rules::Direction::Right;
        const int count = static_cast<int>(std::ceil((horizontal ? r.h : r.w) - 1e-6));
        constexpr double kInset = .38;   // TD2ScreenModels.cpp の PusherParts と同じ
        for (int k = 0; k < count; ++k) {
            double sx = r.x + k + .5, sy = r.y + k + .5;
            switch (d.direction) {
            case Rules::Direction::Up: sy = r.y + kInset; break;
            case Rules::Direction::Down: sy = r.y + r.h - kInset; break;
            case Rules::Direction::Left: sx = r.x + kInset; break;
            default: sx = r.x + r.w - kInset; break;
            }
            static constexpr std::string_view kSpins[]{"Spin0", "Spin1", "Spin2"};
            Role role{Model::Saw, kSpins[k % 3], sx + ox, sy + oy};
            role.pivot = 0; role.loop = true; role.blend = 0;
            out.push_back(role);
        }
        break;
    }
    // チェックポイント：宙に浮く青緑の小さなテレビ（消えている間もゆっくり上下する）。触れると跳ね上がって目を覚まし、
    // 画面が点→横線→全面と点いて笑顔が映る（0.7 秒、床の輪から光が広がる）。点いた後はアンテナから電波の弧が広がり、
    // 光の粒が周りを回る。ほかのチェックポイントに移ると画面が消えて少し下りる。
    case Rules::DeviceKind::Checkpoint: {
        Role role{Model::Checkpoint, "Idle", x, bottom, r.w};
        role.blend = .3f; role.loop = true;
        if (checkpoint && since >= 0 && since < .7) { role.clip = "Wake"; role.once = true; role.hold = .7f; role.blend = 0; }
        else if (checkpoint) { role.clip = "Active"; role.loop = true; role.blend = .15f; }
        out.push_back(role);
        break;
    }
    case Rules::DeviceKind::Collectible: {
        Role role{Model::Gem, d.active ? "Gone" : "Spin", x, bottom};
        role.loop = !d.active; role.blend = .12f;
        out.push_back(role);
        break;
    }
    // 光線：発射器は焼いた部品のまま（形が変わらない）。棒は当たりの長さへ物体ごと伸ばし、先端に火花を置く。
    case Rules::DeviceKind::Laser: {
        const auto direction = d.direction;
        const bool horizontal = direction == Rules::Direction::Left || direction == Rules::Direction::Right;
        const bool shown = beam != nullptr && beam->rect.w > 0 && beam->rect.h > 0;
        const Rules::Rect b = shown ? beam->rect : Rules::Rect{r.x, r.y, 1, 1};
        const double ox = projection.ShiftX(-.35), oy = projection.ShiftY(-.35);
        Role rod{Model::Beam, shown ? "On" : "Off", b.x + b.w * .5 + ox, b.y + b.h * .5 + oy, horizontal ? b.w : b.h};
        if (!horizontal) { rod.rotation = RotateZ(kPi * .5); }
        if (shown) { rod.height = (horizontal ? b.h : b.w) / (4.0 / 32); }   // 太い光（帘）は棒も太く
        rod.pivot = 0; rod.loop = shown; rod.blend = 0;
        out.push_back(rod);
        Role spark{Model::Spark, shown ? "On" : "Off",
                   (horizontal ? (direction == Rules::Direction::Left ? b.x : b.x + b.w) : b.x + b.w * .5) + ox,
                   (horizontal ? b.y + b.h * .5 : (direction == Rules::Direction::Up ? b.y : b.y + b.h)) + .06 + oy};
        spark.depth = .3; spark.pivot = .06; spark.loop = shown; spark.blend = 0;
        out.push_back(spark);
        break;
    }
    // 扉：高さ（1〜3 マス）ごとのモデルを幅へ伸ばす。開閉と通電灯の組で四つの姿。
    case Rules::DeviceKind::Door: {
        const int rows = static_cast<int>(std::lround(r.h));
        if (rows < 1 || rows > 3 || std::abs(r.h - rows) > 1e-3) { break; }
        const bool open = d.active || !d.solid;
        static constexpr Model kDoors[]{Model::Door1, Model::Door2, Model::Door3};
        Role role{kDoors[rows - 1], open ? (d.powered ? "OpenLit" : "Open") : (d.powered ? "ClosedLit" : "Closed"), x, bottom, r.w};
        role.blend = kSwitchSeconds;
        out.push_back(role);
        break;
    }
    case Rules::DeviceKind::Bridge: {
        if (std::abs(r.h - 1) > 1e-3) { break; }
        const bool extended = d.solid;
        Role role{Model::Bridge2, extended ? (d.powered ? "OutLit" : "Out") : (d.powered ? "InLit" : "In"), x, bottom, r.w / 2};
        role.blend = kSwitchSeconds;
        out.push_back(role);
        break;
    }
    // 通関の扉：幅 2・高さ 3 の形のまま矩形に収める。入ると戸が閉じて合わせ目の光が消える片段を一度流す。
    case Rules::DeviceKind::Goal: {
        const double k = (std::min)(r.w / 2, r.h / 3);
        const bool entered = d.pressed || builder.GoalClosingTime() >= 0;
        Role role{Model::Goal, entered ? "Enter" : d.active ? (d.powered ? "OpenLit" : "Open") : "Closed", x, bottom, k, k};
        role.blend = entered ? 0.f : .35f; role.hold = entered ? 1.05f : 0.f; role.once = entered;
        out.push_back(role);
        break;
    }
    // 操作の札と出口の矢印：後ろの壁に掛かった絵。押す・往復する片段を回し続ける。
    case Rules::DeviceKind::Decal: {
        const double cx = x + projection.ShiftX(kSignWall);
        const double cy = r.y + r.h * .5 + .5 * kSignSize + projection.ShiftY(kSignWall);
        std::optional<Model> sign;
        if (d.variant == "ad") { sign = Model::KeyAD; }
        else if (d.variant == "space") { sign = Model::KeySpace; }
        else if (d.variant == "spaceHold") { sign = Model::KeySpaceHold; }
        if (sign) {
            Role role{*sign, "Loop", cx, cy, 1, 1, kSignWall};
            role.pivot = 0; role.loop = true; role.blend = 0;
            out.push_back(role);
        } else if (d.variant.starts_with("exit")) {
            const double turn = d.variant == "exitLeft" ? kPi : d.variant == "exitUp" ? kPi * .5 : d.variant == "exitDown" ? -kPi * .5 : 0.0;
            Role role{Model::ExitArrow, "Push", cx, r.y + r.h * .5 + projection.ShiftY(kSignWall), 1, 1, kSignWall, RotateZ(turn)};
            role.pivot = 0; role.loop = true; role.blend = 0;
            out.push_back(role);
        }
        break;
    }
    default: break;
    }
}

bool SamePose(const HIKARI::GameProjectTransform& a, const HIKARI::GameProjectTransform& b) {
    const auto same = [](float x, float y) { return std::abs(x - y) < 1e-4f; };
    return same(a.position.x, b.position.x) && same(a.position.y, b.position.y) && same(a.position.z, b.position.z) &&
           same(a.rotation.x, b.rotation.x) && same(a.rotation.y, b.rotation.y) && same(a.rotation.z, b.rotation.z) &&
           same(a.rotation.w, b.rotation.w) && same(a.scale.x, b.scale.x) && same(a.scale.y, b.scale.y) && same(a.scale.z, b.scale.z);
}
}

bool StageDevices::Acquire(HIKARI::WorldView& world) {
    if (acquired_) {
        if (actors_.empty() || world.FindObject(actors_.front().handle).IsValid()) { return !actors_.empty(); }
        acquired_ = false;   // 別のシーンへ移った
    }
    actors_.clear(); byKey_.clear(); acquired_ = true;
    const HIKARI::ComponentTypeId types[]{HIKARI::MakeComponentTypeId(kDeviceActorComponentType)};
    (void)world.ForEachComponents<DeviceActorComponent>(types, HIKARI::GameProjectWorldQueryAnchor::FirstRequiredTerm,
        [&](HIKARI::ObjectView object, DeviceActorComponent& actor) {
            Actor item; item.handle = object.GetHandle(); item.model = actor.Model();
            actors_.push_back(std::move(item));
        });
    // 使っていない物体の Animator は片段の時刻を進めるだけでも毎フレーム描画を書き直させるので、最初から止める
    // （まだ姿勢の無い物体はエンジンが一度だけ計算する）。
    const auto animation = world.GetAnimation();
    for (auto& actor : actors_) { (void)animation.SetEvaluationEnabled(actor.handle.value, 0); actor.evaluating = false; }
    return !actors_.empty();
}

void StageDevices::BeginFrame() {
    for (auto& actor : actors_) { actor.used = false; }
}

void StageDevices::Release(size_t index) {
    auto& actor = actors_[index];
    if (actor.key.empty()) { return; }
    if (const auto found = byKey_.find(actor.key); found != byKey_.end() && found->second == index) { byKey_.erase(found); }
    actor.key.clear();
}

bool StageDevices::Draw(HIKARI::WorldView& world, const StagePartBuilder& builder, const Rules::DeviceSnapshot& device,
                        const Rules::BeamState* beam, bool checkpoint, double since, bool seen, bool redrawn, uint64_t frame) {
    roles_.clear();
    RolesFor(builder, device, beam, checkpoint, since, roles_);
    if (roles_.empty()) { return false; }
    // 役ごとに物体を割り当てる：前のフレームの物体をそのまま使い、無ければ同じ型の空いた物体を取る。足りなければ、
    // このフレームに取った物体を返して焼いた部品で描く。
    chosen_.clear(); taken_.clear();
    for (size_t i = 0; i < roles_.size(); ++i) {
        std::string key = device.id; key += '#'; key += std::to_string(i);
        const auto found = byKey_.find(key);
        if (found != byKey_.end() && actors_[found->second].model == roles_[i].model) { chosen_.push_back(found->second); continue; }
        if (found != byKey_.end()) { Release(found->second); }   // 型が変わった（前の物体は EndFrame で退避）
        size_t free = actors_.size();
        for (size_t a = 0; a < actors_.size() && free == actors_.size(); ++a) {
            if (actors_[a].model == roles_[i].model && actors_[a].key.empty()) { free = a; }
        }
        if (free == actors_.size()) {
            for (const size_t t : taken_) { Release(t); }
            return false;
        }
        actors_[free].key = key; byKey_[std::move(key)] = free;
        taken_.push_back(free); chosen_.push_back(free);
    }
    for (size_t i = 0; i < roles_.size(); ++i) { Perform(world, builder, actors_[chosen_[i]], roles_[i], seen, redrawn, frame); }
    // 役の数が減った（バネの幅を変えた等）時は、余った番号の物体を返す。
    for (size_t i = roles_.size();; ++i) {
        std::string key = device.id; key += '#'; key += std::to_string(i);
        const auto found = byKey_.find(key);
        if (found == byKey_.end()) { break; }
        Release(found->second);
    }
    return true;
}

void StageDevices::Perform(HIKARI::WorldView& world, const StagePartBuilder& builder, Actor& actor, const Role& role, bool seen, bool redrawn, uint64_t frame) {
    actor.used = true;
    const auto object = world.FindObject(actor.handle);
    if (!object.IsValid()) { return; }
    const auto animation = world.GetAnimation();
    // 置き場所は焼いた部品と同じ（部屋の点対称・テレビの傾きの打ち消し・投影）。動いた時だけ書く。
    const auto pose = builder.PartPose(role.x, role.bottom, role.width, role.height, role.depth, role.rotation, role.pivot);
    const bool arriving = actor.parked || !actor.placed;
    if (arriving || !SamePose(actor.pose, pose)) { (void)object.SetWorldTransform(pose); actor.pose = pose; }
    // 状態に合う片段へ。着いたばかりなら補間せずにその姿勢から始める。
    if (arriving || actor.clip != role.clip) {
        if (!actor.evaluating) { (void)animation.SetEvaluationEnabled(actor.handle.value, 1); actor.evaluating = true; }
        if (actor.speed != role.speed && animation.SetSpeed(actor.handle.value, role.speed)) { actor.speed = role.speed; }
        const uint8_t loop = role.once ? 0 : 1;
        const bool started = arriving || role.blend <= 0 ? animation.Play(actor.handle.value, role.clip, loop, 1)
                                                         : animation.CrossFade(actor.handle.value, role.clip, role.blend, loop, 1);
        if (started) { actor.clip = role.clip; }
        actor.settle = kSettleFrames + static_cast<int>(std::ceil((role.blend + role.hold / (std::max)(role.speed, 1e-3f)) * 60));
        // 診断（環境変数 TD2_SPIKE_EVENTS）：割り当てと片段の切り替え。
        if (static const bool trace = std::getenv("TD2_SPIKE_EVENTS") != nullptr; trace) {
            std::fprintf(stderr, "[TD2 device-actor] %s model=%d clip=%.*s started=%d\n", actor.key.c_str(), static_cast<int>(actor.model),
                static_cast<int>(role.clip.size()), role.clip.data(), started);
        }
    }
    // 回り続ける片段は写る部屋の間だけ計算する。それ以外は姿勢が片段に追いつけば止める（計算を続けると毎フレーム
    // 骨の姿勢が書き直され、エンジンが面を比べ直す）。
    if (actor.settle > 0) { --actor.settle; }
    // 回り続けるだけの間は、覗き窓が前の絵を残すフレーム（小さな画面の一つおき）には計算しない。描き直すフレームに、
    // 飛ばしたフレームの分だけ速めて進め、平均の速さは変えない（画に出る絵は毎フレーム計算した時と同じ）。
    // 診断（環境変数 TD2_ACTOR_THIN_OFF）：飛ばさずに毎フレーム計算する（比べるため）。
    static const bool thinOff = std::getenv("TD2_ACTOR_THIN_OFF") != nullptr;
    const bool looping = actor.settle == 0 && role.loop && seen;
    const bool evaluate = actor.settle > 0 || (looping && (redrawn || thinOff));
    float speed = role.speed;
    if (evaluate && looping && actor.evaluatedAt != 0 && frame > actor.evaluatedAt && frame - actor.evaluatedAt <= kCatchUpFrames) {
        speed = role.speed * static_cast<float>(frame - actor.evaluatedAt);
    }
    if (evaluate) { actor.evaluatedAt = frame; }
    if (evaluate && actor.speed != speed && animation.SetSpeed(actor.handle.value, speed)) { actor.speed = speed; }
    if (evaluate != actor.evaluating) { (void)animation.SetEvaluationEnabled(actor.handle.value, evaluate ? 1 : 0); actor.evaluating = evaluate; }
    actor.placed = true; actor.parked = false;
}

void StageDevices::EndFrame(HIKARI::WorldView& world) {
    for (size_t i = 0; i < actors_.size(); ++i) {
        if (actors_[i].used) { continue; }
        Release(i);
        ParkOne(world, actors_[i]);
    }
}

void StageDevices::Park(HIKARI::WorldView& world) {
    (void)Acquire(world);
    for (size_t i = 0; i < actors_.size(); ++i) {
        Release(i);
        ParkOne(world, actors_[i]);
    }
}

void StageDevices::ParkOne(HIKARI::WorldView& world, Actor& actor) {
    // 一度も置いていない物体（編集中など）は作者の置いた所のまま触らない。
    if (!actor.placed || actor.parked) { return; }
    const auto object = world.FindObject(actor.handle);
    if (!object.IsValid()) { return; }
    HIKARI::GameProjectTransform pose{};
    pose.position = {kStageCoordinate + static_cast<float>(kParkX), kStageCoordinate - kParkDrop, 0};
    pose.scale = {.01f, .01f, .01f};
    (void)object.SetWorldTransform(pose);
    (void)world.GetAnimation().SetEvaluationEnabled(actor.handle.value, 0);
    actor.evaluating = false; actor.parked = true;
}
}
