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

// 画面の外の機構（転台・吊り台・天平など）。圧力板の targets に「mech/機構ID」と書くと、その通電で状態が変わる。
// 状態 0 はシーンに置いたままの配置、状態 1 と（天平の）状態 -1 は下の表で書く。
//   区域の移動：  区域ID 世界X 世界Y [flip]     （その状態での世界地図の位置。flip は部屋を 180° 回す）
//   物体の移動：  物体ID px py pz rx ry rz [cx cy cz]  （その状態でのローカル位置と回転（度）。cx cy cz を書くと
//                その点を中心に回し、位置はそこから求める（倒立するテレビを画面の中心で回す時など））
// 動かした区域のテレビに付いた区域カメラは、テレビと一緒に動く（その場で画面の向きに回すだけの時は動かさない）。
struct MechanismObjectMove {
    uint64_t object = 0;
    float position[3]{}, rotation[3]{};
    bool hasPivot = false;
    float pivot[3]{};
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
}

class MechanismComponent final : public HIKARI::IGameComponent {
public:
    void DescribeProperties(HIKARI::PropertyFields& fields) override {
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
        fields.Text("regionMoves", "状态B的区域位置（每行：区域ID X Y [flip]）", regionMoves_);
        fields.Text("objectMoves", "状态B的物体姿势（每行：物体ID px py pz rx ry rz）", objectMoves_);
        fields.Text("regionMovesC", "天平另一侧的区域位置（同上）", regionMovesC_);
        fields.Text("objectMovesC", "天平另一侧的物体姿势（同上）", objectMovesC_);
        if (fields.IsInspecting()) {
            fields.Note("状态A是场景里摆好的样子。移动中被搬动的区域不和任何区域相连。flip 把区域转 180°（地图上下左右对调）。");
        }
    }

    float CameraLift() const noexcept { return cameraLift_; }
    float CameraPull() const noexcept { return cameraPull_; }
    // 規則へ渡す形にする。読めない行があれば error に書いて false。objectsC は天平のもう一方の姿勢。
    bool BuildSource(Rules::MechanismSource& out, std::vector<MechanismObjectMove>& objects, std::vector<MechanismObjectMove>& objectsC,
                     std::string& error) const {
        out = {}; objects.clear(); objectsC.clear();
        out.id = id_; out.toggle = toggle_; out.balance = balance_; out.seconds = seconds_; out.dwell = dwell_; out.settle = settle_;
        if (id_.empty()) { error = "机构 ID 为空"; return false; }
        return Detail::ParseRegionMoves(regionMoves_, out.moves, error) && Detail::ParseRegionMoves(regionMovesC_, out.movesC, error) &&
               Detail::ParseObjectMoves(objectMoves_, objects, error) && Detail::ParseObjectMoves(objectMovesC_, objectsC, error);
    }

private:
    std::string id_, regionMoves_, objectMoves_, regionMovesC_, objectMovesC_;
    bool toggle_ = false, balance_ = false;
    float seconds_ = 0.8f, dwell_ = 0.f, settle_ = 0.f, cameraLift_ = 1.5f, cameraPull_ = 1.0f;
};

// 槽の並んだ棚。メンバーの部屋の圧力板に「slide/区域ID」と書くと、踏んだ時その部屋が隣の空いた槽へ滑る。
//   槽：   世界X 世界Y 列 行 px py pz   （世界地図の位置、棚の上の列と行、テレビを置くローカル位置）
//   メンバー：区域ID（カンマか改行で区切る。最初は槽のどれかに置いておく）
struct SlideSlotPose { float position[3]{}; };
class SlideRackComponent final : public HIKARI::IGameComponent {
public:
    void DescribeProperties(HIKARI::PropertyFields& fields) override {
        fields.Text("rackId", "货架 ID", id_);
        fields.Float("seconds", "滑动秒数", seconds_, .2f, 4.f);
        fields.Text("slots", "槽位（每行：世界X 世界Y 列 行 px py pz）", slots_);
        fields.Text("members", "会滑动的区域（逗号分隔）", members_);
        if (fields.IsInspecting()) { fields.Note("区域里的压板写 targets=slide/区域ID；只有空槽在上下左右相邻时才会滑动。"); }
    }
    bool BuildSource(Rules::SlideSource& out, std::vector<SlideSlotPose>& poses, std::string& error) const {
        out = {}; poses.clear();
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
        std::string members = members_;
        std::replace(members.begin(), members.end(), ',', '\n');
        std::istringstream names(members);
        while (std::getline(names, line)) {
            std::istringstream value(line);
            std::string id;
            if (value >> id) out.members.push_back(id);
        }
        if (id_.empty() || out.slots.empty() || out.members.empty()) { error = "货架需要 ID、槽位和区域"; return false; }
        return true;
    }

private:
    std::string id_, slots_, members_;
    float seconds_ = 0.9f;
};
}
