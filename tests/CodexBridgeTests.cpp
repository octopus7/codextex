#include "codex/CodexBridge.hpp"

#include <catch2/catch_test_macros.hpp>
#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace {

class ScopedEnvironment {
public:
    ScopedEnvironment(const wchar_t* name, const wchar_t* value) : name_(name) {
        const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
        if (size > 0) {
            std::wstring existing(size, L'\0');
            GetEnvironmentVariableW(name, existing.data(), size);
            existing.resize(size - 1);
            previous_ = std::move(existing);
        }
        SetEnvironmentVariableW(name, value);
    }

    ~ScopedEnvironment() {
        SetEnvironmentVariableW(name_.c_str(), previous_ ? previous_->c_str() : nullptr);
    }

private:
    std::wstring name_;
    std::optional<std::wstring> previous_;
};

std::filesystem::path Session(const wchar_t* name) {
    static unsigned counter = 0;
    const auto uniqueName = std::wstring(name) + L"-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" +
        std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count()) + L"-" +
        std::to_wstring(++counter);
    const auto path = std::filesystem::temp_directory_path() / "codextex-tests" / uniqueName;
    std::filesystem::create_directories(path);
    return path;
}

std::vector<codextex::CodexEvent> WaitForIdle(codextex::CodexBridge& bridge,
                                              const std::uint64_t jobId) {
    std::vector<codextex::CodexEvent> events;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    do {
        auto next = bridge.PollEvents();
        events.insert(events.end(), std::make_move_iterator(next.begin()),
                      std::make_move_iterator(next.end()));
        if (!bridge.IsBusy(jobId)) {
            Sleep(20);
            auto finalEvents = bridge.PollEvents();
            events.insert(events.end(), std::make_move_iterator(finalEvents.begin()),
                          std::make_move_iterator(finalEvents.end()));
            break;
        }
        Sleep(10);
    } while (std::chrono::steady_clock::now() < deadline);
    return events;
}

} // namespace

TEST_CASE("App Server mock disables AI when signed out or imagegen is missing") {
    const auto executable = std::filesystem::path(CODEXTEX_MOCK_CODEX_PATH);
    SECTION("signed out") {
        ScopedEnvironment mode(L"CODEXTEX_MOCK_MODE", L"signed-out");
        codextex::CodexBridge bridge;
        REQUIRE(bridge.Start(Session(L"mock-signed-out"), executable));
        CHECK_FALSE(bridge.IsAvailable());
        CHECK(bridge.AvailabilityMessage().find("not signed in") != std::string::npos);
    }
    SECTION("missing skill") {
        ScopedEnvironment mode(L"CODEXTEX_MOCK_MODE", L"missing-skill");
        codextex::CodexBridge bridge;
        REQUIRE(bridge.Start(Session(L"mock-missing-skill"), executable));
        CHECK_FALSE(bridge.IsAvailable());
        CHECK(bridge.AvailabilityMessage().find("unavailable") != std::string::npos);
    }
}

TEST_CASE("App Server can launch through a codex.cmd command shim") {
    ScopedEnvironment mode(L"CODEXTEX_MOCK_MODE", L"success");
    const auto session = Session(L"mock-command-shim");
    const auto shim = session / L"codex.cmd";
    std::ofstream command(shim, std::ios::binary | std::ios::trunc);
    command << "@echo off\r\n\"" << std::filesystem::path(CODEXTEX_MOCK_CODEX_PATH).string()
            << "\" %*\r\n";
    command.close();

    codextex::CodexBridge bridge;
    REQUIRE(bridge.Start(session, shim));
    CHECK(bridge.IsAvailable());
    CHECK(bridge.AvailabilityMessage().find("codex.cmd") != std::string::npos);
}

TEST_CASE("App Server mock delivers generated images") {
    ScopedEnvironment mode(L"CODEXTEX_MOCK_MODE", L"success");
    const auto session = Session(L"mock-success");
    const auto sourceImage = session / "mock-source.png";
    const auto capture = session / "capture.png";
    const auto diagnosticLog = session / "CodexTex-ImageGen.log";
    std::ofstream(sourceImage, std::ios::binary) << "mock-png";
    std::ofstream(capture, std::ios::binary) << "mock-capture";
    ScopedEnvironment image(L"CODEXTEX_MOCK_IMAGE", sourceImage.c_str());

    codextex::CodexBridge bridge;
    REQUIRE(bridge.EnableDiagnosticLog(diagnosticLog));
    REQUIRE(bridge.Start(session, std::filesystem::path(CODEXTEX_MOCK_CODEX_PATH)));
    REQUIRE(bridge.IsAvailable());
    constexpr std::uint64_t jobId = 7;
    REQUIRE(bridge.BeginGeneration(jobId, capture, "mock material",
                                   "gpt-5.6-sol", "medium"));
    const auto generationEvents = WaitForIdle(bridge, jobId);
    const auto generated = std::find_if(generationEvents.begin(), generationEvents.end(),
        [](const auto& event) { return event.type == codextex::CodexEventType::GeneratedImage; });
    REQUIRE(generated != generationEvents.end());
    CHECK(generated->jobId == jobId);
    CHECK(std::filesystem::exists(generated->imagePath));

    std::ifstream log(diagnosticLog, std::ios::binary);
    std::ostringstream logText;
    logText << log.rdbuf();
    CHECK(logText.str().find("CodexTex ImageGen session started") != std::string::npos);
    CHECK(logText.str().find("\"method\":\"thread/start\"") != std::string::npos);
    CHECK(logText.str().find("\"sandbox\":\"workspace-write\"") != std::string::npos);
    CHECK(logText.str().find("\"method\":\"turn/start\"") != std::string::npos);
    CHECK(logText.str().find("\"model\":\"gpt-5.6-sol\"") != std::string::npos);
    CHECK(logText.str().find("\"effort\":\"medium\"") != std::string::npos);
    CHECK(logText.str().find("\"type\":\"imageGeneration\"") != std::string::npos);
}

TEST_CASE("App Server falls back to the legacy camel-case sandbox mode") {
    ScopedEnvironment mode(L"CODEXTEX_MOCK_MODE", L"legacy-sandbox");
    const auto session = Session(L"mock-legacy-sandbox");
    const auto sourceImage = session / "mock-source.png";
    const auto capture = session / "capture.png";
    std::ofstream(sourceImage, std::ios::binary) << "mock-png";
    std::ofstream(capture, std::ios::binary) << "mock-capture";
    ScopedEnvironment image(L"CODEXTEX_MOCK_IMAGE", sourceImage.c_str());

    codextex::CodexBridge bridge;
    REQUIRE(bridge.Start(session, std::filesystem::path(CODEXTEX_MOCK_CODEX_PATH)));
    REQUIRE(bridge.BeginGeneration(1, capture, "legacy sandbox",
                                   "gpt-5.6-sol", "medium"));
    const auto events = WaitForIdle(bridge, 1);
    CHECK(std::any_of(events.begin(), events.end(), [](const auto& event) {
        return event.type == codextex::CodexEventType::GeneratedImage;
    }));
}

TEST_CASE("App Server mock interrupts an active generation") {
    ScopedEnvironment mode(L"CODEXTEX_MOCK_MODE", L"hold-generation");
    const auto session = Session(L"mock-cancel");
    const auto capture = session / "capture.png";
    std::ofstream(capture, std::ios::binary) << "mock-capture";

    codextex::CodexBridge bridge;
    REQUIRE(bridge.Start(session, std::filesystem::path(CODEXTEX_MOCK_CODEX_PATH)));
    REQUIRE(bridge.BeginGeneration(11, capture, "hold", "gpt-5.6-sol", "medium"));
    REQUIRE(bridge.IsBusy(11));
    bridge.Cancel(11);
    const auto events = WaitForIdle(bridge, 11);
    CHECK_FALSE(bridge.IsBusy(11));
    CHECK(std::any_of(events.begin(), events.end(), [](const auto& event) {
        return event.type == codextex::CodexEventType::Error &&
               event.message.find("interrupted") != std::string::npos;
    }));
}

TEST_CASE("App Server routes concurrent projection jobs independently") {
    ScopedEnvironment mode(L"CODEXTEX_MOCK_MODE", L"hold-generation");
    const auto session = Session(L"mock-concurrent");
    const auto capture = session / "capture.png";
    std::ofstream(capture, std::ios::binary) << "mock-capture";

    codextex::CodexBridge bridge;
    REQUIRE(bridge.Start(session, std::filesystem::path(CODEXTEX_MOCK_CODEX_PATH)));
    REQUIRE(bridge.BeginGeneration(21, capture, "first", "gpt-5.6-sol", "low"));
    REQUIRE(bridge.BeginGeneration(22, capture, "second", "gpt-5.6-luna", "medium"));
    CHECK(bridge.IsBusy(21));
    CHECK(bridge.IsBusy(22));

    bridge.Cancel(21);
    const auto firstEvents = WaitForIdle(bridge, 21);
    CHECK_FALSE(bridge.IsBusy(21));
    CHECK(bridge.IsBusy(22));
    CHECK(std::any_of(firstEvents.begin(), firstEvents.end(), [](const auto& event) {
        return event.jobId == 21 && event.type == codextex::CodexEventType::Error;
    }));

    bridge.Cancel(22);
    const auto secondEvents = WaitForIdle(bridge, 22);
    CHECK_FALSE(bridge.IsBusy(22));
    CHECK(std::any_of(secondEvents.begin(), secondEvents.end(), [](const auto& event) {
        return event.jobId == 22 && event.type == codextex::CodexEventType::Error;
    }));
}
