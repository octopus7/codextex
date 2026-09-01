#include "core/PromptBuilder.hpp"

#include <algorithm>
#include <sstream>

namespace codextex {

std::string PromptBuilder::GenerationPrompt(const std::string& userRequest) {
    std::ostringstream prompt;
    prompt << "$imagegen\n"
           << "Use case: precise-object-edit\n"
           << "Asset type: screen-space Base Color projection source for an existing 3D mesh\n"
           << "Input images: Image 1: edit target captured from the locked 3D viewport\n"
           << "Reference context: Image 1 may contain surrounding reference objects. Use them only "
              "as visual, material, scale, or scene context; keep them unchanged and edit only the "
              "primary target mesh.\n"
           << "Primary request: " << userRequest << "\n"
           << "Style/medium: production game-asset Base Color texture appearance\n"
           << "Output format: square 1:1 image matching Image 1 dimensions exactly\n"
           << "Composition/framing: preserve Image 1 exactly\n"
           << "Lighting/mood: neutral material reference; avoid baked highlights and cast shadows\n"
           << "Constraints: change only the surface appearance; preserve camera, silhouette, geometry, "
              "proportions, pose, object position, framing, and image dimensions; keep all visible "
              "surface boundaries aligned with Image 1; no text; no logos; no watermark\n"
           << "Avoid: new objects, changed geometry, changed background, dramatic lighting, reflections, "
              "ambient occlusion painted into the Base Color\n";
    return prompt.str();
}

std::string PromptBuilder::MaskPrompt() {
    return
        "Image 1 is the locked original viewport capture. Image 2 is the generated projection source. "
        "Propose the aesthetically useful surface region that should be projected from Image 2 onto "
        "the visible mesh in Image 1. Return normalized 0..1 image-space polygons. Start with include "
        "polygons and add exclude polygons for holes or areas that should retain the existing texture. "
        "Do not include the background, silhouette exterior, hidden geometry, or severe grazing-angle "
        "regions. Prefer a conservative boundary; the user will refine it manually.";
}

nlohmann::json PromptBuilder::MaskOutputSchema() {
    const nlohmann::json coordinate = {
        {"type", "number"}, {"minimum", 0}, {"maximum", 1},
    };
    const nlohmann::json point = {
        {"type", "array"},
        {"prefixItems", nlohmann::json::array({coordinate, coordinate})},
        {"minItems", 2},
        {"maxItems", 2},
    };
    const nlohmann::json polygon = {
        {"type", "object"},
        {"properties",
         {{"operation", {{"type", "string"}, {"enum", {"include", "exclude"}}}},
          {"points", {{"type", "array"}, {"minItems", 3}, {"items", point}}}}},
        {"required", {"operation", "points"}},
        {"additionalProperties", false},
    };
    return {
        {"type", "object"},
        {"properties",
         {{"polygons", {{"type", "array"}, {"items", polygon}}},
          {"confidence", {{"type", "number"}, {"minimum", 0}, {"maximum", 1}}},
          {"suggestedFeatherPx", {{"type", "integer"}, {"minimum", 0}, {"maximum", 128}}},
          {"rationale", {{"type", "string"}}}}},
        {"required", {"polygons", "confidence", "suggestedFeatherPx", "rationale"}},
        {"additionalProperties", false},
    };
}

bool PromptBuilder::ParseMaskProposal(const nlohmann::json& value, MaskProposal& proposal,
                                      std::string& error) {
    try {
        MaskProposal parsed;
        parsed.confidence = value.at("confidence").get<float>();
        parsed.suggestedFeatherPx = std::clamp(value.at("suggestedFeatherPx").get<int>(), 0, 128);
        parsed.rationale = value.at("rationale").get<std::string>();
        for (const auto& polygonValue : value.at("polygons")) {
            MaskPolygon polygon;
            polygon.operation = polygonValue.at("operation").get<std::string>() == "exclude"
                ? MaskPolygon::Operation::Exclude
                : MaskPolygon::Operation::Include;
            for (const auto& point : polygonValue.at("points")) {
                if (!point.is_array() || point.size() != 2) {
                    error = "Mask point must contain exactly two coordinates.";
                    return false;
                }
                polygon.normalizedPoints.push_back(
                    {std::clamp(point[0].get<float>(), 0.0f, 1.0f),
                     std::clamp(point[1].get<float>(), 0.0f, 1.0f)});
            }
            if (polygon.normalizedPoints.size() < 3) {
                error = "Mask polygon must contain at least three points.";
                return false;
            }
            parsed.polygons.push_back(std::move(polygon));
        }
        proposal = std::move(parsed);
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        error = std::string("Invalid Codex mask proposal: ") + exception.what();
        return false;
    }
}

} // namespace codextex
