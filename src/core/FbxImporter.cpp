#include "core/ModelImport.hpp"

#include "ufbx.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <memory>
#include <unordered_map>
#include <utility>

namespace codextex {
namespace {

std::string Text(ufbx_string value) {
    return value.length ? std::string(value.data, value.length) : std::string{};
}

std::string PathKey(const std::filesystem::path& path) {
    const auto utf8 = path.lexically_normal().generic_u8string();
    return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
}

std::filesystem::path TexturePath(ufbx_string value) {
    const auto text = Text(value);
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

bool Finite(ufbx_vec3 value) {
    constexpr auto limit = static_cast<double>(std::numeric_limits<float>::max());
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) &&
           std::abs(value.x) <= limit && std::abs(value.y) <= limit && std::abs(value.z) <= limit;
}

Vec3 Convert(ufbx_vec3 value) {
    return {static_cast<float>(value.x), static_cast<float>(value.y), static_cast<float>(value.z)};
}

struct PendingSurface {
    ModelSurface surface;
    std::vector<Vertex> triangles;
    const ufbx_texture* texture{};
    bool hasUvs{true};
};

const ufbx_texture* EnabledTexture(const ufbx_material_map& map) {
    return map.texture_enabled && !map.feature_disabled ? map.texture : nullptr;
}

bool ReadTexture(const std::filesystem::path& modelPath, const ufbx_scene& scene,
                 const ufbx_texture& texture, ModelSurface& surface, std::string& error,
                 const std::size_t remainingTextureBytes) {
    ufbx_blob content = texture.content;
    if (!content.size && texture.video) content = texture.video->content;
    if (!content.size && texture.has_file && texture.file_index < scene.texture_files.count) {
        content = scene.texture_files.data[texture.file_index].content;
    }
    if (content.size) {
        surface.textureKey = "fbx:embedded:" + PathKey(modelPath) + ":" +
            std::to_string(texture.has_file ? texture.file_index : texture.element_id);
        if (content.size > 128ull * 1024 * 1024) {
            error = "Embedded FBX texture exceeds the 128 MiB encoded image limit.";
            return false;
        }
        return surface.baseColor.LoadEncoded(
            {static_cast<const std::uint8_t*>(content.data), content.size}, error, remainingTextureBytes);
    }

    std::vector<std::filesystem::path> candidates;
    const auto append = [&](ufbx_string name) {
        if (!name.length) return;
        auto candidate = TexturePath(name);
        if (candidate.is_relative()) candidate = modelPath.parent_path() / candidate;
        candidates.push_back(candidate.lexically_normal());
    };
    append(texture.relative_filename);
    append(texture.filename);
    append(texture.absolute_filename);
    // Exporters often leave an absolute path from the author's machine. A
    // colocated image with the same filename is a useful portable fallback.
    if (texture.filename.length) {
        candidates.push_back(modelPath.parent_path() / TexturePath(texture.filename).filename());
    }
    if (!candidates.empty()) {
        // Keep missing-image references shared as well, so relinking one PNG
        // repairs every material that refers to the same authored image.
        std::error_code filesystemError;
        const auto canonical = std::filesystem::weakly_canonical(candidates.front(), filesystemError);
        surface.textureKey = "file:" + PathKey(filesystemError ? candidates.front() : canonical);
    } else {
        surface.textureKey = "fbx:external:" + PathKey(modelPath) + ":" +
            std::to_string(texture.has_file ? texture.file_index : texture.element_id);
    }
    for (const auto& candidate : candidates) {
        std::error_code filesystemError;
        if (!std::filesystem::is_regular_file(candidate, filesystemError)) continue;
        std::ifstream stream(candidate, std::ios::binary | std::ios::ate);
        if (!stream) continue;
        const auto size = stream.tellg();
        if (size <= 0 || static_cast<std::uint64_t>(size) > 128ull * 1024 * 1024) {
            error = "External FBX texture is empty or exceeds the 128 MiB encoded image limit.";
            continue;
        }
        std::vector<std::uint8_t> encoded(static_cast<std::size_t>(size));
        stream.seekg(0);
        if (!stream.read(reinterpret_cast<char*>(encoded.data()), static_cast<std::streamsize>(encoded.size()))) {
            error = "Could not read the complete external FBX texture.";
            continue;
        }
        if (surface.baseColor.LoadEncoded(encoded, error, remainingTextureBytes)) {
            surface.externalTexturePath = candidate;
            auto canonical = std::filesystem::weakly_canonical(candidate, filesystemError);
            surface.textureKey = "file:" + PathKey(filesystemError ? candidate : canonical);
            return true;
        }
    }
    if (error.empty()) error = "Base Color image could not be found or decoded.";
    return false;
}

void SetAlphaMode(ModelSurface& surface) {
    bool hasTransparent = false;
    bool hasPartialAlpha = surface.appearance.baseColorFactor[3] < 1.0f;
    const auto& pixels = surface.baseColor.Pixels();
    for (std::size_t index = 3; index < pixels.size(); index += 4) {
        hasTransparent |= pixels[index] < 255;
        hasPartialAlpha |= pixels[index] != 0 && pixels[index] != 255;
    }
    // FBX has no standardized MASK/BLEND flag. Binary image alpha can safely
    // use a cutout; fractional opacity requires the restricted blend path.
    surface.appearance.alphaMode = hasPartialAlpha ? MaterialAlphaMode::Blend :
        hasTransparent ? MaterialAlphaMode::Mask : MaterialAlphaMode::Opaque;
}

bool PrepareMaterial(const std::filesystem::path& path, const ufbx_scene& scene,
                     const ufbx_material* material, PendingSurface& pending,
                     std::vector<std::string>& warnings, std::string& error,
                     const std::size_t remainingTextureBytes) {
    auto& surface = pending.surface;
    surface.name = material && material->name.length ? Text(material->name) : "Default material";
    surface.textureKey = "fbx:material:" + PathKey(path) + ":" +
        std::to_string(material ? material->element_id : UFBX_NO_INDEX);
    if (material) {
        // ufbx maps the legacy diffuse properties into pbr.base_color as well.
        // Keep the tint separate from the source image for reversible PNG edits.
        const auto& color = material->pbr.base_color;
        const auto& factor = material->pbr.base_factor;
        const double weight = factor.has_value ? factor.value_real : 1.0;
        if (color.has_value) {
            surface.appearance.baseColorFactor = {
                static_cast<float>(color.value_vec4.x * weight),
                static_cast<float>(color.value_vec4.y * weight),
                static_cast<float>(color.value_vec4.z * weight),
                color.value_components == 4 ? static_cast<float>(color.value_vec4.w) : 1.0f};
        } else {
            for (std::size_t index = 0; index < 3; ++index)
                surface.appearance.baseColorFactor[index] = static_cast<float>(weight);
        }
        const auto& opacity = material->pbr.opacity;
        if (opacity.has_value) {
            surface.appearance.baseColorFactor[3] *= static_cast<float>(opacity.value_real);
        } else if (material->fbx.transparency_factor.has_value) {
            const auto& transparentColor = material->fbx.transparency_color;
            const double transparent = transparentColor.has_value ?
                (transparentColor.value_vec3.x + transparentColor.value_vec3.y + transparentColor.value_vec3.z) / 3.0 : 1.0;
            surface.appearance.baseColorFactor[3] *= static_cast<float>(
                1.0 - material->fbx.transparency_factor.value_real * transparent);
        }
        for (auto& component : surface.appearance.baseColorFactor) {
            if (!std::isfinite(component)) {
                error = "FBX material contains a non-finite Base Color or opacity.";
                return false;
            }
            component = std::clamp(component, 0.0f, 1.0f);
        }
        if (material->features.double_sided.is_explicit) {
            surface.appearance.doubleSided = material->features.double_sided.enabled;
        }
        pending.texture = EnabledTexture(color);
        if (!pending.texture) pending.texture = EnabledTexture(material->fbx.diffuse_color);
        if (EnabledTexture(opacity) || EnabledTexture(material->fbx.transparency_color) ||
            EnabledTexture(material->fbx.transparency_factor)) {
            surface.editError = "Separate FBX opacity textures are unsupported; export Base Color with RGBA alpha.";
            warnings.push_back(surface.name + ": " + surface.editError);
        }
    }
    if (pending.texture) {
        if (pending.texture->type != UFBX_TEXTURE_FILE) {
            surface.editError = "Layered or procedural FBX Base Color is unsupported; bake it to an image before export.";
            warnings.push_back(surface.name + ": " + surface.editError);
        } else {
            if (pending.texture->has_uv_transform &&
                (!std::isfinite(ufbx_matrix_determinant(&pending.texture->texture_to_uv)) ||
                 ufbx_matrix_determinant(&pending.texture->texture_to_uv) == 0.0)) {
                error = "FBX Base Color texture has a singular or non-finite UV transform.";
                return false;
            }
            surface.appearance.wrapU = pending.texture->wrap_u == UFBX_WRAP_CLAMP ? TextureWrap::Clamp : TextureWrap::Repeat;
            surface.appearance.wrapV = pending.texture->wrap_v == UFBX_WRAP_CLAMP ? TextureWrap::Clamp : TextureWrap::Repeat;
            std::string textureError;
            if (!ReadTexture(path, scene, *pending.texture, surface, textureError, remainingTextureBytes)) {
                warnings.push_back(surface.name + ": " + textureError + " A white fallback texture is displayed.");
                surface.baseColorMissing = true;
            }
        }
    }
    if (surface.baseColor.Empty()) {
        if (1024ull * 1024 * 4 > remainingTextureBytes) {
            error = "FBX exceeds the 512 MiB decoded texture memory limit.";
            return false;
        }
        MakeFallbackTexture(surface.baseColor);
    }
    SetAlphaMode(surface);
    return true;
}

const ufbx_vertex_vec2* FindUvs(const ufbx_mesh& mesh, const ufbx_texture* texture) {
    if (texture && texture->uv_set.length) {
        for (const auto& set : mesh.uv_sets) {
            if (Text(set.name) == Text(texture->uv_set)) return &set.vertex_uv;
        }
        // "default" is the FBX SDK default alias for the first UV set.
        if (Text(texture->uv_set) != "default") return nullptr;
    }
    return mesh.vertex_uv.exists ? &mesh.vertex_uv : nullptr;
}

bool ImportScene(const std::filesystem::path& path, const ufbx_scene& scene,
                 ImportedModel& result, std::string& error) {
    std::vector<PendingSurface> pending;
    std::unordered_map<const ufbx_material*, std::size_t> materialSurfaces;
    std::size_t triangleCount = 0;
    std::size_t textureBytes = 0;
    if (scene.anim_stacks.count) {
        result.warnings.emplace_back("FBX animation is not played; the authored static transforms are imported.");
    }
    for (const auto* node : scene.nodes) {
        if (!node->mesh || !node->mesh->num_triangles) continue;
        const auto& mesh = *node->mesh;
        if (mesh.skin_deformers.count || mesh.blend_deformers.count || mesh.cache_deformers.count) {
            error = "FBX skinning, morph targets, and geometry caches are unsupported. Export an evaluated static mesh: " + Text(node->name);
            return false;
        }
        if (mesh.num_triangles > MaxImportedTriangles - triangleCount) {
            error = "FBX exceeds the two-million-triangle import limit after expanding instances.";
            return false;
        }
        triangleCount += mesh.num_triangles;
        const double determinant = ufbx_matrix_determinant(&node->geometry_to_world);
        if (!std::isfinite(determinant) || determinant == 0.0) {
            error = "FBX node has a singular or non-finite transform: " + Text(node->name);
            return false;
        }
        // ufbx includes the determinant sign in its cofactor matrix, producing
        // the direction of inverse-transpose normals even for mirrored nodes.
        const auto normalMatrix = ufbx_matrix_for_normals(&node->geometry_to_world);
        std::vector<std::uint32_t> cornerIndices(mesh.max_face_triangles * 3);
        for (const auto& part : mesh.material_parts) {
            if (!part.num_triangles) continue;
            const auto* material = part.index < node->materials.count ? node->materials.data[part.index] : nullptr;
            auto [it, inserted] = materialSurfaces.emplace(material, pending.size());
            if (inserted) {
                pending.emplace_back();
                if (!PrepareMaterial(path, scene, material, pending.back(), result.warnings, error,
                                     MaxModelFileBytes - textureBytes)) return false;
                const auto bytes = pending.back().surface.baseColor.Pixels().size();
                if (bytes > MaxModelFileBytes - textureBytes) {
                    error = "FBX exceeds the 512 MiB decoded texture memory limit.";
                    return false;
                }
                textureBytes += bytes;
            }
            auto& target = pending[it->second];
            if (mesh.vertex_color.exists && target.surface.editError.empty()) {
                target.surface.editError = "FBX vertex colors are unsupported. Bake vertex colors into Base Color before editing.";
                result.warnings.push_back(target.surface.name + ": " + target.surface.editError);
            }
            const auto* uvs = FindUvs(mesh, target.texture);
            target.hasUvs &= uvs && uvs->exists;
            for (const auto faceIndex : part.face_indices) {
                const auto& face = mesh.faces.data[faceIndex];
                const auto count = ufbx_triangulate_face(cornerIndices.data(), cornerIndices.size(), &mesh, face);
                if (face.num_indices >= 3 && count == 0) {
                    error = "FBX polygon could not be triangulated.";
                    return false;
                }
                for (std::uint32_t triangle = 0; triangle < count; ++triangle) {
                    for (std::uint32_t corner = 0; corner < 3; ++corner) {
                        const auto orderedCorner = determinant < 0.0 && corner > 0 ? 3 - corner : corner;
                        const auto index = cornerIndices[triangle * 3 + orderedCorner];
                        const auto position = ufbx_transform_position(&node->geometry_to_world,
                            ufbx_get_vertex_vec3(&mesh.vertex_position, index));
                        const auto transformedNormal = ufbx_transform_direction(&normalMatrix,
                            ufbx_get_vertex_vec3(&mesh.vertex_normal, index));
                        if (!Finite(position) || !Finite(transformedNormal)) {
                            error = "FBX geometry contains non-finite or excessively large coordinates.";
                            return false;
                        }
                        const auto normal = ufbx_vec3_normalize(transformedNormal);
                        Vertex vertex{Convert(position), Convert(normal), {}};
                        if (uvs && uvs->exists) {
                            const auto uv = ufbx_get_vertex_vec2(uvs, index);
                            auto transformedUv = ufbx_vec3{uv.x, uv.y, 0.0};
                            if (target.texture && target.texture->has_uv_transform) {
                                transformedUv = ufbx_transform_position(&target.texture->uv_to_texture, transformedUv);
                            }
                            if (!Finite(transformedUv)) {
                                error = "FBX texture coordinates or UV transform are non-finite.";
                                return false;
                            }
                            // FBX uses bottom-left UVs; the application and WIC
                            // images use top-left. Flip only after texture transforms.
                            vertex.uv = {static_cast<float>(transformedUv.x), static_cast<float>(1.0 - transformedUv.y)};
                        }
                        target.triangles.push_back(vertex);
                    }
                }
            }
        }
    }
    if (pending.empty()) {
        error = "FBX contains no triangulatable mesh geometry.";
        return false;
    }
    for (auto& target : pending) {
        if (!target.surface.mesh.AssignTriangles(path, std::move(target.triangles), error, false)) return false;
        if (target.surface.editError.empty()) {
            target.surface.editError = ValidateEditableSurface(target.surface.mesh, target.hasUvs, target.surface.appearance);
        }
        result.surfaces.push_back(std::move(target.surface));
    }
    return true;
}

} // namespace

bool LoadFbxModel(const std::filesystem::path& path, ImportedModel& model, std::string& error) {
    error.clear();
    try {
        std::ifstream stream(path, std::ios::binary | std::ios::ate);
        if (!stream) {
            error = "Could not open FBX model.";
            return false;
        }
        const auto length = stream.tellg();
        if (length <= 0 || static_cast<std::uint64_t>(length) > MaxModelFileBytes) {
            error = "FBX is empty or exceeds the 512 MiB file limit.";
            return false;
        }
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
        stream.seekg(0);
        if (!stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
            error = "Could not read the complete FBX model.";
            return false;
        }
        const auto filename = PathKey(path);
        ufbx_load_opts options{};
        options.filename = {filename.data(), filename.size()};
        options.file_format = UFBX_FILE_FORMAT_FBX;
        options.target_axes = ufbx_axes_right_handed_y_up;
        options.target_unit_meters = 1.0;
        options.generate_missing_normals = true;
        options.use_blender_pbr_material = true;
        options.load_external_files = false;
        options.skip_skin_vertices = true;
        options.index_error_handling = UFBX_INDEX_ERROR_HANDLING_ABORT_LOADING;
        options.node_depth_limit = 512;
        options.temp_allocator.memory_limit = MaxModelFileBytes;
        options.result_allocator.memory_limit = MaxModelFileBytes;
        ufbx_error importError{};
        std::unique_ptr<ufbx_scene, decltype(&ufbx_free_scene)> scene(
            ufbx_load_memory(bytes.data(), bytes.size(), &options, &importError), &ufbx_free_scene);
        if (!scene) {
            char description[1024]{};
            ufbx_format_error(description, sizeof(description), &importError);
            error = std::string("FBX import failed: ") + description;
            return false;
        }
        ImportedModel loaded;
        loaded.sourcePath = path;
        if (!ImportScene(path, *scene, loaded, error)) return false;
        model = std::move(loaded);
        return true;
    } catch (const std::exception& exception) {
        error = std::string("FBX import failed: ") + exception.what();
        return false;
    }
}

} // namespace codextex
