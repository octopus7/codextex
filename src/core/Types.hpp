#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace codextex {

struct Vec2 {
    float x{};
    float y{};
};

struct Vec3 {
    float x{};
    float y{};
    float z{};
};

struct Vertex {
    Vec3 position;
    Vec3 normal;
    Vec2 uv;
    std::uint32_t triangleId{};
};

struct MaskPolygon {
    enum class Operation { Include, Exclude };
    std::vector<Vec2> normalizedPoints;
    Operation operation{Operation::Include};
};

struct MaskProposal {
    std::vector<MaskPolygon> polygons;
    float confidence{};
    int suggestedFeatherPx{16};
    std::string rationale;
};

} // namespace codextex
