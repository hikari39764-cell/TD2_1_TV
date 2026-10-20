#pragma once
// 画面の中の舞台の骨つき人形（主角・残影）。シーンに置いた人形の物体を毎フレーム人物の位置へ動かし、
// 規則の状態から動作を選んで Animator へ渡す。Play だけで使い、編集中は作者の置いた所のまま。
#include "TD2StageActor.h"
#include "TD2ScreenModels.h"
#include "HIKARI/Project/WorldView.h"
#include <string_view>

namespace TD2 {
class StagePuppets {
public:
    // 人形の物体を探す（一度見つけたら覚える）。主角の人形が無ければ false で、焼いた形を使う。
    bool Acquire(HIKARI::WorldView& world);
    // その役の人形があるか（無い役は焼いた形を使う）。
    bool Has(StageActorRole role) const;
    // 主角：builder は鏡像の設定を済ませたもの（回された部屋の中なら点対称に移す）。
    void Player(HIKARI::WorldView& world, const StagePartBuilder& builder, const Rules::WorldSnapshot& snapshot, double actorScale, double depth);
    // 残影：grow は足元から現れる時の縦の伸び（0→1）。
    void Echo(HIKARI::WorldView& world, const StagePartBuilder& builder, const Rules::BodyState& body, const Rules::Rect& rect,
              bool shown, double grow, double actorScale, double depth, double now);
    // 舞台を使わない時は二体とも退避点へ。
    void Park(HIKARI::WorldView& world);

private:
    struct Puppet {
        HIKARI::GameProjectObjectHandle handle{};
        StageActorSettings settings;
        std::string_view clip;
        float speed = 1;
        double yaw = 0;       // 今の向き（度、なめらかに回す）
        double time = -1;     // 最後に立たせた実時間
        bool parked = false, stood = false;
    };
    // 動作を替える（同じ動作なら速さだけ）。
    void Play(HIKARI::WorldView& world, Puppet& puppet, std::string_view clip, bool loop, float blend, float speed);
    // 人物の矩形の足元へ立たせる。facing は右が正。
    void Stand(HIKARI::WorldView& world, const StagePartBuilder& builder, Puppet& puppet, const Rules::Rect& rect, int facing,
               double actorScale, double depth, double grow, double now);
    void ParkOne(HIKARI::WorldView& world, Puppet& puppet);
    Puppet player_, echo_;
    // 動作の記憶：空中に居た秒数と最後の落下の速さ、交換の回数、一度きりの動作を続ける時刻。
    double lastTime_ = -1, airborne_ = 0, fallSpeed_ = 0, landUntil_ = -1, swapUntil_ = -1;
    uint64_t swaps_ = 0;
    bool swapsKnown_ = false;
};
}
