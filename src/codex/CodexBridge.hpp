#pragma once

#include "core/Types.hpp"

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
#include <thread>
#include <unordered_map>

namespace codextex {

enum class CodexEventType {
    Status,
    Progress,
    GeneratedImage,
    MaskProposalReady,
    Error,
};

struct CodexEvent {
    CodexEventType type{CodexEventType::Status};
    std::string message;
    std::filesystem::path imagePath;
    std::optional<MaskProposal> maskProposal;
};

class CodexBridge {
public:
    CodexBridge() = default;
    ~CodexBridge();
    CodexBridge(const CodexBridge&) = delete;
    CodexBridge& operator=(const CodexBridge&) = delete;

    bool Start(const std::filesystem::path& sessionDirectory,
               const std::filesystem::path& executableOverride = {});
    void Stop();

    [[nodiscard]] bool IsRunning() const noexcept { return running_; }
    [[nodiscard]] bool IsAvailable() const noexcept { return available_; }
    [[nodiscard]] bool IsBusy() const noexcept { return busy_; }
    [[nodiscard]] const std::string& AvailabilityMessage() const noexcept { return availabilityMessage_; }

    bool BeginGeneration(const std::filesystem::path& capturePath, const std::string& userPrompt);
    bool BeginMaskProposal(const std::filesystem::path& capturePath,
                           const std::filesystem::path& generatedPath);
    void Cancel();
    std::vector<CodexEvent> PollEvents();

private:
    enum class Operation { None, Generation, Mask };

    bool LaunchProcess();
    bool InitializeProtocol();
    bool EnsureThread();
    nlohmann::json SendRequest(const std::string& method, nlohmann::json params,
                               std::chrono::milliseconds timeout = std::chrono::seconds(15));
    bool SendLine(const nlohmann::json& message);
    void ReadLoop();
    void HandleMessage(const nlohmann::json& message);
    void PushEvent(CodexEvent event);
    std::filesystem::path CopyGeneratedImage(const std::filesystem::path& source);

    HANDLE process_{nullptr};
    HANDLE processThread_{nullptr};
    HANDLE job_{nullptr};
    HANDLE childStdIn_{nullptr};
    HANDLE childStdOut_{nullptr};
    std::thread reader_;
    std::atomic_bool running_{false};
    std::atomic_bool available_{false};
    std::atomic_bool busy_{false};
    std::atomic<std::uint64_t> nextRequestId_{1};
    std::uint64_t generatedIndex_{};

    std::filesystem::path sessionDirectory_;
    std::filesystem::path executableOverride_;
    std::filesystem::path launchedCommand_;
    std::filesystem::path imagegenSkillPath_;
    std::string availabilityMessage_ = "Codex has not been checked.";
    std::string threadId_;
    std::string activeTurnId_;
    std::atomic<Operation> operation_{Operation::None};

    std::mutex writeMutex_;
    std::mutex stateMutex_;
    std::mutex pendingMutex_;
    std::unordered_map<std::uint64_t, std::shared_ptr<std::promise<nlohmann::json>>> pending_;
    std::mutex eventMutex_;
    std::deque<CodexEvent> events_;
};

} // namespace codextex
