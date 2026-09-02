#include "core/GenerationArchive.hpp"

#include <Windows.h>
#include <nlohmann/json.hpp>

#include <fstream>
#include <iomanip>
#include <sstream>

namespace codextex {
namespace {

std::string PathToUtf8(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

std::string Timestamp(const SYSTEMTIME& time, const bool forDirectory) {
    std::ostringstream stream;
    stream << std::setfill('0') << std::setw(4) << time.wYear;
    if (forDirectory) {
        stream << std::setw(2) << time.wMonth << std::setw(2) << time.wDay << 'T'
               << std::setw(2) << time.wHour << std::setw(2) << time.wMinute
               << std::setw(2) << time.wSecond << '-' << std::setw(3) << time.wMilliseconds << 'Z';
    } else {
        stream << '-' << std::setw(2) << time.wMonth << '-' << std::setw(2) << time.wDay << 'T'
               << std::setw(2) << time.wHour << ':' << std::setw(2) << time.wMinute << ':'
               << std::setw(2) << time.wSecond << '.' << std::setw(3) << time.wMilliseconds << 'Z';
    }
    return stream.str();
}

bool ReplaceFile(const std::filesystem::path& temporary,
                 const std::filesystem::path& destination,
                 std::string& error) {
    if (MoveFileExW(temporary.c_str(), destination.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return true;
    }
    error = "Could not finalize a generated-image archive file (Win32 error " +
            std::to_string(GetLastError()) + ").";
    return false;
}

} // namespace

bool SaveGenerationArchive(const std::filesystem::path& root,
                           const std::filesystem::path& sourceImage,
                           const GenerationArchiveMetadata& metadata,
                           GenerationArchivePaths& paths,
                           std::string& error) {
    paths = {};
    error.clear();
    if (root.empty() || sourceImage.empty() || !std::filesystem::is_regular_file(sourceImage)) {
        error = "The generated image or permanent archive path is unavailable.";
        return false;
    }

    SYSTEMTIME now{};
    GetSystemTime(&now);
    std::error_code fileError;
    std::filesystem::create_directories(root, fileError);
    if (fileError) {
        error = "Could not create the permanent generated-image directory.";
        return false;
    }

    const std::wstring baseName = std::filesystem::path(
        Timestamp(now, true) + "-job-" + std::to_string(metadata.jobId)).wstring();
    std::filesystem::path directory;
    for (std::uint32_t suffix = 0; suffix < 1000; ++suffix) {
        directory = root / (baseName + (suffix == 0 ? L"" : L"-" + std::to_wstring(suffix)));
        if (std::filesystem::create_directory(directory, fileError)) break;
        if (fileError) {
            error = "Could not create a generated-image archive entry.";
            return false;
        }
        directory.clear();
    }
    if (directory.empty()) {
        error = "Could not allocate a unique generated-image archive entry.";
        return false;
    }

    const auto image = directory / L"image.png";
    const auto imageTemporary = directory / L"image.png.tmp";
    std::filesystem::copy_file(sourceImage, imageTemporary,
                               std::filesystem::copy_options::overwrite_existing, fileError);
    if (fileError || !ReplaceFile(imageTemporary, image, error)) {
        std::filesystem::remove_all(directory, fileError);
        if (error.empty()) error = "Could not copy the generated image into permanent storage.";
        return false;
    }

    nlohmann::json hidden = nlohmann::json::array();
    for (const std::uint32_t triangle : metadata.hiddenTriangles) hidden.push_back(triangle);
    nlohmann::json references = nlohmann::json::array();
    for (const auto& reference : metadata.references) {
        references.push_back({{"obj", PathToUtf8(reference.objPath)},
                              {"texture", PathToUtf8(reference.texturePath)}});
    }
    const nlohmann::json document = {
        {"version", 1},
        {"createdAtUtc", Timestamp(now, false)},
        {"jobId", metadata.jobId},
        {"prompt", metadata.prompt},
        {"model", metadata.model},
        {"reasoningEffort", metadata.reasoningEffort},
        {"primaryAssets", {{"obj", PathToUtf8(metadata.objPath)},
                            {"texture", PathToUtf8(metadata.texturePath)}}},
        {"camera", {{"target", {metadata.cameraTarget.x, metadata.cameraTarget.y,
                                  metadata.cameraTarget.z}},
                    {"yaw", metadata.cameraYaw},
                    {"pitch", metadata.cameraPitch},
                    {"distance", metadata.cameraDistance},
                    {"fovDegrees", metadata.cameraFovDegrees}}},
        {"capture", {{"width", metadata.captureWidth},
                      {"height", metadata.captureHeight},
                      {"cropX", metadata.cropX},
                      {"cropY", metadata.cropY},
                      {"cropSize", metadata.cropSize}}},
        {"hiddenTriangles", std::move(hidden)},
        {"viewport", {{"referenceAssetsVisible", metadata.referenceAssetsVisible},
                       {"shadingEnabled", metadata.shadingEnabled},
                       {"backgroundColor", metadata.backgroundColor}}},
        {"references", std::move(references)},
    };

    const auto metadataPath = directory / L"metadata.json";
    const auto metadataTemporary = directory / L"metadata.json.tmp";
    try {
        std::ofstream stream(metadataTemporary, std::ios::binary | std::ios::trunc);
        stream << document.dump(2) << '\n';
        stream.close();
        if (!stream || !ReplaceFile(metadataTemporary, metadataPath, error)) {
            std::filesystem::remove_all(directory, fileError);
            if (error.empty()) error = "Could not write generated-image metadata.";
            return false;
        }
    } catch (const std::exception& exception) {
        std::filesystem::remove_all(directory, fileError);
        error = std::string("Could not write generated-image metadata: ") + exception.what();
        return false;
    }

    paths = {directory, image, metadataPath};
    return true;
}

} // namespace codextex
