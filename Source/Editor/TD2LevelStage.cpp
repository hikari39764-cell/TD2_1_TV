#include "TD2LevelEditor.h"

namespace TD2::Editor::Level {
namespace {
bool SamePose(const Pose& a, const Pose& b) {
    for (size_t i = 0; i < 3; ++i) {
        if (std::abs(a.position[i] - b.position[i]) > .001 || std::abs(a.rotation[i] - b.rotation[i]) > .05 || std::abs(a.scale[i] - b.scale[i]) > .001) { return false; }
    }
    return true;
}
}

// 3D で、地図の隣のテレビの横へ付ける。anchor が空なら、地図で一番長く辺を共有するテレビ。
void LevelEditor::AttachRoom(EditorWorkspaceHostContext& ctx, const std::string& regionId, const std::string& anchor) {
    if (!model_.Commit(ctx) || !EnsureLayout(ctx, true)) { return; }
    const auto* room = layout_.Room(regionId);
    if (!room || !room->placed) { return; }
    std::string chosen = anchor;
    if (chosen.empty()) {
        int64_t best = 0;
        for (const auto& other : layout_.rooms) {
            if (other.regionId == regionId || !other.placed) { continue; }
            // 自分に付いているテレビへは付けない（輪にしない）。
            const auto item = std::find_if(model_.rooms.begin(), model_.rooms.end(), [&](const RoomItem& r) { return r.settings.region.regionId == other.regionId; });
            if (item != model_.rooms.end() && item->settings.stageAnchor == regionId) { continue; }
            if (const auto edge = FindSharedWorldEdge(Editor::PlacementOf(other), Editor::PlacementOf(*room)); edge && edge->overlapEnd - edge->overlapBegin > best) {
                best = edge->overlapEnd - edge->overlapBegin; chosen = other.regionId;
            }
        }
    }
    const auto* neighbor = layout_.Room(chosen);
    if (!neighbor) { SetNote("地図の上で隣り合うテレビがありません"); return; }
    const auto pose = AttachedPose(BoxOf(*room), Editor::PlacementOf(*room), BoxOf(*neighbor), Editor::PlacementOf(*neighbor));
    if (!pose) { SetNote("地図の上で隣り合っていません"); return; }
    const uint64_t tv = room->tvObject;
    if (!SetStagePose(ctx, tv, *pose, model_.error)) { return; }
    // 付けた時の両方の姿勢を覚える（この後に相手が動けば、その分だけ付いて行く）。
    Pose anchorPose; anchorPose.position = neighbor->tv.position; anchorPose.rotation = neighbor->tv.rotation; anchorPose.scale = neighbor->tv.scale;
    anchorPoses_[tv] = seenPoses_[tv] = *pose;
    anchorPoses_[neighbor->tvObject] = seenPoses_[neighbor->tvObject] = anchorPose;
    // 付けた相手を区域に書く（選んでいる区域なら草稿から）。
    for (auto& item : model_.rooms) {
        if (item.settings.region.regionId != regionId) { continue; }
        if (item.object.objectId == model_.selected && model_.ready) { model_.draft.stageAnchor = chosen; model_.Commit(ctx); }
        else if (Transact(ctx, Json::array({PropertyCommand(item.target.objectId, item.target.componentId, "stageAnchor", chosen)}), model_.error)) { model_.refresh = true; model_.Poll(ctx, true); }
        break;
    }
    MarkSceneChanged();
    Generate(ctx, true, false, {}, regionId, nullptr, false);
    SetNote("3D で「" + ShortChoiceLabel(RoomName(chosen), 10) + "」の隣に付け、カメラ位置を作り直しました");
}

// 付けた相手のテレビが 3D で動いたら、付いているテレビも同じだけ動かす。付いているテレビを作者が動かしたら外す。
// 動きの途中は書かず、二回続けて同じ姿勢を読んだ時（手を離した後）だけ動かす。取り消し・やり直しの後は読み直すだけにする。
void LevelEditor::FollowAnchors(EditorWorkspaceHostContext& ctx) {
    followClock_ += ctx.unscaledDeltaSeconds;
    if (followClock_ < .4f || model_.gesture || posing_ || previewState_ != 0 || drag_ != Drag::None) { return; }
    // シーンの目印が変わった後だけ読む。手を離した後の同じ姿勢を二回見るまで続ける。
    if (followPulse_ != model_.Pulses()) { followPulse_ = model_.Pulses(); followReads_ = 2; }
    const bool resync = model_.historyFrames > 0 || followResync_;
    if (!resync && followReads_ <= 0) { return; }
    followClock_ = 0; --followReads_;
    followResync_ = false;
    // 付いているテレビと、その相手。
    std::map<uint64_t, uint64_t> anchorOf;   // 付いているテレビ → 相手のテレビ
    std::map<std::string, uint64_t> tvOf;
    for (const auto& room : model_.rooms) { tvOf[room.settings.region.regionId] = room.object.objectId; }
    for (const auto& room : model_.rooms) {
        if (room.settings.stageAnchor.empty()) { continue; }
        const auto found = tvOf.find(room.settings.stageAnchor);
        if (found != tvOf.end()) { anchorOf[room.object.objectId] = found->second; }
    }
    if (anchorOf.empty()) { anchorPoses_.clear(); seenPoses_.clear(); return; }
    std::vector<uint64_t> ids;
    for (const auto& [child, parent] : anchorOf) { ids.push_back(child); ids.push_back(parent); }
    std::sort(ids.begin(), ids.end()); ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    std::map<uint64_t, Pose> now;
    std::map<uint64_t, StageObject> read;
    std::string error;
    if (!ReadStageObjects(ctx, ids, read, error)) { return; }
    for (const auto& [id, o] : read) { now[id] = o.pose; }
    if (resync) { anchorPoses_ = now; seenPoses_ = now; return; }
    std::map<uint64_t, Pose> stable;   // 前回も同じ姿勢だった物
    for (const auto& [id, pose] : now) { if (const auto seen = seenPoses_.find(id); seen != seenPoses_.end() && SamePose(seen->second, pose)) { stable[id] = pose; } }
    seenPoses_ = now;
    std::set<uint64_t> moved;
    for (const auto& [id, pose] : stable) {
        const auto known = anchorPoses_.find(id);
        if (known == anchorPoses_.end()) { anchorPoses_[id] = pose; continue; }
        if (!SamePose(known->second, pose)) { moved.insert(id); }
    }
    if (moved.empty()) { return; }
    for (const auto id : moved) {
        const Pose before = anchorPoses_[id], after = stable[id];
        anchorPoses_[id] = after;
        // 付いているテレビを作者が直接動かした：付きを外す（相手は動いていない時だけ）。
        if (const auto parent = anchorOf.find(id); parent != anchorOf.end() && !moved.contains(parent->second)) {
            for (auto& room : model_.rooms) {
                if (room.object.objectId != id) { continue; }
                if (room.object.objectId == model_.selected && model_.ready) { model_.draft.stageAnchor.clear(); model_.Commit(ctx); }
                else if (Transact(ctx, Json::array({PropertyCommand(room.target.objectId, room.target.componentId, "stageAnchor", std::string{})}), model_.error)) { model_.refresh = true; }
                SetNote("「" + ShortChoiceLabel(room.object.name, 10) + "」を 3D で動かしたので、自由配置にしました");
                break;
            }
            continue;
        }
        // 相手が動いた：付いているテレビを同じ剛体の動きで運ぶ。
        for (const auto& [child, parent] : anchorOf) {
            if (parent != id || moved.contains(child)) { continue; }
            const auto childPose = anchorPoses_.contains(child) ? anchorPoses_[child] : now[child];
            // 相手から見た相対の姿勢を保ったまま、相手の新しい姿勢へ載せ直す。
            auto next = ApplyDelta(childPose, after, MakeDelta(childPose, before, childPose, {}));
            next.scale = childPose.scale;
            if (SetStagePose(ctx, child, next, model_.error)) { anchorPoses_[child] = next; seenPoses_[child] = next; MarkSceneChanged(); }
        }
    }
}

// 圧力板から仕掛けへ信号線を引く（前に引いた同じ組の線は消してから）。
void LevelEditor::RouteCables(EditorWorkspaceHostContext& ctx, const std::string& plate, const std::string& target) {
    if (!EnsureLayout(ctx, true)) { return; }
    RefreshMachines(ctx, true);
    const auto slash = plate.find('/');
    const auto* room = layout_.Room(plate.substr(0, slash));
    const Machine* machine = nullptr;
    for (const auto& m : model_.machines) for (const auto& [receiver, label] : m.Receivers()) { if (receiver == target) { machine = &m; } }
    if (!room || !machine) { return; }
    const auto to = MachineCableEnd(*machine, layout_);
    if (!to) { return; }
    // 同じ板と行き先の古い線を消す。
    std::vector<SceneItem> cables; std::string error;
    if (QueryObjects(ctx, kSignalCableType, cables, error)) {
        for (const auto& cable : cables) {
            const auto instances = Execute(ctx, "component.instances.query", {{"objectId", cable.objectId}});
            if (!instances.ok) { continue; }
            for (const auto& c : instances.data.value("components", Json::array())) {
                if (c.value("type", "") != kSignalCableType) { continue; }
                const auto described = Execute(ctx, "component.describe", {{"objectId", cable.objectId}, {"componentId", ObjectId(c.value("componentId", Json{}))}, {"propertyIds", {"plate", "target"}}});
                if (!described.ok) { continue; }
                const auto fields = described.data.value("fields", Json::array());
                const auto* p = FieldValue(fields, "plate"); const auto* t = FieldValue(fields, "target");
                if (p && p->is_string() && t && t->is_string() && p->get<std::string>() == plate && t->get<std::string>() == target) { Execute(ctx, "scene.delete_object", {{"objectId", cable.objectId}}); }
            }
        }
    }
    const auto points = RouteCable(CableEnd(*room), *to, layout_);
    int made = 0;
    for (const auto& piece : CablePieces(points)) {
        const auto path = piece.joint ? kCableJointPath : kCableSegmentPath;
        const uint64_t id = CreateStageModel(ctx, path, piece.joint ? "信号線の継ぎ目" : "信号線", piece.pose, model_.error);
        if (!id) { break; }
        JoinFolderOf(ctx, id, room->tvObject);
        const auto added = Execute(ctx, "component.add", {{"objectId", id}, {"componentType", kSignalCableType}});
        if (!added.ok) { model_.error = added.error; break; }
        uint64_t component = 0;
        for (const auto& c : added.data.value("addedComponents", Json::array())) { if (c.value("type", "") == kSignalCableType) { component = ObjectId(c.value("componentId", Json{})); } }
        if (!component) { break; }
        Json commands = Json::array({PropertyCommand(id, component, "plate", plate), PropertyCommand(id, component, "target", target),
                                     PropertyCommand(id, component, "litModel", std::string(piece.joint ? kCableJointLit : kCableSegmentLit))});
        if (!Transact(ctx, commands, model_.error)) { break; }
        ++made;
    }
    if (made) { MarkSceneChanged(); }
}
}
