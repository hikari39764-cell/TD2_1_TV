#pragma once
#include "HIKARI/Project/EditorWorkspace.h"
#include <memory>
#include <vector>

namespace TD2::Editor {
// 二つの入口は同じ Scene 作者モデルを共有し、往復で未適用データを複製しない。
std::vector<std::unique_ptr<HIKARI::EDITOR_PROJECT::IEditorWorkspace>> MakeMapWorkspaces();
}
