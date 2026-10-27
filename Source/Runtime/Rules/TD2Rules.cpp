#include "TD2RulesSimulation.h"
#include "../../Common/TD2MachineText.h"
#include <stdexcept>

namespace TD2::Rules {
using namespace Detail;

namespace Detail {
void Simulation::Tick(const InputState& input, bool actionPressed, bool actionReleased) {
    s.simulationTime += kStep; ++s.tick;
    s.echoPushed = false;
    ResolveEchoSpikes();
    if (s.player.dead) {
        s.deathRemaining -= kStep; Pending(); Switches(); (void)Pushers();
        if (s.echo) Integrate(*s.echo, true);
        RemoveFallenEcho(); Switches(); Beams(); Springs(); Sensors();
        if (s.deathRemaining <= kEps) Respawn();
        UpdateEchoCramped();
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
    if (const auto reason = Pushers(); !reason.empty()) { Die(reason); return; }
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
    UpdateEchoCramped();
}
}

namespace {

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
    case E::EchoSwapped: return "EchoSwapped"; case E::GoalReached: return "GoalReached";
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
                                      it->kind != DeviceKind::Mechanism && it->kind != DeviceKind::Goal && it->kind != DeviceKind::Pusher))
            return "plate targets must reference an existing door, bridge, laser, goal, pusher or mechanism";
    }
    return {};
}
std::optional<Vec2> CheckpointRespawnPoint(const WorldDefinition& d, WorldState& s, size_t device) {
    if (device >= d.devices.size() || d.devices[device].kind != DeviceKind::Checkpoint) return {};
    StepOutput out; Simulation sim(d, s, out);
    return sim.CheckpointAnchor(d.devices[device]);
}
WorldState CreateWorldState(const WorldDefinition& d) {
    if (const auto error = ValidateWorld(d); !error.empty()) throw std::invalid_argument(error);
    WorldState s; s.player = MakeBody(d.spawn); s.checkpointPosition = d.spawn; s.devices.resize(d.devices.size());
    // 往復の押し刺は時計 0 の位置から始める（ずれが伸びた側にある物が、最初の tick で一気に伸びないように）。
    for (size_t i = 0; i < d.devices.size(); ++i)
        if (d.devices[i].kind == DeviceKind::Pusher && d.devices[i].dwell > 0) s.devices[i].push = TimedPush(d.devices[i], 0.0);
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
    result.player = s.player;
    Rect shown = s.player.rect;
    result.echo = s.echo;
    if (s.pendingEcho) { result.pendingEcho = s.pendingEcho->origin; result.pendingEchoAge = s.pendingEcho->age; result.pendingEchoFacing = s.pendingEcho->facing; }
    result.beams = s.beams; result.checkpointId = s.checkpointId;
    result.dashing = s.dash.has_value(); result.bouncing = s.bounce.has_value();
    result.tick = s.tick; result.deaths = s.deaths; result.recalls = s.recalls; result.swaps = s.swaps; result.echoes = s.echoes;
    result.crushes = s.crushes; result.crushSideways = s.crushSideways; result.echoCramped = s.echoCramped;
    result.echoPushed = s.echoPushed;
    result.simulationTime = s.simulationTime; result.realTime = s.realTime;
    // 表示位置だけを補間し、当たり判定とセンサーは確定した物理位置を使う。
    if (s.dash) {
        const double ease = Math::SmoothStep(s.dash->age / s.dash->duration);
        const auto& end = s.bounce ? s.bounce->contact : s.player.rect;
        shown.x = s.dash->source.x + (end.x - s.dash->source.x) * ease;
        shown.y = s.dash->source.y + (end.y - s.dash->source.y) * ease;
    } else if (s.bounce) {
        const double t = std::clamp((s.bounce->age - .08) / .12, 0.0, 1.0), ease = 1 - std::pow(1 - t, 3);
        shown.x = s.bounce->contact.x + (s.player.rect.x - s.bounce->contact.x) * ease;
        shown.y = s.bounce->contact.y + (s.player.rect.y - s.bounce->contact.y) * ease;
    }
    result.presentationPlayer = DrawnPlayer(shown);
    // 圧力板につながっている仕掛け（つながっていない扉・橋は通電しない）。
    std::unordered_set<std::string> linked;
    for (const auto& dev : d.devices) if (dev.kind == DeviceKind::Plate) linked.insert(dev.targets.begin(), dev.targets.end());
    for (size_t i = 0; i < d.devices.size() && i < s.devices.size(); ++i) {
        const auto& dev = d.devices[i]; const auto& state = s.devices[i];
        const bool solid = dev.kind == DeviceKind::Solid || dev.kind == DeviceKind::OneWay || dev.kind == DeviceKind::Spring ||
            ((dev.kind == DeviceKind::Door || dev.kind == DeviceKind::LockWall || dev.kind == DeviceKind::Bridge) && !state.active);
        DeviceSnapshot snapshot{dev.id, dev.kind, dev.kind == DeviceKind::Pusher ? PusherRect(dev, state.push) : dev.rect,
                                dev.direction, state.active, state.pressed, solid || dev.kind == DeviceKind::Pusher};
        snapshot.push = state.push; snapshot.travel = dev.travel; snapshot.timed = dev.dwell > 0;
        if (dev.kind == DeviceKind::Spring) {
            snapshot.charge = std::clamp(state.charge / std::max(0.05, dev.delay), 0.0, 1.0);
            snapshot.cooldown = std::clamp(state.cooldown / std::clamp(dev.cooldown, 3.0, 30.0), 0.0, 1.0);
            snapshot.chargeSeconds = std::max(0.05, dev.delay); snapshot.cooldownSeconds = std::clamp(dev.cooldown, 3.0, 30.0);
        }
        // 通関の扉の灯は開いている間（つながっていなくても）点ける。
        snapshot.powered = dev.kind == DeviceKind::Plate || dev.kind == DeviceKind::Goal ? state.active
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
