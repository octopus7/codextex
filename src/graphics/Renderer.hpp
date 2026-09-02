#pragma once

#include "core/Mask.hpp"
#include "core/Mesh.hpp"
#include "core/TextureImage.hpp"

#include <Windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace codextex {

struct CameraState {
    Vec3 target{};
    float yaw{};
    float pitch{0.15f};
    float distance{3.0f};
    float fovDegrees{45.0f};
};

enum class LocalSideFilter : std::uint32_t {
    Both,
    IgnoreNegativeX,
    IgnorePositiveX,
};

enum class ProjectionPreviewMode : std::uint32_t {
    Disabled,
    Masked,
    Full,
};

class Renderer {
public:
    struct ProjectionFrame {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> depth;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> depthSrv;
        Microsoft::WRL::ComPtr<ID3D11Buffer> indexBuffer;
        CameraState camera{};
        std::uint32_t width{};
        std::uint32_t height{};
        std::uint32_t cropX{};
        std::uint32_t cropY{};
        std::uint32_t cropSize{};
        std::uint32_t indexCount{};

        [[nodiscard]] bool Valid() const noexcept {
            return depthSrv != nullptr && indexBuffer != nullptr && width != 0 && height != 0;
        }
    };

    bool Initialize(HWND window, std::string& error, bool forceWarp = false);
    void Shutdown();
    void Resize(std::uint32_t width, std::uint32_t height);
    void BeginUiFrame(const float clearColor[4]);
    void Present();

    bool SetMesh(const Mesh& mesh, std::string& error);
    bool SetSourceAndWorkingTexture(const TextureImage& image, std::string& error);
    bool SetWorkingTexture(const TextureImage& image, std::string& error);
    bool SetProjectionImage(const TextureImage& image, std::string& error);
    bool SetSessionPreviewImage(const TextureImage& image, std::string& error);
    void ClearSessionPreviewImage();
    bool AddReferenceAsset(const Mesh& mesh, const TextureImage& texture, std::string& error);
    void ClearReferenceAssets();
    void SetReferenceAssetsVisible(bool visible) noexcept { referenceAssetsVisible_ = visible; }
    [[nodiscard]] bool ReferenceAssetsVisible() const noexcept { return referenceAssetsVisible_; }
    void SetShadingEnabled(bool enabled) noexcept { shadingEnabled_ = enabled; }
    void SetViewportBackgroundColor(const std::array<float, 3>& color) noexcept;
    void SetOriginalTexturePreview(bool enabled) noexcept { originalTexturePreview_ = enabled; }
    void SetLocalSideFilter(LocalSideFilter filter) noexcept { localSideFilter_ = filter; }
    void SetProjectionOffset(float normalizedX, float normalizedY) noexcept {
        projectionOffset_[0] = normalizedX;
        projectionOffset_[1] = normalizedY;
    }
    void SetMask(const MaskImage& mask, int featherRadius);
    void SetHiddenFaces(std::span<const std::uint8_t> hidden);
    void SetSelectedFaces(std::span<const std::uint8_t> selected);

    void RenderViewport(std::uint32_t width, std::uint32_t height, const CameraState& camera);
    void RenderViewportRegion(std::uint32_t width, std::uint32_t height,
                              const CameraState& camera, const Vec2& minimumUv,
                              const Vec2& maximumUv);
    [[nodiscard]] ID3D11ShaderResourceView* ViewportTexture() const noexcept { return colorSrv_.Get(); }
    [[nodiscard]] ID3D11ShaderResourceView* WorkingTexture() const noexcept {
        return workingProjectionPreviewEnabled_ && workingProjectionPreviewSrv_
            ? workingProjectionPreviewSrv_.Get() : workingSrv_.Get();
    }
    [[nodiscard]] ID3D11ShaderResourceView* SessionPreviewTexture() const noexcept {
        return sessionPreviewSrv_.Get();
    }
    [[nodiscard]] std::uint32_t ViewportWidth() const noexcept { return viewportWidth_; }
    [[nodiscard]] std::uint32_t ViewportHeight() const noexcept { return viewportHeight_; }

    bool CaptureFrame(const CameraState& camera, TextureImage& image, std::string& error);
    bool CaptureFrame(const CameraState& camera, TextureImage& image, ProjectionFrame& frame,
                      std::string& error);
    bool CaptureFrame(const CameraState& camera, std::uint32_t width, std::uint32_t height,
                      TextureImage& image, ProjectionFrame& frame, std::string& error);
    void ActivateProjectionFrame(const ProjectionFrame& frame);
    bool RefreshWorkingProjectionPreview(float maxAngleDegrees, std::string& error);
    void ClearWorkingProjectionPreview() noexcept { workingProjectionPreviewEnabled_ = false; }
    [[nodiscard]] bool HasWorkingProjectionPreview() const noexcept {
        return workingProjectionPreviewEnabled_ && workingProjectionPreviewSrv_ != nullptr;
    }
    bool BakeProjection(float maxAngleDegrees, std::string& error);
    bool ReadWorkingTexture(TextureImage& image, std::string& error) const;

    std::uint32_t PickTriangle(std::uint32_t x, std::uint32_t y) const;
    std::vector<std::uint32_t> PickTrianglesInLasso(std::span<const Vec2> points) const;
    [[nodiscard]] bool HasFrozenFrame() const noexcept { return frozenDepth_ != nullptr; }
    void ClearFrozenFrame();
    void SetProjectionPreviewMode(ProjectionPreviewMode mode) noexcept {
        projectionPreviewMode_ = mode;
    }

    [[nodiscard]] ID3D11Device* Device() const noexcept { return device_.Get(); }
    [[nodiscard]] ID3D11DeviceContext* Context() const noexcept { return context_.Get(); }

private:
    bool CreateBackBuffer(std::string& error);
    bool CreateViewportTargets(std::uint32_t width, std::uint32_t height, std::string& error);
    bool CreateShaders(std::string& error);
    bool CreateBufferResources(std::string& error);
    bool CreateMaskResources(std::uint32_t width, std::uint32_t height);
    void DispatchMaskFeather(const MaskImage& mask, int featherRadius);
    bool UploadRgbaTexture(const TextureImage& image, bool renderTarget,
                           Microsoft::WRL::ComPtr<ID3D11Texture2D>& texture,
                           Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& srv,
                           Microsoft::WRL::ComPtr<ID3D11RenderTargetView>* rtv,
                           std::string& error);
    bool ReadTexture(ID3D11Texture2D* texture, DXGI_FORMAT format, TextureImage& image,
                     std::string& error) const;
    void DrawScene(std::uint32_t width, std::uint32_t height, const CameraState& camera,
                   const Vec2& minimumUv = {0.0f, 0.0f},
                   const Vec2& maximumUv = {1.0f, 1.0f});
    bool RenderProjectionToTarget(ID3D11RenderTargetView* target, float maxAngleDegrees,
                                  std::string& error);
    bool EnsureWorkingProjectionPreview(std::string& error);
    std::vector<std::uint32_t> ReadIdBuffer() const;
    void UpdateVisibleIndexBuffer();
    void UpdateSelectionBuffer();
    void ComputeMatrices(const CameraState& camera, float aspect, float worldViewProjection[16],
                         float world[16], float cameraPosition[4],
                         const Vec2& minimumUv = {0.0f, 0.0f},
                         const Vec2& maximumUv = {1.0f, 1.0f}) const;

    HWND window_{};
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGISwapChain> swapChain_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> backBufferRtv_;

    Microsoft::WRL::ComPtr<ID3D11VertexShader> viewportVs_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> viewportPs_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> referencePs_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> bakeVs_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> bakePs_;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> maskInitCs_;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> maskJumpCs_;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> maskFinalizeCs_;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> inputLayout_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> constants_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> maskConstants_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterizer_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> bakeBlend_;

    Microsoft::WRL::ComPtr<ID3D11Buffer> vertexBuffer_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> visibleIndexBuffer_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> frozenIndexBuffer_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> selectionBuffer_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> selectionSrv_;

    struct ReferenceGpuAsset {
        Microsoft::WRL::ComPtr<ID3D11Buffer> vertexBuffer;
        Microsoft::WRL::ComPtr<ID3D11Buffer> indexBuffer;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> textureSrv;
        std::uint32_t indexCount{};
    };
    std::vector<ReferenceGpuAsset> referenceAssets_;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> colorTexture_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> colorRtv_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> colorSrv_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> idTexture_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> idRtv_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> normalTexture_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> normalRtv_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> depthTexture_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> depthDsv_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> depthSrv_;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> workingTexture_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> workingSrv_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> workingRtv_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> workingProjectionPreviewTexture_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> workingProjectionPreviewSrv_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> workingProjectionPreviewRtv_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> originalTexture_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> originalSrv_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> projectionTexture_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> projectionSrv_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> sessionPreviewTexture_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> sessionPreviewSrv_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> maskBinaryTexture_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> maskBinarySrv_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> maskTexture_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> maskSrv_;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> maskUav_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> maskSeedTextures_[2];
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> maskSeedSrvs_[2];
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> maskSeedUavs_[2];
    std::uint32_t maskWidth_{};
    std::uint32_t maskHeight_{};

    Microsoft::WRL::ComPtr<ID3D11Texture2D> frozenColor_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> frozenDepth_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> frozenDepthSrv_;
    CameraState frozenCamera_{};
    std::uint32_t frozenWidth_{};
    std::uint32_t frozenHeight_{};
    std::uint32_t frozenCropX_{};
    std::uint32_t frozenCropY_{};
    std::uint32_t frozenCropSize_{};
    std::uint32_t frozenIndexCount_{};

    std::vector<Vertex> vertices_;
    std::vector<std::uint32_t> allIndices_;
    std::vector<std::uint32_t> visibleIndices_;
    std::vector<std::uint8_t> hiddenFaces_;
    std::vector<std::uint8_t> selectedFaces_;
    std::uint32_t viewportWidth_{};
    std::uint32_t viewportHeight_{};
    std::uint32_t textureWidth_{};
    std::uint32_t textureHeight_{};
    ProjectionPreviewMode projectionPreviewMode_{ProjectionPreviewMode::Disabled};
    bool referenceAssetsVisible_{true};
    bool shadingEnabled_{};
    bool originalTexturePreview_{};
    bool workingProjectionPreviewEnabled_{};
    std::array<float, 4> viewportBackgroundColor_{0.0467f, 0.0732f, 0.1070f, 1.0f};
    LocalSideFilter localSideFilter_{LocalSideFilter::Both};
    float localCenterX_{};
    std::array<float, 2> projectionOffset_{};
};

} // namespace codextex
