#include "TD2Recipes.h"
#include "TD2DeviceEditor.h"
#include "../Common/TD2Math.h"
#include <algorithm>
#include <cmath>

namespace TD2::Editor {
namespace {
using Math::Vec3;

Pose PoseOf(const LayoutTransform& t) { Pose p; p.position = t.position; p.rotation = t.rotation; p.scale = t.scale; return p; }
Pose Offset(const Pose& base, double right, double up, double forward) {
    Pose p = base;
    const auto r = PoseAxis(base, {1, 0, 0}), u = PoseAxis(base, {0, 1, 0}), f = PoseAxis(base, {0, 0, 1});
    for (size_t i = 0; i < 3; ++i) { p.position[i] = std::round((base.position[i] + r[i] * right + u[i] * up + f[i] * forward) * 10000) / 10000; }
    return p;
}

// 状態（1 = B、-1 = C）での地図の位置。錨があれば錨の区域から、park はどこにも置かない。
std::vector<RegionPlacement> PlacementsAt(const Machine& m, const LayoutSnapshot& layout, int state) {
    std::vector<RegionPlacement> out;
    for (const auto& room : layout.rooms) { out.push_back(PlacementOf(room)); }
    const auto& offsets = state < 0 ? m.offsetsC : m.offsets;
    for (const auto& o : offsets) {
        const auto self = std::find_if(out.begin(), out.end(), [&](const auto& p) { return p.regionId == o.regionId; });
        const auto* base = layout.Room(o.anchor.empty() ? o.regionId : o.anchor);
        if (self == out.end() || !base) { continue; }
        if (o.park) { self->worldPlaced = false; continue; }
        self->worldPlaced = true; self->worldX = base->worldX + o.dx; self->worldY = base->worldY + o.dy;
    }
    return out;
}

const MachineObjectDelta* FindDelta(const std::vector<MachineObjectDelta>& deltas, uint64_t object) {
    const auto it = std::find_if(deltas.begin(), deltas.end(), [object](const auto& d) { return d.object == object; });
    return it == deltas.end() ? nullptr : &*it;
}

bool Managed(const Machine& m, const LayoutSnapshot& layout, uint64_t object) {
    for (const auto* list : {&m.offsets, &m.offsetsC}) for (const auto& o : *list) {
        if (const auto* room = layout.Room(o.regionId); room && room->tvObject == object) { return true; }
    }
    return std::any_of(m.parts.begin(), m.parts.end(), [object](const auto& p) { return p.object == object && p.moves; });
}

double TopOf(const LayoutRoom& room) { return room.tv.position[1] + room.body[1] * std::abs(room.tv.scale[1]); }
double Width(const LayoutRoom& room) { return room.body[0] * std::abs(room.tv.scale[0]); }
double Height(const LayoutRoom& room) { return room.body[1] * std::abs(room.tv.scale[1]); }

// テレビの物体のずれ（なければ空）。
std::optional<MachineObjectDelta> TvDelta(const Machine& m, const LayoutSnapshot& layout, const std::string& region, int state) {
    const auto* room = layout.Room(region);
    if (!room) { return std::nullopt; }
    const auto* d = FindDelta(state < 0 ? m.deltasC : m.deltas, room->tvObject);
    return d ? std::optional<MachineObjectDelta>(*d) : std::nullopt;
}

// 状態での吊り台の上がり（m）。卷扬箱の高さに使う。
double Rise(const Machine& m, const LayoutSnapshot& layout, const std::string& region) {
    double rise = 0;
    for (const int state : {1, -1}) { if (const auto d = TvDelta(m, layout, region, state)) { rise = (std::max)(rise, d->position[1]); } }
    return rise;
}
}

const std::vector<RecipeInfo>& RecipeList() {
    static const std::vector<RecipeInfo> recipes{
        {RecipeKind::Lift, "lift", "升降台", 1},
        {RecipeKind::Turn, "turn", "转台", 1},
        {RecipeKind::Balance, "balance", "滑轮天平", 2},
        {RecipeKind::Shuttle, "shuttle", "往复", 1},
        {RecipeKind::Flip, "flip", "倒挂翻转", 1},
        {RecipeKind::Slide, "slide", "滑块货架", 1},
        {RecipeKind::Stack, "stack", "电视塔", 1},
        {RecipeKind::Carousel, "carousel", "转柱", 1},
    };
    return recipes;
}

RecipeKind RecipeOf(const Machine& m) {
    switch (m.kind) {
    case MachineKind::Slide: return RecipeKind::Slide;
    case MachineKind::Stack: return RecipeKind::Stack;
    case MachineKind::Carousel: return RecipeKind::Carousel;
    case MachineKind::Mechanism: break;
    }
    for (const auto& r : RecipeList()) { if (r.id == m.recipe) { return r.kind; } }
    return RecipeKind::Custom;
}

std::string RecipeLabel(const Machine& m) {
    const auto kind = RecipeOf(m);
    for (const auto& r : RecipeList()) { if (r.kind == kind) { return std::string(r.label); } }
    return "机构";
}

std::string PlateTargetFor(const Machine& m, const std::string& plateRegion, const std::string& plateId, const std::string& devicesText) {
    switch (m.kind) {
    case MachineKind::Mechanism:
        // 天平：踏んだテレビが沈む側（状態 B で下がるなら mech/ID、上がるなら mech/ID/r）。
        if (m.balance) {
            for (const auto& o : m.offsets) { if (o.regionId == plateRegion) { return o.dy < 0 ? "mech/" + m.id + "/r" : "mech/" + m.id; } }
        }
        return "mech/" + m.id;
    case MachineKind::Slide: {
        const bool member = std::find(m.members.begin(), m.members.end(), plateRegion) != m.members.end();
        return m.members.empty() ? std::string{} : "slide/" + (member ? plateRegion : m.members.front());
    }
    case MachineKind::Stack: {
        DeviceEditor devices; devices.Load(devicesText);
        for (const auto& d : devices.Items()) { if (d.id == plateId && d.Extra("variant") == "reset") { return "stack/" + m.id + "/reset"; } }
        return "stack/" + m.id + "/pull";
    }
    case MachineKind::Carousel: return "carousel/" + m.id + "/cw";
    }
    return {};
}

void InitRecipe(Machine& m, RecipeKind kind, const std::vector<std::string>& regions, const LayoutSnapshot& layout) {
    if (regions.empty()) { return; }
    for (const auto& r : RecipeList()) { if (r.kind == kind) { m.recipe = std::string(r.id); } }
    const auto* first = layout.Room(regions.front());
    const int rows = first ? first->rows : 6;
    switch (kind) {
    case RecipeKind::Lift:
        m.seconds = 1.6;
        m.offsets = {{regions.front(), {}, 0, -(std::max)(3, rows / 2)}};
        break;
    case RecipeKind::Turn:
        m.seconds = 1.4;
        m.offsets = {{regions.front(), {}, 0, 0}};
        break;
    case RecipeKind::Balance:
        if (regions.size() < 2) { return; }
        m.balance = true; m.settle = .6; m.seconds = 1.2;
        m.offsets = {{regions[0], {}, 0, 3}, {regions[1], {}, 0, -3}};
        m.offsetsC = {{regions[0], {}, 0, -3}, {regions[1], {}, 0, 3}};
        break;
    case RecipeKind::Shuttle: {
        m.seconds = 1.4; m.dwell = 2.5;
        MachineRegionOffset o; o.regionId = regions.front(); o.park = true;
        m.offsets = {o};
        // テレビが載っている台（推车など）があれば一緒に動かす。
        if (first) {
            for (const auto& box : layout.obstacles) {
                const auto& p = first->tv.position;
                if (std::abs(box.hi[1] - p[1]) < .08 && p[0] > box.lo[0] && p[0] < box.hi[0] && p[2] > box.lo[2] && p[2] < box.hi[2]) {
                    m.parts.push_back({box.objectId, regions.front(), "support", true, false});
                    break;
                }
            }
        }
        break;
    }
    case RecipeKind::Flip: {
        m.seconds = 1.3; m.toggle = true;
        MachineRegionOffset o; o.regionId = regions.front(); o.flip = true;
        m.offsets = {o};
        break;
    }
    default: break;
    }
}

void RebuildMotion(Machine& m, const LayoutSnapshot& layout) {
    const auto kind = RecipeOf(m);
    if (m.kind != MachineKind::Mechanism || kind == RecipeKind::Custom) { return; }
    double balanceAmount = -1;   // 天平は状態 B で決めた量を状態 C にも使う（両側が同じだけ動く）
    for (const int state : {1, -1}) {
        if (state < 0 && !m.balance) { m.deltasC.clear(); continue; }
        const auto& offsets = state < 0 ? m.offsetsC : m.offsets;
        auto& deltas = state < 0 ? m.deltasC : m.deltas;
        const auto old = deltas;
        // 配方が受け持たない物体（作者が足したもの）の行は残す。
        std::vector<MachineObjectDelta> next;
        for (const auto& d : old) { if (!Managed(m, layout, d.object)) { next.push_back(d); } }
        // 隣に揃える時の相手：この状態で動かないテレビ。
        const auto placementsB = PlacementsAt(m, layout, state);
        std::vector<std::pair<ScreenBox, RegionPlacement>> neighbors;
        for (const auto& room : layout.rooms) {
            if (!room.placed || std::any_of(offsets.begin(), offsets.end(), [&](const auto& o) { return o.regionId == room.regionId; })) { continue; }
            neighbors.push_back({BoxOf(room), PlacementOf(room)});
        }
        struct Item { const LayoutRoom* room; MachineObjectDelta delta; bool aligned; int dy; };
        std::vector<Item> items;
        for (const auto& o : offsets) {
            const auto* room = layout.Room(o.regionId);
            if (!room || !room->tvObject) { continue; }
            const auto* previous = FindDelta(old, room->tvObject);
            MachineObjectDelta d; bool aligned = false;
            const auto a = PlacementOf(*room);
            const auto b = std::find_if(placementsB.begin(), placementsB.end(), [&](const auto& p) { return p.regionId == o.regionId; });
            switch (kind) {
            case RecipeKind::Lift: case RecipeKind::Balance: case RecipeKind::Shuttle:
                if (o.park) {
                    // 世界から離れる時：今のずれを残す（初めは吊り台は上へ、往復は後ろへ）。
                    if (previous) { d = *previous; }
                    else if (kind == RecipeKind::Shuttle) { d.position = {0, 0, -2.4}; }
                    else { d.position = {0, 1.6, 0}; }
                    d.rotation = {0, 0, 0}; d.hasPivot = false;
                } else if (b != placementsB.end()) { d = MoveDelta(BoxOf(*room), a, *b, neighbors, &aligned); }
                break;
            case RecipeKind::Turn:
                d.rotation = {0, previous && std::abs(previous->rotation[1]) > 1 ? previous->rotation[1] : 180.0, 0};
                break;
            case RecipeKind::Flip: {
                const double s = std::abs(room->tv.scale[0]);
                d.rotation = {0, 0, 180}; d.hasPivot = true;
                d.pivot = {std::round(room->screenCenter[0] * s * 1000) / 1000, std::round(room->screenCenter[1] * std::abs(room->tv.scale[1]) * 1000) / 1000, 0};
                break;
            }
            default: break;
            }
            d.object = room->tvObject; d.anchor = 0;
            items.push_back({room, d, aligned, o.dy});
        }
        // 天平は二台が同じだけ逆に動く（隣に揃えられた方の量を使う）。
        if (kind == RecipeKind::Balance && items.size() == 2) {
            double amount = balanceAmount;
            if (amount < 0) {
                bool found = false;
                for (const auto& item : items) { if (item.aligned) { amount = std::abs(item.delta.position[1]); found = true; break; } }
                if (!found) { amount = std::abs(items[0].delta.position[1]); }
                balanceAmount = amount;
            }
            for (auto& item : items) { item.delta.position = {0, item.dy > 0 ? -amount : amount, 0}; }
        }
        for (const auto& item : items) {
            next.push_back(item.delta);
            const Pose tvPose = PoseOf(item.room->tv);
            for (const auto& part : m.parts) {
                if (!part.moves || part.region != item.room->regionId) { continue; }
                const auto t = layout.transforms.find(part.object);
                if (t == layout.transforms.end()) { continue; }
                MachineObjectDelta pd;
                if (kind == RecipeKind::Turn || kind == RecipeKind::Flip) { pd = item.delta; pd.hasPivot = false; pd.pivot = {}; pd.position = {}; }
                else { pd = RetargetMove(item.delta, tvPose, PoseOf(t->second)); }
                pd.object = part.object; pd.anchor = 0;
                next.push_back(pd);
            }
        }
        deltas = next;
    }
}

std::vector<RecipePart> PlanParts(const Machine& m, const LayoutSnapshot& layout) {
    std::vector<RecipePart> out;
    const auto kind = RecipeOf(m);
    const auto has = [&](const std::string& region, std::string_view role) {
        return std::any_of(m.parts.begin(), m.parts.end(), [&](const auto& p) { return p.region == region && p.role == role; });
    };
    const auto prop = [](std::string_view label) -> const StageProp* {
        for (const auto& p : kStageProps) { if (p.label == label) { return &p; } }
        return nullptr;
    };
    const auto add = [&](std::string_view label, const std::string& region, std::string_view role, Pose pose, bool moves) {
        if (const auto* p = prop(label)) { out.push_back({std::string(label), std::string(p->path), region, std::string(role), pose, moves}); }
    };
    if (kind == RecipeKind::Lift) {
        for (const auto& o : m.offsets) {
            const auto* room = layout.Room(o.regionId);
            if (!room || has(o.regionId, "chain")) { continue; }
            const Pose tv = PoseOf(room->tv);
            const double top = TopOf(*room), w = Width(*room);
            const double box = top + Rise(m, layout, o.regionId) + .9;
            for (const double side : {-1.0, 1.0}) {
                auto chain = Offset(tv, side * w / 3, Height(*room), 0); chain.scale = {1, std::round((box + .75 - top) * 1000) / 1000, 1};
                add("吊链", o.regionId, "chain", chain, true);
            }
            auto winch = Offset(tv, 0, box - tv.position[1], 0); winch.scale = {std::round(.45 * std::clamp(w / 1.08, .5, 2.0) * 1000) / 1000, 1.2, 1};
            add("卷扬箱", o.regionId, "box", winch, false);
        }
    }
    if (kind == RecipeKind::Balance && m.offsets.size() == 2) {
        const auto* a = layout.Room(m.offsets[0].regionId); const auto* b = layout.Room(m.offsets[1].regionId);
        if (a && b && !has(a->regionId, "beam")) {
            const double beamY = (std::max)(TopOf(*a) + Rise(m, layout, a->regionId), TopOf(*b) + Rise(m, layout, b->regionId)) + .6;
            for (const auto* room : {a, b}) {
                const Pose tv = PoseOf(room->tv);
                for (const double side : {-1.0, 1.0}) {
                    auto chain = Offset(tv, side * Width(*room) / 3, Height(*room), 0); chain.scale = {1, std::round((beamY + .2 - TopOf(*room)) * 1000) / 1000, 1};
                    add("吊链", room->regionId, "chain", chain, true);
                }
            }
            Pose beam = PoseOf(a->tv);
            beam.position = {(a->tv.position[0] + b->tv.position[0]) * .5, beamY, (a->tv.position[2] + b->tv.position[2]) * .5};
            const double span = std::hypot(a->tv.position[0] - b->tv.position[0], a->tv.position[2] - b->tv.position[2]) + .6;
            beam.scale = {std::round(span / 1.94 * 1000) / 1000, .42, .9};
            add("天平梁", a->regionId, "beam", beam, false);
        }
    }
    for (const auto& o : m.offsets) {
        const auto* room = layout.Room(o.regionId);
        if (!room) { continue; }
        const Pose tv = PoseOf(room->tv);
        const double k = std::clamp(Width(*room) / 1.1, .5, 1.3);
        if (kind == RecipeKind::Turn && !has(o.regionId, "plate")) {
            auto base = tv; base.position[1] = tv.position[1] - .195 * k; base.rotation = {0, tv.rotation[1], 0}; base.scale = {k, k, k};
            auto plate = base; plate.position[1] = tv.position[1] - .035 * k;
            add("转台底座", o.regionId, "base", base, false);
            add("转台转盘", o.regionId, "plate", plate, true);
        }
        if (kind == RecipeKind::Shuttle && !has(o.regionId, "rail")) {
            auto rail = tv; rail.position[1] = 0; rail.rotation = {0, tv.rotation[1], 0}; rail.scale = {1, 1, 1};
            add("滑轨", o.regionId, "rail", rail, false);
        }
        if (kind == RecipeKind::Flip && !has(o.regionId, "disk")) {
            const double s = std::abs(room->tv.scale[1]), depth = room->body[2] * std::abs(room->tv.scale[2]);
            const double q = std::clamp(Height(*room) / 1.0, .6, 1.4);
            auto disk = Offset(tv, room->screenCenter[0] * s, room->screenCenter[1] * s, -(depth * .5 + .06)); disk.scale = {q, q, q};
            auto mount = Offset(disk, 0, .15 * q - 3.3 * q, -.17 * q); mount.scale = {q, q, q};
            add("倒立转盘", o.regionId, "disk", disk, true);
            add("倒立支柱", o.regionId, "mount", mount, false);
        }
    }
    return out;
}

std::vector<std::pair<uint64_t, Pose>> AdjustParts(const Machine& m, const LayoutSnapshot& layout) {
    std::vector<std::pair<uint64_t, Pose>> out;
    const auto kind = RecipeOf(m);
    if (kind != RecipeKind::Lift && kind != RecipeKind::Balance) { return out; }
    const auto pose = [&](uint64_t id) -> std::optional<Pose> {
        const auto t = layout.transforms.find(id);
        return t == layout.transforms.end() ? std::nullopt : std::optional<Pose>(PoseOf(t->second));
    };
    // 卷扬箱・天平梁は、テレビが一番上がった時の上端より上に保つ。鎖はその高さまで伸ばす。
    double required = 0;
    for (const auto& o : m.offsets) {
        if (const auto* room = layout.Room(o.regionId)) { required = (std::max)(required, TopOf(*room) + Rise(m, layout, o.regionId) + (kind == RecipeKind::Lift ? .9 : .6)); }
    }
    for (const auto& part : m.parts) {
        if (part.role != "box" && part.role != "beam") { continue; }
        auto p = pose(part.object);
        if (!p) { continue; }
        double y = p->position[1];
        if (kind == RecipeKind::Lift) {
            const auto* room = layout.Room(part.region);
            if (!room) { continue; }
            const double need = TopOf(*room) + Rise(m, layout, part.region) + .9;
            if (y < need - .01) { y = need; auto moved = *p; moved.position[1] = std::round(y * 1000) / 1000; out.push_back({part.object, moved}); }
            for (const auto& chain : m.parts) {
                if (chain.role != "chain" || chain.region != part.region) { continue; }
                auto c = pose(chain.object);
                if (!c) { continue; }
                const double length = std::round((y + .75 - c->position[1]) * 1000) / 1000;
                if (std::abs(length - c->scale[1]) > .01) { c->scale[1] = length; out.push_back({chain.object, *c}); }
            }
        } else {
            if (y < required - .01) { y = required; auto moved = *p; moved.position[1] = std::round(y * 1000) / 1000; out.push_back({part.object, moved}); }
            for (const auto& chain : m.parts) {
                if (chain.role != "chain") { continue; }
                auto c = pose(chain.object);
                if (!c) { continue; }
                const double length = std::round((y + .2 - c->position[1]) * 1000) / 1000;
                if (std::abs(length - c->scale[1]) > .01) { c->scale[1] = length; out.push_back({chain.object, *c}); }
            }
        }
    }
    return out;
}

double RecipeAngle(const Machine& m, const LayoutSnapshot& layout) {
    if (m.offsets.empty()) { return 180; }
    const auto d = TvDelta(m, layout, m.offsets.front().regionId, 1);
    return d && std::abs(d->rotation[1]) > 1 ? d->rotation[1] : 180;
}

void SetRecipeAngle(Machine& m, const LayoutSnapshot& layout, double degrees) {
    for (const auto& o : m.offsets) {
        const auto* room = layout.Room(o.regionId);
        if (!room) { continue; }
        for (auto& d : m.deltas) {
            const bool part = std::any_of(m.parts.begin(), m.parts.end(), [&](const auto& p) { return p.object == d.object && p.moves && p.region == o.regionId; });
            if (d.object == room->tvObject || part) { d.rotation[1] = degrees; }
        }
    }
}

double RecipeDistance(const Machine& m, const LayoutSnapshot& layout) {
    if (m.offsets.empty()) { return 0; }
    const auto d = TvDelta(m, layout, m.offsets.front().regionId, 1);
    if (!d) { return 0; }
    return RecipeOf(m) == RecipeKind::Shuttle ? -d->position[2] : d->position[1];
}

void SetRecipeDistance(Machine& m, const LayoutSnapshot& layout, double meters) {
    const bool shuttle = RecipeOf(m) == RecipeKind::Shuttle;
    for (const auto& o : m.offsets) {
        const auto* room = layout.Room(o.regionId);
        if (!room || !o.park) { continue; }
        for (auto& d : m.deltas) {
            if (d.object != room->tvObject) { continue; }
            if (shuttle) { d.position = {0, 0, -meters}; } else { d.position = {0, meters, 0}; }
        }
    }
    // 一緒に動く部品は作り直しでテレビに合わせる。
    RebuildMotion(m, layout);
}
}
