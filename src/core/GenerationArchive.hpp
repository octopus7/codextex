#pragma once

#include "core/Types.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace codextex {

struct GenerationReferenceMetadata {
    std::filesystem::path objPath;
    std::filesystem::path texturePath;
};

struct GenerationArchiveMetadata {
    std::uint64_t jobId{};
    std::string prompt;
    std::string model;
    std::string reasoningEffort;
    std::filesystem::path objPath;
    std::filesystem::path texturePath;
    Vec3 cameraTarget{};
    std::string materialName;
    std::string textureKey;
    float cameraYaw{};
    float cameraPitch{};
    float cameraDistance{};
    float cameraFovDegrees{};
    std::uint32_t captureWidth{};
    std::uint32_t captureHeight{};
    std::uint32_t cropX{};
    std::uint32_t cropY{};
    std::uint32_t cropSize{};
    std::vector<std::uint32_t> hiddenTriangles;
    bool referenceAssetsVisible{};
    bool shadingEnabled{};
    std::array<float, 3> backgroundColor{};
    std::vector<GenerationReferenceMetadata> references;
};

struct GenerationArchivePaths {
    std::filesystem::path directory;
    std::filesystem::path image;
    std::filesystem::path metadata;
};

bool SaveGenerationArchive(const std::filesystem::path& root,
                           const std::filesystem::path& sourceImage,
                           const GenerationArchiveMetadata& metadata,
                           GenerationArchivePaths& paths,
                           std::string& error);

} // namespace codextex
