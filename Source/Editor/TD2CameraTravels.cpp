// 大区域の間の切り替え演出：様式ごとのキー、画に入る画面の検査、演出の資産の生成と作者の手直しの見分け。
#include "TD2CameraLayoutShared.h"

namespace TD2::Editor {
using namespace LayoutDetail;

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

// ---- 切り替えの経路 ----
const std::vector<TravelStyle>& TravelStyles() {
    static const std::vector<TravelStyle> styles{
        {"auto", "自動", "二台のテレビの向きと高さで選ぶ"},
        {"reveal", "すり抜け", "出口のテレビをかすめ、上がって行き先を見下ろしてから下りる"},
        {"arc", "外回り", "角の外を上がり、上から両面を見る"},
        {"low", "低い回り込み", "床まで下りて角を回り、柱の角を見上げる"},
        {"pull", "大きく引く", "大きく下がり、望遠で角を見てから回り込む"},
        {"hug", "角すれすれ", "角すれすれを速く回り、曲がる方へ傾く"},
        {"direct", "まっすぐ", "単純な弧で動く"},
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
    if (!a || !b || ca == cameras.end() || cb == cameras.end()) { plan.problem = "区域かカメラがありません"; return plan; }
    plan.style = ResolveStyle(snap, edge);
    std::vector<LayoutBox> boxes = snap.obstacles;
    for (const auto& room : snap.rooms) { boxes.push_back(TvBox(room)); }
    const auto evaluate = [&](const std::vector<TravelKey>& keys, std::string& problem) {
        double worst = 1;
        for (int i = 0; i <= 60; ++i) {
            const auto k = SampleTravel(keys, i / 60.0);
            if (problem.empty() && snap.hasHall) {
                const LayoutBox hall{snap.hallLo, snap.hallHi, 0};
                if (!Inside(k.eye, hall, 0)) { problem = "カメラが大広間の外へ出ます"; }
            }
            if (problem.empty()) {
                for (const auto& box : boxes) { if (Inside(k.eye, box, .12)) { problem = "カメラが物体を通り抜けます " + std::to_string(box.objectId); break; } }
            }
            worst = (std::min)(worst, (std::max)(ScreenShare(*a, k), ScreenShare(*b, k)));
        }
        if (problem.empty() && worst < kMinShare) { problem = "途中で両方の画面が画から外れます"; }
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
        {"bindings", {{{"id", 1}, {"name", "ゲームカメラ"}, {"required", true}, {"slotName", "Camera"}, {"targetKind", "slot"}},
                      {{"id", 2}, {"name", "区域カメラ 始点"}, {"required", true}, {"sceneObjectId", cameraA}, {"targetKind", "sceneObject"}},
                      {{"id", 3}, {"name", "区域カメラ 終点"}, {"required", true}, {"sceneObjectId", cameraB}, {"targetKind", "sceneObject"}}}},
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
        if (onlyEdge) { edge.style = onlyEdge->style; edge.seconds = onlyEdge->seconds; edge.keepAuthored = onlyEdge->keepAuthored; edge.autoPlay = onlyEdge->autoPlay; }
        if (!onlyEdge && !areaName.empty() && !InScope(snap, edge.a, areaName) && !InScope(snap, edge.b, areaName)) { continue; }
        // 縦の辺（落下・跳躍）は大区域の外でも自動で流れるので、手動の演出は横の辺だけに作る。
        const auto* a = snap.Room(edge.a); const auto* b = snap.Room(edge.b);
        if (!a || !b || !a->cameraId || !b->cameraId) { report.notes.push_back(edge.a + " ↔ " + edge.b + "：カメラがありません"); ++report.failed; continue; }
        // 作者が直した演出は残す（保留、または自動の記録と今のキーが違う）。記録が無い・読めない演出も、
        // 作者の物として残す（上書きは自動に戻す時だけ）。
        if (!force && !edge.sequenceGuid.empty()) {
            if (edge.keepAuthored) { ++report.kept; continue; }
            Json keys; std::string readError;
            if (!ReadSequenceKeys(ctx, edge.sequenceGuid, keys, readError)) {
                report.notes.push_back(edge.a + " ↔ " + edge.b + "：演出を読めないので残しました（" + readError + "）"); ++report.kept; continue;
            }
            if (edge.autoHash.empty()) {
                report.notes.push_back(edge.a + " ↔ " + edge.b + "：自動生成の記録が無いので、手作りとして残しました"); ++report.kept; continue;
            }
            if (!KeysMatch(edge.autoHash, keys)) {
                report.notes.push_back(edge.a + " ↔ " + edge.b + "：手直し済みなので残しました"); ++report.kept; continue;
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
            error = opened.error.find("PENDING_CONFIRMATION") != std::string::npos ? "演出ワークスペースに保存していない変更があります。保存か元に戻してから作ってください" : opened.error;
            return false;
        }
        const auto replaced = Execute(ctx, "cinematics.edit", {{"command", {{"action", "replace_sequence"}, {"value", sequence}}}});
        if (!replaced.ok) { error = replaced.error; return false; }
        const auto saved = Execute(ctx, "cinematics.save", Json::object());
        if (!saved.ok) { error = saved.error; return false; }
        if (guid.empty()) {
            const auto named = Execute(ctx, "cinematics.edit", {{"command", {{"action", "rename"}, {"value", name}}}});
            if (named.ok) { guid = named.data.value("assetGuid", ""); (void)Execute(ctx, "cinematics.save", Json::object()); }
            if (guid.empty()) { error = "新しい演出を保存しましたが、資産の GUID を取れません"; return false; }
        }
        Json keys; std::string readError;
        const auto hash = ReadSequenceKeys(ctx, guid, keys, readError) ? KeySignature(keys) : KeySignature(SequenceKeys(sequence));
        // 境界の記録：無ければ作り、演出・速さ・自動の記録を書く。
        uint64_t object = edge.bindingObject, component = edge.bindingComponent;
        if (!object) {
            const auto created = Execute(ctx, "scene.create_empty_object", {{"name", "切り替え " + name}});
            if (!created.ok) { error = created.error; return false; }
            object = ObjectId(created.data.value("objectId", Json{}));
            const auto added = Execute(ctx, "component.add", {{"objectId", object}, {"componentType", kRegionSequenceComponentType}});
            if (!added.ok) { error = added.error; return false; }
            for (const auto& c : added.data.value("addedComponents", Json::array())) {
                if (c.value("type", "") == kRegionSequenceComponentType) { component = ObjectId(c.value("componentId", Json{})); }
            }
        }
        const double seconds = std::clamp(edge.seconds, .4, 10.0);
        const auto set = [&](std::string_view id, const Json& value) { return PropertyCommand(object, component, id, value); };
        Json commands = Json::array({set("regionA", edge.a), set("regionB", edge.b), set("sequenceAssetGuid", guid), set("sequenceSlotName", "Camera"),
                                     set("speed", Round(1 / seconds, 1e-4)), set("seconds", seconds), set("autoHash", hash)});
        commands.push_back(set("style", edge.style.empty() ? std::string("auto") : edge.style));
        commands.push_back(set("keepAuthored", edge.keepAuthored));
        commands.push_back(set("autoPlay", edge.autoPlay));
        // 実行用カメラ（演出の Camera スロット）は既存の境界と同じゲームカメラを使う。
        for (const auto& other : snap.edges) {
            if (other.bindingObject && other.bindingObject != object) {
                std::string ignored;
                const auto fields = Describe(ctx, other.bindingObject, other.bindingComponent, {"cameraObject"}, ignored);
                if (const auto id = ReferenceId(FieldValue(fields, "cameraObject"))) { commands.push_back(set("cameraObject", id)); break; }
            }
        }
        if (!Transact(ctx, commands, error)) { return false; }
        report.notes.push_back(name + "：" + plan.style);
        ++report.written;
    }
    return LoadLayout(ctx, snap, error);
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
    const auto set = [&](std::string_view id, const Json& value) { return PropertyCommand(edge.bindingObject, edge.bindingComponent, id, value); };
    return Transact(ctx, Json::array({set("style", edge.style), set("seconds", seconds), set("speed", Round(1 / seconds, 1e-4)),
                                      set("keepAuthored", edge.keepAuthored), set("autoPlay", edge.autoPlay)}), error);
}
}
