#include "TD2ScreenEffects.h"
#include <algorithm>

namespace TD2 {
void ScreenEffects::Reset() { previous_.reset(); effects_.clear(); }
void ScreenEffects::Observe(const Rules::WorldSnapshot& s) {
    if (previous_ && (s.realTime < previous_->realTime || s.regionId != previous_->regionId)) { Reset(); }
    std::erase_if(effects_, [&s](const auto& e) { return s.realTime - e.born >= e.duration; });
    const auto add = [this, &s](ScreenEffectKind kind, const Rules::Rect& rect, double duration,
                                const Rules::Rect& source = {}, bool reversed = false) {
        if (effects_.size() == 24) { effects_.erase(effects_.begin()); }
        effects_.push_back({kind, rect, source, s.realTime, duration, reversed, s.tick});
    };
    if (previous_) {
        const auto& p = *previous_;
        if (s.playerPresent && p.playerPresent) {
            if (s.dashing && !p.dashing) { add(ScreenEffectKind::BlinkTrail, s.presentationPlayer, .16, p.presentationPlayer); }
            if (p.dashing && !s.dashing) { add(ScreenEffectKind::Impact, s.presentationPlayer, .2); }
            if (s.bouncing && !p.bouncing) { add(ScreenEffectKind::Bounce, s.presentationPlayer, .22); }
            if (!p.player.grounded && s.player.grounded && p.player.vy > 0) { add(ScreenEffectKind::Land, s.presentationPlayer, .18); }
        }
        if (p.playerPresent && (s.deaths > p.deaths || (!p.player.dead && s.player.dead))) { add(ScreenEffectKind::Death, p.presentationPlayer, .36); }
        if (s.playerPresent && !s.player.dead && p.player.dead) { add(ScreenEffectKind::Respawn, s.presentationPlayer, .35); }
        if (s.echo && !p.echo) { add(ScreenEffectKind::EchoCreate, s.echo->rect, .28); }
        if (p.echo && !s.echo) { add(ScreenEffectKind::EchoRecall, p.echo->rect, .22, {}, true); }
        if (p.pendingEcho && !s.pendingEcho && !s.echo && s.playerPresent) { add(ScreenEffectKind::EchoBlocked, *p.pendingEcho, .24); }
        for (const auto& d : s.devices) {
            const auto old = std::find_if(p.devices.begin(), p.devices.end(), [&d](const auto& v) { return v.id == d.id && v.kind == d.kind; });
            if (old != p.devices.end() && (old->active != d.active || old->pressed != d.pressed)) { add(ScreenEffectKind::DeviceSwitch, d.rect, .22, {}, !d.active); }
        }
        if (!s.checkpointId.empty() && s.checkpointId != p.checkpointId) {
            const auto d = std::find_if(s.devices.begin(), s.devices.end(), [&s](const auto& v) { return v.id == s.checkpointId; });
            add(ScreenEffectKind::Checkpoint, d == s.devices.end() ? s.presentationPlayer : d->rect, .45);
        }
        // 入口 snapshot から新しい dash を捏造せず、観測済みの短い軌跡だけ終点まで伸ばす。
        if (s.dashing || p.dashing) for (auto& effect : effects_) {
            if (effect.kind == ScreenEffectKind::BlinkTrail) { effect.rect = s.presentationPlayer; }
        }
    }
    previous_ = s;
}
} // namespace TD2
