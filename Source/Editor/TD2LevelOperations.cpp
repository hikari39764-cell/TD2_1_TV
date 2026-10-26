#include "TD2LevelOperations.h"
#include "TD2LevelModel.h"
#include "TD2LevelCheck.h"
#include "TD2CameraLayout.h"
#include "../Common/TD2MachineText.h"
#include "../Common/TD2TelevisionModels.h"
#include <cmath>
#include <memory>
#include <sstream>

namespace TD2::Editor::Level {
namespace {
using namespace HIKARI::EDITOR_PROJECT;

// 一回の呼び出しの結果。
struct Reply {
    bool ok = true;
    std::string code = "OK", message = "完了";
    Json data = Json::object();
    void Fail(std::string why, std::string kind = "FAILED") { ok = false; code = std::move(kind); message = std::move(why); }
};
using Run = void (*)(EditorWorkspaceHostContext&, const Json&, Reply&);

// 処理の器：引数を読み、画面の無い文脈（命令の入口だけ）を作って本体を呼ぶ。
class Handler final : public IEditorOperationHandler {
public:
    Handler(IEditorWorkspaceCommands* commands, Run run) : commands_(commands), run_(run) {}
    void Execute(std::string_view parametersJson, IEditorOperationResultSink& sink) override {
        Reply reply;
        const auto parameters = Json::parse(parametersJson, nullptr, false);
        if (!commands_) { reply.Fail("この Editor は項目の操作から場面を書けません", "UNAVAILABLE"); }
        else if (!parameters.is_object()) { reply.Fail("引数は JSON のオブジェクト", "INVALID_PARAMETERS"); }
        else {
            EditorWorkspaceHostContext ctx{};
            ctx.commands = commands_;
            try { run_(ctx, parameters, reply); }
            catch (const std::exception& e) { reply.Fail(e.what(), "INVALID_PARAMETERS"); }
        }
        sink.Receive(reply.ok, reply.code, reply.message, reply.data.dump());
    }

private:
    IEditorWorkspaceCommands* commands_;
    Run run_;
};

// ---- 区域の読み書き（レベル編集と同じ草稿・検査・書き込み）
bool Load(EditorWorkspaceHostContext& ctx, MapModel& model, Reply& reply) {
    model.Poll(ctx, true);
    if (!model.error.empty()) { reply.Fail(model.error); return false; }
    return true;
}
const RoomItem* Find(const MapModel& model, const Json& p) {
    const auto region = p.value("regionId", std::string{});
    const uint64_t object = p.contains("objectId") ? ObjectId(p["objectId"]) : 0;
    for (const auto& room : model.rooms) {
        if ((!region.empty() && room.settings.region.regionId == region) || (object && room.object.objectId == object)) { return &room; }
    }
    return nullptr;
}
bool Open(EditorWorkspaceHostContext& ctx, MapModel& model, const Json& p, Reply& reply) {
    if (!Load(ctx, model, reply)) { return false; }
    const auto* room = Find(model, p);
    if (!room) { reply.Fail("区域が見つかりません（regionId か objectId）", "NOT_FOUND"); return false; }
    if (!model.Select(ctx, room->object.objectId)) { reply.Fail(model.error.empty() ? "区域を開けません" : model.error); return false; }
    return true;
}
bool Save(EditorWorkspaceHostContext& ctx, MapModel& model, Reply& reply) {
    if (model.Commit(ctx)) { return true; }
    reply.Fail(model.error.empty() ? "書き込めません" : model.error, "REJECTED");
    return false;
}

Json RoomJson(const RoomItem& room, const LayoutSnapshot* layout, bool detail) {
    const auto& s = room.settings;
    Json j{{"objectId", room.object.objectId}, {"name", room.object.name}, {"regionId", s.region.regionId},
           {"size", {s.region.cols, s.region.rows}}, {"placed", s.worldPlaced}, {"entry", s.entryRegion}};
    if (s.worldPlaced) { j["world"] = {s.worldX, s.worldY}; }
    if (const auto* r = layout ? layout->Room(s.region.regionId) : nullptr) {
        j["facing"] = IsBackFacing(*layout, s.region.regionId) ? "back" : "front";
        j["camera"] = !r->cameraId ? "none" : r->cameraLocked ? "locked" : r->cameraAuto.empty() ? "manual" : CameraEdited(*r) ? "edited" : "auto";
        if (const auto* area = layout->AreaOf(s.region.regionId)) { j["area"] = area->name; }
    }
    if (detail) {
        Json rows = Json::array();
        for (int y = 0; y < s.region.rows; ++y) {
            std::string row;
            for (int x = 0; x < s.region.cols; ++x) { row += s.region.cells[CellIndex(s.region, x, y)]; }
            rows.push_back(row);
        }
        j["cells"] = rows; j["spawn"] = {s.spawnX, s.spawnY}; j["devices"] = s.devices;
    }
    return j;
}
void ReplyRoom(EditorWorkspaceHostContext& ctx, MapModel& model, Reply& reply) {
    LayoutSnapshot layout; std::string error;
    const bool haveLayout = LoadLayout(ctx, layout, error);
    if (const auto* room = model.Current()) { reply.data["room"] = RoomJson(*room, haveLayout ? &layout : nullptr, true); }
}

// ---- 操作
void List(EditorWorkspaceHostContext& ctx, const Json& p, Reply& reply) {
    MapModel model;
    if (!Load(ctx, model, reply)) { return; }
    LayoutSnapshot layout; std::string error;
    const bool haveLayout = LoadLayout(ctx, layout, error);
    if (p.contains("regionId") || p.contains("objectId")) {
        const auto* room = Find(model, p);
        if (!room) { reply.Fail("区域が見つかりません", "NOT_FOUND"); return; }
        reply.data["room"] = RoomJson(*room, haveLayout ? &layout : nullptr, true);
        return;
    }
    Json rooms = Json::array(), models = Json::array(), kinds = Json::array();
    for (const auto& room : model.rooms) { rooms.push_back(RoomJson(room, haveLayout ? &layout : nullptr, false)); }
    for (const auto& m : kTelevisionModels) { models.push_back({{"model", m.stem}, {"label", m.label}, {"grid", {m.cols, m.rows}}}); }
    for (const auto kind : kDeviceKindNames) { kinds.push_back(kind); }
    reply.data = {{"rooms", rooms}, {"models", models}, {"deviceKinds", kinds},
                  {"notes", "座標はマス（左上原点、下が +y）。cells は上の行から 0 空き / 1 地面。装置は一行に「id kind x y w h key=value…」。"
                            "世界地図で辺を共有する部屋が相通じる（角だけ・隙間は通じない）。"}};
}

void NewTelevision(EditorWorkspaceHostContext& ctx, const Json& p, Reply& reply) {
    const auto wanted = p.value("model", std::string("SM_L_TV_Steel"));
    const TelevisionModel* model = nullptr;
    for (const auto& m : kTelevisionModels) { if (m.stem == wanted || m.guid == wanted) { model = &m; } }
    if (!model) { reply.Fail("知らないテレビの型：" + wanted + "（td2.level.list の models）", "INVALID_PARAMETERS"); return; }
    const auto position = p.value("position", Json::array({0.0, 0.0, 0.0}));
    const double yaw = p.value("yaw", 0.0), scale = p.value("scale", 1.0);
    const auto created = Execute(ctx, "scene.create_empty_object", {{"name", p.value("name", std::string("テレビ ") + std::string(model->label))}});
    if (!created.ok) { reply.Fail(created.error); return; }
    const auto id = ObjectId(created.data.value("objectId", Json{}));
    const auto moved = Execute(ctx, "scene.set_transform", {{"objectId", id}, {"position", position}, {"rotation", {0.0, yaw, 0.0}}, {"scale", {scale, scale, scale}}});
    if (!moved.ok) { reply.Fail(moved.error); return; }
    // 区域を付けるとモデルの部品も付く。モデルの型を入れると、レベル編集と同じく型の格子・床・新しい ID・世界の外へ整う。
    const auto added = Execute(ctx, "component.add", {{"objectId", id}, {"componentType", std::string(kRegionComponentType)}});
    if (!added.ok) { reply.Fail(added.error); return; }
    uint64_t modelComponent = 0;
    const auto instances = Execute(ctx, "component.instances.query", {{"objectId", id}});
    for (const auto& c : instances.data.value("components", Json::array())) {
        if (c.value("type", "") == "ModelComponent") { modelComponent = ObjectId(c.value("componentId", Json{})); }
    }
    std::string error;
    if (!modelComponent || !Transact(ctx, Json::array({PropertyCommand(id, modelComponent, "assetId", std::string(model->guid))}), error)) {
        reply.Fail(error.empty() ? "モデルの部品がありません" : error); return;
    }
    MapModel map;
    if (!Load(ctx, map, reply)) { return; }
    map.refresh = true; map.Poll(ctx, true);
    const auto* room = map.Room(id);
    if (!room) { reply.Fail("区域を整えられません"); return; }
    reply.data["room"] = RoomJson(*room, nullptr, true);
}

void Place(EditorWorkspaceHostContext& ctx, const Json& p, Reply& reply) {
    MapModel model;
    if (!Open(ctx, model, p, reply)) { return; }
    auto& s = model.draft;
    const auto region = s.region.regionId;
    if (p.value("remove", false)) {
        s.worldPlaced = false;
        if (Save(ctx, model, reply)) { reply.data = {{"regionId", region}, {"placed", false}}; }
        return;
    }
    if (!p.contains("worldX") || !p.contains("worldY")) { reply.Fail("worldX と worldY（世界地図の整数のマス）が要ります", "INVALID_PARAMETERS"); return; }
    const bool first = !s.worldPlaced;
    s.worldPlaced = true; s.worldX = p["worldX"].get<int>(); s.worldY = p["worldY"].get<int>();
    if (!Save(ctx, model, reply)) { return; }
    reply.data = {{"regionId", region}, {"world", {s.worldX, s.worldY}}};
    if (!first) { return; }
    // 初めて世界に置いた：地図で一番長く辺を共有するテレビの大区域へ入れ、カメラを作る（レベル編集で置いた時と同じ）。
    std::string beside; int64_t length = 0;
    const auto self = Placement(model.draft);
    for (const auto& room : model.rooms) {
        if (room.settings.region.regionId == region || !room.settings.worldPlaced) { continue; }
        if (const auto edge = FindSharedWorldEdge(self, Placement(room.settings)); edge && edge->overlapEnd - edge->overlapBegin > length) {
            length = edge->overlapEnd - edge->overlapBegin; beside = room.settings.region.regionId;
        }
    }
    LayoutSnapshot layout; std::string error;
    if (!LoadLayout(ctx, layout, error)) { reply.Fail(error); return; }
    if (const auto* area = beside.empty() ? nullptr : layout.AreaOf(beside)) {
        auto regions = area->regions;
        regions.push_back(region);
        const auto name = area->name;
        if (!Transact(ctx, Json::array({PropertyCommand(area->objectId, area->componentId, "regions", MachineText::JoinNames(regions))}), error) ||
            !LoadLayout(ctx, layout, error)) { reply.Fail(error); return; }
        reply.data["area"] = name;
    }
    GenerateReport report;
    if (!GenerateCameras(ctx, layout, {}, region, false, report, error)) { reply.Fail(error); return; }
    reply.data["camera"] = report.written ? "created" : "kept";
}

void EditRoom(EditorWorkspaceHostContext& ctx, const Json& p, Reply& reply) {
    MapModel model;
    if (!Open(ctx, model, p, reply)) { return; }
    auto& s = model.draft;
    if (p.contains("cols") || p.contains("rows")) {
        const auto previous = s.region;
        s.region.cols = p.value("cols", s.region.cols); s.region.rows = p.value("rows", s.region.rows);
        if (!ResizeCells(previous, s.region)) { reply.Fail("大きさを変えられません（1〜64 マス）", "INVALID_PARAMETERS"); return; }
    }
    if (p.contains("entry")) { s.entryRegion = p["entry"].get<bool>(); }
    if (p.contains("spawn")) { s.spawnX = p["spawn"].at(0).get<float>(); s.spawnY = p["spawn"].at(1).get<float>(); }
    if (Save(ctx, model, reply)) { ReplyRoom(ctx, model, reply); }
}

void Terrain(EditorWorkspaceHostContext& ctx, const Json& p, Reply& reply) {
    MapModel model;
    if (!Open(ctx, model, p, reply)) { return; }
    auto& r = model.draft.region;
    if (p.contains("cells")) {
        std::string cells;
        if (p["cells"].is_array()) { for (const auto& row : p["cells"]) { cells += row.get<std::string>(); } }
        else { cells = p["cells"].get<std::string>(); }
        std::erase_if(cells, [](char c) { return c == '\n' || c == '\r' || c == ' '; });
        if (cells.size() != static_cast<size_t>(r.cols * r.rows) || cells.find_first_not_of("01") != std::string::npos) {
            reply.Fail("cells は上の行から 0（空き）と 1（地面）を cols×rows 個", "INVALID_PARAMETERS"); return;
        }
        r.cells = cells;
    }
    if (p.contains("rect")) {
        const int x0 = p["rect"].at(0).get<int>(), y0 = p["rect"].at(1).get<int>(), w = p["rect"].at(2).get<int>(), h = p["rect"].at(3).get<int>();
        const char cell = p.value("solid", true) ? '1' : '0';
        for (int y = (std::max)(0, y0); y < (std::min)(r.rows, y0 + h); ++y)
            for (int x = (std::max)(0, x0); x < (std::min)(r.cols, x0 + w); ++x) { r.cells[CellIndex(r, x, y)] = cell; }
    }
    if (Save(ctx, model, reply)) { ReplyRoom(ctx, model, reply); }
}

void Devices(EditorWorkspaceHostContext& ctx, const Json& p, Reply& reply) {
    MapModel model;
    if (!Open(ctx, model, p, reply)) { return; }
    auto& d = model.devices; auto& s = model.draft;
    if (p.contains("text")) {
        d.Load(p["text"].get<std::string>());
        if (!d.IsLoaded()) { reply.Fail(d.Error(), "INVALID_PARAMETERS"); return; }
    }
    if (p.contains("remove")) {
        const auto id = p["remove"].get<std::string>();
        int index = -1;
        for (size_t i = 0; i < d.Items().size(); ++i) { if (d.Items()[i].id == id) { index = static_cast<int>(i); } }
        if (index < 0) { reply.Fail("装置が見つかりません：" + id, "NOT_FOUND"); return; }
        d.Select(index); d.RemoveSelected();
    }
    if (p.contains("add")) {
        // 「kind x y [w h] [key=value…]」。ID は種類の短い名前と英字で付く（plate_A など）。
        std::istringstream in(p["add"].get<std::string>());
        in.imbue(std::locale::classic());
        std::string kind; double x = 0, y = 0, w = -1, h = -1;
        if (!(in >> kind >> x >> y)) { reply.Fail("add は「kind x y [w h] [key=value…]」", "INVALID_PARAMETERS"); return; }
        std::vector<std::string> options;
        for (std::string token; in >> token;) {
            if (token.find('=') != std::string::npos) { options.push_back(token); }
            else if (w < 0) { w = std::stod(token); }
            else if (h < 0) { h = std::stod(token); }
        }
        if (!d.Add(kind, static_cast<int>(std::floor(x)), static_cast<int>(std::floor(y)), s.region.cols, s.region.rows)) {
            reply.Fail("置けません（種類か位置）：" + kind, "INVALID_PARAMETERS"); return;
        }
        auto* added = d.Selected();
        added->x = x; added->y = y;
        if (w > 0) { added->w = w; }
        if (h > 0) { added->h = h; }
        for (const auto& option : options) {
            const auto equals = option.find('=');
            const auto key = option.substr(0, equals), value = option.substr(equals + 1);
            if (key == "dir") { added->direction = value; }
            else if (key == "invert") { added->inverted = value == "1"; }
            else if (key == "latch") { added->latch = value == "1"; }
            else if (key == "range") { added->range = std::stod(value); }
            else if (key == "targets") { std::istringstream ids(value); for (std::string t; std::getline(ids, t, ',');) { added->targets.push_back(t); } }
            else { added->SetExtra(key, value); }
        }
        reply.data["added"] = added->id;
    }
    if (const auto problem = d.Validate(s.region.cols, s.region.rows); !problem.empty()) { reply.Fail(problem, "INVALID_PARAMETERS"); return; }
    s.devices = d.Serialize();
    const auto added = reply.data.value("added", std::string{});
    if (!Save(ctx, model, reply)) { return; }
    ReplyRoom(ctx, model, reply);
    if (!added.empty()) { reply.data["added"] = added; }
}

void Link(EditorWorkspaceHostContext& ctx, const Json& p, Reply& reply) {
    const auto plate = p.value("plate", std::string{});
    auto target = p.value("target", std::string{});
    const auto slash = plate.find('/');
    if (slash == std::string::npos || target.empty()) { reply.Fail("plate は「区域ID/圧力板ID」、target は扉・レーザー・橋・ゴール（同じテレビは ID、別のテレビは「区域ID/ID」）か仕掛けの受け口", "INVALID_PARAMETERS"); return; }
    const auto region = plate.substr(0, slash), id = plate.substr(slash + 1);
    if (target.starts_with(region + "/")) { target = target.substr(region.size() + 1); }   // 同じテレビの装置は ID だけで書く
    MapModel model;
    if (!Open(ctx, model, Json{{"regionId", region}}, reply)) { return; }
    const auto& items = model.devices.Items();
    const auto item = std::find_if(items.begin(), items.end(), [&](const DeviceDraft& d) { return d.id == id; });
    if (item == items.end() || item->kind != "Plate") { reply.Fail("圧力板が見つかりません：" + plate, "NOT_FOUND"); return; }
    const bool present = std::find(item->targets.begin(), item->targets.end(), target) != item->targets.end();
    const bool want = p.value("on", !present);
    if (present != want && !model.devices.ToggleTarget(id, target)) { reply.Fail("つなげません：" + target, "REJECTED"); return; }
    model.draft.devices = model.devices.Serialize();
    if (!Save(ctx, model, reply)) { return; }
    reply.data = {{"plate", plate}, {"target", target}, {"on", want}};
    if (want && IsMachineTarget(target)) { reply.data["note"] = "画面の外の仕掛けへの信号線は、レベル編集の「配線」でつなぎ直すと引かれます"; }
}

void Flip(EditorWorkspaceHostContext& ctx, const Json& p, Reply& reply) {
    MapModel model;
    if (!Open(ctx, model, p, reply)) { return; }
    const auto region = model.draft.region.regionId;
    LayoutSnapshot layout; std::string error;
    if (!LoadLayout(ctx, layout, error)) { reply.Fail(error); return; }
    const bool back = IsBackFacing(layout, region);
    const auto want = p.value("facing", std::string(back ? "front" : "back"));
    if (want != "front" && want != "back") { reply.Fail("facing は front か back", "INVALID_PARAMETERS"); return; }
    if ((want == "back") != back) {
        if (!FlipTelevision(ctx, layout, region, error)) { reply.Fail(error); return; }
        // 3D で隣に付けていたテレビは、裏返すと付き方が変わるので自由配置にする。
        if (!model.draft.stageAnchor.empty()) { model.draft.stageAnchor.clear(); if (!Save(ctx, model, reply)) { return; } }
    }
    reply.data = {{"regionId", region}, {"facing", IsBackFacing(layout, region) ? "back" : "front"}};
}

void Camera(EditorWorkspaceHostContext& ctx, const Json& p, Reply& reply) {
    MapModel model;
    if (!Open(ctx, model, p, reply)) { return; }
    const auto region = model.draft.region.regionId;
    if (!model.draft.worldPlaced) { reply.Fail("カメラは世界地図に置いたテレビだけ", "REJECTED"); return; }
    LayoutSnapshot layout; GenerateReport report; std::string error;
    if (!LoadLayout(ctx, layout, error) || !GenerateCameras(ctx, layout, {}, region, p.value("reset", false), report, error)) { reply.Fail(error); return; }
    const auto* room = layout.Room(region);
    reply.data = {{"regionId", region}, {"written", report.written}, {"kept", report.kept}, {"cameraObject", room ? room->cameraId : 0}};
}

void Check(EditorWorkspaceHostContext& ctx, const Json&, Reply& reply) {
    MapModel model;
    if (!Load(ctx, model, reply)) { return; }
    std::vector<Machine> machines; std::string error;
    (void)LoadMachines(ctx, machines, error);
    LayoutSnapshot layout;
    const bool haveLayout = LoadLayout(ctx, layout, error);
    std::vector<CheckRoom> rooms;
    for (const auto& room : model.rooms) { rooms.push_back({room.settings, room.object.name}); }
    Json issues = Json::array();
    for (const auto& issue : CheckLevel(rooms, machines, haveLayout ? &layout : nullptr)) {
        Json item{{"level", issue.level == IssueLevel::Error ? "error" : "warning"}, {"text", issue.text}, {"regionId", issue.regionId}};
        if (issue.x >= 0) { item["cell"] = {issue.x, issue.y}; }
        issues.push_back(item);
    }
    reply.data = {{"issues", issues}};
}

// ---- 登録
using Parameter = EditorOperationParameterDescriptor;
constexpr auto kString = EditorOperationParameterType::String, kInteger = EditorOperationParameterType::Integer;
constexpr auto kNumber = EditorOperationParameterType::Number, kBoolean = EditorOperationParameterType::Boolean, kJson = EditorOperationParameterType::Json;
Parameter Param(const char* name, EditorOperationParameterType type, bool required, const char* description) {
    Parameter p{}; p.name = name; p.type = type; p.required = required ? 1u : 0u; p.description = description; return p;
}
struct Spec {
    const char* id;
    const char* summary;
    bool mutation;
    std::vector<Parameter> parameters;
    Run run;
};
}

void RegisterLevelOperations(IEditorOperationRegistrar& registrar) {
    // 処理の器はモジュールが持つ（Editor が登録を外すまで生きている）。
    static std::vector<std::unique_ptr<Handler>> handlers;
    static const char* const terms[]{"level", "tv", "map", "td2", "レベル", "テレビ", "关卡", "电视"};
    const auto region = [] { return Param("regionId", kString, false, "区域 ID（td2.level.list の regionId）"); };
    const auto object = [] { return Param("objectId", kInteger, false, "テレビの物体 ID（regionId の代わり）"); };
    const std::vector<Spec> specs{
        {"td2.level.list", "レベルの一覧：テレビ（区域）ごとの大きさ・世界地図の位置・向き・カメラと、置けるテレビの型・装置の種類。regionId を渡すとそのテレビの地形と装置。",
         false, {region(), object()}, List},
        {"td2.level.new_tv", "新しいテレビを 3D に置き、区域を付けて整える（型の格子・下の一列の床・新しい ID、世界地図にはまだ置かない）。",
         true, {Param("model", kString, false, "テレビの型（td2.level.list の models、既定 SM_L_TV_Steel）"),
                Param("position", kJson, false, "3D の位置 [x, y, z]（メートル）"), Param("yaw", kNumber, false, "向き（度、0 で画面が +Z）"),
                Param("scale", kNumber, false, "縮尺"), Param("name", kString, false, "物体の名前")}, NewTelevision},
        {"td2.level.place", "テレビを世界地図のマスへ置く・動かす（remove で外す）。初めて置くと隣のテレビの大区域へ入り、カメラが自動で付く。",
         true, {region(), object(), Param("worldX", kInteger, false, "世界地図の左上のマス x"), Param("worldY", kInteger, false, "世界地図の左上のマス y"),
                Param("remove", kBoolean, false, "true で世界地図から外す")}, Place},
        {"td2.level.room", "テレビの区域の大きさ（cols・rows）・開始テレビ・出生点を変える。",
         true, {region(), object(), Param("cols", kInteger, false, "列の数"), Param("rows", kInteger, false, "行の数"),
                Param("entry", kBoolean, false, "開始テレビにする（他は外れる）"), Param("spawn", kJson, false, "出生点 [x, y]（マス、幅 0.625・高さ 1.5 の当たりの左上）")}, EditRoom},
        {"td2.level.terrain", "地形を描く：cells で全体を置き換えるか、rect の範囲を地面（solid=true）か空き（false）にする。",
         true, {region(), object(), Param("cells", kJson, false, "上の行から 0/1 の文字列（行の配列でも可）"),
                Param("rect", kJson, false, "[x, y, w, h]（マス）"), Param("solid", kBoolean, false, "rect を地面にするか（既定 true）")}, Terrain},
        {"td2.level.devices", "装置を置く・消す・書き換える。add は「kind x y [w h] [key=value…]」（例 Plate 3 5、Door 9 3 1 3、Goal 8 3、Laser 1 2 dir=Right）。",
         true, {region(), object(), Param("add", kString, false, "置く装置の一行（ID は自動）"), Param("remove", kString, false, "消す装置の ID"),
                Param("text", kString, false, "装置の欄を丸ごと置き換える文字列")}, Devices},
        {"td2.level.link", "圧力板の配線：plate から target（扉・レーザー・橋・ゴール、別のテレビは「区域ID/ID」、仕掛けの受け口）へつなぐ・外す。",
         true, {Param("plate", kString, true, "「区域ID/圧力板ID」"), Param("target", kString, true, "つなぎ先"),
                Param("on", kBoolean, false, "true でつなぐ、false で外す（省くと切り替え）")}, Link},
        {"td2.level.flip", "テレビの向き：front（地図で隣り合うテレビと同じ向き）か back。3D でテレビとカメラを画面の中心の縦の軸で 180° 回す。",
         true, {region(), object(), Param("facing", kString, false, "front か back（省くと切り替え）")}, Flip},
        {"td2.level.camera", "テレビの区域カメラを今の配置から作り直す（作者が動かしたカメラは残す。reset で自動に戻す）。",
         true, {region(), object(), Param("reset", kBoolean, false, "true で作者の調整も上書きして自動に戻す")}, Camera},
        {"td2.level.check", "レベルの検査：開始テレビ・ゴールの数・重なり・口・装置・仕掛けの問題を一覧にする。", false, {}, Check},
    };
    for (const auto& spec : specs) {
        handlers.push_back(std::make_unique<Handler>(registrar.Commands(), spec.run));
        EditorOperationDescriptor d{};
        d.id = spec.id; d.summary = spec.summary; d.workspace = kLevelWorkspace;
        d.kind = spec.mutation ? EditorOperationKind::Mutation : EditorOperationKind::Query;
        d.permission = spec.mutation ? EditorOperationPermission::ModifyEditorState : EditorOperationPermission::ReadEditorState;
        d.exposureMask = EditorOperationExposureMask::Human | EditorOperationExposureMask::Agent;
        d.parameters = spec.parameters.data(); d.parameterCount = spec.parameters.size();
        d.searchTerms = terms; d.searchTermCount = std::size(terms);
        d.handler = handlers.back().get();
        if (!registrar.RegisterOperation(d).ok) { return; }
    }
}
}
