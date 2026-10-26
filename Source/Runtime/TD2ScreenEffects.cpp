#include "TD2ScreenEffects.h"
#include <algorithm>
#include <cmath>

namespace TD2 {
namespace {
// 土煙・火花を出す間隔（秒）。続く間ずっと出すと粒が詰まるので間を空ける。
constexpr double kEchoDustEvery = .11, kBeamSparkEvery = .13;
}

EchoBeamContact BeamOnEcho(const Rules::WorldSnapshot& snapshot, const Rules::Rect& e) {
    // 下向きの光の下端が残影の上辺に、横向きの光の先が残影の側面に届いていれば受けている。
    // 規則では光の先は残影の面にちょうど止まる。許しは薄い封じ壁（0.02 マス）より小さく取り、壁越しの残影を数えない。
    constexpr double kTouch = .01;
    for (const auto& beam : snapshot.beams) {
        const auto device = std::find_if(snapshot.devices.begin(), snapshot.devices.end(), [&](const auto& d) { return d.id == beam.sourceId; });
        if (device == snapshot.devices.end()) { continue; }
        const auto& b = beam.rect;
        const bool acrossX = b.x < e.x + e.w && e.x < b.x + b.w, acrossY = b.y < e.y + e.h && e.y < b.y + b.h;
        switch (device->direction) {
        case Rules::Direction::Down:
            if (acrossX && std::abs(b.y + b.h - e.y) < kTouch) { return {EchoBeam::Above, b.x + b.w * .5, e.y}; }
            break;
        case Rules::Direction::Right:
            if (acrossY && std::abs(b.x + b.w - e.x) < kTouch) { return {EchoBeam::FromLeft, e.x, b.y + b.h * .5}; }
            break;
        case Rules::Direction::Left:
            if (acrossY && std::abs(b.x - (e.x + e.w)) < kTouch) { return {EchoBeam::FromRight, e.x + e.w, b.y + b.h * .5}; }
            break;
        default: break;
        }
    }
    return {};
}

void ScreenEffects::Reset() { previous_.reset(); effects_.clear(); nextDust_ = nextSpark_ = 0; }
void ScreenEffects::Add(ScreenEffectKind kind, const Rules::Rect& rect, double now, double duration, uint64_t tick) {
    if (effects_.size() == 24) { effects_.erase(effects_.begin()); }
    effects_.push_back({kind, rect, {}, now, duration, false, tick, nextEventId_++});
}
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
        // ダッシュで前の残影と入れ替わった時（数が増えた）も、新しい残影の光と前の残影の消える光を出す。
        const bool replaced = s.echoes > p.echoes && p.echo;
        if (s.pendingEcho && !p.pendingEcho) { add(ScreenEffectKind::EchoCreate, *s.pendingEcho, .28); }
        if (s.echo && ((!p.echo && !p.pendingEcho) || replaced)) { add(ScreenEffectKind::EchoCreate, s.echo->rect, .28); }
        // 門・押し刺で潰れた時は、回収（吸い戻し）ではなく砕ける方のエフェクト。
        if (p.echo && s.crushes > p.crushes) { add(ScreenEffectKind::EchoBlocked, p.echo->rect, .24); }
        else if (p.echo && (!s.echo || replaced)) { add(ScreenEffectKind::EchoRecall, p.echo->rect, .22, {}, true); }
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
        // 押し刺に押されて滑る残影の足元に土煙、残影で止まった光の当たる所に火花。続く間、間を空けて出す。
        if (s.echo && s.echoPushed && s.echo->grounded && !s.dashing && s.realTime >= nextDust_) {
            add(ScreenEffectKind::EchoDust, s.echo->rect, .2); nextDust_ = s.realTime + kEchoDustEvery;
        }
        if (s.echo && s.realTime >= nextSpark_) {
            if (const auto contact = BeamOnEcho(s, s.echo->rect); contact.from != EchoBeam::None) {
                add(ScreenEffectKind::BeamSpark, Rules::Rect{contact.x - .1, contact.y - .1, .2, .2}, .2);
                nextSpark_ = s.realTime + kBeamSparkEvery;
            }
        }
        for (const auto& d : s.devices) {
            // 圧力板・扉・橋・通関の扉は部品自体の動きで切り替えを見せる。
            if (d.kind == Rules::DeviceKind::Plate || d.kind == Rules::DeviceKind::Door || d.kind == Rules::DeviceKind::Bridge ||
                d.kind == Rules::DeviceKind::Goal) { continue; }
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
