#pragma once
#include "TD2EditorSupport.h"
#include "../Common/TD2MachineText.h"
#include "../Common/TD2WorldLayout.h"
#include <array>
#include <map>
#include <string>
#include <vector>

namespace TD2::Editor {
// 画面の外の仕掛け（機構・スロット棚・テレビの塔・回転柱）の作者データ。世界地図の仕掛けの見方で作り、直し、確かめる。
// 書き方は実行時の部品と同じ文字列（TD2MachineText.h）。位置は作者が置いた今の配置から求める相対の形だけを書く。
enum class MachineKind { Mechanism, Slide, Stack, Carousel };

struct Machine {
    MachineKind kind = MachineKind::Mechanism;
    uint64_t objectId = 0, componentId = 0;
    std::string objectName, id;
    // 機構
    bool toggle = false, balance = false;
    double seconds = .8, dwell = 0, settle = 0, cameraLift = 1.5, cameraPull = 1;
    std::vector<MachineRegionOffset> offsets, offsetsC;
    std::vector<MachineObjectDelta> deltas, deltasC;
    // 編集器の配方と、配方が使う部品（物体・付いているテレビの区域・役目・一緒に動くか・配方が置いた物か）。
    std::string recipe;
    struct Part {
        uint64_t object = 0; std::string region, role; bool moves = false, owned = true;
        bool operator==(const Part&) const = default;
    };
    std::vector<Part> parts;
    // スロット棚（メンバー）・テレビの塔（下から上）・回転柱（時計回りの面）
    std::vector<std::string> members;
    std::vector<std::pair<int, int>> emptySlots;   // スロット棚：一番目のメンバーから見たずれ
    double pullSeconds = .8, dropSeconds = .35, resetSeconds = 2;
    std::vector<double> dockDistances{2.4, 1.25};
    std::string extraObjects;                  // 回転柱：柱のほかに一緒に回す物体 ID

    // 圧力板の目標として選べるもの（目標の文字列、表示名）。
    std::vector<std::pair<std::string, std::string>> Receivers() const;
    // 世界地図で強調する区域。
    std::vector<std::string> Regions() const;
    std::string KindLabel() const;
    bool operator==(const Machine&) const = default;
};

std::string_view MachineComponentType(MachineKind);
std::string FormatParts(const std::vector<Machine::Part>&);
std::vector<Machine::Part> ParseParts(std::string_view text);
bool LoadMachines(EditorWorkspaceHostContext&, std::vector<Machine>&, std::string& error);
// 変わった欄だけを一回の取り消しにまとめて書く。
bool WriteMachine(EditorWorkspaceHostContext&, const Machine& before, const Machine& after, std::string& error);
// host が 0 なら空の物体を作ってそこへ付ける（回転柱は柱の物体を選んでから作る）。
bool CreateMachine(EditorWorkspaceHostContext&, MachineKind, uint64_t host, const std::string& id, std::string& error);
bool RemoveMachine(EditorWorkspaceHostContext&, const Machine&, std::string& error);

// 実行時に動くかの確かめ。placements は世界地図の配置、plateTargets は全部の圧力板の目標（区域ID/装置ID → 目標）。
struct MachineCheckInput {
    std::vector<RegionPlacement> placements;
    std::map<std::string, std::vector<std::string>> plateTargets;   // "区域ID/装置ID" → 目標
    std::vector<uint64_t> sceneObjects;                              // シーンにある物体 ID
    std::map<uint64_t, bool> objectHasModel;
};
std::vector<std::string> CheckMachine(const Machine&, const std::vector<Machine>& all, const MachineCheckInput&);
// この仕掛けを目標にしている圧力板（"区域ID/装置ID"）。
std::vector<std::string> PlatesFor(const Machine&, const MachineCheckInput&);

// ---- 姿勢（編集器で状態 B を置いてみる時に使う。度、エンジンの順 M = Rz·Ry·Rx）
struct Pose { std::array<double, 3> position{}, rotation{}, scale{1, 1, 1}; };
// base に相対の姿勢を載せる（位置は base の向きで測ったずれ）。pivot があればその点を中心に回した位置。
Pose ApplyDelta(const Pose& a, const Pose& base, const MachineObjectDelta& d);
// a（状態 A）・base（基準）・b（置いてみた状態 B）から相対の姿勢を作る。pivot は保つ。
MachineObjectDelta MakeDelta(const Pose& a, const Pose& base, const Pose& b, const MachineObjectDelta& previous);
}
