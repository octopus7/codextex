#include "core/ModelImport.hpp"

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <exception>

namespace codextex {

void MakeFallbackTexture(TextureImage& image) {
    const std::vector<std::uint8_t> white(1024 * 1024 * 4, 255);
    image.Assign(1024, 1024, white);
}

std::string ValidateEditableSurface(const Mesh& mesh, const bool hasUvs,
                                    const MaterialAppearance& appearance) {
    if (!hasUvs) return "This material has no UV coordinates. Supply UVs in the source model.";
    if (appearance.alphaMode == MaterialAlphaMode::Blend)
        return "Translucent materials are display-only; use an opaque or alpha-mask material for editing.";
    for (std::size_t i = 0; i < 3; ++i) {
        if (!std::isfinite(appearance.baseColorFactor[i]) || appearance.baseColorFactor[i] < 0.0001f)
            return "A zero Base Color factor prevents texture projection. Adjust the source material.";
    }
    for (const auto& v : mesh.Vertices()) {
        if (v.uv.x < -1.0e-5f || v.uv.x > 1.00001f ||
            v.uv.y < -1.0e-5f || v.uv.y > 1.00001f)
            return "Editing requires a single 0..1 UV atlas. Tiled and UDIM UVs are display-only.";
    }
    return {};
}

bool LoadModel(const std::filesystem::path& path, ImportedModel& model, std::string& error) {
    try {
        std::error_code fileError;
        const auto bytes = std::filesystem::file_size(path, fileError);
        if (fileError || bytes == 0 || bytes > MaxModelFileBytes) {
            error = "Model file is missing, empty, or exceeds the 512 MiB file limit.";
            return false;
        }
        auto extension = path.extension().wstring();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        ImportedModel candidate;
        if (extension == L".fbx") {
            if (!LoadFbxModel(path, candidate, error)) return false;
        } else if (extension == L".glb") {
            if (!LoadGlbModel(path, candidate, error)) return false;
        } else if (extension == L".obj") {
            ModelSurface surface;
            surface.name = "OBJ";
            surface.appearance.wrapU = surface.appearance.wrapV = TextureWrap::Clamp;
            if (!surface.mesh.LoadObj(path, error)) return false;
            if (!error.empty()) candidate.warnings.push_back(error);
            MakeFallbackTexture(surface.baseColor);
            candidate.surfaces.push_back(std::move(surface));
        } else {
            error = "Unsupported model format. Choose an OBJ, FBX, or GLB file.";
            return false;
        }
        if (candidate.surfaces.empty()) { error = "Model contains no mesh surfaces."; return false; }
        std::size_t triangles = 0;
        std::size_t textureBytes = 0;
        for (const auto& surface : candidate.surfaces) {
            triangles += surface.mesh.TriangleCount();
            textureBytes += surface.baseColor.Pixels().size();
            if (triangles > MaxImportedTriangles || textureBytes > 512ull * 1024 * 1024) {
                error = "Model exceeds the triangle or decoded texture memory limit.";
                return false;
            }
            if (!surface.mesh.UvOverlapCheckComplete())
                candidate.warnings.push_back("UV overlap analysis reached its budget; the overlap count is incomplete.");
        }
        candidate.sourcePath = path;
        model = std::move(candidate);
        error.clear();
        return true;
    } catch (const std::exception& e) {
        error = std::string("Could not import model: ") + e.what();
        return false;
    }
}

} // namespace codextex
