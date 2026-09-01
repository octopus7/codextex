#include "core/TextureImage.hpp"

#include <DirectXTex.h>
#include <Windows.h>
#include <wincodec.h>

#include <algorithm>
#include <cstring>
#include <system_error>

namespace codextex {

bool TextureImage::LoadPng(const std::filesystem::path& path, std::string& error) {
    DirectX::ScratchImage loaded;
    DirectX::TexMetadata metadata{};
    HRESULT hr = DirectX::LoadFromWICFile(path.c_str(), DirectX::WIC_FLAGS_FORCE_SRGB, &metadata, loaded);
    if (FAILED(hr)) {
        error = "Could not decode PNG (HRESULT " + std::to_string(static_cast<unsigned long>(hr)) + ").";
        return false;
    }
    if (metadata.dimension != DirectX::TEX_DIMENSION_TEXTURE2D || metadata.arraySize != 1 ||
        metadata.width == 0 || metadata.height == 0 || metadata.width > 16384 || metadata.height > 16384) {
        error = "PNG must be a single 2D image no larger than 16384x16384.";
        return false;
    }

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
