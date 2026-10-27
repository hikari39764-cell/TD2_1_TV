// 装置：板と配線、光線、押し刺、危険、検査点、バネ、宝石とゴール。
#include "TD2RulesSimulation.h"

namespace TD2::Rules::Detail {
std::string Simulation::Switches() {
    // unpowered はつながる板のうち一枚でも通電していない装置（all の装置はここに入ると動かない）。
    std::unordered_set<std::string> linked, powered, unpowered;
    for (size_t i = 0; i < d.devices.size(); ++i) {
        const auto& dev = d.devices[i]; auto& state = s.devices[i];
        if (dev.kind != DeviceKind::Plate) continue;
        // 天井向きの板は、下から上がって来た頭が板に届いた時に押される（落ち始めると離れる）。
        const bool hanging = dev.direction == Direction::Down;
        const auto touches = [&](const Rect& r) {
            if (!OverlapX(r, dev.rect)) return false;
            if (hanging) { const double top = dev.rect.y; return r.y <= top + 7.0 / 32 && r.y >= top - 2.0 / 32 && r.y + r.h > top; }
            const double feet = r.y + r.h, bottom = dev.rect.y + dev.rect.h;
            return feet >= bottom - 7.0 / 32 && feet <= bottom + 2.0 / 32 && r.y < bottom;
        };
        const auto presses = [&](const BodyState& b) { return !b.dead && touches(b.rect) && (hanging ? b.vy <= kEps : b.vy >= -kEps); };
        // 壁蹴りのダッシュで残影が出るまでの間も、出発点の板は押されたままにする（離して押し直すと二回分の入力になる）。
        const auto pending = [&] {
            return s.pendingEcho && touches(s.pendingEcho->origin);
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
        for (const auto& target : dev.targets) { linked.insert(target); if (state.active) powered.insert(target); else unpowered.insert(target); }
    }
    std::string death;
    for (size_t i = 0; i < d.devices.size(); ++i) {
        const auto& dev = d.devices[i]; auto& state = s.devices[i];
        if (dev.kind != DeviceKind::Door && dev.kind != DeviceKind::Laser && dev.kind != DeviceKind::Bridge &&
            dev.kind != DeviceKind::Mechanism && dev.kind != DeviceKind::Goal && dev.kind != DeviceKind::Pusher) continue;
        // つながっていない光線と通関の扉は動いたまま（開いたまま）、ほかは止まったまま。
        const bool on = powered.contains(dev.id) && !(dev.all && unpowered.contains(dev.id));
        const bool active = linked.contains(dev.id) ? (dev.inverted ? !on : on)
                                                    : dev.kind == DeviceKind::Laser || dev.kind == DeviceKind::Goal;
        // 門と伸縮橋は実体に戻る瞬間に重なる体を挟む。
        const bool closing = state.active && !active && (dev.kind == DeviceKind::Door || dev.kind == DeviceKind::Bridge);
        if (state.active != active) Event(EventKind::DeviceChanged, dev.id, active ? "active" : "inactive", dev.rect);
        state.active = active;
        if (closing) {
            if (s.echo && Overlap(s.echo->rect, dev.rect)) {
                Event(EventKind::EchoRemoved, "echo", "door-crush", s.echo->rect); s.echo.reset(); s.echoOverlapGrace = false;
                ++s.crushes; s.crushSideways = dev.rect.h >= dev.rect.w;
            }
            if (!s.player.dead && Overlap(s.player.rect, dev.rect)) death = "door-crush";
        }
    }
    return death;
}
void Simulation::Beams() {
    s.beams.clear();
    for (size_t i = 0; i < d.devices.size(); ++i) {
        const auto& dev = d.devices[i];
        if (dev.kind != DeviceKind::Laser || !s.devices[i].active) continue;
        const int dx = dev.direction == Direction::Right ? 1 : dev.direction == Direction::Left ? -1 : 0;
        const int dy = dev.direction == Direction::Down ? 1 : dev.direction == Direction::Up ? -1 : 0;
        const double x = dev.rect.x + dev.rect.w / 2, y = dev.rect.y + dev.rect.h / 2;
        double length = dev.range;
        const double half = std::clamp(dev.width, 1.0 / 32, .9) * .5;
        const Rect area{std::min(x, x + dx * length) - half, std::min(y, y + dy * length) - half,
                        std::abs(dx * length) + 2 * half, std::abs(dy * length) + 2 * half};
        auto blockers = Colliders(area, nullptr, false, {}, true, true);
        if (s.echo) blockers.push_back({s.echo->rect, "echo"});
        for (const auto& c : blockers) {
            const auto& r = c.rect;
            double distance;
            if (dx) {
                if (y + half <= r.y + kEps || y - half >= r.y + r.h - kEps) continue;
                if (dx > 0) { if (r.x + r.w <= x + kEps) continue; distance = std::max(0.0, r.x - x); }
                else { if (r.x >= x - kEps) continue; distance = std::max(0.0, x - r.x - r.w); }
            } else {
                if (x + half <= r.x + kEps || x - half >= r.x + r.w - kEps) continue;
                if (dy > 0) { if (r.y + r.h <= y + kEps) continue; distance = std::max(0.0, r.y - y); }
                else { if (r.y >= y - kEps) continue; distance = std::max(0.0, y - r.y - r.h); }
            }
            length = std::min(length, distance);
        }
        if (length <= kEps) continue;
        const Rect beam = dx ? Rect{std::min(x, x + dx * length), y - half, length, 2 * half}
                             : Rect{x - half, std::min(y, y + dy * length), 2 * half, length};
        s.beams.push_back({dev.id, beam});
    }
}
bool Simulation::SpikeAt(const Rect& rect) const {
    return std::any_of(d.devices.begin(), d.devices.end(), [&](const DeviceDefinition& dev) {
        return dev.kind == DeviceKind::Spikes && Overlap(rect, SpikeHitbox(dev.rect, dev.direction));
    });
}
bool Simulation::PusherFaceAt(const Rect& rect) const {
    for (size_t i = 0; i < d.devices.size(); ++i) {
        const auto& dev = d.devices[i];
        if (dev.kind == DeviceKind::Pusher && Overlap(rect, PusherFace(PusherRect(dev, s.devices[i].push), dev.direction))) return true;
    }
    return false;
}
// 押し刺：板で動く物は通電で伸び、切れると戻る。往復の物（dwell > 0）は両端で止まりながら時計で行き来する。
// 動いた分だけ上に乗った体を運び、重なった残影を押し出す（押し切れない残影は潰れて消える）。
// 刺の面が本体に当たれば死ぬ。背中側で本体を押し切れない時は挟まれて死ぬ。
std::string Simulation::Pushers() {
    std::string death;
    for (size_t i = 0; i < d.devices.size(); ++i) {
        const auto& dev = d.devices[i];
        if (dev.kind != DeviceKind::Pusher) continue;
        auto& state = s.devices[i];
        const double seconds = std::max(.05, dev.seconds);
        double next = state.push;
        if (dev.dwell > 0) {
            state.clock += kStep;
            next = TimedPush(dev, state.clock);
        } else {
            const double target = state.active ? 1.0 : 0.0, rate = kStep / seconds;
            next = target > state.push ? std::min(target, state.push + rate) : std::max(target, state.push - rate);
        }
        if (std::abs(next - state.push) < 1e-12) continue;
        const Rect before = PusherRect(dev, state.push), after = PusherRect(dev, next);
        // 通った範囲全体で当たりを見る（速い押し刺が一 tick で体を飛び越さないように）。
        const Rect swept = Sweep(before, after);
        const bool forward = next > state.push;
        state.push = next;
        const double dx = after.x - before.x, dy = after.y - before.y;
        const bool horizontal = std::abs(dx) > 0;
        // 上に乗った体（とその上に載った体）は横の動きだけ一緒に運ぶ。上下の動きは押し出しと重力に任せる。
        if (horizontal) {
            for (BodyState* b : {&s.player, s.echo ? &*s.echo : nullptr}) {
                if (!b || b->dead || b->supportId != dev.id) continue;
                const bool echoBody = b != &s.player;
                BodyState* rider = RiderFor(*b, echoBody);
                const double x0 = b->rect.x;
                Move(*b, echoBody, dx, true, dev.id);
                if (rider) Move(*rider, !echoBody, b->rect.x - x0, true, echoBody ? "echo" : "player");
            }
        }
        // 重なった残影を押し出す。壁や本体に挟まれて押し切れなければ潰れて消える。
        if (s.echo && Overlap(s.echo->rect, swept)) {
            auto& e = *s.echo;
            const double need = horizontal ? (dx > 0 ? after.x + after.w - e.rect.x : after.x - e.rect.x - e.rect.w)
                                           : (dy > 0 ? after.y + after.h - e.rect.y : after.y - e.rect.y - e.rect.h);
            BodyState* rider = RiderFor(e, true);
            const double x0 = e.rect.x, y0 = e.rect.y;
            Move(e, true, need, horizontal);
            bool crushed = Overlap(e.rect, after);
            if (!crushed) {
                (horizontal ? e.vx : e.vy) = 0;
                s.echoPushed = true;
                if (rider) { Move(*rider, false, e.rect.x - x0, true, "echo"); Move(*rider, false, e.rect.y - y0, false, "echo"); }
                // 上に立つ本体が天井で止まったら、残影はその間で潰れる（本体へめり込ませない）。
                crushed = rider && Overlap(rider->rect, e.rect);
            }
            if (crushed) {
                Event(EventKind::EchoRemoved, "echo", "push-crush", e.rect);
                if (s.player.supportId == "echo") { s.player.supportId.clear(); s.player.grounded = false; }
                s.echo.reset(); s.echoOverlapGrace = false;
                ++s.crushes; s.crushSideways = horizontal;
            }
        }
        // 本体：刺の面で触れれば死ぬ。戻る背中に押されて押し切れなければ挟まれて死ぬ。
        if (!s.player.dead && death.empty() && Overlap(s.player.rect, swept)) {
            if (forward) death = "spikes";
            else {
                auto& p = s.player;
                const double need = horizontal ? (dx > 0 ? after.x + after.w - p.rect.x : after.x - p.rect.x - p.rect.w)
                                               : (dy > 0 ? after.y + after.h - p.rect.y : after.y - p.rect.y - p.rect.h);
                // 頭に載せた残影も一緒に押す。天井などで付いて来られなければ潰れる。
                BodyState* rider = RiderFor(p, false);
                const double x0 = p.rect.x, y0 = p.rect.y;
                Move(p, false, need, horizontal, dev.id);
                if (Overlap(p.rect, after)) death = "crush";
                else if (rider) {
                    Move(*rider, true, p.rect.x - x0, true, "player"); Move(*rider, true, p.rect.y - y0, false, "player");
                    if (Overlap(rider->rect, p.rect)) {
                        Event(EventKind::EchoRemoved, "echo", "push-crush", rider->rect);
                        s.echo.reset(); s.echoOverlapGrace = false;
                        ++s.crushes; s.crushSideways = horizontal;
                    }
                }
            }
        }
    }
    return death;
}
std::string Simulation::Hazard() const {
    if (SpikeAt(s.player.rect) || PusherFaceAt(s.player.rect)) return "spikes";
    for (const auto& beam : s.beams) if (Overlap(s.player.rect, beam.rect)) return "laser";
    if (!InWorld(d, s.player.rect)) return "out-of-world";
    return {};
}
std::optional<Vec2> Simulation::CheckpointAnchor(const DeviceDefinition& checkpoint) const {
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
            if (other.kind == DeviceKind::Pusher) {
                const Rect a = Sweep(PusherRect(other, 0.0), PusherRect(other, 1.0)), f = PusherFace(PusherRect(other, 1.0), other.direction);
                const double x0 = std::min(a.x, f.x), y0 = std::min(a.y, f.y);
                const Rect lane{x0, y0, std::max(a.x + a.w, f.x + f.w) - x0, std::max(a.y + a.h, f.y + f.h) - y0};
                return Overlap(rect, lane);
            }
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
void Simulation::Springs() {
    for (size_t i = 0; i < d.devices.size(); ++i) {
        const auto& dev = d.devices[i]; auto& state = s.devices[i];
        if (dev.kind != DeviceKind::Spring) continue;
        state.cooldown = std::max(0.0, state.cooldown - kStep);
        // 天井向きのバネは打ち上げない（体は下へ落ちるので、踏み板の背に乗っても溜めない）。
        if (dev.direction == Direction::Down) { state.charge = 0; state.impact = 0; continue; }
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
void Simulation::Launch(size_t i, const std::vector<BodyState*>& occupants, double impact) {
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
void Simulation::Sensors() {
    for (size_t i = 0; i < d.devices.size(); ++i) {
        const auto& dev = d.devices[i]; auto& state = s.devices[i];
        // 収集物は本体が触れた時だけ拾う。死亡やチェックポイントからの復帰では戻さない。
        if (dev.kind == DeviceKind::Collectible) {
            if (!state.active && !s.player.dead && Overlap(s.player.rect, dev.rect)) {
                state.active = true; Event(EventKind::CollectibleTaken, dev.id, {}, dev.rect);
            }
            continue;
        }
        // 通関の扉：開いている扉の真ん中へ本体が入った時に一度だけ知らせる（pressed は入った印、戻さない）。
        if (dev.kind == DeviceKind::Goal) {
            const Rect inside{dev.rect.x + dev.rect.w * .3, dev.rect.y, dev.rect.w * .4, dev.rect.h};
            if (state.active && !state.pressed && !s.player.dead && Overlap(s.player.rect, inside)) {
                state.pressed = true; Event(EventKind::GoalReached, dev.id, {}, dev.rect);
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
}
