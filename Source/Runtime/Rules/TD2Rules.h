#pragma once

#include "../../Common/TD2RegionData.h"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace TD2::Rules {

// 座標は左上原点、下向き Y の格単位。表示ピクセルと 3D の縮尺は参照しない。
struct Vec2 { double x = 0.0, y = 0.0; };
struct Rect { double x = 0.0, y = 0.0, w = 1.0, h = 1.0; };
enum class Direction { Up, Down, Left, Right };
// Spring は上面 10/32 格の踏み板。Bridge は Door と同じ通電規則（通電で収納）。Collectible は本体だけが拾う。
// Decal は教程の貼り絵で、衝突も光線遮断も持たない表示専用の装置。
enum class DeviceKind { Solid, OneWay, Plate, Door, Laser, Spikes, Checkpoint, LockWall, Spring, Bridge, Collectible, Decal };

// 地形は RegionDefinition を唯一の情報源とし、センサーと可変の障害物だけを別定義にする。
// targets は Plate から装置 ID への OR 接続。Door は active 時に実体を失う。
struct DeviceDefinition {
    std::string id;
    DeviceKind kind = DeviceKind::Solid;
    Rect rect;
    std::vector<std::string> targets;
    bool inverted = false;
    Direction direction = Direction::Up;
    double range = 20.0;
    // Spring：基礎の打ち上げ速度（格/秒）、立って溜める秒数、発射後の冷却秒数（3〜30）。
    double power = 22.5, delay = 0.85, cooldown = 3.0;
    // Decal：図案の種類（ad / space / spaceHold）と不透明度。
    std::string variant;
    double opacity = 0.8;
};
// 配置済みテレビだけを疎な地形として保持する。origin は整数格の世界座標。
struct TerrainChunk { RegionDefinition region; int x = 0, y = 0; };
struct WorldDefinition {
    RegionDefinition region;
    Vec2 spawn{1.0, 1.0}; // プレイヤー左上。実際の床面に合わせる。
    std::vector<DeviceDefinition> devices;
    std::vector<TerrainChunk> chunks;
    // 房間の和集合の露出辺だけを閉じる。共有辺には障壁を作らず、地形・装置が通行を決める。
    std::vector<Rect> boundaries;
};
struct ButtonState { bool held = false, pressed = false, released = false; };
struct InputState { ButtonState left, right, action; };

struct BodyState {
    Rect rect{0.0, 0.0, 0.625, 0.875};
    double vx = 0.0, vy = 0.0;
    double walkVx = 0.0, reboundTime = 0.0;
    int facing = 1, reboundDirection = 0;
    bool grounded = false, dead = false;
    std::string supportId;
};
struct DeviceState {
    bool active = false, pressed = false;
    // Spring の溜め・冷却の経過秒と、この刻みに記録した着地の衝撃速度（格/秒）。
    double charge = 0.0, cooldown = 0.0, impact = 0.0;
    // 別の存档に滞在したまま古い存档を毎フレーム再登録しないため、接触辺沿を持つ。
    bool playerContact = false, echoContact = false;
};
struct BeamState { std::string sourceId; Rect rect; };
enum class EventKind {
    Blink, Bounce, BlinkRejected, EchoCreated, EchoRecalled, EchoRemoved,
    EchoSpawnBlocked, PlateChanged, DeviceChanged, CheckpointActivated,
    PlayerDied, PlayerRespawned, ExplorationRestarted, SpringLaunched, CollectibleTaken
};
struct RuleEvent {
    EventKind kind = EventKind::Blink;
    std::string subjectId, detail;
    Vec2 position;
    double simulationTime = 0.0;
};

// 補間は描画専用。遅延残影の時計は simulationTime、演出の時計は実時間で進む。
struct PendingEcho { Rect origin; int facing = 1; double age = 0.0; };
struct DashState { Rect source, contact; int facing = 1; double age = 0.0, duration = 0.14; };
struct BounceState { Rect contact; double age = 0.0; };
struct WorldState {
    BodyState player;
    // 非活動領域には本体を置かない。死亡完了を探索全体で処理する場合のみ自動復帰を無効にする。
    bool playerPresent = true, automaticRespawn = true;
    std::optional<BodyState> echo;
    std::vector<DeviceState> devices; // WorldDefinition.devices と同じ順序。
    std::vector<BeamState> beams;
    std::optional<PendingEcho> pendingEcho;
    std::optional<DashState> dash;
    std::optional<BounceState> bounce;
    std::string checkpointId;
    Vec2 checkpointPosition;
    double simulationTime = 0.0, realTime = 0.0, accumulator = 0.0;
    double blinkBuffer = 0.0, deathRemaining = 0.0;
    uint64_t tick = 0, deaths = 0;
    bool echoOverlapGrace = false;
    // 固定ステップのないフレームでも入力辺沿を保持し、次のステップだけで消費する。
    bool pendingActionPressed = false, pendingActionReleased = false;
};
struct StepOutput {
    std::vector<RuleEvent> events;
    uint32_t simulationSteps = 0;
    uint64_t tick = 0;
    double simulationTime = 0.0, realTime = 0.0;
};
struct DeviceSnapshot {
    std::string id;
    DeviceKind kind = DeviceKind::Solid;
    Rect rect;
    Direction direction = Direction::Up;
    bool active = false, pressed = false, solid = false;
    // 表示用：Spring の溜め（0〜1）と冷却の残り（1→0）。Decal の図案。
    double charge = 0.0, cooldown = 0.0;
    std::string variant;
};
struct WorldSnapshot {
    std::string regionId;
    bool playerPresent = true;
    BodyState player; // 判定座標。描画には presentationPlayer を使う。
    Rect presentationPlayer;
    std::optional<BodyState> echo;
    std::optional<Rect> pendingEcho;
    std::vector<DeviceSnapshot> devices;
    std::vector<BeamState> beams;
    std::string checkpointId;
    bool dashing = false, bouncing = false;
    uint64_t tick = 0, deaths = 0;
    double simulationTime = 0.0, realTime = 0.0;
};

// 空文字なら有効。初期化前に呼び、無効な作者データを SDK 側で明示する。
std::string ValidateWorld(const WorldDefinition& definition);
WorldState CreateWorldState(const WorldDefinition& definition);
// ホストは実フレーム秒を一度だけ渡す。1/120 固定刻みと演出による時間配分は本関数が所有する。
// 原型同様、一度の入力時間は 0～0.25 秒に制限。held は継続値、pressed/released は一度の辺沿。
StepOutput StepRules(const WorldDefinition& definition, WorldState& state,
                     const InputState& input, double realDeltaSeconds);
WorldSnapshot BuildSnapshot(const WorldDefinition& definition, const WorldState& state);
// 通常復帰は世界を保存する。探索再開だけが残影・装置・存档・死亡回数を初期化する。
StepOutput ReturnToCheckpoint(const WorldDefinition& definition, WorldState& state);
StepOutput RestartExploration(const WorldDefinition& definition, WorldState& state);

// 複数領域は活動区が決めた tick 数を共用し、残影・装置だけを同じ時計で進める。
StepOutput StepBackgroundRules(const WorldDefinition& definition, WorldState& state,
                               uint32_t simulationSteps, double realSeconds);
bool IsSafeArrival(const WorldDefinition& definition, const WorldState& state, Vec2 position,
                   const std::optional<Rect>& carriedEcho = {});
StepOutput RemovePlayer(const WorldDefinition& definition, WorldState& state);
StepOutput PlacePlayer(const WorldDefinition& definition, WorldState& state, Vec2 position,
                      int facing, bool respawning = false);
StepOutput RecallWorldEcho(const WorldDefinition& definition, WorldState& state);

} // namespace TD2::Rules
