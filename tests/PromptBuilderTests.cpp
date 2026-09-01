#include "core/PromptBuilder.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("ImageGen prompt declares edit invariants") {
    const std::string prompt = codextex::PromptBuilder::GenerationPrompt("paint worn red leather");
    CHECK(prompt.find("$imagegen") != std::string::npos);
    CHECK(prompt.find("precise-object-edit") != std::string::npos);
    CHECK(prompt.find("preserve camera") != std::string::npos);
    CHECK(prompt.find("surrounding reference objects") != std::string::npos);
    CHECK(prompt.find("keep them unchanged") != std::string::npos);
    CHECK(prompt.find("paint worn red leather") != std::string::npos);
}

TEST_CASE("ImageGen prompt preserves Korean UTF-8 text") {
    const std::string request = "겨드랑이 안쪽을 붉은 가죽으로 칠해줘";
    const std::string prompt = codextex::PromptBuilder::GenerationPrompt(request);
    CHECK(prompt.find(request) != std::string::npos);
}

TEST_CASE("Mask proposal parses normalized polygons") {
    const nlohmann::json value = {
        {"polygons", {{{"operation", "include"}, {"points", {{0.1, 0.2}, {0.8, 0.2}, {0.5, 0.9}}}}}},
        {"confidence", 0.75},
        {"suggestedFeatherPx", 20},
        {"rationale", "main visible panel"},
    };
    codextex::MaskProposal proposal;
    std::string error;
    REQUIRE(codextex::PromptBuilder::ParseMaskProposal(value, proposal, error));
    REQUIRE(proposal.polygons.size() == 1);
    CHECK(proposal.suggestedFeatherPx == 20);
    CHECK(proposal.polygons.front().normalizedPoints.size() == 3);
}
