#include "app/Application.hpp"

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <commdlg.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <system_error>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND window, UINT message, WPARAM wParam, LPARAM lParam);

namespace codextex {
namespace {

constexpr wchar_t kWindowClass[] = L"CodexTexWindow";
constexpr std::size_t kUndoLimit = 8;

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

} // namespace

bool Application::Initialize(HINSTANCE instance, const int showCommand, std::string& error) {
    instance_ = instance;
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
    window_ = CreateWindowExW(0, kWindowClass, L"CodexTex", WS_OVERLAPPEDWINDOW,
                              CW_USEDEFAULT, CW_USEDEFAULT, 1500, 900, nullptr, nullptr, instance, this);
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
    ImGui::StyleColorsDark();
    ImGui::GetStyle().WindowRounding = 4.0f;
    ImGui_ImplWin32_Init(window_);
    ImGui_ImplDX11_Init(renderer_.Device(), renderer_.Context());

    sessionDirectory_ = CreateSessionDirectory();
    std::error_code directoryError;
    std::filesystem::create_directories(sessionDirectory_, directoryError);
    capturePath_ = sessionDirectory_ / L"capture.png";
    codex_.Start(sessionDirectory_);

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
    if (ImGui::GetCurrentContext() && ImGui_ImplWin32_WndProcHandler(window, message, wParam, lParam)) {
        return TRUE;
    }
    Application* app = reinterpret_cast<Application*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        app = static_cast<Application*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
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

void Application::DrawUi() {
    ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());
    DrawMenuBar();
    DrawViewport();
    DrawTools();
    DrawTexturePreview();
}

void Application::DrawMenuBar() {
    if (!ImGui::BeginMainMenuBar()) return;
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Open OBJ...", "Ctrl+O")) OpenObj();
        if (ImGui::MenuItem("Open Texture PNG...", "Ctrl+T")) OpenTexture();
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
        const float scaleX = static_cast<float>(mask_.Width()) / std::max(size.x, 1.0f);
        const float scaleY = static_cast<float>(mask_.Height()) / std::max(size.y, 1.0f);
        if (!useLasso_) {
            const bool painting = ImGui::IsMouseDown(ImGuiMouseButton_Left) ||
                                  ImGui::IsMouseDown(ImGuiMouseButton_Right);
            if (painting) {
                const bool include = ImGui::IsMouseDown(ImGuiMouseButton_Left);
                mask_.PaintCircle(local.x * scaleX, local.y * scaleY,
                                  brushRadius_ * std::max(scaleX, scaleY), include);
                ApplyMaskChange();
            }
        } else {
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                lassoActive_ = true;
                lassoPoints_.clear();
            }
            if (lassoActive_ && ImGui::IsMouseDown(ImGuiMouseButton_Left)) lassoPoints_.push_back(local);
            if (lassoActive_ && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                std::vector<Vec2> maskPoints;
                maskPoints.reserve(lassoPoints_.size());
                for (const Vec2 point : lassoPoints_) maskPoints.push_back({point.x * scaleX, point.y * scaleY});
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
        }
    }
    if (textureLoaded_) {
        ImGui::Text("Texture: %s (%ux%u)", Narrow(texturePath_.filename()).c_str(),
                    sourceTexture_.Width(), sourceTexture_.Height());
    }

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
                              ImVec2(-1, 90));
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
    hiddenFaces_.assign(mesh_.TriangleCount(), 0);
    selectedFaces_.assign(mesh_.TriangleCount(), 0);
    hiddenHistory_.clear();
    if (!renderer_.SetMesh(mesh_, error)) {
        meshLoaded_ = false;
        SetStatus(error, true);
        return false;
    }
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
    if (!capture.LoadPng(capturePath_, error) || !SameAspect(capture, image)) {
        SetStatus("Projection PNG must have the same aspect ratio as the captured viewport.", true);
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
    SetStatus("View captured and camera locked.");
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
    const float dx = high.x - low.x;
    const float dy = high.y - low.y;
    const float dz = high.z - low.z;
    camera_.distance = std::max(std::sqrt(dx * dx + dy * dy + dz * dz) * 1.35f, 0.01f);
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
                !SameAspect(capture, image)) {
                SetStatus(error.empty() ? "Generated image aspect ratio does not match the captured view." : error, true);
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
