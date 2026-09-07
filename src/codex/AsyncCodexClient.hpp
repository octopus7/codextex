#pragma once

#include "codex/CodexBridge.hpp"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace codextex {

// UI-facing command queue. Only the worker accesses the blocking bridge; all
// getters and PollEvents return snapshots without taking a bridge or I/O lock.
class AsyncCodexClient {
public:
    AsyncCodexClient();
    ~AsyncCodexClient();
    AsyncCodexClient(const AsyncCodexClient&) = delete;
    AsyncCodexClient& operator=(const AsyncCodexClient&) = delete;

    // These return whether the operation was queued, not its eventual result.
    bool Start(const std::filesystem::path& sessionDirectory,
               const std::filesystem::path& executableOverride = {});
    bool EnableDiagnosticLog(const std::filesystem::path& logPath);
    bool BeginGeneration(std::uint64_t jobId, const std::filesystem::path& capturePath,
                         const std::string& userPrompt, const std::string& model,
                         const std::string& reasoningEffort);
    void FinishAfterGeneratedImage(std::uint64_t jobId, bool cleanupTemporaryFiles = false);
    void Cancel(std::uint64_t jobId);
    void Forget(std::uint64_t jobId, bool cleanupTemporaryFiles = false);
    bool DeleteTemporaryFile(const std::filesystem::path& path);
    bool ClearTemporaryFiles();
    void Stop();

    [[nodiscard]] bool IsStarting() const;
    [[nodiscard]] bool IsRunning() const;
    [[nodiscard]] bool IsAvailable() const;
    [[nodiscard]] bool IsBusy() const;
    [[nodiscard]] bool IsBusy(std::uint64_t jobId) const;
    [[nodiscard]] std::string AvailabilityMessage() const;
    [[nodiscard]] std::vector<CodexModelInfo> Models() const;
    std::vector<CodexEvent> PollEvents();

private:
    enum class CommandType { Start, DiagnosticLog, Generate, Finish, Cancel, Forget,
                             DeleteTemporaryFile, ClearTemporaryFiles };
    struct Command {
        CommandType type{};
        std::uint64_t jobId{};
        std::filesystem::path path;
        std::filesystem::path executable;
        std::string prompt;
        std::string model;
        std::string effort;
    };
    struct JobSnapshot {
        bool busy{};
        bool startPending{};
        bool cancelRequested{};
        bool imageAccepted{};
        bool idlePending{};
        bool terminalEventSeen{};
    };

    void WorkerLoop();
    void Execute(const Command& command);
    void PublishState(bool initializationFinished = false);
    void AddEvent(CodexEvent event);
    void CleanupTemporaryFiles(std::uint64_t jobId);
    bool RemoveTemporaryPath(const std::filesystem::path& path);

    CodexBridge bridge_;
    mutable std::mutex mutex_;
    std::mutex stopMutex_;
    std::condition_variable wake_;
    std::deque<Command> commands_;
    std::deque<CodexEvent> events_;
    std::unordered_map<std::uint64_t, JobSnapshot> jobs_;
    std::unordered_set<std::uint64_t> forgottenJobs_;
    std::unordered_map<std::uint64_t, std::filesystem::path> pendingCleanup_;
    std::filesystem::path sessionDirectory_;
    std::vector<CodexModelInfo> models_;
    std::string availabilityMessage_{"Codex has not been checked."};
    bool starting_{};
    bool running_{};
    bool available_{};
    bool sessionStarted_{};
    bool stopping_{};
    bool workerExited_{};
    std::thread worker_;
};

} // namespace codextex
