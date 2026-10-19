#include "TD2MapWorkspaces.h"
#include "TD2StagePanel.h"
#include "TD2TelevisionMode.h"
#include "TD2WorldMode.h"

namespace TD2::Editor {
namespace {
using namespace Level;

// ---------------------------------------------------------------- レベル編集
// テレビ地図と世界地図を一つのワークスペースにまとめる。中央は標準の 3D ビューポート、左は一覧と属性、右は 2D の地図、下は 3D の舞台。
// 2D の地図と 3D のビューポートを同時に見られるので、タブを切り替えずに配置と見た目を確かめられる。
class LevelEditorWorkspace final : public IEditorWorkspace {
public:
    explicit LevelEditorWorkspace(std::shared_ptr<MapModel> model) : model_(model), television_(model), world_(model) {
        const auto request = [this](int view) { requested_ = view; };
        television_.switcher = request; world_.switcher = request;
    }
    EditorWorkspaceDescriptor GetDescriptor() const override {
        EditorWorkspaceDescriptor d; d.workspaceId = kLevelWorkspace; d.displayName = "关卡编辑";
        d.options = static_cast<uint32_t>(EditorWorkspaceOption::RequestSceneViewport); return d;
    }
    std::string_view GetDocumentIdentity() const override { return "td2.scene.maps"; }
    void OnActivate(EditorWorkspaceHostContext& ctx) override {
        Active().OnActivate(ctx);
        // 開いた時の地図の選択をシーンへ押し付けない（シーンで選んでいるテレビの方へ地図が付いて行く）。
        pushed_ = model_->selected; model_->selectionClock = 1; model_->sceneSelection = 0;
    }
    void OnDeactivate(EditorWorkspaceHostContext& ctx) override { Active().OnDeactivate(ctx); }
    void Draw(EditorWorkspaceHostContext& ctx) override {
        if (!ctx.ui || !ctx.commands) { return; }
        ApplyRequestedView(ctx);
        model_->Poll(ctx);
        model_->FollowScene(ctx);
        if (model_->selected != pushed_ && !model_->gesture) { pushed_ = model_->selected; model_->ShowInScene(ctx); }
        auto& ui = *ctx.ui;
        Active().BeginFrame(ctx);
        // 三つの欄は毎フレーム同じ ID で開く（最初のドッキングで各欄を決まった場所に置くため）。
        if (ui.BeginPanel("td2.level.side", "关卡", nullptr, EditorWorkspaceDockRegion::Left)) { Active().DrawSide(ctx); } ui.EndPanel();
        if (ui.BeginPanel("td2.level.map", "地图", nullptr, EditorWorkspaceDockRegion::Right)) { Active().DrawMap(ctx); } ui.EndPanel();
        if (ui.BeginPanel("td2.level.stage", "3D 布景", nullptr, EditorWorkspaceDockRegion::Bottom)) { stage_.Draw(ctx, *model_, world_); } ui.EndPanel();
        model_->Publish(ctx);
    }
    bool IsDirty() const override { return television_.IsDirty() || world_.IsDirty(); }
    EditorWorkspaceSaveResult Save(EditorWorkspaceHostContext& ctx) override { return Active().Save(ctx); }
    void DiscardChanges(EditorWorkspaceHostContext& ctx) override { television_.DiscardChanges(ctx); world_.DiscardChanges(ctx); }
    EditorWorkspaceCloseDisposition QueryClose(EditorWorkspaceCloseReason) const override { return IsDirty() ? EditorWorkspaceCloseDisposition::RequiresSave : EditorWorkspaceCloseDisposition::Allow; }
private:
    MapMode& Active() { return worldActive_ ? static_cast<MapMode&>(world_) : static_cast<MapMode&>(television_); }
    // 見方の切り替えはフレームの初めにまとめて行う（描画の途中で欄の中身を入れ替えない）。
    void ApplyRequestedView(EditorWorkspaceHostContext& ctx) {
        if (!requested_) { return; }
        const int view = *requested_; requested_.reset();
        const bool toWorld = view != kTelevisionView;
        if (toWorld != worldActive_) {
            if (!Active().ApplyDraft(ctx)) { return; }
            Active().OnDeactivate(ctx); worldActive_ = toWorld; Active().OnActivate(ctx);
        }
        if (toWorld && world_.Mode() != view) { world_.ChangeMode(ctx, view); }
    }
    std::shared_ptr<MapModel> model_;
    TelevisionMode television_;
    WorldMode world_;
    StagePanel stage_;
    bool worldActive_ = false;
    std::optional<int> requested_;
    uint64_t pushed_ = 0;
};
}

std::vector<std::unique_ptr<IEditorWorkspace>> MakeMapWorkspaces() {
    std::vector<std::unique_ptr<IEditorWorkspace>> workspaces;
    workspaces.push_back(std::make_unique<LevelEditorWorkspace>(std::make_shared<MapModel>()));
    return workspaces;
}

}
