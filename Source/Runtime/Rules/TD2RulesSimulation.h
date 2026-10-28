#pragma once
// 規則の一歩を進める内部の部品。TD2Rules*.cpp だけが読む（ほかのファイルは TD2Rules.h の公開の関数を使う）。
// Simulation は一つの WorldState を読み書きする一歩分の作業台。本体は体・残影・装置・一歩の四つの .cpp に分けてある。
#include "TD2Rules.h"
#include "../../Common/TD2Math.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace TD2::Rules::Detail {
inline constexpr double kEps = 0.001 / 32.0;
inline constexpr double kStep = 1.0 / 120.0;
inline constexpr double kGravity = 43.75, kMaxFall = 28.125;
inline constexpr double kSwapHold = .3;
inline constexpr double kGroundTolerance = 0.55 / 32.0;
// バネ：踏み板の厚さ、記録する最小の着地速度、打ち上げ速度と冷却の範囲（マス単位）。
inline constexpr double kSpringCap = 10.0 / 32.0, kSpringStep = 10.1 / 32.0, kSpringImpactMin = 60.0 / 32.0;
inline constexpr double kSpringPowerMin = 100.0 / 32.0, kSpringPowerMax = 1400.0 / 32.0;
// 天井向きのバネは踏み板がマスの上端に付く。
inline Rect SpringCap(const DeviceDefinition& d) {
    const Rect& r = d.rect;
    return d.direction == Direction::Down ? Rect{r.x, r.y, r.w, kSpringCap} : Rect{r.x, r.y + r.h - kSpringCap, r.w, kSpringCap};
}
// 棘の当たりはマスより小さい：根元側を残し、先端側 8/32、横 3/32 ずつ内側。
inline Rect SpikeHitbox(const Rect& r, Direction direction) {
    constexpr double tip = 8.0 / 32.0, side = 3.0 / 32.0;
    switch (direction) {
    case Direction::Up: return {r.x + side, r.y + tip, r.w - 2 * side, r.h - tip};
    case Direction::Down: return {r.x + side, r.y, r.w - 2 * side, r.h - tip};
    case Direction::Left: return {r.x + tip, r.y + side, r.w - tip, r.h - 2 * side};
    default: return {r.x, r.y + side, r.w - tip, r.h - 2 * side};
    }
}

// 押し刺の今の矩形（元の矩形を向きへ travel×伸び だけずらす）と、刺の付いた前の面のすぐ外の薄い帯。
inline Rect PusherRect(const DeviceDefinition& d, double push) {
    Rect r = d.rect; const double a = d.travel * push;
    switch (d.direction) {
    case Direction::Up: r.y -= a; break;
    case Direction::Down: r.y += a; break;
    case Direction::Left: r.x -= a; break;
    default: r.x += a; break;
    }
    return r;
}
// 往復の押し刺の伸び（0〜1）：端で dwell 秒止まりながら、seconds 秒で伸び縮みする。
inline double TimedPush(const DeviceDefinition& d, double clock) {
    const double seconds = std::max(.05, d.seconds), hold = d.dwell, period = 2 * (hold + seconds);
    const double t = std::fmod(clock + std::clamp(d.phase, 0.0, 1.0) * period, period);
    return t < hold ? 0.0 : t < hold + seconds ? (t - hold) / seconds : t < 2 * hold + seconds ? 1.0 : 1.0 - (t - 2 * hold - seconds) / seconds;
}
inline Rect PusherFace(const Rect& r, Direction direction) {
    constexpr double thin = 4.0 / 32, side = 1.0 / 32;   // 刃は面から 4/32 ほど出る（画面の丸鋸と同じ）
    switch (direction) {
    case Direction::Up: return {r.x + side, r.y - thin, r.w - 2 * side, thin};
    case Direction::Down: return {r.x + side, r.y + r.h, r.w - 2 * side, thin};
    case Direction::Left: return {r.x - thin, r.y + side, thin, r.h - 2 * side};
    default: return {r.x + r.w, r.y + side, thin, r.h - 2 * side};
    }
}
// 刺の塊（押し刺の variant=block）：腕の無い往復の刺。四方のどの面に触れても死に、本体には当たりが無い
// （乗れず、冲刺で突き抜けられる）。残影には塊として当たり、押される。
inline bool PusherBlock(const DeviceDefinition& d) { return d.kind == DeviceKind::Pusher && d.variant == "block"; }
// 刺の塊の死ぬ範囲：箱の周り 2/32（刃の先）。
inline Rect PusherHurt(const Rect& r) {
    constexpr double tip = 2.0 / 32;
    return {r.x - tip, r.y - tip, r.w + 2 * tip, r.h + 2 * tip};
}

inline bool OverlapX(const Rect& a, const Rect& b) {
    return a.x < b.x + b.w - kEps && a.x + a.w > b.x + kEps;
}
inline bool Overlap(const Rect& a, const Rect& b) {
    return OverlapX(a, b) && a.y < b.y + b.h - kEps && a.y + a.h > b.y + kEps;
}
inline Rect Sweep(const Rect& a, const Rect& b) {
    return {std::min(a.x, b.x), std::min(a.y, b.y),
            a.w + std::abs(b.x - a.x), a.h + std::abs(b.y - a.y)};
}
inline bool FiniteRect(const Rect& r) {
    return std::isfinite(r.x) && std::isfinite(r.y) && std::isfinite(r.w) &&
           std::isfinite(r.h) && r.w > 0 && r.h > 0;
}
inline bool WorldTile(const WorldDefinition& d, int64_t x, int64_t y, TileKind& tile) {
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
inline bool InWorld(const WorldDefinition& d, const Rect& r) {
    // 占有するマスがすべてあれば、テレビをまたぐ体も世界の中にある。
    for (int64_t y = int64_t(std::floor(r.y + kEps)); y <= int64_t(std::floor(r.y + r.h - kEps)); ++y)
        for (int64_t x = int64_t(std::floor(r.x + kEps)); x <= int64_t(std::floor(r.x + r.w - kEps)); ++x) {
            TileKind tile;
            if (!WorldTile(d, x, y, tile)) return false;
        }
    return true;
}
inline BodyState MakeBody(Vec2 at, bool echo = false) {
    BodyState b;
    b.rect = {at.x, at.y, kPlayerWidth, echo ? 1.0 : kPlayerHeight};
    return b;
}
struct Collider {
    Rect rect;
    std::string id;
    bool oneWay = false, spike = false, spring = false;
    Direction direction = Direction::Up;
    bool ceiling = false;   // 天井向きの一方通行：下からだけ当たる（上と横からは通り抜ける）
};
inline bool Rider(const BodyState* rider, const BodyState& carrier, const std::string& carrierId) {
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
    bool IsSolid(size_t i) const;
    std::vector<Collider> Colliders(const Rect& area, const BodyState* body = nullptr,
            bool echoBody = false, const std::string& ignore = {}, bool noBodies = false,
            bool fullOnly = false, bool staticOnly = false) const;
    bool Clear(const Rect& rect, const BodyState* body = nullptr, bool echoBody = false,
               const std::string& ignore = {}, bool noBodies = false, bool staticOnly = false) const;
    void Probe(BodyState& b, bool echoBody = false) const;
    BodyState* RiderFor(const BodyState& b, bool echoBody);
    void ValidateSupports();

    std::optional<double> SpikeTop(const BodyState& b, double x, const Collider& spike, const std::string& ignore);
    void Move(BodyState& b, bool echoBody, double amount, bool horizontal, const std::string& ignore = {});
    void Vertical(BodyState& b, bool echoBody);
    void RecordSpringImpact(const BodyState& b, bool wasGrounded, double speed);
    void Integrate(BodyState& b, bool echoBody);
    void Stack(BodyState& carrier, bool carrierEcho, BodyState& rider);
    void AlignHead();
    void ResolveEchoSpikes();
    Rect EchoOrigin(const Rect& source) const;
    void CreateEcho(const Rect& origin, int facing);
    void Pending();
    double HorizontalVelocity() const;
    void RetireEcho();
    void ReplaceEcho();
    void Swap();
    bool ConsumeBlink();

    std::string Switches();
    void Beams();
    bool SpikeAt(const Rect& rect) const;
    bool PusherFaceAt(const Rect& rect) const;
    std::string Pushers();
    std::string Hazard() const;
    std::optional<Vec2> CheckpointAnchor(const DeviceDefinition& checkpoint) const;
    void Springs();
    void Launch(size_t i, const std::vector<BodyState*>& occupants, double impact);
    void Sensors();
    void DetachHead();
    void Die(const std::string& reason);
    void Respawn();
    void RemoveFallenEcho();
    void Tick(const InputState& input, bool actionPressed, bool actionReleased);
    void UpdateEchoCramped();
};
}
