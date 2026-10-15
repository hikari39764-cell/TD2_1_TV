#pragma once
#include "TD2EditorSupport.h"
#include "../Common/TD2WorldLayout.h"
namespace TD2::Editor {
// 世界地図は関係だけを渡す。区域機位は GameplayCamera、時間演出は Cinematics が編集・保存を所有する。
bool OpenTransitionCamera(EditorWorkspaceHostContext&, const RegionSequenceBinding&, std::string& error);
bool OpenRegionCamera(EditorWorkspaceHostContext&, const std::string& region, const std::string& name,
                      std::string& error);
} // namespace TD2::Editor
