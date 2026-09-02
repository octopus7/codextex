#include "core/PromptBuilder.hpp"

#include <sstream>

namespace codextex {

std::string PromptBuilder::GenerationPrompt(const std::string& userRequest) {
    std::ostringstream prompt;
    prompt << "$imagegen\n"
           << "Use case: precise-object-edit\n"
           << "Asset type: screen-space Base Color projection source for an existing 3D mesh\n"
           << "Input images: Image 1 is the immutable, pixel-registered edit target captured from "
              "the locked 3D viewport. Edit Image 1 in place; this is not a new rendering, "
              "variation, or reinterpretation.\n"
           << "Reference context: Image 1 may contain surrounding reference objects. Use them only "
              "as visual, material, scale, or scene context. Keep every reference object and the "
              "background pixel-for-pixel unchanged; edit only the primary target mesh.\n"
           << "Primary request: " << userRequest << "\n"
           << "Style/medium: production game-asset Base Color texture appearance\n"
           << "Output format: square 1:1 image matching Image 1 dimensions exactly\n"
           << "Composition/framing: preserve Image 1 exactly. Every silhouette contour, occlusion "
              "boundary, surface landmark, and visible part must remain at the same x/y pixel "
              "coordinates. Do not move, resize, rotate, bend, add, remove, or redraw any part.\n"
           << "Lighting/mood: neutral material reference; avoid baked highlights and cast shadows\n"
           << "Allowed change: replace only the RGB surface/material appearance inside the existing "
              "visible pixels of the primary target mesh. If the user request implies a geometry, "
              "pose, camera, or silhouette change, ignore that part of the request.\n"
           << "Hard constraints: preserve camera, silhouette, geometry, proportions, pose, object "
              "position, framing, pixel registration, and image dimensions exactly; keep all visible "
              "surface boundaries aligned with Image 1; no text; no logos; no watermark\n"
           << "Avoid: new objects, changed geometry, changed background, dramatic lighting, reflections, "
              "ambient occlusion painted into the Base Color\n";
    return prompt.str();
}

} // namespace codextex
