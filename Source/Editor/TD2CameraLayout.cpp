#include "TD2CameraLayout.h"
#include "../Common/TD2RegionData.h"
#include "../Common/TD2TelevisionModels.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <sstream>

namespace TD2::Editor {
namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kFov = 26.0;              // 区域カメラと演出の基準の画角（度）
constexpr double kNarrowAspect = 4.0 / 3;  // 画面の検査は一番狭い縦横比（4:3 前後）で行う
constexpr double kMinShare = .6;           // 切り替え中はどの瞬間も、少なくとも一台の画面の半分以上を画に入れる
constexpr std::string_view kAreaComponent = "TD2.CameraArea";

using M3 = std::array<std::array<double, 3>, 3>;
Vec3 Add(const Vec3& a, const Vec3& b, double k = 1) { return {a[0] + b[0] * k, a[1] + b[1] * k, a[2] + b[2] * k}; }
Vec3 Sub(const Vec3& a, const Vec3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
double Dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Vec3 Cross(const Vec3& a, const Vec3& b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
Vec3 Norm(const Vec3& a) { const double n = std::sqrt(Dot(a, a)); return n > 1e-9 ? Vec3{a[0] / n, a[1] / n, a[2] / n} : Vec3{0, 0, 1}; }
Vec3 Lerp(const Vec3& a, const Vec3& b, double t) { return {a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t}; }
Vec3 Flat(const Vec3& a) { return Norm({a[0], 0, a[2]}); }
constexpr Vec3 kUp{0, 1, 0};

// エンジンの回転順：M = Rz·Ry·Rx（X → Y → Z）。カメラもテレビもローカル +Z を向く。
M3 Rotation(const Vec3& euler) {
    const auto r = [](int axis, double deg) {
        const double a = deg * kPi / 180, c = std::cos(a), s = std::sin(a);
        if (axis == 0) return M3{{{1, 0, 0}, {0, c, -s}, {0, s, c}}};
        if (axis == 1) return M3{{{c, 0, s}, {0, 1, 0}, {-s, 0, c}}};
        return M3{{{c, -s, 0}, {s, c, 0}, {0, 0, 1}}};
    };
    const auto mul = [](const M3& a, const M3& b) {
        M3 m{};
        for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) for (int k = 0; k < 3; ++k) { m[i][j] += a[i][k] * b[k][j]; }
        return m;
    };
    return mul(mul(r(2, euler[2]), r(1, euler[1])), r(0, euler[0]));
}
Vec3 Apply(const M3& m, const Vec3& v) {
    return {m[0][0] * v[0] + m[0][1] * v[1] + m[0][2] * v[2], m[1][0] * v[0] + m[1][1] * v[1] + m[1][2] * v[2],
            m[2][0] * v[0] + m[2][1] * v[1] + m[2][2] * v[2]};
}
Vec3 RotateY(const Vec3& v, double deg) {
    const double c = std::cos(deg * kPi / 180), s = std::sin(deg * kPi / 180);
    return {v[0] * c + v[2] * s, v[1], -v[0] * s + v[2] * c};
}
// カメラの回転行列（列 = ローカル X（画の左）・Y（上）・Z（前））。roll は前の軸まわりの傾き（度）。
M3 LookMatrix(const Vec3& eye, const Vec3& look, double roll = 0) {
    const auto f = Norm(Sub(look, eye));
    auto left = Norm(Cross(kUp, f));
    auto up = Cross(f, left);
    if (roll != 0) {
        const double c = std::cos(roll * kPi / 180), s = std::sin(roll * kPi / 180);
        const auto l = left, u = up;
        left = Add({l[0] * c, l[1] * c, l[2] * c}, u, s);
        up = Add({u[0] * c, u[1] * c, u[2] * c}, l, -s);
    }
    return M3{{{left[0], up[0], f[0]}, {left[1], up[1], f[1]}, {left[2], up[2], f[2]}}};
}
// M = Rz(c)·Ry(b)·Rx(a) を (a, b, c) 度へ。真横（b = ±90°）は c = 0 として a だけで表す。
Vec3 EulerFrom(const M3& m) {
    const double sb = std::clamp(-m[2][0], -1.0, 1.0);
    double a, b, c;
    if (std::abs(sb) < .99999) { b = std::asin(sb); a = std::atan2(m[2][1], m[2][2]); c = std::atan2(m[1][0], m[0][0]); }
    else { b = std::copysign(kPi / 2, sb); c = 0; a = sb < 0 ? std::atan2(-m[0][1], m[1][1]) : std::atan2(m[0][1], m[1][1]); }
    a *= 180 / kPi; b *= 180 / kPi; c *= 180 / kPi;
    if (std::abs(c) > 90) { a += 180; b = 180 - b; c += 180; }   // 同じ回転の読みやすい方（傾き 0 付近）
    const auto wrap = [](double v) { return std::fmod(std::fmod(v + 180, 360) + 360, 360) - 180; };
    return {wrap(a), wrap(b), wrap(c)};
}
std::array<double, 4> QuatFrom(const M3& m) {
    const double t = m[0][0] + m[1][1] + m[2][2];
    double x, y, z, w;
    if (t > 0) { const double s = std::sqrt(t + 1) * 2; w = s / 4; x = (m[2][1] - m[1][2]) / s; y = (m[0][2] - m[2][0]) / s; z = (m[1][0] - m[0][1]) / s; }
    else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) { const double s = std::sqrt(1 + m[0][0] - m[1][1] - m[2][2]) * 2; x = s / 4; w = (m[2][1] - m[1][2]) / s; y = (m[0][1] + m[1][0]) / s; z = (m[0][2] + m[2][0]) / s; }
    else if (m[1][1] > m[2][2]) { const double s = std::sqrt(1 + m[1][1] - m[0][0] - m[2][2]) * 2; y = s / 4; w = (m[0][2] - m[2][0]) / s; x = (m[0][1] + m[1][0]) / s; z = (m[1][2] + m[2][1]) / s; }
    else { const double s = std::sqrt(1 + m[2][2] - m[0][0] - m[1][1]) * 2; z = s / 4; w = (m[1][0] - m[0][1]) / s; x = (m[0][2] + m[2][0]) / s; y = (m[1][2] + m[2][1]) / s; }
    return {x, y, z, w};
}
double Round(double v, double step = 1e-4) { return std::round(v / step) * step; }

// ---- テレビの形 ----
M3 TvMatrix(const LayoutRoom& r) { return Rotation(r.tv.rotation); }
Vec3 TvLocal(const LayoutRoom& r, const Vec3& local) {
    return Add(r.tv.position, Apply(TvMatrix(r), {local[0] * r.tv.scale[0], local[1] * r.tv.scale[1], local[2] * r.tv.scale[2]}));
}
// 画面の点（u: 左 0 → 右 1、v: 上 0 → 下 1）。
Vec3 ScreenPoint(const LayoutRoom& r, double u = .5, double v = .5) {
    return TvLocal(r, {r.screenCenter[0] + (u - .5) * r.screenWidth, r.screenCenter[1] + (.5 - v) * r.screenHeight, r.screenCenter[2]});
}
Vec3 TvAxis(const LayoutRoom& r, int axis) { return Norm(Apply(TvMatrix(r), {axis == 0 ? 1.0 : 0, axis == 1 ? 1.0 : 0, axis == 2 ? 1.0 : 0})); }
LayoutBox TvBox(const LayoutRoom& r) {
    LayoutBox box{{1e9, 1e9, 1e9}, {-1e9, -1e9, -1e9}, r.objectId};
    for (const double x : {-r.body[0] / 2, r.body[0] / 2}) for (const double y : {0.0, r.body[1]}) for (const double z : {-r.body[2] / 2, r.body[2] / 2}) {
        const auto p = TvLocal(r, {x, y, z});
        for (int i = 0; i < 3; ++i) { box.lo[i] = (std::min)(box.lo[i], p[i]); box.hi[i] = (std::max)(box.hi[i], p[i]); }
    }
    return box;
}
bool Inside(const Vec3& p, const LayoutBox& box, double margin) {
    for (int i = 0; i < 3; ++i) { if (p[i] <= box.lo[i] - margin || p[i] >= box.hi[i] + margin) { return false; } }
    return true;
}

// ---- シーンの読み取り ----
Vec3 ReadVec3(const Json& value, Vec3 fallback) {
    if (!value.is_array() || value.size() != 3) { return fallback; }
    for (int i = 0; i < 3; ++i) { if (value[i].is_number()) { fallback[i] = value[i].get<double>(); } }
    return fallback;
}
LayoutTransform ReadTransform(const Json& t) {
    LayoutTransform result;
    result.position = ReadVec3(t.value("position", Json{}), {0, 0, 0});
    result.rotation = ReadVec3(t.value("rotation", t.value("rotationEulerDeg", Json{})), {0, 0, 0});
    result.scale = ReadVec3(t.value("scale", Json{}), {1, 1, 1});
    return result;
}
uint64_t ReferenceId(const Json* value) {
    if (!value) { return 0; }
    if (value->is_object()) { return ObjectId(value->value("id", Json{})); }
    return ObjectId(*value);
}
Json Describe(EditorWorkspaceHostContext& ctx, uint64_t objectId, uint64_t componentId, const std::vector<std::string>& fields, std::string& error) {
    const auto result = Execute(ctx, "component.describe", {{"objectId", objectId}, {"componentId", componentId}, {"propertyIds", fields}});
    if (!result.ok) { error = result.error; return Json{}; }
    return result.data.value("fields", Json::array());
}
double NumberField(const Json& fields, std::string_view id, double fallback) {
    const auto* v = FieldValue(fields, id); return v && v->is_number() ? v->get<double>() : fallback;
}
std::string TextField(const Json& fields, std::string_view id) {
    const auto* v = FieldValue(fields, id); return v && v->is_string() ? v->get<std::string>() : std::string{};
}
bool BoolField(const Json& fields, std::string_view id) {
    const auto* v = FieldValue(fields, id); return v && v->is_boolean() && v->get<bool>();
}
std::vector<std::string> SplitRegions(const std::string& text) {
    std::vector<std::string> result; std::stringstream stream(text); std::string item;
    while (std::getline(stream, item, ',')) {
        const auto b = item.find_first_not_of(" \t"), e = item.find_last_not_of(" \t");
        if (b != std::string::npos) { result.push_back(item.substr(b, e - b + 1)); }
    }
    return result;
}
std::string Join(const std::vector<std::string>& items) {
    std::string out;
    for (const auto& item : items) { if (!out.empty()) { out += ","; } out += item; }
    return out;
}

// ---- 経路 ----
struct Pivot { Vec3 corner{}, out{}; bool valid = false; };
// 二台のテレビの背面の線の交点を角とし、二つの法線の和を角から外への向きとする（柱の角を回る演出の中心）。
// 背面を使うのは、柱に付いたテレビでは背面の交点が柱の角に一致するため。
Pivot CornerOf(const LayoutRoom& a, const LayoutRoom& b, double y) {
    Pivot p;
    const auto back = [](const LayoutRoom& r) { return TvLocal(r, {0, r.screenCenter[1], -r.body[2] / 2}); };
    const auto pa = back(a), pb = back(b), da = Flat(TvAxis(a, 0)), db = Flat(TvAxis(b, 0));
    const double det = da[0] * (-db[2]) - da[2] * (-db[0]);
    if (std::abs(det) < .3) { return p; }
    const double rx = pb[0] - pa[0], rz = pb[2] - pa[2];
    const double s = (rx * (-db[2]) - rz * (-db[0])) / det;
    p.corner = {pa[0] + da[0] * s, y, pa[2] + da[2] * s};
    p.out = Flat(Add(TvAxis(a, 2), TvAxis(b, 2)));
    p.valid = true;
    return p;
}

double ScreenShare(const LayoutRoom& r, const TravelKey& key) {
    const auto m = LookMatrix(key.eye, key.look, key.roll);
    const Vec3 left{m[0][0], m[1][0], m[2][0]}, up{m[0][1], m[1][1], m[2][1]}, forward{m[0][2], m[1][2], m[2][2]};
    if (Dot(TvAxis(r, 2), Norm(Sub(key.eye, ScreenPoint(r)))) < .1) { return 0; }
    const double ty = std::tan(key.fov * kPi / 360), tx = ty * kNarrowAspect;
    int seen = 0;
    for (int i = 0; i < 5; ++i) for (int j = 0; j < 5; ++j) {
        const auto d = Sub(ScreenPoint(r, .1 + .2 * i, .1 + .2 * j), key.eye);
        const double z = Dot(d, forward);
        seen += z > .1 && std::abs(Dot(d, left)) < z * tx && std::abs(Dot(d, up)) < z * ty;
    }
    return seen / 25.0;
}
}

TravelKey SampleTravel(const std::vector<TravelKey>& keys, double t) {
    if (keys.empty()) { return {}; }
    if (keys.size() == 1) { return keys.front(); }
    t = std::clamp(t, keys.front().t, keys.back().t);
    size_t i = 0;
    while (i + 2 < keys.size() && keys[i + 1].t <= t) { ++i; }
    const auto& a = keys[i]; const auto& b = keys[i + 1];
    const double span = (std::max)(b.t - a.t, 1e-6), u = (t - a.t) / span, u2 = u * u, u3 = u2 * u;
    const double h00 = 2 * u3 - 3 * u2 + 1, h10 = u3 - 2 * u2 + u, h01 = -2 * u3 + 3 * u2, h11 = u3 - u2;
    // 途中のキーの速さは前後のキーの差を時刻差で割る。最初と最後は 0（エンジンの spline と同じ）。
    const auto velocity = [&](size_t k, auto get) {
        if (k == 0 || k + 1 >= keys.size()) { return 0.0; }
        return (get(keys[k + 1]) - get(keys[k - 1])) / (keys[k + 1].t - keys[k - 1].t);
    };
    const auto channel = [&](auto get) {
        return h00 * get(a) + h10 * span * velocity(i, get) + h01 * get(b) + h11 * span * velocity(i + 1, get);
    };
    TravelKey out; out.t = t;
    for (int c = 0; c < 3; ++c) {
        out.eye[c] = channel([c](const TravelKey& k) { return k.eye[c]; });
        out.look[c] = channel([c](const TravelKey& k) { return k.look[c]; });
    }
    out.fov = channel([](const TravelKey& k) { return k.fov; });
    out.roll = channel([](const TravelKey& k) { return k.roll; });
    return out;
}

const LayoutRoom* LayoutSnapshot::Room(std::string_view regionId) const {
    const auto it = std::find_if(rooms.begin(), rooms.end(), [regionId](const auto& r) { return r.regionId == regionId; });
    return it == rooms.end() ? nullptr : &*it;
}
const LayoutArea* LayoutSnapshot::AreaOf(std::string_view regionId) const {
    for (const auto& area : areas) { if (std::find(area.regions.begin(), area.regions.end(), regionId) != area.regions.end()) { return &area; } }
    return nullptr;
}

bool LoadLayout(EditorWorkspaceHostContext& ctx, LayoutSnapshot& snap, std::string& error) {
    snap = {};
    // 1. 全物体の変換（scene.layout.apply の expected にもなる）とロック状態。
    const auto layout = Execute(ctx, "scene.layout.query", Json::object());
    if (!layout.ok) { error = layout.error; return false; }
    snap.sceneId = layout.data.value("sceneId", "");
    snap.revision = layout.data.value("documentRevision", uint64_t{0});
    std::map<uint64_t, bool> locked;
    for (const auto& object : layout.data.value("objects", Json::array())) {
        const auto id = ObjectId(object.value("objectId", Json{}));
        if (!id) { continue; }
        snap.transforms[id] = ReadTransform(object.value("transform", Json::object()));
        snap.transformJson[id] = object.value("transform", Json::object());
        locked[id] = object.value("locked", false);
    }
    // 2. 全物体の要約（部品の種類と世界の箱）。区域・大区域・境界の部品を見つけ、障害物も集める。
    struct Summary { uint64_t id; std::string name; std::vector<std::pair<std::string, uint64_t>> components; bool bounds = false; Vec3 lo{}, hi{}; };
    std::vector<Summary> summaries;
    for (uint64_t offset = 0;;) {
        const auto page = Execute(ctx, "scene.objects.query", {{"offset", offset}, {"limit", 128}});
        if (!page.ok) { error = page.error; return false; }
        const auto objects = page.data.value("objects", Json::array());
        for (const auto& object : objects) {
            Summary s{ObjectId(object.value("objectId", Json{})), object.value("name", ""), {}};
            for (const auto& c : object.value("components", Json::array())) { s.components.emplace_back(c.value("type", ""), ObjectId(c.value("componentId", Json{}))); }
            if (object.contains("worldBounds") && object["worldBounds"].is_object()) {
                s.bounds = true; s.lo = ReadVec3(object["worldBounds"].value("min", Json{}), {}); s.hi = ReadVec3(object["worldBounds"].value("max", Json{}), {});
            }
            summaries.push_back(std::move(s));
        }
        if (!page.data.contains("nextOffset")) { break; }
        const auto next = ObjectId(page.data["nextOffset"]);
        if (next <= offset || objects.empty()) { break; }
        offset = next;
    }
    const auto componentOf = [](const Summary& s, std::string_view type) -> uint64_t {
        for (const auto& [t, id] : s.components) { if (t == type) { return id; } }
        return 0;
    };
    // 3. 区域（テレビ）。モデルの元パスから型を引き、画面の寸法と筐体を決める。
    std::vector<uint64_t> tvIds;
    for (const auto& s : summaries) {
        const auto componentId = componentOf(s, kRegionComponentType);
        if (!componentId) { continue; }
        const auto fields = Describe(ctx, s.id, componentId, {"regionId", "cols", "rows", "cells", "worldPlaced", "worldX", "worldY", "cameraObject",
            "cameraAuto", "screenObject", "screenCenterX", "screenCenterY", "screenCenterZ", "screenWidth", "screenHeight"}, error);
        if (!fields.is_array()) { return false; }
        LayoutRoom room;
        room.objectId = s.id; room.componentId = componentId; room.name = s.name;
        room.regionId = TextField(fields, "regionId");
        room.cols = static_cast<int>(NumberField(fields, "cols", 1)); room.rows = static_cast<int>(NumberField(fields, "rows", 1));
        room.cells = TextField(fields, "cells");
        room.placed = BoolField(fields, "worldPlaced");
        room.worldX = static_cast<int>(NumberField(fields, "worldX", 0)); room.worldY = static_cast<int>(NumberField(fields, "worldY", 0));
        room.cameraId = ReferenceId(FieldValue(fields, "cameraObject"));
        room.cameraAuto = TextField(fields, "cameraAuto");
        room.screenCenter[0] = NumberField(fields, "screenCenterX", 0); room.screenCenter[1] = NumberField(fields, "screenCenterY", .4);
        room.screenCenter[2] = NumberField(fields, "screenCenterZ", .28);
        room.screenWidth = NumberField(fields, "screenWidth", .84); room.screenHeight = NumberField(fields, "screenHeight", .49);
        const uint64_t screen = ReferenceId(FieldValue(fields, "screenObject"));
        const uint64_t tv = screen ? screen : s.id;
        if (const auto found = snap.transforms.find(tv); found != snap.transforms.end()) { room.tv = found->second; }
        if (room.cameraId) {
            if (const auto found = snap.transforms.find(room.cameraId); found != snap.transforms.end()) { room.camera = found->second; }
            room.cameraLocked = locked[room.cameraId];
        }
        tvIds.push_back(tv);
        snap.rooms.push_back(std::move(room));
    }
    if (!tvIds.empty()) {
        const auto detail = Execute(ctx, "scene.objects.query", {{"objectIds", tvIds}, {"detail", true}});
        if (!detail.ok) { error = detail.error; return false; }
        for (const auto& object : detail.data.value("objects", Json::array())) {
            const auto id = ObjectId(object.value("objectId", Json{}));
            const auto source = object.contains("model") && object["model"].is_object() ? object["model"].value("sourcePath", "") : std::string{};
            const auto* model = FindTelevisionModel(source);
            if (!model) { continue; }
            for (size_t i = 0; i < snap.rooms.size(); ++i) {
                if (tvIds[i] != id) { continue; }
                auto& r = snap.rooms[i];
                r.knownModel = true;
                r.body[0] = model->width; r.body[1] = model->height; r.body[2] = model->depth;
                r.screenCenter[0] = model->screenCenterX; r.screenCenter[1] = model->screenCenterY; r.screenCenter[2] = model->screenCenterZ;
                r.screenWidth = model->screenWidth; r.screenHeight = model->screenHeight;
            }
        }
    }
    // 4. 大区域。
    for (const auto& s : summaries) {
        const auto componentId = componentOf(s, kAreaComponent);
        if (!componentId) { continue; }
        const auto fields = Describe(ctx, s.id, componentId, {"regions", "frameFill", "orbitDegrees", "cameraLift"}, error);
        if (!fields.is_array()) { return false; }
        LayoutArea area{s.id, componentId, s.name, SplitRegions(TextField(fields, "regions"))};
        area.frameFill = NumberField(fields, "frameFill", .35); area.orbitDegrees = NumberField(fields, "orbitDegrees", 15);
        area.cameraLift = NumberField(fields, "cameraLift", .3);
        snap.areas.push_back(std::move(area));
    }
    // 5. 境界（世界地図で共有する辺）と、そこに結んだ演出。
    for (size_t i = 0; i < snap.rooms.size(); ++i) for (size_t j = i + 1; j < snap.rooms.size(); ++j) {
        const auto& a = snap.rooms[i]; const auto& b = snap.rooms[j];
        const auto edge = FindSharedWorldEdge({a.regionId, a.cols, a.rows, a.placed, a.worldX, a.worldY}, {b.regionId, b.cols, b.rows, b.placed, b.worldX, b.worldY});
        if (!edge) { continue; }
        LayoutEdge e; e.a = a.regionId; e.b = b.regionId; e.edge = *edge;
        const auto* areaA = snap.AreaOf(a.regionId); const auto* areaB = snap.AreaOf(b.regionId);
        e.between = !areaA || !areaB || areaA != areaB;
        // 共有する辺の上で、両側のマスがどちらも空いている所があれば越えられる（壁で隔てた辺には演出を作らない）。
        const auto open = [](const LayoutRoom& r, int x, int y) {
            return x >= 0 && y >= 0 && x < r.cols && y < r.rows && r.cells.size() == static_cast<size_t>(r.cols * r.rows) &&
                   r.cells[static_cast<size_t>(y * r.cols + x)] != '1';
        };
        const bool horizontal = edge->sideA == WorldSide::Left || edge->sideA == WorldSide::Right;
        for (int64_t w = edge->overlapBegin; w < edge->overlapEnd && !e.passable; ++w) {
            if (horizontal) {
                const int ax = edge->sideA == WorldSide::Right ? a.cols - 1 : 0, bx = edge->sideB == WorldSide::Right ? b.cols - 1 : 0;
                e.passable = open(a, ax, static_cast<int>(w - a.worldY)) && open(b, bx, static_cast<int>(w - b.worldY));
            } else {
                const int ay = edge->sideA == WorldSide::Bottom ? a.rows - 1 : 0, by = edge->sideB == WorldSide::Bottom ? b.rows - 1 : 0;
                e.passable = open(a, static_cast<int>(w - a.worldX), ay) && open(b, static_cast<int>(w - b.worldX), by);
            }
        }
        snap.edges.push_back(std::move(e));
    }
    for (const auto& s : summaries) {
        const auto componentId = componentOf(s, kRegionSequenceComponentType);
        if (!componentId) { continue; }
        const auto fields = Describe(ctx, s.id, componentId, {"regionA", "regionB", "sequenceAssetGuid", "speed", "style", "seconds", "keepAuthored", "autoHash"}, error);
        if (!fields.is_array()) { return false; }
        const auto ra = TextField(fields, "regionA"), rb = TextField(fields, "regionB");
        for (auto& e : snap.edges) {
            if (!((e.a == ra && e.b == rb) || (e.a == rb && e.b == ra))) { continue; }
            // 演出は regionA → regionB の向きに作る（逆へ越える時は逆再生する）。
            if (e.a != ra) { std::swap(e.a, e.b); std::swap(e.edge.sideA, e.edge.sideB); }
            e.bindingObject = s.id; e.bindingComponent = componentId;
            e.sequenceGuid = TextField(fields, "sequenceAssetGuid");
            e.speed = NumberField(fields, "speed", 1);
            e.style = TextField(fields, "style"); if (e.style.empty()) { e.style = "auto"; }
            e.seconds = NumberField(fields, "seconds", e.speed > 0 ? 1 / e.speed : 3);
            e.keepAuthored = BoolField(fields, "keepAuthored");
            e.autoHash = TextField(fields, "autoHash");
        }
    }
    // 6. 障害物とホール。テレビは別に扱う（自分の画面の検査で自分の箱を除くため）。床のような広く薄い物はホールの範囲に、
    // 鎖や配線のような細い物と、天井・屋根は数えない。
    std::set<uint64_t> tvSet(tvIds.begin(), tvIds.end());
    bool floor = false; Vec3 floorLo{1e9, 0, 1e9}, floorHi{-1e9, 0, -1e9}; double ceiling = 10;
    for (const auto& s : summaries) {
        if (!s.bounds || tvSet.contains(s.id)) { continue; }
        const double sx = s.hi[0] - s.lo[0], sy = s.hi[1] - s.lo[1], sz = s.hi[2] - s.lo[2];
        if (sx * sz > 12 && sy < 1.5 && s.hi[1] < 1.0) {
            floor = true;
            floorLo[0] = (std::min)(floorLo[0], s.lo[0]); floorLo[2] = (std::min)(floorLo[2], s.lo[2]);
            floorHi[0] = (std::max)(floorHi[0], s.hi[0]); floorHi[2] = (std::max)(floorHi[2], s.hi[2]);
            continue;
        }
        if (sx * sz > 12 && s.lo[1] > 6) { ceiling = (std::min)(ceiling, s.lo[1]); continue; }
        if ((std::min)(sx, sz) < .06 || sy > 12 || sx * sz > 40) { continue; }
        snap.obstacles.push_back({s.lo, s.hi, s.id});
    }
    if (floor) { snap.hasHall = true; snap.hallLo = {floorLo[0] + .6, .25, floorLo[2] + .6}; snap.hallHi = {floorHi[0] - .6, ceiling - .3, floorHi[2] - .6}; }
    return true;
}

// ---- 区域カメラ ----
namespace {
// 出口の側（画面の右 = +1）。大区域の外へ横に歩いて出る辺の向き。無ければ大区域全体の出口の向き。
double ExitSide(const LayoutSnapshot& snap, const LayoutRoom& room) {
    const auto sideOf = [&](const LayoutRoom& r) {
        double side = 0;
        for (const auto& e : snap.edges) {
            if (!e.between || !e.passable) { continue; }
            const bool isA = e.a == r.regionId, isB = e.b == r.regionId;
            if (!isA && !isB) { continue; }
            const auto mySide = isA ? e.edge.sideA : e.edge.sideB;
            side += mySide == WorldSide::Right ? 1 : mySide == WorldSide::Left ? -1 : 0;
        }
        return side;
    };
    if (const double own = sideOf(room); own != 0) { return own > 0 ? 1 : -1; }
    double flow = 0;
    if (const auto* area = snap.AreaOf(room.regionId)) {
        for (const auto& id : area->regions) { if (const auto* r = snap.Room(id)) { flow += sideOf(*r); } }
    }
    return flow > 0 ? 1 : flow < 0 ? -1 : 0;
}
}

CameraPose RegionCamera(const LayoutSnapshot& snap, const LayoutRoom& room) {
    const auto* area = snap.AreaOf(room.regionId);
    const double fill = area ? area->frameFill : .35, orbit = area ? area->orbitDegrees : 15, lift = area ? area->cameraLift : .3;
    const double side = ExitSide(snap, room);
    const auto center = ScreenPoint(room);
    const auto normal = TvAxis(room, 2), right = TvAxis(room, 0), up = TvAxis(room, 1);
    const double dist = room.screenHeight * room.tv.scale[1] / ((std::max)(fill, .05) * 2 * std::tan(kFov * kPi / 360));
    // 床に立つテレビは目の高さを下げて正面から。それ以外は少し上から見下ろす（落ちる先のテレビが画の下に覗く）。
    const bool floorStanding = room.tv.position[1] < .3;
    const double sy = floorStanding ? .12 : lift, ly = floorStanding ? .05 : -.6 * lift;
    const double a = orbit * side * kPi / 180;
    const auto direction = Add({normal[0] * std::cos(a), normal[1] * std::cos(a), normal[2] * std::cos(a)}, right, std::sin(a));
    CameraPose pose;
    pose.eye = Add(Add(Add(center, direction, dist), right, .12 * side), up, sy);
    pose.look = Add(Add(center, right, .12 * side), up, ly);
    return pose;
}

CameraPose CurrentCameraPose(const LayoutRoom& room) {
    const auto forward = Norm(Apply(Rotation(room.camera.rotation), {0, 0, 1}));
    const double depth = (std::max)(.5, Dot(Sub(ScreenPoint(room), room.camera.position), forward));
    return {room.camera.position, Add(room.camera.position, forward, depth)};
}

std::string CameraMemo(const LayoutTransform& t) {
    char buffer[160];
    std::snprintf(buffer, sizeof(buffer), "%.3f %.3f %.3f %.2f %.2f %.2f", t.position[0], t.position[1], t.position[2],
                  t.rotation[0], t.rotation[1], t.rotation[2]);
    return buffer;
}
bool CameraEdited(const LayoutRoom& room) {
    // 記録が空なら作者が置いたカメラ。記録と今の姿勢が違えば、自動で置いた後に作者が動かした。
    return room.cameraAuto.empty() || room.cameraAuto != CameraMemo(room.camera);
}

// ---- 切り替えの経路 ----
const std::vector<TravelStyle>& TravelStyles() {
    static const std::vector<TravelStyle> styles{
        {"auto", "自动", "按两台电视的朝向与高度自动选择"},
        {"reveal", "穿过揭示", "擦过出口电视，升高俯瞰目标一侧，再降下"},
        {"arc", "外弧俯瞰", "绕角外侧升高，从上方看两面"},
        {"low", "贴地仰视", "沉到地面绕角，仰望柱角"},
        {"pull", "大幅后拉", "大幅后退，用长焦看角，再绕入"},
        {"hug", "贴角倾斜", "贴着角快速转过，画面向转弯方向倾斜"},
        {"direct", "直接平移", "简单的弧线移动"},
    };
    return styles;
}

std::string ResolveStyle(const LayoutSnapshot& snap, const LayoutEdge& edge) {
    if (edge.style != "auto" && !edge.style.empty()) { return edge.style; }
    const auto* a = snap.Room(edge.a); const auto* b = snap.Room(edge.b);
    if (!a || !b) { return "direct"; }
    const double turn = std::acos(std::clamp(Dot(Flat(TvAxis(*a, 2)), Flat(TvAxis(*b, 2))), -1.0, 1.0)) * 180 / kPi;
    if (turn < 45) { return "reveal"; }
    const double y = (ScreenPoint(*a)[1] + ScreenPoint(*b)[1]) / 2;
    if (y < 1.2) { return "low"; }
    return y > 3.5 ? "arc" : "pull";
}

namespace {
std::vector<TravelKey> StyleKeys(const LayoutSnapshot& snap, const std::string& style, const LayoutRoom& a, const LayoutRoom& b,
                                 const CameraPose& ca, const CameraPose& cb, double spread, double swing) {
    const auto& ea = ca.eye; const auto& la = ca.look; const auto& eb = cb.eye; const auto& lb = cb.look;
    const auto na = TvAxis(a, 2), nb = TvAxis(b, 2);
    const auto sa = ScreenPoint(a), sb = ScreenPoint(b);
    const auto toward = Flat(Sub(sb, sa));
    const double y = (sa[1] + sb[1]) / 2;
    const TravelKey start{0, ea, la, kFov, 0}, end{1, eb, lb, kFov, 0};
    (void)snap;
    if (style == "reveal") {
        // 出口のテレビの脇をかすめて前へ出て、高所から目標側のテレビの並びを見渡し、目標へ降りる。
        const auto k1 = Add(Add(Add(ea, toward, 1.5 * spread), kUp, .35), Flat(Sub(sb, ea)), .8);
        const auto overlook = Add(sb, kUp, -1.0);
        const auto k2 = Add(Add(Add(sb, nb, 6.4 * spread), kUp, 1.6 + .3 * (spread - 1)), TvAxis(b, 0), .3);
        const auto k3 = Add(Lerp(k2, eb, .7), kUp, .25);
        return {start, {.24, k1, Lerp(ScreenPoint(a, 1.15, .45), sb, .35), 27, 0}, {.52, k2, overlook, 31, 0},
                {.8, k3, Lerp(overlook, lb, .75), 27.5, 0}, end};
    }
    const auto pivot = CornerOf(a, b, y);
    if (!pivot.valid || style == "direct") {
        // 二台がほぼ同じ向き（角が無い）：少し引いて昇る弧で移る。
        const auto mid = Add(Add(Lerp(ea, eb, .5), Flat(Add(na, nb)), .8 * spread), kUp, .35);
        return {start, {.5, mid, Lerp(la, lb, .5), 28, 0}, end};
    }
    const auto& c = pivot.corner; const auto& out = pivot.out;
    if (style == "arc") {
        // 右上へ昇りながら角の外へ回り、角と両面のテレビを上から見せてから降りる。
        const auto k1 = Add(Add(Add(ea, toward, 1.1), kUp, .55), na, .4);
        const auto k2 = Add(Add(c, out, 5.4 * spread), kUp, 1.5);
        const auto k3 = Add(Lerp(k2, eb, .65), kUp, .3);
        return {start, {.26, k1, Lerp(la, c, .3), 27, 0}, {.55, k2, Add(c, kUp, -.6), 30, 0}, {.8, k3, Lerp(c, lb, .75), 27.5, 0}, end};
    }
    if (style == "low") {
        // 床の高さまで沈んで角を回り、そびえる角を見上げてから目標へ上がる。
        auto k1 = Add(ea, toward, .9); k1[1] = .45;
        auto k2 = Add({c[0], 1.0, c[2]}, out, 3.3 * spread); k2[1] = .4;
        const auto k3 = Lerp(k2, eb, .65);
        const Vec3 corner{c[0], 1.0, c[2]};
        return {start, {.28, k1, Add(Lerp(la, corner, .35), kUp, .25), 28, 0}, {.58, k2, Add(corner, kUp, .9), 32, 0},
                {.82, k3, Lerp(Add(corner, kUp, .6), lb, .8), 28, 0}, end};
    }
    if (style == "pull") {
        // 真横へ大きく下がり、望遠気味に角を遠くから見せて、目標の面へ回り込む。
        const auto k1 = Add(Add(Add(ea, na, 1.0), kUp, .9), toward, .8);
        const auto k2 = Add(Add(c, RotateY(out, swing), 6.2 * spread), kUp, 1.0);
        const auto k3 = Add(Lerp(k2, eb, .65), kUp, .2);
        return {start, {.28, k1, Lerp(la, Add(sa, kUp, -.3), .3), 25, 0}, {.56, k2, Add(c, kUp, -.3), 23, 0},
                {.82, k3, Lerp(c, lb, .8), 25, 0}, end};
    }
    // hug：角に寄り添って回り、曲がる向きへ画を傾ける。角の真横では画角を広げて両面のテレビを一度に収める。
    const double r = 3.0 * spread;
    // 出発の面の側へ回した向き（法線 a に近い方）を見つける。
    const double toA = Dot(RotateY(out, -28), Flat(na)) >= Dot(RotateY(out, 28), Flat(na)) ? -1 : 1;
    const auto k1 = Add(Add(c, RotateY(out, 28 * toA), r), kUp, .25);
    const auto km = Add(Add(c, out, r * 1.05), kUp, .3);
    const auto k2 = Add(Add(c, RotateY(out, -28 * toA), r), kUp, .15);
    const double lean = toA;   // 曲がる向きへ傾ける
    return {start, {.3, k1, Lerp(ScreenPoint(a, .8, .5), c, .3), 29, -3.5 * lean}, {.5, km, Add(c, kUp, .05), 35, -1.5 * lean},
            {.7, k2, Lerp(c, ScreenPoint(b, .2, .5), .7), 29, 2.5 * lean}, end};
}
}

TravelPlan PlanTravel(const LayoutSnapshot& snap, const LayoutEdge& edge, const std::map<std::string, CameraPose, std::less<>>& cameras) {
    TravelPlan plan;
    const auto* a = snap.Room(edge.a); const auto* b = snap.Room(edge.b);
    const auto ca = cameras.find(edge.a), cb = cameras.find(edge.b);
    if (!a || !b || ca == cameras.end() || cb == cameras.end()) { plan.problem = "缺少区域或机位"; return plan; }
    plan.style = ResolveStyle(snap, edge);
    std::vector<LayoutBox> boxes = snap.obstacles;
    for (const auto& room : snap.rooms) { boxes.push_back(TvBox(room)); }
    const auto evaluate = [&](const std::vector<TravelKey>& keys, std::string& problem) {
        double worst = 1;
        for (int i = 0; i <= 60; ++i) {
            const auto k = SampleTravel(keys, i / 60.0);
            if (problem.empty() && snap.hasHall) {
                const LayoutBox hall{snap.hallLo, snap.hallHi, 0};
                if (!Inside(k.eye, hall, 0)) { problem = "镜头会穿出大厅"; }
            }
            if (problem.empty()) {
                for (const auto& box : boxes) { if (Inside(k.eye, box, .12)) { problem = "镜头会穿过物体 " + std::to_string(box.objectId); break; } }
            }
            worst = (std::min)(worst, (std::max)(ScreenShare(*a, k), ScreenShare(*b, k)));
        }
        if (problem.empty() && worst < kMinShare) { problem = "途中两台屏幕都离开画面"; }
        return worst;
    };
    // 何かを通り抜けるか画面が外れるなら、少しずつ外へ広げる（引きの時は横へ振る向きも試す）。
    std::string lastProblem;
    for (const double spread : {1.0, 1.1, 1.2, 1.32, 1.45, 1.6}) {
        for (const double swing : plan.style == "pull" ? std::vector<double>{-20, 20, 0} : std::vector<double>{0}) {
            auto keys = StyleKeys(snap, plan.style, *a, *b, ca->second, cb->second, spread, swing);
            std::string problem;
            const double worst = evaluate(keys, problem);
            plan.keys = keys; plan.worstShare = worst; lastProblem = problem;
            if (problem.empty()) { plan.problem.clear(); return plan; }
        }
    }
    plan.problem = lastProblem;
    return plan;
}

// ---- 書き込み ----
namespace {
// 演出のキーの記録（キーごとに時刻・画角・参照・位置 3・回転 4 を小数 4 桁で並べた文字列）。自動で書いた直後のキーを
// 境界の autoHash 欄に残し、次の生成の前に今のキーと比べて、作者が手を入れたかを見分ける。
// 丸めの境目で一致しなくならないよう、hash ではなく値を残して誤差を許して比べる（KeysMatch）。
std::vector<double> KeyNumbers(const Json& keys) {
    std::vector<double> numbers;
    const auto number = [](const Json& value) { return value.is_number() ? value.get<double>() : 0.0; };
    for (const auto& key : keys) {
        if (!key.is_object()) { continue; }
        numbers.push_back(number(key.value("time", Json(0.0)))); numbers.push_back(number(key.value("fov", Json(0.0))));
        numbers.push_back(number(key.value("reference", Json(0.0))));
        const auto position = key.value("position", Json::array()), rotation = key.value("rotation", Json::array());
        for (size_t i = 0; i < 3; ++i) { numbers.push_back(i < position.size() ? number(position[i]) : 0.0); }
        for (size_t i = 0; i < 4; ++i) { numbers.push_back(i < rotation.size() ? number(rotation[i]) : 0.0); }
    }
    return numbers;
}
constexpr size_t kKeyNumbers = 10;   // キー一つの数（時刻・画角・参照・位置 3・回転 4）
std::string KeySignature(const Json& keys) {
    std::string out;
    const auto numbers = KeyNumbers(keys);
    for (size_t i = 0; i < numbers.size(); ++i) {
        char buffer[32]; std::snprintf(buffer, sizeof(buffer), "%.4f", numbers[i]);
        if (i) { out += (i % kKeyNumbers) ? "," : ";"; }
        out += buffer;
    }
    return out;
}
// 記録と今のキーが同じか（個数が同じで、どの値も 0.002 以内。四元数の符号の反転は同じ回転とみなす）。
// 記録が空か読めない時は false を返し、呼び出し側は作者の演出として残す。
bool KeysMatch(const std::string& recorded, const Json& keys) {
    std::vector<double> stored;
    for (size_t begin = 0; begin < recorded.size();) {
        const auto end = recorded.find_first_of(",;", begin);
        const auto item = recorded.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
        char* tail = nullptr;
        const double value = std::strtod(item.c_str(), &tail);
        if (item.empty() || !tail || *tail != '\0') { return false; }
        stored.push_back(value);
        if (end == std::string::npos) { break; }
        begin = end + 1;
    }
    const auto current = KeyNumbers(keys);
    if (stored.empty() || stored.size() != current.size() || current.size() % kKeyNumbers) { return false; }
    constexpr double kTolerance = .002;
    for (size_t k = 0; k < current.size(); k += kKeyNumbers) {
        for (size_t i = 0; i < 6; ++i) { if (std::abs(stored[k + i] - current[k + i]) > kTolerance) { return false; } }
        bool same = true, flipped = true;
        for (size_t i = 6; i < kKeyNumbers; ++i) {
            same = same && std::abs(stored[k + i] - current[k + i]) <= kTolerance;
            flipped = flipped && std::abs(stored[k + i] + current[k + i]) <= kTolerance;
        }
        if (!same && !flipped) { return false; }
    }
    return true;
}
Json SequenceKeys(const Json& sequence) {
    try { return sequence.at("tracks").at(0).at("clips").at(0).at("camera").at("keys"); }
    catch (...) { return Json::array(); }
}
Json BuildSequence(const LayoutEdge& edge, const TravelPlan& plan, uint64_t cameraA, uint64_t cameraB, const std::string& name) {
    const auto key = [](const TravelKey& k, int reference) {
        const auto q = QuatFrom(LookMatrix(k.eye, k.look, k.roll));
        return Json{{"customTangents", 0}, {"far", 500.0}, {"fov", reference ? 0.0 : Round(k.fov, 1e-3)}, {"inFovTangent", 0.0},
                    {"inTangent", {0.0, 0.0, 0.0}}, {"interpolation", "spline"}, {"near", .05}, {"outFovTangent", 0.0},
                    {"outTangent", {0.0, 0.0, 0.0}},
                    {"position", reference ? Json{0.0, 0.0, 0.0} : Json{Round(k.eye[0]), Round(k.eye[1]), Round(k.eye[2])}},
                    {"reference", reference},
                    {"rotation", reference ? Json{0.0, 0.0, 0.0, 1.0} : Json{Round(q[0], 1e-6), Round(q[1], 1e-6), Round(q[2], 1e-6), Round(q[3], 1e-6)}},
                    {"time", k.t}};
    };
    Json keys = Json::array();
    for (size_t i = 0; i < plan.keys.size(); ++i) {
        keys.push_back(key(plan.keys[i], i == 0 ? 2 : i + 1 == plan.keys.size() ? 3 : 0));
    }
    Json base = key(plan.keys.front(), 0); base.erase("time"); base["fov"] = kFov;
    (void)edge;
    return Json{
        {"bindings", {{{"id", 1}, {"name", "游戏摄像机"}, {"required", true}, {"slotName", "Camera"}, {"targetKind", "slot"}},
                      {{"id", 2}, {"name", "区域机位 起点"}, {"required", true}, {"sceneObjectId", cameraA}, {"targetKind", "sceneObject"}},
                      {{"id", 3}, {"name", "区域机位 终点"}, {"required", true}, {"sceneObjectId", cameraB}, {"targetKind", "sceneObject"}}}},
        {"durationSeconds", 1.0}, {"id", 1}, {"name", name}, {"screenEffects", Json::array()},
        {"tracks", {{{"clips", {{{"camera", {{"aimWeight", {{"keys", Json::array()}, {"value", 1.0}}}, {"anchor", 0}, {"base", base}, {"keys", keys},
                                            {"lookAt", 0}, {"motion", Json::array()}, {"name", "Shot"}, {"pivotDistance", 5.0},
                                            {"rail", {{"loop", false}, {"points", Json::array()}}}, {"railT", {{"keys", Json::array()}, {"value", 0.0}}},
                                            {"targetOffset", {0.0, 0.0, 0.0}}, {"timing", {{"keys", Json::array()}, {"value", 0.0}}}, {"useScenePose", false}}},
                                {"cameraBindingId", 1}, {"durationSeconds", 1.0}, {"effects", Json::array()}, {"id", 1}, {"sourceOffsetSeconds", 0.0},
                                {"startTimeSeconds", 0.0}, {"transition", {{"durationSeconds", 0.0}, {"mode", "cut"}}}}}},
                     {"enabled", true}, {"id", 1}, {"type", "cameraCut"}}}}};
}
// 開いた演出の今のキー（作者が直したかを見るため）。
bool ReadSequenceKeys(EditorWorkspaceHostContext& ctx, const std::string& guid, Json& keys, std::string& error) {
    const auto opened = Execute(ctx, "authoring.session.open", {{"workspace", "cinematics"}, {"assetGuid", guid}});
    if (!opened.ok) { error = opened.error; return false; }
    const auto document = Execute(ctx, "cinematics.document", {{"sessionId", opened.data.value("sessionId", "")}});
    if (!document.ok) { error = document.error; return false; }
    keys = SequenceKeys(document.data.value("document", Json::object()));
    return true;
}
bool ApplyTransforms(EditorWorkspaceHostContext& ctx, LayoutSnapshot& snap, const Json& commands, std::string& error) {
    if (commands.empty()) { return true; }
    const auto applied = Execute(ctx, "scene.layout.apply", {{"sceneId", snap.sceneId}, {"documentRevision", snap.revision}, {"commands", commands}});
    if (!applied.ok) { error = applied.error; return false; }
    snap.revision = applied.data.value("documentRevision", snap.revision);
    return true;
}
Json TransformJson(const LayoutTransform& t) {
    return {{"position", {t.position[0], t.position[1], t.position[2]}}, {"rotation", {t.rotation[0], t.rotation[1], t.rotation[2]}},
            {"scale", {t.scale[0], t.scale[1], t.scale[2]}}};
}
bool InScope(const LayoutSnapshot& snap, std::string_view regionId, std::string_view areaName) {
    if (areaName.empty()) { return true; }
    const auto* area = snap.AreaOf(regionId);
    return area && area->name == areaName;
}
}

bool GenerateCameras(EditorWorkspaceHostContext& ctx, LayoutSnapshot& snap, std::string_view areaName, std::string_view onlyRegion,
                     bool force, GenerateReport& report, std::string& error) {
    // 1. カメラの無い区域にはカメラを作る（自動のカメラとして以後は作り直せる）。
    bool created = false;
    for (auto& room : snap.rooms) {
        if (!room.placed || room.cameraId || !InScope(snap, room.regionId, areaName) || (!onlyRegion.empty() && room.regionId != onlyRegion)) { continue; }
        const auto object = Execute(ctx, "scene.create_empty_object", {{"name", "区域机位 " + room.regionId}});
        if (!object.ok) { error = object.error; return false; }
        const auto id = ObjectId(object.data.value("objectId", Json{}));
        const auto added = Execute(ctx, "component.add", {{"objectId", id}, {"componentType", "CameraComponent"}});
        if (!added.ok) { error = added.error; return false; }
        const auto bound = Execute(ctx, "authoring.transaction", {{"commands", {{{"objectId", room.objectId}, {"componentId", room.componentId},
                                                                                 {"propertyId", "cameraObject"}, {"value", id}}}}});
        if (!bound.ok) { error = bound.error; return false; }
        report.notes.push_back(room.regionId + "：新建机位");
        created = true;
    }
    if (created && !LoadLayout(ctx, snap, error)) { return false; }
    // 2. 自動のカメラだけを今のテレビの配置から置き直す。作者がロック・調整したカメラは残す（force の時だけ上書き）。
    Json commands = Json::array(), memos = Json::array();
    for (auto& room : snap.rooms) {
        if (!room.placed || !room.cameraId || !InScope(snap, room.regionId, areaName) || (!onlyRegion.empty() && room.regionId != onlyRegion)) { continue; }
        if (!force && (room.cameraLocked || CameraEdited(room))) { ++report.kept; continue; }
        const auto pose = RegionCamera(snap, room);
        LayoutTransform next = room.camera;
        next.position = {Round(pose.eye[0]), Round(pose.eye[1]), Round(pose.eye[2])};
        const auto euler = EulerFrom(LookMatrix(pose.eye, pose.look));
        next.rotation = {Round(euler[0], 1e-3), Round(euler[1], 1e-3), Round(euler[2], 1e-3)};
        const auto expected = snap.transformJson.find(room.cameraId);
        if (expected == snap.transformJson.end()) { continue; }
        commands.push_back({{"objectId", room.cameraId}, {"expected", expected->second},
                            {"position", TransformJson(next)["position"]}, {"rotation", TransformJson(next)["rotation"]}});
        room.camera = next;
        memos.push_back({{"objectId", room.objectId}, {"componentId", room.componentId}, {"propertyId", "cameraAuto"}, {"value", CameraMemo(next)}});
        ++report.written;
    }
    if (!ApplyTransforms(ctx, snap, commands, error)) { return false; }
    if (!memos.empty()) {
        const auto written = Execute(ctx, "authoring.transaction", {{"commands", memos}});
        if (!written.ok) { error = written.error; return false; }
    }
    return LoadLayout(ctx, snap, error);
}

bool GenerateTravels(EditorWorkspaceHostContext& ctx, LayoutSnapshot& snap, std::string_view areaName, const LayoutEdge* onlyEdge,
                     bool force, GenerateReport& report, std::string& error) {
    std::map<std::string, CameraPose, std::less<>> cameras;
    for (const auto& room : snap.rooms) { if (room.cameraId) { cameras[room.regionId] = CurrentCameraPose(room); } }
    const auto edges = snap.edges;
    for (auto edge : edges) {
        if (!edge.between || !edge.passable) { continue; }
        if (onlyEdge && !((onlyEdge->a == edge.a && onlyEdge->b == edge.b) || (onlyEdge->a == edge.b && onlyEdge->b == edge.a))) { continue; }
        // 画面で選んだ辺の設定（様式・秒数）は、まだ記録が無い辺でもここで使う。
        if (onlyEdge) { edge.style = onlyEdge->style; edge.seconds = onlyEdge->seconds; edge.keepAuthored = onlyEdge->keepAuthored; }
        if (!onlyEdge && !areaName.empty() && !InScope(snap, edge.a, areaName) && !InScope(snap, edge.b, areaName)) { continue; }
        // 縦の辺（落下・跳躍）は大区域の外でも自動で流れるので、手動の演出は横の辺だけに作る。
        const auto* a = snap.Room(edge.a); const auto* b = snap.Room(edge.b);
        if (!a || !b || !a->cameraId || !b->cameraId) { report.notes.push_back(edge.a + " ↔ " + edge.b + "：缺少机位"); ++report.failed; continue; }
        // 作者が直した演出は残す（保留、または自動の記録と今のキーが違う）。記録が無い・読めない演出も、
        // 作者の物として残す（上書きは自動に戻す時だけ）。
        if (!force && !edge.sequenceGuid.empty()) {
            if (edge.keepAuthored) { ++report.kept; continue; }
            Json keys; std::string readError;
            if (!ReadSequenceKeys(ctx, edge.sequenceGuid, keys, readError)) {
                report.notes.push_back(edge.a + " ↔ " + edge.b + "：读取演出失败，保留（" + readError + "）"); ++report.kept; continue;
            }
            if (edge.autoHash.empty()) {
                report.notes.push_back(edge.a + " ↔ " + edge.b + "：没有自动生成记录，按手做演出保留"); ++report.kept; continue;
            }
            if (!KeysMatch(edge.autoHash, keys)) {
                report.notes.push_back(edge.a + " ↔ " + edge.b + "：已手调，保留"); ++report.kept; continue;
            }
        }
        const auto plan = PlanTravel(snap, edge, cameras);
        if (!plan.problem.empty()) { report.notes.push_back(edge.a + " ↔ " + edge.b + "：" + plan.problem); ++report.failed; continue; }
        const auto name = edge.a + " ↔ " + edge.b;
        const auto sequence = BuildSequence(edge, plan, a->cameraId, b->cameraId, name);
        // 演出資産：既存なら開いて中身を差し替え、無ければ新しく作る（どちらもエンジンの演出ワークスペースで保存する）。
        std::string guid = edge.sequenceGuid;
        const auto opened = guid.empty() ? Execute(ctx, "cinematics.new_asset", Json::object()) : Execute(ctx, "cinematics.open_asset", {{"assetGuid", guid}});
        if (!opened.ok) {
            error = opened.error.find("PENDING_CONFIRMATION") != std::string::npos ? "演出工作区有未保存的修改，请先保存或撤销后再生成" : opened.error;
            return false;
        }
        const auto replaced = Execute(ctx, "cinematics.edit", {{"command", {{"action", "replace_sequence"}, {"value", sequence}}}});
        if (!replaced.ok) { error = replaced.error; return false; }
        const auto saved = Execute(ctx, "cinematics.save", Json::object());
        if (!saved.ok) { error = saved.error; return false; }
        if (guid.empty()) {
            const auto named = Execute(ctx, "cinematics.edit", {{"command", {{"action", "rename"}, {"value", name}}}});
            if (named.ok) { guid = named.data.value("assetGuid", ""); (void)Execute(ctx, "cinematics.save", Json::object()); }
            if (guid.empty()) { error = "新演出已保存，但未取得资产 GUID"; return false; }
        }
        Json keys; std::string readError;
        const auto hash = ReadSequenceKeys(ctx, guid, keys, readError) ? KeySignature(keys) : KeySignature(SequenceKeys(sequence));
        // 境界の記録：無ければ作り、演出・速さ・自動の記録を書く。
        uint64_t object = edge.bindingObject, component = edge.bindingComponent;
        if (!object) {
            const auto created = Execute(ctx, "scene.create_empty_object", {{"name", "切换 " + name}});
            if (!created.ok) { error = created.error; return false; }
            object = ObjectId(created.data.value("objectId", Json{}));
            const auto added = Execute(ctx, "component.add", {{"objectId", object}, {"componentType", kRegionSequenceComponentType}});
            if (!added.ok) { error = added.error; return false; }
            for (const auto& c : added.data.value("addedComponents", Json::array())) {
                if (c.value("type", "") == kRegionSequenceComponentType) { component = ObjectId(c.value("componentId", Json{})); }
            }
        }
        const double seconds = std::clamp(edge.seconds, .4, 10.0);
        const auto set = [&](std::string_view id, const Json& value) { return Json{{"objectId", object}, {"componentId", component}, {"propertyId", id}, {"value", value}}; };
        Json commands = Json::array({set("regionA", edge.a), set("regionB", edge.b), set("sequenceAssetGuid", guid), set("sequenceSlotName", "Camera"),
                                     set("speed", Round(1 / seconds, 1e-4)), set("seconds", seconds), set("autoHash", hash)});
        commands.push_back(set("style", edge.style.empty() ? std::string("auto") : edge.style));
        commands.push_back(set("keepAuthored", edge.keepAuthored));
        // 実行用カメラ（演出の Camera スロット）は既存の境界と同じゲームカメラを使う。
        for (const auto& other : snap.edges) {
            if (other.bindingObject && other.bindingObject != object) {
                std::string ignored;
                const auto fields = Describe(ctx, other.bindingObject, other.bindingComponent, {"cameraObject"}, ignored);
                if (const auto id = ReferenceId(FieldValue(fields, "cameraObject"))) { commands.push_back(set("cameraObject", id)); break; }
            }
        }
        const auto written = Execute(ctx, "authoring.transaction", {{"commands", commands}});
        if (!written.ok) { error = written.error; return false; }
        report.notes.push_back(name + "：" + plan.style);
        ++report.written;
    }
    return LoadLayout(ctx, snap, error);
}

bool SyncScreenGeometry(EditorWorkspaceHostContext& ctx, const LayoutSnapshot& snap, GenerateReport& report, std::string& error) {
    Json commands = Json::array();
    for (const auto& room : snap.rooms) {
        if (!room.knownModel) { continue; }
        const auto set = [&](std::string_view id, double value) {
            commands.push_back({{"objectId", room.objectId}, {"componentId", room.componentId}, {"propertyId", id}, {"value", value}});
        };
        set("screenCenterX", room.screenCenter[0]); set("screenCenterY", room.screenCenter[1]); set("screenCenterZ", room.screenCenter[2]);
        set("screenWidth", room.screenWidth); set("screenHeight", room.screenHeight);
        ++report.written;
    }
    if (commands.empty()) { return true; }
    const auto written = Execute(ctx, "authoring.transaction", {{"commands", commands}});
    if (!written.ok) { error = written.error; return false; }
    return true;
}

bool WriteAreaRegions(EditorWorkspaceHostContext& ctx, const LayoutArea& area, const std::vector<std::string>& regions, std::string& error) {
    const auto written = Execute(ctx, "authoring.transaction", {{"commands", {{{"objectId", area.objectId}, {"componentId", area.componentId},
                                                                              {"propertyId", "regions"}, {"value", Join(regions)}}}}});
    if (!written.ok) { error = written.error; return false; }
    return true;
}

bool CreateArea(EditorWorkspaceHostContext& ctx, const std::string& name, const std::vector<std::string>& regions, std::string& error) {
    const auto created = Execute(ctx, "scene.create_empty_object", {{"name", name}});
    if (!created.ok) { error = created.error; return false; }
    const auto id = ObjectId(created.data.value("objectId", Json{}));
    const auto added = Execute(ctx, "component.add", {{"objectId", id}, {"componentType", kAreaComponent}});
    if (!added.ok) { error = added.error; return false; }
    uint64_t component = 0;
    for (const auto& c : added.data.value("addedComponents", Json::array())) { if (c.value("type", "") == kAreaComponent) { component = ObjectId(c.value("componentId", Json{})); } }
    if (!component) { error = "镜头大区域组件创建失败"; return false; }
    return WriteAreaRegions(ctx, {id, component, name, {}}, regions, error);
}

bool PruneInAreaBindings(EditorWorkspaceHostContext& ctx, const LayoutSnapshot& snap, int& removed, std::string& error) {
    removed = 0;
    for (const auto& edge : snap.edges) {
        if ((edge.between && edge.passable) || !edge.bindingObject) { continue; }   // 大区域の中や壁で隔てた辺の結びは使われない
        const auto deleted = Execute(ctx, "scene.delete_object", {{"objectId", edge.bindingObject}});
        if (!deleted.ok) { error = deleted.error; return false; }
        ++removed;
    }
    return true;
}

bool WriteEdgeSettings(EditorWorkspaceHostContext& ctx, const LayoutEdge& edge, std::string& error) {
    if (!edge.bindingObject) { return true; }   // まだ演出の無い辺は、生成する時に設定ごと書く
    const double seconds = std::clamp(edge.seconds, .4, 10.0);
    const auto set = [&](std::string_view id, const Json& value) { return Json{{"objectId", edge.bindingObject}, {"componentId", edge.bindingComponent}, {"propertyId", id}, {"value", value}}; };
    const auto written = Execute(ctx, "authoring.transaction", {{"commands", {set("style", edge.style), set("seconds", seconds),
                                                                              set("speed", Round(1 / seconds, 1e-4)), set("keepAuthored", edge.keepAuthored)}}});
    if (!written.ok) { error = written.error; return false; }
    return true;
}

bool EdgeTrialSpawn(const LayoutSnapshot& snap, const LayoutEdge& edge, bool fromA, double& x, double& y, uint64_t& roomObject) {
    const auto* room = snap.Room(fromA ? edge.a : edge.b);
    if (!room || room->cells.size() != static_cast<size_t>(room->cols * room->rows)) { return false; }
    const auto side = fromA ? edge.edge.sideA : edge.edge.sideB;
    const auto solid = [&](int cx, int cy) { return cx < 0 || cy < 0 || cx >= room->cols || cy >= room->rows || room->cells[static_cast<size_t>(cy * room->cols + cx)] == '1'; };
    roomObject = room->objectId;
    if (side == WorldSide::Left || side == WorldSide::Right) {
        // 共有する行のうち、境界から 3〜6 マス内側で、足元が埋まった空きマス（歩いて境界を越えられる所）。
        for (int inset = 3; inset <= 6; ++inset) {
            const int cx = side == WorldSide::Right ? room->cols - 1 - inset : inset;
            for (int wy = static_cast<int>(edge.edge.overlapBegin); wy < static_cast<int>(edge.edge.overlapEnd); ++wy) {
                const int cy = wy - room->worldY;
                if (!solid(cx, cy) && solid(cx, cy + 1)) { x = cx + .1875; y = cy + .125; return true; }
            }
        }
        return false;
    }
    // 上下の辺：共有する列のうち、境界の 2 マス手前の空きマス（空中から落ちる・跳び上がる）。
    for (int wx = static_cast<int>(edge.edge.overlapBegin); wx < static_cast<int>(edge.edge.overlapEnd); ++wx) {
        const int cx = wx - room->worldX;
        const int cy = side == WorldSide::Bottom ? room->rows - 3 : 2;
        if (!solid(cx, cy)) { x = cx + .1875; y = cy + .125; return true; }
    }
    return false;
}
}
