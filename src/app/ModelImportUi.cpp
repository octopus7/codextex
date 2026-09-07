#include "app/Application.hpp"

#include <imgui.h>
#include <algorithm>
#include <chrono>
#include <cwctype>

namespace codextex {
namespace {
std::string FileName(const std::filesystem::path& path) {
    const auto value = path.filename().u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}
bool IsPng(const std::filesystem::path& path) {
    auto extension = path.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return extension == L".png";
}
}

bool Application::BeginModelImport(const std::filesystem::path& path, const bool reference,
                                    const bool recent) {
    if (modelImportFuture_.valid() || pendingModel_) {
        SetStatus("Finish or cancel the current model import first.", true);
        return false;
    }
    importingReference_ = reference;
    importingRecent_ = recent;
    discardImport_ = false;
    pendingRecentSurfaceIndex_ = recent ? codexSettings_.recentSurfaceIndex : -1;
    pendingRecentSurfaceName_ = recent ? codexSettings_.recentSurfaceName : std::string{};
    pendingOverrideTexture_ = recent ? codexSettings_.recentTexturePath : std::filesystem::path{};
    try {
        modelImportFuture_ = std::async(std::launch::async, [path] {
            ModelImportResult result;
            const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            result.success = LoadModel(path, result.model, result.error);
            if (SUCCEEDED(com)) CoUninitialize();
            return result;
        });
    } catch (const std::exception& e) {
        SetStatus(std::string("Could not start model import: ") + e.what(), true);
        return false;
    }
    SetStatus("Loading model...");
    return true;
}

void Application::PollModelImport() {
    if (!modelImportFuture_.valid() ||
        modelImportFuture_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    ModelImportResult result;
    try { result = modelImportFuture_.get(); }
    catch (const std::exception& e) { result.error = e.what(); }
    if (discardImport_) { SetStatus("Model import canceled."); return; }
    if (!result.success) { SetStatus(result.error, true); return; }
    if (importingReference_) {
        std::string error;
        ReferenceAsset reference;
        reference.objPath = result.model.sourcePath;
        reference.model = std::move(result.model);
        referenceAssets_.reserve(referenceAssets_.size() + 1);
        if (!renderer_.AddReferenceModel(*reference.model, error)) { SetStatus(error, true); return; }
        referenceAssets_.push_back(std::move(reference));
        SetStatus("Reference model loaded.");
        return;
    }
    pendingModel_ = std::move(result.model);
    pendingModelSurface_ = 0;
    for (std::size_t i = 0; i < pendingModel_->surfaces.size(); ++i) {
        if (pendingModel_->surfaces[i].editError.empty() && !pendingModel_->surfaces[i].baseColorMissing) {
            pendingModelSurface_ = i; break;
        }
    }
    if (importingRecent_ && pendingRecentSurfaceIndex_ >= 0 &&
        static_cast<std::size_t>(pendingRecentSurfaceIndex_) < pendingModel_->surfaces.size()) {
        const auto index = static_cast<std::size_t>(pendingRecentSurfaceIndex_);
        if (pendingModel_->surfaces[index].name == pendingRecentSurfaceName_ &&
            pendingModel_->surfaces[index].editError.empty() &&
            (!pendingModel_->surfaces[index].baseColorMissing || !pendingOverrideTexture_.empty())) {
            if (ActivateModelSurface(index, &*pendingModel_, pendingOverrideTexture_)) pendingModel_.reset();
            else openImportPopup_ = true;
            return;
        }
    }
    if (importingRecent_) {
        pendingOverrideTexture_.clear();
        pendingModel_->warnings.push_back("The saved material changed. Select the intended material again.");
        openImportPopup_ = true;
        return;
    }
    if (pendingModel_->surfaces.size() == 1 && pendingModel_->surfaces[0].editError.empty() &&
        !pendingModel_->surfaces[0].baseColorMissing) {
        if (ActivateModelSurface(0, &*pendingModel_, pendingOverrideTexture_)) pendingModel_.reset();
        else openImportPopup_ = true;
    } else {
        openImportPopup_ = true;
    }
}

void Application::ResetMeshSelection() {
    hiddenFaces_.assign(mesh_.TriangleCount(), 0);
    selectedFaces_.assign(mesh_.TriangleCount(), 0);
    renderer_.SetHiddenFaces(hiddenFaces_);
    renderer_.SetSelectedFaces(selectedFaces_);
    hiddenHistory_.clear();
    renderer_.SetLocalSideFilter(LocalSideFilter::Both);
    camera_.yaw = 0.0f;
    camera_.pitch = 0.15f;
    camera_.fovDegrees = 45.0f;
    FitCamera();
}

bool Application::ActivateModelSurface(const std::size_t index, ImportedModel* replacement,
                                       const std::filesystem::path& overrideTexture) {
    ImportedModel* model = replacement ? replacement : (importedModel_ ? &*importedModel_ : nullptr);
    if (!model || index >= model->surfaces.size()) return false;
    const auto& surface = model->surfaces[index];
    if (!surface.editError.empty()) { SetStatus(surface.editError, true); return false; }
    if (surface.baseColorMissing && overrideTexture.empty()) {
        SetStatus("Choose a Base Color PNG to replace the missing image.", true);
        return false;
    }
    if (textureHistory_.IsDirty() && !CanClose()) return false;
    TextureImage image = surface.baseColor;
    std::string error;
    if (!overrideTexture.empty() && !image.LoadPng(overrideTexture, error)) {
        SetStatus(error, true);
        return false;
    }
    Mesh mesh = surface.mesh;
    const auto imagePath = !overrideTexture.empty() ? overrideTexture :
        (IsPng(surface.externalTexturePath) ? surface.externalTexturePath : std::filesystem::path{});
    if (!renderer_.SetImportedModel(*model, index, image, error)) { SetStatus(error, true); return false; }
    ClearProjectionTabs();
    mesh_ = std::move(mesh);
    sourceTexture_ = std::move(image);
    texturePath_ = imagePath;
    if (replacement) importedModel_ = std::move(*replacement);
    selectedModelSurface_ = index;
    if (!overrideTexture.empty() && importedModel_) {
        auto& selected = importedModel_->surfaces[index];
        const auto key = selected.textureKey;
        for (auto& item : importedModel_->surfaces) {
            if (&item == &selected || (!key.empty() && item.textureKey == key)) {
                item.baseColor = sourceTexture_;
                item.externalTexturePath = overrideTexture;
                item.baseColorMissing = false;
            }
        }
    }
    meshLoaded_ = textureLoaded_ = true;
    mainOriginalTexturePreview_ = false;
    textureHistory_.Clear();
    ++textureRevision_;
    ResetMeshSelection();
    SetStatus("Model and Base Color loaded.");
    RememberRecentPrimaryAssets();
    return true;
}

void Application::DrawModelImport() {
    if (modelImportFuture_.valid()) {
        ImGui::TextUnformatted(Tr(discardImport_ ? "Finishing canceled import..." : "Loading model..."));
        if (!discardImport_) {
            ImGui::SameLine();
            if (ImGui::SmallButton(Tr("Cancel import"))) discardImport_ = true;
        }
    }
    if (importedModel_ && !activeProjectionId_) {
        const auto& surfaces = importedModel_->surfaces;
        std::optional<std::size_t> select;
        if (ImGui::BeginCombo(Tr("Editing material"), surfaces[selectedModelSurface_].name.c_str())) {
            for (std::size_t i = 0; i < surfaces.size(); ++i) {
                ImGui::PushID(static_cast<int>(i));
                ImGui::BeginDisabled(!surfaces[i].editError.empty());
                if (ImGui::Selectable(surfaces[i].name.c_str(), selectedModelSurface_ == i)) select = i;
                ImGui::EndDisabled();
                if (!surfaces[i].editError.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("%s", LocalizedMessage(surfaces[i].editError).c_str());
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        if (select && *select != selectedModelSurface_) {
            if (surfaces[*select].baseColorMissing) {
                const auto path = OpenFileDialog("Open Base Color PNG", L"PNG image (*.png)\0*.png\0\0");
                if (!path.empty()) ActivateModelSurface(*select, nullptr, path);
            } else ActivateModelSurface(*select);
        }
        const auto& key = surfaces[selectedModelSurface_].textureKey;
        if (!key.empty() && std::count_if(surfaces.begin(), surfaces.end(), [&key](const auto& s) {
            return s.textureKey == key;
        }) > 1) ImGui::TextWrapped("%s", Tr("This texture is shared by multiple materials."));
        if (!importedModel_->warnings.empty()) {
            ImGui::Text(Tr("Import warnings: %zu"), importedModel_->warnings.size());
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::PushTextWrapPos(500 * dpiScale_);
                for (const auto& warning : importedModel_->warnings)
                    ImGui::TextUnformatted(LocalizedMessage(warning).c_str());
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }
        }
    }
    const auto popup = WindowLabel("Choose editing material", "ModelImportPopup");
    if (openImportPopup_) { ImGui::OpenPopup(popup.c_str()); openImportPopup_ = false; }
    if (ImGui::BeginPopupModal(popup.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (pendingModel_) {
            ImGui::TextUnformatted(FileName(pendingModel_->sourcePath).c_str());
            if (ImGui::BeginListBox("##Materials", ImVec2(460 * dpiScale_, 220 * dpiScale_))) {
                for (std::size_t i = 0; i < pendingModel_->surfaces.size(); ++i) {
                    ImGui::PushID(static_cast<int>(i));
                    if (ImGui::Selectable(pendingModel_->surfaces[i].name.c_str(), pendingModelSurface_ == i))
                        pendingModelSurface_ = i;
                    ImGui::PopID();
                }
                ImGui::EndListBox();
            }
            const auto& surface = pendingModel_->surfaces[pendingModelSurface_];
            ImGui::Text(Tr("Triangles: %zu"), surface.mesh.TriangleCount());
            ImGui::Text(Tr("Base Color: %u x %u"), surface.baseColor.Width(), surface.baseColor.Height());
            if (!surface.editError.empty()) {
                ImGui::PushTextWrapPos(460 * dpiScale_);
                ImGui::TextUnformatted(LocalizedMessage(surface.editError).c_str());
                ImGui::PopTextWrapPos();
            }
            if (surface.baseColorMissing) {
                ImGui::TextWrapped("%s", Tr("Choose a Base Color PNG to replace the missing image."));
                if (ImGui::Button(Tr("Choose Base Color PNG"))) {
                    const auto path = OpenFileDialog("Open Base Color PNG", L"PNG image (*.png)\0*.png\0\0");
                    TextureImage image;
                    std::string error;
                    if (!path.empty() && image.LoadPng(path, error)) {
                        const auto key = surface.textureKey;
                        for (auto& item : pendingModel_->surfaces) {
                            if (&item == &surface || (!key.empty() && item.textureKey == key)) {
                                item.baseColor = image;
                                item.externalTexturePath = path;
                                item.baseColorMissing = false;
                            }
                        }
                        pendingOverrideTexture_.clear();
                    } else if (!path.empty()) SetStatus(error, true);
                }
            }
            ImGui::BeginDisabled(!surface.editError.empty() || surface.baseColorMissing);
            if (ImGui::Button(Tr("Use material")) &&
                ActivateModelSurface(pendingModelSurface_, &*pendingModel_, pendingOverrideTexture_)) {
                pendingModel_.reset();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button(Tr("Cancel"))) { pendingModel_.reset(); ImGui::CloseCurrentPopup(); }
        } else ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

} // namespace codextex
