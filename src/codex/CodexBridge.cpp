#include "codex/CodexBridge.hpp"

#include "core/PromptBuilder.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <system_error>

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

std::optional<std::filesystem::path> FindCodexExecutable() {
    std::array<wchar_t, 32768> buffer{};
    const DWORD size = SearchPathW(nullptr, L"codex.exe", nullptr,
                                   static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
    if (size > 0 && size < buffer.size()) {
        return std::filesystem::path(buffer.data());
    }
    return std::nullopt;
}

std::string StripCodeFence(std::string text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    text.erase(0, first);
    if (text.rfind("```", 0) == 0) {
        const auto newline = text.find('\n');
        const auto end = text.rfind("```");
        if (newline != std::string::npos && end != std::string::npos && end > newline) {
            text = text.substr(newline + 1, end - newline - 1);
        }
    }
    return text;
}

} // namespace

CodexBridge::~CodexBridge() {
    Stop();
}

bool CodexBridge::Start(const std::filesystem::path& sessionDirectory,
                        const std::filesystem::path& executableOverride) {
    Stop();
    sessionDirectory_ = sessionDirectory;
    executableOverride_ = executableOverride;
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
    available_ = false;
    busy_ = false;
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
    threadId_.clear();
    activeTurnId_.clear();
    operation_ = Operation::None;
}

bool CodexBridge::LaunchProcess() {
    const auto executable = executableOverride_.empty()
        ? FindCodexExecutable()
        : std::optional<std::filesystem::path>(executableOverride_);
    if (!executable) {
        availabilityMessage_ = "codex.exe was not found on PATH. AI features are disabled.";
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

    HANDLE nullHandle = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = stdinRead;
    startup.hStdOutput = stdoutWrite;
    startup.hStdError = nullHandle;
    PROCESS_INFORMATION processInfo{};
    std::wstring commandLine = Quote(executable->wstring()) + L" app-server --stdio";
    const BOOL created = CreateProcessW(executable->c_str(), commandLine.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW, nullptr, sessionDirectory_.c_str(), &startup,
                                        &processInfo);
    CloseHandle(stdinRead);
    CloseHandle(stdoutWrite);
    if (nullHandle != INVALID_HANDLE_VALUE) CloseHandle(nullHandle);
    if (!created) {
        CloseHandle(stdoutRead);
        CloseHandle(stdinWrite);
        availabilityMessage_ = "Could not start Codex App Server (Win32 error " +
            std::to_string(GetLastError()) + ").";
        return false;
    }

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
        availabilityMessage_ = "Codex ImageGen is ready.";
        PushEvent({CodexEventType::Status, availabilityMessage_});
        return true;
    } catch (const std::exception& exception) {
        availabilityMessage_ = std::string("Codex App Server is unavailable: ") + exception.what();
        return false;
    }
}

bool CodexBridge::EnsureThread() {
    if (!threadId_.empty()) {
        return true;
    }
    try {
        const auto result = SendRequest(
            "thread/start",
            {{"cwd", PathUtf8(sessionDirectory_)},
             {"approvalPolicy", "never"},
             {"sandbox", "workspaceWrite"},
             {"ephemeral", true},
             {"serviceName", "codextex"}});
        threadId_ = result.at("thread").at("id").get<std::string>();
        return true;
    } catch (const std::exception& exception) {
        PushEvent({CodexEventType::Error, std::string("Could not start a Codex session: ") + exception.what()});
        return false;
    }
}

bool CodexBridge::BeginGeneration(const std::filesystem::path& capturePath,
                                  const std::string& userPrompt) {
    if (!available_ || busy_ || userPrompt.empty() || !std::filesystem::exists(capturePath)) {
        return false;
    }
    if (!EnsureThread()) {
        return false;
    }
    busy_ = true;
    operation_ = Operation::Generation;
    try {
        const auto result = SendRequest(
            "turn/start",
            {{"threadId", threadId_},
             {"input",
              {{{"type", "text"}, {"text", PromptBuilder::GenerationPrompt(userPrompt)}},
               {{"type", "localImage"}, {"path", PathUtf8(capturePath)}},
               {{"type", "skill"}, {"name", "imagegen"}, {"path", PathUtf8(imagegenSkillPath_)}}}}});
        {
            std::scoped_lock lock(stateMutex_);
            activeTurnId_ = result.at("turn").at("id").get<std::string>();
        }
        PushEvent({CodexEventType::Progress, "ImageGen started."});
        return true;
    } catch (const std::exception& exception) {
        busy_ = false;
        operation_ = Operation::None;
        PushEvent({CodexEventType::Error, std::string("Could not start ImageGen: ") + exception.what()});
        return false;
    }
}

bool CodexBridge::BeginMaskProposal(const std::filesystem::path& capturePath,
                                    const std::filesystem::path& generatedPath) {
    if (!available_ || busy_ || !std::filesystem::exists(capturePath) ||
        !std::filesystem::exists(generatedPath)) {
        return false;
    }
    if (!EnsureThread()) {
        return false;
    }
    busy_ = true;
    operation_ = Operation::Mask;
    try {
        const auto result = SendRequest(
            "turn/start",
            {{"threadId", threadId_},
             {"input",
              {{{"type", "text"}, {"text", PromptBuilder::MaskPrompt()}},
               {{"type", "localImage"}, {"path", PathUtf8(capturePath)}},
               {{"type", "localImage"}, {"path", PathUtf8(generatedPath)}}}},
             {"outputSchema", PromptBuilder::MaskOutputSchema()}});
        {
            std::scoped_lock lock(stateMutex_);
            activeTurnId_ = result.at("turn").at("id").get<std::string>();
        }
        PushEvent({CodexEventType::Progress, "Codex mask proposal started."});
        return true;
    } catch (const std::exception& exception) {
        busy_ = false;
        operation_ = Operation::None;
        PushEvent({CodexEventType::Error, std::string("Could not start mask proposal: ") + exception.what()});
        return false;
    }
}

void CodexBridge::Cancel() {
    std::string activeTurn;
    {
        std::scoped_lock lock(stateMutex_);
        activeTurn = activeTurnId_;
    }
    if (!busy_ || threadId_.empty() || activeTurn.empty()) {
        return;
    }
    try {
        (void)SendRequest("turn/interrupt", {{"threadId", threadId_}, {"turnId", activeTurn}},
                          std::chrono::seconds(5));
    } catch (...) {
    }
}

std::vector<CodexEvent> CodexBridge::PollEvents() {
    std::scoped_lock lock(eventMutex_);
    std::vector<CodexEvent> output(events_.begin(), events_.end());
    events_.clear();
    return output;
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
    const std::string line = message.dump() + "\n";
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
            try {
                HandleMessage(nlohmann::json::parse(line));
            } catch (const std::exception& exception) {
                PushEvent({CodexEventType::Error, std::string("Invalid App Server event: ") + exception.what()});
            }
        }
    }
    running_ = false;
    available_ = false;
    if (busy_) {
        busy_ = false;
        PushEvent({CodexEventType::Error, "Codex App Server stopped during the operation."});
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
    if ((method == "item/started" || method == "item/completed") && params.contains("item")) {
        const auto& item = params["item"];
        const std::string type = item.value("type", "");
        if (type == "imageGeneration") {
            PushEvent({CodexEventType::Progress, "ImageGen: " + item.value("status", "working")});
            if (method == "item/completed" && item.contains("savedPath") &&
                !item["savedPath"].is_null()) {
                const auto copied = CopyGeneratedImage(
                    PathFromUtf8(item["savedPath"].get<std::string>()));
                if (!copied.empty()) {
                    PushEvent({CodexEventType::GeneratedImage, "ImageGen completed.", copied});
                }
            }
        } else if (type == "agentMessage" && method == "item/completed" &&
                   operation_ == Operation::Mask) {
            try {
                const auto parsed = nlohmann::json::parse(StripCodeFence(item.value("text", "")));
                MaskProposal proposal;
                std::string error;
                if (PromptBuilder::ParseMaskProposal(parsed, proposal, error)) {
                    CodexEvent event{CodexEventType::MaskProposalReady, "Mask proposal completed."};
                    event.maskProposal = std::move(proposal);
                    PushEvent(std::move(event));
                } else {
                    PushEvent({CodexEventType::Error, error});
                }
            } catch (const std::exception& exception) {
                PushEvent({CodexEventType::Error, std::string("Could not parse mask proposal: ") + exception.what()});
            }
        }
    } else if (method == "turn/completed") {
        busy_ = false;
        {
            std::scoped_lock lock(stateMutex_);
            activeTurnId_.clear();
        }
        operation_ = Operation::None;
        const auto& turn = params.value("turn", nlohmann::json::object());
        const std::string status = turn.value("status", "completed");
        if (status != "completed") {
            PushEvent({CodexEventType::Error, "Codex turn ended with status: " + status});
        }
    }
}

void CodexBridge::PushEvent(CodexEvent event) {
    std::scoped_lock lock(eventMutex_);
    events_.push_back(std::move(event));
}

std::filesystem::path CodexBridge::CopyGeneratedImage(const std::filesystem::path& source) {
    if (!std::filesystem::exists(source)) {
        PushEvent({CodexEventType::Error, "ImageGen reported a path that does not exist."});
        return {};
    }
    const auto destination = sessionDirectory_ /
        (L"imagegen-" + std::to_wstring(++generatedIndex_) + L".png");
    std::error_code error;
    std::filesystem::copy_file(source, destination, std::filesystem::copy_options::none, error);
    if (error) {
        PushEvent({CodexEventType::Error, "Could not copy the generated image into the session directory."});
        return {};
    }
    return destination;
}

} // namespace codextex
