#include "TD2MapWorkspaces.h"
#include "TD2MapCanvas.h"
#include "TD2EditorChoices.h"
#include "TD2CameraAuthoring.h"
#include "TD2CameraLayout.h"
#include "TD2MachineEditor.h"
#include "TD2StageTools.h"
#include "../Common/TD2WorldLayout.h"
#include "../Common/TD2PreviewEntry.h"
#include <array>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <set>

namespace TD2::Editor {
namespace {
constexpr uint32_t kPrimary = static_cast<uint32_t>(EditorWorkspacePointerButton::Primary);
constexpr uint32_t kSecondary = static_cast<uint32_t>(EditorWorkspacePointerButton::Secondary);
constexpr const char* kLevelWorkspace = "td2.level_editor";
// 見方：-1 はテレビ地図、0〜3 は世界地図の配置・カメラ大区域・俯瞰カメラ・仕掛け。
constexpr int kTelevisionView = -1;

struct RoomItem {
    SceneItem object;
    RegionTarget target;
    RegionSettings settings;
};

RegionPlacement Placement(const RegionSettings& s) {
    return {s.region.regionId, s.region.cols, s.region.rows, s.worldPlaced, s.worldX, s.worldY};
}

// Scene の作者データを共有する。二つのワークスペースは同じ選択と草稿を見る。
class MapModel {
public:
    std::vector<RoomItem> rooms;
    uint64_t selected = 0;
    RegionSettings draft;
    DeviceEditor devices;
    bool ready = false, gesture = false, refresh = true;
    float elapsed = 0;
    uint64_t revision = 0;
    bool sceneDirty = false, savedOnce = false;
    std::string error, published, screenWarning;
    // 画面の外の仕掛け（読み直しは重いので、開いた時・仕掛けを書いた時・刷新を押した時だけ）。
    std::vector<Machine> machines;
    bool machinesStale = true;
    void RefreshMachines(EditorWorkspaceHostContext& ctx) {
        if (!machinesStale) { return; }
        machinesStale = false;
        std::string problem;
        if (!LoadMachines(ctx, machines, problem)) { error = problem; }
    }
    // 全部の区域の圧力板の目標（"区域ID/装置ID" → 目標）。草稿中の区域は草稿の値。
    std::map<std::string, std::vector<std::string>> PlateTargets() const {
        std::map<std::string, std::vector<std::string>> result;
        for (const auto& room : rooms) {
            const auto& s = room.object.objectId == selected && ready ? draft : room.settings;
            DeviceEditor d; d.Load(s.devices);
            for (const auto& device : d.Items()) { if (device.kind == "Plate") { result[s.region.regionId + "/" + device.id] = device.targets; } }
        }
        return result;
    }

    RoomItem* Current() {
        const auto it = std::find_if(rooms.begin(), rooms.end(), [this](const auto& r) { return r.object.objectId == selected; });
        return it == rooms.end() ? nullptr : &*it;
    }
    const RoomItem* Current() const { return const_cast<MapModel*>(this)->Current(); }
    bool Dirty() const { const auto* item = Current(); return ready && item && draft != item->settings; }
    void Discard() { if (const auto* item = Current()) { draft = item->settings; devices.Load(draft.devices); } gesture = false; error.clear(); screenWarning.clear(); }
    void Poll(EditorWorkspaceHostContext& ctx, bool force = false) {
        elapsed += ctx.unscaledDeltaSeconds;
        // 標準の Ctrl+Z / Ctrl+Y はシーンの履歴を直接動かすので、その後の数フレームは草稿を捨てて読み直す。
        if (historyFrames > 0) { --historyFrames; if (!gesture) { Discard(); force = true; machinesStale = true; } }
        if (gesture || Dirty() || (!force && !refresh && elapsed < .75f)) { return; }
        elapsed = 0; refresh = false;
        std::vector<SceneItem> objects;
        if (!QueryObjects(ctx, kRegionComponentType, objects, error)) { return; }
        std::vector<RoomItem> fresh;
        for (const auto& object : objects) {
            RoomItem item; item.object = object;
            if (!ReadRegion(ctx, object.objectId, item.target, item.settings, error)) { return; }
            fresh.push_back(std::move(item));
        }
        rooms = std::move(fresh);
        ++revision;
        if (!Current()) { selected = rooms.empty() ? 0 : rooms.front().object.objectId; }
        ready = Current() != nullptr;
        if (ready) { draft = Current()->settings; devices.Load(draft.devices); }
        // 他のパネルが作るカメラ参照やプレビューの結びも保存状態に含める。
        const auto state = Execute(ctx, "editor.state", Json::object());
        if (state.ok) {
            const auto scene = state.data.find("scene");
            if (scene != state.data.end() && scene->is_object()) {
                const auto dirty = scene->find("dirty");
                if (dirty != scene->end() && dirty->is_boolean()) { sceneDirty = dirty->get<bool>(); }
            }
        }
    }
    bool Select(EditorWorkspaceHostContext& ctx, uint64_t objectId) {
        if (objectId == selected && ready) { return true; }
        if (Dirty() && !Commit(ctx)) { return false; }
        selected = objectId; ready = Current() != nullptr;
        if (ready) { draft = Current()->settings; devices.Select(-1); devices.Load(draft.devices); screenWarning.clear(); }
        return ready;
    }
    std::vector<RegionPlacement> Layout(const RegionSettings* replacement = nullptr) const {
        std::vector<RegionPlacement> result;
        for (const auto& r : rooms) { result.push_back(Placement(replacement && r.object.objectId == selected ? *replacement : r.settings)); }
        return result;
    }
    std::string Validate(const RegionSettings& s) const {
        if (!std::isfinite(s.screenBrightness) || s.screenBrightness < .5f || s.screenBrightness > 4.0f) { return "屏幕亮度范围为 0.5～4"; }
        const auto region = ValidateRegion(s.region);
        if (!region.valid) { return std::string(region.message); }
        if (s.region.cols > 64 || s.region.rows > 64 || s.region.cells.size() > 4096) { return "电视地图最多 64 × 64 格"; }
        DeviceEditor d; d.Load(s.devices);
        if (const auto problem = d.Validate(s.region.cols, s.region.rows); !problem.empty()) { return problem; }
        if (!std::isfinite(s.spawnX) || !std::isfinite(s.spawnY) || s.spawnX < 0 || s.spawnY < 0 ||
            s.spawnX + .625f > static_cast<float>(s.region.cols) || s.spawnY + .875f > static_cast<float>(s.region.rows)) { return "出生位置超出电视地图"; }
        const auto layout = Layout(&s);
        const auto valid = ValidateWorldLayout(layout);
        return valid.valid ? std::string{} : valid.message + "：" + valid.regionA + " / " + valid.regionB;
    }
    bool Commit(EditorWorkspaceHostContext& ctx) {
        auto* item = Current(); if (!ready || !item || !Dirty()) { return true; }
        // 保存やワークスペース切り替えの時も、画面の index は実際のモデルから取り直す。
        if (draft.screenObject != item->settings.screenObject || draft.screenSlot != item->settings.screenSlot) { ResolveScreenMaterial(ctx, draft, screenWarning); }
        if (const auto problem = Validate(draft); !problem.empty()) { error = problem; return false; }
        RegionTarget target; RegionSettings fresh;
        if (!ReadRegion(ctx, selected, target, fresh, error)) { return false; }
        if (target != item->target || fresh != item->settings) { error = "地图已被其他编辑更新；请撤回草稿并刷新"; return false; }
        Json changes = Json::array(); AppendRegionChanges(changes, target, fresh, draft);
        std::vector<RoomItem*> otherEntries;
        // 開始テレビの変更も同じ取り消しに含め、開始区域を二つ残さない。
        if (draft.entryRegion) for (auto& other : rooms) {
            if (other.object.objectId == selected || !other.settings.entryRegion) { continue; }
            RegionTarget otherTarget; RegionSettings otherFresh;
            if (!ReadRegion(ctx, other.object.objectId, otherTarget, otherFresh, error)) { return false; }
            if (otherTarget != other.target || otherFresh != other.settings) { error = "开始电视已被其他编辑更新"; return false; }
            changes.push_back({{"objectId", otherTarget.objectId}, {"componentId", otherTarget.componentId}, {"propertyId", "entryRegion"}, {"value", false}});
            otherEntries.push_back(&other);
        }
        const auto written = Execute(ctx, "authoring.transaction", {{"commands", changes}});
        if (!written.ok) { error = written.error; return false; }
        sceneDirty = true;
        for (auto* other : otherEntries) {
            RegionTarget observedTarget; RegionSettings observed;
            if (!ReadRegion(ctx, other->object.objectId, observedTarget, observed, error)) { return false; }
            if (observedTarget != other->target || observed.entryRegion) { error = "开始电视写入回读不一致"; return false; }
            other->settings = observed;
        }
        RegionSettings confirmed; RegionTarget confirmedTarget;
        if (!ReadRegion(ctx, selected, confirmedTarget, confirmed, error)) { return false; }
        item->settings = confirmed; item->target = confirmedTarget;
        if (confirmedTarget != target || confirmed != draft) { error = "地图写入回读不一致；草稿保留"; return false; }
        draft = confirmed; devices.Load(draft.devices); error.clear(); refresh = true;
        return true;
    }
    // 一筆・一回のドラッグを一つの authoring.transaction にまとめる。
    bool Finish(EditorWorkspaceHostContext& ctx, bool rejectInvalid = false) {
        gesture = false;
        if (Commit(ctx)) { return true; }
        if (rejectInvalid) { const auto message = error; Discard(); error = message; }
        return false;
    }
    void History(EditorWorkspaceHostContext& ctx, bool redo) {
        if (gesture) { return; }
        if (Dirty()) { error = "先应用或撤回当前草稿"; return; }
        const auto result = Execute(ctx, redo ? "scene.redo" : "scene.undo", Json::object());
        if (!result.ok) { error = result.error; return; }
        sceneDirty = true;
        refresh = true; Poll(ctx, true);
    }
    void Create(EditorWorkspaceHostContext& ctx) {
        if (Dirty() && !Commit(ctx)) { return; }
        const auto created = Execute(ctx, "scene.create_empty_object", {{"name", "电视地图"}});
        if (!created.ok) { error = created.error; return; }
        sceneDirty = true;
        const auto id = ObjectId(created.data.value("objectId", Json{}));
        if (!id || !AddRegion(ctx, id, false, error)) { return; }
        refresh = true; Poll(ctx, true); Select(ctx, id);
    }
    void AddSelectedTelevision(EditorWorkspaceHostContext& ctx) {
        if (Dirty() && !Commit(ctx)) { return; }
        const auto id = SelectedObject(ctx, error);
        if (!id || !AddRegion(ctx, id, true, error)) { return; }
        sceneDirty = true;
        refresh = true; Poll(ctx, true);
        if (!Select(ctx, id)) { return; }
        ResolveScreenMaterial(ctx, draft, screenWarning);
        if (Commit(ctx)) { error.clear(); }
    }
    // ---- シーン（3D）の選択との同期。シーンでテレビを選ぶと地図もそのテレビを開き、地図で選ぶとシーンでもそのテレビを選ぶ。
    uint64_t sceneSelection = 0;
    float selectionClock = 1;
    int historyFrames = 0;
    static uint64_t TelevisionObject(const RoomItem& room) { return room.settings.screenObject ? room.settings.screenObject : room.object.objectId; }
    void FollowScene(EditorWorkspaceHostContext& ctx) {
        selectionClock += ctx.unscaledDeltaSeconds;
        if (gesture || selectionClock < .25f) { return; }
        selectionClock = 0;
        const auto state = Execute(ctx, "editor.state", Json::object());
        if (!state.ok) { return; }
        const auto selection = state.data.value("selection", Json::object());
        const uint64_t id = selection.is_object() ? ObjectId(selection.value("activeObjectId", Json{})) : 0;
        if (id == sceneSelection) { return; }
        sceneSelection = id;
        for (const auto& room : rooms) {
            if (!id || (room.object.objectId != id && room.settings.screenObject != id)) { continue; }
            if (room.object.objectId != selected) { Select(ctx, room.object.objectId); }
            return;
        }
    }
    void ShowInScene(EditorWorkspaceHostContext& ctx) {
        const auto* room = Current();
        if (!room) { return; }
        const auto id = TelevisionObject(*room);
        if (id != sceneSelection && Execute(ctx, "scene.object.select", {{"objectId", id}}).ok) { sceneSelection = id; }
    }
    // 新しく置いたテレビの物体を区域にする：型の格子、床だけの地図、画面の解像度。
    // beside が世界に置いてあれば、世界地図ではその右（空いていなければ左・上・下）に下端を揃えて置く。
    bool AdoptTelevision(EditorWorkspaceHostContext& ctx, uint64_t object, const TelevisionModel& model, std::optional<RegionSettings> beside) {
        if (Dirty() && !Commit(ctx)) { return false; }
        if (!AddRegion(ctx, object, true, error)) { return false; }
        sceneDirty = true;
        refresh = true; Poll(ctx, true);
        if (!Select(ctx, object)) { return false; }
        ResolveScreenMaterial(ctx, draft, screenWarning);
        const auto previous = draft.region;
        draft.region.cols = model.cols; draft.region.rows = model.rows;
        if (!ResizeCells(previous, draft.region)) { draft.region = previous; error = "格子尺寸设置失败"; return false; }
        std::fill(draft.region.cells.begin(), draft.region.cells.end(), '0');
        for (int x = 0; x < model.cols; ++x) { draft.region.cells[CellIndex(draft.region, x, model.rows - 1)] = '1'; }
        const float scale = (std::min)(1.0f, 2048.0f / static_cast<float>((std::max)(model.cols, model.rows) * draft.region.tilePixels));
        draft.outputWidth = (std::max)(32, static_cast<int>(std::round(static_cast<float>(model.cols * draft.region.tilePixels) * scale)));
        draft.outputHeight = (std::max)(32, static_cast<int>(std::round(static_cast<float>(model.rows * draft.region.tilePixels) * scale)));
        draft.spawnX = 1.1875f; draft.spawnY = static_cast<float>(model.rows) - 1.875f;
        draft.devices.clear(); devices.Load(draft.devices);
        draft.worldPlaced = false;
        if (beside && beside->worldPlaced) {
            const int bx = beside->worldX, by = beside->worldY, bc = beside->region.cols, br = beside->region.rows;
            const std::array<std::pair<int, int>, 4> spots{{{bx + bc, by + br - model.rows}, {bx - model.cols, by + br - model.rows}, {bx, by - model.rows}, {bx, by + br}}};
            for (const auto& [x, y] : spots) {
                auto trial = draft; trial.worldPlaced = true; trial.worldX = x; trial.worldY = y;
                if (ValidateWorldLayout(Layout(&trial)).valid) { draft = trial; break; }
            }
        }
        if (!Commit(ctx)) { return false; }
        error = draft.worldPlaced || !beside ? std::string{} : "已建好；世界地图上旁边没有空位，请从列表把它拖进世界";
        return true;
    }
    void Publish(EditorWorkspaceHostContext& ctx) {
        std::string message = !error.empty() ? error : screenWarning;
        if (message.empty() && !rooms.empty()) {
            const auto entries = std::count_if(rooms.begin(), rooms.end(), [](const auto& r) { return r.settings.worldPlaced && r.settings.entryRegion; });
            if (entries == 0) { message = "请在世界地图放置开始电视"; }
            else if (entries > 1) { message = "世界地图需要唯一的开始电视"; }
        }
        if (message == published) { return; }
        ctx.commands->PublishStatus(message.empty() ? EditorWorkspaceStatusSeverity::Info : EditorWorkspaceStatusSeverity::Warning, message);
        published = message;
    }
};

// レベル編集の二つの見方（テレビ地図・世界地図）の共通部分。左の欄（一覧と属性）と右の欄（地図）の中身を描く。
class MapMode {
public:
    explicit MapMode(std::shared_ptr<MapModel> model) : model_(std::move(model)) {}
    virtual ~MapMode() = default;
    virtual void OnActivate(EditorWorkspaceHostContext& ctx) { model_->Poll(ctx, true); model_->machinesStale = true; }
    virtual void OnDeactivate(EditorWorkspaceHostContext& ctx) { if (model_->gesture) { model_->Finish(ctx, true); } }
    virtual bool IsDirty() const { return model_->Dirty(); }
    EditorWorkspaceSaveResult Save(EditorWorkspaceHostContext& ctx) {
        if (!ApplyDraft(ctx)) { return EditorWorkspaceSaveResult::Failed; }
        const auto saved = Execute(ctx, "scene.save", Json::object());
        if (!saved.ok) { model_->error = saved.error; return EditorWorkspaceSaveResult::Failed; }
        model_->sceneDirty = false; model_->savedOnce = true; model_->error.clear();
        return EditorWorkspaceSaveResult::Saved;
    }
    virtual void DiscardChanges(EditorWorkspaceHostContext&) { model_->Discard(); }
    // 見方を変える前に草稿をシーンへ適用する。ディスクへの保存とは別。
    virtual bool ApplyDraft(EditorWorkspaceHostContext& ctx) { return model_->Commit(ctx); }
    // 一フレームの始まり（左右の欄を描く前）。
    virtual void BeginFrame(EditorWorkspaceHostContext&) {}
    virtual void DrawSide(EditorWorkspaceHostContext& ctx) = 0;   // 左：一覧と属性
    virtual void DrawMap(EditorWorkspaceHostContext& ctx) = 0;    // 右：2D の地図
    // 見方の切り替えを頼む（実際の切り替えは次のフレームの初め）。
    std::function<void(int)> switcher;
protected:
    void SaveControls(EditorWorkspaceHostContext& ctx, std::string_view controlId = "td2.map.save") {
        auto& ui = *ctx.ui;
        // 作者が保存した時だけ Scene をディスクへ書く。ブラシやドラッグでは書かない。
        if (ui.Button(controlId, "保存场景")) { Save(ctx); }
        ui.SameLine(); ui.Text(IsDirty() ? "草稿未应用" : model_->sceneDirty ? "已应用 · 未保存" : model_->savedOnce ? "已保存" : "已载入");
    }
    // 左の欄の上：保存・取り消し・やり直し・草稿。
    void HistoryToolbar(EditorWorkspaceHostContext& ctx) {
        auto& ui = *ctx.ui;
        SaveControls(ctx);
        // 取り消し・やり直しの前に草稿を適用する。
        if (ui.Button("td2.map.undo", "撤销")) { if (!IsDirty() || ApplyDraft(ctx)) { model_->History(ctx, false); } }
        ui.SameLine();
        if (ui.Button("td2.map.redo", "重做")) { if (!IsDirty() || ApplyDraft(ctx)) { model_->History(ctx, true); } }
        ui.SameLine();
        if (ui.Button("td2.map.refresh", "刷新")) { model_->Poll(ctx, true); model_->machinesStale = true; }
        if (model_->Dirty() && !model_->gesture) {
            ui.SameLine(); if (ui.Button("td2.map.apply", "应用")) { model_->Commit(ctx); }
            ui.SameLine(); if (ui.Button("td2.map.discard", "撤回草稿")) { model_->Discard(); }
        }
        // 標準の Ctrl+Z / Ctrl+Y はエンジンがシーンの履歴へ送る。ここでは重ねて取り消さず、読み直しだけを頼む。
        const auto interaction = ui.GetInteraction();
        if (!interaction.editing && (interaction.undoPressed || interaction.redoPressed)) { model_->historyFrames = 3; model_->sceneDirty = true; }
        if (!model_->error.empty()) { ui.Text("⚠ " + model_->error); }
        ui.Separator();
    }
    // 右の欄の上：見方の切り替え。
    // 欄が狭くても切れないよう、表の列に並べる。
    void ModeBar(EditorWorkspaceHostContext& ctx, int current) {
        auto& ui = *ctx.ui;
        constexpr std::array<std::string_view, 5> names{"电视地图", "世界布局", "镜头大区域", "俯视镜头", "机关"};
        if (!ui.BeginTable("td2.level.views", static_cast<uint32_t>(names.size()))) { return; }
        ui.TableNextRow();
        for (size_t i = 0; i < names.size(); ++i) {
            const int view = static_cast<int>(i) - 1;
            ui.TableNextColumn();
            if (ui.Button("td2.level.view." + std::to_string(i), (view == current ? "● " : "") + std::string(names[i])) && view != current && switcher) { switcher(view); }
        }
        ui.EndTable();
    }
    // 地図の見方（合わせる・つかんで動かす）。中ボタンのドラッグとホイールはいつでも使える。
    void ViewControls(EditorWorkspaceHostContext& ctx) {
        auto& ui = *ctx.ui;
        if (ui.Button("td2.map.fit", "适应")) { view_.fit = true; }
        ui.SameLine();
        if (ui.Button("td2.map.pan", hand_ ? "平移 ●" : "平移")) { hand_ = !hand_; }
    }
    void DrawThumbnail(IEditorWorkspaceUi& ui, const RoomItem& room, const EditorWorkspaceCanvasInput& canvas) {
        const auto& settings = room.object.objectId == model_->selected && model_->ready ? model_->draft : room.settings;
        MapCanvasView thumbnail;
        thumbnail.tile = (std::min)((canvas.size.x - 12) / static_cast<float>(settings.region.cols), (canvas.size.y - 12) / static_cast<float>(settings.region.rows));
        thumbnail.tile = (std::max)(.5f, thumbnail.tile);
        thumbnail.origin = {(canvas.size.x - static_cast<float>(settings.region.cols) * thumbnail.tile) * .5f, (canvas.size.y - static_cast<float>(settings.region.rows) * thumbnail.tile) * .5f};
        DrawRoomMap(ui, settings, thumbnail, false, false);
        ui.CanvasRect({1, 1}, {canvas.size.x - 1, canvas.size.y - 1}, room.object.objectId == model_->selected ? EditorWorkspaceColor{.77f, .68f, .39f, 1} : EditorWorkspaceColor{.24f, .38f, .43f, 1});
    }
    std::shared_ptr<MapModel> model_;
    MapCanvasView view_;
    bool hand_ = false;
};

// テレビ単位の作者編集。実行時の状態は持たない。
class TelevisionMode final : public MapMode {
public:
    using MapMode::MapMode;
    void OnActivate(EditorWorkspaceHostContext& ctx) override { MapMode::OnActivate(ctx); drag_ = Drag::None; ChooseTool(0); view_.fit = true; }
    void OnDeactivate(EditorWorkspaceHostContext& ctx) override { MapMode::OnDeactivate(ctx); drag_ = Drag::None; linkSource_.clear(); }
    // 左：保存と履歴・今のテレビ・テレビの一覧（畳める）・選んだものの属性。
    void DrawSide(EditorWorkspaceHostContext& ctx) override {
        auto& ui = *ctx.ui;
        HistoryToolbar(ctx);
        if (const auto* room = model_->Current()) {
            ui.Text(ShortChoiceLabel(room->object.name, 16) + "  " + std::to_string(room->settings.region.cols) + "×" + std::to_string(room->settings.region.rows));
            ui.SameLine();
        }
        if (ui.Button("td2.television.gallery", gallery_ ? "收起列表" : "换一台电视")) { gallery_ = !gallery_; }
        if (gallery_ || !model_->ready) { DrawGallery(ctx); ui.Separator(); }
        DrawProperties(ctx);
    }
    // 右：見方の切り替え・道具と仕掛けの板・地図。置くものを選ぶのに欄を切り替えなくてよいよう、全部を地図の上に並べる。
    void DrawMap(EditorWorkspaceHostContext& ctx) override {
        ModeBar(ctx, kTelevisionView);
        // シーンでの選択などでテレビが変わったら、新しいテレビに合わせる。
        if (model_->selected != shown_) { shown_ = model_->selected; view_.fit = true; drag_ = Drag::None; ChooseTool(tool_ == 3 ? 3 : 0); }
        DrawPalette(ctx);
        DrawCanvas(ctx);
    }
private:
    enum class Drag { None, Paint, Move, Resize, Spawn, Box };
    void ChooseTool(int tool) { tool_ = tool; hand_ = false; linkSource_.clear(); groupIds_.clear(); }
    void ChooseKind(size_t index) { kind_ = kEditorDeviceKinds[index]; ChooseTool(3); model_->devices.Select(-1); }
    void DrawGallery(EditorWorkspaceHostContext& ctx) {
        auto& ui = *ctx.ui;
        if (ui.Button("td2.television.add_selected", "选中电视建图")) { model_->AddSelectedTelevision(ctx); view_.fit = true; ChooseTool(0); }
        ui.SameLine();
        if (ui.Button("td2.television.new", "+ 空白地图")) { model_->Create(ctx); view_.fit = true; ChooseTool(0); }
        EditText(ui, "td2.television.gallery.search", "查找", gallerySearch_);
        const auto rooms = model_->rooms;
        for (size_t i = 0; i < rooms.size(); ++i) {
            const auto& room = rooms[i];
            if (!gallerySearch_.empty() && room.object.name.find(gallerySearch_) == std::string::npos &&
                room.settings.region.regionId.find(gallerySearch_) == std::string::npos) { continue; }
            ui.Text(ShortChoiceLabel(room.object.name) + " " + std::to_string(room.settings.region.cols) + "×" + std::to_string(room.settings.region.rows));
            const auto input = ui.BeginCanvas("td2.television.thumbnail." + std::to_string(room.object.objectId), 60);
            DrawThumbnail(ui, room, input);
            if (input.hovered && (input.pressedButtons & kPrimary)) {
                if (model_->Select(ctx, room.object.objectId)) { view_.fit = true; drag_ = Drag::None; gallery_ = false; ChooseTool(0); }
            }
            ui.EndCanvas();
        }
    }
    // 道具（選ぶ・地形・消す・連線・スポーン位置・範囲選択・貼り付け）と仕掛けの種類を一枚の板にする。仕掛けの種類を押すとそのまま置く道具になる。
    void DrawPalette(EditorWorkspaceHostContext& ctx) {
        auto& ui = *ctx.ui;
        constexpr std::array<std::pair<int, std::string_view>, 7> tools{{{0, "↖ 选择"}, {1, "▧ 地形"}, {2, "⌫ 擦除"}, {4, "⌁ 连线"}, {5, "◉ 出生点"}, {6, "□ 框选"}, {7, "▣ 粘贴"}}};
        constexpr size_t columns = 4;
        if (ui.BeginTable("td2.television.tools", static_cast<uint32_t>(columns))) {
            for (size_t i = 0; i <= tools.size(); ++i) {
                if (i % columns == 0) { ui.TableNextRow(); }
                ui.TableNextColumn();
                if (i == tools.size()) { ViewControls(ctx); break; }
                const auto [tool, label] = tools[i];
                if (ui.Button("td2.television.tool." + std::to_string(tool), std::string(tool_ == tool ? "● " : "") + std::string(label))) { ChooseTool(tool); }
            }
            ui.EndTable();
        }
        ui.Text("机关");
        if (ui.BeginTable("td2.television.kinds", static_cast<uint32_t>(columns))) {
            for (size_t i = 0; i < kEditorDeviceKinds.size(); ++i) {
                if (i % columns == 0) { ui.TableNextRow(); }
                ui.TableNextColumn();
                const bool current = tool_ == 3 && kind_ == kEditorDeviceKinds[i];
                if (ui.Button("td2.television.kind." + std::to_string(i), std::string(current ? "● " : "") + std::string(kEditorDeviceLabels[i]))) { ChooseKind(i); }
            }
            ui.EndTable();
        }
        // 道具ごとの補助（筆の太さ・範囲選択のコピー）と一行の説明。
        if (tool_ == 1 || tool_ == 2) {
            EditInt(ui, "td2.television.brush.size", "笔刷", brushSize_, 1, 4);
            ui.SameLine(); ui.Checkbox("td2.television.brush.rectangle", "矩形填充", &rectanglePaint_);
        }
        if (tool_ == 6) {
            ui.Text("框选机关：" + std::to_string(groupIds_.size()));
            ui.SameLine();
            if (ui.Button("td2.television.group.copy", "复制框选")) {
                clipboard_.clear(); for (const auto& d : model_->devices.Items()) { if (std::find(groupIds_.begin(), groupIds_.end(), d.id) != groupIds_.end()) { clipboard_.push_back(d); } }
                if (clipboard_.empty()) { model_->error = "先拖框选中机关"; }
                else { ChooseTool(7); model_->error.clear(); }
            }
        }
        ui.Text(Hint());
    }
    // 一行が欄の幅で切れないよう、短い行に分ける。
    std::string Hint() const {
        switch (tool_) {
        case 1: return "左键拖动画地形 · 右键拖动擦除";
        case 2: return "拖动擦除地形和机关";
        case 3: return "点空格放「" + DeviceLabel(kind_) + "」· 点已有的机关改为选中\nShift+点击叠放 · 新放的沿用上次调好的属性";
        case 4: return "先点压板，再点门 / 激光 / 桥\n再点一次断开";
        case 5: return "点击设出生点";
        case 6: return "拖框选中多个机关，再「复制框选」";
        case 7: return clipboard_.empty() ? "先框选并复制机关" : "点击地图粘贴 " + std::to_string(clipboard_.size()) + " 个机关";
        default: return "点机关选中并拖动 · 拖角改大小 · Delete 删除\n右键拖动擦除 · 滚轮缩放 · 中键平移";
        }
    }
    // 種類ごとに最後に直した形（大きさ・向き・射程・追加の項目）を覚え、次に置くものへ写す。
    void RememberTemplate(const DeviceDraft& d) { templates_[d.kind] = d; }
    void ApplyTemplate(DeviceDraft& d, int cols, int rows) const {
        const auto found = templates_.find(d.kind);
        if (found == templates_.end()) { return; }
        const auto& t = found->second;
        d.w = std::clamp(t.w, .125, cols - d.x); d.h = std::clamp(t.h, .125, rows - d.y);
        d.direction = t.direction; d.range = t.range; d.inverted = t.inverted; d.latch = t.latch; d.extra = t.extra;
    }
    void PaintLine(EditorWorkspacePoint a, EditorWorkspacePoint b, bool erase) {
        int x = static_cast<int>(std::floor(a.x)), y = static_cast<int>(std::floor(a.y));
        const int ex = static_cast<int>(std::floor(b.x)), ey = static_cast<int>(std::floor(b.y));
        const int dx = std::abs(ex - x), dy = -std::abs(ey - y), sx = x < ex ? 1 : -1, sy = y < ey ? 1 : -1;
        int error = dx + dy;
        for (int steps = 0; steps < 512; ++steps) {
            for (int by = 0; by < brushSize_; ++by) for (int bx = 0; bx < brushSize_; ++bx) { PaintCell(x + bx, y + by, erase); }
            if (x == ex && y == ey) { break; }
            const int twice = error * 2;
            if (twice >= dy) { error += dy; x += sx; }
            if (twice <= dx) { error += dx; y += sy; }
        }
    }
    void PaintCell(int x, int y, bool erase) {
        auto& region = model_->draft.region;
        if (x < 0 || y < 0 || x >= region.cols || y >= region.rows) { return; }
        const auto index = CellIndex(region, x, y);
        if (erase) {
            if (erasedDeviceCells_.contains(index)) { return; }
            const int hit = model_->devices.Hit(x + .5, y + .5);
            if (hit >= 0) {
                model_->devices.Select(hit); model_->devices.RemoveSelected(); model_->draft.devices = model_->devices.Serialize();
                erasedDeviceCells_.insert(index); return;
            }
        }
        region.cells[index] = erase ? '0' : '1';
    }
    int HandleAt(EditorWorkspacePoint screen) const {
        const auto* d = model_->devices.Selected(); if (!d) { return -1; }
        const auto a = view_.Screen(static_cast<float>(d->x), static_cast<float>(d->y));
        const auto b = view_.Screen(static_cast<float>(d->x + d->w), static_cast<float>(d->y + d->h));
        const auto points = Handles(a, b);
        // 縮小した薄い装置ではハンドルが近いので、一番近いものを選んで向きを安定させる。
        int hit = -1; float closest = 81;
        for (size_t i = 0; i < points.size(); ++i) {
            const float dx = screen.x - points[i].x, dy = screen.y - points[i].y, distance = dx * dx + dy * dy;
            if (distance <= closest) { closest = distance; hit = static_cast<int>(i); }
        }
        return hit;
    }
    static std::array<EditorWorkspacePoint, 8> Handles(EditorWorkspacePoint a, EditorWorkspacePoint b) {
        const float x = (a.x + b.x) * .5f, y = (a.y + b.y) * .5f;
        return {a, EditorWorkspacePoint{x, a.y}, EditorWorkspacePoint{b.x, a.y}, EditorWorkspacePoint{b.x, y}, b,
                EditorWorkspacePoint{x, b.y}, EditorWorkspacePoint{a.x, b.y}, EditorWorkspacePoint{a.x, y}};
    }
    void DrawSelection(IEditorWorkspaceUi& ui) {
        const auto* d = model_->devices.Selected(); if (!d) { return; }
        const auto a = view_.Screen(static_cast<float>(d->x), static_cast<float>(d->y));
        const auto b = view_.Screen(static_cast<float>(d->x + d->w), static_cast<float>(d->y + d->h));
        ui.CanvasRect(a, b, {.97f, .80f, .40f, 1}, 2);
        for (const auto& p : Handles(a, b)) {
            ui.CanvasFill({p.x - 4, p.y - 4}, {p.x + 4, p.y + 4}, {.97f, .80f, .40f, 1});
        }
    }
    void DrawCanvas(EditorWorkspaceHostContext& ctx) {
        auto& ui = *ctx.ui;
        const auto input = ui.BeginCanvas("td2.television.canvas", 0);
        canvasHovered_ = input.hovered != 0;
        view_.ObserveSize(input.size);
        if (model_->ready && input.size.x > 0 && input.size.y > 0) {
            auto& s = model_->draft;
            if (view_.fit && (!model_->gesture || !view_.hasFit)) { view_.Fit(input.size, 0, 0, static_cast<float>(s.region.cols), static_cast<float>(s.region.rows)); }
            view_.Navigate(input, hand_, drag_ != Drag::None);
            DrawCanvasGrid(ui, view_, input.size);
            const auto valid = ValidateRegion(s.region);
            if (valid.valid) {
                const auto p = view_.World(input.pointer);
                const bool inside = p.x >= 0 && p.y >= 0 && p.x < static_cast<float>(s.region.cols) && p.y < static_cast<float>(s.region.rows);
                if (!hand_ && !view_.panning && input.hovered && (input.pressedButtons & (kPrimary | kSecondary)) && (inside || (tool_ == 0 && HandleAt(input.pointer) >= 0))) {
                    if (model_->Dirty() && !model_->Commit(ctx)) { ui.EndCanvas(); return; }
                    start_ = last_ = p; erase_ = tool_ == 2 || (input.pressedButtons & kSecondary);
                    erasedDeviceCells_.clear();
                    if (erase_) {
                        const int hit = model_->devices.Hit(p.x, p.y);
                        if (hit >= 0) {
                            model_->devices.Select(hit); model_->devices.RemoveSelected(); s.devices = model_->devices.Serialize();
                            erasedDeviceCells_.insert(CellIndex(s.region, static_cast<int>(std::floor(p.x)), static_cast<int>(std::floor(p.y))));
                        }
                    }
                    if (tool_ == 6 && !erase_) { groupIds_.clear(); model_->devices.Select(-1); drag_ = Drag::Box; }
                    else if (tool_ == 7 && !erase_) {
                        if (model_->devices.Paste(clipboard_, std::floor(p.x), std::floor(p.y), s.region.cols, s.region.rows, groupIds_)) { s.devices = model_->devices.Serialize(); model_->Commit(ctx); }
                        else { model_->error = clipboard_.empty() ? "先框选并复制机关" : "复制的机关组超出当前电视"; }
                    }
                    else if (tool_ == 0 && !erase_) {
                        groupIds_.clear();
                        handle_ = HandleAt(input.pointer);
                        if (handle_ < 0) { model_->devices.Select(model_->devices.Hit(p.x, p.y)); }
                        if (const auto* d = model_->devices.Selected()) { original_ = *d; drag_ = handle_ < 0 ? Drag::Move : Drag::Resize; }
                    } else if (tool_ == 4 && !erase_) {
                        const int hit = model_->devices.Hit(p.x, p.y);
                        if (hit >= 0) {
                            model_->devices.Select(hit);
                            const auto* d = model_->devices.Selected();
                            if (d->kind == "Plate") { linkSource_ = d->id; }
                            else if (model_->devices.ToggleLink(linkSource_, hit)) {
                                s.devices = model_->devices.Serialize(); model_->Commit(ctx);
                                for (size_t i = 0; i < model_->devices.Items().size(); ++i) { if (model_->devices.Items()[i].id == linkSource_) { model_->devices.Select(static_cast<int>(i)); break; } }
                            }
                        }
                    } else if (tool_ == 3 && !erase_) {
                        const int hit = model_->devices.Hit(p.x, p.y);
                        if (hit >= 0 && !input.shift) { model_->devices.Select(hit); ChooseTool(0); }
                        else if (model_->devices.Add(kind_, static_cast<int>(std::floor(p.x)), static_cast<int>(std::floor(p.y)), s.region.cols, s.region.rows)) {
                            if (auto* added = model_->devices.Selected()) { ApplyTemplate(*added, s.region.cols, s.region.rows); }
                            s.devices = model_->devices.Serialize(); model_->Commit(ctx);
                        }
                    } else if (tool_ == 5 && !erase_) { drag_ = Drag::Spawn; }
                    else { drag_ = Drag::Paint; }
                    model_->gesture = drag_ != Drag::None;
                }
                if (drag_ != Drag::None) {
                    const auto clamped = EditorWorkspacePoint{std::clamp(p.x, 0.0f, static_cast<float>(s.region.cols) - .001f), std::clamp(p.y, 0.0f, static_cast<float>(s.region.rows) - .001f)};
                    if (drag_ == Drag::Paint) {
                        if (!rectanglePaint_) { PaintLine(last_, clamped, erase_); }
                        last_ = clamped;
                    }
                    if (drag_ == Drag::Box) { last_ = clamped; }
                    if (drag_ == Drag::Spawn) {
                        s.spawnX = (std::min)(std::floor(clamped.x) + .1875f, static_cast<float>(s.region.cols) - .625f);
                        s.spawnY = (std::min)(std::floor(clamped.y) + .125f, static_cast<float>(s.region.rows) - .875f);
                    }
                    if (auto* d = model_->devices.Selected(); d && (drag_ == Drag::Move || drag_ == Drag::Resize)) {
                        const double dx = std::round(p.x - start_.x), dy = std::round(p.y - start_.y);
                        *d = original_;
                        if (drag_ == Drag::Move) {
                            d->x = std::clamp(original_.x + dx, 0.0, s.region.cols - original_.w);
                            d->y = std::clamp(original_.y + dy, 0.0, s.region.rows - original_.h);
                        } else {
                            double left = d->x, top = d->y, right = d->x + d->w, bottom = d->y + d->h;
                            if (handle_ == 0 || handle_ == 6 || handle_ == 7) { left = std::clamp(left + dx, 0.0, right - .125); }
                            if (handle_ == 2 || handle_ == 3 || handle_ == 4) { right = std::clamp(right + dx, left + .125, static_cast<double>(s.region.cols)); }
                            if (handle_ == 0 || handle_ == 1 || handle_ == 2) { top = std::clamp(top + dy, 0.0, bottom - .125); }
                            if (handle_ == 4 || handle_ == 5 || handle_ == 6) { bottom = std::clamp(bottom + dy, top + .125, static_cast<double>(s.region.rows)); }
                            d->x = left; d->y = top; d->w = right - left; d->h = bottom - top;
                        }
                        s.devices = model_->devices.Serialize();
                    }
                    if ((input.releasedButtons & (kPrimary | kSecondary)) || (!(input.heldButtons & (kPrimary | kSecondary)) && !(input.pressedButtons & (kPrimary | kSecondary)))) {
                        if (drag_ == Drag::Paint && rectanglePaint_) {
                            for (int y = static_cast<int>(std::floor((std::min)(start_.y, last_.y))); y <= static_cast<int>(std::floor((std::max)(start_.y, last_.y))); ++y)
                                for (int x = static_cast<int>(std::floor((std::min)(start_.x, last_.x))); x <= static_cast<int>(std::floor((std::max)(start_.x, last_.x))); ++x) { PaintCell(x, y, erase_); }
                        }
                        if (drag_ == Drag::Box) {
                            const double left = (std::min)(start_.x, last_.x), right = (std::max)(start_.x, last_.x);
                            const double top = (std::min)(start_.y, last_.y), bottom = (std::max)(start_.y, last_.y);
                            for (const auto& d : model_->devices.Items()) { if (d.x < right && d.x + d.w > left && d.y < bottom && d.y + d.h > top) { groupIds_.push_back(d.id); } }
                        }
                        if (drag_ == Drag::Resize) { if (const auto* d = model_->devices.Selected()) { RememberTemplate(*d); } }
                        drag_ = Drag::None; model_->Finish(ctx);
                    }
                }
                DrawRoomMap(ui, s, view_, true, true); DrawSelection(ui);
                if ((drag_ == Drag::Paint && rectanglePaint_) || drag_ == Drag::Box) {
                    const float x0 = (std::min)(start_.x, last_.x), y0 = (std::min)(start_.y, last_.y), x1 = (std::max)(start_.x, last_.x), y1 = (std::max)(start_.y, last_.y);
                    ui.CanvasRect(view_.Screen(drag_ == Drag::Box ? x0 : std::floor(x0), drag_ == Drag::Box ? y0 : std::floor(y0)),
                                  view_.Screen(drag_ == Drag::Box ? x1 : std::floor(x1) + 1, drag_ == Drag::Box ? y1 : std::floor(y1) + 1), {.95f, .78f, .4f, 1}, 2);
                }
                for (const auto& d : model_->devices.Items()) { if (std::find(groupIds_.begin(), groupIds_.end(), d.id) != groupIds_.end()) { ui.CanvasRect(view_.Screen(static_cast<float>(d.x), static_cast<float>(d.y)), view_.Screen(static_cast<float>(d.x + d.w), static_cast<float>(d.y + d.h)), {.95f, .78f, .4f, 1}, 2); } }
                ui.CanvasRect(view_.Screen(0, 0), view_.Screen(static_cast<float>(s.region.cols), static_cast<float>(s.region.rows)), {.45f, .66f, .69f, 1}, 2);
                if (!linkSource_.empty() && tool_ == 4) {
                    const auto source = std::find_if(model_->devices.Items().begin(), model_->devices.Items().end(), [this](const auto& d) { return d.id == linkSource_; });
                    if (source != model_->devices.Items().end()) { ui.CanvasLine(view_.Screen(static_cast<float>(source->x + source->w * .5), static_cast<float>(source->y + source->h * .5)), input.pointer, {.93f, .76f, .36f, .75f}, 2); }
                }
                // 置く前の影：指しているマスに、これから置く仕掛けを薄く描く。
                if (tool_ == 3 && drag_ == Drag::None && input.hovered && !view_.panning && !hand_ && inside && (model_->devices.Hit(p.x, p.y) < 0 || input.shift)) {
                    DeviceDraft ghost; ghost.kind = kind_; ghost.x = std::floor(p.x); ghost.y = std::floor(p.y);
                    if (kind_ == "Door" || kind_ == "LockWall") { ghost.h = (std::min)(3.0, s.region.rows - ghost.y); }
                    if (kind_ == "OneWay" || kind_ == "Bridge") { ghost.w = (std::min)(3.0, s.region.cols - ghost.x); }
                    if (kind_ == "Laser") { ghost.direction = "Right"; }
                    ApplyTemplate(ghost, s.region.cols, s.region.rows);
                    DrawDeviceGlyph(ui, ghost, view_);
                    ui.CanvasRect(view_.Screen(static_cast<float>(ghost.x), static_cast<float>(ghost.y)), view_.Screen(static_cast<float>(ghost.x + ghost.w), static_cast<float>(ghost.y + ghost.h)), {.97f, .80f, .40f, .7f}, 1.5f);
                }
            } else { ui.CanvasText({20, 20}, {.85f, .4f, .4f, 1}, valid.message); }
        }
        ui.EndCanvas();
        // Delete は地図の上にマウスがある時だけ（3D ビューポートの Delete は物体を消すので、同じキーで装置まで消さない）。
        if (canvasHovered_ && model_->ready && !model_->gesture && !ui.GetInteraction().editing && ui.GetInteraction().deletePressed) {
            if (model_->devices.RemoveSelected()) { model_->draft.devices = model_->devices.Serialize(); model_->Commit(ctx); }
        }
    }
    void DrawProperties(EditorWorkspaceHostContext& ctx) {
        auto& ui = *ctx.ui; if (!model_->ready) { return; }
        auto& s = model_->draft;
        const auto before = s;
        if (tool_ == 3 && !model_->devices.HasSelection()) { ui.Text("放置 " + DeviceLabel(kind_)); ui.Text("点空格放置；点现有机关选中"); }
        if (auto* d = model_->devices.Selected(); d && tool_ != 5) {
            ui.Text(DeviceLabel(d->kind) + " " + std::to_string(model_->devices.SelectedIndex() + 1));
            if (ui.Button("td2.television.device.deselect", "返回地图属性")) { model_->devices.Select(-1); return; }
            EditFloat(ui, "td2.television.device.x", "X / 格", d->x, 0, static_cast<float>((std::max)(0.0, s.region.cols - d->w)));
            EditFloat(ui, "td2.television.device.y", "Y / 格", d->y, 0, static_cast<float>((std::max)(0.0, s.region.rows - d->h)));
            EditFloat(ui, "td2.television.device.w", "宽 / 格", d->w, .125f, static_cast<float>(s.region.cols - d->x));
            EditFloat(ui, "td2.television.device.h", "高 / 格", d->h, .125f, static_cast<float>(s.region.rows - d->y));
            if (d->kind == "Laser" || d->kind == "Spikes" || d->kind == "OneWay") {
                constexpr std::array<std::string_view, 4> directions{"Up", "Right", "Down", "Left"};
                constexpr std::array<std::string_view, 4> names{"↑", "→", "↓", "←"};
                for (size_t i = 0; i < directions.size(); ++i) { if (i) { ui.SameLine(); } if (ui.Button("td2.television.device.direction." + std::to_string(i), d->direction == directions[i] ? std::string(names[i]) + " ●" : std::string(names[i]))) { d->direction = directions[i]; } }
            }
            if (d->kind == "Laser") { EditFloat(ui, "td2.television.device.range", "射程", d->range, .125f, 128); }
            if (d->kind == "Spring") { DrawSpring(ui, *d); }
            if (d->kind == "Decal") { DrawDecal(ui, *d); }
            if (d->kind == "Laser" || d->kind == "Door" || d->kind == "Bridge") { ui.Checkbox("td2.television.device.invert", "反向激活", &d->inverted); }
            if (d->kind == "Plate") {
                ui.Checkbox("td2.television.device.latch", "踩一下切换（关：踩住时才通电）", &d->latch);
                bool reset = d->Extra("variant") == "reset";
                if (ui.Checkbox("td2.television.device.reset", "圆形复位钮外观（电视塔复位用）", &reset)) { d->SetExtra("variant", reset ? "reset" : ""); }
            }
            if (d->kind == "Plate") {
                ui.Separator(); ui.Text("连接目标（" + std::to_string(d->targets.size()) + "）");
                if (ui.Button("td2.television.device.link", "在地图上连线")) { tool_ = 4; linkSource_ = d->id; hand_ = false; }
                std::vector<int> targets;
                for (size_t i = 0; i < model_->devices.Items().size(); ++i) { const auto& target = model_->devices.Items()[i]; if (target.kind == "Door" || target.kind == "Laser" || target.kind == "Bridge") { targets.push_back(static_cast<int>(i)); } }
                if (targets.empty()) { ui.Text("本屏没有门、激光或桥"); }
                for (size_t i = 0; i < targets.size(); ++i) {
                    const int index = targets[i]; const auto& target = model_->devices.Items()[static_cast<size_t>(index)];
                    bool linked = std::find(d->targets.begin(), d->targets.end(), target.id) != d->targets.end();
                    if (ui.Checkbox("td2.television.device.target." + target.id, DeviceLabel(target.kind) + " " + std::to_string(index + 1), &linked)) { model_->devices.ToggleLink(d->id, index); }
                }
                const std::string plateId = d->id;
                DrawExternalTargets(ctx, plateId);
                d = model_->devices.Selected();
                if (!d) { s.devices = model_->devices.Serialize(); return; }
            }
            if (ui.Button("td2.television.device.duplicate", "复制")) { model_->devices.DuplicateSelected(s.region.cols, s.region.rows); }
            ui.SameLine(); if (ui.Button("td2.television.device.delete", "删除")) { model_->devices.RemoveSelected(); }
            s.devices = model_->devices.Serialize();
        } else {
            if (const auto* item = model_->Current()) { ui.Text(item->object.name); }
            const auto previous = s.region;
            EditInt(ui, "td2.television.cols", "列", s.region.cols, 1, 64);
            EditInt(ui, "td2.television.rows", "行", s.region.rows, 1, 64);
            if (!ResizeCells(previous, s.region)) { s.region = previous; model_->error = "尺寸变化失败"; }
            if (previous.cols != s.region.cols || previous.rows != s.region.rows) {
                view_.fit = true;
                const float scale = (std::min)(1.0f, 2048.0f / static_cast<float>((std::max)(s.region.cols, s.region.rows) * s.region.tilePixels));
                s.outputWidth = (std::max)(32, static_cast<int>(std::round(static_cast<float>(s.region.cols * s.region.tilePixels) * scale)));
                s.outputHeight = (std::max)(32, static_cast<int>(std::round(static_cast<float>(s.region.rows * s.region.tilePixels) * scale)));
            }
            ui.Checkbox("td2.television.entry", "开始电视", &s.entryRegion);
            if (tool_ == 5) {
                double x = s.spawnX, y = s.spawnY;
                if (EditFloat(ui, "td2.television.spawn.x", "X", x, 0, static_cast<float>(s.region.cols) - .625f)) { s.spawnX = static_cast<float>(x); }
                if (EditFloat(ui, "td2.television.spawn.y", "Y", y, 0, static_cast<float>(s.region.rows) - .875f)) { s.spawnY = static_cast<float>(y); }
            }
            if (ui.Checkbox("td2.television.screen.advanced", "屏幕绑定", &screenSettings_) && screenSettings_) { RefreshScreens(ctx); }
            if (screenSettings_) {
                const auto current = std::find_if(screens_.begin(), screens_.end(), [&s](const auto& item) { return item.objectId == s.screenObject; });
                ui.Text("当前：" + (s.screenObject == 0 ? std::string("未绑定") : current == screens_.end() ? "对象 " + std::to_string(s.screenObject) : ShortChoiceLabel(current->name)));
                double brightness = s.screenBrightness;
                if (EditFloat(ui, "td2.television.screen.brightness", "屏幕亮度", brightness, .5f, 4.0f)) { s.screenBrightness = static_cast<float>(brightness); }
                if (ui.Button("td2.television.screen.style", "屏幕风格（原生属性）")) {
                    // 標準の Inspector への入口だけを置く。共有の設定や演出カメラは複製しない。
                    std::vector<SceneItem> styles;
                    if (QueryObjects(ctx, "TD2.ScreenStyle", styles, model_->error)) {
                        const auto own = std::find_if(styles.begin(), styles.end(), [&](const auto& item) { return item.objectId == model_->selected || item.objectId == s.screenObject; });
                        const uint64_t styleObject = own != styles.end() ? own->objectId : styles.size() == 1 ? styles.front().objectId : 0;
                        if (styleObject) {
                            const auto selected = Execute(ctx, "scene.object.select", {{"objectId", styleObject}});
                            model_->error = selected.ok ? std::string{} : selected.error;
                        } else { model_->error = styles.empty() ? "尚未配置屏幕风格组件" : "有多个共享屏幕风格，请在场景中选择要调整的组件"; }
                    }
                }
                if (ui.Button("td2.television.screen.pick", screenPicker_ ? "收起屏幕列表" : "选择屏幕对象")) { screenPicker_ = !screenPicker_; }
                if (screenPicker_) {
                    EditText(ui, "td2.television.screen.search", "查找", screenSearch_);
                    if (ui.Checkbox("td2.television.screen.all", "包括无模型对象", &allScreens_)) { RefreshScreens(ctx); }
                    std::vector<EditorChoice> choices{{"td2.television.screen.none", "未绑定", s.screenObject == 0}};
                    std::vector<uint64_t> ids{0};
                    for (const auto& screen : screens_) {
                        if (!screenSearch_.empty() && screen.name.find(screenSearch_) == std::string::npos) { continue; }
                        choices.push_back({"td2.television.screen." + std::to_string(screen.objectId), screen.name, s.screenObject == screen.objectId}); ids.push_back(screen.objectId);
                    }
                    if (const int chosen = DrawChoices(ui, choices); chosen >= 0) { s.screenObject = ids[static_cast<size_t>(chosen)]; screenPicker_ = false; }
                }
                EditText(ui, "td2.television.screen.slot", "材质槽", s.screenSlot);
                if (!model_->screenWarning.empty()) { ui.Text(model_->screenWarning); }
            }
        }
        // 数値のドラッグも草稿に集め、編集の終わりに一回だけ確定する。直した形は次に置く同じ種類へ写す。
        if (s != before) { propertyPending_ = true; if (const auto* d = model_->devices.Selected()) { RememberTemplate(*d); } }
        if (propertyPending_ && !model_->gesture && !ui.GetInteraction().editing) { model_->Commit(ctx); propertyPending_ = false; }
        if (!model_->devices.HasSelection()) { DrawDeviceList(ctx); }
    }
    // 今のテレビの仕掛けの一覧（地図の属性の下）。地図で見つけにくい小さな仕掛けを選ぶのに使う。
    void DrawDeviceList(EditorWorkspaceHostContext& ctx) {
        auto& ui = *ctx.ui;
        if (model_->devices.Items().empty()) { return; }
        ui.Separator(); ui.Text("现有机关（" + std::to_string(model_->devices.Items().size()) + "）");
        EditText(ui, "td2.television.devices.search", "查找", deviceSearch_);
        std::vector<EditorChoice> choices; std::vector<int> indices;
        for (size_t i = 0; i < model_->devices.Items().size(); ++i) {
            const auto& d = model_->devices.Items()[i];
            const std::string name = DeviceLabel(d.kind) + " " + std::to_string(i + 1);
            if (!deviceSearch_.empty() && name.find(deviceSearch_) == std::string::npos && d.id.find(deviceSearch_) == std::string::npos) { continue; }
            choices.push_back({"td2.television.device.select." + d.id, name, model_->devices.SelectedIndex() == static_cast<int>(i)}); indices.push_back(static_cast<int>(i));
        }
        if (choices.empty()) { ui.Text("没有匹配的机关"); }
        else if (const int selected = DrawChoices(ui, choices); selected >= 0) { model_->devices.Select(indices[static_cast<size_t>(selected)]); ChooseTool(0); }
    }
    // バネ：強さ（高さの目安つき）・立ってから発射までの秒・冷却。既定値は書き出さない。
    static std::string ShortNumber(double v) {
        std::string s = std::to_string(std::round(v * 100) / 100);
        while (!s.empty() && s.back() == '0') { s.pop_back(); }
        if (!s.empty() && s.back() == '.') { s.pop_back(); }
        return s;
    }
    static void DrawSpring(IEditorWorkspaceUi& ui, DeviceDraft& d) {
        const auto read = [&d](std::string_view key, double fallback) {
            const auto text = d.Extra(key);
            try { return text.empty() ? fallback : std::stod(text); } catch (...) { return fallback; }
        };
        double power = read("power", 22.5), delay = read("delay", .85), cooldown = read("cooldown", 3);
        if (EditFloat(ui, "td2.television.device.power", "弹力", power, 3, 44)) { d.SetExtra("power", std::abs(power - 22.5) < 1e-6 ? "" : ShortNumber(power)); }
        ui.Text("站着发射约弹起 " + ShortNumber(power * power / 87.5) + " 格（从高处落下会更高）");
        if (EditFloat(ui, "td2.television.device.delay", "站上去多久发射（秒）", delay, .05f, 3)) { d.SetExtra("delay", std::abs(delay - .85) < 1e-6 ? "" : ShortNumber(delay)); }
        if (EditFloat(ui, "td2.television.device.cooldown", "发射后冷却（秒）", cooldown, 3, 30)) { d.SetExtra("cooldown", std::abs(cooldown - 3) < 1e-6 ? "" : ShortNumber(cooldown)); }
    }
    // 貼り絵の図案：按键の説明と出口の矢印。
    static void DrawDecal(IEditorWorkspaceUi& ui, DeviceDraft& d) {
        constexpr std::array<std::pair<std::string_view, std::string_view>, 7> variants{{{"ad", "A / D 移动"}, {"space", "空格"}, {"spaceHold", "长按空格"},
            {"exit", "出口 →"}, {"exitLeft", "出口 ←"}, {"exitUp", "出口 ↑"}, {"exitDown", "出口 ↓"}}};
        ui.Text("图案");
        const auto current = d.Extra("variant");
        for (size_t i = 0; i < variants.size(); ++i) {
            if (ui.Selectable("td2.television.device.decal." + std::string(variants[i].first), std::string(variants[i].second), current == variants[i].first)) {
                d.SetExtra("variant", variants[i].first);
            }
        }
    }
    // 圧力板の、別のテレビの装置と画面の外の仕掛けへの接続。今の接続の一覧（見つからない目標は印を付ける）も出す。
    void DrawExternalTargets(EditorWorkspaceHostContext& ctx, const std::string& plateId) {
        auto& ui = *ctx.ui;
        model_->RefreshMachines(ctx);
        const auto* self = model_->Current();
        if (!self || !model_->devices.Selected()) { return; }
        const auto here = Placement(model_->draft);
        const auto linked = [&](const std::string& target) {
            const auto* p = model_->devices.Selected();
            return p && std::find(p->targets.begin(), p->targets.end(), target) != p->targets.end();
        };
        // 別のテレビ：既定では辺を接するテレビだけ（作者が確かめやすい近い所から）。
        ui.Separator(); ui.Text("其他电视的门 / 激光 / 桥");
        ui.Checkbox("td2.television.device.remote.all", "显示所有电视（默认只显示相邻的）", &remoteAll_);
        int shown = 0;
        for (const auto& room : model_->rooms) {
            if (room.object.objectId == self->object.objectId) { continue; }
            const bool adjacent = here.worldPlaced && room.settings.worldPlaced && FindSharedWorldEdge(here, Placement(room.settings)).has_value();
            if (!remoteAll_ && !adjacent) { continue; }
            DeviceEditor other; other.Load(room.settings.devices);
            for (size_t i = 0; i < other.Items().size(); ++i) {
                const auto& device = other.Items()[i];
                if (device.kind != "Door" && device.kind != "Laser" && device.kind != "Bridge") { continue; }
                const std::string target = room.settings.region.regionId + "/" + device.id;
                bool on = linked(target);
                if (ui.Checkbox("td2.television.device.remote." + target, ShortChoiceLabel(room.object.name, 12) + " · " + DeviceLabel(device.kind) + " " + std::to_string(i + 1), &on)) {
                    model_->devices.ToggleTarget(plateId, target);
                }
                ++shown;
            }
        }
        if (!shown) { ui.Text(remoteAll_ ? "其他电视没有门、激光或桥" : "相邻电视没有门、激光或桥"); }
        ui.Text("提示：压板尽量和它控制的东西放在同一块屏幕里");
        // 画面の外の仕掛け。
        ui.Separator(); ui.Text("机关目标（转台、吊车、货架、电视塔、转柱）");
        int machines = 0;
        for (const auto& machine : model_->machines) {
            for (const auto& [target, label] : machine.Receivers()) {
                bool on = linked(target);
                if (ui.Checkbox("td2.television.device.machine." + target, label, &on)) { model_->devices.ToggleTarget(plateId, target); }
                ++machines;
            }
        }
        if (!machines) { ui.Text("场景里还没有机关（在世界地图的「机关」模式新建）"); }
        // 今の接続。どこにも見つからない目標は外せるようにする。
        const auto* current = model_->devices.Selected();
        if (!current || current->targets.empty()) { return; }
        ui.Separator(); ui.Text("当前连接");
        std::string removing;
        for (const auto& target : current->targets) {
            bool found = false;
            const auto kind = ClassifyTarget(target);
            if (kind == TargetKind::Local) {
                found = std::any_of(model_->devices.Items().begin(), model_->devices.Items().end(), [&](const auto& d) { return d.id == target; });
            } else if (kind == TargetKind::Machine) {
                for (const auto& m : model_->machines) for (const auto& r : m.Receivers()) { found = found || r.first == target; }
            } else if (kind == TargetKind::OtherRegion) {
                const auto slash = target.find('/');
                for (const auto& room : model_->rooms) {
                    if (room.settings.region.regionId != target.substr(0, slash)) { continue; }
                    DeviceEditor other; other.Load(room.settings.devices);
                    found = std::any_of(other.Items().begin(), other.Items().end(), [&](const auto& d) { return d.id == target.substr(slash + 1); });
                }
            }
            ui.Text(std::string(found ? "· " : "✕ 找不到：") + target);
            ui.SameLine();
            if (ui.Button("td2.television.device.unlink." + target, "移除")) { removing = target; }
        }
        if (!removing.empty()) { model_->devices.ToggleTarget(plateId, removing); }
    }
    void RefreshScreens(EditorWorkspaceHostContext& ctx) {
        screens_.clear(); uint64_t offset = 0;
        // 初期の候補はモデルを持つ物体に絞る（灯や記録用の物体で保存欄を押し出さない）。
        for (int page = 0; page < 256; ++page) {
            const auto result = Execute(ctx, "scene.objects.query", {{"detail", true}, {"offset", offset}, {"limit", 128}});
            if (!result.ok) { model_->error = result.error; return; }
            for (const auto& object : result.data.value("objects", Json::array())) {
                const auto id = ObjectId(object.value("objectId", Json{}));
                const bool model = object.contains("model") && object["model"].is_object() &&
                    (!object["model"].value("assetId", "").empty() || object["model"].value("materialSlotCount", 0) > 0);
                if (!id || (!allScreens_ && id != model_->draft.screenObject && !model)) { continue; }
                screens_.push_back({id, object.value("name", "对象 " + std::to_string(id))});
            }
            const auto next = ObjectId(result.data.value("nextOffset", Json{}));
            if (!next || next <= offset) { break; } offset = next;
        }
    }
    Drag drag_ = Drag::None;
    EditorWorkspacePoint start_{}, last_{};
    DeviceDraft original_;
    int tool_ = 0, handle_ = -1, roomPage_ = 0, devicePage_ = 0, targetPage_ = 0, brushSize_ = 1;
    bool erase_ = false, propertyPending_ = false, screenSettings_ = false, screenPicker_ = false, allScreens_ = false;
    bool gallery_ = false;
    bool rectanglePaint_ = false, remoteAll_ = false;
    std::set<size_t> erasedDeviceCells_;
    std::string kind_ = "Plate", linkSource_, deviceSearch_, gallerySearch_;
    std::string screenSearch_;
    int screenPage_ = 0;
    std::vector<std::string> groupIds_;
    std::vector<DeviceDraft> clipboard_;
    std::vector<SceneItem> screens_;
    std::map<std::string, DeviceDraft, std::less<>> templates_;
    uint64_t shown_ = 0;
    bool canvasHovered_ = false;
};

struct EdgeSelection { uint64_t a = 0, b = 0; SharedWorldEdge edge; };
struct SequenceAsset { std::string guid, name; std::vector<std::string> slots; bool slotsKnown = false; };

// 世界地図は既存テレビの整数原点だけを編集する。
class WorldMode final : public MapMode {
public:
    using MapMode::MapMode;
    bool IsDirty() const override { return MapMode::IsDirty() || (sequenceDraft_ && pendingBinding_ != observedBinding_); }
    void DiscardChanges(EditorWorkspaceHostContext& ctx) override { sequenceDraft_ = false; bindingRefresh_ = true; MapMode::DiscardChanges(ctx); }
    void OnActivate(EditorWorkspaceHostContext& ctx) override { MapMode::OnActivate(ctx); assetsRefresh_ = true; bindingRefresh_ = true; layoutStale_ = true; view_.fit = true; }
    void OnDeactivate(EditorWorkspaceHostContext& ctx) override {
        if (posing_) { CancelPose(ctx); }
        if (libraryDrag_) { model_->Discard(); } else { MapMode::OnDeactivate(ctx); }
        moving_ = false; libraryDrag_ = false; pickingStart_ = false; groupMoving_ = false; boxing_ = false; ghost_ = -1;
    }
    bool ApplyDraft(EditorWorkspaceHostContext& ctx) override {
        if (posing_) { CancelPose(ctx); }
        if (sequenceDraft_ && pendingBinding_ != observedBinding_ && !WriteBinding(ctx, pendingBinding_)) { return false; }
        sequenceDraft_ = false; return MapMode::ApplyDraft(ctx);
    }
    // 見方（配置・カメラ大区域・俯瞰カメラ・仕掛け）を変える。
    void ChangeMode(EditorWorkspaceHostContext& ctx, int mode) { SetMode(ctx, mode); }
    int Mode() const { return mode_; }
    // 新しく置いたテレビのカメラを作る（隣のテレビが大区域に入っていれば同じ大区域へ入れてから）。
    void SetUpNewRoom(EditorWorkspaceHostContext& ctx, const std::string& regionId, const std::string& besideRegion) {
        if (!besideRegion.empty() && EnsureLayout(ctx, true)) {
            if (const auto* area = layout_.AreaOf(besideRegion)) {
                const std::string name = area->name; areaFocus_ = name;
                ToggleArea(ctx, regionId);
            }
        }
        Generate(ctx, true, false, {}, regionId, nullptr, false);
    }
    void BeginFrame(EditorWorkspaceHostContext& ctx) override {
        clock_ += ctx.unscaledDeltaSeconds;
        dragHeldAny_ = dragPressedAny_ = dragReleasedAny_ = false;
    }
    // 左：保存と履歴・テレビの一覧（世界へドラッグして置く）・選んだものの属性。一覧→地図の順でドラッグの入力をまとめる。
    void DrawSide(EditorWorkspaceHostContext& ctx) override {
        auto& ui = *ctx.ui;
        HistoryToolbar(ctx);
        const int unplaced = static_cast<int>(std::count_if(model_->rooms.begin(), model_->rooms.end(), [](const auto& r) { return !r.settings.worldPlaced; }));
        if (ui.Button("td2.world.library.toggle", libraryOpen_ ? "收起电视列表" : "电视列表" + (unplaced ? "（" + std::to_string(unplaced) + " 台未放进世界）" : std::string()))) { libraryOpen_ = !libraryOpen_; }
        if (libraryOpen_ || libraryDrag_) { DrawLibrary(ctx); ui.Separator(); }
        if (!report_.empty()) { ui.Text(report_); }
        DrawProperties(ctx);
    }
    // 右：見方の切り替え・一行の説明・地図（俯瞰カメラの時は上から見た図）。
    void DrawMap(EditorWorkspaceHostContext& ctx) override {
        auto& ui = *ctx.ui;
        ModeBar(ctx, mode_);
        ViewControls(ctx);
        // 説明は短い行に分ける（欄の幅で切れないように）。
        if (pickingStart_) {
            ui.Text("点击角色脚下的位置开始游玩\n空中也可选 · 右键取消");
            if (ui.Button("td2.world.spawn.cancel", "取消选择出生位置")) { pickingStart_ = false; }
            if (!model_->error.empty()) { ui.Text(model_->error); }
        } else if (mode_ == 1) {
            ui.Text(areaFocus_.empty() ? "在左栏选择或新建一个大区域\n再点击电视加入 / 移出" : "点击电视加入或移出\n「" + areaFocus_ + "」");
        } else if (mode_ == 2) {
            ui.Text("点电视或机位选中\n在左栏生成、调整机位");
        } else if (mode_ == 3) {
            const auto* m = SelectedMachine();
            ui.Text(!m ? "在左栏选择或新建机关\n点电视可找到它所属的机关"
                : m->kind == MachineKind::Mechanism ? "点电视加入 / 移出状态" + std::string(machineC_ ? "C" : "B") + "的区域\n拖动虚线框设定它在该状态的位置"
                : m->kind == MachineKind::Slide ? "点电视加入 / 移出货架成员\n点空着的格子设为空槽"
                : m->kind == MachineKind::Stack ? "点电视加入 / 移出电视塔\n自动按自下而上排序"
                : "点电视加入 / 移出转柱的面\n顺序在左栏调整");
        } else if (!multi_.empty()) {
            ui.Text("多选 " + std::to_string(multi_.size()) + " 台 · 拖动其中一台整体平移\nShift+点击增减 · 点空白处取消");
        } else {
            ui.Text("拖动电视移动 · 双击打开电视地图 · 从左栏列表拖入\nShift+点击多选 · 空白处拖框 · Delete 移出世界");
        }
        if (mode_ != 0) { EnsureLayout(ctx); }
        if (mode_ == 2) { DrawPlan(ctx); } else { DrawCanvas(ctx); }
    }
private:
    bool LeaveSequence(EditorWorkspaceHostContext& ctx) {
        // 文脈が変わっても草稿は捨てない。無効な草稿は右欄で取り消せるよう選択を保つ。
        if (sequenceDraft_ && pendingBinding_ != observedBinding_ && !WriteBinding(ctx, pendingBinding_)) { return false; }
        sequenceDraft_ = false; edge_ = {}; picker_ = 0; bindingRefresh_ = true; return true;
    }
    bool ChooseEdge(EditorWorkspaceHostContext& ctx, const EdgeSelection& next) {
        if (next.a == edge_.a && next.b == edge_.b) { return true; }
        if (!LeaveSequence(ctx)) { return false; }
        edge_ = next; bindingRefresh_ = true; return true;
    }
    void OpenTelevision(EditorWorkspaceHostContext& ctx, uint64_t id) { if (LeaveSequence(ctx) && model_->Select(ctx, id) && model_->Commit(ctx) && switcher) { switcher(kTelevisionView); } }
    // テレビの一覧。まだ世界に置いていないテレビを先に並べる。押したまま地図へドラッグすると置ける。ダブルクリックでテレビ地図を開く。
    void DrawLibrary(EditorWorkspaceHostContext& ctx) {
        auto& ui = *ctx.ui;
        {
            if (ui.Button("td2.world.add_selected", "选中电视建图") && LeaveSequence(ctx)) { model_->AddSelectedTelevision(ctx); OpenTelevision(ctx, model_->selected); }
            ui.SameLine();
            if (ui.Button("td2.world.new", "+ 空白地图") && LeaveSequence(ctx)) { model_->Create(ctx); OpenTelevision(ctx, model_->selected); }
            EditText(ui, "td2.world.rooms.search", "查找", librarySearch_);
            auto rooms = model_->rooms;
            std::stable_partition(rooms.begin(), rooms.end(), [](const auto& r) { return !r.settings.worldPlaced; });
            for (size_t i = 0; i < rooms.size(); ++i) {
                const auto& room = rooms[i];
                if (!librarySearch_.empty() && room.object.name.find(librarySearch_) == std::string::npos &&
                    room.settings.region.regionId.find(librarySearch_) == std::string::npos) { continue; }
                ui.Text((room.settings.worldPlaced ? "" : "○ ") + ShortChoiceLabel(room.object.name) + "  " + std::to_string(room.settings.region.cols) + "×" + std::to_string(room.settings.region.rows));
                const auto input = ui.BeginCanvas("td2.world.thumbnail." + std::to_string(room.object.objectId), 64);
                dragHeldAny_ |= (input.heldButtons & kPrimary) != 0;
                dragPressedAny_ |= (input.pressedButtons & kPrimary) != 0;
                dragReleasedAny_ |= (input.releasedButtons & kPrimary) != 0;
                DrawThumbnail(ui, room, input);
                if (input.hovered && (input.pressedButtons & kPrimary)) {
                    pickingStart_ = false;
                    if (LeaveSequence(ctx) && model_->Select(ctx, room.object.objectId)) {
                        if (lastClick_ == room.object.objectId && clock_ - lastClickTime_ < .3f) { OpenTelevision(ctx, room.object.objectId); lastClick_ = 0; }
                        else {
                            libraryDrag_ = true; moving_ = false; model_->gesture = true; edge_ = {}; offset_ = {static_cast<float>(model_->draft.region.cols) * .5f, static_cast<float>(model_->draft.region.rows) * .5f};
                            lastClick_ = room.object.objectId; lastClickTime_ = clock_;
                        }
                    }
                }
                ui.EndCanvas();
            }
        }
    }
    std::vector<EdgeSelection> Edges() const {
        std::vector<EdgeSelection> result;
        for (size_t a = 0; a < model_->rooms.size(); ++a) for (size_t b = a + 1; b < model_->rooms.size(); ++b) {
            const auto& ra = model_->rooms[a]; const auto& rb = model_->rooms[b];
            const auto sa = ra.object.objectId == model_->selected && model_->gesture ? model_->draft : ra.settings;
            const auto sb = rb.object.objectId == model_->selected && model_->gesture ? model_->draft : rb.settings;
            if (const auto edge = FindSharedWorldEdge(Placement(sa), Placement(sb))) { result.push_back({ra.object.objectId, rb.object.objectId, *edge}); }
        }
        return result;
    }
    std::pair<EditorWorkspacePoint, EditorWorkspacePoint> EdgePoints(const SharedWorldEdge& e) const {
        const float c = static_cast<float>(e.coordinate), lo = static_cast<float>(e.overlapBegin), hi = static_cast<float>(e.overlapEnd);
        const bool vertical = e.sideA == WorldSide::Left || e.sideA == WorldSide::Right;
        return vertical ? std::pair{view_.Screen(c, lo), view_.Screen(c, hi)} : std::pair{view_.Screen(lo, c), view_.Screen(hi, c)};
    }
    void FitWorld(EditorWorkspacePoint size) {
        bool any = false; WorldGridRect bounds{};
        for (const auto& r : model_->rooms) if (r.settings.worldPlaced) {
            const auto rect = WorldBounds(Placement(r.settings));
            if (!any) { bounds = rect; any = true; }
            else { bounds.left = (std::min)(bounds.left, rect.left); bounds.top = (std::min)(bounds.top, rect.top); bounds.right = (std::max)(bounds.right, rect.right); bounds.bottom = (std::max)(bounds.bottom, rect.bottom); }
        }
        view_.Fit(size, static_cast<float>(bounds.left), static_cast<float>(bounds.top), any ? static_cast<float>(bounds.right - bounds.left) : 32.0f, any ? static_cast<float>(bounds.bottom - bounds.top) : 18.0f);
    }
    void DrawCanvas(EditorWorkspaceHostContext& ctx) {
        auto& ui = *ctx.ui;
        const auto input = ui.BeginCanvas("td2.world.canvas", 0);
        canvasHovered_ = input.hovered != 0;
        view_.ObserveSize(input.size);
        dragHeldAny_ |= (input.heldButtons & kPrimary) != 0;
        dragPressedAny_ |= (input.pressedButtons & kPrimary) != 0;
        dragReleasedAny_ |= (input.releasedButtons & kPrimary) != 0;
        if (input.size.x > 0 && input.size.y > 0) {
            if (view_.fit && (!model_->gesture || !view_.hasFit)) { FitWorld(input.size); }
            view_.Navigate(input, !pickingStart_ && hand_, moving_ || libraryDrag_);
            DrawCanvasGrid(ui, view_, input.size);
            const auto p = view_.World(input.pointer);
            const bool geometricInside = PointIn(input.pointer, {0, 0}, input.size);
            if (mode_ == 1 && !pickingStart_ && !hand_ && !view_.panning && input.hovered && (input.pressedButtons & kPrimary)) {
                for (auto it = model_->rooms.rbegin(); it != model_->rooms.rend(); ++it) {
                    if (ContainsWorldPoint(Placement(it->settings), {p.x, p.y})) { ToggleArea(ctx, it->settings.region.regionId); break; }
                }
            }
            if (mode_ == 3 && !pickingStart_ && !hand_ && !view_.panning && input.hovered && (input.pressedButtons & kPrimary)) { MachinePress(ctx, p); }
            if (mode_ == 3) { MachineDrag(ctx, input, p); }
            if (mode_ == 0) { GroupDrag(ctx, input, p); }
            if (mode_ == 0 && !pickingStart_ && !moving_ && !libraryDrag_ && !groupMoving_ && !boxing_ && !hand_ && !view_.panning && input.hovered &&
                (input.pressedButtons & kPrimary) && GroupPress(ctx, input, p)) {}
            else if (mode_ == 0 && !pickingStart_ && !moving_ && !libraryDrag_ && !groupMoving_ && !boxing_ && !hand_ && !view_.panning && input.hovered && (input.pressedButtons & kPrimary)) {
                bool hitEdge = false;
                for (const auto& e : Edges()) {
                    const auto [a, b] = EdgePoints(e.edge);
                    const float dx = b.x - a.x, dy = b.y - a.y;
                    const float t = std::clamp(((input.pointer.x - a.x) * dx + (input.pointer.y - a.y) * dy) / (std::max)(.001f, dx * dx + dy * dy), 0.0f, 1.0f);
                    if (std::hypot(input.pointer.x - a.x - t * dx, input.pointer.y - a.y - t * dy) < 6) { ChooseEdge(ctx, e); hitEdge = true; break; }
                }
                if (!hitEdge) {
                    for (auto it = model_->rooms.rbegin(); it != model_->rooms.rend(); ++it) {
                        if (!ContainsWorldPoint(Placement(it->settings), {p.x, p.y})) { continue; }
                        const auto id = it->object.objectId;
                        if (!LeaveSequence(ctx) || !model_->Select(ctx, id)) { break; }
                        if (lastClick_ == id && clock_ - lastClickTime_ < .3f) { OpenTelevision(ctx, id); lastClick_ = 0; }
                        else {
                            moving_ = true; model_->gesture = true; offset_ = {p.x - static_cast<float>(model_->draft.worldX), p.y - static_cast<float>(model_->draft.worldY)};
                            lastClick_ = id; lastClickTime_ = clock_;
                        }
                        break;
                    }
                }
            }
            if ((moving_ || libraryDrag_) && model_->ready) {
                // ドラッグ元が動いている間は落とし先のホバーを抑える。プレビュー範囲はローカルのポインタで判定する。
                if (moving_ || geometricInside) {
                    model_->draft.worldPlaced = true;
                    constexpr float limit = 1000000;
                    model_->draft.worldX = static_cast<int>(std::round(std::clamp(p.x - offset_.x, -limit, limit)));
                    model_->draft.worldY = static_cast<int>(std::round(std::clamp(p.y - offset_.y, -limit, limit)));
                }
                // ドラッグ元を先に描き、落とし先の処理の後で終了を判定する。
                if (dragReleasedAny_ || (!dragHeldAny_ && !dragPressedAny_)) {
                    const bool accept = moving_ || geometricInside;
                    moving_ = false; libraryDrag_ = false;
                    if (accept) { model_->Finish(ctx, true); }
                    else { model_->Discard(); }
                }
            }
            for (const auto& room : model_->rooms) {
                auto s = room.object.objectId == model_->selected && model_->ready ? model_->draft : room.settings;
                if (!s.worldPlaced) { continue; }
                // 複数選択の平行移動の途中は、動かした先に描く。
                if (groupMoving_ && multi_.contains(room.object.objectId)) { s.worldX += groupDx_; s.worldY += groupDy_; }
                MapCanvasView local = view_; local.origin = view_.Screen(static_cast<float>(s.worldX), static_cast<float>(s.worldY));
                DrawRoomMap(ui, s, local, view_.tile >= 16, false);
                const auto a = local.Screen(0, 0), b = local.Screen(static_cast<float>(s.region.cols), static_cast<float>(s.region.rows));
                if (layoutValid_) {
                    const int area = AreaIndex(s.region.regionId);
                    if (area >= 0) {
                        auto tint = AreaColor(area); tint.a = mode_ == 1 ? .26f : .10f;
                        ui.CanvasFill(a, b, tint);
                        if (mode_ == 1 && view_.tile >= 6) {
                            auto label = AreaColor(area); label.a = 1;
                            ui.CanvasText({a.x + 4, b.y - 18}, label, ShortChoiceLabel(layout_.areas[static_cast<size_t>(area)].name, 10));
                        }
                    }
                }
                const bool selected = (room.object.objectId == model_->selected && edge_.a == 0) || multi_.contains(room.object.objectId);
                const bool invalid = model_->gesture && selected && !model_->Validate(s).empty();
                ui.CanvasRect(a, b, invalid ? EditorWorkspaceColor{.92f, .36f, .38f, 1} : selected ? EditorWorkspaceColor{.93f, .78f, .4f, 1} : EditorWorkspaceColor{.32f, .49f, .53f, 1}, selected ? 3.0f : 1.0f);
                if (selected && view_.tile >= 16) { ui.CanvasText({a.x + 5, a.y + 4}, {.9f, .94f, .94f, 1}, room.object.name); }
            }
            bool selectedEdgeExists = false;
            for (const auto& e : Edges()) {
                const auto [a, b] = EdgePoints(e.edge);
                const bool selected = e.a == edge_.a && e.b == edge_.b;
                if (selected) { edge_ = e; selectedEdgeExists = true; }
                const bool vertical = e.edge.sideA == WorldSide::Left || e.edge.sideA == WorldSide::Right;
                const bool hovered = input.hovered && (vertical ? std::abs(input.pointer.x - a.x) <= 5 && input.pointer.y >= a.y && input.pointer.y <= b.y : std::abs(input.pointer.y - a.y) <= 5 && input.pointer.x >= a.x && input.pointer.x <= b.x);
                if (selected || hovered) { ui.CanvasLine(a, b, selected ? EditorWorkspaceColor{.97f, .79f, .39f, 1} : EditorWorkspaceColor{.55f, .74f, .72f, .75f}, selected ? 4.0f : 2.0f); }
                else if (layoutValid_) { DrawEdgeKind(ui, e, a, b); }
            }
            if (edge_.a && !selectedEdgeExists) {
                if (sequenceDraft_) { model_->error = "该相邻区域已移动；请撤回演出草稿"; }
                else { edge_ = {}; bindingRefresh_ = true; }
            }
            if (mode_ == 3) { DrawMachineOverlay(ui); }
            if (boxing_) { ui.CanvasRect(view_.Screen((std::min)(boxStart_.x, p.x), (std::min)(boxStart_.y, p.y)), view_.Screen((std::max)(boxStart_.x, p.x), (std::max)(boxStart_.y, p.y)), {.95f, .78f, .4f, 1}, 1.5f); }
            DrawSpawnPicker(ctx, input, p);
        }
        ui.EndCanvas();
        // 配置の見方だけで、選んだテレビを世界から外す（ほかの見方の Delete では外さない）。
        // 地図の上にマウスがある時だけ（3D ビューポートの Delete は物体を消すので、同じキーでテレビまで外さない）。
        if (canvasHovered_ && mode_ == 0 && multi_.empty() && !pickingStart_ && !model_->gesture && !ui.GetInteraction().editing && ui.GetInteraction().deletePressed && edge_.a == 0 && model_->ready) { model_->draft.worldPlaced = false; model_->Commit(ctx); }
    }
    // 配置のドラッグと試遊のクリックは排他的に扱う。マウスは人物の足元中央を指し、半マスへ丸めない。
    void DrawSpawnPicker(EditorWorkspaceHostContext& ctx, const EditorWorkspaceCanvasInput& input, EditorWorkspacePoint point) {
        if (!pickingStart_ || !input.hovered || view_.panning) { return; }
        if (input.pressedButtons & kSecondary) { pickingStart_ = false; return; }
        for (const auto& room : model_->rooms) {
            const auto& s = room.settings;
            if (!ContainsWorldPoint(Placement(s), {point.x, point.y})) { continue; }
            const PreviewSpawn spawn{double(point.x) - s.worldX - .3125, double(point.y) - s.worldY - .875};
            bool clear = spawn.x >= 0 && spawn.y >= 0 && spawn.x + .625 <= s.region.cols && spawn.y + .875 <= s.region.rows;
            // 壁の中は赤い枠で示し、別の場所へは移さない。空中や仕掛けの近くは自由に指定できる。
            if (clear && ValidateRegion(s.region).valid) {
                for (int y = static_cast<int>(std::floor(spawn.y)); y < static_cast<int>(std::ceil(spawn.y + .875)); ++y)
                    for (int x = static_cast<int>(std::floor(spawn.x)); x < static_cast<int>(std::ceil(spawn.x + .625)); ++x)
                        if (s.region.cells[CellIndex(s.region, x, y)] == '1') { clear = false; }
            } else { clear = false; }
            const auto a = view_.Screen(point.x - .3125f, point.y - .875f), b = view_.Screen(point.x + .3125f, point.y);
            const EditorWorkspaceColor color = clear ? EditorWorkspaceColor{.4f, 1, .7f, 1} : EditorWorkspaceColor{1, .3f, .3f, 1};
            ctx.ui->CanvasRect(a, b, color, 2);
            ctx.ui->CanvasCircle(view_.Screen(point.x, point.y - .7f), view_.tile * .13f, color, 2);
            if (input.pressedButtons & kPrimary) {
                if (!clear) { model_->error = "这里放不下角色，请在区域内的空处点击；也可以选空中"; return; }
                const auto entry = EncodePreviewSpawn(spawn);
                const auto started = Execute(ctx, "play.preview", {{"spawnPointId", entry}, {"userData", room.object.objectId}});
                if (started.ok) { pickingStart_ = false; model_->error.clear(); }
                else {
                    model_->error = started.error.find("OPERATION_NOT_FOUND") != std::string::npos
                        ? "当前引擎尚未更新：请保存并关闭编辑器，完成引擎构建后重新打开，才能从点击位置开始游玩。"
                        : started.error;
                }
            }
            return;
        }
    }
    void DrawProperties(EditorWorkspaceHostContext& ctx);
    // ---- カメラ（区域カメラ・大区域・切り替え演出）の作者編集 ----
    void SetMode(EditorWorkspaceHostContext& ctx, int mode) {
        if (mode == mode_) { return; }
        if (!LeaveSequence(ctx) || !ApplyDraft(ctx)) { return; }
        if (posing_) { CancelPose(ctx); }
        mode_ = mode; layoutStale_ = true; pickingStart_ = false; multi_.clear(); ghost_ = -1; libraryOpen_ = mode == 0;
        if (mode_ == 3) { model_->machinesStale = true; checkClock_ = 10; }
    }
    bool EnsureLayout(EditorWorkspaceHostContext& ctx, bool force = false) {
        layoutClock_ += ctx.unscaledDeltaSeconds;
        // 他の操作（取り消しや標準の Inspector など）の変更も拾うよう、見ている間は時々読み直す。
        if (!force && !layoutStale_ && layoutRevision_ == model_->revision && layoutClock_ < 2.f) { return layoutValid_; }
        layoutClock_ = 0; layoutStale_ = false; layoutRevision_ = model_->revision;
        std::string error;
        layoutValid_ = LoadLayout(ctx, layout_, error);
        if (!layoutValid_) { model_->error = error; return false; }
        // 俯瞰図の切り替えの経路（今のカメラ位置から）。
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
    int AreaIndex(std::string_view regionId) const {
        for (size_t i = 0; i < layout_.areas.size(); ++i) {
            const auto& r = layout_.areas[i].regions;
            if (std::find(r.begin(), r.end(), regionId) != r.end()) { return static_cast<int>(i); }
        }
        return -1;
    }
    static EditorWorkspaceColor AreaColor(int index) {
        // 抑えた色数：琥珀・青緑・菫・砂・青・苔。
        constexpr std::array<EditorWorkspaceColor, 6> colors{{{.93f, .72f, .36f, 1}, {.36f, .78f, .74f, 1}, {.68f, .55f, .92f, 1},
                                                              {.82f, .74f, .56f, 1}, {.42f, .62f, .92f, 1}, {.60f, .78f, .45f, 1}}};
        return colors[static_cast<size_t>(index) % colors.size()];
    }
    const LayoutEdge* LayoutEdgeOf(const EdgeSelection& e) const {
        const auto ra = std::find_if(model_->rooms.begin(), model_->rooms.end(), [&](const auto& r) { return r.object.objectId == e.a; });
        const auto rb = std::find_if(model_->rooms.begin(), model_->rooms.end(), [&](const auto& r) { return r.object.objectId == e.b; });
        if (ra == model_->rooms.end() || rb == model_->rooms.end()) { return nullptr; }
        const auto& ia = ra->settings.region.regionId; const auto& ib = rb->settings.region.regionId;
        for (const auto& edge : layout_.edges) { if ((edge.a == ia && edge.b == ib) || (edge.a == ib && edge.b == ia)) { return &edge; } }
        return nullptr;
    }
    void DrawEdgeKind(IEditorWorkspaceUi& ui, const EdgeSelection& e, EditorWorkspacePoint a, EditorWorkspacePoint b) const {
        const auto* edge = LayoutEdgeOf(e);
        if (!edge) { return; }
        if (!edge->passable) { ui.CanvasLine(a, b, {.32f, .36f, .38f, .7f}, 1.0f); return; }   // 壁で隔てた辺（越えられない）
        if (edge->between) {
            // 大区域どうし：演出が流れる。演出がまだ無い辺は赤みの点で知らせる。
            const EditorWorkspaceColor color = edge->sequenceGuid.empty() ? EditorWorkspaceColor{.95f, .45f, .40f, .95f} : EditorWorkspaceColor{.97f, .79f, .39f, .9f};
            ui.CanvasLine(a, b, color, 3.0f);
            ui.CanvasCircle({(a.x + b.x) * .5f, (a.y + b.y) * .5f}, 4.5f, color, 2.0f);
            return;
        }
        // 大区域の中：点線（カメラは自動で追従し、演出は流れない）。
        const float dx = b.x - a.x, dy = b.y - a.y, length = std::hypot(dx, dy);
        for (float t = 0; t < length; t += 10) {
            const float t1 = (std::min)(t + 5, length);
            ui.CanvasLine({a.x + dx * t / length, a.y + dy * t / length}, {a.x + dx * t1 / length, a.y + dy * t1 / length}, {.45f, .70f, .68f, .65f}, 1.5f);
        }
    }
    void ShowReport(const std::string& title, const GenerateReport& report) {
        report_ = title + "：写入 " + std::to_string(report.written) + "，保留手调 " + std::to_string(report.kept) +
                  (report.failed ? "，失败 " + std::to_string(report.failed) : std::string());
        for (size_t i = 0; i < report.notes.size() && i < 6; ++i) { report_ += "\n· " + report.notes[i]; }
    }
    // 生成は途中で layout_ を何度も読み直す。呼び手が layout_ の中を指す名前を渡すことがあるので、先に値で写す。
    // 呼び手も生成の後は layout_ の中を指すものを使わずに戻ること。
    void Generate(EditorWorkspaceHostContext& ctx, bool cameras, bool travels, std::string_view areaName, std::string_view roomId,
                  const LayoutEdge* onlyEdge, bool force) {
        const std::string area(areaName), room(roomId);
        const std::optional<LayoutEdge> edgeCopy = onlyEdge ? std::optional<LayoutEdge>(*onlyEdge) : std::nullopt;
        const LayoutEdge* edge = edgeCopy ? &*edgeCopy : nullptr;
        if (!ApplyDraft(ctx) || !EnsureLayout(ctx, true)) { return; }
        std::string error; GenerateReport cameraReport, travelReport;
        // 画面の寸法はテレビの型から自動で決める。
        if (!SyncScreenGeometry(ctx, layout_, cameraReport, error)) { model_->error = error; return; }
        cameraReport = {};
        if (cameras && !GenerateCameras(ctx, layout_, area, room, force, cameraReport, error)) { model_->error = error; return; }
        if (travels && !GenerateTravels(ctx, layout_, area, edge, force, travelReport, error)) { model_->error = error; return; }
        GenerateReport merged;
        merged.written = cameraReport.written + travelReport.written; merged.kept = cameraReport.kept + travelReport.kept;
        merged.failed = cameraReport.failed + travelReport.failed;
        merged.notes = cameraReport.notes; merged.notes.insert(merged.notes.end(), travelReport.notes.begin(), travelReport.notes.end());
        ShowReport(cameras && travels ? "机位与演出" : cameras ? "机位" : "演出", merged);
        model_->sceneDirty = true; model_->error.clear(); layoutStale_ = true; model_->refresh = true;
    }
    void ToggleArea(EditorWorkspaceHostContext& ctx, const std::string& regionId) {
        if (!EnsureLayout(ctx, true)) { return; }
        const auto focus = std::find_if(layout_.areas.begin(), layout_.areas.end(), [this](const auto& a) { return a.name == areaFocus_; });
        if (focus == layout_.areas.end()) { model_->error = "先在右栏选择一个大区域"; return; }
        // 選んだ大区域に入っていれば外し、入っていなければ入れる（他の大区域からは外す）。一回の取り消しにまとめる。
        Json commands = Json::array();
        const bool inside = std::find(focus->regions.begin(), focus->regions.end(), regionId) != focus->regions.end();
        for (const auto& area : layout_.areas) {
            auto regions = area.regions;
            const auto found = std::find(regions.begin(), regions.end(), regionId);
            if (&area == &*focus) { if (inside) { regions.erase(found); } else { regions.push_back(regionId); } }
            else if (found != regions.end()) { regions.erase(found); }
            else { continue; }
            std::string joined;
            for (const auto& r : regions) { if (!joined.empty()) { joined += ","; } joined += r; }
            commands.push_back({{"objectId", area.objectId}, {"componentId", area.componentId}, {"propertyId", "regions"}, {"value", joined}});
        }
        const auto written = Execute(ctx, "authoring.transaction", {{"commands", commands}});
        if (!written.ok) { model_->error = written.error; return; }
        model_->sceneDirty = true; model_->error.clear(); layoutStale_ = true;
    }
    void DrawAreaPanel(EditorWorkspaceHostContext& ctx) {
        auto& ui = *ctx.ui;
        if (!layoutValid_) { ui.Text("正在读取镜头布局…"); return; }
        ui.Text("镜头大区域");
        for (size_t i = 0; i < layout_.areas.size(); ++i) {
            const auto& area = layout_.areas[i];
            if (ui.Selectable("td2.world.area." + std::to_string(area.objectId), "■ " + ShortChoiceLabel(area.name, 14) + "  " + std::to_string(area.regions.size()) + " 台", area.name == areaFocus_)) {
                areaFocus_ = area.name;
            }
        }
        if (ui.Button("td2.world.area.new", "+ 新建大区域")) {
            std::string error;
            const auto name = "镜头大区域 " + std::to_string(layout_.areas.size() + 1);
            if (CreateArea(ctx, name, {}, error)) { areaFocus_ = name; layoutStale_ = true; model_->sceneDirty = true; } else { model_->error = error; }
        }
        // 大区域に入っていないテレビは、一台だけの大区域として扱う（出入りに演出が流れる）。
        int loose = 0;
        for (const auto& room : layout_.rooms) { if (room.placed && AreaIndex(room.regionId) < 0) { ++loose; } }
        if (loose) { ui.Text(std::to_string(loose) + " 台电视不属于任何大区域"); }
        ui.Separator();
        const auto focus = std::find_if(layout_.areas.begin(), layout_.areas.end(), [this](const auto& a) { return a.name == areaFocus_; });
        if (focus != layout_.areas.end()) {
            ui.Text(focus->name);
            if (ui.Button("td2.world.area.inspect", "全部参数（检视面板）")) {
                const auto selected = Execute(ctx, "scene.object.select", {{"objectId", focus->objectId}});
                if (!selected.ok) { model_->error = selected.error; }
            }
            // カメラの構図：画面の占める高さ・出口側へ斜めに撮る角度・画面より上げる高さ。変えたらカメラの生成で反映する。
            double fill = focus->frameFill, orbit = focus->orbitDegrees, lift = focus->cameraLift;
            bool changed = EditFloat(ui, "td2.world.area.fill", "屏幕占画面高度", fill, .15f, .8f);
            changed |= EditFloat(ui, "td2.world.area.orbit", "向出口侧斜拍（度）", orbit, 0, 40);
            changed |= EditFloat(ui, "td2.world.area.lift", "机位高于屏幕（米）", lift, -1, 2);
            if (changed) { areaDraft_ = {fill, orbit, lift}; areaDraftPending_ = true; areaDraftObject_ = focus->objectId; areaDraftComponent_ = focus->componentId; }
            if (areaDraftPending_ && !ui.GetInteraction().editing) {
                const auto set = [this](std::string_view id, double v) { return Json{{"objectId", areaDraftObject_}, {"componentId", areaDraftComponent_}, {"propertyId", id}, {"value", v}}; };
                const auto written = Execute(ctx, "authoring.transaction", {{"commands", {set("frameFill", areaDraft_[0]), set("orbitDegrees", areaDraft_[1]), set("cameraLift", areaDraft_[2])}}});
                if (!written.ok) { model_->error = written.error; }
                areaDraftPending_ = false; layoutStale_ = true; model_->sceneDirty = true;
            }
            // 生成の後は layout_ が作り直されて focus が指す先も消えるので、このフレームの残りは描かずに戻る。
            if (ui.Button("td2.world.area.cameras", "按当前布局生成机位")) { Generate(ctx, true, false, focus->name, {}, nullptr, false); return; }
            if (ui.Button("td2.world.area.travels", "生成出入本区域的切换演出")) { Generate(ctx, false, true, focus->name, {}, nullptr, false); return; }
            if (ui.Button("td2.world.area.force", "恢复自动（覆盖本区域的手调）")) { Generate(ctx, true, true, focus->name, {}, nullptr, true); return; }
        }
        ui.Separator();
        int dead = 0;
        for (const auto& edge : layout_.edges) { if ((!edge.between || !edge.passable) && edge.bindingObject) { ++dead; } }
        if (dead && ui.Button("td2.world.area.prune", "清理大区域内的旧演出绑定（" + std::to_string(dead) + "）")) {
            int removed = 0; std::string error;
            if (PruneInAreaBindings(ctx, layout_, removed, error)) { report_ = "已移除 " + std::to_string(removed) + " 个大区域内的旧绑定（演出文件仍保留在磁盘上）"; layoutStale_ = true; model_->sceneDirty = true; }
            else { model_->error = error; }
        }
        if (ui.Button("td2.world.area.all", "全部大区域：生成机位与演出")) { Generate(ctx, true, true, {}, {}, nullptr, false); return; }
    }
    void DrawRoomCamera(EditorWorkspaceHostContext& ctx, const std::string& regionId) {
        auto& ui = *ctx.ui;
        ui.Separator(); ui.Text("区域机位");
        EnsureLayout(ctx);
        const auto* room = layoutValid_ ? layout_.Room(regionId) : nullptr;
        if (room) {
            // 状態：自動（再生成で置き直す）・手で調整済み／作者が置いた（残す）・ロック済み。
            const std::string state = !room->cameraId ? "没有机位" : room->cameraLocked ? "已锁定 · 重新生成时保留"
                : room->cameraAuto.empty() ? "作者放置 · 重新生成时保留" : CameraEdited(*room) ? "已手调 · 重新生成时保留" : "自动";
            ui.Text(state);
            if (const auto* area = layout_.AreaOf(regionId)) { ui.Text("大区域：" + ShortChoiceLabel(area->name, 14)); } else { ui.Text("未加入大区域"); }
        }
        if (ui.Button("td2.world.room.camera", "预览 / 调整机位")) {
            if (model_->Commit(ctx)) {
                const auto* item = model_->Current();
                OpenRegionCamera(ctx, regionId, item ? item->object.name : regionId, model_->error);
                layoutStale_ = true;
            }
        }
        // 生成の後は layout_ が作り直されて room が指す先が消えるので、すぐ戻る（次のフレームで読み直した内容を描く）。
        if (ui.Button("td2.world.room.camera.generate", room && room->cameraId ? "按当前布局重新生成" : "生成机位")) { Generate(ctx, true, false, {}, regionId, nullptr, false); return; }
        if (room && room->cameraId && (room->cameraLocked || CameraEdited(*room))) {
            ui.SameLine(); if (ui.Button("td2.world.room.camera.reset", "恢复自动")) { Generate(ctx, true, false, {}, regionId, nullptr, true); return; }
        }
    }
    void DrawEdgeCamera(EditorWorkspaceHostContext& ctx, const std::string& ra, const std::string& rb) {
        auto& ui = *ctx.ui;
        EnsureLayout(ctx);
        const LayoutEdge* edge = nullptr;
        for (const auto& e : layout_.edges) { if ((e.a == ra && e.b == rb) || (e.a == rb && e.b == ra)) { edge = &e; } }
        if (!edge) { ui.Text("正在读取镜头布局…"); return; }
        if (edgeDraftKey_ != edge->a + "|" + edge->b) { edgeDraft_ = *edge; edgeDraftKey_ = edge->a + "|" + edge->b; }
        if (!edge->passable) {
            ui.Text("这条边被地形隔开，角色无法越过 · 不需要镜头切换");
            return;
        }
        if (!edge->between) {
            ui.Text("同一大区域内 · 镜头自动跟随，无演出");
        } else {
            ui.Text(edge->sequenceGuid.empty() ? "大区域交界 · 尚未生成演出" : edge->keepAuthored ? "大区域交界 · 保留手调" : "大区域交界 · 演出");
            ui.Text(ShortChoiceLabel(edge->a, 12) + " → " + ShortChoiceLabel(edge->b, 12));
            // 様式（選ぶとすぐ反映し、生成の時に演出へ書く）。
            bool changed = false;
            for (const auto& style : TravelStyles()) {
                const bool current = edgeDraft_.style == style.id;
                if (ui.Selectable("td2.world.edge.style." + std::string(style.id), std::string(style.label) + (style.id == std::string_view("auto") ? "（" + ResolveStyleLabel(*edge) + "）" : ""), current)) {
                    edgeDraft_.style = std::string(style.id); changed = true;
                }
            }
            double seconds = edgeDraft_.seconds;
            if (EditFloat(ui, "td2.world.edge.seconds", "全速时长（秒）", seconds, .4f, 10.f)) { edgeDraft_.seconds = seconds; changed = true; }
            changed |= ui.Checkbox("td2.world.edge.keep", "保留手调（重新生成时不覆盖）", &edgeDraft_.keepAuthored);
            if (changed) { edgeDraftPending_ = true; }
            if (edgeDraftPending_ && !ui.GetInteraction().editing) {
                std::string error;
                if (!WriteEdgeSettings(ctx, edgeDraft_, error)) { model_->error = error; }
                edgeDraftPending_ = false; layoutStale_ = true; model_->sceneDirty = true;
            }
            // 生成の後は layout_ が作り直されて edge が指す先が消えるので、すぐ戻る（次のフレームで読み直した内容を描く）。
            if (ui.Button("td2.world.edge.generate", edge->sequenceGuid.empty() ? "生成演出" : "按当前机位重新生成")) { Generate(ctx, false, true, {}, {}, &edgeDraft_, false); return; }
            if (!edge->sequenceGuid.empty()) {
                ui.SameLine(); if (ui.Button("td2.world.edge.force", "恢复自动")) { auto draft = edgeDraft_; draft.keepAuthored = false; Generate(ctx, false, true, {}, {}, &draft, true); return; }
                if (ui.Button("td2.world.edge.edit", "在演出工作区编辑 / 预览")) {
                    RegionSequenceBinding binding; binding.regionA = edge->a; binding.regionB = edge->b;
                    binding.sequenceAssetGuid = edge->sequenceGuid; binding.speed = static_cast<float>(edge->speed);
                    OpenTransitionCamera(ctx, binding, model_->error);
                }
            }
        }
        // 試遊：この辺のどちら側からでも、歩いて越える位置から始める。
        ui.Text("从这条边试走");
        if (ui.Button("td2.world.edge.trial.a", "→ 从 " + ShortChoiceLabel(edge->a, 10) + " 出发")) { TrialFromEdge(ctx, *edge, true); }
        if (ui.Button("td2.world.edge.trial.b", "← 从 " + ShortChoiceLabel(edge->b, 10) + " 出发")) { TrialFromEdge(ctx, *edge, false); }
    }
    std::string ResolveStyleLabel(const LayoutEdge& edge) const {
        auto copy = edge; copy.style = "auto";
        const auto id = ResolveStyle(layout_, copy);
        for (const auto& style : TravelStyles()) { if (style.id == id) { return std::string(style.label); } }
        return id;
    }
    void TrialFromEdge(EditorWorkspaceHostContext& ctx, const LayoutEdge& edge, bool fromA) {
        if (!ApplyDraft(ctx)) { return; }
        double x = 0, y = 0; uint64_t room = 0;
        if (!EdgeTrialSpawn(layout_, edge, fromA, x, y, room)) { model_->error = "这条边附近没有能站立的位置；请用「从此区域开始」手动点选"; return; }
        const auto started = Execute(ctx, "play.preview", {{"spawnPointId", EncodePreviewSpawn({x, y})}, {"userData", room}});
        model_->error = started.ok ? std::string{} : started.error;
    }
    // 俯瞰図：テレビ（色は大区域）・画面の向き・区域カメラ（視錐）・大区域間の経路・障害物を上から見る。
    void DrawPlan(EditorWorkspaceHostContext& ctx) {
        auto& ui = *ctx.ui;
        const auto input = ui.BeginCanvas("td2.world.plan", 0);
        if (input.size.x > 0 && input.size.y > 0 && layoutValid_) {
            ui.CanvasFill({0, 0}, input.size, {.06f, .08f, .09f, 1});
            // 範囲：テレビとカメラの外接（xz）。
            double x0 = 1e9, z0 = 1e9, x1 = -1e9, z1 = -1e9;
            const auto grow = [&](const Vec3& p) { x0 = (std::min)(x0, p[0]); x1 = (std::max)(x1, p[0]); z0 = (std::min)(z0, p[2]); z1 = (std::max)(z1, p[2]); };
            for (const auto& r : layout_.rooms) { if (r.placed) { grow(r.tv.position); if (r.cameraId) { grow(r.camera.position); } } }
            if (x1 <= x0) { ui.EndCanvas(); return; }
            const double margin = 1.5; x0 -= margin; x1 += margin; z0 -= margin; z1 += margin;
            const double scale = (std::min)(input.size.x / (x1 - x0), input.size.y / (z1 - z0));
            const double ox = (input.size.x - (x1 - x0) * scale) * .5, oy = (input.size.y - (z1 - z0) * scale) * .5;
            // 上から見る：+x を右、-z を上（+z を向くテレビは画の下を向く）。
            const auto P = [&](double x, double z) { return EditorWorkspacePoint{static_cast<float>(ox + (x - x0) * scale), static_cast<float>(oy + (z1 - z) * scale)}; };
            for (const auto& box : layout_.obstacles) {
                if (box.hi[0] < x0 || box.lo[0] > x1 || box.hi[2] < z0 || box.lo[2] > z1 || box.hi[1] < .2) { continue; }
                ui.CanvasFill(P(box.lo[0], box.hi[2]), P(box.hi[0], box.lo[2]), {.16f, .18f, .19f, 1});
            }
            for (const auto& path : planPaths_) {
                const EditorWorkspaceColor color = path.problem.empty() ? EditorWorkspaceColor{.97f, .79f, .39f, .85f} : EditorWorkspaceColor{.95f, .4f, .38f, .85f};
                for (size_t i = 1; i < path.points.size(); ++i) { ui.CanvasLine(P(path.points[i - 1][0], path.points[i - 1][2]), P(path.points[i][0], path.points[i][2]), color, 2); }
            }
            uint64_t clicked = 0; double best = 14 * 14;
            for (const auto& r : layout_.rooms) {
                if (!r.placed) { continue; }
                const double yaw = r.tv.rotation[1] * 3.14159265358979 / 180;
                const double rx = std::cos(yaw), rz = -std::sin(yaw), nx = std::sin(yaw), nz = std::cos(yaw);
                const double hw = r.body[0] * r.tv.scale[0] / 2, hd = r.body[2] * r.tv.scale[2] / 2;
                const double cx = r.tv.position[0], cz = r.tv.position[2];
                const auto corner = [&](double u, double v) { return P(cx + rx * u + nx * v, cz + rz * u + nz * v); };
                const int area = AreaIndex(r.regionId);
                auto color = area >= 0 ? AreaColor(area) : EditorWorkspaceColor{.6f, .65f, .66f, 1};
                const bool selected = model_->Current() && model_->Current()->settings.region.regionId == r.regionId;
                ui.CanvasLine(corner(-hw, -hd), corner(hw, -hd), color, selected ? 3.f : 1.5f);
                ui.CanvasLine(corner(hw, -hd), corner(hw, hd), color, selected ? 3.f : 1.5f);
                ui.CanvasLine(corner(-hw, hd), corner(-hw, -hd), color, selected ? 3.f : 1.5f);
                ui.CanvasLine(corner(-hw, hd), corner(hw, hd), {.95f, .96f, .92f, 1}, selected ? 4.f : 2.5f);   // 画面の側
                if (r.cameraId) {
                    const auto eye = P(r.camera.position[0], r.camera.position[2]);
                    const double cy = r.camera.rotation[1] * 3.14159265358979 / 180;
                    const auto ray = [&](double offset) {
                        const double a = cy + offset;
                        return P(r.camera.position[0] + std::sin(a) * 1.4, r.camera.position[2] + std::cos(a) * 1.4);
                    };
                    const auto camColor = r.cameraLocked || CameraEdited(r) ? EditorWorkspaceColor{.97f, .79f, .39f, 1} : EditorWorkspaceColor{.55f, .82f, .88f, 1};
                    ui.CanvasLine(eye, ray(-.39), camColor, 1.2f); ui.CanvasLine(eye, ray(.39), camColor, 1.2f);
                    ui.CanvasCircle(eye, selected ? 5.f : 3.5f, camColor, 2);
                    const double dx = input.pointer.x - eye.x, dy = input.pointer.y - eye.y;
                    if (dx * dx + dy * dy < best) { best = dx * dx + dy * dy; clicked = r.objectId; }
                }
                const auto center = corner(0, 0);
                const double dx = input.pointer.x - center.x, dy = input.pointer.y - center.y;
                if (dx * dx + dy * dy < best) { best = dx * dx + dy * dy; clicked = r.objectId; }
                if (scale > 18) { ui.CanvasText({center.x + 6, center.y - 6}, {.85f, .88f, .88f, .9f}, ShortChoiceLabel(r.regionId, 10)); }
            }
            ui.CanvasText({10, 8}, {.75f, .8f, .8f, 1}, "俯视：白边为屏幕一侧 · 蓝点为自动机位 / 琥珀为已手调 · 线为大区域间的切换路线（红色为有问题）");
            if (input.hovered && (input.pressedButtons & kPrimary) && clicked) {
                for (const auto& room : model_->rooms) { if (room.object.objectId == clicked) { if (LeaveSequence(ctx)) { model_->Select(ctx, clicked); } break; } }
            }
        }
        ui.EndCanvas();
    }
    struct PlanPath { std::string a, b; std::vector<Vec3> points; std::string problem; };
    // ---- 配置：複数選択と平行移動、矩形選択
    bool GroupPress(EditorWorkspaceHostContext& ctx, const EditorWorkspaceCanvasInput& input, EditorWorkspacePoint p);
    void GroupDrag(EditorWorkspaceHostContext& ctx, const EditorWorkspaceCanvasInput& input, EditorWorkspacePoint p);
    std::set<uint64_t> multi_;
    bool groupMoving_ = false, boxing_ = false;
    EditorWorkspacePoint groupPointer_{}, boxStart_{};
    int groupDx_ = 0, groupDy_ = 0;
    // ---- 预算
    void DrawBudget(EditorWorkspaceHostContext& ctx);
    bool budgetOpen_ = false;
    std::vector<std::string> budget_;
    // ---- 仕掛けの見方
    Machine* SelectedMachine() {
        const auto it = std::find_if(model_->machines.begin(), model_->machines.end(), [this](const Machine& m) { return m.objectId == machineObject_ && m.componentId == machineComponent_; });
        return it == model_->machines.end() ? nullptr : &*it;
    }
    std::optional<RegionPlacement> PlacementOf(const std::string& regionId) const {
        for (const auto& room : model_->rooms) { if (room.settings.region.regionId == regionId) { return Placement(room.settings); } }
        return std::nullopt;
    }
    bool WriteSelected(EditorWorkspaceHostContext& ctx, const Machine& after);
    void ToggleMember(EditorWorkspaceHostContext& ctx, const std::string& regionId);
    void MachinePress(EditorWorkspaceHostContext& ctx, EditorWorkspacePoint p);
    void MachineDrag(EditorWorkspaceHostContext& ctx, const EditorWorkspaceCanvasInput& input, EditorWorkspacePoint p);
    void DrawMachineOverlay(IEditorWorkspaceUi& ui);
    void DrawMachinePanel(EditorWorkspaceHostContext& ctx);
    void DrawMechanism(EditorWorkspaceHostContext& ctx, const Machine m);
    void RefreshCheckInput(EditorWorkspaceHostContext& ctx);
    bool RenameMachine(EditorWorkspaceHostContext& ctx, const Machine& before, const std::string& id);
    bool ReadPoses(EditorWorkspaceHostContext& ctx, std::map<uint64_t, Pose>& poses, std::map<uint64_t, Json>& expected, std::string& sceneId, uint64_t& revision);
    bool BeginPose(EditorWorkspaceHostContext& ctx);
    bool SavePose(EditorWorkspaceHostContext& ctx);
    void CancelPose(EditorWorkspaceHostContext& ctx);
    uint64_t machineObject_ = 0, machineComponent_ = 0;
    bool machineC_ = false, posing_ = false, posingC_ = false, confirmDelete_ = false;
    std::map<uint64_t, Pose> poseA_;
    int ghost_ = -1, ghostDx_ = 0, ghostDy_ = 0, newKind_ = 0;
    EditorWorkspacePoint ghostGrab_{};
    std::optional<Machine> ghostPreview_;
    std::string newMachineId_ = "machine1", machineIdDraft_;
    MachineCheckInput checkInput_;
    std::map<uint64_t, std::string> objectNames_;
    float checkClock_ = 10;
    std::optional<Machine> pendingMachine_;
    static std::string ShortNumber(double v) {
        std::string s = std::to_string(std::round(v * 100) / 100);
        while (!s.empty() && s.back() == '0') { s.pop_back(); }
        if (!s.empty() && s.back() == '.') { s.pop_back(); }
        return s;
    }
    int mode_ = 0;
    LayoutSnapshot layout_;
    bool layoutValid_ = false, layoutStale_ = true;
    uint64_t layoutRevision_ = 0;
    float layoutClock_ = 0;
    std::vector<PlanPath> planPaths_;
    std::string areaFocus_, report_, librarySearch_;
    std::array<double, 3> areaDraft_{};
    bool areaDraftPending_ = false;
    uint64_t areaDraftObject_ = 0, areaDraftComponent_ = 0;
    LayoutEdge edgeDraft_;
    std::string edgeDraftKey_;
    bool edgeDraftPending_ = false, edgeAdvanced_ = false;
    void ReadBinding(EditorWorkspaceHostContext& ctx);
    bool WriteBinding(EditorWorkspaceHostContext& ctx, const RegionSequenceBinding& next);
    void RefreshAssets(EditorWorkspaceHostContext& ctx);
    bool ReadAssetSlots(EditorWorkspaceHostContext& ctx, SequenceAsset& asset);
    EdgeSelection edge_;
    EditorWorkspacePoint offset_{};
    bool moving_ = false, libraryDrag_ = false, propertyPending_ = false;
    bool pickingStart_ = false;
    bool dragHeldAny_ = false, dragPressedAny_ = false, dragReleasedAny_ = false;
    bool assetsRefresh_ = true, bindingRefresh_ = true;
    bool sequenceAdvanced_ = false;
    float clock_ = 0, lastClickTime_ = -1;
    uint64_t lastClick_ = 0;
    RegionTarget bindingTarget_;
    RegionSequenceBinding binding_;
    RegionSequenceBinding observedBinding_;
    // 初回も保存前にスロットとカメラを選べる。Scene の値とは分けて持つ。
    RegionSequenceBinding pendingBinding_, pendingObserved_;
    RegionTarget pendingTarget_;
    bool sequenceDraft_ = false;
    uint64_t bindingRevision_ = 0;
    bool bindingValid_ = false;
    std::vector<SequenceAsset> assets_;
    std::vector<SceneItem> cameras_;
    int picker_ = 0, assetPage_ = 0, slotPage_ = 0, cameraPage_ = 0, neighborPage_ = 0, roomPage_ = 0;
    std::string assetSearch_, cameraSearch_;
    bool canvasHovered_ = false, libraryOpen_ = true;
};

// ---------------------------------------------------------------- 配置：複数選択と平行移動
bool WorldMode::GroupPress(EditorWorkspaceHostContext& ctx, const EditorWorkspaceCanvasInput& input, EditorWorkspacePoint p) {
    uint64_t hit = 0;
    for (auto it = model_->rooms.rbegin(); it != model_->rooms.rend(); ++it) {
        if (it->settings.worldPlaced && ContainsWorldPoint(Placement(it->settings), {p.x, p.y})) { hit = it->object.objectId; break; }
    }
    if (input.shift && hit) {
        // Shift+クリック：複数選択に足す・外す（今の選択も入れておく）。
        if (multi_.empty() && model_->selected && model_->selected != hit) { multi_.insert(model_->selected); }
        if (!multi_.erase(hit)) { multi_.insert(hit); }
        return true;
    }
    if (hit && multi_.size() > 1 && multi_.contains(hit)) {
        if (model_->Dirty() && !model_->Commit(ctx)) { return true; }
        groupMoving_ = true; groupPointer_ = p; groupDx_ = groupDy_ = 0; model_->gesture = true;
        return true;
    }
    if (!hit) {
        // 辺の上でなければ、空いた所からの矩形選択。
        for (const auto& e : Edges()) {
            const auto [a, b] = EdgePoints(e.edge);
            const float dx = b.x - a.x, dy = b.y - a.y;
            const float t = std::clamp(((input.pointer.x - a.x) * dx + (input.pointer.y - a.y) * dy) / (std::max)(.001f, dx * dx + dy * dy), 0.0f, 1.0f);
            if (std::hypot(input.pointer.x - a.x - t * dx, input.pointer.y - a.y - t * dy) < 6) { return false; }
        }
        boxing_ = true; boxStart_ = p;
        return true;
    }
    multi_.clear();
    return false;
}

void WorldMode::GroupDrag(EditorWorkspaceHostContext& ctx, const EditorWorkspaceCanvasInput& input, EditorWorkspacePoint p) {
    const bool released = (input.releasedButtons & kPrimary) || !(input.heldButtons & kPrimary);
    if (boxing_ && released) {
        boxing_ = false;
        const float x0 = (std::min)(boxStart_.x, p.x), x1 = (std::max)(boxStart_.x, p.x), y0 = (std::min)(boxStart_.y, p.y), y1 = (std::max)(boxStart_.y, p.y);
        multi_.clear();
        if (x1 - x0 < .5f && y1 - y0 < .5f) { return; }
        for (const auto& room : model_->rooms) {
            if (!room.settings.worldPlaced) { continue; }
            const auto r = WorldBounds(Placement(room.settings));
            if (r.left < x1 && r.right > x0 && r.top < y1 && r.bottom > y0) { multi_.insert(room.object.objectId); }
        }
        return;
    }
    if (!groupMoving_) { return; }
    groupDx_ = static_cast<int>(std::round(p.x - groupPointer_.x)); groupDy_ = static_cast<int>(std::round(p.y - groupPointer_.y));
    if (!released) { return; }
    groupMoving_ = false; model_->gesture = false;
    if (!groupDx_ && !groupDy_) { return; }
    // 動かした後の配置で重なりを確かめ、全部を一回の取り消しで書く。仕掛けは相対で書いてあるので一緒に付いて行く。
    std::vector<RegionPlacement> layout; Json commands = Json::array();
    for (const auto& room : model_->rooms) {
        auto placement = Placement(room.settings);
        if (multi_.contains(room.object.objectId) && placement.worldPlaced) {
            placement.worldX += groupDx_; placement.worldY += groupDy_;
            commands.push_back({{"objectId", room.target.objectId}, {"componentId", room.target.componentId}, {"propertyId", "worldX"}, {"value", placement.worldX}});
            commands.push_back({{"objectId", room.target.objectId}, {"componentId", room.target.componentId}, {"propertyId", "worldY"}, {"value", placement.worldY}});
        }
        layout.push_back(placement);
    }
    groupDx_ = groupDy_ = 0;
    if (const auto valid = ValidateWorldLayout(layout); !valid.valid) { model_->error = "不能放在这里：" + valid.message + "：" + valid.regionA + " / " + valid.regionB; return; }
    const auto written = Execute(ctx, "authoring.transaction", {{"commands", commands}});
    if (!written.ok) { model_->error = written.error; return; }
    model_->error.clear(); model_->sceneDirty = true; model_->refresh = true; model_->Poll(ctx, true); layoutStale_ = true;
}

// ---------------------------------------------------------------- 预算
void WorldMode::DrawBudget(EditorWorkspaceHostContext& ctx) {
    auto& ui = *ctx.ui;
    if (!ui.Checkbox("td2.world.budget", "显示预算", &budgetOpen_) && !budgetOpen_) { return; }
    if (!budgetOpen_) { return; }
    if (budget_.empty() || ui.Button("td2.world.budget.refresh", "重新计算")) {
        budget_.clear();
        // 世界の広さ（編集中は全部の部屋を 2048 texel の一枚に写すので、広いほど一台あたりが粗くなる）。
        int64_t left = 0, top = 0, right = 0, bottom = 0; int placed = 0, devices = 0; bool any = false;
        for (const auto& room : model_->rooms) {
            DeviceEditor d; d.Load(room.settings.devices); devices += static_cast<int>(d.Items().size());
            if (!room.settings.worldPlaced) { continue; }
            ++placed;
            const auto r = WorldBounds(Placement(room.settings));
            if (!any) { left = r.left; top = r.top; right = r.right; bottom = r.bottom; any = true; }
            left = (std::min)(left, r.left); top = (std::min)(top, r.top); right = (std::max)(right, r.right); bottom = (std::max)(bottom, r.bottom);
        }
        const int64_t span = (std::max)(right - left, bottom - top);
        const double texels = span > 0 ? (std::min)(48.0, 2048.0 / static_cast<double>(span)) : 48.0;
        budget_.push_back("电视地图 " + std::to_string(model_->rooms.size()) + " 台（世界里 " + std::to_string(placed) + " 台），机关 " + std::to_string(devices) + " 个");
        budget_.push_back("世界范围 " + std::to_string(right - left) + " × " + std::to_string(bottom - top) + " 格 · 编辑预览每格约 " + std::to_string(static_cast<int>(texels)) + " 像素" +
                          (texels < 8 ? "（偏糊：可把相关区域摆得更紧凑）" : ""));
        // 描画物体（エンジンの 1 フレーム上限 2048）：シーンのモデル + 編集中に全部の部屋を描く舞台の部品（部屋あたり約 21）。
        std::vector<SceneItem> models; std::string error;
        if (QueryObjects(ctx, "ModelComponent", models, error)) {
            const int stage = static_cast<int>(model_->rooms.size()) * 21 + devices * 3;
            const int total = static_cast<int>(models.size()) + stage;
            budget_.push_back("绘制物体约 " + std::to_string(total) + " / 2048（场景模型 " + std::to_string(models.size()) + " + 编辑时屏幕部件约 " + std::to_string(stage) + "）" +
                              (total > 1800 ? " · 接近上限" : ""));
        }
        // 大区域ごとの台数（画面の読みやすさの保護は一度に 12 台まで）。
        if (EnsureLayout(ctx, true)) {
            for (const auto& area : layout_.areas) {
                budget_.push_back("「" + ShortChoiceLabel(area.name, 12) + "」 " + std::to_string(area.regions.size()) + " 台" +
                                  (area.regions.size() > 12 ? " · 超过画面保护上限 12，同时看到的屏幕请控制在 12 台以内" : ""));
            }
        }
    }
    for (const auto& line : budget_) { ui.Text(line); }
    ui.Separator();
}

// ---------------------------------------------------------------- 仕掛けの見方
bool WorldMode::WriteSelected(EditorWorkspaceHostContext& ctx, const Machine& after) {
    const auto* before = SelectedMachine();
    if (!before) { return false; }
    std::string error;
    if (!WriteMachine(ctx, *before, after, error)) { model_->error = error; return false; }
    model_->sceneDirty = true; model_->machinesStale = true; model_->RefreshMachines(ctx); checkClock_ = 10;
    model_->error.clear();
    return true;
}

void WorldMode::ToggleMember(EditorWorkspaceHostContext& ctx, const std::string& regionId) {
    auto* m = SelectedMachine();
    if (!m) { return; }
    auto after = *m;
    if (after.kind == MachineKind::Mechanism) {
        auto& offsets = machineC_ ? after.offsetsC : after.offsets;
        const auto found = std::find_if(offsets.begin(), offsets.end(), [&](const auto& o) { return o.regionId == regionId; });
        if (found != offsets.end()) { offsets.erase(found); } else { offsets.push_back({regionId}); }
    } else {
        const auto found = std::find(after.members.begin(), after.members.end(), regionId);
        if (found != after.members.end()) { after.members.erase(found); } else { after.members.push_back(regionId); }
        // テレビの塔は世界地図の下（Y が大きい）から上へ並べる。
        if (after.kind == MachineKind::Stack) {
            std::stable_sort(after.members.begin(), after.members.end(), [this](const std::string& a, const std::string& b) {
                const auto pa = PlacementOf(a); const auto pb = PlacementOf(b);
                return (pa ? pa->worldY : 0) > (pb ? pb->worldY : 0);
            });
        }
        // スロット棚の一番目が変わったら、空きスロットのずれを新しい一番目から測り直す。
        if (after.kind == MachineKind::Slide && !m->members.empty() && !after.members.empty() && m->members.front() != after.members.front()) {
            const auto oldFirst = PlacementOf(m->members.front()); const auto newFirst = PlacementOf(after.members.front());
            if (oldFirst && newFirst) { for (auto& [x, y] : after.emptySlots) { x += oldFirst->worldX - newFirst->worldX; y += oldFirst->worldY - newFirst->worldY; } }
        }
    }
    WriteSelected(ctx, after);
}

void WorldMode::MachinePress(EditorWorkspaceHostContext& ctx, EditorWorkspacePoint p) {
    std::string room;
    for (auto it = model_->rooms.rbegin(); it != model_->rooms.rend(); ++it) {
        if (it->settings.worldPlaced && ContainsWorldPoint(Placement(it->settings), {p.x, p.y})) { room = it->settings.region.regionId; break; }
    }
    auto* m = SelectedMachine();
    if (!m) {
        // 仕掛けを選んでいない時：そのテレビが入っている仕掛けを選ぶ。
        for (const auto& machine : model_->machines) {
            const auto regions = machine.Regions();
            if (!room.empty() && std::find(regions.begin(), regions.end(), room) != regions.end()) {
                machineObject_ = machine.objectId; machineComponent_ = machine.componentId; machineIdDraft_ = machine.id; return;
            }
        }
        return;
    }
    if (posing_) { model_->error = "正在摆状态B：先保存或放弃"; return; }
    // 機構：状態の位置を表す点線の枠をつかむ。
    if (m->kind == MachineKind::Mechanism) {
        const auto& offsets = machineC_ ? m->offsetsC : m->offsets;
        for (size_t i = 0; i < offsets.size(); ++i) {
            const auto& o = offsets[i];
            const auto self = PlacementOf(o.regionId); const auto base = PlacementOf(o.anchor.empty() ? o.regionId : o.anchor);
            if (o.park || !self || !base || !base->worldPlaced) { continue; }
            RegionPlacement ghost = *self; ghost.worldPlaced = true; ghost.worldX = base->worldX + o.dx; ghost.worldY = base->worldY + o.dy;
            if (ContainsWorldPoint(ghost, {p.x, p.y}) && (o.dx || o.dy || !o.anchor.empty())) {
                ghost_ = static_cast<int>(i); ghostGrab_ = p; ghostDx_ = o.dx; ghostDy_ = o.dy; return;
            }
        }
    }
    if (!room.empty()) { ToggleMember(ctx, room); return; }
    // スロット棚：空いた格子を空きスロットにする・外す（一番目のメンバーの大きさの格子に合わせる）。
    if (m->kind == MachineKind::Slide && !m->members.empty()) {
        const auto first = PlacementOf(m->members.front());
        if (!first) { return; }
        const int w = (std::max)(1, first->cols), h = (std::max)(1, first->rows);
        const int ox = static_cast<int>(std::floor((p.x - first->worldX) / w)) * w, oy = static_cast<int>(std::floor((p.y - first->worldY) / h)) * h;
        auto after = *m;
        const auto found = std::find(after.emptySlots.begin(), after.emptySlots.end(), std::pair{ox, oy});
        if (found != after.emptySlots.end()) { after.emptySlots.erase(found); } else { after.emptySlots.push_back({ox, oy}); }
        WriteSelected(ctx, after);
    }
}

void WorldMode::MachineDrag(EditorWorkspaceHostContext& ctx, const EditorWorkspaceCanvasInput& input, EditorWorkspacePoint p) {
    if (ghost_ < 0) { ghostPreview_.reset(); return; }
    auto* m = SelectedMachine();
    if (!m) { ghost_ = -1; ghostPreview_.reset(); return; }
    auto after = *m;
    auto& offsets = machineC_ ? after.offsetsC : after.offsets;
    if (ghost_ >= static_cast<int>(offsets.size())) { ghost_ = -1; ghostPreview_.reset(); return; }
    offsets[static_cast<size_t>(ghost_)].dx = ghostDx_ + static_cast<int>(std::round(p.x - ghostGrab_.x));
    offsets[static_cast<size_t>(ghost_)].dy = ghostDy_ + static_cast<int>(std::round(p.y - ghostGrab_.y));
    ghostPreview_ = after;
    if ((input.releasedButtons & kPrimary) || !(input.heldButtons & kPrimary)) {
        ghost_ = -1; ghostPreview_.reset();
        WriteSelected(ctx, after);
    }
}

void WorldMode::DrawMachineOverlay(IEditorWorkspaceUi& ui) {
    const Machine* m = ghostPreview_ ? &*ghostPreview_ : SelectedMachine();
    constexpr EditorWorkspaceColor magenta{.93f, .42f, .74f, 1}, faint{.93f, .42f, .74f, .35f};
    // 選んでいない仕掛けのテレビは薄く縁取る（どのテレビが仕掛けに入っているか見える）。
    for (const auto& machine : model_->machines) {
        if (m && machine.objectId == m->objectId && machine.componentId == m->componentId) { continue; }
        for (const auto& id : machine.Regions()) {
            const auto p = PlacementOf(id);
            if (!p || !p->worldPlaced) { continue; }
            ui.CanvasRect(view_.Screen(static_cast<float>(p->worldX), static_cast<float>(p->worldY)), view_.Screen(static_cast<float>(p->worldX + p->cols), static_cast<float>(p->worldY + p->rows)), faint, 1);
        }
    }
    if (!m) { return; }
    const auto rect = [&](const RegionPlacement& p, EditorWorkspaceColor color, float width) {
        ui.CanvasRect(view_.Screen(static_cast<float>(p.worldX), static_cast<float>(p.worldY)), view_.Screen(static_cast<float>(p.worldX + p.cols), static_cast<float>(p.worldY + p.rows)), color, width);
    };
    const auto label = [&](const RegionPlacement& p, const std::string& text) {
        if (view_.tile >= 4) { ui.CanvasText(view_.Screen(static_cast<float>(p.worldX) + .3f, static_cast<float>(p.worldY) + .2f), magenta, text); }
    };
    if (m->kind == MachineKind::Mechanism) {
        for (const auto* list : {&m->offsets, &m->offsetsC}) {
            const bool current = (list == &m->offsetsC) == machineC_;
            for (const auto& o : *list) {
                const auto self = PlacementOf(o.regionId);
                if (!self) { continue; }
                if (self->worldPlaced) { rect(*self, current ? magenta : faint, current ? 3.f : 1.f); }
                const auto base = PlacementOf(o.anchor.empty() ? o.regionId : o.anchor);
                if (o.park) { if (current && self->worldPlaced) { label(*self, "离开世界"); } continue; }
                if (!base || !base->worldPlaced) { continue; }
                RegionPlacement ghost = *self; ghost.worldX = base->worldX + o.dx; ghost.worldY = base->worldY + o.dy;
                // 状態の位置：点線の枠と、元の位置からの線。
                const auto a = view_.Screen(static_cast<float>(ghost.worldX), static_cast<float>(ghost.worldY));
                const auto b = view_.Screen(static_cast<float>(ghost.worldX + ghost.cols), static_cast<float>(ghost.worldY + ghost.rows));
                const auto color = current ? magenta : faint;
                for (float t = 0; t < 1; t += .1f) {
                    ui.CanvasLine({a.x + (b.x - a.x) * t, a.y}, {a.x + (b.x - a.x) * (t + .05f), a.y}, color, 2);
                    ui.CanvasLine({a.x + (b.x - a.x) * t, b.y}, {a.x + (b.x - a.x) * (t + .05f), b.y}, color, 2);
                    ui.CanvasLine({a.x, a.y + (b.y - a.y) * t}, {a.x, a.y + (b.y - a.y) * (t + .05f)}, color, 2);
                    ui.CanvasLine({b.x, a.y + (b.y - a.y) * t}, {b.x, a.y + (b.y - a.y) * (t + .05f)}, color, 2);
                }
                if (self->worldPlaced && (o.dx || o.dy || !o.anchor.empty())) {
                    ui.CanvasLine(view_.Screen(self->worldX + self->cols * .5f, self->worldY + self->rows * .5f), {(a.x + b.x) * .5f, (a.y + b.y) * .5f}, color, 1.5f);
                }
                if (current) { label(ghost, std::string(machineC_ ? "状态C" : "状态B") + (o.flip ? " · 翻转" : "")); }
            }
        }
        return;
    }
    for (size_t i = 0; i < m->members.size(); ++i) {
        const auto p = PlacementOf(m->members[i]);
        if (!p) { continue; }
        if (!p->worldPlaced) { continue; }
        rect(*p, magenta, 3);
        std::string text = std::to_string(i + 1);
        if (m->kind == MachineKind::Stack) { text = i == 0 ? "底 1" : std::to_string(i + 1); }
        if (m->kind == MachineKind::Carousel && m->members.size() == 4) { constexpr std::array<std::string_view, 4> names{"正面", "右", "背面", "左"}; text = std::string(names[i]); }
        label(*p, text);
    }
    if (m->kind == MachineKind::Slide && !m->members.empty()) {
        if (const auto first = PlacementOf(m->members.front())) {
            for (const auto& [dx, dy] : m->emptySlots) {
                RegionPlacement slot = *first; slot.worldX += dx; slot.worldY += dy;
                rect(slot, faint, 2); label(slot, "空槽");
            }
        }
    }
}

void WorldMode::RefreshCheckInput(EditorWorkspaceHostContext& ctx) {
    checkClock_ += ctx.unscaledDeltaSeconds;
    if (checkClock_ < 2) { return; }
    checkClock_ = 0;
    checkInput_ = {};
    checkInput_.placements = model_->Layout();
    checkInput_.plateTargets = model_->PlateTargets();
    objectNames_.clear();
    for (uint64_t offset = 0;;) {
        const auto page = Execute(ctx, "scene.objects.query", {{"offset", offset}, {"limit", 128}});
        if (!page.ok) { break; }
        const auto objects = page.data.value("objects", Json::array());
        for (const auto& object : objects) {
            const auto id = ObjectId(object.value("objectId", Json{}));
            if (!id) { continue; }
            checkInput_.sceneObjects.push_back(id);
            objectNames_[id] = object.value("name", "");
            bool model = false;
            for (const auto& c : object.value("components", Json::array())) { model = model || c.value("type", "") == "ModelComponent"; }
            checkInput_.objectHasModel[id] = model;
        }
        if (!page.data.contains("nextOffset")) { break; }
        const auto next = ObjectId(page.data["nextOffset"]);
        if (next <= offset || objects.empty()) { break; }
        offset = next;
    }
}

// ID を変えたら、その仕掛けを目標にしている圧力板の目標も同じ取り消しの中で書き換える。
bool WorldMode::RenameMachine(EditorWorkspaceHostContext& ctx, const Machine& before, const std::string& id) {
    if (!StableAuthoringId(id)) { model_->error = "ID 只能用英文字母、数字、点、短横线和下划线"; return false; }
    if (model_->Dirty() && !model_->Commit(ctx)) { return false; }
    auto after = before; after.id = id;
    const auto oldReceivers = before.Receivers(), newReceivers = after.Receivers();
    Json commands = Json::array();
    for (const auto& room : model_->rooms) {
        DeviceEditor devices; devices.Load(room.settings.devices);
        bool changed = false;
        for (const auto& device : devices.Items()) {
            for (const auto& target : device.targets) {
                for (size_t i = 0; i < oldReceivers.size() && i < newReceivers.size(); ++i) {
                    if (target != oldReceivers[i].first) { continue; }
                    devices.ToggleTarget(device.id, target); devices.ToggleTarget(device.id, newReceivers[i].first); changed = true;
                }
            }
        }
        if (changed) { commands.push_back({{"objectId", room.target.objectId}, {"componentId", room.target.componentId}, {"propertyId", "devices"}, {"value", devices.Serialize()}}); }
    }
    Json idCommand = {{"objectId", before.objectId}, {"componentId", before.componentId}, {"value", id}};
    idCommand["propertyId"] = before.kind == MachineKind::Mechanism ? "mechanismId" : before.kind == MachineKind::Slide ? "rackId" : before.kind == MachineKind::Stack ? "stackId" : "carouselId";
    commands.push_back(idCommand);
    const auto written = Execute(ctx, "authoring.transaction", {{"commands", commands}});
    if (!written.ok) { model_->error = written.error; return false; }
    model_->sceneDirty = true; model_->machinesStale = true; model_->refresh = true; model_->Poll(ctx, true); model_->RefreshMachines(ctx);
    return true;
}

bool WorldMode::ReadPoses(EditorWorkspaceHostContext& ctx, std::map<uint64_t, Pose>& poses, std::map<uint64_t, Json>& expected, std::string& sceneId, uint64_t& revision) {
    const auto layout = Execute(ctx, "scene.layout.query", Json::object());
    if (!layout.ok) { model_->error = layout.error; return false; }
    sceneId = layout.data.value("sceneId", ""); revision = layout.data.value("documentRevision", uint64_t{0});
    const auto vec = [](const Json& v, std::array<double, 3> fallback) {
        if (!v.is_array() || v.size() != 3) { return fallback; }
        return std::array<double, 3>{v[0].get<double>(), v[1].get<double>(), v[2].get<double>()};
    };
    for (const auto& object : layout.data.value("objects", Json::array())) {
        const auto id = ObjectId(object.value("objectId", Json{}));
        const auto t = object.value("transform", Json::object());
        Pose pose; pose.position = vec(t.value("position", Json{}), {0, 0, 0});
        pose.rotation = vec(t.contains("rotation") ? t["rotation"] : t.value("rotationEulerDeg", Json{}), {0, 0, 0});
        pose.scale = vec(t.value("scale", Json{}), {1, 1, 1});
        poses[id] = pose; expected[id] = t;
    }
    return true;
}

// 状態 B（または天平の状態 C）をシーンに置いて見せる。作者はビューポートで物体を動かし、保存で相対の姿勢に直す。
bool WorldMode::BeginPose(EditorWorkspaceHostContext& ctx) {
    auto* m = SelectedMachine();
    if (!m || m->kind != MachineKind::Mechanism) { return false; }
    std::map<uint64_t, Pose> poses; std::map<uint64_t, Json> expected; std::string sceneId; uint64_t revision = 0;
    if (!ReadPoses(ctx, poses, expected, sceneId, revision)) { return false; }
    const auto& deltas = machineC_ ? m->deltasC : m->deltas;
    Json commands = Json::array();
    poseA_.clear();
    for (const auto& d : deltas) {
        const auto a = poses.find(d.object);
        if (a == poses.end()) { model_->error = "物体不存在：" + std::to_string(d.object); return false; }
        poseA_[d.object] = a->second;
        const auto base = d.anchor ? poses.find(d.anchor) : a;
        if (base == poses.end()) { model_->error = "锚点物体不存在：" + std::to_string(d.anchor); return false; }
        const auto b = ApplyDelta(a->second, base->second, d);
        commands.push_back({{"objectId", d.object}, {"expected", expected[d.object]}, {"position", b.position}, {"rotation", b.rotation}});
    }
    if (!commands.empty()) {
        const auto applied = Execute(ctx, "scene.layout.apply", {{"sceneId", sceneId}, {"documentRevision", revision}, {"commands", commands}});
        if (!applied.ok) { model_->error = applied.error; return false; }
    }
    posing_ = true; posingC_ = machineC_; model_->error.clear();
    return true;
}

bool WorldMode::SavePose(EditorWorkspaceHostContext& ctx) {
    auto* m = SelectedMachine();
    if (!posing_ || !m) { posing_ = false; return false; }
    std::map<uint64_t, Pose> poses; std::map<uint64_t, Json> expected; std::string sceneId; uint64_t revision = 0;
    if (!ReadPoses(ctx, poses, expected, sceneId, revision)) { return false; }
    auto after = *m;
    auto& deltas = posingC_ ? after.deltasC : after.deltas;
    Json restore = Json::array();
    for (auto& d : deltas) {
        const auto a = poseA_.find(d.object); const auto b = poses.find(d.object);
        if (a == poseA_.end() || b == poses.end()) { continue; }
        // 錨は動かさない前提で、今の錨の姿勢を基準にする（錨が同じ機構で動く物なら状態 A の姿勢）。
        Pose base = a->second;
        if (d.anchor) { base = poseA_.contains(d.anchor) ? poseA_[d.anchor] : poses.contains(d.anchor) ? poses[d.anchor] : base; }
        d = MakeDelta(a->second, base, b->second, d);
        restore.push_back({{"objectId", d.object}, {"expected", expected[d.object]}, {"position", a->second.position}, {"rotation", a->second.rotation}});
    }
    if (!restore.empty()) {
        const auto applied = Execute(ctx, "scene.layout.apply", {{"sceneId", sceneId}, {"documentRevision", revision}, {"commands", restore}});
        if (!applied.ok) { model_->error = "状态B已读取，但恢复状态A失败：" + applied.error; return false; }
    }
    posing_ = false; poseA_.clear();
    return WriteSelected(ctx, after);
}

void WorldMode::CancelPose(EditorWorkspaceHostContext& ctx) {
    if (!posing_) { return; }
    posing_ = false;
    std::map<uint64_t, Pose> poses; std::map<uint64_t, Json> expected; std::string sceneId; uint64_t revision = 0;
    if (!ReadPoses(ctx, poses, expected, sceneId, revision)) { return; }
    Json restore = Json::array();
    for (const auto& [id, a] : poseA_) {
        if (!expected.contains(id)) { continue; }
        restore.push_back({{"objectId", id}, {"expected", expected[id]}, {"position", a.position}, {"rotation", a.rotation}});
    }
    poseA_.clear();
    if (restore.empty()) { return; }
    const auto applied = Execute(ctx, "scene.layout.apply", {{"sceneId", sceneId}, {"documentRevision", revision}, {"commands", restore}});
    if (!applied.ok) { model_->error = "恢复状态A失败：" + applied.error; }
}

void WorldMode::DrawMechanism(EditorWorkspaceHostContext& ctx, const Machine m) {
    auto& ui = *ctx.ui;
    auto after = m;
    bool changed = false;
    changed |= ui.Checkbox("td2.machine.toggle", "每踩一次切换（关：踩住时才动）", &after.toggle);
    changed |= ui.Checkbox("td2.machine.balance", "天平（两块压板对抗，三个状态）", &after.balance);
    changed |= EditFloat(ui, "td2.machine.seconds", "移动秒数", after.seconds, .2f, 8);
    changed |= EditFloat(ui, "td2.machine.dwell", "自动往复：两端停留秒数（0=由压板驱动）", after.dwell, 0, 20);
    changed |= EditFloat(ui, "td2.machine.settle", "压稳多久才动（秒）", after.settle, 0, 2);
    changed |= EditFloat(ui, "td2.machine.lift", "移动途中镜头抬高（米）", after.cameraLift, 0, 4);
    changed |= EditFloat(ui, "td2.machine.pull", "移动途中镜头后拉（米）", after.cameraPull, 0, 4);
    if (after.balance) {
        ui.Text("正在编辑：" + std::string(machineC_ ? "状态C（mech/ID/r 一侧）" : "状态B（mech/ID 一侧）"));
        if (ui.Button("td2.machine.side", machineC_ ? "改编辑状态B" : "改编辑状态C")) { if (posing_) { CancelPose(ctx); } machineC_ = !machineC_; }
    } else if (machineC_) { machineC_ = false; }
    // 区域：状態での世界地図の位置。
    ui.Separator(); ui.Text("区域（状态" + std::string(machineC_ ? "C" : "B") + "的位置）");
    auto& offsets = machineC_ ? after.offsetsC : after.offsets;
    if (offsets.empty()) { ui.Text("在地图上点电视加入"); }
    for (size_t i = 0; i < offsets.size(); ++i) {
        auto& o = offsets[i];
        const std::string key = "td2.machine.offset." + std::to_string(i);
        ui.Text(ShortChoiceLabel(o.regionId, 16));
        changed |= ui.Checkbox(key + ".park", "离开世界（不与任何区域相连）", &o.park);
        if (!o.park) {
            EditInt(ui, key + ".dx", "偏移 X", o.dx, -100000, 100000);
            EditInt(ui, key + ".dy", "偏移 Y", o.dy, -100000, 100000);
            changed |= ui.Checkbox(key + ".flip", "翻转 180°", &o.flip);
            ui.Text(o.anchor.empty() ? "相对：自己原来的位置" : "相对：区域 " + o.anchor);
            // 錨：状態の位置で辺を接するテレビに固定すると、そのテレビ（の章）を動かしても隣に付いて行く。錨を変えても状態の位置は変えない。
            const auto self = PlacementOf(o.regionId);
            const auto oldBase = PlacementOf(o.anchor.empty() ? o.regionId : o.anchor);
            if (self && oldBase && oldBase->worldPlaced) {
                RegionPlacement ghost = *self; ghost.worldPlaced = true; ghost.worldX = oldBase->worldX + o.dx; ghost.worldY = oldBase->worldY + o.dy;
                std::string choice;
                for (const auto& room : model_->rooms) {
                    const auto& id = room.settings.region.regionId;
                    if (id == o.regionId || id == o.anchor || !room.settings.worldPlaced) { continue; }
                    if (std::any_of(offsets.begin(), offsets.end(), [&](const auto& other) { return other.regionId == id; })) { continue; }
                    if (!FindSharedWorldEdge(ghost, Placement(room.settings))) { continue; }
                    if (ui.Button(key + ".anchor." + id, "锚定到相邻的 " + ShortChoiceLabel(room.object.name, 12))) { choice = id; }
                }
                if (!o.anchor.empty() && ui.Button(key + ".anchor.self", "改为相对自己")) { choice = o.regionId; }
                if (!choice.empty()) {
                    const auto newBase = PlacementOf(choice);
                    if (newBase) { o.dx += oldBase->worldX - newBase->worldX; o.dy += oldBase->worldY - newBase->worldY; o.anchor = choice == o.regionId ? std::string{} : choice; changed = true; }
                }
            }
        }
        if (ui.Button(key + ".remove", "移除此区域")) { offsets.erase(offsets.begin() + static_cast<std::ptrdiff_t>(i)); changed = true; break; }
    }
    // 物体：状態 A（シーンに置いた姿勢）から見た状態 B の姿勢。
    ui.Separator(); ui.Text("物体（状态" + std::string(machineC_ ? "C" : "B") + "的姿势）");
    auto& deltas = machineC_ ? after.deltasC : after.deltas;
    for (size_t i = 0; i < deltas.size(); ++i) {
        auto& d = deltas[i];
        const std::string key = "td2.machine.delta." + std::to_string(i);
        const auto name = objectNames_.contains(d.object) ? objectNames_[d.object] : "物体 " + std::to_string(d.object);
        ui.Text(ShortChoiceLabel(name, 16) + "  移动 " + ShortNumber(std::hypot(d.position[0], d.position[1], d.position[2])) + " 米");
        if (d.anchor) { ui.Text("相对锚点：" + (objectNames_.contains(d.anchor) ? objectNames_[d.anchor] : std::to_string(d.anchor))); }
        bool pivot = d.hasPivot;
        if (ui.Checkbox(key + ".pivot", "绕中心转动（倒立、翻转用）", &pivot)) { d.hasPivot = pivot; changed = true; }
        if (d.hasPivot) {
            for (size_t k = 0; k < 3; ++k) { changed |= EditFloat(ui, key + ".pivot." + std::to_string(k), std::string("中心 ") + "XYZ"[k] + "（物体自身坐标，米）", d.pivot[k], -20, 20); }
        }
        if (!posing_ && ui.Button(key + ".remove", "移除此物体")) { deltas.erase(deltas.begin() + static_cast<std::ptrdiff_t>(i)); changed = true; break; }
    }
    if (!posing_) {
        if (ui.Button("td2.machine.add_selected", "加入场景中选中的物体")) {
            const auto state = Execute(ctx, "editor.state", Json::object());
            int added = 0;
            if (state.ok) {
                for (const auto& object : state.data.value("selection", Json::object()).value("objects", Json::array())) {
                    const auto id = ObjectId(object.value("objectId", Json{}));
                    if (!id || std::any_of(deltas.begin(), deltas.end(), [id](const auto& d) { return d.object == id; })) { continue; }
                    MachineObjectDelta d; d.object = id; deltas.push_back(d); ++added;
                }
            }
            if (added) { changed = true; } else { model_->error = "先在场景里选中要动的物体（可多选）"; }
        }
        if (!deltas.empty() && ui.Button("td2.machine.pose", "在视口里摆状态" + std::string(machineC_ ? "C" : "B"))) {
            if (!(after == m)) { WriteSelected(ctx, after); }
            BeginPose(ctx);
            return;
        }
    } else {
        ui.Text("正在摆状态" + std::string(posingC_ ? "C" : "B") + "：用视口的移动、旋转工具摆好这些物体，然后保存。");
        ui.Text("摆的期间请不要保存场景。");
        if (ui.Button("td2.machine.pose.save", "保存这个姿势")) { SavePose(ctx); return; }
        ui.SameLine(); if (ui.Button("td2.machine.pose.cancel", "放弃，回到状态A")) { CancelPose(ctx); }
    }
    // 入力中の数値は入力が終わってから一回で書く。
    if (changed || !(after == m)) {
        if (!ui.GetInteraction().editing) { WriteSelected(ctx, after); } else { pendingMachine_ = after; }
    }
}

void WorldMode::DrawMachinePanel(EditorWorkspaceHostContext& ctx) {
    auto& ui = *ctx.ui;
    model_->RefreshMachines(ctx);
    RefreshCheckInput(ctx);
    // 数値の入力中に貯めた変更は、入力が終わった時に一回で書く。
    if (pendingMachine_ && !ui.GetInteraction().editing) { const auto next = *pendingMachine_; pendingMachine_.reset(); WriteSelected(ctx, next); }
    ui.Text("机关");
    for (const auto& machine : model_->machines) {
        const bool selected = machine.objectId == machineObject_ && machine.componentId == machineComponent_;
        const auto problems = CheckMachine(machine, model_->machines, checkInput_);
        if (ui.Selectable("td2.machine.item." + std::to_string(machine.objectId) + "." + std::to_string(machine.componentId),
                          machine.KindLabel() + "  " + machine.id + (problems.empty() ? "" : "  ⚠"), selected)) {
            if (posing_) { CancelPose(ctx); }
            machineObject_ = machine.objectId; machineComponent_ = machine.componentId; machineIdDraft_ = machine.id; confirmDelete_ = false; machineC_ = false;
        }
    }
    // 新規作成：機構・スロット棚・テレビの塔は空の物体に付ける。回転柱は柱の物体に付ける（シーンで柱を選んでから）。
    ui.Separator(); ui.Text("新建机关");
    constexpr std::array<std::string_view, 4> kinds{"机构（转台、吊车、往复…）", "滑块货架", "电视塔", "转柱（先在场景选中柱子）"};
    for (size_t i = 0; i < kinds.size(); ++i) {
        if (ui.Selectable("td2.machine.newkind." + std::to_string(i), std::string(kinds[i]), newKind_ == static_cast<int>(i))) { newKind_ = static_cast<int>(i); }
    }
    EditText(ui, "td2.machine.newid", "ID", newMachineId_);
    if (ui.Button("td2.machine.create", "新建")) {
        const auto kind = static_cast<MachineKind>(newKind_);
        std::string error;
        uint64_t host = 0;
        if (kind == MachineKind::Carousel) { host = SelectedObject(ctx, error); }
        if (kind == MachineKind::Carousel && !host) { model_->error = "先在场景里选中转柱的柱子（柱子的位置就是转轴）"; }
        else if (!StableAuthoringId(newMachineId_)) { model_->error = "ID 只能用英文字母、数字、点、短横线和下划线"; }
        else if (!CreateMachine(ctx, kind, host, newMachineId_, error)) { model_->error = error; }
        else {
            model_->sceneDirty = true; model_->machinesStale = true; model_->RefreshMachines(ctx); checkClock_ = 10;
            for (const auto& machine : model_->machines) { if (machine.kind == kind && machine.id == newMachineId_) { machineObject_ = machine.objectId; machineComponent_ = machine.componentId; machineIdDraft_ = machine.id; } }
            model_->error.clear();
        }
    }
    auto* m = SelectedMachine();
    if (!m) { return; }
    ui.Separator();
    ui.Text(m->KindLabel() + "  " + m->id + "  （" + ShortChoiceLabel(m->objectName, 14) + "）");
    EditText(ui, "td2.machine.id", "ID（改名会同步压板的目标）", machineIdDraft_);
    if (machineIdDraft_ != m->id && !ui.GetInteraction().editing) {
        if (!RenameMachine(ctx, *m, machineIdDraft_)) { machineIdDraft_ = m->id; }
        return;
    }
    if (m->kind == MachineKind::Mechanism) { DrawMechanism(ctx, *m); m = SelectedMachine(); if (!m) { return; } }
    else {
        auto after = *m;
        bool changed = false;
        if (m->kind == MachineKind::Slide) { changed |= EditFloat(ui, "td2.machine.seconds", "滑动秒数", after.seconds, .2f, 4); }
        if (m->kind == MachineKind::Carousel) { changed |= EditFloat(ui, "td2.machine.seconds", "每转一面秒数", after.seconds, .3f, 6); }
        if (m->kind == MachineKind::Stack) {
            changed |= EditFloat(ui, "td2.machine.pull", "抽出秒数", after.pullSeconds, .2f, 4);
            changed |= EditFloat(ui, "td2.machine.drop", "每层下落秒数", after.dropSeconds, .1f, 2);
            changed |= EditFloat(ui, "td2.machine.reset", "复位秒数", after.resetSeconds, .5f, 6);
            ui.Text("抽出距离（米，向最下面电视的背面；先抽的停得远）");
            for (size_t i = 0; i < after.dockDistances.size(); ++i) {
                changed |= EditFloat(ui, "td2.machine.dock." + std::to_string(i), "第 " + std::to_string(i + 1) + " 台", after.dockDistances[i], .3f, 10);
            }
            if (ui.Button("td2.machine.dock.add", "+ 停放位")) { after.dockDistances.push_back(after.dockDistances.empty() ? 2.4 : (std::max)(.6, after.dockDistances.back() - 1.15)); changed = true; }
            if (!after.dockDistances.empty()) { ui.SameLine(); if (ui.Button("td2.machine.dock.remove", "- 停放位")) { after.dockDistances.pop_back(); changed = true; } }
        }
        ui.Separator();
        ui.Text(m->kind == MachineKind::Stack ? "电视（自下而上）" : m->kind == MachineKind::Carousel ? "面（顺时针：正面、右、背面、左）" : "成员（第一台是空槽的基准）");
        for (size_t i = 0; i < after.members.size(); ++i) {
            const auto p = PlacementOf(after.members[i]);
            ui.Text(std::to_string(i + 1) + ". " + ShortChoiceLabel(after.members[i], 16) + (p && p->worldPlaced ? "" : m->kind == MachineKind::Carousel ? "（口袋：不在世界里）" : "（未放进世界）"));
            if (m->kind == MachineKind::Carousel || m->kind == MachineKind::Slide) {
                ui.SameLine();
                if (i > 0 && ui.Button("td2.machine.member.up." + std::to_string(i), "↑")) { std::swap(after.members[i], after.members[i - 1]); changed = true; }
                ui.SameLine();
                if (i + 1 < after.members.size() && ui.Button("td2.machine.member.down." + std::to_string(i), "↓")) { std::swap(after.members[i], after.members[i + 1]); changed = true; }
            }
        }
        if (m->kind == MachineKind::Slide) {
            ui.Text("空槽（相对第一台）");
            for (const auto& [x, y] : after.emptySlots) { ui.Text("· " + std::to_string(x) + ", " + std::to_string(y)); }
        }
        if (changed || !(after == *m)) {
            if (!ui.GetInteraction().editing) { WriteSelected(ctx, after); m = SelectedMachine(); if (!m) { return; } } else { pendingMachine_ = after; }
        }
    }
    // 確かめ：実行時に動かない理由と、つながっている圧力板。
    ui.Separator();
    const auto problems = CheckMachine(*m, model_->machines, checkInput_);
    if (problems.empty()) { ui.Text("✓ 可以运行"); }
    for (const auto& problem : problems) { ui.Text("⚠ " + problem); }
    const auto plates = PlatesFor(*m, checkInput_);
    if (!plates.empty()) {
        ui.Text("连到它的压板");
        for (const auto& plate : plates) {
            const auto region = plate.substr(0, plate.find('/'));
            if (ui.Button("td2.machine.plate." + plate, "打开 " + ShortChoiceLabel(plate, 22))) {
                for (const auto& room : model_->rooms) { if (room.settings.region.regionId == region) { OpenTelevision(ctx, room.object.objectId); return; } }
            }
        }
    }
    ui.Separator();
    if (!confirmDelete_) { if (ui.Button("td2.machine.delete", "删除这个机关")) { confirmDelete_ = true; } }
    else {
        ui.Text("删除后连到它的压板会显示「找不到」。");
        if (ui.Button("td2.machine.delete.confirm", "确认删除")) {
            std::string error;
            if (posing_) { CancelPose(ctx); }
            if (!RemoveMachine(ctx, *m, error)) { model_->error = error; }
            else { model_->sceneDirty = true; model_->machinesStale = true; machineObject_ = machineComponent_ = 0; model_->RefreshMachines(ctx); }
            confirmDelete_ = false;
        }
        ui.SameLine(); if (ui.Button("td2.machine.delete.cancel", "取消")) { confirmDelete_ = false; }
    }
}

void WorldMode::RefreshAssets(EditorWorkspaceHostContext& ctx) {
    assetsRefresh_ = false; assets_.clear(); cameras_.clear();
    uint64_t offset = 0;
    // インポーターの公開の索引を使う。選んだ資産のスロットは標準の文書から取る。
    for (;;) {
        const auto result = Execute(ctx, "asset.query", {{"type", "Sequence"}, {"offset", offset}, {"limit", 128}});
        if (!result.ok) { model_->error = result.error; return; }
        const auto entries = result.data.value("assets", Json::array());
        if (!entries.is_array()) { model_->error = "Sequence 资产索引未返回数组"; return; }
        for (const auto& item : entries) {
            const auto guid = item.value("guid", ""), name = item.value("name", item.value("path", "Sequence"));
            if (!guid.empty()) { assets_.push_back({guid, name, {}, false}); }
        }
        if (!result.data.contains("nextOffset")) { break; }
        const auto next = ObjectId(result.data["nextOffset"]);
        if (next <= offset || entries.empty()) { model_->error = "Sequence 资产索引分页无效"; return; }
        offset = next;
    }
    std::sort(assets_.begin(), assets_.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
    QueryObjects(ctx, "CameraComponent", cameras_, model_->error);
}

bool WorldMode::ReadAssetSlots(EditorWorkspaceHostContext& ctx, SequenceAsset& asset) {
    if (asset.slotsKnown) { return true; }
    const auto opened = Execute(ctx, "authoring.session.open", {{"workspace", "cinematics"}, {"assetGuid", asset.guid}});
    if (!opened.ok) { model_->error = opened.error; return false; }
    const auto session = opened.data.value("sessionId", "");
    if (session.empty()) { model_->error = "演出未返回文档会话"; return false; }
    const auto result = Execute(ctx, "cinematics.document", {{"sessionId", session}});
    if (!result.ok) { model_->error = result.error; return false; }
    const auto document = result.data.value("document", Json::object());
    if (!document.is_object() || !document.contains("bindings") || !document["bindings"].is_array()) { model_->error = "原生演出缺少有效绑定定义"; return false; }
    asset.slots.clear();
    for (const auto& binding : document["bindings"]) {
        if (binding.value("targetKind", "") == "slot") {
            const auto name = binding.value("slotName", "");
            if (!name.empty() && std::find(asset.slots.begin(), asset.slots.end(), name) == asset.slots.end()) { asset.slots.push_back(name); }
        }
    }
    asset.slotsKnown = true; return true;
}

void WorldMode::ReadBinding(EditorWorkspaceHostContext& ctx) {
    bindingRefresh_ = false; bindingTarget_ = {}; binding_ = {}; bindingValid_ = false; bindingRevision_ = model_->revision;
    if (!edge_.a || !edge_.b) { return; }
    const auto a = std::find_if(model_->rooms.begin(), model_->rooms.end(), [this](const auto& r) { return r.object.objectId == edge_.a; });
    const auto b = std::find_if(model_->rooms.begin(), model_->rooms.end(), [this](const auto& r) { return r.object.objectId == edge_.b; });
    if (a == model_->rooms.end() || b == model_->rooms.end()) { return; }
    binding_.regionA = a->settings.region.regionId; binding_.regionB = b->settings.region.regionId;
    std::vector<SceneItem> items;
    if (!QueryObjects(ctx, kRegionSequenceComponentType, items, model_->error)) { return; }
    for (const auto& item : items) {
        const auto instances = Execute(ctx, "component.instances.query", {{"objectId", item.objectId}});
        if (!instances.ok) { model_->error = instances.error; return; }
        for (const auto& component : instances.data.value("components", Json::array())) {
            if (component.value("type", "") != kRegionSequenceComponentType) { continue; }
            const auto componentId = ObjectId(component.value("componentId", Json{}));
            const auto result = Execute(ctx, "component.describe", {{"objectId", item.objectId}, {"componentId", componentId}, {"propertyIds", {"regionA", "regionB", "sequenceAssetGuid", "sequenceSlotName", "cameraObject", "speed"}}});
            if (!result.ok) { model_->error = result.error; return; }
            const auto fields = result.data.value("fields", Json::array());
            const auto* fa = FieldValue(fields, "regionA"); const auto* fb = FieldValue(fields, "regionB");
            const auto* guid = FieldValue(fields, "sequenceAssetGuid"); const auto* slot = FieldValue(fields, "sequenceSlotName"); const auto* camera = FieldValue(fields, "cameraObject");
            if (!fa || !fa->is_string() || !fb || !fb->is_string() || !guid || !guid->is_string() || !slot || !slot->is_string() || !camera || !camera->is_number_integer()) { model_->error = "Sequence 绑定字段无效"; return; }
            RegionSequenceBinding value{fa->get<std::string>(), fb->get<std::string>(), guid->get<std::string>(), slot->get<std::string>(), ObjectId(*camera)};
            if (const auto* speed = FieldValue(fields, "speed"); speed && speed->is_number()) value.speed = speed->get<float>();
            if (!SameRegionPair(binding_, value)) { continue; }
            if (bindingTarget_.objectId) { model_->error = "同一交界有重复 Sequence 绑定"; bindingTarget_ = {}; return; }
            binding_ = std::move(value); bindingTarget_ = {item.objectId, componentId};
        }
    }
    observedBinding_ = binding_; bindingValid_ = true;
    if (sequenceDraft_ && !SameRegionPair(pendingBinding_, binding_)) { sequenceDraft_ = false; }
}

bool WorldMode::WriteBinding(EditorWorkspaceHostContext& ctx, const RegionSequenceBinding& requested) {
    if (!bindingValid_) { return false; }
    const auto next = requested;
    if (next.sequenceAssetGuid.empty()) {
        if (!next.sequenceSlotName.empty() || next.cameraObject) { model_->error = "未绑定演出时请清除槽和摄像机"; return false; }
    }
    else {
        const auto asset = std::find_if(assets_.begin(), assets_.end(), [&next](const auto& a) { return a.guid == next.sequenceAssetGuid; });
        if (asset == assets_.end()) { model_->error = "Sequence 资产不存在"; return false; }
        if (!ReadAssetSlots(ctx, *asset)) { return false; }
        if (!next.sequenceSlotName.empty() && std::find(asset->slots.begin(), asset->slots.end(), next.sequenceSlotName) == asset->slots.end()) { model_->error = "所选槽不属于该演出"; return false; }
        if (!next.sequenceSlotName.empty() && std::none_of(cameras_.begin(), cameras_.end(), [&next](const auto& camera) { return camera.objectId == next.cameraObject; })) { model_->error = "请选择有效摄像机"; return false; }
    }
    if (const auto valid = ValidateRegionSequenceBinding(next); !valid.valid) { model_->error = valid.message; return false; }
    const auto expected = sequenceDraft_ ? pendingObserved_ : observedBinding_;
    const auto expectedTarget = sequenceDraft_ ? pendingTarget_ : bindingTarget_;
    ReadBinding(ctx);
    if (!bindingValid_ || bindingTarget_ != expectedTarget || binding_ != expected) { model_->error = "Sequence 绑定已被其他编辑修改；请刷新后重试"; return false; }
    if (next == binding_) { sequenceDraft_ = false; model_->error.clear(); return true; }
    RegionTarget target = bindingTarget_;
    bool createdRecord = false;
    if (!target.objectId) {
        if (next.sequenceAssetGuid.empty()) { sequenceDraft_ = false; return true; }
        // 辺ごとに小さい記録を持つ。同じカメラを複数の辺で使っても記録を上書きしない。
        const auto created = Execute(ctx, "scene.create_empty_object", {{"name", "电视交界演出"}});
        if (!created.ok) { model_->error = created.error; return false; }
        target.objectId = ObjectId(created.data.value("objectId", Json{}));
        if (!target.objectId) { model_->error = "Sequence 配置对象创建失败"; return false; }
        createdRecord = true;
        const auto added = Execute(ctx, "component.add", {{"objectId", target.objectId}, {"componentType", kRegionSequenceComponentType}});
        if (!added.ok) { Execute(ctx, "scene.delete_object", {{"objectId", target.objectId}}); model_->error = added.error; return false; }
        for (const auto& c : added.data.value("addedComponents", Json::array())) { if (c.value("type", "") == kRegionSequenceComponentType) { target.componentId = ObjectId(c.value("componentId", Json{})); } }
        if (!target.componentId) { Execute(ctx, "scene.delete_object", {{"objectId", target.objectId}}); model_->error = "Sequence 配置组件创建失败"; return false; }
    }
    Json commands = Json::array();
    const auto add = [&commands, &target](std::string_view id, const auto& value) { commands.push_back({{"objectId", target.objectId}, {"componentId", target.componentId}, {"propertyId", id}, {"value", value}}); };
    add("regionA", next.regionA); add("regionB", next.regionB); add("sequenceAssetGuid", next.sequenceAssetGuid); add("sequenceSlotName", next.sequenceSlotName); add("cameraObject", next.cameraObject);
    const auto result = Execute(ctx, "authoring.transaction", {{"commands", commands}});
    if (!result.ok) { if (createdRecord) { Execute(ctx, "scene.delete_object", {{"objectId", target.objectId}}); } model_->error = result.error; return false; }
    model_->sceneDirty = true;
    bindingRefresh_ = true; ReadBinding(ctx);
    if (bindingTarget_ != target || binding_ != next) { model_->error = "Sequence 写入回读不一致"; return false; }
    sequenceDraft_ = false; model_->error.clear(); return true;
}

void WorldMode::DrawProperties(EditorWorkspaceHostContext& ctx) {
    auto& ui = *ctx.ui;
    if (mode_ == 1) { DrawAreaPanel(ctx); return; }
    if (mode_ == 3) { DrawMachinePanel(ctx); return; }
    DrawBudget(ctx);
    if (pickingStart_) {
        ui.Text("在世界地图中选择出生位置");
        ui.Text("绿色轮廓是角色位置；点击立即开始");
        if (ui.Button("td2.world.spawn.cancel.properties", "取消")) { pickingStart_ = false; }
        return;
    }
    if (edge_.a) {
        if (assetsRefresh_) { RefreshAssets(ctx); }
        if (bindingRefresh_ || bindingRevision_ != model_->revision) { ReadBinding(ctx); }
        const auto a = std::find_if(model_->rooms.begin(), model_->rooms.end(), [this](const auto& r) { return r.object.objectId == edge_.a; });
        const auto b = std::find_if(model_->rooms.begin(), model_->rooms.end(), [this](const auto& r) { return r.object.objectId == edge_.b; });
        if (a == model_->rooms.end() || b == model_->rooms.end()) { return; }
        ui.Text("相邻区域 / 切换演出");
        if (ui.Button("td2.world.sequence.back", "返回房间属性")) { LeaveSequence(ctx); return; }
        DrawEdgeCamera(ctx, a->settings.region.regionId, b->settings.region.regionId);
        ui.Checkbox("td2.world.sequence.manual", "高级：手动指定演出资产", &edgeAdvanced_);
        if (!edgeAdvanced_) { return; }
        const auto beginDraft = [this] {
            if (!sequenceDraft_) { pendingBinding_ = binding_; pendingObserved_ = observedBinding_; pendingTarget_ = bindingTarget_; sequenceDraft_ = true; }
        };
        const std::string selectedGuid = sequenceDraft_ ? pendingBinding_.sequenceAssetGuid : binding_.sequenceAssetGuid;
        auto selectedAsset = std::find_if(assets_.begin(), assets_.end(), [&selectedGuid](const auto& asset) { return asset.guid == selectedGuid; });
        ui.Text("演出：" + (selectedGuid.empty() ? std::string("保持当前镜头") : selectedAsset == assets_.end() ? std::string("资产缺失") : ShortChoiceLabel(selectedAsset->name)));
        ui.Text(sequenceDraft_ && pendingBinding_ != observedBinding_ ? "绑定草稿未应用" : model_->sceneDirty ? "绑定已应用 · 场景未保存" : model_->savedOnce ? "绑定已保存" : "场景中的绑定");
        if (ui.Button("td2.world.sequence.pick", picker_ == 1 ? "收起演出列表" : "选择切换演出")) { picker_ = picker_ == 1 ? 0 : 1; }
        ui.SameLine(); if (ui.Button("td2.world.sequence.refresh", "刷新")) { assetsRefresh_ = true; bindingRefresh_ = true; }
        if (picker_ == 1) {
            EditText(ui, "td2.world.sequence.search", "查找演出", assetSearch_);
            std::vector<EditorChoice> choices{{"td2.world.sequence.none", "保持当前镜头", selectedGuid.empty()}};
            std::vector<std::string> guids{std::string{}};
            for (const auto& asset : assets_) {
                if (!assetSearch_.empty() && asset.name.find(assetSearch_) == std::string::npos) { continue; }
                choices.push_back({"td2.world.sequence." + asset.guid, asset.name, selectedGuid == asset.guid}); guids.push_back(asset.guid);
            }
            if (const int selected = DrawPagedChoices(ui, choices, assetPage_, "td2.world.sequences", 4); selected >= 0) {
                beginDraft(); pendingBinding_.sequenceAssetGuid = guids[static_cast<size_t>(selected)];
                const auto asset = std::find_if(assets_.begin(), assets_.end(), [this](const auto& value) { return value.guid == pendingBinding_.sequenceAssetGuid; });
                pendingBinding_.sequenceSlotName.clear(); pendingBinding_.cameraObject = 0;
                if (asset != assets_.end() && ReadAssetSlots(ctx, *asset) && !asset->slots.empty()) {
                    pendingBinding_.sequenceSlotName = asset->slots.front();
                    if (!cameras_.empty()) { pendingBinding_.cameraObject = cameras_.front().objectId; }
                }
                picker_ = 0;
            }
            if (assets_.empty()) { ui.Text("项目没有 Sequence；可先保持镜头"); }
        }
        if (ui.Button("td2.world.sequence.none.quick", "保持当前镜头") && !selectedGuid.empty()) {
            beginDraft(); pendingBinding_.sequenceAssetGuid.clear(); pendingBinding_.sequenceSlotName.clear(); pendingBinding_.cameraObject = 0;
            picker_ = 0;
        }
        const auto& editable = sequenceDraft_ ? pendingBinding_ : binding_;
        if (!editable.sequenceAssetGuid.empty()) {
            const auto asset = std::find_if(assets_.begin(), assets_.end(), [&editable](const auto& s) { return s.guid == editable.sequenceAssetGuid; });
            if (asset != assets_.end() && ReadAssetSlots(ctx, *asset)) {
                if (asset->slots.empty()) { ui.Text("使用资产内部镜头"); }
                else {
                    const auto currentCamera = std::find_if(cameras_.begin(), cameras_.end(), [&editable](const auto& camera) { return camera.objectId == editable.cameraObject; });
                    ui.Text("执行相机：" + (currentCamera == cameras_.end() ? std::string("未指定") : ShortChoiceLabel(currentCamera->name)));
                    ui.Checkbox("td2.world.sequence.advanced", "高级：执行相机 / 绑定槽", &sequenceAdvanced_);
                    if (sequenceAdvanced_) {
                        ui.Text("执行相机承载演出");
                        ui.Text("机位在原生演出工作区编辑");
                        if (ui.Button("td2.world.sequence.slot.pick", "槽：" + (editable.sequenceSlotName.empty() ? std::string("未指定") : editable.sequenceSlotName))) { picker_ = picker_ == 2 ? 0 : 2; }
                        if (picker_ == 2) {
                            std::vector<EditorChoice> choices;
                            for (const auto& slot : asset->slots) { choices.push_back({"td2.world.sequence.slot." + slot, slot, editable.sequenceSlotName == slot}); }
                            if (const int selected = DrawPagedChoices(ui, choices, slotPage_, "td2.world.slots", 4); selected >= 0) { beginDraft(); pendingBinding_.sequenceSlotName = asset->slots[static_cast<size_t>(selected)]; picker_ = 0; }
                        }
                        if (ui.Button("td2.world.sequence.camera.pick", "选择执行相机")) { picker_ = picker_ == 3 ? 0 : 3; }
                        if (picker_ == 3) {
                            EditText(ui, "td2.world.sequence.camera.search", "查找相机", cameraSearch_);
                            std::vector<EditorChoice> choices; std::vector<uint64_t> ids;
                            for (const auto& camera : cameras_) {
                                if (!cameraSearch_.empty() && camera.name.find(cameraSearch_) == std::string::npos) { continue; }
                                choices.push_back({"td2.world.sequence.camera." + std::to_string(camera.objectId), camera.name, editable.cameraObject == camera.objectId}); ids.push_back(camera.objectId);
                            }
                            if (const int selected = DrawPagedChoices(ui, choices, cameraPage_, "td2.world.cameras", 4); selected >= 0) { beginDraft(); pendingBinding_.cameraObject = ids[static_cast<size_t>(selected)]; picker_ = 0; }
                            if (cameras_.empty()) { ui.Text("场景没有 CameraComponent；先添加相机"); }
                        }
                    }
                }
            }
        }
        if (ui.Button("td2.world.sequence.apply", "应用绑定")) { if (sequenceDraft_) { WriteBinding(ctx, pendingBinding_); } }
        ui.SameLine(); if (ui.Button("td2.world.sequence.discard", "撤回草稿")) { sequenceDraft_ = false; bindingRefresh_ = true; model_->error.clear(); }
        if (!bindingValid_) { ui.Text("绑定读取失败；请刷新或处理上方错误"); return; }
        if (!editable.sequenceAssetGuid.empty() && ui.Button("td2.world.sequence.edit", "编辑演出 / 预览")) {
            if (!sequenceDraft_ || WriteBinding(ctx, pendingBinding_)) OpenTransitionCamera(ctx, binding_, model_->error);
        }
        // 演出の中身はエンジンの演出ワークスペースで作る。ここは区域の組へ既存の演出資産を結ぶだけ。
        ui.Text("镜头关键帧请在引擎的演出（Cinematics）工作区编辑");
        return;
    }
    if (!model_->ready) { return; }
    auto& s = model_->draft;
    if (const auto* item = model_->Current()) { ui.Text(item->object.name); }
    ui.Text(std::to_string(s.region.cols) + " × " + std::to_string(s.region.rows));
    if (ui.Button("td2.world.room.edit", "编辑电视地图")) { OpenTelevision(ctx, model_->selected); }
    DrawRoomCamera(ctx, s.region.regionId);
    if (s.worldPlaced) {
        // 正式なスポーン位置は変えず、次のキャンバスのクリックを一回限りの試遊の入口にする。
        if (ui.Button("td2.world.room.play", "从此区域开始")) {
            if (model_->Commit(ctx)) {
                pickingStart_ = true; hand_ = false; moving_ = libraryDrag_ = false;
                view_.Fit(view_.lastSize, static_cast<float>(s.worldX), static_cast<float>(s.worldY),
                          static_cast<float>(s.region.cols), static_cast<float>(s.region.rows));
            }
        }
        const auto before = s;
        EditInt(ui, "td2.world.room.x", "世界 X", s.worldX, -1000000, 1000000);
        EditInt(ui, "td2.world.room.y", "世界 Y", s.worldY, -1000000, 1000000);
        if (s != before) { propertyPending_ = true; }
        if (propertyPending_ && !ui.GetInteraction().editing && !model_->gesture) { model_->Finish(ctx, true); propertyPending_ = false; }
        if (ui.Button("td2.world.room.remove", "从世界移除")) { s.worldPlaced = false; model_->Commit(ctx); }
    }
    ui.Separator(); ui.Text("相邻区域 / 切换演出");
    const auto edges = Edges(); std::vector<EdgeSelection> neighbors; std::vector<EditorChoice> choices;
    for (const auto& edge : edges) {
        if (edge.a != model_->selected && edge.b != model_->selected) { continue; }
        const auto otherId = edge.a == model_->selected ? edge.b : edge.a;
        const auto other = std::find_if(model_->rooms.begin(), model_->rooms.end(), [otherId](const auto& room) { return room.object.objectId == otherId; });
        if (other == model_->rooms.end()) { continue; }
        const auto side = edge.a == model_->selected ? edge.edge.sideA : edge.edge.sideB;
        const std::string direction = side == WorldSide::Left ? "← " : side == WorldSide::Right ? "→ " : side == WorldSide::Top ? "↑ " : "↓ ";
        choices.push_back({"td2.world.neighbor." + std::to_string(otherId), direction + other->object.name, false}); neighbors.push_back(edge);
    }
    if (choices.empty()) { ui.Text(s.worldPlaced ? "没有共享边；拖动电视贴齐即可相邻" : "先把电视拖入世界地图"); }
    else {
        ui.Text("选择邻居配置镜头切换");
        if (const int selected = DrawPagedChoices(ui, choices, neighborPage_, "td2.world.neighbors", 4); selected >= 0) { ChooseEdge(ctx, neighbors[static_cast<size_t>(selected)]); }
    }
}

// ---------------------------------------------------------------- 3D の舞台（下の欄）
// ビューポートの操作の案内、選んだ物体の位置と向き、テレビの新規作成、台や道具の配置、地図の隣に合わせた並べ方。
// 物体を動かすのは標準のビューポートのギズモ（W / E / R）が主で、ここは数値と「一押しで済む」並べ方だけを受け持つ。
class StagePanel {
public:
    void Draw(EditorWorkspaceHostContext& ctx, MapModel& model, WorldMode& world) {
        auto& ui = *ctx.ui;
        ui.Text("视口：左键点选 · 按住右键拖动看四周（同时 WASD 移动、Q / E 降升、滚轮调速度）· 滚轮前后推 · F 聚焦选中\n"
                "手柄：W 移动 / E 旋转 / R 缩放 / Q 收起 · X 切换世界 / 本地坐标 · Ctrl+D 复制 · Delete 删除");
        if (!error_.empty()) { ui.Text("⚠ " + error_); } else if (!note_.empty()) { ui.Text(note_); }
        Refresh(ctx, model);
        FinishStand(ctx, model, world);
        if (ui.BeginTable("td2.stage.columns", 3)) {
            ui.TableNextRow();
            ui.TableNextColumn(); DrawSelection(ctx, model, world);
            ui.TableNextColumn(); DrawTelevisions(ctx, model, world);
            ui.TableNextColumn(); DrawProps(ctx, model, world);
            ui.EndTable();
        }
    }
private:
    // 選んだ物体の姿勢を読む（選択が変わった時と、時々）。数値を入力している間は読み直さない。
    void Refresh(EditorWorkspaceHostContext& ctx, MapModel& model) {
        clock_ += ctx.unscaledDeltaSeconds;
        const uint64_t id = model.sceneSelection;
        if (pending_ || (id == object_ && clock_ < .5f)) { return; }
        clock_ = 0; object_ = id; readOk_ = false;
        std::string error;
        if (id) { readOk_ = ReadStageObject(ctx, id, read_, error); }
    }
    const RoomItem* RoomOf(const MapModel& model, uint64_t object) const {
        for (const auto& room : model.rooms) { if (object && MapModel::TelevisionObject(room) == object) { return &room; } }
        return nullptr;
    }
    // テレビを動かした後、そのカメラを今の配置で作り直す（作者が手で直したカメラは残る）。
    void Recamera(EditorWorkspaceHostContext& ctx, MapModel& model, WorldMode& world, uint64_t television) {
        if (const auto* room = RoomOf(model, television)) { const std::string region = room->settings.region.regionId; world.SetUpNewRoom(ctx, region, {}); }
    }
    void DrawSelection(EditorWorkspaceHostContext& ctx, MapModel& model, WorldMode& world) {
        auto& ui = *ctx.ui;
        if (!object_ || !readOk_) { ui.Text("在视口里点选一个物体"); return; }
        const auto* room = RoomOf(model, object_);
        ui.Text("选中：" + ShortChoiceLabel(read_.name, 18) + (read_.television ? "（" + std::string(read_.television->label) + "）" : ""));
        if (room) { ui.Text("区域 " + ShortChoiceLabel(room->settings.region.regionId, 14) + " · 左栏和右侧地图编辑它"); }
        else if (read_.television && ui.Button("td2.stage.adopt", "把这台电视建成区域")) {
            const auto model3d = *read_.television;
            if (model.AdoptTelevision(ctx, object_, model3d, std::nullopt)) { const auto* fresh = model.Current(); if (fresh) { world.SetUpNewRoom(ctx, fresh->settings.region.regionId, {}); } note_ = "已建成区域，请在世界布局里把它拖进世界"; error_.clear(); }
            else { error_ = model.error; }
            return;
        }
        if (!pending_) { editing_ = read_.pose; }
        bool changed = false;
        changed |= EditFloat(ui, "td2.stage.x", "X（米）", editing_.position[0], -1000, 1000);
        changed |= EditFloat(ui, "td2.stage.y", "Y（米，高度）", editing_.position[1], -1000, 1000);
        changed |= EditFloat(ui, "td2.stage.z", "Z（米）", editing_.position[2], -1000, 1000);
        changed |= EditFloat(ui, "td2.stage.yaw", "朝向（度，绕竖轴）", editing_.rotation[1], -360, 360);
        const bool uniform = std::abs(editing_.scale[0] - editing_.scale[1]) < 1e-4 && std::abs(editing_.scale[0] - editing_.scale[2]) < 1e-4;
        if (uniform) {
            double s = editing_.scale[0];
            if (EditFloat(ui, "td2.stage.scale", "缩放", s, .05f, 20)) { editing_.scale = {s, s, s}; changed = true; }
        }
        // よく使う向きの直し：15° と 90° ずつ。
        constexpr std::array<std::pair<double, std::string_view>, 4> turns{{{-90, "↺ 90°"}, {-15, "↺ 15°"}, {15, "↻ 15°"}, {90, "↻ 90°"}}};
        for (size_t i = 0; i < turns.size(); ++i) {
            if (i) { ui.SameLine(); }
            if (ui.Button("td2.stage.turn." + std::to_string(i), turns[i].second)) { editing_.rotation[1] = std::fmod(editing_.rotation[1] + turns[i].first + 540.0, 360.0) - 180; changed = true; }
        }
        if (ui.Button("td2.stage.floor", "落到地面") && read_.hasBounds) { editing_.position[1] -= read_.lo[1]; changed = true; }
        if (room) { ui.SameLine(); if (ui.Button("td2.stage.recamera", "按现在的位置重新生成机位")) { Recamera(ctx, model, world, object_); note_ = "已重新生成机位（手调过的机位保留）"; } }
        if (changed) { pending_ = true; }
        if (pending_ && !ui.GetInteraction().editing) {
            pending_ = false;
            std::string error;
            if (SetStagePose(ctx, object_, editing_, error)) { error_.clear(); model.sceneDirty = true; } else { error_ = error; }
            clock_ = 1;
        }
    }
    void DrawTelevisions(EditorWorkspaceHostContext& ctx, MapModel& model, WorldMode& world) {
        auto& ui = *ctx.ui;
        const auto* current = model.Current();
        ui.Text(current ? "新建电视（放在「" + ShortChoiceLabel(current->object.name, 10) + "」右边，地图接在它右侧）" : "新建电视（放在视口前方的地面）");
        constexpr size_t columns = 3;
        if (ui.BeginTable("td2.stage.televisions", static_cast<uint32_t>(columns))) {
            for (size_t i = 0; i < std::size(kTelevisionModels); ++i) {
                if (i % columns == 0) { ui.TableNextRow(); }
                ui.TableNextColumn();
                const auto& m = kTelevisionModels[i];
                if (ui.Button("td2.stage.tv." + std::to_string(i), std::string(m.label) + " " + std::to_string(m.cols) + "×" + std::to_string(m.rows))) { CreateTelevision(ctx, model, world, m); return; }
            }
            ui.EndTable();
        }
        // 地図で隣り合うテレビの横・上・下へ、今のテレビを 3D でも寄せる。
        if (!current || !current->settings.worldPlaced) { return; }
        const auto here = Placement(current->settings);
        bool any = false;
        for (const auto& other : model.rooms) {
            if (other.object.objectId == current->object.objectId || !other.settings.worldPlaced) { continue; }
            const auto edge = FindSharedWorldEdge(Placement(other.settings), here);
            if (!edge) { continue; }
            const int side = edge->sideA == WorldSide::Right ? 1 : edge->sideA == WorldSide::Left ? -1 : edge->sideA == WorldSide::Top ? 2 : -2;
            const std::string where = side == 1 ? "右边" : side == -1 ? "左边" : side == 2 ? "上面" : "下面";
            if (!any) { ui.Text("按世界地图贴到邻居旁（3D）"); any = true; }
            if (ui.Button("td2.stage.snap." + std::to_string(other.object.objectId), "贴到「" + ShortChoiceLabel(other.object.name, 10) + "」" + where)) {
                SnapBeside(ctx, model, world, MapModel::TelevisionObject(*current), MapModel::TelevisionObject(other), side);
                return;
            }
        }
    }
    void CreateTelevision(EditorWorkspaceHostContext& ctx, MapModel& model, WorldMode& world, const TelevisionModel& m) {
        error_.clear(); note_.clear();
        const auto* current = model.Current();
        // 区域の一覧は作り直されるので、隣の情報は先に値で写す。
        const std::optional<RegionSettings> beside = current ? std::optional<RegionSettings>(current->settings) : std::nullopt;
        const std::string besideRegion = current ? current->settings.region.regionId : std::string{};
        StageObject anchor;
        std::string error;
        const bool hasAnchor = current && ReadStageObject(ctx, MapModel::TelevisionObject(*current), anchor, error);
        Pose pose;
        if (hasAnchor) { pose = PoseBeside(anchor, m.width * m.scale, m.height * m.scale, 1); }
        else if (!PoseAtView(ctx, pose, error_)) { return; }
        pose.scale = {m.scale, m.scale, m.scale};
        const std::string name = "电视 " + std::string(m.label);
        const uint64_t id = CreateStageModel(ctx, m.path, name, pose, error_);
        if (!id) { return; }
        model.sceneDirty = true;
        if (hasAnchor) { JoinFolderOf(ctx, id, anchor.objectId); }
        if (!model.AdoptTelevision(ctx, id, m, beside)) { error_ = model.error; return; }
        const auto* fresh = model.Current();
        if (fresh) { const std::string region = fresh->settings.region.regionId; world.SetUpNewRoom(ctx, region, besideRegion); }
        if (Execute(ctx, "scene.object.select", {{"objectId", id}}).ok) { model.sceneSelection = id; }
        fresh = model.Current();
        note_ = "已新建「" + name + "」：地图 " + std::to_string(m.cols) + "×" + std::to_string(m.rows) +
                (fresh && fresh->settings.worldPlaced ? "，已接到世界地图里并生成机位" : "，已生成机位") + " · 在右侧地图画地形";
        if (!model.error.empty()) { note_ += " · " + model.error; }
    }
    void SnapBeside(EditorWorkspaceHostContext& ctx, MapModel& model, WorldMode& world, uint64_t television, uint64_t neighbor, int side) {
        StageObject self, other;
        if (!ReadStageObject(ctx, television, self, error_) || !ReadStageObject(ctx, neighbor, other, error_)) { return; }
        auto pose = PoseBeside(other, StageWidth(self), StageHeight(self), side);
        pose.scale = self.pose.scale;
        if (!SetStagePose(ctx, television, pose, error_)) { return; }
        model.sceneDirty = true; error_.clear(); clock_ = 1;
        Recamera(ctx, model, world, television);
        note_ = "已贴到「" + ShortChoiceLabel(other.name, 12) + "」旁边并重新生成机位";
    }
    void DrawProps(EditorWorkspaceHostContext& ctx, MapModel& model, WorldMode&) {
        auto& ui = *ctx.ui;
        const auto* current = model.Current();
        ui.Text(current ? "布景（台类垫在「" + ShortChoiceLabel(current->object.name, 10) + "」下面，其余放在视口前方）" : "布景（放在视口前方）");
        constexpr size_t columns = 4;
        if (ui.BeginTable("td2.stage.props", static_cast<uint32_t>(columns))) {
            for (size_t i = 0; i < std::size(kStageProps); ++i) {
                if (i % columns == 0) { ui.TableNextRow(); }
                ui.TableNextColumn();
                const auto& prop = kStageProps[i];
                if (ui.Button("td2.stage.prop." + std::to_string(i), std::string(prop.stand && current ? "▁ " : "") + std::string(prop.label))) { PlaceProp(ctx, model, prop); return; }
            }
            ui.EndTable();
        }
    }
    // 台はテレビの真下の床に置き、モデルが読み込まれて高さが分かったらテレビを台の上へ載せる（FinishStand）。
    void PlaceProp(EditorWorkspaceHostContext& ctx, MapModel& model, const StageProp& prop) {
        error_.clear(); note_.clear();
        const auto* current = model.Current();
        StageObject tv;
        Pose pose;
        const bool under = prop.stand && current && ReadStageObject(ctx, MapModel::TelevisionObject(*current), tv, error_);
        // 台は床に立てる：テレビの真下、向きはテレビの縦軸の向きだけ写す。
        if (under) { pose = tv.pose; pose.position[1] = 0; pose.rotation = {0, tv.pose.rotation[1], 0}; pose.scale = {1, 1, 1}; }
        else if (!PoseAtView(ctx, pose, error_)) { return; }
        const uint64_t id = CreateStageModel(ctx, prop.path, prop.label, pose, error_);
        if (!id) { return; }
        model.sceneDirty = true;
        if (under) {
            JoinFolderOf(ctx, id, tv.objectId);
            stand_ = id; standTelevision_ = tv.objectId; standFrames_ = 240;
            note_ = "已在「" + ShortChoiceLabel(tv.name, 12) + "」下面放「" + std::string(prop.label) + "」，读出台高后把电视放上去…";
        } else {
            if (Execute(ctx, "scene.object.select", {{"objectId", id}}).ok) { model.sceneSelection = id; }
            note_ = "已放「" + std::string(prop.label) + "」：用 W / E 移动、旋转它";
        }
    }
    void FinishStand(EditorWorkspaceHostContext& ctx, MapModel& model, WorldMode& world) {
        if (!stand_) { return; }
        StageObject stand, tv;
        std::string error;
        if (!ReadStageObject(ctx, stand_, stand, error) || !ReadStageObject(ctx, standTelevision_, tv, error)) { stand_ = 0; error_ = error; return; }
        if (!stand.hasBounds) {
            if (--standFrames_ <= 0) { stand_ = 0; error_ = "读不到台的高度：请用 W 手动把电视移到台面上"; }
            return;
        }
        tv.pose.position[1] = stand.hi[1];
        const auto television = standTelevision_;
        stand_ = 0;
        if (!SetStagePose(ctx, television, tv.pose, error_)) { return; }
        clock_ = 1;
        Recamera(ctx, model, world, television);
        note_ = "电视已放到台面上（高 " + std::to_string(static_cast<int>(std::round(stand.hi[1] * 100))) + " 厘米）并重新生成机位";
    }
    float clock_ = 1;
    uint64_t object_ = 0;
    StageObject read_;
    bool readOk_ = false, pending_ = false;
    Pose editing_;
    std::string error_, note_;
    uint64_t stand_ = 0, standTelevision_ = 0;
    int standFrames_ = 0;
};

// ---------------------------------------------------------------- 关卡编辑
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
