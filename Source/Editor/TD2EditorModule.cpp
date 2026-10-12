#include "TD2MapWorkspaces.h"
#include "HIKARI/Project/GameProjectEditorModule.h"

namespace TD2 {
namespace {
// 通路・自作カメラ頁は登録しない。地図編集と世界配置だけを専用入口にする。
class EditorModule final : public HIKARI::EDITOR_PROJECT::IGameProjectEditorModule {
public:
    std::string_view GetModuleId() const override { return "Project_TD2_TV"; }
    std::vector<std::unique_ptr<HIKARI::EDITOR_PROJECT::IEditorWorkspace>> CreateEditorWorkspaces() const override {
        return Editor::MakeMapWorkspaces();
    }
};
}
}

namespace HIKARI::EDITOR_PROJECT {
std::unique_ptr<IGameProjectEditorModule> CreateGameProjectEditorModule() {
    return std::make_unique<TD2::EditorModule>();
}
}
