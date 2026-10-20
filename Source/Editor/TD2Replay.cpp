#include "TD2Replay.h"
#include "../Runtime/TD2RegionAuthoring.h"
#include <algorithm>
#include <cmath>

namespace TD2::Editor {
namespace {
RegionPlacement PlaceOf(const RegionSettings& s) { return {s.region.regionId, s.region.cols, s.region.rows, s.worldPlaced, s.worldX, s.worldY}; }
std::string Seconds(double t) { return std::to_string(static_cast<int>(std::floor(t))) + " 秒"; }
}

std::filesystem::path SolutionFolder(std::string_view projectRoot) {
    return std::filesystem::path(std::u8string(projectRoot.begin(), projectRoot.end())) / "ProjectSettings" / "TD2Solutions";
}

std::string BuildExploration(const std::vector<ReplayRoom>& rooms, const std::vector<Machine>& machines, const PlayRecord& record, Rules::Exploration& out) {
    std::vector<Rules::RegionSource> sources;
    for (const auto& room : rooms) {
        Rules::RegionSource source;
        source.placement = PlaceOf(room.settings); source.entry = room.settings.entryRegion;
        source.definition.region = room.settings.region;
        source.definition.spawn = {room.settings.spawnX, room.settings.spawnY};
        std::string error;
        if (!ParseDevices(room.settings.devices, source.definition.devices, error) && source.placement.worldPlaced) { return room.name + "：" + error; }
        if (const auto problem = Rules::ValidateWorld(source.definition); !problem.empty() && source.placement.worldPlaced) { return room.name + "：" + problem; }
        sources.push_back(std::move(source));
    }
    // 実行時と同じく、どことも重ならない預け場所を一つずつ配る。
    int parked = 0;
    const auto park = [&parked] { const int k = parked++; return std::pair{-100000 - 128 * k, -100000}; };
    const auto find = [&](const std::string& id) -> Rules::RegionSource* {
        const auto it = std::find_if(sources.begin(), sources.end(), [&](const auto& s) { return s.placement.regionId == id; });
        return it == sources.end() ? nullptr : &*it;
    };
    std::vector<Rules::MechanismSource> mechanisms;
    std::vector<Rules::SlideSource> slides;
    std::vector<Rules::StackSource> stacks;
    std::vector<Rules::CarouselSource> carousels;
    for (const auto& m : machines) {
        if (m.kind != MachineKind::Carousel) { continue; }
        Rules::CarouselSource c; c.id = m.id; c.seconds = m.seconds; c.faces = m.members;
        bool ok = c.faces.size() >= 2;
        for (const auto& face : c.faces) {
            auto* region = find(face);
            if (!region) { ok = false; break; }
            if (!region->placement.worldPlaced) { const auto [x, y] = park(); region->placement.worldPlaced = true; region->placement.worldX = x; region->placement.worldY = y; }
            c.stations.push_back({region->placement.worldX, region->placement.worldY});
        }
        if (ok) { carousels.push_back(std::move(c)); }
    }
    for (const auto& m : machines) {
        if (m.kind == MachineKind::Mechanism) {
            Rules::MechanismSource s;
            s.id = m.id; s.toggle = m.toggle; s.balance = m.balance; s.seconds = m.seconds; s.dwell = m.dwell; s.settle = m.settle;
            bool ok = true;
            for (const auto& [offsets, moves] : {std::pair{&m.offsets, &s.moves}, std::pair{&m.offsetsC, &s.movesC}}) {
                for (const auto& o : *offsets) {
                    if (o.park) { const auto [x, y] = park(); moves->push_back({o.regionId, x, y, o.flip}); continue; }
                    const auto* base = find(o.anchor.empty() ? o.regionId : o.anchor);
                    if (!base || !base->placement.worldPlaced) { ok = false; break; }
                    moves->push_back({o.regionId, base->placement.worldX + o.dx, base->placement.worldY + o.dy, o.flip});
                }
            }
            if (ok) { mechanisms.push_back(std::move(s)); }
        } else if (m.kind == MachineKind::Slide) {
            // スロットはメンバーの配置と空きスロット（一番目からのずれ）。列と行は配置の X・Y の順位。
            Rules::SlideSource s; s.id = m.id; s.seconds = m.seconds; s.members = m.members;
            std::vector<std::pair<int, int>> places;
            bool ok = !m.members.empty();
            for (const auto& id : m.members) { const auto* r = find(id); if (!r || !r->placement.worldPlaced) { ok = false; break; } places.push_back({r->placement.worldX, r->placement.worldY}); }
            if (!ok) { continue; }
            for (const auto& [ex, ey] : m.emptySlots) { places.push_back({places.front().first + ex, places.front().second + ey}); }
            std::vector<int> xs, ys;
            for (const auto& [x, y] : places) { xs.push_back(x); ys.push_back(y); }
            std::sort(xs.begin(), xs.end()); xs.erase(std::unique(xs.begin(), xs.end()), xs.end());
            std::sort(ys.begin(), ys.end()); ys.erase(std::unique(ys.begin(), ys.end()), ys.end());
            for (const auto& [x, y] : places) {
                s.slots.push_back({x, y, static_cast<int>(std::lower_bound(xs.begin(), xs.end(), x) - xs.begin()), static_cast<int>(std::lower_bound(ys.begin(), ys.end(), y) - ys.begin())});
            }
            slides.push_back(std::move(s));
        } else if (m.kind == MachineKind::Stack) {
            Rules::StackSource s; s.id = m.id; s.pullSeconds = m.pullSeconds; s.dropSeconds = m.dropSeconds; s.resetSeconds = m.resetSeconds; s.members = m.members;
            bool ok = !m.members.empty();
            for (const auto& id : m.members) { const auto* r = find(id); if (!r || !r->placement.worldPlaced) { ok = false; break; } s.slots.push_back({r->placement.worldX, r->placement.worldY}); }
            if (!ok) { continue; }
            for (size_t i = 0; i < m.dockDistances.size(); ++i) { s.docks.push_back(park()); }
            stacks.push_back(std::move(s));
        }
    }
    std::optional<Rules::Vec2> spawn;
    if (record.spawn) { spawn = Rules::Vec2{record.spawn->first, record.spawn->second}; }
    const auto error = out.Initialize(std::move(sources), std::move(mechanisms), std::move(slides), std::move(stacks), std::move(carousels), record.entryRegion, spawn);
    if (!error.empty()) { return error; }
    for (const auto& [id, state] : record.states) { (void)out.SetMechanismState(id, state); }
    (void)out.TakeCalls();
    return {};
}

SolutionResult ReplaySolution(const std::vector<ReplayRoom>& rooms, const std::vector<Machine>& machines, const PlayRecord& record) {
    SolutionResult result;
    Rules::Exploration world;
    if (const auto error = BuildExploration(rooms, machines, record, world); !error.empty()) { result.detail = "无法开始：" + error; return result; }
    const auto nameOf = [&rooms](const std::string& id) {
        for (const auto& r : rooms) { if (r.settings.region.regionId == id) { return r.name; } }
        return id;
    };
    // 記録で通った区域の順。再生でも同じ順に通れば、途中の時刻が少しずれても通れたとみなす。
    size_t nextMark = 1;
    size_t call = 0;
    bool left = false, right = false;
    double time = 0;
    for (uint32_t frame = 0; frame < record.frames.size(); ++frame) {
        const auto& f = record.frames[frame];
        const bool l = f.flags & 1, r = f.flags & 2, act = f.flags & 4, hold = f.flags & 8;
        Rules::InputState buttons;
        buttons.left = {l, l && !left, !l && left};
        buttons.right = {r, r && !right, !r && right};
        buttons.action = {act, (f.flags & 16) != 0, (f.flags & 32) != 0};
        left = l; right = r;
        double remaining = hold ? 0 : f.seconds;
        uint32_t slice = 0;
        const auto applyCalls = [&](bool frameEnd) {
            while (call < record.calls.size() && record.calls[call].frame == frame && (frameEnd || record.calls[call].slice == slice)) {
                const auto& c = record.calls[call++];
                if (c.kind == "clear") { world.ClearEcho(); }
                else if (c.kind == "back") { (void)world.StepBack(c.region, c.direction); }
            }
        };
        while (remaining > 1e-12) {
            const double step = (std::min)(remaining, 1.0 / 120);
            const auto output = world.Step(buttons, step);
            (void)output;
            applyCalls(false);
            ++slice;
            buttons.action.pressed = buttons.action.released = false; remaining -= step;
        }
        applyCalls(true);
        (void)world.TakeCalls();
        time += f.seconds;
        if (frame % 10 == 0) {
            const auto& body = world.State().player.rect;
            result.path.push_back({body.x + body.w * .5, body.y + body.h * .5});
        }
        if (nextMark < record.regions.size() && world.ActiveRegion() == record.regions[nextMark].region) { ++nextMark; }
        // 記録より多く死んだら、その時点で通れなくなっている。
        const auto recordedDeaths = static_cast<uint64_t>(std::count_if(record.deaths.begin(), record.deaths.end(), [frame](const auto& d) { return d.frame <= frame; }));
        if (world.State().deaths > recordedDeaths) {
            result.detail = Seconds(time) + "在「" + nameOf(world.ActiveRegion()) + "」死亡";
            result.regionId = world.ActiveRegion();
            return result;
        }
    }
    if (nextMark < record.regions.size()) {
        const auto& missed = record.regions[nextMark];
        result.detail = "没能到「" + nameOf(missed.region) + "」（停在「" + nameOf(world.ActiveRegion()) + "」）";
        result.regionId = world.ActiveRegion();
        return result;
    }
    if (!record.endRegion.empty() && world.ActiveRegion() != record.endRegion) {
        result.detail = "最后停在「" + nameOf(world.ActiveRegion()) + "」，应在「" + nameOf(record.endRegion) + "」";
        result.regionId = world.ActiveRegion();
        return result;
    }
    result.passed = true;
    return result;
}
}
