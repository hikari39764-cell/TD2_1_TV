#pragma once
#include "Rules/TD2Rules.h"
#include <optional>
#include <vector>

namespace TD2 {
// EchoAbsorb は回収の瞬間に主角の体で鳴らす特効、RecallTrail は残影から主角へ伸びる光跡（部品で描き、VFX は鳴らさない）。
enum class ScreenEffectKind { BlinkTrail, Impact, Bounce, EchoCreate, EchoRecall, EchoBlocked, Death, Respawn, DeviceSwitch, Checkpoint, Land,
                              EchoAbsorb, RecallTrail };
struct ScreenEffect {
    ScreenEffectKind kind = ScreenEffectKind::Impact;
    Rules::Rect rect, source;
    double born = 0, duration = .2;
    bool reversed = false;
    uint64_t tick = 0;
    // 座標や描画時刻から独立した事件 ID。追従しても同じ VFX を再生し直さない。
    uint64_t id = 0;
};

// 固定 tick を再実行せず、観測した差分から有界な立体 feedback を作る。
class ScreenEffects {
public:
    void Observe(const Rules::WorldSnapshot& snapshot);
    void Reset();
    const std::vector<ScreenEffect>& Items() const noexcept { return effects_; }
private:
    std::optional<Rules::WorldSnapshot> previous_;
    std::vector<ScreenEffect> effects_;
    // Reset 後も旧 VFX が終わるまでは記録が残るため、ID を再利用しない。
    uint64_t nextEventId_ = 1;
};
} // namespace TD2
