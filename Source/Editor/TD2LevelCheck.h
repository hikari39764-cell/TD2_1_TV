#pragma once
// レベルの検査：作者がよく見落とすこと（つながっていない圧力板、光路の上のチェックポイント、大区域に入っていないテレビなど）を地図の上に示す。
#include "TD2CameraLayout.h"
#include "TD2MachineEditor.h"

namespace TD2::Editor {
enum class IssueLevel { Error, Warning };
struct LevelIssue {
    IssueLevel level = IssueLevel::Warning;
    std::string text, regionId;
    double x = -1, y = -1;     // 区域の中の位置（マス、左上基準）。負なら区域全体
};
struct CheckRoom { RegionSettings settings; std::string name; };
std::vector<LevelIssue> CheckLevel(const std::vector<CheckRoom>& rooms, const std::vector<Machine>& machines, const LayoutSnapshot* layout);
}
