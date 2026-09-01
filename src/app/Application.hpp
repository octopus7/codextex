#pragma once

#include "codex/CodexBridge.hpp"
#include "core/Mask.hpp"
#include "core/Mesh.hpp"
#include "core/TextureImage.hpp"
#include "graphics/Renderer.hpp"

#include <Windows.h>

#include <array>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <string>
#include <vector>

namespace codextex {

class Application {
public:
    bool Initialize(HINSTANCE instance, int showCommand, std::string& error);
    int Run();
    void Shutdown();
    bool CanClose();

    static LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

private:
    enum class EditMode { Navigate, Face, Mask };

    void DrawUi();
    void DrawMenuBar();
    void DrawViewport();
    void DrawTools();
    void DrawTexturePreview();
    void HandleCodexEvents();
    void HandleViewportInput(const Vec2& topLeft, const Vec2& size);

    bool OpenObj();
    bool OpenTexture();
    bool OpenProjection();
    bool SaveTexture(bool choosePath);
    bool CaptureView();
    void CancelProjection();
    void FitCamera();
    void HideSelectedFaces();
    void UndoHiddenFaces();
    void ShowAllFaces();
    void ApplyMaskChange();
    void Bake();
    void UndoTexture();
    void RedoTexture();
    void SetStatus(std::string status, bool error = false);

    std::filesystem::path OpenFileDialog(const wchar_t* title, const wchar_t* filter);
    std::filesystem::path SaveFileDialog(const wchar_t* title, const wchar_t* filter,
                                         const std::filesystem::path& initial);
    std::filesystem::path CreateSessionDirectory() const;

    HINSTANCE instance_{};
    HWND window_{};
    Renderer renderer_;
    CodexBridge codex_;
    Mesh mesh_;
    TextureImage sourceTexture_;
    TextureImage projectionImage_;
    MaskImage mask_;
    CameraState camera_{};

    std::filesystem::path sessionDirectory_;
    std::filesystem::path texturePath_;
    std::filesystem::path capturePath_;
    std::filesystem::path projectionPath_;
    bool meshLoaded_{};
    bool textureLoaded_{};
    bool captured_{};
    bool projectionLoaded_{};
    bool dirty_{};

    std::vector<std::uint8_t> hiddenFaces_;
    std::vector<std::uint8_t> selectedFaces_;
    std::vector<std::vector<std::uint8_t>> hiddenHistory_;
    std::deque<TextureImage> undoTextures_;
    std::deque<TextureImage> redoTextures_;

    EditMode editMode_{EditMode::Navigate};
    bool useLasso_{};
    bool maskInclude_{true};
    bool lassoActive_{};
    std::vector<Vec2> lassoPoints_;
    float brushRadius_{28.0f};
    int featherRadius_{16};
    float maxAngleDegrees_{75.0f};
    std::array<char, 2048> generationPrompt_{};
    std::string status_ = "Open an OBJ and a PNG texture.";
    bool statusIsError_{};
};

} // namespace codextex
