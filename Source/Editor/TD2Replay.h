#pragma once
// 解法の再生：試遊の記録を、編集器が持っている今の作者データと同じ規則で進め直し、まだ通れるかを確かめる。
#include "TD2EditorSupport.h"
#include "TD2MachineEditor.h"
#include "../Common/TD2PlayRecord.h"
#include "../Runtime/Rules/TD2Exploration.h"
#include <filesystem>

namespace TD2::Editor {
struct ReplayRoom { RegionSettings settings; std::string name; };
struct SolutionResult {
    std::string name, file;
    bool passed = false;
    std::string detail;        // 通れなかった理由（何秒にどの区域で）
    std::string regionId;      // 問題の起きた区域
    std::vector<std::pair<double, double>> path;   // 再生したプレイヤーの世界の位置（地図に線で描く）
};
// 作者データから規則の世界を作る（実行時の MachineViews と同じ置き方）。
std::string BuildExploration(const std::vector<ReplayRoom>& rooms, const std::vector<Machine>& machines, const PlayRecord& record, Rules::Exploration& out);
SolutionResult ReplaySolution(const std::vector<ReplayRoom>& rooms, const std::vector<Machine>& machines, const PlayRecord& record);
// 解法の置き場（プロジェクトの設定の下、リポジトリに入る）。
std::filesystem::path SolutionFolder(std::string_view projectRoot);
}
