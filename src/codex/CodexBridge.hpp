#pragma once

#include <Windows.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace codextex {

enum class CodexEventType {
    Status,
    Progress,
    GeneratedImage,
    Error,
    TemporaryFilesCleared,
};

struct CodexEvent {
    CodexEventType type{CodexEventType::Status};
    std::string message;
    std::filesystem::path imagePath;
    std::uint64_t jobId{};
};

struct CodexReasoningOption {
    std::string value;
    std::string description;
};

struct CodexModelInfo {
    std::string id;
    std::string displayName;
    std::string defaultReasoningEffort;
    std::vector<CodexReasoningOption> supportedReasoningEfforts;
};

class CodexBridge {
public:
    CodexBridge() = default;
    ~CodexBridge();
    CodexBridge(const CodexBridge&) = delete;
    CodexBridge& operator=(const CodexBridge&) = delete;

    bool Start(const std::filesystem::path& sessionDirectory,
               const std::filesystem::path& executableOverride = {});
    bool EnableDiagnosticLog(const std::filesystem::path& logPath);
    void Stop();
    // Thread-safe terminal abort used to unblock the owner before Stop.
    // Construct a new bridge if another session is needed after this call.
    void AbortPendingRequests();

    [[nodiscard]] bool IsRunning() const noexcept { return running_; }
    [[nodiscard]] bool IsAvailable() const noexcept { return available_ && running_; }
    [[nodiscard]] bool IsBusy() const noexcept;
    [[nodiscard]] bool IsBusy(std::uint64_t jobId) const noexcept;
    [[nodiscard]] const std::string& AvailabilityMessage() const noexcept { return availabilityMessage_; }
    [[nodiscard]] const std::vector<CodexModelInfo>& Models() const noexcept { return models_; }

    bool BeginGeneration(std::uint64_t jobId, const std::filesystem::path& capturePath,
                         const std::string& userPrompt, const std::string& model,
                         const std::string& reasoningEffort);
    void FinishAfterGeneratedImage(std::uint64_t jobId);
    void Cancel(std::uint64_t jobId);
    void Forget(std::uint64_t jobId);
    std::vector<CodexEvent> PollEvents();

private:
    bool LaunchProcess();
    bool InitializeProtocol();
    struct JobState {
        std::string threadId;
        std::string activeTurnId;
        std::string model;
        std::string reasoningEffort;
        bool busy{};
        bool generatedImageAccepted{};
        bool turnStartPending{};
        std::deque<nlohmann::json> pendingTurnEvents;
    };

    struct PendingRequest {
        std::shared_ptr<std::promise<nlohmann::json>> promise;
        std::optional<std::uint64_t> turnStartJobId;
    };

    bool EnsureThread(std::uint64_t jobId, const std::string& model);
    std::optional<std::uint64_t> FindJob(const nlohmann::json& params) const;
    nlohmann::json SendRequest(const std::string& method, nlohmann::json params,
                               std::chrono::milliseconds timeout = std::chrono::seconds(15),
                               std::optional<std::uint64_t> turnStartJobId = std::nullopt);
    bool SendLine(const nlohmann::json& message);
    void ReadLoop();
    void HandleMessage(const nlohmann::json& message);
    void PushEvent(CodexEvent event);
    void FailPendingRequests(const char* message);
    void CompleteRequest(std::uint64_t id, nlohmann::json result,
                         std::exception_ptr error = {});
    void LogDiagnostic(std::string_view message);
    std::filesystem::path CopyGeneratedImage(std::uint64_t jobId,
                                             const std::filesystem::path& source);

    HANDLE process_{nullptr};
    HANDLE processThread_{nullptr};
    HANDLE job_{nullptr};
    HANDLE childStdIn_{nullptr};
    HANDLE childStdOut_{nullptr};
    std::thread reader_;
    std::atomic_bool running_{false};
    std::atomic_bool available_{false};
    std::atomic_bool abortRequested_{false};
    std::atomic<std::uint64_t> nextRequestId_{1};
    std::uint64_t generatedIndex_{};

    std::filesystem::path sessionDirectory_;
    std::filesystem::path executableOverride_;
    std::filesystem::path launchedCommand_;
    std::filesystem::path imagegenSkillPath_;
    std::filesystem::path diagnosticLogPath_;
    std::string availabilityMessage_ = "Codex has not been checked.";
    std::vector<CodexModelInfo> models_;
    std::unordered_map<std::uint64_t, JobState> jobs_;
    std::unordered_map<std::string, std::uint64_t> jobsByThread_;

    std::mutex writeMutex_;
    std::mutex ioMutex_;
    HANDLE writerIoThread_{nullptr};
    HANDLE readerIoThread_{nullptr};
    mutable std::mutex stateMutex_;
    std::mutex pendingMutex_;
    std::unordered_map<std::uint64_t, PendingRequest> pending_;
    std::mutex eventMutex_;
    std::deque<CodexEvent> events_;
    std::mutex logMutex_;
};

} // namespace codextex
