#include "core/ProjectionViewTransform.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

TEST_CASE("Projection display zoom preserves the source point under the cursor") {
    codextex::ProjectionViewTransform transform;
    const codextex::Vec2 anchor{0.75f, 0.25f};
    const codextex::Vec2 before = transform.ViewToSource(anchor);

    transform.ZoomAt(2.0f, anchor);
    const codextex::Vec2 after = transform.ViewToSource(anchor);

    CHECK(after.x == Catch::Approx(before.x));
    CHECK(after.y == Catch::Approx(before.y));
    CHECK(transform.MinimumUv().x >= 0.0f);
    CHECK(transform.MinimumUv().y >= 0.0f);
    CHECK(transform.MaximumUv().x <= 1.0f);
    CHECK(transform.MaximumUv().y <= 1.0f);
}

TEST_CASE("Projection display pan remains within the frozen source frame") {
    codextex::ProjectionViewTransform transform;
    transform.ZoomAt(4.0f, {0.5f, 0.5f});
    transform.PanByViewDelta({10.0f, -10.0f});

    CHECK(transform.MinimumUv().x == Catch::Approx(0.0f));
    CHECK(transform.MaximumUv().y == Catch::Approx(1.0f));
    CHECK(transform.MinimumUv().y >= 0.0f);
    CHECK(transform.MaximumUv().x <= 1.0f);
}
