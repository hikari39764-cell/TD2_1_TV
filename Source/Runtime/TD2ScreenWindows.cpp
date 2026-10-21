#include "TD2Screen3D.h"
#include "TD2Screen3DShared.h"
#include <algorithm>
#include <cmath>

namespace TD2 {
namespace {
using namespace Stage3D;
// 窓の配置：鮮明・大区域・遠くの二組と、編集中の一枚の窓。
// 1 マスの texel 数の上限。部屋のマスが少なく画面の中で 1 マスが大きいので 48（ピクセルの描き方では 3×3 texel が
// 1 ゲーム画素）。組の窓は組の大きさに応じてこれより粗くなる。
constexpr int kSharpTexelsPerCell = 48;
// 透視の窓は部屋の周りに余白を取る（目を動かしても部屋が像に収まる）。窓の行数と列数に掛ける。組の窓は部屋が多いぶん小さめ。
constexpr double kLensMargin = 1.5, kGroupMargin = 1.25;
// 組の窓のキャプチャの大きさ（窓の種類ごとに固定。大きさを変えると数フレーム映らない）。大区域は数部屋を収める横長、
// 遠くの二組は関卡の並びに合わせた横長と、離れた小さな組用の小さめ。鮮明は部屋の大きさで決まる（ここでは使わない）。
constexpr std::array<std::array<uint32_t, 2>, 4> kGroupCapture{{{0u, 0u}, {2048u, 1280u}, {1536u, 512u}, {1024u, 512u}}};
// 大区域の窓の 1 マスの texel（窓の大きさが決まる）。操作中の部屋と、それに接する部屋（次に入る部屋・いま出た部屋）が
// 余白込みで必ず収まる細かさ（今の地図では横に約 38 マス要る。2048 / (38 × 1.25) ≒ 43）。収まらない隣が遠くの窓へ回ると、
// 部屋を移る瞬間に奥行きと角度が跳ぶ。鮮明の透視カメラ（11381、19°）はこの窓とほぼ同じ距離から撮る（差は 1 割未満）。
constexpr double kAreaTexels = 42;
// 遠くの窓に入れるテレビの数の上限、写っているとみなす画の割合、組の行数の上限（超える離れた部屋は入れない）。
constexpr size_t kFarRooms = 12;
constexpr double kFarVisible = .0002, kFarMaxRows = 128;
// 遠くの組の細かさ（1 マスの texel）：足しても kFarGoodTexels を保てる組が無ければ空いた組に分ける。画面ごとに要る細かさは
// 画面上の画素だが、kFarMinTexels より粗くはしない。
constexpr double kFarGoodTexels = 20, kFarMinTexels = 8;
// 窓のキャプチャの大きさを決める。行数はカメラの正投影の高さ（マス）で固定し、列数は縦横比から求め直す。
// 2048 texel を超える時は精細度を下げる。
void FitWindow(double rows, double wantedCols, int density, double& outCols, int& outDensity, uint32_t& width, uint32_t& height) {
    const double limit = HIKARI::kGameProjectSceneCaptureMaximumExtent;
    outDensity = (std::max)(1, (std::min)(density, static_cast<int>(std::floor(limit / (std::max)(rows, wantedCols)))));
    height = static_cast<uint32_t>(std::clamp(std::lround(rows * outDensity), 1l, static_cast<long>(limit)));
    // 列数は幅と高さの比で決まる。丸めで列が足りないと両端の部屋が入らず黒い画面になるので、
    // 幅は欲しい列数を必ず覆うよう切り上げる。
    width = static_cast<uint32_t>(std::clamp(static_cast<long>(std::ceil(wantedCols * height / rows - 1e-6)), 1l, static_cast<long>(limit)));
    outCols = rows * width / height;
}
// 部屋の組（と extra）の外接（マス。左・上・右・下）。
std::array<double, 4> GroupBox(const std::vector<const StageRoom*>& rooms, const StageRoom* extra) {
    std::array<double, 4> box{};
    bool any = false;
    const auto add = [&](const StageRoom* room) {
        const double l = room->x, t = room->y, r = room->x + room->Region().cols, b = room->y + room->Region().rows;
        if (!any) { box = {l, t, r, b}; any = true; return; }
        box = {(std::min)(box[0], l), (std::min)(box[1], t), (std::max)(box[2], r), (std::max)(box[3], b)};
    };
    for (const auto* room : rooms) { add(room); }
    if (extra) { add(extra); }
    return box;
}
// 外接を width×height のキャプチャの透視の窓に余白付きで収めた時の行数（1 マスの texel は鮮明の窓より細かくしない）。
double GroupRows(const std::array<double, 4>& box, uint32_t width, uint32_t height) {
    const double aspect = double(width) / height;
    return (std::max)({(box[3] - box[1]) * kGroupMargin, (box[2] - box[0]) * kGroupMargin / aspect, double(height) / kSharpTexelsPerCell});
}
}

std::array<double, 2> Screen3DStage::EyeOffset(const Window& window, const std::array<double, 4>& box,
                                              const std::vector<const StageRoom*>& members) const {
    // 目の位置：部屋ごとに「外のカメラと同じ角度でその部屋を見る目」（部屋の中心から、見る角度の正接 × 目までの距離だけ
    // ずれた所）を求め、写っている割合で平均する（回された部屋は角度が逆向き）。一部屋なら角度どおり、組なら組の部屋が
    // それぞれの角度に近く見える一つの目になる。写っていなければ外接の正面。
    const double cx = (box[0] + box[2]) * .5, cy = (box[1] + box[3]) * .5;
    const double reach = window.distance / projection_.scale;   // 目から正面の面まで（マス）
    double dx = 0, dy = 0, weight = 0;
    for (const auto* room : members) {
        const auto seen = seen_.find(room->regionId);
        const auto eye = parallax_.find(room->regionId);
        if (seen == seen_.end() || eye == parallax_.end() || seen->second.coverage <= 0) { continue; }
        const double k = seen->second.coverage, sign = room->flipped ? -1.0 : 1.0;
        const auto& r = room->Region();
        // 地図の y は下向き、角度の正接は上向き。
        dx += k * (room->x + r.cols * .5 - cx + reach * sign * eye->second[0] * parallaxGain_);
        dy += k * (cy - (room->y + r.rows * .5) + reach * sign * eye->second[1] * parallaxGain_);
        weight += k;
    }
    if (weight > 0) { dx /= weight; dy /= weight; }
    return {dx, dy};
}

void Screen3DStage::AimWindow(Window& window, const std::array<double, 4>& box, std::array<double, 2> offset) const {
    const double w = box[2] - box[0], h = box[3] - box[1];
    const double dx = offset[0], dy = offset[1];
    // 透視の窓：余白を取った窓の中心（目の真正面）をその目の位置へずらす。正面の面（判定の面）はどこから見ても
    // 切り出しにぴったり写り、奥ほど目の側へずれて見える。余白の端へ近づくほど緩やかに止める。
    const auto soft = [](double value, double limit) { return limit > 0 ? limit * std::tanh(value / limit) : 0.0; };
    const double ox = soft(dx, (window.cols - w) * .5 - .25);
    const double oy = soft(dy, (window.rows - h) * .5 - .25);
    // 切り出しの左上をゲーム画素の格子に合わせる（ピクセルの描き方で絵が揺らがない）。
    const double pixel = double((std::max)(1, window.density / kGamePixelsPerCell)) / window.density;
    const double left = std::round((window.cols * .5 - w * .5 - ox) / pixel) * pixel;
    const double top = std::round((window.rows * .5 - h * .5 + oy) / pixel) * pixel;
    window.x = box[0] - left; window.y = box[1] - top;
}

void Screen3DStage::PlaceWindows(const std::vector<const StageRoom*>& rooms, const StageFrame& frame, int maxCols) {
    windows_ = {};
    for (auto& members : members_) { members.clear(); }
    const StageRoom* active = nullptr;
    for (const auto* room : rooms) { if (room->placed && room->regionId == frame.activeRegion) { active = room; } }
    if (!active) { for (const auto* room : rooms) { if (room->placed) { active = room; break; } } }
    if (!active) { return; }
    // 大区域が替わったら遠くの組を選び直す。同じ大区域の間は遠くの組に入れた部屋を外さない（部屋の出し入れのたびに
    // 部品と画面の割り当てを作り直すと、エンジンが GPU のシーンを作り直して画が止まる）。大区域の窓の目は替わっても
    // そのまま新しい行き先へ移す（跨ぐ瞬間に画面の角度が跳ばない）。
    const std::string area = frame.areaRooms.empty() ? frame.activeRegion : frame.areaRooms.front();
    if (area != groupArea_) {
        groupArea_ = area; groupEyeValid_ = {}; farSeen_.clear(); farGroup_.clear(); farNeed_.clear();
    }
    // 大区域：操作中の部屋の周りの同じ镜头大区域の部屋。
    PlaceArea(rooms, active, frame);
    // 鮮明：操作中の部屋だけを中央に撮る。
    const auto& camera = cameras_[static_cast<size_t>(ViewKind::Sharp)];
    const auto& lens = lensCameras_[static_cast<size_t>(ViewKind::Sharp)];
    if (camera.handle.IsValid()) {
        auto& window = windows_[static_cast<size_t>(ViewKind::Sharp)];
        const auto& r = active->Region();
        window.regionId = active->regionId;
        window.valid = true;
        window.rows = camera.orthographicHeight / projection_.scale;
        if (!perspective_ || !lens.handle.IsValid()) {
            FitWindow(window.rows, maxCols, kSharpTexelsPerCell, window.cols, window.density, window.width, window.height);
            window.x = active->x + r.cols * .5 - window.cols * .5; window.y = active->y + r.rows * .5 - window.rows * .5;
        } else {
            window.rows = std::ceil(window.rows * kLensMargin);
            FitWindow(window.rows, maxCols * kLensMargin, kSharpTexelsPerCell, window.cols, window.density, window.width, window.height);
            window.distance = window.rows * projection_.scale * .5 / std::tan(lens.verticalFov * .5);
            window.lens = static_cast<int>(ViewKind::Sharp);
            // 目：大区域の窓があればその目から見た角度で撮る（同じ大区域のテレビが一つの目で舞台を見るので、テレビが窓を
            // 移っても角度が跳ばない）。回された部屋と大区域の窓が無い時は、部屋を外のカメラが見る角度そのもの。
            const std::array<double, 4> box{double(active->x), double(active->y), double(active->x + r.cols), double(active->y + r.rows)};
            const auto& group = WindowOf(ViewKind::Area);
            std::array<double, 2> offset{};
            if (group.valid && group.lens >= 0 && !active->flipped) {
                const double reachGroup = group.distance / projection_.scale, reach = window.distance / projection_.scale;
                const double tx = (group.x + group.cols * .5 - (active->x + r.cols * .5)) / reachGroup;
                const double ty = ((active->y + r.rows * .5) - (group.y + group.rows * .5)) / reachGroup;
                offset = {reach * tx, reach * ty};
            } else {
                offset = EyeOffset(window, box, {active});
            }
            AimWindow(window, box, offset);
        }
    }
}

void Screen3DStage::PlaceArea(const std::vector<const StageRoom*>& rooms, const StageRoom* active, const StageFrame& frame) {
    const auto index = static_cast<size_t>(ViewKind::Area);
    auto& window = windows_[index];
    window = {};
    members_[index].clear();
    // 候補：同じ镜头大区域の部屋と、世界地図でそれに接する部屋（次の大区域の入口など）、画に写っているほかの部屋
    // （大区域を移った直後の前の大区域の部屋など）。預け場所の部屋は除く。収まるかどうかは下で目立つ順に決める。
    const auto parked = [](const StageRoom* room) { return room->x <= -50000 || room->y <= -50000; };
    const auto inArea = [&](const StageRoom* room) {
        return room == active || std::find(frame.areaRooms.begin(), frame.areaRooms.end(), room->regionId) != frame.areaRooms.end();
    };
    // 二部屋の外周の間のマス（重なり・接触は 0）。
    const auto gap = [](const StageRoom* a, const StageRoom* b) {
        const double gx = (std::max)({0.0, double(a->x - (b->x + b->Region().cols)), double(b->x - (a->x + a->Region().cols))});
        const double gy = (std::max)({0.0, double(a->y - (b->y + b->Region().rows)), double(b->y - (a->y + a->Region().rows))});
        return (std::max)(gx, gy);
    };
    std::vector<const StageRoom*> own, candidates;
    for (const auto* room : rooms) { if (room->placed && !parked(room) && inArea(room)) { own.push_back(room); } }
    for (const auto* room : rooms) {
        if (!room->placed || parked(room) || room == active) { continue; }
        const auto seen = seen_.find(room->regionId);
        const bool visible = seen != seen_.end() && seen->second.coverage > kFarVisible;
        if (visible || inArea(room) || std::any_of(own.begin(), own.end(), [&](const StageRoom* a) { return gap(a, room) <= 1; })) {
            candidates.push_back(room);
        }
    }
    const auto [width, height] = kGroupCapture[index];
    const auto& lens = lensCameras_[index];
    // 透視カメラが無い時（透視の窓を切った時）は正投影で候補の外接を撮る。
    if (!lens.handle.IsValid()) { candidates.push_back(active); PlaceGroup(ViewKind::Area, candidates, width, height); return; }
    // 窓の大きさは固定（1 マス kAreaTexels）。
    window.rows = height / kAreaTexels; window.cols = window.rows * width / height;
    window.width = width; window.height = height;
    window.density = static_cast<int>(kAreaTexels);
    window.lens = static_cast<int>(index);
    window.distance = window.rows * projection_.scale * .5 / std::tan(lens.verticalFov * .5);
    const double innerW = window.cols / kGroupMargin, innerH = window.rows / kGroupMargin;
    // 目を向ける組：操作中の部屋、接する部屋（次に入る・いま出た部屋）、画に写っている割合の大きい部屋の順に、余白
    // （kGroupMargin）を残した内側に収まるだけ。前のフレームに入れていた部屋は 1.5 倍に数える。
    const auto score = [&](const StageRoom* room) {
        const auto seen = seen_.find(room->regionId);
        const bool was = std::find(areaMembers_.begin(), areaMembers_.end(), room->regionId) != areaMembers_.end();
        return (seen == seen_.end() ? 0.0 : seen->second.coverage) * (was ? 1.5 : 1.0);
    };
    std::stable_sort(candidates.begin(), candidates.end(), [&](const StageRoom* a, const StageRoom* b) {
        const bool na = gap(active, a) <= 0, nb = gap(active, b) <= 0;
        return na != nb ? na : score(a) > score(b);
    });
    std::vector<const StageRoom*> wanted{active};
    auto box = GroupBox(wanted, nullptr);
    for (const auto* room : candidates) {
        const auto next = GroupBox(wanted, room);
        if (next[2] - next[0] > innerW || next[3] - next[1] > innerH) { continue; }
        wanted.push_back(room); box = next;
    }
    // 目＝窓の中心（地図のマス。y は下向き。透視カメラは窓の中心の正面にいる）。行き先は、組を外のカメラと同じ角度で
    // 見る位置（EyeOffset）を、組が窓に収まる範囲へ寄せた所。目は行き先へ視差と同じ速さで移す。組が替わって外接が
    // 跳んでも目は跳ばないので、窓の中の画面（操作中の部屋と、いま出た部屋）の角度は連続する。操作中の部屋は必ず窓に入れる。
    const auto offset = EyeOffset(window, box, wanted);
    const auto soft = [](double value, double limit) { return limit > 0 ? limit * std::tanh(value / limit) : 0.0; };
    const std::array<double, 2> target{
        (box[0] + box[2]) * .5 + soft(offset[0], (window.cols - (box[2] - box[0])) * .5 - .25),
        (box[1] + box[3]) * .5 - soft(offset[1], (window.rows - (box[3] - box[1])) * .5 - .25)};
    if (!areaEyeValid_) { areaEye_ = target; areaEyeValid_ = true; }
    else { for (size_t k = 0; k < 2; ++k) { areaEye_[k] += (target[k] - areaEye_[k]) * parallaxFollow_; } }
    const auto& r = active->Region();
    const auto keep = [](double value, double low, double high) { return low <= high ? std::clamp(value, low, high) : (low + high) * .5; };
    areaEye_[0] = keep(areaEye_[0], active->x + r.cols - window.cols * .5, active->x + window.cols * .5);
    areaEye_[1] = keep(areaEye_[1], active->y + r.rows - window.rows * .5, active->y + window.rows * .5);
    // 切り出しの左上をゲーム画素の格子に合わせる（ピクセルの描き方で絵が揺らがない）。丸めで操作中の部屋がはみ出さないよう、
    // 格子の内側へ寄せる。
    const double pixel = double((std::max)(1, window.density / kGamePixelsPerCell)) / window.density;
    const auto snap = [&](double value, double low, double high) {
        value = std::round(value / pixel) * pixel;
        if (value < low) { value = std::ceil(low / pixel) * pixel; }
        if (value > high) { value = std::floor(high / pixel) * pixel; }
        return value;
    };
    window.x = snap(areaEye_[0] - window.cols * .5, active->x + r.cols - window.cols, active->x);
    window.y = snap(areaEye_[1] - window.rows * .5, active->y + r.rows - window.rows, active->y);
    // 窓に入れる部屋：窓にまるごと写っている候補の部屋（と操作中の部屋）。新しく入れるのは縁から半マス内側に収まってから
    // （目が動く間に窓の縁で出入りを繰り返さない）。目が行き先へ着くまで、組の端の部屋は遠くの窓のままのことがある。
    const auto inside = [&](const StageRoom* room, double slack) {
        const auto& g = room->Region();
        return room->x >= window.x + slack && room->x + g.cols <= window.x + window.cols - slack &&
               room->y >= window.y + slack && room->y + g.rows <= window.y + window.rows - slack;
    };
    std::vector<std::string> members{active->regionId};
    for (const auto* room : candidates) {
        const bool was = std::find(areaMembers_.begin(), areaMembers_.end(), room->regionId) != areaMembers_.end();
        if (inside(room, was ? 0.0 : .5)) { members.push_back(room->regionId); }
    }
    areaMembers_ = members;
    members_[index] = std::move(members);
    window.valid = true;
}

void Screen3DStage::PlaceGroup(ViewKind kind, const std::vector<const StageRoom*>& members, uint32_t width, uint32_t height) {
    const auto index = static_cast<size_t>(kind);
    auto& window = windows_[index];
    window = {};
    if (members.empty()) { return; }
    for (const auto* room : members) { members_[index].push_back(room->regionId); }
    const auto box = GroupBox(members, nullptr);
    const auto& lens = lensCameras_[index];
    if (!lens.handle.IsValid()) {
        // 透視カメラが無い時（透視の窓を切った時）：同じ種類の正投影カメラで外接の中心を撮る。行数はカメラの高さで決まる。
        if (index >= kOrthoKinds || !cameras_[index].handle.IsValid()) { return; }
        window.rows = cameras_[index].orthographicHeight / projection_.scale;
        FitWindow(window.rows, box[2] - box[0], kSharpTexelsPerCell, window.cols, window.density, window.width, window.height);
        window.x = (box[0] + box[2]) * .5 - window.cols * .5; window.y = (box[1] + box[3]) * .5 - window.rows * .5;
        window.valid = true;
        return;
    }
    window.rows = GroupRows(box, width, height);
    window.cols = window.rows * width / height;
    window.width = width; window.height = height;
    window.density = (std::max)(1, static_cast<int>(std::floor(height / window.rows)));
    window.lens = static_cast<int>(index);
    window.distance = window.rows * projection_.scale * .5 / std::tan(lens.verticalFov * .5);
    // 目（地図のマス。y は下向き）の行き先は組が替わった時だけ決め直す（背景の小さな画面は視差が見えず、手持ちの揺れで
    // 毎フレーム動かすと画面の中がちらつく）。目は行き先へ視差と同じ速さで移す（決め直しで跳ばすと、組の画面がそろって
    // 角度を変える）。
    const std::array<double, 2> center{(box[0] + box[2]) * .5, (box[1] + box[3]) * .5};
    if (!groupEyeValid_[index] || groupEyeBox_[index] != box) {
        const auto offset = EyeOffset(window, box, members);
        groupTarget_[index] = {center[0] + offset[0], center[1] - offset[1]};
        groupEyeBox_[index] = box;
        if (!groupEyeValid_[index]) { groupEye_[index] = groupTarget_[index]; groupEyeValid_[index] = true; }
    }
    auto& eye = groupEye_[index];
    for (size_t k = 0; k < 2; ++k) {
        eye[k] += (groupTarget_[index][k] - eye[k]) * parallaxFollow_;
        if (std::abs(groupTarget_[index][k] - eye[k]) < 1e-3) { eye[k] = groupTarget_[index][k]; }
    }
    AimWindow(window, box, {eye[0] - center[0], center[1] - eye[1]});
    window.valid = true;
}

void Screen3DStage::PlaceFar(const std::vector<const StageRoom*>& rooms) {
    for (const auto kind : kFarKinds) { windows_[static_cast<size_t>(kind)] = {}; members_[static_cast<size_t>(kind)].clear(); }
    if (!lensCameras_[static_cast<size_t>(ViewKind::Far)].handle.IsValid() || seen_.empty()) { return; }
    // 鮮明と大区域の窓に入る部屋は数えない。
    const auto& areaMembers = members_[static_cast<size_t>(ViewKind::Area)];
    const auto covered = [&](const StageRoom* room) {
        return WindowOf(ViewKind::Sharp).regionId == room->regionId ||
               std::find(areaMembers.begin(), areaMembers.end(), room->regionId) != areaMembers.end();
    };
    // 入れるかどうかは画に写っている割合だけで決め、一度写った部屋は大区域の間は残す（遮りは筐体の見積もりの箱で
    // 数えるので、見えている画面を隠れていると数えることがある。外すと黒く映る）。並べる順は今の画面上の 1 マスの画素
    // （下の組の細かさの条件と同じ物差し。大きく写る画面から組に入れ、小さな画面に組を取られない）。
    std::vector<std::pair<double, const StageRoom*>> wanted;
    for (const auto* room : rooms) {
        if (!room->placed || covered(room)) { continue; }
        const auto found = seen_.find(room->regionId);
        const bool visible = found != seen_.end() && found->second.coverage > kFarVisible;
        if (visible) { farSeen_.insert(room->regionId); }
        if (!farSeen_.contains(room->regionId)) { continue; }
        wanted.emplace_back(visible ? CellPixels(*room) : 0.0, room);
    }
    std::stable_sort(wanted.begin(), wanted.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    // 目立つ部屋から順に組へ振り分ける。部屋を足した組の細かさは、組のどの部屋の画面上の画素（kFarGoodTexels まで、
    // kFarMinTexels 以上）も下回ってはならない（後から来た小さな画面が目立つ画面をぼかさない）。前に入れた組は自分の
    // 分だけ少し粗くなっても続けて使う（割り当てを作り直すと画が止まる）。新しい部屋は kFarGoodTexels を保てる一番細かい
    // 組へ、無ければ空いた組へ、空きも無ければ収まる一番細かい組へ。どこにも収まらない画面は映さない。
    // 要る細かさは上がる時はすぐ、下がる時はゆっくり追う（手持ちの揺れで境目を行き来しない）。
    const auto usable = [&](size_t k) { return lensCameras_[static_cast<size_t>(kFarKinds[k])].handle.IsValid(); };
    const auto capture = [](size_t k) { return kGroupCapture[static_cast<size_t>(kFarKinds[k])]; };
    std::array<std::vector<const StageRoom*>, kFarKinds.size()> groups;
    for (const auto& [value, room] : wanted) {
        const double now = std::clamp(CellPixels(*room), kFarMinTexels, kFarGoodTexels);
        auto& held = farNeed_[room->regionId];
        held = (std::max)(now, held * .998);
    }
    const auto need = [&](const StageRoom* room) { return farNeed_[room->regionId]; };
    // 部屋を足した組の細かさ（1 マスの texel）。足す部屋の要る細かさに slack を掛けた値か、組の部屋の要る細かさを
    // 下回れば 0。
    const auto texelsWith = [&](size_t k, const StageRoom* room, double slack) {
        auto members = groups[k];
        members.push_back(room);
        const double rows = GroupRows(GroupBox(members, nullptr), capture(k)[0], capture(k)[1]);
        double wanted = need(room) * slack;
        for (const auto* member : groups[k]) { wanted = (std::max)(wanted, need(member)); }
        const double texels = capture(k)[1] / rows;
        return rows <= kFarMaxRows && texels >= wanted ? texels : 0.0;
    };
    size_t count = 0;
    for (const auto& [value, room] : wanted) {
        if (count == kFarRooms) { break; }
        size_t pick = kFarKinds.size();
        if (const auto old = farGroup_.find(room->regionId); old != farGroup_.end() && usable(old->second) && texelsWith(old->second, room, .75) > 0) {
            pick = old->second;
        } else {
            double best = 0;
            size_t empty = kFarKinds.size();
            for (size_t k = 0; k < kFarKinds.size(); ++k) {
                if (!usable(k)) { continue; }
                if (groups[k].empty()) { if (empty == kFarKinds.size()) { empty = k; } continue; }
                if (const double texels = texelsWith(k, room, 1); texels > best) { best = texels; pick = k; }
            }
            if (best < kFarGoodTexels && empty != kFarKinds.size()) { pick = empty; }
        }
        if (pick == kFarKinds.size()) { continue; }
        groups[pick].push_back(room);
        farGroup_[room->regionId] = pick;
        ++count;
    }
    for (size_t k = 0; k < kFarKinds.size(); ++k) { PlaceGroup(kFarKinds[k], groups[k], capture(k)[0], capture(k)[1]); }
}

double Screen3DStage::CellPixels(const StageRoom& room) const {
    const auto seen = seen_.find(room.regionId);
    const auto& r = room.Region();
    if (seen == seen_.end() || r.cols <= 0 || r.rows <= 0) { return 0; }
    return std::sqrt(seen->second.coverage * 1920.0 * 1080.0 / (double(r.cols) * r.rows));
}

void Screen3DStage::PlaceNoise() {
    // 遠くの窓の透視カメラを借り、舞台の外のノイズの板を撮る。オープニングが終わると遠くの窓は選び直す。
    for (const auto kind : kFarKinds) { windows_[static_cast<size_t>(kind)] = {}; members_[static_cast<size_t>(kind)].clear(); }
    farSeen_.clear(); farGroup_.clear(); farNeed_.clear(); groupEyeValid_ = {}; groupArea_.clear();
    auto& noise = windows_[static_cast<size_t>(ViewKind::Far)];
    const auto& camera = lensCameras_[static_cast<size_t>(ViewKind::Far)];
    const auto [width, height] = kGroupCapture[static_cast<size_t>(ViewKind::Far)];
    if (!camera.handle.IsValid()) { introStatic_ = false; return; }
    noise.rows = kNoiseRows; noise.cols = kNoiseRows * double(width) / height;
    noise.x = kNoiseX - noise.cols * .5; noise.y = kNoiseY - noise.rows * .5;
    noise.width = width; noise.height = height;
    noise.density = (std::max)(1, static_cast<int>(height / noise.rows));
    noise.lens = static_cast<int>(ViewKind::Far);
    noise.distance = noise.rows * projection_.scale * .5 / std::tan(camera.verticalFov * .5);
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
