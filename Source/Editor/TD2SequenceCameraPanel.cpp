#include "TD2SequenceCameraPanel.h"
#include "TD2EditorSupport.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <optional>
#include <set>
#include <sstream>

namespace TD2::Editor {
namespace {
using Clock = std::chrono::steady_clock;
constexpr std::string_view kPrefix = "td2.sequence_camera.";
struct Reply final : IEditorOperationResultSink {
    bool ok = false; std::string code, message; Json data;
    void Receive(bool success, std::string_view c, std::string_view m, std::string_view json) override {
        ok = success; code = c; message = m; data = Json::parse(json, nullptr, false);
    }
};
Reply Call(EditorWorkspaceHostContext& ctx, std::string_view operation, const Json& args) {
    Reply r; ctx.commands->ExecuteOperation(operation, args.dump(), r); return r;
}
std::string Failure(std::string_view op, const Reply& reply) { return std::string(op) + ": " + reply.code + ": " + reply.message; }
bool Number(const Json& value) { return value.is_number() && std::isfinite(value.get<double>()); }
bool Vector3(const Json& value) { return value.is_array() && value.size() == 3 && std::all_of(value.begin(), value.end(), Number); }
std::string Num(double value) { std::ostringstream s; s << std::fixed << std::setprecision(2) << value; return s.str(); }
std::string VectorLabel(const Json& v) { return Num(v[0].get<double>()) + ", " + Num(v[1].get<double>()) + ", " + Num(v[2].get<double>()); }
bool Close(const Json& a, const Json& b, bool angle = false) {
    if (Vector3(a) && Vector3(b)) { for (size_t i = 0; i < 3; ++i) if (!Close(a[i], b[i], angle)) return false; return true; }
    if (!Number(a) || !Number(b)) return false;
    double d = a.get<double>() - b.get<double>(); if (angle) d = std::remainder(d, 360.0);
    return std::abs(d) < 0.001;
}
struct Pose { Json position, rotation; double fov = 50, nearClip = .1, farClip = 2000; bool valid = false, fullLens = true; std::string label, reason; };
struct SequenceShape {
    double duration = 0; std::string slot, name;
    std::array<std::string, 2> transform, lens;
};

// 対象キーの場所だけを返す。ID・時間・補間・中間キーや無関係なフィールドを再構築しない。
std::optional<SequenceShape> InspectSequence(const Json& doc, std::string& why) {
    const auto unsupported = [&why](std::string_view detail) -> std::optional<SequenceShape> { why = std::string(detail) + "；请在原生演出编辑器修改"; return {}; };
    if (!doc.is_object() || !doc.contains("durationSeconds") || !Number(doc["durationSeconds"]) || doc["durationSeconds"].get<double>() <= 0)
        return unsupported("演出时长无效");
    if (!doc.contains("bindings") || !doc["bindings"].is_array() || doc["bindings"].size() != 1) return unsupported("快捷编辑仅支持一个相机槽");
    const auto& binding = doc["bindings"][0];
    if (!binding.is_object() || binding.value("targetKind", "") != "slot" || !binding.contains("id") || binding.value("slotName", "").empty())
        return unsupported("快捷编辑需要外部相机槽");
    SequenceShape shape; shape.duration = doc["durationSeconds"].get<double>(); shape.slot = binding["slotName"].get<std::string>(); shape.name = doc.value("name", "原生演出");
    if (!doc.contains("tracks") || !doc["tracks"].is_array()) return unsupported("演出轨道缺失");
    int cuts = 0, transforms = 0, lenses = 0;
    for (size_t t = 0; t < doc["tracks"].size(); ++t) {
        const auto& track = doc["tracks"][t]; if (!track.is_object()) return unsupported("轨道结构无效");
        const std::string type = track.value("type", "");
        if (!track.value("enabled", true)) continue;
        if (type == "cameraCut") {
            ++cuts;
            if (!track.contains("clips") || !track["clips"].is_array() || track["clips"].size() != 1) return unsupported("快捷编辑仅支持单 Shot");
            const auto& shot = track["clips"][0];
            if (!shot.is_object() || shot.value("cameraBindingId", Json{}) != binding["id"] ||
                !Close(shot.value("startTimeSeconds", Json{}), 0) || !Close(shot.value("durationSeconds", Json{}), shape.duration) ||
                !shot.contains("camera") || !shot["camera"].is_object() || shot["camera"].value("owned", true)) return unsupported("Shot 未覆盖整段或使用自有相机");
            continue;
        }
        if (type != "cameraTransform" && type != "cameraLens") return unsupported("包含其他启用的动画轨道");
        if (type == "cameraTransform") ++transforms; else ++lenses;
        if (!track.contains("channels") || !track["channels"].is_array() || track["channels"].size() != 1) return unsupported("相机轨道有多个通道");
        const auto& channel = track["channels"][0];
        if (channel.value("cameraBindingId", Json{}) != binding["id"] || !channel.contains("keyframes") || !channel["keyframes"].is_array()) return unsupported("相机轨道未对应 Shot 槽");
        std::set<std::string> ids; std::array<int, 2> found{};
        for (size_t k = 0; k < channel["keyframes"].size(); ++k) {
            const auto& key = channel["keyframes"][k];
            if (!key.is_object() || !key.contains("id") || !ids.insert(key["id"].dump()).second || !Number(key.value("timeSeconds", Json{}))) return unsupported("关键帧 ID 或时间无效");
            const double time = key["timeSeconds"].get<double>();
            if (time < 0 || time > shape.duration) return unsupported("关键帧超出演出时段");
            for (size_t end = 0; end < 2; ++end) if (std::abs(time - (end ? shape.duration : 0)) < 1e-6) {
                ++found[end]; const std::string path = "/tracks/" + std::to_string(t) + "/channels/0/keyframes/" + std::to_string(k);
                if (type == "cameraTransform") {
                    if (!Vector3(key.value("position", Json{})) || !Vector3(key.value("rotationEulerDeg", Json{}))) return unsupported("首末机位姿态缺失");
                    shape.transform[end] = path;
                } else {
                    if (!Number(key.value("verticalFovDegrees", Json{})) || !Number(key.value("nearClip", Json{})) || !Number(key.value("farClip", Json{}))) return unsupported("首末镜头参数缺失");
                    shape.lens[end] = path;
                }
            }
        }
        if (found[0] != 1 || found[1] != 1) return unsupported("需要唯一的 0 秒与终点关键帧");
    }
    if (cuts != 1 || transforms != 1 || lenses != 1) return unsupported("快捷编辑需要单 Shot、单姿态轨与单镜头轨");
    why.clear(); return shape;
}
Pose Endpoint(const Json& doc, const SequenceShape& shape, size_t end) {
    Pose p; const auto& t = doc.at(Json::json_pointer(shape.transform[end])); const auto& l = doc.at(Json::json_pointer(shape.lens[end]));
    p.position = t["position"]; p.rotation = t["rotationEulerDeg"]; p.fov = l["verticalFovDegrees"].get<double>();
    p.nearClip = l["nearClip"].get<double>(); p.farClip = l["farClip"].get<double>(); p.valid = true; return p;
}
bool SamePose(const Pose& a, const Pose& b) {
    return a.valid && b.valid && Close(a.position, b.position) && Close(a.rotation, b.rotation, true) && Close(a.fov, b.fov) &&
        (!a.fullLens || (Close(a.nearClip, b.nearClip) && Close(a.farClip, b.farClip)));
}
uint64_t Component(const Json& object, std::string_view type) {
    if (!object.contains("components") || !object["components"].is_array()) return 0;
    for (const auto& c : object["components"]) if (c.is_object() && c.value("type", "") == type) return ObjectId(c.value("componentId", Json{}));
    return 0;
}
bool ReadObject(EditorWorkspaceHostContext& ctx, uint64_t id, Json& object, std::string& error) {
    const auto r = Call(ctx, "scene.objects.query", {{"objectIds", {id}}, {"detail", true}});
    if (!r.ok) { error = Failure("scene.objects.query", r); return false; }
    if (r.data.is_object() && r.data.contains("objects") && r.data["objects"].is_array())
        for (const auto& item : r.data["objects"]) if (ObjectId(item.value("objectId", Json{})) == id) { object = item; return true; }
    error = "镜头对象已不存在"; return false;
}
Pose ReadCamera(EditorWorkspaceHostContext& ctx, uint64_t id) {
    Pose p; if (!id) { p.reason = "未选择参考镜头"; return p; }
    Json object; if (!ReadObject(ctx, id, object, p.reason)) return p;
    p.label = object.value("name", "相机") + " #" + std::to_string(id);
    const uint64_t camera = Component(object, "CameraComponent");
    if (!camera) { p.reason = "所选对象没有 CameraComponent"; return p; }
    // 無父の作者姿態だけをコピーする。親の影響を localTransform で代用しない。
    const auto parent = object.find("parentObjectId");
    if (parent == object.end()) { p.reason = "查询未提供父节点身份，无法确认世界姿态；请用当前编辑视角或原生编辑器"; return p; }
    if (!parent->is_null() && (!parent->is_number_integer() || ObjectId(*parent) != 0)) { p.reason = "参考相机有父节点或父节点身份不明；请使用无父节点相机"; return p; }
    const auto transform = object.value("localTransform", Json{});
    if (!transform.is_object() || !Vector3(transform.value("position", Json{})) || !Vector3(transform.value("rotation", Json{}))) { p.reason = "相机本地姿态格式不可用"; return p; }
    const auto r = Call(ctx, "component.describe", {{"objectId", id}, {"componentId", camera}, {"propertyIds", {"orthographic", "verticalFovDegrees", "nearClip", "farClip"}}});
    if (!r.ok) { p.reason = Failure("component.describe", r); return p; }
    const auto fields = r.data.value("fields", Json::array());
    const auto* ortho = FieldValue(fields, "orthographic"); const auto* fov = FieldValue(fields, "verticalFovDegrees");
    const auto* near = FieldValue(fields, "nearClip"); const auto* far = FieldValue(fields, "farClip");
    if (!ortho || !ortho->is_boolean() || ortho->get<bool>() || !fov || !near || !far || !Number(*fov) || !Number(*near) || !Number(*far)) { p.reason = "快捷同步仅支持完整的透视相机参数"; return p; }
    p.position = transform["position"]; p.rotation = transform["rotation"]; p.fov = fov->get<double>(); p.nearClip = near->get<double>(); p.farClip = far->get<double>();
    p.valid = p.fov > 0 && p.fov < 180 && p.nearClip > 0 && p.farClip > p.nearClip;
    if (!p.valid) p.reason = "镜头裁面或视野无效"; return p;
}
Pose ReadEditorView(EditorWorkspaceHostContext& ctx) {
    Pose p; p.label = "当前普通透视编辑视角"; p.fullLens = false;
    const auto r = Call(ctx, "editor.state", Json::object());
    if (!r.ok) { p.reason = Failure("editor.state", r); return p; }
    const auto camera = r.data.value("camera", Json{});
    if (!camera.is_object() || camera.value("orthographic", true) || !Vector3(camera.value("position", Json{})) ||
        !Vector3(camera.value("target", Json{})) || !Number(camera.value("fovYDegrees", Json{}))) { p.reason = "仅支持本工作区普通透视编辑视角；不支持正交或 Cinematics 观察器"; return p; }
    p.position = camera["position"]; std::array<double, 3> d{}; double length = 0;
    for (size_t i = 0; i < 3; ++i) { d[i] = camera["target"][i].get<double>() - p.position[i].get<double>(); length += d[i] * d[i]; }
    if (length < 1e-12) { p.reason = "编辑视角方向无效"; return p; }
    length = std::sqrt(length); for (auto& v : d) v /= length;
    constexpr double degrees = 180.0 / 3.14159265358979323846;
    p.rotation = Json::array({-std::asin(std::clamp(d[1], -1.0, 1.0)) * degrees, std::atan2(d[0], d[2]) * degrees, 0.0});
    p.fov = camera["fovYDegrees"].get<double>(); p.valid = p.fov > 0 && p.fov < 180; return p;
}
} // namespace

struct SequenceCameraPanel::State {
    struct EndUi { bool useView = false, picker = false; std::string search; uint64_t cursor = 0, next = 0, readId = 0; bool searching = false, hasNext = false; std::vector<uint64_t> history; std::vector<SceneItem> cameras; Pose pose; };
    struct Impact { uint64_t object = 0, component = 0, camera = 0; bool hint = false; std::string a, b, slot; };
    struct PreviewBinding {
        uint64_t object = 0, component = 0, regionComponent = 0, camera = 0;
        std::string asset, slot;
        std::string Signature() const { return Json::array({object, component, regionComponent, camera, asset, slot}).dump(); }
    };
    std::string identity, session, why, status, previewStatus = "原生预览尚未核对";
    Json document; std::optional<SequenceShape> shape;
    std::array<EndUi, 2> ends;
    bool changed = false, needsSave = false, previewPending = false;
    std::optional<PreviewBinding> previewRebind;
    std::string confirmedPreviewBinding;
    double previewTime = 0; uint64_t previewCamera = 0;
    Clock::time_point refreshed{}, polled{};
    std::vector<Impact> impacts;
    bool scanning = false, scanComplete = false, details = false, bindPreviewAfterScan = false;
    uint64_t scanOffset = 0, scanNext = 0; size_t scanIndex = 0;
    std::vector<std::array<uint64_t, 3>> scanQueue;
    bool scanHasNext = false;
    int impactPage = 0;
    int pendingEnd = -1; Json pendingBefore, pendingAfter;
    bool sharedAck = false, awaitingConfirmation = false;
    std::string confirmedSignature;

    bool Load(EditorWorkspaceHostContext& ctx, const RegionSequenceBinding& draft, std::string& error) {
        // 同じ資産の原生セッションを再取得し、接続切替や再読込後も未保存状態を復元する。
        const auto open = Call(ctx, "authoring.session.open", {{"workspace", "cinematics"}, {"assetGuid", draft.sequenceAssetGuid}});
        if (!open.ok || !open.data.is_object() || !open.data.contains("sessionId") || !open.data["sessionId"].is_string()) { error = Failure("authoring.session.open", open); return false; }
        session = open.data["sessionId"].get<std::string>();
        if (!open.data.contains("dirty") || !open.data["dirty"].is_boolean()) { error = "无法确认原生会话的未保存状态"; return false; }
        needsSave = open.data["dirty"].get<bool>();
        if (needsSave) status = "原生演出草稿尚未保存，可重试保存";
        const auto read = Call(ctx, "cinematics.document", {{"sessionId", session}});
        if (!read.ok || !read.data.is_object() || !read.data.contains("document")) { error = Failure("cinematics.document", read); return false; }
        if (document != read.data["document"]) previewStatus = "原生预览尚未核对（草稿已更新）";
        document = read.data["document"]; shape = InspectSequence(document, why); refreshed = Clock::now();
        for (size_t end = 0; end < 2; ++end) {
            auto& e = ends[end]; e.readId = end ? draft.targetCameraObject : draft.sourceCameraObject;
            e.pose = e.useView ? ReadEditorView(ctx) : ReadCamera(ctx, e.readId);
        }
        return true;
    }
    void SaveNative(EditorWorkspaceHostContext& ctx, const RegionSequenceBinding& draft, std::string_view success, std::string& error) {
        // 保存結果だけで成功表示にせず、原生 dirty が解消されたことまで確認する。
        needsSave = true; status = "原生演出草稿尚未保存";
        const auto saved = Call(ctx, "cinematics.save", {{"sessionId", session}});
        if (!saved.ok) { error = "原生演出保存失败：" + Failure("cinematics.save", saved); Load(ctx, draft, error); return; }
        error.clear(); status = "保存操作已返回，正在确认原生草稿状态";
        if (!Load(ctx, draft, error)) return;
        if (needsSave) { error = "原生会话仍有未保存修改，请重试保存"; return; }
        status = success;
    }
    void BeginScan() {
        scanning = true; scanComplete = false; scanOffset = scanNext = 0; scanQueue.clear(); scanIndex = 0; scanHasNext = false; impacts.clear(); impactPage = 0;
    }
    std::vector<std::string> Edges(const RegionSequenceBinding& draft) const {
        std::set<std::string> edges;
        const auto edge = [](std::string a, std::string b) { if (a > b) std::swap(a, b); return a + " ↔ " + b; };
        edges.insert(edge(draft.regionA, draft.regionB));
        for (const auto& i : impacts) edges.insert(edge(i.a, i.b));
        return {edges.begin(), edges.end()};
    }
    std::string Signature() const {
        std::vector<std::string> values;
        for (const auto& i : impacts) values.push_back(std::to_string(i.object) + "/" + std::to_string(i.component) + "/" + i.a + "/" + i.b + "/" + i.slot + "/" + std::to_string(i.camera) + "/" + std::to_string(i.hint));
        std::sort(values.begin(), values.end()); std::string s; for (const auto& v : values) s += v + "\n"; return s;
    }
    void ApplyWrite(EditorWorkspaceHostContext& ctx, const RegionSequenceBinding& draft, std::string& error) {
        // 共有確認の間に原生編集が進んだ場合、古い全ドキュメントで上書きしない。
        const auto fresh = Call(ctx, "cinematics.document", {{"sessionId", session}});
        if (!fresh.ok || fresh.data.value("document", Json{}) != pendingBefore) { error = "原生草稿已变化；已取消本次写入，请刷新后重试"; pendingEnd = -1; return; }
        const auto edit = Call(ctx, "cinematics.edit", {{"sessionId", session}, {"command", {{"action", "replace_sequence"}, {"value", pendingAfter}}}});
        const int end = pendingEnd; pendingEnd = -1; awaitingConfirmation = sharedAck = false; confirmedSignature.clear();
        if (!edit.ok) { error = Failure("cinematics.edit", edit); return; }
        if (!edit.data.is_object() || !edit.data.value("documentChanged", false)) { error = "原生文档未确认变更；未执行保存"; return; }
        SaveNative(ctx, draft, std::string(end ? "终点" : "起点") + "已写入并保存原生演出", error);
    }
    void AdvanceScan(EditorWorkspaceHostContext& ctx, const RegionSequenceBinding& draft, std::string& error) {
        if (!scanning) return;
        const auto fail = [&](std::string value) { error = std::move(value); scanning = false; pendingEnd = -1; bindPreviewAfterScan = false; };
        if (scanQueue.empty()) {
            const auto r = Call(ctx, "scene.objects.query", {{"componentType", kRegionSequenceComponentType}, {"detail", true}, {"offset", scanOffset}, {"limit", 16}});
            if (!r.ok || !r.data.is_object() || !r.data.contains("objects") || !r.data["objects"].is_array()) { fail(Failure("scene.objects.query", r)); return; }
            for (const auto& object : r.data["objects"]) {
                const auto id = ObjectId(object.value("objectId", Json{})); const bool hint = Component(object, "CameraComponent") != 0;
                for (const auto& component : object.value("components", Json::array()))
                    if (component.value("type", "") == kRegionSequenceComponentType) scanQueue.push_back({id, ObjectId(component.value("componentId", Json{})), hint ? 1u : 0u});
            }
            scanHasNext = r.data.contains("nextOffset"); scanNext = scanHasNext ? ObjectId(r.data["nextOffset"]) : 0;
            if (scanHasNext && scanNext <= scanOffset) { fail("共享资产扫描分页未前进"); return; }
            scanIndex = 0;
        }
        // 一フレームの読取は四件まで。大きな Scene でも画布操作を長時間止めない。
        for (int budget = 0; scanIndex < scanQueue.size() && budget < 4; ++budget, ++scanIndex) {
            const auto ids = scanQueue[scanIndex];
            const auto r = Call(ctx, "component.describe", {{"objectId", ids[0]}, {"componentId", ids[1]}, {"propertyIds", {"regionA", "regionB", "sequenceAssetGuid", "sequenceSlotName", "cameraObject"}}});
            if (!r.ok) { fail(Failure("component.describe", r)); return; }
            const auto fields = r.data.value("fields", Json::array()); const auto* guid = FieldValue(fields, "sequenceAssetGuid");
            if (!guid || !guid->is_string()) { fail("无法完整核对共享演出引用"); return; }
            if (guid->get<std::string>() != draft.sequenceAssetGuid) continue;
            const auto* a = FieldValue(fields, "regionA"); const auto* b = FieldValue(fields, "regionB"); const auto* slot = FieldValue(fields, "sequenceSlotName"); const auto* camera = FieldValue(fields, "cameraObject");
            if (!a || !b || !slot || !camera || !a->is_string() || !b->is_string() || !slot->is_string()) { fail("共享演出记录不完整"); return; }
            impacts.push_back({ids[0], ids[1], ObjectId(*camera), ids[2] != 0, a->get<std::string>(), b->get<std::string>(), slot->get<std::string>()});
        }
        if (scanIndex < scanQueue.size()) return;
        if (scanHasNext) { scanOffset = scanNext; scanQueue.clear(); return; }
        scanning = false; scanComplete = true;
        if (bindPreviewAfterScan) { bindPreviewAfterScan = false; BindPreview(ctx, draft, error); return; }
        if (pendingEnd < 0) return;
        if (Edges(draft).size() <= 1 || (!confirmedSignature.empty() && confirmedSignature == Signature())) ApplyWrite(ctx, draft, error);
        else { awaitingConfirmation = true; sharedAck = false; confirmedSignature.clear(); }
    }
    void PrepareWrite(EditorWorkspaceHostContext& ctx, const RegionSequenceBinding& draft, size_t end, std::string& error) {
        if (!Load(ctx, draft, error) || !shape) { if (error.empty()) error = why; return; }
        const auto& pose = ends[end].pose;
        if (!pose.valid) { error = pose.reason; return; }
        if (SamePose(pose, Endpoint(document, *shape, end))) {
            // 前回のキー編集が成功して保存だけ失敗した場合、同じ端点でも保存を再試行する。
            if (needsSave) SaveNative(ctx, draft, "参考机位已一致，原生演出已保存", error);
            else { error.clear(); status = "参考机位已与演出一致，无需写入"; }
            return;
        }
        pendingBefore = document; pendingAfter = document;
        auto& transform = pendingAfter.at(Json::json_pointer(shape->transform[end])); auto& lens = pendingAfter.at(Json::json_pointer(shape->lens[end]));
        transform["position"] = pose.position; transform["rotationEulerDeg"] = pose.rotation; lens["verticalFovDegrees"] = pose.fov;
        if (pose.fullLens) { lens["nearClip"] = pose.nearClip; lens["farClip"] = pose.farClip; }
        pendingEnd = static_cast<int>(end); awaitingConfirmation = sharedAck = false; confirmedSignature.clear(); error.clear(); BeginScan();
    }
    void CameraPage(EditorWorkspaceHostContext& ctx, EndUi& e, std::string& error) {
        e.cameras.clear(); e.hasNext = false;
        const auto r = Call(ctx, "scene.objects.query", {{"componentType", "CameraComponent"}, {"detail", true}, {"offset", e.cursor}, {"limit", 32}});
        if (!r.ok || !r.data.is_object() || !r.data.contains("objects") || !r.data["objects"].is_array()) { error = Failure("scene.objects.query", r); e.searching = false; return; }
        const auto& objects = r.data["objects"];
        for (size_t i = 0; i < objects.size(); ++i) {
            const auto& object = objects[i]; const auto name = object.value("name", "相机");
            if (e.search.empty() || name.find(e.search) != std::string::npos || std::to_string(ObjectId(object.value("objectId", Json{}))).find(e.search) != std::string::npos)
                e.cameras.push_back({ObjectId(object.value("objectId", Json{})), name});
            if (e.cameras.size() == 6) { e.next = e.cursor + i + 1; e.hasNext = i + 1 < objects.size() || r.data.contains("nextOffset"); break; }
        }
        if (e.cameras.size() < 6 && r.data.contains("nextOffset")) { e.next = ObjectId(r.data["nextOffset"]); e.hasNext = e.next > e.cursor; }
        e.searching = e.cameras.empty() && e.hasNext;
        if (e.searching) e.cursor = e.next;
    }
    void RequestPreview(EditorWorkspaceHostContext& ctx, const RegionSequenceBinding& draft, double time, std::string& error) {
        if (!shape || !draft.cameraObject || draft.sequenceSlotName != shape->slot) { error = "请先在邻接绑定中选择正确的执行相机与原生槽"; return; }
        const auto r = Call(ctx, "authoring.preview", {{"sessionId", session}, {"time", time}, {"width", 960}, {"height", 540}});
        if (!r.ok && r.code != "PENDING") { error = Failure("authoring.preview", r); previewStatus = "原生预览未通过；检查提示组件或标准绑定冲突"; return; }
        previewPending = true; previewTime = time; previewCamera = draft.cameraObject; polled = Clock::now(); previewStatus = "原生端点采样中…";
    }
    void PollPreview(EditorWorkspaceHostContext& ctx, std::string& error) {
        if (!previewPending || Clock::now() - polled < std::chrono::milliseconds(200)) return;
        polled = Clock::now(); const auto r = Call(ctx, "authoring.capture_status", {{"sessionId", session}});
        if (r.code == "PENDING") return;
        previewPending = false;
        if (!r.ok) { error = Failure("authoring.capture_status", r); previewStatus = "原生预览未通过；请核对重复绑定或相机提示"; return; }
        const auto camera = r.data.value("camera", Json{}); const auto actual = camera.is_object() ? ObjectId(camera.value("objectId", Json{})) : 0;
        if (actual != previewCamera) {
            previewStatus = "原生解析相机 #" + std::to_string(actual) + "，不是执行相机 #" + std::to_string(previewCamera);
            error = "原生预览绑定未一致。保留旧提示；请选择其相机为执行相机，或原生另存演出后配置独立绑定。"; return;
        }
        previewStatus = "采样已核对：执行相机 #" + std::to_string(actual) + " @ " + Num(previewTime) + "s"; error.clear();
        // 捕獲ファイルは診断だけに使う。PNG path は Texture GUID ではなく、CanvasImage に渡さない。
        if (r.data.contains("images") && r.data["images"].is_array() && !r.data["images"].empty()) status = "采样文件：" + r.data["images"][0].value("path", "");
    }
    void BindPreview(EditorWorkspaceHostContext& ctx, const RegionSequenceBinding& draft, std::string& error) {
        if (!shape || !draft.cameraObject || draft.sequenceSlotName != shape->slot) { error = "请先应用正确的执行相机与演出槽"; return; }
        Json camera;
        if (!ReadObject(ctx, draft.cameraObject, camera, error) || !Component(camera, "CameraComponent")) { error = "执行对象必须是 CameraComponent 相机"; return; }
        bool matchingHint = false; uint64_t record = 0, regionComponent = 0;
        for (const auto& i : impacts) {
            if (i.hint && i.slot == draft.sequenceSlotName) {
                if (i.object != draft.cameraObject) { error = "旧相机 #" + std::to_string(i.object) + " 仍提供同一演出的预览提示；请保留它作为执行相机，或在原生另存演出后绑定。未修改旧记录。"; return; }
                matchingHint = true;
            }
            if (!i.hint && ((i.a == draft.regionA && i.b == draft.regionB) || (i.a == draft.regionB && i.b == draft.regionA)) && i.camera == draft.cameraObject && i.slot == draft.sequenceSlotName) {
                if (record) { error = "当前邻接有重复绑定，请先在原生检查"; return; }
                record = i.object; regionComponent = i.component;
            }
        }
        bool standardMatch = false;
        std::optional<PreviewBinding> currentBinding;
        // 明示操作時だけ標準バインディングを確認する。別の記録や異なる槽を勝手に修正しない。
        uint64_t offset = 0;
        for (;;) {
            const auto objects = Call(ctx, "scene.objects.query", {{"componentType", "SequencePlayerComponent"}, {"detail", true}, {"offset", offset}, {"limit", 32}});
            if (!objects.ok || !objects.data.contains("objects") || !objects.data["objects"].is_array()) { error = Failure("scene.objects.query", objects); return; }
            for (const auto& item : objects.data["objects"]) {
                const auto objectId = ObjectId(item.value("objectId", Json{})); const auto componentId = Component(item, "SequencePlayerComponent");
                const auto described = Call(ctx, "component.describe", {{"objectId", objectId}, {"componentId", componentId}, {"propertyIds", {"sequenceAssetGuid", "bindingCount", "playOnStart"}}});
                if (!described.ok) { error = Failure("component.describe", described); return; }
                const auto fields = described.data.value("fields", Json::array()); const auto* guid = FieldValue(fields, "sequenceAssetGuid");
                if (!guid || !guid->is_string()) { error = "无法核对已有标准演出绑定"; return; }
                const bool currentRecord = record && objectId == record;
                if (guid->get<std::string>() != draft.sequenceAssetGuid && !currentRecord) continue;
                const auto* count = FieldValue(fields, "bindingCount"); const auto* autoPlay = FieldValue(fields, "playOnStart");
                if (!count || !count->is_number_integer() || *count < 0 || *count > 32 || !autoPlay || !autoPlay->is_boolean()) { error = "标准演出绑定结构超出快捷处理范围"; return; }
                if (currentRecord && (*count != 1 || autoPlay->get<bool>())) { error = "此连接的标准播放器不是关闭自动播放的单槽绑定，请在原生核对；未覆盖。"; return; }
                Json keys = Json::array();
                for (int i = 0; i < count->get<int>(); ++i) { keys.push_back("bindings." + std::to_string(i) + ".slotName"); keys.push_back("bindings." + std::to_string(i) + ".sceneObjectId"); }
                if (keys.empty()) continue;
                const auto values = Call(ctx, "component.describe", {{"objectId", objectId}, {"componentId", componentId}, {"propertyIds", keys}});
                if (!values.ok) { error = Failure("component.describe", values); return; }
                const auto slots = values.data.value("fields", Json::array());
                if (currentRecord) {
                    const auto* slot = FieldValue(slots, "bindings.0.slotName"); const auto* target = FieldValue(slots, "bindings.0.sceneObjectId");
                    if (!slot || !slot->is_string() || !target) { error = "此连接的标准预览绑定读取不完整"; return; }
                    // 所有権を名前やメモリに推測させない。現在の独立記録だけを明示確認の対象にする。
                    currentBinding = PreviewBinding{record, componentId, regionComponent, ObjectId(*target), guid->get<std::string>(), slot->get<std::string>()};
                    if (currentBinding->asset == draft.sequenceAssetGuid && currentBinding->slot == draft.sequenceSlotName && currentBinding->camera == draft.cameraObject) standardMatch = true;
                    continue;
                }
                for (int i = 0; i < count->get<int>(); ++i) {
                    const std::string prefix = "bindings." + std::to_string(i);
                    const auto* slot = FieldValue(slots, prefix + ".slotName"); const auto* target = FieldValue(slots, prefix + ".sceneObjectId");
                    if (!slot || !slot->is_string() || !target) { error = "标准绑定槽读取不完整"; return; }
                    if (slot->get<std::string>() != draft.sequenceSlotName) continue;
                    if (ObjectId(*target) != draft.cameraObject || autoPlay->get<bool>()) { error = "已有标准绑定指向其他相机或启用了自动播放；请在原生修正，面板未覆盖它。"; return; }
                    standardMatch = true;
                }
            }
            if (!objects.data.contains("nextOffset")) break;
            const auto next = ObjectId(objects.data["nextOffset"]);
            if (next <= offset) { error = "标准绑定查询分页未前进"; return; }
            offset = next;
        }
        const bool updateCurrent = currentBinding && (currentBinding->asset != draft.sequenceAssetGuid || currentBinding->slot != draft.sequenceSlotName || currentBinding->camera != draft.cameraObject);
        if (updateCurrent && confirmedPreviewBinding != currentBinding->Signature()) {
            previewRebind = currentBinding; confirmedPreviewBinding.clear(); error.clear();
            status = "请确认更新当前连接的预览绑定"; return;
        }
        if (updateCurrent || (!matchingHint && !standardMatch)) {
            if (!record) { error = "请先应用当前邻接草稿，再绑定原生预览相机"; return; }
            uint64_t component = currentBinding ? currentBinding->component : 0;
            if (!component) {
                Json object; if (!ReadObject(ctx, record, object, error)) return;
                if (Component(object, "SequencePlayerComponent")) { error = "邻接记录已有其他标准演出组件；请在原生核对，未覆盖。"; return; }
                const auto added = Call(ctx, "component.add", {{"objectId", record}, {"componentType", "SequencePlayerComponent"}});
                if (!added.ok) { error = Failure("component.add", added); return; }
                for (const auto& c : added.data.value("addedComponents", Json::array())) if (c.value("type", "") == "SequencePlayerComponent") component = ObjectId(c.value("componentId", Json{}));
                if (!component) { error = "标准演出组件已添加，但未返回可确认的组件身份"; return; }
            }
            Json commands = Json::array();
            const auto add = [&](std::string_view id, const Json& value) { commands.push_back({{"objectId", record}, {"componentId", component}, {"propertyId", id}, {"value", value}}); };
            // 同じ原生資産のプレビュー解決だけを接続する。起動時再生や別の Runtime 軌道は作らない。
            add("playOnStart", false); add("sequenceAssetGuid", draft.sequenceAssetGuid); add("bindingCount", 1);
            add("bindings.0.slotName", draft.sequenceSlotName); add("bindings.0.sceneObjectId", draft.cameraObject);
            const auto configured = Call(ctx, "authoring.transaction", {{"commands", commands}});
            if (!configured.ok) { error = "标准预览组件未配置完成：" + Failure("authoring.transaction", configured); return; }
            status = "已配置标准预览绑定（不自动播放）；请保存场景。正在采样核对执行相机。";
        }
        previewRebind.reset(); confirmedPreviewBinding.clear();
        RequestPreview(ctx, draft, 0, error);
    }
    void CreateReference(EditorWorkspaceHostContext& ctx, RegionSequenceBinding& draft, size_t end,
                         std::string_view roomName, std::string& error) {
        if (!Load(ctx, draft, error) || !shape) { if (error.empty()) error = why; return; }
        const auto pose = Endpoint(document, *shape, end);
        const auto created = Call(ctx, "scene.create_empty_object", {{"name", std::string(roomName) + (end ? " · 终点参考镜头" : " · 起点参考镜头")}});
        if (!created.ok) { error = Failure("scene.create_empty_object", created); return; }
        const auto id = ObjectId(created.data.value("objectId", Json{}));
        if (!id) { error = "创建参考镜头未返回对象 ID"; return; }
        const auto added = Call(ctx, "component.add", {{"objectId", id}, {"componentType", "CameraComponent"}});
        if (!added.ok) { error = "已创建对象 #" + std::to_string(id) + "，相机组件添加失败：" + added.message; return; }
        uint64_t component = 0;
        for (const auto& c : added.data.value("addedComponents", Json::array())) if (c.value("type", "") == "CameraComponent") component = ObjectId(c.value("componentId", Json{}));
        if (!component) { error = "已创建对象 #" + std::to_string(id) + "，无法确认相机组件 ID"; return; }
        const auto moved = Call(ctx, "scene.set_transform", {{"objectId", id}, {"position", pose.position}, {"rotation", pose.rotation}});
        if (!moved.ok) { error = Failure("scene.set_transform", moved); return; }
        Json commands = Json::array();
        const auto field = [&](std::string_view key, const Json& value) { commands.push_back({{"objectId", id}, {"componentId", component}, {"propertyId", key}, {"value", value}}); };
        field("orthographic", false); field("verticalFovDegrees", pose.fov); field("nearClip", pose.nearClip); field("farClip", pose.farClip);
        const auto configured = Call(ctx, "authoring.transaction", {{"commands", commands}});
        if (!configured.ok) { error = Failure("authoring.transaction", configured); return; }
        // 明示ボタン以外ではオブジェクトを増やさない。参照だけを Scene 草稿へ渡し、演出はまだ書き換えない。
        (end ? draft.targetCameraObject : draft.sourceCameraObject) = id; ends[end].useView = false; changed = true;
        status = "已创建参考镜头；请应用邻接草稿并保存场景"; Load(ctx, draft, error);
    }
    void DrawEnd(EditorWorkspaceHostContext& ctx, RegionSequenceBinding& draft, size_t end,
                 std::string_view roomName, std::string& error) {
        auto& ui = *ctx.ui; auto& e = ends[end]; const std::string key = std::string(kPrefix) + (end ? "target." : "source.");
        uint64_t& reference = end ? draft.targetCameraObject : draft.sourceCameraObject;
        ui.Text(std::string(end ? "终点 · " : "起点 · ") + std::string(roomName));
        const bool same = shape && SamePose(e.pose, Endpoint(document, *shape, end));
        ui.Text((e.useView ? "编辑视角" : e.pose.label.empty() ? "未选择参考镜头" : e.pose.label) + std::string(same ? " · 已与演出一致" : " · 未同步"));
        if (ui.Button(key + "choose", e.picker ? "收起选择" : "选择参考镜头")) {
            e.picker = !e.picker;
            if (e.picker) { e.cursor = 0; e.history.clear(); e.searching = true; CameraPage(ctx, e, error); }
        }
        ui.SameLine(); if (ui.Button(key + "write", end ? "写入终点" : "写入起点")) { PrepareWrite(ctx, draft, end, error); return; }
        if (reference && ui.Button(key + "pip", "在 3D 查看参考镜头")) {
            const auto selected = Call(ctx, "scene.object.select", {{"objectId", reference}});
            if (!selected.ok) error = Failure("scene.object.select", selected);
            else status = "已选择参考相机；在原生 3D 视口查看画中画（视口至少 480×300）";
        }
        if (!e.picker) return;
        if (ui.Checkbox(key + "editor_view", "使用当前普通编辑视角（透视、无 roll）", &e.useView)) Load(ctx, draft, error);
        if (e.useView) ui.Text("只读取位置、方向与 FOV；近远裁面保持演出原值。");
        if (EditText(ui, key + "search", "名称 / ID", e.search)) { e.cursor = 0; e.history.clear(); e.searching = true; }
        if (e.searching) CameraPage(ctx, e, error);
        for (const auto& camera : e.cameras) {
            if (ui.Selectable(key + "camera." + std::to_string(camera.objectId), camera.name + " #" + std::to_string(camera.objectId), reference == camera.objectId && !e.useView)) {
                reference = camera.objectId; e.useView = false; changed = true; e.picker = false; Load(ctx, draft, error); break;
            }
        }
        if (e.searching) ui.Text("继续搜索…");
        else if (e.cameras.empty()) ui.Text("未找到匹配的相机");
        if (!e.history.empty() && ui.Button(key + "previous", "上一页")) { e.cursor = e.history.back(); e.history.pop_back(); CameraPage(ctx, e, error); }
        if (e.hasNext && ui.Button(key + "next", "下一页")) { e.history.push_back(e.cursor); e.cursor = e.next; CameraPage(ctx, e, error); }
        if (reference && ui.Button(key + "clear", "清除制作引用")) { reference = 0; changed = true; Load(ctx, draft, error); }
        if (ui.Button(key + "create", end ? "新建终点参考镜头" : "新建起点参考镜头")) CreateReference(ctx, draft, end, roomName, error);
        if (!e.pose.valid && !e.pose.reason.empty()) ui.Text(e.pose.reason);
    }
    void DrawShared(IEditorWorkspaceUi& ui, const RegionSequenceBinding& draft) {
        const auto edges = Edges(draft); ui.Text("共享资产影响 " + std::to_string(edges.size()) + " 条绑定（含已不相邻记录）：");
        impactPage = std::clamp(impactPage, 0, static_cast<int>((edges.size() - 1) / 4));
        for (size_t i = static_cast<size_t>(impactPage) * 4; i < (std::min)(edges.size(), static_cast<size_t>(impactPage + 1) * 4); ++i) ui.Text(edges[i]);
        if (impactPage && ui.Button("td2.sequence_camera.impact.previous", "上一页影响")) --impactPage;
        if (static_cast<size_t>(impactPage + 1) * 4 < edges.size() && ui.Button("td2.sequence_camera.impact.next", "下一页影响")) ++impactPage;
    }
};

SequenceCameraPanel::SequenceCameraPanel() : state_(std::make_unique<State>()) {}
SequenceCameraPanel::~SequenceCameraPanel() = default;
bool SequenceCameraPanel::ConsumeDraftChanged() noexcept { const bool value = state_->changed; state_->changed = false; return value; }
void SequenceCameraPanel::Reset() { state_ = std::make_unique<State>(); }

void SequenceCameraPanel::Draw(EditorWorkspaceHostContext& ctx, RegionSequenceBinding& draft,
                               std::string_view fromName, std::string_view toName, std::string& error) {
    if (!ctx.ui || !ctx.commands) return;
    try {
        auto& ui = *ctx.ui;
        const std::string identity = draft.regionA + "\n" + draft.regionB + "\n" + draft.sequenceAssetGuid + "\n" + draft.sequenceSlotName + "\n" + std::to_string(draft.cameraObject);
        if (state_->identity != identity) {
            if (state_->previewPending) (void)Call(ctx, "authoring.cancel_preview", {{"sessionId", state_->session}});
            Reset(); state_->identity = identity;
        }
        auto& s = *state_;
        if (draft.sequenceAssetGuid.empty()) { ui.Text("选择原生演出后配置首末镜头；未绑定时保持当前镜头。"); return; }
        if (s.session.empty()) { if (!s.Load(ctx, draft, error)) return; s.BeginScan(); }
        else if (!s.scanning && s.pendingEnd < 0 && !s.previewPending &&
                 (Clock::now() - s.refreshed > std::chrono::seconds(1) || s.ends[0].readId != draft.sourceCameraObject || s.ends[1].readId != draft.targetCameraObject))
            s.Load(ctx, draft, error);
        s.AdvanceScan(ctx, draft, error); s.PollPreview(ctx, error);
        ui.Text(std::string(fromName) + " → " + (s.shape ? s.shape->name : s.document.value("name", "原生演出")) + " → " + std::string(toName));
        if (s.shape) ui.Text(Num(s.shape->duration) + " 秒 · 正向 / 返回反向");
        if (s.pendingEnd >= 0) {
            if (s.scanning) ui.Text("正在核对共享影响…");
            if (s.awaitingConfirmation) {
                s.DrawShared(ui, draft);
                ui.Text("会修改这些绑定共用的原生资产；需要独立效果时请先在原生编辑器另存。");
                ui.Checkbox("td2.sequence_camera.shared_confirm", "确认同时更新以上绑定的演出端点", &s.sharedAck);
                if (s.sharedAck && ui.Button("td2.sequence_camera.shared_write", s.pendingEnd ? "确认写入终点" : "确认写入起点")) {
                    s.confirmedSignature = s.Signature(); s.awaitingConfirmation = false; s.BeginScan();
                }
            }
            if (ui.Button("td2.sequence_camera.write_cancel", "取消本次写入")) { s.pendingEnd = -1; s.awaitingConfirmation = false; s.scanning = false; s.sharedAck = false; }
            return;
        }
        if (s.shape) {
            s.DrawEnd(ctx, draft, 0, fromName, error);
            if (s.pendingEnd >= 0) return;
            ui.Separator(); s.DrawEnd(ctx, draft, 1, toName, error);
            if (s.pendingEnd >= 0) return;
            ui.Text("写入会保存当前原生演出草稿。");
        } else ui.Text(s.why);
        if (ui.Button("td2.sequence_camera.open_native", "打开原生演出")) {
            const auto r = Call(ctx, "authoring.reveal", {{"sessionId", s.session}});
            if (!r.ok) error = Failure("authoring.reveal", r);
            else s.status = "原生演出已打开：点 Shot 看首帧，时间框填终点秒数，再用播放三角播放整段。";
        }
        if (s.needsSave && ui.Button("td2.sequence_camera.save_retry", "重试保存原生草稿")) s.SaveNative(ctx, draft, "原生演出已保存", error);
        if (ui.Button("td2.sequence_camera.details", s.details ? "收起机位与预览详情" : "机位与预览详情")) s.details = !s.details;
        if (s.details) {
            if (s.shape) for (size_t end = 0; end < 2; ++end) {
                const auto p = Endpoint(s.document, *s.shape, end);
                ui.Text(std::string(end ? "终点 @ " : "起点 @ ") + Num(end ? s.shape->duration : 0) + "s  位置 " + VectorLabel(p.position));
                ui.Text("角度 " + VectorLabel(p.rotation) + "  FOV " + Num(p.fov) + "  裁面 " + Num(p.nearClip) + " / " + Num(p.farClip));
            }
            ui.Text("执行相机 #" + std::to_string(draft.cameraObject) + " · 槽 " + draft.sequenceSlotName + "（与制作参考镜头分开）");
            if (s.scanComplete) {
                if (s.Edges(draft).size() > 1) s.DrawShared(ui, draft);
                bool matchingHint = false, conflict = false;
                for (const auto& impact : s.impacts) if (impact.hint && impact.slot == draft.sequenceSlotName) {
                    matchingHint |= impact.object == draft.cameraObject; conflict |= impact.object != draft.cameraObject;
                    if (impact.object != draft.cameraObject) ui.Text("旧相机 #" + std::to_string(impact.object) + " 的 " + impact.a + " ↔ " + impact.b + " 记录仍提供此资产的原生预览提示。");
                }
                if (conflict) ui.Text("请选择提示相机作为执行相机，或原生另存资产后配置独立绑定；本面板不改旧记录。");
                else if (!matchingHint) ui.Text("独立记录需要标准预览绑定；点击“绑定预览相机”配置，不自动播放。成功仍以采样相机核对为准。");
            }
            ui.Text(s.previewStatus);
            if (s.previewRebind) {
                const auto& old = *s.previewRebind;
                ui.Text("仅更新当前独立连接记录 #" + std::to_string(old.object));
                ui.Text("资产 " + old.asset + " → " + draft.sequenceAssetGuid);
                ui.Text("槽 " + old.slot + " → " + draft.sequenceSlotName + " · 相机 #" + std::to_string(old.camera) + " → #" + std::to_string(draft.cameraObject));
                if (ui.Button("td2.sequence_camera.update_preview", "更新此连接的预览绑定")) {
                    // 確認後も共有・標準バインディングを再読込し、変更されていれば再確認する。
                    s.confirmedPreviewBinding = old.Signature(); s.previewRebind.reset(); s.bindPreviewAfterScan = true; s.BeginScan();
                }
                if (ui.Button("td2.sequence_camera.update_preview_cancel", "取消更新预览绑定")) { s.previewRebind.reset(); s.confirmedPreviewBinding.clear(); }
            } else if (s.shape && !s.previewPending && !s.scanning) {
                if (ui.Button("td2.sequence_camera.bind_preview", "绑定预览相机（不自动播放）")) { s.confirmedPreviewBinding.clear(); s.bindPreviewAfterScan = true; s.BeginScan(); }
                if (ui.Button("td2.sequence_camera.sample_start", "采样首帧并核对相机")) s.RequestPreview(ctx, draft, 0, error);
                if (ui.Button("td2.sequence_camera.sample_end", "采样末帧并核对相机")) s.RequestPreview(ctx, draft, s.shape->duration, error);
            }
            ui.Text("采样是静态核对。真正播放请打开原生演出，选 Shot / 时间，再点播放三角。");
            if (!s.status.empty()) ui.Text(s.status);
        }
        if (ui.Button("td2.sequence_camera.refresh", "刷新机位")) { s.Load(ctx, draft, error); s.BeginScan(); }
    } catch (const std::exception& exception) {
        error = "镜头面板返回数据不符合公开结构：" + std::string(exception.what());
        state_->pendingEnd = -1; state_->scanning = false; state_->previewPending = false;
    }
}
} // namespace TD2::Editor
