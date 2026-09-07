#pragma once

#include "core/Mask.hpp"
#include "core/ProjectionViewTransform.hpp"
#include "core/TextureImage.hpp"
#include "graphics/Renderer.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace codextex {

enum class ProjectionViewMode { Working, GeneratedFull, Original };

struct ProjectionWorkspace {
    std::uint64_t id{};
    Renderer::ProjectionFrame frame;
    CameraState camera{};
    std::vector<std::uint8_t> hiddenFaces;
    TextureImage projectionImage;
    bool projectionUploadPending{true};
    MaskImage mask;
    bool maskUploadPending{true};
    std::filesystem::path capturePath;
    std::filesystem::path temporaryDirectory;
    std::filesystem::path projectionPath;
    std::filesystem::path metadataPath;
    std::string prompt;
    std::string status = "Waiting for projection image.";
    std::string model;
    std::string reasoningEffort;
    bool statusIsError{};
    bool projectionLoaded{};
    bool referenceAssetsVisible{true};
    bool captureShadingEnabled{};
    bool temporaryCleanupBlocked{};
    std::array<float, 3> captureBackgroundColor{};
    std::vector<std::pair<std::filesystem::path, std::filesystem::path>> referencePaths;
    bool applied{};
    ProjectionViewMode viewMode{ProjectionViewMode::Working};
    ProjectionViewTransform displayTransform{};
    Vec2 projectionOffsetPixels{};
    float brushRadius{28.0f};
    int featherRadius{16};
    float maxAngleDegrees{75.0f};
    LocalSideFilter localSideFilter{LocalSideFilter::Both};
    std::uint64_t baseTextureRevision{};
    std::optional<std::chrono::steady_clock::time_point> generationStartedAt;
    std::optional<std::int64_t> generationDurationSeconds;
    bool generationDurationRecorded{};
};

class ProjectionWorkspaces {
public:
    using BeforeRemove = std::function<void(ProjectionWorkspace&)>;

    // IDs route asynchronous results and must be unique within the collection.
    ProjectionWorkspace& Add(ProjectionWorkspace workspace);
    [[nodiscard]] ProjectionWorkspace* Find(std::uint64_t id) noexcept;
    [[nodiscard]] const ProjectionWorkspace* Find(std::uint64_t id) const noexcept;

    // Hooks run while the workspace is still owned, before frozen resources are released.
    // A hook may update its workspace, but must not add/remove collection entries.
    bool Erase(std::uint64_t id, const BeforeRemove& beforeRemove);
    void EraseAll(const BeforeRemove& beforeRemove);
    [[nodiscard]] bool HasRecoveryFiles() const noexcept;
    // Called only after an explicitly requested session-wide deletion succeeds.
    void ForgetRecoveryFiles() noexcept { recoveryDirectories_.clear(); }

    [[nodiscard]] bool empty() const noexcept { return workspaces_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return workspaces_.size(); }
    auto begin() noexcept { return workspaces_.begin(); }
    auto end() noexcept { return workspaces_.end(); }
    auto begin() const noexcept { return workspaces_.begin(); }
    auto end() const noexcept { return workspaces_.end(); }

private:
    void RememberRecoveryFiles(const ProjectionWorkspace& workspace);
    std::vector<ProjectionWorkspace> workspaces_;
    std::vector<std::filesystem::path> recoveryDirectories_;
};

} // namespace codextex
