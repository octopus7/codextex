#include "graphics/Renderer.hpp"

#include <DirectXMath.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <numbers>
#include <unordered_set>

using Microsoft::WRL::ComPtr;

namespace codextex {
namespace {

struct ShaderConstants {
    float worldViewProjection[16]{};
    float world[16]{};
    float cameraPosition[4]{};
    float parameters[4]{};
};

struct MaskConstants {
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t step{};
    std::uint32_t featherRadius{};
};

constexpr char kViewportShader[] = R"HLSL(
cbuffer Constants : register(b0) {
    row_major float4x4 worldViewProjection;
    row_major float4x4 world;
    float4 cameraPosition;
    float4 parameters;
};
Texture2D<float4> baseColor : register(t0);
StructuredBuffer<uint> selectedFaces : register(t1);
Texture2D<float4> projectionPreview : register(t2);
Texture2D<float> maskPreview : register(t3);
SamplerState linearSampler : register(s0);

struct VSInput {
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
    uint triangleId : TEXCOORD1;
};
struct VSOutput {
    float4 position : SV_POSITION;
    float3 worldPosition : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 uv : TEXCOORD2;
    nointerpolation uint triangleId : TEXCOORD3;
};
VSOutput VSMain(VSInput input) {
    VSOutput output;
    const float4 worldPosition = mul(float4(input.position, 1.0), world);
    output.position = mul(float4(input.position, 1.0), worldViewProjection);
    output.worldPosition = worldPosition.xyz;
    output.normal = normalize(mul(float4(input.normal, 0.0), world).xyz);
    output.uv = input.uv;
    output.triangleId = input.triangleId;
    return output;
}
struct PSOutput {
    float4 color : SV_TARGET0;
    uint triangleId : SV_TARGET1;
    float4 normal : SV_TARGET2;
};
PSOutput PSMain(VSOutput input) {
    PSOutput output;
    const float3 albedo = baseColor.Sample(linearSampler, input.uv).rgb;
    const float3 lightDirection = normalize(float3(-0.35, 0.75, -0.55));
    const float diffuse = saturate(dot(normalize(input.normal), lightDirection));
    float3 color = albedo * (0.42 + diffuse * 0.58);
    if (parameters.z > 0.5) {
        const float2 screenUv = input.position.xy / parameters.xy;
        const float mask = maskPreview.SampleLevel(linearSampler, screenUv, 0);
        const float3 projected = projectionPreview.SampleLevel(linearSampler, screenUv, 0).rgb;
        color = lerp(color, projected, mask * 0.82);
    }
    if (selectedFaces[input.triangleId] != 0) {
        color = lerp(color, float3(1.0, 0.55, 0.05), 0.55);
    }
    output.color = float4(color, 1.0);
    output.triangleId = input.triangleId + 1;
    output.normal = float4(normalize(input.normal) * 0.5 + 0.5, 1.0);
    return output;
}
)HLSL";

constexpr char kBakeShader[] = R"HLSL(
cbuffer Constants : register(b0) {
    row_major float4x4 worldViewProjection;
    row_major float4x4 world;
    float4 cameraPosition;
    float4 parameters;
};
Texture2D<float4> projectionImage : register(t0);
Texture2D<float> capturedDepth : register(t1);
Texture2D<float> projectionMask : register(t2);
SamplerState linearSampler : register(s0);

struct VSInput {
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
    uint triangleId : TEXCOORD1;
};
struct VSOutput {
    float4 position : SV_POSITION;
    float4 captureClip : TEXCOORD0;
    float3 worldPosition : TEXCOORD1;
    float3 normal : TEXCOORD2;
};
VSOutput VSMain(VSInput input) {
    VSOutput output;
    output.position = float4(input.uv.x * 2.0 - 1.0, 1.0 - input.uv.y * 2.0, 0.0, 1.0);
    output.captureClip = mul(float4(input.position, 1.0), worldViewProjection);
    output.worldPosition = mul(float4(input.position, 1.0), world).xyz;
    output.normal = normalize(mul(float4(input.normal, 0.0), world).xyz);
    return output;
}
float4 PSMain(VSOutput input) : SV_TARGET0 {
    if (input.captureClip.w <= 0.0) discard;
    const float3 ndc = input.captureClip.xyz / input.captureClip.w;
    const float2 screenUv = float2(ndc.x * 0.5 + 0.5, -ndc.y * 0.5 + 0.5);
    if (any(screenUv < 0.0) || any(screenUv > 1.0)) discard;
    const float sampledDepth = capturedDepth.SampleLevel(linearSampler, screenUv, 0);
    if (abs(sampledDepth - ndc.z) > parameters.w) discard;
    const float3 viewDirection = normalize(cameraPosition.xyz - input.worldPosition);
    if (dot(normalize(input.normal), viewDirection) < parameters.z) discard;
    const float mask = projectionMask.SampleLevel(linearSampler, screenUv, 0);
    if (mask <= 0.0001) discard;
    const float3 generated = projectionImage.SampleLevel(linearSampler, screenUv, 0).rgb;
    return float4(generated, mask);
}
)HLSL";

constexpr char kMaskComputeShader[] = R"HLSL(
cbuffer MaskConstants : register(b0) {
    uint maskWidth;
    uint maskHeight;
    uint jumpStep;
    uint featherRadius;
};

Texture2D<float> binaryMask : register(t0);
Texture2D<int2> inputSeeds : register(t1);
RWTexture2D<int2> outputSeeds : register(u0);
RWTexture2D<float> featherMask : register(u1);

[numthreads(8, 8, 1)]
void InitSeeds(uint3 dispatchId : SV_DispatchThreadID) {
    const uint2 pixel = dispatchId.xy;
    if (pixel.x >= maskWidth || pixel.y >= maskHeight) return;
    if (binaryMask[pixel] < 0.5) {
        outputSeeds[pixel] = int2(pixel);
        return;
    }
    if (pixel.x == 0) outputSeeds[pixel] = int2(-1, pixel.y);
    else if (pixel.x + 1 == maskWidth) outputSeeds[pixel] = int2(maskWidth, pixel.y);
    else if (pixel.y == 0) outputSeeds[pixel] = int2(pixel.x, -1);
    else if (pixel.y + 1 == maskHeight) outputSeeds[pixel] = int2(pixel.x, maskHeight);
    else outputSeeds[pixel] = int2(-32768, -32768);
}

[numthreads(8, 8, 1)]
void JumpFlood(uint3 dispatchId : SV_DispatchThreadID) {
    const uint2 pixel = dispatchId.xy;
    if (pixel.x >= maskWidth || pixel.y >= maskHeight) return;
    int2 best = inputSeeds[pixel];
    int bestDistance = 0x7fffffff;
    if (best.x > -32768) {
        const int2 delta = best - int2(pixel);
        bestDistance = dot(delta, delta);
    }
    [unroll]
    for (int oy = -1; oy <= 1; ++oy) {
        [unroll]
        for (int ox = -1; ox <= 1; ++ox) {
            const int2 samplePixel = int2(pixel) + int2(ox, oy) * int(jumpStep);
            if (samplePixel.x < 0 || samplePixel.y < 0 ||
                samplePixel.x >= int(maskWidth) || samplePixel.y >= int(maskHeight)) continue;
            const int2 candidate = inputSeeds[samplePixel];
            if (candidate.x <= -32768) continue;
            const int2 delta = candidate - int2(pixel);
            const int distance = dot(delta, delta);
            if (distance < bestDistance) {
                bestDistance = distance;
                best = candidate;
            }
        }
    }
    outputSeeds[pixel] = best;
}

[numthreads(8, 8, 1)]
void FinalizeMask(uint3 dispatchId : SV_DispatchThreadID) {
    const uint2 pixel = dispatchId.xy;
    if (pixel.x >= maskWidth || pixel.y >= maskHeight) return;
    if (binaryMask[pixel] < 0.5) {
        featherMask[pixel] = 0.0;
        return;
    }
    if (featherRadius == 0) {
        featherMask[pixel] = 1.0;
        return;
    }
    const int2 seed = inputSeeds[pixel];
    if (seed.x <= -32768) {
        featherMask[pixel] = 1.0;
        return;
    }
    const float2 delta = float2(seed - int2(pixel));
    featherMask[pixel] = saturate(length(delta) / float(featherRadius));
}
)HLSL";

std::string HrError(const char* action, const HRESULT hr) {
    return std::string(action) + " (HRESULT " + std::to_string(static_cast<unsigned long>(hr)) + ").";
}

bool PointInside(const float x, const float y, const std::span<const Vec2> polygon) {
    if (polygon.size() < 3) return false;
    bool inside = false;
    for (std::size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++) {
        const Vec2& a = polygon[i];
        const Vec2& b = polygon[j];
        if (((a.y > y) != (b.y > y)) &&
            x < (b.x - a.x) * (y - a.y) / ((b.y - a.y) + 1.0e-20f) + a.x) {
            inside = !inside;
        }
    }
    return inside;
}

HRESULT CompileShader(const char* source, const char* entry, const char* target,
                      ComPtr<ID3DBlob>& output, std::string& error) {
    ComPtr<ID3DBlob> errors;
    const HRESULT hr = D3DCompile(source, std::strlen(source), nullptr, nullptr, nullptr, entry, target,
                                  D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_PACK_MATRIX_ROW_MAJOR,
                                  0, output.GetAddressOf(), errors.GetAddressOf());
    if (FAILED(hr)) {
        error = errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()),
                                     errors->GetBufferSize())
                       : HrError("Shader compilation failed", hr);
    }
    return hr;
}

} // namespace

bool Renderer::Initialize(HWND window, std::string& error, const bool forceWarp) {
    window_ = window;
    RECT client{};
    GetClientRect(window, &client);
    DXGI_SWAP_CHAIN_DESC swapDesc{};
    swapDesc.BufferCount = 2;
    swapDesc.BufferDesc.Width = std::max<LONG>(client.right - client.left, 1);
    swapDesc.BufferDesc.Height = std::max<LONG>(client.bottom - client.top, 1);
    swapDesc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapDesc.OutputWindow = window;
    swapDesc.SampleDesc.Count = 1;
    swapDesc.Windowed = TRUE;
    swapDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    D3D_FEATURE_LEVEL featureLevel{};
    constexpr std::array requested{D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    UINT flags = 0;
#if defined(_DEBUG)
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    HRESULT hr = E_FAIL;
    if (!forceWarp) {
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, requested.data(),
            static_cast<UINT>(requested.size()), D3D11_SDK_VERSION, &swapDesc,
            swapChain_.GetAddressOf(), device_.GetAddressOf(), &featureLevel,
            context_.GetAddressOf());
    }
    if (forceWarp || FAILED(hr)) {
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags & ~D3D11_CREATE_DEVICE_DEBUG, requested.data(),
            static_cast<UINT>(requested.size()), D3D11_SDK_VERSION, &swapDesc,
            swapChain_.ReleaseAndGetAddressOf(), device_.ReleaseAndGetAddressOf(), &featureLevel,
            context_.ReleaseAndGetAddressOf());
    }
    if (FAILED(hr)) {
        error = HrError("Could not create a Direct3D 11 device", hr);
        return false;
    }
    if (!CreateBackBuffer(error) || !CreateShaders(error) || !CreateBufferResources(error)) {
        return false;
    }
    return true;
}

void Renderer::Shutdown() {
    if (context_) context_->ClearState();
    *this = Renderer{};
}

bool Renderer::CreateBackBuffer(std::string& error) {
    ComPtr<ID3D11Texture2D> backBuffer;
    HRESULT hr = swapChain_->GetBuffer(0, IID_PPV_ARGS(backBuffer.GetAddressOf()));
    if (SUCCEEDED(hr)) {
        hr = device_->CreateRenderTargetView(backBuffer.Get(), nullptr, backBufferRtv_.GetAddressOf());
    }
    if (FAILED(hr)) {
        error = HrError("Could not create the swap-chain render target", hr);
        return false;
    }
    return true;
}

void Renderer::Resize(const std::uint32_t width, const std::uint32_t height) {
    if (!swapChain_ || width == 0 || height == 0) return;
    context_->OMSetRenderTargets(0, nullptr, nullptr);
    backBufferRtv_.Reset();
    if (SUCCEEDED(swapChain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0))) {
        std::string ignored;
        CreateBackBuffer(ignored);
    }
}

void Renderer::BeginUiFrame(const float clearColor[4]) {
    context_->OMSetRenderTargets(1, backBufferRtv_.GetAddressOf(), nullptr);
    context_->ClearRenderTargetView(backBufferRtv_.Get(), clearColor);
}

void Renderer::Present() {
    swapChain_->Present(1, 0);
}

bool Renderer::CreateShaders(std::string& error) {
    ComPtr<ID3DBlob> viewportVs;
    ComPtr<ID3DBlob> viewportPs;
    ComPtr<ID3DBlob> bakeVs;
    ComPtr<ID3DBlob> bakePs;
    ComPtr<ID3DBlob> maskInitCs;
    ComPtr<ID3DBlob> maskJumpCs;
    ComPtr<ID3DBlob> maskFinalizeCs;
    if (FAILED(CompileShader(kViewportShader, "VSMain", "vs_5_0", viewportVs, error)) ||
        FAILED(CompileShader(kViewportShader, "PSMain", "ps_5_0", viewportPs, error)) ||
        FAILED(CompileShader(kBakeShader, "VSMain", "vs_5_0", bakeVs, error)) ||
        FAILED(CompileShader(kBakeShader, "PSMain", "ps_5_0", bakePs, error)) ||
        FAILED(CompileShader(kMaskComputeShader, "InitSeeds", "cs_5_0", maskInitCs, error)) ||
        FAILED(CompileShader(kMaskComputeShader, "JumpFlood", "cs_5_0", maskJumpCs, error)) ||
        FAILED(CompileShader(kMaskComputeShader, "FinalizeMask", "cs_5_0", maskFinalizeCs, error))) {
        return false;
    }
    HRESULT hr = device_->CreateVertexShader(viewportVs->GetBufferPointer(), viewportVs->GetBufferSize(),
                                              nullptr, viewportVs_.GetAddressOf());
    if (SUCCEEDED(hr)) hr = device_->CreatePixelShader(viewportPs->GetBufferPointer(), viewportPs->GetBufferSize(),
                                                       nullptr, viewportPs_.GetAddressOf());
    if (SUCCEEDED(hr)) hr = device_->CreateVertexShader(bakeVs->GetBufferPointer(), bakeVs->GetBufferSize(),
                                                        nullptr, bakeVs_.GetAddressOf());
    if (SUCCEEDED(hr)) hr = device_->CreatePixelShader(bakePs->GetBufferPointer(), bakePs->GetBufferSize(),
                                                       nullptr, bakePs_.GetAddressOf());
    if (SUCCEEDED(hr)) hr = device_->CreateComputeShader(maskInitCs->GetBufferPointer(),
                                                         maskInitCs->GetBufferSize(), nullptr,
                                                         maskInitCs_.GetAddressOf());
    if (SUCCEEDED(hr)) hr = device_->CreateComputeShader(maskJumpCs->GetBufferPointer(),
                                                         maskJumpCs->GetBufferSize(), nullptr,
                                                         maskJumpCs_.GetAddressOf());
    if (SUCCEEDED(hr)) hr = device_->CreateComputeShader(maskFinalizeCs->GetBufferPointer(),
                                                         maskFinalizeCs->GetBufferSize(), nullptr,
                                                         maskFinalizeCs_.GetAddressOf());
    const std::array<D3D11_INPUT_ELEMENT_DESC, 4> elements{{
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 1, DXGI_FORMAT_R32_UINT, 0, 32, D3D11_INPUT_PER_VERTEX_DATA, 0},
    }};
    if (SUCCEEDED(hr)) {
        hr = device_->CreateInputLayout(elements.data(), static_cast<UINT>(elements.size()),
                                        viewportVs->GetBufferPointer(), viewportVs->GetBufferSize(),
                                        inputLayout_.GetAddressOf());
    }
    if (FAILED(hr)) {
        error = HrError("Could not create shaders", hr);
        return false;
    }
    return true;
}

bool Renderer::CreateBufferResources(std::string& error) {
    D3D11_BUFFER_DESC constantsDesc{};
    constantsDesc.ByteWidth = sizeof(ShaderConstants);
    constantsDesc.Usage = D3D11_USAGE_DYNAMIC;
    constantsDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    constantsDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    HRESULT hr = device_->CreateBuffer(&constantsDesc, nullptr, constants_.GetAddressOf());

    D3D11_BUFFER_DESC maskConstantsDesc = constantsDesc;
    maskConstantsDesc.ByteWidth = sizeof(MaskConstants);
    if (SUCCEEDED(hr)) hr = device_->CreateBuffer(&maskConstantsDesc, nullptr,
                                                  maskConstants_.GetAddressOf());

    D3D11_SAMPLER_DESC samplerDesc{};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
    if (SUCCEEDED(hr)) hr = device_->CreateSamplerState(&samplerDesc, sampler_.GetAddressOf());

    D3D11_RASTERIZER_DESC rasterDesc{};
    rasterDesc.FillMode = D3D11_FILL_SOLID;
    rasterDesc.CullMode = D3D11_CULL_NONE;
    rasterDesc.DepthClipEnable = TRUE;
    rasterDesc.MultisampleEnable = FALSE;
    if (SUCCEEDED(hr)) hr = device_->CreateRasterizerState(&rasterDesc, rasterizer_.GetAddressOf());

    D3D11_BLEND_DESC blendDesc{};
    auto& target = blendDesc.RenderTarget[0];
    target.BlendEnable = TRUE;
    target.SrcBlend = D3D11_BLEND_SRC_ALPHA;
    target.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    target.BlendOp = D3D11_BLEND_OP_ADD;
    target.SrcBlendAlpha = D3D11_BLEND_ZERO;
    target.DestBlendAlpha = D3D11_BLEND_ONE;
    target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (SUCCEEDED(hr)) hr = device_->CreateBlendState(&blendDesc, bakeBlend_.GetAddressOf());
    if (FAILED(hr)) {
        error = HrError("Could not create D3D11 pipeline state", hr);
        return false;
    }
    return true;
}

bool Renderer::SetMesh(const Mesh& mesh, std::string& error) {
    vertices_ = mesh.Vertices();
    allIndices_ = mesh.Indices();
    hiddenFaces_.assign(mesh.TriangleCount(), 0);
    selectedFaces_.assign(mesh.TriangleCount(), 0);
    if (vertices_.empty()) {
        error = "Mesh contains no vertices.";
        return false;
    }
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = static_cast<UINT>(vertices_.size() * sizeof(Vertex));
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA data{vertices_.data(), 0, 0};
    const HRESULT hr = device_->CreateBuffer(&desc, &data, vertexBuffer_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
        error = HrError("Could not upload the OBJ vertex buffer", hr);
        return false;
    }
    UpdateVisibleIndexBuffer();
    UpdateSelectionBuffer();
    return true;
}

void Renderer::UpdateVisibleIndexBuffer() {
    visibleIndices_.clear();
    visibleIndices_.reserve(allIndices_.size());
    for (std::size_t triangle = 0; triangle < allIndices_.size() / 3; ++triangle) {
        if (triangle < hiddenFaces_.size() && hiddenFaces_[triangle]) continue;
        visibleIndices_.insert(visibleIndices_.end(), allIndices_.begin() + triangle * 3,
                               allIndices_.begin() + triangle * 3 + 3);
    }
    visibleIndexBuffer_.Reset();
    if (visibleIndices_.empty()) return;
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = static_cast<UINT>(visibleIndices_.size() * sizeof(std::uint32_t));
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA data{visibleIndices_.data(), 0, 0};
    device_->CreateBuffer(&desc, &data, visibleIndexBuffer_.GetAddressOf());
}

void Renderer::UpdateSelectionBuffer() {
    std::vector<std::uint32_t> values(std::max<std::size_t>(selectedFaces_.size(), 1), 0);
    for (std::size_t i = 0; i < selectedFaces_.size(); ++i) values[i] = selectedFaces_[i] ? 1u : 0u;
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = static_cast<UINT>(values.size() * sizeof(std::uint32_t));
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    desc.StructureByteStride = sizeof(std::uint32_t);
    D3D11_SUBRESOURCE_DATA data{values.data(), 0, 0};
    selectionBuffer_.Reset();
    selectionSrv_.Reset();
    if (FAILED(device_->CreateBuffer(&desc, &data, selectionBuffer_.GetAddressOf()))) return;
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = DXGI_FORMAT_UNKNOWN;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    srvDesc.Buffer.NumElements = static_cast<UINT>(values.size());
    device_->CreateShaderResourceView(selectionBuffer_.Get(), &srvDesc, selectionSrv_.GetAddressOf());
}

void Renderer::SetHiddenFaces(const std::span<const std::uint8_t> hidden) {
    hiddenFaces_.assign(hidden.begin(), hidden.end());
    hiddenFaces_.resize(allIndices_.size() / 3, 0);
    UpdateVisibleIndexBuffer();
}

void Renderer::SetSelectedFaces(const std::span<const std::uint8_t> selected) {
    selectedFaces_.assign(selected.begin(), selected.end());
    selectedFaces_.resize(allIndices_.size() / 3, 0);
    UpdateSelectionBuffer();
}

bool Renderer::UploadRgbaTexture(const TextureImage& image, const bool renderTarget,
                                 ComPtr<ID3D11Texture2D>& texture, ComPtr<ID3D11ShaderResourceView>& srv,
                                 ComPtr<ID3D11RenderTargetView>* rtv, std::string& error) {
    if (image.Empty()) {
        error = "PNG contains no pixels.";
        return false;
    }
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = image.Width();
    desc.Height = image.Height();
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | (renderTarget ? D3D11_BIND_RENDER_TARGET : 0);
    D3D11_SUBRESOURCE_DATA data{image.Pixels().data(), image.Width() * 4, 0};
    HRESULT hr = device_->CreateTexture2D(&desc, &data, texture.ReleaseAndGetAddressOf());
    if (SUCCEEDED(hr)) hr = device_->CreateShaderResourceView(texture.Get(), nullptr, srv.ReleaseAndGetAddressOf());
    if (SUCCEEDED(hr) && rtv) hr = device_->CreateRenderTargetView(texture.Get(), nullptr, rtv->ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
        error = HrError("Could not upload PNG to the GPU", hr);
        return false;
    }
    return true;
}

bool Renderer::SetWorkingTexture(const TextureImage& image, std::string& error) {
    if (!UploadRgbaTexture(image, true, workingTexture_, workingSrv_, &workingRtv_, error)) return false;
    textureWidth_ = image.Width();
    textureHeight_ = image.Height();
    return true;
}

bool Renderer::SetProjectionImage(const TextureImage& image, std::string& error) {
    return UploadRgbaTexture(image, false, projectionTexture_, projectionSrv_, nullptr, error);
}

bool Renderer::CreateMaskResources(const std::uint32_t width, const std::uint32_t height) {
    maskBinaryTexture_.Reset();
    maskBinarySrv_.Reset();
    maskTexture_.Reset();
    maskSrv_.Reset();
    maskUav_.Reset();
    for (int i = 0; i < 2; ++i) {
        maskSeedTextures_[i].Reset();
        maskSeedSrvs_[i].Reset();
        maskSeedUavs_[i].Reset();
    }
    maskWidth_ = maskHeight_ = 0;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.Format = DXGI_FORMAT_R8_UNORM;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    HRESULT hr = device_->CreateTexture2D(&desc, nullptr, maskBinaryTexture_.GetAddressOf());
    if (SUCCEEDED(hr)) {
        hr = device_->CreateShaderResourceView(maskBinaryTexture_.Get(), nullptr,
                                               maskBinarySrv_.GetAddressOf());
    }

    desc.Format = DXGI_FORMAT_R32_FLOAT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    if (SUCCEEDED(hr)) hr = device_->CreateTexture2D(&desc, nullptr, maskTexture_.GetAddressOf());
    if (SUCCEEDED(hr)) hr = device_->CreateShaderResourceView(maskTexture_.Get(), nullptr,
                                                              maskSrv_.GetAddressOf());
    if (SUCCEEDED(hr)) hr = device_->CreateUnorderedAccessView(maskTexture_.Get(), nullptr,
                                                               maskUav_.GetAddressOf());

    desc.Format = DXGI_FORMAT_R32G32_SINT;
    for (int i = 0; i < 2 && SUCCEEDED(hr); ++i) {
        hr = device_->CreateTexture2D(&desc, nullptr, maskSeedTextures_[i].GetAddressOf());
        if (SUCCEEDED(hr)) hr = device_->CreateShaderResourceView(maskSeedTextures_[i].Get(), nullptr,
                                                                  maskSeedSrvs_[i].GetAddressOf());
        if (SUCCEEDED(hr)) hr = device_->CreateUnorderedAccessView(maskSeedTextures_[i].Get(), nullptr,
                                                                   maskSeedUavs_[i].GetAddressOf());
    }
    if (FAILED(hr)) {
        maskBinaryTexture_.Reset();
        maskBinarySrv_.Reset();
        maskTexture_.Reset();
        maskSrv_.Reset();
        maskUav_.Reset();
        return false;
    }
    maskWidth_ = width;
    maskHeight_ = height;
    return true;
}

void Renderer::DispatchMaskFeather(const MaskImage& mask, const int featherRadius) {
    context_->UpdateSubresource(maskBinaryTexture_.Get(), 0, nullptr, mask.Binary().data(),
                                mask.Width(), 0);
    const UINT groupsX = (mask.Width() + 7) / 8;
    const UINT groupsY = (mask.Height() + 7) / 8;

    const auto updateConstants = [&](const std::uint32_t step) {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context_->Map(maskConstants_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            return false;
        }
        const MaskConstants constants{mask.Width(), mask.Height(), step,
                                      static_cast<std::uint32_t>(std::clamp(featherRadius, 0, 128))};
        std::memcpy(mapped.pData, &constants, sizeof(constants));
        context_->Unmap(maskConstants_.Get(), 0);
        return true;
    };
    if (!updateConstants(0)) return;

    context_->CSSetConstantBuffers(0, 1, maskConstants_.GetAddressOf());
    context_->CSSetShader(maskInitCs_.Get(), nullptr, 0);
    context_->CSSetShaderResources(0, 1, maskBinarySrv_.GetAddressOf());
    context_->CSSetUnorderedAccessViews(0, 1, maskSeedUavs_[0].GetAddressOf(), nullptr);
    context_->Dispatch(groupsX, groupsY, 1);

    ID3D11ShaderResourceView* nullSrvs[2]{};
    ID3D11UnorderedAccessView* nullUavs[2]{};
    context_->CSSetUnorderedAccessViews(0, 1, nullUavs, nullptr);
    context_->CSSetShaderResources(0, 2, nullSrvs);

    std::uint32_t step = 1;
    while (step < std::max(mask.Width(), mask.Height())) step <<= 1;
    step >>= 1;
    int source = 0;
    while (step > 0) {
        if (!updateConstants(step)) return;
        const int destination = 1 - source;
        context_->CSSetShader(maskJumpCs_.Get(), nullptr, 0);
        context_->CSSetShaderResources(1, 1, maskSeedSrvs_[source].GetAddressOf());
        context_->CSSetUnorderedAccessViews(0, 1, maskSeedUavs_[destination].GetAddressOf(), nullptr);
        context_->Dispatch(groupsX, groupsY, 1);
        context_->CSSetUnorderedAccessViews(0, 1, nullUavs, nullptr);
        context_->CSSetShaderResources(1, 1, nullSrvs);
        source = destination;
        step >>= 1;
    }

    if (!updateConstants(0)) return;
    ID3D11ShaderResourceView* finalSrvs[]{maskBinarySrv_.Get(), maskSeedSrvs_[source].Get()};
    context_->CSSetShader(maskFinalizeCs_.Get(), nullptr, 0);
    context_->CSSetShaderResources(0, 2, finalSrvs);
    context_->CSSetUnorderedAccessViews(1, 1, maskUav_.GetAddressOf(), nullptr);
    context_->Dispatch(groupsX, groupsY, 1);
    context_->CSSetUnorderedAccessViews(1, 1, nullUavs, nullptr);
    context_->CSSetShaderResources(0, 2, nullSrvs);
    context_->CSSetShader(nullptr, nullptr, 0);
}

void Renderer::SetMask(const MaskImage& mask, const int featherRadius) {
    if (mask.Width() == 0 || mask.Height() == 0 || mask.Binary().empty()) {
        maskBinaryTexture_.Reset();
        maskBinarySrv_.Reset();
        maskTexture_.Reset();
        maskSrv_.Reset();
        maskUav_.Reset();
        maskWidth_ = maskHeight_ = 0;
        return;
    }
    if ((mask.Width() != maskWidth_ || mask.Height() != maskHeight_) &&
        !CreateMaskResources(mask.Width(), mask.Height())) {
        return;
    }
    DispatchMaskFeather(mask, featherRadius);
}

bool Renderer::CreateViewportTargets(const std::uint32_t width, const std::uint32_t height,
                                     std::string& error) {
    colorTexture_.Reset(); colorRtv_.Reset(); colorSrv_.Reset();
    idTexture_.Reset(); idRtv_.Reset();
    normalTexture_.Reset(); normalRtv_.Reset();
    depthTexture_.Reset(); depthDsv_.Reset(); depthSrv_.Reset();

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    HRESULT hr = device_->CreateTexture2D(&desc, nullptr, colorTexture_.GetAddressOf());
    if (SUCCEEDED(hr)) hr = device_->CreateRenderTargetView(colorTexture_.Get(), nullptr, colorRtv_.GetAddressOf());
    if (SUCCEEDED(hr)) hr = device_->CreateShaderResourceView(colorTexture_.Get(), nullptr, colorSrv_.GetAddressOf());

    desc.Format = DXGI_FORMAT_R32_UINT;
    if (SUCCEEDED(hr)) hr = device_->CreateTexture2D(&desc, nullptr, idTexture_.GetAddressOf());
    if (SUCCEEDED(hr)) hr = device_->CreateRenderTargetView(idTexture_.Get(), nullptr, idRtv_.GetAddressOf());

    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    if (SUCCEEDED(hr)) hr = device_->CreateTexture2D(&desc, nullptr, normalTexture_.GetAddressOf());
    if (SUCCEEDED(hr)) hr = device_->CreateRenderTargetView(normalTexture_.Get(), nullptr, normalRtv_.GetAddressOf());

    desc.Format = DXGI_FORMAT_R32_TYPELESS;
    desc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    if (SUCCEEDED(hr)) hr = device_->CreateTexture2D(&desc, nullptr, depthTexture_.GetAddressOf());
    D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
    dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
    dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    if (SUCCEEDED(hr)) hr = device_->CreateDepthStencilView(depthTexture_.Get(), &dsvDesc, depthDsv_.GetAddressOf());
    D3D11_SHADER_RESOURCE_VIEW_DESC depthSrvDesc{};
    depthSrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
    depthSrvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    depthSrvDesc.Texture2D.MipLevels = 1;
    if (SUCCEEDED(hr)) hr = device_->CreateShaderResourceView(depthTexture_.Get(), &depthSrvDesc, depthSrv_.GetAddressOf());
    if (FAILED(hr)) {
        error = HrError("Could not create viewport render targets", hr);
        return false;
    }
    viewportWidth_ = width;
    viewportHeight_ = height;
    return true;
}

void Renderer::ComputeMatrices(const CameraState& camera, const float aspect,
                               float worldViewProjection[16], float world[16],
                               float cameraPosition[4]) const {
    using namespace DirectX;
    const float cp = std::cos(camera.pitch);
    const XMVECTOR target = XMVectorSet(camera.target.x, camera.target.y, camera.target.z, 1.0f);
    const XMVECTOR eye = XMVectorSet(
        camera.target.x + std::sin(camera.yaw) * cp * camera.distance,
        camera.target.y + std::sin(camera.pitch) * camera.distance,
        camera.target.z + std::cos(camera.yaw) * cp * camera.distance, 1.0f);
    const XMMATRIX worldMatrix = XMMatrixIdentity();
    const XMMATRIX view = XMMatrixLookAtLH(eye, target, XMVectorSet(0, 1, 0, 0));
    const XMMATRIX projection = XMMatrixPerspectiveFovLH(
        camera.fovDegrees * std::numbers::pi_v<float> / 180.0f, std::max(aspect, 0.01f),
        std::max(camera.distance * 0.001f, 0.001f), std::max(camera.distance * 10.0f, 100.0f));
    XMFLOAT4X4 matrix{};
    XMStoreFloat4x4(&matrix, worldMatrix * view * projection);
    std::memcpy(worldViewProjection, &matrix, sizeof(matrix));
    XMStoreFloat4x4(&matrix, worldMatrix);
    std::memcpy(world, &matrix, sizeof(matrix));
    XMFLOAT4 eyeValue{};
    XMStoreFloat4(&eyeValue, eye);
    cameraPosition[0] = eyeValue.x;
    cameraPosition[1] = eyeValue.y;
    cameraPosition[2] = eyeValue.z;
    cameraPosition[3] = 1.0f;
}

void Renderer::RenderViewport(const std::uint32_t width, const std::uint32_t height,
                              const CameraState& camera) {
    if (!device_ || width == 0 || height == 0) return;
    if (width != viewportWidth_ || height != viewportHeight_) {
        std::string ignored;
        if (!CreateViewportTargets(width, height, ignored)) return;
    }
    const std::array<float, 4> clear{0.075f, 0.08f, 0.09f, 1.0f};
    const std::array<float, 4> zero{};
    ID3D11RenderTargetView* targets[]{colorRtv_.Get(), idRtv_.Get(), normalRtv_.Get()};
    context_->OMSetRenderTargets(3, targets, depthDsv_.Get());
    context_->ClearRenderTargetView(colorRtv_.Get(), clear.data());
    context_->ClearRenderTargetView(idRtv_.Get(), zero.data());
    context_->ClearRenderTargetView(normalRtv_.Get(), zero.data());
    context_->ClearDepthStencilView(depthDsv_.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
    if (!vertexBuffer_ || !visibleIndexBuffer_ || !workingSrv_ || visibleIndices_.empty()) return;

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(context_->Map(constants_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        ShaderConstants constants{};
        ComputeMatrices(camera, static_cast<float>(width) / height, constants.worldViewProjection,
                        constants.world, constants.cameraPosition);
        constants.parameters[0] = static_cast<float>(width);
        constants.parameters[1] = static_cast<float>(height);
        constants.parameters[2] = projectionPreview_ && projectionSrv_ && maskSrv_ ? 1.0f : 0.0f;
        std::memcpy(mapped.pData, &constants, sizeof(constants));
        context_->Unmap(constants_.Get(), 0);
    }
    const D3D11_VIEWPORT viewport{0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
    context_->RSSetViewports(1, &viewport);
    context_->RSSetState(rasterizer_.Get());
    const UINT stride = sizeof(Vertex);
    const UINT offset = 0;
    context_->IASetInputLayout(inputLayout_.Get());
    context_->IASetVertexBuffers(0, 1, vertexBuffer_.GetAddressOf(), &stride, &offset);
    context_->IASetIndexBuffer(visibleIndexBuffer_.Get(), DXGI_FORMAT_R32_UINT, 0);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(viewportVs_.Get(), nullptr, 0);
    context_->VSSetConstantBuffers(0, 1, constants_.GetAddressOf());
    context_->PSSetShader(viewportPs_.Get(), nullptr, 0);
    ID3D11ShaderResourceView* resources[]{workingSrv_.Get(), selectionSrv_.Get(), projectionSrv_.Get(), maskSrv_.Get()};
    context_->PSSetShaderResources(0, 4, resources);
    context_->PSSetSamplers(0, 1, sampler_.GetAddressOf());
    context_->DrawIndexed(static_cast<UINT>(visibleIndices_.size()), 0, 0);
    ID3D11ShaderResourceView* nullResources[4]{};
    context_->PSSetShaderResources(0, 4, nullResources);
}

bool Renderer::CaptureFrame(const CameraState& camera, TextureImage& image, std::string& error) {
    if (!colorTexture_ || !depthTexture_ || viewportWidth_ == 0 || visibleIndices_.empty()) {
        error = "Render a visible mesh before capturing the view.";
        return false;
    }
    D3D11_TEXTURE2D_DESC colorDesc{};
    colorTexture_->GetDesc(&colorDesc);
    HRESULT hr = device_->CreateTexture2D(&colorDesc, nullptr, frozenColor_.ReleaseAndGetAddressOf());
    if (SUCCEEDED(hr)) context_->CopyResource(frozenColor_.Get(), colorTexture_.Get());
    D3D11_TEXTURE2D_DESC depthDesc{};
    depthTexture_->GetDesc(&depthDesc);
    if (SUCCEEDED(hr)) hr = device_->CreateTexture2D(&depthDesc, nullptr, frozenDepth_.ReleaseAndGetAddressOf());
    if (SUCCEEDED(hr)) context_->CopyResource(frozenDepth_.Get(), depthTexture_.Get());
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    if (SUCCEEDED(hr)) hr = device_->CreateShaderResourceView(frozenDepth_.Get(), &srvDesc,
                                                              frozenDepthSrv_.ReleaseAndGetAddressOf());

    D3D11_BUFFER_DESC indexDesc{};
    indexDesc.ByteWidth = static_cast<UINT>(visibleIndices_.size() * sizeof(std::uint32_t));
    indexDesc.Usage = D3D11_USAGE_IMMUTABLE;
    indexDesc.BindFlags = D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA indexData{visibleIndices_.data(), 0, 0};
    if (SUCCEEDED(hr)) hr = device_->CreateBuffer(&indexDesc, &indexData, frozenIndexBuffer_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
        error = HrError("Could not freeze the projection frame", hr);
        ClearFrozenFrame();
        return false;
    }
    frozenCamera_ = camera;
    frozenWidth_ = viewportWidth_;
    frozenHeight_ = viewportHeight_;
    frozenIndexCount_ = static_cast<std::uint32_t>(visibleIndices_.size());
    return ReadTexture(frozenColor_.Get(), DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, image, error);
}

void Renderer::ClearFrozenFrame() {
    frozenColor_.Reset();
    frozenDepth_.Reset();
    frozenDepthSrv_.Reset();
    frozenIndexBuffer_.Reset();
    frozenWidth_ = frozenHeight_ = frozenIndexCount_ = 0;
}

bool Renderer::BakeProjection(const float maxAngleDegrees, std::string& error) {
    if (!workingRtv_ || !projectionSrv_ || !maskSrv_ || !frozenDepthSrv_ || !frozenIndexBuffer_) {
        error = "Capture a view and provide a projection image and mask before baking.";
        return false;
    }
    ID3D11ShaderResourceView* nullResources[3]{};
    context_->PSSetShaderResources(0, 3, nullResources);
    context_->OMSetRenderTargets(1, workingRtv_.GetAddressOf(), nullptr);
    const D3D11_VIEWPORT viewport{0, 0, static_cast<float>(textureWidth_), static_cast<float>(textureHeight_), 0, 1};
    context_->RSSetViewports(1, &viewport);
    context_->RSSetState(rasterizer_.Get());

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(constants_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        error = "Could not update projection constants.";
        return false;
    }
    ShaderConstants constants{};
    ComputeMatrices(frozenCamera_, static_cast<float>(frozenWidth_) / frozenHeight_,
                    constants.worldViewProjection, constants.world, constants.cameraPosition);
    constants.parameters[0] = static_cast<float>(frozenWidth_);
    constants.parameters[1] = static_cast<float>(frozenHeight_);
    constants.parameters[2] = std::cos(std::clamp(maxAngleDegrees, 0.0f, 89.9f) *
                                       std::numbers::pi_v<float> / 180.0f);
    constants.parameters[3] = 0.003f;
    std::memcpy(mapped.pData, &constants, sizeof(constants));
    context_->Unmap(constants_.Get(), 0);

    const UINT stride = sizeof(Vertex);
    const UINT offset = 0;
    context_->IASetInputLayout(inputLayout_.Get());
    context_->IASetVertexBuffers(0, 1, vertexBuffer_.GetAddressOf(), &stride, &offset);
    context_->IASetIndexBuffer(frozenIndexBuffer_.Get(), DXGI_FORMAT_R32_UINT, 0);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(bakeVs_.Get(), nullptr, 0);
    context_->VSSetConstantBuffers(0, 1, constants_.GetAddressOf());
    context_->PSSetShader(bakePs_.Get(), nullptr, 0);
    context_->PSSetConstantBuffers(0, 1, constants_.GetAddressOf());
    ID3D11ShaderResourceView* resources[]{projectionSrv_.Get(), frozenDepthSrv_.Get(), maskSrv_.Get()};
    context_->PSSetShaderResources(0, 3, resources);
    context_->PSSetSamplers(0, 1, sampler_.GetAddressOf());
    const float blendFactor[4]{};
    context_->OMSetBlendState(bakeBlend_.Get(), blendFactor, 0xffffffffu);
    context_->DrawIndexed(frozenIndexCount_, 0, 0);
    context_->OMSetBlendState(nullptr, blendFactor, 0xffffffffu);
    context_->PSSetShaderResources(0, 3, nullResources);
    context_->Flush();
    error.clear();
    return true;
}

bool Renderer::ReadTexture(ID3D11Texture2D* texture, const DXGI_FORMAT format,
                           TextureImage& image, std::string& error) const {
    (void)format;
    if (!texture) return false;
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    D3D11_TEXTURE2D_DESC stagingDesc = desc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    HRESULT hr = device_->CreateTexture2D(&stagingDesc, nullptr, staging.GetAddressOf());
    if (FAILED(hr)) {
        error = HrError("Could not create a texture readback", hr);
        return false;
    }
    context_->CopyResource(staging.Get(), texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr = context_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        error = HrError("Could not map a texture readback", hr);
        return false;
    }
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(desc.Width) * desc.Height * 4);
    for (UINT y = 0; y < desc.Height; ++y) {
        std::memcpy(pixels.data() + static_cast<std::size_t>(y) * desc.Width * 4,
                    static_cast<const std::uint8_t*>(mapped.pData) + static_cast<std::size_t>(y) * mapped.RowPitch,
                    static_cast<std::size_t>(desc.Width) * 4);
    }
    context_->Unmap(staging.Get(), 0);
    image.Assign(desc.Width, desc.Height, pixels);
    error.clear();
    return true;
}

bool Renderer::ReadWorkingTexture(TextureImage& image, std::string& error) const {
    return ReadTexture(workingTexture_.Get(), DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, image, error);
}

std::vector<std::uint32_t> Renderer::ReadIdBuffer() const {
    if (!idTexture_) return {};
    D3D11_TEXTURE2D_DESC desc{};
    idTexture_->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device_->CreateTexture2D(&desc, nullptr, staging.GetAddressOf()))) return {};
    context_->CopyResource(staging.Get(), idTexture_.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return {};
    std::vector<std::uint32_t> ids(static_cast<std::size_t>(desc.Width) * desc.Height);
    for (UINT y = 0; y < desc.Height; ++y) {
        std::memcpy(ids.data() + static_cast<std::size_t>(y) * desc.Width,
                    static_cast<const std::uint8_t*>(mapped.pData) + static_cast<std::size_t>(y) * mapped.RowPitch,
                    static_cast<std::size_t>(desc.Width) * sizeof(std::uint32_t));
    }
    context_->Unmap(staging.Get(), 0);
    return ids;
}

std::uint32_t Renderer::PickTriangle(const std::uint32_t x, const std::uint32_t y) const {
    if (x >= viewportWidth_ || y >= viewportHeight_) return UINT32_MAX;
    const auto ids = ReadIdBuffer();
    if (ids.empty()) return UINT32_MAX;
    const std::uint32_t encoded = ids[static_cast<std::size_t>(y) * viewportWidth_ + x];
    return encoded == 0 ? UINT32_MAX : encoded - 1;
}

std::vector<std::uint32_t> Renderer::PickTrianglesInLasso(const std::span<const Vec2> points) const {
    const auto ids = ReadIdBuffer();
    if (ids.empty() || points.size() < 3) return {};
    float minX = static_cast<float>(viewportWidth_), maxX = 0;
    float minY = static_cast<float>(viewportHeight_), maxY = 0;
    for (const Vec2& point : points) {
        minX = std::min(minX, point.x); maxX = std::max(maxX, point.x);
        minY = std::min(minY, point.y); maxY = std::max(maxY, point.y);
    }
    std::unordered_set<std::uint32_t> selected;
    const int x0 = std::clamp(static_cast<int>(minX), 0, static_cast<int>(viewportWidth_) - 1);
    const int x1 = std::clamp(static_cast<int>(std::ceil(maxX)), 0, static_cast<int>(viewportWidth_) - 1);
    const int y0 = std::clamp(static_cast<int>(minY), 0, static_cast<int>(viewportHeight_) - 1);
    const int y1 = std::clamp(static_cast<int>(std::ceil(maxY)), 0, static_cast<int>(viewportHeight_) - 1);
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            if (!PointInside(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f, points)) continue;
            const std::uint32_t encoded = ids[static_cast<std::size_t>(y) * viewportWidth_ + x];
            if (encoded != 0) selected.insert(encoded - 1);
        }
    }
    return {selected.begin(), selected.end()};
}

} // namespace codextex
