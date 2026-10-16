#pragma once

#include "TD2Rules.h"
#include "../../Common/TD2WorldLayout.h"
#include <map>
#include <set>

namespace TD2::Rules {
struct RegionSource { WorldDefinition definition; RegionPlacement placement; bool entry = false; };
// 各画面は作者の定義と配置だけを持つ。物理状態は世界に一つ。
struct RegionRuntime {
    WorldDefinition definition;
    RegionPlacement placement;
    bool flipped = false;   // 機構で 180° 回され、地図が上下左右に入れ替わっている（definition は作者のまま）
};
// 画面の外の機構（転台・吊り台・天平など）。状態 0 は作者の配置、状態 1 は moves、状態 -1 は movesC の位置。
// 圧力板の通電で切り替え、動いている間は動く部屋をどこともつながない（中の時間は進む）。
// flip は部屋を 180° 回した向き（地図の上下左右が入れ替わる）。
struct MechanismMove { std::string regionId; int worldX = 0, worldY = 0; bool flip = false; };
struct MechanismSource {
    std::string id;
    bool toggle = false;      // false：踏んでいる間だけ状態 1（離すと戻る）、true：踏むたびに切り替え
    bool balance = false;     // 天平：受け口は mech/ID と mech/ID/r。片方だけ踏まれるとその側へ傾き（1 / -1）、両方か無しで 0
    double seconds = 1.5;     // 状態の間を動く秒数
    double dwell = 0.0;       // 0 より大きければ圧力板に関係なく自動で往復し、両端でこの秒数だけ止まる
    double settle = 0.0;      // 踏み続け式・天平：踏まれ方がこの秒数変わらなかった時だけ動き出す（走り抜けでは動かない）
    std::vector<MechanismMove> moves;    // 状態 1
    std::vector<MechanismMove> movesC;   // 状態 -1（天平だけ）
};
struct MechanismRuntime {
    MechanismSource source;
    std::vector<MechanismMove> origins;   // 状態 0 の位置（moves と movesC の部屋すべて）
    size_t device = 0, deviceR = 0;       // 圧力板の受け口（世界の装置の番号。deviceR は天平の右）
    int target = 0, state = 0;            // 向かっている状態と確定した状態
    bool powered = false, moving = false;
    double progress = 0.0;                // 今の状態（-1〜1。動いている間は途中の値）
    double rest = 0.0;                    // 自動往復で、端に着いてから止まっている秒数
    int pending = 0; double pendingTime = 0.0;   // settle の間、待っている次の状態と経過秒
};
// 槽の並んだ棚。メンバーの部屋の圧力板（targets=slide/区域ID）を踏むと、その部屋が隣の空いた槽へ滑る。
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
    std::vector<int> slotOf;          // メンバーごとの今の槽
    std::vector<size_t> devices;      // メンバーごとの受け口（slide/区域ID）
    std::vector<char> powered;
    int moving = -1, from = -1, to = -1;
    double progress = 0.0;
    int blocked = -1;                 // 最後に動けなかったメンバー
    uint64_t blockedCount = 0;
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
                           std::vector<SlideSource> slides = {}, std::string_view entryOverride = {}, std::optional<Vec2> spawnOverride = {});
    const std::vector<MechanismRuntime>& Mechanisms() const noexcept { return mechanisms_; }
    const std::vector<SlideRuntime>& Slides() const noexcept { return slides_; }
    ExplorationOutput Step(const InputState& input, double seconds);
    const RegionRuntime* FindRegion(std::string_view id) const;
    const std::map<std::string, RegionRuntime>& Regions() const noexcept { return regions_; }
    const std::vector<RegionPlacement>& Placements() const noexcept { return placements_; }
    const WorldDefinition& Definition() const noexcept { return world_; }
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
private:
    void BuildBoundaries();
    void Record(StepOutput output, ExplorationOutput& out);
    std::string RegionAt(const Rect& body) const;
    void UpdateMechanisms(double seconds);
    void UpdateSlides(double seconds);
    std::vector<MechanismMove> TargetsFor(const MechanismRuntime& mechanism, int state) const;
    // 部屋をまとめて動かす（重なるなら何もしない）。中の体・装置・記録も一緒に運び、flip が変われば 180° 回す。
    bool MoveRegions(const std::vector<MechanismMove>& targets);

    std::vector<MechanismRuntime> mechanisms_;
    std::vector<SlideRuntime> slides_;
    std::set<std::string, std::less<>> isolated_;   // 動いている部屋（境界でどこともつながない）

    std::map<std::string, RegionRuntime> regions_;
    std::map<std::string, std::string> deviceOwners_;
    std::vector<RegionPlacement> placements_;
    WorldDefinition world_;
    WorldState state_;
    WorldSnapshot snapshot_;
    bool snapshotDirty_ = true;
    std::string activeRegion_;
};
}
