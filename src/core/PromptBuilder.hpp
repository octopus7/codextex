#pragma once

#include "core/Types.hpp"

#include <nlohmann/json.hpp>

#include <string>

namespace codextex {

class PromptBuilder {
public:
    static std::string GenerationPrompt(const std::string& userRequest);
    static std::string MaskPrompt();
    static nlohmann::json MaskOutputSchema();
    static bool ParseMaskProposal(const nlohmann::json& value, MaskProposal& proposal,
                                  std::string& error);
};

} // namespace codextex
