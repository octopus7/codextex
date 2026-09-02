#pragma once

#include <filesystem>
#include <string>

namespace codextex {

struct CodexRequestSettings {
    std::string model{"gpt-5.6-sol"};
    std::string reasoningEffort{"medium"};
};

bool LoadCodexRequestSettings(const std::filesystem::path& path,
                              CodexRequestSettings& settings,
                              bool& loadedFromDisk,
                              std::string& error);
bool SaveCodexRequestSettings(const std::filesystem::path& path,
                              const CodexRequestSettings& settings,
                              std::string& error);

} // namespace codextex
