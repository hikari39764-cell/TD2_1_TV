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
        effects_.push_back({kind, rect, source, s.realTime, duration, reversed, s.tick, nextEventId_++});
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
        // 蹴り返しの残影は予告が始まった時に生成の特効を鳴らす（予告の間に姿を刷り出し、出現時は重ねて鳴らさない）。
        if (s.pendingEcho && !p.pendingEcho) { add(ScreenEffectKind::EchoCreate, *s.pendingEcho, .28); }
        if (s.echo && !p.echo && !p.pendingEcho) { add(ScreenEffectKind::EchoCreate, s.echo->rect, .28); }
        if (p.echo && !s.echo) { add(ScreenEffectKind::EchoRecall, p.echo->rect, .22, {}, true); }
        // 回収：残影側の消える特効に加え、主角の体で吸い込む特効と、残影から主角へ伸びる光跡（向きが分かる）。
        if (s.recalls > p.recalls && s.playerPresent && !s.player.dead) {
            add(ScreenEffectKind::EchoAbsorb, s.presentationPlayer, .42);
            if (p.echo || p.pendingEcho) { add(ScreenEffectKind::RecallTrail, s.presentationPlayer, .2, p.echo ? p.echo->rect : *p.pendingEcho); }
        }
        if (p.pendingEcho && !s.pendingEcho && !s.echo && s.playerPresent) { add(ScreenEffectKind::EchoBlocked, *p.pendingEcho, .24); }
        for (const auto& d : s.devices) {
            // 圧力板・扉・橋は部品そのものの動き（灯の明滅、扉の羽根・橋板の出入り）で切り替えを見せる。
            if (d.kind == Rules::DeviceKind::Plate || d.kind == Rules::DeviceKind::Door || d.kind == Rules::DeviceKind::Bridge) { continue; }
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
        // 回収の光跡と吸い込みは主角に付いて動く（回収した直後も歩いて動けるため）。
        for (auto& effect : effects_) {
            if (effect.kind == ScreenEffectKind::RecallTrail || effect.kind == ScreenEffectKind::EchoAbsorb) { effect.rect = s.presentationPlayer; }
        }
    }
    previous_ = s;
}
} // namespace TD2
