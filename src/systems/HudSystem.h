#pragma once
// =============================================================================
// systems/HudSystem.h — the heads-up overlay over the 3D view (Dear ImGui)
// =============================================================================
//
// Milestone 1 drew this with bgfx's debug text (dbgTextPrintf): a fixed 8x16
// pixel VGA font in a character grid, zero setup, but ASCII only, no HiDPI
// scaling and no way to keep text out from under a UI panel. Now that Dear
// ImGui is in, the overlay uses it too:
//   * top-left: title, visible star count, frame time, renderer, camera
//     (and the scheme + legend text when the filter panel is hidden);
//   * bottom-left: the controls (H toggles);
//   * ring labels ("50 ly", "NGP", ...) drawn with ImGui's BACKGROUND draw
//     list at the projected 3D anchor points. The background list is drawn
//     behind every ImGui window, so the panel naturally covers labels, and
//     we clip to the 3D viewport so nothing spills under the panel.
//
// Overlay windows use ImGuiWindowFlags_NoInputs: they never capture the
// mouse, so orbiting/zooming works even with the pointer over the text.
//
// It is a "system" in the ECS sense: reads ctx state (camera, colour scheme,
// view settings), holds no state of its own. Call between
// ImGuiLayer::begin_frame() and end_frame(), after CameraSystem::update().
// =============================================================================

#include "systems/PickingSystem.h"

#include <entt/entity/fwd.hpp>

#include <cstddef>
#include <string>
#include <vector>

namespace starmap::render { struct GuideLabel; }

namespace starmap::systems {

struct HudInfo {
    std::string renderer;          // bgfx::getRendererName()
    std::size_t visible_stars = 0; // stars drawn this frame (after filters)
    std::size_t total_stars = 0;
    double frame_ms = 0.0;         // smoothed wall-clock frame time
    float pixels_per_point = 1.0f; // io.DisplayFramebufferScale: camera maths is in pixels, ImGui in points
    PickParams pick;               // billboard size rules, so the reticle hugs the star as drawn
};

void draw_hud(const entt::registry& registry, const HudInfo& info, const std::vector<render::GuideLabel>* labels);

}  // namespace starmap::systems
