#include "app/KeyboardShortcuts.hpp"

#include <catch2/catch_test_macros.hpp>
#include <imgui.h>

#include <array>
#include <string>
#include <utility>
#include <vector>

namespace {

class HeadlessKeyboardUi {
public:
    HeadlessKeyboardUi() : previousContext_(ImGui::GetCurrentContext()) {
        context_ = ImGui::CreateContext();
        ImGui::SetCurrentContext(context_);
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.DisplaySize = {800, 600};
        io.DeltaTime = 1.0f / 60.0f;
        io.ConfigInputTrickleEventQueue = false;
        unsigned char* pixels = nullptr;
        int width = 0;
        int height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    }

    ~HeadlessKeyboardUi() {
        ImGui::DestroyContext(context_);
        ImGui::SetCurrentContext(previousContext_);
    }

    std::vector<codextex::KeyboardCommand> Frame(const codextex::KeyboardShortcutContext& context,
                                                const bool focusText = false) {
        ImGui::NewFrame();
        auto commands = codextex::PollKeyboardShortcuts(context);
        ImGui::SetNextWindowPos({10, 10}, ImGuiCond_Always);
        ImGui::SetNextWindowSize({400, 200}, ImGuiCond_Always);
        ImGui::Begin("Editor");
        if (focusText) ImGui::SetKeyboardFocusHere();
        ImGui::InputText("Prompt", text_.data(), text_.size());
        textActive_ = ImGui::IsItemActive();
        ImGui::End();
        ImGui::Render();
        return commands;
    }

    void Prime(const codextex::KeyboardShortcutContext& context) {
        Frame(context);
        Frame(context);
    }

    std::vector<codextex::KeyboardCommand> Press(const ImGuiKey key,
                                                const codextex::KeyboardShortcutContext& context) {
        auto& io = ImGui::GetIO();
        io.AddKeyEvent(ImGuiMod_Ctrl, true);
        io.AddKeyEvent(key, true);
        auto commands = Frame(context);
        io.AddKeyEvent(key, false);
        io.AddKeyEvent(ImGuiMod_Ctrl, false);
        Frame(context);
        return commands;
    }

    [[nodiscard]] std::string Text() const { return text_.data(); }
    [[nodiscard]] bool TextActive() const noexcept { return textActive_; }

private:
    ImGuiContext* previousContext_{};
    ImGuiContext* context_{};
    std::array<char, 128> text_{};
    bool textActive_{};
};

} // namespace

TEST_CASE("Global file and texture shortcuts route outside an open menu") {
    HeadlessKeyboardUi ui;
    const codextex::KeyboardShortcutContext context{true, true, true, true};
    ui.Prime(context);
    const std::array bindings{
        std::pair{ImGuiKey_O, codextex::KeyboardCommand::OpenObj},
        std::pair{ImGuiKey_T, codextex::KeyboardCommand::OpenTexture},
        std::pair{ImGuiKey_S, codextex::KeyboardCommand::SaveTexture},
        std::pair{ImGuiKey_Z, codextex::KeyboardCommand::UndoTexture},
        std::pair{ImGuiKey_Y, codextex::KeyboardCommand::RedoTexture},
    };
    for (const auto& [key, command] : bindings) {
        CAPTURE(key);
        const auto requested = ui.Press(key, context);
        REQUIRE(requested.size() == 1);
        CHECK(requested.front() == command);
    }
}

TEST_CASE("Keyboard shortcuts respect asset loading and texture history availability") {
    HeadlessKeyboardUi ui;
    codextex::KeyboardShortcutContext context{};
    ui.Prime(context);
    for (const auto key : {ImGuiKey_O, ImGuiKey_T, ImGuiKey_S, ImGuiKey_Z, ImGuiKey_Y}) {
        CHECK(ui.Press(key, context).empty());
    }

    context.textureLoaded = true;
    ui.Prime(context);
    CHECK(ui.Press(ImGuiKey_O, context).empty());
    CHECK(ui.Press(ImGuiKey_T, context).empty());
    const auto save = ui.Press(ImGuiKey_S, context);
    REQUIRE(save.size() == 1);
    CHECK(save.front() == codextex::KeyboardCommand::SaveTexture);
    CHECK(ui.Press(ImGuiKey_Z, context).empty());
    CHECK(ui.Press(ImGuiKey_Y, context).empty());
}

TEST_CASE("Texture shortcuts preserve active text input undo and redo") {
    HeadlessKeyboardUi ui;
    const codextex::KeyboardShortcutContext context{true, true, true, true};
    ui.Prime(context);
    ui.Frame(context, true);
    ui.Frame(context);
    ui.Frame(context);
    REQUIRE(ui.TextActive());
    REQUIRE(ImGui::GetIO().WantTextInput);
    ImGui::GetIO().AddInputCharactersUTF8("abc");
    ui.Frame(context);
    const auto typedText = ui.Text();
    REQUIRE(typedText == "abc");

    CHECK(ui.Press(ImGuiKey_Z, context).empty());
    CHECK(ui.Text() != typedText);
    CHECK(ui.Press(ImGuiKey_Y, context).empty());
    CHECK(ui.Text() == typedText);
    const auto save = ui.Press(ImGuiKey_S, context);
    REQUIRE(save.size() == 1);
    CHECK(save.front() == codextex::KeyboardCommand::SaveTexture);
    CHECK(ui.Text() == typedText);
}

TEST_CASE("Unmodified and held keys do not repeatedly trigger application shortcuts") {
    HeadlessKeyboardUi ui;
    const codextex::KeyboardShortcutContext context{true, true, true, true};
    ui.Prime(context);
    auto& io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiKey_S, true);
    CHECK(ui.Frame(context).empty());
    io.AddKeyEvent(ImGuiKey_S, false);
    ui.Frame(context);
    io.AddKeyEvent(ImGuiMod_Ctrl, true);
    io.AddKeyEvent(ImGuiKey_S, true);
    const auto save = ui.Frame(context);
    REQUIRE(save.size() == 1);
    CHECK(save.front() == codextex::KeyboardCommand::SaveTexture);
    io.DeltaTime = 0.6f;
    CHECK(ui.Frame(context).empty());
    CHECK(ui.Frame(context).empty());
    io.AddKeyEvent(ImGuiKey_S, false);
    io.AddKeyEvent(ImGuiMod_Ctrl, false);
    ui.Frame(context);
}
