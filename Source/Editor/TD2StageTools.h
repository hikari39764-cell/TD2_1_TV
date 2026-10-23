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
    std::string name;
    Pose pose;                              // シーンに書いた姿勢（親の座標）
    bool hasBounds = false;
    std::array<double, 3> lo{}, hi{};       // 世界の外接箱
    const TelevisionModel* television = nullptr;
};
bool ReadStageObject(EditorWorkspaceHostContext&, uint64_t objectId, StageObject&, std::string& error);
// 幾つもの物体を一回の問い合わせで読む（見つからない物は out に入らない）。
bool ReadStageObjects(EditorWorkspaceHostContext&, const std::vector<uint64_t>& objectIds, std::map<uint64_t, StageObject>& out, std::string& error);

// 置ける道具。group は棚の分け方（0 台・1 仕掛けの部品・2 棚・3 灯・4 小物）。stand はテレビの下に敷ける台（テレビをその上へ載せる）。
struct StageProp { std::string_view label, path; bool stand; int group; std::array<double, 3> scale{1, 1, 1}; };
inline constexpr std::array<std::string_view, 5> kStagePropGroups{"台", "仕掛けの部品", "棚", "灯", "小物"};
inline constexpr StageProp kStageProps[]{
    {"鉄の台", "TD2Props/SM_TD2_Pedestal.gltf", true, 0},
    {"テレビ台", "TD2Props/SM_TD2_TVStand.gltf", true, 0},
    {"パレットの山", "TD2Props/SM_TD2_PalletStack.gltf", true, 0},
    {"AV カート", "TD2Props/SM_TD2_AVCart.gltf", true, 0},
    {"机", "FactorySet01/SM_F_Desk.fbx", true, 0},
    {"段ボール", "FactorySet01/SM_F_Box_A.fbx", true, 0},
    {"木箱の山", "FactorySet01/SM_F_Crate_Stack.fbx", true, 0},
    {"パレット", "FactorySet01/SM_F_Pallet.fbx", true, 0},
    {"吊りチェーン", "FactorySet01/SM_F_Chain_1m.fbx", false, 1, {1, 2, 1}},
    {"巻き上げ箱", "TD2Props/SM_TD2_BalanceBeam.gltf", false, 1, {.45, 1.2, 1}},
    {"天秤の梁", "TD2Props/SM_TD2_BalanceBeam.gltf", false, 1, {2.1, .42, .9}},
    {"転台の土台", "TD2Props/SM_TD2_Turntable_Base.gltf", true, 1},
    {"転台の盤", "TD2Props/SM_TD2_Turntable_Plate.gltf", false, 1},
    {"レール", "TD2Props/SM_TD2_Rail.gltf", false, 1},
    {"逆さ転台の盤", "TD2Props/SM_TD2_RollDisk.gltf", false, 1},
    {"逆さ転台の支柱", "TD2Props/SM_TD2_RollMount.gltf", false, 1},
    {"テレビの塔の枠", "TD2Props/SM_TD2_StackFrame.gltf", false, 1},
    {"高いテレビの塔の枠", "TD2Props/SM_TD2_StackFrameRaised.gltf", false, 1},
    {"回転柱の土台", "TD2Props/SM_TD2_CarouselBase.gltf", false, 1},
    {"回転柱", "TD2Props/SM_TD2_CarouselColumn.gltf", false, 1},
    {"棚", "FactorySet01/SM_F_Rack_Bay.fbx", false, 2},
    {"棚の端枠", "FactorySet01/SM_F_Rack_EndFrame.fbx", false, 2},
    {"吊り灯", "FactorySet01/SM_F_Lamp_Pendant.fbx", false, 3},
    {"作業灯", "FactorySet01/SM_F_Lamp_WorkTripod.fbx", false, 3},
    {"電気スタンド", "FactorySet01/SM_F_Lamp_Desk.fbx", false, 3},
    {"椅子", "FactorySet01/SM_F_Chair_Office.fbx", false, 4},
    {"ケーブルリール", "FactorySet01/SM_F_CableReel.fbx", false, 4},
    {"段ボール B", "FactorySet01/SM_F_Box_B.fbx", false, 4},
    {"段ボール C", "FactorySet01/SM_F_Box_C.fbx", false, 4},
};

// モデルを置いて物体 ID を返す（失敗は 0）。
uint64_t CreateStageModel(EditorWorkspaceHostContext&, std::string_view path, std::string_view name, const Pose&, std::string& error);
bool SetStagePose(EditorWorkspaceHostContext&, uint64_t objectId, const Pose&, std::string& error);
// like と同じシーンのフォルダーへ入れる（like がフォルダーに無ければ何もしない）。
void JoinFolderOf(EditorWorkspaceHostContext&, uint64_t objectId, uint64_t like);
// 編集カメラの注視点の床に、カメラの方を向けて置く姿勢。
bool PoseAtView(EditorWorkspaceHostContext&, Pose&, std::string& error);
}
