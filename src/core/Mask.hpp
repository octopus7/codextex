#pragma once

#include "core/Types.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace codextex {

class MaskImage {
public:
    void Resize(std::uint32_t width, std::uint32_t height, bool selected = false);
    void Clear(bool selected = false);
    void PaintCircle(float x, float y, float radius, bool include);
    void ApplyLasso(std::span<const Vec2> pixelPoints, bool include);
    void RecomputeInwardFeather(int radiusPx);

    [[nodiscard]] std::uint32_t Width() const noexcept { return width_; }
    [[nodiscard]] std::uint32_t Height() const noexcept { return height_; }
    [[nodiscard]] const std::vector<std::uint8_t>& Binary() const noexcept { return binary_; }
    [[nodiscard]] const std::vector<std::uint8_t>& Feathered() const noexcept { return feathered_; }

private:
    std::uint32_t width_{};
    std::uint32_t height_{};
    std::vector<std::uint8_t> binary_;
    std::vector<std::uint8_t> feathered_;
};

} // namespace codextex
