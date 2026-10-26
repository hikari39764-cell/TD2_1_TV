#include "TD2StagePuppet.h"
#include "TD2Transform.h"
#include "HIKARI/Project/ActionViews.h"
#include "HIKARI/Project/WorldQuery.h"
#include <algorithm>
#include <cmath>

namespace TD2 {
namespace {
// 走りの動作は速さ 1 で毎秒 3.07（モデルの単位）進む。歩きの速さに合わせて足が滑らない速さにするが、
// 足さばきが速すぎて読めなくならないよう 0.7〜1.6 倍に収める。
constexpr double kRunModelSpeed = 3.07;
constexpr float kRunSlowest = .7f, kRunFastest = 1.6f;
// 着地の動作は、空中に 0.3 秒以上居て毎秒 9 マス以上で落ちてきた時だけ（小さな段差では出さない）。歩きながらは出さない。
constexpr double kLandAirborne = .3, kLandSpeed = 9.0, kLandSeconds = .32;
// 交換の印を結ぶ動作を見せる秒数。
constexpr double kSwapSeconds = .45;
// 向きを替える時の回り方の時定数（秒）。真横から真横へ、途中でカメラの方を向いて回る。
constexpr double kTurnSeconds = .05;
// 焼いた形は正面が depth で、奥へ 0.42 マス（高さ 1 あたり）の厚み。人形は中心が原点なので、その中ほどに立たせる。
constexpr double kBodyHalfDepth = .21;
// 退避点：舞台の下の、どの舞台カメラにも写らない所。
constexpr float kParkDrop = 2000.0f;
// 残影の背丈（マス）。主角の 0.84 倍で、どの動作も当たりの箱（高さ 1 マス）の中に収まる（頭上を受ける腕がちょうど上辺）。
constexpr double kEchoSize = .99;
// 残影はほぼカメラを向き、向いている側へ少しだけ振る（しゃがんだ横向きは頭が箱からはみ出す）。
constexpr double kEchoYaw = 25.0;
// 押され始めにひるむ秒数。
constexpr double kEchoFlinchSeconds = .5;

}

bool StagePuppets::Acquire(HIKARI::WorldView& world) {
    if (player_.handle.IsValid() && world.FindObject(player_.handle).IsValid()) { return true; }
    // 別のシーンへ移った後などは探し直す（主角の人形が無いシーンでは毎回すぐ終わる問い合わせ）。
    player_ = {}; echo_ = {};
    lastTime_ = -1; airborne_ = fallSpeed_ = 0; landUntil_ = swapUntil_ = -1; swapsKnown_ = false;
    const HIKARI::ComponentTypeId types[]{HIKARI::MakeComponentTypeId(kStageActorComponentType)};
    (void)world.ForEachComponents<StageActorComponent>(types, HIKARI::GameProjectWorldQueryAnchor::FirstRequiredTerm,
        [&](HIKARI::ObjectView object, StageActorComponent& actor) {
            auto& slot = actor.Settings().role == static_cast<int>(StageActorRole::Echo) ? echo_ : player_;
            if (!slot.handle.IsValid()) { slot.handle = object.GetHandle(); slot.settings = actor.Settings(); }
        });
    return player_.handle.IsValid();
}

bool StagePuppets::Has(StageActorRole role) const {
    return (role == StageActorRole::Echo ? echo_ : player_).handle.IsValid();
}

void StagePuppets::Player(HIKARI::WorldView& world, const StagePartBuilder& builder, const Rules::WorldSnapshot& snapshot,
                          double actorScale, double depth, bool shown, double grow) {
    if (!player_.handle.IsValid()) { return; }
    const auto& body = snapshot.player;
    const double now = snapshot.realTime;
    const double dt = lastTime_ < 0 ? 0.0 : std::clamp(now - lastTime_, 0.0, .1);
    lastTime_ = now;
    if (!snapshot.playerPresent || body.dead || !shown) { ParkOne(world, player_); airborne_ = fallSpeed_ = 0; return; }
    // 交換した瞬間から、印を結ぶ動作を少しの間見せる。
    if (!swapsKnown_) { swaps_ = snapshot.swaps; swapsKnown_ = true; }
    if (snapshot.swaps != swaps_) { swaps_ = snapshot.swaps; swapUntil_ = now + kSwapSeconds; }
    if (!body.grounded) { airborne_ += dt; fallSpeed_ = (std::max)(fallSpeed_, body.vy); }
    else {
        if (airborne_ >= kLandAirborne && fallSpeed_ >= kLandSpeed && std::abs(body.walkVx) < .01) { landUntil_ = now + kLandSeconds; }
        airborne_ = fallSpeed_ = 0;
    }
    // 動作の優先：ダッシュ → 壁蹴りの跳ね返り → 交換 → 空中（上りは跳び、下りは落ち）→ 着地 → 走り → 待機。
    // ダッシュ・壁蹴り・交換は今は仮の動作（走りを速めたもの、元の忍者の動作）。
    // 残影を頭に載せている間は、待機・走り・空中を両手で支える版にし、着地の屈みは出さない（手が残影から離れる）。
    const bool carrying = snapshot.echo && snapshot.echo->supportId == "player";
    if (snapshot.dashing) { Play(world, player_, "Dash", true, .04f, 1); }
    else if (snapshot.bouncing) { Play(world, player_, "WallJump", false, .04f, 1); }
    else if (now < swapUntil_) { Play(world, player_, "Swap", false, .05f, 1); }
    else if (!body.grounded) {
        if (body.vy < 0) { PlayCarry(world, carrying, "Jump", false, .1f, 1); }
        else { PlayCarry(world, carrying, "Fall", true, .18f, 1); }
    }
    else if (now < landUntil_ && !carrying) { Play(world, player_, "Land", false, .05f, 1.3f); }
    else if (std::abs(body.walkVx) > .01) {
        // 見た目の背丈あたりの歩きの速さを、走りの動作が進む速さで割る。
        const double heights = std::abs(body.walkVx) / (std::max)(snapshot.presentationPlayer.h * actorScale, .1);
        const double stride = kRunModelSpeed / player_.settings.height;
        PlayCarry(world, carrying, "Run", true, .1f, std::clamp(static_cast<float>(heights / stride), kRunSlowest, kRunFastest));
    }
    else { PlayCarry(world, carrying, "Idle", true, carrying ? .12f : .22f, 1); }
    const double yaw = (body.facing < 0 ? -1.0 : 1.0) * (90.0 - player_.settings.turn);
    Stand(world, builder, player_, snapshot.presentationPlayer, yaw, snapshot.presentationPlayer.h * actorScale, depth, grow, now);
}

void StagePuppets::Echo(HIKARI::WorldView& world, const StagePartBuilder& builder, const Rules::BodyState& body, const Rules::Rect& rect,
                        bool shown, double grow, double depth, double now, const EchoCue& cue) {
    if (!echo_.handle.IsValid()) { return; }
    if (!shown || body.dead) { ParkOne(world, echo_); echoPushed_ = false; echoFlinchUntil_ = -1; return; }
    if (cue.pushed && !echoPushed_) { echoFlinchUntil_ = now + kEchoFlinchSeconds; }
    echoPushed_ = cue.pushed;
    // 動作の優先：横からの光 → 上からの光か上に立たれる → 狭い所 → 押され始め → しゃがんで待つ（頭の上・空中も同じ）。
    // 横からの光は光の方を向き、腕を前に構えて受ける。上からの光と、上に立つ主角は、座って両腕を頭の上で組んで受け止める
    // （腕の先が箱の上辺で、光の止まる所・主角の足にちょうど届く）。頭のすぐ上が塞がっていれば、頭を抱えて縮こまる。
    double yaw = (body.facing < 0 ? -1.0 : 1.0) * kEchoYaw;
    if (cue.beam == EchoBeam::FromLeft || cue.beam == EchoBeam::FromRight) {
        yaw = (cue.beam == EchoBeam::FromLeft ? -1.0 : 1.0) * (90.0 - echo_.settings.turn);
        PlayEcho(world, "EchoGuard", true, .1f, 1);
    }
    else if (cue.beam == EchoBeam::Above || cue.stoodOn) { PlayEcho(world, "EchoBrace", false, .1f, 1.6f); }
    else if (cue.cramped) { PlayEcho(world, "EchoDuck", false, .12f, 1.5f); }
    else if (now < echoFlinchUntil_) { PlayEcho(world, "EchoFlinch", false, .06f, 1.3f); }
    else { PlayEcho(world, "EchoIdle", true, .2f, 1); }
    Stand(world, builder, echo_, rect, yaw, rect.h * kEchoSize, depth, grow, now, cue.lean, cue.width, cue.height);
}

void StagePuppets::PlayEcho(HIKARI::WorldView& world, std::string_view clip, bool loop, float blend, float speed) {
    if (!Play(world, echo_, clip, loop, blend, speed)) { (void)Play(world, echo_, "Echo", true, 0, 1); }
}

void StagePuppets::Park(HIKARI::WorldView& world) {
    ParkOne(world, player_);
    ParkOne(world, echo_);
}

bool StagePuppets::Play(HIKARI::WorldView& world, Puppet& puppet, std::string_view clip, bool loop, float blend, float speed) {
    const auto animation = world.GetAnimation();
    const uint64_t id = puppet.handle.value;
    if (puppet.clip != clip) {
        const bool started = blend > 0 ? animation.CrossFade(id, clip, blend, loop ? 1 : 0, 1) : animation.Play(id, clip, loop ? 1 : 0, 1);
        // その動作がモデルに無ければ今の動作のまま。
        if (!started) { return false; }
        puppet.clip = clip; puppet.speed = -1;
    }
    if (std::abs(puppet.speed - speed) > .02f && animation.SetSpeed(id, speed)) { puppet.speed = speed; }
    return true;
}

void StagePuppets::PlayCarry(HIKARI::WorldView& world, bool carrying, std::string_view clip, bool loop, float blend, float speed) {
    if (carrying) {
        const std::string name = "Carry" + std::string(clip);
        if (Play(world, player_, name, loop, blend, speed)) { return; }
    }
    (void)Play(world, player_, clip, loop, blend, speed);
}

void StagePuppets::Stand(HIKARI::WorldView& world, const StagePartBuilder& builder, Puppet& puppet, const Rules::Rect& rect, double yaw,
                         double size, double depth, double grow, double now, double lean, double width, double height) {
    const auto object = world.FindObject(puppet.handle);
    if (!object.IsValid()) { return; }
    const double dt = puppet.time < 0 ? 0.0 : std::clamp(now - puppet.time, 0.0, .1);
    puppet.time = now;
    // 0 がモデルの正面（+Z）をカメラへ向けた向きで、90 は舞台の右（+X）。
    const double target = yaw;
    if (puppet.parked || dt <= 0) { puppet.yaw = target; }
    else { puppet.yaw += (target - puppet.yaw) * (1 - std::exp(-dt / kTurnSeconds)); }
    if (puppet.parked) { (void)world.GetAnimation().SetEvaluationEnabled(puppet.handle.value, 1); }
    // 斜投影を焼いた組では、焼いた形の奥ほど右上へずれている。人形は斜めにできないので、体の中ほどの奥行きの分だけ
    // 全体を移して焼いた形と重ねる（実体の組ではずれは 0）。
    const double half = kBodyHalfDepth * size;
    const auto& projection = builder.Projection();
    // 傾きは足元を軸にした画面の上での回転（向きの後に掛ける）。
    auto pose = builder.Place(rect.x + rect.w * .5 + projection.ShiftX(-half), rect.y + rect.h + projection.ShiftY(-half), depth - half,
                              QMul(RotateZ(lean), QFromEulerDeg(0, puppet.yaw, 0)));
    const auto s = static_cast<float>(size * builder.StageScale() / (std::max)(puppet.settings.height, .1f));
    pose.scale = {static_cast<float>(s * width), static_cast<float>(s * height * std::clamp(grow, .01, 1.0)), static_cast<float>(s * width)};
    (void)object.SetWorldTransform(pose);
    puppet.parked = false;
    puppet.stood = true;
}

void StagePuppets::ParkOne(HIKARI::WorldView& world, Puppet& puppet) {
    // 一度も立たせていない人形（編集中・平面の舞台）は作者の置いた所のまま触らない。
    if (!puppet.stood || puppet.parked) { return; }
    const auto object = world.FindObject(puppet.handle);
    if (!object.IsValid()) { return; }
    HIKARI::GameProjectTransform pose{};
    pose.position = {kStageCoordinate + static_cast<float>(kParkX), kStageCoordinate - kParkDrop, 0};
    pose.scale = {.01f, .01f, .01f};
    (void)object.SetWorldTransform(pose);
    // 見えない間は動作を計算しない。
    (void)world.GetAnimation().SetEvaluationEnabled(puppet.handle.value, 0);
    puppet.parked = true;
}
}
