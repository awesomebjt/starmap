#pragma once
// =============================================================================
// systems/CameraSystem.h — orbit-camera controls and the per-frame matrices.
//
// ROLE IN THE ARCHITECTURE
//   A "system" in ECS terms is just a function that runs every frame over some
//   data. This one works on the OrbitCamera context variable (scene/OrbitCamera.h):
//     * the small free functions (orbit/zoom/pan/...) are called by InputHandler
//       when the user drags or scrolls;
//     * update() runs once per frame BEFORE rendering and turns the user state
//       into eye position, view matrix and projection matrix.
//   Nothing here touches the GPU. The only renderer-specific input is
//   `homogeneous_depth` (see update()), passed in as a bool so this file stays
//   free of bgfx and is unit-testable.
// =============================================================================

#include "scene/OrbitCamera.h"

#include <entt/entity/fwd.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <optional>
#include <string_view>

namespace starmap::systems {

// Mouse-drag rotation: dx/dy in PIXELS. Dragging right spins the scene right.
void orbit(scene::OrbitCamera& cam, float dx_px, float dy_px);
// Mouse wheel: positive = zoom in. Exponential: each notch changes distance by
// a constant FACTOR, so zooming feels the same at 1 pc and at 500 pc.
void zoom(scene::OrbitCamera& cam, float wheel_steps);
// Right/middle drag: move the target in the camera's screen plane so the point
// under the cursor follows the mouse. dx/dy in pixels.
void pan(scene::OrbitCamera& cam, float dx_px, float dy_px);
// Move the orbit centre to `p` (keeps distance and angles).
void focus(scene::OrbitCamera& cam, const glm::vec3& p);
// Restore the home pose (R key).
void reset(scene::OrbitCamera& cam);

// Eye position from the spherical coordinates (formula in OrbitCamera.h).
[[nodiscard]] glm::vec3 eye_position(const scene::OrbitCamera& cam);

// Compute eye/view/proj/view_proj for a viewport of `width` x `height` PIXELS.
//
// `homogeneous_depth` = bgfx::getCaps()->homogeneousDepth. Graphics APIs
// disagree on the depth range of clip space after the perspective divide:
//   OpenGL:                    z_ndc in [-1, +1]   ("homogeneous" depth)
//   Direct3D, Metal, Vulkan:   z_ndc in [ 0, +1]
// A projection matrix built for the wrong convention either wastes half the
// depth range or clips geometry near the camera. bx::mtxProj() builds the right
// one when told which convention the active renderer uses — this is one of the
// few places where application code has to care which backend bgfx picked.
void update(scene::OrbitCamera& cam, int width, int height, bool homogeneous_depth);
// Convenience: the same for the OrbitCamera stored in registry.ctx().
void update(entt::registry& registry, int width, int height, bool homogeneous_depth);

// Project a world point to window PIXELS (origin top-left). nullopt if the point
// is behind the camera. Used to place HUD labels next to 3D guides.
[[nodiscard]] std::optional<glm::vec2> world_to_screen(const scene::OrbitCamera& cam, const glm::vec3& p);

// Galactic position of a star looked up by name through the NameIndex in
// registry.ctx() (e.g. "Sol", "Sirius"). nullopt if unknown or not loaded.
[[nodiscard]] std::optional<glm::vec3> star_position(const entt::registry& registry, std::string_view name);

}  // namespace starmap::systems
