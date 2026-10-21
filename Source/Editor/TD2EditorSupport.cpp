#include "TD2EditorSupport.h"
#include "HIKARI/Project/GameProjectEditorModule.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace TD2::Editor {

using Json = nlohmann::json;
using namespace HIKARI::EDITOR_PROJECT;

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

bool QueryAll(EditorWorkspaceHostContext& context, std::string_view operation, Json parameters, std::string_view listKey,
              const std::function<bool(const Json&)>& visit, std::string& error) {
    if (!parameters.contains("limit")) { parameters["limit"] = 128; }
    for (uint64_t offset = 0;;) {
        parameters["offset"] = offset;
        const auto page = Execute(context, operation, parameters);
        if (!page.ok) { error = page.error; return false; }
        const auto entries = page.data.find(listKey);
        if (entries == page.data.end() || !entries->is_array()) {
            error = std::string(operation) + ": INVALID_RESULT: " + std::string(listKey) + " の配列がありません"; return false;
        }
        for (const auto& entry : *entries) { if (!visit(entry)) { return false; } }
        const auto next = page.data.find("nextOffset");
        if (next == page.data.end()) { return true; }
        const uint64_t nextOffset = ObjectId(*next);
        if (nextOffset <= offset || entries->empty()) { error = std::string(operation) + ": INVALID_RESULT: ページが進みません"; return false; }
        offset = nextOffset;
    }
}

Json PropertyCommand(uint64_t objectId, uint64_t componentId, std::string_view propertyId, const Json& value) {
    return {{"objectId", objectId}, {"componentId", componentId}, {"propertyId", std::string(propertyId)}, {"value", value}};
}

bool Transact(EditorWorkspaceHostContext& context, const Json& commands, std::string& error) {
    if (commands.empty()) { return true; }
    const auto result = Execute(context, "authoring.transaction", {{"commands", commands}});
    if (!result.ok) { error = result.error; return false; }
    return true;
}

// 決まった領域操作だけを使い、ブリッジには依存しない。
bool QueryObjects(EditorWorkspaceHostContext& context, std::string_view componentType,
                  std::vector<SceneItem>& objects, std::string& error) {
    objects.clear();
    Json parameters{{"limit", 64}};
    if (!componentType.empty()) { parameters["componentType"] = componentType; }
    return QueryAll(context, "scene.objects.query", parameters, "objects", [&objects, &error](const Json& object) {
        if (!object.is_object() || !object.contains("objectId") || !object.contains("name") ||
            ObjectId(object["objectId"]) == 0 || !object["name"].is_string()) {
            error = "scene.objects.query: INVALID_RESULT: 物体の ID か名前がありません"; return false;
        }
        objects.push_back({ObjectId(object["objectId"]), object["name"].get<std::string>()});
        return true;
    }, error);
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
        error = "component.instances.query: この物体に TD2 テレビの区域がありません。追加ボタンを使ってください"; return false;
    }
    target = {objectId, componentId};
    return true;
}

// component.describe の fields[] を id で読む。
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
                          "screenSlot", "spawnX", "spawnY", "entryRegion", "devices", "worldPlaced", "worldX", "worldY",
                          "screenBrightness", "stageAnchor"}}});
    if (!result.ok) { error = result.error; return false; }
    const auto fields = result.data.find("fields");
    if (fields == result.data.end() || !fields->is_array()) {
        error = "component.describe: INVALID_RESULT: fields の配列がありません"; return false;
    }
    const auto text = [&fields, &error](std::string_view id, std::string& value) {
        const auto* field = FieldValue(*fields, id);
        if (field == nullptr || !field->is_string()) {
            error = "component.describe: INVALID_RESULT: 文字列の項目 " + std::string(id) + " がありません"; return false;
        }
        value = field->get<std::string>();
        return true;
    };
    const auto number = [&fields, &error](std::string_view id, int& value) {
        const auto* field = FieldValue(*fields, id);
        if (field == nullptr || !field->is_number_integer() || *field < 1 ||
            *field > (std::numeric_limits<int>::max)()) {
            error = "component.describe: INVALID_RESULT: 正の整数の項目 " + std::string(id) + " がありません"; return false;
        }
        value = field->get<int>();
        return true;
    };
    const auto* screen = FieldValue(*fields, "screenObject");
    if (screen == nullptr || !screen->is_number_integer() || *screen < 0) {
        error = "component.describe: INVALID_RESULT: screenObject の参照が無効です"; return false;
    }
    settings.screenObject = ObjectId(*screen);
    const auto* spawnX = FieldValue(*fields, "spawnX");
    const auto* spawnY = FieldValue(*fields, "spawnY");
    const auto* entry = FieldValue(*fields, "entryRegion");
    if (spawnX == nullptr || !spawnX->is_number() || spawnY == nullptr || !spawnY->is_number() ||
        entry == nullptr || !entry->is_boolean()) {
        error = "component.describe: INVALID_RESULT: スポーン位置か開始区域の項目がありません"; return false;
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
        error = "component.describe: INVALID_RESULT: 世界の配置の項目が無いか無効です"; return false;
    }
    settings.worldPlaced = placed->get<bool>(); settings.worldX = worldX->get<int>(); settings.worldY = worldY->get<int>();
    // 古い Scene に無い項目は既定値で読む。型が違う時はエラーにする。
    settings.screenBrightness = 2.2f;
    if (const auto* brightness = FieldValue(*fields, "screenBrightness")) {
        if (!brightness->is_number() || !std::isfinite(brightness->get<float>())) { error = "component.describe: INVALID_RESULT: 画面の明るさが無効です"; return false; }
        settings.screenBrightness = brightness->get<float>();
    }
    settings.stageAnchor.clear();
    if (const auto* anchor = FieldValue(*fields, "stageAnchor"); anchor && anchor->is_string()) { settings.stageAnchor = anchor->get<std::string>(); }
    return text("regionId", settings.region.regionId) && number("cols", settings.region.cols) &&
        number("rows", settings.region.rows) && number("tilePixels", settings.region.tilePixels) &&
        text("cells", settings.region.cells) && text("screenSlot", settings.screenSlot) && text("devices", settings.devices);
}

void AppendRegionChanges(Json& commands, const RegionTarget& target, const RegionSettings& before, const RegionSettings& after) {
    const auto add = [&commands, &target](std::string_view id, const auto& oldValue, const auto& newValue) {
        if (oldValue != newValue) { commands.push_back(PropertyCommand(target.objectId, target.componentId, id, newValue)); }
    };
    add("regionId", before.region.regionId, after.region.regionId);
    add("cols", before.region.cols, after.region.cols);
    add("rows", before.region.rows, after.region.rows);
    add("tilePixels", before.region.tilePixels, after.region.tilePixels);
    add("cells", before.region.cells, after.region.cells);
    add("screenObject", before.screenObject, after.screenObject);
    add("screenSlot", before.screenSlot, after.screenSlot);
    add("screenBrightness", before.screenBrightness, after.screenBrightness);
    add("spawnX", before.spawnX, after.spawnX);
    add("spawnY", before.spawnY, after.spawnY);
    add("entryRegion", before.entryRegion, after.entryRegion);
    add("devices", before.devices, after.devices);
    add("worldPlaced", before.worldPlaced, after.worldPlaced);
    add("worldX", before.worldX, after.worldX);
    add("worldY", before.worldY, after.worldY);
    add("stageAnchor", before.stageAnchor, after.stageAnchor);
}

std::string ScreenSlotWarning(EditorWorkspaceHostContext& context, const RegionSettings& settings) {
    if (!settings.screenObject) { return {}; }
    if (settings.screenSlot.empty()) { return "画面の材質スロット名を指定してください"; }
    const auto result = Execute(context, "scene.objects.query", {{"objectIds", {settings.screenObject}}, {"detail", true}});
    if (!result.ok) { return result.error; }
    for (const auto& object : result.data.value("objects", Json::array())) {
        if (!object.is_object() || ObjectId(object.value("objectId", Json{})) != settings.screenObject) { continue; }
        const auto model = object.find("model");
        if (model == object.end() || !model->is_object()) { break; }
        // 一覧が途中で切れている時は確かめられないので注意しない。
        if (model->value("materialSlotsTruncated", false)) { return {}; }
        for (const auto& slot : model->value("materialSlots", Json::array())) {
            if (slot.is_object() && slot.value("name", "") == settings.screenSlot) { return {}; }
        }
        return "画面の材質スロットが見つかりません：" + settings.screenSlot;
    }
    return "画面の物体のモデルを確認できません";
}

bool AddRegion(EditorWorkspaceHostContext& context, uint64_t objectId, bool bindSelf, std::string& status) {
    const auto instances = Execute(context, "component.instances.query", {{"objectId", objectId}});
    if (!instances.ok) { status = instances.error; return false; }
    const auto components = instances.data.find("components");
    if (components == instances.data.end() || !components->is_array()) {
        status = "component.instances.query: INVALID_RESULT: components の配列がありません"; return false;
    }
    if (RegionComponentId(*components) != 0) { return true; }
    const auto result = Execute(context, "component.add", {{"objectId", objectId}, {"componentType", kRegionComponentType}});
    if (!result.ok) { status = result.error; return false; }
    const auto added = result.data.find("addedComponents");
    const uint64_t componentId = added == result.data.end() ? 0 : RegionComponentId(*added);
    if (componentId == 0) { status = "component.add: INVALID_RESULT: 新しい区域の部品の ID がありません"; return false; }
    // 新しい ID は Scene の Object ID から作る。既存の ID は変えない。
    Json commands = Json::array({PropertyCommand(objectId, componentId, "regionId", "td2.region." + std::to_string(objectId))});
    // 最初の地図だけを開始区域にする。
    std::vector<SceneItem> regions;
    if (!QueryObjects(context, kRegionComponentType, regions, status)) { return false; }
    bool firstEntry = true;
    for (const auto& item : regions) {
        if (item.objectId == objectId) { continue; }
        RegionTarget existingTarget; RegionSettings existing;
        if (!ReadRegion(context, item.objectId, existingTarget, existing, status)) { return false; }
        if (existing.entryRegion) { firstEntry = false; break; }
    }
    commands.push_back(PropertyCommand(objectId, componentId, "entryRegion", firstEntry));
    if (bindSelf) {
        commands.push_back(PropertyCommand(objectId, componentId, "screenObject", objectId));
        commands.push_back(PropertyCommand(objectId, componentId, "screenSlot", "TD2_Screen"));
    }
    if (std::string error; !Transact(context, commands, error)) {
        status = "区域の部品は追加しましたが、初期設定に失敗しました：" + error; return false;
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

// 左上の重なる部分を残し、増えたマスは空にする。縮小で消える範囲は適用前に表示する。
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


}
