#include "core/TextureImage.hpp"

#include <catch2/catch_test_macros.hpp>
#include <Windows.h>
#include <objbase.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>

namespace {

struct TestComApartment {
    explicit TestComApartment(const DWORD model) : result(CoInitializeEx(nullptr, model)) {}
    ~TestComApartment() { if (SUCCEEDED(result)) CoUninitialize(); }
    HRESULT result;
};

struct ImageOperationResult {
    bool success{};
    std::string error;
    codextex::TextureImage image;
};

constexpr std::array<std::uint8_t, 16> apartmentPixels{
    240, 10, 20, 255, 30, 220, 40, 127, 50, 60, 200, 63, 90, 100, 110, 0,
};

} // namespace

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

TEST_CASE("PNG codecs survive repeated worker and UI COM apartment lifetimes") {
    const auto directory = std::filesystem::temp_directory_path() / "codextex-tests";
    std::filesystem::create_directories(directory);
    const auto sourcePath = directory / "apartment-source.png";
    const auto outputPath = directory / "apartment-roundtrip.png";
    // End the factory's original apartment before any other image operation.
    const auto source = std::async(std::launch::async, [sourcePath] {
        TestComApartment apartment(COINIT_APARTMENTTHREADED);
        ImageOperationResult result;
        if (FAILED(apartment.result)) { result.error = "Could not create the source STA."; return result; }
        result.image.Assign(2, 2, apartmentPixels);
        result.success = result.image.SavePng(sourcePath, result.error);
        return result;
    }).get();
    INFO(source.error);
    REQUIRE(source.success);
    std::ifstream stream(sourcePath, std::ios::binary);
    const std::vector<std::uint8_t> encoded{std::istreambuf_iterator<char>(stream), {}};
    REQUIRE_FALSE(encoded.empty());

    for (int iteration = 0; iteration < 4; ++iteration) {
        const auto imported = std::async(std::launch::async, [&encoded] {
            TestComApartment apartment(COINIT_MULTITHREADED);
            ImageOperationResult result;
            if (FAILED(apartment.result)) { result.error = "Could not create the import MTA."; return result; }
            // Exercise early-return cleanup after metadata preflight as well.
            if (result.image.LoadEncoded(encoded, result.error, 0)) {
                result.error = "The decoded memory budget was not enforced.";
                return result;
            }
            result.success = result.image.LoadEncoded(encoded, result.error);
            return result;
        }).get();
        INFO(imported.error);
        REQUIRE(imported.success);
        CHECK(imported.image.Pixels() == source.image.Pixels());
        const auto roundTrip = std::async(std::launch::async, [&imported, outputPath] {
            TestComApartment apartment(COINIT_APARTMENTTHREADED);
            ImageOperationResult result;
            if (FAILED(apartment.result)) { result.error = "Could not create the UI STA."; return result; }
            result.success = imported.image.SavePng(outputPath, result.error) &&
                result.image.LoadPng(outputPath, result.error);
            return result;
        }).get();
        INFO(roundTrip.error);
        REQUIRE(roundTrip.success);
        CHECK(roundTrip.image.Pixels() == source.image.Pixels());
    }
}

TEST_CASE("Concurrent PNG decode and save operations isolate DirectXTex WIC factories") {
    const auto directory = std::filesystem::temp_directory_path() / "codextex-tests";
    std::filesystem::create_directories(directory);
    const auto sourcePath = directory / "concurrent-codec-source.png";
    codextex::TextureImage source;
    source.Assign(2, 2, apartmentPixels);
    std::string error;
    REQUIRE(source.SavePng(sourcePath, error));
    std::ifstream stream(sourcePath, std::ios::binary);
    const std::vector<std::uint8_t> encoded{std::istreambuf_iterator<char>(stream), {}};
    REQUIRE_FALSE(encoded.empty());
    std::vector<std::future<ImageOperationResult>> workers;
    for (int worker = 0; worker < 6; ++worker) {
        workers.push_back(std::async(std::launch::async, [&, worker] {
            TestComApartment apartment(worker % 2 == 0 ? COINIT_MULTITHREADED : COINIT_APARTMENTTHREADED);
            ImageOperationResult result;
            if (FAILED(apartment.result)) { result.error = "Could not initialize worker COM."; return result; }
            const auto path = directory / ("concurrent-codec-" + std::to_string(worker) + ".png");
            for (int pass = 0; pass < 3; ++pass) {
                if (!result.image.LoadEncoded(encoded, result.error) ||
                    !result.image.SavePng(path, result.error) ||
                    !result.image.LoadPng(path, result.error)) return result;
            }
            result.success = true;
            return result;
        }));
    }
    for (auto& worker : workers) {
        const auto result = worker.get();
        INFO(result.error);
        REQUIRE(result.success);
        CHECK(result.image.Pixels() == source.Pixels());
    }
}
