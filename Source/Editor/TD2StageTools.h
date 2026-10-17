#pragma once
#include "TD2MachineEditor.h"
#include "../Common/TD2TelevisionModels.h"
#include <array>
#include <string>

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

// 置ける道具。stand はテレビの下に敷ける台（テレビをその上へ載せる）。
struct StageProp { std::string_view label, path; bool stand; };
inline constexpr StageProp kStageProps[]{
    {"钢台座", "TD2Props/SM_TD2_Pedestal.gltf", true},
    {"电视台", "TD2Props/SM_TD2_TVStand.gltf", true},
    {"托盘堆", "TD2Props/SM_TD2_PalletStack.gltf", true},
    {"影音推车", "TD2Props/SM_TD2_AVCart.gltf", true},
    {"书桌", "FactorySet01/SM_F_Desk.fbx", true},
    {"纸箱", "FactorySet01/SM_F_Box_A.fbx", true},
    {"箱堆", "FactorySet01/SM_F_Crate_Stack.fbx", true},
    {"托盘", "FactorySet01/SM_F_Pallet.fbx", true},
    {"货架", "FactorySet01/SM_F_Rack_Bay.fbx", false},
    {"吊灯", "FactorySet01/SM_F_Lamp_Pendant.fbx", false},
    {"工作灯", "FactorySet01/SM_F_Lamp_WorkTripod.fbx", false},
    {"椅子", "FactorySet01/SM_F_Chair_Office.fbx", false},
    {"线缆卷", "FactorySet01/SM_F_CableReel.fbx", false},
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
