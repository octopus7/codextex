#include "core/ModelImport.hpp"
#include "graphics/Renderer.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include <Windows.h>
#include <objbase.h>

#include <array>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <span>

namespace {
using Json = nlohmann::json;
using Bytes = std::vector<std::uint8_t>;

struct ComScope {
    HRESULT result{CoInitializeEx(nullptr, COINIT_MULTITHREADED)};
    ~ComScope() { if (SUCCEEDED(result)) CoUninitialize(); }
};

class GlbHiddenWindow {
public:
    GlbHiddenWindow() {
        WNDCLASSEXW windowClass{sizeof(WNDCLASSEXW)};
        windowClass.lpfnWndProc = DefWindowProcW;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.lpszClassName = className_;
        registered_ = RegisterClassExW(&windowClass) != 0;
        if (registered_)
            window_ = CreateWindowExW(0, className_, L"GLB import WARP test", WS_OVERLAPPED,
                                      0, 0, 64, 64, nullptr, nullptr, windowClass.hInstance, nullptr);
    }
    ~GlbHiddenWindow() {
        if (window_) DestroyWindow(window_);
        if (registered_) UnregisterClassW(className_, GetModuleHandleW(nullptr));
    }
    HWND Get() const noexcept { return window_; }
private:
    static constexpr wchar_t className_[] = L"CodexTexGlbEndToEndWindow";
    HWND window_{};
    bool registered_{};
};

std::filesystem::path Directory() {
    const auto directory = std::filesystem::temp_directory_path() / "codextex-glb-tests";
    std::filesystem::create_directories(directory);
    return directory;
}

void AppendU32(Bytes& bytes, std::uint32_t value) {
    for (unsigned int shift = 0; shift < 32; shift += 8)
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
}

struct Fixture {
    Json json{{"asset", {{"version", "2.0"}}}, {"scene", 0},
              {"scenes", Json::array({{{"nodes", {0}}}})},
              {"nodes", Json::array({{{"mesh", 0}}})},
              {"meshes", Json::array()}, {"bufferViews", Json::array()},
              {"accessors", Json::array()}};
    Bytes binary;

    std::size_t AddView(std::span<const std::uint8_t> bytes) {
        while (binary.size() % 4) binary.push_back(0);
        const auto index = json["bufferViews"].size();
        json["bufferViews"].push_back({{"buffer", 0}, {"byteOffset", binary.size()}, {"byteLength", bytes.size()}});
        binary.insert(binary.end(), bytes.begin(), bytes.end());
        return index;
    }

    template <typename T, std::size_t N>
    std::size_t Accessor(const std::array<T, N>& values, std::string type,
                         std::size_t components, int componentType = 5126) {
        const auto view = AddView({reinterpret_cast<const std::uint8_t*>(values.data()), sizeof(values)});
        const auto index = json["accessors"].size();
        json["accessors"].push_back({{"bufferView", view}, {"componentType", componentType},
                                     {"count", N / components}, {"type", type}});
        return index;
    }

    Fixture() {
        const auto position = Accessor(std::array<float, 9>{0, 0, 0, 1, 0, 0, 0, 1, 0}, "VEC3", 3);
        const auto uv = Accessor(std::array<float, 6>{0, 0, 1, 0, 0, 1}, "VEC2", 2);
        const auto normal = Accessor(std::array<float, 9>{0, 0, 1, 0, 0, 1, 0, 0, 1}, "VEC3", 3);
        const auto indices = Accessor(std::array<std::uint16_t, 3>{0, 1, 2}, "SCALAR", 1, 5123);
        json["meshes"].push_back({{"primitives", Json::array({
            {{"attributes", {{"POSITION", position}, {"TEXCOORD_0", uv}, {"NORMAL", normal}}},
             {"indices", indices}}})}});
    }

    Bytes ImageBytes() {
        codextex::TextureImage image;
        const std::array<std::uint8_t, 16> pixels{255, 0, 0, 255, 0, 255, 0, 255,
                                                   0, 0, 255, 255, 255, 255, 0, 255};
        image.Assign(2, 2, pixels);
        std::string error;
        const auto path = Directory() / "corners.png";
        REQUIRE(image.SavePng(path, error));
        std::ifstream stream(path, std::ios::binary);
        return Bytes(std::istreambuf_iterator<char>(stream), {});
    }

    void EmbeddedTexture() {
        const auto encoded = ImageBytes();
        const auto view = AddView(encoded);
        json["images"] = Json::array({{{"bufferView", view}, {"mimeType", "image/png"}}});
        json["textures"] = Json::array({{{"source", 0}}});
        json["materials"] = Json::array({{{"name", "Paint"}, {"pbrMetallicRoughness",
            {{"baseColorTexture", {{"index", 0}}}, {"baseColorFactor", {0.5, 0.75, 1.0, 1.0}}}}}});
        json["meshes"][0]["primitives"][0]["material"] = 0;
    }

    std::filesystem::path Write(const std::string& name) {
        json["buffers"] = Json::array({{{"byteLength", binary.size()}}});
        std::string text = json.dump();
        while (text.size() % 4) text.push_back(' ');
        while (binary.size() % 4) binary.push_back(0);
        Bytes bytes;
        AppendU32(bytes, 0x46546c67); AppendU32(bytes, 2);
        AppendU32(bytes, static_cast<std::uint32_t>(12 + 8 + text.size() + 8 + binary.size()));
        AppendU32(bytes, static_cast<std::uint32_t>(text.size())); AppendU32(bytes, 0x4e4f534a);
        bytes.insert(bytes.end(), text.begin(), text.end());
        AppendU32(bytes, static_cast<std::uint32_t>(binary.size())); AppendU32(bytes, 0x004e4942);
        bytes.insert(bytes.end(), binary.begin(), binary.end());
        const auto path = Directory() / name;
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        return path;
    }
};

std::string Base64(std::span<const std::uint8_t> bytes) {
    constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const unsigned int a = bytes[i];
        const unsigned int b = i + 1 < bytes.size() ? bytes[i + 1] : 0;
        const unsigned int c = i + 2 < bytes.size() ? bytes[i + 2] : 0;
        result.push_back(alphabet[a >> 2]);
        result.push_back(alphabet[((a & 3) << 4) | (b >> 4)]);
        result.push_back(i + 1 < bytes.size() ? alphabet[((b & 15) << 2) | (c >> 6)] : '=');
        result.push_back(i + 2 < bytes.size() ? alphabet[c & 63] : '=');
    }
    return result;
}
} // namespace

TEST_CASE("GLB loads embedded PNG and preserves material factor and top-left UVs", "[glb]") {
    ComScope com;
    Fixture fixture;
    fixture.EmbeddedTexture();
    codextex::ImportedModel model;
    std::string error;
    REQUIRE(codextex::LoadGlbModel(fixture.Write("embedded.glb"), model, error));
    REQUIRE(model.surfaces.size() == 1);
    const auto& surface = model.surfaces[0];
    CHECK(surface.name == "Paint");
    CHECK(surface.editError.empty());
    CHECK(surface.baseColor.Width() == 2);
    CHECK(surface.baseColor.Height() == 2);
    CHECK(surface.baseColor.Pixels()[0] == 255);
    CHECK(surface.baseColor.Pixels()[1] == 0);
    CHECK(surface.baseColor.Pixels()[8 + 2] == 255);
    CHECK(surface.externalTexturePath.empty());
    CHECK(surface.appearance.baseColorFactor[0] == Catch::Approx(0.5f));
    CHECK(surface.mesh.Vertices()[0].uv.y == 0.0f);
    CHECK(surface.mesh.Vertices()[2].uv.y == 1.0f);
}

TEST_CASE("GLB applies parent transforms negative scale and inverse transpose normals", "[glb]") {
    Fixture fixture;
    fixture.json["nodes"] = Json::array({{{"translation", {5, 6, 7}}, {"children", {1}}},
                                           {{"mesh", 0}, {"scale", {-2, 3, 4}}}});
    const auto normals = fixture.Accessor(std::array<float, 9>{1, 1, 1, 1, 1, 1, 1, 1, 1}, "VEC3", 3);
    fixture.json["meshes"][0]["primitives"][0]["attributes"]["NORMAL"] = normals;
    codextex::ImportedModel model;
    std::string error;
    REQUIRE(codextex::LoadGlbModel(fixture.Write("transform.glb"), model, error));
    const auto& mesh = model.surfaces[0].mesh;
    CHECK(mesh.BoundsMin().x == Catch::Approx(3));
    CHECK(mesh.BoundsMax().y == Catch::Approx(9));
    CHECK(mesh.BoundsMin().z == Catch::Approx(7));
    const auto& v = mesh.Vertices();
    CHECK(v[1].position.y == Catch::Approx(9));
    CHECK(v[2].position.x == Catch::Approx(3));
    CHECK(v[0].normal.x / v[0].normal.z == Catch::Approx(-2));
    CHECK(v[0].normal.y / v[0].normal.z == Catch::Approx(4.0 / 3.0));
}

TEST_CASE("GLB separates materials while preserving a shared image identity", "[glb]") {
    ComScope com;
    Fixture fixture;
    fixture.EmbeddedTexture();
    fixture.json["materials"].push_back(fixture.json["materials"][0]);
    fixture.json["materials"][1]["name"] = "Second";
    fixture.json["materials"][1]["alphaMode"] = "MASK";
    fixture.json["materials"][1]["alphaCutoff"] = 0.25;
    fixture.json["meshes"][0]["primitives"].push_back(fixture.json["meshes"][0]["primitives"][0]);
    fixture.json["meshes"][0]["primitives"][1]["material"] = 1;
    codextex::ImportedModel model;
    std::string error;
    REQUIRE(codextex::LoadGlbModel(fixture.Write("materials.glb"), model, error));
    REQUIRE(model.surfaces.size() == 2);
    CHECK(model.surfaces[0].mesh.TriangleCount() == 1);
    CHECK(model.surfaces[1].mesh.TriangleCount() == 1);
    CHECK(model.surfaces[0].textureKey == model.surfaces[1].textureKey);
    CHECK(model.surfaces[1].appearance.alphaMode == codextex::MaterialAlphaMode::Mask);
    CHECK(model.surfaces[1].appearance.alphaCutoff == Catch::Approx(0.25));
}

TEST_CASE("GLB chooses UV1 and applies KHR texture transform", "[glb]") {
    ComScope com;
    Fixture fixture;
    fixture.EmbeddedTexture();
    const auto uv1 = fixture.Accessor(std::array<float, 6>{0.2f, 0.4f, 0.6f, 0.4f, 0.2f, 0.8f}, "VEC2", 2);
    fixture.json["meshes"][0]["primitives"][0]["attributes"]["TEXCOORD_1"] = uv1;
    fixture.json["extensionsRequired"] = {"KHR_texture_transform"};
    fixture.json["extensionsUsed"] = {"KHR_texture_transform"};
    fixture.json["materials"][0]["pbrMetallicRoughness"]["baseColorTexture"]["extensions"] =
        {{"KHR_texture_transform", {{"texCoord", 1}, {"offset", {0.7, 0.1}}, {"scale", {0.5, 0.5}},
                                    {"rotation", 1.5707963267948966}}}};
    codextex::ImportedModel model;
    std::string error;
    REQUIRE(codextex::LoadGlbModel(fixture.Write("uv1.glb"), model, error));
    const auto& uv = model.surfaces[0].mesh.Vertices()[0].uv;
    CHECK(uv.x == Catch::Approx(0.5f));
    CHECK(uv.y == Catch::Approx(0.2f));
    CHECK(model.surfaces[0].editError.empty());
}

TEST_CASE("GLB reads external percent-encoded and data URI images", "[glb]") {
    ComScope com;
    Fixture fixture;
    fixture.EmbeddedTexture();
    SECTION("relative PNG URI") {
        std::filesystem::copy_file(Directory() / "corners.png", Directory() / "corner colors.png",
                                   std::filesystem::copy_options::overwrite_existing);
        fixture.json["images"][0] = {{"uri", "corner%20colors.png"}};
    }
    SECTION("base64 PNG URI") {
        fixture.json["images"][0] = {{"uri", "data:image/png;base64," + Base64(fixture.ImageBytes())}};
    }
    SECTION("Unicode image URI") {
        std::filesystem::copy_file(Directory() / "corners.png", Directory() / std::filesystem::path(u8"색상.png"),
                                   std::filesystem::copy_options::overwrite_existing);
        fixture.json["images"][0] = {{"uri", "색상.png"}};
    }
    codextex::ImportedModel model;
    std::string error;
    REQUIRE(codextex::LoadGlbModel(fixture.Write("image-uri.glb"), model, error));
    CHECK(model.surfaces[0].baseColor.Width() == 2);
    CHECK(model.surfaces[0].editError.empty());
}

TEST_CASE("GLB opens a Unicode model path", "[glb]") {
    Fixture fixture;
    const auto source = fixture.Write("unicode-source.glb");
    const auto destination = Directory() / std::filesystem::path(u8"모델.glb");
    std::filesystem::copy_file(source, destination, std::filesystem::copy_options::overwrite_existing);
    codextex::ImportedModel model;
    std::string error;
    REQUIRE(codextex::LoadGlbModel(destination, model, error));
    CHECK(model.sourcePath == destination);
}

TEST_CASE("GLB reports missing textures and display-only UV or alpha limitations", "[glb]") {
    ComScope com;
    Fixture fixture;
    fixture.EmbeddedTexture();
    std::string expected;
    bool missing = false;
    SECTION("missing texture") {
        fixture.json["images"][0] = {{"uri", "file-that-does-not-exist.png"}};
        missing = true;
    }
    SECTION("missing texture preserves missing UV restriction") {
        fixture.json["images"][0] = {{"uri", "file-that-does-not-exist.png"}};
        fixture.json["meshes"][0]["primitives"][0]["attributes"].erase("TEXCOORD_0");
        missing = true;
        expected = "UV";
    }
    SECTION("missing UV channel") {
        fixture.json["meshes"][0]["primitives"][0]["attributes"].erase("TEXCOORD_0");
        expected = "UV";
    }
    SECTION("alpha blend") {
        fixture.json["materials"][0]["alphaMode"] = "BLEND";
        expected = "Translucent";
    }
    codextex::ImportedModel model;
    std::string error;
    REQUIRE(codextex::LoadGlbModel(fixture.Write("display-only.glb"), model, error));
    if (expected.empty()) CHECK(model.surfaces[0].editError.empty());
    else CHECK(model.surfaces[0].editError.find(expected) != std::string::npos);
    CHECK(model.surfaces[0].baseColorMissing == missing);
    CHECK_FALSE(model.surfaces[0].baseColor.Empty());
}

TEST_CASE("GLB rejects invalid or unsupported models without replacing the previous model", "[glb]") {
    Fixture fixture;
    std::string expected;
    SECTION("required compression") {
        fixture.json["extensionsRequired"] = {"KHR_draco_mesh_compression"};
        expected = "KHR_draco_mesh_compression";
    }
    SECTION("unknown required extension") {
        fixture.json["extensionsRequired"] = {"VENDOR_unknown_feature"};
        expected = "VENDOR_unknown_feature";
    }
    SECTION("buffer range") {
        fixture.json["bufferViews"][0]["byteLength"] = 1000000;
        expected = "out of bounds";
    }
    SECTION("invalid index") {
        const auto index = fixture.Accessor(std::array<std::uint16_t, 3>{0, 1, 99}, "SCALAR", 1, 5123);
        fixture.json["meshes"][0]["primitives"][0]["indices"] = index;
        expected = "invalid scene";
    }
    SECTION("accessor integer overflow") {
        fixture.json["accessors"][0]["count"] = std::numeric_limits<std::uint64_t>::max();
        expected = "accessor";
    }
    SECTION("morph targets") {
        fixture.json["meshes"][0]["primitives"][0]["targets"] = Json::array({{{"POSITION", 0}}});
        expected = "morph";
    }
    SECTION("skinned mesh") {
        fixture.json["nodes"][0]["skin"] = 0;
        fixture.json["nodes"].push_back(Json::object());
        fixture.json["skins"] = Json::array({{{"joints", {1}}}});
        expected = "skinned";
    }
    SECTION("cyclic nodes") {
        fixture.json["nodes"][0]["children"] = {0};
        expected = "invalid";
    }
    codextex::ImportedModel model;
    model.sourcePath = "previous.obj";
    model.warnings = {"preserved"};
    std::string error;
    CHECK_FALSE(codextex::LoadGlbModel(fixture.Write("invalid.glb"), model, error));
    CHECK(error.find(expected) != std::string::npos);
    CHECK(model.sourcePath == "previous.obj");
    CHECK(model.warnings == std::vector<std::string>{"preserved"});
}

TEST_CASE("GLB reads sparse attributes with an interleaved base stride", "[glb]") {
    Fixture fixture;
    const std::array<float, 12> interleaved{0, 0, 0, 99, 0, 0, 0, 99, 0, 0, 0, 99};
    const auto baseView = fixture.AddView({reinterpret_cast<const std::uint8_t*>(interleaved.data()), sizeof(interleaved)});
    fixture.json["bufferViews"][baseView]["byteStride"] = 16;
    const std::array<std::uint8_t, 2> sparseIndices{1, 2};
    const auto indicesView = fixture.AddView(sparseIndices);
    const std::array<float, 6> sparseValues{2, 0, 0, 0, 3, 0};
    const auto valuesView = fixture.AddView({reinterpret_cast<const std::uint8_t*>(sparseValues.data()), sizeof(sparseValues)});
    fixture.json["accessors"][0]["bufferView"] = baseView;
    fixture.json["accessors"][0]["sparse"] = {{"count", 2},
        {"indices", {{"bufferView", indicesView}, {"componentType", 5121}}},
        {"values", {{"bufferView", valuesView}}}};
    codextex::ImportedModel model;
    std::string error;
    REQUIRE(codextex::LoadGlbModel(fixture.Write("sparse.glb"), model, error));
    CHECK(model.surfaces[0].mesh.BoundsMax().x == 2.0f);
    CHECK(model.surfaces[0].mesh.BoundsMax().y == 3.0f);
}

TEST_CASE("GLB triangulates strips and fans", "[glb]") {
    Fixture fixture;
    const auto position = fixture.Accessor(std::array<float, 12>{0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0}, "VEC3", 3);
    const auto uv = fixture.Accessor(std::array<float, 8>{0, 0, 1, 0, 0, 1, 1, 1}, "VEC2", 2);
    auto& primitive = fixture.json["meshes"][0]["primitives"][0];
    primitive["attributes"] = {{"POSITION", position}, {"TEXCOORD_0", uv}};
    primitive.erase("indices");
    SECTION("strip") { primitive["mode"] = 5; }
    SECTION("fan") {
        primitive["mode"] = 6;
        primitive["indices"] = fixture.Accessor(std::array<std::uint16_t, 4>{0, 1, 3, 2}, "SCALAR", 1, 5123);
    }
    codextex::ImportedModel model;
    std::string error;
    REQUIRE(codextex::LoadGlbModel(fixture.Write("strip-fan.glb"), model, error));
    CHECK(model.surfaces[0].mesh.TriangleCount() == 2);
    for (const auto& vertex : model.surfaces[0].mesh.Vertices())
        CHECK(vertex.normal.z == Catch::Approx(1));
}

TEST_CASE("GLB embedded UV1 alpha-mask survives WARP capture bake and PNG roundtrip", "[glb][warp]") {
    ComScope com;
    Fixture fixture;
    fixture.EmbeddedTexture();
    const auto position = fixture.Accessor(
        std::array<float, 12>{-1, -1, 0, 1, -1, 0, 1, 1, 0, -1, 1, 0}, "VEC3", 3);
    const auto uv0 = fixture.Accessor(std::array<float, 8>{0, 0, 1, 0, 1, 1, 0, 1}, "VEC2", 2);
    const auto uv1 = fixture.Accessor(std::array<float, 8>{0, 1, 1, 1, 1, 0, 0, 0}, "VEC2", 2);
    const auto indices = fixture.Accessor(std::array<std::uint16_t, 6>{0, 1, 2, 0, 2, 3}, "SCALAR", 1, 5123);
    fixture.json["meshes"][0]["primitives"][0] = {
        {"attributes", {{"POSITION", position}, {"TEXCOORD_0", uv0}, {"TEXCOORD_1", uv1}}},
        {"indices", indices}, {"material", 0}};
    auto& material = fixture.json["materials"][0];
    material["alphaMode"] = "MASK";
    material["alphaCutoff"] = 0.5;
    material["pbrMetallicRoughness"]["baseColorFactor"] = {1, 1, 1, 1};
    material["pbrMetallicRoughness"]["baseColorTexture"]["texCoord"] = 1;

    Bytes originalPixels(16 * 16 * 4);
    for (std::size_t y = 0; y < 16; ++y) {
        for (std::size_t x = 0; x < 16; ++x) {
            const std::array<std::uint8_t, 4> color = y < 8 ?
                (x < 8 ? std::array<std::uint8_t, 4>{255, 0, 0, 255} :
                         std::array<std::uint8_t, 4>{0, 255, 0, 255}) :
                (x < 8 ? std::array<std::uint8_t, 4>{0, 0, 255, 255} :
                         std::array<std::uint8_t, 4>{255, 255, 0, 0});
            std::copy(color.begin(), color.end(), originalPixels.begin() + (y * 16 + x) * 4);
        }
    }
    codextex::TextureImage authored;
    authored.Assign(16, 16, originalPixels);
    std::string error;
    const auto imagePath = Directory() / "end-to-end-source.png";
    REQUIRE(authored.SavePng(imagePath, error));
    std::ifstream imageFile(imagePath, std::ios::binary);
    const Bytes encoded(std::istreambuf_iterator<char>(imageFile), {});
    fixture.json["images"][0]["bufferView"] = fixture.AddView(encoded);
    const auto modelPath = fixture.Write("end-to-end.glb");
    const auto readBytes = [](const std::filesystem::path& path) {
        std::ifstream stream(path, std::ios::binary);
        return Bytes(std::istreambuf_iterator<char>(stream), {});
    };
    const auto originalGlb = readBytes(modelPath);

    codextex::ImportedModel model;
    REQUIRE(codextex::LoadModel(modelPath, model, error));
    REQUIRE(model.surfaces.size() == 1);
    REQUIRE(model.surfaces[0].editError.empty());
    REQUIRE(model.surfaces[0].appearance.alphaMode == codextex::MaterialAlphaMode::Mask);
    REQUIRE(model.surfaces[0].baseColor.Pixels() == originalPixels);

    GlbHiddenWindow window;
    REQUIRE(window.Get() != nullptr);
    codextex::Renderer renderer;
    REQUIRE(renderer.Initialize(window.Get(), error, true));
    REQUIRE(renderer.SetImportedModel(model, 0, model.surfaces[0].baseColor, error));
    renderer.SetShadingEnabled(false);
    codextex::CameraState camera;
    camera.pitch = 0;
    camera.distance = 3;
    renderer.RenderViewport(128, 128, camera);
    // The +Z camera reverses screen X. Screen Y must retain glTF's top-left UV origin.
    CHECK(renderer.PickTriangle(32, 32) < 2);
    CHECK(renderer.PickTriangle(96, 32) < 2);
    CHECK(renderer.PickTriangle(96, 96) < 2);
    CHECK(renderer.PickTriangle(32, 96) == UINT32_MAX);
    codextex::TextureImage capture;
    codextex::Renderer::ProjectionFrame frame;
    REQUIRE(renderer.CaptureFrame(camera, capture, frame, error));
    REQUIRE(frame.Valid());
    const auto sample = [](const codextex::TextureImage& image, std::size_t x, std::size_t y, std::size_t channel) {
        return image.Pixels()[(y * image.Width() + x) * 4 + channel];
    };
    CHECK(sample(capture, 32, 32, 1) > 245);
    CHECK(sample(capture, 32, 32, 0) < 10);
    CHECK(sample(capture, 96, 32, 0) > 245);
    CHECK(sample(capture, 96, 32, 2) < 10);
    CHECK(sample(capture, 96, 96, 2) > 245);

    auto projection = capture;
    for (std::size_t y = 0; y < projection.Height(); ++y) {
        for (std::size_t x = 0; x < projection.Width(); ++x) {
            const std::array<std::uint8_t, 4> color = y < projection.Height() / 2 ?
                std::array<std::uint8_t, 4>{230, 180, 20, 255} :
                std::array<std::uint8_t, 4>{20, 180, 230, 255};
            std::copy(color.begin(), color.end(), projection.Pixels().begin() + (y * projection.Width() + x) * 4);
        }
    }
    REQUIRE(renderer.SetProjectionImage(projection, error));
    codextex::MaskImage mask;
    mask.Resize(128, 128, true);
    REQUIRE(renderer.SetMask(mask, 0));
    REQUIRE(renderer.BakeProjection(75, error));
    codextex::TextureImage baked;
    REQUIRE(renderer.ReadWorkingTexture(baked, error));
    CHECK(sample(baked, 4, 4, 0) >= 228);
    CHECK(sample(baked, 4, 4, 2) <= 22);
    CHECK(sample(baked, 12, 4, 0) >= 228);
    CHECK(sample(baked, 4, 12, 0) <= 22);
    CHECK(sample(baked, 4, 12, 2) >= 228);
    // The lower-right authored alpha hole must keep every source channel.
    for (std::size_t channel = 0; channel < 4; ++channel)
        CHECK(sample(baked, 12, 12, channel) == originalPixels[(12 * 16 + 12) * 4 + channel]);

    const auto outputPath = Directory() / "end-to-end-baked.png";
    REQUIRE(baked.SavePng(outputPath, error));
    codextex::TextureImage reopened;
    REQUIRE(reopened.LoadPng(outputPath, error));
    CHECK(reopened.Pixels() == baked.Pixels());
    CHECK(model.surfaces[0].baseColor.Pixels() == originalPixels);
    CHECK(readBytes(modelPath) == originalGlb);
    CHECK(readBytes(imagePath) == encoded);
}
