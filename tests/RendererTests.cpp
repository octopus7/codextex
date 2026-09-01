#include "graphics/Renderer.hpp"

#include <catch2/catch_test_macros.hpp>
#include <Windows.h>

#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

class HiddenWindow {
public:
    HiddenWindow() {
        instance_ = GetModuleHandleW(nullptr);
        WNDCLASSEXW windowClass{sizeof(WNDCLASSEXW)};
        windowClass.lpfnWndProc = DefWindowProcW;
        windowClass.hInstance = instance_;
        windowClass.lpszClassName = className_;
        registered_ = RegisterClassExW(&windowClass) != 0;
        if (registered_) {
            window_ = CreateWindowExW(0, className_, L"CodexTex WARP test", WS_OVERLAPPED,
                                      0, 0, 64, 64, nullptr, nullptr, instance_, nullptr);
        }
    }

    ~HiddenWindow() {
        if (window_) DestroyWindow(window_);
        if (registered_) UnregisterClassW(className_, instance_);
    }

    [[nodiscard]] HWND Get() const noexcept { return window_; }

private:
    static constexpr wchar_t className_[] = L"CodexTexWarpTestWindow";
    HINSTANCE instance_{};
    HWND window_{};
    bool registered_{};
};

} // namespace

TEST_CASE("D3D11 WARP initializes viewport bake and GPU mask shaders") {
    HiddenWindow window;
    REQUIRE(window.Get() != nullptr);

    codextex::Renderer renderer;
    std::string error;
    INFO(error);
    REQUIRE(renderer.Initialize(window.Get(), error, true));
    CHECK(renderer.Device() != nullptr);
    CHECK(renderer.Context() != nullptr);
    renderer.Shutdown();
}

TEST_CASE("WARP exposes hidden geometry and bakes only the frozen visible surface") {
    HiddenWindow window;
    REQUIRE(window.Get() != nullptr);

    codextex::Renderer renderer;
    std::string error;
    REQUIRE(renderer.Initialize(window.Get(), error, true));

    const auto directory = std::filesystem::temp_directory_path() / "codextex-tests";
    std::filesystem::create_directories(directory);
    const auto objPath = directory / "two-layer-quads.obj";
    std::ofstream obj(objPath, std::ios::binary | std::ios::trunc);
    obj << R"OBJ(
v -1 -1 0
v  1 -1 0
v  1  1 0
v -1  1 0
v -1 -1 -0.2
v  1 -1 -0.2
v  1  1 -0.2
v -1  1 -0.2
vt 0 0
vt 1 0
vt 1 1
vt 0 1
f 1/1 2/2 3/3 4/4
f 5/1 6/2 7/3 8/4
)OBJ";
    obj.close();

    codextex::Mesh mesh;
    REQUIRE(mesh.LoadObj(objPath, error));
    REQUIRE(mesh.TriangleCount() == 4);
    REQUIRE(renderer.SetMesh(mesh, error));

    std::vector<std::uint8_t> basePixels(32 * 32 * 4, 255);
    for (std::size_t i = 0; i < basePixels.size(); i += 4) {
        basePixels[i] = 12;
        basePixels[i + 1] = 24;
        basePixels[i + 2] = 36;
        basePixels[i + 3] = 77;
    }
    codextex::TextureImage base;
    base.Assign(32, 32, basePixels);
    REQUIRE(renderer.SetWorkingTexture(base, error));

    const auto referenceObjPath = directory / "front-reference.obj";
    std::ofstream referenceObj(referenceObjPath, std::ios::binary | std::ios::trunc);
    referenceObj << R"OBJ(
v -0.8 -0.8 0.15
v  0.8 -0.8 0.15
v  0.8  0.8 0.15
v -0.8  0.8 0.15
vt 0 0
vt 1 0
vt 1 1
vt 0 1
f 1/1 2/2 3/3 4/4
)OBJ";
    referenceObj.close();
    codextex::Mesh referenceMesh;
    REQUIRE(referenceMesh.LoadObj(referenceObjPath, error));
    std::vector<std::uint8_t> referencePixels(4 * 4 * 4, 255);
    for (std::size_t i = 0; i < referencePixels.size(); i += 4) {
        referencePixels[i] = 10;
        referencePixels[i + 1] = 30;
        referencePixels[i + 2] = 240;
    }
    codextex::TextureImage referenceTexture;
    referenceTexture.Assign(4, 4, referencePixels);
    REQUIRE(renderer.AddReferenceAsset(referenceMesh, referenceTexture, error));

    codextex::CameraState camera;
    camera.pitch = 0;
    camera.distance = 3;
    renderer.SetReferenceAssetsVisible(false);
    renderer.RenderViewport(128, 128, camera);
    CHECK(renderer.PickTriangle(64, 64) < 2);
    codextex::TextureImage unlitCapture;
    REQUIRE(renderer.CaptureFrame(camera, unlitCapture, error));
    const std::size_t unlitCenter = (64 * 128 + 64) * 4;
    CHECK(std::abs(static_cast<int>(unlitCapture.Pixels()[unlitCenter]) - 12) <= 2);
    CHECK(std::abs(static_cast<int>(unlitCapture.Pixels()[unlitCenter + 1]) - 24) <= 2);
    CHECK(std::abs(static_cast<int>(unlitCapture.Pixels()[unlitCenter + 2]) - 36) <= 2);
    renderer.SetShadingEnabled(true);
    renderer.RenderViewport(128, 128, camera);
    codextex::TextureImage shadedCapture;
    REQUIRE(renderer.CaptureFrame(camera, shadedCapture, error));
    CHECK(shadedCapture.Pixels()[unlitCenter + 2] < unlitCapture.Pixels()[unlitCenter + 2]);
    renderer.SetShadingEnabled(false);

    const std::array<std::uint8_t, 4> hidden{1, 1, 0, 0};
    renderer.SetHiddenFaces(hidden);
    renderer.SetReferenceAssetsVisible(false);
    renderer.RenderViewport(128, 128, camera);
    const auto exposedTriangle = renderer.PickTriangle(64, 64);
    CHECK(exposedTriangle >= 2);
    CHECK(exposedTriangle < 4);

    renderer.SetReferenceAssetsVisible(true);
    renderer.RenderViewport(128, 128, camera);
    CHECK(renderer.PickTriangle(64, 64) == UINT32_MAX);

    codextex::TextureImage capture;
    REQUIRE(renderer.CaptureFrame(camera, capture, error));
    REQUIRE(capture.Width() == 128);
    const std::size_t captureCenter = (64 * 128 + 64) * 4;
    CHECK(capture.Pixels()[captureCenter + 2] > capture.Pixels()[captureCenter]);

    std::vector<std::uint8_t> projectionPixels(128 * 128 * 4, 255);
    for (std::size_t i = 0; i < projectionPixels.size(); i += 4) {
        projectionPixels[i] = 235;
        projectionPixels[i + 1] = 20;
        projectionPixels[i + 2] = 15;
    }
    codextex::TextureImage projection;
    projection.Assign(128, 128, projectionPixels);
    REQUIRE(renderer.SetProjectionImage(projection, error));

    codextex::MaskImage mask;
    mask.Resize(128, 128, false);
    const std::array<codextex::Vec2, 4> lasso{{{32, 32}, {96, 32}, {96, 96}, {32, 96}}};
    mask.ApplyLasso(lasso, true);
    renderer.SetMask(mask, 8);
    REQUIRE(renderer.BakeProjection(75, error));

    codextex::TextureImage baked;
    REQUIRE(renderer.ReadWorkingTexture(baked, error));
    std::size_t changed = 0;
    for (std::size_t i = 0; i < baked.Pixels().size(); i += 4) {
        CHECK(baked.Pixels()[i + 3] == 77);
        if (baked.Pixels()[i] > 180 && baked.Pixels()[i + 1] < 60) ++changed;
    }
    CHECK(changed > 200);
    CHECK(changed < 700);
    const auto& pixels = baked.Pixels();
    CHECK(pixels[0] == 12);
    const std::size_t center = (16 * 32 + 16) * 4;
    CHECK(pixels[center] > 180);
    renderer.Shutdown();
}
