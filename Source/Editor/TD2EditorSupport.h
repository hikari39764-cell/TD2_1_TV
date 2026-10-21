#pragma once

#include "../Common/TD2RegionData.h"
#include "HIKARI/Project/EditorWorkspace.h"
#include <nlohmann/json.hpp>
#include <functional>
#include <string>
#include <vector>

namespace TD2::Editor {

using Json = nlohmann::json;
using namespace HIKARI::EDITOR_PROJECT;

// Host の文字列は callback の中でコピーする。
struct OperationResult final : IEditorOperationResultSink {
    bool ok = false;
    std::string error;
    Json data = Json::object();
    void Receive(bool succeeded, std::string_view code, std::string_view message,
                 std::string_view dataJson) override {
        ok = succeeded;
        if (!ok) { error = std::string(code) + ": " + std::string(message); return; }
        data = Json::parse(dataJson, nullptr, false);
        if (!data.is_object()) { ok = false; error = "INVALID_RESULT: 操作が JSON のオブジェクトを返しません"; }
    }
};

struct SceneItem { uint64_t objectId = 0; std::string name; };
struct RegionTarget {
    uint64_t objectId = 0, componentId = 0;
    bool operator==(const RegionTarget&) const = default;
};

// 作者データを丸ごと比べ、他での編集を古い草稿で上書きしない。
struct RegionSettings {
    RegionDefinition region;
    // 明るさはテレビの画面材質だけに掛ける。
    float screenBrightness = 2.2f;
    float spawnX = 0.5f, spawnY = 0.5f;
    bool entryRegion = true;
    // 配置は整数のマスで表した世界原点。Scene の 3D Transform とは関係ない。
    bool worldPlaced = false;
    int worldX = 0, worldY = 0;
    std::string devices;
    // 3D で付けている隣の区域（空なら自由に置いたテレビ）。編集器だけが使う。
    std::string stageAnchor;
    bool operator==(const RegionSettings&) const = default;
};

OperationResult Execute(EditorWorkspaceHostContext&, std::string_view operation, const Json& parameters);
// 分頁する問い合わせ（scene.objects.query・asset.query など）を最後まで読む。visit が false を返したらそこで止める。
// 分頁が進まない時は error に書いて false。
bool QueryAll(EditorWorkspaceHostContext&, std::string_view operation, Json parameters, std::string_view listKey,
              const std::function<bool(const Json&)>& visit, std::string& error);
// 一つの属性を書き換える命令（authoring.transaction の commands の一件）。
Json PropertyCommand(uint64_t objectId, uint64_t componentId, std::string_view propertyId, const Json& value);
// 命令をまとめて一回の取り消し単位で書く。空なら何もしない。
bool Transact(EditorWorkspaceHostContext&, const Json& commands, std::string& error);
uint64_t ObjectId(const Json&);
const Json* FieldValue(const Json&, std::string_view id);
bool QueryObjects(EditorWorkspaceHostContext&, std::string_view componentType, std::vector<SceneItem>&, std::string& error);
bool ReadRegion(EditorWorkspaceHostContext&, uint64_t objectId, RegionTarget&, RegionSettings&, std::string& error);
void AppendRegionChanges(Json& commands, const RegionTarget&, const RegionSettings& before, const RegionSettings& after);
bool EditText(IEditorWorkspaceUi&, std::string_view id, std::string_view label, std::string& value);
void EditInt(IEditorWorkspaceUi&, std::string_view id, std::string_view label, int& value, int minimum, int maximum);
bool EditFloat(IEditorWorkspaceUi&, std::string_view id, std::string_view label, double& value,
               float minimum, float maximum);
bool ResizeCells(const RegionDefinition& before, RegionDefinition& after);

}
