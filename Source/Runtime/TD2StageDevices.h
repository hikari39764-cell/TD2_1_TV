#pragma once
// 画面の中の舞台で装置を演じる骨つきの物体（TD2.DeviceActor）。Play の間だけ装置へ割り当てて部屋の位置へ置き、
// 状態に合う片段へ Animator を切り替える。物体の数は変えない（割り当て・返すのは動かすだけ）ので、装置の状態が変わっても
// エンジンが GPU のシーンを作り直さない。一つの装置を幾つかの物体で演じることもある（マスごとのバネ・丸鋸、光線と火花）。
// 割り当てる物体が足りない装置は、焼いた部品で描く。
#include "TD2DeviceActor.h"
#include "TD2ScreenModels.h"
#include "HIKARI/Project/WorldView.h"
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace TD2 {
class StageDevices {
public:
    // 物体を探す（一度見つけたら覚える）。見つけた物体は動作の計算を止めておく。一つも無ければ false。
    bool Acquire(HIKARI::WorldView& world);
    // フレームの初め：どの物体もまだ使っていない印を付ける。
    void BeginFrame();
    // 装置を物体で演じる。builder は部屋の回転・点対称の設定を済ませたもの。beam は光線の今の当たり（光線だけ）、
    // checkpoint は今のチェックポイントか、since は通電・実体が最後に変わってからの実秒。seen は画に写る部屋か
    // （写らなければ回り続ける動作の計算を止める）。演じられた（焼いた部品を出さなくてよい）時 true。
    bool Draw(HIKARI::WorldView& world, const StagePartBuilder& builder, const Rules::DeviceSnapshot& device,
              const Rules::BeamState* beam, bool checkpoint, double since, bool seen);
    // フレームの終わり：このフレームに演じなかった物体を退避点へ。
    void EndFrame(HIKARI::WorldView& world);
    // 舞台を使わない時（編集中など）は全部を退避点へ。
    void Park(HIKARI::WorldView& world);

    // 装置の役一つ：演じる物体の型と片段、置き方（StagePartBuilder::Add と同じ指定）。
    struct Role {
        DeviceActorModel model{};
        std::string_view clip;
        double x = 0, bottom = 0, width = 1, height = 1, depth = 0;
        HIKARI::GameProjectQuaternion rotation{};
        double pivot = .5;
        bool loop = false;   // 回り続ける片段（写る部屋の間だけ計算を続ける）
        bool once = false;   // 一度だけ流して最後の姿勢で止まる片段（ほかは繰り返しで流す。止まった姿勢の片段は 1 フレームしか
                             // ないので、繰り返さないとつなぎの途中で終わってしまう）
        float blend = .1f;   // 前の片段からのつなぎ（秒。0 は切り替え）
        float speed = 1;     // 片段を流す速さ
        float hold = 0;      // 一度きりの片段の長さ（速さ 1 の秒）。流れ終わるまでは写らなくても計算を続ける
    };

private:
    struct Actor {
        HIKARI::GameProjectObjectHandle handle{};
        DeviceActorModel model = DeviceActorModel::PlateHold2;
        std::string key;               // 割り当てた役（装置の ID と役の番号。空なら空き）
        std::string_view clip;         // 今の片段
        float speed = 1;
        HIKARI::GameProjectTransform pose{};
        int settle = 0;                // 姿勢が落ち着くまで動作を計算し続けるフレーム数
        bool placed = false, parked = false, used = false, evaluating = true;
    };
    void Perform(HIKARI::WorldView& world, const StagePartBuilder& builder, Actor& actor, const Role& role, bool seen);
    void ParkOne(HIKARI::WorldView& world, Actor& actor);
    void Release(size_t index);
    std::vector<Actor> actors_;
    std::map<std::string, size_t, std::less<>> byKey_;
    std::vector<Role> roles_;          // Draw の作業用
    std::vector<size_t> chosen_, taken_;
    bool acquired_ = false;
};
}
