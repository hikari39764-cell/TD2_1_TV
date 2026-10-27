// 機構の姿勢：状態ごとの 3D の物体の姿勢を読み、状態を試しに見せ、作者が動かした姿勢を状態 B として記録する。
#include "TD2LevelEditor.h"

namespace TD2::Editor::Level {

// ---------------------------------------------------------------- 状態の見本と、手作りの姿勢
bool LevelEditor::ReadPoses(EditorWorkspaceHostContext& ctx, std::map<uint64_t, Pose>& poses, std::map<uint64_t, Json>& expected, std::string& sceneId, uint64_t& revision) {
    const auto layout = Execute(ctx, "scene.layout.query", Json::object());
    if (!layout.ok) { model_.error = layout.error; return false; }
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

bool LevelEditor::PreviewState(EditorWorkspaceHostContext& ctx, int state) {
    if (posing_) { return false; }
    std::map<uint64_t, Pose> poses; std::map<uint64_t, Json> expected; std::string sceneId; uint64_t revision = 0;
    if (!ReadPoses(ctx, poses, expected, sceneId, revision)) { return false; }
    Json commands = Json::array();
    // まず A へ戻す。
    for (const auto& [id, a] : poseA_) {
        if (expected.contains(id)) { commands.push_back({{"objectId", id}, {"expected", expected[id]}, {"position", a.position}, {"rotation", a.rotation}}); }
    }
    if (!commands.empty()) {
        const auto applied = Execute(ctx, "scene.layout.apply", {{"sceneId", sceneId}, {"documentRevision", revision}, {"commands", commands}});
        if (!applied.ok) { model_.error = applied.error; return false; }
        if (!ReadPoses(ctx, poses, expected, sceneId, revision)) { return false; }
    }
    poseA_.clear(); previewState_ = 0;
    auto* m = SelectedMachine();
    if (state == 0 || !m || m->kind != MachineKind::Mechanism) { return true; }
    const auto& deltas = state < 0 ? m->deltasC : m->deltas;
    commands = Json::array();
    for (const auto& d : deltas) {
        const auto a = poses.find(d.object);
        if (a == poses.end()) { continue; }
        poseA_[d.object] = a->second;
        const auto base = d.anchor ? poses.find(d.anchor) : a;
        if (base == poses.end()) { continue; }
        const auto b = ApplyDelta(a->second, base->second, d);
        commands.push_back({{"objectId", d.object}, {"expected", expected[d.object]}, {"position", b.position}, {"rotation", b.rotation}});
    }
    if (!commands.empty()) {
        const auto applied = Execute(ctx, "scene.layout.apply", {{"sceneId", sceneId}, {"documentRevision", revision}, {"commands", commands}});
        if (!applied.ok) { model_.error = applied.error; poseA_.clear(); return false; }
    }
    previewState_ = state;
    return true;
}

// 手作りの機構：状態 B（または天秤の状態 C）をシーンに置き、作者がビューポートで動かしてから保存で相対の姿勢に直す。
bool LevelEditor::BeginPose(EditorWorkspaceHostContext& ctx) {
    if (previewState_ != 0) { PreviewState(ctx, 0); }
    auto* m = SelectedMachine();
    if (!m || m->kind != MachineKind::Mechanism) { return false; }
    std::map<uint64_t, Pose> poses; std::map<uint64_t, Json> expected; std::string sceneId; uint64_t revision = 0;
    if (!ReadPoses(ctx, poses, expected, sceneId, revision)) { return false; }
    const auto& deltas = machineC_ ? m->deltasC : m->deltas;
    Json commands = Json::array();
    poseA_.clear();
    for (const auto& d : deltas) {
        const auto a = poses.find(d.object);
        if (a == poses.end()) { model_.error = "物体がありません：" + std::to_string(d.object); return false; }
        poseA_[d.object] = a->second;
        const auto base = d.anchor ? poses.find(d.anchor) : a;
        if (base == poses.end()) { model_.error = "基準の物体がありません：" + std::to_string(d.anchor); return false; }
        const auto b = ApplyDelta(a->second, base->second, d);
        commands.push_back({{"objectId", d.object}, {"expected", expected[d.object]}, {"position", b.position}, {"rotation", b.rotation}});
    }
    if (!commands.empty()) {
        const auto applied = Execute(ctx, "scene.layout.apply", {{"sceneId", sceneId}, {"documentRevision", revision}, {"commands", commands}});
        if (!applied.ok) { model_.error = applied.error; return false; }
    }
    posing_ = true; posingC_ = machineC_; model_.error.clear();
    return true;
}

bool LevelEditor::SavePose(EditorWorkspaceHostContext& ctx) {
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
        Pose base = a->second;
        if (d.anchor) { base = poseA_.contains(d.anchor) ? poseA_[d.anchor] : poses.contains(d.anchor) ? poses[d.anchor] : base; }
        d = MakeDelta(a->second, base, b->second, d);
        restore.push_back({{"objectId", d.object}, {"expected", expected[d.object]}, {"position", a->second.position}, {"rotation", a->second.rotation}});
    }
    if (!restore.empty()) {
        const auto applied = Execute(ctx, "scene.layout.apply", {{"sceneId", sceneId}, {"documentRevision", revision}, {"commands", restore}});
        if (!applied.ok) { model_.error = "状態 B は読みましたが、状態 A に戻せません：" + applied.error; return false; }
    }
    posing_ = false; poseA_.clear();
    // 手で姿勢を直したら、レシピの自動の動きはやめる（手作りの機構にする）。
    after.recipe.clear();
    return WriteSelectedMachine(ctx, after);
}

void LevelEditor::CancelPose(EditorWorkspaceHostContext& ctx) {
    if (!posing_) { return; }
    posing_ = false;
    std::map<uint64_t, Pose> poses; std::map<uint64_t, Json> expected; std::string sceneId; uint64_t revision = 0;
    if (!ReadPoses(ctx, poses, expected, sceneId, revision)) { return; }
    Json restore = Json::array();
    for (const auto& [id, a] : poseA_) {
        if (expected.contains(id)) { restore.push_back({{"objectId", id}, {"expected", expected[id]}, {"position", a.position}, {"rotation", a.rotation}}); }
    }
    poseA_.clear();
    if (restore.empty()) { return; }
    const auto applied = Execute(ctx, "scene.layout.apply", {{"sceneId", sceneId}, {"documentRevision", revision}, {"commands", restore}});
    if (!applied.ok) { model_.error = "状態 A に戻せません：" + applied.error; }
}
}
