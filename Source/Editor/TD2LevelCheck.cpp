#include "TD2LevelCheck.h"
#include "TD2DeviceEditor.h"
#include "TD2EditorChoices.h"
#include <algorithm>
#include <cmath>
#include <optional>
#include <set>

namespace TD2::Editor {
namespace {
struct Box { double x, y, w, h; };
bool Overlap(const Box& a, const Box& b) { return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h; }

bool SolidCell(const RegionSettings& s, int x, int y) {
    if (x < 0 || y < 0 || x >= s.region.cols || y >= s.region.rows) { return true; }
    return s.region.cells[static_cast<size_t>(y) * static_cast<size_t>(s.region.cols) + static_cast<size_t>(x)] == '1';
}
bool InWall(const RegionSettings& s, const Box& b) {
    for (int y = static_cast<int>(std::floor(b.y)); y < static_cast<int>(std::ceil(b.y + b.h)); ++y)
        for (int x = static_cast<int>(std::floor(b.x)); x < static_cast<int>(std::ceil(b.x + b.w)); ++x)
            if (SolidCell(s, x, y)) { return true; }
    return false;
}
// 激光の光路：装置の中心から向きへ、壁に当たるか射程まで（半マス幅）。
Box Beam(const RegionSettings& s, const DeviceDraft& d) {
    const double cx = d.x + d.w * .5, cy = d.y + d.h * .5;
    double dx = 0, dy = 0;
    if (d.direction == "Left") { dx = -1; } else if (d.direction == "Right") { dx = 1; } else if (d.direction == "Down") { dy = 1; } else { dy = -1; }
    double length = 0;
    while (length < d.range) {
        const double px = cx + dx * (length + .5), py = cy + dy * (length + .5);
        if (SolidCell(s, static_cast<int>(std::floor(px)), static_cast<int>(std::floor(py)))) { break; }
        length += 1;
    }
    const double x0 = (std::min)(cx, cx + dx * length), y0 = (std::min)(cy, cy + dy * length);
    return dx != 0 ? Box{x0, cy - .25, std::abs(dx) * length, .5} : Box{cx - .25, y0, .5, std::abs(dy) * length};
}
}

std::vector<LevelIssue> CheckLevel(const std::vector<CheckRoom>& rooms, const std::vector<Machine>& machines, const LayoutSnapshot* layout) {
    std::vector<LevelIssue> issues;
    const auto add = [&issues](IssueLevel level, std::string text, const std::string& region, double x = -1, double y = -1) { issues.push_back({level, std::move(text), region, x, y}); };
    std::set<std::string> receivers, members;
    for (const auto& m : machines) {
        for (const auto& [r, label] : m.Receivers()) { receivers.insert(r); }
        for (const auto& id : m.Regions()) { members.insert(id); }
    }
    const auto roomOf = [&rooms](std::string_view id) -> const CheckRoom* {
        const auto it = std::find_if(rooms.begin(), rooms.end(), [&](const auto& r) { return r.settings.region.regionId == id; });
        return it == rooms.end() ? nullptr : &*it;
    };
    // すべての区域の配置と、機構の状態 B・C で動いた先の配置（口の検査に使う）。
    std::vector<std::pair<std::string, RegionPlacement>> statePlacements;
    for (const auto& r : rooms) {
        const auto& s = r.settings;
        statePlacements.push_back({s.region.regionId, {s.region.regionId, s.region.cols, s.region.rows, s.worldPlaced, s.worldX, s.worldY}});
    }
    for (const auto& m : machines) for (const auto* list : {&m.offsets, &m.offsetsC}) for (const auto& o : *list) {
        const auto* self = roomOf(o.regionId); const auto* base = roomOf(o.anchor.empty() ? o.regionId : o.anchor);
        if (!self || !base || o.park || !base->settings.worldPlaced) { continue; }
        const auto& s = self->settings;
        statePlacements.push_back({o.regionId, {o.regionId, s.region.cols, s.region.rows, true, base->settings.worldX + o.dx, base->settings.worldY + o.dy}});
    }
    const int entries = static_cast<int>(std::count_if(rooms.begin(), rooms.end(), [](const auto& r) { return r.settings.worldPlaced && r.settings.entryRegion; }));
    if (entries == 0 && !rooms.empty()) { add(IssueLevel::Error, "開始テレビがありません", {}); }
    if (entries > 1) { add(IssueLevel::Error, "開始テレビが " + std::to_string(entries) + " 台あります", {}); }
    for (const auto& room : rooms) {
        const auto& s = room.settings;
        if (!s.worldPlaced) { continue; }
        const auto& id = s.region.regionId;
        if (s.region.cells.size() != static_cast<size_t>(s.region.cols) * static_cast<size_t>(s.region.rows)) { continue; }
        DeviceEditor devices; devices.Load(s.devices);
        if (!devices.IsLoaded()) { add(IssueLevel::Error, room.name + "：装置のデータを読めません", id); continue; }
        std::vector<Box> beams, spikes;
        for (const auto& d : devices.Items()) {
            if (d.kind == "Laser") { beams.push_back(Beam(s, d)); }
            if (d.kind == "Spikes") { spikes.push_back({d.x, d.y, d.w, d.h}); }
        }
        // 立つ所（チェックポイントと開始位置）が壁・光路・トゲの上にないか。
        const auto checkStand = [&](const Box& b, const std::string& what) {
            if (InWall(s, b)) { add(IssueLevel::Error, room.name + "：" + what + "が壁の中", id, b.x, b.y); return; }
            if (std::any_of(spikes.begin(), spikes.end(), [&](const Box& k) { return Overlap(k, b); })) { add(IssueLevel::Error, room.name + "：" + what + "がトゲの上", id, b.x, b.y); return; }
            if (std::any_of(beams.begin(), beams.end(), [&](const Box& k) { return Overlap(k, b); })) { add(IssueLevel::Warning, room.name + "：" + what + "がレーザーの通り道", id, b.x, b.y); }
        };
        if (s.entryRegion) { checkStand({s.spawnX, s.spawnY, .625, .875}, "開始位置"); }
        // チェックポイント：実行時と同じく、近くの床の上で一番近い立てる所へ復活する。立てる所が無ければ使えない。
        const auto anchor = [&](const DeviceDraft& d) -> std::optional<Box> {
            const double ix = d.x + (d.w - .625) * .5, iy = d.y + d.h - .875;
            std::optional<Box> best; double score = 1e9;
            for (int y = static_cast<int>(std::floor(d.y)) - 1; y <= static_cast<int>(std::floor(d.y + d.h)) + 3; ++y)
                for (int x = static_cast<int>(std::floor(d.x)) - 2; x <= static_cast<int>(std::floor(d.x + d.w - 1.0 / 32)) + 2; ++x) {
                    if (x < 0 || y < 0 || x >= s.region.cols || y >= s.region.rows || !SolidCell(s, x, y)) { continue; }
                    const Box b{std::clamp(ix, double(x), double(x) + 1 - .625), double(y) - .875, .625, .875};
                    if (b.y < 0 || InWall(s, b) || std::any_of(spikes.begin(), spikes.end(), [&](const Box& k) { return Overlap(k, b); })) { continue; }
                    const double candidate = std::hypot(b.x - ix, b.y - iy);
                    if (candidate < score) { score = candidate; best = b; }
                }
            return best;
        };
        for (const auto& d : devices.Items()) {
            if (d.kind == "Checkpoint") {
                const auto at = anchor(d);
                if (!at) { add(IssueLevel::Error, room.name + "：" + DeviceName(d) + " の近くに立てる場所がありません", id, d.x, d.y); }
                else if (std::any_of(beams.begin(), beams.end(), [&](const Box& k) { return Overlap(k, *at); })) { add(IssueLevel::Warning, room.name + "：" + DeviceName(d) + " がレーザーの通り道", id, d.x, d.y); }
            }
            if (d.kind != "Plate") { continue; }
            if (d.targets.empty()) { add(IssueLevel::Warning, room.name + "：" + DeviceName(d) + " がどこにもつながっていません", id, d.x, d.y); continue; }
            for (const auto& target : d.targets) {
                const auto kind = ClassifyTarget(target);
                bool found = false;
                if (kind == TargetKind::Local) {
                    found = std::any_of(devices.Items().begin(), devices.Items().end(), [&](const auto& t) { return t.id == target; });
                } else if (kind == TargetKind::OtherRegion) {
                    const auto slash = target.find('/');
                    if (const auto* other = roomOf(target.substr(0, slash))) {
                        DeviceEditor od; od.Load(other->settings.devices);
                        found = std::any_of(od.Items().begin(), od.Items().end(), [&](const auto& t) { return t.id == target.substr(slash + 1); });
                        if (found) { add(IssueLevel::Warning, room.name + "：" + DeviceName(d) + " はほかの画面の装置につながっています", id, d.x, d.y); }
                    }
                } else if (kind == TargetKind::Machine) { found = receivers.contains(target); }
                if (!found) { add(IssueLevel::Error, room.name + "：" + DeviceName(d) + " のつなぎ先がありません", id, d.x, d.y); }
            }
        }
        // 外周の口：地図で何ともつながっていない辺の空いたマス（実行時は閉じた扉になる）。
        // 仕掛けで動くテレビは別の状態でつながるので見ない。隣が仕掛けで寄って来る口も、その状態の位置で見る。
        if (!members.contains(id)) {
            const auto covered = [&](WorldSide side, int along) {
                for (const auto& [other, place] : statePlacements) {
                    if (other == id || !place.worldPlaced) { continue; }
                    const auto edge = FindSharedWorldEdge({id, s.region.cols, s.region.rows, true, s.worldX, s.worldY}, place);
                    if (!edge || edge->sideA != side) { continue; }
                    const int64_t world = (side == WorldSide::Left || side == WorldSide::Right) ? s.worldY + along : s.worldX + along;
                    if (world >= edge->overlapBegin && world < edge->overlapEnd) { return true; }
                }
                return false;
            };
            const std::array<std::pair<WorldSide, std::string_view>, 4> sides{{{WorldSide::Left, "左"}, {WorldSide::Right, "右"}, {WorldSide::Top, "上"}, {WorldSide::Bottom, "下"}}};
            for (const auto& [side, label] : sides) {
                const bool vertical = side == WorldSide::Left || side == WorldSide::Right;
                const int count = vertical ? s.region.rows : s.region.cols;
                for (int i = 0; i < count; ++i) {
                    const int x = side == WorldSide::Left ? 0 : side == WorldSide::Right ? s.region.cols - 1 : i;
                    const int y = side == WorldSide::Top ? 0 : side == WorldSide::Bottom ? s.region.rows - 1 : i;
                    if (SolidCell(s, x, y) || covered(side, i)) { continue; }
                    add(IssueLevel::Warning, room.name + "：" + std::string(label) + "の口がテレビにつながっていません（扉が閉まって見える）", id, x, y);
                    break;
                }
            }
        }
        if (layout && layout->AreaOf(id) == nullptr) { add(IssueLevel::Warning, room.name + "：どの大区域にも入っていません", id); }
    }
    for (const auto& m : machines) {
        MachineCheckInput input;
        for (const auto& room : rooms) { input.placements.push_back({room.settings.region.regionId, room.settings.region.cols, room.settings.region.rows, room.settings.worldPlaced, room.settings.worldX, room.settings.worldY}); }
        for (const auto& room : rooms) {
            DeviceEditor d; d.Load(room.settings.devices);
            for (const auto& device : d.Items()) { if (device.kind == "Plate") { input.plateTargets[room.settings.region.regionId + "/" + device.id] = device.targets; } }
        }
        if (layout) { for (const auto& [object, t] : layout->transforms) { input.sceneObjects.push_back(object); input.objectHasModel[object] = true; } }
        const auto regions = m.Regions();
        for (const auto& problem : CheckMachine(m, machines, input)) {
            if (!layout && problem.find("物体") != std::string::npos) { continue; }
            add(problem.find("圧力板がまだありません") != std::string::npos ? IssueLevel::Warning : IssueLevel::Error, m.id + "：" + problem, regions.empty() ? std::string{} : regions.front());
        }
    }
    // 重いものを先に。
    std::stable_sort(issues.begin(), issues.end(), [](const LevelIssue& a, const LevelIssue& b) { return a.level == IssueLevel::Error && b.level != IssueLevel::Error; });
    return issues;
}
}
