#pragma once

#include "TD2ScreenPresenter.h"
#include "../Common/TD2WorldLayout.h"
#include "HIKARI/Project/GameComponent.h"
#include <algorithm>

namespace TD2 {

// 唯一のシーン作者データ。Editor は公開 operation を通してこのプロパティを変更する。
class RegionComponent final : public HIKARI::IGameComponent {
public:
    void DescribeProperties(HIKARI::PropertyFields& fields) override {
        fields.Text("regionId", "稳定区域 ID", region_.regionId);
        fields.Int("cols", "整数列数", region_.cols, 1, 64);
        fields.Int("rows", "整数行数", region_.rows, 1, 64);
        fields.Int("tilePixels", "每格逻辑像素", region_.tilePixels, 1, 1024);
        DescribeLargeText(fields, "cells", "格数据（行优先，0/1）", region_.cells, 64u * 64u);
        fields.Float("spawnX", "出生点左上 X（格）", spawnX_, 0.0f, 64.0f);
        fields.Float("spawnY", "出生点左上 Y（格）", spawnY_, 0.0f, 64.0f);
        fields.Bool("entryRegion", "初始可控制区块", entryRegion_);
        fields.Bool("worldPlaced", "已布置到世界", worldPlaced_);
        fields.Int("worldX", "世界格 X", worldX_);
        fields.Int("worldY", "世界格 Y", worldY_);
        DescribeLargeText(fields, "devices", "设备列表（每行一个）", devices_, 256u * 1024u);
        fields.SceneObject("screenObject", "屏幕模型对象", screen_.screenObject);
        fields.Text("screenSlot", "屏幕材质槽", screen_.screenSlot);
        // 明るさは画面専用の Unlit 材質だけへ適用し、元スプライトやテレビ筐体を変えない。
        fields.Float("screenBrightness", "屏幕亮度", screen_.brightness, .5f, 4.0f);
        // 公開 property operation の対象にするため常に記述する。専用工作区では自動解決し表示しない。
        fields.Int("screenMaterialIndex", "屏幕材质索引（自动）", screen_.materialIndex);
        fields.Text("screenModelAssetGuid", "屏幕模型身份（自动）", screen_.modelAssetGuid);
        fields.Text("texturePath", "色块纹理地址或项目纹理路径", screen_.texturePath);
        // これはプロジェクトの初期編集範囲であり、SDK の受理寸法を保証する値ではない。
        fields.Int("outputWidth", "画布输出宽度", screen_.outputWidth, 32, 2048);
        fields.Int("outputHeight", "画布输出高度", screen_.outputHeight, 32, 2048);
        fields.Note("格数据与电视缩放无关。修改行列后，明确重建测试网格；不要自动覆盖原数据。");
        fields.Note("多个区域须使用不同的稳定 ID。生成白纹理提交失败时，请改用已导入白纹理。");
        fields.Note("设备：id kind x y w h；可选 targets=id,id dir=Up/Down/Left/Right range=n invert=0/1。");
    }

    bool BuildDefinition(Rules::WorldDefinition& definition, std::string& error) const;
    bool IsEntryRegion() const noexcept { return entryRegion_; }
    RegionPlacement Placement() const { return {region_.regionId, region_.cols, region_.rows, worldPlaced_, worldX_, worldY_}; }
    void SetRuntimeStatus(std::string_view status) { runtimeStatus_ = status; }
    void RefreshScreen(HIKARI::WorldView& world);

    void OnDetach(HIKARI::GameComponentContext& context) override {
        auto world = context.GetWorld();
        (void)presenter_.Release(world);
    }

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
    ScreenPresenter presenter_{};
    float spawnX_ = 0.5f, spawnY_ = 0.5f;
    bool entryRegion_ = true;
    // 未配置が既定。旧シーンを読み込んでも全テレビを原点へ重ねて参加させない。
    bool worldPlaced_ = false;
    int worldX_ = 0, worldY_ = 0;
    std::string devices_;
    std::string runtimeStatus_ = "编辑态只显示作者地图；Play 后运行规则";
};

} // namespace TD2
