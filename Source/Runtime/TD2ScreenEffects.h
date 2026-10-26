#pragma once
#include "Rules/TD2Rules.h"
#include <optional>
#include <vector>

namespace TD2 {
// EchoAbsorb は回収時にプレイヤーで出すエフェクト、RecallTrail は残影からプレイヤーへの光跡（部品で描く）。
// Arrive はオープニングの終わりにプレイヤーが画面の中へ現れる時（規則からは起きず、演出が足す）。
// EchoDust は押し刺に押されて滑る残影の足元の土煙、BeamSpark は残影で止まった光の当たる所の火花（どちらも続く間、間を空けて出す）。
enum class ScreenEffectKind { BlinkTrail, Impact, Bounce, EchoCreate, EchoRecall, EchoBlocked, Death, Respawn, DeviceSwitch, Checkpoint, Land,
                              EchoAbsorb, RecallTrail, Arrive, EchoDust, BeamSpark };

// 残影の箱で止まっている光の来る向きと、光が当たる点（規則の座標。回された部屋でも規則の上下で見る）。
enum class EchoBeam { None, Above, FromLeft, FromRight };
struct EchoBeamContact {
    EchoBeam from = EchoBeam::None;
    double x = 0, y = 0;
};
EchoBeamContact BeamOnEcho(const Rules::WorldSnapshot& snapshot, const Rules::Rect& echo);
struct ScreenEffect {
    ScreenEffectKind kind = ScreenEffectKind::Impact;
    Rules::Rect rect, source;
    double born = 0, duration = .2;
    bool reversed = false;
    uint64_t tick = 0;
    // 座標や時刻に依らないイベント ID。同じ VFX を再生し直さない。
    uint64_t id = 0;
};

// tick は再実行せず、観測した差分から表示用の反応を作る。
class ScreenEffects {
public:
    void Observe(const Rules::WorldSnapshot& snapshot);
    void Reset();
    const std::vector<ScreenEffect>& Items() const noexcept { return effects_; }
    // 規則の差分からではなく、演出が直接足すイベント（now は規則の時計）。
    void Add(ScreenEffectKind kind, const Rules::Rect& rect, double now, double duration, uint64_t tick);
private:
    std::optional<Rules::WorldSnapshot> previous_;
    std::vector<ScreenEffect> effects_;
    // Reset 後も旧 VFX が終わるまでは記録が残るため、ID を再利用しない。
    uint64_t nextEventId_ = 1;
    // 土煙と火花を次に出してよい時刻（規則の実時刻）。
    double nextDust_ = 0, nextSpark_ = 0;
};
}
