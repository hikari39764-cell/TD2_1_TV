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
        if (s.dashing && !p.dashing) { add(ScreenEffectKind::BlinkTrail, s.presentationPlayer, .16, p.presentationPlayer); }
        if (p.dashing && !s.dashing) { add(ScreenEffectKind::Impact, s.presentationPlayer, .2); }
        if (s.bouncing && !p.bouncing) { add(ScreenEffectKind::Bounce, s.presentationPlayer, .22); }
        if (!p.player.grounded && s.player.grounded && p.player.vy > 0) { add(ScreenEffectKind::Land, s.presentationPlayer, .18); }
        if (s.deaths > p.deaths || (!p.player.dead && s.player.dead)) { add(ScreenEffectKind::Death, p.presentationPlayer, .36); }
        if (!s.player.dead && p.player.dead) { add(ScreenEffectKind::Respawn, s.presentationPlayer, .35); }
        // 跳ね返りの残影は予告の開始時に生成エフェクトを出す（出現時には重ねない）。
        // 冲刺で前の残影と入れ替わった時（数が増えた）も、新しい残影の光と前の残影の消える光を出す。
        const bool replaced = s.echoes > p.echoes && p.echo;
        if (s.pendingEcho && !p.pendingEcho) { add(ScreenEffectKind::EchoCreate, *s.pendingEcho, .28); }
        if (s.echo && ((!p.echo && !p.pendingEcho) || replaced)) { add(ScreenEffectKind::EchoCreate, s.echo->rect, .28); }
        if (p.echo && (!s.echo || replaced)) { add(ScreenEffectKind::EchoRecall, p.echo->rect, .22, {}, true); }
        // 交換：入れ替わった本体に復活の光、元の場所へ移った残影に生成の光。
        if (s.swaps > p.swaps) {
            add(ScreenEffectKind::Respawn, s.presentationPlayer, .3);
            if (s.echo) { add(ScreenEffectKind::EchoCreate, s.echo->rect, .28); }
        }
        // 回収：残影が消えるエフェクト、プレイヤーへの吸い込み、残影からプレイヤーへの光跡。
        if (s.recalls > p.recalls && !s.player.dead) {
            add(ScreenEffectKind::EchoAbsorb, s.presentationPlayer, .42);
            if (p.echo || p.pendingEcho) { add(ScreenEffectKind::RecallTrail, s.presentationPlayer, .2, p.echo ? p.echo->rect : *p.pendingEcho); }
        }
        if (p.pendingEcho && !s.pendingEcho && !s.echo) { add(ScreenEffectKind::EchoBlocked, *p.pendingEcho, .24); }
        for (const auto& d : s.devices) {
            // 圧力板・扉・橋は部品自体の動きで切り替えを見せる。
            if (d.kind == Rules::DeviceKind::Plate || d.kind == Rules::DeviceKind::Door || d.kind == Rules::DeviceKind::Bridge) { continue; }
            const auto old = std::find_if(p.devices.begin(), p.devices.end(), [&d](const auto& v) { return v.id == d.id && v.kind == d.kind; });
            if (old != p.devices.end() && (old->active != d.active || old->pressed != d.pressed)) { add(ScreenEffectKind::DeviceSwitch, d.rect, .22, {}, !d.active); }
        }
        if (!s.checkpointId.empty() && s.checkpointId != p.checkpointId) {
            const auto d = std::find_if(s.devices.begin(), s.devices.end(), [&s](const auto& v) { return v.id == s.checkpointId; });
            add(ScreenEffectKind::Checkpoint, d == s.devices.end() ? s.presentationPlayer : d->rect, .45);
        }
        // 観測した軌跡だけを終点まで伸ばす（新しい dash は作らない）。
        if (s.dashing || p.dashing) for (auto& effect : effects_) {
            if (effect.kind == ScreenEffectKind::BlinkTrail) { effect.rect = s.presentationPlayer; }
        }
        // 回収の光跡と吸い込みはプレイヤーに付いて動く。
        for (auto& effect : effects_) {
            if (effect.kind == ScreenEffectKind::RecallTrail || effect.kind == ScreenEffectKind::EchoAbsorb) { effect.rect = s.presentationPlayer; }
        }
    }
    previous_ = s;
}
}
