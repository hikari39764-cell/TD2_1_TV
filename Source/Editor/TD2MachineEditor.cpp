#include "TD2MachineEditor.h"
#include "TD2DeviceEditor.h"
#include "../Common/TD2Math.h"
#include <algorithm>
#include <cmath>
#include <sstream>

namespace TD2::Editor {
namespace {
using namespace MachineText;
using Math::kPi;

// ---- 部品の欄を読む
std::string Text(const Json& fields, std::string_view id) { const auto* v = FieldValue(fields, id); return v && v->is_string() ? v->get<std::string>() : std::string{}; }
double Real(const Json& fields, std::string_view id, double fallback) { const auto* v = FieldValue(fields, id); return v && v->is_number() ? v->get<double>() : fallback; }
bool Flag(const Json& fields, std::string_view id) { const auto* v = FieldValue(fields, id); return v && v->is_boolean() && v->get<bool>(); }

struct KindInfo { MachineKind kind; std::string_view type, idField; std::vector<std::string> fields; };
const std::vector<KindInfo>& Kinds() {
    static const std::vector<KindInfo> kinds{
        {MachineKind::Mechanism, "TD2.Mechanism", "mechanismId", {"mechanismId", "toggle", "balance", "seconds", "dwell", "settle", "cameraLift", "cameraPull",
            "regionOffsets", "objectDeltas", "regionOffsetsC", "objectDeltasC", "recipe", "parts"}},
        {MachineKind::Slide, "TD2.SlideRack", "rackId", {"rackId", "seconds", "members", "emptySlots"}},
        {MachineKind::Stack, "TD2.StackColumn", "stackId", {"stackId", "pullSeconds", "dropSeconds", "resetSeconds", "members", "dockDistances"}},
        {MachineKind::Carousel, "TD2.Carousel", "carouselId", {"carouselId", "seconds", "faces", "objects"}},
    };
    return kinds;
}
const KindInfo& InfoOf(MachineKind kind) { return *std::find_if(Kinds().begin(), Kinds().end(), [kind](const auto& k) { return k.kind == kind; }); }

// ---- 姿勢の四元数（x, y, z, w）。度とエンジンの順 M = Rz·Ry·Rx。
using Q = Math::Quat;
using V = Math::Vec3;
using Math::QConj;
using Math::QMul;
using Math::QRotate;
// 作者が読みやすい角度へ戻す（同じ回転の二通りのうち X と Z が小さい方、小数 3 桁）。
V ToEuler(const Q& q) {
    const double x = q[0], y = q[1], z = q[2], w = q[3];
    const double r00 = 1 - 2 * (y * y + z * z), r10 = 2 * (x * y + w * z), r20 = 2 * (x * z - w * y);
    const double r21 = 2 * (y * z + w * x), r22 = 1 - 2 * (x * x + y * y);
    const double b = std::asin(std::clamp(-r20, -1.0, 1.0));
    double a, c;
    if (std::abs(r20) < .9999) { a = std::atan2(r21, r22); c = std::atan2(r10, r00); }
    else { a = 0; c = std::atan2(-2 * (x * y - w * z), 1 - 2 * (x * x + z * z)); }
    V e{a * 180 / kPi, b * 180 / kPi, c * 180 / kPi};
    // 同じ回転の二通りの書き方のうち、X と Z が小さい方。
    const V alt{e[0] > 0 ? e[0] - 180 : e[0] + 180, e[1] >= 0 ? 180 - e[1] : -180 - e[1], e[2] > 0 ? e[2] - 180 : e[2] + 180};
    if (std::abs(alt[0]) + std::abs(alt[2]) < std::abs(e[0]) + std::abs(e[2]) - 1e-6) { e = alt; }
    for (auto& v : e) { v = std::round(v * 1000) / 1000; }
    return e;
}
}

// 部品の欄：「物体ID 区域ID 役目 m|s|b」を「;」で区切る（m はレシピが置いて一緒に動く、s はレシピが置いて動かない、b は元からある物を一緒に動かす）。
std::string FormatParts(const std::vector<Machine::Part>& parts) {
    std::string text;
    for (const auto& p : parts) {
        if (!text.empty()) { text += "; "; }
        text += std::to_string(p.object) + " " + p.region + " " + (p.role.empty() ? "part" : p.role) + (p.owned ? (p.moves ? " m" : " s") : " b");
    }
    return text;
}

std::vector<Machine::Part> ParseParts(std::string_view text) {
    std::vector<Machine::Part> parts;
    std::string line;
    const auto flush = [&] {
        std::istringstream in(line); Machine::Part p; std::string mode;
        if (in >> p.object >> p.region >> p.role >> mode && p.object) { p.moves = mode != "s"; p.owned = mode != "b"; parts.push_back(p); }
        line.clear();
    };
    for (const char c : text) { if (c == ';' || c == '\n') { flush(); } else { line += c; } }
    flush();
    return parts;
}

std::string Machine::KindLabel() const {
    switch (kind) {
    case MachineKind::Mechanism: return "仕掛け";
    case MachineKind::Slide: return "スライド棚";
    case MachineKind::Stack: return "テレビの塔";
    case MachineKind::Carousel: return "回転柱";
    }
    return {};
}

std::vector<std::pair<std::string, std::string>> Machine::Receivers() const {
    std::vector<std::pair<std::string, std::string>> out;
    switch (kind) {
    case MachineKind::Mechanism:
        out.push_back({"mech/" + id, KindLabel() + " " + id});
        if (balance) { out.push_back({"mech/" + id + "/r", KindLabel() + " " + id + "（逆側）"}); }
        break;
    case MachineKind::Slide:
        for (const auto& m : members) { out.push_back({"slide/" + m, "棚 " + id + "：" + m + " を滑らす"}); }
        break;
    case MachineKind::Stack:
        out.push_back({"stack/" + id + "/pull", "テレビの塔 " + id + "：一番下を抜く"});
        out.push_back({"stack/" + id + "/reset", "テレビの塔 " + id + "：戻す"});
        break;
    case MachineKind::Carousel:
        out.push_back({"carousel/" + id + "/cw", "回転柱 " + id + "：時計回り"});
        out.push_back({"carousel/" + id + "/ccw", "回転柱 " + id + "：反時計回り"});
        break;
    }
    return out;
}

std::vector<std::string> Machine::Regions() const {
    std::vector<std::string> out = members;
    for (const auto* list : {&offsets, &offsetsC}) for (const auto& o : *list) {
        if (std::find(out.begin(), out.end(), o.regionId) == out.end()) { out.push_back(o.regionId); }
    }
    return out;
}

bool LoadMachines(EditorWorkspaceHostContext& ctx, std::vector<Machine>& machines, std::string& error) {
    machines.clear();
    for (const auto& info : Kinds()) {
        std::vector<SceneItem> objects;
        if (!QueryObjects(ctx, info.type, objects, error)) { return false; }
        for (const auto& object : objects) {
            const auto instances = Execute(ctx, "component.instances.query", {{"objectId", object.objectId}});
            if (!instances.ok) { error = instances.error; return false; }
            for (const auto& component : instances.data.value("components", Json::array())) {
                if (component.value("type", "") != info.type) { continue; }
                const auto componentId = ObjectId(component.value("componentId", Json{}));
                const auto described = Execute(ctx, "component.describe", {{"objectId", object.objectId}, {"componentId", componentId}, {"propertyIds", info.fields}});
                if (!described.ok) { error = described.error; return false; }
                const auto fields = described.data.value("fields", Json::array());
                Machine m; m.kind = info.kind; m.objectId = object.objectId; m.componentId = componentId; m.objectName = object.name;
                std::string ignored;
                m.id = Text(fields, info.idField);
                switch (info.kind) {
                case MachineKind::Mechanism:
                    m.toggle = Flag(fields, "toggle"); m.balance = Flag(fields, "balance");
                    m.seconds = Real(fields, "seconds", .8); m.dwell = Real(fields, "dwell", 0); m.settle = Real(fields, "settle", 0);
                    m.cameraLift = Real(fields, "cameraLift", 1.5); m.cameraPull = Real(fields, "cameraPull", 1);
                    // 読めない行は飛ばし、読めた分だけを編集に使う。
                    (void)ParseOffsets(Text(fields, "regionOffsets"), m.offsets, ignored); (void)ParseOffsets(Text(fields, "regionOffsetsC"), m.offsetsC, ignored);
                    (void)ParseDeltas(Text(fields, "objectDeltas"), m.deltas, ignored); (void)ParseDeltas(Text(fields, "objectDeltasC"), m.deltasC, ignored);
                    m.recipe = Text(fields, "recipe"); m.parts = ParseParts(Text(fields, "parts"));
                    break;
                case MachineKind::Slide:
                    m.seconds = Real(fields, "seconds", .9); m.members = ParseNames(Text(fields, "members"));
                    (void)ParseSlots(Text(fields, "emptySlots"), m.emptySlots, ignored);
                    break;
                case MachineKind::Stack:
                    m.pullSeconds = Real(fields, "pullSeconds", .8); m.dropSeconds = Real(fields, "dropSeconds", .35); m.resetSeconds = Real(fields, "resetSeconds", 2);
                    m.members = ParseNames(Text(fields, "members")); m.dockDistances = ParseNumbers(Text(fields, "dockDistances"));
                    break;
                case MachineKind::Carousel:
                    m.seconds = Real(fields, "seconds", 1.6); m.members = ParseNames(Text(fields, "faces")); m.extraObjects = Text(fields, "objects");
                    break;
                }
                machines.push_back(std::move(m));
            }
        }
    }
    return true;
}

bool WriteMachine(EditorWorkspaceHostContext& ctx, const Machine& before, const Machine& after, std::string& error) {
    Json commands = Json::array();
    const auto set = [&](std::string_view id, const Json& oldValue, const Json& newValue) {
        if (oldValue != newValue) { commands.push_back(PropertyCommand(after.objectId, after.componentId, id, newValue)); }
    };
    set(InfoOf(after.kind).idField, before.id, after.id);
    switch (after.kind) {
    case MachineKind::Mechanism:
        set("toggle", before.toggle, after.toggle); set("balance", before.balance, after.balance);
        set("seconds", before.seconds, after.seconds); set("dwell", before.dwell, after.dwell); set("settle", before.settle, after.settle);
        set("cameraLift", before.cameraLift, after.cameraLift); set("cameraPull", before.cameraPull, after.cameraPull);
        set("regionOffsets", FormatOffsets(before.offsets), FormatOffsets(after.offsets));
        set("regionOffsetsC", FormatOffsets(before.offsetsC), FormatOffsets(after.offsetsC));
        set("objectDeltas", FormatDeltas(before.deltas), FormatDeltas(after.deltas));
        set("objectDeltasC", FormatDeltas(before.deltasC), FormatDeltas(after.deltasC));
        set("recipe", before.recipe, after.recipe);
        set("parts", FormatParts(before.parts), FormatParts(after.parts));
        break;
    case MachineKind::Slide:
        set("seconds", before.seconds, after.seconds); set("members", JoinNames(before.members), JoinNames(after.members));
        set("emptySlots", FormatSlots(before.emptySlots), FormatSlots(after.emptySlots));
        break;
    case MachineKind::Stack:
        set("pullSeconds", before.pullSeconds, after.pullSeconds); set("dropSeconds", before.dropSeconds, after.dropSeconds);
        set("resetSeconds", before.resetSeconds, after.resetSeconds); set("members", JoinNames(before.members), JoinNames(after.members));
        set("dockDistances", FormatNumbers(before.dockDistances), FormatNumbers(after.dockDistances));
        break;
    case MachineKind::Carousel:
        set("seconds", before.seconds, after.seconds); set("faces", JoinNames(before.members), JoinNames(after.members));
        set("objects", before.extraObjects, after.extraObjects);
        break;
    }
    return Transact(ctx, commands, error);
}

bool CreateMachine(EditorWorkspaceHostContext& ctx, MachineKind kind, uint64_t host, const std::string& id, std::string& error) {
    const auto& info = InfoOf(kind);
    if (!host) {
        Machine label; label.kind = kind;
        const auto created = Execute(ctx, "scene.create_empty_object", {{"name", label.KindLabel() + " " + id}});
        if (!created.ok) { error = created.error; return false; }
        host = ObjectId(created.data.value("objectId", Json{}));
        if (!host) { error = "仕掛けの物体を作れません"; return false; }
    }
    const auto added = Execute(ctx, "component.add", {{"objectId", host}, {"componentType", info.type}});
    if (!added.ok) { error = added.error; return false; }
    uint64_t componentId = 0;
    for (const auto& c : added.data.value("addedComponents", Json::array())) { if (c.value("type", "") == info.type) { componentId = ObjectId(c.value("componentId", Json{})); } }
    if (!componentId) { error = "仕掛けの部品を作れません"; return false; }
    return Transact(ctx, Json::array({PropertyCommand(host, componentId, info.idField, id)}), error);
}

bool RemoveMachine(EditorWorkspaceHostContext& ctx, const Machine& machine, std::string& error) {
    const auto removed = Execute(ctx, "component.remove", {{"objectId", machine.objectId}, {"componentId", machine.componentId}});
    if (!removed.ok) { error = removed.error; return false; }
    return true;
}

std::vector<std::string> PlatesFor(const Machine& machine, const MachineCheckInput& input) {
    std::vector<std::string> plates;
    const auto receivers = machine.Receivers();
    for (const auto& [plate, targets] : input.plateTargets) for (const auto& target : targets) {
        if (std::any_of(receivers.begin(), receivers.end(), [&](const auto& r) { return r.first == target; })) { plates.push_back(plate); break; }
    }
    return plates;
}

std::vector<std::string> CheckMachine(const Machine& m, const std::vector<Machine>& all, const MachineCheckInput& input) {
    std::vector<std::string> problems;
    const auto placementOf = [&](const std::string& id) -> const RegionPlacement* {
        const auto it = std::find_if(input.placements.begin(), input.placements.end(), [&](const RegionPlacement& p) { return p.regionId == id; });
        return it == input.placements.end() ? nullptr : &*it;
    };
    const auto placed = [&](const std::string& id) { const auto* p = placementOf(id); return p && p->worldPlaced; };
    if (!IsStableId(m.id)) { problems.push_back("ID は英数字・点・ハイフン・下線だけ"); }
    if (std::count_if(all.begin(), all.end(), [&](const Machine& o) { return o.kind == m.kind && o.id == m.id; }) > 1) { problems.push_back("同じ種類の仕掛けで ID が重なっています：" + m.id); }
    switch (m.kind) {
    case MachineKind::Mechanism:
        if (m.offsets.empty() && m.deltas.empty()) { problems.push_back("状態 B が空です：テレビをクリックして区域のずれを加えるか、物体を加えて状態 B を作ってください"); }
        for (const auto* list : {&m.offsets, &m.offsetsC}) for (const auto& o : *list) {
            if (!placed(o.regionId)) { problems.push_back("区域が世界にありません：" + o.regionId); }
            if (!o.anchor.empty() && !placed(o.anchor)) { problems.push_back("基準の区域が世界にありません：" + o.anchor); }
        }
        if (m.balance && m.offsetsC.empty() && m.deltasC.empty()) { problems.push_back("天秤の逆側（状態 C）が空です"); }
        for (const auto* list : {&m.deltas, &m.deltasC}) for (const auto& d : *list) {
            if (std::find(input.sceneObjects.begin(), input.sceneObjects.end(), d.object) == input.sceneObjects.end()) { problems.push_back("物体がありません：" + std::to_string(d.object)); }
            if (d.anchor && std::find(input.sceneObjects.begin(), input.sceneObjects.end(), d.anchor) == input.sceneObjects.end()) { problems.push_back("基準の物体がありません：" + std::to_string(d.anchor)); }
        }
        break;
    case MachineKind::Slide: {
        if (m.members.size() < 1) { problems.push_back("メンバーがいません：テレビをクリックして加える"); break; }
        for (const auto& id : m.members) { if (!placed(id)) { problems.push_back("メンバーが世界にありません：" + id); } }
        if (m.emptySlots.empty()) { problems.push_back("空きスロットが要ります：地図の空いたマスをクリック"); }
        // 空きスロットの 3D の置き場は、メンバーの並び（同じ行・同じ列）から求める。求められない向きがあれば知らせる。
        const auto* o = placementOf(m.members.front());
        bool hasU = false, hasV = false;
        for (const auto& id : m.members) {
            const auto* p = placementOf(id); if (!o || !p) { continue; }
            hasU |= p->worldX != o->worldX && p->worldY == o->worldY; hasV |= p->worldY != o->worldY && p->worldX == o->worldX;
        }
        for (const auto& id : m.members) {
            const auto* p = placementOf(id); if (!o || !p) { continue; }
            if (p->worldX != o->worldX && p->worldY != o->worldY) { if (hasU) { hasV = true; } else if (hasV) { hasU = true; } }
        }
        for (const auto& [ex, ey] : m.emptySlots) {
            if ((ex && !hasU) || (ey && !hasV)) { problems.push_back("空きスロットの 3D 位置を求められません：メンバーを同じ行か列に並べてください"); break; }
        }
        break;
    }
    case MachineKind::Stack:
        if (m.members.size() < 2) { problems.push_back("テレビの塔には二台以上のテレビが要ります（下から）"); }
        for (const auto& id : m.members) { if (!placed(id)) { problems.push_back("メンバーが世界にありません：" + id); } }
        if (m.dockDistances.empty()) { problems.push_back("抜く距離が要ります"); }
        if (m.dockDistances.size() + 1 < m.members.size()) { problems.push_back("抜く距離が段数より少ないので、多い分は揺れるだけです"); }
        break;
    case MachineKind::Carousel:
        if (m.members.size() < 2) { problems.push_back("回転柱には二つ以上の面が要ります"); }
        for (const auto& id : m.members) { if (!placementOf(id)) { problems.push_back("面がテレビの地図ではありません：" + id); } }
        if (const auto has = input.objectHasModel.find(m.objectId); has != input.objectHasModel.end() && !has->second) {
            problems.push_back("部品の付いた物体にモデルがありません：回転柱の部品は柱に付けてください（柱の位置が回転軸）");
        }
        break;
    }
    if (PlatesFor(m, input).empty() && m.dwell <= 0) { problems.push_back("圧力板がまだありません（「配線」で圧力板からアイコンへドラッグ）"); }
    return problems;
}

Pose ApplyDelta(const Pose& a, const Pose& base, const MachineObjectDelta& d) {
    const auto qa = Math::QFromEulerDeg(a.rotation), qbase = Math::QFromEulerDeg(base.rotation);
    const auto qb = QMul(qbase, Math::QFromEulerDeg(d.rotation));
    const auto offset = QRotate(qbase, d.position);
    Pose b = a;
    b.position = {base.position[0] + offset[0], base.position[1] + offset[1], base.position[2] + offset[2]};
    if (d.hasPivot) {
        const auto c = QRotate(qbase, d.pivot);
        const V center{base.position[0] + c[0], base.position[1] + c[1], base.position[2] + c[2]};
        const auto turned = QRotate(QMul(qb, QConj(qa)), {a.position[0] - center[0], a.position[1] - center[1], a.position[2] - center[2]});
        b.position = {center[0] + turned[0], center[1] + turned[1], center[2] + turned[2]};
    }
    b.rotation = ToEuler(qb);
    return b;
}

MachineObjectDelta MakeDelta(const Pose&, const Pose& base, const Pose& b, const MachineObjectDelta& previous) {
    auto d = previous;
    const auto inverse = QConj(Math::QFromEulerDeg(base.rotation));
    const auto p = QRotate(inverse, {b.position[0] - base.position[0], b.position[1] - base.position[1], b.position[2] - base.position[2]});
    for (size_t i = 0; i < 3; ++i) { d.position[i] = std::round(p[i] * 10000) / 10000; }
    d.rotation = ToEuler(QMul(inverse, Math::QFromEulerDeg(b.rotation)));
    return d;
}
}
