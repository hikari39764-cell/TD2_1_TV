#include "TD2StageTools.h"
#include "../Common/TD2Math.h"
#include <algorithm>
#include <cctype>
#include <cmath>

namespace TD2::Editor {
namespace {
std::array<double, 3> Vec(const Json& v, std::array<double, 3> fallback) {
    if (!v.is_array() || v.size() != 3) { return fallback; }
    for (const auto& n : v) { if (!n.is_number()) { return fallback; } }
    return {v[0].get<double>(), v[1].get<double>(), v[2].get<double>()};
}
}

// scene.objects.query の詳しい一件を読む。
static StageObject ParseStageObject(uint64_t objectId, const Json& o) {
    StageObject out;
    out.objectId = objectId;
    out.name = o.value("name", "");
    out.parentId = ObjectId(o.value("parentId", Json{}));
    const auto t = o.value("localTransform", Json::object());
    out.pose.position = Vec(t.value("position", Json{}), {0, 0, 0});
    out.pose.rotation = Vec(t.contains("rotation") ? t["rotation"] : t.value("rotationEulerDeg", Json{}), {0, 0, 0});
    out.pose.scale = Vec(t.value("scale", Json{}), {1, 1, 1});
    if (o.contains("model") && o["model"].is_object()) { out.television = FindTelevisionModel(o["model"].value("assetId", "")); }
    if (o.contains("worldBounds") && o["worldBounds"].is_object()) {
        out.lo = Vec(o["worldBounds"].value("min", Json{}), {0, 0, 0});
        out.hi = Vec(o["worldBounds"].value("max", Json{}), {0, 0, 0});
        out.hasBounds = true;
    }
    return out;
}

bool ReadStageObject(EditorWorkspaceHostContext& ctx, uint64_t objectId, StageObject& out, std::string& error) {
    out = {};
    const auto result = Execute(ctx, "scene.objects.query", {{"objectIds", Json::array({objectId})}, {"detail", true}});
    if (!result.ok) { error = result.error; return false; }
    const auto objects = result.data.value("objects", Json::array());
    if (!objects.is_array() || objects.empty()) { error = "物体がありません：" + std::to_string(objectId); return false; }
    out = ParseStageObject(objectId, objects.front());
    return true;
}

bool ReadStageObjects(EditorWorkspaceHostContext& ctx, const std::vector<uint64_t>& objectIds, std::map<uint64_t, StageObject>& out, std::string& error) {
    out.clear();
    // 一頁は 64 件まで。
    for (size_t start = 0; start < objectIds.size(); start += 64) {
        const size_t count = std::min<size_t>(64, objectIds.size() - start);
        Json ids = Json::array();
        for (size_t i = start; i < start + count; ++i) { ids.push_back(objectIds[i]); }
        const auto result = Execute(ctx, "scene.objects.query", {{"objectIds", ids}, {"detail", true}, {"limit", static_cast<int>(count)}});
        if (!result.ok) { error = result.error; return false; }
        for (const auto& o : result.data.value("objects", Json::array())) {
            if (const auto id = ObjectId(o.value("objectId", Json{}))) { out[id] = ParseStageObject(id, o); }
        }
    }
    return true;
}

// 元パスの一部（"TD2Props/SM_X.gltf"）からモデル資産の GUID を引く。scene.create_object は導入後の成果物のパスで
// 探すので、元パスのままでは見つからない。GUID は資産の設定に固定なので、一度引いた物は覚えておく。
static std::string ModelAssetGuid(EditorWorkspaceHostContext& ctx, std::string_view path, std::string& error) {
    static std::map<std::string, std::string, std::less<>> known;
    if (const auto found = known.find(path); found != known.end()) { return found->second; }
    const auto normalize = [](std::string text) {
        for (auto& c : text) { c = c == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
        return text;
    };
    const auto wanted = normalize(std::string(path));
    const auto file = wanted.substr(wanted.find_last_of('/') == std::string::npos ? 0 : wanted.find_last_of('/') + 1);
    const auto result = Execute(ctx, "asset.query", {{"type", "Model"}, {"keyword", file}, {"limit", 128}});
    if (!result.ok) { error = result.error; return {}; }
    for (const auto& asset : result.data.value("assets", Json::array())) {
        const auto source = normalize(asset.value("path", ""));
        const auto guid = asset.value("guid", "");
        if (!guid.empty() && source.size() >= wanted.size() && source.compare(source.size() - wanted.size(), wanted.size(), wanted) == 0) {
            known.emplace(std::string(path), guid);
            return guid;
        }
    }
    error = "モデルの資産が見つかりません：" + std::string(path);
    return {};
}

uint64_t CreateStageModel(EditorWorkspaceHostContext& ctx, std::string_view path, std::string_view name, const Pose& pose, std::string& error) {
    const auto guid = ModelAssetGuid(ctx, path, error);
    if (guid.empty()) { return 0; }
    const auto created = Execute(ctx, "scene.create_object", {{"model", guid}, {"name", std::string(name)},
        {"position", pose.position}, {"rotation", pose.rotation}, {"scale", pose.scale}});
    if (!created.ok) { error = created.error; return 0; }
    const auto id = ObjectId(created.data.value("objectId", Json{}));
    if (!id) { error = "モデルを作りましたが物体 ID が返りません"; }
    return id;
}

bool SetStagePose(EditorWorkspaceHostContext& ctx, uint64_t objectId, const Pose& pose, std::string& error) {
    const auto set = Execute(ctx, "scene.set_transform", {{"objectId", objectId}, {"position", pose.position}, {"rotation", pose.rotation}, {"scale", pose.scale}});
    if (!set.ok) { error = set.error; }
    return set.ok;
}

void JoinFolderOf(EditorWorkspaceHostContext& ctx, uint64_t objectId, uint64_t like) {
    if (!like) { return; }
    const auto folders = Execute(ctx, "scene.folder.query", Json::object());
    if (!folders.ok) { return; }
    for (const auto& o : folders.data.value("objects", Json::array())) {
        if (ObjectId(o.value("objectId", Json{})) != like) { continue; }
        const auto folder = o.value("folder", "");
        if (!folder.empty()) { Execute(ctx, "scene.folder.assign", {{"path", folder}, {"objectIds", Json::array({objectId})}}); }
        return;
    }
}

double StageWidth(const StageObject& o) {
    if (o.television) { return o.television->width * std::abs(o.pose.scale[0]); }
    // 回っている物体の外接箱は幅より広いので、横の二辺の小さい方を使う。
    return o.hasBounds ? (std::min)(o.hi[0] - o.lo[0], o.hi[2] - o.lo[2]) : 1.0;
}

double StageHeight(const StageObject& o) {
    if (o.television) { return o.television->height * std::abs(o.pose.scale[1]); }
    return o.hasBounds ? o.hi[1] - o.lo[1] : 1.0;
}

Pose PoseBeside(const StageObject& anchor, double width, double height, int side, double gap) {
    // 筐体の原点は底面の中心。横は anchor の X 方向に幅の半分ずつ、上下は高さで離す。
    MachineObjectDelta d;
    if (side == 1 || side == -1) { d.position = {side * (StageWidth(anchor) * .5 + width * .5 + gap), 0, 0}; }
    else if (side == 2) { d.position = {0, StageHeight(anchor) + gap, 0}; }
    else { d.position = {0, -(height + gap), 0}; }
    return ApplyDelta(anchor.pose, anchor.pose, d);
}

bool PoseAtView(EditorWorkspaceHostContext& ctx, Pose& pose, std::string& error) {
    const auto state = Execute(ctx, "editor.state", Json::object());
    if (!state.ok) { error = state.error; return false; }
    const auto camera = state.data.value("camera", Json::object());
    const auto eye = Vec(camera.value("position", Json{}), {0, 2, 6});
    const auto target = Vec(camera.value("target", Json{}), {0, 0, 0});
    std::array<double, 3> dir{target[0] - eye[0], target[1] - eye[1], target[2] - eye[2]};
    const double length = std::hypot(dir[0], dir[1], dir[2]);
    if (length < 1e-6) { dir = {0, 0, -1}; } else { for (auto& v : dir) { v /= length; } }
    // 視線が床（y = 0）に当たる所。当たらない時は前方 4 m の床。近すぎ・遠すぎは水平の距離で抑える。
    const double flat = std::hypot(dir[0], dir[2]);
    double horizontal = 4;
    if (dir[1] < -.05 && eye[1] > 0) { horizontal = std::clamp(-eye[1] / dir[1] * flat, 1.5, 12.0); }
    pose.position = flat < 1e-3 ? std::array<double, 3>{eye[0], 0, eye[2]}
                                : std::array<double, 3>{eye[0] + dir[0] / flat * horizontal, 0, eye[2] + dir[2] / flat * horizontal};
    // 画面（+Z）をカメラへ向ける。
    pose.rotation = {0, std::round(std::atan2(eye[0] - pose.position[0], eye[2] - pose.position[2]) * 180 / Math::kPi / 15) * 15, 0};
    return true;
}
}
