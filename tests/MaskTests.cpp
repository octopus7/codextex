#include "core/Mask.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("Lasso fills only its polygon") {
    codextex::MaskImage mask;
    mask.Resize(16, 16);
    const std::vector<codextex::Vec2> polygon{{4, 4}, {12, 4}, {12, 12}, {4, 12}};
    mask.ApplyLasso(polygon, true);
    CHECK(mask.Binary()[8 * 16 + 8] == 255);
    CHECK(mask.Binary()[2 * 16 + 2] == 0);
}

TEST_CASE("Inward feather never expands outside the binary mask") {
    codextex::MaskImage mask;
    mask.Resize(32, 32);
    const std::vector<codextex::Vec2> polygon{{4, 4}, {28, 4}, {28, 28}, {4, 28}};
    mask.ApplyLasso(polygon, true);
    mask.RecomputeInwardFeather(8);

    for (std::size_t i = 0; i < mask.Binary().size(); ++i) {
        if (mask.Binary()[i] == 0) {
            CHECK(mask.Feathered()[i] == 0);
        }
    }
    CHECK(mask.Feathered()[16 * 32 + 16] > mask.Feathered()[4 * 32 + 4]);
}

TEST_CASE("Brush can include and erase") {
    codextex::MaskImage mask;
    mask.Resize(32, 32);
    mask.PaintCircle(16, 16, 5, true);
    CHECK(mask.Binary()[16 * 32 + 16] == 255);
    mask.PaintCircle(16, 16, 2, false);
    CHECK(mask.Binary()[16 * 32 + 16] == 0);
}

TEST_CASE("Repeated brush strokes leave the binary mask revision unchanged") {
    codextex::MaskImage mask;
    CHECK_FALSE(mask.PaintCircle(8, 8, 3, true));
    CHECK(mask.Revision() == 0);
    mask.Resize(16, 16);
    const auto emptyRevision = mask.Revision();
    REQUIRE(mask.PaintCircle(8, 8, 3, true));
    CHECK(mask.Revision() == emptyRevision + 1);
    const auto paintedPixels = mask.Binary();
    const auto paintedRevision = mask.Revision();
    CHECK_FALSE(mask.PaintCircle(8, 8, 3, true));
    CHECK_FALSE(mask.PaintCircle(8, 8, 1, true));
    CHECK_FALSE(mask.PaintCircle(-10, -10, 2, true));
    CHECK_FALSE(mask.PaintCircle(8, 8, 0, false));
    CHECK(mask.Binary() == paintedPixels);
    CHECK(mask.Revision() == paintedRevision);

    mask.RecomputeInwardFeather(4);
    CHECK(mask.Revision() == paintedRevision);
    REQUIRE(mask.PaintCircle(8, 8, 2, false));
    CHECK(mask.Revision() == paintedRevision + 1);
    CHECK_FALSE(mask.PaintCircle(8, 8, 2, false));
    CHECK(mask.Revision() == paintedRevision + 1);
}

TEST_CASE("Lasso reports only actual binary pixel changes") {
    codextex::MaskImage mask;
    mask.Resize(16, 16);
    const auto initialRevision = mask.Revision();
    const std::vector<codextex::Vec2> polygon{{4, 4}, {12, 4}, {12, 12}, {4, 12}};
    CHECK_FALSE(mask.ApplyLasso(polygon, false));
    REQUIRE(mask.ApplyLasso(polygon, true));
    CHECK(mask.Revision() == initialRevision + 1);
    CHECK_FALSE(mask.ApplyLasso(polygon, true));
    const std::vector<codextex::Vec2> outside{{20, 20}, {24, 20}, {24, 24}, {20, 24}};
    CHECK_FALSE(mask.ApplyLasso(outside, true));
    CHECK_FALSE(mask.ApplyLasso(std::span<const codextex::Vec2>(polygon).first(2), true));
    CHECK(mask.Revision() == initialRevision + 1);
    REQUIRE(mask.ApplyLasso(polygon, false));
    CHECK(mask.Revision() == initialRevision + 2);
    CHECK_FALSE(mask.ApplyLasso(polygon, false));
    CHECK(mask.Revision() == initialRevision + 2);
}

TEST_CASE("Clear reports binary changes while preserving its feather reset behavior") {
    codextex::MaskImage mask;
    CHECK_FALSE(mask.Clear(true));
    CHECK(mask.Revision() == 0);
    mask.Resize(16, 16);
    const auto initialRevision = mask.Revision();
    CHECK_FALSE(mask.Clear());
    CHECK(mask.Revision() == initialRevision);
    REQUIRE(mask.Clear(true));
    CHECK(mask.Revision() == initialRevision + 1);
    mask.RecomputeInwardFeather(4);
    REQUIRE(mask.Feathered() != mask.Binary());
    CHECK_FALSE(mask.Clear(true));
    CHECK(mask.Feathered() == mask.Binary());
    CHECK(mask.Revision() == initialRevision + 1);
    REQUIRE(mask.Clear());
    CHECK(mask.Revision() == initialRevision + 2);
    CHECK_FALSE(mask.Clear());
    CHECK(mask.Revision() == initialRevision + 2);
}

TEST_CASE("Mask resize revisions reflect dimensions and pixels but not identical resets") {
    codextex::MaskImage mask;
    mask.Resize(0, 0);
    CHECK(mask.Revision() == 0);
    mask.Resize(4, 8);
    REQUIRE(mask.Revision() == 1);
    mask.Resize(4, 8);
    CHECK(mask.Revision() == 1);
    mask.Resize(4, 8, true);
    CHECK(mask.Revision() == 2);
    mask.Resize(4, 8, true);
    CHECK(mask.Revision() == 2);
    mask.Resize(8, 4, true);
    CHECK(mask.Revision() == 3);
    CHECK(mask.Width() == 8);
    CHECK(mask.Height() == 4);
    REQUIRE(mask.PaintCircle(2, 2, 1, false));
    mask.Resize(8, 4, true);
    CHECK(mask.Revision() == 5);

    auto copied = mask;
    CHECK(copied.Revision() == mask.Revision());
    REQUIRE(copied.Clear());
    CHECK(copied.Revision() == mask.Revision() + 1);
    CHECK(copied.Binary() != mask.Binary());
    mask.Resize(0, 0);
    CHECK(mask.Revision() == 6);
    CHECK(mask.Binary().empty());
    mask.Resize(0, 0);
    CHECK(mask.Revision() == 6);
}
