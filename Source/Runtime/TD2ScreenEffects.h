#pragma once
#include "Rules/TD2Rules.h"
#include <optional>
#include <vector>

namespace TD2 {
enum class ScreenEffectKind { BlinkTrail, Impact, Bounce, EchoCreate, EchoRecall, EchoBlocked, Death, Respawn, DeviceSwitch, Checkpoint, Land };
struct ScreenEffect {
    ScreenEffectKind kind = ScreenEffectKind::Impact;
    Rules::Rect rect, source;
    double born = 0, duration = .2;
    bool reversed = false;
    uint64_t tick = 0;
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
};
} // namespace TD2
