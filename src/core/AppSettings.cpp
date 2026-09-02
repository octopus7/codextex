#include "core/AppSettings.hpp"

#include <Windows.h>
#include <nlohmann/json.hpp>

#include <fstream>
#include <system_error>

namespace codextex {

bool LoadCodexRequestSettings(const std::filesystem::path& path,
                              CodexRequestSettings& settings,
                              bool& loadedFromDisk,
                              std::string& error) {
    settings = {};
    loadedFromDisk = false;
    error.clear();

    std::error_code fileError;
    if (!std::filesystem::exists(path, fileError)) {
        if (fileError) error = "Could not inspect CodexTex.settings.json.";
        return !fileError;
    }

    try {
        std::ifstream stream(path, std::ios::binary);
        if (!stream) {
            error = "Could not open CodexTex.settings.json.";
            return false;
        }
        const nlohmann::json document = nlohmann::json::parse(stream);
        const auto& codex = document.at("codex");
        const std::string model = codex.at("model").get<std::string>();
        const std::string effort = codex.at("reasoningEffort").get<std::string>();
        if (model.empty() || effort.empty()) {
            error = "CodexTex.settings.json contains an empty model or reasoning effort.";
            return false;
        }
        settings.model = model;
        settings.reasoningEffort = effort;
        loadedFromDisk = true;
        return true;
    } catch (const std::exception& exception) {
        error = std::string("Could not read CodexTex.settings.json: ") + exception.what();
        return false;
    }
}

bool SaveCodexRequestSettings(const std::filesystem::path& path,
                              const CodexRequestSettings& settings,
                              std::string& error) {
    error.clear();
    if (path.empty()) {
        error = "Could not locate the executable directory for Codex settings.";
        return false;
    }
    if (settings.model.empty() || settings.reasoningEffort.empty()) {
        error = "The Codex model settings are incomplete.";
        return false;
    }

    const std::filesystem::path temporary = path.wstring() + L".tmp";
    try {
        const nlohmann::json document = {
            {"codex", {{"model", settings.model},
                       {"reasoningEffort", settings.reasoningEffort}}},
        };
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) {
            error = "Could not create CodexTex.settings.json beside the executable.";
            return false;
        }
        stream << document.dump(2) << '\n';
        stream.close();
        if (!stream) {
            error = "Could not finish writing CodexTex.settings.json.";
            DeleteFileW(temporary.c_str());
            return false;
        }
    } catch (const std::exception& exception) {
        error = std::string("Could not write CodexTex.settings.json: ") + exception.what();
        DeleteFileW(temporary.c_str());
        return false;
    }

    if (!MoveFileExW(temporary.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = "Could not replace CodexTex.settings.json (Win32 error " +
            std::to_string(GetLastError()) + ").";
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

} // namespace codextex
