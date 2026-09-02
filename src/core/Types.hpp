#pragma once

#include <cstdint>

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

} // namespace codextex
