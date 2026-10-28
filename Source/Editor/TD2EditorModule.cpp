#include "TD2LevelEditor.h"
#include "TD2LevelOperations.h"
#include "TD2PopupEditor.h"
#include "HIKARI/Project/GameProjectEditorModule.h"

namespace TD2 {
namespace {
// 専用の入口はレベル編集（先頭）と弾窓。レベル編集の操作はエンジンの AI からも呼べる（td2.level.*）。
class EditorModule final : public HIKARI::EDITOR_PROJECT::IGameProjectEditorModule {
public:
    std::string_view GetModuleId() const override { return "Project_TD2_TV"; }
    std::vector<std::unique_ptr<HIKARI::EDITOR_PROJECT::IEditorWorkspace>> CreateEditorWorkspaces() const override {
        auto workspaces = Editor::Level::MakeLevelWorkspaces();
        for (auto& workspace : Editor::Popup::MakePopupWorkspaces()) { workspaces.push_back(std::move(workspace)); }
        return workspaces;
    }
    void RegisterEditorOperations(HIKARI::EDITOR_PROJECT::IEditorOperationRegistrar& registrar) override {
        Editor::Level::RegisterLevelOperations(registrar);
    }
};
}
}

namespace HIKARI::EDITOR_PROJECT {
std::unique_ptr<IGameProjectEditorModule> CreateGameProjectEditorModule() {
    return std::make_unique<TD2::EditorModule>();
}
}
