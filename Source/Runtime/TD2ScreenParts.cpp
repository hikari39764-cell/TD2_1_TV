#include "TD2Screen3D.h"
#include "TD2Screen3DShared.h"
#include "TD2StageProjection.h"
#include "TD2ScreenKitAssets.h"
#include "TD2StageLight.h"
#include "TD2Transform.h"
#include "HIKARI/Project/ActionViews.h"
#include "HIKARI/Project/WorldQuery.h"
#include "HIKARI/Project/PresentationViews.h"
#include <optional>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>

namespace TD2 {
namespace {
using namespace Stage3D;
// 舞台の部品（部屋の背景・地形・装置・人形）と舞台の灯の提出。
// 舞台の部品の数の目安。部品は材質ごとに GPU のシーンの面を使い、工場の物体と合わせてエンジンの上限（16384 面）を超えると
// 画面全体の描画が止まる。
constexpr size_t kPartWarning = 7000;
// オープニングの板を使わない間に退ける下への距離（舞台カメラに写らない所）。
constexpr float kIntroPlaneHide = 2000.0f;
// 瞬間移動の残像が足元へ潰れて消えるまでの秒数。
constexpr double kSmearSeconds = 0.12;
// 跳ね返りの残影を足元から現す秒数（ルールの予告と同じ 0.16 秒）と、回収の光跡が消える秒数。
constexpr double kEchoPrintSeconds = 0.16, kRecallTrailSeconds = 0.2;
}

void Screen3DStage::SyncStageLights(HIKARI::WorldView& world, const std::vector<Assigned>& shown, double now) {
    // 灯は部屋に付けて離さない：その部屋が写っている間は同じ灯が照らし続ける（並び順で灯を配ると、部屋が出入りする
    // たびに灯が別の部屋へ飛び、写っている部屋の明るさが跳ねた）。空いた灯を新しく写った部屋へ、鮮明の窓の部屋から配る。
    // 灯を移す時は今の部屋で消えてから次の部屋で点く（それぞれ kFade 秒）。
    constexpr double kFade = .2;
    // 時計の無い時（編集中など）は待たずに切り替える。
    const double dt = now > 0 ? std::clamp(now - stageLightTime_, 0.0, .1) : kFade;
    stageLightTime_ = now;
    std::array<std::vector<HIKARI::ObjectView>, kStageLightRoleCount> pools;
    std::map<uint64_t, StageLightSettings> settings;
    const HIKARI::ComponentTypeId types[]{HIKARI::MakeComponentTypeId(kStageLightComponentType)};
    (void)world.ForEachComponents<StageLightComponent>(types, HIKARI::GameProjectWorldQueryAnchor::FirstRequiredTerm,
        [&](HIKARI::ObjectView object, StageLightComponent& light) {
            pools[static_cast<size_t>(std::clamp(light.Settings().role, 0, kStageLightRoleCount - 1))].push_back(object);
            settings[object.GetHandle().value] = light.Settings();
        });
    const auto shownRoom = [&](const std::string& id) {
        return std::find_if(shown.begin(), shown.end(), [&](const Assigned& a) { return a.room->regionId == id; });
    };
    for (auto& pool : pools) {
        std::sort(pool.begin(), pool.end(), [](const auto& a, const auto& b) { return a.GetHandle().value < b.GetHandle().value; });
        // 照らす部屋：写っている順（鮮明の窓が先）に灯の数まで。今照らしている部屋はその中にある限り灯を保つ。
        std::vector<std::string> wanted;
        for (size_t i = 0; lit_ && i < shown.size() && wanted.size() < pool.size(); ++i) { wanted.push_back(shown[i].room->regionId); }
        std::vector<StageLightTrack*> free;
        for (const auto& object : pool) {
            auto& t = stageLights_[object.GetHandle().value];
            if (!t.known) {
                HIKARI::GameProjectLightState state{};
                if (!object.GetLight().TryGetState(state)) { continue; }
                t.handle = object.GetHandle(); t.base = state.intensity; t.shadows = state.castsShadows != 0; t.known = true;
            }
            const auto keep = std::find(wanted.begin(), wanted.end(), t.target);
            if (!t.target.empty() && keep != wanted.end()) { wanted.erase(keep); } else { t.target.clear(); free.push_back(&t); }
        }
        for (size_t i = 0; i < free.size() && i < wanted.size(); ++i) { free[i]->target = wanted[i]; }
    }
    for (const auto& pool : pools) for (const auto& object : pool) {
        auto& t = stageLights_[object.GetHandle().value];
        if (!t.known) { continue; }
        // 今の部屋が画面から消えたら、その場で消す（写っていない所の光は見えない）。
        if (!t.room.empty() && shownRoom(t.room) == shown.end()) { t.room.clear(); t.weight = 0; }
        if (t.room != t.target) {
            t.weight = std::max(0.0, t.weight - dt / kFade);
            if (t.weight <= 0) { t.room = t.target; }
        } else if (!t.room.empty()) { t.weight = std::min(1.0, t.weight + dt / kFade); }
        const auto at = t.room.empty() ? shown.end() : shownRoom(t.room);
        const bool use = at != shown.end();
        if (use) {
            // 部屋の大きさに合わせて置き、部屋の正面の中心から奥 aimDepth の所へ向ける（灯の前は +Z）。
            const auto& s = settings[object.GetHandle().value];
            const auto& room = *at->room; const auto& r = room.Region();
            const double cx = room.x + r.cols * .5, cy = room.y + r.rows * .5;
            const auto from = projection_.Point(cx + s.offsetX * r.cols * .5, cy - s.offsetY * r.rows * .5, s.offsetZ);
            const auto aim = projection_.Point(cx, cy, -s.aimDepth);
            Vector3 forward{double(aim.x - from.x), double(aim.y - from.y), double(aim.z - from.z)};
            const double length = std::sqrt(forward[0] * forward[0] + forward[1] * forward[1] + forward[2] * forward[2]);
            if (length > 1e-6) {
                for (auto& v : forward) { v /= length; }
                const Vector3 worldUp = std::abs(forward[1]) > .99 ? Vector3{0, 0, 1} : Vector3{0, 1, 0};
                auto side = Cross(worldUp, forward);
                const double sl = std::sqrt(side[0] * side[0] + side[1] * side[1] + side[2] * side[2]);
                for (auto& v : side) { v /= sl; }
                HIKARI::GameProjectTransform pose{};
                pose.position = from;
                pose.rotation = QuaternionFromBasis(side, Cross(forward, side), forward);
                pose.scale = {1, 1, 1};
                (void)object.SetWorldTransform(pose);
            }
        }
        // 強さは作者の値に滑らかな重みを掛ける。変わった時だけ送る。
        const double w = t.weight * t.weight * (3 - 2 * t.weight);
        const float intensity = static_cast<float>(t.base * w);
        const bool enabled = use && w > 0;
        HIKARI::GameProjectLightPatch patch{};
        if (enabled != t.enabled) {
            patch.fieldMask |= HIKARI::GameProjectLightPatchFields(HIKARI::GameProjectLightPatchField::Enabled);
            patch.enabled = enabled ? 1u : 0u;
        }
        // 舞台は場景キャプチャにしか写らず、キャプチャには局所光の影が出ない。毎フレーム動く灯の影を描かないよう切る。
        if (!t.shadowOff) {
            patch.fieldMask |= HIKARI::GameProjectLightPatchFields(HIKARI::GameProjectLightPatchField::CastShadows);
            patch.castsShadows = 0u;
        }
        if (enabled && std::abs(intensity - t.sent) > t.base * .002f) {
            patch.fieldMask |= HIKARI::GameProjectLightPatchFields(HIKARI::GameProjectLightPatchField::Intensity);
            patch.intensity = intensity;
        }
        if (patch.fieldMask != 0 && object.GetLight().ApplyPatch(patch).Accepted()) {
            t.enabled = enabled; t.shadowOff = true;
            if (patch.fieldMask & HIKARI::GameProjectLightPatchFields(HIKARI::GameProjectLightPatchField::Intensity)) { t.sent = intensity; }
        }
    }
}

void Screen3DStage::ReleaseStageLights(HIKARI::WorldView& world) {
    // 消して作者の強さへ戻す（Play を止めた後の場景に半端な値を残さない）。
    for (auto& [id, t] : stageLights_) {
        if (!t.known) { continue; }
        if (const auto object = world.FindObject(t.handle); object.IsValid()) {
            HIKARI::GameProjectLightPatch patch{};
            patch.fieldMask = HIKARI::GameProjectLightPatchFields(HIKARI::GameProjectLightPatchField::Enabled) |
                              HIKARI::GameProjectLightPatchFields(HIKARI::GameProjectLightPatchField::Intensity) |
                              HIKARI::GameProjectLightPatchFields(HIKARI::GameProjectLightPatchField::CastShadows);
            patch.enabled = 0u; patch.intensity = t.base; patch.castsShadows = t.shadows ? 1u : 0u;
            (void)object.GetLight().ApplyPatch(patch);
        }
    }
    stageLights_.clear();
}

void Screen3DStage::SubmitParts(HIKARI::WorldView& world, const std::vector<const StageRoom*>& rooms, const std::vector<Assigned>& shown,
                                const Rules::WorldSnapshot& snapshot, int palette, bool smear, double now, bool animate) {
    const auto models = world.GetPresentationModels();
    submitted_.clear(); frameParts_.clear(); accepted_ = failed_ = 0;
    const auto submit = [&](const PartRequest& request) {
        if (models.Submit(request)) { ++accepted_; submitted_.emplace_back(request.ownerObjectHandle, request.instanceId); }
        else { ++failed_; }
    };
    // 世界の占有（マス）：舞台に並べた全部屋の地図と、実体のある Solid 装置。部屋の無い所は埋まり扱い。
    // 隣り合いの判定は世界の位置（linkX, linkY）で行う（編集中は描く位置を詰めて並べ直している）。
    const auto solidAt = [&](int wx, int wy) {
        for (const auto* room : rooms) {
            const auto& r = room->Region();
            if (wx >= room->linkX && wy >= room->linkY && wx < room->linkX + r.cols && wy < room->linkY + r.rows) {
                const int lx = wx - room->linkX, ly = wy - room->linkY;
                return r.cells[CellIndex(r, room->flipped ? r.cols - 1 - lx : lx, room->flipped ? r.rows - 1 - ly : ly)] == '1';
            }
        }
        return true;
    };
    // 回された部屋：その部屋の中の世界の点を、作者の向きの絵の上の点へ移す中心（無ければ null）。
    const auto flippedRoomAt = [&](double wx, double wy) -> const StageRoom* {
        for (const auto* room : rooms) {
            const auto& r = room->Region();
            if (room->flipped && wx >= room->x && wy >= room->y && wx < room->x + r.cols && wy < room->y + r.rows) return room;
        }
        return nullptr;
    };
    const auto mirrorFor = [&](StagePartBuilder& builder, const StageRoom* room) {
        if (room && room->flipped) builder.Mirror(room->x + room->Region().cols * .5, room->y + room->Region().rows * .5);
        else builder.NoMirror();
    };
    // そのマスに別の部屋があるか（外周の口がどこかへつながっているか）。
    const auto roomAt = [&](int wx, int wy, const StageRoom* self) {
        return std::any_of(rooms.begin(), rooms.end(), [&](const StageRoom* room) {
            const auto& r = room->Region();
            return room != self && wx >= room->linkX && wy >= room->linkY && wx < room->linkX + r.cols && wy < room->linkY + r.rows;
        });
    };
    // 描く部屋：Play は世界に置いた全部の部屋（仕掛けで預け場所へ離した部屋も、離れた所でそのまま出す）。写っているか・
    // 預けたかで部品を出し入れすると、エンジンが GPU のシーンを作り直して画が止まる（部屋ごとに数百の面が出入りする）。
    // 写らない部屋の部品はどのキャプチャにも入らない。編集中は窓に収めた部屋。
    std::vector<const StageRoom*> drawn;
    if (animate) {
        for (const auto* room : rooms) { if (room->placed) { drawn.push_back(room); } }
    } else {
        for (const auto& item : shown) { drawn.push_back(item.room); }
    }
    std::map<std::string_view, const StageRoom*> byRegion;
    for (const auto* room : drawn) { byRegion.emplace(room->regionId, room); }

    // 診断の計時：区間ごとに次の区間を始めると前の区間が閉じる。
    std::optional<StageTimer> lap; lap.emplace("parts.terrain");
    // 背景と地形：部屋と周り 1 マスの占有が変わった時だけ作り直し、毎フレームは作った要求をそのまま出す。
    // Play：地形の入力（全部の部屋の位置・向き・地図、装置の位置と実体、描き方）の指紋が前のフレームと同じなら、
    // 部屋ごとの占有の数え直しと比べ直しを丸ごと飛ばす（作った要求は残してあり、結果は変わらない）。
    uint64_t terrainKey = 0;
    if (animate) {
        uint64_t key = 1469598103934665603ull;
        const auto mixBytes = [&key](const void* data, size_t size) {
            for (size_t i = 0; i < size; ++i) { key = (key ^ static_cast<const unsigned char*>(data)[i]) * 1099511628211ull; }
        };
        const auto mix = [&mixBytes](const auto& value) { mixBytes(&value, sizeof(value)); };
        mix(palette); mix(projection_.scale); mix(projection_.pitch); mix(projection_.yaw); mix(hall_); mix(depthRows_); mix(playDepth_); mix(solid_);
        for (const auto* room : rooms) {
            const auto& r = room->Region();
            mixBytes(room->regionId.data(), room->regionId.size()); mix(room->x); mix(room->y); mix(room->linkX); mix(room->linkY); mix(room->flipped); mix(room->placed);
            mix(r.cols); mix(r.rows); mixBytes(r.cells.data(), r.cells.size());
        }
        for (const auto& device : snapshot.devices) { mix(device.kind); mix(device.rect); mix(device.solid); }
        terrainKey = key == 0 ? 1 : key;
    }
    // 診断（環境変数 TD2_TERRAIN_SKIP_OFF）：飛ばさずに毎フレーム数え直す（比べるため）。
    static const bool skipOff = std::getenv("TD2_TERRAIN_SKIP_OFF") != nullptr;
    const bool terrainSame = animate && !skipOff && terrainKey == terrainKey_;
    terrainKey_ = terrainKey;
    std::vector<char> occupancy, linked;
    for (const auto* drawnRoom : drawn) {
        if (terrainSame) { break; }
        const auto& room = *drawnRoom; const auto& r = room.Region();
        const int width = r.cols + 2, height = r.rows + 2;
        occupancy.assign(static_cast<size_t>(width * height), 0);
        linked.assign(static_cast<size_t>(width * height), 1);
        // 絵の上のマス (i, j)（周り 1 マス込み）が、規則の世界のどのマスか。回された部屋は点対称に読む。
        const auto worldCell = [&](int i, int j) {
            return room.flipped ? std::pair{room.linkX + r.cols - i, room.linkY + r.rows - j} : std::pair{room.linkX + i - 1, room.linkY + j - 1};
        };
        for (int j = 0; j < height; ++j) for (int i = 0; i < width; ++i) {
            const bool inside = i > 0 && j > 0 && i <= r.cols && j <= r.rows;
            const auto [wx, wy] = worldCell(i, j);
            // Play では周り 1 マスを隣に関係なく埋まり扱いにする。隣の部屋が仕掛けで動くと継ぎ目の部品の数が変わり、
            // 新しい部品が出るたびにエンジンが GPU のシーンを作り直して画が止まっていた（画面の縁の継ぎ目だけの違い）。
            occupancy[static_cast<size_t>(j * width + i)] = inside ? r.cells[CellIndex(r, i - 1, j - 1)] == '1' : (animate || solidAt(wx, wy));
            if (!inside) { linked[static_cast<size_t>(j * width + i)] = roomAt(wx, wy, &room); }
        }
        for (const auto& device : snapshot.devices) if (device.kind == Rules::DeviceKind::Solid && device.solid) {
            auto rect = device.rect;
            if (room.flipped) { rect.x = 2.0 * room.x + r.cols - rect.x - rect.w; rect.y = 2.0 * room.y + r.rows - rect.y - rect.h; }
            const int left = static_cast<int>(std::floor(rect.x)) - room.x + 1, top = static_cast<int>(std::floor(rect.y)) - room.y + 1;
            const int right = static_cast<int>(std::ceil(rect.x + rect.w)) - room.x + 1;
            const int bottom = static_cast<int>(std::ceil(rect.y + rect.h)) - room.y + 1;
            for (int j = (std::max)(top, 0); j < (std::min)(bottom, height); ++j)
                for (int i = (std::max)(left, 0); i < (std::min)(right, width); ++i) { occupancy[static_cast<size_t>(j * width + i)] = 1; }
        }
        // 窓の上下：操作の札にかからない所へ寄せる。
        const WindowSpan span = StagePartBuilder{projection_, palette, room.owner.value, frameParts_}.SpanAvoidingSigns(
            snapshot.devices, room.regionId, room.y, r.rows);
        windowSpans_[room.regionId] = span;
        auto& cache = roomParts_[room.regionId];
        // 編集中は毎フレーム出す（Play で残した要求が残っていれば消す）。
        if (!animate && !cache.retained.empty()) {
            for (const auto& [owner, id] : cache.retained) { models.Remove(owner, id); }
            cache.retained.clear(); cache.retainedValid = false;
        }
        if (cache.occupancy != occupancy || cache.linked != linked || cache.x != room.x || cache.y != room.y || cache.palette != palette ||
            cache.scale != projection_.scale || cache.pitch != projection_.pitch || cache.yaw != projection_.yaw || cache.compact != !animate ||
            cache.hall != hall_ || cache.depthRows != depthRows_ || cache.playDepth != playDepth_ ||
            !(cache.window == span)) {
            auto retained = std::move(cache.retained);
            auto series = std::move(cache.series), hallSeries = std::move(cache.hallSeries);
            cache = {occupancy, linked, room.x, room.y, palette, projection_.scale, projection_.pitch, projection_.yaw, !animate, hall_,
                     depthRows_, playDepth_, span, {}};
            cache.retained = std::move(retained); cache.hallSeries = std::move(hallSeries);
            StagePartBuilder builder{projection_, palette, room.owner.value, cache.parts};
            builder.Compact(!animate);
            builder.UseHall(hall_);
            builder.DepthRows(depthRows_);
            builder.Solid(solid_, playDepth_);
            builder.Backdrop(room.x, room.y, r.cols, r.rows, span);
            builder.Terrain(room.x, room.y, r.cols, r.rows, occupancy);
            builder.Edges(room.x, room.y, r.cols, r.rows, occupancy, linked);
            // 仕掛けで部屋が動くと隣との境の地形の部品の数が変わる。減った分は隠した部品で埋め、組を減らさない
            // （組が変わるとエンジンが GPU のシーンを作り直して画が止まる）。
            // 診断（環境変数 TD2_SPIKE_EVENTS）：作り直した部屋と、部品の数が前より増えたか（増えると GPU のシーンを作り直す）。
            static const bool spikeEvents = std::getenv("TD2_SPIKE_EVENTS") != nullptr;
            size_t before = 0, after = 0;
            if (spikeEvents) { for (const auto& item : series) { before += item.second.count; } }
            builder.PadSeries(series);
            cache.series = builder.Counts();
            if (spikeEvents) {
                for (const auto& item : cache.series) { after += item.second.count; }
                std::fprintf(stderr, "[TD2 parts] rebuild room=%s parts=%zu series %zu->%zu\n", room.regionId.c_str(), cache.parts.size(), before, after);
            }
            failed_ += builder.Failed();
        }
        if (!animate) { for (const auto& request : cache.parts) { submit(request); } }
    }
    std::erase_if(roomParts_, [&](const auto& item) {
        if (byRegion.contains(item.first)) { return false; }
        for (const auto& [owner, id] : item.second.retained) { models.Remove(owner, id); }
        return true;
    });

    lap.emplace("parts.hall");
    // 窓の向こうの大広間：外のカメラが画面を見る角度（なめらかにした正接）だけ奥の層をずらす。編集中は出さない。
    // 透視で撮る時は本当の透視で奥の層がずれるので、手で付ける視差は足さない（向きが 0 のまま変わらず、作った部品を使い回す）。
    if (animate && hall_) {
        for (const auto* drawnRoom : drawn) {
            const auto& room = *drawnRoom; const auto& r = room.Region();
            const auto eye = parallax_.find(room.regionId);
            double ex = eye != parallax_.end() ? eye->second[0] : 0.0, ey = eye != parallax_.end() ? eye->second[1] : 0.0;
            // 回されたテレビは画面の右・上が絵の左・下になる。
            if (room.flipped) { ex = -ex; ey = -ey; }
            if (perspective_) { ex = ey = 0; }
            auto& cache = roomParts_[room.regionId];
            const std::array<double, 2> look{ex * parallaxGain_, ey * parallaxGain_};
            if (!cache.hallValid || cache.hallEye != look) {
                cache.hallParts.clear(); cache.hallEye = look; cache.hallValid = true; cache.retainedValid = false;
                StagePartBuilder builder{projection_, palette, room.owner.value, cache.hallParts};
                builder.UseHall(true);
                builder.DepthRows(depthRows_);
                builder.Solid(solid_, playDepth_);
                builder.Hall(room.regionId, room.x, room.y, r.cols, r.rows, windowSpans_[room.regionId], look[0], look[1]);
                builder.PadSeries(cache.hallSeries);
                cache.hallSeries = builder.Counts();
                failed_ += builder.Failed();
            }
        }
    }
    // Play：部屋の背景・地形・大広間は残しておく要求（retain）で一度だけ出し、作り直した時だけ出し直す（数千の動かない部品を
    // 毎フレーム出してエンジンに比べ直させない）。出し直しで減った部品は消す。
    if (animate) {
        for (const auto* drawnRoom : drawn) {
            auto& cache = roomParts_[drawnRoom->regionId];
            if (!cache.retainedValid) {
                std::vector<std::pair<uint64_t, uint64_t>> ids;
                std::unordered_map<uint64_t, PartRequest> submitted;
                for (const auto* list : {&cache.parts, &cache.hallParts}) for (auto request : *list) {
                    request.retain = 1;
                    const auto last = cache.submitted.find(request.instanceId);
                    const bool same = last != cache.submitted.end() && std::memcmp(&last->second, &request, sizeof(request)) == 0;
                    if (same || models.Submit(request)) {
                        ids.emplace_back(request.ownerObjectHandle, request.instanceId); submitted.emplace(request.instanceId, request);
                    } else { ++failed_; }
                }
                std::sort(ids.begin(), ids.end());
                for (const auto& old : cache.retained) { if (!std::binary_search(ids.begin(), ids.end(), old)) { models.Remove(old.first, old.second); } }
                cache.submitted = std::move(submitted);
                cache.retained = std::move(ids); cache.retainedValid = true;
            }
            accepted_ += cache.retained.size();
        }
    }

    lap.emplace("parts.devices");
    // 装置と光線：描く部屋の装置だけ（Play は全部の部屋）。状態で変わる部品は全部出し、表示だけを切り替える。
    // 通電・実体が変わった時刻は描かない部屋の装置も覚える（後で画面に入った時に古い変化を動かし直さない）。
    // 編集中は時刻が進まないので、動きを付けず最終の姿にする。Play でも画に写らない部屋の装置は時刻を止める（回る・脈打つ
    // 部品が毎フレーム GPU のシーンを書き直させない。写る前に画の外側で動き出すので、止めた所は見えない）。
    std::set<std::string_view> seenRooms;
    for (const auto& item : shown) { seenRooms.insert(item.room->regionId); }
    // 回す機構のある部屋の装置は回さない：テレビの今の傾き（画面の外から見て反時計回りの角）の分だけ逆に回して描き、
    // テレビと地形が回っている途中も、回り終えた後も、装置は正立して同じ所に見える。
    std::map<std::string_view, double> roomTurn;
    for (const auto* drawnRoom : drawn) {
        HIKARI::GameProjectTransform tv{};
        if (!drawnRoom->flippable || !world.FindSceneObject(drawnRoom->configuration.screenObject).GetWorldTransform(tv)) { continue; }
        const auto right = QRotate(tv.rotation, {1, 0, 0}), up = QRotate(tv.rotation, {0, 1, 0});
        // 置いたままの傾きからの差（-π〜π）。
        roomTurn[drawnRoom->regionId] = std::remainder(std::atan2(right.y, up.y) - drawnRoom->restTurn, 2 * 3.14159265358979323846);
    }
    // 骨つきの物体で演じる装置（Play だけ）。演じた装置は焼いた部品を出さない。
    const bool actors = animate && devices_.Acquire(world);
    if (actors) { devices_.BeginFrame(); } else { devices_.Park(world); }
    for (const auto& device : snapshot.devices) {
        // バネは使えるようになった時刻も覚える（満ちた瞬間に矢印を弾ませる）。
        const bool ready = device.kind == Rules::DeviceKind::Spring && device.cooldown <= 0;
        // チェックポイントは触れて有効になった時刻も覚える（旗を上げる）。
        const bool checkpoint = device.id == snapshot.checkpointId;
        auto [memory, fresh] = deviceMemory_.try_emplace(device.id, DeviceMemory{device.powered, device.solid, ready, checkpoint});
        if (!fresh && (memory->second.powered != device.powered || memory->second.solid != device.solid || memory->second.ready != ready ||
                       memory->second.checkpoint != checkpoint)) {
            memory->second = {device.powered, device.solid, ready, checkpoint, animate ? now : -1e9};
        }
        const auto slash = device.id.find('/');
        const auto room = byRegion.find(std::string_view(device.id).substr(0, slash));
        if (slash == std::string::npos || room == byRegion.end()) { continue; }
        StagePartBuilder builder{projection_, palette, room->second->owner.value, frameParts_};
        builder.SetTime(animate && seenRooms.contains(room->first) ? now : 0.0);
        builder.DepthRows(depthRows_);
        builder.Solid(solid_, playDepth_);
        // 通関の扉は演出の秒で閉じる（プレビューでも同じ姿）。
        if (intro_ && intro_->ending && intro_->time >= intro_->closeStart) { builder.GoalClosing(intro_->time - intro_->closeStart); }
        if (const auto turn = roomTurn.find(room->first); turn != roomTurn.end()) {
            const auto& r = room->second->Region();
            builder.Turn(room->second->x + r.cols * .5, room->second->y + r.rows * .5, turn->second);
        } else {
            mirrorFor(builder, room->second);
        }
        const Rules::BeamState* beam = nullptr;
        if (device.kind == Rules::DeviceKind::Laser) {
            const auto found = std::find_if(snapshot.beams.begin(), snapshot.beams.end(), [&](const Rules::BeamState& b) { return b.sourceId == device.id; });
            if (found != snapshot.beams.end()) { beam = &*found; }
        }
        const double since = now - memory->second.changed;
        if (actors && devices_.Draw(world, builder, device, beam, checkpoint, since, seenRooms.contains(room->first),
                                    redrawnRooms_.find(room->first) != redrawnRooms_.end(), frame_)) {
            // 光線の発射器は形が変わらないので焼いた部品のまま。
            if (device.kind == Rules::DeviceKind::Laser) { builder.LaserBody(device); failed_ += builder.Failed(); }
            continue;
        }
        builder.Device(device, checkpoint, since);
        if (device.kind == Rules::DeviceKind::Laser) { builder.Beam(device, beam); }
        failed_ += builder.Failed();
    }

    if (actors) { devices_.EndFrame(world); }
    lap.emplace("parts.actors");
    // 人物・残影・予告・瞬間移動の残像：世界に一組ずつ（持ち主は舞台カメラ）。部屋の境に立つ体も一つで、
    // 両側のテレビの切り出しにそれぞれ写る。
    StagePartBuilder global{projection_, palette, cameras_[static_cast<size_t>(ViewKind::Sharp)].handle.value, frameParts_};
    global.ActorScale(actorScale_);
    Rules::BodyState idle; idle.grounded = true;
    const auto& shownPlayer = snapshot.presentationPlayer;
    // 骨つきの人形は Play だけ（編集中は焼いた静止の形）。人形の立つ役は、焼いた形を全部退避点へ置く。
    // 斜投影を焼いた組でも使う（人物は薄いので、焼いた形との斜めのずれは体の中ほどへの移しだけで合う）。
    const bool puppets = animate && puppets_.Acquire(world);
    mirrorFor(global, flippedRoomAt(shownPlayer.x + shownPlayer.w * .5, shownPlayer.y + shownPlayer.h * .5));
    global.BakedActors(!(puppets && puppets_.Has(StageActorRole::Player)));
    global.Actor(snapshot.player, snapshot.presentationPlayer, false, snapshot.realTime, snapshot.playerPresent && heroShown_, heroGrow_);
    if (puppets) { puppets_.Player(world, global, snapshot, actorScale_, projection_.ActorDepth(), heroShown_, heroGrow_); }
    global.NoMirror();
    // 残影：いる時はその姿。跳ね返りの予告中は、出る所で姿を足元から現す（0.16 秒で全高、
    // 生成のエフェクトも同時に出る）。出る所が塞がって予告が長引く間は明滅させる。
    Rules::BodyState echoBody = snapshot.echo ? *snapshot.echo : idle;
    Rules::Rect echoRect = snapshot.echo ? snapshot.echo->rect : Rules::Rect{};
    bool echoShown = snapshot.echo.has_value();
    double echoGrow = 1;
    if (!snapshot.echo && snapshot.pendingEcho) {
        echoRect = *snapshot.pendingEcho; echoBody.facing = snapshot.pendingEchoFacing;
        echoGrow = std::clamp(snapshot.pendingEchoAge / kEchoPrintSeconds, .08, 1.0);
        echoShown = snapshot.pendingEchoAge < kEchoPrintSeconds || std::fmod(snapshot.pendingEchoAge, .1) < .06;
    }
    mirrorFor(global, flippedRoomAt(echoRect.x + echoRect.w * .5, echoRect.y + echoRect.h * .5));
    global.BakedActors(!(puppets && puppets_.Has(StageActorRole::Echo)));
    global.Actor(echoBody, echoRect, true, snapshot.realTime, echoShown, echoGrow);
    if (puppets) { puppets_.Echo(world, global, echoBody, echoRect, echoShown, echoGrow, actorScale_, projection_.ActorDepth() - .05, snapshot.realTime); }
    else { puppets_.Park(world); }
    global.BakedActors(true);
    mirrorFor(global, flippedRoomAt(shownPlayer.x + shownPlayer.w * .5, shownPlayer.y + shownPlayer.h * .5));
    // 残影の印：残影がいる間、プレイヤーの頭上に菫の菱形を浮かべる（スペースで回収できる合図）。
    // 残影が別のテレビにいても分かる。
    global.Begin("echoMarker");
    const auto& body = snapshot.presentationPlayer;
    const bool marked = snapshot.playerPresent && !snapshot.player.dead && (snapshot.echo || snapshot.pendingEcho);
    // 見た目を大きくした人物の頭の上に浮かべる。
    const double head = body.y + body.h - body.h * actorScale_;
    if (marked) { global.Add("Echo_Marker", body.x + body.w * .5, head - .1 - .05 * (1 + std::sin(now * 4.0)), 1, 1, projection_.ActorDepth()); }
    else { global.Add("Echo_Marker", kParkX, kParkY, 1, 1, projection_.ActorDepth(), {}, -1, false); }
    // 回収の光跡：残影がいた所からプレイヤーへ伸びる菫の棒が、プレイヤーの方へ縮みながら消える。
    const ScreenEffect* recall = nullptr;
    for (const auto& item : worldEffects_.Items()) {
        const double age = (now - item.born) / kRecallTrailSeconds;
        if (item.kind == ScreenEffectKind::RecallTrail && age >= 0 && age < 1) { recall = &item; }
    }
    global.Begin("recallTrail");
    if (recall) {
        const double t = (now - recall->born) / kRecallTrailSeconds;
        const double sx = recall->source.x + recall->source.w * .5, sy = recall->source.y + recall->source.h * .5;
        const double ex = recall->rect.x + recall->rect.w * .5, ey = recall->rect.y + recall->rect.h * .5;
        const double tx = sx + (ex - sx) * t, ty = sy + (ey - sy) * t, length = std::hypot(ex - tx, ey - ty);
        global.Add("Echo_Streak", (tx + ex) * .5, (ty + ey) * .5, (std::max)(length, .01), (std::max)(1 - t, .05), projection_.ActorDepth() + .05,
                   RotateZ(std::atan2(-(ey - ty), ex - tx)), 0, length > .05);
    } else {
        global.Add("Echo_Streak", kParkX, kParkY, 1, 1, projection_.ActorDepth() + .05, {}, 0, false);
    }
    // 瞬間移動の残像：出発点と終点の間にプレイヤーの姿を二体、横に伸ばして置き、足元へ潰して消す。
    // 左右の向きごとに二体ずつ常に出し、使う向きだけを見せる。
    const ScreenEffect* trail = nullptr;
    if (smear) for (const auto& item : worldEffects_.Items()) {
        const double age = (now - item.born) / kSmearSeconds;
        if (item.kind == ScreenEffectKind::BlinkTrail && age >= 0 && age < 1) { trail = &item; break; }
    }
    global.Begin("smear");
    for (const bool rightward : {true, false}) for (int i = 1; i <= 2; ++i) {
        const std::string_view pose = rightward ? "Ninja_Air0_R" : "Ninja_Air0_L";
        if (!trail) { global.Add(pose, kParkX, kParkY, 1, 1, projection_.ActorDepth() - .08, {}, -1, false); continue; }
        const double t = (now - trail->born) / kSmearSeconds, k = i / 3.0;
        const double sx = trail->source.x + trail->source.w * .5, sb = trail->source.y + trail->source.h;
        const double dx = trail->rect.x + trail->rect.w * .5 - sx, db = trail->rect.y + trail->rect.h - sb, h = trail->rect.h;
        const bool used = (dx >= 0) == rightward;
        if (used) { global.Add(pose, sx + dx * k, sb + db * k, h * 1.3, (std::max)(h * (1 - t), .01), projection_.ActorDepth() - .08); }
        else { global.Add(pose, kParkX, kParkY, 1, 1, projection_.ActorDepth() - .08, {}, -1, false); }
    }
    global.NoMirror();
    failed_ += global.Failed();
    lap.emplace("parts.submit");
    for (const auto& request : frameParts_) { submit(request); }
    lap.reset();
    // オープニングの砂嵐の板：窓の 2〜4 段目に暗い・普通・明るい板を一枚ずつ。毎フレーム縦に跳ばし、ノイズを動かす。
    // 板は窓の倍の高さで、どこへ跳んでも窓を覆う。1 段目は板を置かず、キャプチャの暗い地のまま（黒い画面）。
    // 出した板は覚えておき、砂嵐が終わった後も下へ退けて出し続ける（板を出さなくなるとモデルの組が変わって GPU のシーンが
    // 作り直しになり、ゲームが始まる所で大きく引っかかる）。
    const auto keep = [&](const PartRequest& request) { submit(request); introPlanes_.push_back(request); };
    if (const auto& noise = WindowOf(ViewKind::Noise); introStatic_ && noise.valid) {
        introPlanes_.clear();
        // タイトル待ちとスペース直後の乱れの間（通関では目覚めから）は、2・3 段目に仮タイトルの板を置く（パソコンの画面の比に近い 3:2）。
        const bool title = intro_ && (intro_->ending ? intro_->time >= intro_->revealTime : intro_->time < intro_->titleOff + .3);
        // 出す板の組はいつも同じにする（タイトルの板と砂嵐の板を出し入れすると、モデルの組が変わって GPU のシーンが
        // 作り直しになり、演出の始まりで引っかかる）。使わない板は舞台カメラに写らない下へ退ける。
        const auto plane = [&](std::string_view guid, uint64_t instance, double cx, double bottom, double width, double height, bool shown) {
            PartRequest request{};
            request.ownerObjectHandle = cameras_[static_cast<size_t>(ViewKind::Sharp)].handle.value;
            request.instanceId = instance;
            std::memcpy(request.model.assetGuid, guid.data(), guid.size());
            request.worldTransform.position = projection_.Point(cx, bottom, 0);
            if (!shown) { request.worldTransform.position.y -= kIntroPlaneHide; }
            request.worldTransform.scale = {static_cast<float>(width * projection_.scale), static_cast<float>(height * projection_.scale), 1.0f};
            request.castShadow = 0; request.receiveShadow = 0; request.visible = 1;
            keep(request);
        };
        plane(kTitleCardGuid, 0x5A71C0DE20ull, noise.x + noise.cols * .5, noise.y + noise.rows, noise.cols * .5, noise.rows, title);
        // 真っ白：黒い段の下の方。光の点：黒い段の上の小さな白い点。
        plane(kWhitePlaneGuid, 0x5A71C0DE11ull, noise.x + noise.cols * .125, noise.y + noise.rows, noise.cols * .24, noise.rows * .4, true);
        plane(kWhitePlaneGuid, 0x5A71C0DE12ull, noise.x + noise.cols * .125, noise.y + noise.rows * (kNoiseDot + .01), noise.cols * .012, noise.rows * .02, true);
        for (size_t band = 1; band <= kStaticPlaneGuids.size(); ++band) {
            // タイトルの間の 2・3 段目はタイトルの板が映るので、砂嵐の板は退けておく。
            const bool shown = !(title && band <= 2);
            const auto guid = kStaticPlaneGuids[band - 1];
            PartRequest request{};
            request.ownerObjectHandle = cameras_[static_cast<size_t>(ViewKind::Sharp)].handle.value;
            request.instanceId = 0x5A71C0DE00ull + band;
            std::memcpy(request.model.assetGuid, guid.data(), guid.size());
            // 吸い込まれる間は板を縦に引き伸ばして速く流し、砂嵐が筋になって流れる（信号の中を落ちていく感じ）。
            const double pull = intro_ ? std::clamp((intro_->time - intro_->pullStart) / (std::max)(.1f, intro_->blackTime - intro_->pullStart), 0.0, 1.0) : 0.0;
            const double stretch = 1 + 7 * pull * pull, height = noise.rows * 2 * stretch;
            const double jump = pull > 0 ? std::fmod(intro_->time * (3 + 14 * pull) + band * .37, 1.0) : IntroHash(frame_, static_cast<uint32_t>(band));
            const double cx = noise.x + noise.cols * (band * .25 + .125);
            // 板の下端は窓の下端から（板の高さ − 窓の高さ）までの間。どこにあっても窓を覆う。
            const double bottom = noise.y + noise.rows + (height - noise.rows) * jump;
            request.worldTransform.position = projection_.Point(cx, bottom, 0);
            if (!shown) { request.worldTransform.position.y -= kIntroPlaneHide; }
            request.worldTransform.scale = {static_cast<float>(noise.cols * .25 * projection_.scale), static_cast<float>(height * projection_.scale), 1.0f};
            request.castShadow = 0; request.receiveShadow = 0; request.visible = 1;
            keep(request);
        }
        // 消える瞬間の線：黒い段（1 段目）の真ん中に、明るい砂嵐を細く潰した板を一枚。
        PartRequest line{};
        line.ownerObjectHandle = cameras_[static_cast<size_t>(ViewKind::Sharp)].handle.value;
        line.instanceId = 0x5A71C0DE10ull;
        std::memcpy(line.model.assetGuid, kStaticPlaneGuids[2].data(), kStaticPlaneGuids[2].size());
        line.worldTransform.position = projection_.Point(noise.x + noise.cols * .125, noise.y + noise.rows * (.5 + kNoiseLine * .5), 0);
        line.worldTransform.scale = {static_cast<float>(noise.cols * .24 * projection_.scale), static_cast<float>(noise.rows * kNoiseLine * projection_.scale), 1.0f};
        line.castShadow = 0; line.receiveShadow = 0; line.visible = 1;
        keep(line);
    } else {
        for (auto request : introPlanes_) { request.worldTransform.position.y -= kIntroPlaneHide; submit(request); }
    }
    // 舞台の部品が多すぎる時は一度だけ知らせる（GPU のシーンの面の上限に近づく）。
    if (static bool warned = false; !warned && accepted_ > kPartWarning) {
        std::fprintf(stderr, "[TD2 Screen3D] stage parts=%zu exceed %zu; the engine frame may reject drawing\n", accepted_, kPartWarning);
        warned = true;
    }
}
}
