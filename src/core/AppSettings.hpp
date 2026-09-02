#pragma once

#include <filesystem>
#include <string>

namespace codextex {

struct CodexRequestSettings {
    std::string model{"gpt-5.6-sol"};
    std::string reasoningEffort{"medium"};
    // Empty means no language has been persisted yet. The application then
    // selects a language from the Windows user locale without creating a file.
    std::string language;
    // These paths form one atomic recent primary-asset pair. An incomplete
    // pair is neither persisted nor offered by the UI.
    std::filesystem::path recentObjPath;
    std::filesystem::path recentTexturePath;
};

[[nodiscard]] bool HasRecentPrimaryAssetPair(
    const CodexRequestSettings& settings) noexcept;

bool LoadCodexRequestSettings(const std::filesystem::path& path,
                              CodexRequestSettings& settings,
                              bool& loadedFromDisk,
                              std::string& error);
bool SaveCodexRequestSettings(const std::filesystem::path& path,
                              const CodexRequestSettings& settings,
                              std::string& error);

} // namespace codextex
