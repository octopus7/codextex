#include "core/AppSettings.hpp"

#include <Windows.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <system_error>

namespace codextex {
namespace {

std::string PathToUtf8(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

std::filesystem::path PathFromUtf8(const std::string& value) {
    const std::u8string utf8(reinterpret_cast<const char8_t*>(value.data()), value.size());
    return std::filesystem::path(utf8);
}

} // namespace

bool HasRecentPrimaryAssetPair(const CodexRequestSettings& settings) noexcept {
    return !settings.recentObjPath.empty() && !settings.recentTexturePath.empty();
}

void AddImageGenPromptToHistory(CodexRequestSettings& settings,
                                const std::string_view prompt) {
    if (prompt.empty()) return;
    constexpr std::size_t kMaximumEntries = 50;
    auto& history = settings.imageGenPromptHistory;
    std::erase(history, prompt);
    history.insert(history.begin(), std::string(prompt));
    if (history.size() > kMaximumEntries) history.resize(kMaximumEntries);
}

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
        if (const auto ui = document.find("ui"); ui != document.end() && ui->is_object()) {
            if (const auto language = ui->find("language");
                language != ui->end() && language->is_string()) {
                settings.language = language->get<std::string>();
            }
        }
        if (const auto recent = document.find("recentPrimaryAssets");
            recent != document.end() && recent->is_object()) {
            const auto obj = recent->find("obj");
            const auto texture = recent->find("texture");
            if (obj != recent->end() && obj->is_string() &&
                texture != recent->end() && texture->is_string()) {
                const std::string objPath = obj->get<std::string>();
                const std::string texturePath = texture->get<std::string>();
                if (!objPath.empty() && !texturePath.empty()) {
                    settings.recentObjPath = PathFromUtf8(objPath);
                    settings.recentTexturePath = PathFromUtf8(texturePath);
                }
            }
        }
        if (const auto imageGen = document.find("imageGen");
            imageGen != document.end() && imageGen->is_object()) {
            if (const auto history = imageGen->find("promptHistory");
                history != imageGen->end() && history->is_array()) {
                for (auto item = history->rbegin(); item != history->rend(); ++item) {
                    if (item->is_string()) {
                        AddImageGenPromptToHistory(settings, item->get<std::string>());
                    }
                }
            }
        }
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
        nlohmann::json document = {
            {"codex", {{"model", settings.model},
                       {"reasoningEffort", settings.reasoningEffort}}},
            {"ui", {{"language", settings.language}}},
        };
        if (HasRecentPrimaryAssetPair(settings)) {
            document["recentPrimaryAssets"] = {
                {"obj", PathToUtf8(settings.recentObjPath)},
                {"texture", PathToUtf8(settings.recentTexturePath)},
            };
        }
        if (!settings.imageGenPromptHistory.empty()) {
            document["imageGen"]["promptHistory"] = settings.imageGenPromptHistory;
        }
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
