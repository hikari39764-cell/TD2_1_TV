#pragma once
// 画面の中の舞台の骨つき人形（主角・残影）。シーンに置いた人形の物体を毎フレーム人物の位置へ動かし、
// 規則の状態から動作を選んで Animator へ渡す。Play だけで使い、編集中は作者の置いた所のまま。
#include "TD2StageActor.h"
#include "TD2ScreenEffects.h"
#include "TD2ScreenModels.h"
#include "HIKARI/Project/WorldView.h"
#include <string>
#include <string_view>

namespace TD2 {
// 残影の人形への合図（舞台の描き手が規則の状態から作る）。光の向きは規則の座標（下が +y）で、光が来る側。
struct EchoCue {
    bool carried = false;   // 主角の頭の上
    bool stoodOn = false;   // 主角が上に立っている
    bool cramped = false;   // 頭のすぐ上が塞がっている（1 マスの隙間など）
    bool pushed = false;    // 押し刺に押し動かされている
    bool onSpikes = false;  // 上向きの棘の先に載っている（釣り合いを取って揺れる）
    EchoBeam beam = EchoBeam::None;
    double lean = 0;               // 足元を軸にした傾き（ラジアン、正で上が左へ）
    double width = 1, height = 1;  // 形の伸び縮み（潰れ・沈み・伸び）
};

class StagePuppets {
public:
    // 人形の物体を探す（一度見つけたら覚える）。主角の人形が無ければ false で、焼いた形を使う。
    bool Acquire(HIKARI::WorldView& world);
    // その役の人形があるか（無い役は焼いた形を使う）。
    bool Has(StageActorRole role) const;
    // 主角：builder は鏡像の設定を済ませたもの（回された部屋の中なら点対称に移す）。
    // shown が false なら隠す。grow は足元から現れる時の縦の伸び（0→1、オープニングの登場で使う）。
    void Player(HIKARI::WorldView& world, const StagePartBuilder& builder, const Rules::WorldSnapshot& snapshot, double actorScale, double depth,
                bool shown = true, double grow = 1.0);
    // 残影：grow は足元から現れる時の縦の伸び（0→1）。当たりの箱（1 マス）に収まる大きさで、合図に応じて動作を選ぶ。
    void Echo(HIKARI::WorldView& world, const StagePartBuilder& builder, const Rules::BodyState& body, const Rules::Rect& rect,
              bool shown, double grow, double depth, double now, const EchoCue& cue);
    // 舞台を使わない時は二体とも退避点へ。
    void Park(HIKARI::WorldView& world);

private:
    struct Puppet {
        HIKARI::GameProjectObjectHandle handle{};
        StageActorSettings settings;
        std::string clip;
        float speed = 1;
        double yaw = 0;       // 今の向き（度、なめらかに回す）
        double time = -1;     // 最後に立たせた実時間
        bool parked = false, stood = false;
    };
    // 動作を替える（同じ動作なら速さだけ）。
    // その動作がモデルに無ければ今の動作のまま false。
    bool Play(HIKARI::WorldView& world, Puppet& puppet, std::string_view clip, bool loop, float blend, float speed);
    // 頭の上に残影を載せている間は、両手で支える版（Carry…）を先に試す。無ければ元の動作。
    void PlayCarry(HIKARI::WorldView& world, bool carrying, std::string_view clip, bool loop, float blend, float speed);
    // 人物の矩形の足元へ立たせる。yaw は向き（度、0 がカメラ、正で右）、size は背丈（マス）。lean・width・height は形。
    void Stand(HIKARI::WorldView& world, const StagePartBuilder& builder, Puppet& puppet, const Rules::Rect& rect, double yaw,
               double size, double depth, double grow, double now, double lean = 0, double width = 1, double height = 1);
    // 残影の動作を替える。新しい動作がモデルに無ければ元の残影の姿勢。
    void PlayEcho(HIKARI::WorldView& world, std::string_view clip, bool loop, float blend, float speed);
    void ParkOne(HIKARI::WorldView& world, Puppet& puppet);
    Puppet player_, echo_;
    // 動作の記憶：空中に居た秒数と最後の落下の速さ、交換の回数、一度きりの動作を続ける時刻。
    double lastTime_ = -1, airborne_ = 0, fallSpeed_ = 0, landUntil_ = -1, swapUntil_ = -1;
    uint64_t swaps_ = 0;
    bool swapsKnown_ = false;
    // 残影：押され始めにひるむ動作を続ける時刻と、前のフレームに押されていたか。
    double echoFlinchUntil_ = -1;
    bool echoPushed_ = false;
};
}
