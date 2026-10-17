#pragma once
#include "Rules/TD2Exploration.h"
#include "HIKARI/Project/GameComponent.h"
#include <algorithm>
#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace TD2 {
inline constexpr std::string_view kMechanismComponentType = "TD2.Mechanism";
inline constexpr std::string_view kSlideRackComponentType = "TD2.SlideRack";
inline constexpr std::string_view kStackColumnComponentType = "TD2.StackColumn";
inline constexpr std::string_view kCarouselComponentType = "TD2.Carousel";

// 仕掛けの書き方の約束：位置はなるべく「作者が置いた今の配置」から求める。
// 区域や物体を編集器で動かしても仕掛けが壊れないよう、絶対の座標は旧形式としてだけ読む。

// 画面の外の機構（転台・吊り台・天平など）。圧力板の targets に「mech/機構ID」と書くと、その通電で状態が変わる。
// 状態 0 はシーンに置いたままの配置、状態 1 と（天平の）状態 -1 は下の表で書く。
//   区域のずれ：  区域ID dX dY [flip] [@錨の区域ID]
//                作者が置いた位置からのずれ（マス）。錨を書くとその区域の位置からのずれ（遠くの区域の隣へ運ぶ時）。
//                区域ID park と書くと、その状態では世界地図のどことも重ならない預け場所へ離す（奥へ下がる・吊り上げるなど）。
//   物体のずれ：  物体ID dx dy dz rx ry rz [cx cy cz] [@錨の物体ID]
//                基準（状態 A の姿勢、錨を書けばその物体）の向きで測った位置のずれ（m）と、基準に後から掛ける回転（度）。
//                cx cy cz は回す中心（基準の向きで測った、基準の位置からのずれ）。書くとその点を中心に回す。
//   旧形式（絶対）：区域ID 世界X 世界Y [flip] / 物体ID px py pz rx ry rz [cx cy cz]
// 動かした区域のテレビに付いた区域カメラは、テレビと一緒に動く（その場で画面の向きに回すだけの時は動かさない）。
struct MechanismObjectMove {
    uint64_t object = 0;
    float position[3]{}, rotation[3]{};
    bool hasPivot = false;
    float pivot[3]{};
};
struct MechanismRegionOffset { std::string regionId, anchor; int dx = 0, dy = 0; bool flip = false, park = false; };
struct MechanismObjectDelta {
    uint64_t object = 0, anchor = 0;
    float position[3]{}, rotation[3]{};
    bool hasPivot = false;
    float pivot[3]{};
};
// 部品から読んだままの機構（相対の行は Session が今の配置で絶対へ直してから規則へ渡す）。
struct MechanismAuthoring {
    Rules::MechanismSource source;                       // 旧形式の区域の動きはここへ直接入る
    std::vector<MechanismRegionOffset> offsets, offsetsC;
    std::vector<MechanismObjectMove> objects, objectsC;   // 旧形式（絶対）
    std::vector<MechanismObjectDelta> deltas, deltasC;
};

namespace Detail {
// 一行の欄でも書けるよう、「;」も行の区切りとして読む。
inline std::string Lines(std::string text) { std::replace(text.begin(), text.end(), ';', '\n'); return text; }
inline bool ParseRegionMoves(const std::string& text, std::vector<Rules::MechanismMove>& out, std::string& error) {
    std::istringstream lines(Lines(text));
    std::string line;
    while (std::getline(lines, line)) {
        std::istringstream values(line);
        Rules::MechanismMove move;
        if (!(values >> move.regionId)) continue;
        if (!(values >> move.worldX >> move.worldY)) { error = "区域行需要：区域ID X Y [flip]"; return false; }
        std::string option;
        if (values >> option) {
            if (option != "flip") { error = "区域行的第四项只能是 flip"; return false; }
            move.flip = true;
        }
        out.push_back(std::move(move));
    }
    return true;
}
inline bool ParseObjectMoves(const std::string& text, std::vector<MechanismObjectMove>& out, std::string& error) {
    std::istringstream lines(Lines(text));
    std::string line;
    while (std::getline(lines, line)) {
        std::istringstream values(line);
        MechanismObjectMove move;
        if (!(values >> move.object)) continue;
        if (!(values >> move.position[0] >> move.position[1] >> move.position[2] >> move.rotation[0] >> move.rotation[1] >> move.rotation[2])) {
            error = "物体行需要：物体ID px py pz rx ry rz [cx cy cz]"; return false;
        }
        move.hasPivot = static_cast<bool>(values >> move.pivot[0] >> move.pivot[1] >> move.pivot[2]);
        out.push_back(move);
    }
    return true;
}
inline bool ParseRegionOffsets(const std::string& text, std::vector<MechanismRegionOffset>& out, std::string& error) {
    std::istringstream lines(Lines(text));
    std::string line;
    while (std::getline(lines, line)) {
        std::istringstream values(line);
        MechanismRegionOffset offset;
        if (!(values >> offset.regionId)) continue;
        std::string first;
        if (!(values >> first)) { error = "区域行需要：区域ID dX dY [flip] [@锚点区域] 或 区域ID park"; return false; }
        if (first == "park") { offset.park = true; out.push_back(std::move(offset)); continue; }
        try { offset.dx = std::stoi(first); } catch (...) { error = "区域行的 dX 需要整数"; return false; }
        if (!(values >> offset.dy)) { error = "区域行需要：区域ID dX dY [flip] [@锚点区域]"; return false; }
        std::string option;
        while (values >> option) {
            if (option == "flip") offset.flip = true;
            else if (option.size() > 1 && option[0] == '@') offset.anchor = option.substr(1);
            else { error = "区域行的附加项只能是 flip 或 @锚点区域"; return false; }
        }
        out.push_back(std::move(offset));
    }
    return true;
}
inline bool ParseObjectDeltas(const std::string& text, std::vector<MechanismObjectDelta>& out, std::string& error) {
    std::istringstream lines(Lines(text));
    std::string line;
    while (std::getline(lines, line)) {
        std::istringstream values(line);
        MechanismObjectDelta delta;
        if (!(values >> delta.object)) continue;
        if (!(values >> delta.position[0] >> delta.position[1] >> delta.position[2] >> delta.rotation[0] >> delta.rotation[1] >> delta.rotation[2])) {
            error = "物体行需要：物体ID dx dy dz rx ry rz [cx cy cz] [@锚点物体]"; return false;
        }
        std::vector<std::string> rest;
        for (std::string token; values >> token;) rest.push_back(token);
        size_t i = 0;
        if (rest.size() >= 3 && rest[0][0] != '@') {
            try { for (int k = 0; k < 3; ++k) delta.pivot[k] = std::stof(rest[static_cast<size_t>(k)]); }
            catch (...) { error = "物体行的转动中心需要三个数"; return false; }
            delta.hasPivot = true; i = 3;
        }
        if (i < rest.size()) {
            if (rest[i].size() < 2 || rest[i][0] != '@') { error = "物体行的末尾只能是 @锚点物体"; return false; }
            try { delta.anchor = std::stoull(rest[i].substr(1)); } catch (...) { error = "锚点物体需要物体 ID"; return false; }
            ++i;
        }
        if (i != rest.size()) { error = "物体行有多余的项"; return false; }
        out.push_back(delta);
    }
    return true;
}
inline std::vector<std::string> ParseNames(std::string text) {
    std::replace(text.begin(), text.end(), ',', '\n');
    std::vector<std::string> names;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) { std::istringstream value(line); std::string id; if (value >> id) names.push_back(id); }
    return names;
}
inline std::vector<float> ParseNumbers(const std::string& text) {
    std::istringstream values(Lines(text));
    std::vector<float> numbers;
    for (float v = 0; values >> v;) numbers.push_back(v);
    return numbers;
}
}

class MechanismComponent final : public HIKARI::IGameComponent {
public:
    void DescribeProperties(HIKARI::PropertyFields& fields) override {
        if (fields.IsInspecting()) { fields.Note("在「关卡编辑 › 机关」里编辑：点电视加入、拖虚线框设位置、在视口里摆状态B。"); }
        fields.Text("mechanismId", "机构 ID（压板写 mech/ID）", id_);
        fields.Bool("toggle", "每踩一次切换（关：踩住时才动）", toggle_);
        // 天平：mech/ID の板だけ踏まれると状態 1、mech/ID/r の板だけなら状態 -1、両方か無しで 0。
        fields.Bool("balance", "天平（mech/ID 与 mech/ID/r 两块压板对抗）", balance_);
        fields.Float("seconds", "转动/移动秒数", seconds_, .2f, 8.f);
        // 0 より大きいと圧力板に関係なく往復し続ける（時機を計る仕掛け）。
        fields.Float("dwell", "自动往复：两端各停留秒数（0=由压板驱动）", dwell_, 0.f, 20.f);
        fields.Float("settle", "压稳多久才动（秒，踩住式与天平）", settle_, 0.f, 2.f);
        // 乗っている区域カメラを、動きの途中だけ持ち上げて引く（吊り台など）。120°以上回る転台では使わず、
        // 画面が真横を向いた瞬間に向こう側のカメラへ切り替える。
        fields.Float("cameraLift", "移动途中镜头抬高（米，转台不用）", cameraLift_, 0.f, 4.f);
        fields.Float("cameraPull", "移动途中镜头后拉（米，转台不用）", cameraPull_, 0.f, 4.f);
        if (!fields.Section("原始数据（一般不用手改）")) { return; }
        fields.Text("regionOffsets", "状态B的区域偏移（每行：区域ID dX dY [flip] [@锚点区域]；或 区域ID park 表示离开世界）", regionOffsets_);
        fields.Text("objectDeltas", "状态B的物体相对姿势（每行：物体ID dx dy dz rx ry rz [cx cy cz] [@锚点物体]）", objectDeltas_);
        fields.Text("regionOffsetsC", "天平另一侧的区域偏移（同上）", regionOffsetsC_);
        fields.Text("objectDeltasC", "天平另一侧的物体相对姿势（同上）", objectDeltasC_);
        fields.Text("regionMoves", "旧格式：状态B的区域绝对位置", regionMoves_);
        fields.Text("objectMoves", "旧格式：状态B的物体绝对姿势", objectMoves_);
        fields.Text("regionMovesC", "旧格式：天平另一侧的区域绝对位置", regionMovesC_);
        fields.Text("objectMovesC", "旧格式：天平另一侧的物体绝对姿势", objectMovesC_);
        if (fields.IsInspecting()) { fields.Note("状态A是场景里摆好的样子；状态B写成相对状态A的偏移，移动整组布景也不会坏。"); }
    }

    float CameraLift() const noexcept { return cameraLift_; }
    float CameraPull() const noexcept { return cameraPull_; }
    // 部品の値を読む。読めない行があれば error に書いて false。
    bool BuildSource(MechanismAuthoring& out, std::string& error) const {
        out = {};
        auto& s = out.source;
        s.id = id_; s.toggle = toggle_; s.balance = balance_; s.seconds = seconds_; s.dwell = dwell_; s.settle = settle_;
        if (id_.empty()) { error = "机构 ID 为空"; return false; }
        return Detail::ParseRegionMoves(regionMoves_, s.moves, error) && Detail::ParseRegionMoves(regionMovesC_, s.movesC, error) &&
               Detail::ParseObjectMoves(objectMoves_, out.objects, error) && Detail::ParseObjectMoves(objectMovesC_, out.objectsC, error) &&
               Detail::ParseRegionOffsets(regionOffsets_, out.offsets, error) && Detail::ParseRegionOffsets(regionOffsetsC_, out.offsetsC, error) &&
               Detail::ParseObjectDeltas(objectDeltas_, out.deltas, error) && Detail::ParseObjectDeltas(objectDeltasC_, out.deltasC, error);
    }

private:
    std::string id_, regionMoves_, objectMoves_, regionMovesC_, objectMovesC_;
    std::string regionOffsets_, objectDeltas_, regionOffsetsC_, objectDeltasC_;
    bool toggle_ = false, balance_ = false;
    float seconds_ = 0.8f, dwell_ = 0.f, settle_ = 0.f, cameraLift_ = 1.5f, cameraPull_ = 1.0f;
};

// スロットの並んだ棚。メンバーの部屋の圧力板に「slide/区域ID」と書くと、踏んだ時その部屋が隣の空いたスロットへ滑る。
//   メンバー：区域ID（カンマ区切り）。メンバーが今いる所がそれぞれスロットになる（世界地図の位置とテレビの姿勢）。
//   空きスロット：  dX dY（一番目のメンバーの世界地図の位置からのずれ）。3D の置き場はメンバーの並びから求める。
//   旧形式：  スロット = 世界X 世界Y 列 行 px py pz
struct SlideSlotPose { float position[3]{}; };
class SlideRackComponent final : public HIKARI::IGameComponent {
public:
    void DescribeProperties(HIKARI::PropertyFields& fields) override {
        if (fields.IsInspecting()) { fields.Note("在「关卡编辑 › 机关」里编辑：点电视加入成员，点空格子设空槽。"); }
        fields.Text("rackId", "货架 ID", id_);
        fields.Float("seconds", "滑动秒数", seconds_, .2f, 4.f);
        if (!fields.Section("原始数据（一般不用手改）")) { return; }
        fields.Text("members", "会滑动的区域（逗号分隔）", members_);
        fields.Text("emptySlots", "空槽（每行：相对第一台的 dX dY）", emptySlots_);
        fields.Text("slots", "旧格式：槽位（每行：世界X 世界Y 列 行 px py pz）", slots_);
        if (fields.IsInspecting()) { fields.Note("区域里的压板写 targets=slide/区域ID；只有空槽在上下左右相邻时才会滑动。"); }
    }
    // 旧形式のスロットがあれば poses にも入れる。無ければ空のまま返し、Session がメンバーの配置から作る。
    bool BuildSource(Rules::SlideSource& out, std::vector<SlideSlotPose>& poses, std::vector<std::pair<int, int>>& empty, std::string& error) const {
        out = {}; poses.clear(); empty.clear();
        out.id = id_; out.seconds = seconds_;
        std::istringstream lines(Detail::Lines(slots_));
        std::string line;
        while (std::getline(lines, line)) {
            std::istringstream values(line);
            Rules::SlideSlot slot; SlideSlotPose pose;
            if (!(values >> slot.worldX)) continue;
            if (!(values >> slot.worldY >> slot.col >> slot.row >> pose.position[0] >> pose.position[1] >> pose.position[2])) {
                error = "槽位行需要：世界X 世界Y 列 行 px py pz"; return false;
            }
            out.slots.push_back(slot); poses.push_back(pose);
        }
        std::istringstream emptyLines(Detail::Lines(emptySlots_));
        while (std::getline(emptyLines, line)) {
            std::istringstream values(line);
            std::pair<int, int> offset;
            if (!(values >> offset.first)) continue;
            if (!(values >> offset.second)) { error = "空槽行需要：dX dY"; return false; }
            empty.push_back(offset);
        }
        out.members = Detail::ParseNames(members_);
        if (id_.empty() || out.members.empty()) { error = "货架需要 ID 和区域"; return false; }
        if (out.slots.empty() && empty.empty()) { error = "货架需要至少一个空槽"; return false; }
        return true;
    }

private:
    std::string id_, slots_, members_, emptySlots_;
    float seconds_ = 0.9f;
};

namespace Detail {
// 「世界X 世界Y px py pz」の行を読む（世界地図の位置と、そこへ置く時のテレビのローカル位置）。
inline bool ParsePlaces(const std::string& text, std::vector<std::pair<int, int>>& places, std::vector<SlideSlotPose>& poses, std::string& error) {
    std::istringstream lines(Lines(text));
    std::string line;
    while (std::getline(lines, line)) {
        std::istringstream values(line);
        std::pair<int, int> place; SlideSlotPose pose;
        if (!(values >> place.first)) continue;
        if (!(values >> place.second >> pose.position[0] >> pose.position[1] >> pose.position[2])) { error = "位置行需要：世界X 世界Y px py pz"; return false; }
        places.push_back(place); poses.push_back(pose);
    }
    return true;
}
}

// 重ねたテレビ。圧力板に「stack/ID/pull」と書くと一番下のテレビを後ろの置き場へ抜き、上のテレビが一段ずつ落ちる。
// 「stack/ID/reset」で最初の並びへ戻す。
//   メンバー：区域ID（下から上の順、カンマ区切り）。メンバーが今いる所がそれぞれスロットになる。
//   抜く距離：一番下のテレビの背の側へ、置き場ごとの距離（m）。抜いたテレビは世界地図のどこともつながらない。
//   旧形式：  スロット・置き場 = 世界X 世界Y px py pz
class StackColumnComponent final : public HIKARI::IGameComponent {
public:
    void DescribeProperties(HIKARI::PropertyFields& fields) override {
        if (fields.IsInspecting()) { fields.Note("在「关卡编辑 › 机关」里编辑：点电视加入，自动按自下而上排序。"); }
        fields.Text("stackId", "电视塔 ID", id_);
        fields.Float("pullSeconds", "抽出秒数", pullSeconds_, .2f, 4.f);
        fields.Float("dropSeconds", "每层下落秒数", dropSeconds_, .1f, 2.f);
        fields.Float("resetSeconds", "复位秒数", resetSeconds_, .5f, 6.f);
        if (!fields.Section("原始数据（一般不用手改）")) { return; }
        fields.Text("members", "电视（自下而上，逗号分隔）", members_);
        fields.Text("dockDistances", "抽出距离（米，向最下面电视的背面；每个停放位一个数）", dockDistances_);
        fields.Text("slots", "旧格式：槽位（每行：世界X 世界Y px py pz）", slots_);
        fields.Text("docks", "旧格式：停放位（每行：世界X 世界Y px py pz）", docks_);
        if (fields.IsInspecting()) { fields.Note("压板写 targets=stack/ID/pull 抽出最下面一台，targets=stack/ID/reset 复位。"); }
    }
    // 旧形式のスロット・置き場があればそのまま返す。無ければ空のまま返し、Session がメンバーの配置と抜く距離から作る。
    bool BuildSource(Rules::StackSource& out, std::vector<SlideSlotPose>& slotPoses, std::vector<SlideSlotPose>& dockPoses,
                     std::vector<float>& distances, std::string& error) const {
        out = {}; slotPoses.clear(); dockPoses.clear();
        out.id = id_; out.pullSeconds = pullSeconds_; out.dropSeconds = dropSeconds_; out.resetSeconds = resetSeconds_;
        if (!Detail::ParsePlaces(slots_, out.slots, slotPoses, error) || !Detail::ParsePlaces(docks_, out.docks, dockPoses, error)) return false;
        out.members = Detail::ParseNames(members_);
        distances = Detail::ParseNumbers(dockDistances_);
        if (id_.empty() || out.members.empty()) { error = "电视塔需要 ID 和电视"; return false; }
        if (out.docks.empty() && distances.empty()) { error = "电视塔需要抽出距离"; return false; }
        return true;
    }

private:
    std::string id_, slots_, docks_, members_, dockDistances_ = "2.4 1.25";
    float pullSeconds_ = .8f, dropSeconds_ = .35f, resetSeconds_ = 2.f;
};

// 四面の回転柱。圧力板に「carousel/ID/cw」（時計回り）「carousel/ID/ccw」と書くと、柱が一面ぶん回る。
// この部品は柱の物体に付ける。柱の位置を通る縦の軸で、柱と面のテレビが一緒に回る。
//   面：     区域ID（時計回りの順＝正面・右・背面・左、カンマ区切り）。面が今いる所がそれぞれ立ち位置になる。
//            世界地図に置いていない面は、どこともつながらない立ち位置（左右の行き止まりなど）。
//   一緒に回る物：物体ID（柱以外に回す物があれば、空白区切り）
//   旧形式：  中心 px py pz と立ち位置 世界X 世界Y（中心を書くと柱の物体自体は回さない）
// 面のテレビに付いた区域カメラは面と一緒に回る（乗っている面を見続ける）。
class CarouselComponent final : public HIKARI::IGameComponent {
public:
    void DescribeProperties(HIKARI::PropertyFields& fields) override {
        if (fields.IsInspecting()) { fields.Note("在「关卡编辑 › 机关」里编辑：点电视加入面，在左栏调顺序。"); }
        fields.Text("carouselId", "转柱 ID", id_);
        fields.Float("seconds", "每转一面秒数", seconds_, .3f, 6.f);
        if (!fields.Section("原始数据（一般不用手改）")) { return; }
        fields.Text("faces", "各面的区域（顺时针：正面、右、背面、左，逗号分隔）", faces_);
        fields.Text("objects", "另外一起转的物体 ID（空格分隔）", objects_);
        fields.Text("pivot", "旧格式：转轴经过的点（px py pz）", pivot_);
        fields.Text("stations", "旧格式：站位（每行：世界X 世界Y）", stations_);
        if (fields.IsInspecting()) { fields.Note("把本组件放在柱子上。压板写 targets=carousel/ID/cw 或 carousel/ID/ccw。"); }
    }
    // hasPivot：旧形式の中心がある（無ければ Session が部品の物体の位置を中心にし、その物体も回す）。
    bool BuildSource(Rules::CarouselSource& out, float pivot[3], bool& hasPivot, std::vector<uint64_t>& objects, std::string& error) const {
        out = {}; objects.clear();
        out.id = id_; out.seconds = seconds_;
        std::istringstream center(pivot_);
        hasPivot = static_cast<bool>(center >> pivot[0] >> pivot[1] >> pivot[2]);
        std::istringstream lines(Detail::Lines(stations_));
        std::string line;
        while (std::getline(lines, line)) {
            std::istringstream values(line);
            std::pair<int, int> station;
            if (!(values >> station.first)) continue;
            if (!(values >> station.second)) { error = "站位行需要：世界X 世界Y"; return false; }
            out.stations.push_back(station);
        }
        out.faces = Detail::ParseNames(faces_);
        std::istringstream ids(Detail::Lines(objects_));
        uint64_t id = 0;
        while (ids >> id) objects.push_back(id);
        if (id_.empty() || out.faces.size() < 2) { error = "转柱需要 ID 和至少两个面"; return false; }
        if (!out.stations.empty() && out.faces.size() != out.stations.size()) { error = "转柱的面与站位数量不同"; return false; }
        return true;
    }

private:
    std::string id_, pivot_, stations_, faces_, objects_;
    float seconds_ = 1.6f;
};
}
