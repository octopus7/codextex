#include "core/TextureImage.hpp"

#include <DirectXTex.h>
#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <system_error>

namespace codextex {
namespace {

std::mutex& WicMutex() {
    static std::mutex mutex;
    return mutex;
}

// DirectXTex caches its WIC factory process-wide. A factory created during an
// import must be released before that worker's COM apartment is uninitialized.
// Serialize every WIC operation, install a fresh factory for this apartment,
// and clear both references while COM is still alive, including on failures.
class ScopedWicFactory {
public:
    explicit ScopedWicFactory(std::string& error) : lock_(WicMutex()) {
        comResult_ = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(comResult_) && comResult_ != RPC_E_CHANGED_MODE) {
            error = "Could not initialize COM for image processing (HRESULT " +
                std::to_string(static_cast<unsigned long>(comResult_)) + ").";
            return;
        }
        HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory2, nullptr, CLSCTX_INPROC_SERVER,
                                       IID_PPV_ARGS(factory_.GetAddressOf()));
        if (FAILED(hr)) {
            hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(factory_.ReleaseAndGetAddressOf()));
        }
        if (FAILED(hr)) {
            error = "Could not create the image decoder factory (HRESULT " +
                std::to_string(static_cast<unsigned long>(hr)) + ").";
            return;
        }
        DirectX::SetWICFactory(factory_.Get());
        installed_ = true;
    }

    ~ScopedWicFactory() {
        if (installed_) DirectX::SetWICFactory(nullptr);
        factory_.Reset();
        if (SUCCEEDED(comResult_)) CoUninitialize();
    }

    [[nodiscard]] bool Valid() const noexcept { return installed_; }

private:
    std::unique_lock<std::mutex> lock_;
    HRESULT comResult_{E_FAIL};
    Microsoft::WRL::ComPtr<IWICImagingFactory> factory_;
    bool installed_{};
};

} // namespace

bool TextureImage::LoadEncoded(const std::span<const std::uint8_t> bytes, std::string& error,
                               const std::size_t maxDecodedBytes) {
    if (bytes.empty() || bytes.size() > 128ull * 1024 * 1024) {
        error = "Embedded image is empty or exceeds the 128 MiB encoded image limit.";
        return false;
    }
    ScopedWicFactory wic(error);
    if (!wic.Valid()) return false;
    DirectX::ScratchImage loaded;
    DirectX::TexMetadata metadata{};
    HRESULT hr = DirectX::GetMetadataFromWICMemory(bytes.data(), bytes.size(),
                                                  DirectX::WIC_FLAGS_FORCE_SRGB, metadata);
    if (FAILED(hr)) {
        error = "Could not decode embedded image (PNG or JPEG required).";
        return false;
    }
    if (metadata.dimension != DirectX::TEX_DIMENSION_TEXTURE2D || metadata.arraySize != 1 ||
        metadata.width == 0 || metadata.height == 0 || metadata.width > 16384 ||
        metadata.height > 16384 || metadata.width * metadata.height > 64ull * 1024 * 1024 ||
        metadata.width * metadata.height > maxDecodedBytes / 4) {
        error = "Embedded image exceeds the dimension or decoded memory limit.";
        return false;
    }
    hr = DirectX::LoadFromWICMemory(bytes.data(), bytes.size(),
                                    DirectX::WIC_FLAGS_FORCE_SRGB, &metadata, loaded);
    if (FAILED(hr)) { error = "Could not decode embedded image pixels."; return false; }
    DirectX::ScratchImage converted;
    const DirectX::Image* image = loaded.GetImage(0, 0, 0);
    if (image == nullptr) { error = "Embedded image contains no pixels."; return false; }
    if (image->format != DXGI_FORMAT_R8G8B8A8_UNORM && image->format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) {
        hr = DirectX::Convert(*image, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DirectX::TEX_FILTER_DEFAULT,
                              DirectX::TEX_THRESHOLD_DEFAULT, converted);
        if (FAILED(hr)) { error = "Could not convert embedded image to RGBA8."; return false; }
        image = converted.GetImage(0, 0, 0);
    }
    TextureImage candidate;
    candidate.width_ = static_cast<std::uint32_t>(image->width);
    candidate.height_ = static_cast<std::uint32_t>(image->height);
    candidate.pixels_.resize(static_cast<std::size_t>(candidate.width_) * candidate.height_ * 4);
    for (std::uint32_t y = 0; y < candidate.height_; ++y) {
        std::memcpy(candidate.pixels_.data() + static_cast<std::size_t>(y) * candidate.width_ * 4,
                    image->pixels + static_cast<std::size_t>(y) * image->rowPitch,
                    static_cast<std::size_t>(candidate.width_) * 4);
    }
    *this = std::move(candidate);
    error.clear();
    return true;
}

bool TextureImage::LoadPng(const std::filesystem::path& path, std::string& error) {
    ScopedWicFactory wic(error);
    if (!wic.Valid()) return false;
    DirectX::ScratchImage loaded;
    DirectX::TexMetadata metadata{};
    HRESULT hr = DirectX::GetMetadataFromWICFile(path.c_str(), DirectX::WIC_FLAGS_FORCE_SRGB, metadata);
    if (FAILED(hr)) {
        error = "Could not decode PNG (HRESULT " + std::to_string(static_cast<unsigned long>(hr)) + ").";
        return false;
    }
    if (metadata.dimension != DirectX::TEX_DIMENSION_TEXTURE2D || metadata.arraySize != 1 ||
        metadata.width == 0 || metadata.height == 0 || metadata.width > 16384 || metadata.height > 16384 ||
        metadata.width * metadata.height > 64ull * 1024 * 1024) {
        error = "PNG must be a single 2D image no larger than 16384x16384 and 64 megapixels.";
        return false;
    }
    hr = DirectX::LoadFromWICFile(path.c_str(), DirectX::WIC_FLAGS_FORCE_SRGB, &metadata, loaded);
    if (FAILED(hr)) { error = "Could not decode PNG pixels."; return false; }

    DirectX::ScratchImage converted;
    const DirectX::Image* image = loaded.GetImage(0, 0, 0);
    if (image == nullptr) {
        error = "PNG did not contain image pixels.";
        return false;
    }
    if (image->format != DXGI_FORMAT_R8G8B8A8_UNORM && image->format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) {
        hr = DirectX::Convert(*image, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DirectX::TEX_FILTER_DEFAULT,
                              DirectX::TEX_THRESHOLD_DEFAULT, converted);
        if (FAILED(hr)) {
            error = "Could not convert PNG to RGBA8.";
            return false;
        }
        image = converted.GetImage(0, 0, 0);
    }

    width_ = static_cast<std::uint32_t>(image->width);
    height_ = static_cast<std::uint32_t>(image->height);
    pixels_.resize(static_cast<std::size_t>(width_) * height_ * 4);
    for (std::uint32_t y = 0; y < height_; ++y) {
        std::memcpy(pixels_.data() + static_cast<std::size_t>(y) * width_ * 4,
                    image->pixels + static_cast<std::size_t>(y) * image->rowPitch,
                    static_cast<std::size_t>(width_) * 4);
    }
    sourcePath_ = path;
    error.clear();
    return true;
}

bool TextureImage::SavePng(const std::filesystem::path& path, std::string& error) const {
    if (Empty()) {
        error = "There is no texture to save.";
        return false;
    }
    ScopedWicFactory wic(error);
    if (!wic.Valid()) return false;

    DirectX::Image image{};
    image.width = width_;
    image.height = height_;
    image.format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    image.rowPitch = static_cast<std::size_t>(width_) * 4;
    image.slicePitch = image.rowPitch * height_;
    image.pixels = const_cast<std::uint8_t*>(pixels_.data());

    std::filesystem::path temporary = path;
    temporary += L".codextex.tmp.png";
    const HRESULT hr = DirectX::SaveToWICFile(image, DirectX::WIC_FLAGS_FORCE_SRGB,
                                              GUID_ContainerFormatPng, temporary.c_str());
    if (FAILED(hr)) {
        error = "Could not encode PNG (HRESULT " +
            std::to_string(static_cast<unsigned long>(hr)) + ").";
        return false;
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD code = GetLastError();
        DeleteFileW(temporary.c_str());
        error = "Could not replace the destination PNG (Win32 error " + std::to_string(code) + ").";
        return false;
    }
    error.clear();
    return true;
}

void TextureImage::Assign(const std::uint32_t width, const std::uint32_t height,
                          const std::span<const std::uint8_t> rgba) {
    width_ = width;
    height_ = height;
    pixels_.assign(rgba.begin(), rgba.end());
}

TextureImage TextureImage::CenterCroppedSquare() const {
    TextureImage cropped;
    if (Empty() || width_ == 0 || height_ == 0) return cropped;
    const std::uint32_t side = std::min(width_, height_);
    const std::uint32_t offsetX = (width_ - side) / 2;
    const std::uint32_t offsetY = (height_ - side) / 2;
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(side) * side * 4);
    for (std::uint32_t y = 0; y < side; ++y) {
        const std::size_t sourceOffset =
            (static_cast<std::size_t>(y + offsetY) * width_ + offsetX) * 4;
        const std::size_t destinationOffset = static_cast<std::size_t>(y) * side * 4;
        std::memcpy(pixels.data() + destinationOffset, pixels_.data() + sourceOffset,
                    static_cast<std::size_t>(side) * 4);
    }
    cropped.Assign(side, side, pixels);
    return cropped;
}

} // namespace codextex
