#pragma once

#include "codex/CodexBridge.hpp"
#include "core/AppSettings.hpp"
#include "core/Localization.hpp"
#include "core/Mask.hpp"
#include "core/Mesh.hpp"
#include "core/TextureImage.hpp"
#include "graphics/Renderer.hpp"

#include <Windows.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <optional>
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
    enum class EditMode { Navigate, Face };
    enum class ProjectionViewMode { Working, Original, GeneratedFull };

    void DrawUi();
    void DrawMenuBar();
    void DrawViewport();
    void DrawTools();
    void DrawTexturePreview();
    void DrawSessionTemp();
    void HandleCodexEvents();
    struct ProjectionTab;
    void HandleViewportInput(const Vec2& topLeft, const Vec2& size, ProjectionTab* tab,
                             bool hovered);
    void ApplyDpiScale(float scale);

    bool OpenObj();
    bool OpenTexture();
    bool OpenRecentPrimaryAssets();
    void RememberRecentPrimaryAssets();
    [[nodiscard]] bool RecentPrimaryAssetsAvailable() const;
    bool OpenProjection(ProjectionTab& tab);
    bool AddReferenceAsset();
    void RemoveReferenceAsset(std::size_t index);
    bool RebuildReferenceAssets();
    bool SaveTexture(bool choosePath);
    bool CreateProjectionTab(bool generate);
    bool SaveCodexSettingsForRequest();
    bool SelectUiLanguage(UiLanguage language);
    void ReloadUiFont();
    void NormalizeCodexSettings();
    void CloseProjectionTab(std::uint64_t id);
    void ClearProjectionTabs();
    ProjectionTab* ActiveProjectionTab();
    ProjectionTab* FindProjectionTab(std::uint64_t id);
    void ActivateMainViewport();
    void ActivateProjectionTab(ProjectionTab& tab);
    void FitCamera();
    void HideSelectedFaces();
    void UndoHiddenFaces();
    void ShowAllFaces();
    void ApplyMaskChange(ProjectionTab& tab);
    void Bake(ProjectionTab& tab);
    void UndoTexture();
    void RedoTexture();
    void RefreshTempFiles();
    void SelectTempFile(const std::filesystem::path& path);
    void DeleteTempFile(const std::filesystem::path& path);
    void DeleteAllTempFiles();
    void SetStatus(std::string status, bool error = false);
    [[nodiscard]] const char* Tr(std::string_view english) const noexcept;
    [[nodiscard]] std::string LocalizedMessage(std::string_view message) const;
    [[nodiscard]] std::string WindowLabel(std::string_view english,
                                          std::string_view stableId) const;

    std::filesystem::path OpenFileDialog(std::string_view title, const wchar_t* filter);
    std::filesystem::path SaveFileDialog(std::string_view title, const wchar_t* filter,
                                         const std::filesystem::path& initial);
    std::filesystem::path CreateSessionDirectory() const;

    HINSTANCE instance_{};
    HWND window_{};
    Renderer renderer_;
    CodexBridge codex_;
    Mesh mesh_;
    TextureImage sourceTexture_;
    CameraState camera_{};

    struct ReferenceAsset {
        Mesh mesh;
        TextureImage texture;
        std::filesystem::path objPath;
        std::filesystem::path texturePath;
    };
    std::vector<ReferenceAsset> referenceAssets_;

    struct ProjectionTab {
        std::uint64_t id{};
        Renderer::ProjectionFrame frame;
        CameraState camera{};
        std::vector<std::uint8_t> hiddenFaces;
        TextureImage projectionImage;
        MaskImage mask;
        std::filesystem::path capturePath;
        std::filesystem::path projectionPath;
        std::string status = "Waiting for projection image.";
        std::string model;
        std::string reasoningEffort;
        bool statusIsError{};
        bool projectionLoaded{};
        bool referenceAssetsVisible{true};
        bool applied{};
        ProjectionViewMode viewMode{ProjectionViewMode::Working};
        float brushRadius{28.0f};
        int featherRadius{16};
        float maxAngleDegrees{75.0f};
        LocalSideFilter localSideFilter{LocalSideFilter::Both};
        std::uint64_t baseTextureRevision{};
        std::optional<std::chrono::steady_clock::time_point> generationStartedAt;
    };
    std::vector<ProjectionTab> projectionTabs_;

    struct TempFileInfo {
        std::filesystem::path path;
        std::uintmax_t size{};
    };
    std::vector<TempFileInfo> tempFiles_;
    std::filesystem::path selectedTempFile_;
    TextureImage tempPreviewImage_;
    std::string tempPreviewText_;
    std::string tempPreviewMessage_;

    std::filesystem::path sessionDirectory_;
    std::filesystem::path texturePath_;
    std::filesystem::path imageGenLogPath_;
    std::filesystem::path settingsPath_;
    CodexRequestSettings codexSettings_;
    CodexRequestSettings persistedSettings_;
    UiLanguage uiLanguage_{UiLanguage::English};
    bool settingsLoadedFromDisk_{};
    bool settingsChanged_{};
    std::string settingsMessage_;
    bool meshLoaded_{};
    bool textureLoaded_{};
    bool dirty_{};
    bool referenceAssetsVisible_{true};
    bool shadingEnabled_{};
    bool mainOriginalTexturePreview_{};
    std::array<float, 3> viewportBackgroundColor_{0.24f, 0.30f, 0.36f};
    bool dockLayoutInitialized_{};
    bool imguiBackendsInitialized_{};
    bool uiFontReloadPending_{};
    float dpiScale_{1.0f};

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
    std::array<char, 2048> generationPrompt_{};
    std::optional<std::uint64_t> activeProjectionId_;
    std::optional<std::uint64_t> pendingProjectionSelection_;
    std::optional<std::uint64_t> rendererProjectionId_;
    std::uint64_t nextProjectionId_{1};
    std::uint64_t textureRevision_{};
    bool tempFilesDirty_{true};
    std::string status_ = "Open an OBJ and a PNG texture.";
    bool statusIsError_{};
};

} // namespace codextex
