#pragma once
#include "../Common/TD2WorldLayout.h"
#include "HIKARI/Project/EditorWorkspace.h"
#include <memory>
#include <string>
#include <string_view>

namespace TD2::Editor {
// Scene 草稿の提出は呼出側が所有する。演出の明示的なキー書込だけをこの面板が担当する。
class SequenceCameraPanel {
public:
    SequenceCameraPanel();
    ~SequenceCameraPanel();
    void Draw(HIKARI::EDITOR_PROJECT::EditorWorkspaceHostContext&, RegionSequenceBinding& draft,
              std::string_view fromName, std::string_view toName, std::string& error);
    bool ConsumeDraftChanged() noexcept;
    void Reset();
private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace TD2::Editor
