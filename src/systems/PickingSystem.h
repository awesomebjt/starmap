#pragma once
// =============================================================================
// systems/PickingSystem.h — which star is under the mouse cursor?
// =============================================================================
//
// WHY SCREEN-SPACE NEAREST POINT, NOT A 3D RAY TEST?
//   The textbook approach is a ray cast: turn the cursor into a ray and
//   intersect it with each object's bounding sphere. For our stars that is
//   the wrong question, because what the user SEES is not a sphere in the
//   world but a billboard whose size is chosen in PIXELS (vs_star.sc clamps
//   every star to 1.6..40 px regardless of its physical radius). A star 50 pc
//   away has a physical radius of ~1e-8 of the view; its ray/sphere hit test
//   would essentially never succeed, while on screen it is a 3-pixel dot the
//   user expects to be clickable. So we pick in the SAME space the user
//   perceives: project every star centre to the screen exactly as the vertex
//   shader does, compute the radius it is drawn with, and choose the star
//   whose centre is nearest to the cursor among those whose (slightly
//   enlarged) disc contains it.
//
// VISIBILITY-AWARE HIT RADII (learned the hard way, driving the live app)
//   The first version gave every star at least a 5 px (+3 px slack) target.
//   With ~92k stars, thousands of them far away and drawn at 10 % brightness
//   (vs_star.sc's `dim` floor), those invisible specks tiled the whole screen:
//   a click on "empty space" nearly always selected some Gaia star 190 ly
//   away that the user could not even see. So the picker also mirrors the
//   shader's BRIGHTNESS: a star drawn fainter than faint_visibility only
//   counts when the cursor is within faint_hit_px (you must click right on
//   it), while visible stars keep the generous radius.
//   Among the candidates the smallest NORMALISED distance d / r wins
//   (rather than the smallest d): a bright star 5 px away with a 12 px
//   target beats nothing, but a faint star you clicked dead-centre still
//   beats a bright neighbour whose glow you merely brushed.
//
//   For reference, the ray math (useful for picking real 3D geometry later):
//     1. cursor pixel (px, py) -> NDC: x = 2 px / w - 1, y = 1 - 2 py / h
//        (screen y grows DOWN, NDC y grows UP).
//     2. Unproject two points with the INVERSE view-projection matrix:
//          near = inv(P V) * (x, y, z_near, 1),  far = inv(P V) * (x, y, 1, 1),
//        then divide each by its w. z_near depends on the backend's clip-space
//        convention, which bgfx reports as caps->homogeneousDepth:
//          true  (OpenGL): NDC z in [-1, 1] -> z_near = -1
//          false (D3D, Metal, Vulkan): NDC z in [0, 1] -> z_near = 0
//        (CameraSystem builds proj with the same flag, so the two agree.)
//     3. ray origin = near, direction = normalize(far - near).
//     4. sphere (c, r): solve |o + t d - c|^2 = r^2; hit if the discriminant
//        b^2 - (|o-c|^2 - r^2) >= 0 with b = d . (c - o) and t > 0.
//
// COST: one mat4 x vec4 per star, 92k stars, no allocation. Measured
//   0.76-1.1 ms per pick in Release on this machine (README; ~36-54 ms in
//   Debug), far below a frame, so no spatial acceleration structure (grid /
//   BVH) is needed: picking runs once per click, plus at most once per frame
//   for the hover tooltip, and only on frames where the mouse moved. A grid
//   would have to be rebuilt whenever the camera moves (screen-space cells)
//   or would need a frustum-cone query (world-space cells): complexity that
//   would buy ~1 ms on a click.
// =============================================================================

#include "scene/OrbitCamera.h"

#include <entt/entity/fwd.hpp>
#include <entt/entity/entity.hpp>
#include <glm/vec2.hpp>

#include <cstddef>

namespace starmap::systems {

// The billboard size rules of vs_star.sc, duplicated on the CPU. Filled from
// render::StarRenderParams by the App (that header pulls in bgfx; the core
// library stays GPU-free).
struct PickParams {
    float min_px = 1.6f;          // StarRenderParams::min_px
    float max_px = 40.0f;         // StarRenderParams::max_px
    float marker_min_px = 9.0f;   // StarRenderParams::marker_min_px (Sol's ring)
    float glow_fraction = 0.6f;   // fs_star.sc fades the glow out at ~0.6 of the quad radius
    // Visible tiny stars still get a usable target, plus a little
    // forgiveness for shaky hands.
    float min_hit_px = 4.0f;
    float slack_px = 2.0f;
    // Brightness rules of vs_star.sc (StarRenderParams), for visibility():
    float dim_exponent = 0.8f;
    float min_intensity = 0.10f;
    float max_intensity = 2.0f;
    float exposure = 1.0f;
    // Stars drawn below this relative brightness are "barely visible"...
    float faint_visibility = 0.3f;
    // ...and need a (nearly) direct hit.
    float faint_hit_px = 2.5f;
};

struct PickResult {
    entt::entity entity = entt::null;
    float screen_dist_px = 0.0f;  // cursor to star centre
    float hit_radius_px = 0.0f;   // the radius it had to be within
    std::size_t tested = 0;       // stars examined
    double elapsed_ms = 0.0;
};

// Focal length in pixels: f = (viewport_h / 2) / tan(fovY / 2). A size s at
// depth d appears s * f / d pixels big (pinhole camera).
[[nodiscard]] float focal_length_px(const scene::OrbitCamera& cam);

// Drawn billboard radius in pixels, mirroring vs_star.sc step 2.
[[nodiscard]] float billboard_radius_px(float radius_pc, float depth, float focal_px, float intensity, bool marked,
                                        const PickParams& p);

// Relative brightness the shader draws a star with (1 = a normal star at
// its true size; 0.1 = the floor for far-away dots), mirroring vs_star.sc:
//   min(intensity, max_intensity) * max((true_px / px)^dim_exponent, min_intensity) * exposure
[[nodiscard]] float visibility(float true_px, float drawn_px, float intensity, const PickParams& p);

// Radius (pixels) within which a click on that billboard counts: the visible
// glow (60 % of the quad radius, at least min_hit_px) plus slack for a
// visible star; faint_hit_px for a star fainter than faint_visibility.
[[nodiscard]] float hit_radius_px(float billboard_px, const PickParams& p);
[[nodiscard]] float hit_radius_px(float billboard_px, float star_visibility, const PickParams& p);

// The not-FilteredOut star whose hit disc contains `cursor_px` (back-buffer
// pixels, origin top-left of the 3D viewport) at the smallest normalised
// distance d / hit_radius.
// Uses cam.view_proj as last computed by CameraSystem::update, i.e. what the
// user saw on screen when clicking.
[[nodiscard]] PickResult pick_star(const entt::registry& registry, const scene::OrbitCamera& cam,
                                   const glm::vec2& cursor_px, const PickParams& params);

}  // namespace starmap::systems
