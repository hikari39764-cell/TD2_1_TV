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

}
