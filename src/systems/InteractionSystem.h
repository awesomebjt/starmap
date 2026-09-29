#pragma once
// =============================================================================
// systems/InteractionSystem.h — pointer & key intents -> pick / select / fly
// =============================================================================
//
// ROLE IN THE ARCHITECTURE
//   InputHandler (SDL-aware) only records WHAT happened: "a click at (x, y)",
//   "the pointer hovers at (x, y)", "F was pressed". This system decides what
//   it MEANS, using the scene: pick the star, change the selection, start a
//   camera flight. Splitting it this way keeps SDL out of the logic, so the
//   logic can be unit-tested with a synthetic registry, and lets the panel's
//   search box reuse the very same select_and_fly().
//
// CLICK POLICY (documented choice)
//   * click on a star          -> select it and fly to it;
//   * click on empty space     -> clear the selection, camera stays put
//                                 (the common convention in 3D tools; Esc does
//                                 the same from the keyboard);
//   * double-click on empty    -> fly back to Sol (M1's "double-click: Sol");
//   * double-click on a star   -> same as a click (already selected);
//   * right-click on a star    -> ask the tag menu to open there (scene::TagMenuRequest).
//                                 The camera stays put and the selection does
//                                 not change: tagging is not navigation.
//   * right-click on empty     -> no menu request. ImGui closes an open menu
//                                 itself because the click landed outside it.
// =============================================================================

#include "scene/CameraFlight.h"
#include "systems/PickingSystem.h"

#include <entt/entity/fwd.hpp>

namespace starmap::systems {

// Consume this frame's scene::PointerInput (ctx): click -> pick/select/fly,
// hover -> HoverState. Uses the camera matrices of the previous frame, which
// is what was on screen when the user clicked.
void process_pointer(entt::registry& registry, const PickParams& params);

// Select `e` and fly the camera to it (search box, clicks, --select).
void select_and_fly(entt::registry& registry, entt::entity e, float duration_s = scene::kFlightSeconds);

void focus_selection(entt::registry& registry);  // F: fly to the selection, or to Sol if none
void fly_home(entt::registry& registry);         // H / Home: fly to Sol, keeping the distance
void reset_view(entt::registry& registry);       // R: fly to the start-up pose (angles too)

}  // namespace starmap::systems
