#pragma once

#include <vector>

namespace codextex {

enum class KeyboardCommand {
    OpenObj,
    OpenTexture,
    SaveTexture,
    UndoTexture,
    RedoTexture,
};

struct KeyboardShortcutContext {
    bool primaryAssetLoadingEnabled{};
    bool textureLoaded{};
    bool canUndo{};
    bool canRedo{};
};

// Call once per ImGui frame, before drawing menus, from a stable window scope.
// Text widgets retain their own Ctrl+Z/Y handling while text input is active.
std::vector<KeyboardCommand> PollKeyboardShortcuts(const KeyboardShortcutContext& context);

} // namespace codextex
