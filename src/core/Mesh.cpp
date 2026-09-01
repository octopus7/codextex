#include "core/Mesh.hpp"

#include <tiny_obj_loader.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace codextex {
namespace {

std::string ToUtf8(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

Vec3 Sub(const Vec3& a, const Vec3& b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

Vec3 Normalize(const Vec3& v) {
    const float length = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (length < 1.0e-12f) {
        return {0.0f, 1.0f, 0.0f};
    }
    return {v.x / length, v.y / length, v.z / length};
}

Vec3 Cross(const Vec3& a, const Vec3& b) {
    return {
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x,
    };
}

float Orient(const Vec2& a, const Vec2& b, const Vec2& c) {
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

bool StrictPointInTriangle(const Vec2& p, const std::array<Vec2, 3>& t) {
    constexpr float epsilon = 1.0e-7f;
    const float a = Orient(t[0], t[1], p);
    const float b = Orient(t[1], t[2], p);
    const float c = Orient(t[2], t[0], p);
    const bool positive = a > epsilon && b > epsilon && c > epsilon;
    const bool negative = a < -epsilon && b < -epsilon && c < -epsilon;
    return positive || negative;
}

bool ProperSegmentsIntersect(const Vec2& a, const Vec2& b, const Vec2& c, const Vec2& d) {
    constexpr float epsilon = 1.0e-7f;
    const float abC = Orient(a, b, c);
    const float abD = Orient(a, b, d);
    const float cdA = Orient(c, d, a);
    const float cdB = Orient(c, d, b);
    return abC * abD < -epsilon && cdA * cdB < -epsilon;
}

bool TrianglesOverlapWithArea(const std::array<Vec2, 3>& a, const std::array<Vec2, 3>& b) {
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            if (ProperSegmentsIntersect(a[i], a[(i + 1) % 3], b[j], b[(j + 1) % 3])) {
                return true;
            }
        }
    }
    for (const Vec2& point : a) {
        if (StrictPointInTriangle(point, b)) {
            return true;
        }
    }
    for (const Vec2& point : b) {
        if (StrictPointInTriangle(point, a)) {
            return true;
        }
    }
    const Vec2 centroidA{(a[0].x + a[1].x + a[2].x) / 3.0f,
                         (a[0].y + a[1].y + a[2].y) / 3.0f};
    const Vec2 centroidB{(b[0].x + b[1].x + b[2].x) / 3.0f,
                         (b[0].y + b[1].y + b[2].y) / 3.0f};
    return StrictPointInTriangle(centroidA, b) || StrictPointInTriangle(centroidB, a);
}

} // namespace

bool Mesh::LoadObj(const std::filesystem::path& path, std::string& error) {
    tinyobj::ObjReaderConfig config;
    config.triangulate = true;
    config.vertex_color = false;

    tinyobj::ObjReader reader;
    if (!reader.ParseFromFile(ToUtf8(path), config)) {
        error = reader.Error().empty() ? "Failed to parse OBJ." : reader.Error();
        return false;
    }

    const auto& attributes = reader.GetAttrib();
    const auto& shapes = reader.GetShapes();
    if (attributes.vertices.empty() || shapes.empty()) {
        error = "OBJ contains no mesh geometry.";
        return false;
    }
    if (attributes.texcoords.empty()) {
        error = "OBJ has no UV coordinates. CodexTex does not unwrap UVs.";
        return false;
    }

    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;
    Vec3 minBounds{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                   std::numeric_limits<float>::max()};
    Vec3 maxBounds{-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(),
                   -std::numeric_limits<float>::max()};
    std::uint32_t triangleId = 0;

    for (const auto& shape : shapes) {
        std::size_t offset = 0;
        for (const auto faceSize : shape.mesh.num_face_vertices) {
            if (faceSize != 3) {
                error = "OBJ triangulation produced a non-triangle face.";
                return false;
            }

            std::array<Vertex, 3> face{};
            for (std::size_t corner = 0; corner < 3; ++corner) {
                const tinyobj::index_t index = shape.mesh.indices[offset + corner];
                if (index.vertex_index < 0 || index.texcoord_index < 0) {
                    error = "Every OBJ face corner must have a UV coordinate.";
                    return false;
                }
                const std::size_t vi = static_cast<std::size_t>(index.vertex_index) * 3;
                const std::size_t ti = static_cast<std::size_t>(index.texcoord_index) * 2;
                if (vi + 2 >= attributes.vertices.size() || ti + 1 >= attributes.texcoords.size()) {
                    error = "OBJ contains an out-of-range vertex or UV index.";
                    return false;
                }
                face[corner].position = {
                    attributes.vertices[vi], attributes.vertices[vi + 1], attributes.vertices[vi + 2]};
                face[corner].uv = {attributes.texcoords[ti], 1.0f - attributes.texcoords[ti + 1]};
                if (face[corner].uv.x < -1.0e-5f || face[corner].uv.x > 1.00001f ||
                    face[corner].uv.y < -1.0e-5f || face[corner].uv.y > 1.00001f) {
                    error = "OBJ UVs must stay inside the 0..1 atlas for version 1.";
                    return false;
                }
                if (index.normal_index >= 0) {
                    const std::size_t ni = static_cast<std::size_t>(index.normal_index) * 3;
                    if (ni + 2 >= attributes.normals.size()) {
                        error = "OBJ contains an out-of-range normal index.";
                        return false;
                    }
                    face[corner].normal = Normalize({attributes.normals[ni], attributes.normals[ni + 1],
                                                     attributes.normals[ni + 2]});
                }
                face[corner].triangleId = triangleId;
            }
            const Vec3 faceNormal = Normalize(Cross(Sub(face[1].position, face[0].position),
                                                    Sub(face[2].position, face[0].position)));
            for (Vertex& vertex : face) {
                if (std::abs(vertex.normal.x) + std::abs(vertex.normal.y) + std::abs(vertex.normal.z) <
                    1.0e-6f) {
                    vertex.normal = faceNormal;
                }
                minBounds.x = std::min(minBounds.x, vertex.position.x);
                minBounds.y = std::min(minBounds.y, vertex.position.y);
                minBounds.z = std::min(minBounds.z, vertex.position.z);
                maxBounds.x = std::max(maxBounds.x, vertex.position.x);
                maxBounds.y = std::max(maxBounds.y, vertex.position.y);
                maxBounds.z = std::max(maxBounds.z, vertex.position.z);
                indices.push_back(static_cast<std::uint32_t>(vertices.size()));
                vertices.push_back(vertex);
            }
            offset += faceSize;
            ++triangleId;
        }
    }

    if (vertices.empty()) {
        error = "OBJ contains no triangles.";
        return false;
    }

    sourcePath_ = path;
    vertices_ = std::move(vertices);
    indices_ = std::move(indices);
    boundsMin_ = minBounds;
    boundsMax_ = maxBounds;
    uvOverlapCount_ = DetectUvOverlaps();
    error = reader.Warning();
    return true;
}

std::size_t Mesh::DetectUvOverlaps() const {
    constexpr int gridSize = 64;
    constexpr std::size_t maxCandidatePairs = 2'000'000;
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> cells;
    cells.reserve(TriangleCount() * 2);

    for (std::uint32_t triangle = 0; triangle < TriangleCount(); ++triangle) {
        const Vec2 a = vertices_[indices_[triangle * 3]].uv;
        const Vec2 b = vertices_[indices_[triangle * 3 + 1]].uv;
        const Vec2 c = vertices_[indices_[triangle * 3 + 2]].uv;
        const float minX = std::min({a.x, b.x, c.x});
        const float maxX = std::max({a.x, b.x, c.x});
        const float minY = std::min({a.y, b.y, c.y});
        const float maxY = std::max({a.y, b.y, c.y});
        const int x0 = std::clamp(static_cast<int>(std::floor(minX * gridSize)), 0, gridSize - 1);
        const int x1 = std::clamp(static_cast<int>(std::floor(maxX * gridSize)), 0, gridSize - 1);
        const int y0 = std::clamp(static_cast<int>(std::floor(minY * gridSize)), 0, gridSize - 1);
        const int y1 = std::clamp(static_cast<int>(std::floor(maxY * gridSize)), 0, gridSize - 1);
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                cells[static_cast<std::uint32_t>(y * gridSize + x)].push_back(triangle);
            }
        }
    }

    std::unordered_set<std::uint64_t> visited;
    std::size_t overlaps = 0;
    for (const auto& [cell, triangles] : cells) {
        (void)cell;
        for (std::size_t i = 0; i < triangles.size(); ++i) {
            for (std::size_t j = i + 1; j < triangles.size(); ++j) {
                const std::uint32_t low = std::min(triangles[i], triangles[j]);
                const std::uint32_t high = std::max(triangles[i], triangles[j]);
                const std::uint64_t key = (static_cast<std::uint64_t>(low) << 32) | high;
                if (!visited.insert(key).second) {
                    continue;
                }
                if (visited.size() > maxCandidatePairs) {
                    return overlaps;
                }
                std::array<Vec2, 3> lhs{};
                std::array<Vec2, 3> rhs{};
                for (std::size_t k = 0; k < 3; ++k) {
                    lhs[k] = vertices_[indices_[low * 3 + k]].uv;
                    rhs[k] = vertices_[indices_[high * 3 + k]].uv;
                }
                if (TrianglesOverlapWithArea(lhs, rhs)) {
                    ++overlaps;
                }
            }
        }
    }
    return overlaps;
}

} // namespace codextex
