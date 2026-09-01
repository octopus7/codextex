#include "core/TextureImage.hpp"

#include <catch2/catch_test_macros.hpp>
#include <Windows.h>
#include <objbase.h>

#include <array>
#include <filesystem>

TEST_CASE("PNG round trip preserves RGBA pixels") {
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    REQUIRE((SUCCEEDED(comResult) || comResult == RPC_E_CHANGED_MODE));
    const std::array<std::uint8_t, 16> pixels{
        255, 0, 0, 255,
        0, 255, 0, 128,
        0, 0, 255, 64,
        255, 255, 255, 0,
    };
    const auto directory = std::filesystem::temp_directory_path() / "codextex-tests";
    std::filesystem::create_directories(directory);
    const auto path = directory / "roundtrip.png";

    codextex::TextureImage source;
    source.Assign(2, 2, pixels);
    std::string error;
    INFO(error);
    REQUIRE(source.SavePng(path, error));

    codextex::TextureImage loaded;
    REQUIRE(loaded.LoadPng(path, error));
    CHECK(loaded.Width() == 2);
    CHECK(loaded.Height() == 2);
    CHECK(loaded.Pixels() == std::vector<std::uint8_t>(pixels.begin(), pixels.end()));
    if (SUCCEEDED(comResult)) CoUninitialize();
}

TEST_CASE("Center square crop removes equal pixels from the long axis") {
    std::vector<std::uint8_t> pixels(4 * 2 * 4, 255);
    for (std::uint32_t y = 0; y < 2; ++y) {
        for (std::uint32_t x = 0; x < 4; ++x) {
            const std::size_t offset = (static_cast<std::size_t>(y) * 4 + x) * 4;
            pixels[offset] = static_cast<std::uint8_t>(x + y * 10);
        }
    }
    codextex::TextureImage source;
    source.Assign(4, 2, pixels);
    const codextex::TextureImage cropped = source.CenterCroppedSquare();
    REQUIRE(cropped.Width() == 2);
    REQUIRE(cropped.Height() == 2);
    CHECK(cropped.Pixels()[0] == 1);
    CHECK(cropped.Pixels()[4] == 2);
    CHECK(cropped.Pixels()[8] == 11);
    CHECK(cropped.Pixels()[12] == 12);
}
