#include "TD2LevelEditor.h"

namespace TD2::Editor::Level {
namespace {
// 道具の列。レベルを作る人が使うのは四つ。カメラと舞台は「…」で開く（カメラ位置と 3D の配置を直す人向け）。
struct ToolInfo { Tool tool; std::string_view label; };
constexpr std::array<ToolInfo, 4> kTools{{{Tool::Select, "地図"}, {Tool::Terrain, "描く"}, {Tool::Link, "配線"}, {Tool::Machine, "仕掛け"}}};
constexpr std::array<ToolInfo, 2> kMoreTools{{{Tool::Camera, "カメラ"}, {Tool::Stage, "舞台"}}};
}

EditorWorkspaceDescriptor LevelEditor::GetDescriptor() const {
    EditorWorkspaceDescriptor d; d.workspaceId = kLevelWorkspace; d.displayName = "レベル編集";
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
    SetNote("保存しました");
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
    if (ui.BeginPanel("td2.level.side", "プロパティ", nullptr, EditorWorkspaceDockRegion::Left)) { DrawInspector(ctx); } ui.EndPanel();
    if (ui.BeginPanel("td2.level.map", "地図", nullptr, EditorWorkspaceDockRegion::Right)) { DrawToolbar(ctx); DrawMap(ctx); } ui.EndPanel();
    if (ui.BeginPanel("td2.level.stage", "部品", nullptr, EditorWorkspaceDockRegion::Bottom)) { DrawPalette(ctx); } ui.EndPanel();
    model_.Publish(ctx);
}

// 地図の上の一列：道具と、試遊・検査・保存。
void LevelEditor::DrawToolbar(EditorWorkspaceHostContext& ctx) {
    auto& ui = *ctx.ui;
    const auto button = [&](std::string_view id, std::string_view label, bool on) {
        ui.TableNextColumn();
        return ui.Button("td2.level." + std::string(id), (on ? "● " : "") + std::string(label));
    };
    const size_t columns = kTools.size() + 1 + (moreTools_ ? kMoreTools.size() : 0) + 3;
    if (ui.BeginTable("td2.level.tools", static_cast<uint32_t>(columns))) {
        ui.TableNextRow();
        for (const auto& info : kTools) {
            // 「描く」は地形と装置のどちらか（最後に選んだ方）。
            const bool painting = info.tool == Tool::Terrain;
            const bool on = painting ? tool_ == Tool::Terrain || tool_ == Tool::Device : tool_ == info.tool;
            if (button("tool." + std::string(info.label), info.label, on)) { ChooseTool(ctx, painting ? paintTool_ : info.tool); }
        }
        if (button("tool.more", moreTools_ ? "‹" : "…", false)) { moreTools_ = !moreTools_; }
        if (moreTools_) for (const auto& info : kMoreTools) {
            if (button("tool." + std::string(info.label), info.label, tool_ == info.tool)) { ChooseTool(ctx, info.tool); }
        }
        if (button("play", "▶ 試遊", pickingStart_)) {
            if (pickingStart_) { pickingStart_ = false; }
            else if (model_.Commit(ctx)) { pickingStart_ = true; drag_ = Drag::None; recipe_ = -1; focus_ = Focus::None; }
        }
        // 検査：問題と、通れなくなった解法の数。押すと解法を流し直し、左の欄に結果を出す。
        const size_t failed = static_cast<size_t>(std::count_if(solutions_.begin(), solutions_.end(), [](const auto& s) { return !s.passed; }));
        const size_t problems = issues_.size() + failed;
        if (button("check", problems ? "⚠ 検査 " + std::to_string(problems) : "✓ 検査", false)) { RunSolutions(ctx); focus_ = Focus::None; }
        if (button("save", model_.sceneDirty || model_.Dirty() ? "保存 ●" : "保存", false)) { Save(ctx); }
        ui.EndTable();
    }
    if (!model_.error.empty()) { ui.Text("⚠ " + model_.error); }
}

void LevelEditor::ChooseTool(EditorWorkspaceHostContext& ctx, Tool tool) {
    if (tool == tool_) { return; }
    if (posing_) { CancelPose(ctx); }
    if (previewState_ != 0) { PreviewState(ctx, 0); }
    if (model_.gesture) { model_.Finish(ctx, true); }
    tool_ = tool; drag_ = Drag::None; link_ = {}; recipe_ = -1; recipeFirst_.clear(); pickingStart_ = false; plan_ = false;
    multi_.clear();
    if (tool_ == Tool::Machine) { model_.machinesStale = true; checkInputClock_ = 10; if (machineObject_) { focus_ = Focus::Machine; } }
    if (tool_ == Tool::Camera) { layoutStale_ = true; }
    if (tool_ == Tool::Terrain || tool_ == Tool::Device) { paintTool_ = tool_; }
    if (tool_ == Tool::Terrain || tool_ == Tool::Device || tool_ == Tool::Link) { model_.devices.Select(-1); if (model_.ready) { focus_ = Focus::Room; } }
}

bool LevelEditor::SelectRoom(EditorWorkspaceHostContext& ctx, uint64_t objectId) {
    if (!model_.Select(ctx, objectId)) { return false; }
    edge_ = {};
    return true;
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
    ++layoutVersion_;
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
