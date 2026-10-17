#include "TD2MachineEditor.h"
#include "TD2DeviceEditor.h"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <locale>
#include <set>
#include <sstream>

namespace TD2::Editor {
namespace {
constexpr double kPi = 3.14159265358979323846;

// ---- 文字列の欄（実行時の部品と同じ書式）
std::string Lines(std::string text) { std::replace(text.begin(), text.end(), ';', '\n'); return text; }
std::string Number(double v) {
    if (std::abs(v) < 5e-5) { return "0"; }
    std::ostringstream out; out.imbue(std::locale::classic()); out << std::fixed << std::setprecision(4) << v;
    auto text = out.str();
    while (!text.empty() && text.back() == '0') { text.pop_back(); }
    if (!text.empty() && text.back() == '.') { text.pop_back(); }
    return text;
}
std::vector<std::string> Names(std::string text) {
    std::replace(text.begin(), text.end(), ',', '\n');
    std::vector<std::string> names; std::istringstream lines(text); std::string line;
    while (std::getline(lines, line)) { std::istringstream value(line); std::string id; if (value >> id) { names.push_back(id); } }
    return names;
}
std::string JoinNames(const std::vector<std::string>& names) {
    std::string out;
    for (const auto& n : names) { if (!out.empty()) { out += ","; } out += n; }
    return out;
}
std::vector<MachineRegionOffset> ParseOffsets(const std::string& text) {
    std::vector<MachineRegionOffset> out;
    std::istringstream lines(Lines(text)); std::string line;
    while (std::getline(lines, line)) {
        std::istringstream values(line); values.imbue(std::locale::classic());
        MachineRegionOffset o; std::string first;
        if (!(values >> o.regionId >> first)) { continue; }
        if (first == "park") { o.park = true; out.push_back(o); continue; }
        try { o.dx = std::stoi(first); } catch (...) { continue; }
        values >> o.dy;
        for (std::string option; values >> option;) {
            if (option == "flip") { o.flip = true; } else if (option.size() > 1 && option[0] == '@') { o.anchor = option.substr(1); }
        }
        out.push_back(o);
    }
    return out;
}
std::string FormatOffsets(const std::vector<MachineRegionOffset>& offsets) {
    std::string out;
    for (const auto& o : offsets) {
        if (!out.empty()) { out += "; "; }
        if (o.park) { out += o.regionId + " park"; continue; }
        out += o.regionId + " " + std::to_string(o.dx) + " " + std::to_string(o.dy);
        if (o.flip) { out += " flip"; }
        if (!o.anchor.empty()) { out += " @" + o.anchor; }
    }
    return out;
}
std::vector<MachineObjectDelta> ParseDeltas(const std::string& text) {
    std::vector<MachineObjectDelta> out;
    std::istringstream lines(Lines(text)); std::string line;
    while (std::getline(lines, line)) {
        std::istringstream values(line); values.imbue(std::locale::classic());
        MachineObjectDelta d;
        if (!(values >> d.object >> d.position[0] >> d.position[1] >> d.position[2] >> d.rotation[0] >> d.rotation[1] >> d.rotation[2])) { continue; }
        std::vector<std::string> rest; for (std::string token; values >> token;) { rest.push_back(token); }
        size_t i = 0;
        if (rest.size() >= 3 && rest[0][0] != '@') {
            try { for (int k = 0; k < 3; ++k) { d.pivot[static_cast<size_t>(k)] = std::stod(rest[static_cast<size_t>(k)]); } d.hasPivot = true; i = 3; } catch (...) {}
        }
        if (i < rest.size() && rest[i].size() > 1 && rest[i][0] == '@') { try { d.anchor = std::stoull(rest[i].substr(1)); } catch (...) {} }
        out.push_back(d);
    }
    return out;
}
std::string FormatDeltas(const std::vector<MachineObjectDelta>& deltas) {
    std::string out;
    for (const auto& d : deltas) {
        if (!out.empty()) { out += "; "; }
        out += std::to_string(d.object);
        for (const double v : d.position) { out += " " + Number(v); }
        for (const double v : d.rotation) { out += " " + Number(v); }
        if (d.hasPivot) { for (const double v : d.pivot) { out += " " + Number(v); } }
        if (d.anchor) { out += " @" + std::to_string(d.anchor); }
    }
    return out;
}
std::string FormatSlots(const std::vector<std::pair<int, int>>& slots) {
    std::string out;
    for (const auto& [x, y] : slots) { if (!out.empty()) { out += "; "; } out += std::to_string(x) + " " + std::to_string(y); }
    return out;
}
std::vector<std::pair<int, int>> ParseSlots(const std::string& text) {
    std::vector<std::pair<int, int>> out; std::istringstream lines(Lines(text)); std::string line;
    while (std::getline(lines, line)) { std::istringstream v(line); std::pair<int, int> p; if (v >> p.first >> p.second) { out.push_back(p); } }
    return out;
}
std::string FormatNumbers(const std::vector<double>& numbers) {
    std::string out;
    for (const double v : numbers) { if (!out.empty()) { out += " "; } out += Number(v); }
    return out;
}
std::vector<double> ParseNumbers(const std::string& text) {
    std::vector<double> out; std::istringstream v(Lines(text)); v.imbue(std::locale::classic());
    for (double x = 0; v >> x;) { out.push_back(x); }
    return out;
}

// ---- 部品の欄を読む
std::string Text(const Json& fields, std::string_view id) { const auto* v = FieldValue(fields, id); return v && v->is_string() ? v->get<std::string>() : std::string{}; }
double Real(const Json& fields, std::string_view id, double fallback) { const auto* v = FieldValue(fields, id); return v && v->is_number() ? v->get<double>() : fallback; }
bool Flag(const Json& fields, std::string_view id) { const auto* v = FieldValue(fields, id); return v && v->is_boolean() && v->get<bool>(); }

struct KindInfo { MachineKind kind; std::string_view type, idField; std::vector<std::string> fields; };
const std::vector<KindInfo>& Kinds() {
    static const std::vector<KindInfo> kinds{
        {MachineKind::Mechanism, "TD2.Mechanism", "mechanismId", {"mechanismId", "toggle", "balance", "seconds", "dwell", "settle", "cameraLift", "cameraPull",
            "regionOffsets", "objectDeltas", "regionOffsetsC", "objectDeltasC", "regionMoves", "objectMoves", "regionMovesC", "objectMovesC"}},
        {MachineKind::Slide, "TD2.SlideRack", "rackId", {"rackId", "seconds", "members", "emptySlots", "slots"}},
        {MachineKind::Stack, "TD2.StackColumn", "stackId", {"stackId", "pullSeconds", "dropSeconds", "resetSeconds", "members", "dockDistances", "slots", "docks"}},
        {MachineKind::Carousel, "TD2.Carousel", "carouselId", {"carouselId", "seconds", "faces", "objects", "pivot", "stations"}},
    };
    return kinds;
}
const KindInfo& InfoOf(MachineKind kind) { return *std::find_if(Kinds().begin(), Kinds().end(), [kind](const auto& k) { return k.kind == kind; }); }

// ---- 四元数（x, y, z, w）。度とエンジンの順 M = Rz·Ry·Rx。
using Q = std::array<double, 4>;
using V = std::array<double, 3>;
Q Mul(const Q& a, const Q& b) {
    return {a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1], a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
            a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3], a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
}
Q Conj(const Q& q) { return {-q[0], -q[1], -q[2], q[3]}; }
V Rotate(const Q& q, const V& v) { const auto r = Mul(Mul(q, {v[0], v[1], v[2], 0}), Conj(q)); return {r[0], r[1], r[2]}; }
Q FromEuler(const V& e) {
    const auto axis = [](double degrees, int i) { const double h = degrees * kPi / 360; Q q{0, 0, 0, std::cos(h)}; q[static_cast<size_t>(i)] = std::sin(h); return q; };
    return Mul(Mul(axis(e[2], 2), axis(e[1], 1)), axis(e[0], 0));
}
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

std::string_view MachineComponentType(MachineKind kind) { return InfoOf(kind).type; }

std::string Machine::KindLabel() const {
    switch (kind) {
    case MachineKind::Mechanism: return "机构";
    case MachineKind::Slide: return "滑块货架";
    case MachineKind::Stack: return "电视塔";
    case MachineKind::Carousel: return "转柱";
    }
    return {};
}

std::vector<std::pair<std::string, std::string>> Machine::Receivers() const {
    std::vector<std::pair<std::string, std::string>> out;
    switch (kind) {
    case MachineKind::Mechanism:
        out.push_back({"mech/" + id, KindLabel() + " " + id});
        if (balance) { out.push_back({"mech/" + id + "/r", KindLabel() + " " + id + "（另一侧）"}); }
        break;
    case MachineKind::Slide:
        for (const auto& m : members) { out.push_back({"slide/" + m, "货架 " + id + "：滑动 " + m}); }
        break;
    case MachineKind::Stack:
        out.push_back({"stack/" + id + "/pull", "电视塔 " + id + "：抽出最下面"});
        out.push_back({"stack/" + id + "/reset", "电视塔 " + id + "：复位"});
        break;
    case MachineKind::Carousel:
        out.push_back({"carousel/" + id + "/cw", "转柱 " + id + "：顺时针"});
        out.push_back({"carousel/" + id + "/ccw", "转柱 " + id + "：逆时针"});
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
                m.id = Text(fields, info.idField);
                switch (info.kind) {
                case MachineKind::Mechanism:
                    m.toggle = Flag(fields, "toggle"); m.balance = Flag(fields, "balance");
                    m.seconds = Real(fields, "seconds", .8); m.dwell = Real(fields, "dwell", 0); m.settle = Real(fields, "settle", 0);
                    m.cameraLift = Real(fields, "cameraLift", 1.5); m.cameraPull = Real(fields, "cameraPull", 1);
                    m.offsets = ParseOffsets(Text(fields, "regionOffsets")); m.offsetsC = ParseOffsets(Text(fields, "regionOffsetsC"));
                    m.deltas = ParseDeltas(Text(fields, "objectDeltas")); m.deltasC = ParseDeltas(Text(fields, "objectDeltasC"));
                    for (const auto* legacy : {"regionMoves", "objectMoves", "regionMovesC", "objectMovesC"}) { m.legacy |= !Text(fields, legacy).empty(); }
                    break;
                case MachineKind::Slide:
                    m.seconds = Real(fields, "seconds", .9); m.members = Names(Text(fields, "members"));
                    m.emptySlots = ParseSlots(Text(fields, "emptySlots")); m.legacy = !Text(fields, "slots").empty();
                    break;
                case MachineKind::Stack:
                    m.pullSeconds = Real(fields, "pullSeconds", .8); m.dropSeconds = Real(fields, "dropSeconds", .35); m.resetSeconds = Real(fields, "resetSeconds", 2);
                    m.members = Names(Text(fields, "members")); m.dockDistances = ParseNumbers(Text(fields, "dockDistances"));
                    m.legacy = !Text(fields, "slots").empty() || !Text(fields, "docks").empty();
                    break;
                case MachineKind::Carousel:
                    m.seconds = Real(fields, "seconds", 1.6); m.members = Names(Text(fields, "faces")); m.extraObjects = Text(fields, "objects");
                    m.legacy = !Text(fields, "pivot").empty() || !Text(fields, "stations").empty();
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
        if (oldValue != newValue) { commands.push_back({{"objectId", after.objectId}, {"componentId", after.componentId}, {"propertyId", id}, {"value", newValue}}); }
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
        // 相対で書き直したら旧形式の行は消す（二重に動かさない）。
        if (before.legacy && !after.legacy) { for (const auto* legacy : {"regionMoves", "objectMoves", "regionMovesC", "objectMovesC"}) { set(legacy, "x", ""); } }
        break;
    case MachineKind::Slide:
        set("seconds", before.seconds, after.seconds); set("members", JoinNames(before.members), JoinNames(after.members));
        set("emptySlots", FormatSlots(before.emptySlots), FormatSlots(after.emptySlots));
        if (before.legacy && !after.legacy) { set("slots", "x", ""); }
        break;
    case MachineKind::Stack:
        set("pullSeconds", before.pullSeconds, after.pullSeconds); set("dropSeconds", before.dropSeconds, after.dropSeconds);
        set("resetSeconds", before.resetSeconds, after.resetSeconds); set("members", JoinNames(before.members), JoinNames(after.members));
        set("dockDistances", FormatNumbers(before.dockDistances), FormatNumbers(after.dockDistances));
        if (before.legacy && !after.legacy) { set("slots", "x", ""); set("docks", "x", ""); }
        break;
    case MachineKind::Carousel:
        set("seconds", before.seconds, after.seconds); set("faces", JoinNames(before.members), JoinNames(after.members));
        set("objects", before.extraObjects, after.extraObjects);
        if (before.legacy && !after.legacy) { set("pivot", "x", ""); set("stations", "x", ""); }
        break;
    }
    if (commands.empty()) { return true; }
    const auto written = Execute(ctx, "authoring.transaction", {{"commands", commands}});
    if (!written.ok) { error = written.error; return false; }
    return true;
}

bool CreateMachine(EditorWorkspaceHostContext& ctx, MachineKind kind, uint64_t host, const std::string& id, std::string& error) {
    const auto& info = InfoOf(kind);
    if (!host) {
        Machine label; label.kind = kind;
        const auto created = Execute(ctx, "scene.create_empty_object", {{"name", label.KindLabel() + " " + id}});
        if (!created.ok) { error = created.error; return false; }
        host = ObjectId(created.data.value("objectId", Json{}));
        if (!host) { error = "机关对象创建失败"; return false; }
    }
    const auto added = Execute(ctx, "component.add", {{"objectId", host}, {"componentType", info.type}});
    if (!added.ok) { error = added.error; return false; }
    uint64_t componentId = 0;
    for (const auto& c : added.data.value("addedComponents", Json::array())) { if (c.value("type", "") == info.type) { componentId = ObjectId(c.value("componentId", Json{})); } }
    if (!componentId) { error = "机关组件创建失败"; return false; }
    const auto named = Execute(ctx, "authoring.transaction", {{"commands", {{{"objectId", host}, {"componentId", componentId}, {"propertyId", info.idField}, {"value", id}}}}});
    if (!named.ok) { error = named.error; return false; }
    return true;
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
    if (m.id.empty() || !StableAuthoringId(m.id)) { problems.push_back("ID 只能用英文字母、数字、点、短横线和下划线"); }
    if (std::count_if(all.begin(), all.end(), [&](const Machine& o) { return o.kind == m.kind && o.id == m.id; }) > 1) { problems.push_back("同类机关里 ID 重复：" + m.id); }
    if (m.legacy) { problems.push_back("还留着旧格式的绝对坐标；重新记录一次即可改成相对"); }
    switch (m.kind) {
    case MachineKind::Mechanism:
        if (m.offsets.empty() && m.deltas.empty()) { problems.push_back("状态B还没有内容：点电视加入区域偏移，或加入物体并摆状态B"); }
        for (const auto* list : {&m.offsets, &m.offsetsC}) for (const auto& o : *list) {
            if (!placed(o.regionId)) { problems.push_back("区域没有放进世界：" + o.regionId); }
            if (!o.anchor.empty() && !placed(o.anchor)) { problems.push_back("锚点区域没有放进世界：" + o.anchor); }
        }
        if (m.balance && m.offsetsC.empty() && m.deltasC.empty()) { problems.push_back("天平还没有另一侧（状态C）的内容"); }
        for (const auto* list : {&m.deltas, &m.deltasC}) for (const auto& d : *list) {
            if (std::find(input.sceneObjects.begin(), input.sceneObjects.end(), d.object) == input.sceneObjects.end()) { problems.push_back("物体不存在：" + std::to_string(d.object)); }
            if (d.anchor && std::find(input.sceneObjects.begin(), input.sceneObjects.end(), d.anchor) == input.sceneObjects.end()) { problems.push_back("锚点物体不存在：" + std::to_string(d.anchor)); }
        }
        break;
    case MachineKind::Slide: {
        if (m.members.size() < 1) { problems.push_back("还没有成员：点电视加入"); break; }
        for (const auto& id : m.members) { if (!placed(id)) { problems.push_back("成员没有放进世界：" + id); } }
        if (m.emptySlots.empty()) { problems.push_back("需要至少一个空槽：点地图上空着的格子"); }
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
            if ((ex && !hasU) || (ey && !hasV)) { problems.push_back("空槽的 3D 位置推不出来：成员需要在同一行或同一列上排开"); break; }
        }
        break;
    }
    case MachineKind::Stack:
        if (m.members.size() < 2) { problems.push_back("电视塔至少要两台电视（自下而上）"); }
        for (const auto& id : m.members) { if (!placed(id)) { problems.push_back("成员没有放进世界：" + id); } }
        if (m.dockDistances.empty()) { problems.push_back("需要至少一个抽出距离"); }
        if (m.dockDistances.size() + 1 < m.members.size()) { problems.push_back("抽出距离少于可以抽出的层数；多出的抽板只会晃一下"); }
        break;
    case MachineKind::Carousel:
        if (m.members.size() < 2) { problems.push_back("转柱至少要两个面"); }
        for (const auto& id : m.members) { if (!placementOf(id)) { problems.push_back("面不是电视地图：" + id); } }
        if (const auto has = input.objectHasModel.find(m.objectId); has != input.objectHasModel.end() && !has->second) {
            problems.push_back("组件所在的物体没有模型；请把转柱组件放在柱子上（柱子的位置就是转轴）");
        }
        break;
    }
    if (PlatesFor(m, input).empty() && m.dwell <= 0) { problems.push_back("还没有压板连到它（在电视地图里选压板，在「机关目标」里勾选）"); }
    return problems;
}

Pose ApplyDelta(const Pose& a, const Pose& base, const MachineObjectDelta& d) {
    const auto qa = FromEuler(a.rotation), qbase = FromEuler(base.rotation);
    const auto qb = Mul(qbase, FromEuler(d.rotation));
    const auto offset = Rotate(qbase, d.position);
    Pose b = a;
    b.position = {base.position[0] + offset[0], base.position[1] + offset[1], base.position[2] + offset[2]};
    if (d.hasPivot) {
        const auto c = Rotate(qbase, d.pivot);
        const V center{base.position[0] + c[0], base.position[1] + c[1], base.position[2] + c[2]};
        const auto turned = Rotate(Mul(qb, Conj(qa)), {a.position[0] - center[0], a.position[1] - center[1], a.position[2] - center[2]});
        b.position = {center[0] + turned[0], center[1] + turned[1], center[2] + turned[2]};
    }
    b.rotation = ToEuler(qb);
    return b;
}

MachineObjectDelta MakeDelta(const Pose&, const Pose& base, const Pose& b, const MachineObjectDelta& previous) {
    auto d = previous;
    const auto inverse = Conj(FromEuler(base.rotation));
    const auto p = Rotate(inverse, {b.position[0] - base.position[0], b.position[1] - base.position[1], b.position[2] - base.position[2]});
    for (size_t i = 0; i < 3; ++i) { d.position[i] = std::round(p[i] * 10000) / 10000; }
    d.rotation = ToEuler(Mul(inverse, FromEuler(b.rotation)));
    return d;
}
}
