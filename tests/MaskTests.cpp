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
