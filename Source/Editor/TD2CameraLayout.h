#pragma once

#include "TD2EditorSupport.h"
#include "../Common/TD2WorldLayout.h"
#include <array>
#include <map>
#include <string>
#include <vector>

namespace TD2::Editor {
// 編集器の中で、場景の「今の」配置から区域机位と大区域どうしの切り替え演出を作る。
// 作者が編集器で電視を動かせば、その配置から作り直せる（外の脚本や表に頼らない）。作者が手で調整した机位・演出は
// 自動の記録と違うことで見分け、作り直しても上書きしない（「保留」を外すか「恢复自动」を押した時だけ作り直す）。
using Vec3 = std::array<double, 3>;

struct LayoutTransform { Vec3 position{}, rotation{}, scale{1, 1, 1}; };   // rotation は度（引擎の X→Y→Z の順に回す）

struct LayoutRoom {
    uint64_t objectId = 0, componentId = 0;
    std::string regionId, name;
    LayoutTransform tv;
    bool knownModel = false;                                  // 電視の型が表にある（画面の寸法を自動で決められる）
    double body[3]{1.1, .86, .7};                              // 筐体の幅・高さ・奥行き（縮尺前）
    double screenCenter[3]{0, .4, .28}, screenWidth = .84, screenHeight = .49;
    uint64_t cameraId = 0;
    LayoutTransform camera;
    bool cameraLocked = false;                                // 机位の物体が編集器で鎖定されている
    std::string cameraAuto;                                   // 最後に自動で置いた机位（空は作者が置いた机位）
    bool placed = false;
    int worldX = 0, worldY = 0, cols = 1, rows = 1;
    std::string cells;
};

struct LayoutArea {
    uint64_t objectId = 0, componentId = 0;
    std::string name;
    std::vector<std::string> regions;
    double frameFill = .35, orbitDegrees = 15, cameraLift = .3;
};

struct LayoutEdge {
    std::string a, b;                                         // 区域 ID（a は FindSharedWorldEdge の A 側）
    SharedWorldEdge edge;
    bool between = false;                                     // 大区域どうし（演出が流れる辺）
    bool passable = false;                                    // 共有する辺の上で両側の格が空いている（実際に越えられる）
    uint64_t bindingObject = 0, bindingComponent = 0;
    // autoHash：自動で書いた時の演出の鍵の記録（欄の名前は旧版のまま。中身は KeySignature の文字列）。空は手做りの演出。
    std::string sequenceGuid, style = "auto", autoHash;
    double seconds = 3, speed = 1;
    bool keepAuthored = false;
};

struct LayoutBox { Vec3 lo{}, hi{}; uint64_t objectId = 0; };

struct LayoutSnapshot {
    std::string sceneId;
    uint64_t revision = 0;
    std::vector<LayoutRoom> rooms;
    std::vector<LayoutArea> areas;
    std::vector<LayoutEdge> edges;
    std::vector<LayoutBox> obstacles;                         // 机位と演出の道筋が入ってはいけない物（電視以外）
    bool hasHall = false;
    Vec3 hallLo{}, hallHi{};                                  // 大厅の内側（床の範囲・天井の下）
    std::map<uint64_t, LayoutTransform> transforms;           // scene.layout.apply の expected 用
    std::map<uint64_t, Json> transformJson;                   // 問い合わせたままの変換（expected は丸めずにこれを渡す）
    const LayoutRoom* Room(std::string_view regionId) const;
    const LayoutArea* AreaOf(std::string_view regionId) const;
};

// 場景を読み直す（電視・机位・大区域・交界・障碍物）。
bool LoadLayout(EditorWorkspaceHostContext&, LayoutSnapshot&, std::string& error);

struct CameraPose { Vec3 eye{}, look{}; };
// 区域机位（作者の基準の構図）：画面を出口の側から少し斜めに、画面の高さが画の frameFill になる距離で撮る。
CameraPose RegionCamera(const LayoutSnapshot&, const LayoutRoom&);
// 机位の姿勢（位置と回転）を記録の文字列へ。今の机位がこれと違えば作者が調整したとみなす。
std::string CameraMemo(const LayoutTransform&);
// 今の机位の物体の姿勢（局所 +Z を見る。注視点は画面の中心の深さ）。
CameraPose CurrentCameraPose(const LayoutRoom&);
bool CameraEdited(const LayoutRoom&);

// 切り替えの道筋（時刻 0〜1、目・注視点・画角（度）・傾き（度））。
struct TravelKey { double t = 0; Vec3 eye{}, look{}; double fov = 26, roll = 0; };
struct TravelPlan { std::vector<TravelKey> keys; std::string style; double worstShare = 0; std::string problem; };
// 生成できる様式（id と表示名）。auto は二台の向きと高さから選ぶ。
struct TravelStyle { std::string_view id, label, hint; };
const std::vector<TravelStyle>& TravelStyles();
std::string ResolveStyle(const LayoutSnapshot&, const LayoutEdge&);
TravelPlan PlanTravel(const LayoutSnapshot&, const LayoutEdge&, const std::map<std::string, CameraPose, std::less<>>& cameras);
// 道筋を時刻で辿る（引擎の spline 補間と同じ Hermite）。
TravelKey SampleTravel(const std::vector<TravelKey>&, double t);

// 作り直し。areaName が空なら全部の大区域。force は作者の調整も上書きする（「恢复自动」）。
struct GenerateReport { int written = 0, kept = 0, failed = 0; std::vector<std::string> notes; };
bool GenerateCameras(EditorWorkspaceHostContext&, LayoutSnapshot&, std::string_view areaName, std::string_view onlyRegion,
                     bool force, GenerateReport&, std::string& error);
bool GenerateTravels(EditorWorkspaceHostContext&, LayoutSnapshot&, std::string_view areaName, const LayoutEdge* onlyEdge,
                     bool force, GenerateReport&, std::string& error);
// 画面の寸法を電視の型から区域の属性へ書く（型が表に無い区域は触らない）。
bool SyncScreenGeometry(EditorWorkspaceHostContext&, const LayoutSnapshot&, GenerateReport&, std::string& error);
// 大区域の区域 ID の列を書く（区域を大区域へ入れる・外す）。
bool WriteAreaRegions(EditorWorkspaceHostContext&, const LayoutArea&, const std::vector<std::string>& regions, std::string& error);
bool CreateArea(EditorWorkspaceHostContext&, const std::string& name, const std::vector<std::string>& regions, std::string& error);
// 大区域の中の辺に残った演出の結びを外す（実行時はもう使わない）。
bool PruneInAreaBindings(EditorWorkspaceHostContext&, const LayoutSnapshot&, int& removed, std::string& error);
// 辺の生成設定（様式・秒数・保留）を書く。秒数は実行時の倍速へも写す。
bool WriteEdgeSettings(EditorWorkspaceHostContext&, const LayoutEdge&, std::string& error);
// その辺を歩いて越える試遊の出生点（A 側の区域内の格、左上基準）。
bool EdgeTrialSpawn(const LayoutSnapshot&, const LayoutEdge&, bool fromA, double& x, double& y, uint64_t& roomObject);
} // namespace TD2::Editor
