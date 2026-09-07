#include "core/ModelImport.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <Windows.h>
#include <objbase.h>

#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iterator>

namespace {

struct ComScope {
    HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~ComScope() { if (SUCCEEDED(result)) CoUninitialize(); }
};

std::string Base64(const std::vector<std::uint8_t>& bytes) {
    constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    for (std::size_t offset = 0; offset < bytes.size(); offset += 3) {
        const std::uint32_t value = static_cast<std::uint32_t>(bytes[offset]) << 16 |
            (offset + 1 < bytes.size() ? static_cast<std::uint32_t>(bytes[offset + 1]) << 8 : 0) |
            (offset + 2 < bytes.size() ? bytes[offset + 2] : 0);
        result.push_back(alphabet[(value >> 18) & 63]);
        result.push_back(alphabet[(value >> 12) & 63]);
        result.push_back(offset + 1 < bytes.size() ? alphabet[(value >> 6) & 63] : '=');
        result.push_back(offset + 2 < bytes.size() ? alphabet[value & 63] : '=');
    }
    return result;
}

std::filesystem::path WriteFbx(const std::string& name, const std::string& contents) {
    const auto directory = std::filesystem::temp_directory_path() / "codextex-fbx-tests";
    std::filesystem::create_directories(directory);
    const auto path = directory / name;
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << contents;
    return path;
}

std::string FbxFixture(bool mirrored = false, bool tiledUvs = false, bool missingUvs = false,
                       bool textured = false, bool deformed = false) {
    std::ostringstream fixture;
    fixture << R"FBX(; FBX 7.4.0 project file
FBXHeaderExtension: {
    FBXHeaderVersion: 1003
    FBXVersion: 7400
}
GlobalSettings: {
    Version: 1000
    Properties70: {
        P: "UpAxis", "int", "Integer", "", 1
        P: "UpAxisSign", "int", "Integer", "", 1
        P: "FrontAxis", "int", "Integer", "", 2
        P: "FrontAxisSign", "int", "Integer", "", 1
        P: "CoordAxis", "int", "Integer", "", 0
        P: "CoordAxisSign", "int", "Integer", "", 1
        P: "UnitScaleFactor", "double", "Number", "", 1
    }
}
Objects: {
    Model: 100, "Model::Parent", "Null" {
        Version: 232
        Properties70: {
            P: "Lcl Translation", "Lcl Translation", "", "A", 100, 0, 0
        }
    }
    Model: 101, "Model::Quad", "Mesh" {
        Version: 232
        Properties70: {
            P: "Lcl Translation", "Lcl Translation", "", "A", 0, 200, 0
            P: "GeometricTranslation", "Vector3D", "Vector", "", 0, 0, 300
            P: "Lcl Scaling", "Lcl Scaling", "", "A", )FBX";
    fixture << (mirrored ? "-2" : "2") << R"FBX(, 3, 1
        }
    }
    Geometry: 200, "Geometry::Quad", "Mesh" {
        GeometryVersion: 124
        Vertices: *12 {
            a: 0,0,0, 100,0,0, 100,100,0, 0,100,0
        }
        PolygonVertexIndex: *6 {
            a: 0,1,-3, 0,2,-4
        }
        LayerElementMaterial: 0 {
            Version: 101
            Name: ""
            MappingInformationType: "ByPolygon"
            ReferenceInformationType: "IndexToDirect"
            Materials: *2 { a: 0,1 }
        }
)FBX";
    if (!missingUvs) {
        fixture << R"FBX(        LayerElementUV: 0 {
            Version: 101
            Name: "UVMap"
            MappingInformationType: "ByPolygonVertex"
            ReferenceInformationType: "IndexToDirect"
            UV: *8 { a: 0,0, )FBX" << (tiledUvs ? "2" : "1") << R"FBX(,0, 1,1, 0,1 }
            UVIndex: *6 { a: 0,1,2, 0,2,3 }
        }
)FBX";
    }
    fixture << R"FBX(        Layer: 0 {
            Version: 100
            LayerElement: { Type: "LayerElementMaterial" TypedIndex: 0 }
)FBX";
    if (!missingUvs) fixture << "            LayerElement: { Type: \"LayerElementUV\" TypedIndex: 0 }\n";
    fixture << R"FBX(        }
    }
    Material: 300, "Material::Body", "" {
        Version: 102
        ShadingModel: "phong"
        MultiLayer: 0
        Properties70: {
            P: "DiffuseColor", "Color", "", "A", 0.8,0.6,0.4
            P: "DiffuseFactor", "Number", "", "A", 0.5
        }
    }
    Material: 301, "Material::Cloth", "" {
        Version: 102
        ShadingModel: "phong"
        MultiLayer: 0
        Properties70: {
            P: "DiffuseColor", "Color", "", "A", 1,1,1
            P: "DiffuseFactor", "Number", "", "A", 1
        }
    }
)FBX";
    if (textured) {
        fixture << R"FBX(    Texture: 400, "Texture::Shared", "" {
        Type: "TextureVideoClip"
        Version: 202
        TextureName: "Texture::Shared"
        Properties70: {
            P: "UVSet", "KString", "", "", "UVMap"
        }
        FileName: "texture.png"
        RelativeFilename: "texture.png"
        ModelUVTranslation: 0,0
        ModelUVScaling: 1,1
    }
)FBX";
    }
    if (deformed) fixture << "    Deformer: 500, \"Deformer::Morph\", \"BlendShape\" { Version: 100 }\n";
    fixture << R"FBX(}
Connections: {
    C: "OO", 100, 0
    C: "OO", 101, 100
    C: "OO", 200, 101
    C: "OO", 300, 101
    C: "OO", 301, 101
)FBX";
    if (textured) fixture << "    C: \"OP\", 400, 300, \"DiffuseColor\"\n    C: \"OP\", 400, 301, \"DiffuseColor\"\n";
    if (deformed) fixture << "    C: \"OO\", 500, 200\n";
    fixture << "}\n";
    return fixture.str();
}

} // namespace

TEST_CASE("FBX imports hierarchy geometry transforms centimeters and material groups") {
    const auto path = WriteFbx("transforms.fbx", FbxFixture());
    codextex::ImportedModel model;
    std::string error;
    INFO(error);
    REQUIRE(codextex::LoadFbxModel(path, model, error));
    REQUIRE(model.surfaces.size() == 2);
    CHECK(model.sourcePath == path);
    const auto& body = model.surfaces[0];
    CHECK(body.name == "Body");
    CHECK(body.mesh.TriangleCount() == 1);
    CHECK(body.mesh.BoundsMin().x == Catch::Approx(1));
    CHECK(body.mesh.BoundsMax().x == Catch::Approx(3));
    CHECK(body.mesh.BoundsMin().y == Catch::Approx(2));
    CHECK(body.mesh.BoundsMax().y == Catch::Approx(5));
    CHECK(body.mesh.BoundsMin().z == Catch::Approx(3));
    CHECK(body.appearance.baseColorFactor[0] == Catch::Approx(0.4));
    CHECK(body.appearance.baseColorFactor[1] == Catch::Approx(0.3));
    CHECK(body.appearance.baseColorFactor[2] == Catch::Approx(0.2));
    CHECK(body.baseColor.Pixels()[0] == 255);
    CHECK(body.editError.empty());
    CHECK(body.mesh.Vertices()[0].uv.x == Catch::Approx(0));
    CHECK(body.mesh.Vertices()[0].uv.y == Catch::Approx(1));
    CHECK(body.mesh.Vertices()[1].uv.x == Catch::Approx(1));
    CHECK(body.mesh.Vertices()[1].uv.y == Catch::Approx(1));
}

TEST_CASE("FBX mirrored nonuniform scale preserves winding and normal orientation") {
    const auto path = WriteFbx("mirrored.fbx", FbxFixture(true));
    codextex::ImportedModel model;
    std::string error;
    INFO(error);
    REQUIRE(codextex::LoadFbxModel(path, model, error));
    const auto& mesh = model.surfaces[0].mesh;
    CHECK(mesh.BoundsMin().x == Catch::Approx(-1));
    CHECK(mesh.BoundsMax().x == Catch::Approx(1));
    const auto& vertices = mesh.Vertices();
    const auto a = vertices[0].position;
    const auto b = vertices[1].position;
    const auto c = vertices[2].position;
    const auto windingZ = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    CHECK(windingZ > 0);
    for (const auto& vertex : vertices) CHECK(vertex.normal.z == Catch::Approx(1));
}

TEST_CASE("FBX invalid editing UVs remain display-only") {
    for (const bool missing : {false, true}) {
        const auto path = WriteFbx(missing ? "missing_uv.fbx" : "tiled_uv.fbx", FbxFixture(false, !missing, missing));
        codextex::ImportedModel model;
        std::string error;
        INFO(error);
        REQUIRE(codextex::LoadFbxModel(path, model, error));
        REQUIRE(model.surfaces.size() == 2);
        CHECK(model.surfaces[0].editError.find("UV") != std::string::npos);
    }
}

TEST_CASE("FBX authored normals use inverse transpose under nonuniform scaling") {
    auto fixture = FbxFixture(true);
    const auto layer = fixture.find("        Layer: 0 {");
    REQUIRE(layer != std::string::npos);
    fixture.insert(layer, R"FBX(        LayerElementNormal: 0 {
            Version: 101
            Name: ""
            MappingInformationType: "ByPolygonVertex"
            ReferenceInformationType: "Direct"
            Normals: *18 { a: 1,1,1, 1,1,1, 1,1,1, 1,1,1, 1,1,1, 1,1,1 }
        }
)FBX");
    const auto layerBody = fixture.find("        Layer: 0 {\n");
    fixture.insert(layerBody + std::string("        Layer: 0 {\n").size(),
        "            LayerElement: { Type: \"LayerElementNormal\" TypedIndex: 0 }\n");
    const auto path = WriteFbx("authored-normals.fbx", fixture);
    codextex::ImportedModel model;
    std::string error;
    INFO(error);
    REQUIRE(codextex::LoadFbxModel(path, model, error));
    const float length = std::sqrt(0.5f * 0.5f + 1.0f / 9.0f + 1.0f);
    const auto& normal = model.surfaces[0].mesh.Vertices()[0].normal;
    CHECK(normal.x == Catch::Approx(-0.5f / length));
    CHECK(normal.y == Catch::Approx(1.0f / 3.0f / length));
    CHECK(normal.z == Catch::Approx(1.0f / length));
}

TEST_CASE("FBX shared external images retain unmodified pixels and identity") {
    const auto path = WriteFbx("textured.fbx", FbxFixture(false, false, false, true));
    const ComScope com;
    codextex::TextureImage image;
    const std::array<std::uint8_t, 16> pixels{255,0,0,255, 0,255,0,255, 0,0,255,255, 255,255,255,255};
    image.Assign(2, 2, pixels);
    std::string error;
    REQUIRE(image.SavePng(path.parent_path() / "texture.png", error));
    codextex::ImportedModel model;
    INFO(error);
    REQUIRE(codextex::LoadFbxModel(path, model, error));
    REQUIRE(model.surfaces.size() == 2);
    CHECK(model.surfaces[0].baseColor.Width() == 2);
    CHECK(model.surfaces[0].baseColor.Pixels() == image.Pixels());
    CHECK(model.surfaces[0].textureKey == model.surfaces[1].textureKey);
    CHECK(model.surfaces[0].externalTexturePath.filename() == "texture.png");
    CHECK(model.surfaces[0].appearance.baseColorFactor[0] == Catch::Approx(0.4));
}

TEST_CASE("FBX embedded images load without an external image file") {
    const ComScope com;
    const auto temporary = WriteFbx("embedded-source.png", "");
    codextex::TextureImage image;
    const std::array<std::uint8_t, 16> pixels{255,0,0,255, 0,255,0,255, 0,0,255,255, 255,255,255,255};
    image.Assign(2, 2, pixels);
    std::string error;
    REQUIRE(image.SavePng(temporary, error));
    std::ifstream stream(temporary, std::ios::binary);
    const std::vector<std::uint8_t> bytes(std::istreambuf_iterator<char>{stream}, {});
    stream.close();
    auto fixture = FbxFixture(false, false, false, true);
    const auto objectEnd = fixture.find("}\nConnections:");
    REQUIRE(objectEnd != std::string::npos);
    fixture.insert(objectEnd, "    Video: 450, \"Video::Embedded\", \"Clip\" {\n"
        "        Type: \"Clip\"\n        FileName: \"unavailable-embedded-image.png\"\n"
        "        RelativeFilename: \"unavailable-embedded-image.png\"\n"
        "        Content: , \"" + Base64(bytes) + "\"\n    }\n");
    const auto connections = fixture.find("Connections: {");
    fixture.insert(connections + std::string("Connections: {").size(), "\n    C: \"OO\", 450, 400");
    const auto path = WriteFbx("embedded.fbx", fixture);
    codextex::ImportedModel model;
    INFO(error);
    REQUIRE(codextex::LoadFbxModel(path, model, error));
    REQUIRE(model.surfaces.size() == 2);
    CHECK(model.surfaces[0].baseColor.Pixels() == image.Pixels());
    CHECK(model.surfaces[0].externalTexturePath.empty());
    CHECK(model.surfaces[0].textureKey.starts_with("fbx:embedded:"));
    CHECK(model.surfaces[0].textureKey == model.surfaces[1].textureKey);
}

TEST_CASE("FBX failure and unsupported deformation preserve the previous model") {
    const auto valid = WriteFbx("rollback-valid.fbx", FbxFixture());
    const auto corrupt = WriteFbx("rollback-invalid.fbx", "This is not an FBX file");
    const auto deformed = WriteFbx("rollback-morph.fbx", FbxFixture(false, false, false, false, true));
    codextex::ImportedModel model;
    std::string error;
    REQUIRE(codextex::LoadFbxModel(valid, model, error));
    for (const auto& path : {corrupt, deformed}) {
        CHECK_FALSE(codextex::LoadFbxModel(path, model, error));
        CHECK_FALSE(error.empty());
        CHECK(model.sourcePath == valid);
        REQUIRE(model.surfaces.size() == 2);
        CHECK(model.surfaces[0].mesh.TriangleCount() == 1);
    }
}

TEST_CASE("FBX missing Base Color is tracked independently from UV editing restrictions") {
    auto fixture = FbxFixture(false, true, false, true);
    std::size_t position = 0;
    while ((position = fixture.find("texture.png", position)) != std::string::npos) {
        fixture.replace(position, std::string("texture.png").size(), "missing-base-color-for-test.png");
        position += std::string("missing-base-color-for-test.png").size();
    }
    const auto path = WriteFbx("missing-image.fbx", fixture);
    codextex::ImportedModel model;
    std::string error;
    INFO(error);
    REQUIRE(codextex::LoadFbxModel(path, model, error));
    REQUIRE(model.surfaces.size() == 2);
    CHECK(model.surfaces[0].baseColorMissing);
    CHECK(model.surfaces[0].textureKey == model.surfaces[1].textureKey);
    CHECK(model.surfaces[0].editError.find("UV") != std::string::npos);
    CHECK_FALSE(model.surfaces[0].baseColor.Empty());
    CHECK_FALSE(model.warnings.empty());
}

TEST_CASE("FBX vertex color surfaces explicitly restrict texture-only editing") {
    auto fixture = FbxFixture();
    const auto layer = fixture.find("        Layer: 0 {");
    REQUIRE(layer != std::string::npos);
    fixture.insert(layer, R"FBX(        LayerElementColor: 0 {
            Version: 101
            Name: "Color"
            MappingInformationType: "ByPolygonVertex"
            ReferenceInformationType: "Direct"
            Colors: *24 { a: 1,0,0,1, 1,0,0,1, 1,0,0,1, 1,0,0,1, 1,0,0,1, 1,0,0,1 }
        }
)FBX");
    const auto layerBody = fixture.find("        Layer: 0 {\n");
    fixture.insert(layerBody + std::string("        Layer: 0 {\n").size(),
        "            LayerElement: { Type: \"LayerElementColor\" TypedIndex: 0 }\n");
    const auto path = WriteFbx("vertex-colors.fbx", fixture);
    codextex::ImportedModel model;
    std::string error;
    INFO(error);
    REQUIRE(codextex::LoadFbxModel(path, model, error));
    CHECK(model.surfaces[0].editError.find("vertex colors") != std::string::npos);
    CHECK_FALSE(model.warnings.empty());
}
