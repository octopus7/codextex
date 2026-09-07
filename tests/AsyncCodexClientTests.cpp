#include "codex/AsyncCodexClient.hpp"

#include <catch2/catch_test_macros.hpp>
#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

class EnvironmentOverride {
public:
    EnvironmentOverride(const wchar_t* name, const std::wstring& value) : name_(name) {
        const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
        if (size > 0) {
            std::wstring previous(size, L'\0');
            GetEnvironmentVariableW(name, previous.data(), size);
            previous.resize(size - 1);
            previous_ = std::move(previous);
        }
        SetEnvironmentVariableW(name, value.c_str());
    }
    ~EnvironmentOverride() {
        SetEnvironmentVariableW(name_.c_str(), previous_ ? previous_->c_str() : nullptr);
    }
private:
    std::wstring name_;
    std::optional<std::wstring> previous_;
};

struct MockSession {
    std::filesystem::path directory;
    std::filesystem::path entered;
    std::filesystem::path release;
    std::filesystem::path sent;
    std::filesystem::path interrupted;
    std::vector<std::unique_ptr<EnvironmentOverride>> environment;

    MockSession() {
        static unsigned counter = 0;
        directory = std::filesystem::temp_directory_path() / "codextex-tests" /
            (L"async-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
             std::to_wstring(Clock::now().time_since_epoch().count()) + L"-" + std::to_wstring(++counter));
        std::filesystem::create_directories(directory);
        entered = directory / "entered";
        release = directory / "release";
        sent = directory / "sent";
        interrupted = directory / "interrupted";
        Set(L"CODEXTEX_MOCK_MODE", L"hold-generation");
        Set(L"CODEXTEX_MOCK_BLOCK_METHOD", L"");
        Set(L"CODEXTEX_MOCK_BLOCK_OCCURRENCE", L"1");
        Set(L"CODEXTEX_MOCK_DISCONNECT_METHOD", L"");
        Set(L"CODEXTEX_MOCK_LATE_IMAGE_ON_INTERRUPT", L"");
        Set(L"CODEXTEX_MOCK_ENTERED", entered.wstring());
        Set(L"CODEXTEX_MOCK_RELEASE", release.wstring());
        Set(L"CODEXTEX_MOCK_SENT", sent.wstring());
        Set(L"CODEXTEX_MOCK_INTERRUPTED", interrupted.wstring());
        const auto source = directory / "source.png";
        std::ofstream(source) << "mock-png";
        Set(L"CODEXTEX_MOCK_IMAGE", source.wstring());
    }
    void Set(const wchar_t* name, const std::wstring& value) {
        environment.push_back(std::make_unique<EnvironmentOverride>(name, value));
    }
    ~MockSession() {
        // The same variable may be overridden more than once in a test.
        // Undo the overrides as a stack, restoring the original process value.
        while (!environment.empty()) environment.pop_back();
    }
    std::filesystem::path Capture(const std::uint64_t id) const {
        const auto path = directory / (L"projection-" + std::to_wstring(id)) / "capture.png";
        std::filesystem::create_directories(path.parent_path());
        std::ofstream(path) << "mock-capture";
        return path;
    }
    void Release() const { std::ofstream(release) << "release"; }
};

template<class Predicate>
bool WaitUntil(Predicate predicate) {
    const auto deadline = Clock::now() + std::chrono::seconds(4);
    while (Clock::now() < deadline) {
        if (predicate()) return true;
        Sleep(5);
    }
    return predicate();
}

double MillisecondsSince(const Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

void StartReady(codextex::AsyncCodexClient& client, const MockSession& session) {
    REQUIRE(client.Start(session.directory, std::filesystem::path(CODEXTEX_MOCK_CODEX_PATH)));
    REQUIRE(WaitUntil([&] { return !client.IsStarting(); }));
    REQUIRE(client.IsAvailable());
    REQUIRE_FALSE(client.Models().empty());
    (void)client.PollEvents();
}

std::vector<codextex::CodexEvent> WaitForMessage(codextex::AsyncCodexClient& client,
                                                const std::string& message) {
    std::vector<codextex::CodexEvent> events;
    REQUIRE(WaitUntil([&] {
        auto next = client.PollEvents();
        events.insert(events.end(), std::make_move_iterator(next.begin()),
                      std::make_move_iterator(next.end()));
        return std::ranges::any_of(events, [&](const auto& event) {
            return event.message.find(message) != std::string::npos;
        });
    }));
    return events;
}

} // namespace

TEST_CASE("Async Codex startup and snapshots remain responsive while initialize is blocked") {
    MockSession session;
    session.Set(L"CODEXTEX_MOCK_BLOCK_METHOD", L"initialize");
    codextex::AsyncCodexClient client;
    const auto start = Clock::now();
    REQUIRE(client.Start(session.directory, std::filesystem::path(CODEXTEX_MOCK_CODEX_PATH)));
    CHECK(MillisecondsSince(start) < 500);
    REQUIRE(WaitUntil([&] { return std::filesystem::exists(session.entered); }));
    CHECK(client.IsStarting());
    CHECK_FALSE(client.IsAvailable());
    const auto read = Clock::now();
    (void)client.AvailabilityMessage();
    (void)client.Models();
    (void)client.PollEvents();
    CHECK(MillisecondsSince(read) < 500);

    const auto stop = Clock::now();
    client.Stop();
    CHECK(MillisecondsSince(stop) < 2000);
    CHECK_FALSE(client.IsStarting());
    CHECK_FALSE(client.IsRunning());
}

TEST_CASE("Async generation and cancellation do not wait for App Server responses") {
    MockSession session;
    std::wstring method;
    SECTION("thread start") { method = L"thread/start"; }
    SECTION("turn start") { method = L"turn/start"; }
    SECTION("interrupt") { method = L"turn/interrupt"; }
    session.Set(L"CODEXTEX_MOCK_BLOCK_METHOD", method);
    codextex::AsyncCodexClient client;
    StartReady(client, session);
    const auto capture = session.Capture(10);
    const auto begin = Clock::now();
    REQUIRE(client.BeginGeneration(10, capture, "material", "gpt-5.6-sol", "low"));
    CHECK(MillisecondsSince(begin) < 500);
    CHECK(client.IsBusy(10));
    if (method == L"turn/interrupt") {
        (void)WaitForMessage(client, "ImageGen started.");
        const auto cancel = Clock::now();
        client.Cancel(10);
        CHECK(MillisecondsSince(cancel) < 500);
    }
    REQUIRE(WaitUntil([&] { return std::filesystem::exists(session.entered); }));
    const auto read = Clock::now();
    CHECK(client.IsBusy(10));
    (void)client.Models();
    (void)client.PollEvents();
    CHECK(MillisecondsSince(read) < 500);
    const auto stop = Clock::now();
    client.Stop();
    CHECK(MillisecondsSince(stop) < 2000);
}

TEST_CASE("Async startup cannot stay available after its final response is followed by EOF") {
    MockSession session;
    session.Set(L"CODEXTEX_MOCK_MODE", L"exit-after-skills-response");
    codextex::AsyncCodexClient client;
    REQUIRE(client.Start(session.directory, std::filesystem::path(CODEXTEX_MOCK_CODEX_PATH)));
    REQUIRE(WaitUntil([&] { return !client.IsStarting() && !client.IsRunning(); }));
    CHECK_FALSE(client.IsAvailable());
    CHECK(client.AvailabilityMessage().find("disconnected") != std::string::npos);
    CHECK_FALSE(client.BeginGeneration(19, session.Capture(19), "offline", "gpt-5.6-sol", "low"));
}

TEST_CASE("Async completed images stay busy until their events are handed to the UI") {
    MockSession session;
    session.Set(L"CODEXTEX_MOCK_MODE", L"success");
    codextex::AsyncCodexClient client;
    StartReady(client, session);
    REQUIRE(client.BeginGeneration(20, session.Capture(20), "material", "gpt-5.6-sol", "low"));
    REQUIRE(WaitUntil([&] { return std::filesystem::exists(session.sent); }));
    // Do not consume events while the worker observes the completed turn.
    const auto deadline = Clock::now() + std::chrono::milliseconds(250);
    while (Clock::now() < deadline) {
        REQUIRE(client.IsBusy(20));
        Sleep(5);
    }
    const auto events = WaitForMessage(client, "ImageGen completed.");
    CHECK_FALSE(client.IsBusy(20));
    CHECK(std::ranges::count_if(events, [](const auto& event) {
        return event.type == codextex::CodexEventType::GeneratedImage &&
               std::filesystem::is_regular_file(event.imagePath);
    }) == 1);
    client.FinishAfterGeneratedImage(20, true);
    CHECK_FALSE(client.IsBusy(20));
    REQUIRE(WaitUntil([&] { return !std::filesystem::exists(session.directory / "projection-20"); }));
}

TEST_CASE("Async forgotten pending jobs cancel and clean up without disturbing another job") {
    MockSession session;
    session.Set(L"CODEXTEX_MOCK_BLOCK_METHOD", L"turn/start");
    session.Set(L"CODEXTEX_MOCK_BLOCK_OCCURRENCE", L"2");
    session.Set(L"CODEXTEX_MOCK_LATE_IMAGE_ON_INTERRUPT", L"1");
    codextex::AsyncCodexClient client;
    StartReady(client, session);
    REQUIRE(client.BeginGeneration(30, session.Capture(30), "keep", "gpt-5.6-sol", "low"));
    (void)WaitForMessage(client, "ImageGen started.");
    const auto capture = session.Capture(31);
    REQUIRE(client.BeginGeneration(31, capture, "close", "gpt-5.6-sol", "low"));
    REQUIRE(WaitUntil([&] { return std::filesystem::exists(session.entered); }));
    const auto forget = Clock::now();
    client.Forget(31, true);
    CHECK(MillisecondsSince(forget) < 500);
    CHECK_FALSE(client.IsBusy(31));
    CHECK(client.IsBusy(30));
    CHECK(std::filesystem::exists(capture));
    session.Release();
    REQUIRE(WaitUntil([&] { return !std::filesystem::exists(capture.parent_path()); }));
    CHECK(std::filesystem::exists(session.interrupted));
    CHECK(client.IsBusy(30));
    const auto events = client.PollEvents();
    CHECK_FALSE(std::ranges::any_of(events, [](const auto& event) {
        return event.jobId == 31 || event.type == codextex::CodexEventType::Error ||
               event.type == codextex::CodexEventType::GeneratedImage;
    }));
    client.Forget(30, true);
}

TEST_CASE("Async disconnects reject outstanding startup and generation requests promptly") {
    MockSession session;
    std::wstring method;
    SECTION("initialize") { method = L"initialize"; }
    SECTION("thread start") { method = L"thread/start"; }
    SECTION("turn start") { method = L"turn/start"; }
    session.Set(L"CODEXTEX_MOCK_BLOCK_METHOD", method);
    session.Set(L"CODEXTEX_MOCK_DISCONNECT_METHOD", method);
    codextex::AsyncCodexClient client;
    if (method == L"initialize") {
        REQUIRE(client.Start(session.directory, std::filesystem::path(CODEXTEX_MOCK_CODEX_PATH)));
    } else {
        StartReady(client, session);
        REQUIRE(client.BeginGeneration(40, session.Capture(40), "disconnect", "gpt-5.6-sol", "low"));
    }
    REQUIRE(WaitUntil([&] { return std::filesystem::exists(session.entered); }));
    const auto disconnected = Clock::now();
    session.Release();
    (void)WaitForMessage(client, "disconnected");
    CHECK(MillisecondsSince(disconnected) < 2000);
    REQUIRE(WaitUntil([&] { return !client.IsStarting() && !client.IsRunning(); }));
    CHECK_FALSE(client.IsAvailable());
}

TEST_CASE("Async diagnostic failures and file cleanup report actual completion") {
    MockSession session;
    codextex::AsyncCodexClient client;
    StartReady(client, session);
    REQUIRE(client.EnableDiagnosticLog(session.directory / "missing" / "log.txt"));
    const auto failedLog = WaitForMessage(client, "diagnostic log");
    CHECK(std::ranges::any_of(failedLog, [](const auto& event) {
        return event.type == codextex::CodexEventType::Error;
    }));
    const auto external = session.Capture(50);
    const auto recovery = session.Capture(51);
    client.Forget(50, true);
    client.Forget(51, false);
    REQUIRE(WaitUntil([&] { return !std::filesystem::exists(external.parent_path()); }));
    CHECK(std::filesystem::exists(recovery));
    REQUIRE(client.DeleteTemporaryFile(recovery));
    (void)WaitForMessage(client, "Temporary file deleted.");
    CHECK_FALSE(std::filesystem::exists(recovery));
    REQUIRE(client.ClearTemporaryFiles());
    const auto cleared = WaitForMessage(client, "Temporary files cleared.");
    CHECK(std::ranges::any_of(cleared, [](const auto& event) {
        return event.type == codextex::CodexEventType::TemporaryFilesCleared;
    }));
    CHECK(std::filesystem::is_empty(session.directory));
}

TEST_CASE("Async cleanup rejects paths outside its session") {
    MockSession session;
    const auto outside = session.directory.parent_path() / (session.directory.filename().wstring() + L"-outside.txt");
    std::ofstream(outside) << "keep";
    codextex::AsyncCodexClient client;
    StartReady(client, session);
    REQUIRE(client.DeleteTemporaryFile(outside));
    const auto events = WaitForMessage(client, "Could not safely delete");
    CHECK(std::filesystem::is_regular_file(outside));
    CHECK(std::ranges::any_of(events, [](const auto& event) { return event.type == codextex::CodexEventType::Error; }));
    std::filesystem::remove(outside);
}

TEST_CASE("Async shutdown preserves recovery entries and drains queued ordinary cleanup") {
    MockSession session;
    session.Set(L"CODEXTEX_MOCK_BLOCK_METHOD", L"turn/start");
    codextex::AsyncCodexClient client;
    StartReady(client, session);
    const auto capture = session.Capture(60);
    const auto recovery = session.Capture(61);
    REQUIRE(client.BeginGeneration(60, capture, "stop", "gpt-5.6-sol", "low"));
    REQUIRE(WaitUntil([&] { return std::filesystem::exists(session.entered); }));
    client.Forget(60, true);
    client.Forget(61, false);
    const auto stop = Clock::now();
    client.Stop();
    CHECK(MillisecondsSince(stop) < 2000);
    CHECK_FALSE(std::filesystem::exists(capture.parent_path()));
    CHECK(std::filesystem::exists(recovery));
}

TEST_CASE("Async bulk cleanup gates new work until its completion event is consumed") {
    MockSession session;
    session.Set(L"CODEXTEX_MOCK_BLOCK_METHOD", L"turn/start");
    codextex::AsyncCodexClient client;
    StartReady(client, session);
    REQUIRE(client.BeginGeneration(70, session.Capture(70), "old", "gpt-5.6-sol", "low"));
    REQUIRE(WaitUntil([&] { return std::filesystem::exists(session.entered); }));
    client.Forget(70, true);
    REQUIRE(client.ClearTemporaryFiles());
    CHECK(client.IsClearingTemporaryFiles());
    CHECK_FALSE(client.ClearTemporaryFiles());
    CHECK_FALSE(client.Start(session.directory, std::filesystem::path(CODEXTEX_MOCK_CODEX_PATH)));
    const auto futureCapture = session.directory / "projection-71" / "capture.png";
    CHECK_FALSE(client.BeginGeneration(71, futureCapture, "new", "gpt-5.6-sol", "low"));

    SECTION("successful cleanup releases the gate at event handoff") {
        session.Release();
        REQUIRE(WaitUntil([&] { return std::filesystem::is_empty(session.directory); }));
        CHECK(client.IsClearingTemporaryFiles());
        CHECK_FALSE(client.BeginGeneration(71, futureCapture, "new", "gpt-5.6-sol", "low"));
        const auto events = WaitForMessage(client, "Temporary files cleared.");
        CHECK(std::ranges::any_of(events, [](const auto& event) {
            return event.type == codextex::CodexEventType::TemporaryFilesCleared;
        }));
        CHECK_FALSE(client.IsClearingTemporaryFiles());
        REQUIRE(client.BeginGeneration(71, session.Capture(71), "new", "gpt-5.6-sol", "low"));
    }
    SECTION("shutdown releases an unprocessed cleanup gate") {
        client.Stop();
        CHECK_FALSE(client.IsClearingTemporaryFiles());
    }
}

TEST_CASE("Async failed bulk cleanup releases its gate after delivering the error") {
    MockSession session;
    codextex::AsyncCodexClient client;
    StartReady(client, session);
    const auto lockedPath = session.directory / "locked.tmp";
    std::ofstream(lockedPath) << "locked";
    struct FileLock {
        HANDLE handle;
        ~FileLock() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
    } locked{CreateFileW(lockedPath.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL, nullptr)};
    REQUIRE(locked.handle != INVALID_HANDLE_VALUE);
    REQUIRE(client.ClearTemporaryFiles());
    CHECK(client.IsClearingTemporaryFiles());
    CHECK_FALSE(client.BeginGeneration(80, session.directory / "future.png", "new", "gpt-5.6-sol", "low"));
    const auto events = WaitForMessage(client, "Could not safely clear all temporary files.");
    CHECK(std::ranges::any_of(events, [](const auto& event) {
        return event.type == codextex::CodexEventType::Error;
    }));
    CHECK_FALSE(client.IsClearingTemporaryFiles());
    CHECK(std::filesystem::exists(lockedPath));
    REQUIRE(client.BeginGeneration(80, session.Capture(80), "new", "gpt-5.6-sol", "low"));
}
