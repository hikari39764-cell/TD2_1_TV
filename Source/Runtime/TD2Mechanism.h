#pragma once
#include "Rules/TD2Exploration.h"
#include "../Common/TD2MachineText.h"
#include "HIKARI/Project/GameComponent.h"
#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace TD2 {
inline constexpr std::string_view kMechanismComponentType = "TD2.Mechanism";
inline constexpr std::string_view kSlideRackComponentType = "TD2.SlideRack";
inline constexpr std::string_view kStackColumnComponentType = "TD2.StackColumn";
inline constexpr std::string_view kCarouselComponentType = "TD2.Carousel";

// 仕掛けの位置は「作者が置いた今の配置」から求める。区域や物体を編集器で動かしても仕掛けは壊れない。
// 文字列欄の書式は TD2MachineText.h にまとめてある。

// 画面の外の機構（転台・吊り台・天平など）。圧力板の targets に「mech/機構ID」と書くと、その通電で状態が変わる。
// 状態 0 はシーンに置いたままの配置、状態 1 と（天平の）状態 -1 は区域のずれと物体のずれで書く。
// 動かした区域のテレビに付いた区域カメラは、テレビと一緒に動く（その場で画面の向きに回すだけの時は動かさない）。
// 部品から読んだままの機構。区域のずれは Session が今の配置で絶対の位置へ直してから規則へ渡す。
struct MechanismAuthoring {
    Rules::MechanismSource source;
    std::vector<MachineRegionOffset> offsets, offsetsC;
    std::vector<MachineObjectDelta> deltas, deltasC;
};

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
        return MachineText::ParseOffsets(regionOffsets_, out.offsets, error) && MachineText::ParseOffsets(regionOffsetsC_, out.offsetsC, error) &&
               MachineText::ParseDeltas(objectDeltas_, out.deltas, error) && MachineText::ParseDeltas(objectDeltasC_, out.deltasC, error);
    }

private:
    std::string id_, regionOffsets_, objectDeltas_, regionOffsetsC_, objectDeltasC_;
    bool toggle_ = false, balance_ = false;
    float seconds_ = 0.8f, dwell_ = 0.f, settle_ = 0.f, cameraLift_ = 1.5f, cameraPull_ = 1.0f;
};

// スロットの並んだ棚。メンバーの部屋の圧力板に「slide/区域ID」と書くと、踏んだ時その部屋が隣の空いたスロットへ滑る。
// メンバーが今いる所がそれぞれスロットになり、空きスロットは一番目のメンバーの世界地図の位置からのずれで書く。
// 空きスロットの 3D の置き場はメンバーの並びから求める。
class SlideRackComponent final : public HIKARI::IGameComponent {
public:
    void DescribeProperties(HIKARI::PropertyFields& fields) override {
        if (fields.IsInspecting()) { fields.Note("在「关卡编辑 › 机关」里编辑：点电视加入成员，点空格子设空槽。"); }
        fields.Text("rackId", "货架 ID", id_);
        fields.Float("seconds", "滑动秒数", seconds_, .2f, 4.f);
        if (!fields.Section("原始数据（一般不用手改）")) { return; }
        fields.Text("members", "会滑动的区域（逗号分隔）", members_);
        fields.Text("emptySlots", "空槽（每行：相对第一台的 dX dY）", emptySlots_);
        if (fields.IsInspecting()) { fields.Note("区域里的压板写 targets=slide/区域ID；只有空槽在上下左右相邻时才会滑动。"); }
    }
    // スロットは空のまま返し、Session がメンバーの配置と empty（空きスロットのずれ）から作る。
    bool BuildSource(Rules::SlideSource& out, std::vector<std::pair<int, int>>& empty, std::string& error) const {
        out = {};
        out.id = id_; out.seconds = seconds_;
        if (!MachineText::ParseSlots(emptySlots_, empty, error)) { return false; }
        out.members = MachineText::ParseNames(members_);
        if (id_.empty() || out.members.empty()) { error = "货架需要 ID 和区域"; return false; }
        if (empty.empty()) { error = "货架需要至少一个空槽"; return false; }
        return true;
    }

private:
    std::string id_, members_, emptySlots_;
    float seconds_ = 0.9f;
};

// 重ねたテレビ。圧力板に「stack/ID/pull」と書くと一番下のテレビを後ろの置き場へ抜き、上のテレビが一段ずつ落ちる。
// 「stack/ID/reset」で最初の並びへ戻す。メンバー（下から上）が今いる所がそれぞれスロットになり、
// 置き場は一番下のテレビの背の側へ、置き場ごとの距離（m）。抜いたテレビは世界地図のどこともつながらない。
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
        if (fields.IsInspecting()) { fields.Note("压板写 targets=stack/ID/pull 抽出最下面一台，targets=stack/ID/reset 复位。"); }
    }
    // スロットと置き場は空のまま返し、Session がメンバーの配置と抜く距離から作る。
    bool BuildSource(Rules::StackSource& out, std::vector<double>& distances, std::string& error) const {
        out = {};
        out.id = id_; out.pullSeconds = pullSeconds_; out.dropSeconds = dropSeconds_; out.resetSeconds = resetSeconds_;
        out.members = MachineText::ParseNames(members_);
        distances = MachineText::ParseNumbers(dockDistances_);
        if (id_.empty() || out.members.empty()) { error = "电视塔需要 ID 和电视"; return false; }
        if (distances.empty()) { error = "电视塔需要抽出距离"; return false; }
        return true;
    }

private:
    std::string id_, members_, dockDistances_ = "2.4 1.25";
    float pullSeconds_ = .8f, dropSeconds_ = .35f, resetSeconds_ = 2.f;
};

// 四面の回転柱。圧力板に「carousel/ID/cw」（時計回り）「carousel/ID/ccw」と書くと、柱が一面ぶん回る。
// この部品は柱の物体に付ける。柱の位置を通る縦の軸で、柱と面のテレビ（と区域カメラ）が一緒に回る。
// 面（時計回りの順＝正面・右・背面・左）が今いる所がそれぞれ立ち位置になる。世界地図に置いていない面は行き止まり。
class CarouselComponent final : public HIKARI::IGameComponent {
public:
    void DescribeProperties(HIKARI::PropertyFields& fields) override {
        if (fields.IsInspecting()) { fields.Note("在「关卡编辑 › 机关」里编辑：点电视加入面，在左栏调顺序。"); }
        fields.Text("carouselId", "转柱 ID", id_);
        fields.Float("seconds", "每转一面秒数", seconds_, .3f, 6.f);
        if (!fields.Section("原始数据（一般不用手改）")) { return; }
        fields.Text("faces", "各面的区域（顺时针：正面、右、背面、左，逗号分隔）", faces_);
        fields.Text("objects", "另外一起转的物体 ID（空格分隔）", objects_);
        if (fields.IsInspecting()) { fields.Note("把本组件放在柱子上。压板写 targets=carousel/ID/cw 或 carousel/ID/ccw。"); }
    }
    // 立ち位置は空のまま返し、Session が面の配置から作る。objects は柱のほかに一緒に回す物体。
    bool BuildSource(Rules::CarouselSource& out, std::vector<uint64_t>& objects, std::string& error) const {
        out = {}; objects.clear();
        out.id = id_; out.seconds = seconds_;
        out.faces = MachineText::ParseNames(faces_);
        std::istringstream ids(MachineText::Lines(objects_));
        for (uint64_t id = 0; ids >> id;) { objects.push_back(id); }
        if (id_.empty() || out.faces.size() < 2) { error = "转柱需要 ID 和至少两个面"; return false; }
        return true;
    }

private:
    std::string id_, faces_, objects_;
    float seconds_ = 1.6f;
};
}
