#pragma once

#include "../../Common/TD2RegionData.h"
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace TD2::Rules {

// 座標は左上原点、下向き Y のマス単位。表示ピクセルと 3D の縮尺は参照しない。
struct Vec2 { double x = 0.0, y = 0.0; };
struct Rect { double x = 0.0, y = 0.0, w = 1.0, h = 1.0; };
// 当たりの矩形から、足元で揃えた描画用の矩形を作る。
inline Rect DrawnPlayer(const Rect& body) { return {body.x, body.y + body.h - kPlayerDrawHeight, body.w, kPlayerDrawHeight}; }
enum class Direction { Up, Down, Left, Right };
// 半回転した向き（部屋が 180° 回った時の装置の向き）。
inline Direction Opposite(Direction d) {
    return d == Direction::Up ? Direction::Down : d == Direction::Down ? Direction::Up : d == Direction::Left ? Direction::Right : Direction::Left;
}
// Spring は上面 10/32 マスの踏み板。Bridge は Door と同じ通電規則（通電で収納）。Collectible は本体だけが拾う。
// Decal はチュートリアルの貼り絵。衝突も光線の遮断もない。
// Goal は通関の扉。圧力板につながっていなければ開いたまま、つながっていれば通電の間だけ開く。開いた扉に本体が入ると一度だけ知らせる。
// Mechanism は画面の外の機構（転台・吊り台など）の受け口。当たりも表示も持たず、圧力板の通電だけを受ける。
// Spring・Plate・OneWay は direction が Down の時に天井向き（回す部屋が回ると、装置は舞台ごと回って向きが裏返る）。
// 天井向きのバネは打ち上げず踏み板が当たるだけ、天井向きの板は下から頭が当たった時に押され、天井向きの一方通行は下からだけ当たる。
enum class DeviceKind { Solid, OneWay, Plate, Door, Laser, Spikes, Checkpoint, LockWall, Spring, Bridge, Collectible, Decal, Goal, Pusher, Mechanism };

// 地形は RegionDefinition だけから作り、センサーと動く障害物だけを別に定義する。
// targets は Plate から装置 ID への OR 接続。Door は active 時に実体を失う。
struct DeviceDefinition {
    std::string id;
    DeviceKind kind = DeviceKind::Solid;
    Rect rect;
    std::vector<std::string> targets;
    bool inverted = false;
    // Plate：踏むたびに入・切が切り替わる（離しても保つ）。false は踏んでいる間だけ通電。
    bool latch = false;
    // 圧力板につながる装置：つながる板の全部が通電した時だけ動く（二か所を同時に押さえる謎）。false はどれか一つで動く。
    bool all = false;
    Direction direction = Direction::Up;
    double range = 20.0;
    // Spring：基礎の打ち上げ速度（マス/秒）、溜める秒数、発射後の冷却秒数（3〜30）。
    double power = 22.5, delay = 0.85, cooldown = 3.0;
    // Decal：図案の種類（ad / space / spaceHold、前景の fore / foreCable）と不透明度。
    std::string variant;
    // Decal の前景：玩法の面（人物の立つ面）より手前の距離（マス）。見た目だけで判定には使わない。
    double depth = 1.0;
    // Pusher（押し刺）：dir の向きへ travel マス、seconds 秒で伸びる刺の塊。dwell が 0 なら圧力板の通電で伸び、
    // 0 より大きいと両端で dwell 秒止まりながら往復する（phase は往復の始まりのずれ、0〜1）。
    double travel = 3.0, seconds = 0.5, dwell = 0.0, phase = 0.0;
    // Laser：光の太さ（マス）。並べて帘にする時は太くして、隙間を体の幅より狭くする（細い光の間は体がすり抜けられる）。
    double width = 4.0 / 32;
};
// 配置済みテレビだけを疎な地形として保持する。origin は整数マスの世界座標。
struct TerrainChunk { RegionDefinition region; int x = 0, y = 0; };
struct WorldDefinition {
    RegionDefinition region;
    Vec2 spawn{1.0, 1.0}; // プレイヤー左上。実際の床面に合わせる。
    std::vector<DeviceDefinition> devices;
    std::vector<TerrainChunk> chunks;
    // 部屋の和集合の外周だけを閉じる。共有辺には壁を作らず、地形と装置が通行を決める。
    std::vector<Rect> boundaries;
};
struct ButtonState { bool held = false, pressed = false, released = false; };
struct InputState { ButtonState left, right, action; };

struct BodyState {
    Rect rect{0.0, 0.0, kPlayerWidth, kPlayerHeight};
    double vx = 0.0, vy = 0.0;
    double walkVx = 0.0, reboundTime = 0.0;
    int facing = 1, reboundDirection = 0;
    bool grounded = false, dead = false;
    std::string supportId;
};
struct DeviceState {
    bool active = false, pressed = false;
    // Spring の溜め・冷却の経過秒と、この刻みの着地の衝撃速度（マス/秒）。
    double charge = 0.0, cooldown = 0.0, impact = 0.0;
    // 別のチェックポイントにいる間、古いものを毎フレーム登録し直さないよう接触の状態を持つ。
    bool playerContact = false, echoContact = false;
    // Pusher の伸び（0 = 元の位置、1 = 伸び切り）と往復の時計（秒）。
    double push = 0.0, clock = 0.0;
};
struct BeamState { std::string sourceId; Rect rect; };
enum class EventKind {
    Blink, Bounce, BlinkRejected, EchoCreated, EchoRecalled, EchoRemoved,
    EchoSpawnBlocked, PlateChanged, DeviceChanged, CheckpointActivated,
    PlayerDied, PlayerRespawned, SpringLaunched, CollectibleTaken, EchoSwapped, GoalReached
};
// ログ用のイベント名。
std::string_view EventName(EventKind kind);
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
    // 残影を回収した回数（表示用。回収時にプレイヤー側のエフェクトを一度出す）。
    uint64_t recalls = 0;
    uint64_t swaps = 0;   // 交換の回数（表示用）
    uint64_t echoes = 0;  // 残影を出した回数（表示用。ダッシュで前の残影と入れ替わった時も新しい残影の光を出す）
    // 残影が門・橋・押し刺で潰れた回数と、最後に横から潰れたか（表示用。回収とは別の潰れ方を見せる）。
    uint64_t crushes = 0;
    bool crushSideways = false;
    // 残影のすぐ上（0.3 マス以内）が地形や仕掛けで塞がっているか（表示用。狭い所では身を縮める）。
    bool echoCramped = false;
    // 最後の tick で押し刺が残影を押し動かしたか（表示用。ひるみと土煙。乗った物ごと動くのは数えない）。
    bool echoPushed = false;
    // 固定ステップの無いフレームでも入力の押下を持ち越し、次のステップで使う。
    bool pendingActionPressed = false, pendingActionReleased = false;
    // 残影がいる時のスペース：押してからの秒数。短く離せば回収、押し続ければ交換。
    double actionHold = 0.0;
    bool actionArmed = false;
};
struct StepOutput {
    std::vector<RuleEvent> events;
    uint64_t tick = 0;
    double simulationTime = 0.0, realTime = 0.0;
};
struct DeviceSnapshot {
    std::string id;
    DeviceKind kind = DeviceKind::Solid;
    Rect rect;
    Direction direction = Direction::Up;
    bool active = false, pressed = false, solid = false;
    // 表示用：Spring の溜め（0〜1）と冷却の残り（1→0）、溜めと冷却にかかる秒数（骨つきの物体の動作の長さに使う）。
    double charge = 0.0, cooldown = 0.0, chargeSeconds = 0.0, cooldownSeconds = 0.0;
    // 表示用：通電しているか（扉・橋・光線は反転前の値、つながっていなければ false）。圧力板は通電を出しているか。
    // 圧力板とその先の仕掛けの灯を同時に点け、どれとどれがつながっているかを見せる。
    bool powered = false;
    std::string variant;
    bool latch = false;   // 切り替え式の圧力板
    bool world = false;   // 画面の外のテレビ（機構・スロット棚）を動かす圧力板。画面の中だけの圧力板と色と形を分ける
    double depth = 1.0;   // Decal の前景の手前への距離（マス）
    double push = 0.0;    // Pusher の伸び（0〜1）。rect は伸びた今の位置
    double travel = 0.0;  // Pusher の伸びるマス数（腕の長さ = travel×push）
    bool timed = false;   // Pusher が時計で往復する（圧力板につながらず、通電灯を出さない）
};
struct WorldSnapshot {
    std::string regionId;
    bool playerPresent = true;   // 常に true（画面の描画がまだ読む）
    BodyState player; // 判定座標。描画には presentationPlayer を使う。
    Rect presentationPlayer;   // 描画用：足元を判定に揃えた見た目の大きさ（冲刺・跳ね返りの間は補間した位置）
    std::optional<BodyState> echo;
    std::optional<Rect> pendingEcho;
    // 予告中の残影の経過秒と向き（跳ね返りの後、足元から現れる表示に使う）。
    double pendingEchoAge = 0.0;
    int pendingEchoFacing = 1;
    std::vector<DeviceSnapshot> devices;
    std::vector<BeamState> beams;
    std::string checkpointId;
    bool dashing = false, bouncing = false;
    uint64_t tick = 0, deaths = 0, recalls = 0, swaps = 0, echoes = 0, crushes = 0;
    bool crushSideways = false, echoCramped = false, echoPushed = false;
    double simulationTime = 0.0, realTime = 0.0;
};

// 空文字なら有効。初期化前に呼び、不正な作者データを知らせる。
std::string ValidateWorld(const WorldDefinition& definition);
WorldState CreateWorldState(const WorldDefinition& definition);
// ホストは実フレーム秒を一度だけ渡す。1/120 秒の固定刻みと演出の時間配分はここで行う。
// 一度の入力時間は 0〜0.25 秒まで。held は押しっぱなし、pressed/released はその瞬間だけ。
StepOutput StepRules(const WorldDefinition& definition, WorldState& state,
                     const InputState& input, double realDeltaSeconds);
WorldSnapshot BuildSnapshot(const WorldDefinition& definition, const WorldState& state);
// 残影を回収の演出なしで消す（大区域を出た時など）。消えた残影が押していた装置はその場で戻る。
StepOutput RemoveWorldEcho(const WorldDefinition& definition, WorldState& state);
// 検査点 device（devices の番号）で起き上がる位置（本体の左上）。立てる床が近くに無ければ空。
std::optional<Vec2> CheckpointRespawnPoint(const WorldDefinition& definition, WorldState& state, size_t device);

}
