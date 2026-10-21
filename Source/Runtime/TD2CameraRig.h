#pragma once
// 大区域のカメラ。作者が置いた区域カメラ（その部屋の最良の構図）を基準に、毎フレームプレイヤーへ向けて解く。
// ・同じ大区域の中で部屋を移ると、基準を次の区域カメラへ滑らかに移す。
// ・基準の上に、プレイヤーへの平行移動（視差）、注視の振り、進む向きへの先読み、
// 横移動時のわずかな傾き、止まっても続く呼吸を重ね、作者が決めた範囲に収める。
// ・大区域どうしの切り替えはエンジンの演出（Sequence）が受け持ち、その間はカメラを出さない。
#include "../Common/TD2Math.h"
#include "HIKARI/Project/GameComponent.h"
#include "HIKARI/Project/ValueTypes.h"
#include <string>
#include <string_view>

namespace TD2 {
inline constexpr std::string_view kCameraAreaComponentType = "TD2.CameraArea";

// 大区域ひとつ分のカメラの動き方（初期値は注視の振り 10° 前後、平滑 0.8 秒程度）。
struct CameraAreaSettings {
    std::string regions;                                       // この大区域の区域 ID（カンマ区切り）
    float followX = .32f, followY = .45f;                      // プレイヤーの方へカメラを平行移動する割合（横・縦）
    float rangeX = .7f, rangeY = .9f;                          // カメラが基準から動ける範囲（m）
    float aimFollow = .3f, maxYaw = 8.f, maxPitch = 6.f;       // 注視をプレイヤーへ振る割合と上限（度）
    float lookCarry = .55f;                                    // 平行移動に注視が付いて行く割合（残りは基準の方を向く）
    float lookAhead = .35f;                                    // 進む向きへの先読み（秒）
    float regionSeconds = .85f, positionSeconds = .5f, aimSeconds = .8f;  // 平滑（区域カメラの乗り換え・位置・注視）
    float breathe = .03f, breatheAngle = .45f;                 // 止まっていても続く呼吸（m・度）
    float handheld = 1.f;                                      // 手持ちの細かい揺れの強さ（0 で無し、1 で標準）
    float bank = 2.2f;                                         // 横に動く時の傾き（度 / (m/s)）
    // 区域カメラの自動生成用（エディタだけが使う）：画面の高さが画に占める割合・出口側へ回す角度・画面より上げる高さ。
    float frameFill = .35f, orbitDegrees = 15.f, cameraLift = .3f;
};

class CameraAreaComponent final : public HIKARI::IGameComponent {
public:
    const CameraAreaSettings& Settings() const noexcept { return settings_; }
    void DescribeProperties(HIKARI::PropertyFields& f) override {
        f.Text("regions", "区域", settings_.regions);
        f.Float("followX", "横の追従", settings_.followX, 0, 1);
        f.Float("followY", "縦の追従", settings_.followY, 0, 1);
        f.Float("rangeX", "横の範囲", settings_.rangeX, 0, 3);
        f.Float("rangeY", "縦の範囲", settings_.rangeY, 0, 3);
        f.Float("aimFollow", "視線の追従", settings_.aimFollow, 0, 1);
        f.Float("maxYaw", "視線の左右の上限", settings_.maxYaw, 0, 25);
        f.Float("maxPitch", "視線の上下の上限", settings_.maxPitch, 0, 25);
        f.Float("lookCarry", "視線の平行移動", settings_.lookCarry, 0, 1);
        f.Float("lookAhead", "先読み", settings_.lookAhead, 0, 1.5f);
        f.Float("regionSeconds", "区域を移る時のなめらかさ", settings_.regionSeconds, .1f, 3);
        f.Float("positionSeconds", "位置のなめらかさ", settings_.positionSeconds, .05f, 3);
        f.Float("aimSeconds", "視線のなめらかさ", settings_.aimSeconds, .05f, 3);
        f.Float("breathe", "呼吸の幅", settings_.breathe, 0, .2f);
        f.Float("breatheAngle", "呼吸の角度", settings_.breatheAngle, 0, 3);
        f.Float("handheld", "手持ちの揺れ", settings_.handheld, 0, 3);
        f.Float("bank", "横移動の傾き", settings_.bank, 0, 6);
        f.Float("frameFill", "画面が占める高さ", settings_.frameFill, .15f, .8f);
        f.Float("orbitDegrees", "出口側への角度", settings_.orbitDegrees, 0, 40);
        f.Float("cameraLift", "画面より上の高さ", settings_.cameraLift, -1, 2);
    }

private:
    CameraAreaSettings settings_;
};

// 区域ひとつの基準と画面の位置（プレイヤーの 2D 位置を 3D の画面上の点へ移す）。
struct CameraRegionFrame {
    HIKARI::GameProjectFloat3 eye{}, look{};                    // 区域カメラの目と注視点（注視点は画面の中心の深さ）
    float verticalFovRadians = .4538f;
    HIKARI::GameProjectTransform screen{};                      // テレビのワールド変換
    HIKARI::GameProjectFloat3 screenCenter{0, .4f, .28f};       // テレビのローカルでの画面の中心
    float screenWidth = .84f, screenHeight = .49f;              // テレビのローカルでの画面の大きさ
    double worldX = 0, worldY = 0, cols = 1, rows = 1;          // 区域の世界のマス
    bool valid = false;
};

struct CameraRigPose {
    HIKARI::GameProjectFloat3 eye{}, look{}, up{0, 1, 0};
    float verticalFovRadians = .4538f;
};

class CameraRig {
public:
    // 世界のマスの点（プレイヤーの中心）と速度を、テレビの画面上の 3D の点と速度へ移す。
    static HIKARI::GameProjectFloat3 ScreenPoint(const CameraRegionFrame& frame, double x, double y);
    static HIKARI::GameProjectFloat3 ScreenVelocity(const CameraRegionFrame& frame, double vx, double vy);
    // 区域カメラの向き（ローカル +Z をワールドへ回した単位ベクトル）。
    static HIKARI::GameProjectFloat3 Forward(const HIKARI::GameProjectTransform& transform);

    // 基準をこの姿勢へ置き直す（ずれは 0 から寄せ直す）。復活と演出の終わりに使う。
    void Snap(const HIKARI::GameProjectFloat3& eye, const HIKARI::GameProjectFloat3& look, float verticalFovRadians);
    bool Ready() const noexcept { return ready_; }
    // 基準のカメラを from から to への剛体の動きで運ぶ（乗っているテレビが回る・動く間、画面との位置関係を保つ）。
    void Carry(const HIKARI::GameProjectTransform& from, const HIKARI::GameProjectTransform& to);
    // 1 フレーム進める。target はプレイヤーの画面上の点、velocity はその速さ（m/s）。present=false なら追わない。
    CameraRigPose Update(const CameraRegionFrame& frame, const CameraAreaSettings& area, const HIKARI::GameProjectFloat3& target,
                         const HIKARI::GameProjectFloat3& velocity, bool present, double dt, double time);
    // 今の呼吸と手持ちの揺れ。roll は前の軸まわりの傾き（ラジアン）。揺らす向きは追従の基準の向き。
    void Breath(const CameraAreaSettings& area, double time, HIKARI::GameProjectFloat3& eyeOffset,
                HIKARI::GameProjectFloat3& lookOffset, double& roll) const;
    // 与えたカメラ（目と注視点）の向きで揺らす版。切り替え中は描いているカメラの向きで揺らし、終了時に揺れの向きが跳ばないようにする。
    static void BreathAround(const CameraAreaSettings& area, double time, const HIKARI::GameProjectFloat3& eye,
                             const HIKARI::GameProjectFloat3& look, HIKARI::GameProjectFloat3& eyeOffset,
                             HIKARI::GameProjectFloat3& lookOffset, double& roll);
    const CameraRigPose& Last() const noexcept { return last_; }

private:
    // 三成分それぞれの臨界減衰のばね。
    struct Spring {
        Math::Vec3 value{}, velocity{};
        void Snap(const Math::Vec3& v) { value = v; velocity = {}; }
        void Step(const Math::Vec3& target, double seconds, double dt) {
            for (size_t i = 0; i < 3; ++i) { value[i] = Math::CriticalDamp(value[i], target[i], velocity[i], seconds, dt); }
        }
    };
    Spring baseEye_, baseLook_, offset_, aim_, roll_;
    Math::Vec3 right_{1, 0, 0}, up_{0, 1, 0};
    CameraRigPose last_{};
    // 画角（ラジアン）とその速さ。区域カメラの画角が違っても、位置と同じ速さで寄せる（乗り換えの瞬間に画がズームしない）。
    double fov_ = .4538, fovVelocity_ = 0;
    bool ready_ = false;
};
}
