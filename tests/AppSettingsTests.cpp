#include "core/AppSettings.hpp"

#include <catch2/catch_test_macros.hpp>
#include <Windows.h>

#include <filesystem>
#include <fstream>

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
    CHECK_FALSE(std::filesystem::exists(path));
}

TEST_CASE("App settings persist beside the requested binary path") {
    const auto path = SettingsPath(L"settings-roundtrip");
    std::filesystem::create_directories(path.parent_path());
    const codextex::CodexRequestSettings written{"gpt-5.6-luna", "low", "ja"};
    std::string error;
    REQUIRE(codextex::SaveCodexRequestSettings(path, written, error));

    codextex::CodexRequestSettings loadedSettings;
    bool loaded = false;
    REQUIRE(codextex::LoadCodexRequestSettings(path, loadedSettings, loaded, error));
    CHECK(loaded);
    CHECK(loadedSettings.model == "gpt-5.6-luna");
    CHECK(loadedSettings.reasoningEffort == "low");
    CHECK(loadedSettings.language == "ja");
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
}
