#include "TD2CameraRig.h"
#include "TD2Transform.h"
#include <algorithm>
#include <cmath>

namespace TD2 {
namespace {
using F3 = HIKARI::GameProjectFloat3;
using Math::Vec3;
using Math::kPi;

// 長さが 0 に近ければ false を返し、v は変えない。
bool Normalize(Vec3& v) {
    const double length = std::sqrt(Math::Dot(v, v));
    if (length < 1e-9) { return false; }
    for (double& e : v) { e /= length; }
    return true;
}
// 前の向きから右（前 × 世界の上）と上（右 × 前）を作る。前がほぼ 0 か真上・真下なら false で、right と up は変えない。
bool SideAxes(Vec3 forward, Vec3& right, Vec3& up) {
    if (!Normalize(forward)) { return false; }
    Vec3 candidate = Math::Cross(forward, {0, 1, 0});
    if (!Normalize(candidate)) { return false; }
    right = candidate;
    up = Math::Cross(right, forward);
    return true;
}
// ローカルの点をテレビのワールド変換で移す（縮尺→回転→平行移動）。
F3 Transform(const HIKARI::GameProjectTransform& t, double x, double y, double z, bool translate) {
    const Vec3 v{x * t.scale.x, y * t.scale.y, z * t.scale.z};
    const Vec3 q{t.rotation.x, t.rotation.y, t.rotation.z};
    Vec3 c = Math::Cross(q, v);
    for (double& e : c) { e *= 2; }
    const Vec3 d = Math::Cross(q, c);
    Vec3 out;
    for (size_t i = 0; i < 3; ++i) { out[i] = v[i] + t.rotation.w * c[i] + d[i]; }
    if (translate) { out[0] += t.position.x; out[1] += t.position.y; out[2] += t.position.z; }
    return ToF3(out);
}
}

F3 CameraRig::ScreenPoint(const CameraRegionFrame& frame, double x, double y) {
    // 画面の左上がマス (0,0)。右がローカル +X、下がローカル -Y（画面は +Z 向き）。
    const double u = std::clamp((x - frame.worldX) / (std::max)(frame.cols, 1.0), -.25, 1.25);
    const double v = std::clamp((y - frame.worldY) / (std::max)(frame.rows, 1.0), -.25, 1.25);
    return Transform(frame.screen, frame.screenCenter.x + (u - .5) * frame.screenWidth,
                     frame.screenCenter.y + (.5 - v) * frame.screenHeight, frame.screenCenter.z, true);
}

F3 CameraRig::ScreenVelocity(const CameraRegionFrame& frame, double vx, double vy) {
    return Transform(frame.screen, vx / (std::max)(frame.cols, 1.0) * frame.screenWidth,
                     -vy / (std::max)(frame.rows, 1.0) * frame.screenHeight, 0, false);
}

F3 CameraRig::Forward(const HIKARI::GameProjectTransform& transform) {
    HIKARI::GameProjectTransform rotation = transform;
    rotation.scale = {1, 1, 1};
    return ToF3(Math::Normalize(ToVec3(Transform(rotation, 0, 0, 1, false))));
}

void CameraRig::Snap(const F3& eye, const F3& look, float verticalFovRadians) {
    baseEye_.Snap(ToVec3(eye)); baseLook_.Snap(ToVec3(look));
    offset_.Snap({}); aim_.Snap({}); roll_.Snap({});
    fov_ = verticalFovRadians;
    last_ = {eye, look, {0, 1, 0}, verticalFovRadians};
    ready_ = true;
}

namespace {
// 右・上の向きと注視点までの距離から、揺れのずれと傾きを出す。
void BreathAlong(const CameraAreaSettings& area, double time, const Vec3& right, const Vec3& up, double distance,
                 F3& eyeOffset, F3& lookOffset, double& roll) {
    const auto wave = [time](double period, double phase) { return std::sin(time * 2 * kPi / period + phase); };
    // 呼吸：周期の違う正弦の和（7〜11 秒）。
    double bx = area.breathe * wave(7.3, 0), by = area.breathe * .7 * wave(9.1, 1.3);
    const double angle = std::tan(area.breatheAngle * kPi / 180) * distance;
    double lx = angle * wave(8.2, .7), ly = angle * .6 * wave(10.7, 2.1);
    // 手持ち：1.6〜4 秒の細かい揺れ。主に向きを揺らし、傾きも少し足す。
    // 周期は割り切れない値にして繰り返しを目立たせない。角度は 0.3° 程度まで。
    const double hand = (std::max)(0.0, static_cast<double>(area.handheld));
    bx += hand * .008 * (wave(2.7, .4) + .6 * wave(1.9, 2.2));
    by += hand * .006 * (wave(3.3, 1.1) + .5 * wave(2.1, .3));
    // 揺れの幅（m）は 4 m より近い構図では距離に比例して小さくする（手のひらの画面に寄った時も画が泳がない）。
    const double reach = (std::min)(1.0, distance / 4.0);
    bx *= reach; by *= reach;
    const double handAngle = std::tan(hand * .18 * kPi / 180) * distance;
    lx += handAngle * (wave(2.3, .9) + .5 * wave(1.6, 2.7));
    ly += handAngle * .8 * (wave(2.9, 1.7) + .5 * wave(1.7, .2));
    roll = hand * .25 * kPi / 180 * (wave(3.9, .5) + .4 * wave(2.6, 1.9));
    Vec3 e, l;
    for (size_t i = 0; i < 3; ++i) { e[i] = right[i] * bx + up[i] * by; l[i] = right[i] * lx + up[i] * ly; }
    eyeOffset = ToF3(e); lookOffset = ToF3(l);
}
}

void CameraRig::Breath(const CameraAreaSettings& area, double time, F3& eyeOffset, F3& lookOffset, double& roll) const {
    const Vec3 d = Math::Sub(baseLook_.value, baseEye_.value);
    BreathAlong(area, time, right_, up_, std::sqrt(Math::Dot(d, d)), eyeOffset, lookOffset, roll);
}

void CameraRig::BreathAround(const CameraAreaSettings& area, double time, const F3& eye, const F3& look, F3& eyeOffset,
                             F3& lookOffset, double& roll) {
    // 追従の Update と同じ取り方（前 × 世界の上 = 右、右 × 前 = 上）。
    const Vec3 forward{look.x - eye.x, look.y - eye.y, look.z - eye.z};
    Vec3 right{1, 0, 0}, up{0, 1, 0};
    (void)SideAxes(forward, right, up);
    BreathAlong(area, time, right, up, std::sqrt(Math::Dot(forward, forward)), eyeOffset, lookOffset, roll);
}

void CameraRig::Carry(const HIKARI::GameProjectTransform& from, const HIKARI::GameProjectTransform& to) {
    if (!ready_) { return; }
    const auto delta = QMul(to.rotation, QConj(from.rotation));
    for (Spring* spring : {&baseEye_, &baseLook_}) {
        const auto p = QRotate(delta, {static_cast<float>(spring->value[0] - from.position.x), static_cast<float>(spring->value[1] - from.position.y),
                                       static_cast<float>(spring->value[2] - from.position.z)});
        const auto v = QRotate(delta, {static_cast<float>(spring->velocity[0]), static_cast<float>(spring->velocity[1]), static_cast<float>(spring->velocity[2])});
        spring->value[0] = p.x + to.position.x; spring->value[1] = p.y + to.position.y; spring->value[2] = p.z + to.position.z;
        spring->velocity[0] = v.x; spring->velocity[1] = v.y; spring->velocity[2] = v.z;
    }
}

CameraRigPose CameraRig::Update(const CameraRegionFrame& frame, const CameraAreaSettings& area, const F3& target, const F3& velocity,
                                bool present, double dt, double time) {
    dt = std::clamp(dt, 0.0, .1);
    // 1. 今の区域のカメラ位置へ滑らかに寄せる。
    if (!ready_) { Snap(frame.eye, frame.look, frame.verticalFovRadians); }
    baseEye_.Step(ToVec3(frame.eye), area.regionSeconds, dt);
    baseLook_.Step(ToVec3(frame.look), area.regionSeconds, dt);
    fov_ = frame.verticalFovRadians;
    const Vec3 forward = Math::Sub(baseLook_.value, baseEye_.value);
    const double distance = std::sqrt(Math::Dot(forward, forward));
    (void)SideAxes(forward, right_, up_);
    // 2. プレイヤーの方へ平行移動し、残りの差の一部だけ注視を振る（どちらも上限内）。
    // プレイヤーがいない時（死亡中など）は今の位置を保つ。
    if (present) {
        const Vec3 point = ToVec3(target), speed = ToVec3(velocity);
        Vec3 offsetTarget{}, aimTarget{}, ahead, delta;
        for (size_t i = 0; i < 3; ++i) { ahead[i] = speed[i] * area.lookAhead; }
        if (const double length = std::sqrt(Math::Dot(ahead, ahead)); length > .6) { for (double& e : ahead) { e *= .6 / length; } }
        for (size_t i = 0; i < 3; ++i) { delta[i] = point[i] + ahead[i] - baseLook_.value[i]; }
        const double dx = Math::Dot(delta, right_), dy = Math::Dot(delta, up_);
        offsetTarget[0] = std::clamp(dx * area.followX, -static_cast<double>(area.rangeX), static_cast<double>(area.rangeX));
        offsetTarget[1] = std::clamp(dy * area.followY, -static_cast<double>(area.rangeY), static_cast<double>(area.rangeY));
        offset_.Step(offsetTarget, area.positionSeconds, dt);
        const double maxX = std::tan(area.maxYaw * kPi / 180) * distance, maxY = std::tan(area.maxPitch * kPi / 180) * distance;
        aimTarget[0] = std::clamp((dx - offset_.value[0] * area.lookCarry) * area.aimFollow, -maxX, maxX);
        aimTarget[1] = std::clamp((dy - offset_.value[1] * area.lookCarry) * area.aimFollow, -maxY, maxY);
        aim_.Step(aimTarget, area.aimSeconds, dt);
    } else {
        offset_.Step(offset_.value, area.positionSeconds, dt);
        aim_.Step(aim_.value, area.aimSeconds, dt);
    }
    // 3. 横の速さに応じたわずかな傾き（±3° まで）。
    const double lateral = offset_.velocity[0] + Math::Dot(baseEye_.velocity, right_);
    const Vec3 rollTarget{std::clamp(-lateral * area.bank, -3.0, 3.0) * kPi / 180, 0, 0};
    roll_.Step(rollTarget, .35, dt);
    // 4. 呼吸を足す。注視は平行移動に lookCarry の分だけ付いて行く（視差が出る）。
    F3 breathEye, breathLook;
    double handRoll = 0;
    Breath(area, time, breathEye, breathLook, handRoll);
    Vec3 eye, look, up;
    const Vec3 be = ToVec3(breathEye), bl = ToVec3(breathLook);
    for (size_t i = 0; i < 3; ++i) {
        eye[i] = baseEye_.value[i] + right_[i] * offset_.value[0] + up_[i] * offset_.value[1] + be[i];
        look[i] = baseLook_.value[i] + right_[i] * (offset_.value[0] * area.lookCarry + aim_.value[0]) +
                  up_[i] * (offset_.value[1] * area.lookCarry + aim_.value[1]) + bl[i];
        up[i] = up_[i] * std::cos(roll_.value[0] + handRoll) + right_[i] * std::sin(roll_.value[0] + handRoll);
    }
    last_ = {ToF3(eye), ToF3(look), ToF3(up), fov_};
    return last_;
}
}
