#pragma once
// レベル編集ワークスペースの共有部分。シーンの作者データの写し（MapModel）。
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
    std::string error, published;
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
    void Discard() { if (const auto* item = Current()) { draft = item->settings; devices.Load(draft.devices); } gesture = false; error.clear(); }
    // ---- シーンの変化の目印。editor.state を一回だけ読み、文書（編集の版・物体数・取り消し札・未保存）と選んだ物（ID と包囲箱）の変化を数える。
    // 操作は一回ごとにログへ残るので、重い読み直しはこの数が変わった時だけにする（テレビ全部を毎秒読むとログが膨らむ）。
    uint64_t documentPulse = 0, selectionPulse = 0, activeObject = 0;
    std::string documentMark, selectionMark;
    // やり直せる記録がある（取り消した直後）。この間に自動で書くと、やり直しが消え、取り消した物がまた戻るので書かない。
    bool canRedo = false;
    float pulseClock = 1;
    uint64_t Pulses() const { return documentPulse + selectionPulse; }
    void Pulse(EditorWorkspaceHostContext& ctx, bool force = false) {
        pulseClock += ctx.unscaledDeltaSeconds;
        if (!force && pulseClock < .3f) { return; }
        pulseClock = 0;
        const auto state = Execute(ctx, "editor.state", Json::object());
        if (!state.ok) { return; }
        const auto scene = state.data.value("scene", Json::object());
        const auto selection = state.data.value("selection", Json::object());
        if (scene.is_object()) {
            if (const auto d = scene.find("dirty"); d != scene.end() && d->is_boolean()) { sceneDirty = d->get<bool>(); }
            canRedo = scene.value("canRedo", false);
        }
        activeObject = selection.is_object() ? ObjectId(selection.value("activeObjectId", Json{})) : 0;
        // 編集カメラは毎フレーム動くので目印に入れない。
        auto document = scene.dump() + state.data.value("play", Json::object()).dump();
        auto chosen = selection.dump();
        if (document != documentMark) { documentMark = std::move(document); ++documentPulse; }
        if (chosen != selectionMark) { selectionMark = std::move(chosen); ++selectionPulse; }
    }
    uint64_t polledPulse = 0;
    void Poll(EditorWorkspaceHostContext& ctx, bool force = false) {
        elapsed += ctx.unscaledDeltaSeconds;
        // 標準の Ctrl+Z / Ctrl+Y はシーンの履歴を直接動かすので、その後の数フレームは草稿を捨てて読み直す。
        if (historyFrames > 0) { --historyFrames; if (!gesture) { Discard(); force = true; machinesStale = true; } }
        Pulse(ctx);
        // 文書が変わった時だけ読み直す。目印に出ない変化（同じ札の続けての編集）は 30 秒ごとに拾う。
        if (gesture || Dirty() || (!force && !refresh && polledPulse == documentPulse && elapsed < 30.f)) { return; }
        elapsed = 0; refresh = false;
        // 自分の書き込みも目印に含めてから読む（直後にもう一度読み直さない）。
        Pulse(ctx, true); polledPulse = documentPulse;
        std::vector<SceneItem> objects;
        if (!QueryObjects(ctx, kRegionComponentType, objects, error)) { return; }
        std::vector<RoomItem> fresh;
        for (const auto& object : objects) {
            RoomItem item; item.object = object;
            if (!ReadRegion(ctx, object.objectId, item.target, item.settings, error)) { return; }
            fresh.push_back(std::move(item));
        }
        // 中身が変わった時だけ版を進める（版を見て読み直す布局などを、毎回の読み直しで動かさない）。
        const bool same = fresh.size() == rooms.size() && std::equal(fresh.begin(), fresh.end(), rooms.begin(), [](const RoomItem& a, const RoomItem& b) {
            return a.object.objectId == b.object.objectId && a.object.name == b.object.name && a.target == b.target && a.settings == b.settings;
        });
        rooms = std::move(fresh);
        if (!same) { ++revision; }
        if (!Current()) { selected = rooms.empty() ? 0 : rooms.front().object.objectId; }
        ready = Current() != nullptr;
        if (ready) { draft = Current()->settings; devices.Load(draft.devices); }
        if (!canRedo && PrepareNewTelevisions(ctx)) { refresh = true; }
    }
    bool Select(EditorWorkspaceHostContext& ctx, uint64_t objectId) {
        if (objectId == selected && ready) { return true; }
        if (Dirty() && !Commit(ctx)) { return false; }
        selected = objectId; ready = Current() != nullptr;
        if (ready) { draft = Current()->settings; devices.Select(-1); devices.Load(draft.devices); }
        return ready;
    }
    std::vector<RegionPlacement> Layout(const RegionSettings* replacement = nullptr) const {
        std::vector<RegionPlacement> result;
        for (const auto& r : rooms) { result.push_back(Placement(replacement && r.object.objectId == selected ? *replacement : r.settings)); }
        return result;
    }
    std::string Validate(const RegionSettings& s) const {
        if (!std::isfinite(s.screenBrightness) || s.screenBrightness < .5f || s.screenBrightness > 4.0f) { return "画面の明るさは 0.5～4"; }
        const auto region = ValidateRegion(s.region);
        if (!region.valid) { return std::string(region.message); }
        if (s.region.cols > 64 || s.region.rows > 64 || s.region.cells.size() > 4096) { return "テレビの地図は 64 × 64 マスまで"; }
        DeviceEditor d; d.Load(s.devices);
        if (const auto problem = d.Validate(s.region.cols, s.region.rows); !problem.empty()) { return problem; }
        if (!std::isfinite(s.spawnX) || !std::isfinite(s.spawnY) || s.spawnX < 0 || s.spawnY < 0 ||
            s.spawnX + float(kPlayerWidth) > static_cast<float>(s.region.cols) || s.spawnY + float(kPlayerHeight) > static_cast<float>(s.region.rows)) { return "スポーン位置が地図の外です"; }
        const auto layout = Layout(&s);
        const auto valid = ValidateWorldLayout(layout);
        return valid.valid ? std::string{} : valid.message + "：" + valid.regionA + " / " + valid.regionB;
    }
    bool Commit(EditorWorkspaceHostContext& ctx) {
        auto* item = Current(); if (!ready || !item || !Dirty()) { return true; }
        if (const auto problem = Validate(draft); !problem.empty()) { error = problem; return false; }
        RegionTarget target; RegionSettings fresh;
        if (!ReadRegion(ctx, selected, target, fresh, error)) { return false; }
        if (target != item->target || fresh != item->settings) { error = "地図がほかの編集で変わりました。下書きを戻して読み直してください"; return false; }
        Json changes = Json::array(); AppendRegionChanges(changes, target, fresh, draft);
        std::vector<RoomItem*> otherEntries;
        // 開始テレビの変更も同じ取り消しに含め、開始区域を二つ残さない。
        if (draft.entryRegion) for (auto& other : rooms) {
            if (other.object.objectId == selected || !other.settings.entryRegion) { continue; }
            RegionTarget otherTarget; RegionSettings otherFresh;
            if (!ReadRegion(ctx, other.object.objectId, otherTarget, otherFresh, error)) { return false; }
            if (otherTarget != other.target || otherFresh != other.settings) { error = "開始テレビがほかの編集で変わりました"; return false; }
            changes.push_back(PropertyCommand(otherTarget.objectId, otherTarget.componentId, "entryRegion", false));
            otherEntries.push_back(&other);
        }
        if (!Transact(ctx, changes, error)) { return false; }
        sceneDirty = true;
        for (auto* other : otherEntries) {
            RegionTarget observedTarget; RegionSettings observed;
            if (!ReadRegion(ctx, other->object.objectId, observedTarget, observed, error)) { return false; }
            if (observedTarget != other->target || observed.entryRegion) { error = "開始テレビの書き込みを確認できません"; return false; }
            other->settings = observed;
        }
        RegionSettings confirmed; RegionTarget confirmedTarget;
        if (!ReadRegion(ctx, selected, confirmedTarget, confirmed, error)) { return false; }
        item->settings = confirmed; item->target = confirmedTarget;
        if (confirmedTarget != target || confirmed != draft) { error = "地図の書き込みを確認できません（下書きは残しています）"; return false; }
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
    // ---- シーン（3D）の選択との同期。シーンでテレビを選ぶと地図もそのテレビを開き、地図で選ぶとシーンでもそのテレビを選ぶ。
    uint64_t sceneSelection = 0, followedPulse = 0;
    int historyFrames = 0;
    // 目印が新しく読んだ選択だけを見る（地図から選んだ直後に、古い選択へ戻さない）。
    void FollowScene(EditorWorkspaceHostContext& ctx) {
        if (gesture || followedPulse == selectionPulse) { return; }
        followedPulse = selectionPulse;
        const uint64_t id = activeObject;
        if (id == sceneSelection) { return; }
        sceneSelection = id;
        for (const auto& room : rooms) {
            if (!id || room.object.objectId != id) { continue; }
            if (room.object.objectId != selected) { Select(ctx, room.object.objectId); }
            return;
        }
    }
    void ShowInScene(EditorWorkspaceHostContext& ctx) {
        const auto* room = Current();
        if (!room) { return; }
        const auto id = room->object.objectId;
        if (id != sceneSelection && Execute(ctx, "scene.object.select", {{"objectId", id}}).ok) { sceneSelection = id; }
    }
    // 新しく区域を付けたテレビ（ID が既定のまま）と、シーンで複製したテレビ（ID が重なる。物体 ID の大きい方）を整える。
    // 取り消しでこの整えを戻した時は、次に普通の編集をするまで整え直さない（canRedo の間は呼ばない）。
    // 新しいテレビは型の格子と床だけの地図、複製は地図を残す。どちらも新しい ID で世界の外に置き、一回の取り消しにまとめる。
    bool PrepareNewTelevisions(EditorWorkspaceHostContext& ctx) {
        const std::string unset = RegionDefinition{}.regionId;
        std::vector<RoomItem*> order;
        for (auto& room : rooms) { order.push_back(&room); }
        std::sort(order.begin(), order.end(), [](const RoomItem* a, const RoomItem* b) { return a->object.objectId < b->object.objectId; });
        std::set<std::string> seen;
        std::vector<RoomItem*> renamed;
        for (auto* room : order) {
            const auto& id = room->settings.region.regionId;
            if (id != unset && seen.insert(id).second) { continue; }
            renamed.push_back(room);
        }
        if (renamed.empty()) { return false; }
        // 新しいテレビの型（モデルの GUID）。
        std::map<uint64_t, const TelevisionModel*> models;
        std::vector<uint64_t> ids;
        for (const auto* room : renamed) { if (room->settings.region.regionId == unset) { ids.push_back(room->object.objectId); } }
        if (!ids.empty()) {
            const auto detail = Execute(ctx, "scene.objects.query", {{"objectIds", ids}, {"detail", true}});
            if (!detail.ok) { error = detail.error; return false; }
            for (const auto& object : detail.data.value("objects", Json::array())) {
                const auto model = object.find("model");
                const auto asset = model != object.end() && model->is_object() ? model->value("assetId", "") : std::string{};
                if (!asset.empty()) { models[ObjectId(object.value("objectId", Json{}))] = FindTelevisionModel(asset); }
            }
            // 空の物体に区域を付けたばかりのテレビは、モデルを選ぶまで待つ（型の格子で整える）。
            std::erase_if(renamed, [&](const RoomItem* room) { return room->settings.region.regionId == unset && !models.contains(room->object.objectId); });
            if (renamed.empty()) { return false; }
        }
        bool hasEntry = std::any_of(rooms.begin(), rooms.end(), [&](const RoomItem& r) {
            return r.settings.entryRegion && std::find(renamed.begin(), renamed.end(), &r) == renamed.end();
        });
        Json commands = Json::array();
        for (auto* room : renamed) {
            auto next = room->settings;
            next.region.regionId = "td2.region." + std::to_string(room->object.objectId);
            next.worldPlaced = false; next.stageAnchor.clear();
            next.entryRegion = !hasEntry; hasEntry = true;
            if (room->settings.region.regionId == unset) {
                // 型の格子（表に無いモデルは一番多い型と同じ 12×7）に、下の一列だけの床。
                const auto found = models.find(room->object.objectId);
                const auto* model = found == models.end() ? nullptr : found->second;
                next.region.cols = model ? model->cols : 12; next.region.rows = model ? model->rows : 7;
                next.region.cells.assign(static_cast<size_t>(next.region.cols * next.region.rows), '0');
                for (int x = 0; x < next.region.cols; ++x) { next.region.cells[CellIndex(next.region, x, next.region.rows - 1)] = '1'; }
                next.spawnX = 1.1875f; next.spawnY = static_cast<float>(next.region.rows) - 1 - float(kPlayerHeight);
                next.devices.clear();
            }
            AppendRegionChanges(commands, room->target, room->settings, next);
        }
        if (!Transact(ctx, commands, error)) { return false; }
        sceneDirty = true;
        return true;
    }
    void Publish(EditorWorkspaceHostContext& ctx) {
        std::string message = error;
        if (message.empty() && !rooms.empty()) {
            const auto entries = std::count_if(rooms.begin(), rooms.end(), [](const auto& r) { return r.settings.worldPlaced && r.settings.entryRegion; });
            if (entries == 0) { message = "世界地図に開始テレビを置いてください"; }
            else if (entries > 1) { message = "開始テレビは世界地図に一台だけ"; }
        }
        if (message == published) { return; }
        ctx.commands->PublishStatus(message.empty() ? EditorWorkspaceStatusSeverity::Info : EditorWorkspaceStatusSeverity::Warning, message);
        published = message;
    }
};

}
