#include "core/Mesh.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <filesystem>
#include <fstream>

namespace {

std::filesystem::path WriteObj(const std::string& name, const std::string& contents) {
    const auto directory = std::filesystem::temp_directory_path() / "codextex-tests";
    std::filesystem::create_directories(directory);
    const auto path = directory / name;
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << contents;
    return path;
}

} // namespace

TEST_CASE("OBJ loader accepts a single UV atlas and triangulates faces") {
    const auto path = WriteObj("quad.obj", R"OBJ(
v -1 -1 0
v  1 -1 0
v  1  1 0
v -1  1 0
vt 0 0
vt 1 0
vt 1 1
vt 0 1
f 1/1 2/2 3/3 4/4
)OBJ");

    codextex::Mesh mesh;
    std::string error;
    REQUIRE(mesh.LoadObj(path, error));
    CHECK(mesh.TriangleCount() == 2);
    CHECK(mesh.Vertices().size() == 6);
    CHECK(mesh.UvOverlapCount() == 0);
}

TEST_CASE("OBJ loader rejects missing UVs") {
    const auto path = WriteObj("missing-uv.obj", "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
    codextex::Mesh mesh;
    std::string error;
    CHECK_FALSE(mesh.LoadObj(path, error));
    CHECK(error.find("UV") != std::string::npos);
}

TEST_CASE("OBJ loader rejects UVs outside the version-one atlas") {
    const auto path = WriteObj("outside-uv.obj", R"OBJ(
v 0 0 0
v 1 0 0
v 0 1 0
vt 0 0
vt 1.2 0
vt 0 1
f 1/1 2/2 3/3
)OBJ");
    codextex::Mesh mesh;
    std::string error;
    CHECK_FALSE(mesh.LoadObj(path, error));
    CHECK(error.find("0..1") != std::string::npos);
}

TEST_CASE("OBJ loader computes normals when they are omitted") {
    const auto path = WriteObj("computed-normal.obj", R"OBJ(
v 0 0 0
v 1 0 0
v 0 1 0
vt 0 0
vt 1 0
vt 0 1
f 1/1 2/2 3/3
)OBJ");
    codextex::Mesh mesh;
    std::string error;
    REQUIRE(mesh.LoadObj(path, error));
    for (const auto& vertex : mesh.Vertices()) {
        CHECK(vertex.normal.z == Catch::Approx(1.0f));
    }
}

TEST_CASE("OBJ loader reports UV triangles that overlap with area") {
    const auto path = WriteObj("overlap.obj", R"OBJ(
v 0 0 0
v 1 0 0
v 0 1 0
v 0 0 1
v 1 0 1
v 0 1 1
vt 0 0
vt 1 0
vt 0 1
f 1/1 2/2 3/3
f 4/1 5/2 6/3
)OBJ");

    codextex::Mesh mesh;
    std::string error;
    REQUIRE(mesh.LoadObj(path, error));
    CHECK(mesh.UvOverlapCount() == 1);
}
