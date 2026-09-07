#include "core/ModelImport.hpp"

#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#define CGLTF_IMPLEMENTATION
#include "../../third_party/cgltf/cgltf.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace codextex {
namespace {

using Bytes = std::vector<std::uint8_t>;

struct ParserBudget { std::size_t used{}; };
struct alignas(std::max_align_t) Allocation { std::size_t size{}; };

void* ParserAllocate(void* user, cgltf_size size) {
    auto& budget = *static_cast<ParserBudget*>(user);
    if (size > MaxModelFileBytes - sizeof(Allocation) ||
        size + sizeof(Allocation) > MaxModelFileBytes - budget.used) return nullptr;
    auto* allocation = static_cast<Allocation*>(std::malloc(size + sizeof(Allocation)));
    if (!allocation) return nullptr;
    allocation->size = size + sizeof(Allocation);
    budget.used += allocation->size;
    return allocation + 1;
}

void ParserFree(void* user, void* pointer) {
    if (!pointer) return;
    auto& budget = *static_cast<ParserBudget*>(user);
    auto* allocation = static_cast<Allocation*>(pointer) - 1;
    budget.used -= allocation->size;
    std::free(allocation);
}

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error("GLB: " + message);
}

Bytes ReadFile(const std::filesystem::path& path, std::size_t limit = MaxModelFileBytes) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    const auto utf8Path = path.generic_u8string();
    Require(static_cast<bool>(stream), "cannot open file: " + std::string(utf8Path.begin(), utf8Path.end()));
    const auto length = stream.tellg();
    Require(length >= 0 && static_cast<std::uint64_t>(length) <= limit,
            "file exceeds the 512 MiB resource limit.");
    Bytes bytes(static_cast<std::size_t>(length));
    stream.seekg(0);
    if (!bytes.empty()) stream.read(reinterpret_cast<char*>(bytes.data()), length);
    Require(static_cast<bool>(stream), "could not read the complete file.");
    return bytes;
}

int Hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::filesystem::path ResolveUri(const std::filesystem::path& model, std::string_view uri) {
    std::string decoded;
    for (std::size_t i = 0; i < uri.size(); ++i) {
        char c = uri[i];
        if (c == '%') {
            Require(i + 2 < uri.size() && Hex(uri[i + 1]) >= 0 && Hex(uri[i + 2]) >= 0,
                    "invalid percent encoding in resource URI.");
            c = static_cast<char>(Hex(uri[i + 1]) * 16 + Hex(uri[i + 2]));
            i += 2;
        }
        Require(c != '\0', "resource URI contains a null character.");
        decoded.push_back(c);
    }
    const auto relative = std::filesystem::path(std::u8string(decoded.begin(), decoded.end()));
    Require(!relative.is_absolute() && !relative.has_root_name() &&
            decoded.find(':') == std::string::npos && !decoded.starts_with('\\') &&
            !decoded.starts_with('/'), "only local relative resource URIs are supported.");
    return (model.parent_path() / relative).lexically_normal();
}

Bytes DecodeDataUri(std::string_view uri, std::size_t limit) {
    const auto comma = uri.find(',');
    Require(comma != std::string_view::npos && uri.substr(0, comma).ends_with(";base64"),
            "only base64 data URIs are supported.");
    const auto encoded = uri.substr(comma + 1);
    Require(encoded.size() % 4 == 0 && encoded.size() / 4 <= (limit + 2) / 3,
            "invalid or oversized base64 resource.");
    Bytes result;
    result.reserve(encoded.size() / 4 * 3);
    const auto digit = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    for (std::size_t i = 0; i < encoded.size(); i += 4) {
        const int a = digit(encoded[i]), b = digit(encoded[i + 1]);
        const int c = encoded[i + 2] == '=' ? 0 : digit(encoded[i + 2]);
        const int d = encoded[i + 3] == '=' ? 0 : digit(encoded[i + 3]);
        const bool pad2 = encoded[i + 2] == '=', pad1 = encoded[i + 3] == '=';
        Require(a >= 0 && b >= 0 && c >= 0 && d >= 0 && (!pad2 || pad1) &&
                (!(pad1 || pad2) || i + 4 == encoded.size()), "invalid base64 resource.");
        result.push_back(static_cast<std::uint8_t>((a << 2) | (b >> 4)));
        if (!pad2) result.push_back(static_cast<std::uint8_t>((b << 4) | (c >> 2)));
        if (!pad1) result.push_back(static_cast<std::uint8_t>((c << 6) | d));
    }
    Require(result.size() <= limit, "resource exceeds the memory limit.");
    return result;
}

bool RangeFits(std::size_t length, std::size_t offset, std::size_t count,
               std::size_t stride, std::size_t element) {
    return count > 0 && element > 0 && stride >= element && offset <= length &&
           element <= length - offset && count - 1 <= (length - offset - element) / stride;
}

void ValidateRanges(cgltf_data& data) {
    for (std::size_t i = 0; i < data.buffer_views_count; ++i) {
        const auto& view = data.buffer_views[i];
        Require(!view.has_meshopt_compression,
                "EXT_meshopt_compression / KHR_meshopt_compression is unsupported; export uncompressed GLB.");
        Require(view.buffer && view.offset <= view.buffer->size &&
                view.size <= view.buffer->size - view.offset, "buffer view is out of bounds.");
    }
    for (std::size_t i = 0; i < data.accessors_count; ++i) {
        const auto& a = data.accessors[i];
        const auto element = cgltf_calc_size(a.type, a.component_type);
        Require(a.count > 0 && a.count <= MaxModelFileBytes && element > 0,
                "invalid accessor type or count.");
        if (a.buffer_view) {
            Require(RangeFits(a.buffer_view->size, a.offset, a.count, a.stride, element),
                    "accessor byte range is out of bounds.");
        }
        if (a.is_sparse) {
            const auto& s = a.sparse;
            const auto indexSize = cgltf_component_size(s.indices_component_type);
            Require(s.indices_component_type == cgltf_component_type_r_8u ||
                    s.indices_component_type == cgltf_component_type_r_16u ||
                    s.indices_component_type == cgltf_component_type_r_32u,
                    "invalid sparse index component type.");
            Require(s.count <= a.count && s.indices_buffer_view && s.values_buffer_view &&
                    RangeFits(s.indices_buffer_view->size, s.indices_byte_offset, s.count, indexSize, indexSize) &&
                    RangeFits(s.values_buffer_view->size, s.values_byte_offset, s.count, element, element),
                    "sparse accessor byte range is out of bounds.");
            cgltf_accessor indices{};
            indices.type = cgltf_type_scalar;
            indices.component_type = s.indices_component_type;
            indices.buffer_view = s.indices_buffer_view;
            indices.offset = s.indices_byte_offset;
            indices.stride = indexSize;
            std::size_t previous = 0;
            for (std::size_t j = 0; j < s.count; ++j) {
                const auto index = cgltf_accessor_read_index(&indices, j);
                Require(index < a.count && (j == 0 || index > previous),
                        "sparse indices must be increasing and in bounds.");
                previous = index;
            }
        }
    }
}

void ReadFloats(const cgltf_accessor& accessor, std::size_t index, float* output, std::size_t count) {
    // cgltf's sparse reader uses the base stride for sparse values. glTF sparse
    // values are tightly packed even when the base buffer view is interleaved.
    auto base = accessor;
    base.is_sparse = false;
    Require(index < accessor.count && cgltf_accessor_read_float(&base, index, output, count),
            "cannot read vertex attribute.");
    if (accessor.is_sparse) {
        const auto& s = accessor.sparse;
        cgltf_accessor indices{};
        indices.type = cgltf_type_scalar;
        indices.component_type = s.indices_component_type;
        indices.buffer_view = s.indices_buffer_view;
        indices.offset = s.indices_byte_offset;
        indices.stride = cgltf_component_size(s.indices_component_type);
        std::size_t lo = 0, hi = s.count;
        while (lo < hi) {
            const auto mid = lo + (hi - lo) / 2;
            if (cgltf_accessor_read_index(&indices, mid) < index) lo = mid + 1;
            else hi = mid;
        }
        if (lo < s.count && cgltf_accessor_read_index(&indices, lo) == index) {
            base.buffer_view = s.values_buffer_view;
            base.offset = s.values_byte_offset;
            base.stride = cgltf_calc_size(base.type, base.component_type);
            Require(cgltf_accessor_read_float(&base, lo, output, count), "cannot read sparse attribute.");
        }
    }
    for (std::size_t i = 0; i < count; ++i)
        Require(std::isfinite(output[i]), "vertex attribute contains a non-finite value.");
}

Vec3 Cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

Vec3 Normalize(Vec3 v) {
    const double length = std::sqrt(double(v.x) * v.x + double(v.y) * v.y + double(v.z) * v.z);
    Require(std::isfinite(length), "transformed normal is non-finite.");
    if (length < 1e-20) return {0, 0, 1};
    return {static_cast<float>(v.x / length), static_cast<float>(v.y / length), static_cast<float>(v.z / length)};
}

TextureWrap Wrap(cgltf_wrap_mode mode) {
    if (mode == cgltf_wrap_mode_clamp_to_edge) return TextureWrap::Clamp;
    if (mode == cgltf_wrap_mode_mirrored_repeat) return TextureWrap::Mirror;
    return TextureWrap::Repeat;
}

struct SurfaceBuilder {
    const cgltf_material* material{};
    std::vector<Vertex> vertices;
    bool hasUvs{true};
    bool hasVertexColors{};
};

const cgltf_texture_view* BaseTexture(const cgltf_material* material) {
    return material && material->has_pbr_metallic_roughness ?
        &material->pbr_metallic_roughness.base_color_texture : nullptr;
}

void AppendPrimitive(const cgltf_node& node, const cgltf_primitive& primitive,
                     SurfaceBuilder& surface, std::size_t& totalTriangles) {
    Require(!node.skin, "skinned meshes are unsupported; export a mesh with the desired pose applied.");
    Require(!node.has_mesh_gpu_instancing, "EXT_mesh_gpu_instancing is unsupported; realize instances before export.");
    Require(primitive.targets_count == 0, "morph targets are unsupported; apply the desired shape before export.");
    Require(!primitive.has_draco_mesh_compression,
            "KHR_draco_mesh_compression is unsupported; export uncompressed GLB.");
    Require(primitive.type == cgltf_primitive_type_triangles ||
            primitive.type == cgltf_primitive_type_triangle_strip ||
            primitive.type == cgltf_primitive_type_triangle_fan,
            "only triangle meshes are supported; convert points and lines before export.");
    const auto* positions = cgltf_find_accessor(&primitive, cgltf_attribute_type_position, 0);
    const auto* normals = cgltf_find_accessor(&primitive, cgltf_attribute_type_normal, 0);
    const auto* texture = BaseTexture(primitive.material);
    int uvIndex = texture ? texture->texcoord : 0;
    if (texture && texture->has_transform && texture->transform.has_texcoord)
        uvIndex = texture->transform.texcoord;
    const auto* uvs = cgltf_find_accessor(&primitive, cgltf_attribute_type_texcoord, uvIndex);
    Require(positions && positions->type == cgltf_type_vec3, "mesh is missing a VEC3 POSITION attribute.");
    Require(!normals || normals->type == cgltf_type_vec3, "NORMAL attribute must be VEC3.");
    Require(!uvs || uvs->type == cgltf_type_vec2, "TEXCOORD attribute must be VEC2.");
    surface.hasUvs &= uvs != nullptr;
    surface.hasVertexColors |= cgltf_find_accessor(&primitive, cgltf_attribute_type_color, 0) != nullptr;

    const auto count = primitive.indices ? primitive.indices->count : positions->count;
    Require(count >= 3 && (primitive.type != cgltf_primitive_type_triangles || count % 3 == 0),
            "invalid triangle index count.");
    const auto triangles = primitive.type == cgltf_primitive_type_triangles ? count / 3 : count - 2;
    Require(triangles <= MaxImportedTriangles - totalTriangles, "model exceeds the 2,000,000 triangle limit.");
    totalTriangles += triangles;
    surface.vertices.reserve(surface.vertices.size() + triangles * 3);
    float m[16];
    cgltf_node_transform_world(&node, m);
    for (float value : m) Require(std::isfinite(value), "node transform is non-finite.");
    Require(m[3] == 0 && m[7] == 0 && m[11] == 0 && m[15] == 1,
            "node matrix is not an affine transform.");
    const Vec3 c0 = Cross({m[4], m[5], m[6]}, {m[8], m[9], m[10]});
    const Vec3 c1 = Cross({m[8], m[9], m[10]}, {m[0], m[1], m[2]});
    const Vec3 c2 = Cross({m[0], m[1], m[2]}, {m[4], m[5], m[6]});
    const double determinant = double(m[0]) * c0.x + double(m[1]) * c0.y + double(m[2]) * c0.z;
    Require(std::isfinite(determinant) && std::abs(determinant) > 1e-20,
            "node has a singular or invalid transform; apply a non-zero scale before export.");
    const float sign = determinant < 0 ? -1.0f : 1.0f;

    for (std::size_t triangle = 0; triangle < triangles; ++triangle) {
        std::size_t corners[3];
        if (primitive.type == cgltf_primitive_type_triangles) {
            corners[0] = triangle * 3; corners[1] = triangle * 3 + 1; corners[2] = triangle * 3 + 2;
        } else if (primitive.type == cgltf_primitive_type_triangle_strip) {
            corners[0] = triangle; corners[1] = triangle + 1; corners[2] = triangle + 2;
            if (triangle % 2 != 0) std::swap(corners[0], corners[1]);
        } else {
            corners[0] = 0; corners[1] = triangle + 1; corners[2] = triangle + 2;
        }
        if (determinant < 0) std::swap(corners[1], corners[2]);
        Vertex vertices[3]{};
        for (std::size_t corner = 0; corner < 3; ++corner) {
            const auto index = primitive.indices ? cgltf_accessor_read_index(primitive.indices, corners[corner]) : corners[corner];
            Require(index < positions->count, "triangle index is outside POSITION bounds.");
            float p[3];
            ReadFloats(*positions, index, p, 3);
            auto& vertex = vertices[corner];
            vertex.position = {m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12],
                               m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13],
                               m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14]};
            Require(std::isfinite(vertex.position.x) && std::isfinite(vertex.position.y) &&
                    std::isfinite(vertex.position.z), "transformed position is non-finite.");
            if (normals) {
                float n[3];
                ReadFloats(*normals, index, n, 3);
                vertex.normal = Normalize({sign * (c0.x * n[0] + c1.x * n[1] + c2.x * n[2]),
                                           sign * (c0.y * n[0] + c1.y * n[1] + c2.y * n[2]),
                                           sign * (c0.z * n[0] + c1.z * n[1] + c2.z * n[2])});
            }
            if (uvs) {
                float uv[2];
                ReadFloats(*uvs, index, uv, 2);
                if (texture && texture->has_transform) {
                    const auto& t = texture->transform;
                    const float u = uv[0] * t.scale[0], v = uv[1] * t.scale[1];
                    uv[0] = t.offset[0] + std::cos(t.rotation) * u - std::sin(t.rotation) * v;
                    uv[1] = t.offset[1] + std::sin(t.rotation) * u + std::cos(t.rotation) * v;
                }
                Require(std::isfinite(uv[0]) && std::isfinite(uv[1]), "transformed UV is non-finite.");
                vertex.uv = {uv[0], uv[1]};
            }
        }
        if (!normals) {
            const auto& a = vertices[0].position;
            const auto& b = vertices[1].position;
            const auto& c = vertices[2].position;
            const auto normal = Normalize(Cross({b.x - a.x, b.y - a.y, b.z - a.z},
                                                {c.x - a.x, c.y - a.y, c.z - a.z}));
            for (auto& vertex : vertices) vertex.normal = normal;
        }
        surface.vertices.insert(surface.vertices.end(), std::begin(vertices), std::end(vertices));
    }
}

void ApplyMaterial(const cgltf_material* material, ModelSurface& surface) {
    if (!material) return;
    surface.appearance.doubleSided = material->double_sided != 0;
    surface.appearance.alphaCutoff = material->alpha_cutoff;
    if (material->alpha_mode == cgltf_alpha_mode_mask) surface.appearance.alphaMode = MaterialAlphaMode::Mask;
    if (material->alpha_mode == cgltf_alpha_mode_blend) surface.appearance.alphaMode = MaterialAlphaMode::Blend;
    if (material->has_pbr_metallic_roughness)
        std::copy_n(material->pbr_metallic_roughness.base_color_factor, 4, surface.appearance.baseColorFactor.begin());
    for (float factor : surface.appearance.baseColorFactor)
        Require(std::isfinite(factor) && factor >= 0 && factor <= 1, "invalid base color factor.");
    Require(std::isfinite(surface.appearance.alphaCutoff), "invalid alpha cutoff.");
    const auto* texture = BaseTexture(material);
    if (texture && texture->texture && texture->texture->sampler) {
        surface.appearance.wrapU = Wrap(texture->texture->sampler->wrap_s);
        surface.appearance.wrapV = Wrap(texture->texture->sampler->wrap_t);
    }
}

} // namespace

bool LoadGlbModel(const std::filesystem::path& path, ImportedModel& model, std::string& error) {
    try {
        const Bytes file = ReadFile(path);
        Require(file.size() >= 20, "file is too short for a GLB 2.0 header.");
        ParserBudget parserBudget;
        cgltf_options options{};
        options.type = cgltf_file_type_glb;
        options.memory = {ParserAllocate, ParserFree, &parserBudget};
        cgltf_data* parsed = nullptr;
        const auto parsedResult = cgltf_parse(&options, file.data(), file.size(), &parsed);
        std::unique_ptr<cgltf_data, decltype(&cgltf_free)> data(parsed, cgltf_free);
        Require(parsedResult != cgltf_result_out_of_memory, "parser exceeds the 512 MiB memory limit.");
        Require(parsedResult == cgltf_result_success && data, "invalid GLB 2.0 container or JSON.");
        Require(data->file_type == cgltf_file_type_glb && data->asset.version &&
                std::string_view(data->asset.version) == "2.0", "only GLB 2.0 is supported.");
        Require(!data->asset.min_version || std::string_view(data->asset.min_version) == "2.0",
                "asset requires a newer glTF version.");
        Require(data->nodes_count <= 100'000 && data->materials_count <= 100'000,
                "model contains too many nodes or materials.");
        for (std::size_t i = 0; i < data->extensions_required_count; ++i) {
            const std::string extension = data->extensions_required[i];
            Require(extension == "KHR_texture_transform" || extension == "KHR_mesh_quantization" ||
                    extension == "KHR_materials_unlit",
                    "unsupported required extension " + extension + "; export without this extension.");
        }

        std::vector<Bytes> buffers(data->buffers_count);
        std::size_t bufferBytes = 0;
        for (std::size_t i = 0; i < data->buffers_count; ++i) {
            auto& buffer = data->buffers[i];
            Require(buffer.size <= MaxModelFileBytes - bufferBytes, "buffers exceed the 512 MiB memory limit.");
            if (!buffer.uri) {
                Require(i == 0 && data->bin && buffer.size <= data->bin_size, "missing or truncated binary buffer.");
                buffer.data = const_cast<void*>(data->bin);
                bufferBytes += buffer.size;
            } else {
                const std::string_view uri(buffer.uri);
                buffers[i] = uri.starts_with("data:") ? DecodeDataUri(uri, MaxModelFileBytes - bufferBytes) :
                             ReadFile(ResolveUri(path, uri), MaxModelFileBytes - bufferBytes);
                Require(buffers[i].size() >= buffer.size, "external buffer is shorter than its declared size.");
                bufferBytes += buffers[i].size();
                buffer.data = buffers[i].data();
            }
            buffer.data_free_method = cgltf_data_free_method_none;
        }
        ValidateRanges(*data);
        Require(cgltf_validate(data.get()) == cgltf_result_success, "invalid scene, accessor, or triangle index.");

        ImportedModel candidate;
        candidate.sourcePath = path;
        if (data->animations_count) candidate.warnings.emplace_back("GLB animations are not played; the authored static node transforms are used.");
        candidate.warnings.emplace_back("GLB preview uses Base Color; lighting and metallic/roughness material effects are not evaluated.");
        std::vector<const cgltf_node*> pending;
        const auto* scene = data->scene ? data->scene : (data->scenes_count ? &data->scenes[0] : nullptr);
        if (scene) {
            pending.assign(scene->nodes, scene->nodes + scene->nodes_count);
        } else {
            for (std::size_t i = 0; i < data->nodes_count; ++i)
                if (!data->nodes[i].parent) pending.push_back(&data->nodes[i]);
        }
        std::vector<bool> visited(data->nodes_count);
        std::vector<SurfaceBuilder> builders;
        std::unordered_map<const cgltf_material*, std::size_t> byMaterial;
        std::size_t totalTriangles = 0;
        for (std::size_t next = 0; next < pending.size(); ++next) {
            const auto* node = pending[next];
            const auto index = static_cast<std::size_t>(node - data->nodes);
            Require(!visited[index], "scene contains a duplicate node or cycle.");
            visited[index] = true;
            for (std::size_t i = 0; i < node->children_count; ++i) pending.push_back(node->children[i]);
            if (!node->mesh) continue;
            for (std::size_t i = 0; i < node->mesh->primitives_count; ++i) {
                const auto& primitive = node->mesh->primitives[i];
                auto [it, inserted] = byMaterial.try_emplace(primitive.material, builders.size());
                if (inserted) builders.push_back({primitive.material});
                AppendPrimitive(*node, primitive, builders[it->second], totalTriangles);
            }
        }
        Require(totalTriangles > 0, "selected scene contains no triangle meshes.");

        struct CachedImage { std::size_t surfaceIndex{}; std::filesystem::path external; std::string warning; };
        std::unordered_map<const cgltf_image*, CachedImage> imageCache;
        std::size_t textureBytes = 0;
        for (auto& builder : builders) {
            ModelSurface surface;
            surface.name = builder.material && builder.material->name ? builder.material->name :
                           "Material " + std::to_string(candidate.surfaces.size() + 1);
            Require(surface.mesh.AssignTriangles(path, std::move(builder.vertices), error), error);
            ApplyMaterial(builder.material, surface);
            surface.editError = ValidateEditableSurface(surface.mesh, builder.hasUvs, surface.appearance);
            if (builder.hasVertexColors) {
                candidate.warnings.push_back(surface.name + ": vertex colors are not displayed; texture editing is disabled.");
                surface.editError = "Vertex colors are unsupported; bake them into Base Color before editing.";
            }
            if (builder.material && builder.material->has_pbr_specular_glossiness) {
                candidate.warnings.push_back(surface.name + ": specular/glossiness material is unsupported.");
                surface.editError = "Convert the specular/glossiness material to metallic/roughness Base Color before editing.";
            }
            const auto* view = BaseTexture(builder.material);
            const auto* texture = view ? view->texture : nullptr;
            const auto* image = texture ? texture->image : nullptr;
            if (texture && !image) {
                candidate.warnings.push_back(surface.name + ": texture has no supported PNG/JPEG source; a white fallback is used.");
                surface.baseColorMissing = true;
            }
            if (image) {
                auto [it, inserted] = imageCache.try_emplace(image);
                auto& cached = it->second;
                if (inserted) {
                    cached.surfaceIndex = candidate.surfaces.size();
                    try {
                        Bytes encoded;
                        if (image->buffer_view) {
                            const auto* start = cgltf_buffer_view_data(image->buffer_view);
                            Require(start != nullptr, "image buffer is missing.");
                            Require(image->buffer_view->size <= 128ull * 1024 * 1024,
                                    "encoded image exceeds the 128 MiB limit.");
                            encoded.assign(start, start + image->buffer_view->size);
                        } else if (image->uri) {
                            const std::string_view uri(image->uri);
                            if (uri.starts_with("data:")) encoded = DecodeDataUri(uri, 128ull * 1024 * 1024);
                            else {
                                cached.external = ResolveUri(path, uri);
                                encoded = ReadFile(cached.external, 128ull * 1024 * 1024);
                            }
                        } else {
                            throw std::runtime_error("image has no URI or buffer view.");
                        }
                        std::string decodeError;
                        Require(surface.baseColor.LoadEncoded(encoded, decodeError, MaxModelFileBytes - textureBytes), decodeError);
                    } catch (const std::exception& failure) {
                        cached.warning = failure.what();
                        Require(1024ull * 1024 * 4 <= MaxModelFileBytes - textureBytes,
                                "material textures exceed the 512 MiB memory limit.");
                        MakeFallbackTexture(surface.baseColor);
                    }
                } else {
                    const auto& original = candidate.surfaces[cached.surfaceIndex].baseColor;
                    Require(original.Pixels().size() <= MaxModelFileBytes - textureBytes,
                            "decoded material textures exceed the 512 MiB memory limit.");
                    surface.baseColor = original;
                }
                surface.externalTexturePath = cached.external;
                surface.textureKey = "glb-image:" + std::to_string(image - data->images);
                if (!cached.warning.empty()) {
                    candidate.warnings.push_back(surface.name + ": " + cached.warning + " A white fallback is used.");
                    surface.baseColorMissing = true;
                }
            } else {
                Require(1024ull * 1024 * 4 <= MaxModelFileBytes - textureBytes,
                        "material textures exceed the 512 MiB memory limit.");
                MakeFallbackTexture(surface.baseColor);
                surface.textureKey = "glb-material:" + std::to_string(candidate.surfaces.size());
            }
            Require(surface.baseColor.Pixels().size() <= MaxModelFileBytes - textureBytes,
                    "decoded material textures exceed the 512 MiB memory limit.");
            textureBytes += surface.baseColor.Pixels().size();
            candidate.surfaces.push_back(std::move(surface));
        }
        model = std::move(candidate);
        error.clear();
        return true;
    } catch (const std::exception& failure) {
        error = failure.what();
        return false;
    }
}

} // namespace codextex
