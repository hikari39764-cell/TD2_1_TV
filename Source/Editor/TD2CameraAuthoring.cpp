#include "TD2CameraAuthoring.h"

namespace TD2::Editor {
bool OpenTransitionCamera(EditorWorkspaceHostContext& ctx, const RegionSequenceBinding& binding, std::string& error) {
    if (binding.sequenceAssetGuid.empty()) { error = "请先绑定切换演出。"; return false; }
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
    // 区域選択は Scene の共有機位へ案内する。隣接演出を列挙して端点を複写しない。
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
            if (!camera) { error = "该区域尚未指定共享机位，请在区域属性中选择摄像机。"; return false; }
            const auto reveal = Execute(ctx, "gameplay_camera.reveal", {{"cameraId", camera}});
            error = reveal.ok ? "" : reveal.error;
            return reveal.ok;
        }
    }
    error = "未找到区域。";
    return false;
}
} // namespace TD2::Editor
