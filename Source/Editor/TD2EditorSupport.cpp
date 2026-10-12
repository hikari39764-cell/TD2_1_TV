#include "TD2EditorSupport.h"
#include "HIKARI/Project/GameProjectEditorModule.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace TD2::Editor {

using Json = nlohmann::json;
using namespace HIKARI::EDITOR_PROJECT;

bool StableAuthoringId(std::string_view id) {
    return !id.empty() && id.size() <= 95 && std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
    });
}

OperationResult Execute(EditorWorkspaceHostContext& context, std::string_view operation,
                        const Json& parameters) {
    OperationResult result;
    context.commands->ExecuteOperation(operation, parameters.dump(), result);
    if (!result.ok) { result.error = std::string(operation) + ": " + result.error; }
    return result;
}

uint64_t ObjectId(const Json& value) {
    if (value.is_number_unsigned()) { return value.get<uint64_t>(); }
    if (value.is_number_integer() && value >= 0) { return value.get<uint64_t>(); }
    return 0;
}

// 固定の Human 領域操作だけを使う。AI discovery やブリッジを実行時の前提にしない。
bool QueryObjects(EditorWorkspaceHostContext& context, std::string_view componentType,
                  std::vector<SceneItem>& objects, std::string& error) {
    objects.clear();
    uint64_t offset = 0;
    for (;;) {
        Json parameters{{"offset", offset}, {"limit", 64}};
        if (!componentType.empty()) { parameters["componentType"] = componentType; }
        const auto result = Execute(context, "scene.objects.query", parameters);
        if (!result.ok) { error = result.error; return false; }
        const auto entries = result.data.find("objects");
        if (entries == result.data.end() || !entries->is_array()) {
            error = "scene.objects.query: INVALID_RESULT: objects 数组缺失"; return false;
        }
        for (const auto& object : *entries) {
            if (!object.is_object() || !object.contains("objectId") || !object.contains("name") ||
                ObjectId(object["objectId"]) == 0 || !object["name"].is_string()) {
                error = "scene.objects.query: INVALID_RESULT: 对象身份或名称缺失"; return false;
            }
            objects.push_back({ObjectId(object["objectId"]), object["name"].get<std::string>()});
        }
        const auto next = result.data.find("nextOffset");
        if (next == result.data.end()) { return true; }
        const uint64_t nextOffset = ObjectId(*next);
        if (nextOffset <= offset || entries->empty()) {
            error = "scene.objects.query: INVALID_RESULT: 分页没有前进"; return false;
        }
        offset = nextOffset;
    }
}

uint64_t RegionComponentId(const Json& components) {
    if (!components.is_array()) { return 0; }
    for (const auto& component : components) {
        if (component.is_object() && component.contains("type") && component["type"].is_string() &&
            component["type"].get<std::string>() == kRegionComponentType && component.contains("componentId")) {
            return ObjectId(component["componentId"]);
        }
    }
    return 0;
}

bool ResolveRegion(EditorWorkspaceHostContext& context, uint64_t objectId, RegionTarget& target, std::string& error) {
    const auto result = Execute(context, "component.instances.query", {{"objectId", objectId}});
    if (!result.ok) { error = result.error; return false; }
    const auto components = result.data.find("components");
    const uint64_t componentId = components == result.data.end() ? 0 : RegionComponentId(*components);
    if (componentId == 0) {
        error = "component.instances.query: 此对象没有 TD2 电视区块，请使用添加按钮"; return false;
    }
    target = {objectId, componentId};
    return true;
}

// component.describe の fields[] を id で直接読む。別形式の再帰検索や表示名での推測はしない。
const Json* FieldValue(const Json& fields, std::string_view id) {
    for (const auto& field : fields) {
        if (field.is_object() && field.contains("id") && field["id"].is_string() &&
            field["id"].get<std::string>() == id && field.contains("value")) { return &field["value"]; }
    }
    return nullptr;
}

bool ReadRegion(EditorWorkspaceHostContext& context, uint64_t objectId, RegionTarget& target,
                RegionSettings& settings, std::string& error) {
    if (!ResolveRegion(context, objectId, target, error)) { return false; }
    const auto result = Execute(context, "component.describe",
        {{"objectId", target.objectId}, {"componentId", target.componentId},
         {"propertyIds", {"regionId", "cols", "rows", "tilePixels", "cells", "screenObject",
                          "screenSlot", "texturePath", "outputWidth", "outputHeight",
                          "spawnX", "spawnY", "entryRegion", "devices", "worldPlaced", "worldX", "worldY",
                          "screenBrightness", "screenMaterialIndex", "screenModelAssetGuid"}}});
    if (!result.ok) { error = result.error; return false; }
    const auto fields = result.data.find("fields");
    if (fields == result.data.end() || !fields->is_array()) {
        error = "component.describe: INVALID_RESULT: fields 数组缺失"; return false;
    }
    const auto text = [&fields, &error](std::string_view id, std::string& value) {
        const auto* field = FieldValue(*fields, id);
        if (field == nullptr || !field->is_string()) {
            error = "component.describe: INVALID_RESULT: 字符串字段 " + std::string(id) + " 缺失"; return false;
        }
        value = field->get<std::string>();
        return true;
    };
    const auto number = [&fields, &error](std::string_view id, int& value) {
        const auto* field = FieldValue(*fields, id);
        if (field == nullptr || !field->is_number_integer() || *field < 1 ||
            *field > (std::numeric_limits<int>::max)()) {
            error = "component.describe: INVALID_RESULT: 正整数字段 " + std::string(id) + " 缺失"; return false;
        }
        value = field->get<int>();
        return true;
    };
    const auto* screen = FieldValue(*fields, "screenObject");
    if (screen == nullptr || !screen->is_number_integer() || *screen < 0) {
        error = "component.describe: INVALID_RESULT: screenObject 场景引用无效"; return false;
    }
    settings.screenObject = ObjectId(*screen);
    const auto* spawnX = FieldValue(*fields, "spawnX");
    const auto* spawnY = FieldValue(*fields, "spawnY");
    const auto* entry = FieldValue(*fields, "entryRegion");
    if (spawnX == nullptr || !spawnX->is_number() || spawnY == nullptr || !spawnY->is_number() ||
        entry == nullptr || !entry->is_boolean()) {
        error = "component.describe: INVALID_RESULT: 出生点或初始区块字段缺失"; return false;
    }
    settings.spawnX = spawnX->get<float>();
    settings.spawnY = spawnY->get<float>();
    settings.entryRegion = entry->get<bool>();
    const auto* placed = FieldValue(*fields, "worldPlaced");
    const auto* worldX = FieldValue(*fields, "worldX");
    const auto* worldY = FieldValue(*fields, "worldY");
    if (!placed || !placed->is_boolean() || !worldX || !worldX->is_number_integer() || !worldY || !worldY->is_number_integer() ||
        *worldX < (std::numeric_limits<int>::min)() || *worldX > (std::numeric_limits<int>::max)() ||
        *worldY < (std::numeric_limits<int>::min)() || *worldY > (std::numeric_limits<int>::max)()) {
        error = "component.describe: INVALID_RESULT: Stage04 世界布局字段缺失或无效"; return false;
    }
    settings.worldPlaced = placed->get<bool>(); settings.worldX = worldX->get<int>(); settings.worldY = worldY->get<int>();
    // 旧 Scene に存在しない追加フィールドは既定値で読む。不正な型を既定値に偽装しない。
    settings.screenBrightness = 2.2f; settings.screenMaterialIndex = -1; settings.screenModelAssetGuid.clear();
    if (const auto* brightness = FieldValue(*fields, "screenBrightness")) {
        if (!brightness->is_number() || !std::isfinite(brightness->get<float>())) { error = "component.describe: INVALID_RESULT: 屏幕亮度无效"; return false; }
        settings.screenBrightness = brightness->get<float>();
    }
    if (const auto* materialIndex = FieldValue(*fields, "screenMaterialIndex")) {
        if (!materialIndex->is_number_integer() || *materialIndex < -1 || *materialIndex > (std::numeric_limits<int>::max)()) { error = "component.describe: INVALID_RESULT: 屏幕材质缓存无效"; return false; }
        settings.screenMaterialIndex = materialIndex->get<int>();
    }
    if (const auto* modelGuid = FieldValue(*fields, "screenModelAssetGuid")) {
        if (!modelGuid->is_string()) { error = "component.describe: INVALID_RESULT: 屏幕模型身份缓存无效"; return false; }
        settings.screenModelAssetGuid = modelGuid->get<std::string>();
    }
    return text("regionId", settings.region.regionId) && number("cols", settings.region.cols) &&
        number("rows", settings.region.rows) && number("tilePixels", settings.region.tilePixels) &&
        text("cells", settings.region.cells) && text("screenSlot", settings.screenSlot) &&
        text("texturePath", settings.texturePath) && number("outputWidth", settings.outputWidth) &&
        number("outputHeight", settings.outputHeight) && text("devices", settings.devices);
}

void AppendRegionChanges(Json& commands, const RegionTarget& target, const RegionSettings& before, const RegionSettings& after) {
    const auto add = [&commands, &target](std::string_view id, const auto& oldValue, const auto& newValue) {
        if (oldValue != newValue) {
            commands.push_back({{"objectId", target.objectId}, {"componentId", target.componentId},
                                {"propertyId", id}, {"value", newValue}});
        }
    };
    add("regionId", before.region.regionId, after.region.regionId);
    add("cols", before.region.cols, after.region.cols);
    add("rows", before.region.rows, after.region.rows);
    add("tilePixels", before.region.tilePixels, after.region.tilePixels);
    add("cells", before.region.cells, after.region.cells);
    add("screenObject", before.screenObject, after.screenObject);
    add("screenSlot", before.screenSlot, after.screenSlot);
    add("screenBrightness", before.screenBrightness, after.screenBrightness);
    add("screenMaterialIndex", before.screenMaterialIndex, after.screenMaterialIndex);
    add("screenModelAssetGuid", before.screenModelAssetGuid, after.screenModelAssetGuid);
    add("texturePath", before.texturePath, after.texturePath);
    add("outputWidth", before.outputWidth, after.outputWidth);
    add("outputHeight", before.outputHeight, after.outputHeight);
    add("spawnX", before.spawnX, after.spawnX);
    add("spawnY", before.spawnY, after.spawnY);
    add("entryRegion", before.entryRegion, after.entryRegion);
    add("devices", before.devices, after.devices);
    add("worldPlaced", before.worldPlaced, after.worldPlaced);
    add("worldX", before.worldX, after.worldX);
    add("worldY", before.worldY, after.worldY);
}

bool CommitProperties(EditorWorkspaceHostContext& context, const RegionTarget& target,
                      const RegionSettings& before, const RegionSettings& after, std::string& error) {
    Json commands = Json::array();
    AppendRegionChanges(commands, target, before, after);
    if (commands.empty()) { return true; }
    // サイズと配列の変更を途中状態にせず、一つの Scene 撤回で戻せるようにする。
    const auto result = Execute(context, "authoring.transaction", {{"commands", commands}});
    if (!result.ok) { error = result.error; return false; }
    return true;
}

bool ResolveScreenMaterial(EditorWorkspaceHostContext& context, RegionSettings& settings, std::string& diagnostic) {
    // 新しいモデルへの古い index の流用を防ぐ。slotId は材質配列 index ではない。
    settings.screenMaterialIndex = -1; settings.screenModelAssetGuid.clear(); diagnostic.clear();
    if (!settings.screenObject) { return true; }
    if (settings.screenSlot.empty()) { diagnostic = "请指定屏幕材质槽名称；亮度增益未启用"; return false; }
    const auto result = Execute(context, "scene.objects.query", {{"objectIds", {settings.screenObject}}, {"detail", true}});
    if (!result.ok) { diagnostic = result.error; return false; }
    const auto objects = result.data.find("objects");
    if (objects == result.data.end() || !objects->is_array()) { diagnostic = "屏幕对象的模型信息缺失"; return false; }
    const Json* model = nullptr;
    for (const auto& object : *objects) {
        if (!object.is_object() || ObjectId(object.value("objectId", Json{})) != settings.screenObject) { continue; }
        if (object.contains("model") && object["model"].is_object()) { model = &object["model"]; }
        break;
    }
    if (!model) { diagnostic = "屏幕对象没有可确认的模型"; return false; }
    const auto asset = model->find("assetId"), count = model->find("materialSlotCount"), slots = model->find("materialSlots"), truncated = model->find("materialSlotsTruncated");
    if (asset == model->end() || !asset->is_string() || asset->get<std::string>().empty() || count == model->end() || !count->is_number_integer() ||
        *count < 0 || *count > (std::numeric_limits<int>::max)() || slots == model->end() || !slots->is_array() || truncated == model->end() ||
        !truncated->is_boolean() || truncated->get<bool>() || slots->size() != count->get<size_t>()) {
        diagnostic = "屏幕材质槽信息缺失或被截断；亮度增益未启用"; return false;
    }
    settings.screenModelAssetGuid = asset->get<std::string>();
    for (size_t i = 0; i < slots->size(); ++i) {
        const auto& slot = (*slots)[i];
        if (!slot.is_object() || !slot.contains("name") || !slot["name"].is_string()) { diagnostic = "屏幕材质槽名称无效"; return false; }
        if (slot["name"].get<std::string>() == settings.screenSlot) {
            // BindSurface と同じ最初の完全一致を採用する。公開 DTO の配列順が materialIndex に対応する。
            settings.screenMaterialIndex = static_cast<int>(i); return true;
        }
    }
    diagnostic = "找不到屏幕材质槽：" + settings.screenSlot + "；亮度增益未启用"; return false;
}

uint64_t SelectedObject(EditorWorkspaceHostContext& context, std::string& error) {
    const auto result = Execute(context, "editor.state", Json::object());
    if (!result.ok) { error = result.error; return 0; }
    const auto selection = result.data.find("selection");
    if (selection == result.data.end() || !selection->is_object() || !selection->contains("activeObjectId")) {
        error = "editor.state: INVALID_RESULT: selection.activeObjectId 缺失"; return 0;
    }
    const uint64_t selected = ObjectId((*selection)["activeObjectId"]);
    if (selected == 0) { error = "请先在场景中选择电视或其他承载区块的对象"; }
    return selected;
}

bool AddRegion(EditorWorkspaceHostContext& context, uint64_t objectId, bool bindSelf, std::string& status) {
    const auto instances = Execute(context, "component.instances.query", {{"objectId", objectId}});
    if (!instances.ok) { status = instances.error; return false; }
    const auto components = instances.data.find("components");
    if (components == instances.data.end() || !components->is_array()) {
        status = "component.instances.query: INVALID_RESULT: components 数组缺失"; return false;
    }
    if (RegionComponentId(*components) != 0) { return true; }
    const auto result = Execute(context, "component.add", {{"objectId", objectId}, {"componentType", kRegionComponentType}});
    if (!result.ok) { status = result.error; return false; }
    const auto added = result.data.find("addedComponents");
    const uint64_t componentId = added == result.data.end() ? 0 : RegionComponentId(*added);
    if (componentId == 0) { status = "component.add: INVALID_RESULT: 新区块组件身份缺失"; return false; }
    // 新規作者 ID は Scene の安定 Object ID から作る。既存 Component の ID は変更しない。
    Json commands = {{{"objectId", objectId}, {"componentId", componentId}, {"propertyId", "regionId"},
                      {"value", "td2.region." + std::to_string(objectId)}}};
    // 最初の地図だけを開始候補にする。追加地図は既存の開始点を奪わない。
    std::vector<SceneItem> regions;
    if (!QueryObjects(context, kRegionComponentType, regions, status)) { return false; }
    bool firstEntry = true;
    for (const auto& item : regions) {
        if (item.objectId == objectId) { continue; }
        RegionTarget existingTarget; RegionSettings existing;
        if (!ReadRegion(context, item.objectId, existingTarget, existing, status)) { return false; }
        if (existing.entryRegion) { firstEntry = false; break; }
    }
    commands.push_back({{"objectId", objectId}, {"componentId", componentId}, {"propertyId", "entryRegion"}, {"value", firstEntry}});
    if (bindSelf) {
        commands.push_back({{"objectId", objectId}, {"componentId", componentId}, {"propertyId", "screenObject"}, {"value", objectId}});
        commands.push_back({{"objectId", objectId}, {"componentId", componentId}, {"propertyId", "screenSlot"}, {"value", "TD2_Screen"}});
    }
    const auto initialized = Execute(context, "authoring.transaction", {{"commands", commands}});
    if (!initialized.ok) {
        status = "区块组件已添加；初始化配置失败：" + initialized.error; return false;
    }
    return true;
}

bool EditText(IEditorWorkspaceUi& ui, std::string_view id, std::string_view label, std::string& value) {
    std::vector<char> buffer((std::max)(size_t{1024}, value.size() + 256), '\0');
    std::memcpy(buffer.data(), value.data(), value.size());
    if (!ui.InputText(id, label, buffer.data(), buffer.size())) { return false; }
    value = buffer.data();
    return true;
}

bool EditFloat(IEditorWorkspaceUi& ui, std::string_view id, std::string_view label,
               double& value, float minimum, float maximum) {
    float number = static_cast<float>(value);
    EditorWorkspaceFloatInputOptions options{};
    options.minimum = minimum; options.maximum = maximum; options.clampToRange = 1;
    options.step = 0.125f;
    if (!ui.InputFloat(id, label, &number, options)) { return false; }
    value = number;
    return true;
}

void EditInt(IEditorWorkspaceUi& ui, std::string_view id, std::string_view label,
             int& value, int minimum, int maximum) {
    int32_t number = value;
    EditorWorkspaceIntInputOptions options{};
    options.minimum = minimum;
    options.maximum = maximum;
    options.clampToRange = 1;
    if (ui.InputInt(id, label, &number, options)) { value = number; }
}

// 左上の重なりを保持し、新しい格だけ空にする。縮小して失う範囲は適用前に UI で伝える。
bool ResizeCells(const RegionDefinition& before, RegionDefinition& after) {
    if (after.cols < 1 || after.cols > 64 || after.rows < 1 || after.rows > 64 ||
        static_cast<uint64_t>(after.cols) * static_cast<uint64_t>(after.rows) > 4096) { return false; }
    if (before.cols == after.cols && before.rows == after.rows) { return true; }
    if (before.cols < 1 || before.rows < 1 || before.cols > 64 || before.rows > 64 ||
        before.cells.size() != static_cast<size_t>(before.cols) * static_cast<size_t>(before.rows)) { return false; }
    after.cells.assign(static_cast<size_t>(after.cols) * static_cast<size_t>(after.rows), '0');
    for (int row = 0; row < (std::min)(before.rows, after.rows); ++row) {
        for (int col = 0; col < (std::min)(before.cols, after.cols); ++col) {
            after.cells[CellIndex(after, col, row)] = before.cells[CellIndex(before, col, row)];
        }
    }
    return true;
}


} // namespace TD2::Editor
