#include "core/Mask.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace codextex {
namespace {

bool PointInside(const float x, const float y, const std::span<const Vec2> polygon) {
    bool inside = false;
    for (std::size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++) {
        const Vec2& a = polygon[i];
        const Vec2& b = polygon[j];
        const bool crosses = ((a.y > y) != (b.y > y)) &&
            (x < (b.x - a.x) * (y - a.y) / ((b.y - a.y) + 1.0e-20f) + a.x);
        if (crosses) {
            inside = !inside;
        }
    }
    return inside;
}

void DistanceTransform1D(const std::vector<float>& source, std::vector<float>& destination) {
    const int count = static_cast<int>(source.size());
    std::vector<int> sites(static_cast<std::size_t>(count));
    std::vector<float> boundaries(static_cast<std::size_t>(count + 1));
    int k = 0;
    sites[0] = 0;
    boundaries[0] = -std::numeric_limits<float>::infinity();
    boundaries[1] = std::numeric_limits<float>::infinity();

    for (int q = 1; q < count; ++q) {
        float s = ((source[q] + static_cast<float>(q * q)) -
                   (source[sites[k]] + static_cast<float>(sites[k] * sites[k]))) /
            static_cast<float>(2 * q - 2 * sites[k]);
        while (s <= boundaries[k] && k > 0) {
            --k;
            s = ((source[q] + static_cast<float>(q * q)) -
                 (source[sites[k]] + static_cast<float>(sites[k] * sites[k]))) /
                static_cast<float>(2 * q - 2 * sites[k]);
        }
        ++k;
        sites[k] = q;
        boundaries[k] = s;
        boundaries[k + 1] = std::numeric_limits<float>::infinity();
    }

    k = 0;
    for (int q = 0; q < count; ++q) {
        while (boundaries[k + 1] < static_cast<float>(q)) {
            ++k;
        }
        const float delta = static_cast<float>(q - sites[k]);
        destination[q] = delta * delta + source[sites[k]];
    }
}

float SmoothStep(const float value) {
    const float t = std::clamp(value, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

} // namespace

void MaskImage::Resize(const std::uint32_t width, const std::uint32_t height, const bool selected) {
    width_ = width;
    height_ = height;
    binary_.assign(static_cast<std::size_t>(width) * height, selected ? 255 : 0);
    feathered_ = binary_;
}

void MaskImage::Clear(const bool selected) {
    std::fill(binary_.begin(), binary_.end(), selected ? 255 : 0);
    feathered_ = binary_;
}

void MaskImage::PaintCircle(const float x, const float y, const float radius, const bool include) {
    if (width_ == 0 || height_ == 0 || radius <= 0.0f) {
        return;
    }
    const int minX = std::max(0, static_cast<int>(std::floor(x - radius)));
    const int maxX = std::min(static_cast<int>(width_) - 1, static_cast<int>(std::ceil(x + radius)));
    const int minY = std::max(0, static_cast<int>(std::floor(y - radius)));
    const int maxY = std::min(static_cast<int>(height_) - 1, static_cast<int>(std::ceil(y + radius)));
    const float radiusSquared = radius * radius;
    for (int py = minY; py <= maxY; ++py) {
        for (int px = minX; px <= maxX; ++px) {
            const float dx = static_cast<float>(px) + 0.5f - x;
            const float dy = static_cast<float>(py) + 0.5f - y;
            if (dx * dx + dy * dy <= radiusSquared) {
                binary_[static_cast<std::size_t>(py) * width_ + px] = include ? 255 : 0;
            }
        }
    }
}

void MaskImage::ApplyLasso(const std::span<const Vec2> pixelPoints, const bool include) {
    if (pixelPoints.size() < 3 || width_ == 0 || height_ == 0) {
        return;
    }
    float minX = static_cast<float>(width_);
    float maxX = 0.0f;
    float minY = static_cast<float>(height_);
    float maxY = 0.0f;
    for (const Vec2& point : pixelPoints) {
        minX = std::min(minX, point.x);
        maxX = std::max(maxX, point.x);
        minY = std::min(minY, point.y);
        maxY = std::max(maxY, point.y);
    }
    const int x0 = std::clamp(static_cast<int>(std::floor(minX)), 0, static_cast<int>(width_) - 1);
    const int x1 = std::clamp(static_cast<int>(std::ceil(maxX)), 0, static_cast<int>(width_) - 1);
    const int y0 = std::clamp(static_cast<int>(std::floor(minY)), 0, static_cast<int>(height_) - 1);
    const int y1 = std::clamp(static_cast<int>(std::ceil(maxY)), 0, static_cast<int>(height_) - 1);
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            if (PointInside(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f, pixelPoints)) {
                binary_[static_cast<std::size_t>(y) * width_ + x] = include ? 255 : 0;
            }
        }
    }
}

void MaskImage::RecomputeInwardFeather(const int radiusPx) {
    if (width_ == 0 || height_ == 0) {
        return;
    }
    if (radiusPx <= 0) {
        feathered_ = binary_;
        return;
    }

    const int paddedWidth = static_cast<int>(width_) + 2;
    const int paddedHeight = static_cast<int>(height_) + 2;
    constexpr float infinity = 1.0e20f;
    std::vector<float> pass(static_cast<std::size_t>(paddedWidth) * paddedHeight, 0.0f);
    for (std::uint32_t y = 0; y < height_; ++y) {
        for (std::uint32_t x = 0; x < width_; ++x) {
            pass[static_cast<std::size_t>(y + 1) * paddedWidth + x + 1] =
                binary_[static_cast<std::size_t>(y) * width_ + x] ? infinity : 0.0f;
        }
    }

    std::vector<float> input(static_cast<std::size_t>(std::max(paddedWidth, paddedHeight)));
    std::vector<float> output(input.size());
    for (int y = 0; y < paddedHeight; ++y) {
        input.assign(pass.begin() + static_cast<std::size_t>(y) * paddedWidth,
                     pass.begin() + static_cast<std::size_t>(y + 1) * paddedWidth);
        output.resize(input.size());
        DistanceTransform1D(input, output);
        std::copy(output.begin(), output.end(), pass.begin() + static_cast<std::size_t>(y) * paddedWidth);
    }
    input.resize(static_cast<std::size_t>(paddedHeight));
    output.resize(input.size());
    for (int x = 0; x < paddedWidth; ++x) {
        for (int y = 0; y < paddedHeight; ++y) {
            input[y] = pass[static_cast<std::size_t>(y) * paddedWidth + x];
        }
        DistanceTransform1D(input, output);
        for (int y = 0; y < paddedHeight; ++y) {
            pass[static_cast<std::size_t>(y) * paddedWidth + x] = output[y];
        }
    }

    feathered_.resize(binary_.size());
    for (std::uint32_t y = 0; y < height_; ++y) {
        for (std::uint32_t x = 0; x < width_; ++x) {
            const std::size_t index = static_cast<std::size_t>(y) * width_ + x;
            if (binary_[index] == 0) {
                feathered_[index] = 0;
                continue;
            }
            const float distance = std::sqrt(pass[static_cast<std::size_t>(y + 1) * paddedWidth + x + 1]);
            const float normalized = (distance - 0.5f) / static_cast<float>(radiusPx);
            feathered_[index] = static_cast<std::uint8_t>(std::lround(SmoothStep(normalized) * 255.0f));
        }
    }
}

} // namespace codextex
