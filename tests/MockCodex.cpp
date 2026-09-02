#include <nlohmann/json.hpp>

#include <cstdlib>
#include <iostream>
#include <string>

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

std::string Environment(const char* name, const std::string& fallback = {}) {
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) return fallback;
    std::string result(value);
    std::free(value);
    return result;
}

} // namespace

int main() {
    const std::string mode = Environment("CODEXTEX_MOCK_MODE", "success");
    std::uint64_t threadCounter = 0;
    std::uint64_t turnCounter = 0;
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        const auto request = nlohmann::json::parse(line);
        const std::string method = request.value("method", "");
        if (!request.contains("id")) continue;

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
            const bool mask = request.at("params").contains("outputSchema");
            const std::string threadId = request.at("params").at("threadId").get<std::string>();
            const std::string turnId = (mask ? "mask-turn-" : "generation-turn-") +
                std::to_string(++turnCounter);
            Respond(request, {{"turn", {{"id", turnId}}}});
            if (mask) {
                const nlohmann::json proposal = {
                    {"polygons", {{{"operation", "include"},
                                    {"points", {{0.2, 0.2}, {0.8, 0.2}, {0.5, 0.8}}}}}},
                    {"confidence", 0.8},
                    {"suggestedFeatherPx", 12},
                    {"rationale", "mock visible surface"},
                };
                Notify("item/completed", {{"threadId", threadId}, {"turnId", turnId},
                    {"item", {{"type", "agentMessage"}, {"text", proposal.dump()}}}});
                Notify("turn/completed", {{"threadId", threadId}, {"turnId", turnId},
                    {"turn", {{"id", turnId}, {"status", "completed"}}}});
            } else if (mode != "hold-generation") {
                Notify("item/completed", {{"threadId", threadId}, {"turnId", turnId},
                    {"item", {{"type", "imageGeneration"}, {"status", "completed"},
                              {"savedPath", Environment("CODEXTEX_MOCK_IMAGE")}}}});
                Notify("turn/completed", {{"threadId", threadId}, {"turnId", turnId},
                    {"turn", {{"id", turnId}, {"status", "completed"}}}});
            }
        } else if (method == "turn/interrupt") {
            Respond(request, nlohmann::json::object());
            const auto& params = request.at("params");
            const std::string threadId = params.at("threadId").get<std::string>();
            const std::string turnId = params.at("turnId").get<std::string>();
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
