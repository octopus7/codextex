#pragma once

#include "app/ProjectionWorkspace.hpp"
#include "codex/AsyncCodexClient.hpp"
#include "core/AppSettings.hpp"
#include "core/Localization.hpp"
#include "core/Mask.hpp"
#include "core/ProjectionViewTransform.hpp"
#include "core/Mesh.hpp"
#include "core/TextureImage.hpp"
#include "core/TextureHistory.hpp"
#include "graphics/Renderer.hpp"

#include <Windows.h>

#include <array>
#include <chrono>
#include <cstdint>
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

    void DrawUi();
    void DrawMenuBar();
    void DrawViewport();
    void DrawTools();
    void DrawPromptHistory();
    void DrawTexturePreview();
    void DrawSessionTemp();
    void HandleCodexEvents();
    using ProjectionTab = ProjectionWorkspace;
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
    void RecordGenerationDuration(ProjectionTab& tab);
    bool DeletePromptHistoryEntry(std::size_t index);
    void CleanupProjectionTemp(ProjectionTab& tab);
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
    bool UploadProjectionMask(ProjectionTab& tab);
    bool UploadProjectionImage(ProjectionTab& tab);
    bool RefreshProjectionWorkingPreview(ProjectionTab& tab);
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
    AsyncCodexClient codex_;
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

    ProjectionWorkspaces projectionTabs_;
    std::optional<std::uint64_t> workingPreviewProjectionId_;

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
    std::filesystem::path generationArchiveDirectory_;
    std::filesystem::path settingsPath_;
    CodexRequestSettings codexSettings_;
    CodexRequestSettings persistedSettings_;
    UiLanguage uiLanguage_{UiLanguage::English};
    bool settingsLoadedFromDisk_{};
    bool settingsChanged_{};
    std::string settingsMessage_;
    bool meshLoaded_{};
    bool textureLoaded_{};
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
    TextureHistory textureHistory_;

    EditMode editMode_{EditMode::Navigate};
    bool useLasso_{};
    bool maskInclude_{true};
    bool lassoActive_{};
    std::vector<Vec2> lassoPoints_;
    std::array<char, 2048> generationPrompt_{};
    std::optional<std::size_t> selectedPromptHistoryIndex_;
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
