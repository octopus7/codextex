#include "core/AppSettings.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <Windows.h>

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace {

std::filesystem::path SettingsPath(const wchar_t* name) {
    static unsigned counter = 0;
    return std::filesystem::temp_directory_path() / "codextex-tests" /
        (std::wstring(name) + L"-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
         std::to_wstring(++counter) + L".json");
}

} // namespace

TEST_CASE("Missing app settings use Sol medium without creating a file") {
    const auto path = SettingsPath(L"settings-missing");
    codextex::CodexRequestSettings settings;
    bool loaded = true;
    std::string error;
    REQUIRE(codextex::LoadCodexRequestSettings(path, settings, loaded, error));
    CHECK_FALSE(loaded);
    CHECK(settings.model == "gpt-5.6-sol");
    CHECK(settings.reasoningEffort == "medium");
    CHECK(settings.language.empty());
    CHECK(settings.lastImageGenDurationSeconds == 0);
    CHECK_FALSE(std::filesystem::exists(path));
}

TEST_CASE("App settings persist beside the requested binary path") {
    const auto path = SettingsPath(L"settings-roundtrip");
    std::filesystem::create_directories(path.parent_path());
    codextex::CodexRequestSettings written{"gpt-5.6-luna", "low", "ja"};
    written.recentObjPath = LR"(D:\素材\character.obj)";
    written.recentTexturePath = LR"(D:\素材\character.png)";
    written.imageGenPromptHistory = {
        {"은발 양갈래 메이드", 1'725'000'000},
        {"weathered brass armor", 1'724'000'000},
    };
    written.lastImageGenDurationSeconds = 247;
    std::string error;
    REQUIRE(codextex::SaveCodexRequestSettings(path, written, error));

    codextex::CodexRequestSettings loadedSettings;
    bool loaded = false;
    REQUIRE(codextex::LoadCodexRequestSettings(path, loadedSettings, loaded, error));
    CHECK(loaded);
    CHECK(loadedSettings.model == "gpt-5.6-luna");
    CHECK(loadedSettings.reasoningEffort == "low");
    CHECK(loadedSettings.language == "ja");
    CHECK(loadedSettings.recentObjPath == written.recentObjPath);
    CHECK(loadedSettings.recentTexturePath == written.recentTexturePath);
    CHECK(codextex::HasRecentPrimaryAssetPair(loadedSettings));
    CHECK(loadedSettings.imageGenPromptHistory == written.imageGenPromptHistory);
    CHECK(loadedSettings.lastImageGenDurationSeconds == 247);
}

TEST_CASE("ImageGen progress uses the latest successful duration without reaching completion") {
    CHECK(codextex::EstimateImageGenProgress(0.0, 100) == 0.0f);
    CHECK(codextex::EstimateImageGenProgress(50.0, 100) == Catch::Approx(0.45f));
    CHECK(codextex::EstimateImageGenProgress(100.0, 100) == Catch::Approx(0.9f));
    CHECK(codextex::EstimateImageGenProgress(200.0, 100) > 0.9f);
    CHECK(codextex::EstimateImageGenProgress(200.0, 100) < 0.99f);
    CHECK(codextex::EstimateImageGenProgress(100000.0, 100) <= 0.99f);
    CHECK(codextex::EstimateImageGenProgress(150.0, 0) == Catch::Approx(0.45f));
}

TEST_CASE("ImageGen duration is formatted as minutes and padded seconds") {
    CHECK(codextex::FormatImageGenDuration(0) == "0:00");
    CHECK(codextex::FormatImageGenDuration(9) == "0:09");
    CHECK(codextex::FormatImageGenDuration(65) == "1:05");
    CHECK(codextex::FormatImageGenDuration(3'661) == "61:01");
    CHECK(codextex::FormatImageGenDuration(-5) == "0:00");
}

TEST_CASE("ImageGen prompt history is unique newest first and bounded") {
    codextex::CodexRequestSettings settings;
    codextex::AddImageGenPromptToHistory(settings, "first", 100);
    codextex::AddImageGenPromptToHistory(settings, "second", 200);
    codextex::AddImageGenPromptToHistory(settings, "first", 300);
    REQUIRE(settings.imageGenPromptHistory.size() == 2);
    CHECK(settings.imageGenPromptHistory[0] ==
          codextex::ImageGenPromptHistoryEntry{"first", 300});
    CHECK(settings.imageGenPromptHistory[1] ==
          codextex::ImageGenPromptHistoryEntry{"second", 200});

    for (int index = 0; index < 60; ++index) {
        codextex::AddImageGenPromptToHistory(settings, "prompt-" + std::to_string(index),
                                             1'000 + index);
    }
    REQUIRE(settings.imageGenPromptHistory.size() == 50);
    CHECK(settings.imageGenPromptHistory.front().prompt == "prompt-59");
    CHECK(settings.imageGenPromptHistory.front().lastUsedUnixSeconds == 1'059);
    CHECK(settings.imageGenPromptHistory.back().prompt == "prompt-10");
}

TEST_CASE("Prompt history formats compact Twitter-style elapsed time") {
    constexpr std::int64_t now = 2'000'000'000;
    CHECK(codextex::FormatPromptHistoryAge(now, now) == "now");
    CHECK(codextex::FormatPromptHistoryAge(now - 59, now) == "now");
    CHECK(codextex::FormatPromptHistoryAge(now - 60, now) == "1m");
    CHECK(codextex::FormatPromptHistoryAge(now - 3 * 60 * 60, now) == "3h");
    CHECK(codextex::FormatPromptHistoryAge(now - 24 * 60 * 60, now) == "1d");
    CHECK(codextex::FormatPromptHistoryAge(now - 14 * 24 * 60 * 60, now) == "2w");
    CHECK(codextex::FormatPromptHistoryAge(now - 60 * 24 * 60 * 60, now) == "2mo");
    CHECK(codextex::FormatPromptHistoryAge(now - 730LL * 24 * 60 * 60, now) == "2y");
    CHECK(codextex::FormatPromptHistoryAge(now + 60, now) == "now");
}

TEST_CASE("Legacy string prompt history loads and upgrades to timestamped entries") {
    const auto legacyPath = SettingsPath(L"settings-legacy-prompt-history");
    std::filesystem::create_directories(legacyPath.parent_path());
    {
        std::ofstream stream(legacyPath, std::ios::binary | std::ios::trunc);
        stream << R"({"codex":{"model":"gpt-5.6-sol","reasoningEffort":"medium"},"imageGen":{"promptHistory":["newest","older"]}})";
    }
    std::string error;
    codextex::CodexRequestSettings settings;
    bool loaded = false;
    REQUIRE(codextex::LoadCodexRequestSettings(legacyPath, settings, loaded, error));
    REQUIRE(loaded);
    REQUIRE(settings.imageGenPromptHistory.size() == 2);
    CHECK(settings.imageGenPromptHistory[0].prompt == "newest");
    CHECK(settings.imageGenPromptHistory[1].prompt == "older");
    CHECK(settings.imageGenPromptHistory[0].lastUsedUnixSeconds > 0);

    const auto upgradedPath = SettingsPath(L"settings-upgraded-prompt-history");
    REQUIRE(codextex::SaveCodexRequestSettings(upgradedPath, settings, error));
    std::ifstream upgradedStream(upgradedPath, std::ios::binary);
    const nlohmann::json upgraded = nlohmann::json::parse(upgradedStream);
    REQUIRE(upgraded["imageGen"]["promptHistory"][0].is_object());
    CHECK(upgraded["imageGen"]["promptHistory"][0]["prompt"] == "newest");
    CHECK(upgraded["imageGen"]["promptHistory"][0]["lastUsedUnixSeconds"].is_number_integer());
}

TEST_CASE("Incomplete recent primary assets are not persisted") {
    const auto path = SettingsPath(L"settings-incomplete-recent");
    std::filesystem::create_directories(path.parent_path());
    codextex::CodexRequestSettings written;
    written.recentObjPath = LR"(D:\assets\only.obj)";
    std::string error;
    REQUIRE(codextex::SaveCodexRequestSettings(path, written, error));

    codextex::CodexRequestSettings loadedSettings;
    bool loaded = false;
    REQUIRE(codextex::LoadCodexRequestSettings(path, loadedSettings, loaded, error));
    CHECK(loaded);
    CHECK_FALSE(codextex::HasRecentPrimaryAssetPair(loadedSettings));
}

TEST_CASE("Legacy app settings without a UI language remain loadable") {
    const auto path = SettingsPath(L"settings-legacy");
    std::filesystem::create_directories(path.parent_path());
    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream << R"({"codex":{"model":"gpt-5.6-sol","reasoningEffort":"medium"}})";
    }
    std::string error;

    codextex::CodexRequestSettings loadedSettings;
    bool loaded = false;
    REQUIRE(codextex::LoadCodexRequestSettings(path, loadedSettings, loaded, error));
    CHECK(loaded);
    CHECK(loadedSettings.language.empty());
    CHECK(loadedSettings.lastImageGenDurationSeconds == 0);
}
