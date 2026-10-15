#pragma once

#include "TD2Screen3D.h"
#include "../Common/TD2WorldLayout.h"
#include "HIKARI/Project/GameComponent.h"
#include <algorithm>

namespace TD2 {

// 唯一のシーン作者データ。Editor は公開 operation を通してこのプロパティを変更する。
class RegionComponent final : public HIKARI::IGameComponent {
public:
    void DescribeProperties(HIKARI::PropertyFields& fields) override {
        fields.Text("regionId", "稳定区域 ID", region_.regionId);
        // 縦長の大画面（竖井）などに合わせ、一台の TV は 128 格まで受け付ける。
        fields.Int("cols", "整数列数", region_.cols, 1, 128);
        fields.Int("rows", "整数行数", region_.rows, 1, 128);
        fields.Int("tilePixels", "每格逻辑像素", region_.tilePixels, 1, 1024);
        DescribeLargeText(fields, "cells", "格数据（行优先，0/1）", region_.cells, 128u * 128u);
        fields.Float("spawnX", "出生点左上 X（格）", spawnX_, 0.0f, 64.0f);
        fields.Float("spawnY", "出生点左上 Y（格）", spawnY_, 0.0f, 64.0f);
        fields.SceneObject("cameraObject", "区域共享机位", cameraObject_);
        fields.Bool("entryRegion", "初始可控制区块", entryRegion_);
        fields.Bool("worldPlaced", "已布置到世界", worldPlaced_);
        fields.Int("worldX", "世界格 X", worldX_);
        fields.Int("worldY", "世界格 Y", worldY_);
        DescribeLargeText(fields, "devices", "设备列表（每行一个）", devices_, 256u * 1024u);
        fields.SceneObject("screenObject", "屏幕模型对象", screen_.screenObject);
        fields.Text("screenSlot", "屏幕材质槽", screen_.screenSlot);
        // 明るさは capture の画面専用材質だけへ適用し、テレビ筐体を変えない。
        fields.Float("screenBrightness", "屏幕亮度", screen_.brightness, .5f, 4.0f);
        // 公開 property operation の対象にするため常に記述する。専用工作区では自動解決し表示しない。
        fields.Int("screenMaterialIndex", "屏幕材质索引（自动）", screen_.materialIndex);
        fields.Text("screenModelAssetGuid", "屏幕模型身份（自动）", screen_.modelAssetGuid);
        // これはプロジェクトの初期編集範囲であり、SDK の受理寸法を保証する値ではない。
        fields.Int("outputWidth", "屏幕目标宽度", screen_.outputWidth, 32, 2048);
        fields.Int("outputHeight", "屏幕目标高度", screen_.outputHeight, 32, 2048);
        fields.Note("格数据与电视缩放无关。修改行列后，明确重建测试网格；不要自动覆盖原数据。");
        fields.Note("多个区域须使用不同的稳定 ID；真实3D风格由 TD2.ScreenStyle 配置。");
        fields.Note("设备：id kind x y w h；可选 targets=id,区域ID/id dir=Up/Down/Left/Right range=n invert=0/1 "
                    "power=格每秒 delay=秒 cooldown=秒 variant=ad/space/spaceHold opacity=0~1。");
    }

    bool BuildDefinition(Rules::WorldDefinition& definition, std::string& error) const;
    uint64_t CameraObject() const noexcept { return cameraObject_; }
    bool IsEntryRegion() const noexcept { return entryRegion_; }
    RegionPlacement Placement() const { return {region_.regionId, region_.cols, region_.rows, worldPlaced_, worldX_, worldY_}; }
    const ScreenConfiguration& Screen() const noexcept { return screen_; }
    // 作者の地図と装置の解釈（編集中の電視の舞台が読む）。格・装置・寸法・出生点が変わった時だけ作り直す。
    const Rules::WorldDefinition& AuthoredDefinition();

private:
    static void DescribeLargeText(HIKARI::PropertyFields& fields, std::string_view id,
                                  std::string_view label, std::string& value, size_t editableBytes) {
        // Load/Save は SDK の可変長文字列に任せ、既存の長い保存値を固定容量で失わない。
        if (!fields.IsInspecting()) {
            fields.Text(id, label, value);
            return;
        }
        // Inspect の既定追加容量 256 バイトでは一括編集が収まらない。
        // 格は最大 64×64、装置は UTF-8 で 256 KiB と終端を確保する。既存値は切り詰めない。
        std::string buffer = value;
        buffer.resize((std::max)(value.size(), editableBytes) + 1u, '\0');
        if (fields.Text(id, label, HIKARI::GameProjectTextBuffer(buffer.data(), buffer.size()))) {
            value.assign(buffer.c_str());
        }
    }

    RegionDefinition region_{};
    ScreenConfiguration screen_{};
    float spawnX_ = 0.5f, spawnY_ = 0.5f;
    uint64_t cameraObject_ = 0;
    bool entryRegion_ = true;
    // 未配置が既定。旧シーンを読み込んでも全テレビを原点へ重ねて参加させない。
    bool worldPlaced_ = false;
    int worldX_ = 0, worldY_ = 0;
    std::string devices_;
    struct AuthoredCache {
        RegionDefinition region; std::string devices; float spawnX = -1, spawnY = -1; bool valid = false;
        Rules::WorldDefinition definition;
    } authored_;
};

} // namespace TD2
