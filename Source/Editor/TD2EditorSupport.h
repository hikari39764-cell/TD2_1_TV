#pragma once

#include "../Common/TD2RegionData.h"
#include "HIKARI/Project/EditorWorkspace.h"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace TD2::Editor {

using Json = nlohmann::json;
using namespace HIKARI::EDITOR_PROJECT;

// Host の借用文字列は callback 内でコピーし、公開領域操作の診断をそのまま保持する。
struct OperationResult final : IEditorOperationResultSink {
    bool ok = false;
    std::string error;
    Json data = Json::object();
    void Receive(bool succeeded, std::string_view code, std::string_view message,
                 std::string_view dataJson) override {
        ok = succeeded;
        if (!ok) { error = std::string(code) + ": " + std::string(message); return; }
        data = Json::parse(dataJson, nullptr, false);
        if (!data.is_object()) { ok = false; error = "INVALID_RESULT: 操作未返回有效 JSON 对象"; }
    }
};

struct SceneItem { uint64_t objectId = 0; std::string name; };
struct RegionTarget {
    uint64_t objectId = 0, componentId = 0;
    bool operator==(const RegionTarget&) const = default;
};

// 一つの Component の作者データを丸ごと比較し、他の UI の編集を古い草稿で上書きしない。
struct RegionSettings {
    RegionDefinition region;
    uint64_t screenObject = 0;
    std::string screenSlot = "TD2_Screen", texturePath = "generated://solid/ffffffff";
    // 増幅はテレビの専用材質だけに適用する。index とモデル GUID は作者 UI に露出しない。
    float screenBrightness = 2.2f;
    int screenMaterialIndex = -1;
    std::string screenModelAssetGuid;
    int outputWidth = 256, outputHeight = 192;
    float spawnX = 0.5f, spawnY = 0.5f;
    bool entryRegion = true;
    // 配置は整数格の世界原点であり、Scene の三次元 Transform とは無関係。
    bool worldPlaced = false;
    int worldX = 0, worldY = 0;
    std::string devices;
    bool operator==(const RegionSettings&) const = default;
};

OperationResult Execute(EditorWorkspaceHostContext&, std::string_view operation, const Json& parameters);
uint64_t ObjectId(const Json&);
const Json* FieldValue(const Json&, std::string_view id);
bool QueryObjects(EditorWorkspaceHostContext&, std::string_view componentType, std::vector<SceneItem>&, std::string& error);
bool ReadRegion(EditorWorkspaceHostContext&, uint64_t objectId, RegionTarget&, RegionSettings&, std::string& error);
bool CommitProperties(EditorWorkspaceHostContext&, const RegionTarget&, const RegionSettings& before,
                      const RegionSettings& after, std::string& error);
void AppendRegionChanges(Json& commands, const RegionTarget&, const RegionSettings& before, const RegionSettings& after);
uint64_t SelectedObject(EditorWorkspaceHostContext&, std::string& error);
bool AddRegion(EditorWorkspaceHostContext&, uint64_t objectId, bool bindSelf, std::string& status);
bool ResolveScreenMaterial(EditorWorkspaceHostContext&, RegionSettings&, std::string& diagnostic);
bool EditText(IEditorWorkspaceUi&, std::string_view id, std::string_view label, std::string& value);
void EditInt(IEditorWorkspaceUi&, std::string_view id, std::string_view label, int& value, int minimum, int maximum);
bool EditFloat(IEditorWorkspaceUi&, std::string_view id, std::string_view label, double& value,
               float minimum, float maximum);
bool ResizeCells(const RegionDefinition& before, RegionDefinition& after);
bool StableAuthoringId(std::string_view id);

} // namespace TD2::Editor
