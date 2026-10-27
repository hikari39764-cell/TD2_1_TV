#pragma once

#include "TD2Rules.h"
#include "../../Common/TD2WorldLayout.h"
#include <map>
#include <set>
#include <utility>

namespace TD2::Rules {
struct RegionSource { WorldDefinition definition; RegionPlacement placement; bool entry = false; };
// 各画面は作者の定義と配置だけを持つ。物理状態は世界に一つ。
struct RegionRuntime {
    WorldDefinition definition;
    RegionPlacement placement;
    bool flipped = false;   // 機構で 180° 回され、地図が上下左右に入れ替わっている（definition は作者のまま）
};
// 画面の外の機構（転台・吊り台・天秤など）。状態 0 は作者の配置、状態 1 は moves、状態 -1 は movesC の位置。
// 圧力板の通電で切り替え、動いている間は動く部屋をどこともつながない（中の時間は進む）。
// flip は部屋を 180° 回した向き（地図の上下左右が入れ替わる）。
struct MechanismMove { std::string regionId; int worldX = 0, worldY = 0; bool flip = false; };
struct MechanismSource {
    std::string id;
    bool toggle = false;      // false：踏んでいる間だけ状態 1（離すと戻る）、true：踏むたびに切り替え
    bool balance = false;     // 天秤：受け口は mech/ID と mech/ID/r。片方だけ踏まれるとその側へ傾き（1 / -1）、両方か無しで 0
    double seconds = 1.5;     // 状態の間を動く秒数
    double dwell = 0.0;       // 0 より大きければ圧力板に関係なく自動で往復し、両端でこの秒数だけ止まる
    double settle = 0.0;      // 踏み続け式・天秤：踏まれ方がこの秒数変わらなかった時だけ動き出す（走り抜けでは動かない）
    bool ride = false;        // 乗り物：動く部屋に外から入り、部屋の中に収まって着地したら反対の端へ向かう（圧力板は見ない）
    std::vector<std::string> rideFrom;   // 乗り場：空なら動く部屋そのもの。書けばそこへ入って着地した時に状態 1 へ動き、戻らない（道が降ってくる）
    // 乗り込む前の部屋：書けば、そこから入って来た時だけ動く（降りた先から一歩戻っただけでは動かない）。空ならどこから入っても動く。
    std::vector<std::string> rideEnter;
    // 降りた後に閉じる口：運ばれた後にこの部屋へ降りると、乗り物の部屋との境を閉じる（区域を移る乗り物を後戻りさせない）。
    std::vector<std::string> sealExits;
    std::vector<MechanismMove> moves;    // 状態 1
    std::vector<MechanismMove> movesC;   // 状態 -1（天秤だけ）
};
struct MechanismRuntime {
    MechanismSource source;
    std::vector<MechanismMove> origins;   // 状態 0 の位置（moves と movesC の部屋すべて）
    size_t device = 0, deviceR = 0;       // 圧力板の受け口（世界の装置の番号。deviceR は天秤の右）
    int target = 0, state = 0;            // 向かっている状態と確定した状態
    bool powered = false, moving = false;
    double progress = 0.0;                // 今の状態（-1〜1。動いている間は途中の値）
    double rest = 0.0;                    // 自動往復で、端に着いてから止まっている秒数
    int pending = 0; double pendingTime = 0.0;   // settle の間、待っている次の状態と経過秒
    bool rideArmed = false;               // 乗り物：外から乗り込んだばかりで、着地を待っている
    bool carried = false;                 // 乗り物：最後の動きで中の体を運んだ（乗り込み直すと消える）
};
// スロットの並んだ棚。メンバーの部屋の圧力板（targets=slide/区域ID）を踏むと、その部屋が隣の空いたスロットへ滑る。
// 空きが隣に無ければ動かず、blockedCount が増える（揺れの合図）。
struct SlideSlot { int worldX = 0, worldY = 0, col = 0, row = 0; };
struct SlideSource {
    std::string id;
    double seconds = 1.0;
    std::vector<SlideSlot> slots;
    std::vector<std::string> members;
};
struct SlideRuntime {
    SlideSource source;
    std::vector<int> slotOf;          // メンバーごとの今のスロット
    std::vector<size_t> devices;      // メンバーごとの受け口（slide/区域ID）
    std::vector<char> powered;
    int moving = -1, from = -1, to = -1;
    double progress = 0.0;
    int blocked = -1;                 // 最後に動けなかったメンバー
    uint64_t blockedCount = 0;
};
// 同じ受け口を指す圧力板ごとの踏み込み。どれか一枚が押されたままでも、別の板を踏めば一回分になる。
struct PlateEdges {
    std::vector<size_t> plates;
    std::vector<char> was;
};
// 重ねたテレビ：下から上へのスロットと、抜いたテレビの置き場。一番下を後ろへ抜くと、上のテレビが一段ずつ落ちる。
struct StackSource {
    std::string id;
    double pullSeconds = .8, dropSeconds = .35, resetSeconds = 2.0;
    std::vector<std::pair<int, int>> slots;   // 世界位置（下から上）
    std::vector<std::pair<int, int>> docks;   // 抜いたテレビの置き場（どこともつながらない所）
    std::vector<std::string> members;         // 最初に下のスロットから順に並ぶ部屋
};
struct StackRuntime {
    StackSource source;
    std::vector<int> at, to;      // メンバーごとの今の場所と向かう先：0 以上はスロット、-1-k は置き場 k
    PlateEdges pull, reset;
    int phase = 0;                // 0 止まっている、1 抜く、2 落ちる、3 元へ戻す
    double progress = 0.0;
    uint64_t blockedCount = 0;    // 抜けなかった回数（画面側の揺れの合図）
};
// 四面の回転柱：面（部屋）が立ち位置を時計回りに巡る。step だけ回した時、面 i は立ち位置 (i + step) mod 面の数。
struct CarouselSource {
    std::string id;
    double seconds = 1.6;
    std::vector<std::pair<int, int>> stations;   // 世界位置（正面・右・背面・左の順）
    std::vector<std::string> faces;              // 最初に立ち位置 0 から順にいる部屋
};
struct CarouselRuntime {
    CarouselSource source;
    int step = 0, direction = 0;   // direction：回っている間 +1（時計回り）か -1
    double progress = 0.0;
    PlateEdges cw, ccw;
};
// 遊びの進み（保存して次の起動で戻すもの）。仕掛けの状態（機構は向かっている先）・切り替え式の板・拾った宝石・
// 復活点だけで、体の勢いや残影、動いている途中の様子は含めない。
struct Progress {
    std::string region, checkpoint;   // 復活点のある部屋と、最後に触れたチェックポイント（装置 ID。無ければ空）
    Vec2 local;                       // 復活点の部屋の中の位置（マス、体の左端と足元）
    std::vector<std::pair<std::string, int>> mechanisms, carousels;            // 機構の状態、回転柱の回した数
    std::vector<std::pair<std::string, std::vector<int>>> slides, stacks;      // 棚のメンバーごとのスロット、塔の場所
    std::vector<std::string> latched, taken;   // 入っている切り替え式の板と、拾った宝石（装置 ID）
    std::vector<std::pair<std::string, std::string>> sealed;   // 乗り物で降りて閉じた境（部屋の組）
    // 通関の道：空は普通、"hidden" は隠しの道の途中、"end" は隠しの道を通した後（次の起動は机のパソコンのメールから）。
    // 規則は使わない（ゲームのセッションが書き、読む）。
    std::string finale;
};
struct RegionEvents { std::string regionId; StepOutput output; };
struct RegionTransition { std::string kind, fromRegion, toRegion; Vec2 worldPosition; };
struct ExplorationOutput {
    std::vector<RegionEvents> regions;
    std::vector<RegionTransition> transitions;
};

// 一つの連続した世界を進める。装置 ID は「区域ID/装置ID」。
class Exploration {
public:
    // この試遊だけ開始区域を差し替える。作者の設定は変えない。
    std::string Initialize(std::vector<RegionSource> sources, std::vector<MechanismSource> mechanisms,
                           std::vector<SlideSource> slides = {}, std::vector<StackSource> stacks = {}, std::vector<CarouselSource> carousels = {},
                           std::string_view entryOverride = {}, std::optional<Vec2> spawnOverride = {});
    const std::vector<MechanismRuntime>& Mechanisms() const noexcept { return mechanisms_; }
    const std::vector<SlideRuntime>& Slides() const noexcept { return slides_; }
    const std::vector<StackRuntime>& Stacks() const noexcept { return stacks_; }
    const std::vector<CarouselRuntime>& Carousels() const noexcept { return carousels_; }
    // 初期化で止めた仕掛けや外した圧力板の目標（世界は動かしたまま、作者へ知らせる）。
    const std::vector<std::string>& Problems() const noexcept { return problems_; }
    ExplorationOutput Step(const InputState& input, double seconds);
    const RegionRuntime* FindRegion(std::string_view id) const;
    const std::map<std::string, RegionRuntime>& Regions() const noexcept { return regions_; }
    const WorldState& State() const noexcept { return state_; }
    const std::string& ActiveRegion() const noexcept { return activeRegion_; }
    // 世界座標の表示状態。進めた後、最初に読まれた時に作る。
    const WorldSnapshot& Snapshot();
    bool Ready() const noexcept { return !activeRegion_.empty(); }
    // 手動の切り替えを起点まで戻した時、プレイヤーを元の区域の境の内側へ戻す。
    // direction は越えた向き（+1 右、-1 左）。時間や装置は戻さない。
    bool StepBack(std::string_view region, int direction);
    // 残影を消す（大区域を出た時。前の箱庭の装置を押したまま残さない）。
    void ClearEcho();
    // 残影を頭に載せて運んでいるか。運んでいる残影は大区域を出ても消さず、本体と一緒に持って行く。
    bool CarryingEcho() const noexcept { return state_.echo && state_.echo->supportId == "player"; }
    // 宝石（収集物）の拾った数と世界にある数。拾った物は死んでも戻らない。
    struct CollectibleCount { int taken = 0, total = 0; };
    CollectibleCount Collectibles() const;
    // 開発用：プレイヤーをその区域の出生点へ移す（残影は消し、速さと復活点も合わせる）。世界地図に無い区域なら false。
    // at を渡すとその部屋の中の位置（マス、本体の左上）へ、無ければ出生点へ置く。
    bool Teleport(std::string_view region, const Vec2* at = nullptr);
    // 試遊を途中の状態から始める：踏み方に関係なく切り替わる機構（切り替え式・自動往復）をその状態へ置く。
    bool SetMechanismState(std::string_view id, int state);
    // 遊びの進みを書き出す。
    Progress SaveProgress() const;
    // 書き出した進みへ戻す（Initialize した直後の世界に使う）。部屋はまとめて一度に動かし、復活点へ立たせる。
    // 戻せない時は理由を返す（世界は Initialize し直して使う）。今の世界に無い仕掛けや装置の行は無視する。
    std::string RestoreProgress(const Progress& progress);
    // 外から呼ばれた ClearEcho と StepBack の記録（試遊の記録が受け取って消す）。
    struct ExternalCall { std::string kind, region; int direction = 0; };
    std::vector<ExternalCall> TakeCalls() { return std::exchange(calls_, {}); }
private:
    void BuildBoundaries();
    void Record(StepOutput output, ExplorationOutput& out);
    std::string RegionAt(const Rect& body) const;
    void UpdateMechanisms(double seconds);
    void UpdateSlides(double seconds);
    bool Rising(PlateEdges& edges) const;
    void UpdateStacks(double seconds);
    void UpdateCarousels(double seconds);
    std::vector<MechanismMove> TargetsFor(const MechanismRuntime& mechanism, int state) const;
    // 部屋をまとめて動かす（重なるなら何もしない）。中の体・装置・記録も一緒に運び、flip が変われば 180° 回す。
    bool MoveRegions(const std::vector<MechanismMove>& targets);

    std::vector<MechanismRuntime> mechanisms_;
    std::vector<SlideRuntime> slides_;
    std::vector<StackRuntime> stacks_;
    std::vector<CarouselRuntime> carousels_;
    std::vector<std::string> problems_;
    std::set<std::string, std::less<>> isolated_;   // 動いている部屋（境界でどこともつながない）
    std::set<std::pair<std::string, std::string>> sealed_;   // 乗り物で降りて閉じた境（名前の小さい方が先）
    std::vector<RuleEvent> mechanismEvents_;   // 機構の側で起きた出来事（乗り物が着いて有効にした検査点）。次の Record で出す
    std::vector<std::pair<std::string, std::string>> pendingSeals_;   // 降りかけている（乗り物の部屋, 降りた部屋）。出きったら閉じる

    std::map<std::string, RegionRuntime> regions_;
    std::map<std::string, std::string> deviceOwners_;
    std::vector<RegionPlacement> placements_;
    WorldDefinition world_;
    WorldState state_;
    WorldSnapshot snapshot_;
    bool snapshotDirty_ = true;
    std::string activeRegion_;
    std::vector<ExternalCall> calls_;
};
}
