#include "TD2LevelEditor.h"
#include "TD2LevelOperations.h"
#include "HIKARI/Project/GameProjectEditorModule.h"

namespace TD2 {
namespace {
// 専用の入口はレベル編集だけ。レベル編集の操作は引擎の AI からも呼べる（td2.level.*）。
class EditorModule final : public HIKARI::EDITOR_PROJECT::IGameProjectEditorModule {
public:
    std::string_view GetModuleId() const override { return "Project_TD2_TV"; }
    std::vector<std::unique_ptr<HIKARI::EDITOR_PROJECT::IEditorWorkspace>> CreateEditorWorkspaces() const override {
        return Editor::Level::MakeLevelWorkspaces();
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
