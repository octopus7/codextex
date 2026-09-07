#pragma once

#include "core/Mesh.hpp"
#include "core/TextureImage.hpp"

#include <array>
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace codextex {

enum class MaterialAlphaMode { Opaque, Mask, Blend };
enum class TextureWrap { Clamp, Repeat, Mirror };

struct MaterialAppearance {
    std::array<float, 4> baseColorFactor{1, 1, 1, 1};
    MaterialAlphaMode alphaMode{MaterialAlphaMode::Opaque};
    float alphaCutoff{0.5f};
    bool doubleSided{true};
    TextureWrap wrapU{TextureWrap::Repeat};
    TextureWrap wrapV{TextureWrap::Repeat};
};

// Each surface combines the triangles using one material. Geometry is static,
// triangulated, expanded per corner, and converted to right-handed Y-up meters.
struct ModelSurface {
    std::string name;
    Mesh mesh;
    TextureImage baseColor;
    std::filesystem::path externalTexturePath;
    std::string textureKey;
    MaterialAppearance appearance;
    bool baseColorMissing{};
    // Empty means UV-space editing is supported. Unsupported surfaces may still
    // be displayed as context, but must never silently enter the bake target.
    std::string editError;
};

struct ImportedModel {
    std::filesystem::path sourcePath;
    std::vector<ModelSurface> surfaces;
    std::vector<std::string> warnings;
};

inline constexpr std::size_t MaxImportedTriangles = 2'000'000;
inline constexpr std::size_t MaxModelFileBytes = 512ull * 1024 * 1024;

bool LoadModel(const std::filesystem::path& path, ImportedModel& model, std::string& error);
bool LoadFbxModel(const std::filesystem::path& path, ImportedModel& model, std::string& error);
bool LoadGlbModel(const std::filesystem::path& path, ImportedModel& model, std::string& error);

// Shared importer validation. Does not repair or repack the authored UV atlas.
std::string ValidateEditableSurface(const Mesh& mesh, bool hasUvs,
                                    const MaterialAppearance& appearance);
void MakeFallbackTexture(TextureImage& image);

} // namespace codextex
