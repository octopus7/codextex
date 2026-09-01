#include "codex/CodexBridge.hpp"

#include <catch2/catch_test_macros.hpp>
#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
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
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++counter);
    const auto path = std::filesystem::temp_directory_path() / "codextex-tests" / uniqueName;
    std::filesystem::create_directories(path);
    return path;
}

std::vector<codextex::CodexEvent> WaitForIdle(codextex::CodexBridge& bridge) {
    std::vector<codextex::CodexEvent> events;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    do {
        auto next = bridge.PollEvents();
        events.insert(events.end(), std::make_move_iterator(next.begin()),
                      std::make_move_iterator(next.end()));
        if (!bridge.IsBusy()) {
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

TEST_CASE("App Server mock delivers generated images and structured mask proposals") {
    ScopedEnvironment mode(L"CODEXTEX_MOCK_MODE", L"success");
    const auto session = Session(L"mock-success");
    const auto sourceImage = session / "mock-source.png";
    const auto capture = session / "capture.png";
    std::ofstream(sourceImage, std::ios::binary) << "mock-png";
    std::ofstream(capture, std::ios::binary) << "mock-capture";
    ScopedEnvironment image(L"CODEXTEX_MOCK_IMAGE", sourceImage.c_str());

    codextex::CodexBridge bridge;
    REQUIRE(bridge.Start(session, std::filesystem::path(CODEXTEX_MOCK_CODEX_PATH)));
    REQUIRE(bridge.IsAvailable());
    REQUIRE(bridge.BeginGeneration(capture, "mock material"));
    const auto generationEvents = WaitForIdle(bridge);
    const auto generated = std::find_if(generationEvents.begin(), generationEvents.end(),
        [](const auto& event) { return event.type == codextex::CodexEventType::GeneratedImage; });
    REQUIRE(generated != generationEvents.end());
    CHECK(std::filesystem::exists(generated->imagePath));

    REQUIRE(bridge.BeginMaskProposal(capture, generated->imagePath));
    const auto maskEvents = WaitForIdle(bridge);
    const auto proposal = std::find_if(maskEvents.begin(), maskEvents.end(),
        [](const auto& event) { return event.type == codextex::CodexEventType::MaskProposalReady; });
    REQUIRE(proposal != maskEvents.end());
    REQUIRE(proposal->maskProposal.has_value());
    CHECK(proposal->maskProposal->suggestedFeatherPx == 12);
}

TEST_CASE("App Server mock interrupts an active generation") {
    ScopedEnvironment mode(L"CODEXTEX_MOCK_MODE", L"hold-generation");
    const auto session = Session(L"mock-cancel");
    const auto capture = session / "capture.png";
    std::ofstream(capture, std::ios::binary) << "mock-capture";

    codextex::CodexBridge bridge;
    REQUIRE(bridge.Start(session, std::filesystem::path(CODEXTEX_MOCK_CODEX_PATH)));
    REQUIRE(bridge.BeginGeneration(capture, "hold"));
    REQUIRE(bridge.IsBusy());
    bridge.Cancel();
    const auto events = WaitForIdle(bridge);
    CHECK_FALSE(bridge.IsBusy());
    CHECK(std::any_of(events.begin(), events.end(), [](const auto& event) {
        return event.type == codextex::CodexEventType::Error &&
               event.message.find("interrupted") != std::string::npos;
    }));
}
