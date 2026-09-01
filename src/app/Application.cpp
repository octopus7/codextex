#include "app/Application.hpp"

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <commdlg.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <numbers>
#include <optional>
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
    capturePath_ = sessionDirectory_ / L"capture.png";
    codex_.Start(sessionDirectory_);
    if (!koreanFontLoaded) {
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
        ImGui::DockBuilderFinish(dockspace);
    }
    DrawViewport();
    DrawTools();
    DrawTexturePreview();
}

void Application::DrawMenuBar() {
    if (!ImGui::BeginMainMenuBar()) return;
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Open OBJ...", "Ctrl+O")) OpenObj();
        if (ImGui::MenuItem("Open Texture PNG...", "Ctrl+T")) OpenTexture();
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
    Vec2 available{std::max(ImGui::GetContentRegionAvail().x, 1.0f),
                   std::max(ImGui::GetContentRegionAvail().y, 1.0f)};
    renderer_.RenderViewport(static_cast<std::uint32_t>(available.x),
                             static_cast<std::uint32_t>(available.y), camera_);
    const ImVec2 topLeft = ImGui::GetCursorScreenPos();
    ImGui::Image(reinterpret_cast<ImTextureID>(renderer_.ViewportTexture()),
                 ImVec2(available.x, available.y));
    HandleViewportInput({topLeft.x, topLeft.y}, available);

    if (meshLoaded_) {
        const SquareCropFrame crop = CenteredSquare(available);
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImU32 color = captured_ ? IM_COL32(255, 196, 48, 255) : IM_COL32(70, 210, 255, 255);
        const ImVec2 minimum{topLeft.x + crop.origin.x, topLeft.y + crop.origin.y};
        const ImVec2 maximum{minimum.x + crop.side, minimum.y + crop.side};
        draw->AddRect(minimum, maximum, color, 0.0f, 0, 2.0f * dpiScale_);
        draw->AddText(ImVec2(minimum.x + 6.0f * dpiScale_, minimum.y + 5.0f * dpiScale_),
                      color, "ImageGen 1:1 crop");
    }

    if (lassoActive_ && lassoPoints_.size() > 1) {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        for (std::size_t i = 1; i < lassoPoints_.size(); ++i) {
            draw->AddLine(ImVec2(topLeft.x + lassoPoints_[i - 1].x, topLeft.y + lassoPoints_[i - 1].y),
                          ImVec2(topLeft.x + lassoPoints_[i].x, topLeft.y + lassoPoints_[i].y),
                          IM_COL32(255, 205, 40, 255), 2.0f);
        }
    }
    ImGui::End();
}

void Application::HandleViewportInput(const Vec2& topLeft, const Vec2& size) {
    const ImGuiIO& io = ImGui::GetIO();
    const bool hovered = ImGui::IsItemHovered();
    const Vec2 local{io.MousePos.x - topLeft.x, io.MousePos.y - topLeft.y};
    if (hovered && !captured_ && editMode_ == EditMode::Navigate) {
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
    if (!captured_ && ImGui::IsKeyPressed(ImGuiKey_F)) FitCamera();
    if (editMode_ == EditMode::Face && !captured_) {
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
    } else if (editMode_ == EditMode::Mask && captured_) {
        const SquareCropFrame crop = CenteredSquare(size);
        const Vec2 cropLocal{local.x - crop.origin.x, local.y - crop.origin.y};
        const bool insideCrop = crop.Contains(local);
        const float maskScale = static_cast<float>(mask_.Width()) / crop.side;
        if (!useLasso_) {
            const bool painting = ImGui::IsMouseDown(ImGuiMouseButton_Left) ||
                                  ImGui::IsMouseDown(ImGuiMouseButton_Right);
            if (painting && insideCrop) {
                const bool include = ImGui::IsMouseDown(ImGuiMouseButton_Left);
                mask_.PaintCircle(cropLocal.x * maskScale, cropLocal.y * maskScale,
                                  brushRadius_ * maskScale, include);
                ApplyMaskChange();
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
                mask_.ApplyLasso(maskPoints, maskInclude_);
                ApplyMaskChange();
                lassoActive_ = false;
            }
        }
    }
}

void Application::DrawTools() {
    ImGui::Begin("Projection Tools");
    if (ImGui::Button("Open OBJ")) OpenObj();
    ImGui::SameLine();
    if (ImGui::Button("Open Texture PNG")) OpenTexture();
    if (meshLoaded_) {
        ImGui::Text("OBJ: %s", Narrow(mesh_.SourcePath().filename()).c_str());
        ImGui::Text("Triangles: %zu", mesh_.TriangleCount());
        if (mesh_.UvOverlapCount() > 0) {
            ImGui::TextColored(ImVec4(1, 0.65f, 0.2f, 1), "Warning: %zu overlapping UV pair(s)",
                               mesh_.UvOverlapCount());
            ImGui::TextWrapped("Shared or mirrored UVs may let the opposite local-X side overwrite the bake.");
        }
    }
    if (textureLoaded_) {
        ImGui::Text("Texture: %s (%ux%u)", Narrow(texturePath_.filename()).c_str(),
                    sourceTexture_.Width(), sourceTexture_.Height());
    }

    ImGui::SeparatorText("ImageGen reference sets");
    if (ImGui::Button("Add reference OBJ + PNG")) AddReferenceAsset();
    ImGui::SameLine();
    ImGui::BeginDisabled(referenceAssets_.empty());
    if (ImGui::Checkbox("Show in viewport", &referenceAssetsVisible_)) {
        renderer_.SetReferenceAssetsVisible(referenceAssetsVisible_);
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

    ImGui::SeparatorText("Viewport display");
    if (ImGui::Checkbox("Neutral shading", &shadingEnabled_)) {
        renderer_.SetShadingEnabled(shadingEnabled_);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!meshLoaded_ || captured_);
    if (ImGui::Button("Fit primary view (F)")) FitCamera();
    ImGui::EndDisabled();
    ImGui::TextDisabled("Shading is off by default; Base Color is shown unchanged.");

    ImGui::SeparatorText("Mode");
    if (ImGui::RadioButton("Navigate", editMode_ == EditMode::Navigate)) editMode_ = EditMode::Navigate;
    ImGui::SameLine();
    if (ImGui::RadioButton("Faces", editMode_ == EditMode::Face)) editMode_ = EditMode::Face;
    ImGui::SameLine();
    if (ImGui::RadioButton("Mask", editMode_ == EditMode::Mask)) editMode_ = EditMode::Mask;
    if (editMode_ != EditMode::Navigate) {
        ImGui::Checkbox("Lasso", &useLasso_);
    }

    if (editMode_ == EditMode::Face) {
        const auto selectedCount = std::count(selectedFaces_.begin(), selectedFaces_.end(), std::uint8_t{1});
        ImGui::Text("Selected faces: %zu", selectedCount);
        if (ImGui::Button("Hide selected") && selectedCount > 0) HideSelectedFaces();
        ImGui::SameLine();
        if (ImGui::Button("Undo hide") && !hiddenHistory_.empty()) UndoHiddenFaces();
        ImGui::SameLine();
        if (ImGui::Button("Show all")) ShowAllFaces();
    }

    ImGui::SeparatorText("Projection frame");
    if (!captured_) {
        ImGui::BeginDisabled(codex_.IsBusy() || !meshLoaded_ || !textureLoaded_);
        if (ImGui::Button("Capture current view")) CaptureView();
        ImGui::EndDisabled();
    } else {
        ImGui::TextColored(ImVec4(0.45f, 0.85f, 1, 1), "Camera locked to captured view");
        if (ImGui::Button("Cancel projection")) CancelProjection();
    }

    ImGui::SeparatorText("Projection image");
    ImGui::BeginDisabled(!captured_ || codex_.IsBusy());
    if (ImGui::Button("Open external PNG")) OpenProjection();
    ImGui::EndDisabled();
    ImGui::InputTextMultiline("ImageGen prompt", generationPrompt_.data(), generationPrompt_.size(),
                              ImVec2(-1, 90.0f * dpiScale_));
    const bool canGenerate = captured_ && codex_.IsAvailable() && !codex_.IsBusy() &&
                             generationPrompt_[0] != '\0';
    ImGui::BeginDisabled(!canGenerate);
    if (ImGui::Button("Generate with Codex ImageGen")) {
        codex_.BeginGeneration(capturePath_, generationPrompt_.data());
    }
    ImGui::EndDisabled();
    if (codex_.IsBusy()) {
        ImGui::SameLine();
        if (ImGui::Button("Cancel AI")) codex_.Cancel();
    }
    ImGui::TextWrapped("%s", codex_.AvailabilityMessage().c_str());
    if (!codex_.IsAvailable() && !codex_.IsBusy()) {
        if (ImGui::Button("Retry Codex detection")) {
            const bool started = codex_.Start(sessionDirectory_);
            SetStatus(codex_.AvailabilityMessage(), !started || !codex_.IsAvailable());
        }
    }

    if (projectionLoaded_) {
        ImGui::Text("Projection: %s", Narrow(projectionPath_.filename()).c_str());
        ImGui::BeginDisabled(!codex_.IsAvailable() || codex_.IsBusy());
        if (ImGui::Button("Suggest mask with Codex")) {
            codex_.BeginMaskProposal(capturePath_, projectionPath_);
        }
        ImGui::EndDisabled();
    }

    if (editMode_ == EditMode::Mask && captured_) {
        if (!useLasso_) ImGui::SliderFloat("Brush radius", &brushRadius_, 2.0f, 160.0f, "%.0f px");
        if (useLasso_) ImGui::Checkbox("Lasso includes area", &maskInclude_);
        if (ImGui::SliderInt("Inward feather", &featherRadius_, 0, 128, "%d px")) ApplyMaskChange();
        if (ImGui::Button("Clear mask")) {
            mask_.Clear(false);
            ApplyMaskChange();
        }
        ImGui::SameLine();
        if (ImGui::Button("Select all visible")) {
            mask_.Clear(true);
            ApplyMaskChange();
        }
    }
    ImGui::SliderFloat("Max surface angle", &maxAngleDegrees_, 0.0f, 89.0f, "%.0f deg");
    constexpr const char* sideFilterLabels[]{
        "Paint both local-X sides",
        "Ignore local -X side",
        "Ignore local +X side",
    };
    int sideFilter = static_cast<int>(localSideFilter_);
    ImGui::BeginDisabled(!meshLoaded_);
    if (ImGui::Combo("Mirrored UV side", &sideFilter, sideFilterLabels,
                     static_cast<int>(std::size(sideFilterLabels)))) {
        localSideFilter_ = static_cast<LocalSideFilter>(sideFilter);
        renderer_.SetLocalSideFilter(localSideFilter_);
    }
    ImGui::EndDisabled();
    if (localSideFilter_ != LocalSideFilter::Both) {
        ImGui::TextWrapped("The ignored side cannot overwrite this bake. Shared UV texels will still appear on both model sides.");
    }

    ImGui::BeginDisabled(!captured_ || !projectionLoaded_);
    if (ImGui::Button("Bake into texture")) Bake();
    ImGui::EndDisabled();
    ImGui::SameLine();
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

bool Application::OpenObj() {
    const auto path = OpenFileDialog(L"Open UV-mapped OBJ", L"Wavefront OBJ (*.obj)\0*.obj\0\0");
    if (path.empty()) return false;
    Mesh mesh;
    std::string error;
    if (!mesh.LoadObj(path, error)) {
        SetStatus(error, true);
        return false;
    }
    CancelProjection();
    mesh_ = std::move(mesh);
    meshLoaded_ = true;
    localSideFilter_ = LocalSideFilter::Both;
    renderer_.SetLocalSideFilter(localSideFilter_);
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
    CancelProjection();
    sourceTexture_ = std::move(image);
    texturePath_ = path;
    textureLoaded_ = true;
    dirty_ = false;
    undoTextures_.clear();
    redoTextures_.clear();
    SetStatus("Texture PNG loaded.");
    return true;
}

bool Application::OpenProjection() {
    const auto path = OpenFileDialog(L"Open projection PNG", L"PNG image (*.png)\0*.png\0\0");
    if (path.empty()) return false;
    TextureImage image;
    std::string error;
    if (!image.LoadPng(path, error)) {
        SetStatus(error, true);
        return false;
    }
    TextureImage capture;
    if (!capture.LoadPng(capturePath_, error) || !SameAspect(capture, image) ||
        image.Width() != image.Height()) {
        SetStatus("Projection PNG must be square to match the ImageGen crop.", true);
        return false;
    }
    if (!renderer_.SetProjectionImage(image, error)) {
        SetStatus(error, true);
        return false;
    }
    projectionImage_ = std::move(image);
    projectionPath_ = path;
    projectionLoaded_ = true;
    renderer_.SetProjectionPreview(true);
    SetStatus("External projection PNG loaded.");
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

bool Application::CaptureView() {
    std::fill(selectedFaces_.begin(), selectedFaces_.end(), 0);
    renderer_.SetSelectedFaces(selectedFaces_);
    renderer_.RenderViewport(renderer_.ViewportWidth(), renderer_.ViewportHeight(), camera_);
    TextureImage capture;
    std::string error;
    if (!renderer_.CaptureFrame(camera_, capture, error) || !capture.SavePng(capturePath_, error)) {
        SetStatus(error, true);
        return false;
    }
    mask_.Resize(capture.Width(), capture.Height(), false);
    renderer_.SetMask(mask_, featherRadius_);
    captured_ = true;
    projectionLoaded_ = false;
    projectionPath_.clear();
    renderer_.SetProjectionPreview(false);
    editMode_ = EditMode::Mask;
    SetStatus("Square ImageGen crop captured and camera locked.");
    return true;
}

void Application::CancelProjection() {
    if (codex_.IsBusy()) codex_.Cancel();
    captured_ = false;
    projectionLoaded_ = false;
    projectionPath_.clear();
    projectionImage_ = {};
    mask_ = {};
    renderer_.SetMask(mask_, 0);
    renderer_.SetProjectionPreview(false);
    renderer_.ClearFrozenFrame();
    editMode_ = EditMode::Navigate;
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

void Application::ApplyMaskChange() {
    renderer_.SetMask(mask_, featherRadius_);
}

void Application::Bake() {
    TextureImage before;
    std::string error;
    if (!renderer_.ReadWorkingTexture(before, error)) {
        SetStatus(error, true);
        return;
    }
    if (!renderer_.BakeProjection(maxAngleDegrees_, error)) {
        SetStatus(error, true);
        return;
    }
    undoTextures_.push_back(std::move(before));
    while (undoTextures_.size() > kUndoLimit) undoTextures_.pop_front();
    redoTextures_.clear();
    dirty_ = true;
    CancelProjection();
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
        SetStatus("Texture change redone.");
    }
}

void Application::HandleCodexEvents() {
    for (CodexEvent& event : codex_.PollEvents()) {
        if (event.type == CodexEventType::GeneratedImage) {
            if (!captured_) {
                SetStatus("A result from a cancelled projection was discarded.");
                continue;
            }
            TextureImage image;
            TextureImage capture;
            std::string error;
            if (!image.LoadPng(event.imagePath, error) || !capture.LoadPng(capturePath_, error) ||
                !SameAspect(capture, image) || image.Width() != image.Height()) {
                SetStatus(error.empty() ? "ImageGen result must be square to match the captured crop." : error, true);
                continue;
            }
            if (!renderer_.SetProjectionImage(image, error)) {
                SetStatus(error, true);
                continue;
            }
            projectionImage_ = std::move(image);
            projectionPath_ = event.imagePath;
            projectionLoaded_ = true;
            renderer_.SetProjectionPreview(true);
            SetStatus("ImageGen result loaded into the projection session.");
        } else if (event.type == CodexEventType::MaskProposalReady && event.maskProposal) {
            if (!captured_ || !projectionLoaded_) {
                SetStatus("A mask proposal from a cancelled projection was discarded.");
                continue;
            }
            mask_.Clear(false);
            mask_.ApplyProposal(*event.maskProposal);
            featherRadius_ = event.maskProposal->suggestedFeatherPx;
            ApplyMaskChange();
            SetStatus("Codex mask proposal applied; refine it before baking.");
        } else {
            SetStatus(event.message, event.type == CodexEventType::Error);
        }
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
