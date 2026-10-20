#include "TD2LevelEditor.h"
#include "HIKARI/Project/GameProjectEditorModule.h"

namespace TD2 {
namespace {
// 専用の入口はレベル編集だけ。
class EditorModule final : public HIKARI::EDITOR_PROJECT::IGameProjectEditorModule {
public:
    std::string_view GetModuleId() const override { return "Project_TD2_TV"; }
    std::vector<std::unique_ptr<HIKARI::EDITOR_PROJECT::IEditorWorkspace>> CreateEditorWorkspaces() const override {
        return Editor::Level::MakeLevelWorkspaces();
    }
};
}
}

namespace HIKARI::EDITOR_PROJECT {
std::unique_ptr<IGameProjectEditorModule> CreateGameProjectEditorModule() {
    return std::make_unique<TD2::EditorModule>();
}
}
