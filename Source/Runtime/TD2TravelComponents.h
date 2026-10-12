#pragma once

#include "../Common/TD2TravelData.h"
#include "../Common/TD2WorldLayout.h"
#include "HIKARI/Project/GameComponent.h"

namespace TD2 {
// 演出の値だけを持つ。空間接続は Region の配置から求め、ここに通行許可を置かない。
class RegionSequenceComponent final : public HIKARI::IGameComponent {
public:
    void DescribeProperties(HIKARI::PropertyFields& f) override {
        f.Text("regionA", "正向起点电视", binding_.regionA);
        f.Text("regionB", "正向终点电视", binding_.regionB);
        f.AssetId("sequenceAssetGuid", "引擎演出", HIKARI::GameProjectAssetKind::Sequence, binding_.sequenceAssetGuid);
        f.Text("sequenceSlotName", "摄像机槽（可选）", binding_.sequenceSlotName);
        f.SceneObject("cameraObject", "槽对应摄像机（可选）", binding_.cameraObject);
        // 制作用の比較元だけを保存する。実行姿勢は既存 Sequence のキーが所有する。
        f.SceneObject("sourceCameraObject", "起点参考镜头（制作）", binding_.sourceCameraObject);
        f.SceneObject("targetCameraObject", "终点参考镜头（制作）", binding_.targetCameraObject);
    }
    RegionSequenceBinding Definition() const { return binding_; }
private:
    RegionSequenceBinding binding_;
};

// 通路・演出は Region と独立した作者コンポーネント。Editor と runtime の保存契約を共用する。
class PassageComponent final : public HIKARI::IGameComponent {
public:
    void DescribeProperties(HIKARI::PropertyFields& f) override {
        // 旧データは Load/Save で保全するが、Stage04 の操作面と実行系からは外す。
        if (f.IsInspecting()) { f.Note("旧通路数据已停用；请在世界地图布置电视。"); return; }
        f.Text("passageId", "稳定通路 ID", passageId_);
        f.Text("regionA", "A 区域 ID", regionA_); f.Text("regionB", "B 区域 ID", regionB_);
        f.Bool("bidirectional", "允许 B 返回 A", bidirectional_);
        f.Text("performanceId", "镜头演出 ID（空值保持镜头）", performanceId_);
        f.Float("aX", "A 触发区 X", aX_); f.Float("aY", "A 触发区 Y", aY_);
        f.Float("aW", "A 触发区宽", aW_, .01f, 64.f); f.Float("aH", "A 触发区高", aH_, .01f, 64.f);
        f.Float("aLandingX", "进入 A 的玩家左上 X", aLandingX_); f.Float("aLandingY", "进入 A 的玩家左上 Y", aLandingY_);
        f.Int("aDirection", "离开 A 的方向（-1 左 / 1 右）", aDirection_, -1, 1);
        f.Float("bX", "B 触发区 X", bX_); f.Float("bY", "B 触发区 Y", bY_);
        f.Float("bW", "B 触发区宽", bW_, .01f, 64.f); f.Float("bH", "B 触发区高", bH_, .01f, 64.f);
        f.Float("bLandingX", "进入 B 的玩家左上 X", bLandingX_); f.Float("bLandingY", "进入 B 的玩家左上 Y", bLandingY_);
        f.Int("bDirection", "离开 B 的方向（-1 左 / 1 右）", bDirection_, -1, 1);
        f.Note("左右边界端口须方向相反；朝出口触及端口才通行，落点需要完整、安全且有支撑。");
    }
    PassageDefinition Definition() const {
        return {passageId_, {regionA_, aX_, aY_, aW_, aH_, aLandingX_, aLandingY_, aDirection_},
            {regionB_, bX_, bY_, bW_, bH_, bLandingX_, bLandingY_, bDirection_}, bidirectional_, performanceId_};
    }
private:
    std::string passageId_ = "td2.passage", regionA_, regionB_, performanceId_;
    bool bidirectional_ = true;
    float aX_ = 0, aY_ = 0, aW_ = 1, aH_ = 1, aLandingX_ = 1, aLandingY_ = 1;
    float bX_ = 0, bY_ = 0, bW_ = 1, bH_ = 1, bLandingX_ = 1, bLandingY_ = 1;
    int aDirection_ = 1, bDirection_ = -1;
};
class CameraPerformanceComponent final : public HIKARI::IGameComponent {
public:
    void DescribeProperties(HIKARI::PropertyFields& f) override {
        if (f.IsInspecting()) { f.Note("旧镜头路径已停用；请绑定引擎 Sequence 演出。"); return; }
        f.Text("performanceId", "稳定演出 ID", performanceId_);
        f.SceneObject("cameraObject", "运行时游戏摄像机", cameraObject_);
        f.Float("durationSeconds", "完整路径时长（秒）", duration_, .05f, 60.f);
        f.Float3("startEye", "A 端镜头位置", startEye_); f.Float3("startTarget", "A 端注视位置", startTarget_);
        f.Float3("endEye", "B 端镜头位置", endEye_); f.Float3("endTarget", "B 端注视位置", endTarget_);
        f.Float3("up", "镜头上方向", up_); f.Float("fovDegrees", "垂直视野（度）", fov_, 5.f, 150.f);
        f.Note("A→B 正向，B→A 反向；演出中锁定玩家操作，结束后恢复，世界模拟始终向前。");
    }
    CameraPerformanceDefinition Definition() const {
        const auto point = [](HIKARI::GameProjectFloat3 v) { return CameraPoint{v.x, v.y, v.z}; };
        return {performanceId_, cameraObject_, duration_, fov_, point(startEye_), point(startTarget_),
                point(endEye_), point(endTarget_), point(up_)};
    }
private:
    std::string performanceId_ = "td2.camera.performance";
    uint64_t cameraObject_ = 0;
    float duration_ = 1, fov_ = 50;
    HIKARI::GameProjectFloat3 startEye_{0, 2, -5}, startTarget_{0, 2, 0};
    HIKARI::GameProjectFloat3 endEye_{3, 2, -5}, endTarget_{3, 2, 0}, up_{0, 1, 0};
};
} // namespace TD2
