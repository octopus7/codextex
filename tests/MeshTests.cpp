#include "core/Mesh.hpp"
#include "core/ModelImport.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <filesystem>
#include <fstream>
#include <limits>

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

TEST_CASE("Imported triangles receive stable IDs and invalid replacement preserves the mesh") {
    std::vector<codextex::Vertex> vertices{
        {{0, 0, 0}, {}, {0, 0}, 999}, {{1, 0, 0}, {}, {1, 0}, 999},
        {{0, 1, 0}, {}, {0, 1}, 999}, {{0, 0, 1}, {}, {0, 0}, 999},
        {{1, 0, 1}, {}, {1, 0}, 999}, {{0, 1, 1}, {}, {0, 1}, 999}};
    codextex::Mesh mesh;
    std::string error;
    REQUIRE(mesh.AssignTriangles("original.glb", vertices, error, true));
    REQUIRE(mesh.TriangleCount() == 2);
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        CHECK(mesh.Indices()[i] == i);
        CHECK(mesh.Vertices()[i].triangleId == i / 3);
        CHECK(mesh.Vertices()[i].normal.z == Catch::Approx(1.0f));
    }
    vertices[0].position.x = std::numeric_limits<float>::quiet_NaN();
    CHECK_FALSE(mesh.AssignTriangles("invalid.fbx", vertices, error));
    CHECK(mesh.SourcePath() == "original.glb");
    CHECK(mesh.TriangleCount() == 2);
}

TEST_CASE("Imported material edit eligibility preserves authored UV limitations") {
    std::vector<codextex::Vertex> vertices{
        {{0, 0, 0}, {}, {0, 0}}, {{1, 0, 0}, {}, {2, 0}}, {{0, 1, 0}, {}, {0, 1}}};
    codextex::Mesh mesh;
    std::string error;
    REQUIRE(mesh.AssignTriangles("tiled.glb", vertices, error));
    codextex::MaterialAppearance appearance;
    CHECK(codextex::ValidateEditableSurface(mesh, true, appearance).find("0..1") != std::string::npos);
    CHECK(codextex::ValidateEditableSurface(mesh, false, appearance).find("no UV") != std::string::npos);
    vertices[1].uv.x = 1;
    REQUIRE(mesh.AssignTriangles("atlas.glb", vertices, error));
    CHECK(codextex::ValidateEditableSurface(mesh, true, appearance).empty());
    appearance.alphaMode = codextex::MaterialAlphaMode::Blend;
    CHECK_FALSE(codextex::ValidateEditableSurface(mesh, true, appearance).empty());
}

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
