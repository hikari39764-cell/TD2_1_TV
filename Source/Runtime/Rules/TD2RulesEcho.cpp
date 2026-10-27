// 残影と冲刺：残影の出し入れ、交換、冲刺と壁蹴り、死と復活。
#include "TD2RulesSimulation.h"

namespace TD2::Rules::Detail {
Rect Simulation::EchoOrigin(const Rect& source) const {
    Rect origin{source.x, source.y + source.h - 1.0, source.w, 1.0};
    const Rect foot{origin.x, origin.y + origin.h - .01 / 32, origin.w, .6 / 32};
    auto supports = Colliders(foot, nullptr, false, {}, true);
    std::erase_if(supports, [&](const Collider& c) {
        return !OverlapX(origin, c.rect) || std::abs(origin.y + origin.h - c.rect.y) >= kGroundTolerance;
    });
    std::vector<Rect> candidates;
    const int64_t first = int64_t(std::floor(origin.x + kEps)), last = int64_t(std::floor(origin.x + origin.w - kEps));
    for (int64_t col = first; col <= last; ++col)
        candidates.push_back({double(col) + (1.0 - origin.w) / 2, origin.y, origin.w, origin.h});
    std::sort(candidates.begin(), candidates.end(), [&](const Rect& a, const Rect& b) {
        const double da = std::abs(a.x - origin.x), db = std::abs(b.x - origin.x);
        return da == db ? a.x < b.x : da < db;
    });
    for (const auto& candidate : candidates) {
        const bool supported = std::any_of(supports.begin(), supports.end(), [&](const Collider& c) { return OverlapX(candidate, c.rect); });
        BodyState body = MakeBody({origin.x, origin.y}, true);
        if (supported && Clear(Sweep(origin, candidate), &body, true, {}, true)) return candidate;
    }
    return origin;
}
void Simulation::CreateEcho(const Rect& origin, int facing) {
    s.echo = MakeBody({origin.x, origin.y}, true); s.echo->facing = facing; ++s.echoes;
    s.echoOverlapGrace = !s.player.dead && Overlap(s.player.rect, s.echo->rect);
    ResolveEchoSpikes(); Probe(*s.echo, true);
    Event(EventKind::EchoCreated, "echo", {}, s.echo->rect);
}
void Simulation::Pending() {
    if (!s.pendingEcho) return;
    s.pendingEcho->age += kStep;
    if (s.pendingEcho->age + kEps < .16) return;
    const auto pending = *s.pendingEcho;
    if (!Clear(pending.origin, nullptr, false, {}, true)) {
        if (pending.age >= .66) {
            s.pendingEcho.reset(); Event(EventKind::EchoSpawnBlocked, "echo", "source-occupied", pending.origin);
        }
        return;
    }
    s.pendingEcho.reset(); CreateEcho(pending.origin, pending.facing);
}
double Simulation::HorizontalVelocity() const {
    return s.player.walkVx + s.player.reboundDirection * 7.5 * std::clamp(s.player.reboundTime / .14, 0.0, 1.0);
}
// ダッシュが決まった時に前の残影を引き取る（新しい残影はダッシュの出発点に出る）。
void Simulation::RetireEcho() {
    if (!s.echo && !s.pendingEcho) return;
    const Rect source = s.echo ? s.echo->rect : s.pendingEcho->origin;
    s.echo.reset(); s.pendingEcho.reset(); s.echoOverlapGrace = false;
    ++s.recalls;
    Event(EventKind::EchoRecalled, "echo", {}, source);
}
// 残影をその場で消す（大区域を出た時。回収の演出は出さない）。
void Simulation::ReplaceEcho() {
    const Rect source = s.echo ? s.echo->rect : s.pendingEcho->origin;
    s.echo.reset(); s.pendingEcho.reset(); s.echoOverlapGrace = false;
    if (s.player.supportId == "echo") { s.player.supportId.clear(); s.player.grounded = false; }
    Event(EventKind::EchoRemoved, "echo", "replaced", source);
}
// 交換：本体と残影の足元を入れ替える（大きさはそれぞれのまま。本体は残影より背が高い）。勢いは捨てる。
// 重なっている時は上下を入れ替える：残影に乗って交換すると残影が頭の上へ、頭に載せて交換すると残影の上に立つ。
// 入れ替えた先が塞がっている時は交換しない。
void Simulation::Swap() {
    if (!s.echo || s.player.dead) return;
    auto& p = s.player; auto& e = *s.echo;
    const Rect from = p.rect;
    Rect playerTo{e.rect.x + (e.rect.w - p.rect.w) / 2, e.rect.y + e.rect.h - p.rect.h, p.rect.w, p.rect.h};
    Rect echoTo{p.rect.x + (p.rect.w - e.rect.w) / 2, p.rect.y + p.rect.h - e.rect.h, e.rect.w, e.rect.h};
    if (Rider(&p, e, "echo")) {
        playerTo = {e.rect.x + (e.rect.w - p.rect.w) / 2, e.rect.y + e.rect.h - p.rect.h, p.rect.w, p.rect.h};
        echoTo = {playerTo.x + (p.rect.w - e.rect.w) / 2, playerTo.y - e.rect.h, e.rect.w, e.rect.h};
    } else if (Rider(&e, p, "player")) {
        echoTo = {p.rect.x + (p.rect.w - e.rect.w) / 2, p.rect.y + p.rect.h - e.rect.h, e.rect.w, e.rect.h};
        playerTo = {echoTo.x + (e.rect.w - p.rect.w) / 2, echoTo.y - p.rect.h, p.rect.w, p.rect.h};
    }
    if (!Clear(playerTo, nullptr, false, {}, true) || !Clear(echoTo, nullptr, true, {}, true)) {
        Event(EventKind::BlinkRejected, "player", "swap-blocked", p.rect); return;
    }
    p.rect = playerTo; e.rect = echoTo;
    p.vx = p.vy = p.walkVx = 0; p.reboundTime = 0; p.reboundDirection = 0;
    e.vx = e.vy = 0;
    s.dash.reset(); s.bounce.reset(); s.blinkBuffer = 0; s.echoOverlapGrace = false;
    Probe(p); Probe(e, true); ValidateSupports();
    ++s.swaps;
    Event(EventKind::EchoSwapped, "player", {}, from);
}
bool Simulation::ConsumeBlink() {
    if (s.blinkBuffer <= 0 || !s.player.grounded) return false;
    Probe(s.player);
    if (!s.player.grounded) return false;
    s.blinkBuffer = 0;
    auto& p = s.player;
    const Rect source = p.rect;
    const Rect unsnapped{p.rect.x, p.rect.y + p.rect.h - 1.0, p.rect.w, 1.0};
    if (!Clear(unsnapped, nullptr, false, {}, true)) {
        Event(EventKind::BlinkRejected, "player", "echo-headroom", p.rect); return false;
    }
    const Rect origin = EchoOrigin(source);
    const int facing = p.facing;
    const double intended = source.x + facing * 4.0;
    double x = intended;
    bool bounced = false;
    const Rect route = Sweep(source, {x, source.y, source.w, source.h});
    // ダッシュは壁を通り抜けない。経路で最初に当たる実体で止まり、そこで壁蹴り（跳ね返り）になる。
    // 一方通行の床は横から当たらず、バネの踏み板は下の着地処理で上に乗せる。
    for (const auto& c : Colliders(route, nullptr, false, {}, true, true)) {
        if (c.spring || !Overlap(route, c.rect)) continue;
        const double limit = facing > 0 ? std::max(source.x, c.rect.x - source.w) : std::min(source.x, c.rect.x + c.rect.w);
        if (facing > 0 ? limit < x : limit > x) { x = limit; bounced = true; }
    }
    double y = source.y;
    for (int pass = 0; pass < 256; ++pass) {
        const Rect endpoint{x, y, source.w, source.h};
        std::vector<Collider> blockers;
        for (const auto& c : Colliders(endpoint, nullptr, false, {}, true, true)) if (Overlap(endpoint, c.rect)) blockers.push_back(c);
        if (blockers.empty()) break;
        // 着地点がバネの踏み板だけに掛かるなら、横の位置のまま踏み板の上に乗せる。
        if (std::all_of(blockers.begin(), blockers.end(), [](const Collider& c) { return c.spring; })) {
            double top = blockers.front().rect.y;
            for (const auto& c : blockers) top = std::min(top, c.rect.y);
            const Rect landing{x, top - source.h, source.w, source.h};
            const Rect clearance{x, landing.y, source.w, source.y + source.h - landing.y};
            bool ceiling = false;
            for (const auto& c : Colliders(clearance, nullptr, false, {}, true, true))
                if (!c.spring && Overlap(clearance, c.rect)) ceiling = true;
            if (!ceiling && Clear(landing, nullptr, false, {}, true)) { y = landing.y; break; }
        }
        double nextX = x;
        for (const auto& c : blockers) {
            const double near = facing > 0 ? std::max(source.x, c.rect.x - source.w) : std::min(source.x, c.rect.x + c.rect.w);
            if (facing > 0 ? near < nextX : near > nextX) { nextX = near; bounced = true; }
        }
        if (std::abs(nextX - x) < kEps) break;
        x = nextX;
    }
    const Rect contact{x, y, source.w, source.h};
    // 出発地点まで埋まっている時は瞬間移動しない。
    if (!Clear(contact, nullptr, false, {}, true)) {
        Event(EventKind::BlinkRejected, "player", "destination-occupied", p.rect); return false;
    }
    s.bounce.reset(); s.dash.reset();
    RetireEcho();
    const double distance = std::abs(x - source.x);
    if (distance > 1.0 / 32) s.dash = DashState{source, contact, facing, 0, .14 * (.5 + .5 * std::min(1.0, distance / 4.0))};
    p.rect = contact; p.vy = 0; p.vx = p.walkVx;
    p.grounded = false; p.supportId.clear(); p.reboundTime = 0; p.reboundDirection = 0;
    if (bounced) {
        p.reboundTime = .14; p.reboundDirection = -facing; p.vx = HorizontalVelocity(); p.vy = -15.0;
        Move(p, false, p.reboundDirection * 22.0 / 32, true);
        if (Overlap(p.rect, origin)) {
            const double outside = facing > 0 ? origin.x - p.rect.w - 2.0 / 32 : origin.x + origin.w + 2.0 / 32;
            Move(p, false, outside - p.rect.x, true);
        }
        Move(p, false, -2.0 / 32, false);
        s.pendingEcho = PendingEcho{origin, facing, 0}; s.bounce = BounceState{contact, 0};
    } else CreateEcho(origin, facing);
    Probe(p);
    Event(bounced ? EventKind::Bounce : EventKind::Blink, "player", {}, contact);
    return true;
}
void Simulation::DetachHead() {
    if (s.echo && s.echo->supportId == "player") { s.echo->grounded = false; s.echo->supportId.clear(); }
}
void Simulation::Die(const std::string& reason) {
    if (s.player.dead) return;
    s.bounce.reset(); ++s.deaths; s.deathRemaining = .65;
    auto& p = s.player;
    p.dead = true; p.vx = p.vy = p.walkVx = p.reboundTime = 0; p.reboundDirection = 0;
    p.grounded = false; p.supportId.clear(); DetachHead(); s.blinkBuffer = 0; s.echoOverlapGrace = false;
    Event(EventKind::PlayerDied, "player", reason, p.rect);
    Switches(); Beams();
}
void Simulation::Respawn() {
    // 保留中の残影も世界の一部。死亡からの復帰では残影と装置を初期化しない。
    s.dash.reset(); s.bounce.reset(); s.deathRemaining = 0; s.blinkBuffer = 0;
    s.pendingActionPressed = s.pendingActionReleased = false;
    DetachHead(); s.player = MakeBody(s.checkpointId.empty() ? d.spawn : s.checkpointPosition);
    s.echoOverlapGrace = s.echo && Overlap(s.player.rect, s.echo->rect);
    Probe(s.player); Switches(); Beams();
    Event(EventKind::PlayerRespawned, "player", s.checkpointId, s.player.rect);
}
void Simulation::RemoveFallenEcho() {
    if (s.echo && !InWorld(d, s.echo->rect)) {
        Event(EventKind::EchoRemoved, "echo", "out-of-world", s.echo->rect);
        s.echo.reset(); s.echoOverlapGrace = false;
    }
}
// 表示用：残影の頭の上（0.3 マス）が塞がっているか（本体は数えない）。
void Simulation::UpdateEchoCramped() {
    s.echoCramped = s.echo && !Clear(Rect{s.echo->rect.x, s.echo->rect.y - .3, s.echo->rect.w, .3}, &*s.echo, true, {}, true);
}
}
