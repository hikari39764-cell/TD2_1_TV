#pragma once
// レベル編集：世界地図を主の画布にして、テレビの地形と装置・画面の外の仕掛け・カメラ・3D の布景を一つのワークスペースで直す。
// 右は世界地図（上に道具の列、右端に未配置のテレビ）、左は選んだものの属性（何も選んでいない時は試遊・問題・解法）、下は置くものの棚。中央は 3D ビューポート。
#include "TD2LevelModel.h"
#include "TD2Recipes.h"
#include "TD2StageAttach.h"
#include "TD2Cables.h"
#include "TD2LevelCheck.h"
#include "TD2Replay.h"

namespace TD2::Editor::Level {
// 道具。地図の上の押し・ドラッグの意味が変わる。
enum class Tool { Select, Terrain, Device, Link, Machine, Camera, Stage };
// 左の欄に出すもの（最後に選んだもの）。
enum class Focus { None, Room, Device, Machine, Edge, Object };

struct EdgeSelection { uint64_t a = 0, b = 0; SharedWorldEdge edge; };
// 圧力板の連線の始点。
struct LinkSource { uint64_t room = 0; std::string device; };

class LevelEditor final : public IEditorWorkspace {
public:
    LevelEditor() = default;
    EditorWorkspaceDescriptor GetDescriptor() const override;
    std::string_view GetDocumentIdentity() const override { return "td2.scene.maps"; }
    void OnActivate(EditorWorkspaceHostContext& ctx) override;
    void OnDeactivate(EditorWorkspaceHostContext& ctx) override;
    void Draw(EditorWorkspaceHostContext& ctx) override;
    bool IsDirty() const override { return model_.Dirty(); }
    EditorWorkspaceSaveResult Save(EditorWorkspaceHostContext& ctx) override;
    void DiscardChanges(EditorWorkspaceHostContext&) override { model_.Discard(); }
    EditorWorkspaceCloseDisposition QueryClose(EditorWorkspaceCloseReason) const override {
        return IsDirty() ? EditorWorkspaceCloseDisposition::RequiresSave : EditorWorkspaceCloseDisposition::Allow;
    }

private:
    enum class Drag { None, Room, Group, Box, Device, Resize, Spawn, Paint, Library, Link, Ghost };

    // ---- 全体（TD2LevelEditor.cpp）
    void DrawToolbar(EditorWorkspaceHostContext& ctx);
    void ChooseTool(EditorWorkspaceHostContext& ctx, Tool tool);
    bool EnsureLayout(EditorWorkspaceHostContext& ctx, bool force = false);
    void RefreshMachines(EditorWorkspaceHostContext& ctx, bool force = false);
    void RefreshCheck(EditorWorkspaceHostContext& ctx);
    // 区域を選ぶ（草稿を確定してから）。シーンの選択も合わせる。
    bool SelectRoom(EditorWorkspaceHostContext& ctx, uint64_t objectId);
    void FitAll(EditorWorkspacePoint size);
    void FitRoom(const RegionSettings& s);
    void SetNote(std::string text) { note_ = std::move(text); noteClock_ = 0; }
    void MarkSceneChanged() { model_.sceneDirty = true; layoutStale_ = true; checkClock_ = 10; solutionsStale_ = true; }

    // ---- 地図（TD2LevelMap.cpp）
    void DrawMap(EditorWorkspaceHostContext& ctx);
    void TrackDrag(const EditorWorkspaceCanvasInput& input);
    const RegionSettings& SettingsOf(const RoomItem& room) const;
    RoomItem* RoomAt(EditorWorkspacePoint world, bool placedOnly = true);
    std::optional<EdgeSelection> EdgeAt(EditorWorkspacePoint pointer) const;
    std::vector<EdgeSelection> Edges() const;
    std::pair<EditorWorkspacePoint, EditorWorkspacePoint> EdgePoints(const SharedWorldEdge& e) const;
    MapCanvasView RoomView(const RegionSettings& s) const;
    void DrawRooms(EditorWorkspaceHostContext& ctx, const EditorWorkspaceCanvasInput& input);
    void DrawEdges(IEditorWorkspaceUi& ui, const EditorWorkspaceCanvasInput& input);
    void DrawLinks(IEditorWorkspaceUi& ui, const EditorWorkspaceCanvasInput& input);
    void DrawHover(IEditorWorkspaceUi& ui, const EditorWorkspaceCanvasInput& input, EditorWorkspacePoint p);
    void DrawIssues(IEditorWorkspaceUi& ui);
    void DrawChapterLabels(IEditorWorkspaceUi& ui);
    // 地図の右の「未配置」：世界に置いていないテレビを並べ、地図へドラッグして置く（ここへ戻すと取り消し）。
    struct TrayItem { uint64_t objectId = 0; EditorWorkspacePoint a, b; };
    std::vector<TrayItem> TrayItems(EditorWorkspacePoint size) const;
    bool OverTray(EditorWorkspacePoint pointer, EditorWorkspacePoint size) const;
    bool PressTray(EditorWorkspaceHostContext& ctx, const EditorWorkspaceCanvasInput& input);
    void DrawTray(IEditorWorkspaceUi& ui, const EditorWorkspaceCanvasInput& input);
    // 地図の左下に、今の道具の操作を一行だけ出す。
    void DrawToolHint(IEditorWorkspaceUi& ui, const EditorWorkspaceCanvasInput& input);
    // 3D の向き：地図で隣り合うテレビ（世界に無ければ 3D で一番近いテレビ）と逆を向いていれば「裏」。
    bool BackFacing(const RoomItem& room);
    bool FitChapter(EditorWorkspacePoint size);
    void PressSelect(EditorWorkspaceHostContext& ctx, const EditorWorkspaceCanvasInput& input, EditorWorkspacePoint p);
    void PressTerrain(EditorWorkspaceHostContext& ctx, const EditorWorkspaceCanvasInput& input, EditorWorkspacePoint p);
    void PressDevice(EditorWorkspaceHostContext& ctx, const EditorWorkspaceCanvasInput& input, EditorWorkspacePoint p);
    void PressLink(EditorWorkspaceHostContext& ctx, EditorWorkspacePoint p);
    void ReleaseLink(EditorWorkspaceHostContext& ctx, EditorWorkspacePoint p);
    void PressStage(EditorWorkspaceHostContext& ctx, const EditorWorkspaceCanvasInput& input, EditorWorkspacePoint p);
    void ContinueDrag(EditorWorkspaceHostContext& ctx, const EditorWorkspaceCanvasInput& input, EditorWorkspacePoint p, bool inside);
    void FinishDrag(EditorWorkspaceHostContext& ctx, EditorWorkspacePoint p, bool inside);
    void PaintLine(EditorWorkspacePoint a, EditorWorkspacePoint b, bool erase);
    void PaintCell(int x, int y, bool erase);
    int HandleAt(EditorWorkspacePoint screen) const;
    void DrawSelectionHandles(IEditorWorkspaceUi& ui);
    void DrawPlacementGhost(IEditorWorkspaceUi& ui, const EditorWorkspaceCanvasInput& input, EditorWorkspacePoint p);
    void DrawSpawnPicker(EditorWorkspaceHostContext& ctx, const EditorWorkspaceCanvasInput& input, EditorWorkspacePoint p);
    void HandleDelete(EditorWorkspaceHostContext& ctx);
    void RememberTemplate(const DeviceDraft& d) { templates_[d.kind] = d; }
    void ApplyTemplate(DeviceDraft& d, int cols, int rows) const;
    bool FinishGroupMove(EditorWorkspaceHostContext& ctx);
    std::string NeighborRegion(const RegionSettings& s) const;

    // ---- 左の欄（TD2LevelInspector.cpp）
    void DrawInspector(EditorWorkspaceHostContext& ctx);
    void DrawOverview(EditorWorkspaceHostContext& ctx);
    void DrawRoomInspector(EditorWorkspaceHostContext& ctx);
    void DrawDeviceInspector(EditorWorkspaceHostContext& ctx);
    void DrawObjectInspector(EditorWorkspaceHostContext& ctx);
    void DrawRoomStage(EditorWorkspaceHostContext& ctx, const RoomItem& room);
    void DrawRoomCamera(EditorWorkspaceHostContext& ctx, const std::string& regionId);
    void DrawPlateLinks(EditorWorkspaceHostContext& ctx);
    void DrawScreenBinding(EditorWorkspaceHostContext& ctx);
    void RefreshChapters(EditorWorkspaceHostContext& ctx);
    void CommitPending(EditorWorkspaceHostContext& ctx);

    // ---- 下の棚（TD2LevelPalette.cpp）
    void DrawPalette(EditorWorkspaceHostContext& ctx);
    bool Tile(IEditorWorkspaceUi& ui, std::string_view id, std::string_view label, bool selected, const std::function<void(IEditorWorkspaceUi&, EditorWorkspacePoint)>& icon);
    void DrawPropShelf(EditorWorkspaceHostContext& ctx);
    void PlaceProp(EditorWorkspaceHostContext& ctx, const StageProp& prop);
    void FinishStand(EditorWorkspaceHostContext& ctx);

    // ---- 仕掛け（TD2LevelMachines.cpp）
    Machine* SelectedMachine();
    void DrawMachineShelf(EditorWorkspaceHostContext& ctx);
    void DrawMachineInspector(EditorWorkspaceHostContext& ctx);
    void DrawMachineAdvanced(EditorWorkspaceHostContext& ctx, const Machine& m);
    void DrawMachineOverlay(IEditorWorkspaceUi& ui, const EditorWorkspaceCanvasInput& input);
    void DrawMachineIcons(IEditorWorkspaceUi& ui);
    std::optional<EditorWorkspacePoint> MachineIcon(const Machine& m) const;
    bool PressMachine(EditorWorkspaceHostContext& ctx, const EditorWorkspaceCanvasInput& input, EditorWorkspacePoint p);
    void DragGhost(EditorWorkspaceHostContext& ctx, const EditorWorkspaceCanvasInput& input, EditorWorkspacePoint p);
    bool WriteSelectedMachine(EditorWorkspaceHostContext& ctx, const Machine& after);
    bool ApplyRecipe(EditorWorkspaceHostContext& ctx, const Machine& before, Machine after, bool rebuildParts);
    void CreateFromRecipe(EditorWorkspaceHostContext& ctx, const std::string& regionId);
    void ToggleMember(EditorWorkspaceHostContext& ctx, const std::string& regionId);
    bool RenameMachine(EditorWorkspaceHostContext& ctx, const Machine& before, const std::string& id);
    void TogglePlateTarget(EditorWorkspaceHostContext& ctx, const std::string& plate, const std::string& target);
    // 状態の見本（0 = A、1 = B、-1 = C）をシーンに置く。0 で元に戻す。
    bool PreviewState(EditorWorkspaceHostContext& ctx, int state);
    bool BeginPose(EditorWorkspaceHostContext& ctx);
    bool SavePose(EditorWorkspaceHostContext& ctx);
    void CancelPose(EditorWorkspaceHostContext& ctx);
    bool ReadPoses(EditorWorkspaceHostContext& ctx, std::map<uint64_t, Pose>& poses, std::map<uint64_t, Json>& expected, std::string& sceneId, uint64_t& revision);
    void RefreshCheckInput(EditorWorkspaceHostContext& ctx);
    std::optional<RegionPlacement> PlacementOf(const std::string& regionId) const;
    std::string RoomName(const std::string& regionId) const;

    // ---- カメラ（TD2LevelCamera.cpp）
    void DrawCameraShelf(EditorWorkspaceHostContext& ctx);
    void DrawEdgeInspector(EditorWorkspaceHostContext& ctx);
    void DrawAreaInspector(EditorWorkspaceHostContext& ctx);
    void DrawPlan(EditorWorkspaceHostContext& ctx, const EditorWorkspaceCanvasInput& input);
    void DrawAreaTint(IEditorWorkspaceUi& ui, const RegionSettings& s, EditorWorkspacePoint a, EditorWorkspacePoint b);
    void DrawEdgeKind(IEditorWorkspaceUi& ui, const EdgeSelection& e, EditorWorkspacePoint a, EditorWorkspacePoint b) const;
    const LayoutEdge* LayoutEdgeOf(const EdgeSelection& e) const;
    int AreaIndex(std::string_view regionId) const;
    static EditorWorkspaceColor AreaColor(int index);
    void ToggleArea(EditorWorkspaceHostContext& ctx, const std::string& regionId);
    void Generate(EditorWorkspaceHostContext& ctx, bool cameras, bool travels, std::string_view areaName, std::string_view roomId,
                  const LayoutEdge* onlyEdge, bool force);
    void SetUpNewRoom(EditorWorkspaceHostContext& ctx, const std::string& regionId, const std::string& besideRegion);
    void TrialFromEdge(EditorWorkspaceHostContext& ctx, const LayoutEdge& edge, bool fromA);
    std::string ResolveStyleLabel(const LayoutEdge& edge) const;

    // ---- 3D の布景（TD2LevelStage.cpp）
    void RefreshStageObject(EditorWorkspaceHostContext& ctx);
    void AttachRoom(EditorWorkspaceHostContext& ctx, const std::string& regionId, const std::string& anchor);
    void FollowAnchors(EditorWorkspaceHostContext& ctx);
    void RouteCables(EditorWorkspaceHostContext& ctx, const std::string& plate, const std::string& target);

    // ---- 試遊・検査・解法（TD2LevelPlay.cpp）
    void StartPreview(EditorWorkspaceHostContext& ctx, uint64_t room, PreviewSpawn spawn);
    void DrawPlayInspector(EditorWorkspaceHostContext& ctx);
    void PollRecording(EditorWorkspaceHostContext& ctx);
    void RunSolutions(EditorWorkspaceHostContext& ctx);
    void JumpToIssue(EditorWorkspaceHostContext& ctx, size_t index);
    void SaveRecording(EditorWorkspaceHostContext& ctx);

    // ---- データ
    MapModel model_;
    Tool tool_ = Tool::Select;
    Tool paintTool_ = Tool::Terrain;   // 「描く」で最後に選んだもの（地形か装置）
    bool moreTools_ = false;           // カメラと舞台の道具を出すか
    Focus focus_ = Focus::None;
    MapCanvasView view_;
    bool fitChapter_ = true;   // 合わせる範囲：今のテレビの章（false で世界全体）
    float clock_ = 0, noteClock_ = 0;
    std::string note_;
    uint64_t pushed_ = 0;
    bool canvasHovered_ = false;
    float emptyPressAt_ = -1;            // 空いた所を最後に押した時刻（二度押しで全体を見る）
    uint64_t backKey_ = 0;
    std::map<uint64_t, bool> back_;      // テレビの物体 → 裏か（布局と配置が変わった時だけ求め直す）

    // 地図の操作
    Drag drag_ = Drag::None;
    EditorWorkspacePoint dragStart_{}, dragLast_{}, dragOffset_{};
    DeviceDraft original_;
    int handle_ = -1, groupDx_ = 0, groupDy_ = 0;
    std::set<uint64_t> multi_;
    uint64_t lastClick_ = 0;
    float lastClickTime_ = -1;
    bool dragHeldAny_ = false, dragPressedAny_ = false, dragReleasedAny_ = false;
    bool erase_ = false, pickingStart_ = false;
    std::set<size_t> erasedDeviceCells_;

    // 地形・装置
    int brushSize_ = 1;
    bool rectanglePaint_ = false;
    std::string kind_ = "Solid";
    std::map<std::string, DeviceDraft, std::less<>> templates_;
    bool propertyPending_ = false;
    LinkSource link_;
    std::string hoverText_;

    // 布景
    uint64_t stageObject_ = 0;
    StageObject stageRead_;
    bool stageReadOk_ = false, stagePending_ = false;
    Pose stageEditing_;
    float stageClock_ = 10;
    uint64_t stagePulse_ = 0;
    uint64_t stand_ = 0, standTelevision_ = 0;
    int standFrames_ = 0;
    std::map<uint64_t, Pose> anchorPoses_, seenPoses_;
    float followClock_ = 0;
    uint64_t followPulse_ = 0;
    int followReads_ = 0;
    bool followResync_ = true;

    // 一覧
    std::map<uint64_t, std::string> chapterOf_;
    float chapterClock_ = 10;
    uint64_t chapterPulse_ = 0;
    bool screenSettings_ = false, deviceAdvanced_ = false, stagePoseOpen_ = false;

    // カメラ・布局
    LayoutSnapshot layout_;
    bool layoutValid_ = false, layoutStale_ = true, plan_ = false;
    uint64_t layoutRevision_ = 0, layoutPulse_ = 0, layoutVersion_ = 0;   // layoutVersion_ は読み直すたびに増える
    float layoutClock_ = 0;
    std::string areaFocus_;
    EdgeSelection edge_;
    LayoutEdge edgeDraft_;
    std::string edgeDraftKey_;
    bool edgeDraftPending_ = false;
    std::array<double, 3> areaDraft_{};
    bool areaDraftPending_ = false;
    uint64_t areaDraftObject_ = 0, areaDraftComponent_ = 0;
    struct PlanPath { std::string a, b; std::vector<Vec3> points; std::string problem; };
    std::vector<PlanPath> planPaths_;

    // 仕掛け
    uint64_t machineObject_ = 0, machineComponent_ = 0;
    bool machineC_ = false, posing_ = false, posingC_ = false, confirmDelete_ = false, machineAdvanced_ = false;
    int previewState_ = 0;
    std::map<uint64_t, Pose> poseA_;
    int ghost_ = -1, ghostDx_ = 0, ghostDy_ = 0;
    bool ghostC_ = false;
    EditorWorkspacePoint ghostGrab_{};
    std::optional<Machine> ghostPreview_;
    std::string machineIdDraft_;
    MachineCheckInput checkInput_;
    std::map<uint64_t, std::string> objectNames_;
    float checkInputClock_ = 10;
    uint64_t checkInputPulse_ = 0;
    std::optional<Machine> pendingMachine_;
    int recipe_ = -1;                 // 選んだレシピ（次に押したテレビで作る）
    std::string recipeFirst_;         // 二台を選ぶレシピの一台目

    // 検査・解法
    std::vector<LevelIssue> issues_;
    size_t issueIndex_ = 0;
    float checkClock_ = 10;
    std::vector<SolutionResult> solutions_;
    bool solutionsStale_ = true;
    uint64_t solutionsRevision_ = 0;     // 再生した時の区域の読み直し回数（変わったら結果は古い）
    std::optional<PlayRecord> lastRecord_;
    std::filesystem::file_time_type recordingTime_{};
    bool recordingSeen_ = false;
    float recordingClock_ = 0;
    int pathIndex_ = -1, confirmSolution_ = -1;
    std::map<std::string, int> previewStates_;   // 試遊を始める時の仕掛けの状態（仕掛けの ID → 状態）
};

std::vector<std::unique_ptr<IEditorWorkspace>> MakeLevelWorkspaces();
}
