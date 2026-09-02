#include "app/Application.hpp"

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <commdlg.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <iomanip>
#include <numbers>
#include <optional>
#include <sstream>
#include <system_error>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND window, UINT message, WPARAM wParam, LPARAM lParam);

namespace codextex {
namespace {

constexpr wchar_t kWindowClass[] = L"CodexTexWindow";
constexpr std::size_t kUndoLimit = 8;
constexpr ImWchar kKoreanGlyphRanges[] = {
    0x0020, 0x00ff, // Basic Latin and Latin-1
    0x1100, 0x11ff, // Hangul Jamo
    0x2000, 0x206f, // General punctuation
    0x3000, 0x30ff, // CJK punctuation and symbols
    0x3130, 0x318f, // Hangul Compatibility Jamo
    0xa960, 0xa97f, // Hangul Jamo Extended-A
    0xac00, 0xd7a3, // All modern precomposed Hangul syllables
    0xd7b0, 0xd7ff, // Hangul Jamo Extended-B
    0,
};

std::string Narrow(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

std::filesystem::path ExecutableDirectory() {
    std::array<wchar_t, 32768> path{};
    const DWORD length = GetModuleFileNameW(nullptr, path.data(),
                                            static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) return {};
    return std::filesystem::path(path.data()).parent_path();
}

std::string ElapsedLabel(const std::chrono::steady_clock::time_point startedAt) {
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - startedAt).count();
    const auto minutes = elapsed / 60;
    const auto seconds = elapsed % 60;
    std::array<char, 64> label{};
    std::snprintf(label.data(), label.size(), "Generating %lld:%02lld",
                  static_cast<long long>(minutes), static_cast<long long>(seconds));
    return label.data();
}

std::string FileSizeLabel(const std::uintmax_t bytes) {
    constexpr double kib = 1024.0;
    constexpr double mib = kib * 1024.0;
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(bytes >= static_cast<std::uintmax_t>(mib) ? 1 : 0);
    if (bytes >= static_cast<std::uintmax_t>(mib)) stream << bytes / mib << " MiB";
    else if (bytes >= 1024) stream << bytes / kib << " KiB";
    else stream << bytes << " B";
    return stream.str();
}

bool IsInsideDirectory(const std::filesystem::path& root,
                       const std::filesystem::path& candidate) {
    std::error_code error;
    const auto canonicalRoot = std::filesystem::weakly_canonical(root, error);
    if (error) return false;
    const auto canonicalCandidate = std::filesystem::weakly_canonical(candidate, error);
    if (error) return false;
    const auto relative = canonicalCandidate.lexically_relative(canonicalRoot);
    if (relative.empty() || relative.is_absolute()) return false;
    const auto first = relative.begin();
    return first != relative.end() && *first != L"..";
}

bool SameAspect(const TextureImage& lhs, const TextureImage& rhs) {
    if (lhs.Empty() || rhs.Empty()) return false;
    const double left = static_cast<double>(lhs.Width()) / lhs.Height();
    const double right = static_cast<double>(rhs.Width()) / rhs.Height();
    return std::abs(left - right) < 0.005;
}

struct SquareCropFrame {
    Vec2 origin;
    float side{};

    [[nodiscard]] bool Contains(const Vec2& point) const noexcept {
        return point.x >= origin.x && point.y >= origin.y &&
               point.x <= origin.x + side && point.y <= origin.y + side;
    }
};

SquareCropFrame CenteredSquare(const Vec2& size) {
    const float side = std::max(std::min(size.x, size.y), 1.0f);
    return {{(size.x - side) * 0.5f, (size.y - side) * 0.5f}, side};
}

float Dot(const Vec3& a, const Vec3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vec3 Cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x};
}

Vec3 Normalize(const Vec3& value) {
    const float length = std::sqrt(Dot(value, value));
    if (length < 1.0e-6f) return {1, 0, 0};
    return {value.x / length, value.y / length, value.z / length};
}

const CodexModelInfo* FindModel(const std::vector<CodexModelInfo>& models,
                                const std::string& id) {
    const auto found = std::ranges::find(models, id, &CodexModelInfo::id);
    return found == models.end() ? nullptr : &*found;
}

bool SupportsEffort(const CodexModelInfo& model, const std::string& effort) {
    return std::ranges::any_of(model.supportedReasoningEfforts,
        [&effort](const CodexReasoningOption& option) { return option.value == effort; });
}

bool LoadUiFont(ImGuiIO& io, const float dpiScale) {
    std::array<wchar_t, MAX_PATH> windowsDirectory{};
    const UINT length = GetWindowsDirectoryW(windowsDirectory.data(),
                                             static_cast<UINT>(windowsDirectory.size()));
    if (length == 0 || length >= windowsDirectory.size()) return false;

    const std::filesystem::path fontsDirectory =
        std::filesystem::path(windowsDirectory.data()) / L"Fonts";
    constexpr std::array<const wchar_t*, 3> candidates{
        L"malgun.ttf", L"malgunsl.ttf", L"gulim.ttc"};
    for (const wchar_t* filename : candidates) {
        const std::filesystem::path fontPath = fontsDirectory / filename;
        std::error_code fileError;
        if (!std::filesystem::is_regular_file(fontPath, fileError)) continue;
        const std::string utf8Path = Narrow(fontPath);
        if (ImFont* font = io.Fonts->AddFontFromFileTTF(
                utf8Path.c_str(), 17.0f * dpiScale, nullptr, kKoreanGlyphRanges)) {
            io.FontDefault = font;
            return true;
        }
    }
    ImFontConfig fallbackConfig{};
    fallbackConfig.SizePixels = 13.0f * dpiScale;
    io.FontDefault = io.Fonts->AddFontDefault(&fallbackConfig);
    return false;
}

} // namespace

bool Application::Initialize(HINSTANCE instance, const int showCommand, std::string& error) {
    instance_ = instance;
    const POINT primaryPoint{};
    dpiScale_ = std::clamp(ImGui_ImplWin32_GetDpiScaleForMonitor(
                               MonitorFromPoint(primaryPoint, MONITOR_DEFAULTTOPRIMARY)),
                           1.0f, 4.0f);
    WNDCLASSEXW windowClass{sizeof(WNDCLASSEXW)};
    windowClass.style = CS_CLASSDC;
    windowClass.lpfnWndProc = WindowProcedure;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&windowClass)) {
        error = "Could not register the CodexTex window class.";
        return false;
    }
    const int initialWidth = static_cast<int>(std::lround(1500.0f * dpiScale_));
    const int initialHeight = static_cast<int>(std::lround(900.0f * dpiScale_));
    window_ = CreateWindowExW(0, kWindowClass, L"CodexTex", WS_OVERLAPPEDWINDOW,
                              CW_USEDEFAULT, CW_USEDEFAULT, initialWidth, initialHeight,
                              nullptr, nullptr, instance, this);
    if (!window_) {
        error = "Could not create the CodexTex window.";
        return false;
    }
    if (!renderer_.Initialize(window_, error)) {
        return false;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
    dpiScale_ = std::clamp(ImGui_ImplWin32_GetDpiScaleForHwnd(window_), 1.0f, 4.0f);
    const bool koreanFontLoaded = LoadUiFont(io, dpiScale_);
    ImGui::StyleColorsDark();
    ImGui::GetStyle().WindowRounding = 4.0f;
    ImGui::GetStyle().ScaleAllSizes(dpiScale_);
    ImGui_ImplWin32_Init(window_);
    ImGui_ImplDX11_Init(renderer_.Device(), renderer_.Context());
    imguiBackendsInitialized_ = true;

    sessionDirectory_ = CreateSessionDirectory();
    std::error_code directoryError;
    std::filesystem::create_directories(sessionDirectory_, directoryError);
    const auto executableDirectory = ExecutableDirectory();
    settingsPath_ = executableDirectory.empty()
        ? std::filesystem::path{}
        : executableDirectory / L"CodexTex.settings.json";
    std::string settingsError;
    if (settingsPath_.empty() ||
        !LoadCodexRequestSettings(settingsPath_, codexSettings_,
                                  settingsLoadedFromDisk_, settingsError)) {
        settingsMessage_ = settingsError.empty()
            ? "Could not locate the executable directory for Codex settings."
            : settingsError;
    }
    imageGenLogPath_ = executableDirectory.empty()
        ? std::filesystem::path{}
        : executableDirectory / L"CodexTex-ImageGen.log";
    const bool diagnosticLogReady = !imageGenLogPath_.empty() &&
        codex_.EnableDiagnosticLog(imageGenLogPath_);
    codex_.Start(sessionDirectory_);
    NormalizeCodexSettings();
    if (!diagnosticLogReady) {
        SetStatus("Could not create CodexTex-ImageGen.log beside the executable.", true);
    } else if (!koreanFontLoaded) {
        SetStatus("A Windows Korean font could not be loaded; Korean text may not render.", true);
    }

    ShowWindow(window_, showCommand);
    UpdateWindow(window_);
    return true;
}

int Application::Run() {
    MSG message{};
    while (message.message != WM_QUIT) {
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (message.message == WM_QUIT) break;

        HandleCodexEvents();
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        DrawUi();
        ImGui::Render();
        constexpr float clear[4]{0.035f, 0.038f, 0.045f, 1.0f};
        renderer_.BeginUiFrame(clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        renderer_.Present();
    }
    return static_cast<int>(message.wParam);
}

void Application::Shutdown() {
    codex_.Stop();
    if (ImGui::GetCurrentContext()) {
        imguiBackendsInitialized_ = false;
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }
    renderer_.Shutdown();
    if (window_) {
        DestroyWindow(window_);
        window_ = nullptr;
    }
    UnregisterClassW(kWindowClass, instance_);

    std::error_code error;
    const auto temp = std::filesystem::temp_directory_path(error);
    if (!error && sessionDirectory_.parent_path() == temp &&
        sessionDirectory_.filename().wstring().starts_with(L"CodexTex-")) {
        std::filesystem::remove_all(sessionDirectory_, error);
    }
}

bool Application::CanClose() {
    if (!dirty_) return true;
    const int choice = MessageBoxW(window_, L"Save the modified PNG texture before closing?",
                                   L"CodexTex", MB_ICONQUESTION | MB_YESNOCANCEL);
    if (choice == IDCANCEL) return false;
    if (choice == IDYES) return SaveTexture(false);
    return true;
}

LRESULT CALLBACK Application::WindowProcedure(HWND window, const UINT message, const WPARAM wParam,
                                               const LPARAM lParam) {
    Application* app = reinterpret_cast<Application*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        app = static_cast<Application*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    if (message == WM_DPICHANGED && app) {
        if (ImGui::GetCurrentContext()) {
            ImGui_ImplWin32_WndProcHandler(window, message, wParam, lParam);
        }
        const auto* suggested = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(window, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left, suggested->bottom - suggested->top,
                     SWP_NOACTIVATE | SWP_NOZORDER);
        app->ApplyDpiScale(static_cast<float>(HIWORD(wParam)) / 96.0f);
        return 0;
    }
    if (ImGui::GetCurrentContext() && ImGui_ImplWin32_WndProcHandler(window, message, wParam, lParam)) {
        return TRUE;
    }
    switch (message) {
    case WM_SIZE:
        if (app && wParam != SIZE_MINIMIZED) {
            app->renderer_.Resize(LOWORD(lParam), HIWORD(lParam));
        }
        return 0;
    case WM_CLOSE:
        if (!app || app->CanClose()) DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

void Application::ApplyDpiScale(const float scale) {
    const float nextScale = std::clamp(scale, 1.0f, 4.0f);
    if (std::abs(nextScale - dpiScale_) < 0.01f) return;
    dpiScale_ = nextScale;
    if (!imguiBackendsInitialized_ || !ImGui::GetCurrentContext()) return;

    ImGui_ImplDX11_InvalidateDeviceObjects();
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    if (!LoadUiFont(io, dpiScale_)) {
        SetStatus("A Windows Korean font could not be loaded; Korean text may not render.", true);
    }
    ImGui::StyleColorsDark();
    ImGui::GetStyle().WindowRounding = 4.0f;
    ImGui::GetStyle().ScaleAllSizes(dpiScale_);
    ImGui_ImplDX11_CreateDeviceObjects();
}

void Application::DrawUi() {
    DrawMenuBar();
    const ImGuiViewport* mainViewport = ImGui::GetMainViewport();
    const ImGuiID dockspace = ImGui::DockSpaceOverViewport(0, mainViewport);
    if (!dockLayoutInitialized_) {
        dockLayoutInitialized_ = true;
        ImGui::DockBuilderRemoveNode(dockspace);
        ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspace, mainViewport->WorkSize);
        ImGuiID right = 0;
        ImGuiID center = dockspace;
        ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.28f, &right, &center);
        ImGuiID bottom = 0;
        ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.30f, &bottom, &center);
        ImGui::DockBuilderDockWindow("3D Viewport", center);
        ImGui::DockBuilderDockWindow("Projection Tools", right);
        ImGui::DockBuilderDockWindow("Texture Preview", bottom);
        ImGui::DockBuilderDockWindow("Session Temp", bottom);
        ImGui::DockBuilderFinish(dockspace);
    }
    DrawViewport();
    DrawTools();
    DrawTexturePreview();
    DrawSessionTemp();
}

void Application::DrawMenuBar() {
    if (!ImGui::BeginMainMenuBar()) return;
    if (ImGui::BeginMenu("File")) {
        const bool primaryAssetLoadingEnabled = !activeProjectionId_.has_value();
        if (ImGui::MenuItem("Open OBJ...", "Ctrl+O", false, primaryAssetLoadingEnabled)) OpenObj();
        if (ImGui::MenuItem("Open Texture PNG...", "Ctrl+T", false,
                            primaryAssetLoadingEnabled)) OpenTexture();
        if (ImGui::MenuItem("Add Reference OBJ + PNG...")) AddReferenceAsset();
        ImGui::Separator();
        if (ImGui::MenuItem("Save Texture", "Ctrl+S", false, textureLoaded_)) SaveTexture(false);
        if (ImGui::MenuItem("Save Texture As...", nullptr, false, textureLoaded_)) SaveTexture(true);
        ImGui::Separator();
        if (ImGui::MenuItem("Exit")) PostMessageW(window_, WM_CLOSE, 0, 0);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        if (ImGui::MenuItem("Undo Texture", "Ctrl+Z", false, !undoTextures_.empty())) UndoTexture();
        if (ImGui::MenuItem("Redo Texture", "Ctrl+Y", false, !redoTextures_.empty())) RedoTexture();
        ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
}

void Application::DrawViewport() {
    ImGui::Begin("3D Viewport");
    const auto drawLasso = [this](const ImVec2 topLeft) {
        if (!lassoActive_ || lassoPoints_.size() < 2) return;
        ImDrawList* draw = ImGui::GetWindowDrawList();
        for (std::size_t i = 1; i < lassoPoints_.size(); ++i) {
            draw->AddLine(ImVec2(topLeft.x + lassoPoints_[i - 1].x,
                                 topLeft.y + lassoPoints_[i - 1].y),
                          ImVec2(topLeft.x + lassoPoints_[i].x,
                                 topLeft.y + lassoPoints_[i].y),
                          IM_COL32(255, 205, 40, 255), 2.0f);
        }
    };
    std::optional<std::uint64_t> closeTab;
    if (ImGui::BeginTabBar("ViewportTabs", ImGuiTabBarFlags_Reorderable)) {
        if (ImGui::BeginTabItem("Main Viewport")) {
            ActivateMainViewport();
            Vec2 available{std::max(ImGui::GetContentRegionAvail().x, 1.0f),
                           std::max(ImGui::GetContentRegionAvail().y, 1.0f)};
            renderer_.RenderViewport(static_cast<std::uint32_t>(available.x),
                                     static_cast<std::uint32_t>(available.y), camera_);
            const ImVec2 topLeft = ImGui::GetCursorScreenPos();
            ImGui::Image(reinterpret_cast<ImTextureID>(renderer_.ViewportTexture()),
                         ImVec2(available.x, available.y));
            HandleViewportInput({topLeft.x, topLeft.y}, available, nullptr);
            drawLasso(topLeft);
            if (meshLoaded_) {
                const SquareCropFrame crop = CenteredSquare(available);
                const ImU32 color = IM_COL32(70, 210, 255, 255);
                const ImVec2 minimum{topLeft.x + crop.origin.x, topLeft.y + crop.origin.y};
                const ImVec2 maximum{minimum.x + crop.side, minimum.y + crop.side};
                ImDrawList* draw = ImGui::GetWindowDrawList();
                draw->AddRect(minimum, maximum, color, 0.0f, 0, 2.0f * dpiScale_);
                draw->AddText(ImVec2(minimum.x + 6.0f * dpiScale_, minimum.y + 5.0f * dpiScale_),
                              color, "ImageGen 1:1 crop");
            }
            ImGui::EndTabItem();
        }

        for (auto& tab : projectionTabs_) {
            std::string visible = "Projection " + std::to_string(tab.id);
            if (tab.generationStartedAt && codex_.IsBusy(tab.id)) {
                visible += " (" + ElapsedLabel(*tab.generationStartedAt).substr(11) + ")";
            }
            const std::string label = visible + "###ProjectionTab" + std::to_string(tab.id);
            bool open = true;
            const ImGuiTabItemFlags flags = pendingProjectionSelection_ == tab.id
                ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
            if (ImGui::BeginTabItem(label.c_str(), &open, flags)) {
                pendingProjectionSelection_.reset();
                ActivateProjectionTab(tab);
                Vec2 available{std::max(ImGui::GetContentRegionAvail().x, 1.0f),
                               std::max(ImGui::GetContentRegionAvail().y, 1.0f)};
                Vec2 drawSize = available;
                const float frozenAspect = static_cast<float>(tab.frame.width) /
                    std::max(tab.frame.height, std::uint32_t{1});
                if (drawSize.x / drawSize.y > frozenAspect) {
                    drawSize.x = drawSize.y * frozenAspect;
                } else {
                    drawSize.y = drawSize.x / frozenAspect;
                }
                const ImVec2 regionTopLeft = ImGui::GetCursorScreenPos();
                const ImVec2 topLeft{regionTopLeft.x + (available.x - drawSize.x) * 0.5f,
                                     regionTopLeft.y + (available.y - drawSize.y) * 0.5f};
                ImGui::SetCursorScreenPos(topLeft);
                renderer_.RenderViewport(static_cast<std::uint32_t>(drawSize.x),
                                         static_cast<std::uint32_t>(drawSize.y), tab.camera);
                ImGui::Image(reinterpret_cast<ImTextureID>(renderer_.ViewportTexture()),
                             ImVec2(drawSize.x, drawSize.y));
                HandleViewportInput({topLeft.x, topLeft.y}, drawSize, &tab);
                drawLasso(topLeft);
                const SquareCropFrame crop = CenteredSquare(drawSize);
                const ImU32 color = IM_COL32(255, 196, 48, 255);
                const ImVec2 minimum{topLeft.x + crop.origin.x, topLeft.y + crop.origin.y};
                const ImVec2 maximum{minimum.x + crop.side, minimum.y + crop.side};
                ImDrawList* draw = ImGui::GetWindowDrawList();
                draw->AddRect(minimum, maximum, color, 0.0f, 0, 2.0f * dpiScale_);
                draw->AddText(ImVec2(minimum.x + 6.0f * dpiScale_, minimum.y + 5.0f * dpiScale_),
                              color, "Locked projection crop");
                const ImVec2 mouse = ImGui::GetIO().MousePos;
                const Vec2 localMouse{mouse.x - topLeft.x, mouse.y - topLeft.y};
                const bool brushAvailable = tab.projectionLoaded && !tab.applied &&
                    !codex_.IsBusy(tab.id) && !useLasso_ &&
                    tab.mask.Width() != 0 && tab.mask.Height() != 0;
                if (brushAvailable && ImGui::IsItemHovered() && crop.Contains(localMouse)) {
                    const bool erasing = ImGui::IsMouseDown(ImGuiMouseButton_Right);
                    const ImU32 brushColor = erasing
                        ? IM_COL32(255, 85, 85, 235)
                        : IM_COL32(70, 220, 255, 235);
                    draw->PushClipRect(minimum, maximum, true);
                    draw->AddCircleFilled(mouse, tab.brushRadius,
                                          erasing ? IM_COL32(255, 70, 70, 35)
                                                  : IM_COL32(70, 220, 255, 35),
                                          48);
                    draw->AddCircle(mouse, tab.brushRadius, IM_COL32(0, 0, 0, 230),
                                    48, 4.0f * dpiScale_);
                    draw->AddCircle(mouse, tab.brushRadius, brushColor,
                                    48, 2.0f * dpiScale_);
                    const float cross = 4.0f * dpiScale_;
                    draw->AddLine({mouse.x - cross, mouse.y}, {mouse.x + cross, mouse.y},
                                  brushColor, 1.5f * dpiScale_);
                    draw->AddLine({mouse.x, mouse.y - cross}, {mouse.x, mouse.y + cross},
                                  brushColor, 1.5f * dpiScale_);
                    draw->PopClipRect();
                }
                ImGui::EndTabItem();
            }
            if (!open) closeTab = tab.id;
        }
        ImGui::EndTabBar();
    }
    if (closeTab) CloseProjectionTab(*closeTab);
    ImGui::End();
}

void Application::HandleViewportInput(const Vec2& topLeft, const Vec2& size, ProjectionTab* tab) {
    const ImGuiIO& io = ImGui::GetIO();
    const bool hovered = ImGui::IsItemHovered();
    const Vec2 local{io.MousePos.x - topLeft.x, io.MousePos.y - topLeft.y};
    if (hovered && tab == nullptr && editMode_ == EditMode::Navigate) {
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Right)) {
            camera_.yaw += io.MouseDelta.x * 0.008f;
            camera_.pitch = std::clamp(camera_.pitch + io.MouseDelta.y * 0.008f, -1.5f, 1.5f);
        }
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
            const float scale = camera_.distance * 0.0015f;
            camera_.target.x -= io.MouseDelta.x * scale;
            camera_.target.y += io.MouseDelta.y * scale;
        }
        if (io.MouseWheel != 0.0f) {
            camera_.distance = std::max(0.001f, camera_.distance * std::pow(0.88f, io.MouseWheel));
        }
    }

    if (!hovered || !meshLoaded_) return;
    if (tab == nullptr && ImGui::IsKeyPressed(ImGuiKey_F)) FitCamera();
    if (tab == nullptr && editMode_ == EditMode::Face) {
        if (!useLasso_ && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            const auto triangle = renderer_.PickTriangle(
                static_cast<std::uint32_t>(std::clamp(local.x, 0.0f, size.x - 1)),
                static_cast<std::uint32_t>(std::clamp(local.y, 0.0f, size.y - 1)));
            if (triangle < selectedFaces_.size()) {
                if (!io.KeyCtrl) std::fill(selectedFaces_.begin(), selectedFaces_.end(), 0);
                selectedFaces_[triangle] = selectedFaces_[triangle] ? 0 : 1;
                renderer_.SetSelectedFaces(selectedFaces_);
            }
        } else if (useLasso_) {
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                lassoActive_ = true;
                lassoPoints_.clear();
            }
            if (lassoActive_ && ImGui::IsMouseDown(ImGuiMouseButton_Left)) lassoPoints_.push_back(local);
            if (lassoActive_ && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                const auto triangles = renderer_.PickTrianglesInLasso(lassoPoints_);
                if (!io.KeyCtrl) std::fill(selectedFaces_.begin(), selectedFaces_.end(), 0);
                for (const auto triangle : triangles) {
                    if (triangle < selectedFaces_.size()) selectedFaces_[triangle] = 1;
                }
                renderer_.SetSelectedFaces(selectedFaces_);
                lassoActive_ = false;
            }
        }
    } else if (tab != nullptr && tab->projectionLoaded && !tab->applied &&
               !codex_.IsBusy(tab->id) && tab->mask.Width() != 0 && tab->mask.Height() != 0) {
        const SquareCropFrame crop = CenteredSquare(size);
        const Vec2 cropLocal{local.x - crop.origin.x, local.y - crop.origin.y};
        const bool insideCrop = crop.Contains(local);
        const float maskScale = static_cast<float>(tab->mask.Width()) / crop.side;
        if (!useLasso_) {
            const bool painting = ImGui::IsMouseDown(ImGuiMouseButton_Left) ||
                                  ImGui::IsMouseDown(ImGuiMouseButton_Right);
            if (painting && insideCrop) {
                const bool include = ImGui::IsMouseDown(ImGuiMouseButton_Left);
                tab->mask.PaintCircle(cropLocal.x * maskScale, cropLocal.y * maskScale,
                                      tab->brushRadius * maskScale, include);
                ApplyMaskChange(*tab);
            }
        } else {
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && insideCrop) {
                lassoActive_ = true;
                lassoPoints_.clear();
            }
            if (lassoActive_ && ImGui::IsMouseDown(ImGuiMouseButton_Left)) lassoPoints_.push_back(local);
            if (lassoActive_ && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                std::vector<Vec2> maskPoints;
                maskPoints.reserve(lassoPoints_.size());
                for (const Vec2 point : lassoPoints_) {
                    maskPoints.push_back({(point.x - crop.origin.x) * maskScale,
                                          (point.y - crop.origin.y) * maskScale});
                }
                tab->mask.ApplyLasso(maskPoints, maskInclude_);
                ApplyMaskChange(*tab);
                lassoActive_ = false;
            }
        }
    }
}

void Application::DrawTools() {
    ImGui::Begin("Projection Tools");
    ProjectionTab* tab = ActiveProjectionTab();
    if (tab == nullptr) {
        if (ImGui::Button("Open OBJ")) OpenObj();
        ImGui::SameLine();
        if (ImGui::Button("Open Texture PNG")) OpenTexture();
        if (meshLoaded_) {
            ImGui::Text("OBJ: %s", Narrow(mesh_.SourcePath().filename()).c_str());
            ImGui::Text("Triangles: %zu", mesh_.TriangleCount());
        }
        if (textureLoaded_) {
            ImGui::Text("Texture: %s (%ux%u)", Narrow(texturePath_.filename()).c_str(),
                        sourceTexture_.Width(), sourceTexture_.Height());
        }
    } else {
        ImGui::SeparatorText("Locked projection source");
        ImGui::BeginChild("LockedSourceInfo", ImVec2(0, 164.0f * dpiScale_), true);
        ImGui::TextColored(ImVec4(0.45f, 0.85f, 1, 1),
                           "Read-only snapshot for Projection %llu",
                           static_cast<unsigned long long>(tab->id));
        ImGui::Text("OBJ: %s", Narrow(mesh_.SourcePath().filename()).c_str());
        ImGui::Text("Triangles: %zu", mesh_.TriangleCount());
        ImGui::Text("Texture: %s (%ux%u)", Narrow(texturePath_.filename()).c_str(),
                    sourceTexture_.Width(), sourceTexture_.Height());
        ImGui::Text("Captured viewport: %u x %u; ImageGen crop: %u x %u",
                    tab->frame.width, tab->frame.height,
                    tab->frame.cropSize, tab->frame.cropSize);
        ImGui::Text("Codex: %s / %s", tab->model.c_str(), tab->reasoningEffort.c_str());
        const auto hiddenCount = std::count(tab->hiddenFaces.begin(), tab->hiddenFaces.end(),
                                            std::uint8_t{1});
        ImGui::Text("Frozen hidden faces: %zu", hiddenCount);
        ImGui::TextDisabled("OBJ and Base Color loading is available only in Main Viewport.");
        ImGui::EndChild();
    }
    if (meshLoaded_ && mesh_.UvOverlapCount() > 0) {
        ImGui::TextColored(ImVec4(1, 0.65f, 0.2f, 1), "Warning: %zu overlapping UV pair(s)",
                           mesh_.UvOverlapCount());
        ImGui::TextWrapped("Shared or mirrored UVs may let the opposite local-X side overwrite the bake.");
    }

    ImGui::SeparatorText("ImageGen reference sets");
    if (ImGui::Button("Add reference OBJ + PNG")) AddReferenceAsset();
    ImGui::SameLine();
    ImGui::BeginDisabled(referenceAssets_.empty());
    bool& showReferences = tab == nullptr ? referenceAssetsVisible_ : tab->referenceAssetsVisible;
    if (ImGui::Checkbox(tab == nullptr ? "Show in viewport" : "Show in this projection tab",
                        &showReferences)) {
        renderer_.SetReferenceAssetsVisible(showReferences);
    }
    ImGui::EndDisabled();
    ImGui::TextWrapped("Reference sets are viewport/ImageGen context only. Toggle them off manually while projection painting if desired.");
    std::optional<std::size_t> removeReference;
    for (std::size_t i = 0; i < referenceAssets_.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        ImGui::Text("%s + %s", Narrow(referenceAssets_[i].objPath.filename()).c_str(),
                    Narrow(referenceAssets_[i].texturePath.filename()).c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Remove")) removeReference = i;
        ImGui::PopID();
    }
    if (removeReference) RemoveReferenceAsset(*removeReference);
    if (!referenceAssets_.empty()) {
        if (ImGui::Button("Clear all references")) {
            referenceAssets_.clear();
            renderer_.ClearReferenceAssets();
            SetStatus("All inference reference sets were removed.");
        }
    }
    renderer_.SetReferenceAssetsVisible(
        tab == nullptr ? referenceAssetsVisible_ : tab->referenceAssetsVisible);

    ImGui::SeparatorText("Viewport display");
    if (ImGui::Checkbox("Neutral shading", &shadingEnabled_)) {
        renderer_.SetShadingEnabled(shadingEnabled_);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!meshLoaded_ || activeProjectionId_.has_value());
    if (ImGui::Button("Fit primary view (F)")) FitCamera();
    ImGui::EndDisabled();
    ImGui::TextDisabled("Shading is off by default; Base Color is shown unchanged.");

    if (tab == nullptr) {
        ImGui::SeparatorText("Main viewport mode");
        if (ImGui::RadioButton("Navigate", editMode_ == EditMode::Navigate)) editMode_ = EditMode::Navigate;
        ImGui::SameLine();
        if (ImGui::RadioButton("Faces", editMode_ == EditMode::Face)) editMode_ = EditMode::Face;
        if (editMode_ == EditMode::Face) ImGui::Checkbox("Lasso", &useLasso_);
    }

    if (tab == nullptr && editMode_ == EditMode::Face) {
        const auto selectedCount = std::count(selectedFaces_.begin(), selectedFaces_.end(), std::uint8_t{1});
        ImGui::Text("Selected faces: %zu", selectedCount);
        if (ImGui::Button("Hide selected") && selectedCount > 0) HideSelectedFaces();
        ImGui::SameLine();
        if (ImGui::Button("Undo hide") && !hiddenHistory_.empty()) UndoHiddenFaces();
        ImGui::SameLine();
        if (ImGui::Button("Show all")) ShowAllFaces();
    }

    if (tab == nullptr) {
        ImGui::SeparatorText("Create projection tab");
        ImGui::TextWrapped("Generate captures the cyan square immediately, then opens an independent locked painting tab. The main viewport remains usable.");
        ImGui::InputTextMultiline("ImageGen prompt", generationPrompt_.data(), generationPrompt_.size(),
                                  ImVec2(-1, 90.0f * dpiScale_));
        const auto& models = codex_.Models();
        const CodexModelInfo* selectedModel = FindModel(models, codexSettings_.model);
        const std::string modelPreview = selectedModel
            ? selectedModel->displayName + " (" + selectedModel->id + ")"
            : codexSettings_.model;
        if (ImGui::BeginCombo("Codex model", modelPreview.c_str())) {
            for (const auto& model : models) {
                const bool selected = model.id == codexSettings_.model;
                const std::string label = model.displayName + " (" + model.id + ")";
                if (ImGui::Selectable(label.c_str(), selected)) {
                    codexSettings_.model = model.id;
                    if (!SupportsEffort(model, codexSettings_.reasoningEffort)) {
                        codexSettings_.reasoningEffort = SupportsEffort(model, "medium")
                            ? "medium" : model.defaultReasoningEffort;
                    }
                    settingsChanged_ = true;
                    settingsMessage_.clear();
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        selectedModel = FindModel(models, codexSettings_.model);
        if (selectedModel && ImGui::BeginCombo("Reasoning effort",
                                               codexSettings_.reasoningEffort.c_str())) {
            for (const auto& option : selectedModel->supportedReasoningEfforts) {
                const bool selected = option.value == codexSettings_.reasoningEffort;
                if (ImGui::Selectable(option.value.c_str(), selected)) {
                    codexSettings_.reasoningEffort = option.value;
                    settingsChanged_ = true;
                    settingsMessage_.clear();
                }
                if (ImGui::IsItemHovered() && !option.description.empty()) {
                    ImGui::SetTooltip("%s", option.description.c_str());
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        if (settingsChanged_ || !settingsLoadedFromDisk_) {
            ImGui::TextDisabled("Pending: saved beside the executable immediately before generation.");
        } else {
            ImGui::TextDisabled("Loaded from CodexTex.settings.json.");
        }
        if (!settingsMessage_.empty()) {
            ImGui::TextColored(ImVec4(1, 0.35f, 0.3f, 1), "%s", settingsMessage_.c_str());
        }
        const bool canGenerate = meshLoaded_ && textureLoaded_ && codex_.IsAvailable() &&
                                 generationPrompt_[0] != '\0';
        ImGui::BeginDisabled(!canGenerate);
        if (ImGui::Button("Generate from current view")) CreateProjectionTab(true);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!meshLoaded_ || !textureLoaded_);
        if (ImGui::Button("External PNG from current view")) CreateProjectionTab(false);
        ImGui::EndDisabled();
    } else {
        ImGui::SeparatorText("Projection workspace");
        ImGui::TextColored(ImVec4(0.45f, 0.85f, 1, 1),
                           "Camera and visibility are locked for this tab");
        if (codex_.IsBusy(tab->id)) {
            const std::string elapsed = tab->generationStartedAt
                ? ElapsedLabel(*tab->generationStartedAt) : "AI working";
            ImGui::BeginDisabled();
            ImGui::Button(elapsed.c_str());
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Cancel AI")) codex_.Cancel(tab->id);
        } else {
            ImGui::BeginDisabled(tab->applied);
            if (ImGui::Button("Open/replace external PNG")) OpenProjection(*tab);
            ImGui::EndDisabled();
        }
        if (tab->projectionLoaded) {
            ImGui::Text("Projection: %s", Narrow(tab->projectionPath.filename()).c_str());
            ImGui::BeginDisabled(!codex_.IsAvailable() || codex_.IsBusy(tab->id) || tab->applied);
            if (ImGui::Button("Suggest mask with Codex")) {
                codex_.BeginMaskProposal(tab->id, tab->capturePath, tab->projectionPath,
                                         tab->model, tab->reasoningEffort);
            }
            ImGui::EndDisabled();
        }

        ImGui::SeparatorText("Mask and bake");
        ImGui::BeginDisabled(!tab->projectionLoaded || codex_.IsBusy(tab->id) || tab->applied);
        ImGui::Checkbox("Lasso", &useLasso_);
        if (!useLasso_) ImGui::SliderFloat("Brush radius", &tab->brushRadius, 2.0f, 160.0f, "%.0f px");
        if (useLasso_) ImGui::Checkbox("Lasso includes area", &maskInclude_);
        if (ImGui::SliderInt("Inward feather", &tab->featherRadius, 0, 128, "%d px")) {
            ApplyMaskChange(*tab);
        }
        if (ImGui::Button("Clear mask")) {
            tab->mask.Clear(false);
            ApplyMaskChange(*tab);
        }
        ImGui::SameLine();
        if (ImGui::Button("Select all visible")) {
            tab->mask.Clear(true);
            ApplyMaskChange(*tab);
        }
        ImGui::SliderFloat("Max surface angle", &tab->maxAngleDegrees, 0.0f, 89.0f, "%.0f deg");
        constexpr const char* sideFilterLabels[]{
            "Paint both local-X sides", "Ignore local -X side", "Ignore local +X side"};
        int sideFilter = static_cast<int>(tab->localSideFilter);
        if (ImGui::Combo("Mirrored UV side", &sideFilter, sideFilterLabels,
                         static_cast<int>(std::size(sideFilterLabels)))) {
            tab->localSideFilter = static_cast<LocalSideFilter>(sideFilter);
            renderer_.SetLocalSideFilter(tab->localSideFilter);
        }
        ImGui::EndDisabled();
        if (tab->baseTextureRevision != textureRevision_) {
            ImGui::TextColored(ImVec4(1, 0.75f, 0.25f, 1),
                               "Shared texture changed since this tab was created; bake uses the latest texture.");
        }
        ImGui::BeginDisabled(!tab->projectionLoaded || codex_.IsBusy(tab->id) || tab->applied);
        if (ImGui::Button(tab->applied ? "Already baked" : "Bake into shared texture")) Bake(*tab);
        ImGui::EndDisabled();
        ImGui::SameLine();
        const bool closeRequested = ImGui::Button("Close tab");
        const ImVec4 tabStatusColor = tab->statusIsError
            ? ImVec4(1, 0.35f, 0.3f, 1) : ImVec4(0.7f, 0.85f, 0.75f, 1);
        ImGui::TextColored(tabStatusColor, "%s", tab->status.c_str());
        if (closeRequested) CloseProjectionTab(tab->id);
    }

    ImGui::TextWrapped("%s", codex_.AvailabilityMessage().c_str());
    if (!imageGenLogPath_.empty()) {
        ImGui::TextWrapped("ImageGen log: %s", Narrow(imageGenLogPath_).c_str());
    }
    if (!codex_.IsAvailable() && !codex_.IsBusy()) {
        if (ImGui::Button("Retry Codex detection")) {
            const bool started = codex_.Start(sessionDirectory_);
            NormalizeCodexSettings();
            SetStatus(codex_.AvailabilityMessage(), !started || !codex_.IsAvailable());
        }
    }

    ImGui::BeginDisabled(undoTextures_.empty());
    if (ImGui::Button("Undo")) UndoTexture();
    ImGui::EndDisabled();

    ImGui::Separator();
    const ImVec4 statusColor = statusIsError_ ? ImVec4(1, 0.35f, 0.3f, 1) : ImVec4(0.7f, 0.85f, 0.75f, 1);
    ImGui::TextColored(statusColor, "%s", status_.c_str());
    ImGui::End();
}

void Application::DrawTexturePreview() {
    ImGui::Begin("Texture Preview");
    if (textureLoaded_ && renderer_.WorkingTexture()) {
        const ImVec2 available = ImGui::GetContentRegionAvail();
        const float aspect = static_cast<float>(sourceTexture_.Width()) / sourceTexture_.Height();
        ImVec2 size{available.x, available.x / aspect};
        if (size.y > available.y) size = {available.y * aspect, available.y};
        ImGui::Image(reinterpret_cast<ImTextureID>(renderer_.WorkingTexture()), size);
    } else {
        ImGui::TextUnformatted("No PNG texture loaded.");
    }
    ImGui::End();
}

void Application::DrawSessionTemp() {
    ImGui::Begin("Session Temp");
    if (tempFilesDirty_) RefreshTempFiles();
    ImGui::TextWrapped("Session folder: %s", Narrow(sessionDirectory_).c_str());
    if (ImGui::Button("Refresh")) RefreshTempFiles();
    ImGui::SameLine();
    ImGui::BeginDisabled(selectedTempFile_.empty());
    if (ImGui::Button("Delete selected")) DeleteTempFile(selectedTempFile_);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(tempFiles_.empty());
    if (ImGui::Button("Delete all temp files")) DeleteAllTempFiles();
    ImGui::EndDisabled();

    const float listWidth = std::max(ImGui::GetContentRegionAvail().x * 0.42f,
                                     220.0f * dpiScale_);
    ImGui::BeginChild("TempFileList", ImVec2(listWidth, 0), true);
    if (tempFiles_.empty()) ImGui::TextDisabled("No session temporary files.");
    for (const auto& file : tempFiles_) {
        const auto relative = file.path.lexically_relative(sessionDirectory_);
        const std::string label = Narrow(relative) + "  (" + FileSizeLabel(file.size) + ")";
        if (ImGui::Selectable(label.c_str(), selectedTempFile_ == file.path)) {
            SelectTempFile(file.path);
        }
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginGroup();
    if (selectedTempFile_.empty()) {
        ImGui::TextDisabled("Select a file to inspect its contents.");
    } else {
        ImGui::TextWrapped("%s", Narrow(selectedTempFile_.filename()).c_str());
        if (!tempPreviewMessage_.empty()) {
            ImGui::TextWrapped("%s", tempPreviewMessage_.c_str());
        }
        if (!tempPreviewImage_.Empty() && renderer_.SessionPreviewTexture()) {
            const ImVec2 available = ImGui::GetContentRegionAvail();
            const float aspect = static_cast<float>(tempPreviewImage_.Width()) /
                tempPreviewImage_.Height();
            ImVec2 size{available.x, available.x / aspect};
            if (size.y > available.y) size = {available.y * aspect, available.y};
            ImGui::Image(reinterpret_cast<ImTextureID>(renderer_.SessionPreviewTexture()), size);
        } else if (!tempPreviewText_.empty()) {
            ImGui::BeginChild("TempTextContents", ImVec2(0, 0), true,
                              ImGuiWindowFlags_HorizontalScrollbar);
            ImGui::TextUnformatted(tempPreviewText_.data(),
                                   tempPreviewText_.data() + tempPreviewText_.size());
            ImGui::EndChild();
        }
    }
    ImGui::EndGroup();
    ImGui::End();
}

void Application::RefreshTempFiles() {
    tempFiles_.clear();
    std::error_code error;
    for (std::filesystem::recursive_directory_iterator iterator(
             sessionDirectory_, std::filesystem::directory_options::skip_permission_denied,
             error), end;
         !error && iterator != end; iterator.increment(error)) {
        if (!iterator->is_regular_file(error)) {
            error.clear();
            continue;
        }
        const auto size = iterator->file_size(error);
        if (error) {
            error.clear();
            continue;
        }
        tempFiles_.push_back({iterator->path(), size});
    }
    std::ranges::sort(tempFiles_, {}, [](const TempFileInfo& file) {
        return file.path.generic_wstring();
    });
    if (!selectedTempFile_.empty() &&
        std::ranges::none_of(tempFiles_, [this](const TempFileInfo& file) {
            return file.path == selectedTempFile_;
        })) {
        selectedTempFile_.clear();
        tempPreviewImage_ = {};
        tempPreviewText_.clear();
        tempPreviewMessage_.clear();
        renderer_.ClearSessionPreviewImage();
    }
    tempFilesDirty_ = false;
}

void Application::SelectTempFile(const std::filesystem::path& path) {
    if (!IsInsideDirectory(sessionDirectory_, path)) {
        SetStatus("Refused to inspect a path outside the managed session folder.", true);
        return;
    }
    selectedTempFile_ = path;
    tempPreviewImage_ = {};
    tempPreviewText_.clear();
    tempPreviewMessage_.clear();
    renderer_.ClearSessionPreviewImage();

    std::wstring extension = path.extension().wstring();
    std::ranges::transform(extension, extension.begin(),
                           [](const wchar_t value) { return std::towlower(value); });
    if (extension == L".png") {
        std::string error;
        if (!tempPreviewImage_.LoadPng(path, error) ||
            !renderer_.SetSessionPreviewImage(tempPreviewImage_, error)) {
            tempPreviewImage_ = {};
            tempPreviewMessage_ = error;
            return;
        }
        tempPreviewMessage_ = std::to_string(tempPreviewImage_.Width()) + " x " +
            std::to_string(tempPreviewImage_.Height()) + " RGBA PNG";
        return;
    }

    constexpr std::size_t previewLimit = 64 * 1024;
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        tempPreviewMessage_ = "Could not read this file.";
        return;
    }
    tempPreviewText_.resize(previewLimit);
    stream.read(tempPreviewText_.data(), static_cast<std::streamsize>(tempPreviewText_.size()));
    const auto bytesRead = static_cast<std::size_t>(stream.gcount());
    tempPreviewText_.resize(bytesRead);
    for (char& value : tempPreviewText_) {
        const auto byte = static_cast<unsigned char>(value);
        if (byte < 0x20 && value != '\r' && value != '\n' && value != '\t') value = '.';
    }
    std::error_code sizeError;
    const auto fileSize = std::filesystem::file_size(path, sizeError);
    tempPreviewMessage_ = !sizeError && fileSize > previewLimit
        ? "Showing the first 64 KiB." : "Text/binary preview.";
}

void Application::DeleteTempFile(const std::filesystem::path& path) {
    if (!IsInsideDirectory(sessionDirectory_, path)) {
        SetStatus("Refused to delete a path outside the managed session folder.", true);
        return;
    }
    std::vector<std::uint64_t> affectedTabs;
    for (const auto& tab : projectionTabs_) {
        if (tab.capturePath == path || tab.projectionPath == path) affectedTabs.push_back(tab.id);
    }
    std::wstring prompt = L"Delete this temporary file?\n\n" + path.filename().wstring();
    if (!affectedTabs.empty()) {
        prompt += L"\n\nIt is used by a projection workspace. That tab will be closed and its AI task cancelled.";
    }
    if (MessageBoxW(window_, prompt.c_str(), L"Delete temporary file",
                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return;
    for (const auto id : affectedTabs) CloseProjectionTab(id);
    std::error_code error;
    const bool removed = std::filesystem::remove(path, error);
    if (error || !removed) {
        SetStatus("Could not delete the selected temporary file.", true);
        return;
    }
    selectedTempFile_.clear();
    tempPreviewImage_ = {};
    tempPreviewText_.clear();
    tempPreviewMessage_.clear();
    renderer_.ClearSessionPreviewImage();
    tempFilesDirty_ = true;
    SetStatus("Selected session temporary file deleted.");
}

void Application::DeleteAllTempFiles() {
    std::error_code error;
    const auto tempRoot = std::filesystem::temp_directory_path(error);
    if (error || !IsInsideDirectory(tempRoot, sessionDirectory_) ||
        !sessionDirectory_.filename().wstring().starts_with(L"CodexTex-")) {
        SetStatus("The managed session folder failed its safety check.", true);
        return;
    }
    std::wstring prompt = L"Delete every file in this session temp folder?";
    if (!projectionTabs_.empty()) {
        prompt += L"\n\nAll projection workspace tabs will be closed and active AI tasks cancelled.";
    }
    if (MessageBoxW(window_, prompt.c_str(), L"Delete all session temporary files",
                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return;
    ClearProjectionTabs();
    selectedTempFile_.clear();
    tempPreviewImage_ = {};
    tempPreviewText_.clear();
    tempPreviewMessage_.clear();
    renderer_.ClearSessionPreviewImage();

    std::uintmax_t removedCount = 0;
    for (std::filesystem::directory_iterator iterator(
             sessionDirectory_, std::filesystem::directory_options::skip_permission_denied,
             error), end;
         !error && iterator != end; iterator.increment(error)) {
        removedCount += std::filesystem::remove_all(iterator->path(), error);
        if (error) break;
    }
    tempFilesDirty_ = true;
    RefreshTempFiles();
    if (error) {
        SetStatus("Some session temporary files could not be deleted.", true);
    } else {
        SetStatus("Deleted " + std::to_string(removedCount) +
                  " session temporary file(s). The session folder remains active.");
    }
}

bool Application::OpenObj() {
    const auto path = OpenFileDialog(L"Open UV-mapped OBJ", L"Wavefront OBJ (*.obj)\0*.obj\0\0");
    if (path.empty()) return false;
    Mesh mesh;
    std::string error;
    if (!mesh.LoadObj(path, error)) {
        SetStatus(error, true);
        return false;
    }
    ClearProjectionTabs();
    mesh_ = std::move(mesh);
    meshLoaded_ = true;
    renderer_.SetLocalSideFilter(LocalSideFilter::Both);
    hiddenFaces_.assign(mesh_.TriangleCount(), 0);
    selectedFaces_.assign(mesh_.TriangleCount(), 0);
    hiddenHistory_.clear();
    if (!renderer_.SetMesh(mesh_, error)) {
        meshLoaded_ = false;
        SetStatus(error, true);
        return false;
    }
    camera_.yaw = 0.0f;
    camera_.pitch = 0.15f;
    camera_.fovDegrees = 45.0f;
    FitCamera();
    SetStatus(error.empty() ? "OBJ loaded." : error);
    return true;
}

bool Application::OpenTexture() {
    if (dirty_ && !CanClose()) return false;
    const auto path = OpenFileDialog(L"Open Base Color PNG", L"PNG image (*.png)\0*.png\0\0");
    if (path.empty()) return false;
    TextureImage image;
    std::string error;
    if (!image.LoadPng(path, error) || !renderer_.SetWorkingTexture(image, error)) {
        SetStatus(error, true);
        return false;
    }
    ClearProjectionTabs();
    sourceTexture_ = std::move(image);
    texturePath_ = path;
    textureLoaded_ = true;
    dirty_ = false;
    undoTextures_.clear();
    redoTextures_.clear();
    ++textureRevision_;
    SetStatus("Texture PNG loaded.");
    return true;
}

bool Application::OpenProjection(ProjectionTab& tab) {
    const auto path = OpenFileDialog(L"Open projection PNG", L"PNG image (*.png)\0*.png\0\0");
    if (path.empty()) return false;
    TextureImage image;
    std::string error;
    if (!image.LoadPng(path, error)) {
        SetStatus(error, true);
        return false;
    }
    TextureImage capture;
    if (!capture.LoadPng(tab.capturePath, error) || !SameAspect(capture, image) ||
        image.Width() != image.Height()) {
        SetStatus("Projection PNG must be square to match the ImageGen crop.", true);
        return false;
    }
    if (activeProjectionId_ == tab.id && !renderer_.SetProjectionImage(image, error)) {
        tab.status = error;
        tab.statusIsError = true;
        return false;
    }
    tab.projectionImage = std::move(image);
    tab.projectionPath = path;
    tab.projectionLoaded = true;
    tab.status = "External projection PNG loaded.";
    tab.statusIsError = false;
    if (activeProjectionId_ == tab.id) renderer_.SetProjectionPreview(true);
    return true;
}

bool Application::AddReferenceAsset() {
    const auto objPath = OpenFileDialog(L"Open inference reference OBJ",
                                        L"Wavefront OBJ (*.obj)\0*.obj\0\0");
    if (objPath.empty()) return false;
    const auto texturePath = OpenFileDialog(L"Open texture for the reference OBJ",
                                            L"PNG image (*.png)\0*.png\0\0");
    if (texturePath.empty()) return false;

    Mesh mesh;
    TextureImage texture;
    std::string error;
    if (!mesh.LoadObj(objPath, error)) {
        SetStatus("Reference OBJ: " + error, true);
        return false;
    }
    if (!texture.LoadPng(texturePath, error)) {
        SetStatus("Reference texture: " + error, true);
        return false;
    }
    if (!renderer_.AddReferenceAsset(mesh, texture, error)) {
        SetStatus(error, true);
        return false;
    }
    referenceAssets_.push_back({std::move(mesh), std::move(texture), objPath, texturePath});
    renderer_.SetReferenceAssetsVisible(referenceAssetsVisible_);
    SetStatus("Inference reference OBJ + PNG added. It will never be baked or saved.");
    return true;
}

void Application::RemoveReferenceAsset(const std::size_t index) {
    if (index >= referenceAssets_.size()) return;
    referenceAssets_.erase(referenceAssets_.begin() + static_cast<std::ptrdiff_t>(index));
    if (RebuildReferenceAssets()) SetStatus("Inference reference set removed.");
}

bool Application::RebuildReferenceAssets() {
    renderer_.ClearReferenceAssets();
    std::string error;
    for (const auto& asset : referenceAssets_) {
        if (!renderer_.AddReferenceAsset(asset.mesh, asset.texture, error)) {
            SetStatus("Could not rebuild reference viewport assets: " + error, true);
            return false;
        }
    }
    renderer_.SetReferenceAssetsVisible(referenceAssetsVisible_);
    return true;
}

bool Application::SaveTexture(const bool choosePath) {
    if (!textureLoaded_) return false;
    std::filesystem::path path = texturePath_;
    if (choosePath || path.empty()) {
        path = SaveFileDialog(L"Save Base Color PNG", L"PNG image (*.png)\0*.png\0\0", path);
        if (path.empty()) return false;
    }
    TextureImage current;
    std::string error;
    if (!renderer_.ReadWorkingTexture(current, error) || !current.SavePng(path, error)) {
        SetStatus(error, true);
        return false;
    }
    texturePath_ = path;
    sourceTexture_ = std::move(current);
    dirty_ = false;
    SetStatus("Texture PNG saved. No OBJ or project file was written.");
    return true;
}

bool Application::CreateProjectionTab(const bool generate) {
    if (!meshLoaded_ || !textureLoaded_) return false;
    std::fill(selectedFaces_.begin(), selectedFaces_.end(), 0);
    renderer_.SetSelectedFaces(selectedFaces_);
    renderer_.SetHiddenFaces(hiddenFaces_);
    renderer_.SetReferenceAssetsVisible(referenceAssetsVisible_);
    renderer_.SetProjectionPreview(false);
    renderer_.RenderViewport(renderer_.ViewportWidth(), renderer_.ViewportHeight(), camera_);
    ProjectionTab tab;
    tab.id = nextProjectionId_++;
    tab.camera = camera_;
    tab.hiddenFaces = hiddenFaces_;
    tab.referenceAssetsVisible = referenceAssetsVisible_;
    tab.baseTextureRevision = textureRevision_;
    tab.model = codexSettings_.model;
    tab.reasoningEffort = codexSettings_.reasoningEffort;
    tab.capturePath = sessionDirectory_ /
        (L"capture-" + std::to_wstring(tab.id) + L".png");
    TextureImage capture;
    std::string error;
    if (!renderer_.CaptureFrame(camera_, capture, tab.frame, error) ||
        !capture.SavePng(tab.capturePath, error)) {
        SetStatus(error, true);
        ActivateMainViewport();
        return false;
    }
    tempFilesDirty_ = true;
    tab.mask.Resize(capture.Width(), capture.Height(), false);
    projectionTabs_.push_back(std::move(tab));
    ProjectionTab& created = projectionTabs_.back();
    pendingProjectionSelection_ = created.id;
    ActivateProjectionTab(created);
    if (generate) {
        created.generationStartedAt = std::chrono::steady_clock::now();
        created.status = "ImageGen request is starting.";
        if (!SaveCodexSettingsForRequest() ||
            !codex_.BeginGeneration(created.id, created.capturePath,
                                    generationPrompt_.data(), created.model,
                                    created.reasoningEffort)) {
            created.generationStartedAt.reset();
            created.status = "Could not start ImageGen; an external PNG can still be loaded.";
            created.statusIsError = true;
        }
    } else if (!OpenProjection(created)) {
        CloseProjectionTab(created.id);
        return false;
    }
    SetStatus("Projection workspace created; the main viewport remains available.");
    return true;
}

bool Application::SaveCodexSettingsForRequest() {
    std::string error;
    if (!SaveCodexRequestSettings(settingsPath_, codexSettings_, error)) {
        settingsMessage_ = error;
        SetStatus(error, true);
        return false;
    }
    settingsLoadedFromDisk_ = true;
    settingsChanged_ = false;
    settingsMessage_.clear();
    return true;
}

void Application::NormalizeCodexSettings() {
    const auto& models = codex_.Models();
    if (models.empty()) return;
    const CodexModelInfo* model = FindModel(models, codexSettings_.model);
    if (!model) {
        const CodexModelInfo* defaultModel = FindModel(models, "gpt-5.6-sol");
        model = defaultModel ? defaultModel : &models.front();
        codexSettings_.model = model->id;
        settingsChanged_ = settingsLoadedFromDisk_;
    }
    if (!SupportsEffort(*model, codexSettings_.reasoningEffort)) {
        codexSettings_.reasoningEffort = SupportsEffort(*model, "medium")
            ? "medium" : model->defaultReasoningEffort;
        settingsChanged_ = settingsLoadedFromDisk_;
    }
}

Application::ProjectionTab* Application::FindProjectionTab(const std::uint64_t id) {
    const auto found = std::ranges::find(projectionTabs_, id, &ProjectionTab::id);
    return found == projectionTabs_.end() ? nullptr : &*found;
}

Application::ProjectionTab* Application::ActiveProjectionTab() {
    return activeProjectionId_ ? FindProjectionTab(*activeProjectionId_) : nullptr;
}

void Application::ActivateMainViewport() {
    if (!activeProjectionId_ && !rendererProjectionId_) return;
    activeProjectionId_.reset();
    rendererProjectionId_.reset();
    renderer_.ClearFrozenFrame();
    renderer_.SetHiddenFaces(hiddenFaces_);
    renderer_.SetSelectedFaces(selectedFaces_);
    renderer_.SetReferenceAssetsVisible(referenceAssetsVisible_);
    renderer_.SetLocalSideFilter(LocalSideFilter::Both);
    renderer_.SetProjectionPreview(false);
    lassoActive_ = false;
    lassoPoints_.clear();
}

void Application::ActivateProjectionTab(ProjectionTab& tab) {
    activeProjectionId_ = tab.id;
    if (rendererProjectionId_ == tab.id) return;
    rendererProjectionId_ = tab.id;
    renderer_.ActivateProjectionFrame(tab.frame);
    renderer_.SetHiddenFaces(tab.hiddenFaces);
    std::vector<std::uint8_t> none(selectedFaces_.size(), 0);
    renderer_.SetSelectedFaces(none);
    renderer_.SetReferenceAssetsVisible(tab.referenceAssetsVisible);
    renderer_.SetLocalSideFilter(tab.localSideFilter);
    renderer_.SetMask(tab.mask, tab.featherRadius);
    std::string error;
    if (tab.projectionLoaded && renderer_.SetProjectionImage(tab.projectionImage, error)) {
        renderer_.SetProjectionPreview(!tab.applied);
    } else {
        renderer_.SetProjectionPreview(false);
    }
    lassoActive_ = false;
    lassoPoints_.clear();
}

void Application::CloseProjectionTab(const std::uint64_t id) {
    codex_.Cancel(id);
    codex_.Forget(id);
    const auto found = std::ranges::find(projectionTabs_, id, &ProjectionTab::id);
    if (found == projectionTabs_.end()) return;
    const bool wasActive = activeProjectionId_ == id;
    projectionTabs_.erase(found);
    if (wasActive) {
        activeProjectionId_.reset();
        rendererProjectionId_.reset();
        renderer_.ClearFrozenFrame();
        renderer_.SetHiddenFaces(hiddenFaces_);
        renderer_.SetSelectedFaces(selectedFaces_);
        renderer_.SetReferenceAssetsVisible(referenceAssetsVisible_);
        renderer_.SetProjectionPreview(false);
    }
}

void Application::ClearProjectionTabs() {
    for (const auto& tab : projectionTabs_) {
        codex_.Cancel(tab.id);
        codex_.Forget(tab.id);
    }
    projectionTabs_.clear();
    activeProjectionId_.reset();
    pendingProjectionSelection_.reset();
    rendererProjectionId_.reset();
    renderer_.ClearFrozenFrame();
    renderer_.SetHiddenFaces(hiddenFaces_);
    renderer_.SetSelectedFaces(selectedFaces_);
    renderer_.SetReferenceAssetsVisible(referenceAssetsVisible_);
    renderer_.SetLocalSideFilter(LocalSideFilter::Both);
    renderer_.SetProjectionPreview(false);
}

void Application::FitCamera() {
    const Vec3& low = mesh_.BoundsMin();
    const Vec3& high = mesh_.BoundsMax();
    camera_.target = {(low.x + high.x) * 0.5f, (low.y + high.y) * 0.5f, (low.z + high.z) * 0.5f};
    const float cp = std::cos(camera_.pitch);
    const Vec3 eyeDirection = Normalize({std::sin(camera_.yaw) * cp,
                                         std::sin(camera_.pitch),
                                         std::cos(camera_.yaw) * cp});
    const Vec3 forward{-eyeDirection.x, -eyeDirection.y, -eyeDirection.z};
    const Vec3 right = Normalize(Cross({0, 1, 0}, forward));
    const Vec3 up = Normalize(Cross(forward, right));
    const float aspect = renderer_.ViewportHeight() > 0
        ? static_cast<float>(renderer_.ViewportWidth()) / renderer_.ViewportHeight()
        : 16.0f / 9.0f;
    const float tanVertical = std::tan(camera_.fovDegrees * std::numbers::pi_v<float> / 360.0f);
    const float tanHorizontal = tanVertical * std::max(aspect, 0.1f);
    float requiredDistance = 0.0f;
    for (int corner = 0; corner < 8; ++corner) {
        const Vec3 point{(corner & 1) ? high.x : low.x,
                         (corner & 2) ? high.y : low.y,
                         (corner & 4) ? high.z : low.z};
        const Vec3 delta{point.x - camera_.target.x,
                         point.y - camera_.target.y,
                         point.z - camera_.target.z};
        const float towardEye = Dot(delta, eyeDirection);
        requiredDistance = std::max(requiredDistance,
            towardEye + std::abs(Dot(delta, right)) / std::max(tanHorizontal, 0.01f));
        requiredDistance = std::max(requiredDistance,
            towardEye + std::abs(Dot(delta, up)) / std::max(tanVertical, 0.01f));
    }
    const float dx = high.x - low.x;
    const float dy = high.y - low.y;
    const float dz = high.z - low.z;
    const float diagonal = std::sqrt(dx * dx + dy * dy + dz * dz);
    camera_.distance = std::max(requiredDistance * 1.12f, std::max(diagonal * 0.01f, 0.01f));
}

void Application::HideSelectedFaces() {
    hiddenHistory_.push_back(hiddenFaces_);
    for (std::size_t i = 0; i < hiddenFaces_.size(); ++i) {
        if (selectedFaces_[i]) hiddenFaces_[i] = 1;
    }
    std::fill(selectedFaces_.begin(), selectedFaces_.end(), 0);
    renderer_.SetHiddenFaces(hiddenFaces_);
    renderer_.SetSelectedFaces(selectedFaces_);
}

void Application::UndoHiddenFaces() {
    if (hiddenHistory_.empty()) return;
    hiddenFaces_ = std::move(hiddenHistory_.back());
    hiddenHistory_.pop_back();
    renderer_.SetHiddenFaces(hiddenFaces_);
}

void Application::ShowAllFaces() {
    hiddenHistory_.push_back(hiddenFaces_);
    std::fill(hiddenFaces_.begin(), hiddenFaces_.end(), 0);
    renderer_.SetHiddenFaces(hiddenFaces_);
}

void Application::ApplyMaskChange(ProjectionTab& tab) {
    if (activeProjectionId_ == tab.id) renderer_.SetMask(tab.mask, tab.featherRadius);
}

void Application::Bake(ProjectionTab& tab) {
    ActivateProjectionTab(tab);
    TextureImage before;
    std::string error;
    if (!renderer_.ReadWorkingTexture(before, error)) {
        SetStatus(error, true);
        return;
    }
    if (!renderer_.BakeProjection(tab.maxAngleDegrees, error)) {
        SetStatus(error, true);
        return;
    }
    undoTextures_.push_back(std::move(before));
    while (undoTextures_.size() > kUndoLimit) undoTextures_.pop_front();
    redoTextures_.clear();
    dirty_ = true;
    ++textureRevision_;
    tab.applied = true;
    tab.status = "Projection baked into the shared working texture.";
    tab.statusIsError = false;
    renderer_.SetProjectionPreview(false);
    SetStatus("Projection baked into the working texture.");
}

void Application::UndoTexture() {
    if (undoTextures_.empty()) return;
    TextureImage current;
    std::string error;
    if (!renderer_.ReadWorkingTexture(current, error)) return;
    redoTextures_.push_back(std::move(current));
    TextureImage previous = std::move(undoTextures_.back());
    undoTextures_.pop_back();
    if (renderer_.SetWorkingTexture(previous, error)) {
        dirty_ = true;
        ++textureRevision_;
        SetStatus("Texture change undone.");
    }
}

void Application::RedoTexture() {
    if (redoTextures_.empty()) return;
    TextureImage current;
    std::string error;
    if (!renderer_.ReadWorkingTexture(current, error)) return;
    undoTextures_.push_back(std::move(current));
    TextureImage next = std::move(redoTextures_.back());
    redoTextures_.pop_back();
    if (renderer_.SetWorkingTexture(next, error)) {
        dirty_ = true;
        ++textureRevision_;
        SetStatus("Texture change redone.");
    }
}

void Application::HandleCodexEvents() {
    for (CodexEvent& event : codex_.PollEvents()) {
        ProjectionTab* tab = FindProjectionTab(event.jobId);
        if (!tab) {
            if (event.jobId == 0) SetStatus(event.message, event.type == CodexEventType::Error);
            continue;
        }
        if (event.type == CodexEventType::GeneratedImage) {
            tempFilesDirty_ = true;
            TextureImage image;
            TextureImage capture;
            std::string error;
            if (!image.LoadPng(event.imagePath, error) || !capture.LoadPng(tab->capturePath, error) ||
                !SameAspect(capture, image) || image.Width() != image.Height()) {
                tab->status = error.empty()
                    ? "ImageGen result must be square to match the captured crop." : error;
                tab->statusIsError = true;
                continue;
            }
            if (activeProjectionId_ == tab->id && !renderer_.SetProjectionImage(image, error)) {
                tab->status = error;
                tab->statusIsError = true;
                continue;
            }
            tab->projectionImage = std::move(image);
            tab->projectionPath = event.imagePath;
            tab->projectionLoaded = true;
            tab->status = "ImageGen result loaded; refine the mask before baking.";
            tab->statusIsError = false;
            if (activeProjectionId_ == tab->id) renderer_.SetProjectionPreview(true);
        } else if (event.type == CodexEventType::MaskProposalReady && event.maskProposal) {
            tab->mask.Clear(false);
            tab->mask.ApplyProposal(*event.maskProposal);
            tab->featherRadius = event.maskProposal->suggestedFeatherPx;
            ApplyMaskChange(*tab);
            tab->status = "Codex mask proposal applied; refine it before baking.";
            tab->statusIsError = false;
        } else {
            tab->status = event.message;
            tab->statusIsError = event.type == CodexEventType::Error;
        }
    }
    for (auto& tab : projectionTabs_) {
        if (!codex_.IsBusy(tab.id)) tab.generationStartedAt.reset();
    }
}

void Application::SetStatus(std::string status, const bool error) {
    status_ = std::move(status);
    statusIsError_ = error;
}

std::filesystem::path Application::OpenFileDialog(const wchar_t* title, const wchar_t* filter) {
    std::array<wchar_t, 32768> path{};
    OPENFILENAMEW dialog{sizeof(OPENFILENAMEW)};
    dialog.hwndOwner = window_;
    dialog.lpstrTitle = title;
    dialog.lpstrFilter = filter;
    dialog.lpstrFile = path.data();
    dialog.nMaxFile = static_cast<DWORD>(path.size());
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameW(&dialog) ? std::filesystem::path(path.data()) : std::filesystem::path{};
}

std::filesystem::path Application::SaveFileDialog(const wchar_t* title, const wchar_t* filter,
                                                   const std::filesystem::path& initial) {
    std::array<wchar_t, 32768> path{};
    if (!initial.empty()) wcsncpy_s(path.data(), path.size(), initial.c_str(), _TRUNCATE);
    OPENFILENAMEW dialog{sizeof(OPENFILENAMEW)};
    dialog.hwndOwner = window_;
    dialog.lpstrTitle = title;
    dialog.lpstrFilter = filter;
    dialog.lpstrDefExt = L"png";
    dialog.lpstrFile = path.data();
    dialog.nMaxFile = static_cast<DWORD>(path.size());
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetSaveFileNameW(&dialog) ? std::filesystem::path(path.data()) : std::filesystem::path{};
}

std::filesystem::path Application::CreateSessionDirectory() const {
    std::error_code error;
    const auto temp = std::filesystem::temp_directory_path(error);
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    return temp / (L"CodexTex-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(ticks));
}

} // namespace codextex
