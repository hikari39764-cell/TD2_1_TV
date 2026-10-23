#pragma once
#include "TD2EditorSupport.h"
#include "../Common/TD2WorldLayout.h"
namespace TD2::Editor {
// 世界地図は対応関係だけを渡す。区域カメラは GameplayCamera、演出は Cinematics で編集・保存する。
bool OpenTransitionCamera(EditorWorkspaceHostContext&, const RegionSequenceBinding&, std::string& error);
}
