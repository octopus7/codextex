#include "app/KeyboardShortcuts.hpp"

#include <imgui.h>

namespace codextex {

std::vector<KeyboardCommand> PollKeyboardShortcuts(const KeyboardShortcutContext& context) {
    std::vector<KeyboardCommand> commands;
    const auto route = [&](const ImGuiKey key, const KeyboardCommand command, const bool enabled) {
        if (enabled && ImGui::Shortcut(ImGuiMod_Ctrl | key, ImGuiInputFlags_RouteGlobal)) {
            commands.push_back(command);
        }
    };
    route(ImGuiKey_O, KeyboardCommand::OpenObj, context.primaryAssetLoadingEnabled);
    route(ImGuiKey_T, KeyboardCommand::OpenTexture, context.primaryAssetLoadingEnabled);
    route(ImGuiKey_S, KeyboardCommand::SaveTexture, context.textureLoaded);
    if (!ImGui::GetIO().WantTextInput) {
        route(ImGuiKey_Z, KeyboardCommand::UndoTexture, context.textureLoaded && context.canUndo);
        route(ImGuiKey_Y, KeyboardCommand::RedoTexture, context.textureLoaded && context.canRedo);
    }
    return commands;
}

} // namespace codextex
