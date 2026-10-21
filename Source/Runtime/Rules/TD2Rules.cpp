#include "TD2Rules.h"
#include "../../Common/TD2MachineText.h"
#include "../../Common/TD2Math.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace TD2::Rules {
namespace {
constexpr double kEps = 0.001 / 32.0;
constexpr double kStep = 1.0 / 120.0;
constexpr double kPlayerWidth = 0.625, kPlayerHeight = 0.875;
constexpr double kGravity = 43.75, kMaxFall = 28.125;
constexpr double kSwapHold = .3;
constexpr double kGroundTolerance = 0.55 / 32.0;
// バネ：踏み板の厚さ、記録する最小の着地速度、打ち上げ速度と冷却の範囲（マス単位）。
constexpr double kSpringCap = 10.0 / 32.0, kSpringStep = 10.1 / 32.0, kSpringImpactMin = 60.0 / 32.0;
constexpr double kSpringPowerMin = 100.0 / 32.0, kSpringPowerMax = 1400.0 / 32.0;
Rect SpringCap(const Rect& r) { return {r.x, r.y + r.h - kSpringCap, r.w, kSpringCap}; }
// 棘の当たりはマスより小さい：根元側を残し、先端側 8/32、横 3/32 ずつ内側。
Rect SpikeHitbox(const Rect& r, Direction direction) {
    constexpr double tip = 8.0 / 32.0, side = 3.0 / 32.0;
    switch (direction) {
    case Direction::Up: return {r.x + side, r.y + tip, r.w - 2 * side, r.h - tip};
    case Direction::Down: return {r.x + side, r.y, r.w - 2 * side, r.h - tip};
    case Direction::Left: return {r.x + tip, r.y + side, r.w - tip, r.h - 2 * side};
    default: return {r.x, r.y + side, r.w - tip, r.h - 2 * side};
    }
}

bool OverlapX(const Rect& a, const Rect& b) {
    return a.x < b.x + b.w - kEps && a.x + a.w > b.x + kEps;
}
bool Overlap(const Rect& a, const Rect& b) {
    return OverlapX(a, b) && a.y < b.y + b.h - kEps && a.y + a.h > b.y + kEps;
}
Rect Sweep(const Rect& a, const Rect& b) {
    return {std::min(a.x, b.x), std::min(a.y, b.y),
            a.w + std::abs(b.x - a.x), a.h + std::abs(b.y - a.y)};
}
bool FiniteRect(const Rect& r) {
    return std::isfinite(r.x) && std::isfinite(r.y) && std::isfinite(r.w) &&
           std::isfinite(r.h) && r.w > 0 && r.h > 0;
}
bool WorldTile(const WorldDefinition& d, int64_t x, int64_t y, TileKind& tile) {
    if (d.chunks.empty()) {
        if (x < 0 || y < 0 || x >= d.region.cols || y >= d.region.rows) return false;
        return TryGetTile(d.region, int(x), int(y), tile);
    }
    for (const auto& chunk : d.chunks) {
        const auto localX = x - chunk.x, localY = y - chunk.y;
        if (localX >= 0 && localY >= 0 && localX < chunk.region.cols && localY < chunk.region.rows)
            return TryGetTile(chunk.region, int(localX), int(localY), tile);
    }
    return false;
}
bool InWorld(const WorldDefinition& d, const Rect& r) {
    // 占有するマスがすべてあれば、テレビをまたぐ体も世界の中にある。
    for (int64_t y = int64_t(std::floor(r.y + kEps)); y <= int64_t(std::floor(r.y + r.h - kEps)); ++y)
        for (int64_t x = int64_t(std::floor(r.x + kEps)); x <= int64_t(std::floor(r.x + r.w - kEps)); ++x) {
            TileKind tile;
            if (!WorldTile(d, x, y, tile)) return false;
        }
    return true;
}
BodyState MakeBody(Vec2 at, bool echo = false) {
    BodyState b;
    b.rect = {at.x, at.y, kPlayerWidth, echo ? 1.0 : kPlayerHeight};
    return b;
}
struct Collider {
    Rect rect;
    std::string id;
    bool oneWay = false, spike = false, spring = false;
    Direction direction = Direction::Up;
};
bool Rider(const BodyState* rider, const BodyState& carrier, const std::string& carrierId) {
    return rider && !rider->dead && !carrier.dead && rider->supportId == carrierId &&
           OverlapX(rider->rect, carrier.rect) &&
           std::abs(rider->rect.y + rider->rect.h - carrier.rect.y) < 0.6 / 32.0 && rider->vy >= -kEps;
}

// 判定はすべて同じ WorldState を読む。
class Simulation {
public:
    const WorldDefinition& d;
    WorldState& s;
    StepOutput& out;
    Simulation(const WorldDefinition& definition, WorldState& state, StepOutput& output)
        : d(definition), s(state), out(output) {}

    void Event(EventKind kind, std::string id, std::string detail, const Rect& r) {
        out.events.push_back({kind, std::move(id), std::move(detail), {r.x, r.y}, s.simulationTime});
    }
    bool IsSolid(size_t i) const {
        const auto kind = d.devices[i].kind;
        return kind == DeviceKind::Solid || kind == DeviceKind::OneWay || kind == DeviceKind::Spring ||
               ((kind == DeviceKind::Door || kind == DeviceKind::LockWall || kind == DeviceKind::Bridge) && !s.devices[i].active);
    }
    std::vector<Collider> Colliders(const Rect& area, const BodyState* body = nullptr,
            bool echoBody = false, const std::string& ignore = {}, bool noBodies = false,
            bool fullOnly = false, bool staticOnly = false) const {
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
            const bool oneWay = dev.kind == DeviceKind::OneWay;
            if (dev.id == ignore || (oneWay && fullOnly)) continue;
            if (staticOnly && dev.kind != DeviceKind::Solid && !oneWay) continue;
            // バネは上面の踏み板だけが当たる（光線もここで止まる）。
            const Rect rect = dev.kind == DeviceKind::Spring ? SpringCap(dev.rect) : spike ? SpikeHitbox(dev.rect, dev.direction) : dev.rect;
            if ((IsSolid(i) || (spike && echoBody)) && Overlap(area, rect))
                result.push_back({rect, dev.id, oneWay, spike, dev.kind == DeviceKind::Spring, dev.direction});
        }
        // 残影とプレイヤーは互いに当たらない（自分の残影は通り抜ける。乗ることも押すこともない）。
        (void)body; (void)noBodies;
        return result;
    }
    bool Clear(const Rect& rect, const BodyState* body = nullptr, bool echoBody = false,
               const std::string& ignore = {}, bool noBodies = false, bool staticOnly = false) const {
        const auto colliders = Colliders(rect, body, echoBody, ignore, noBodies, true, staticOnly);
        return std::none_of(colliders.begin(), colliders.end(), [&](const Collider& c) { return Overlap(rect, c.rect); });
    }
    void Probe(BodyState& b, bool echoBody = false) const {
        b.grounded = false; b.supportId.clear();
        const Rect foot{b.rect.x, b.rect.y + b.rect.h - 0.01 / 32, b.rect.w, 0.6 / 32};
        for (const auto& c : Colliders(foot, &b, echoBody)) {
            if (OverlapX(b.rect, c.rect) && std::abs(b.rect.y + b.rect.h - c.rect.y) < kGroundTolerance && b.vy >= -kEps) {
                b.grounded = true; b.supportId = c.id; b.vy = 0; return;
            }
        }
    }
    BodyState* RiderFor(const BodyState& b, bool echoBody) {
        BodyState* other = echoBody ? &s.player : (s.echo ? &*s.echo : nullptr);
        return Rider(other, b, echoBody ? "echo" : "player") ? other : nullptr;
    }
    void ValidateSupports() {
        if (!s.echo) return;
        if (s.echo->supportId == "player" && !Rider(&*s.echo, s.player, "player")) {
            s.echo->grounded = false; s.echo->supportId.clear();
        }
        if (s.player.supportId == "echo" && !Rider(&s.player, *s.echo, "echo")) {
            s.player.grounded = false; s.player.supportId.clear();
        }
    }

    // 上向きの棘だけが残影を先端へ押し上げる。横・下向きの棘は段差にしない。
    std::optional<double> SpikeTop(const BodyState& b, double x, const Collider& spike, const std::string& ignore) {
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
    void Move(BodyState& b, bool echoBody, double amount, bool horizontal, const std::string& ignore = {}) {
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
                if (c.oneWay || b.rect.y >= r.y + r.h - kEps || b.rect.y + b.rect.h <= r.y + kEps) continue;
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
                if (amount > 0 && before.y + before.h <= r.y + kEps && target + b.rect.h >= r.y) {
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
    void Vertical(BodyState& b, bool echoBody) {
        const bool wasGrounded = b.grounded;
        b.grounded = false; b.supportId.clear();
        b.vy = std::min(kMaxFall, b.vy + kGravity * kStep);
        const double speed = b.vy;
        Move(b, echoBody, b.vy * kStep, false);
        RecordSpringImpact(b, wasGrounded, speed);
    }
    // 衝突解決後の着地だけを記録する。積み重なった体は一番下の支えのバネへ衝撃を渡す（2 段まで）。
    void RecordSpringImpact(const BodyState& b, bool wasGrounded, double speed) {
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
    void Integrate(BodyState& b, bool echoBody) {
        Move(b, echoBody, b.vx * kStep, true); Vertical(b, echoBody);
    }
    void Stack(BodyState& carrier, bool carrierEcho, BodyState& rider) {
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
    void AlignHead() {
        if (!s.echo || !Rider(&*s.echo, s.player, "player")) return;
        Rect target = s.echo->rect;
        target.x = s.player.rect.x + (s.player.rect.w - target.w) / 2;
        if (Clear(Sweep(s.echo->rect, target), &*s.echo, true, "player", true)) s.echo->rect.x = target.x;
    }
    void ResolveEchoSpikes() {
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
    Rect EchoOrigin(const Rect& source) const {
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
    void CreateEcho(const Rect& origin, int facing) {
        s.echo = MakeBody({origin.x, origin.y}, true); s.echo->facing = facing; ++s.echoes;
        s.echoOverlapGrace = !s.player.dead && Overlap(s.player.rect, s.echo->rect);
        ResolveEchoSpikes(); Probe(*s.echo, true);
        Event(EventKind::EchoCreated, "echo", {}, s.echo->rect);
    }
    void Pending() {
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
    double HorizontalVelocity() const {
        return s.player.walkVx + s.player.reboundDirection * 7.5 * std::clamp(s.player.reboundTime / .14, 0.0, 1.0);
    }
    // ダッシュが決まった時に前の残影を引き取る（新しい残影はダッシュの出発点に出る）。
    void RetireEcho() {
        if (!s.echo && !s.pendingEcho) return;
        const Rect source = s.echo ? s.echo->rect : s.pendingEcho->origin;
        s.echo.reset(); s.pendingEcho.reset(); s.echoOverlapGrace = false;
        ++s.recalls;
        Event(EventKind::EchoRecalled, "echo", {}, source);
    }
    // 残影をその場で消す（大区域を出た時。回収の演出は出さない）。
    void ReplaceEcho() {
        const Rect source = s.echo ? s.echo->rect : s.pendingEcho->origin;
        s.echo.reset(); s.pendingEcho.reset(); s.echoOverlapGrace = false;
        if (s.player.supportId == "echo") { s.player.supportId.clear(); s.player.grounded = false; }
        Event(EventKind::EchoRemoved, "echo", "replaced", source);
    }
    // 交換：本体と残影の足元を入れ替える（大きさはそれぞれのまま。残影は本体より少し背が高い）。勢いは捨てる。
    // 入れ替えた先が塞がっている時は交換しない。
    void Swap() {
        if (!s.echo || s.player.dead) return;
        auto& p = s.player; auto& e = *s.echo;
        const Rect from = p.rect;
        const Rect playerTo{e.rect.x + (e.rect.w - p.rect.w) / 2, e.rect.y + e.rect.h - p.rect.h, p.rect.w, p.rect.h};
        const Rect echoTo{p.rect.x + (p.rect.w - e.rect.w) / 2, p.rect.y + p.rect.h - e.rect.h, e.rect.w, e.rect.h};
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
    bool ConsumeBlink() {
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

    std::string Switches() {
        std::unordered_set<std::string> linked, powered;
        for (size_t i = 0; i < d.devices.size(); ++i) {
            const auto& dev = d.devices[i]; auto& state = s.devices[i];
            if (dev.kind != DeviceKind::Plate) continue;
            const auto presses = [&](const BodyState& b) {
                const double feet = b.rect.y + b.rect.h, bottom = dev.rect.y + dev.rect.h;
                return !b.dead && OverlapX(b.rect, dev.rect) && feet >= bottom - 7.0 / 32 &&
                       feet <= bottom + 2.0 / 32 && b.rect.y < bottom && b.vy >= -kEps;
            };
            // 壁蹴りのダッシュで残影が出るまでの間も、出発点の板は押されたままにする（離して押し直すと二回分の入力になる）。
            const auto pending = [&] {
                if (!s.pendingEcho) return false;
                const Rect& r = s.pendingEcho->origin;
                const double feet = r.y + r.h, bottom = dev.rect.y + dev.rect.h;
                return OverlapX(r, dev.rect) && feet >= bottom - 7.0 / 32 && feet <= bottom + 2.0 / 32 && r.y < bottom;
            };
            const bool pressed = presses(s.player) || (s.echo && presses(*s.echo)) || pending();
            if (state.pressed != pressed) Event(EventKind::PlateChanged, dev.id, pressed ? "pressed" : "released", dev.rect);
            // 切り替え式は踏み込んだ瞬間だけ入・切を反転し、離しても保つ。
            if (!dev.latch) state.active = pressed;
            else if (pressed && !state.pressed) {
                state.active = !state.active;
                Event(EventKind::PlateChanged, dev.id, state.active ? "latched" : "unlatched", dev.rect);
            }
            state.pressed = pressed;
            for (const auto& target : dev.targets) { linked.insert(target); if (state.active) powered.insert(target); }
        }
        std::string death;
        for (size_t i = 0; i < d.devices.size(); ++i) {
            const auto& dev = d.devices[i]; auto& state = s.devices[i];
            if (dev.kind != DeviceKind::Door && dev.kind != DeviceKind::Laser && dev.kind != DeviceKind::Bridge &&
                dev.kind != DeviceKind::Mechanism) continue;
            const bool active = linked.contains(dev.id) ? (dev.inverted ? !powered.contains(dev.id) : powered.contains(dev.id)) : dev.kind == DeviceKind::Laser;
            // 門と伸縮橋は実体に戻る瞬間に重なる体を挟む。
            const bool closing = state.active && !active && (dev.kind == DeviceKind::Door || dev.kind == DeviceKind::Bridge);
            if (state.active != active) Event(EventKind::DeviceChanged, dev.id, active ? "active" : "inactive", dev.rect);
            state.active = active;
            if (closing) {
                if (s.echo && Overlap(s.echo->rect, dev.rect)) {
                    Event(EventKind::EchoRemoved, "echo", "door-crush", s.echo->rect); s.echo.reset(); s.echoOverlapGrace = false;
                }
                if (!s.player.dead && Overlap(s.player.rect, dev.rect)) death = "door-crush";
            }
        }
        return death;
    }
    void Beams() {
        s.beams.clear();
        for (size_t i = 0; i < d.devices.size(); ++i) {
            const auto& dev = d.devices[i];
            if (dev.kind != DeviceKind::Laser || !s.devices[i].active) continue;
            const int dx = dev.direction == Direction::Right ? 1 : dev.direction == Direction::Left ? -1 : 0;
            const int dy = dev.direction == Direction::Down ? 1 : dev.direction == Direction::Up ? -1 : 0;
            const double x = dev.rect.x + dev.rect.w / 2, y = dev.rect.y + dev.rect.h / 2;
            double length = dev.range;
            const Rect area{std::min(x, x + dx * length) - 2.0 / 32, std::min(y, y + dy * length) - 2.0 / 32,
                            std::abs(dx * length) + 4.0 / 32, std::abs(dy * length) + 4.0 / 32};
            auto blockers = Colliders(area, nullptr, false, {}, true, true);
            if (s.echo) blockers.push_back({s.echo->rect, "echo"});
            for (const auto& c : blockers) {
                const auto& r = c.rect;
                double distance;
                if (dx) {
                    if (y + 2.0 / 32 <= r.y + kEps || y - 2.0 / 32 >= r.y + r.h - kEps) continue;
                    if (dx > 0) { if (r.x + r.w <= x + kEps) continue; distance = std::max(0.0, r.x - x); }
                    else { if (r.x >= x - kEps) continue; distance = std::max(0.0, x - r.x - r.w); }
                } else {
                    if (x + 2.0 / 32 <= r.x + kEps || x - 2.0 / 32 >= r.x + r.w - kEps) continue;
                    if (dy > 0) { if (r.y + r.h <= y + kEps) continue; distance = std::max(0.0, r.y - y); }
                    else { if (r.y >= y - kEps) continue; distance = std::max(0.0, y - r.y - r.h); }
                }
                length = std::min(length, distance);
            }
            if (length <= kEps) continue;
            const Rect beam = dx ? Rect{std::min(x, x + dx * length), y - 2.0 / 32, length, 4.0 / 32}
                                 : Rect{x - 2.0 / 32, std::min(y, y + dy * length), 4.0 / 32, length};
            s.beams.push_back({dev.id, beam});
        }
    }
    bool SpikeAt(const Rect& rect) const {
        return std::any_of(d.devices.begin(), d.devices.end(), [&](const DeviceDefinition& dev) {
            return dev.kind == DeviceKind::Spikes && Overlap(rect, SpikeHitbox(dev.rect, dev.direction));
        });
    }
    std::string Hazard() const {
        if (SpikeAt(s.player.rect)) return "spikes";
        for (const auto& beam : s.beams) if (Overlap(s.player.rect, beam.rect)) return "laser";
        if (!InWorld(d, s.player.rect)) return "out-of-world";
        return {};
    }
    std::optional<Vec2> CheckpointAnchor(const DeviceDefinition& checkpoint) const {
        const Rect& r = checkpoint.rect;
        const Vec2 ideal{r.x + (r.w - kPlayerWidth) / 2, r.y + r.h - kPlayerHeight};
        int64_t left = int64_t(std::floor(r.x)) - 2, right = int64_t(std::floor(r.x + r.w - 1.0 / 32)) + 2;
        int64_t top = int64_t(std::floor(r.y)) - 1, bottom = int64_t(std::floor(r.y + r.h)) + 3;
        std::optional<Vec2> best;
        double score = std::numeric_limits<double>::max();
        for (int64_t y = top; y <= bottom; ++y) for (int64_t x = left; x <= right; ++x) {
            TileKind tile;
            if (!WorldTile(d, x, y, tile) || tile != TileKind::Solid) continue;
            const Rect rect{std::clamp(ideal.x, double(x), double(x) + 1.0 - kPlayerWidth), double(y) - kPlayerHeight, kPlayerWidth, kPlayerHeight};
            if (!InWorld(d, rect) || !Clear(rect, nullptr, false, {}, true, true) || SpikeAt(rect)) continue;
            const bool blocked = std::any_of(d.devices.begin(), d.devices.end(), [&](const DeviceDefinition& other) {
                return (other.kind == DeviceKind::Door || other.kind == DeviceKind::LockWall || other.kind == DeviceKind::Bridge) &&
                       Overlap(rect, other.rect);
            });
            if (blocked) continue;
            const double candidate = std::hypot(rect.x - ideal.x, rect.y - ideal.y);
            if (candidate < score) { score = candidate; best = Vec2{rect.x, rect.y}; }
        }
        return best;
    }
    // バネ：冷却中は衝撃も溜めも捨てる。準備ができていれば、落下の衝撃ですぐに、立っていれば溜め切って打ち上げる。
    void Springs() {
        for (size_t i = 0; i < d.devices.size(); ++i) {
            const auto& dev = d.devices[i]; auto& state = s.devices[i];
            if (dev.kind != DeviceKind::Spring) continue;
            state.cooldown = std::max(0.0, state.cooldown - kStep);
            std::vector<BodyState*> occupants;
            if (!s.player.dead && s.player.grounded && s.player.supportId == dev.id) occupants.push_back(&s.player);
            if (s.echo && !s.echo->dead && s.echo->grounded && s.echo->supportId == dev.id) occupants.push_back(&*s.echo);
            const double impact = state.impact;
            state.impact = 0;
            if (state.cooldown > kEps) { state.charge = 0; continue; }
            state.cooldown = 0;
            if (!occupants.empty() && impact > 0) { Launch(i, occupants, impact); continue; }
            if (occupants.empty()) { state.charge = 0; continue; }
            state.charge += kStep;
            if (state.charge >= std::max(0.05, dev.delay)) Launch(i, occupants, 0);
        }
    }
    // 打ち上げ速度は基礎の強さと着地の衝撃の合成。上に乗っている体も一緒に運ぶ。
    void Launch(size_t i, const std::vector<BodyState*>& occupants, double impact) {
        const auto& dev = d.devices[i]; auto& state = s.devices[i];
        const double power = std::hypot(std::clamp(dev.power, kSpringPowerMin, kSpringPowerMax), impact);
        for (auto* b : occupants) {
            const bool echoBody = s.echo && b == &*s.echo;
            if (auto* rider = RiderFor(*b, echoBody)) { rider->vy = 0; rider->grounded = true; rider->supportId = echoBody ? "echo" : "player"; }
            b->vy = -power; b->grounded = false; b->supportId.clear();
        }
        state.charge = 0; state.cooldown = std::clamp(dev.cooldown, 3.0, 30.0);
        Event(EventKind::SpringLaunched, dev.id, impact > 0 ? "impact" : "charged", dev.rect);
    }
    void Sensors() {
        for (size_t i = 0; i < d.devices.size(); ++i) {
            const auto& dev = d.devices[i]; auto& state = s.devices[i];
            // 収集物は本体が触れた時だけ拾う。死亡やチェックポイントからの復帰では戻さない。
            if (dev.kind == DeviceKind::Collectible) {
                if (!state.active && !s.player.dead && Overlap(s.player.rect, dev.rect)) {
                    state.active = true; Event(EventKind::CollectibleTaken, dev.id, {}, dev.rect);
                }
                continue;
            }
            if (dev.kind != DeviceKind::Checkpoint) continue;
            const bool playerContact = !s.player.dead && Overlap(s.player.rect, dev.rect);
            const bool echoContact = s.echo && Overlap(s.echo->rect, dev.rect);
            const bool playerEdge = playerContact && !state.playerContact;
            const bool echoEdge = echoContact && !state.echoContact;
            state.playerContact = playerContact; state.echoContact = echoContact;
            // 同じ ID でも再接触は通知する（最新のセーブが別の領域へ移っていることがある）。
            if (!playerEdge && !echoEdge) continue;
            if (const auto anchor = CheckpointAnchor(dev)) {
                s.checkpointId = dev.id; s.checkpointPosition = *anchor;
                for (size_t j = 0; j < d.devices.size(); ++j)
                    if (d.devices[j].kind == DeviceKind::Checkpoint) s.devices[j].active = j == i;
                Event(EventKind::CheckpointActivated, dev.id, playerEdge ? "player" : "echo", dev.rect);
            }
        }
    }
    void DetachHead() {
        if (s.echo && s.echo->supportId == "player") { s.echo->grounded = false; s.echo->supportId.clear(); }
    }
    void Die(const std::string& reason) {
        if (s.player.dead) return;
        s.bounce.reset(); ++s.deaths; s.deathRemaining = .65;
        auto& p = s.player;
        p.dead = true; p.vx = p.vy = p.walkVx = p.reboundTime = 0; p.reboundDirection = 0;
        p.grounded = false; p.supportId.clear(); DetachHead(); s.blinkBuffer = 0; s.echoOverlapGrace = false;
        Event(EventKind::PlayerDied, "player", reason, p.rect);
        Switches(); Beams();
    }
    void Respawn() {
        // 保留中の残影も世界の一部。死亡からの復帰では残影と装置を初期化しない。
        s.dash.reset(); s.bounce.reset(); s.deathRemaining = 0; s.blinkBuffer = 0;
        s.pendingActionPressed = s.pendingActionReleased = false;
        DetachHead(); s.player = MakeBody(s.checkpointId.empty() ? d.spawn : s.checkpointPosition);
        s.echoOverlapGrace = s.echo && Overlap(s.player.rect, s.echo->rect);
        Probe(s.player); Switches(); Beams();
        Event(EventKind::PlayerRespawned, "player", s.checkpointId, s.player.rect);
    }
    void RemoveFallenEcho() {
        if (s.echo && !InWorld(d, s.echo->rect)) {
            Event(EventKind::EchoRemoved, "echo", "out-of-world", s.echo->rect);
            s.echo.reset(); s.echoOverlapGrace = false;
        }
    }
    void Tick(const InputState& input, bool actionPressed, bool actionReleased) {
        s.simulationTime += kStep; ++s.tick; ++out.simulationSteps;
        ResolveEchoSpikes();
        if (s.player.dead) {
            s.deathRemaining -= kStep; Pending(); Switches();
            if (s.echo) Integrate(*s.echo, true);
            RemoveFallenEcho(); Switches(); Beams(); Springs(); Sensors();
            if (s.deathRemaining <= kEps) Respawn();
            return;
        }
        // スペース：点按は常にダッシュ（前の残影は自動で引き取られ、出発点に新しい残影が出る）。
        // 残影がいる時だけ、kSwapHold 秒押し続けると交換（離すのを待たずに入れ替える）。点按か長押しかは離すまで分からないので、
        // 残影がいる時のダッシュは離した時に出る。
        const bool hasEcho = s.echo || s.pendingEcho;
        if (actionPressed && hasEcho) { s.actionArmed = true; s.actionHold = 0; }
        else if (actionPressed) s.blinkBuffer = .12;
        else if (s.blinkBuffer > 0) {
            s.blinkBuffer = std::max(0.0, s.blinkBuffer - kStep);
            if (s.blinkBuffer == 0) Event(EventKind::BlinkRejected, "player", "support-required", s.player.rect);
        }
        if (s.actionArmed) {
            if (!hasEcho) { s.actionArmed = false; s.blinkBuffer = .12; }
            else if (input.action.held && !actionReleased) {
                s.actionHold += kStep;
                if (s.actionHold + 1e-9 >= kSwapHold) { s.actionArmed = false; if (s.echo) Swap(); }
            } else { s.actionArmed = false; s.blinkBuffer = .12; }
        }
        Pending();
        auto& p = s.player;
        const int direction = int(input.right.held) - int(input.left.held);
        p.walkVx = direction * 5.0; p.vx = HorizontalVelocity();
        if (direction) p.facing = direction;
        ConsumeBlink();
        if (const auto reason = Switches(); !reason.empty()) { Die(reason); return; }
        if (s.echo && Rider(&*s.echo, p, "player")) Stack(p, false, *s.echo);
        else if (s.echo && Rider(&p, *s.echo, "echo")) Stack(*s.echo, true, p);
        else {
            if (s.echo) Integrate(*s.echo, true);
            if (s.echo && Rider(&*s.echo, p, "player")) Stack(p, false, *s.echo);
            else Integrate(p, false);
        }
        AlignHead(); p.reboundTime = std::max(0.0, p.reboundTime - kStep);
        if (s.echoOverlapGrace && (!s.echo || !Overlap(p.rect, s.echo->rect))) s.echoOverlapGrace = false;
        RemoveFallenEcho();
        if (const auto reason = Switches(); !reason.empty()) { Die(reason); return; }
        Beams();
        if (const auto reason = Hazard(); !reason.empty()) { Die(reason); return; }
        // 着地直前の入力は衝突解決後に一度だけ再評価する。
        if (ConsumeBlink()) {
            if (const auto reason = Switches(); !reason.empty()) { Die(reason); return; }
            Beams();
            if (const auto reason = Hazard(); !reason.empty()) { Die(reason); return; }
        }
        Springs(); Sensors(); ValidateSupports();
    }
};

void FinishOutput(const WorldState& s, StepOutput& out) {
    out.tick = s.tick; out.simulationTime = s.simulationTime; out.realTime = s.realTime;
}
}

std::string_view EventName(EventKind event) {
    using E = EventKind;
    switch (event) {
    case E::Blink: return "Blink"; case E::Bounce: return "Bounce"; case E::BlinkRejected: return "BlinkRejected";
    case E::EchoCreated: return "EchoCreated"; case E::EchoRecalled: return "EchoRecalled"; case E::EchoRemoved: return "EchoRemoved";
    case E::EchoSpawnBlocked: return "EchoSpawnBlocked"; case E::PlateChanged: return "PlateChanged";
    case E::DeviceChanged: return "DeviceChanged"; case E::CheckpointActivated: return "CheckpointActivated";
    case E::PlayerDied: return "PlayerDied"; case E::PlayerRespawned: return "PlayerRespawned";
    case E::SpringLaunched: return "SpringLaunched"; case E::CollectibleTaken: return "CollectibleTaken";
    case E::EchoSwapped: return "EchoSwapped";
    }
    return "Unknown";
}

std::string ValidateWorld(const WorldDefinition& d) {
    if (d.chunks.empty()) {
        if (const auto validation = ValidateRegion(d.region); !validation.valid) return std::string(validation.message);
    } else for (const auto& chunk : d.chunks) {
        if (const auto validation = ValidateRegion(chunk.region); !validation.valid) return std::string(validation.message);
    }
    if (!std::isfinite(d.spawn.x) || !std::isfinite(d.spawn.y) || std::abs(d.spawn.x) > 4294967296.0 ||
        std::abs(d.spawn.y) > 4294967296.0 || !InWorld(d, {d.spawn.x, d.spawn.y, kPlayerWidth, kPlayerHeight}))
        return "spawn must be a finite player origin inside the region";
    std::unordered_set<std::string> ids;
    for (const auto& dev : d.devices) {
        if (dev.id.empty() || dev.id == "player" || dev.id == "echo" || dev.id.starts_with("tile:") ||
            dev.id.starts_with("bound:") || !ids.insert(dev.id).second) return "device IDs must be unique and non-reserved";
        if (!FiniteRect(dev.rect) || std::abs(dev.rect.x) > 4294967296.0 || std::abs(dev.rect.y) > 4294967296.0 ||
            dev.rect.w > 100000 || dev.rect.h > 100000 || !std::isfinite(dev.range) || dev.range < 0 || dev.range > 100000)
            return "device rectangle/range is invalid";
    }
    for (const auto& dev : d.devices) for (const auto& target : dev.targets) {
        if (dev.kind != DeviceKind::Plate) return "only plates may have targets";
        const auto it = std::find_if(d.devices.begin(), d.devices.end(), [&](const DeviceDefinition& other) { return other.id == target; });
        // 「区域ID/装置ID」は別のテレビの装置。世界を組んだ後に検査する。
        if (it == d.devices.end() && d.chunks.empty() && target.find('/') != std::string::npos) continue;
        if (it == d.devices.end() || (it->kind != DeviceKind::Door && it->kind != DeviceKind::Laser && it->kind != DeviceKind::Bridge &&
                                      it->kind != DeviceKind::Mechanism))
            return "plate targets must reference an existing door, bridge, laser or mechanism";
    }
    return {};
}
WorldState CreateWorldState(const WorldDefinition& d) {
    if (const auto error = ValidateWorld(d); !error.empty()) throw std::invalid_argument(error);
    WorldState s; s.player = MakeBody(d.spawn); s.checkpointPosition = d.spawn; s.devices.resize(d.devices.size());
    StepOutput out; Simulation sim(d, s, out); sim.Probe(s.player); sim.Switches(); sim.Beams();
    return s;
}
StepOutput StepRules(const WorldDefinition& d, WorldState& s, const InputState& input, double realDeltaSeconds) {
    if (s.devices.size() != d.devices.size()) throw std::invalid_argument("world definition changed; restart exploration explicitly");
    StepOutput out; Simulation sim(d, s, out);
    s.pendingActionPressed = s.pendingActionPressed || input.action.pressed;
    s.pendingActionReleased = s.pendingActionReleased || input.action.released;
    double remaining = std::isfinite(realDeltaSeconds) ? std::clamp(realDeltaSeconds, 0.0, .25) : 0.0;
    // 演出の境界と物理 tick で実時間を分ける。60/120 Hz の違いで停止時間を二重に使わない。
    while (remaining > 1e-12) {
        double rate = 1.0, boundary = remaining;
        if (s.dash) { rate = 0; boundary = std::min(boundary, s.dash->duration - s.dash->age); }
        else if (s.bounce) {
            const double age = s.bounce->age;
            if (age < .08 - 1e-12) { rate = 0; boundary = std::min(boundary, .08 - age); }
            else if (age < .30 - 1e-12) { rate = .45; boundary = std::min(boundary, .30 - age); }
            else if (age < .46 - 1e-12) { rate = .8; boundary = std::min(boundary, .46 - age); }
            else boundary = std::min(boundary, .72 - age);
        }
        if (rate > 0) boundary = std::min(boundary, std::max(0.0, (kStep - s.accumulator) / rate));
        s.realTime += boundary; remaining -= boundary;
        if (s.dash) {
            s.dash->age += boundary;
            if (s.dash->age + 1e-12 >= s.dash->duration) s.dash.reset();
        } else if (s.bounce) {
            s.bounce->age += boundary;
            if (s.bounce->age + 1e-12 >= .72) s.bounce.reset();
        }
        s.accumulator += boundary * rate;
        if (s.accumulator + 1e-12 >= kStep) {
            s.accumulator = std::max(0.0, s.accumulator - kStep);
            const bool pressed = s.pendingActionPressed, released = s.pendingActionReleased;
            s.pendingActionPressed = s.pendingActionReleased = false;
            sim.Tick(input, pressed, released);
        }
    }
    FinishOutput(s, out); return out;
}
WorldSnapshot BuildSnapshot(const WorldDefinition& d, const WorldState& s) {
    WorldSnapshot result;
    result.regionId = d.region.regionId;
    result.player = s.player; result.presentationPlayer = s.player.rect;
    result.echo = s.echo;
    if (s.pendingEcho) { result.pendingEcho = s.pendingEcho->origin; result.pendingEchoAge = s.pendingEcho->age; result.pendingEchoFacing = s.pendingEcho->facing; }
    result.beams = s.beams; result.checkpointId = s.checkpointId;
    result.dashing = s.dash.has_value(); result.bouncing = s.bounce.has_value();
    result.tick = s.tick; result.deaths = s.deaths; result.recalls = s.recalls; result.swaps = s.swaps; result.echoes = s.echoes;
    result.simulationTime = s.simulationTime; result.realTime = s.realTime;
    // 表示位置だけを補間し、当たり判定とセンサーは確定した物理位置を使う。
    if (s.dash) {
        const double ease = Math::SmoothStep(s.dash->age / s.dash->duration);
        const auto& end = s.bounce ? s.bounce->contact : s.player.rect;
        result.presentationPlayer.x = s.dash->source.x + (end.x - s.dash->source.x) * ease;
        result.presentationPlayer.y = s.dash->source.y + (end.y - s.dash->source.y) * ease;
    } else if (s.bounce) {
        const double t = std::clamp((s.bounce->age - .08) / .12, 0.0, 1.0), ease = 1 - std::pow(1 - t, 3);
        result.presentationPlayer.x = s.bounce->contact.x + (s.player.rect.x - s.bounce->contact.x) * ease;
        result.presentationPlayer.y = s.bounce->contact.y + (s.player.rect.y - s.bounce->contact.y) * ease;
    }
    // 圧力板につながっている仕掛け（つながっていない扉・橋は通電しない）。
    std::unordered_set<std::string> linked;
    for (const auto& dev : d.devices) if (dev.kind == DeviceKind::Plate) linked.insert(dev.targets.begin(), dev.targets.end());
    for (size_t i = 0; i < d.devices.size() && i < s.devices.size(); ++i) {
        const auto& dev = d.devices[i]; const auto& state = s.devices[i];
        const bool solid = dev.kind == DeviceKind::Solid || dev.kind == DeviceKind::OneWay || dev.kind == DeviceKind::Spring ||
            ((dev.kind == DeviceKind::Door || dev.kind == DeviceKind::LockWall || dev.kind == DeviceKind::Bridge) && !state.active);
        DeviceSnapshot snapshot{dev.id, dev.kind, dev.rect, dev.direction, state.active, state.pressed, solid};
        if (dev.kind == DeviceKind::Spring) {
            snapshot.charge = std::clamp(state.charge / std::max(0.05, dev.delay), 0.0, 1.0);
            snapshot.cooldown = std::clamp(state.cooldown / std::clamp(dev.cooldown, 3.0, 30.0), 0.0, 1.0);
        }
        snapshot.powered = dev.kind == DeviceKind::Plate ? state.active
                         : linked.contains(dev.id) && (dev.inverted ? !state.active : state.active);
        snapshot.variant = dev.variant; snapshot.latch = dev.latch; snapshot.depth = dev.depth;
        snapshot.world = dev.kind == DeviceKind::Plate && std::any_of(dev.targets.begin(), dev.targets.end(), IsMachineTarget);
        result.devices.push_back(std::move(snapshot));
    }
    return result;
}
StepOutput RemoveWorldEcho(const WorldDefinition& d, WorldState& s) {
    StepOutput out; Simulation sim(d, s, out);
    if (s.echo || s.pendingEcho) sim.ReplaceEcho();
    const auto crush = sim.Switches(); sim.Beams();
    if (!crush.empty()) sim.Die(crush);
    FinishOutput(s, out); return out;
}
}
