#pragma once

#include "core/Types.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace codextex {

class Mesh {
public:
    bool LoadObj(const std::filesystem::path& path, std::string& error);
    bool AssignTriangles(const std::filesystem::path& path, std::vector<Vertex> vertices,
                         std::string& error, bool validateUvs = false);

    [[nodiscard]] const std::vector<Vertex>& Vertices() const noexcept { return vertices_; }
    [[nodiscard]] const std::vector<std::uint32_t>& Indices() const noexcept { return indices_; }
    [[nodiscard]] std::size_t TriangleCount() const noexcept { return indices_.size() / 3; }
    [[nodiscard]] const Vec3& BoundsMin() const noexcept { return boundsMin_; }
    [[nodiscard]] const Vec3& BoundsMax() const noexcept { return boundsMax_; }
    [[nodiscard]] std::size_t UvOverlapCount() const noexcept { return uvOverlapCount_; }
    [[nodiscard]] bool UvOverlapCheckComplete() const noexcept { return uvOverlapCheckComplete_; }
    [[nodiscard]] const std::filesystem::path& SourcePath() const noexcept { return sourcePath_; }

private:
    std::size_t DetectUvOverlaps();

    std::filesystem::path sourcePath_;
    std::vector<Vertex> vertices_;
    std::vector<std::uint32_t> indices_;
    Vec3 boundsMin_{};
    Vec3 boundsMax_{};
    std::size_t uvOverlapCount_{};
    bool uvOverlapCheckComplete_{true};
};

} // namespace codextex
