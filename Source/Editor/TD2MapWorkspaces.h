#pragma once
#include "HIKARI/Project/EditorWorkspace.h"
#include <memory>
#include <vector>

namespace TD2::Editor {
// 二つの入口は同じ作者データを共有する。
std::vector<std::unique_ptr<HIKARI::EDITOR_PROJECT::IEditorWorkspace>> MakeMapWorkspaces();
}
