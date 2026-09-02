#include "core/GenerationArchive.hpp"

#include <catch2/catch_test_macros.hpp>
#include <Windows.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>

TEST_CASE("Generated images and viewport metadata are archived together") {
    const auto root = std::filesystem::temp_directory_path() / "codextex-tests" /
        (L"generation-archive-" + std::to_wstring(GetCurrentProcessId()));
    std::error_code cleanupError;
    std::filesystem::remove_all(root, cleanupError);
    std::filesystem::create_directories(root);
    const auto source = root / "source.png";
    {
        std::ofstream stream(source, std::ios::binary | std::ios::trunc);
        stream << "mock-png";
    }

    codextex::GenerationArchiveMetadata metadata;
    metadata.jobId = 42;
    metadata.prompt = "은발 메이드";
    metadata.model = "gpt-5.6-sol";
    metadata.reasoningEffort = "medium";
    metadata.objPath = LR"(D:\assets\model.obj)";
    metadata.texturePath = LR"(D:\assets\base.png)";
    metadata.cameraTarget = {1, 2, 3};
    metadata.cameraYaw = 0.5f;
    metadata.cameraFovDegrees = 35.0f;
    metadata.captureWidth = 1024;
    metadata.captureHeight = 1024;
    metadata.cropSize = 1024;
    metadata.hiddenTriangles = {2, 7};
    metadata.referenceAssetsVisible = true;
    metadata.references.push_back({LR"(D:\assets\reference.obj)",
                                   LR"(D:\assets\reference.png)"});

    codextex::GenerationArchivePaths paths;
    std::string error;
    REQUIRE(codextex::SaveGenerationArchive(root / "permanent", source, metadata, paths, error));
    CHECK(std::filesystem::is_regular_file(paths.image));
    CHECK(std::filesystem::is_regular_file(paths.metadata));

    std::ifstream metadataStream(paths.metadata, std::ios::binary);
    const auto document = nlohmann::json::parse(metadataStream);
    CHECK(document.at("jobId") == 42);
    CHECK(document.at("prompt") == "은발 메이드");
    CHECK(document.at("capture").at("width") == 1024);
    CHECK(document.at("camera").at("target") == nlohmann::json::array({1, 2, 3}));
    CHECK(document.at("hiddenTriangles") == nlohmann::json::array({2, 7}));
    CHECK(document.at("references").size() == 1);

    std::filesystem::remove_all(root, cleanupError);
}
