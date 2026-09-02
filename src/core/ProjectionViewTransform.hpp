#pragma once

#include "core/Types.hpp"

#include <algorithm>

namespace codextex {

struct ProjectionViewTransform {
    static constexpr float MinimumZoom = 1.0f;
    static constexpr float MaximumZoom = 8.0f;

    float zoom{MinimumZoom};
    Vec2 center{0.5f, 0.5f};

    [[nodiscard]] float Span() const noexcept { return 1.0f / zoom; }

    [[nodiscard]] Vec2 MinimumUv() const noexcept {
        const float halfSpan = Span() * 0.5f;
        return {center.x - halfSpan, center.y - halfSpan};
    }

    [[nodiscard]] Vec2 MaximumUv() const noexcept {
        const float halfSpan = Span() * 0.5f;
        return {center.x + halfSpan, center.y + halfSpan};
    }

    [[nodiscard]] Vec2 ViewToSource(const Vec2 viewUv) const noexcept {
        const Vec2 minimum = MinimumUv();
        const float span = Span();
        return {minimum.x + viewUv.x * span, minimum.y + viewUv.y * span};
    }

    void ZoomAt(const float requestedZoom, const Vec2 anchorViewUv) noexcept {
        const Vec2 sourceAnchor = ViewToSource(anchorViewUv);
        zoom = std::clamp(requestedZoom, MinimumZoom, MaximumZoom);
        const float span = Span();
        center.x = sourceAnchor.x - (anchorViewUv.x - 0.5f) * span;
        center.y = sourceAnchor.y - (anchorViewUv.y - 0.5f) * span;
        ClampCenter();
    }

    void PanByViewDelta(const Vec2 viewDelta) noexcept {
        const float span = Span();
        center.x -= viewDelta.x * span;
        center.y -= viewDelta.y * span;
        ClampCenter();
    }

private:
    void ClampCenter() noexcept {
        const float halfSpan = Span() * 0.5f;
        center.x = std::clamp(center.x, halfSpan, 1.0f - halfSpan);
        center.y = std::clamp(center.y, halfSpan, 1.0f - halfSpan);
    }
};

} // namespace codextex
