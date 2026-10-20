#pragma once
#include "TD2MachineEditor.h"
#include "../Common/TD2TelevisionModels.h"
#include <array>
#include <map>
#include <string>
#include <vector>

namespace TD2::Editor {
// 3D の舞台の道具：シーンの物体を読み、置き、並べる。レベル編集の下の欄（3D の舞台）が使う。
struct StageObject {
    uint64_t objectId = 0, parentId = 0;
    std::string name, source;              // source はモデルの元パス
    Pose pose;                              // シーンに書いた姿勢（親の座標）
    bool hasBounds = false;
    std::array<double, 3> lo{}, hi{};       // 世界の外接箱
    const TelevisionModel* television = nullptr;
};
bool ReadStageObject(EditorWorkspaceHostContext&, uint64_t objectId, StageObject&, std::string& error);
// 幾つもの物体を一回の問い合わせで読む（見つからない物は out に入らない）。
bool ReadStageObjects(EditorWorkspaceHostContext&, const std::vector<uint64_t>& objectIds, std::map<uint64_t, StageObject>& out, std::string& error);

// 置ける道具。group は棚の分け方（0 台・1 机构件・2 货架・3 灯・4 杂物）。stand はテレビの下に敷ける台（テレビをその上へ載せる）。
struct StageProp { std::string_view label, path; bool stand; int group; std::array<double, 3> scale{1, 1, 1}; };
inline constexpr std::array<std::string_view, 5> kStagePropGroups{"台", "机构件", "货架", "灯", "杂物"};
inline constexpr StageProp kStageProps[]{
    {"钢台座", "TD2Props/SM_TD2_Pedestal.gltf", true, 0},
    {"电视台", "TD2Props/SM_TD2_TVStand.gltf", true, 0},
    {"托盘堆", "TD2Props/SM_TD2_PalletStack.gltf", true, 0},
    {"影音推车", "TD2Props/SM_TD2_AVCart.gltf", true, 0},
    {"书桌", "FactorySet01/SM_F_Desk.fbx", true, 0},
    {"纸箱", "FactorySet01/SM_F_Box_A.fbx", true, 0},
    {"箱堆", "FactorySet01/SM_F_Crate_Stack.fbx", true, 0},
    {"托盘", "FactorySet01/SM_F_Pallet.fbx", true, 0},
    {"吊链", "FactorySet01/SM_F_Chain_1m.fbx", false, 1, {1, 2, 1}},
    {"卷扬箱", "TD2Props/SM_TD2_BalanceBeam.gltf", false, 1, {.45, 1.2, 1}},
    {"天平梁", "TD2Props/SM_TD2_BalanceBeam.gltf", false, 1, {2.1, .42, .9}},
    {"转台底座", "TD2Props/SM_TD2_Turntable_Base.gltf", true, 1},
    {"转台转盘", "TD2Props/SM_TD2_Turntable_Plate.gltf", false, 1},
    {"滑轨", "TD2Props/SM_TD2_Rail.gltf", false, 1},
    {"倒立转盘", "TD2Props/SM_TD2_RollDisk.gltf", false, 1},
    {"倒立支柱", "TD2Props/SM_TD2_RollMount.gltf", false, 1},
    {"电视塔框", "TD2Props/SM_TD2_StackFrame.gltf", false, 1},
    {"加高电视塔框", "TD2Props/SM_TD2_StackFrameRaised.gltf", false, 1},
    {"转柱底座", "TD2Props/SM_TD2_CarouselBase.gltf", false, 1},
    {"转柱", "TD2Props/SM_TD2_CarouselColumn.gltf", false, 1},
    {"货架", "FactorySet01/SM_F_Rack_Bay.fbx", false, 2},
    {"货架端框", "FactorySet01/SM_F_Rack_EndFrame.fbx", false, 2},
    {"吊灯", "FactorySet01/SM_F_Lamp_Pendant.fbx", false, 3},
    {"工作灯", "FactorySet01/SM_F_Lamp_WorkTripod.fbx", false, 3},
    {"台灯", "FactorySet01/SM_F_Lamp_Desk.fbx", false, 3},
    {"椅子", "FactorySet01/SM_F_Chair_Office.fbx", false, 4},
    {"线缆卷", "FactorySet01/SM_F_CableReel.fbx", false, 4},
    {"纸箱 B", "FactorySet01/SM_F_Box_B.fbx", false, 4},
    {"纸箱 C", "FactorySet01/SM_F_Box_C.fbx", false, 4},
};

// モデルを置いて物体 ID を返す（失敗は 0）。
uint64_t CreateStageModel(EditorWorkspaceHostContext&, std::string_view path, std::string_view name, const Pose&, std::string& error);
bool SetStagePose(EditorWorkspaceHostContext&, uint64_t objectId, const Pose&, std::string& error);
// like と同じシーンのフォルダーへ入れる（like がフォルダーに無ければ何もしない）。
void JoinFolderOf(EditorWorkspaceHostContext&, uint64_t objectId, uint64_t like);
// 筐体の大きさ（縮尺込み、m）。表にあるテレビは表の寸法、それ以外は外接箱。
double StageWidth(const StageObject&);
double StageHeight(const StageObject&);
// anchor の隣に置く姿勢。向きは anchor と同じ。side：1 右・-1 左（anchor の X 方向）、2 上・-2 下。
Pose PoseBeside(const StageObject& anchor, double width, double height, int side, double gap = .12);
// 編集カメラの注視点の床に、カメラの方を向けて置く姿勢。
bool PoseAtView(EditorWorkspaceHostContext&, Pose&, std::string& error);
}
