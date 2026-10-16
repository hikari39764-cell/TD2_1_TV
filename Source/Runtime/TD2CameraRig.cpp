#include "TD2CameraRig.h"
#include <algorithm>
#include <cmath>

namespace TD2 {
namespace {
constexpr double kPi = 3.14159265358979323846;
using F3 = HIKARI::GameProjectFloat3;

void Load(const F3& v, double out[3]) { out[0] = v.x; out[1] = v.y; out[2] = v.z; }
F3 Store(const double v[3]) { return {static_cast<float>(v[0]), static_cast<float>(v[1]), static_cast<float>(v[2])}; }
double Dot(const double a[3], const double b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
void Cross(const double a[3], const double b[3], double out[3]) {
    out[0] = a[1] * b[2] - a[2] * b[1]; out[1] = a[2] * b[0] - a[0] * b[2]; out[2] = a[0] * b[1] - a[1] * b[0];
}
bool Normalize(double v[3]) {
    const double length = std::sqrt(Dot(v, v));
    if (length < 1e-9) { return false; }
    for (int i = 0; i < 3; ++i) { v[i] /= length; }
    return true;
}
// 局所の点を電視の世界変換で移す（縮尺 → 四元数の回転 → 平行移動。v' = v + 2w(q×v) + 2q×(q×v)）。
F3 Transform(const HIKARI::GameProjectTransform& t, double x, double y, double z, bool translate) {
    const double v[3]{x * t.scale.x, y * t.scale.y, z * t.scale.z};
    const double q[3]{t.rotation.x, t.rotation.y, t.rotation.z};
    double c[3], d[3];
    Cross(q, v, c);
    for (double& e : c) { e *= 2; }
    Cross(q, c, d);
    double out[3];
    for (int i = 0; i < 3; ++i) { out[i] = v[i] + t.rotation.w * c[i] + d[i]; }
    if (translate) { out[0] += t.position.x; out[1] += t.position.y; out[2] += t.position.z; }
    return Store(out);
}
} // namespace

// 臨界減衰のばね（Unity の SmoothDamp と同じ近似）。止まり際に行き過ぎず、目標が跳んでも速度が連続する。
void CameraRig::Spring::Step(const double target[3], double seconds, double dt) {
    const double omega = 2.0 / (std::max)(seconds, 1e-3), x = omega * dt;
    const double decay = 1.0 / (1.0 + x + .48 * x * x + .235 * x * x * x);
    for (int i = 0; i < 3; ++i) {
        const double change = value[i] - target[i], temp = (velocity[i] + omega * change) * dt;
        velocity[i] = (velocity[i] - omega * temp) * decay;
        value[i] = target[i] + (change + temp) * decay;
    }
}

F3 CameraRig::ScreenPoint(const CameraRegionFrame& frame, double x, double y) {
    // 画面の左上が区域の格 (0,0)。格の右は電視の局所 +X、下は局所 -Y（画面は局所 +Z を向く）。
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
    const F3 f = Transform(rotation, 0, 0, 1, false);
    double v[3]{f.x, f.y, f.z};
    if (!Normalize(v)) { v[0] = 0; v[1] = 0; v[2] = 1; }
    return Store(v);
}

void CameraRig::Snap(const F3& eye, const F3& look, float verticalFovRadians) {
    double e[3], l[3];
    Load(eye, e); Load(look, l);
    baseEye_.Snap(e); baseLook_.Snap(l);
    const double zero[3]{};
    offset_.Snap(zero); aim_.Snap(zero); roll_.Snap(zero);
    fov_ = verticalFovRadians;
    last_ = {eye, look, {0, 1, 0}, verticalFovRadians};
    ready_ = true;
}

namespace {
// 揺れの本体：右・上の単位ベクトルと注視点までの距離から、目と注視点のずれと傾きを出す。
void BreathAlong(const CameraAreaSettings& area, double time, const double right[3], const double up[3], double distance,
                 F3& eyeOffset, F3& lookOffset, double& roll) {
    const auto wave = [time](double period, double phase) { return std::sin(time * 2 * kPi / period + phase); };
    // 1. 呼吸：周期をずらした正弦の和（7〜11 秒）。止まっていても画がわずかに息づく。
    double bx = area.breathe * wave(7.3, 0), by = area.breathe * .7 * wave(9.1, 1.3);
    const double angle = std::tan(area.breatheAngle * kPi / 180) * distance;
    double lx = angle * wave(8.2, .7), ly = angle * .6 * wave(10.7, 2.1);
    // 2. 手持ち：1.6〜4 秒の細かい揺れ（人が構えたカメラの止めきれない揺れ）。位置より向きの揺れを主にし、傾きも少し。
    //    周期は互いに割り切れない値にして、繰り返しが目に付かないようにする。読みやすさのため角度は 0.3° 程度まで。
    const double hand = (std::max)(0.0, static_cast<double>(area.handheld));
    bx += hand * .008 * (wave(2.7, .4) + .6 * wave(1.9, 2.2));
    by += hand * .006 * (wave(3.3, 1.1) + .5 * wave(2.1, .3));
    const double handAngle = std::tan(hand * .18 * kPi / 180) * distance;
    lx += handAngle * (wave(2.3, .9) + .5 * wave(1.6, 2.7));
    ly += handAngle * .8 * (wave(2.9, 1.7) + .5 * wave(1.7, .2));
    roll = hand * .25 * kPi / 180 * (wave(3.9, .5) + .4 * wave(2.6, 1.9));
    double e[3], l[3];
    for (int i = 0; i < 3; ++i) { e[i] = right[i] * bx + up[i] * by; l[i] = right[i] * lx + up[i] * ly; }
    eyeOffset = Store(e); lookOffset = Store(l);
}
} // namespace

void CameraRig::Breath(const CameraAreaSettings& area, double time, F3& eyeOffset, F3& lookOffset, double& roll) const {
    double d[3];
    for (int i = 0; i < 3; ++i) { d[i] = baseLook_.value[i] - baseEye_.value[i]; }
    BreathAlong(area, time, right_, up_, std::sqrt(Dot(d, d)), eyeOffset, lookOffset, roll);
}

void CameraRig::BreathAround(const CameraAreaSettings& area, double time, const F3& eye, const F3& look, F3& eyeOffset,
                             F3& lookOffset, double& roll) {
    // 追従の Update と同じ取り方（前 × 世界の上 = 右、右 × 前 = 上）。
    double forward[3]{look.x - eye.x, look.y - eye.y, look.z - eye.z}, right[3]{1, 0, 0}, up[3]{0, 1, 0};
    const double distance = std::sqrt(Dot(forward, forward));
    if (Normalize(forward)) {
        const double worldUp[3]{0, 1, 0};
        double candidate[3];
        Cross(forward, worldUp, candidate);
        if (Normalize(candidate)) {
            for (int i = 0; i < 3; ++i) { right[i] = candidate[i]; }
            Cross(right, forward, up);
        }
    }
    BreathAlong(area, time, right, up, distance, eyeOffset, lookOffset, roll);
}

CameraRigPose CameraRig::Update(const CameraRegionFrame& frame, const CameraAreaSettings& area, const F3& target, const F3& velocity,
                                bool present, double dt, double time) {
    dt = std::clamp(dt, 0.0, .1);
    // 1. 基準：今の区域机位へ滑らせる（同じ大区域の部屋の乗り換えはこれだけで繋がる）。
    double frameEye[3], frameLook[3];
    Load(frame.eye, frameEye); Load(frame.look, frameLook);
    if (!ready_) { Snap(frame.eye, frame.look, frame.verticalFovRadians); }
    baseEye_.Step(frameEye, area.regionSeconds, dt);
    baseLook_.Step(frameLook, area.regionSeconds, dt);
    fov_ = frame.verticalFovRadians;
    double forward[3];
    for (int i = 0; i < 3; ++i) { forward[i] = baseLook_.value[i] - baseEye_.value[i]; }
    const double distance = std::sqrt(Dot(forward, forward));
    if (Normalize(forward)) {
        const double worldUp[3]{0, 1, 0};
        double right[3];
        Cross(forward, worldUp, right);
        if (Normalize(right)) {
            for (int i = 0; i < 3; ++i) { right_[i] = right[i]; }
            Cross(right_, forward, up_);
        }
    }
    // 2. 主角の方へ：機位は横・縦へ平行移動（範囲内）、残りの差の一部だけ注視を振る（角度の上限内）。
    //    主角が見えない時（死亡中など）は今の位置を保つ。
    if (present) {
        double point[3], speed[3], offsetTarget[3]{}, aimTarget[3]{};
        Load(target, point); Load(velocity, speed);
        double ahead[3];
        for (int i = 0; i < 3; ++i) { ahead[i] = speed[i] * area.lookAhead; }
        if (const double length = std::sqrt(Dot(ahead, ahead)); length > .6) { for (double& e : ahead) { e *= .6 / length; } }
        double delta[3];
        for (int i = 0; i < 3; ++i) { delta[i] = point[i] + ahead[i] - baseLook_.value[i]; }
        const double dx = Dot(delta, right_), dy = Dot(delta, up_);
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
    // 3. 横へ動く速さに応じたごくわずかな傾き（±3° まで）。手で振ったような柔らかさを足す。
    const double lateral = offset_.velocity[0] + Dot(baseEye_.velocity, right_);
    const double rollTarget[3]{std::clamp(-lateral * area.bank, -3.0, 3.0) * kPi / 180, 0, 0};
    roll_.Step(rollTarget, .35, dt);
    // 4. 呼吸と合成。注視は機位の平行移動に lookCarry だけ付いて行き、残りは基準の方へ振り返る（回り込む視差）。
    F3 breathEye, breathLook;
    double handRoll = 0;
    Breath(area, time, breathEye, breathLook, handRoll);
    double eye[3], look[3], up[3];
    const double be[3]{breathEye.x, breathEye.y, breathEye.z}, bl[3]{breathLook.x, breathLook.y, breathLook.z};
    for (int i = 0; i < 3; ++i) {
        eye[i] = baseEye_.value[i] + right_[i] * offset_.value[0] + up_[i] * offset_.value[1] + be[i];
        look[i] = baseLook_.value[i] + right_[i] * (offset_.value[0] * area.lookCarry + aim_.value[0]) +
                  up_[i] * (offset_.value[1] * area.lookCarry + aim_.value[1]) + bl[i];
        up[i] = up_[i] * std::cos(roll_.value[0] + handRoll) + right_[i] * std::sin(roll_.value[0] + handRoll);
    }
    last_ = {Store(eye), Store(look), Store(up), fov_};
    return last_;
}
} // namespace TD2
