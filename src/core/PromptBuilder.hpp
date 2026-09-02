#pragma once

#include <string>

namespace codextex {

class PromptBuilder {
public:
    static std::string GenerationPrompt(const std::string& userRequest);
};

} // namespace codextex
