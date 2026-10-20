#include "TD2LevelEditor.h"
#include <chrono>
#include <fstream>

namespace TD2::Editor::Level {
namespace {
std::vector<ReplayRoom> ReplayRooms(const std::vector<RoomItem>& rooms, uint64_t selected, bool ready, const RegionSettings& draft) {
    std::vector<ReplayRoom> out;
    for (const auto& room : rooms) { out.push_back({room.object.objectId == selected && ready ? draft : room.settings, room.object.name}); }
    return out;
}
}

// 試遊：押した位置から始める。試遊の初めの仕掛けの状態は一時ファイルでゲームへ渡す。
void LevelEditor::StartPreview(EditorWorkspaceHostContext& ctx, uint64_t room, PreviewSpawn spawn) {
    if (!model_.Commit(ctx)) { return; }
    {
        std::ofstream out(PreviewStatesPath(), std::ios::trunc);
        for (const auto& [id, state] : previewStates_) { if (state != 0) { out << id << ' ' << state << '\n'; } }
    }
    const auto started = Execute(ctx, "play.preview", {{"spawnPointId", EncodePreviewSpawn(spawn)}, {"userData", room}});
    if (started.ok) { pickingStart_ = false; model_.error.clear(); }
    else {
        model_.error = started.error.find("SCENE_NOT_SAVED") != std::string::npos ? "先保存场景再试玩" : started.error;
    }
}

// 試遊を選んでいる間の左の欄：始める時の仕掛けの状態（切り替え式と自動往復だけ）。
void LevelEditor::DrawPlayInspector(EditorWorkspaceHostContext& ctx) {
    auto& ui = *ctx.ui;
    ui.Text("点地图上的位置开始试玩 · 右键取消");
    RefreshMachines(ctx);
    bool any = false;
    for (const auto& m : model_.machines) {
        if (m.kind != MachineKind::Mechanism || (!m.toggle && m.dwell <= 0)) { continue; }
        if (!any) { ui.Separator(); ui.Text("开始时机构的状态"); any = true; }
        int& state = previewStates_[m.id];
        ui.Text(RecipeLabel(m) + " " + ShortChoiceLabel(m.id, 10));
        ui.SameLine(); if (ui.Button("td2.play.state.a." + m.id, state == 0 ? "● A" : "A")) { state = 0; }
        ui.SameLine(); if (ui.Button("td2.play.state.b." + m.id, state == 1 ? "● B" : "B")) { state = 1; }
        if (m.balance) { ui.SameLine(); if (ui.Button("td2.play.state.c." + m.id, state == -1 ? "● C" : "C")) { state = -1; } }
    }
    if (ui.Button("td2.play.cancel", "取消")) { pickingStart_ = false; }
}

// 試遊が終わると記録が一時ディレクトリに書かれる。新しい記録が来たら、解法として残すか尋ねる。
void LevelEditor::PollRecording(EditorWorkspaceHostContext& ctx) {
    recordingClock_ += ctx.unscaledDeltaSeconds;
    if (recordingClock_ < 1) { return; }
    recordingClock_ = 0;
    std::error_code error;
    const auto path = LastPlayPath();
    if (!std::filesystem::exists(path, error)) { return; }
    const auto time = std::filesystem::last_write_time(path, error);
    if (error) { return; }
    if (!recordingSeen_) { recordingTime_ = time; recordingSeen_ = true; return; }   // 開いた時に残っていた古い記録は出さない
    if (time == recordingTime_) { return; }
    recordingTime_ = time;
    lastRecord_ = ReadPlayRecord(path);
}

void LevelEditor::RunSolutions(EditorWorkspaceHostContext& ctx) {
    if (!model_.Commit(ctx)) { return; }
    RefreshMachines(ctx);
    solutions_.clear();
    std::error_code error;
    const auto folder = SolutionFolder(ctx.projectRootUtf8 ? ctx.projectRootUtf8 : "");
    if (!std::filesystem::exists(folder, error)) { SetNote("还没有解法：试玩后在左栏「存为解法」"); return; }
    const auto rooms = ReplayRooms(model_.rooms, model_.selected, model_.ready, model_.draft);
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(folder, error)) { if (entry.path().extension() == ".td2play") { files.push_back(entry.path()); } }
    std::sort(files.begin(), files.end());
    for (const auto& file : files) {
        const auto record = ReadPlayRecord(file);
        SolutionResult result;
        if (record) { result = ReplaySolution(rooms, model_.machines, *record); }
        else { result.detail = "读不了"; }
        result.file = file.u8string().empty() ? std::string{} : std::string(reinterpret_cast<const char*>(file.u8string().c_str()));
        // 名前：始めた区域 → 最後の区域。
        const auto stem = file.stem().u8string();
        result.name = std::string(reinterpret_cast<const char*>(stem.c_str()));
        if (record) {
            const auto nameOf = [this](const std::string& id) { return RoomName(id); };
            result.name = ShortChoiceLabel(nameOf(record->entryRegion), 8) + " → " + ShortChoiceLabel(nameOf(record->endRegion), 8);
        }
        solutions_.push_back(std::move(result));
    }
    solutionsStale_ = false; solutionsRevision_ = model_.revision;
    size_t passed = 0;
    for (const auto& s : solutions_) { passed += s.passed ? 1 : 0; }
    SetNote("解法回放：" + std::to_string(passed) + " / " + std::to_string(solutions_.size()) + " 通过");
}

void LevelEditor::RefreshCheck(EditorWorkspaceHostContext& ctx) {
    checkClock_ += ctx.unscaledDeltaSeconds;
    if (checkClock_ < 2.5f || model_.gesture || model_.rooms.empty()) { return; }
    checkClock_ = 0;
    RefreshMachines(ctx);
    std::vector<CheckRoom> rooms;
    for (const auto& room : model_.rooms) { rooms.push_back({SettingsOf(room), room.object.name}); }
    issues_ = CheckLevel(rooms, model_.machines, layoutValid_ ? &layout_ : nullptr);
    if (issueIndex_ >= issues_.size()) { issueIndex_ = 0; }
}

void LevelEditor::JumpToIssue(EditorWorkspaceHostContext& ctx, size_t index) {
    if (index >= issues_.size()) { return; }
    const auto issue = issues_[index];
    if (issue.regionId.empty()) { return; }
    for (const auto& room : model_.rooms) {
        if (room.settings.region.regionId != issue.regionId) { continue; }
        const auto id = room.object.objectId;
        const auto placed = room.settings;
        if (!SelectRoom(ctx, id)) { return; }
        model_.devices.Select(-1); focus_ = Focus::Room;
        if (placed.worldPlaced) { FitRoom(placed); }
        return;
    }
}

// 刚才的试玩を解法として残す（プロジェクトの設定の下、リポジトリに入る）。
void LevelEditor::SaveRecording(EditorWorkspaceHostContext& ctx) {
    if (!lastRecord_) { return; }
    std::error_code error;
    const auto folder = SolutionFolder(ctx.projectRootUtf8 ? ctx.projectRootUtf8 : "");
    std::filesystem::create_directories(folder, error);
    const auto stamp = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    const auto file = folder / ("solution_" + std::to_string(stamp) + ".td2play");
    if (!WritePlayRecord(file, *lastRecord_)) { model_.error = "解法写入失败"; return; }
    lastRecord_.reset();
    RunSolutions(ctx);
}

// 問題の印：区域の中の位置（無ければ区域の左上）に小さな丸。重いものは赤、注意は琥珀。
void LevelEditor::DrawIssues(IEditorWorkspaceUi& ui) {
    if (tool_ == Tool::Camera && plan_) { return; }
    std::map<std::string, int, std::less<>> perRoom;
    for (const auto& issue : issues_) {
        if (issue.regionId.empty()) { continue; }
        const auto p = PlacementOf(issue.regionId);
        if (!p || !p->worldPlaced) { continue; }
        const int k = perRoom[issue.regionId]++;
        const float x = issue.x >= 0 ? static_cast<float>(p->worldX + issue.x + .5) : static_cast<float>(p->worldX) + 1.2f + k * 1.4f;
        const float y = issue.y >= 0 ? static_cast<float>(p->worldY + issue.y + .5) : static_cast<float>(p->worldY + p->rows) - 1.0f;
        const auto at = view_.Screen(x, y);
        const EditorWorkspaceColor color = issue.level == IssueLevel::Error ? EditorWorkspaceColor{.95f, .32f, .32f, 1} : EditorWorkspaceColor{.97f, .74f, .30f, 1};
        ui.CanvasFill({at.x - 6, at.y - 6}, {at.x + 6, at.y + 6}, {.08f, .04f, .04f, .85f});
        ui.CanvasCircle(at, 7, color, 2);
        ui.CanvasText({at.x - 2, at.y - 8}, color, "!");
    }
    // 回放で通れなかった解法の、プレイヤーの道筋。
    if (pathIndex_ >= 0 && pathIndex_ < static_cast<int>(solutions_.size())) {
        const auto& path = solutions_[static_cast<size_t>(pathIndex_)].path;
        for (size_t i = 1; i < path.size(); ++i) {
            ui.CanvasLine(view_.Screen(static_cast<float>(path[i - 1].first), static_cast<float>(path[i - 1].second)),
                          view_.Screen(static_cast<float>(path[i].first), static_cast<float>(path[i].second)), {.98f, .55f, .25f, .9f}, 2);
        }
    }
}
}
