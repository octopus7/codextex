#include "codex/CodexBridge.hpp"

#include "core/PromptBuilder.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <system_error>
#include <vector>

namespace codextex {
namespace {

std::string PathUtf8(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

std::filesystem::path PathFromUtf8(const std::string& value) {
    return std::filesystem::path(std::u8string(
        reinterpret_cast<const char8_t*>(value.data()), value.size()));
}

std::wstring Quote(const std::wstring& value) {
    return L"\"" + value + L"\"";
}

struct LaunchCandidate {
    std::filesystem::path command;
    std::filesystem::path application;
    std::wstring commandLine;
};

std::optional<std::filesystem::path> SearchCommand(const wchar_t* extension) {
    std::array<wchar_t, 32768> buffer{};
    const DWORD size = SearchPathW(nullptr, L"codex", extension,
                                   static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
    if (size > 0 && size < buffer.size()) {
        return std::filesystem::path(buffer.data());
    }
    return std::nullopt;
}

std::optional<std::filesystem::path> EnvironmentPath(const wchar_t* name) {
    const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
    if (size == 0) return std::nullopt;
    std::wstring value(size, L'\0');
    if (GetEnvironmentVariableW(name, value.data(), size) == 0) return std::nullopt;
    value.resize(size - 1);
    return std::filesystem::path(value);
}

std::filesystem::path SystemCommandInterpreter() {
    std::array<wchar_t, MAX_PATH> directory{};
    const UINT size = GetSystemDirectoryW(directory.data(), static_cast<UINT>(directory.size()));
    if (size == 0 || size >= directory.size()) return L"cmd.exe";
    return std::filesystem::path(directory.data()) / L"cmd.exe";
}

void AddLaunchCandidate(std::vector<LaunchCandidate>& candidates,
                        const std::filesystem::path& command) {
    std::error_code error;
    if (command.empty() || !std::filesystem::is_regular_file(command, error)) return;
    for (const auto& candidate : candidates) {
        if (std::filesystem::equivalent(candidate.command, command, error) && !error) return;
        error.clear();
    }

    std::wstring extension = command.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](const wchar_t value) { return static_cast<wchar_t>(std::towlower(value)); });
    LaunchCandidate candidate;
    candidate.command = command;
    if (extension == L".cmd" || extension == L".bat") {
        candidate.application = SystemCommandInterpreter();
        candidate.commandLine = Quote(candidate.application.wstring()) + L" /d /s /c \"\"" +
            command.wstring() + L"\" app-server --stdio\"";
    } else {
        candidate.application = command;
        candidate.commandLine = Quote(command.wstring()) + L" app-server --stdio";
    }
    candidates.push_back(std::move(candidate));
}

std::vector<LaunchCandidate> FindCodexCommands(
    const std::filesystem::path& executableOverride) {
    std::vector<LaunchCandidate> candidates;
    if (!executableOverride.empty()) {
        AddLaunchCandidate(candidates, executableOverride);
        return candidates;
    }

    if (const auto executable = SearchCommand(L".exe")) AddLaunchCandidate(candidates, *executable);
    if (const auto script = SearchCommand(L".cmd")) AddLaunchCandidate(candidates, *script);
    if (const auto script = SearchCommand(L".bat")) AddLaunchCandidate(candidates, *script);

    if (const auto localAppData = EnvironmentPath(L"LOCALAPPDATA")) {
        const auto binDirectory = *localAppData / L"OpenAI" / L"Codex" / L"bin";
        AddLaunchCandidate(candidates, binDirectory / L"codex.exe");
        std::error_code error;
        std::vector<std::filesystem::path> desktopExecutables;
        for (std::filesystem::directory_iterator iterator(
                 binDirectory, std::filesystem::directory_options::skip_permission_denied, error), end;
             !error && iterator != end; iterator.increment(error)) {
            if (!iterator->is_directory(error)) continue;
            const auto executable = iterator->path() / L"codex.exe";
            if (std::filesystem::is_regular_file(executable, error)) {
                desktopExecutables.push_back(executable);
            }
            error.clear();
        }
        std::ranges::sort(desktopExecutables, std::greater{}, [](const auto& path) {
            std::error_code timeError;
            return std::filesystem::last_write_time(path, timeError);
        });
        for (const auto& executable : desktopExecutables) AddLaunchCandidate(candidates, executable);
    }
    if (const auto appData = EnvironmentPath(L"APPDATA")) {
        AddLaunchCandidate(candidates, *appData / L"npm" / L"codex.cmd");
    }
    return candidates;
}

std::string LocalTimestamp() {
    SYSTEMTIME time{};
    GetLocalTime(&time);
    std::array<char, 32> buffer{};
    std::snprintf(buffer.data(), buffer.size(), "%04u-%02u-%02u %02u:%02u:%02u.%03u",
                  time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute,
                  time.wSecond, time.wMilliseconds);
    return buffer.data();
}

std::vector<CodexModelInfo> FallbackModels() {
    return {{"gpt-5.6-sol", "GPT-5.6-Sol", "medium",
             {{"low", "Fast reasoning"},
              {"medium", "Balanced reasoning"},
              {"high", "Deeper reasoning"},
              {"xhigh", "Very deep reasoning"},
              {"max", "Maximum reasoning"},
              {"ultra", "Ultra reasoning"}}}};
}

} // namespace

CodexBridge::~CodexBridge() {
    Stop();
}

bool CodexBridge::EnableDiagnosticLog(const std::filesystem::path& logPath) {
    std::scoped_lock lock(logMutex_);
    diagnosticLogPath_ = logPath;
    HANDLE file = CreateFileW(diagnosticLogPath_.c_str(), FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        diagnosticLogPath_.clear();
        return false;
    }
    const std::string header = "\r\n[" + LocalTimestamp() +
        "] ===== CodexTex ImageGen session started =====\r\n";
    DWORD written = 0;
    const bool success = WriteFile(file, header.data(), static_cast<DWORD>(header.size()),
                                   &written, nullptr) && written == header.size();
    CloseHandle(file);
    if (!success) diagnosticLogPath_.clear();
    return success;
}

bool CodexBridge::Start(const std::filesystem::path& sessionDirectory,
                        const std::filesystem::path& executableOverride) {
    Stop();
    models_ = FallbackModels();
    sessionDirectory_ = sessionDirectory;
    executableOverride_ = executableOverride;
    LogDiagnostic("Starting Codex bridge. Session directory: " + PathUtf8(sessionDirectory_));
    std::error_code error;
    std::filesystem::create_directories(sessionDirectory_, error);
    if (error) {
        availabilityMessage_ = "Could not create the CodexTex session directory.";
        return false;
    }
    if (!LaunchProcess()) {
        return false;
    }
    reader_ = std::thread(&CodexBridge::ReadLoop, this);
    if (!InitializeProtocol()) {
        Stop();
        return false;
    }
    return true;
}

void CodexBridge::Stop() {
    if (running_ || process_ != nullptr) LogDiagnostic("Stopping Codex App Server.");
    available_ = false;
    running_ = false;
    if (childStdIn_ != nullptr) {
        CloseHandle(childStdIn_);
        childStdIn_ = nullptr;
    }
    if (process_ != nullptr) {
        WaitForSingleObject(process_, 500);
    }
    if (job_ != nullptr) {
        CloseHandle(job_);
        job_ = nullptr;
    } else if (process_ != nullptr) {
        TerminateProcess(process_, 0);
    }
    if (childStdOut_ != nullptr) {
        CloseHandle(childStdOut_);
        childStdOut_ = nullptr;
    }
    if (reader_.joinable()) {
        reader_.join();
    }
    if (processThread_ != nullptr) {
        CloseHandle(processThread_);
        processThread_ = nullptr;
    }
    if (process_ != nullptr) {
        CloseHandle(process_);
        process_ = nullptr;
    }

    std::scoped_lock lock(pendingMutex_);
    for (auto& [id, promise] : pending_) {
        (void)id;
        try {
            promise->set_exception(std::make_exception_ptr(std::runtime_error("Codex stopped.")));
        } catch (...) {
        }
    }
    pending_.clear();
    {
        std::scoped_lock stateLock(stateMutex_);
        jobs_.clear();
        jobsByThread_.clear();
    }
}

bool CodexBridge::LaunchProcess() {
    const auto candidates = FindCodexCommands(executableOverride_);
    if (candidates.empty()) {
        availabilityMessage_ = "No runnable Codex CLI was found on PATH, in the Codex desktop install, or in the npm user bin. AI features are disabled.";
        return false;
    }

    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE stdoutRead = nullptr;
    HANDLE stdoutWrite = nullptr;
    HANDLE stdinRead = nullptr;
    HANDLE stdinWrite = nullptr;
    if (!CreatePipe(&stdoutRead, &stdoutWrite, &security, 0) ||
        !SetHandleInformation(stdoutRead, HANDLE_FLAG_INHERIT, 0) ||
        !CreatePipe(&stdinRead, &stdinWrite, &security, 0) ||
        !SetHandleInformation(stdinWrite, HANDLE_FLAG_INHERIT, 0)) {
        availabilityMessage_ = "Could not create pipes for Codex App Server.";
        if (stdoutRead) CloseHandle(stdoutRead);
        if (stdoutWrite) CloseHandle(stdoutWrite);
        if (stdinRead) CloseHandle(stdinRead);
        if (stdinWrite) CloseHandle(stdinWrite);
        return false;
    }

    HANDLE errorHandle = INVALID_HANDLE_VALUE;
    if (!diagnosticLogPath_.empty()) {
        errorHandle = CreateFileW(diagnosticLogPath_.c_str(), FILE_APPEND_DATA,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  &security, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (errorHandle != INVALID_HANDLE_VALUE) {
            LogDiagnostic("Codex App Server stderr follows as raw timestamped records.");
        }
    }
    if (errorHandle == INVALID_HANDLE_VALUE) {
        errorHandle = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = stdinRead;
    startup.hStdOutput = stdoutWrite;
    startup.hStdError = errorHandle;
    PROCESS_INFORMATION processInfo{};
    DWORD launchError = ERROR_FILE_NOT_FOUND;
    const LaunchCandidate* launched = nullptr;
    for (const auto& candidate : candidates) {
        LogDiagnostic("Trying App Server command: " + PathUtf8(candidate.command));
        std::wstring commandLine = candidate.commandLine;
        if (CreateProcessW(candidate.application.c_str(), commandLine.data(), nullptr, nullptr, TRUE,
                           CREATE_NO_WINDOW, nullptr, sessionDirectory_.c_str(), &startup,
                           &processInfo)) {
            launched = &candidate;
            break;
        }
        launchError = GetLastError();
    }
    CloseHandle(stdinRead);
    CloseHandle(stdoutWrite);
    if (errorHandle != INVALID_HANDLE_VALUE) CloseHandle(errorHandle);
    if (!launched) {
        CloseHandle(stdoutRead);
        CloseHandle(stdinWrite);
        availabilityMessage_ = "Codex CLI candidates were found, but none could start App Server (Win32 error " +
            std::to_string(launchError) + ").";
        return false;
    }

    launchedCommand_ = launched->command;
    LogDiagnostic("App Server launched via: " + PathUtf8(launchedCommand_));

    process_ = processInfo.hProcess;
    processThread_ = processInfo.hThread;
    childStdIn_ = stdinWrite;
    childStdOut_ = stdoutRead;
    job_ = CreateJobObjectW(nullptr, nullptr);
    if (job_) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
        AssignProcessToJobObject(job_, process_);
    }
    running_ = true;
    return true;
}

bool CodexBridge::InitializeProtocol() {
    try {
        const auto initialized = SendRequest(
            "initialize",
            {{"clientInfo", {{"name", "codextex"}, {"title", "CodexTex"}, {"version", "0.1.0"}}},
             {"capabilities", {{"experimentalApi", true}}}});
        (void)initialized;
        if (!SendLine({{"method", "initialized"}, {"params", nlohmann::json::object()}})) {
            throw std::runtime_error("Could not acknowledge App Server initialization.");
        }

        const auto accountResult = SendRequest("account/read", nlohmann::json::object());
        if (!accountResult.contains("account") || accountResult["account"].is_null()) {
            availabilityMessage_ = "Codex is not signed in. AI features are disabled; external PNG projection remains available.";
            return true;
        }

        try {
            const auto modelsResult = SendRequest(
                "model/list", {{"limit", 100}, {"includeHidden", false}});
            std::vector<CodexModelInfo> advertisedModels;
            if (modelsResult.contains("data") && modelsResult["data"].is_array()) {
                for (const auto& model : modelsResult["data"]) {
                    CodexModelInfo info;
                    info.id = model.value("model", model.value("id", ""));
                    info.displayName = model.value("displayName", info.id);
                    info.defaultReasoningEffort = model.value("defaultReasoningEffort", "medium");
                    if (model.contains("supportedReasoningEfforts") &&
                        model["supportedReasoningEfforts"].is_array()) {
                        for (const auto& option : model["supportedReasoningEfforts"]) {
                            const std::string value = option.value("reasoningEffort", "");
                            if (!value.empty()) {
                                info.supportedReasoningEfforts.push_back(
                                    {value, option.value("description", "")});
                            }
                        }
                    }
                    if (!info.id.empty() && !info.supportedReasoningEfforts.empty()) {
                        advertisedModels.push_back(std::move(info));
                    }
                }
            }
            if (!advertisedModels.empty()) models_ = std::move(advertisedModels);
        } catch (const std::exception& exception) {
            LogDiagnostic(std::string("model/list unavailable; using built-in fallback: ") +
                          exception.what());
        }
        const auto skillsResult = SendRequest(
            "skills/list", {{"cwds", {PathUtf8(sessionDirectory_)}}, {"forceReload", true}});
        if (skillsResult.contains("data")) {
            for (const auto& scope : skillsResult["data"]) {
                if (!scope.contains("skills")) continue;
                for (const auto& skill : scope["skills"]) {
                    if (skill.value("name", "") == "imagegen" && skill.value("enabled", false) &&
                        skill.contains("path")) {
                        imagegenSkillPath_ = PathFromUtf8(skill["path"].get<std::string>());
                        break;
                    }
                }
            }
        }
        if (imagegenSkillPath_.empty()) {
            availabilityMessage_ = "The built-in imagegen skill is unavailable or disabled. External PNG projection remains available.";
            return true;
        }
        available_ = true;
        availabilityMessage_ = "Codex ImageGen is ready via " +
            PathUtf8(launchedCommand_.filename()) + ".";
        PushEvent({CodexEventType::Status, availabilityMessage_});
        return true;
    } catch (const std::exception& exception) {
        availabilityMessage_ = std::string("Codex App Server is unavailable: ") + exception.what();
        return false;
    }
}

bool CodexBridge::IsBusy() const noexcept {
    std::scoped_lock lock(stateMutex_);
    return std::ranges::any_of(jobs_, [](const auto& entry) { return entry.second.busy; });
}

bool CodexBridge::IsBusy(const std::uint64_t jobId) const noexcept {
    std::scoped_lock lock(stateMutex_);
    const auto found = jobs_.find(jobId);
    return found != jobs_.end() && found->second.busy;
}

bool CodexBridge::EnsureThread(const std::uint64_t jobId, const std::string& model) {
    {
        std::scoped_lock lock(stateMutex_);
        const auto found = jobs_.find(jobId);
        if (found != jobs_.end() && !found->second.threadId.empty()) return true;
    }
    try {
        const auto startThread = [this, &model](const char* sandbox) {
            return SendRequest(
                "thread/start",
                {{"cwd", PathUtf8(sessionDirectory_)},
                 {"model", model},
                 {"approvalPolicy", "never"},
                 {"sandbox", sandbox},
                 {"ephemeral", true},
                 {"serviceName", "codextex"}});
        };

        nlohmann::json result;
        try {
            // The Codex 0.152.0 schema serializes SandboxMode using kebab-case.
            result = startThread("workspace-write");
        } catch (const std::exception& exception) {
            const std::string error = exception.what();
            if (error.find("unknown variant") == std::string::npos) {
                throw;
            }
            // Older App Server releases and the current online example use camelCase.
            result = startThread("workspaceWrite");
        }
        const auto threadId = result.at("thread").at("id").get<std::string>();
        {
            std::scoped_lock lock(stateMutex_);
            auto& job = jobs_[jobId];
            job.threadId = threadId;
            jobsByThread_[threadId] = jobId;
        }
        return true;
    } catch (const std::exception& exception) {
        CodexEvent event{CodexEventType::Error,
                         std::string("Could not start a Codex session: ") + exception.what()};
        event.jobId = jobId;
        PushEvent(std::move(event));
        return false;
    }
}

bool CodexBridge::BeginGeneration(const std::uint64_t jobId,
                                  const std::filesystem::path& capturePath,
                                  const std::string& userPrompt,
                                  const std::string& model,
                                  const std::string& reasoningEffort) {
    if (!available_ || userPrompt.empty() || !std::filesystem::exists(capturePath) ||
        model.empty() || reasoningEffort.empty() || IsBusy(jobId)) {
        return false;
    }
    if (!EnsureThread(jobId, model)) {
        return false;
    }
    std::string threadId;
    {
        std::scoped_lock lock(stateMutex_);
        auto& job = jobs_[jobId];
        job.busy = true;
        job.generatedImageAccepted = false;
        job.model = model;
        job.reasoningEffort = reasoningEffort;
        threadId = job.threadId;
    }
    try {
        const auto result = SendRequest(
            "turn/start",
            {{"threadId", threadId},
             {"model", model},
             {"effort", reasoningEffort},
             {"input",
              {{{"type", "text"}, {"text", PromptBuilder::GenerationPrompt(userPrompt)}},
               {{"type", "localImage"}, {"path", PathUtf8(capturePath)}},
               {{"type", "skill"}, {"name", "imagegen"}, {"path", PathUtf8(imagegenSkillPath_)}}}}});
        {
            std::scoped_lock lock(stateMutex_);
            auto& job = jobs_[jobId];
            if (job.busy) job.activeTurnId = result.at("turn").at("id").get<std::string>();
        }
        CodexEvent event{CodexEventType::Progress, "ImageGen started."};
        event.jobId = jobId;
        PushEvent(std::move(event));
        return true;
    } catch (const std::exception& exception) {
        {
            std::scoped_lock lock(stateMutex_);
            auto& job = jobs_[jobId];
            job.busy = false;
            job.activeTurnId.clear();
        }
        CodexEvent event{CodexEventType::Error,
                         std::string("Could not start ImageGen: ") + exception.what()};
        event.jobId = jobId;
        PushEvent(std::move(event));
        return false;
    }
}

void CodexBridge::FinishAfterGeneratedImage(const std::uint64_t jobId) {
    std::string threadId;
    std::string activeTurn;
    {
        std::scoped_lock lock(stateMutex_);
        const auto found = jobs_.find(jobId);
        if (found == jobs_.end()) return;
        auto& job = found->second;
        job.generatedImageAccepted = true;
        if (!job.busy) return;
        threadId = job.threadId;
        activeTurn = job.activeTurnId;
        job.busy = false;
        job.activeTurnId.clear();
    }
    if (threadId.empty() || activeTurn.empty()) return;

    // The generated PNG has already been copied and permanently archived by the
    // application. Do not keep the UI waiting for post-generation assistant text.
    // This request deliberately has no pending promise so the UI thread never blocks.
    const std::uint64_t id = nextRequestId_++;
    if (!SendLine({{"method", "turn/interrupt"}, {"id", id},
                   {"params", {{"threadId", threadId}, {"turnId", activeTurn}}}})) {
        LogDiagnostic("Could not interrupt completed ImageGen turn for job " +
                      std::to_string(jobId) + ".");
    }
}

void CodexBridge::Cancel(const std::uint64_t jobId) {
    std::string threadId;
    std::string activeTurn;
    {
        std::scoped_lock lock(stateMutex_);
        const auto found = jobs_.find(jobId);
        if (found == jobs_.end() || !found->second.busy) return;
        threadId = found->second.threadId;
        activeTurn = found->second.activeTurnId;
    }
    if (threadId.empty() || activeTurn.empty()) {
        return;
    }
    try {
        (void)SendRequest("turn/interrupt", {{"threadId", threadId}, {"turnId", activeTurn}},
                          std::chrono::seconds(5));
    } catch (...) {
    }
}

void CodexBridge::Forget(const std::uint64_t jobId) {
    std::scoped_lock lock(stateMutex_);
    const auto found = jobs_.find(jobId);
    if (found == jobs_.end()) return;
    if (!found->second.threadId.empty()) jobsByThread_.erase(found->second.threadId);
    jobs_.erase(found);
}

std::vector<CodexEvent> CodexBridge::PollEvents() {
    std::scoped_lock lock(eventMutex_);
    std::vector<CodexEvent> output(events_.begin(), events_.end());
    events_.clear();
    return output;
}

std::optional<std::uint64_t> CodexBridge::FindJob(const nlohmann::json& params) const {
    std::scoped_lock lock(stateMutex_);
    const std::string threadId = params.value("threadId", "");
    if (!threadId.empty()) {
        const auto found = jobsByThread_.find(threadId);
        if (found != jobsByThread_.end()) return found->second;
    }
    std::optional<std::uint64_t> onlyBusy;
    for (const auto& [jobId, job] : jobs_) {
        if (!job.busy) continue;
        if (onlyBusy) return std::nullopt;
        onlyBusy = jobId;
    }
    return onlyBusy;
}

nlohmann::json CodexBridge::SendRequest(const std::string& method, nlohmann::json params,
                                        const std::chrono::milliseconds timeout) {
    if (!running_) {
        throw std::runtime_error("Codex process is not running.");
    }
    const std::uint64_t id = nextRequestId_++;
    auto promise = std::make_shared<std::promise<nlohmann::json>>();
    auto future = promise->get_future();
    {
        std::scoped_lock lock(pendingMutex_);
        pending_.emplace(id, promise);
    }
    if (!SendLine({{"method", method}, {"id", id}, {"params", std::move(params)}})) {
        std::scoped_lock lock(pendingMutex_);
        pending_.erase(id);
        throw std::runtime_error("Could not write to Codex App Server.");
    }
    if (future.wait_for(timeout) != std::future_status::ready) {
        std::scoped_lock lock(pendingMutex_);
        pending_.erase(id);
        throw std::runtime_error(method + " timed out.");
    }
    return future.get();
}

bool CodexBridge::SendLine(const nlohmann::json& message) {
    const std::string json = message.dump();
    LogDiagnostic("CLIENT -> " + json);
    const std::string line = json + "\n";
    std::scoped_lock lock(writeMutex_);
    DWORD written = 0;
    return childStdIn_ != nullptr &&
        WriteFile(childStdIn_, line.data(), static_cast<DWORD>(line.size()), &written, nullptr) &&
        written == line.size();
}

void CodexBridge::ReadLoop() {
    std::array<char, 8192> buffer{};
    std::string pendingText;
    while (running_) {
        DWORD read = 0;
        if (!ReadFile(childStdOut_, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) ||
            read == 0) {
            break;
        }
        pendingText.append(buffer.data(), read);
        std::size_t newline = 0;
        while ((newline = pendingText.find('\n')) != std::string::npos) {
            std::string line = pendingText.substr(0, newline);
            pendingText.erase(0, newline + 1);
            if (line.empty()) continue;
            LogDiagnostic("SERVER <- " + line);
            try {
                HandleMessage(nlohmann::json::parse(line));
            } catch (const std::exception& exception) {
                PushEvent({CodexEventType::Error, std::string("Invalid App Server event: ") + exception.what()});
            }
        }
    }
    running_ = false;
    available_ = false;
    std::vector<std::uint64_t> interruptedJobs;
    {
        std::scoped_lock lock(stateMutex_);
        for (auto& [jobId, job] : jobs_) {
            if (job.busy) interruptedJobs.push_back(jobId);
            job.busy = false;
            job.activeTurnId.clear();
        }
    }
    for (const auto jobId : interruptedJobs) {
        CodexEvent event{CodexEventType::Error,
                         "Codex App Server stopped during the operation."};
        event.jobId = jobId;
        PushEvent(std::move(event));
    }
}

void CodexBridge::HandleMessage(const nlohmann::json& message) {
    if (message.contains("id") && !message["id"].is_null()) {
        const std::uint64_t id = message["id"].get<std::uint64_t>();
        std::shared_ptr<std::promise<nlohmann::json>> promise;
        {
            std::scoped_lock lock(pendingMutex_);
            const auto found = pending_.find(id);
            if (found != pending_.end()) {
                promise = found->second;
                pending_.erase(found);
            }
        }
        if (promise) {
            if (message.contains("error")) {
                promise->set_exception(std::make_exception_ptr(
                    std::runtime_error(message["error"].dump())));
            } else {
                promise->set_value(message.value("result", nlohmann::json::object()));
            }
        }
        return;
    }
    const std::string method = message.value("method", "");
    const nlohmann::json& params = message.value("params", nlohmann::json::object());
    const auto jobId = FindJob(params);
    if (!jobId) return;
    bool knownJob = false;
    {
        std::scoped_lock lock(stateMutex_);
        const auto found = jobs_.find(*jobId);
        if (found != jobs_.end()) {
            knownJob = true;
        }
    }
    if (!knownJob) return;
    const auto pushForJob = [this, jobId](CodexEvent event) {
        event.jobId = *jobId;
        PushEvent(std::move(event));
    };
    if ((method == "item/started" || method == "item/completed") && params.contains("item")) {
        const auto& item = params["item"];
        const std::string type = item.value("type", "");
        if (type == "imageGeneration") {
            pushForJob({CodexEventType::Progress,
                        "ImageGen: " + item.value("status", "working")});
            if (method == "item/completed" && item.contains("savedPath") &&
                !item["savedPath"].is_null()) {
                const auto copied = CopyGeneratedImage(*jobId,
                    PathFromUtf8(item["savedPath"].get<std::string>()));
                if (!copied.empty()) {
                    pushForJob({CodexEventType::GeneratedImage, "ImageGen completed.", copied});
                }
            }
        }
    } else if (method == "turn/completed") {
        bool generatedImageAccepted = false;
        {
            std::scoped_lock lock(stateMutex_);
            auto found = jobs_.find(*jobId);
            if (found != jobs_.end()) {
                generatedImageAccepted = found->second.generatedImageAccepted;
                found->second.busy = false;
                found->second.activeTurnId.clear();
            }
        }
        const auto& turn = params.value("turn", nlohmann::json::object());
        const std::string status = turn.value("status", "completed");
        if (status != "completed" && !generatedImageAccepted) {
            pushForJob({CodexEventType::Error, "Codex turn ended with status: " + status});
        }
    }
}

void CodexBridge::PushEvent(CodexEvent event) {
    std::scoped_lock lock(eventMutex_);
    events_.push_back(std::move(event));
}

void CodexBridge::LogDiagnostic(const std::string_view message) {
    std::scoped_lock lock(logMutex_);
    if (diagnosticLogPath_.empty()) return;
    HANDLE file = CreateFileW(diagnosticLogPath_.c_str(), FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    const std::string entry = "[" + LocalTimestamp() + "] " + std::string(message) + "\r\n";
    DWORD written = 0;
    WriteFile(file, entry.data(), static_cast<DWORD>(entry.size()), &written, nullptr);
    CloseHandle(file);
}

std::filesystem::path CodexBridge::CopyGeneratedImage(const std::uint64_t jobId,
                                                      const std::filesystem::path& source) {
    if (!std::filesystem::exists(source)) {
        CodexEvent event{CodexEventType::Error,
                         "ImageGen reported a path that does not exist."};
        event.jobId = jobId;
        PushEvent(std::move(event));
        return {};
    }
    const auto jobDirectory = sessionDirectory_ /
        (L"projection-" + std::to_wstring(jobId));
    std::error_code error;
    std::filesystem::create_directories(jobDirectory, error);
    if (error) {
        CodexEvent event{CodexEventType::Error,
                         "Could not create the temporary directory for this projection."};
        event.jobId = jobId;
        PushEvent(std::move(event));
        return {};
    }
    const auto destination = jobDirectory /
        (L"imagegen-" + std::to_wstring(++generatedIndex_) + L".png");
    std::filesystem::copy_file(source, destination, std::filesystem::copy_options::none, error);
    if (error) {
        CodexEvent event{CodexEventType::Error,
                         "Could not copy the generated image into the session directory."};
        event.jobId = jobId;
        PushEvent(std::move(event));
        return {};
    }
    LogDiagnostic("Copied generated image to: " + PathUtf8(destination));
    return destination;
}

} // namespace codextex
