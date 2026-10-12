#pragma once
#include "TD2EditorSupport.h"
#include "../Common/TD2SpritePalette.h"
#include <filesystem>
#include <fstream>
#include <unordered_map>

namespace TD2::Editor {
// SDK の UTF-8 パスを char8_t として渡し、Windows のローカル文字コードへ誤変換しない。
inline std::filesystem::path Utf8Path(std::string_view value) {
    return std::filesystem::path(std::u8string(value.begin(), value.end()));
}
inline Json ReadProjectJson(const std::filesystem::path& path) {
    std::ifstream input(path); if (!input) { return Json{}; }
    return Json::parse(input, nullptr, false);
}

// importer が生成した source meta の GUID を使う。PNG のパスを Texture GUID と偽装しない。
class MapSpriteCatalog {
public:
    void Load(const char* projectRoot) {
        if (!projectRoot || loaded_) { return; }
        loaded_ = true;
        const auto root = Utf8Path(projectRoot);
        const auto settings = ReadProjectJson(root / "ProjectSettings/asset_pipeline.json");
        const auto metaRoot = settings.is_object() ? settings.value("sourceMetaRoot", "ProjectSettings/AssetMeta") : "ProjectSettings/AssetMeta";
        const auto manifest = ReadProjectJson(root / "Assets/TD2/Sprites04/td2_sprites04.manifest.json");
        if (!manifest.is_object() || !manifest.contains("frames") || !manifest["frames"].is_array()) { warning = "地图精灵清单尚未导入"; return; }
        for (const auto& frame : manifest["frames"]) {
            if (!frame.is_object() || !frame.contains("key") || !frame["key"].is_string() || !frame.contains("path") || !frame["path"].is_string()) { continue; }
            const auto meta = ReadProjectJson(root / Utf8Path(metaRoot) / Utf8Path(SpritePalette::FramePath(frame["key"].get<std::string>()) + ".hikari.asset.json"));
            if (meta.is_object() && meta.contains("guid") && meta["guid"].is_string()) { guids_[frame["key"].get<std::string>()] = meta["guid"].get<std::string>(); }
        }
        if (guids_.empty()) { warning = "地图精灵缺少导入 GUID"; }
    }
    bool Draw(IEditorWorkspaceUi& ui, std::string_view key, EditorWorkspacePoint a, EditorWorkspacePoint b) const {
        const auto it = guids_.find(std::string(key));
        if (it == guids_.end()) { return false; }
        return ui.CanvasImage(it->second, a, b);
    }
    std::string warning;
private:
    bool loaded_ = false;
    std::unordered_map<std::string, std::string> guids_;
};
inline MapSpriteCatalog gMapSprites;
}
