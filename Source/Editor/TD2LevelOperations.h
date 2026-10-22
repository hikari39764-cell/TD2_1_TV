#pragma once
// レベル編集の操作（引擎の AI からも人からも呼べる）。画面を描かずに、レベル編集と同じ読み書き・検査・カメラの作り方で
// 場面を直す。処理の中では登録時に受け取った命令の入口から場面の操作を呼び、取り消しも工作区と同じ履歴へ積む。
#include "HIKARI/Project/EditorOperations.h"

namespace TD2::Editor::Level {
void RegisterLevelOperations(HIKARI::EDITOR_PROJECT::IEditorOperationRegistrar& registrar);
}
