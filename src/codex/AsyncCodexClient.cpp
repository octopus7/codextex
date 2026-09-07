#include "codex/AsyncCodexClient.hpp"

#include <algorithm>
#include <chrono>

namespace codextex {

AsyncCodexClient::AsyncCodexClient() : worker_(&AsyncCodexClient::WorkerLoop, this) {}

AsyncCodexClient::~AsyncCodexClient() {
    Stop();
}

bool AsyncCodexClient::Start(const std::filesystem::path& sessionDirectory,
                             const std::filesystem::path& executableOverride) {
    std::scoped_lock lock(mutex_);
    if (stopping_ || starting_ || std::ranges::any_of(jobs_, [](const auto& entry) {
            return entry.second.busy;
        })) return false;
    starting_ = true;
    available_ = false;
    sessionStarted_ = false;
    sessionDirectory_ = sessionDirectory;
    availabilityMessage_ = "Checking Codex App Server.";
    Command command{CommandType::Start};
    command.path = sessionDirectory;
    command.executable = executableOverride;
    commands_.push_back(std::move(command));
    wake_.notify_one();
    return true;
}

bool AsyncCodexClient::EnableDiagnosticLog(const std::filesystem::path& logPath) {
    std::scoped_lock lock(mutex_);
    if (stopping_) return false;
    Command command{CommandType::DiagnosticLog};
    command.path = logPath;
    commands_.push_back(std::move(command));
    wake_.notify_one();
    return true;
}

bool AsyncCodexClient::BeginGeneration(const std::uint64_t jobId,
                                       const std::filesystem::path& capturePath,
                                       const std::string& userPrompt,
                                       const std::string& model,
                                       const std::string& reasoningEffort) {
    std::scoped_lock lock(mutex_);
    if (stopping_ || starting_ || !available_ || jobId == 0 || capturePath.empty() ||
        userPrompt.empty() || model.empty() || reasoningEffort.empty() ||
        forgottenJobs_.contains(jobId) || jobs_.contains(jobId)) return false;
    jobs_.emplace(jobId, JobSnapshot{true, true});
    Command command{CommandType::Generate};
    command.jobId = jobId;
    command.path = capturePath;
    command.prompt = userPrompt;
    command.model = model;
    command.effort = reasoningEffort;
    commands_.push_back(std::move(command));
    wake_.notify_one();
    return true;
}

void AsyncCodexClient::Cancel(const std::uint64_t jobId) {
    std::scoped_lock lock(mutex_);
    const auto found = jobs_.find(jobId);
    if (stopping_ || found == jobs_.end() || !found->second.busy ||
        found->second.cancelRequested) return;
    found->second.cancelRequested = true;
    commands_.push_front(Command{CommandType::Cancel, jobId});
    wake_.notify_one();
}

void AsyncCodexClient::FinishAfterGeneratedImage(const std::uint64_t jobId,
                                                 const bool cleanupTemporaryFiles) {
    std::scoped_lock lock(mutex_);
    const auto found = jobs_.find(jobId);
    if (stopping_ || found == jobs_.end() || found->second.imageAccepted) return;
    found->second.imageAccepted = true;
    found->second.busy = false;
    found->second.idlePending = false;
    if (cleanupTemporaryFiles && !sessionDirectory_.empty()) {
        pendingCleanup_[jobId] = sessionDirectory_ / (L"projection-" + std::to_wstring(jobId));
    }
    std::erase_if(events_, [jobId](const auto& event) { return event.jobId == jobId; });
    commands_.push_front(Command{CommandType::Finish, jobId});
    wake_.notify_one();
}

void AsyncCodexClient::Forget(const std::uint64_t jobId, const bool cleanupTemporaryFiles) {
    std::scoped_lock lock(mutex_);
    if (stopping_ || !forgottenJobs_.insert(jobId).second) return;
    if (cleanupTemporaryFiles && !sessionDirectory_.empty()) {
        pendingCleanup_[jobId] = sessionDirectory_ / (L"projection-" + std::to_wstring(jobId));
    }
    jobs_.erase(jobId);
    std::erase_if(events_, [jobId](const auto& event) { return event.jobId == jobId; });
    std::erase_if(commands_, [jobId](const auto& command) {
        return command.jobId == jobId && command.type != CommandType::Start &&
               command.type != CommandType::DiagnosticLog;
    });
    commands_.push_front(Command{CommandType::Forget, jobId});
    wake_.notify_one();
}

bool AsyncCodexClient::DeleteTemporaryFile(const std::filesystem::path& path) {
    std::scoped_lock lock(mutex_);
    if (stopping_ || path.empty()) return false;
    Command command{CommandType::DeleteTemporaryFile};
    command.path = path;
    commands_.push_back(std::move(command));
    wake_.notify_one();
    return true;
}

bool AsyncCodexClient::ClearTemporaryFiles() {
    std::scoped_lock lock(mutex_);
    if (stopping_ || sessionDirectory_.empty()) return false;
    commands_.push_back(Command{CommandType::ClearTemporaryFiles});
    wake_.notify_one();
    return true;
}

bool AsyncCodexClient::IsStarting() const {
    std::scoped_lock lock(mutex_);
    return starting_;
}

bool AsyncCodexClient::IsRunning() const {
    std::scoped_lock lock(mutex_);
    return running_;
}

bool AsyncCodexClient::IsAvailable() const {
    std::scoped_lock lock(mutex_);
    return available_;
}

bool AsyncCodexClient::IsBusy() const {
    std::scoped_lock lock(mutex_);
    return std::ranges::any_of(jobs_, [](const auto& entry) { return entry.second.busy; });
}

bool AsyncCodexClient::IsBusy(const std::uint64_t jobId) const {
    std::scoped_lock lock(mutex_);
    const auto found = jobs_.find(jobId);
    return found != jobs_.end() && found->second.busy;
}

std::string AsyncCodexClient::AvailabilityMessage() const {
    std::scoped_lock lock(mutex_);
    return availabilityMessage_;
}

std::vector<CodexModelInfo> AsyncCodexClient::Models() const {
    std::scoped_lock lock(mutex_);
    return models_;
}

std::vector<CodexEvent> AsyncCodexClient::PollEvents() {
    std::scoped_lock lock(mutex_);
    std::vector<CodexEvent> events;
    events.reserve(events_.size());
    while (!events_.empty()) {
        events.push_back(std::move(events_.front()));
        events_.pop_front();
    }
    // The UI owns image files only after receiving their events. Keep a job
    // busy until this handoff, even if the server finished between UI frames.
    for (auto& [id, job] : jobs_) {
        (void)id;
        if (job.idlePending) {
            job.busy = false;
            job.idlePending = false;
        }
    }
    return events;
}

void AsyncCodexClient::AddEvent(CodexEvent event) {
    std::scoped_lock lock(mutex_);
    if (stopping_ || forgottenJobs_.contains(event.jobId)) return;
    const auto found = jobs_.find(event.jobId);
    if (event.jobId != 0 && (found == jobs_.end() || found->second.imageAccepted)) return;
    if (found != jobs_.end() &&
        (event.type == CodexEventType::Error || event.type == CodexEventType::GeneratedImage)) {
        found->second.terminalEventSeen = true;
    }
    events_.push_back(std::move(event));
}

void AsyncCodexClient::Execute(const Command& command) {
    switch (command.type) {
    case CommandType::Start: {
        const bool started = bridge_.Start(command.path, command.executable);
        {
            std::scoped_lock lock(mutex_);
            sessionStarted_ = started;
        }
        PublishState(true);
        if (started && !bridge_.IsRunning()) {
            std::scoped_lock lock(mutex_);
            availabilityMessage_ = "Codex App Server disconnected during initialization.";
            if (!stopping_) events_.push_back({CodexEventType::Error, availabilityMessage_});
            break;
        }
        AddEvent({started && bridge_.IsAvailable() ? CodexEventType::Status : CodexEventType::Error,
                  bridge_.AvailabilityMessage()});
        break;
    }
    case CommandType::DiagnosticLog:
        if (!bridge_.EnableDiagnosticLog(command.path)) {
            AddEvent({CodexEventType::Error, "Could not open the Codex ImageGen diagnostic log."});
        }
        break;
    case CommandType::Generate: {
        {
            std::scoped_lock lock(mutex_);
            const auto found = jobs_.find(command.jobId);
            if (found == jobs_.end()) break;
            if (found->second.cancelRequested) {
                found->second.startPending = false;
                found->second.idlePending = true;
                found->second.terminalEventSeen = true;
                events_.push_back({CodexEventType::Error, "ImageGen was canceled.", {}, command.jobId});
                break;
            }
        }
        const bool started = bridge_.BeginGeneration(command.jobId, command.path, command.prompt,
                                                      command.model, command.effort);
        {
            std::scoped_lock lock(mutex_);
            if (const auto found = jobs_.find(command.jobId); found != jobs_.end()) {
                found->second.startPending = false;
            }
        }
        if (!started) {
            AddEvent({CodexEventType::Error, "Could not start ImageGen.", {}, command.jobId});
        }
        break;
    }
    case CommandType::Cancel:
        bridge_.Cancel(command.jobId);
        break;
    case CommandType::Finish:
        bridge_.FinishAfterGeneratedImage(command.jobId);
        CleanupTemporaryFiles(command.jobId);
        break;
    case CommandType::Forget:
        bridge_.Cancel(command.jobId);
        bridge_.Forget(command.jobId);
        CleanupTemporaryFiles(command.jobId);
        break;
    case CommandType::DeleteTemporaryFile: {
        const bool removed = RemoveTemporaryPath(command.path);
        AddEvent({removed ? CodexEventType::Status : CodexEventType::Error,
                  removed ? "Temporary file deleted." : "Could not safely delete the temporary file."});
        break;
    }
    case CommandType::ClearTemporaryFiles: {
        std::filesystem::path directory;
        {
            std::scoped_lock lock(mutex_);
            directory = sessionDirectory_;
        }
        std::error_code error;
        bool removed = true;
        for (std::filesystem::directory_iterator iterator(directory, error), end;
             !error && iterator != end; iterator.increment(error)) {
            if (!RemoveTemporaryPath(iterator->path())) removed = false;
        }
        removed = removed && !error;
        AddEvent({removed ? CodexEventType::TemporaryFilesCleared : CodexEventType::Error,
                  removed ? "Temporary files cleared." : "Could not safely clear all temporary files."});
        break;
    }
    }
}

bool AsyncCodexClient::RemoveTemporaryPath(const std::filesystem::path& path) {
    std::filesystem::path directory;
    {
        std::scoped_lock lock(mutex_);
        directory = sessionDirectory_;
    }
    std::error_code error;
    const auto root = std::filesystem::weakly_canonical(directory, error);
    if (error || root.empty()) return false;
    const auto resolved = std::filesystem::weakly_canonical(path, error);
    if (error) return false;
    const auto relative = resolved.lexically_relative(root);
    if (relative.empty() || relative == "." || relative.is_absolute() ||
        *relative.begin() == "..") return false;
    std::filesystem::remove_all(path, error);
    return !error;
}

void AsyncCodexClient::PublishState(const bool initializationFinished) {
    std::vector<std::pair<std::uint64_t, bool>> busy;
    {
        std::scoped_lock lock(mutex_);
        if (stopping_) return;
        for (const auto& [id, job] : jobs_) {
            if (!job.startPending && !job.imageAccepted) busy.emplace_back(id, false);
        }
    }
    for (auto& [id, value] : busy) value = bridge_.IsBusy(id);
    // Read events after busy state: a completion must not become visible to the
    // UI before the already-enqueued image that it might otherwise clean up.
    auto events = bridge_.PollEvents();
    const bool running = bridge_.IsRunning();
    const bool available = bridge_.IsAvailable();
    auto models = bridge_.Models();
    auto message = bridge_.AvailabilityMessage();
    std::scoped_lock lock(mutex_);
    if (stopping_) return;
    if (initializationFinished) starting_ = false;
    if (sessionStarted_ && !running) message = "Codex App Server disconnected.";
    running_ = running;
    if (!starting_) {
        available_ = available;
        availabilityMessage_ = std::move(message);
    }
    models_ = std::move(models);
    for (auto& event : events) {
        const auto found = jobs_.find(event.jobId);
        if (event.jobId == 0 || (found != jobs_.end() && !found->second.imageAccepted)) {
            if (found != jobs_.end() &&
                (event.type == CodexEventType::Error || event.type == CodexEventType::GeneratedImage)) {
                found->second.terminalEventSeen = true;
            }
            events_.push_back(std::move(event));
        }
    }
    for (const auto& [id, value] : busy) {
        const auto found = jobs_.find(id);
        if (found == jobs_.end() || found->second.startPending || found->second.imageAccepted) continue;
        auto& job = found->second;
        if (!value && job.busy && !job.idlePending) {
            if (!job.terminalEventSeen) {
                events_.push_back({CodexEventType::Error,
                    "ImageGen ended without a generated image.", {}, id});
                job.terminalEventSeen = true;
            }
            job.idlePending = true;
        }
    }
}

void AsyncCodexClient::CleanupTemporaryFiles(const std::uint64_t jobId) {
    std::filesystem::path directory;
    {
        std::scoped_lock lock(mutex_);
        const auto found = pendingCleanup_.find(jobId);
        if (found == pendingCleanup_.end()) return;
        directory = std::move(found->second);
        pendingCleanup_.erase(found);
    }
    std::error_code error;
    const auto parent = std::filesystem::weakly_canonical(directory.parent_path(), error);
    if (!error) {
        const auto resolved = std::filesystem::weakly_canonical(directory, error);
        if (!error && resolved.parent_path() == parent &&
            resolved.filename() == (L"projection-" + std::to_wstring(jobId))) {
            std::filesystem::remove_all(resolved, error);
            if (!error) return;
        }
    }
    AddEvent({CodexEventType::Error, "Could not safely clean the closed projection temporary files."});
}

void AsyncCodexClient::WorkerLoop() {
    for (;;) {
        std::optional<Command> command;
        {
            std::unique_lock lock(mutex_);
            wake_.wait_for(lock, std::chrono::milliseconds(20), [this] {
                return stopping_ || !commands_.empty();
            });
            if (stopping_) break;
            if (!commands_.empty()) {
                command = std::move(commands_.front());
                commands_.pop_front();
            }
        }
        try {
            if (command) Execute(*command);
            PublishState();
        } catch (const std::exception& exception) {
            {
                std::scoped_lock lock(mutex_);
                if (stopping_) continue;
                if (command && command->type == CommandType::Start) starting_ = false;
                if (command) {
                    if (const auto found = jobs_.find(command->jobId); found != jobs_.end()) {
                        found->second.startPending = false;
                        found->second.idlePending = true;
                        found->second.terminalEventSeen = true;
                    }
                }
                if (!command || !forgottenJobs_.contains(command->jobId)) {
                    events_.push_back({CodexEventType::Error,
                        std::string("Codex operation failed: ") + exception.what(),
                        {}, command ? command->jobId : 0});
                }
            }
        }
    }
    bridge_.Stop();
    // Shutdown may overtake queued Forget commands. The bridge is now stopped,
    // so their cleanup is safe; entries explicitly retained for recovery are absent.
    std::vector<std::uint64_t> cleanupJobs;
    {
        std::scoped_lock lock(mutex_);
        for (const auto& [id, directory] : pendingCleanup_) {
            (void)directory;
            cleanupJobs.push_back(id);
        }
    }
    for (const auto id : cleanupJobs) CleanupTemporaryFiles(id);
    {
        std::scoped_lock lock(mutex_);
        workerExited_ = true;
    }
    wake_.notify_all();
}

void AsyncCodexClient::Stop() {
    std::scoped_lock stopLock(stopMutex_);
    {
        std::scoped_lock lock(mutex_);
        stopping_ = true;
        starting_ = running_ = available_ = false;
        commands_.clear();
        for (auto& [id, job] : jobs_) {
            (void)id;
            job.busy = false;
        }
    }
    wake_.notify_all();
    // Repeat cancellation until the owner exits, covering an I/O operation
    // that starts just after the first cancellation request was observed.
    for (;;) {
        bridge_.AbortPendingRequests();
        std::unique_lock lock(mutex_);
        if (workerExited_) break;
        wake_.wait_for(lock, std::chrono::milliseconds(20), [this] { return workerExited_; });
        if (workerExited_) break;
    }
    if (worker_.joinable()) worker_.join();
}

} // namespace codextex
