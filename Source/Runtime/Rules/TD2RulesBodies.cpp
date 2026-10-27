// 体の当たりと動き：地形・装置の当たりの集め方、押し出し、重力、積み重なり、刺の上の残影。
#include "TD2RulesSimulation.h"

namespace TD2::Rules::Detail {
bool Simulation::IsSolid(size_t i) const {
    const auto kind = d.devices[i].kind;
    return kind == DeviceKind::Solid || kind == DeviceKind::OneWay || kind == DeviceKind::Spring || kind == DeviceKind::Pusher ||
           ((kind == DeviceKind::Door || kind == DeviceKind::LockWall || kind == DeviceKind::Bridge) && !s.devices[i].active);
}
std::vector<Collider> Simulation::Colliders(const Rect& area, const BodyState* body,
        bool echoBody, const std::string& ignore, bool noBodies,
        bool fullOnly, bool staticOnly) const {
    std::vector<Collider> result;
    const auto terrain = [&](const RegionDefinition& region, int originX, int originY) {
        const int minX = int(std::clamp(std::floor(area.x - originX) - 1.0, 0.0, double(region.cols)));
        const int maxX = int(std::clamp(std::floor(area.x + area.w - originX) + 1.0, -1.0, double(region.cols - 1)));
        const int minY = int(std::clamp(std::floor(area.y - originY) - 1.0, 0.0, double(region.rows)));
        const int maxY = int(std::clamp(std::floor(area.y + area.h - originY) + 1.0, -1.0, double(region.rows - 1)));
        for (int y = minY; y <= maxY; ++y) for (int x = minX; x <= maxX; ++x) {
            TileKind tile;
            if (TryGetTile(region, x, y, tile) && tile == TileKind::Solid)
                result.push_back({{double(x) + originX, double(y) + originY, 1, 1},
                    "tile:" + std::to_string(int64_t(x) + originX) + ":" + std::to_string(int64_t(y) + originY)});
        }
    };
    for (const auto& chunk : d.chunks) terrain(chunk.region, chunk.x, chunk.y);
    for (size_t i = 0; i < d.boundaries.size(); ++i)
        if (Overlap(area, d.boundaries[i])) result.push_back({d.boundaries[i], "bound:world:" + std::to_string(i)});
    for (size_t i = 0; i < d.devices.size(); ++i) {
        const auto& dev = d.devices[i];
        const bool spike = dev.kind == DeviceKind::Spikes;
        const bool hanging = dev.direction == Direction::Down;
        const bool oneWay = dev.kind == DeviceKind::OneWay && !hanging, ceiling = dev.kind == DeviceKind::OneWay && hanging;
        if (dev.id == ignore || ((oneWay || ceiling) && fullOnly)) continue;
        if (staticOnly && dev.kind != DeviceKind::Solid && dev.kind != DeviceKind::OneWay) continue;
        // バネは上面の踏み板だけが当たる（光線もここで止まる）。
        const Rect rect = dev.kind == DeviceKind::Spring ? SpringCap(dev) : spike ? SpikeHitbox(dev.rect, dev.direction)
                        : dev.kind == DeviceKind::Pusher ? PusherRect(dev, s.devices[i].push) : dev.rect;
        if ((IsSolid(i) || (spike && echoBody)) && Overlap(area, rect))
            result.push_back({rect, dev.id, oneWay, spike, dev.kind == DeviceKind::Spring && !hanging, dev.direction, ceiling});
    }
    // 残影はプレイヤーにとって上からだけ乗れる台（横や下からは通り抜け、道を塞がない）。
    // 残影にとってプレイヤーは塊：落ちてきた残影は頭の上に載る。互いに載っている相手は外す（積み重ねの中で自分に当たらない）。
    if (!staticOnly && !noBodies && body && !s.echoOverlapGrace) {
        const BodyState* other = echoBody ? &s.player : (s.echo ? &*s.echo : nullptr);
        const std::string otherId = echoBody ? "player" : "echo";
        const bool oneWayBody = !echoBody;
        if (other && !other->dead && otherId != ignore && !(oneWayBody && fullOnly) &&
            !Rider(other, *body, echoBody ? "echo" : "player"))
            result.push_back({other->rect, otherId, oneWayBody});
    }
    return result;
}
bool Simulation::Clear(const Rect& rect, const BodyState* body, bool echoBody,
           const std::string& ignore, bool noBodies, bool staticOnly) const {
    const auto colliders = Colliders(rect, body, echoBody, ignore, noBodies, true, staticOnly);
    return std::none_of(colliders.begin(), colliders.end(), [&](const Collider& c) { return Overlap(rect, c.rect); });
}
void Simulation::Probe(BodyState& b, bool echoBody) const {
    b.grounded = false; b.supportId.clear();
    const Rect foot{b.rect.x, b.rect.y + b.rect.h - 0.01 / 32, b.rect.w, 0.6 / 32};
    for (const auto& c : Colliders(foot, &b, echoBody)) {
        if (!c.ceiling && OverlapX(b.rect, c.rect) && std::abs(b.rect.y + b.rect.h - c.rect.y) < kGroundTolerance && b.vy >= -kEps) {
            b.grounded = true; b.supportId = c.id; b.vy = 0; return;
        }
    }
}
BodyState* Simulation::RiderFor(const BodyState& b, bool echoBody) {
    BodyState* other = echoBody ? &s.player : (s.echo ? &*s.echo : nullptr);
    return Rider(other, b, echoBody ? "echo" : "player") ? other : nullptr;
}
void Simulation::ValidateSupports() {
    if (!s.echo) return;
    if (s.echo->supportId == "player" && !Rider(&*s.echo, s.player, "player")) {
        s.echo->grounded = false; s.echo->supportId.clear();
    }
    if (s.player.supportId == "echo" && !Rider(&s.player, *s.echo, "echo")) {
        s.player.grounded = false; s.player.supportId.clear();
    }
}
// 上向きの棘だけが残影を先端へ押し上げる。横・下向きの棘は段差にしない。
std::optional<double> Simulation::SpikeTop(const BodyState& b, double x, const Collider& spike, const std::string& ignore) {
    double y = spike.rect.y - b.rect.h;
    for (size_t pass = 0; pass <= d.devices.size(); ++pass) {
        const Rect raised{x, y, b.rect.w, b.rect.h};
        double nextY = y;
        for (const auto& c : Colliders(raised, &b, true, ignore, false, true))
            if (c.spike && c.direction == Direction::Up && Overlap(raised, c.rect)) nextY = std::min(nextY, c.rect.y - b.rect.h);
        if (nextY == y) break;
        y = nextY;
    }
    const Rect raised{x, y, b.rect.w, b.rect.h};
    const Rect swept{x, y, b.rect.w, b.rect.y + b.rect.h - y};
    if (!Clear(raised, &b, true, ignore)) return {};
    for (const auto& c : Colliders(swept, &b, true, ignore, false, true))
        if ((!c.spike || c.direction != Direction::Up) && Overlap(swept, c.rect)) return {};
    if (auto* rider = RiderFor(b, true)) {
        BodyState test = *rider;
        Move(test, false, x - b.rect.x, true, "echo");
        Move(test, false, y - b.rect.y, false, "echo");
        if (std::abs(test.rect.x - rider->rect.x - (x - b.rect.x)) > kEps ||
            std::abs(test.rect.y - rider->rect.y - (y - b.rect.y)) > kEps) return {};
    }
    return y;
}
void Simulation::Move(BodyState& b, bool echoBody, double amount, bool horizontal, const std::string& ignore) {
    if (amount == 0) return;
    const Rect before = b.rect;
    Rect after = before;
    (horizontal ? after.x : after.y) += amount;
    Rect area = Sweep(before, after);
    area.x -= 0.01 / 32; area.y -= 0.01 / 32; area.w += 0.02 / 32; area.h += 0.02 / 32;
    double target = horizontal ? after.x : after.y;
    for (const auto& c : Colliders(area, &b, echoBody, ignore)) {
        const auto& r = c.rect;
        if (horizontal) {
            if (c.oneWay || c.ceiling || b.rect.y >= r.y + r.h - kEps || b.rect.y + b.rect.h <= r.y + kEps) continue;
            const bool hit = amount > 0 ? before.x + before.w <= r.x + kEps && target + b.rect.w > r.x
                                        : before.x >= r.x + r.w - kEps && target < r.x + r.w;
            if (!hit) continue;
            if (c.spike && c.direction == Direction::Up) {
                if (const auto y = SpikeTop(b, target, c, ignore)) {
                    b.rect.y = *y; b.vy = 0; b.grounded = true; b.supportId = c.id; continue;
                }
            }
            // 低いバネの踏み板は壁ではなく段差。立っている体は踏み板の上へ上がる。
            if (const double climb = b.rect.y + b.rect.h - r.y; c.spring && b.grounded && climb > 0 && climb <= kSpringStep) {
                const Rect raised{target, r.y - b.rect.h, b.rect.w, b.rect.h};
                if (Clear(raised, &b, echoBody, c.id)) { b.rect.y = raised.y; b.supportId = c.id; b.vy = 0; continue; }
            }
            target = amount > 0 ? std::min(target, r.x - b.rect.w) : std::max(target, r.x + r.w);
            b.vx = 0;
        } else {
            if (!OverlapX(b.rect, r)) continue;
            if (amount > 0 && !c.ceiling && before.y + before.h <= r.y + kEps && target + b.rect.h >= r.y) {
                target = std::min(target, r.y - b.rect.h); b.vy = 0; b.grounded = true; b.supportId = c.id;
            } else if (amount < 0 && !c.oneWay && before.y >= r.y + r.h - kEps && target < r.y + r.h) {
                if (c.spike && c.direction == Direction::Up) {
                    if (const auto y = SpikeTop(b, b.rect.x, c, ignore)) {
                        target = std::min(target, *y); b.vy = 0; b.grounded = true; b.supportId = c.id; continue;
                    }
                }
                target = std::max(target, r.y + r.h); b.vy = 0;
            }
        }
    }
    (horizontal ? b.rect.x : b.rect.y) = target;
}
void Simulation::Vertical(BodyState& b, bool echoBody) {
    const bool wasGrounded = b.grounded;
    b.grounded = false; b.supportId.clear();
    b.vy = std::min(kMaxFall, b.vy + kGravity * kStep);
    const double speed = b.vy;
    Move(b, echoBody, b.vy * kStep, false);
    RecordSpringImpact(b, wasGrounded, speed);
}
// 衝突解決後の着地だけを記録する。積み重なった体は一番下の支えのバネへ衝撃を渡す（2 段まで）。
void Simulation::RecordSpringImpact(const BodyState& b, bool wasGrounded, double speed) {
    if (wasGrounded || !b.grounded || speed < kSpringImpactMin) return;
    std::string support = b.supportId;
    for (int depth = 0; depth < 2; ++depth) {
        const BodyState* carrier = support == "player" ? &s.player
                                 : support == "echo" ? (s.echo ? &*s.echo : nullptr) : nullptr;
        if (!carrier || carrier->dead || !carrier->grounded) break;
        support = carrier->supportId;
    }
    for (size_t i = 0; i < d.devices.size(); ++i)
        if (d.devices[i].kind == DeviceKind::Spring && d.devices[i].id == support)
            s.devices[i].impact = std::max(s.devices[i].impact, std::min(speed, kMaxFall));
}
void Simulation::Integrate(BodyState& b, bool echoBody) {
    Move(b, echoBody, b.vx * kStep, true); Vertical(b, echoBody);
}
void Simulation::Stack(BodyState& carrier, bool carrierEcho, BodyState& rider) {
    const std::string carrierId = carrierEcho ? "echo" : "player";
    const bool carrierWasGrounded = carrier.grounded;
    const double oldX = carrier.rect.x, oldY = carrier.rect.y;
    Move(carrier, carrierEcho, carrier.vx * kStep, true);
    Move(rider, !carrierEcho, carrier.rect.x - oldX, true, carrierId);
    Move(rider, !carrierEcho, carrier.rect.y - oldY, false, carrierId);
    Move(rider, !carrierEcho, rider.vx * kStep, true, carrierId);
    if (!Rider(&rider, carrier, carrierId)) {
        if (rider.supportId == carrierId) { rider.supportId.clear(); rider.grounded = false; }
        Vertical(carrier, carrierEcho); Vertical(rider, !carrierEcho); return;
    }
    carrier.grounded = false; carrier.supportId.clear();
    carrier.vy = std::min(kMaxFall, carrier.vy + kGravity * kStep);
    BodyState carrierProbe = carrier, riderProbe = rider;
    Move(carrierProbe, carrierEcho, carrier.vy * kStep, false);
    const double carrierDelta = carrierProbe.rect.y - carrier.rect.y;
    Move(riderProbe, !carrierEcho, carrierDelta, false, carrierId);
    const double riderDelta = riderProbe.rect.y - rider.rect.y;
    // 頭上の天井で積み重なり全体を止める。下降時は上の体が足場に残れる。
    const double allowed = carrier.vy < 0 ? std::max(carrierDelta, riderDelta) : carrierDelta;
    Move(carrier, carrierEcho, allowed, false);
    Move(rider, !carrierEcho, carrier.vy < 0 ? allowed : riderDelta, false, carrierId);
    const double carrierSpeed = carrier.vy;
    carrier.vy = std::abs(allowed - carrierDelta) > kEps ? 0 : carrierProbe.vy;
    carrier.grounded = carrierProbe.grounded; carrier.supportId = carrierProbe.supportId;
    RecordSpringImpact(carrier, carrierWasGrounded, carrierSpeed);
    if (OverlapX(rider.rect, carrier.rect) && std::abs(rider.rect.y + rider.rect.h - carrier.rect.y) < 0.6 / 32) {
        rider.vy = 0; rider.grounded = true; rider.supportId = carrierId;
    } else {
        rider.vy = riderProbe.vy; rider.grounded = riderProbe.grounded; rider.supportId = riderProbe.supportId;
        if (rider.supportId == carrierId) { rider.supportId.clear(); rider.grounded = false; }
    }
}
void Simulation::AlignHead() {
    if (!s.echo || !Rider(&*s.echo, s.player, "player")) return;
    Rect target = s.echo->rect;
    target.x = s.player.rect.x + (s.player.rect.w - target.w) / 2;
    if (Clear(Sweep(s.echo->rect, target), &*s.echo, true, "player", true)) s.echo->rect.x = target.x;
}
void Simulation::ResolveEchoSpikes() {
    if (!s.echo) return;
    auto& e = *s.echo;
    auto spikes = Colliders(e.rect, &e, true, {}, false, true);
    std::erase_if(spikes, [&](const Collider& c) { return !c.spike || !Overlap(e.rect, c.rect); });
    if (spikes.empty()) return;
    const Collider* up = nullptr;
    for (const auto& c : spikes) if (c.direction == Direction::Up && (!up || c.rect.y < up->rect.y)) up = &c;
    if (up) {
        if (const auto y = SpikeTop(e, e.rect.x, *up, {})) {
            if (auto* rider = RiderFor(e, true)) Move(*rider, false, *y - e.rect.y, false, "echo");
            e.rect.y = *y; e.vy = 0; Probe(e, true); ValidateSupports();
        }
        return;
    }
    for (const auto& spike : spikes) {
        const bool horizontal = spike.direction != Direction::Down;
        const bool positive = spike.direction != Direction::Left;
        Rect target = e.rect;
        for (size_t pass = 0; pass <= d.devices.size(); ++pass) {
            bool hit = false;
            for (const auto& c : Colliders(target, &e, true, {}, false, true)) if (c.spike && Overlap(target, c.rect)) {
                hit = true;
                double& axis = horizontal ? target.x : target.y;
                const double near = horizontal ? c.rect.x : c.rect.y;
                const double size = horizontal ? c.rect.w : c.rect.h;
                axis = positive ? std::max(axis, near + size) : std::min(axis, near - (horizontal ? e.rect.w : e.rect.h));
            }
            if (!hit) break;
        }
        if (!Clear(target, &e, true)) continue;
        const Rect swept = Sweep(e.rect, target);
        bool blocked = false;
        for (const auto& c : Colliders(swept, &e, true, {}, false, true)) if (!c.spike && Overlap(swept, c.rect)) blocked = true;
        if (blocked) continue;
        const double delta = horizontal ? target.x - e.rect.x : target.y - e.rect.y;
        if (auto* rider = RiderFor(e, true)) {
            BodyState probe = *rider; Move(probe, false, delta, horizontal, "echo");
            if (std::abs((horizontal ? probe.rect.x - rider->rect.x : probe.rect.y - rider->rect.y) - delta) > kEps) continue;
            Move(*rider, false, delta, horizontal, "echo");
        }
        e.rect = target; (horizontal ? e.vx : e.vy) = 0; Probe(e, true); ValidateSupports(); return;
    }
}
}
