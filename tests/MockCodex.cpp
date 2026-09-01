#include <nlohmann/json.hpp>

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void Respond(const nlohmann::json& request, nlohmann::json result) {
    std::cout << nlohmann::json{{"id", request.at("id")}, {"result", std::move(result)}}.dump()
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
        } else if (method == "skills/list") {
            const bool enabled = mode != "missing-skill";
            Respond(request, {{"data", {{{"skills", {{{"name", "imagegen"},
                                                        {"enabled", enabled},
                                                        {"path", "C:/mock/imagegen/SKILL.md"}}}}}}}});
        } else if (method == "thread/start") {
            Respond(request, {{"thread", {{"id", "mock-thread"}}}});
        } else if (method == "turn/start") {
            const bool mask = request.at("params").contains("outputSchema");
            Respond(request, {{"turn", {{"id", mask ? "mask-turn" : "generation-turn"}}}});
            if (mask) {
                const nlohmann::json proposal = {
                    {"polygons", {{{"operation", "include"},
                                    {"points", {{0.2, 0.2}, {0.8, 0.2}, {0.5, 0.8}}}}}},
                    {"confidence", 0.8},
                    {"suggestedFeatherPx", 12},
                    {"rationale", "mock visible surface"},
                };
                Notify("item/completed", {{"item", {{"type", "agentMessage"},
                                                       {"text", proposal.dump()}}}});
                Notify("turn/completed", {{"turn", {{"id", "mask-turn"},
                                                       {"status", "completed"}}}});
            } else if (mode != "hold-generation") {
                Notify("item/completed", {{"item", {{"type", "imageGeneration"},
                                                       {"status", "completed"},
                                                       {"savedPath", Environment("CODEXTEX_MOCK_IMAGE")}}}});
                Notify("turn/completed", {{"turn", {{"id", "generation-turn"},
                                                       {"status", "completed"}}}});
            }
        } else if (method == "turn/interrupt") {
            Respond(request, nlohmann::json::object());
            Notify("turn/completed", {{"turn", {{"id", "generation-turn"},
                                                   {"status", "interrupted"}}}});
        } else {
            std::cout << nlohmann::json{{"id", request.at("id")},
                                        {"error", {{"message", "unsupported mock method"}}}}.dump()
                      << '\n' << std::flush;
        }
    }
    return 0;
}
