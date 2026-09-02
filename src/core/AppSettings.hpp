#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace codextex {

struct ImageGenPromptHistoryEntry {
    std::string prompt;
    std::int64_t lastUsedUnixSeconds{};

    bool operator==(const ImageGenPromptHistoryEntry&) const = default;
};

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
    std::vector<ImageGenPromptHistoryEntry> imageGenPromptHistory;
    // Zero means no successful ImageGen duration has been observed yet.
    std::int64_t lastImageGenDurationSeconds{};
};

[[nodiscard]] bool HasRecentPrimaryAssetPair(
    const CodexRequestSettings& settings) noexcept;
void AddImageGenPromptToHistory(CodexRequestSettings& settings,
                                std::string_view prompt);
void AddImageGenPromptToHistory(CodexRequestSettings& settings,
                                std::string_view prompt,
                                std::int64_t usedAtUnixSeconds);
[[nodiscard]] std::string FormatPromptHistoryAge(
    std::int64_t lastUsedUnixSeconds,
    std::int64_t nowUnixSeconds);
[[nodiscard]] float EstimateImageGenProgress(
    double elapsedSeconds,
    std::int64_t lastDurationSeconds) noexcept;

bool LoadCodexRequestSettings(const std::filesystem::path& path,
                              CodexRequestSettings& settings,
                              bool& loadedFromDisk,
                              std::string& error);
bool SaveCodexRequestSettings(const std::filesystem::path& path,
                              const CodexRequestSettings& settings,
                              std::string& error);

} // namespace codextex
