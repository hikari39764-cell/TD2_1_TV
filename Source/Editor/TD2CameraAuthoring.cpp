#include "TD2CameraAuthoring.h"

namespace TD2::Editor {
bool OpenTransitionCamera(EditorWorkspaceHostContext& ctx, const RegionSequenceBinding& binding, std::string& error) {
    if (binding.sequenceAssetGuid.empty()) { error = "先に切り替えの演出を結び付けてください"; return false; }
    const auto opened = Execute(ctx, "authoring.session.open", {{"workspace", "cinematics"}, {"assetGuid", binding.sequenceAssetGuid}});
    if (!opened.ok) { error = opened.error; return false; }
    const auto session = opened.data.value("sessionId", "");
    const auto configured = Execute(ctx, "cinematics.camera_edit_context", {{"sessionId", session}, {"speed", binding.speed}});
    if (!configured.ok) { error = configured.error; return false; }
    const auto reveal = Execute(ctx, "authoring.reveal", {{"sessionId", session}});
    error = reveal.ok ? "" : reveal.error;
    return reveal.ok;
}

bool OpenRegionCamera(EditorWorkspaceHostContext& ctx, const std::string& region, const std::string& name,
                      std::string& error) {
    (void)name;
    // 区域を選ぶと Scene の共有カメラへ移る。演出の端点は複製しない。
    std::vector<SceneItem> items;
    if (!QueryObjects(ctx, kRegionComponentType, items, error)) return false;
    for (const auto& item : items) {
        const auto instances = Execute(ctx, "component.instances.query", {{"objectId", item.objectId}});
        if (!instances.ok) { error = instances.error; return false; }
        for (const auto& component : instances.data.value("components", Json::array())) {
            if (component.value("type", "") != kRegionComponentType) continue;
            const auto read = Execute(ctx, "component.describe", {{"objectId", item.objectId},
                {"componentId", ObjectId(component.at("componentId"))}, {"propertyIds", {"regionId", "cameraObject"}}});
            if (!read.ok) { error = read.error; return false; }
            const auto fields = read.data.value("fields", Json::array());
            const auto* id = FieldValue(fields, "regionId");
            if (!id || !id->is_string() || id->get<std::string>() != region) continue;
            const auto* reference = FieldValue(fields, "cameraObject");
            const uint64_t camera = !reference ? 0 : reference->is_object() && reference->value("$hikariRef", "") == "SceneObject"
                ? ObjectId(reference->value("id", Json{})) : ObjectId(*reference);
            if (!camera) { error = "この区域にはカメラがありません。区域のプロパティでカメラを選んでください"; return false; }
            const auto reveal = Execute(ctx, "gameplay_camera.reveal", {{"cameraId", camera}});
            error = reveal.ok ? "" : reveal.error;
            return reveal.ok;
        }
    }
    error = "区域が見つかりません";
    return false;
}
}
