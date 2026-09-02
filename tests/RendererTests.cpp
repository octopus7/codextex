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

TEST_CASE("WARP viewport shares the working texture and can preview the loaded original") {
    HiddenWindow window;
    REQUIRE(window.Get() != nullptr);

    codextex::Renderer renderer;
    std::string error;
    REQUIRE(renderer.Initialize(window.Get(), error, true));

    const auto directory = std::filesystem::temp_directory_path() / "codextex-tests";
    std::filesystem::create_directories(directory);
    const auto objPath = directory / "original-preview-quad.obj";
    std::ofstream obj(objPath, std::ios::binary | std::ios::trunc);
    obj << R"OBJ(
v -1 -1 0
v  1 -1 0
v  1  1 0
v -1  1 0
vt 0 0
vt 1 0
vt 1 1
vt 0 1
f 1/1 2/2 3/3 4/4
)OBJ";
    obj.close();

    codextex::Mesh mesh;
    REQUIRE(mesh.LoadObj(objPath, error));
    REQUIRE(renderer.SetMesh(mesh, error));

    std::vector<std::uint8_t> originalPixels(4 * 4 * 4, 255);
    std::vector<std::uint8_t> workingPixels(4 * 4 * 4, 255);
    for (std::size_t i = 0; i < originalPixels.size(); i += 4) {
        originalPixels[i] = 230;
        originalPixels[i + 1] = 20;
        originalPixels[i + 2] = 20;
        workingPixels[i] = 20;
        workingPixels[i + 1] = 20;
        workingPixels[i + 2] = 230;
    }
    codextex::TextureImage original;
    codextex::TextureImage working;
    original.Assign(4, 4, originalPixels);
    working.Assign(4, 4, workingPixels);
    REQUIRE(renderer.SetSourceAndWorkingTexture(original, error));
    REQUIRE(renderer.SetWorkingTexture(working, error));

    codextex::CameraState camera;
    camera.pitch = 0;
    camera.distance = 3;
    renderer.SetViewportBackgroundColor({0.20f, 0.40f, 0.60f});
    renderer.SetOriginalTexturePreview(false);
    renderer.RenderViewport(64, 64, camera);
    codextex::TextureImage workingCapture;
    REQUIRE(renderer.CaptureFrame(camera, workingCapture, error));
    const std::size_t center = (32 * 64 + 32) * 4;
    CHECK(workingCapture.Pixels()[center] < 50);
    CHECK(workingCapture.Pixels()[center + 2] > 200);
    CHECK(workingCapture.Pixels()[0] >= 49);
    CHECK(workingCapture.Pixels()[0] <= 53);
    CHECK(workingCapture.Pixels()[1] >= 100);
    CHECK(workingCapture.Pixels()[1] <= 104);
    CHECK(workingCapture.Pixels()[2] >= 151);
    CHECK(workingCapture.Pixels()[2] <= 155);

    codextex::TextureImage offlineCapture;
    codextex::Renderer::ProjectionFrame offlineFrame;
    REQUIRE(renderer.CaptureFrame(camera, 1024, 1024, offlineCapture, offlineFrame, error));
    CHECK(offlineCapture.Width() == 1024);
    CHECK(offlineCapture.Height() == 1024);
    CHECK(offlineFrame.width == 1024);
    CHECK(offlineFrame.height == 1024);
    CHECK(offlineFrame.cropX == 0);
    CHECK(offlineFrame.cropY == 0);
    CHECK(offlineFrame.cropSize == 1024);
    CHECK(renderer.ViewportWidth() == 64);
    CHECK(renderer.ViewportHeight() == 64);

    renderer.SetOriginalTexturePreview(true);
    renderer.RenderViewport(64, 64, camera);
    codextex::TextureImage originalCapture;
    REQUIRE(renderer.CaptureFrame(camera, originalCapture, error));
    CHECK(originalCapture.Pixels()[center] > 200);
    CHECK(originalCapture.Pixels()[center + 2] < 50);
    renderer.Shutdown();
}

TEST_CASE("WARP bake publishes the latest working texture after previewing original") {
    HiddenWindow window;
    REQUIRE(window.Get() != nullptr);

    codextex::Renderer renderer;
    std::string error;
    REQUIRE(renderer.Initialize(window.Get(), error, true));

    const auto directory = std::filesystem::temp_directory_path() / "codextex-tests";
    std::filesystem::create_directories(directory);
    const auto objPath = directory / "shared-working-texture-quad.obj";
    std::ofstream obj(objPath, std::ios::binary | std::ios::trunc);
    obj << R"OBJ(
v -1 -1 0
v  1 -1 0
v  1  1 0
v -1  1 0
vt 0 0
vt 1 0
vt 1 1
vt 0 1
f 1/1 2/2 3/3 4/4
)OBJ";
    obj.close();

    codextex::Mesh mesh;
    REQUIRE(mesh.LoadObj(objPath, error));
    REQUIRE(renderer.SetMesh(mesh, error));

    const auto solidImage = [](const std::uint8_t red, const std::uint8_t green,
                               const std::uint8_t blue, const std::uint32_t size) {
        std::vector<std::uint8_t> pixels(
            static_cast<std::size_t>(size) * size * 4, 255);
        for (std::size_t i = 0; i < pixels.size(); i += 4) {
            pixels[i] = red;
            pixels[i + 1] = green;
            pixels[i + 2] = blue;
        }
        codextex::TextureImage image;
        image.Assign(size, size, std::move(pixels));
        return image;
    };

    const codextex::TextureImage original = solidImage(230, 20, 20, 16);
    const codextex::TextureImage generated = solidImage(20, 230, 20, 128);
    REQUIRE(renderer.SetSourceAndWorkingTexture(original, error));

    codextex::CameraState camera;
    camera.pitch = 0;
    camera.distance = 3;
    codextex::TextureImage capture;
    codextex::Renderer::ProjectionFrame frame;
    REQUIRE(renderer.CaptureFrame(camera, 128, 128, capture, frame, error));
    REQUIRE(renderer.SetProjectionImage(generated, error));
    codextex::MaskImage mask;
    mask.Resize(128, 128, true);
    renderer.SetMask(mask, 0);

    renderer.SetOriginalTexturePreview(true);
    renderer.SetProjectionPreviewMode(codextex::ProjectionPreviewMode::Full);
    REQUIRE(renderer.BakeProjection(75, error));

    codextex::TextureImage baked;
    REQUIRE(renderer.ReadWorkingTexture(baked, error));
    const std::size_t textureCenter = (8 * 16 + 8) * 4;
    CHECK(baked.Pixels()[textureCenter] < 50);
    CHECK(baked.Pixels()[textureCenter + 1] > 200);
    CHECK(baked.Pixels()[textureCenter + 2] < 50);

    renderer.RenderViewport(128, 128, camera);
    codextex::TextureImage workingCapture;
    REQUIRE(renderer.CaptureFrame(camera, workingCapture, error));
    const std::size_t viewportCenter = (64 * 128 + 64) * 4;
    CHECK(workingCapture.Pixels()[viewportCenter] < 50);
    CHECK(workingCapture.Pixels()[viewportCenter + 1] > 200);
    CHECK(workingCapture.Pixels()[viewportCenter + 2] < 50);

    renderer.SetOriginalTexturePreview(true);
    codextex::TextureImage originalCapture;
    REQUIRE(renderer.CaptureFrame(camera, originalCapture, error));
    CHECK(originalCapture.Pixels()[viewportCenter] > 200);
    CHECK(originalCapture.Pixels()[viewportCenter + 1] < 50);
    CHECK(originalCapture.Pixels()[viewportCenter + 2] < 50);

    renderer.SetOriginalTexturePreview(false);
    codextex::TextureImage sharedWorkingCapture;
    REQUIRE(renderer.CaptureFrame(camera, sharedWorkingCapture, error));
    CHECK(sharedWorkingCapture.Pixels()[viewportCenter] < 50);
    CHECK(sharedWorkingCapture.Pixels()[viewportCenter + 1] > 200);
    CHECK(sharedWorkingCapture.Pixels()[viewportCenter + 2] < 50);
    renderer.Shutdown();
}

TEST_CASE("WARP projection viewport switches between masked original and full generated views") {
    HiddenWindow window;
    REQUIRE(window.Get() != nullptr);

    codextex::Renderer renderer;
    std::string error;
    REQUIRE(renderer.Initialize(window.Get(), error, true));

    const auto directory = std::filesystem::temp_directory_path() / "codextex-tests";
    std::filesystem::create_directories(directory);
    const auto objPath = directory / "projection-preview-modes.obj";
    std::ofstream obj(objPath, std::ios::binary | std::ios::trunc);
    obj << R"OBJ(
v -1 -1 0
v  1 -1 0
v  1  1 0
v -1  1 0
vt 0 0
vt 1 0
vt 1 1
vt 0 1
f 1/1 2/2 3/3 4/4
)OBJ";
    obj.close();

    codextex::Mesh mesh;
    REQUIRE(mesh.LoadObj(objPath, error));
    REQUIRE(renderer.SetMesh(mesh, error));

    const auto solidImage = [](const std::uint8_t r, const std::uint8_t g,
                               const std::uint8_t b) {
        std::vector<std::uint8_t> pixels(16 * 16 * 4, 255);
        for (std::size_t i = 0; i < pixels.size(); i += 4) {
            pixels[i] = r;
            pixels[i + 1] = g;
            pixels[i + 2] = b;
        }
        codextex::TextureImage image;
        image.Assign(16, 16, std::move(pixels));
        return image;
    };
    const codextex::TextureImage original = solidImage(230, 20, 20);
    const codextex::TextureImage working = solidImage(20, 20, 230);
    const codextex::TextureImage generated = solidImage(20, 230, 20);
    REQUIRE(renderer.SetSourceAndWorkingTexture(original, error));
    REQUIRE(renderer.SetWorkingTexture(working, error));
    REQUIRE(renderer.SetProjectionImage(generated, error));

    const auto referenceObjPath = directory / "projection-preview-reference.obj";
    std::ofstream referenceObj(referenceObjPath, std::ios::binary | std::ios::trunc);
    referenceObj << R"OBJ(
v -0.22 -0.22 0.2
v  0.22 -0.22 0.2
v  0.22  0.22 0.2
v -0.22  0.22 0.2
vt 0 0
vt 1 0
vt 1 1
vt 0 1
f 1/1 2/2 3/3 4/4
)OBJ";
    referenceObj.close();
    codextex::Mesh referenceMesh;
    REQUIRE(referenceMesh.LoadObj(referenceObjPath, error));
    const codextex::TextureImage reference = solidImage(230, 210, 20);
    REQUIRE(renderer.AddReferenceAsset(referenceMesh, reference, error));
    renderer.SetReferenceAssetsVisible(true);

    codextex::MaskImage emptyMask;
    emptyMask.Resize(128, 128, false);
    renderer.SetMask(emptyMask, 0);
    codextex::CameraState camera;
    camera.pitch = 0;
    camera.distance = 3;

    const auto pixelAt = [](const codextex::TextureImage& image, const std::uint32_t x,
                            const std::uint32_t y) {
        return (static_cast<std::size_t>(y) * image.Width() + x) * 4;
    };

    renderer.SetOriginalTexturePreview(false);
    renderer.SetProjectionPreviewMode(codextex::ProjectionPreviewMode::Masked);
    renderer.RenderViewport(128, 128, camera);
    codextex::TextureImage maskedCapture;
    REQUIRE(renderer.CaptureFrame(camera, maskedCapture, error));
    const std::size_t surfacePixel = pixelAt(maskedCapture, 40, 64);
    CHECK(maskedCapture.Pixels()[surfacePixel] < 50);
    CHECK(maskedCapture.Pixels()[surfacePixel + 2] > 200);

    renderer.SetOriginalTexturePreview(false);
    renderer.SetProjectionPreviewMode(codextex::ProjectionPreviewMode::Full);
    renderer.RenderViewport(128, 128, camera);
    codextex::TextureImage fullCapture;
    REQUIRE(renderer.CaptureFrame(camera, fullCapture, error));
    CHECK(fullCapture.Pixels()[surfacePixel + 1] > 200);
    CHECK(fullCapture.Pixels()[surfacePixel + 2] < 50);
    const std::size_t referencePixel = pixelAt(fullCapture, 64, 64);
    CHECK(fullCapture.Pixels()[referencePixel] > 200);
    CHECK(fullCapture.Pixels()[referencePixel + 1] > 180);
    CHECK(fullCapture.Pixels()[referencePixel + 2] < 50);

    renderer.SetOriginalTexturePreview(true);
    renderer.SetProjectionPreviewMode(codextex::ProjectionPreviewMode::Disabled);
    renderer.RenderViewport(128, 128, camera);
    codextex::TextureImage originalCapture;
    REQUIRE(renderer.CaptureFrame(camera, originalCapture, error));
    CHECK(originalCapture.Pixels()[surfacePixel] > 200);
    CHECK(originalCapture.Pixels()[surfacePixel + 2] < 50);
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

TEST_CASE("WARP local X filter prevents mirrored UV sides from overwriting each other") {
    HiddenWindow window;
    REQUIRE(window.Get() != nullptr);

    codextex::Renderer renderer;
    std::string error;
    REQUIRE(renderer.Initialize(window.Get(), error, true));

    const auto directory = std::filesystem::temp_directory_path() / "codextex-tests";
    std::filesystem::create_directories(directory);
    const auto objPath = directory / "mirrored-local-x-quads.obj";
    std::ofstream obj(objPath, std::ios::binary | std::ios::trunc);
    obj << R"OBJ(
v -1.8 -0.8 0
v -0.2 -0.8 0
v -0.2  0.8 0
v -1.8  0.8 0
v  0.2 -0.8 0
v  1.8 -0.8 0
v  1.8  0.8 0
v  0.2  0.8 0
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
    REQUIRE(mesh.UvOverlapCount() > 0);
    REQUIRE(renderer.SetMesh(mesh, error));

    std::vector<std::uint8_t> basePixels(16 * 16 * 4, 255);
    for (std::size_t i = 0; i < basePixels.size(); i += 4) {
        basePixels[i] = 12;
        basePixels[i + 1] = 24;
        basePixels[i + 2] = 36;
        basePixels[i + 3] = 91;
    }
    codextex::TextureImage base;
    base.Assign(16, 16, basePixels);
    REQUIRE(renderer.SetWorkingTexture(base, error));

    codextex::CameraState camera;
    camera.pitch = 0;
    camera.distance = 5;
    renderer.RenderViewport(256, 128, camera);
    codextex::TextureImage capture;
    codextex::Renderer::ProjectionFrame savedFrame;
    REQUIRE(renderer.CaptureFrame(camera, capture, savedFrame, error));
    REQUIRE(savedFrame.Valid());
    renderer.ClearFrozenFrame();
    CHECK_FALSE(renderer.HasFrozenFrame());
    renderer.ActivateProjectionFrame(savedFrame);
    CHECK(renderer.HasFrozenFrame());
    REQUIRE(capture.Width() == 128);
    REQUIRE(capture.Height() == 128);

    std::vector<std::uint8_t> projectionPixels(128 * 128 * 4, 255);
    for (std::uint32_t y = 0; y < 128; ++y) {
        for (std::uint32_t x = 0; x < 128; ++x) {
            const std::size_t pixel = (static_cast<std::size_t>(y) * 128 + x) * 4;
            projectionPixels[pixel] = x < 64 ? 235 : 15;
            projectionPixels[pixel + 1] = 20;
            projectionPixels[pixel + 2] = x < 64 ? 15 : 235;
        }
    }
    codextex::TextureImage projection;
    projection.Assign(128, 128, projectionPixels);
    REQUIRE(renderer.SetProjectionImage(projection, error));
    codextex::MaskImage mask;
    mask.Resize(128, 128, true);
    renderer.SetMask(mask, 0);

    renderer.SetLocalSideFilter(codextex::LocalSideFilter::IgnorePositiveX);
    REQUIRE(renderer.BakeProjection(75, error));
    codextex::TextureImage negativeSideBake;
    REQUIRE(renderer.ReadWorkingTexture(negativeSideBake, error));
    const std::size_t center = (8 * 16 + 8) * 4;
    // From the default +Z camera, local -X is on the right half of the capture.
    CHECK(negativeSideBake.Pixels()[center] < 60);
    CHECK(negativeSideBake.Pixels()[center + 2] > 180);
    CHECK(negativeSideBake.Pixels()[center + 3] == 91);

    REQUIRE(renderer.SetWorkingTexture(base, error));
    renderer.SetLocalSideFilter(codextex::LocalSideFilter::IgnoreNegativeX);
    REQUIRE(renderer.BakeProjection(75, error));
    codextex::TextureImage positiveSideBake;
    REQUIRE(renderer.ReadWorkingTexture(positiveSideBake, error));
    CHECK(positiveSideBake.Pixels()[center] > 180);
    CHECK(positiveSideBake.Pixels()[center + 2] < 60);
    CHECK(positiveSideBake.Pixels()[center + 3] == 91);
    renderer.Shutdown();
}

TEST_CASE("WARP square crop prevents baking viewport content outside the frame") {
    HiddenWindow window;
    REQUIRE(window.Get() != nullptr);

    codextex::Renderer renderer;
    std::string error;
    REQUIRE(renderer.Initialize(window.Get(), error, true));
    const auto directory = std::filesystem::temp_directory_path() / "codextex-tests";
    std::filesystem::create_directories(directory);
    const auto objPath = directory / "outside-square-crop.obj";
    std::ofstream obj(objPath, std::ios::binary | std::ios::trunc);
    obj << R"OBJ(
v 2.6 -0.5 0
v 3.4 -0.5 0
v 3.4  0.5 0
v 2.6  0.5 0
vt 0 0
vt 1 0
vt 1 1
vt 0 1
f 1/1 2/2 3/3 4/4
)OBJ";
    obj.close();
    codextex::Mesh mesh;
    REQUIRE(mesh.LoadObj(objPath, error));
    REQUIRE(renderer.SetMesh(mesh, error));

    std::vector<std::uint8_t> basePixels(8 * 8 * 4, 255);
    for (std::size_t i = 0; i < basePixels.size(); i += 4) {
        basePixels[i] = 14;
        basePixels[i + 1] = 28;
        basePixels[i + 2] = 42;
        basePixels[i + 3] = 73;
    }
    codextex::TextureImage base;
    base.Assign(8, 8, basePixels);
    REQUIRE(renderer.SetWorkingTexture(base, error));

    codextex::CameraState camera;
    camera.pitch = 0;
    camera.distance = 5;
    renderer.RenderViewport(256, 128, camera);
    codextex::TextureImage capture;
    REQUIRE(renderer.CaptureFrame(camera, capture, error));
    REQUIRE(capture.Width() == 128);
    REQUIRE(capture.Height() == 128);

    std::vector<std::uint8_t> redPixels(128 * 128 * 4, 255);
    for (std::size_t i = 0; i < redPixels.size(); i += 4) {
        redPixels[i] = 240;
        redPixels[i + 1] = 10;
        redPixels[i + 2] = 10;
    }
    codextex::TextureImage projection;
    projection.Assign(128, 128, redPixels);
    REQUIRE(renderer.SetProjectionImage(projection, error));
    codextex::MaskImage mask;
    mask.Resize(128, 128, true);
    renderer.SetMask(mask, 0);
    REQUIRE(renderer.BakeProjection(75, error));

    codextex::TextureImage baked;
    REQUIRE(renderer.ReadWorkingTexture(baked, error));
    CHECK(baked.Pixels() == basePixels);
    renderer.Shutdown();
}

TEST_CASE("WARP projection shifting never paints outside the shifted image") {
    HiddenWindow window;
    REQUIRE(window.Get() != nullptr);

    codextex::Renderer renderer;
    std::string error;
    REQUIRE(renderer.Initialize(window.Get(), error, true));
    const auto directory = std::filesystem::temp_directory_path() / "codextex-tests";
    std::filesystem::create_directories(directory);
    const auto objPath = directory / "projection-shift-quad.obj";
    std::ofstream obj(objPath, std::ios::binary | std::ios::trunc);
    obj << R"OBJ(
v -1 -1 0
v  1 -1 0
v  1  1 0
v -1  1 0
vt 0 0
vt 1 0
vt 1 1
vt 0 1
f 1/1 2/2 3/3 4/4
)OBJ";
    obj.close();
    codextex::Mesh mesh;
    REQUIRE(mesh.LoadObj(objPath, error));
    REQUIRE(renderer.SetMesh(mesh, error));

    std::vector<std::uint8_t> basePixels(8 * 8 * 4, 255);
    for (std::size_t pixel = 0; pixel < basePixels.size(); pixel += 4) {
        basePixels[pixel] = 14;
        basePixels[pixel + 1] = 28;
        basePixels[pixel + 2] = 42;
        basePixels[pixel + 3] = 73;
    }
    codextex::TextureImage base;
    base.Assign(8, 8, basePixels);
    REQUIRE(renderer.SetWorkingTexture(base, error));

    codextex::CameraState camera;
    camera.pitch = 0;
    camera.distance = 5;
    codextex::TextureImage capture;
    codextex::Renderer::ProjectionFrame frame;
    REQUIRE(renderer.CaptureFrame(camera, 128, 128, capture, frame, error));
    renderer.ActivateProjectionFrame(frame);

    std::vector<std::uint8_t> projectionPixels(128 * 128 * 4, 255);
    for (std::size_t pixel = 0; pixel < projectionPixels.size(); pixel += 4) {
        projectionPixels[pixel] = 240;
        projectionPixels[pixel + 1] = 10;
        projectionPixels[pixel + 2] = 10;
    }
    codextex::TextureImage projection;
    projection.Assign(128, 128, projectionPixels);
    REQUIRE(renderer.SetProjectionImage(projection, error));
    codextex::MaskImage mask;
    mask.Resize(128, 128, true);
    renderer.SetMask(mask, 0);

    renderer.SetProjectionOffset(2.0f, 0.0f);
    REQUIRE(renderer.BakeProjection(75, error));
    codextex::TextureImage baked;
    REQUIRE(renderer.ReadWorkingTexture(baked, error));
    CHECK(baked.Pixels() == basePixels);
    renderer.Shutdown();
}
