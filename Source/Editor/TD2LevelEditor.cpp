#include "TD2LevelEditor.h"

namespace TD2::Editor::Level {
namespace {
// 道具の列。名前は短く、押すとその道具の棚が下に出る。
struct ToolInfo { Tool tool; std::string_view label; };
constexpr std::array<ToolInfo, 7> kTools{{{Tool::Select, "选择"}, {Tool::Terrain, "地形"}, {Tool::Device, "装置"}, {Tool::Link, "连线"},
                                         {Tool::Machine, "机构"}, {Tool::Camera, "镜头"}, {Tool::Stage, "布景"}}};
}

EditorWorkspaceDescriptor LevelEditor::GetDescriptor() const {
    EditorWorkspaceDescriptor d; d.workspaceId = kLevelWorkspace; d.displayName = "关卡编辑";
    d.options = static_cast<uint32_t>(EditorWorkspaceOption::RequestSceneViewport);
    return d;
}

void LevelEditor::OnActivate(EditorWorkspaceHostContext& ctx) {
    model_.Poll(ctx, true);
    model_.machinesStale = true; layoutStale_ = true; checkClock_ = 10; chapterClock_ = 10; solutionsStale_ = true;
    view_.fit = true; fitChapter_ = true;
    // 開いた時の地図の選択をシーンへ押し付けない（シーンで選んでいるテレビの方へ地図が付いて行く）。
    pushed_ = model_.selected; model_.sceneSelection = 0; model_.selectionMark.clear(); model_.pulseClock = 1;
}

void LevelEditor::OnDeactivate(EditorWorkspaceHostContext& ctx) {
    if (posing_) { CancelPose(ctx); }
    if (previewState_ != 0) { PreviewState(ctx, 0); }
    if (drag_ == Drag::Library) { model_.Discard(); }
    else if (model_.gesture) { model_.Finish(ctx, true); }
    drag_ = Drag::None; pickingStart_ = false; ghost_ = -1; recipe_ = -1;
}

EditorWorkspaceSaveResult LevelEditor::Save(EditorWorkspaceHostContext& ctx) {
    if (posing_) { CancelPose(ctx); }
    if (previewState_ != 0) { PreviewState(ctx, 0); }
    if (!model_.Commit(ctx)) { return EditorWorkspaceSaveResult::Failed; }
    const auto saved = Execute(ctx, "scene.save", Json::object());
    if (!saved.ok) { model_.error = saved.error; return EditorWorkspaceSaveResult::Failed; }
    model_.sceneDirty = false; model_.savedOnce = true; model_.error.clear();
    SetNote("已保存");
    return EditorWorkspaceSaveResult::Saved;
}

void LevelEditor::Draw(EditorWorkspaceHostContext& ctx) {
    if (!ctx.ui || !ctx.commands) { return; }
    clock_ += ctx.unscaledDeltaSeconds; noteClock_ += ctx.unscaledDeltaSeconds;
    dragHeldAny_ = dragPressedAny_ = dragReleasedAny_ = false;
    model_.Poll(ctx);
    model_.FollowScene(ctx);
    // シーンで選んだテレビへ地図の選択が付いて行く。地図で選んだテレビはシーンでも選ぶ。
    if (model_.selected != pushed_ && !model_.gesture) {
        pushed_ = model_.selected; model_.ShowInScene(ctx);
        if (focus_ == Focus::None || focus_ == Focus::Object) { focus_ = Focus::Room; }
    }
    RefreshStageObject(ctx);
    FinishStand(ctx);
    FollowAnchors(ctx);
    PollRecording(ctx);
    RefreshCheck(ctx);
    auto& ui = *ctx.ui;
    // 標準の Ctrl+Z / Ctrl+Y はエンジンがシーンの履歴へ送る。ここでは重ねず、読み直しだけを頼む。
    const auto interaction = ui.GetInteraction();
    if (!interaction.editing && (interaction.undoPressed || interaction.redoPressed)) {
        model_.historyFrames = 3; model_.sceneDirty = true; model_.machinesStale = true; layoutStale_ = true; followResync_ = true;
    }
    // 三つの欄は毎フレーム同じ ID で開く（最初のドッキングで各欄を決まった場所に置くため）。左を先に描き、地図へのドラッグを集める。
    if (ui.BeginPanel("td2.level.side", "属性", nullptr, EditorWorkspaceDockRegion::Left)) { DrawInspector(ctx); } ui.EndPanel();
    if (ui.BeginPanel("td2.level.map", "地图", nullptr, EditorWorkspaceDockRegion::Right)) { DrawToolbar(ctx); DrawMap(ctx); } ui.EndPanel();
    if (ui.BeginPanel("td2.level.stage", "零件", nullptr, EditorWorkspaceDockRegion::Bottom)) { DrawPalette(ctx); } ui.EndPanel();
    model_.Publish(ctx);
}

// 地図の上の列：道具・試遊・問題・保存と履歴。狭い欄でも切れないよう表に並べる。
void LevelEditor::DrawToolbar(EditorWorkspaceHostContext& ctx) {
    auto& ui = *ctx.ui;
    if (ui.BeginTable("td2.level.tools", static_cast<uint32_t>(kTools.size()))) {
        ui.TableNextRow();
        for (const auto& info : kTools) {
            ui.TableNextColumn();
            if (ui.Button("td2.level.tool." + std::string(info.label), (tool_ == info.tool ? "● " : "") + std::string(info.label))) { ChooseTool(ctx, info.tool); }
        }
        ui.TableNextRow();
        ui.TableNextColumn();
        if (ui.Button("td2.level.play", pickingStart_ ? "● 试玩" : "▶ 试玩")) {
            if (pickingStart_) { pickingStart_ = false; }
            else if (model_.Commit(ctx)) { pickingStart_ = true; drag_ = Drag::None; recipe_ = -1; placeModel_ = -1; focus_ = Focus::None; }
        }
        ui.TableNextColumn();
        const size_t errors = static_cast<size_t>(std::count_if(issues_.begin(), issues_.end(), [](const LevelIssue& i) { return i.level == IssueLevel::Error; }));
        const std::string issueLabel = issues_.empty() ? "✓ 检查" : (errors ? "⚠ " : "△ ") + std::to_string(issues_.size());
        if (ui.Button("td2.level.issues", issueLabel) && !issues_.empty()) { JumpToIssue(ctx, issueIndex_ % issues_.size()); ++issueIndex_; }
        ui.TableNextColumn();
        size_t passed = 0;
        for (const auto& s : solutions_) { passed += s.passed ? 1 : 0; }
        // 回放の後に関卡を直したら、結果は古い（もう一度押して回放する）。
        const bool stale = solutionsStale_ || solutionsRevision_ != model_.revision;
        const std::string solutionLabel = solutions_.empty() ? "解法" : (stale ? "解法 " : passed == solutions_.size() ? "✓ 解法 " : "✕ 解法 ") + std::to_string(passed) + "/" + std::to_string(solutions_.size()) + (stale ? " ↻" : "");
        if (ui.Button("td2.level.solutions", solutionLabel)) { RunSolutions(ctx); focus_ = Focus::None; }
        ui.TableNextColumn();
        if (ui.Button("td2.level.fit", "适应")) { fitChapter_ = false; view_.fit = true; }
        ui.TableNextColumn();
        if (ui.Button("td2.level.undo", "撤销")) { History(ctx, false); }
        ui.TableNextColumn();
        if (ui.Button("td2.level.redo", "重做")) { History(ctx, true); }
        ui.TableNextColumn();
        if (ui.Button("td2.level.save", model_.sceneDirty || model_.Dirty() ? "保存 ●" : "保存")) { Save(ctx); }
        ui.EndTable();
    }
    if (!model_.error.empty()) { ui.Text("⚠ " + model_.error); }
}

void LevelEditor::ChooseTool(EditorWorkspaceHostContext& ctx, Tool tool) {
    if (tool == tool_) { return; }
    if (posing_) { CancelPose(ctx); }
    if (previewState_ != 0) { PreviewState(ctx, 0); }
    if (model_.gesture) { model_.Finish(ctx, true); }
    tool_ = tool; drag_ = Drag::None; link_ = {}; recipe_ = -1; recipeFirst_.clear(); placeModel_ = -1; pickingStart_ = false; plan_ = false;
    multi_.clear();
    if (tool_ == Tool::Machine) { model_.machinesStale = true; checkInputClock_ = 10; if (machineObject_) { focus_ = Focus::Machine; } }
    if (tool_ == Tool::Camera) { layoutStale_ = true; }
    if (tool_ == Tool::Terrain || tool_ == Tool::Device || tool_ == Tool::Link) { model_.devices.Select(-1); if (model_.ready) { focus_ = Focus::Room; } }
}

void LevelEditor::History(EditorWorkspaceHostContext& ctx, bool redo) {
    if (posing_) { CancelPose(ctx); }
    if (model_.Dirty() && !model_.Commit(ctx)) { return; }
    model_.History(ctx, redo);
    model_.machinesStale = true; layoutStale_ = true; checkClock_ = 10; followResync_ = true;
}

bool LevelEditor::SelectRoom(EditorWorkspaceHostContext& ctx, uint64_t objectId) {
    if (!model_.Select(ctx, objectId)) { return false; }
    edge_ = {};
    return true;
}

void LevelEditor::FocusRoom(uint64_t objectId) {
    for (const auto& room : model_.rooms) {
        if (room.object.objectId != objectId) { continue; }
        const auto& s = SettingsOf(room);
        if (s.worldPlaced) { FitRoom(s); }
        return;
    }
}

void LevelEditor::FitAll(EditorWorkspacePoint size) {
    bool any = false; WorldGridRect bounds{};
    for (const auto& r : model_.rooms) if (r.settings.worldPlaced) {
        const auto rect = WorldBounds(Placement(r.settings));
        if (!any) { bounds = rect; any = true; }
        else { bounds.left = (std::min)(bounds.left, rect.left); bounds.top = (std::min)(bounds.top, rect.top); bounds.right = (std::max)(bounds.right, rect.right); bounds.bottom = (std::max)(bounds.bottom, rect.bottom); }
    }
    view_.Fit(size, static_cast<float>(bounds.left), static_cast<float>(bounds.top), any ? static_cast<float>(bounds.right - bounds.left) : 32.0f, any ? static_cast<float>(bounds.bottom - bounds.top) : 18.0f);
}

void LevelEditor::FitRoom(const RegionSettings& s) {
    // 隣のテレビも少し見えるよう、一回り広く合わせる。
    const float margin = (std::max)(4.0f, static_cast<float>((std::max)(s.region.cols, s.region.rows)) * .35f);
    view_.Fit(view_.lastSize, static_cast<float>(s.worldX) - margin, static_cast<float>(s.worldY) - margin,
              static_cast<float>(s.region.cols) + margin * 2, static_cast<float>(s.region.rows) + margin * 2);
}

bool LevelEditor::EnsureLayout(EditorWorkspaceHostContext& ctx, bool force) {
    layoutClock_ += ctx.unscaledDeltaSeconds;
    // シーンの目印が変わった時（3D の移動・取り消し・他の編集）だけ読み直す。動かしている間は 1 秒に一回まで。
    if (!force && !layoutStale_ && layoutRevision_ == model_.revision &&
        (layoutClock_ < 1.f || (layoutPulse_ == model_.Pulses() && layoutClock_ < 30.f))) { return layoutValid_; }
    layoutClock_ = 0; layoutStale_ = false; layoutRevision_ = model_.revision; layoutPulse_ = model_.Pulses();
    std::string error;
    layoutValid_ = LoadLayout(ctx, layout_, error);
    if (!layoutValid_) { model_.error = error; return false; }
    planPaths_.clear();
    std::map<std::string, CameraPose, std::less<>> cameras;
    for (const auto& room : layout_.rooms) { if (room.cameraId) { cameras[room.regionId] = CurrentCameraPose(room); } }
    for (const auto& edge : layout_.edges) {
        if (!edge.between || !edge.passable) { continue; }
        const auto plan = PlanTravel(layout_, edge, cameras);
        PlanPath path{edge.a, edge.b, {}, plan.problem};
        for (int i = 0; i <= 24; ++i) { path.points.push_back(SampleTravel(plan.keys, i / 24.0).eye); }
        planPaths_.push_back(std::move(path));
    }
    return true;
}

void LevelEditor::RefreshMachines(EditorWorkspaceHostContext& ctx, bool force) {
    if (force) { model_.machinesStale = true; }
    model_.RefreshMachines(ctx);
}

std::vector<std::unique_ptr<IEditorWorkspace>> MakeLevelWorkspaces() {
    std::vector<std::unique_ptr<IEditorWorkspace>> workspaces;
    workspaces.push_back(std::make_unique<LevelEditor>());
    return workspaces;
}
}
