#include "TD2ScreenModels.h"
#include "TD2ScreenKitAssets.h"
#include "../Common/TD2Math.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <span>
#include <string>

namespace TD2 {
namespace {
// 焼いた残影の背丈（当たりの箱の高さ 1 マスに対する割合）。1 マスの隙間でも天井に刺さらない。
constexpr double kEchoFillHeight = .98;
// 隠す部品を退避位置からさらに下げる距離（舞台カメラに写らない所まで）。
constexpr float kHiddenDrop = 2000.0f;

}

void StagePartBuilder::Begin(std::string_view key) {
    semantic_ = 1469598103934665603ull;
    for (const unsigned char c : key) { semantic_ = (semantic_ ^ c) * 1099511628211ull; }
}

void StagePartBuilder::PadSeries(const SeriesCounts& previous) {
    const uint64_t semantic = semantic_;
    for (const auto& [key, series] : previous) {
        const auto found = series_.find(key);
        for (size_t i = found != series_.end() ? found->second.count : 0; i < series.count; ++i) {
            semantic_ = series.semantic;
            Add(series.part, kParkX, kParkY, 1, 1, 0, {}, -1, false);
        }
    }
    semantic_ = semantic;
}

bool StagePartBuilder::Add(std::string_view part, double x, double bottomY, double width, double height, double depth,
                           HIKARI::GameProjectQuaternion rotation, double leanPivot, bool visible) {
    const auto guid = ScreenKitGuid(part, palette_);
    PartRequest request{};
    // ID：系列（意味名と部品名）の鍵と、系列の中の順番。
    uint64_t key = semantic_;
    for (const unsigned char c : part) { key = (key ^ c) * 1099511628211ull; }
    auto& series = series_[key];
    if (series.part.empty()) { series.semantic = semantic_; series.part = std::string(part); }
    uint64_t id = key ^ (++series.count * 0x9e3779b97f4a7c15ull);
    id ^= id >> 31; id *= 0xBF58476D1CE4E5B9ull; id ^= id >> 29;
    request.ownerObjectHandle = owner_; request.instanceId = id ? id : 1;
    if (guid.empty() || guid.size() >= sizeof(request.model.assetGuid)) { ++failed_; return false; }
    std::memcpy(request.model.assetGuid, guid.data(), guid.size());
    // 俯角と側角を両方付けた時の傾きは部品の中ほどで釣り合わせ、マスの中心からのずれを上下に半分ずつにする。
    // 光線は中心が原点なのでずらさない。
    const double pivot = leanPivot >= 0 ? leanPivot : (part == "LaserBeam" ? 0.0 : height * .5);
    request.worldTransform = PartPose(x, bottomY, width, height, depth, rotation, pivot);
    // 陰影は部品に焼いてあり、キャプチャは灯も影も使わない。visible=0 にするとシーンの作り直しになるので、
    // 表示したまま舞台カメラに写らない所へ退ける。
    request.castShadow = 0; request.receiveShadow = 0; request.visible = 1;
    if (!visible) { request.worldTransform.position.y -= kHiddenDrop; }
    out_.push_back(request);
    return true;
}

HIKARI::GameProjectTransform StagePartBuilder::PartPose(double x, double bottomY, double width, double height, double depth,
                                                      HIKARI::GameProjectQuaternion rotation, double pivot) const {
    auto pose = Place(x, bottomY, depth, rotation, pivot);
    const auto& turned = pose.rotation;
    const bool quarterTurn = std::abs(std::abs(turned.z) - .70710678f) < .0001f && turned.x == 0 && turned.y == 0;
    // 正面の 1 マスが像でもちょうど 1 マスになるよう、俯角で縦、側角で横の縮みを補う。90°回した部品は軸を入れ替える。
    const double pitchCos = std::cos(projection_.pitch), yawCos = std::cos(projection_.yaw);
    pose.scale = {static_cast<float>(width * projection_.scale / (quarterTurn ? pitchCos : yawCos)),
        static_cast<float>(height * projection_.scale / (quarterTurn ? yawCos : pitchCos)), static_cast<float>(projection_.scale * partDepth_)};
    return pose;
}

HIKARI::GameProjectTransform StagePartBuilder::Place(double x, double bottomY, double depth, HIKARI::GameProjectQuaternion rotation,
                                                    double pivot) const {
    if (mirror_) {
        // 中心について画面の上で時計回りに turn_ 回す（地図の y は下向き）。180° なら点対称で、部品は原点から下へ伸びるので
        // 傾きの釣り合いも下向きに取る。
        const double dx = x - mirrorX_, dy = bottomY - mirrorY_, c = std::cos(turn_), s = std::sin(turn_);
        x = mirrorX_ + dx * c - dy * s; bottomY = mirrorY_ + dx * s + dy * c;
        if (c < 0) { pivot = -pivot; }
        const auto spin = RotateZ(-turn_);
        rotation = {spin.w * rotation.x - spin.z * rotation.y, spin.w * rotation.y + spin.z * rotation.x,
                    spin.w * rotation.z + spin.z * rotation.w, spin.w * rotation.w - spin.z * rotation.z};
    }
    HIKARI::GameProjectTransform pose{};
    pose.position = projection_.Point(x + projection_.Lean() * pivot, bottomY, depth);
    pose.rotation = rotation;
    return pose;
}

double StagePartBuilder::WallDepth() const {
    // 地形の一列の奥行き（実体の地形は 1 マスの立方体、ほかは部品の厚み）の分だけ、最後の列の奥 0.15 マスに置く。
    const double row = solid_ ? 1.0 : kTerrainDepth;
    return kBackdropDepth + kTerrainDepth - row * (depthRows_ + 1) + SolidFront();
}

// 姿勢（待機・走り・空中）と左右の向きごとに焼いたモデル。高さ 1 をルールの身長へ合わせる。
void StagePartBuilder::Actor(const Rules::BodyState& body, const Rules::Rect& r, bool echo, double time, bool shown, double grow) {
    static constexpr std::array<std::string_view, 18> kPlayerPoses{
        "Ninja_Idle0_R", "Ninja_Idle1_R", "Ninja_Jog0_R", "Ninja_Jog1_R", "Ninja_Jog2_R", "Ninja_Jog3_R", "Ninja_Jog4_R", "Ninja_Jog5_R", "Ninja_Air0_R",
        "Ninja_Idle0_L", "Ninja_Idle1_L", "Ninja_Jog0_L", "Ninja_Jog1_L", "Ninja_Jog2_L", "Ninja_Jog3_L", "Ninja_Jog4_L", "Ninja_Jog5_L", "Ninja_Air0_L"};
    static constexpr std::array<std::string_view, 2> kEchoPoses{"Ninja_Echo_R", "Ninja_Echo_L"};
    Begin(echo ? "echo" : "player");
    const bool left = body.facing < 0;
    size_t current = 0;
    if (echo) { current = left ? 1 : 0; }
    else {
        const size_t pose = !body.grounded ? 8
            : std::abs(body.walkVx) > .01 ? 2 + static_cast<size_t>(static_cast<int>(std::floor(time * 7.2)) % 6)
            : static_cast<size_t>(static_cast<int>(std::floor(time * 1.5)) % 2);
        current = pose + (left ? 9 : 0);
    }
    // 骨つきの人形が立つ間も、焼いた形は全部出したまま退避点へ置く（出す組を変えるとシーンの作り直しになる）。
    const bool visible = shown && !body.dead && bakedActors_;
    const auto& poses = echo ? std::span<const std::string_view>(kEchoPoses) : std::span<const std::string_view>(kPlayerPoses);
    const double depth = projection_.ActorDepth() + (echo ? -.05 : 0);
    for (size_t i = 0; i < poses.size(); ++i) {
        // 使う一体だけを人物の位置へ。ほかは退避点に留め、人物が動いても触らない。
        if (visible && i == current && echo) {
            // 残影は当たりの箱（高さ 1 マス）に収まる大きさで、形の変化（潰れ・沈み・伸び・傾き）を足元を軸に掛ける。
            const double size = r.h * kEchoFillHeight;
            Add(poses[i], r.x + r.w * .5, r.y + r.h, size * echoWidth_, size * echoHeight_ * std::clamp(grow, .01, 1.0), depth,
                RotateZ(echoLean_), 0);
        } else if (visible && i == current) {
            const double size = r.h * actorScale_;
            Add(poses[i], r.x + r.w * .5, r.y + r.h, size, size * std::clamp(grow, .01, 1.0), depth);
        }
        else { Add(poses[i], kParkX, kParkY, 1, 1, depth, {}, -1, false); }
    }
}
}
