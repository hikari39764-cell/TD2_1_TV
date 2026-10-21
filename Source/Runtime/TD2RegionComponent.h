#pragma once

#include "TD2Screen3D.h"
#include "../Common/TD2TelevisionModels.h"
#include "../Common/TD2WorldLayout.h"
#include "HIKARI/Project/GameComponent.h"
#include <algorithm>

namespace TD2 {

// シーン上の作者データ。エディタは公開の操作でこれを変える。
class RegionComponent final : public HIKARI::IGameComponent {
public:
    void DescribeProperties(HIKARI::PropertyFields& fields) override {
        // 地図・仕掛け・世界の位置・カメラはレベル編集ワークスペースで直す。Inspector は名前と明るさだけを前に出し、残りは畳んでおく。
        fields.Text("regionId", "区域 ID", region_.regionId);
        fields.Bool("entryRegion", "開始テレビ", entryRegion_);
        // 明るさは画面の材質だけに掛け、筐体は変えない。
        fields.Float("screenBrightness", "画面の明るさ", brightness_, .5f, 4.0f);
        if (!fields.Section("元データ")) { return; }
        // 縦穴などの縦長の画面に合わせ、一台 128 マスまで受け付ける。
        fields.Int("cols", "列", region_.cols, 1, 128);
        fields.Int("rows", "行", region_.rows, 1, 128);
        fields.Int("tilePixels", "1 マスの画素", region_.tilePixels, 1, 1024);
        DescribeLargeText(fields, "cells", "マス", region_.cells, 128u * 128u);
        fields.Float("spawnX", "スポーン X", spawnX_, 0.0f, 64.0f);
        fields.Float("spawnY", "スポーン Y", spawnY_, 0.0f, 64.0f);
        fields.SceneObject("cameraObject", "区域カメラ", cameraObject_);
        // エディタが自動で置いたカメラの姿勢（"x y z rx ry rz"）。今の姿勢と違えば手で調整したとみなし、
        // 再生成で上書きしない。空なら作者が置いたカメラ。
        fields.Text("cameraAuto", "カメラの自動記録", cameraAuto_);
        fields.Bool("worldPlaced", "世界に配置", worldPlaced_);
        fields.Int("worldX", "世界 X", worldX_);
        fields.Int("worldY", "世界 Y", worldY_);
        // 3D で付けている地図の隣の区域。編集器が隣に合わせて置き直すのに使い、実行時は読まない。
        fields.Text("stageAnchor", "3D の付け先", stageAnchor_);
        DescribeLargeText(fields, "devices", "装置", devices_, 256u * 1024u);
    }

    bool BuildDefinition(Rules::WorldDefinition& definition, std::string& error) const;
    uint64_t CameraObject() const noexcept { return cameraObject_; }
    bool IsEntryRegion() const noexcept { return entryRegion_; }
    RegionPlacement Placement() const { return {region_.regionId, region_.cols, region_.rows, worldPlaced_, worldX_, worldY_}; }
    // 画面を映すのは、この区域の付いたテレビ自身。
    ScreenConfiguration Screen(HIKARI::ObjectView self) const { return {self.GetSceneObjectId(), brightness_}; }
    // 画面の位置と大きさはテレビの型（モデル）から決める。表に無いモデルは既定の画面。
    static ScreenSurface Surface(HIKARI::ObjectView self) {
        HIKARI::GameProjectModelAppearance appearance{};
        const auto* model = self.GetModel().TryGetAppearance(appearance) ? FindTelevisionModel(appearance.assetId) : nullptr;
        if (!model) { return {}; }
        return {model->screenCenterX, model->screenCenterY, model->screenCenterZ, model->screenWidth, model->screenHeight};
    }
    // 作者の地図と装置を解釈したもの（編集中の画面が読む）。変わった時だけ作り直す。
    const Rules::WorldDefinition& AuthoredDefinition();

private:
    static void DescribeLargeText(HIKARI::PropertyFields& fields, std::string_view id,
                                  std::string_view label, std::string& value, size_t editableBytes) {
        // Load/Save は SDK の可変長文字列に任せ、長い値を切り詰めない。
        if (!fields.IsInspecting()) {
            fields.Text(id, label, value);
            return;
        }
        // Inspector の既定の追加容量（256 バイト）では足りないので広げる。
        // マスは最大 64×64、装置は 256 KiB まで。
        std::string buffer = value;
        buffer.resize((std::max)(value.size(), editableBytes) + 1u, '\0');
        if (fields.Text(id, label, HIKARI::GameProjectTextBuffer(buffer.data(), buffer.size()))) {
            value.assign(buffer.c_str());
        }
    }

    RegionDefinition region_{};
    float brightness_ = 2.2f;
    float spawnX_ = 0.5f, spawnY_ = 0.5f;
    uint64_t cameraObject_ = 0;
    std::string cameraAuto_;
    // 新しく付けたテレビは開始テレビにしない（開始テレビは世界に一台）。
    bool entryRegion_ = false;
    // 既定は未配置。古いシーンでもテレビを原点に重ねない。
    bool worldPlaced_ = false;
    int worldX_ = 0, worldY_ = 0;
    std::string devices_, stageAnchor_;
    struct AuthoredCache {
        RegionDefinition region; std::string devices; float spawnX = -1, spawnY = -1; bool valid = false;
        Rules::WorldDefinition definition;
    } authored_;
};

}
