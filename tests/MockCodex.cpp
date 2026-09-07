#include <nlohmann/json.hpp>

#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <process.h>
#include <string>
#include <thread>

namespace {

void Respond(const nlohmann::json& request, nlohmann::json result) {
    std::cout << nlohmann::json{{"id", request.at("id")}, {"result", std::move(result)}}.dump()
              << '\n' << std::flush;
}

void RespondError(const nlohmann::json& request, const std::string& message) {
    std::cout << nlohmann::json{{"id", request.at("id")},
                                {"error", {{"code", -32600}, {"message", message}}}}.dump()
              << '\n' << std::flush;
}

void Notify(const std::string& method, nlohmann::json params) {
    std::cout << nlohmann::json{{"method", method}, {"params", std::move(params)}}.dump()
              << '\n' << std::flush;
}

void NotifyImage(const std::string& threadId, const std::string& turnId,
                 const std::string& imagePath) {
    Notify("item/completed", {{"threadId", threadId}, {"turnId", turnId},
        {"item", {{"type", "imageGeneration"}, {"status", "completed"},
                  {"savedPath", imagePath}}}});
}

std::string Environment(const char* name, const std::string& fallback = {}) {
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) return fallback;
    std::string result(value);
    std::free(value);
    return result;
}

void Mark(const char* name, const std::string& value) {
    const auto path = Environment(name);
    if (!path.empty()) std::ofstream(path, std::ios::binary | std::ios::app) << value << '\n';
}

} // namespace

int main() {
    const std::string mode = Environment("CODEXTEX_MOCK_MODE", "success");
    const std::string blockMethod = Environment("CODEXTEX_MOCK_BLOCK_METHOD");
    const int blockOccurrence = std::stoi(Environment("CODEXTEX_MOCK_BLOCK_OCCURRENCE", "1"));
    int blockCount = 0;
    Mark("CODEXTEX_MOCK_PID", std::to_string(_getpid()));
    std::uint64_t threadCounter = 0;
    std::uint64_t turnCounter = 0;
    std::string latestThreadId;
    std::string latestTurnId;
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        const auto request = nlohmann::json::parse(line);
        const std::string method = request.value("method", "");
        if (!request.contains("id")) continue;
        if (method == blockMethod && ++blockCount == blockOccurrence) {
            Mark("CODEXTEX_MOCK_ENTERED", method);
            const auto release = std::filesystem::path(Environment("CODEXTEX_MOCK_RELEASE"));
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (!std::filesystem::exists(release) &&
                   std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
        if (method == Environment("CODEXTEX_MOCK_DISCONNECT_METHOD")) return 0;

        if (method == "initialize") {
            Respond(request, nlohmann::json::object());
        } else if (method == "account/read") {
            if (mode == "signed-out") Respond(request, {{"account", nullptr}});
            else Respond(request, {{"account", {{"type", "chatgpt"}}}});
        } else if (method == "model/list") {
            Respond(request, {{"data", {
                {{"id", "gpt-5.6-sol"}, {"model", "gpt-5.6-sol"},
                 {"displayName", "GPT-5.6-Sol"}, {"defaultReasoningEffort", "medium"},
                 {"supportedReasoningEfforts", {
                     {{"reasoningEffort", "low"}, {"description", "Fast"}},
                     {{"reasoningEffort", "medium"}, {"description", "Balanced"}},
                     {{"reasoningEffort", "high"}, {"description", "Deep"}},
                     {{"reasoningEffort", "xhigh"}, {"description", "Very deep"}}}}},
                {{"id", "gpt-5.6-luna"}, {"model", "gpt-5.6-luna"},
                 {"displayName", "GPT-5.6-Luna"}, {"defaultReasoningEffort", "medium"},
                 {"supportedReasoningEfforts", {
                     {{"reasoningEffort", "low"}, {"description", "Fast"}},
                     {{"reasoningEffort", "medium"}, {"description", "Balanced"}}}}}
            }}});
        } else if (method == "skills/list") {
            const bool enabled = mode != "missing-skill";
            Respond(request, {{"data", {{{"skills", {{{"name", "imagegen"},
                                                        {"enabled", enabled},
                                                        {"path", "C:/mock/imagegen/SKILL.md"}}}}}}}});
            if (mode == "exit-after-skills-response") return 0;
        } else if (method == "thread/start") {
            if (request.at("params").value("model", "").empty()) {
                RespondError(request, "thread/start requires a model");
                continue;
            }
            const std::string sandbox = request.at("params").value("sandbox", "");
            const std::string expected = mode == "legacy-sandbox"
                ? "workspaceWrite" : "workspace-write";
            if (sandbox != expected) {
                RespondError(request, "Invalid request: unknown variant `" + sandbox + "`");
            } else {
                Respond(request, {{"thread", {{"id", "mock-thread-" +
                    std::to_string(++threadCounter)}}}});
            }
        } else if (method == "turn/start") {
            if (request.at("params").value("model", "").empty() ||
                request.at("params").value("effort", "").empty()) {
                RespondError(request, "turn/start requires model and effort");
                continue;
            }
            const std::string threadId = request.at("params").at("threadId").get<std::string>();
            const std::string turnId = "generation-turn-" + std::to_string(++turnCounter);
            latestThreadId = threadId;
            latestTurnId = turnId;
            if (mode == "completion-before-start-response") {
                NotifyImage(threadId, turnId, Environment("CODEXTEX_MOCK_IMAGE"));
                Notify("turn/completed", {{"threadId", threadId},
                    {"turn", {{"id", turnId}, {"status", "completed"}}}});
                Respond(request, {{"turn", {{"id", turnId}}}});
                continue;
            }
            if (mode == "mismatched-turn-events") {
                // Emit a stale turn before the response to exercise the window
                // where the client has not learned the active turn ID yet.
                NotifyImage(threadId, "stale-turn", Environment("CODEXTEX_MOCK_IMAGE"));
                Notify("turn/completed", {{"threadId", threadId},
                    {"turn", {{"id", "stale-turn"}, {"status", "interrupted"}}}});
                Respond(request, {{"turn", {{"id", turnId}}}});
                NotifyImage(threadId, "stale-turn", Environment("CODEXTEX_MOCK_IMAGE"));
                Notify("turn/completed", {{"threadId", threadId}, {"turnId", turnId},
                    {"turn", {{"id", "stale-turn"}, {"status", "interrupted"}}}});
                Notify("item/started", {{"threadId", threadId}, {"turnId", turnId},
                    {"item", {{"type", "imageGeneration"}, {"status", "stale-events-delivered"}}}});
                continue;
            }
            Respond(request, {{"turn", {{"id", turnId}}}});
            if (mode != "hold-generation" && mode != "late-events-after-forget") {
                NotifyImage(threadId, turnId, Environment("CODEXTEX_MOCK_IMAGE"));
                if (mode != "image-without-turn-completion") {
                    Notify("turn/completed", {{"threadId", threadId}, {"turnId", turnId},
                        {"turn", {{"id", turnId}, {"status", "completed"}}}});
                    Mark("CODEXTEX_MOCK_SENT", turnId);
                }
            }
        } else if (method == "turn/interrupt") {
            Respond(request, nlohmann::json::object());
            const auto& params = request.at("params");
            const std::string threadId = params.at("threadId").get<std::string>();
            const std::string turnId = params.at("turnId").get<std::string>();
            Mark("CODEXTEX_MOCK_INTERRUPTED", threadId);
            if (Environment("CODEXTEX_MOCK_LATE_IMAGE_ON_INTERRUPT") == "1") {
                NotifyImage(threadId, turnId, Environment("CODEXTEX_MOCK_IMAGE"));
            }
            if (mode == "late-events-after-forget" && threadId != latestThreadId) {
                // The test releases these notifications only after Forget has
                // removed the first job, without relying on scheduling delays.
                const auto release = std::filesystem::path(Environment("CODEXTEX_MOCK_RELEASE"));
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
                while (!std::filesystem::exists(release) &&
                       std::chrono::steady_clock::now() < deadline) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                NotifyImage(threadId, turnId, Environment("CODEXTEX_MOCK_IMAGE"));
                Notify("turn/completed", {{"threadId", threadId}, {"turnId", turnId},
                    {"turn", {{"id", turnId}, {"status", "interrupted"}}}});
                Notify("item/started", {{"threadId", latestThreadId}, {"turnId", latestTurnId},
                    {"item", {{"type", "imageGeneration"}, {"status", "late-events-delivered"}}}});
                continue;
            }
            Notify("turn/completed", {{"threadId", threadId}, {"turnId", turnId},
                {"turn", {{"id", turnId}, {"status", "interrupted"}}}});
        } else {
            std::cout << nlohmann::json{{"id", request.at("id")},
                                        {"error", {{"message", "unsupported mock method"}}}}.dump()
                      << '\n' << std::flush;
        }
    }
    return 0;
}
