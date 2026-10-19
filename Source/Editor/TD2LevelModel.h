#pragma once
// レベル編集ワークスペースの共有部分。シーンの作者データの写し（MapModel）と、
// テレビ地図・世界地図の二つの見方に共通する欄の描き方（MapMode）。
#include "TD2MapCanvas.h"
#include "TD2EditorChoices.h"
#include "TD2CameraAuthoring.h"
#include "TD2CameraLayout.h"
#include "TD2MachineEditor.h"
#include "TD2StageTools.h"
#include "../Common/TD2Math.h"
#include "../Common/TD2MachineText.h"
#include "../Common/TD2WorldLayout.h"
#include "../Common/TD2PreviewEntry.h"
#include <array>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <set>

namespace TD2::Editor::Level {
inline constexpr uint32_t kPrimary = static_cast<uint32_t>(EditorWorkspacePointerButton::Primary);
inline constexpr uint32_t kSecondary = static_cast<uint32_t>(EditorWorkspacePointerButton::Secondary);
inline constexpr const char* kLevelWorkspace = "td2.level_editor";
// 見方：-1 はテレビ地図、0〜3 は世界地図の配置・カメラ大区域・俯瞰カメラ・仕掛け。
inline constexpr int kTelevisionView = -1;
// 小数 2 桁までの短い表示。
inline std::string ShortNumber(double v) {
    std::string s = std::to_string(std::round(v * 100) / 100);
    while (!s.empty() && s.back() == '0') { s.pop_back(); }
    if (!s.empty() && s.back() == '.') { s.pop_back(); }
    return s;
}

struct RoomItem {
    SceneItem object;
    RegionTarget target;
    RegionSettings settings;
};

inline RegionPlacement Placement(const RegionSettings& s) {
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
    const RoomItem* Room(uint64_t objectId) const {
        const auto found = std::find_if(rooms.begin(), rooms.end(), [objectId](const RoomItem& r) { return r.object.objectId == objectId; });
        return found == rooms.end() ? nullptr : &*found;
    }
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
        // 画面の物体か材質スロットを変えたら、モデルにそのスロットがあるか確かめ直す。
        if (draft.screenObject != item->settings.screenObject || draft.screenSlot != item->settings.screenSlot) { screenWarning = ScreenSlotWarning(ctx, draft); }
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
            changes.push_back(PropertyCommand(otherTarget.objectId, otherTarget.componentId, "entryRegion", false));
            otherEntries.push_back(&other);
        }
        if (!Transact(ctx, changes, error)) { return false; }
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
        screenWarning = ScreenSlotWarning(ctx, draft);
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
    // 新しく置いたテレビの物体を区域にする：型の格子と床だけの地図。
    // beside が世界に置いてあれば、世界地図ではその右（空いていなければ左・上・下）に下端を揃えて置く。
    bool AdoptTelevision(EditorWorkspaceHostContext& ctx, uint64_t object, const TelevisionModel& model, std::optional<RegionSettings> beside) {
        if (Dirty() && !Commit(ctx)) { return false; }
        if (!AddRegion(ctx, object, true, error)) { return false; }
        sceneDirty = true;
        refresh = true; Poll(ctx, true);
        if (!Select(ctx, object)) { return false; }
        screenWarning = ScreenSlotWarning(ctx, draft);
        const auto previous = draft.region;
        draft.region.cols = model.cols; draft.region.rows = model.rows;
        if (!ResizeCells(previous, draft.region)) { draft.region = previous; error = "格子尺寸设置失败"; return false; }
        std::fill(draft.region.cells.begin(), draft.region.cells.end(), '0');
        for (int x = 0; x < model.cols; ++x) { draft.region.cells[CellIndex(draft.region, x, model.rows - 1)] = '1'; }
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
}
