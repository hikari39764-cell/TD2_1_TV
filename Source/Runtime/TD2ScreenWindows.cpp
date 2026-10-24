#include "TD2Screen3D.h"
#include "TD2Screen3DShared.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>

namespace TD2 {
namespace {
using namespace Stage3D;
// 窓の配置：Play のテレビごとの覗き窓と、編集中の窓・オープニングの砂嵐の窓。
// 1 マスの texel 数の上限（編集中の窓）。部屋のマスが少なく画面の中で 1 マスが大きいので 48。
constexpr int kSharpTexelsPerCell = 48;
// 段を選ぶ画面上の 1 マスの画素（1080p 換算）：kTierNeed[i] 以上なら段 i。細かい段から粗い段へ移るのは、その段の境を
// 1 割 5 分下回ってから（画面の大きさが境目を行き来しても段を往復させない）。
constexpr std::array<double, kPortalTiers - 1> kTierNeed{30, 18, 9};
constexpr double kTierKeep = .85;
// 画に入っていたテレビを、画から出た後も撮り続ける秒（カメラが振れて戻った時に結び直さない）。
constexpr double kPortalHold = .5;
// 一度に撮るテレビの数の上限（視点ごとに GPU の選別と描画が一回ずつ増える）。
constexpr size_t kMaxPortals = 40;
// 舞台で部屋を見る角度の正接の上限。これより斜めは緩やかに止める（部屋の外の何も無い所を覗かせない）。
constexpr double kMaxTan = .45;
// オープニングの砂嵐の窓のキャプチャの大きさ。
constexpr uint32_t kNoiseWidth = 1536u, kNoiseHeight = 512u;
// 上限の手前で緩やかに止める。7 割まではそのまま、そこから先だけ縮める。
double SoftLimit(double value, double limit) {
    if (limit <= 0) { return 0.0; }
    const double knee = limit * .7, size = std::abs(value);
    if (size <= knee) { return value; }
    return std::copysign(knee + (limit - knee) * std::tanh((size - knee) / (limit - knee)), value);
}
}

void Screen3DStage::PlacePortals(const std::vector<const StageRoom*>& rooms, const StageFrame& frame, double now) {
    portals_.clear();
    if (seen_.empty()) { return; }
    // 撮るテレビ：画を一回り広げた枠に入った画面と、入っていた画面（kPortalHold 秒まで）。操作中の部屋、切り替え中の二部屋、
    // 残りは画面上の 1 マスの大きい順（同じなら区域 ID の順で、毎フレーム同じ並び）。
    struct Candidate { const StageRoom* room; int rank; double pixels; };
    std::vector<Candidate> candidates;
    for (const auto* room : rooms) {
        if (!room->placed || room->x <= -50000 || room->y <= -50000) { continue; }
        const auto seen = seen_.find(room->regionId);
        if (seen != seen_.end() && seen->second.reach > 0) { portalSeen_[room->regionId] = now; }
        const auto last = portalSeen_.find(room->regionId);
        if (last == portalSeen_.end() || now - last->second > kPortalHold) { continue; }
        const bool travel = room->regionId == travel_[0] || room->regionId == travel_[1];
        candidates.push_back({room, room->regionId == frame.activeRegion ? 0 : travel ? 1 : 2, CellPixels(*room)});
    }
    std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        if (a.rank != b.rank) { return a.rank < b.rank; }
        if (a.pixels != b.pixels) { return a.pixels > b.pixels; }
        return a.room->regionId < b.room->regionId;
    });
    if (candidates.size() > kMaxPortals) { candidates.resize(kMaxPortals); }
    // 砂嵐の間はキャプチャを一枚譲る（一番粗い段を使わない）。
    const int tiers = static_cast<int>(introStatic_ ? kPortalTiers - 1 : kPortalTiers);
    // 段：操作中と切り替え中の部屋は一番細かい段。ほかは画面上の大きさで決め、前の段より粗くするのは境を下回ってから。
    std::vector<int> wants;
    for (const auto& candidate : candidates) {
        int want = static_cast<int>(kPortalTiers) - 1;
        for (size_t i = 0; i < kTierNeed.size(); ++i) { if (candidate.pixels >= kTierNeed[i]) { want = static_cast<int>(i); break; } }
        if (const auto old = portalTier_.find(candidate.room->regionId); old != portalTier_.end() && old->second < want &&
            static_cast<size_t>(old->second) < kTierNeed.size() && candidate.pixels >= kTierNeed[static_cast<size_t>(old->second)] * kTierKeep) {
            want = old->second;
        }
        if (candidate.rank <= 1) { want = 0; }
        wants.push_back((std::min)(want, tiers - 1));
    }
    // 段での矩形の大きさ。位置と大きさは 1 ゲーム画素の倍数（描き方の画素の格子を部屋のマスにそろえる）。
    const auto sized = [](int tier, const StageRoom& room, Portal& portal) {
        const auto& r = room.Region();
        const int density = kTierTexels[static_cast<size_t>(tier)];
        const auto margins = MarginsFor(density);
        const uint32_t block = PortalBlock(density);
        const auto align = [block](uint32_t value) { return (value + block - 1) / block * block; };
        portal.tier = tier; portal.left = margins.left; portal.top = margins.top;
        portal.width = align(margins.left + static_cast<uint32_t>(r.cols * density) + margins.right);
        portal.height = align(margins.top + static_cast<uint32_t>(r.rows * density) + margins.bottom);
    };
    std::vector<Portal> placed(candidates.size());
    std::vector<bool> done(candidates.size(), false);
    for (size_t i = 0; i < candidates.size(); ++i) {
        placed[i].room = candidates[i].room;
        placed[i].always = candidates[i].rank <= 1;
        if (const auto seen = seen_.find(candidates[i].room->regionId); seen != seen_.end() && seen->second.reach > 0) { placed[i].shown = true; }
    }
    // 置き方は二通り。まず前のフレームの所に残す：同じ段・同じ大きさの画面は同じ矩形に置き、新しい画面だけを空いた所の
    // 上・左から詰める（並びを毎フレーム詰め直すと前の画面の大きさが変わるたびに後ろの画面が全部ずれ、ずれた画面は材質を
    // 結び直して影の元の並びが変わり、エンジンが影の場面を毎回作り直していた）。詰め切れない画面が出た時だけ、全部を
    // 左から棚に詰め直す（隙間ができても数は減らさない）。
    std::array<std::vector<const Portal*>, kPortalTiers> used{};
    const auto overlaps = [](const Portal& a, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
        return x < a.x + a.width && a.x < x + w && y < a.y + a.height && a.y < y + h;
    };
    const auto free = [&](int tier, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
        const auto [captureWidth, captureHeight] = kTierCapture[static_cast<size_t>(tier)];
        if (x + w > captureWidth || y + h > captureHeight - kPortalParkRows) { return false; }
        return std::none_of(used[static_cast<size_t>(tier)].begin(), used[static_cast<size_t>(tier)].end(),
                            [&](const Portal* p) { return overlaps(*p, x, y, w, h); });
    };
    const auto keep = [&](size_t i, int tier) {
        const auto last = portalPlace_.find(candidates[i].room->regionId);
        Portal& portal = placed[i];
        sized(tier, *portal.room, portal);
        if (last == portalPlace_.end() || last->second.tier != tier || last->second.width != portal.width || last->second.height != portal.height ||
            !free(tier, last->second.x, last->second.y, portal.width, portal.height)) { return false; }
        portal.x = last->second.x; portal.y = last->second.y;
        used[static_cast<size_t>(tier)].push_back(&portal); done[i] = true;
        return true;
    };
    const auto fill = [&](size_t i, int tier) {
        // 置ける所の候補は、原点と、置いた矩形の右端・下端の組み合わせ（下・左に寄せた所）。上の行から順に一番左。
        Portal& portal = placed[i];
        sized(tier, *portal.room, portal);
        std::vector<uint32_t> xs{0}, ys{0};
        for (const auto* p : used[static_cast<size_t>(tier)]) { xs.push_back(p->x + p->width); ys.push_back(p->y + p->height); }
        std::sort(xs.begin(), xs.end()); std::sort(ys.begin(), ys.end());
        for (const uint32_t y : ys) for (const uint32_t x : xs) {
            if (!free(tier, x, y, portal.width, portal.height)) { continue; }
            portal.x = x; portal.y = y;
            used[static_cast<size_t>(tier)].push_back(&portal); done[i] = true;
            return true;
        }
        return false;
    };
    // 診断（環境変数 TD2_PORTAL_STICKY_OFF）：前の所に残さず、毎フレーム棚に詰め直す（比べるため）。
    static const bool stickyOff = std::getenv("TD2_PORTAL_STICKY_OFF") != nullptr;
    if (!stickyOff) { for (size_t i = 0; i < candidates.size(); ++i) { (void)keep(i, wants[i]); } }
    bool complete = !stickyOff;
    for (size_t i = 0; i < candidates.size() && !stickyOff; ++i) {
        if (done[i]) { continue; }
        bool ok = false;
        for (int tier = wants[i]; tier < tiers && !ok; ++tier) { ok = keep(i, tier) || fill(i, tier); }
        complete = complete && ok;
    }
    if (!complete) {
        // 棚詰め：左から並べ、幅が尽きたら一段下へ。その段に収まらなければ一つずつ粗い段へ。どこにも収まらない画面は映さない。
        struct Shelf { uint32_t x = 0, y = 0, height = 0; };
        std::array<Shelf, kPortalTiers> shelves{};
        std::fill(done.begin(), done.end(), false);
        for (size_t i = 0; i < candidates.size(); ++i) {
            for (int tier = wants[i]; tier < tiers && !done[i]; ++tier) {
                Portal& portal = placed[i];
                sized(tier, *portal.room, portal);
                const auto [captureWidth, captureHeight] = kTierCapture[static_cast<size_t>(tier)];
                Shelf next = shelves[static_cast<size_t>(tier)];
                if (next.x + portal.width > captureWidth) { next = {0, next.y + next.height, 0}; }
                if (next.x + portal.width > captureWidth || next.y + portal.height > captureHeight - kPortalParkRows) { continue; }
                portal.x = next.x; portal.y = next.y;
                next.x += portal.width; next.height = (std::max)(next.height, portal.height);
                shelves[static_cast<size_t>(tier)] = next; done[i] = true;
            }
        }
    }
    portalPlace_.clear();
    for (size_t i = 0; i < candidates.size(); ++i) {
        if (!done[i]) { continue; }
        const auto& portal = placed[i];
        portals_.push_back(portal);
        portalTier_[portal.room->regionId] = portal.tier;
        portalPlace_[portal.room->regionId] = {portal.tier, portal.x, portal.y, portal.width, portal.height};
    }
    // 人物の前を地形が覆わない角度の上限（右・左・上の正接）。人物は奥行き playDepth_ に立つので、外の目が斜めに見ると
    // 手前の地形が「奥行き×正接 − 隙間」だけ人物に掛かる（上から見ると頭の上の天井、横から見ると隣の壁）。プレイヤーのいる
    // 部屋だけ、掛かる量が kCoverAllow 以下になる正接まで絞り、急に変わらないよう上限は時間で追う。回された部屋は対象外。
    constexpr double kCoverAllow = .1, kLimitSeconds = .2;
    const double limitStep = portalLimitTime_ < 0 ? 1.0 : 1 - std::exp(-std::clamp(now - portalLimitTime_, 0.0, .1) / kLimitSeconds);
    portalLimitTime_ = now;
    std::string playerRoom;
    std::array<double, 3> limitTarget{kMaxTan, kMaxTan, kMaxTan};
    if (playDepth_ > 0 && frame.simulated && frame.snapshot && frame.snapshot->playerPresent) {
        const auto& body = frame.snapshot->presentationPlayer;
        for (const auto* room : rooms) {
            const auto& r = room->Region();
            const double lx = body.x - room->x, ly = body.y - room->y;
            if (room->flipped || lx + body.w <= 0 || ly + body.h <= 0 || lx >= r.cols || ly >= r.rows) { continue; }
            playerRoom = room->regionId;
            const auto solid = [&r](int c, int row) {
                return c >= 0 && row >= 0 && c < r.cols && row < r.rows && r.cells[static_cast<size_t>(row * r.cols + c)] == '1';
            };
            const int c0 = static_cast<int>(std::floor(lx)), c1 = static_cast<int>(std::floor(lx + body.w - 1e-6));
            const int r0 = static_cast<int>(std::floor(ly)), r1 = static_cast<int>(std::floor(ly + body.h - 1e-6));
            const auto rowHit = [&](int row) { for (int c = c0; c <= c1; ++c) { if (solid(c, row)) { return true; } } return false; };
            const auto colHit = [&](int c) { for (int row = r0; row <= r1; ++row) { if (solid(c, row)) { return true; } } return false; };
            // 体が地形に重なっている時（瞬間移動の直後など）は負になるので 0 で止める。負だと逆側から覗き込み、壁の側面が縞に写る。
            const auto tanFor = [this](double gap) { return (std::max)(0.0, (gap + kCoverAllow) / playDepth_); };
            for (int row = static_cast<int>(std::floor(ly - 1e-6)); row >= r0 - 2; --row) {
                if (rowHit(row)) { limitTarget[2] = tanFor(ly - (row + 1)); break; }
            }
            for (int c = static_cast<int>(std::floor(lx + body.w)); c <= c1 + 2; ++c) {
                if (colHit(c)) { limitTarget[0] = tanFor(c - (lx + body.w)); break; }
            }
            for (int c = static_cast<int>(std::floor(lx - 1e-6)); c >= c0 - 2; --c) {
                if (colHit(c)) { limitTarget[1] = tanFor(lx - (c + 1)); break; }
            }
            break;
        }
    }
    // 角度：外のカメラがその画面を見る向きの正接（画面の右・上に対して）そのまま。プレイヤーのいる部屋は目が少し後を追う。
    // 画に写っていない間（裏向き・画の外）は最後の角度のまま。
    for (auto& portal : portals_) {
        const auto& id = portal.room->regionId;
        auto& tan = portalTan_[id];
        if (const auto seen = seen_.find(id); seen != seen_.end() && seen->second.facing > .05 && seen->second.depth > .05) {
            const auto follow = follow_.find(id);
            const double fx = follow != follow_.end() ? follow->second[0] : 0.0, fy = follow != follow_.end() ? follow->second[1] : 0.0;
            tan = {SoftLimit(seen->second.eyeX * parallaxGain_ + fx, kMaxTan), SoftLimit(seen->second.eyeY * parallaxGain_ + fy, kMaxTan)};
        }
        auto [limit, fresh] = portalLimit_.try_emplace(id, std::array<double, 3>{kMaxTan, kMaxTan, kMaxTan});
        for (size_t k = 0; k < limit->second.size(); ++k) {
            const double goal = id == playerRoom ? (std::min)(limitTarget[k], kMaxTan) : kMaxTan;
            limit->second[k] += (goal - limit->second[k]) * (fresh ? 1.0 : limitStep);
        }
        portal.tanX = std::clamp(tan[0], -limit->second[1], limit->second[0]);
        portal.tanY = (std::min)(tan[1], limit->second[2]);
    }
}

HIKARI::GameProjectSceneCaptureViewDesc Screen3DStage::PortalView(const Portal& portal, double shakeX, double shakeY) const {
    const auto& room = *portal.room;
    const auto& r = room.Region();
    const double density = kTierTexels[static_cast<size_t>(portal.tier)], scale = projection_.scale;
    // 視錐台が通る地図の矩形（余白込み。マス、y は下向き）と、部屋の中心。
    const double left = room.x - portal.left / density, top = room.y - portal.top / density;
    const double right = left + portal.width / density, bottom = top + portal.height / density;
    const double cx = room.x + r.cols * .5, cy = room.y + r.rows * .5;
    // 目：部屋の中心の正面 kPortalEyeCells の所から、見る角度の分だけ横と上へずらす（地図の y は下向き、角度は上向き）。
    const double ex = perspective_ ? cx + kPortalEyeCells * portal.tanX : cx;
    const double ey = perspective_ ? cy - kPortalEyeCells * portal.tanY : cy;
    HIKARI::GameProjectSceneCaptureViewDesc view{};
    view.x = portal.x; view.y = portal.y; view.width = portal.width; view.height = portal.height;
    const auto eye = projection_.Point(ex, ey, kPortalEyeCells);
    const auto right3 = projection_.Right(), up3 = projection_.Up(), forward3 = projection_.Forward();
    view.eye = {eye.x + static_cast<float>(right3[0] * shakeX + up3[0] * shakeY), eye.y + static_cast<float>(right3[1] * shakeX + up3[1] * shakeY),
                eye.z + static_cast<float>(right3[2] * shakeX + up3[2] * shakeY)};
    view.forward = {static_cast<float>(forward3[0]), static_cast<float>(forward3[1]), static_cast<float>(forward3[2])};
    view.up = {static_cast<float>(up3[0]), static_cast<float>(up3[1]), static_cast<float>(up3[2])};
    view.nearZ = static_cast<float>((kPortalEyeCells - kPortalNearCells) * scale);
    view.farZ = static_cast<float>((kPortalEyeCells + kPortalFarCells) * scale);
    if (perspective_) {
        // 正面の面（目から kPortalEyeCells）の矩形を近い面へ縮めた端。正面の面はどの角度から見ても矩形にぴったり写る。
        const double k = view.nearZ / kPortalEyeCells;
        view.left = static_cast<float>((left - ex) * k); view.right = static_cast<float>((right - ex) * k);
        view.top = static_cast<float>((ey - top) * k); view.bottom = static_cast<float>((ey - bottom) * k);
    } else {
        view.orthographic = 1u;
        view.left = static_cast<float>((left - cx) * scale); view.right = static_cast<float>((right - cx) * scale);
        view.top = static_cast<float>((cy - top) * scale); view.bottom = static_cast<float>((cy - bottom) * scale);
    }
    return view;
}

double Screen3DStage::CellPixels(const StageRoom& room) const {
    const auto seen = seen_.find(room.regionId);
    const auto& r = room.Region();
    if (seen == seen_.end() || r.cols <= 0 || r.rows <= 0) { return 0; }
    return std::sqrt(seen->second.coverage * 1920.0 * 1080.0 / (double(r.cols) * r.rows));
}

void Screen3DStage::PlaceNoise() {
    // 砂嵐の窓：舞台の外のノイズの板を透視カメラで撮る。
    auto& noise = windows_[static_cast<size_t>(ViewKind::Noise)];
    noise = {};
    if (!noiseCamera_.handle.IsValid()) { introStatic_ = false; return; }
    noise.rows = kNoiseRows; noise.cols = kNoiseRows * double(kNoiseWidth) / kNoiseHeight;
    noise.x = kNoiseX - noise.cols * .5; noise.y = kNoiseY - noise.rows * .5;
    noise.width = kNoiseWidth; noise.height = kNoiseHeight;
    noise.density = (std::max)(1, static_cast<int>(kNoiseHeight / noise.rows));
    noise.distance = noise.rows * projection_.scale * .5 / std::tan(noiseCamera_.verticalFov * .5);
    noise.valid = true;
}

void PackRoomsForEditor(std::vector<StageRoom>& rooms) {
    constexpr int kGap = 2;          // 部屋の間（よその部屋の影や壁が写り込まない）
    constexpr int kGroupGap = 8;     // 組の間
    // 背の高い部屋から棚に並べる（同じ並びが毎フレーム同じになるよう、区域 ID でも並べる）。
    std::vector<size_t> order(rooms.size());
    for (size_t i = 0; i < order.size(); ++i) { order[i] = i; }
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        const auto& ra = rooms[a].Region(); const auto& rb = rooms[b].Region();
        if (ra.rows != rb.rows) { return ra.rows > rb.rows; }
        if (ra.cols != rb.cols) { return ra.cols > rb.cols; }
        return rooms[a].regionId < rooms[b].regionId;
    });
    // 面積の釣り合う二つの組へ（大きい部屋から、面積の小さい組へ）。組ごとに別の窓で撮り、1 マスの texel を倍近くにする。
    std::array<std::vector<size_t>, 2> groups;
    std::array<double, 2> area{};
    for (const size_t i : order) {
        const auto& r = rooms[i].Region();
        const size_t g = area[0] <= area[1] ? 0 : 1;
        groups[g].push_back(i); area[g] += double(r.cols + kGap) * (r.rows + kGap);
    }
    int offset = 0;
    for (size_t g = 0; g < groups.size(); ++g) {
        if (groups[g].empty()) { continue; }
        int widest = 0;
        for (const size_t i : groups[g]) { widest = (std::max)(widest, rooms[i].Region().cols + kGap); }
        // 棚の幅を変えて試し、縦横の長い方が一番短くなる幅で並べる。
        const auto shelve = [&](int width, bool apply) {
            int x = 0, y = 0, shelf = 0, right = 0;
            for (const size_t i : groups[g]) {
                const auto& r = rooms[i].Region();
                if (x > 0 && x + r.cols + kGap > width) { y += shelf; x = 0; shelf = 0; }
                if (apply) { rooms[i].x = offset + x; rooms[i].y = y; rooms[i].editorGroup = static_cast<int>(g); }
                x += r.cols + kGap; shelf = (std::max)(shelf, r.rows + kGap); right = (std::max)(right, x);
            }
            return std::pair{right, y + shelf};
        };
        const int start = (std::max)(widest, static_cast<int>(std::sqrt(area[g])));
        int best = start, bestSide = INT_MAX;
        for (int width = start; width <= 2 * start + widest; ++width) {
            const auto [w, h] = shelve(width, false);
            if ((std::max)(w, h) < bestSide) { bestSide = (std::max)(w, h); best = width; }
        }
        offset += shelve(best, true).first + kGroupGap;
    }
}

void Screen3DStage::PlaceEditorWindow(const std::vector<const StageRoom*>& rooms) {
    windows_ = {};
    if (rooms.empty()) { return; }
    // 組ごとの範囲。二つ目の組は未配置用のカメラで撮る（鮮明のカメラと正投影の高さが同じ時だけ。窓の行数が舞台の縮尺を
    // 共有するため）。使えなければ一枚の窓に全部を収める。
    struct Box { double left = 0, top = 0, right = 0, bottom = 0; bool any = false; };
    std::array<Box, 2> boxes{};
    const auto& sharp = cameras_[static_cast<size_t>(ViewKind::Sharp)];
    const auto& second = cameras_[static_cast<size_t>(ViewKind::Area)];
    const bool split = second.handle.IsValid() && std::abs(second.orthographicHeight - sharp.orthographicHeight) < 1e-3;
    for (const auto* room : rooms) {
        auto& box = boxes[split && room->editorGroup == 1 ? 1 : 0];
        const double l = room->x, t = room->y, r = room->x + room->Region().cols, b = room->y + room->Region().rows;
        if (!box.any) { box = {l, t, r, b, true}; continue; }
        box.left = (std::min)(box.left, l); box.top = (std::min)(box.top, t);
        box.right = (std::max)(box.right, r); box.bottom = (std::max)(box.bottom, b);
    }
    // 窓は全部同じ行数（舞台の縮尺が共通）。一番大きい辺が 2048 texel に収まる精細度（上限は鮮明と同じ）で撮る。
    double rows = 1, side = 1;
    for (const auto& box : boxes) if (box.any) {
        rows = (std::max)(rows, box.bottom - box.top);
        side = (std::max)({side, box.right - box.left, box.bottom - box.top});
    }
    const double maxExtent = HIKARI::kGameProjectSceneCaptureMaximumExtent - 8.0;
    const double density = (std::min)(double(kSharpTexelsPerCell), maxExtent / side);
    const auto height = static_cast<uint32_t>(std::clamp(std::ceil(rows * density),
        double(HIKARI::kGameProjectSceneCaptureMinimumExtent), double(HIKARI::kGameProjectSceneCaptureMaximumExtent)));
    const double exact = height / rows;   // 丸めた後の 1 マスの texel（行数をちょうど rows にする）
    for (size_t k = 0; k < boxes.size(); ++k) {
        if (!boxes[k].any) { continue; }
        auto& window = windows_[static_cast<size_t>(k == 0 ? ViewKind::Sharp : ViewKind::Area)];
        const double cols = (std::max)(1.0, boxes[k].right - boxes[k].left);
        window.width = static_cast<uint32_t>(std::clamp(std::ceil(cols * exact),
            double(HIKARI::kGameProjectSceneCaptureMinimumExtent), double(HIKARI::kGameProjectSceneCaptureMaximumExtent)));
        window.height = height;
        window.rows = rows; window.cols = window.width / exact;
        window.x = boxes[k].left; window.y = boxes[k].top; window.valid = true;
        window.density = (std::max)(1, static_cast<int>(std::lround(exact)));
    }
    projection_.scale = sharp.orthographicHeight / rows;
}
}
